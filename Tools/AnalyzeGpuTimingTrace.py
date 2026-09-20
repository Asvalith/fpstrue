"""Per-scene-root analysis of complete Insights GPU timing events, in seconds.

GPU1/GPU2 are kept separate. Top-level pass intervals are unioned within a frame,
never added to their descendants. Uncovered time is NOT GPU idle time. This tool
does not infer query/fence dependency from temporal overlap with CPU events.
"""
import argparse
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from statistics import mean
from bisect import bisect_right


def percentile(values, q):
    ordered = sorted(values)
    x = (len(ordered) - 1) * q
    lo, hi = math.floor(x), math.ceil(x)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (x - lo)


def stats(values):
    return {"mean_ms": mean(values), "median_ms": percentile(values, .5),
            "p95_ms": percentile(values, .95), "p99_ms": percentile(values, .99)}


def union_seconds(intervals):
    merged = []
    for start, end in sorted(intervals):
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(end, merged[-1][1])
        else:
            merged.append([start, end])
    return sum(end - start for start, end in merged)


def analyze_waits(path, start_trim, end_trim):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        events = [{"thread": r["ThreadId"], "thread_name": r["ThreadName"],
                   "name": r["TimerName"], "start": float(r["StartTime"]),
                   "end": float(r["EndTime"])} for r in csv.DictReader(stream)]
    waits = [e for e in events if e["name"] == "WaitForGatherDynamicMeshElements" and e["thread_name"].startswith("RenderThread")]
    if not waits:
        return {"error": "No render-thread Gather wait scopes"}
    lower = min(e["start"] for e in waits) + start_trim
    upper = max(e["end"] for e in waits) - end_trim
    waits = [e for e in waits if e["start"] >= lower and e["end"] <= upper]
    pipes = defaultdict(list)
    for e in events:
        if e["name"] == "OcclusionCullPipe":
            pipes[e["thread"]].append(e)
    for entries in pipes.values():
        entries.sort(key=lambda e: e["start"])
    starts = {thread: [e["start"] for e in entries] for thread, entries in pipes.items()}
    nested_sync = []
    for e in events:
        if e["name"] == "SyncPoint_Wait" and e["thread"] in pipes:
            i = bisect_right(starts[e["thread"]], e["start"]) - 1
            if i >= 0 and e["end"] <= pipes[e["thread"]][i]["end"] + 1e-7:
                nested_sync.append((e["start"], e["end"]))
    durations = [(e["end"] - e["start"]) * 1000 for e in waits]
    overlaps = [union_seconds([(max(e["start"], s), min(e["end"], t)) for s, t in nested_sync if t > e["start"] and s < e["end"]]) * 1000 for e in waits]
    return {"scope_count": len(waits), "gather_wait": stats(durations),
            "nested_occlusion_sync_overlap_mean_ms": mean(overlaps),
            "time_weighted_overlap_fraction": sum(overlaps) / sum(durations),
            "note": "Wall-time scopes and same-thread nested sync overlap only. New trace TaskIds/fences were not joined. Not GPU execution time or direct proof of a particular GPU pass dependency."}


