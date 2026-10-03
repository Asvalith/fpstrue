"""Inspect render-wait timing exports, then join real TaskTrace waits by IDs.

Inputs are Unreal Insights timing CSVs and the local TraceAuditTool CSVs.
All input timestamps are seconds. This script reports wall time, not CPU cycles.
"""
import argparse
import csv
import json
from bisect import bisect_left, bisect_right
from collections import defaultdict
from pathlib import Path


def rows(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        yield from csv.DictReader(stream)


def event(row):
    return {
        "thread_id": int(row["ThreadId"]), "thread": row["ThreadName"],
        "name": row["TimerName"], "start": float(row["StartTime"]),
        "end": float(row["EndTime"]), "ms": float(row["Duration"]) * 1000,
        "depth": int(row["Depth"]),
    }


def analyze(root, timing_only=False):
    timing = root / "TimingExport"
    audit = root / "TaskAudit"
    scopes = []
    rt_stats = defaultdict(lambda: [0, 0.0, 0.0])
    for row in rows(timing / "RenderThreadEvents.csv"):
        current = event(row)
        stat = rt_stats[current["name"]]
        stat[0] += 1
        stat[1] += current["ms"]
        stat[2] = max(stat[2], current["ms"])
        if current["name"] == "WaitForGatherDynamicMeshElements":
            scopes.append(current)
    if not scopes:
        raise RuntimeError("No WaitForGatherDynamicMeshElements scopes in this export")
    lower = min(item["start"] for item in scopes) + 1.0
    upper = max(item["end"] for item in scopes) - 0.5
    steady = sorted((x for x in scopes if x["start"] >= lower and x["end"] <= upper), key=lambda x: x["ms"])
    selected = []
    for label, percentile in [("median", 0.5), ("p95", 0.95)]:
        selected.append({"label": label, "scope": steady[int((len(steady) - 1) * percentile)], "rt_children": [], "overlap_events": [], "waits": []})
    for row in rows(timing / "RenderThreadEvents.csv"):
        current = event(row)
        for example in selected:
            scope = example["scope"]
            if current["start"] >= scope["start"] - 1e-7 and current["end"] <= scope["end"] + 1e-7 and current["ms"] >= 0.05:
                example["rt_children"].append(current)
    occlusion_scopes = defaultdict(list)
    sync_scopes = defaultdict(list)
    for row in rows(timing / "RelevantEvents.csv"):
        current = event(row)
        if current["name"] == "OcclusionCullPipe":
            occlusion_scopes[current["thread_id"]].append(current)
        elif current["name"] == "SyncPoint_Wait":
            sync_scopes[current["thread_id"]].append(current)
        if current["ms"] < 0.1:
            continue
        for example in selected:
            scope = example["scope"]
            overlap = min(current["end"], scope["end"]) - max(current["start"], scope["start"])
            if current["thread_id"] != scope["thread_id"] and overlap > 0.0001:
                example["overlap_events"].append(current | {"overlap_ms": overlap * 1000})
    # Retain only SyncPoint waits nested in OcclusionCullPipe on the SAME thread.
    # Union overlapping intervals before attribution: parallel waits are not additive.
    query_intervals = []
    for thread_id, pipes in occlusion_scopes.items():
        pipes.sort(key=lambda x: x["start"])
        pipe_starts = [x["start"] for x in pipes]
        for sync in sync_scopes[thread_id]:
            i = bisect_right(pipe_starts, sync["start"]) - 1
            if i >= 0 and sync["end"] <= pipes[i]["end"] + 1e-7:
                query_intervals.append((sync["start"], sync["end"]))
    merged_queries = []
    for start, end in sorted(query_intervals):
        if merged_queries and start <= merged_queries[-1][1]:
            merged_queries[-1][1] = max(merged_queries[-1][1], end)
        else:
            merged_queries.append([start, end])
    query_starts = [x[0] for x in merged_queries]
    overlaps = []
    for scope in steady:
        first = max(0, bisect_right(query_starts, scope["start"]) - 1)
        stop = bisect_left(query_starts, scope["end"])
        overlap = sum(max(0, min(end, scope["end"]) - max(start, scope["start"])) for start, end in merged_queries[first:stop])
        overlaps.append({"start": scope["start"], "rt_wait_ms": scope["ms"], "occlusion_sync_overlap_ms": overlap * 1000})
    wait_ids = set()
    for row in (() if timing_only else rows(audit / "waits.csv")):
        if not row["Finished"]:
            continue
        start, end = float(row["Started"]), float(row["Finished"])
        for example in selected:
            scope = example["scope"]
            if int(row["ThreadId"]) == scope["thread_id"] and start >= scope["start"] - 1e-6 and end <= scope["end"] + 1e-6:
                example["waits"].append(dict(row))
                wait_ids.add(row["WaitId"])
    wait_to_tasks = defaultdict(list)
    for row in (() if timing_only else rows(audit / "wait_tasks.csv")):
        if row["WaitId"] in wait_ids:
            wait_to_tasks[row["WaitId"]].append(row["TaskId"])
    target_ids = {task_id for task_ids in wait_to_tasks.values() for task_id in task_ids}
    # Also retain tasks whose execution overlaps the selected windows; this permits
    # inspecting producer candidates without inventing missing prerequisite edges.
    tasks = {}
    for row in (() if timing_only else rows(audit / "tasks.csv")):
        relevant = row["TaskId"] in target_ids
        if not relevant and row["Started"] and row["Finished"]:
            start, end = float(row["Started"]), float(row["Finished"])
            relevant = any(end >= e["scope"]["start"] - 0.01 and start <= e["scope"]["end"] + 0.001 for e in selected)
        if relevant:
            tasks[row["TaskId"]] = dict(row)
    related = []
    for row in (() if timing_only else rows(audit / "task_relations.csv")):
        if row["TaskId"] in target_ids:
            related.append(dict(row))
    for example in selected:
        example["rt_children"].sort(key=lambda x: (x["start"], x["depth"]))
        example["overlap_events"].sort(key=lambda x: (-x["overlap_ms"], x["thread_id"]))
        for wait in example["waits"]:
            wait["awaited_ids"] = wait_to_tasks[wait["WaitId"]]
    result = {
        "trace_root": str(root), "task_audit_included": not timing_only, "steady_scope_count": len(steady),
        "wait_scope_mean_ms": sum(x["ms"] for x in steady) / len(steady),
        "occlusion_sync_overlap": {
            "nested_query_sync_count": len(query_intervals),
            "mean_overlap_ms": sum(x["occlusion_sync_overlap_ms"] for x in overlaps) / len(overlaps),
            "time_weighted_fraction": sum(x["occlusion_sync_overlap_ms"] for x in overlaps) / max(1e-12, sum(x["rt_wait_ms"] for x in overlaps)),
            "scopes_with_overlap": sum(x["occlusion_sync_overlap_ms"] > 0 for x in overlaps),
            "scopes_at_least_90_percent_overlap": sum(x["occlusion_sync_overlap_ms"] >= 0.9 * x["rt_wait_ms"] for x in overlaps),
            "note": "Same-thread nested scopes plus temporal overlap; selected task IDs and engine source independently validate the dependency. Not a GPU-pass execution measurement.",
        },
        "rt_wait_stats": {name: {"count": stat[0], "mean_ms": stat[1] / stat[0], "max_ms": stat[2]} for name, stat in rt_stats.items() if any(word in name for word in ("WaitForGather", "FinishDynamicMesh", "Occlusion", "Visibility", "SyncPoint", "GatherDynamicMesh", "ComputeRelevance"))},
        "selected": selected, "awaited_tasks": {key: tasks.get(key) for key in target_ids},
        "awaited_relations": related, "nearby_tasks": tasks,
    }
    destination = root / "render_wait_inspection.json"
    destination.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(destination), "steady_scopes": len(steady), "mean_ms": result["wait_scope_mean_ms"], "selected": [{"label": e["label"], "scope": e["scope"], "wait_count": len(e["waits"])} for e in selected]}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--timing-only", action="store_true", help="Inspect scope timing without claiming a TaskTrace dependency join")
    args = parser.parse_args()
    analyze(args.root, args.timing_only)
