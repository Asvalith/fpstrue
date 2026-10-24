"""Validate and summarize the real DS/two-client enemy-UI profile.

No discarded slow frames, no zero-filling missing metrics, no pooling unrelated
thread times. View-frustum candidates are NOT occlusion/pixel visibility.
"""
from __future__ import annotations

import csv
import json
import math
import re
import statistics
import struct
import sys
from pathlib import Path


METRICS = {
    "GT": "GameThreadTime", "RT": "RenderThreadTime", "RHI": "RHIThreadTime", "GPU": "GPUTime",
    "WorldTick": "PvpProfileWorldTickMs",
    "CPUProcessFraction": "CPUUsage_Process",
    "DrawCalls": "RHI/DrawCalls", "SlateDrawCalls": "DrawCall/SlateUI", "SlateGPU": "GPU/SlateUI",
    "SlatePrepass": "Slate/GameThread/DrawPrePass", "SlateWidgets": "Slate/GameThread/SObjectWidget_Tick",
    "SlateDrawWindows": "Slate/GameThread/DrawWindows_Private", "SlateRT": "Exclusive/RenderThread/Slate",
    "Movement": "Exclusive/GameThread/CharacterMovement", "Animation": "Exclusive/GameThread/Animation",
    "Physics": "Exclusive/GameThread/Physics", "GPULocalUsedMB": "GPUMem/LocalUsedMB",
    "TickActors": "Exclusive/GameThread/TickActors", "AIDecision": "fpstrueAI/GameThread/DecisionTime",
    "EQSGenerate": "fpstrueEQS/GameThread/GeneratePositions", "EQSValidate": "fpstrueEQS/GameThread/ValidatePositions",
    "UIComponent": "fpstrueEnemyUI/GameThread/ViewAndWidgetTick",
    "UIViewSamples": "fpstrueEnemyUI/ViewSamples", "UIViewCacheHits": "fpstrueEnemyUI/ViewCacheHits",
    "UIRedraw": "fpstrueEnemyUI/GameThread/RenderTargetRedrawTime", "PhysicalUsedMB": "PhysicalUsedMB",
}
UI_PATTERN = re.compile(
    r"PVP_PROFILE UI Role=(\w+) Mode=(\w+) Components=(\d+) Widgets=(\d+) Frustum=(\d+) "
    r"World=(\d+) Screen=(\d+) Manual=(\d+) Created=(\d+) Updates=(\d+) Requests=(\d+) Draws=(\d+) "
    r"PhysicalMB=([\d.]+) PeakPhysicalMB=([\d.]+) Elapsed=([\d.]+)"
)


def percentile(values: list[float], fraction: float) -> float:
    return sorted(values)[max(0, min(len(values) - 1, math.ceil(fraction * len(values)) - 1))]


def load_frames(path: Path) -> list[dict[str, float]]:
    with path.open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        names = next(reader)
        # UE may repeat a GPU field name; preserve the FIRST exact column rather
        # than having DictReader silently replace it with a later duplicate.
        indexes: dict[str, int] = {}
        for index, name in enumerate(names):
            indexes.setdefault(name.strip(), index)
        if "FrameTime" not in indexes:
            raise ValueError(f"Missing FrameTime: {path}")
        result = []
        for cells in reader:
            text = cells[indexes["FrameTime"]] if len(cells) > indexes["FrameTime"] else ""
            if text in ("", "FrameTime", "[HasHeaderRowAtEnd]") or text.startswith("["):
                continue
            frame = float(text)
            if not math.isfinite(frame):
                raise ValueError(f"Nonfinite FrameTime: {path}")
            if frame <= 0:
                continue
            row = {}
            for name, index in indexes.items():
                if index >= len(cells):
                    continue
                try:
                    value = float(cells[index])
                except ValueError:
                    continue
                if math.isfinite(value):
                    row[name] = value
            result.append(row)
    if len(result) < 10:
        raise ValueError(f"Insufficient samples: {path}")
    return result