def analyze(path, start_trim, end_trim):
    tracks = defaultdict(list)
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            start, end = float(row["StartTime"]), float(row["EndTime"])
            if not (math.isfinite(start) and math.isfinite(end) and end >= start):
                raise ValueError("Invalid event interval")
            if not row["ThreadName"].startswith("GPU"):
                raise ValueError("Only complete GPU event exports are supported")
            tracks[row["ThreadName"]].append({"start": start, "end": end,
                "depth": int(row["Depth"]), "name": row["TimerName"]})
    if not tracks:
        raise ValueError("No GPU events; check trace channels and export filters")
    output = {"source": str(path.resolve()), "units": "ms", "tracks": {},
              "method": "Recognized depth-0 deferred scene/view-family roots labelled Frame:N, NOT whole GPU frames. Discard boundary roots then trim. Union sibling pass intervals per root; absent pass in complete root is zero. Never sum parent/child or GPU tracks. Uncovered time is NOT idle. Whole-frame GPUTime comes from CSV. CPU/GPU frame indices are not joined.",
              "start_trim_seconds": start_trim, "end_trim_seconds": end_trim}
    for track, events in tracks.items():
        # Keep exported enumeration order at equal timestamps: parents precede children.
        events.sort(key=lambda e: e["start"])
        frames, active = [], None
        ignored_roots = defaultdict(list)
        for event in events:
            if event["depth"] == 0:
                active = None
                if re.search(r"Frame:\s*\d+", event["name"]):
                    active = dict(event, events=[])
                    frames.append(active)
                else:
                    ignored_roots[event["name"]].append((event["end"] - event["start"]) * 1000)
            elif active is not None:
                if event["end"] > active["end"] + 1e-6:
                    raise ValueError("Child extends outside GPU frame root")
                active["events"].append(event)
        if not frames:
            output["tracks"][track] = {"error": "No recognized GPU frame roots"}
            continue
        lower = frames[0]["start"] + start_trim
        upper = frames[-1]["end"] - end_trim
        # Region filtering can retain a parent but drop children outside the
        # capture interval. Always discard both boundary roots, even with zero trim.
        frames = [f for f in frames[1:-1] if f["start"] >= lower and f["end"] <= upper]
        if not frames:
            raise ValueError("No complete GPU frames after trimming")
        per_frame, names = [], set()
        for frame in frames:
            passes = defaultdict(list)
            for event in frame["events"]:
                if event["depth"] == 1:
                    passes[event["name"]].append((event["start"], event["end"]))
            names.update(passes)
            values = {name: union_seconds(spans) * 1000 for name, spans in passes.items()}
            total = (frame["end"] - frame["start"]) * 1000
            covered = union_seconds([span for spans in passes.values() for span in spans]) * 1000
            if covered > total + .002:
                raise ValueError("Top-level coverage exceeds frame")
            per_frame.append({"gpu_frame": frame["name"], "start": frame["start"],
                "root_ms": total, "uncovered_ms": max(0, total - covered), "passes_ms": values})
        ranking = []
        for name in names:
            vals = [f["passes_ms"].get(name, 0) for f in per_frame]
            ranking.append({"pass": name, "frames_present": sum(name in f["passes_ms"] for f in per_frame), **stats(vals)})
        ranking.sort(key=lambda s: s["mean_ms"], reverse=True)
        ordered = sorted(per_frame, key=lambda f: f["root_ms"])
        selected = {label: ordered[round((len(ordered) - 1) * q)] for label, q in (("median", .5), ("p95", .95))}
        # Children of selected frames remain available for targeted pass drill-down.
        for example in selected.values():
            frame = next(f for f in frames if f["name"] == example["gpu_frame"] and f["start"] == example["start"])
            example["nested_scopes"] = [{"name": e["name"], "depth": e["depth"], "duration_ms": (e["end"]-e["start"])*1000} for e in frame["events"]]
        output["tracks"][track] = {"scene_root_count": len(frames), "scene_root_elapsed": stats([f["root_ms"] for f in per_frame]),
            "ignored_depth0_scopes": {name: {"count": len(values), "total_ms": sum(values)} for name, values in ignored_roots.items()},
            "uncovered_not_idle": stats([f["uncovered_ms"] for f in per_frame]), "top_level_passes": ranking, "selected_frames": selected}
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("events", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start-trim", type=float, default=1)
    parser.add_argument("--end-trim", type=float, default=.5)
    parser.add_argument("--wait-events", type=Path)
    args = parser.parse_args()
    if any(not math.isfinite(x) or x < 0 for x in (args.start_trim, args.end_trim)):
        parser.error("Trimming must be finite and nonnegative")
    if args.output.exists():
        parser.error("Choose a new output file; do not overwrite prior analysis")
    result = analyze(args.events, args.start_trim, args.end_trim)
    if args.wait_events:
        result["cpu_waits"] = analyze_waits(args.wait_events, args.start_trim, args.end_trim)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    for name, track in result["tracks"].items():
        print(json.dumps({"track": name, **{k:v for k,v in track.items() if k not in ("selected_frames", "top_level_passes")},
                          "top_passes": track.get("top_level_passes", [])[:12]}, ensure_ascii=False, indent=2))
    if "cpu_waits" in result:
        print(json.dumps(result["cpu_waits"], indent=2))
