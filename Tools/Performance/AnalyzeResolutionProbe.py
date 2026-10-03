"""Read-only raw CSV comparison; preserves failed manifest flags in output.

This intentionally does not promote rejected captures to formal valid samples.
Explicit --exploratory permits inspection for diagnosis only. All values are
per-CSV-frame measurements; GPU scope attribution is analyzed separately.
"""
import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean
from AnalyzeGpuTimingTrace import percentile


def analyze(root):
    with (root / "manifest.csv").open(encoding="utf-8-sig", newline="") as f:
        manifests = list(csv.DictReader(f))
    if len(manifests) != 1:
        raise ValueError("Expected exactly one run in each probe")
    manifest = manifests[0]
    raw = []
    with Path(manifest["Csv"]).open(encoding="utf-8-sig", newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        counts, names = {}, []
        for name in header:
            counts[name] = counts.get(name, 0) + 1
            names.append(name if counts[name] == 1 else f"{name}__{counts[name]}")
        for row in reader:
            if not row or any(x == "[HasHeaderRowAtEnd]" for x in row):
                continue
            if row == header:
                continue
            item = dict(zip(names, row))
            if item.get("FrameTime") == "FrameTime":
                continue  # UE appends a header which may include extra metadata columns.
            value = float(item["FrameTime"])
            if not math.isfinite(value) or value < 0:
                raise ValueError("Invalid frame duration")
            if value > 0:
                raw.append(item)
    total = sum(float(r["FrameTime"]) for r in raw)
    elapsed, rows = 0., []
    # Keep complete frames inside the predeclared elapsed-time crop.
    for row in raw:
        end = elapsed + float(row["FrameTime"])
        if elapsed >= 1000 and end <= total - 500:
            rows.append(row)
        elapsed = end
    if not rows:
        raise ValueError("No steady samples")
    required = ["FrameTime", "GameThreadTime", "RenderThreadTime", "RHIThreadTime", "GPUTime",
                "Exclusive/RenderThread/EventWait/Visibility", "RHI/DrawCalls"]
    columns = required + [name for name in names if name.startswith("GPU/")]
    metrics = {}
    for col in columns:
        values = [float(row[col]) for row in rows]
        if not all(math.isfinite(x) for x in values):
            raise ValueError(f"Missing or nonfinite measurement: {col}")
        metrics[col] = {"mean": mean(values), "median": percentile(values, .5),
                        "p95": percentile(values, .95), "p99": percentile(values, .99)}
    with (root / "environment.json").open(encoding="utf-8-sig") as f:
        environment = json.load(f)
    return {"root": str(root.resolve()), "manifest": manifest,
            "input_hashes": {i["Path"]: i["SHA256"] for i in environment["Inputs"]},
            "raw_frames": len(raw), "steady_frames": len(rows), "raw_elapsed_ms": total,
            "metrics": metrics}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--exploratory", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Refusing to overwrite prior analysis")
    a, b = analyze(args.a), analyze(args.b)
    if not args.exploratory and any(r["manifest"]["Valid"] != "True" for r in (a, b)):
        parser.error("Rejected manifests: only explicit exploratory analysis is allowed")
    if a["input_hashes"] != b["input_hashes"]:
        parser.error("Binary/map/config hashes differ: not a same-input comparison")
    comparison = {key: {"A_mean": vals["mean"], "B_mean": b["metrics"][key]["mean"],
                       "B_minus_A": b["metrics"][key]["mean"] - vals["mean"]}
                  for key, vals in a["metrics"].items() if key in b["metrics"]}
    result = {"classification": "exploratory, NOT formal performance acceptance" if args.exploratory else "comparison",
              "method": "Per-CSV-frame values; complete frames after first1000ms/before last500ms of cumulative FrameTime; linear quantiles; nested GPU counters not added together.",
              "A": a, "B": b, "comparison": comparison}
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"A_frames": a["steady_frames"], "B_frames": b["steady_frames"], "comparison": comparison}, indent=2))