def metric_mean(frames: list[dict[str, float]], key: str) -> float | None:
    values = [row.get(key) for row in frames]
    return statistics.mean(values) if all(value is not None for value in values) else None


def has_required_timings(summary: dict, rendered: bool) -> bool:
    # Dedicated/NullRHI roles have no trustworthy stat-unit pipeline values.
    # Every role must instead provide the same real World::Tick scope; only a
    # genuine renderer can additionally prove GT and GPU timings.
    world_tick = summary.get("WorldTick")
    if world_tick is None or world_tick <= 0:
        return False
    return not rendered or all(summary.get(key) is not None and summary[key] > 0 for key in ("GT", "GPU"))


def local_ui_mode(case: dict, role: str) -> str:
    return case["Mode"] if role == case["RenderRole"] else "Off"


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as handle:
        header = handle.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Invalid rendered PNG: {path}")
    return struct.unpack(">II", header[16:24])


def summarize_role(root: Path, case: dict, role: str) -> dict:
    case_root = root / case["Name"]
    text = (case_root / f"{role}.log").read_text(encoding="utf-8-sig", errors="replace")
    if f"PVP_PROFILE COMPLETE Role={role}" not in text:
        raise ValueError(f"Missing normal completion: {case_root}/{role}")
    if re.search(r"PVP_PROFILE (?:FAIL|QUARANTINED)|Fatal error:|Assertion failed:|Ensure condition failed:", text):
        raise ValueError(f"Runtime failure: {case_root}/{role}")
    samples = [m.groups() for m in UI_PATTERN.finditer(text) if m.group(1) == role]
    if len(samples) < 100:
        raise ValueError(f"UI readback incomplete: {case_root}/{role}")
    actual_mode = local_ui_mode(case, role)
    has_ui = actual_mode != "Off"
    for row in samples:
        elapsed = float(row[14])
        expected_widgets = 160 if has_ui else 0
        if case.get("Workload") == "HealthAndDeath" and has_ui:
            if elapsed >= 23:
                expected_widgets = 128
            elif elapsed >= 19.5:
                expected_widgets = int(row[3])
                if not 128 <= expected_widgets <= 160:
                    raise ValueError(f"Unexpected death transition: {case_root}/{role}")
        if row[1] != actual_mode or int(row[2]) != 160 or int(row[3]) != expected_widgets:
            raise ValueError(f"Actual UI population/mode differs from request: {case_root}/{role}")
        expected_world = expected_widgets if actual_mode in ("WorldAlways", "WorldEvent") else 0
        expected_screen = expected_widgets if actual_mode == "Screen" else 0
        expected_manual = expected_world if actual_mode == "WorldEvent" else 0
        if int(row[5]) != expected_world or int(row[6]) != expected_screen or int(row[7]) != expected_manual:
            raise ValueError(f"Actual component space/manual-redraw differs from request: {case_root}/{role}")
    rendered = role == case["RenderRole"]
    if rendered:
        if png_dimensions(case_root / f"{role}.png") != (1600, 900):
            raise ValueError(f"Actual rendered resolution differs: {case_root}/{role}")
        if case["Mode"] != "Off" and max(int(row[4]) for row in samples) == 0:
            raise ValueError(f"No blood bars entered the view; UI cost not exercised: {case_root}/{role}")
        if case["Mode"] in ("WorldAlways", "WorldEvent") and max(int(row[11]) for row in samples) == 0:
            raise ValueError(f"World components never submitted a real render-target draw: {case_root}/{role}")
        if case.get("Workload") == "HealthAndDeath" and png_dimensions(case_root / f"{role}_AfterDeath.png") != (1600, 900):
            raise ValueError(f"Post-death rendered capture missing: {case_root}/{role}")
    if case.get("Workload") == "HealthAndDeath" and role == "Server":
        pulses = re.findall(r"WORKLOAD HealthPulse=(\d+) Damage=0.5 Elapsed=([\d.]+)", text)
        if len(pulses) != 17 or any(int(count) != 160 for count, _ in pulses):
            raise ValueError(f"Dynamic-health workload was not 17 real 160-enemy pulses: {case_root}")
        if len(re.findall(r"WORKLOAD DeathBatch=32 ExpectedAlive=128", text)) != 1:
            raise ValueError(f"Missing/duplicate real death batch: {case_root}")
    gc = re.findall(r"PVP_PROFILE UI_GC Role=(\w+) Widgets=(\d+) Expected=(\d+) PhysicalMB=([\d.]+) CaptureStopped=1", text)
    expected_after_gc = (128 if case.get("Workload") == "HealthAndDeath" else 160) if has_ui else 0
    if len(gc) != 1 or gc[0][0] != role or int(gc[0][1]) != expected_after_gc or int(gc[0][2]) != expected_after_gc:
        raise ValueError(f"Post-capture Widget UObject cleanup not proven: {case_root}/{role}")
    frames = load_frames(case_root / f"{role}.csv")
    frame_values = [row["FrameTime"] for row in frames]
    duration = sum(frame_values) / 1000
    if not 25 <= duration <= 45:
        raise ValueError(f"CSV duration outside capture bounds: {duration}: {case_root}/{role}")
    summary = dict(Case=case["Name"], Mode=case["Mode"], LocalMode=actual_mode, Workload=case.get("Workload", "Static"), RenderRole=case["RenderRole"], Role=role,
                   Round=case["Round"], Rendered=rendered, Samples=len(frames), Duration=duration,
                   Frame=statistics.mean(frame_values), P95=percentile(frame_values, .95), P99=percentile(frame_values, .99))
    for name, key in METRICS.items():
        summary[name] = metric_mean(frames, key)
    if not has_required_timings(summary, rendered):
        raise ValueError(f"Required real WorldTick/rendered GT/GPU timing absent: {case_root}/{role}")
    # NullRHI peer GPU values are not real measurements, even if a zero column exists.
    if not rendered:
        for name in ("GT", "RT", "RHI", "GPU", "SlateGPU", "DrawCalls", "SlateDrawCalls", "GPULocalUsedMB"):
            summary[name] = None
    summary["UIWidgets"] = statistics.mean(int(row[3]) for row in samples)
    summary["UIWidgetsFinal"] = int(samples[-1][3])
    summary["UIFrustumMean"] = statistics.mean(int(row[4]) for row in samples)
    summary["UIFrustumMin"] = min(int(row[4]) for row in samples)
    summary["UIFrustumMax"] = max(int(row[4]) for row in samples)
    for label, index in (("CreatedDelta", 8), ("UpdateDelta", 9), ("RequestDelta", 10), ("DrawDelta", 11)):
        values = [int(row[index]) for row in samples]
        if any(b < a for a, b in zip(values, values[1:])):
            raise ValueError(f"Counter loss/object churn invalidates fixed-population deltas: {case_root}/{role}")
        summary[label] = values[-1] - values[0]
    expected_updates = 17 * 160 if has_ui and case.get("Workload") == "HealthAndDeath" else 0
    if summary["UpdateDelta"] != expected_updates:
        raise ValueError(f"Actual UI health-event load differs from the fixed workload ({summary['UpdateDelta']} versus {expected_updates}): {case_root}/{role}")
    summary["ReadbackPhysicalStartMB"] = float(samples[0][12])
    summary["ReadbackPhysicalEndMB"] = float(samples[-1][12])
    summary["ReadbackPhysicalPeakMB"] = max(float(row[12]) for row in samples)
    summary["PostGcWidgets"] = int(gc[0][1])
    summary["PostGcPhysicalMB"] = float(gc[0][3])
    summary["VideoMemoryWarnings"] = len(re.findall(r"Video memory has been exhausted|out of video memory", text, re.IGNORECASE))
    summary["TexturePoolWarnings"] = len(re.findall(r"texture streaming pool.*over budget|texture pool.*over budget", text, re.IGNORECASE))
    summary["ShadowOverflowWarnings"] = len(re.findall(r"virtual shadow map.*overflow|non.nanite.*overflow", text, re.IGNORECASE))
    summary["MissingMetrics"] = [name for name in METRICS if summary[name] is None]
    summary["ViewReadback"] = re.findall(r"PVP_PROFILE READY.*Location=(.*?) Rotation=(.*?) Width=(\d+) Height=(\d+)", text)[-1]
    return summary


