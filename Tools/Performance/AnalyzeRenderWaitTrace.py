"""Inspect render-wait timing exports, then join real TaskTrace waits by IDs.

Inputs are Unreal Insights timing CSVs and the local TraceAuditTool CSVs.
All input timestamps are seconds. This script reports wall time, not CPU cycles.
"""
import argparse
import csv
import json
import math
import tempfile
from bisect import bisect_left, bisect_right
from collections import defaultdict
from pathlib import Path


def rows(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        yield from csv.DictReader(stream)


def event(row):
    start, end, duration = float(row["StartTime"]), float(row["EndTime"]), float(row["Duration"])
    depth = int(row["Depth"])
    if not all(math.isfinite(x) for x in (start, end, duration)) or start < 0 or end < start or duration < 0 or depth < 0:
        raise ValueError("Invalid timing event interval/depth")
    if abs(duration - (end - start)) > 0.000002:
        raise ValueError("Duration differs from event endpoints")
    return {
        "thread_id": int(row["ThreadId"]), "thread": row["ThreadName"],
        "timer_id": int(row["TimerId"]), "name": row["TimerName"], "start": start,
        "end": end, "ms": (end - start) * 1000, "depth": depth,
    }


def write_result(path, result):
    # A fresh analysis must not silently replace an earlier interpretation.
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)


def analyze(root, timing_only=False, output=None):
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
    if not steady:
        raise RuntimeError("Legacy trimming left no scopes; use explicit --start-time/--end-time for a short window")
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
            "note": "Same-thread scope containment and temporal overlap only. Overlap is a producer candidate, not proof of a dependency. Task ID mappings, when present, are reported separately; this tool does not certify their completeness. Not a GPU-pass execution measurement.",
        },
        "rt_wait_stats": {name: {"count": stat[0], "mean_ms": stat[1] / stat[0], "max_ms": stat[2]} for name, stat in rt_stats.items() if any(word in name for word in ("WaitForGather", "FinishDynamicMesh", "Occlusion", "Visibility", "SyncPoint", "GatherDynamicMesh", "ComputeRelevance"))},
        "selected": selected, "awaited_tasks": {key: tasks.get(key) for key in target_ids},
        "awaited_relations": related, "nearby_tasks": tasks,
        "dependency_evidence": {"task_audit_loaded": not timing_only,
            "unresolved_awaited_task_ids": sorted(key for key in target_ids if key not in tasks),
            "status": "not_joined_timing_only" if timing_only else "mappings_reported_not_independently_certified"},
        "limitations": ["Historical region analysis retains its original display thresholds and trimming; it does not split nested task execution from blocking scopes.",
                        "Scope wall durations do not establish on-core, ready, idle or OS-blocked time."],
    }
    destination = output or root / "render_wait_inspection.json"
    write_result(destination, result)
    print(json.dumps({"output": str(destination), "steady_scopes": len(steady), "mean_ms": result["wait_scope_mean_ms"], "selected": [{"label": e["label"], "scope": e["scope"], "wait_count": len(e["waits"])} for e in selected]}, ensure_ascii=False, indent=2))


EVENT_COLUMNS = ("ThreadId", "ThreadName", "TimerId", "TimerName", "StartTime", "EndTime", "Duration", "Depth")
# Source-verified UE5.5 explicit wait-call scopes. The function wrappers around
# them may help execute tasks; never classify every name containing Wait as a
# blocking call. Even these intervals are scope wall time, not OS blocked time.
BLOCKING_SOURCES = {
    "WaitForTasks": "/async/taskgraph.cpp",
    "FTaskBase::WaitImpl_StateChangeEvent_WaitFor": "/tasks/taskprivate.cpp",
}
TASK_EXECUTION_SCOPES = {"ExecuteTask", "ExecuteForegroundTask", "ExecuteBackgroundTask"}
EPSILON = 0.0000002


def merged_intervals(intervals):
    result = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1][1] = max(result[-1][1], end)
        else:
            result.append([start, end])
    return result


def interval_seconds(intervals):
    return sum(end - start for start, end in merged_intervals(intervals))


