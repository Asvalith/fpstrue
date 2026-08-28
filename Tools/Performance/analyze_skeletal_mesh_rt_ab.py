from __future__ import annotations

import csv
import math
from pathlib import Path
from statistics import fmean


ROOT = Path(__file__).resolve().parents[2]
CSV_FILES = {
    "OFF": ROOT / "Saved/Profiling/CSV/Profile(20260827_211637).csv",
    "ON": ROOT / "Saved/Profiling/CSV/Profile(20260827_223644).csv",
}
OUTPUT = ROOT / "Saved/Profiling/CSV/SkeletalMeshes_AB_summary.txt"

METRICS = {
    "Frame": "frametime",
    "GT": "gamethreadtime",
    "RT": "renderthreadtime",
    "RHI": "rhithreadtime",
    "GPU": "gputime",
    "DrawCalls": "drawcalls",
}


def normalize(value: str) -> str:
    return "".join(character.lower() for character in value if character.isalnum())


def find_column(fieldnames: list[str], target: str) -> str | None:
    normalized = [(normalize(name), name) for name in fieldnames if name]
    for key, original in normalized:
        if key == target:
            return original

    matches = [original for key, original in normalized if target in key]
    return min(matches, key=len) if matches else None


def to_float(value: str | None) -> float | None:
    if value is None:
        return None

    try:
        result = float(value.strip())
    except (TypeError, ValueError):
        return None

    return result if math.isfinite(result) else None


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan

    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]

    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def analyze(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8-sig", errors="replace", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise RuntimeError(f"CSV has no header: {path}")

        columns = {
            name: find_column(reader.fieldnames, target)
            for name, target in METRICS.items()
        }
        if columns["Frame"] is None:
            raise RuntimeError(f"FrameTime column not found: {path}")

        rows: list[dict[str, float | None]] = []
        for raw_row in reader:
            frame = to_float(raw_row.get(columns["Frame"]))
            if frame is None or frame <= 0.0 or frame >= 1000.0:
                continue

            row = {"Frame": frame}
            for name in ("GT", "RT", "RHI", "GPU", "DrawCalls"):
                column = columns[name]
                row[name] = to_float(raw_row.get(column)) if column else None
            rows.append(row)

    if len(rows) < 20:
        raise RuntimeError(f"Too few valid frames in {path}: {len(rows)}")

    trim_count = int(len(rows) * 0.05)
    sampled_rows = rows[trim_count:-trim_count] if trim_count else rows

    result: dict[str, object] = {
        "source_frames": len(rows),
        "sampled_frames": len(sampled_rows),
        "columns": columns,
    }

    for name in METRICS:
        values = [row[name] for row in sampled_rows if row.get(name) is not None]
        numeric_values = [float(value) for value in values]
        if numeric_values:
            result[name] = {
                "avg": fmean(numeric_values),
                "p95": percentile(numeric_values, 0.95),
                "p99": percentile(numeric_values, 0.99),
            }

    frame_values = [float(row["Frame"]) for row in sampled_rows]
    result["duration_seconds"] = sum(frame_values) / 1000.0
    result["fps_avg"] = fmean(1000.0 / frame for frame in frame_values)
    result["fps_5_low"] = 1000.0 / percentile(frame_values, 0.95)
    result["fps_1_low"] = 1000.0 / percentile(frame_values, 0.99)
    return result


def value(result: dict[str, object], metric: str, statistic: str = "avg") -> float:
    metric_result = result.get(metric)
    if not isinstance(metric_result, dict):
        return math.nan
    raw_value = metric_result.get(statistic)
    return float(raw_value) if isinstance(raw_value, (int, float)) else math.nan


def delta_percent(off_value: float, on_value: float) -> float:
    if not math.isfinite(off_value) or off_value == 0.0 or not math.isfinite(on_value):
        return math.nan
    return (on_value / off_value - 1.0) * 100.0


def format_number(number: float, suffix: str = "") -> str:
    return f"{number:.2f}{suffix}" if math.isfinite(number) else "N/A"


def main() -> None:
    results = {name: analyze(path) for name, path in CSV_FILES.items()}
    off = results["OFF"]
    on = results["ON"]

    duration_delta = delta_percent(
        float(off["duration_seconds"]), float(on["duration_seconds"])
    )
    draw_delta = delta_percent(value(off, "DrawCalls"), value(on, "DrawCalls"))
    comparable = abs(duration_delta) <= 20.0 and (
        not math.isfinite(draw_delta) or abs(draw_delta) <= 10.0
    )

    lines = [
        "Skeletal Mesh Ray Tracing A/B",
        "OFF: r.RayTracing.Geometry.SkeletalMeshes=0",
        "ON : r.RayTracing.Geometry.SkeletalMeshes=1",
        "Sampling: discard invalid frames, then trim first/last 5% of valid frames.",
        "",
        "Metric                         OFF          ON       ON vs OFF",
        "----------------------------------------------------------------",
    ]

    rows = [
        ("Duration (s)", float(off["duration_seconds"]), float(on["duration_seconds"]), ""),
        ("Frame avg (ms)", value(off, "Frame"), value(on, "Frame"), ""),
        ("Frame P95 (ms)", value(off, "Frame", "p95"), value(on, "Frame", "p95"), ""),
        ("Frame P99 (ms)", value(off, "Frame", "p99"), value(on, "Frame", "p99"), ""),
        ("FPS avg", float(off["fps_avg"]), float(on["fps_avg"]), ""),
        ("FPS 5% low", float(off["fps_5_low"]), float(on["fps_5_low"]), ""),
        ("FPS 1% low", float(off["fps_1_low"]), float(on["fps_1_low"]), ""),
        ("GT avg (ms)", value(off, "GT"), value(on, "GT"), ""),
        ("RT avg (ms)", value(off, "RT"), value(on, "RT"), ""),
        ("RHI avg (ms)", value(off, "RHI"), value(on, "RHI"), ""),
        ("GPU avg (ms)", value(off, "GPU"), value(on, "GPU"), ""),
        ("DrawCalls avg", value(off, "DrawCalls"), value(on, "DrawCalls"), ""),
    ]

    for label, off_value, on_value, suffix in rows:
        delta = delta_percent(off_value, on_value)
        lines.append(
            f"{label:<27} {format_number(off_value, suffix):>10} "
            f"{format_number(on_value, suffix):>10} "
            f"{format_number(delta, '%'):>13}"
        )

    lines.extend(
        [
            "",
            f"Source frames: OFF={off['source_frames']}, ON={on['source_frames']}",
            f"Sampled frames: OFF={off['sampled_frames']}, ON={on['sampled_frames']}",
            f"Duration delta: {format_number(duration_delta, '%')}",
            f"DrawCalls delta: {format_number(draw_delta, '%')}",
            f"Comparable: {'YES' if comparable else 'NO'}",
            "",
            "Detected columns:",
            f"OFF: {off['columns']}",
            f"ON : {on['columns']}",
        ]
    )

    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