def main(root: Path) -> None:
    cases = json.loads((root / "results.json").read_text(encoding="utf-8-sig"))
    if not isinstance(cases, list):
        cases = [cases]
    if not cases or any(not case["Valid"] or any(p["Forced"] or p["ExitCode"] != 0 for p in case["Processes"]) for case in cases):
        raise ValueError("Incomplete/forced cases must not be aggregated as a successful experiment")
    if any(case.get("PeerUIPolicy") != "Off" for case in cases):
        raise ValueError("Only the rendered role may vary UI; missing/changed peer-Off policy is a different experiment")
    summaries = [summarize_role(root, case, role) for case in cases for role in ("Server", "Client1", "Client2")]
    for rendered_role in {r["RenderRole"] for r in summaries}:
        views = {tuple(r["ViewReadback"]) for r in summaries if r["Role"] == rendered_role and r["RenderRole"] == rendered_role}
        if len(views) != 1:
            raise ValueError(f"Camera changed between UI modes/workloads for {rendered_role}: {views}")
    groups = []
    for workload in sorted({r["Workload"] for r in summaries}):
        for rendered_role in sorted({r["RenderRole"] for r in summaries}):
            for mode in ("Off", "Screen", "WorldAlways", "WorldEvent"):
                for role in ("Server", rendered_role):
                    rows = [r for r in summaries if r["Mode"] == mode and r["Workload"] == workload and r["RenderRole"] == rendered_role and r["Role"] == role]
                    if not rows:
                        continue
                    group = dict(Mode=mode, LocalMode=rows[0]["LocalMode"], Workload=workload, RenderRole=rendered_role, Role=role, Runs=len(rows))
                    for key in ("Frame", "P95", "P99", *METRICS, "UIWidgets", "UIWidgetsFinal", "UIFrustumMean", "UIFrustumMin", "UIFrustumMax",
                                "CreatedDelta", "UpdateDelta", "RequestDelta", "DrawDelta", "ReadbackPhysicalStartMB", "ReadbackPhysicalEndMB", "ReadbackPhysicalPeakMB", "PostGcWidgets", "PostGcPhysicalMB", "VideoMemoryWarnings", "TexturePoolWarnings", "ShadowOverflowWarnings"):
                        values = [r.get(key) for r in rows]
                        group[key] = statistics.mean(values) if all(v is not None for v in values) else None
                    run_means = [r["Frame"] for r in rows]
                    group["FrameRunMin"] = min(run_means)
                    group["FrameRunMax"] = max(run_means)
                    group["FrameRunStd"] = statistics.stdev(run_means) if len(run_means) > 1 else None
                    groups.append(group)
    for filename, rows in (("run_summary.csv", summaries), ("group_summary.csv", groups)):
        with (root / filename).open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    (root / "summary.json").write_text(json.dumps(dict(runs=summaries, groups=groups), ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    lines = ["# 160 AI / enemy health-bar profile", "", "Only the real rendered client's UI mode varies. DS and the real NullRHI network peer keep UI Off; network actors, HP replication and AI are unchanged. Server CPU is separate.",
             "All positive-frame samples retained, including screenshot/slow frames. Group timings are equal-weight per-run means; group P95/P99 are means of each run's quantiles.", "",
             "| Workload | Rendered role | Rendered UI mode | Measured role | Runs | Frame | P95 | P99 | GT | RT | RHI | GPU | WorldTick | UI frustum mean | RT redraw delta |", "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    def fmt(value):
        return "N/A" if value is None else f"{value:.3f}"
    for g in groups:
        lines.append("| " + " | ".join([g["Workload"], g["RenderRole"], g["Mode"], g["Role"], str(g["Runs"])] +
                     [fmt(g[key]) for key in ("Frame", "P95", "P99", "GT", "RT", "RHI", "GPU", "WorldTick", "UIFrustumMean", "DrawDelta")]) + " |")
    lines += ["", "## Boundaries", "", "- Frustum counts are geometric candidates, not actual unoccluded/pixel-visible bars. World masked quads depth-test in the scene.",
              "- RT redraws count WidgetComponent render-target submissions, not total scene/UI draw calls. Screen mode renders through Slate, not one RT per bar.",
              "- Process physical memory is sampled residency, not per-system allocation ownership or a proof of no leak. An explicit GC AFTER capture checks real remaining Widget UObjects (160 static / 128 after 32 deaths; DS/Off 0); its pause is outside frame timing.",
              "- Missing optional counters remain N/A. Thread times are parallel pipeline stages and must not be added.",
              "- WorldTick is the measured OnWorldTickStart-to-End wall-time on the game thread, not stat-unit GT or OS CPU occupancy. It excludes external frame-limiter waits but includes work/waits inside World::Tick. DS/NullRHI stat-unit GT/RT/RHI/GPU are N/A; FrameTime still includes pacing.",
              "- CPUProcessFraction is the engine's Windows whole-process CPU fraction of total logical-processor capacity (multiply by 100 for percent), not game-thread utilization; UE does not emit it for DedicatedServer, which remains N/A.",
              "- Group frame times weight each repeat equally; FrameRunMin/Max/Std describe variation between run means, not confidence intervals or independent-frame significance.",
              "- VRAM/texture/shadow overflow warnings are counted per run. A captured overload is evidence of a bottleneck, not a healthy or playable-performance certification.",
              "- The open stress arena is intentionally separate from the industrial normal-play map. It isolates a dense UI/AI load; fixed-view results do not certify all gameplay views, packaged capacity, or long-session performance.",
              "- The recorded fixed camera must match across modes/workloads for each rendered role; differing camera geometry rejects aggregation. AI movement is still real and its actual frustum counts remain recorded per run."]
    (root / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Validated {len(cases)} cases / {len(summaries)} peers: {root / 'summary.md'}")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        assert percentile([1., 2., 3., 100.], .99) == 100.
        assert metric_mean([{"x": 1.}, {}], "x") is None
        assert metric_mean([{"x": 1.}, {"x": 3.}], "x") == 2.
        sample = UI_PATTERN.search("PVP_PROFILE UI Role=Client1 Mode=WorldEvent Components=160 Widgets=128 Frustum=12 World=128 Screen=0 Manual=128 Created=160 Updates=2700 Requests=2800 Draws=1000 PhysicalMB=1234.5 PeakPhysicalMB=1400.0 Elapsed=25.0")
        assert sample is not None and len(sample.groups()) == 15 and int(sample.group(8)) == 128
        assert float(sample.group(15)) == 25.0
        assert has_required_timings({"WorldTick": 2.5, "GT": None, "GPU": None}, False)
        assert not has_required_timings({"WorldTick": None, "GT": 10., "GPU": 8.}, False)
        assert not has_required_timings({"WorldTick": 2.5, "GT": None, "GPU": 8.}, True)
        assert not has_required_timings({"WorldTick": 2.5, "GT": 10., "GPU": 0.}, True)
        assert has_required_timings({"WorldTick": 2.5, "GT": 10., "GPU": 8.}, True)
        case = {"Mode": "WorldEvent", "RenderRole": "Client2"}
        assert local_ui_mode(case, "Client2") == "WorldEvent"
        assert local_ui_mode(case, "Client1") == "Off"
        assert local_ui_mode(case, "Server") == "Off"
        print("Enemy UI summarizer self-test: 13 checks passed (no UE, no files).")
    else:
        main(Path(sys.argv[1]).resolve())