def intersection_seconds(left, right):
    left, right = merged_intervals(left), merged_intervals(right)
    i = j = 0
    total = 0.0
    while i < len(left) and j < len(right):
        total += max(0.0, min(left[i][1], right[j][1]) - max(left[i][0], right[j][0]))
        if left[i][1] <= right[j][1]:
            i += 1
        else:
            j += 1
    return total


def checked_rows(path, required):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None or len(reader.fieldnames) != len(set(reader.fieldnames)) or not set(required).issubset(reader.fieldnames):
            raise ValueError(f"Missing/duplicate CSV columns: {path}")
        yield from reader


def public_scope(node):
    return {key: node[key] for key in (
        "thread_id", "thread", "timer_id", "name", "start", "end", "ms", "depth",
        "raw_start", "raw_end", "clipped_at_start", "clipped_at_end", "hierarchy_complete",
        "exclusive_scope_wall_ms")}


def analyze_window(root, start, end, alignment_json=None, max_events=500000):
    """Analyze a bounded all-scope CPU export; no TaskAudit or OS data implied."""
    if not (math.isfinite(start) and math.isfinite(end) and 0 <= start < end):
        raise ValueError("Explicit window must be finite and have 0 <= start < end")
    if max_events < 1:
        raise ValueError("max-events must be positive")
    timing = root if (root / "CPUEvents.csv").is_file() else root / "TimingExport"
    source = timing / "CPUEvents.csv"
    if not source.is_file():
        raise ValueError("Explicit mode requires CPUEvents.csv from an all-timer short-window export; filtered RelevantEvents is insufficient")
    manifest_path = timing / "export_manifest.json"
    provenance = None
    if manifest_path.is_file():
        provenance = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        if not provenance.get("Valid") or provenance.get("Mode") != "TimeWindow":
            raise ValueError("Export manifest is invalid or not a complete all-scope time-window export")
        if start < float(provenance["StartTime"]) - EPSILON or end > float(provenance["EndTime"]) + EPSILON:
            raise ValueError("Analysis window extends outside the exported window")
    metadata = {int(row["Id"]): row for row in checked_rows(timing / "Timers.csv", ("Id", "Type", "Name", "File", "Line"))}
    nodes = []
    per_thread = defaultdict(list)
    missing_metadata = 0
    for ordinal, row in enumerate(checked_rows(source, EVENT_COLUMNS)):
        if ordinal >= max_events:
            raise ValueError("Event budget exceeded; export a shorter window instead of reading a multi-GB event dump")
        node = event(row)
        if node["end"] < start or node["start"] > end:
            continue
        timer = metadata.get(node["timer_id"])
        if node["thread"].startswith("GPU") or timer is not None and timer["Type"] == "GPU":
            raise ValueError("CPUEvents contains GPU events; use a CPU-thread all-scope export")
        if timer is not None and timer["Name"] != node["name"]:
            raise ValueError("Timer identity/name differs between Timers.csv and CPUEvents.csv")
        missing_metadata += timer is None
        node.update(raw_start=node["start"], raw_end=node["end"],
                    clipped_at_start=node["start"] < start, clipped_at_end=node["end"] > end,
                    ordinal=ordinal, children=[], parent=None, hierarchy_complete=True)
        node["start"], node["end"] = max(start, node["start"]), min(end, node["end"])
        node["ms"] = (node["end"] - node["start"]) * 1000
        name = node["name"]
        expected_source = BLOCKING_SOURCES.get(name)
        filename = timer.get("File", "").replace("\\", "/").lower() if timer else ""
        node["explicit_blocking"] = bool(timer and timer["Type"] == "CPU" and expected_source and filename.endswith(expected_source))
        node["task_execution"] = bool(timer and timer["Type"] == "CPU" and name in TASK_EXECUTION_SCOPES)
        nodes.append(node)
        per_thread[node["thread_id"]].append(len(nodes) - 1)
    if not nodes:
        raise ValueError("No intersecting CPU events")

    issues = []
    for identifiers in per_thread.values():
        # The provider exports entry order. Equal rounded starts can include a
        # zero-duration sibling BEFORE the actual parent of the next child.
        # Sorting those siblings by descending end reverses them and falsely
        # leaves the zero-duration scope on the parent stack. Preserve provider
        # ordinal for timestamp ties; retain every event and existing tolerance.
        identifiers.sort(key=lambda i: (nodes[i]["raw_start"], nodes[i]["ordinal"]))
        stack = []
        for identifier in identifiers:
            current = nodes[identifier]
            while stack and (nodes[stack[-1]]["depth"] >= current["depth"] or nodes[stack[-1]]["raw_end"] < current["raw_start"] - EPSILON):
                stack.pop()
            if current["depth"] > 0:
                if not stack or nodes[stack[-1]]["depth"] != current["depth"] - 1:
                    current["hierarchy_complete"] = False
                    issues.append({"event_ordinal": current["ordinal"], "reason": "missing_parent_depth"})
                if stack:
                    parent = nodes[stack[-1]]
                    if current["raw_end"] > parent["raw_end"] + EPSILON:
                        raise ValueError("Crossing same-thread scopes: not a valid nested timing hierarchy")
                    current["parent"] = stack[-1]
                    current["hierarchy_complete"] &= parent["hierarchy_complete"]
                    parent["children"].append(identifier)
            stack.append(identifier)
    for node in nodes:
        child_union = interval_seconds((nodes[i]["start"], nodes[i]["end"]) for i in node["children"])
        node["exclusive_scope_wall_ms"] = max(0.0, node["ms"] - child_union * 1000)

    waits = []
    for identifier, node in enumerate(nodes):
        if "Wait" not in node["name"] and node["name"] not in ("Frame Sync Time", "Game thread idle time"):
            continue
        if node["ms"] == 0:
            continue
        descendants, pending = [], list(node["children"])
        while pending:
            child = pending.pop()
            descendants.append(nodes[child])
            pending.extend(nodes[child]["children"])
        task_intervals = [(item["start"], item["end"]) for item in descendants if item["task_execution"]]
        block_intervals = [(item["start"], item["end"]) for item in [node, *descendants] if item["explicit_blocking"]]
        task_wall = interval_seconds(task_intervals)
        block_wall = interval_seconds(block_intervals)
        task_block_overlap = intersection_seconds(task_intervals, block_intervals)
        classified_union = interval_seconds(task_intervals + block_intervals)
        summary = public_scope(node)
        summary.update(event_id=identifier, wrapper_scope_wall_ms=node["ms"],
            nested_task_scope_wall_ms=task_wall * 1000,
            explicit_blocking_scope_wall_ms=block_wall * 1000,
            blocking_inside_nested_task_scope_ms=task_block_overlap * 1000,
            nested_task_excluding_explicit_blocking_wall_ms=max(0.0, task_wall - task_block_overlap) * 1000,
            unknown_remainder_scope_wall_ms=max(0.0, node["ms"] - classified_union * 1000),
            decomposition_complete=node["hierarchy_complete"] and all(x["hierarchy_complete"] for x in descendants),
            source_verified_blocking_scope_count=sum(x["explicit_blocking"] for x in [node, *descendants]),
            unverified_blocking_name_candidates=sum(x["name"] in BLOCKING_SOURCES and not x["explicit_blocking"] for x in [node, *descendants]))
        waits.append(summary)

    # CPU timelines share a clock, not a dependency ID. Report a bounded list of
    # concurrent candidates for representative waits, never a producer verdict.
    selected = []
    for thread_prefix, preferred_name in (("GameThread", "Frame Sync Time"), ("RenderThread", "WaitForGatherDynamicMeshElements"), ("RHIThread", "Submission_Wait")):
        thread_waits = [w for w in waits if w["thread"].startswith(thread_prefix)]
        preferred = [w for w in thread_waits if w["name"] == preferred_name]
        candidates = sorted(preferred or thread_waits, key=lambda w: w["ms"])
        if not candidates:
            continue
        for label, q in (("median_wait_scope", .5), ("p95_wait_scope", .95), ("max_wait_scope", 1.0)):
            chosen = candidates[math.ceil((len(candidates) - 1) * q)]
            overlap = []
            for other in nodes:
                duration = min(chosen["end"], other["end"]) - max(chosen["start"], other["start"])
                if other["thread_id"] != chosen["thread_id"] and duration > 0:
                    overlap.append(public_scope(other) | {"overlap_scope_wall_ms": duration * 1000})
            overlap.sort(key=lambda x: (-x["overlap_scope_wall_ms"], -x["depth"]))
            selected.append({"selection": label, "selection_population": preferred_name if preferred else "all wait-like scopes on this thread", "scope": chosen,
                "overlap_candidates": overlap[:25], "overlap_candidate_count": len(overlap),
                "candidate_list_truncated_for_display": len(overlap) > 25,
                "dependency_status": "Temporal overlap only; no TaskId/fence dependency has been joined."})

    ticks = [node for node in nodes if node["thread"] == "GameThread" and node["name"] == "FEngineLoop::Tick"
             and not node["clipped_at_start"] and not node["clipped_at_end"]]
    ticks.sort(key=lambda node: node["start"])
    observed_complete_bounds = bool(ticks and abs(ticks[0]["start"] - start) <= EPSILON and abs(ticks[-1]["end"] - end) <= EPSILON)
    alignment_context = None
    if alignment_json is not None:
        alignment_context = json.loads(Path(alignment_json).read_text(encoding="utf-8-sig"))
        validated = alignment_context.get("validated_full_frame_window", {})
        if not alignment_context.get("valid") or not validated or start < float(validated["start_time_seconds"]) - EPSILON or end > float(validated["end_time_seconds"]) + EPSILON:
            raise ValueError("Alignment reference is invalid or does not contain the requested window")
    frames = []
    gc_scopes = [node for node in nodes if node["thread"] == "GameThread" and node["name"] == "GC Mark Time"]
    for tick in ticks:
        frames.append({"start": tick["start"], "end": tick["end"], "engine_tick_wall_ms": tick["ms"],
            "gc_mark_scope_present": any(gc["start"] >= tick["start"] - EPSILON and gc["end"] <= tick["end"] + EPSILON for gc in gc_scopes)})
    selected_frames = []
    normal_frames = sorted((frame for frame in frames if not frame["gc_mark_scope_present"]), key=lambda frame: frame["engine_tick_wall_ms"])
    for label, q in (("median_non_gc_engine_tick", .5), ("p95_non_gc_engine_tick", .95), ("max_non_gc_engine_tick", 1.0)):
        if normal_frames:
            selected_frames.append({"selection": label, "frame": normal_frames[math.ceil((len(normal_frames)-1)*q)]})
    gc_frames = [frame for frame in frames if frame["gc_mark_scope_present"]]
    if gc_frames:
        selected_frames.append({"selection": "max_gc_engine_tick", "frame": max(gc_frames, key=lambda frame: frame["engine_tick_wall_ms"])})
    for example in selected_frames:
        frame = example["frame"]
        example["overlapping_wait_scope_event_ids"] = [w["event_id"] for w in waits if w["end"] > frame["start"] and w["start"] < frame["end"]]
        example["boundary"] = "Same-clock overlap with a complete engine Tick; no GPU-frame or dependency join implied."
    rankings = defaultdict(dict)
    for node in nodes:
        entry = rankings[str(node["thread_id"])].setdefault(node["timer_id"], {
            "thread": node["thread"], "timer_id": node["timer_id"], "name": node["name"],
            "event_count": 0, "inclusive_scope_wall_ms": 0.0, "exclusive_scope_wall_ms": 0.0})
        entry["event_count"] += 1
        entry["inclusive_scope_wall_ms"] += node["ms"]
        entry["exclusive_scope_wall_ms"] += node["exclusive_scope_wall_ms"]
    return {
        "schema_version": 2, "mode": "explicit_cpu_window", "source": str(source.resolve()),
        "export_manifest": provenance, "timestamp_units": "seconds", "scope_units": "ms",
        "window": {"start": start, "end": end, "wall_ms": (end-start)*1000},
        "frame_alignment": {"complete_engine_ticks_observed": len(ticks),
            "window_matches_complete_engine_tick_bounds": observed_complete_bounds,
            "native_clock_alignment_reference_validated": alignment_context is not None,
            "alignment_reference": str(alignment_json) if alignment_json else None,
            "raw_frame_ids_joined": False, "gpu_frame_ids_joined": False,
            "note": "Engine Tick bounds are observed, not assumed from a Region. Native GFrameCounter and GPU frame indices are not inferred."},
        "hierarchy": {"event_count": len(nodes), "all_timer_scopes_exported": provenance is not None,
            "complete": not issues, "missing_timer_metadata_events": missing_metadata,
            "issue_count": len(issues), "first_issues": issues[:25],
            "boundary_events_clipped": sum(n["clipped_at_start"] or n["clipped_at_end"] for n in nodes)},
        "thread_coverage": {"observed_names": sorted({node["thread"] for node in nodes}),
            "missing_primary_prefixes": [prefix for prefix in ("GameThread", "RenderThread", "RHIThread") if not any(node["thread"].startswith(prefix) for node in nodes)],
            "note": "An absent thread is unobserved/not applicable, not zero-cost. Check role, RHI configuration and trace coverage."},
        "method": "Per-thread depth/containment tree; equal-start events retain provider CSV entry order, including zero-duration siblings and parent/child scopes. Clip boundary intersections; subtract direct-child interval unions for exclusive scope wall time. Task and explicit-blocking intervals are unioned; blocking within task scopes is removed from the task-minus-blocking field. No duration threshold is applied to analysis events.",
        "wait_scope_count": len(waits), "wait_scopes": waits, "selected_wait_examples": selected,
        "engine_frames": frames, "selected_frame_examples": selected_frames,
        "per_thread_top_exclusive_scope_wall": {thread: sorted(entries.values(), key=lambda entry: -entry["exclusive_scope_wall_ms"])[:25] for thread, entries in rankings.items()},
        "per_thread_wait_wrapper_union_ms": {str(thread): interval_seconds((w["start"], w["end"]) for w in waits if w["thread_id"] == thread)*1000 for thread in per_thread},
        "dependency_evidence": {"task_audit_loaded": False, "task_id_join_performed": False,
            "status": "not_proven_by_timing_overlap"},
        "scheduling_evidence": {"available": False, "status": "No context-switch/OS scheduling data consumed; no on-core, ready or OS-blocked attribution is emitted."},
        "gpu_total_frame_time": {"available": False, "milliseconds": None,
            "reason": "CPU hierarchy exports do not establish a complete GPU frame. GPU timestamp scopes/Unaccounted roots cannot fill this gap."},
        "limitations": ["Nested wrappers overlap; never add their reported durations or parallel-thread totals as frame time.",
            "Task-minus-explicit-blocking scope wall time is not guaranteed CPU work: uninstrumented waits and preemption can remain.",
            "Unknown remainder is unclassified scope wall time, not idle time. A source-verified wait-call scope is not an OS wait-state measurement.",
            "Hierarchy completeness checks cannot detect timer scopes that were never instrumented. Missing depth parents or metadata lower attribution confidence.",
            "Representative wait percentiles describe waits, not frame percentiles. Complete engine Tick examples, excluding/separating GC, are selected independently; Tick duration is not Present latency.",
            "Without an export manifest, an apparently intact depth tree does not certify that all timer scopes were exported."]}


