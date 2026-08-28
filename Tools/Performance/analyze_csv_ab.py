import csv
import math
from pathlib import Path


ROOT = Path(r"E:\ueprojrct\fpstrue_safe2")
FILES = {
    "OFF": ROOT / r"Saved\Profiling\CSV\Profile(20260827_211637).csv",
    "ON": ROOT / r"Saved\Profiling\CSV\Profile(20260827_223644).csv",
}
METRICS = [
    "FrameTime",
    "GameThreadTime",
    "RenderThreadTime",
    "RHIThreadTime",
    "GPUTime",
    "DrawCalls",
]


def percentile(values, p):
    values = sorted(values)
    if not values:
        return math.nan
    index = (len(values) - 1) * p
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return values[lower]
    return values[lower] * (upper - index) + values[upper] * (index - lower)


def find_column(fieldnames, metric):
    exact = next((name for name in fieldnames if name.strip() == metric), None)
    if exact:
        return exact
    return next((name for name in fieldnames if name.strip().startswith(metric)), None)


def load(path):
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        columns = {metric: find_column(reader.fieldnames or [], metric) for metric in METRICS}
        if not columns["FrameTime"]:
            raise RuntimeError(f"FrameTime column missing: {path}")
        rows = []
        for row in reader:
            try:
                frame = float(row[columns["FrameTime"]])
            except (TypeError, ValueError, KeyError):
                continue
            if 0.0 < frame < 1000.0:
                rows.append(row)

    trim = int(len(rows) * 0.05)
    if trim and len(rows) > trim * 2:
        rows = rows[trim:-trim]

    result = {"frames": len(rows)}
    for metric, column in columns.items():
        values = []
        if column:
            for row in rows:
                try:
                    values.append(float(row[column]))
                except (TypeError, ValueError, KeyError):
                    pass
        result[metric] = {
            "avg": sum(values) / len(values) if values else math.nan,
            "p95": percentile(values, 0.95),
            "p99": percentile(values, 0.99),
        }

    frame_values = []
    frame_column = columns["FrameTime"]
    for row in rows:
        try:
            frame_values.append(float(row[frame_column]))
        except (TypeError, ValueError, KeyError):
            pass
    result["FPS"] = {
        "avg": 1000.0 / result["FrameTime"]["avg"],
        "low_5": 1000.0 / result["FrameTime"]["p95"],
        "low_1": 1000.0 / result["FrameTime"]["p99"],
    }
    return result


results = {name: load(path) for name, path in FILES.items()}
lines = []
for name in ("OFF", "ON"):
    data = results[name]
    lines.append(f"[{name}] frames={data['frames']}")
    lines.append(
        "FPS avg={avg:.2f}, 5%low={low_5:.2f}, 1%low={low_1:.2f}".format(**data["FPS"])
    )
    for metric in METRICS:
        stats = data[metric]
        lines.append(
            f"{metric}: avg={stats['avg']:.3f}, p95={stats['p95']:.3f}, p99={stats['p99']:.3f}"
        )

off = results["OFF"]
on = results["ON"]
lines.append("[ON relative to OFF]")
for metric in METRICS:
    before = off[metric]["avg"]
    after = on[metric]["avg"]
    delta = (after - before) / before * 100.0 if before else math.nan
    lines.append(f"{metric}: {delta:+.2f}%")

frame_ratio = on["frames"] / off["frames"] if off["frames"] else math.nan
draw_off = off["DrawCalls"]["avg"]
draw_on = on["DrawCalls"]["avg"]
draw_delta = abs(draw_on - draw_off) / draw_off if draw_off else math.inf
comparable = 0.8 <= frame_ratio <= 1.2 and draw_delta <= 0.1
lines.append(f"Comparable: {'YES' if comparable else 'NO'}")
lines.append(f"Frame-count ratio ON/OFF: {frame_ratio:.3f}")
lines.append(f"DrawCalls absolute delta: {draw_delta * 100.0:.2f}%")

output = ROOT / r"Saved\Profiling\CSV\SkeletalMeshes_AB_summary.txt"
output.write_text("\n".join(lines) + "\n", encoding="utf-8")