def self_test():
    """Small disposable fixtures only; never launches Unreal or reads real traces."""
    with tempfile.TemporaryDirectory(prefix="RenderWaitTraceSelfTest_") as directory:
        root = Path(directory)
        names = {1: "FEngineLoop::Tick", 2: "WaitUntilTasksComplete", 3: "ExecuteTask", 4: "WaitForTasks", 5: "Work", 6: "WaitForGatherDynamicMeshElements"}
        with (root / "Timers.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("Id", "Type", "Name", "File", "Line"))
            for identifier, name in names.items():
                writer.writerow((identifier, "CPU", name, "D:/Engine/Source/Runtime/Core/Private/Async/TaskGraph.cpp" if name == "WaitForTasks" else "", 0))
        fixture = [(2,"GameThread",1,0,20,0),(2,"GameThread",2,2,12,1),(2,"GameThread",3,3,9,2),
            (2,"GameThread",5,3,4,3),(2,"GameThread",4,4,6,3),(2,"GameThread",3,6,8,3),
            (2,"GameThread",5,6.5,7,4),(2,"GameThread",5,8,9,3),(2,"GameThread",5,10,11,2),
            (49,"RenderThread 0",5,0,20,0),(49,"RenderThread 0",6,5,10,1),(49,"RenderThread 0",4,6,8,2),
            (48,"RHIThread",5,0,20,0),(48,"RHIThread",2,1,14,1),(48,"RHIThread",3,1.5,3,2),(48,"RHIThread",4,11,14,2)]
        def save_fixture(items):
            with (root / "CPUEvents.csv").open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream)
                writer.writerow(EVENT_COLUMNS)
                for thread, thread_name, timer, start, end, depth in items:
                    writer.writerow((thread, thread_name, timer, names[timer], start, end, end-start, depth))
        save_fixture(fixture)
        manifest = {"Valid": True, "Mode": "TimeWindow", "StartTime": 2, "EndTime": 12}
        (root / "export_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        result = analyze_window(root, 2, 12)
        wait = next(w for w in result["wait_scopes"] if w["thread"] == "GameThread" and w["name"] == "WaitUntilTasksComplete")
        expected = {"wrapper_scope_wall_ms":10000, "nested_task_scope_wall_ms":6000,
            "explicit_blocking_scope_wall_ms":2000, "blocking_inside_nested_task_scope_ms":2000,
            "nested_task_excluding_explicit_blocking_wall_ms":4000, "unknown_remainder_scope_wall_ms":4000,
            "exclusive_scope_wall_ms":3000}
        for key, value in expected.items():
            assert abs(wait[key]-value) < 1e-6, (key,wait[key],value)
        assert result["hierarchy"]["complete"]
        assert not result["frame_alignment"]["window_matches_complete_engine_tick_bounds"]
        rhi = next(w for w in result["wait_scopes"] if w["thread"] == "RHIThread" and w["name"] == "WaitUntilTasksComplete")
        assert rhi["clipped_at_start"] and rhi["clipped_at_end"]
        assert abs(rhi["nested_task_scope_wall_ms"]-1000) < 1e-6
        assert abs(rhi["explicit_blocking_scope_wall_ms"]-1000) < 1e-6
        assert not result["scheduling_evidence"]["available"] and not result["gpu_total_frame_time"]["available"]
        assert all("not" in x["dependency_status"].lower() or "no taskid" in x["dependency_status"].lower() for x in result["selected_wait_examples"])
        path = root / "result.json"
        write_result(path, result)
        try:
            write_result(path, result)
        except FileExistsError:
            pass
        else:
            raise AssertionError("Analysis overwrote existing evidence")
        for bounds, budget in [((12,2),500000),((1,12),500000),((2,12),2)]:
            try:
                analyze_window(root, *bounds, max_events=budget)
            except ValueError:
                pass
            else:
                raise AssertionError("Invalid window/event budget was accepted")
        malformed = dict(zip(EVENT_COLUMNS, (2,"GameThread",1,"Frame","nan",3,1,0)))
        try:
            event(malformed)
        except ValueError:
            pass
        else:
            raise AssertionError("NaN event accepted")
        manifest["StartTime"], manifest["EndTime"] = 0, 20
        (root / "export_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        complete = analyze_window(root, 0, 20)
        assert complete["frame_alignment"]["window_matches_complete_engine_tick_bounds"]
        assert not complete["frame_alignment"]["native_clock_alignment_reference_validated"]
        assert len(complete["selected_frame_examples"]) == 3
        save_fixture([(2,"GameThread",2,2,12,1)])
        incomplete = analyze_window(root, 2, 12)
        assert not incomplete["hierarchy"]["complete"]
        assert not incomplete["wait_scopes"][0]["decomposition_complete"]
        # Real exporter tie: zero sibling and following nonzero sibling share
        # a start/depth; the latter, not the zero sibling, owns the next child.
        # A zero parent with a zero child is valid and both remain in the tree.
        save_fixture([(2,"GameThread",1,0,20,0), (2,"GameThread",5,2,2,1),
            (2,"GameThread",2,2,8,1), (2,"GameThread",3,2.0000001,3,2),
            (2,"GameThread",5,4,4,2), (2,"GameThread",5,4,4,3), (2,"GameThread",5,4,5,2)])
        tied = analyze_window(root, 0, 20)
        assert tied["hierarchy"]["complete"] and tied["hierarchy"]["event_count"] == 7
        work_rank = next(row for row in tied["per_thread_top_exclusive_scope_wall"]["2"] if row["name"] == "Work")
        assert work_rank["event_count"] == 4  # Three zero events are preserved.
        tied_wait = next(row for row in tied["wait_scopes"] if row["name"] == "WaitUntilTasksComplete")
        assert tied_wait["decomposition_complete"]
        assert abs(tied_wait["nested_task_scope_wall_ms"] - 999.9999) < 1e-6
        # Stable ties must not turn genuine positive-duration overruns into
        # accepted data, including a positive child of a zero-duration parent.
        for crossing in (
            [(2,"GameThread",1,0,20,0), (2,"GameThread",2,2,8,1), (2,"GameThread",5,3,9,2)],
            [(2,"GameThread",1,0,20,0), (2,"GameThread",2,2,2,1), (2,"GameThread",5,2,3,2)],
        ):
            save_fixture(crossing)
            try:
                analyze_window(root, 0, 20)
            except ValueError as error:
                assert "Crossing same-thread scopes" in str(error)
            else:
                raise AssertionError("A real crossing or positive child of a zero parent was accepted")
        save_fixture([(1196447025,"GPU1",1,2,12,0)])
        try:
            analyze_window(root, 2, 12)
        except ValueError:
            pass
        else:
            raise AssertionError("A GPU timeline was treated as CPU evidence")
    print("Render-wait self-test passed: hierarchy, nested task/block union, zero double-counting, clipping, provider-order timestamp ties, zero-duration parent/child preservation, real-crossing rejection, missing evidence, overwrite guard and invalid input checks. No Unreal application launched.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path, nargs="?")
    parser.add_argument("--timing-only", action="store_true", help="Legacy mode: do not require TaskAudit or claim a dependency join")
    parser.add_argument("--start-time", type=float)
    parser.add_argument("--end-time", type=float)
    parser.add_argument("--alignment-json", type=Path, help="Optional independently validated native/Trace alignment context")
    parser.add_argument("--max-events", type=int, default=500000, help="Fail rather than process an unexpectedly large event dump")
    parser.add_argument("--output", type=Path, help="New output file; existing evidence is never overwritten")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        if args.root is not None or args.start_time is not None or args.end_time is not None or args.output or args.alignment_json:
            parser.error("--self-test does not consume real evidence or an output path")
        self_test()
    else:
        if args.root is None:
            parser.error("root is required unless --self-test is used")
        if (args.start_time is None) != (args.end_time is None):
            parser.error("Provide both --start-time and --end-time")
        if args.start_time is None:
            if args.alignment_json:
                parser.error("--alignment-json requires explicit time bounds")
            analyze(args.root, args.timing_only, args.output)
        else:
            result = analyze_window(args.root, args.start_time, args.end_time, args.alignment_json, args.max_events)
            destination = args.output or args.root / "render_wait_window_inspection.json"
            write_result(destination, result)
            print(json.dumps({"output": str(destination), "mode": result["mode"], "events": result["hierarchy"]["event_count"],
                "wait_scopes": result["wait_scope_count"], "frame_alignment": result["frame_alignment"],
                "hierarchy_complete": result["hierarchy"]["complete"], "gpu_total_frame_time": result["gpu_total_frame_time"]}, ensure_ascii=False, indent=2))
