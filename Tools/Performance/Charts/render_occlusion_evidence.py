"""Rebuild E09/E10 evidence figures from existing records; never launch Unreal.

Raw task events, exact CPU scopes and CSV wait proxies deliberately remain
separate. GPU execution spans are not available and are never synthesized.
"""
from __future__ import annotations

import csv
import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw

from render_closeout_charts import font, NAVY, MUTED, GRID, GRAY, TEAL, ORANGE

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "Docs/Performance/figures"
TRACE = ROOT / "Saved/Profiling/RenderWaitTaskTrace_20260909"
TOGGLE = ROOT / "Saved/Profiling/OcclusionQueryDiagnosis_Checked_20260909"
ENGINE = Path("E:/program/ue554/UE_5.5/Engine/Source/Runtime")
PURPLE = "#7060a3"
SPECS = [
    (0, "median", "23880", "23885", "1410954", "1411241"),
    (0, "P95", "26699", "26702", "1480032", "1480342"),
    (160, "median", "24023", "24024", "2496165", "2496785"),
    (160, "P95", "44372", "44373", "3177585", "3178199"),
]


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def selected_rows(path, ids):
    """Stream large task exports; parse only selected rows, keep IDs as strings."""
    result = {}
    with path.open(encoding="utf-8-sig", newline="") as handle:
        header = next(csv.reader([next(handle)]))
        for line in handle:
            identity = line.split(",", 1)[0]
            if identity in ids:
                result[identity] = dict(zip(header, next(csv.reader([line]))))
                if len(result) == len(ids):
                    break
    assert set(result) == set(ids), (path, ids - result.keys())
    return result


def load_evidence():
    samples, coverage = [], []
    for count in (0, 160):
        directory = TRACE / f"N{count}_Baseline_R1"
        inspection = json.loads((directory / "render_wait_inspection.json").read_text(encoding="utf-8"))
        specs = [s for s in SPECS if s[0] == count]
        audit = directory / "TaskAudit"
        waits = selected_rows(audit / "waits.csv", {s[i] for s in specs for i in (2, 3)})
        tasks = selected_rows(audit / "tasks.csv", {s[i] for s in specs for i in (4, 5)})
        mappings = selected_rows(audit / "wait_tasks.csv", set(waits))
        threads = {r["ThreadId"]: r for r in read_csv(audit / "threads.csv")}
        for _, label, wid, rid, bid, aid in specs:
            w, r, b, a = waits[wid], waits[rid], tasks[bid], tasks[aid]
            assert mappings[wid]["TaskId"] == bid and mappings[rid]["TaskId"] == aid
            assert int(b["RawLifecycleMask"]) & 32 and int(a["RawLifecycleMask"]) & 32
            assert "RHIInterruptThread" in str(threads[b["CompletedThreadId"]])
            picked = next(s for s in inspection["selected"] if s["label"].lower() == label.lower())
            scope = picked["scope"]
            times = dict(worker_start=float(w["Started"]), worker_end=float(w["Finished"]),
                         rt_wait_start=float(r["Started"]), rt_wait_end=float(r["Finished"]),
                         rt_start=scope["start"], rt_end=scope["end"],
                         query_completed=float(b["Completed"]), pipe_completed=float(a["Completed"]))
            assert times["worker_start"] < times["query_completed"] <= times["worker_end"]
            assert times["query_completed"] < times["pipe_completed"] <= times["rt_wait_end"] <= times["rt_end"]
            samples.append(dict(label=f"{count} 敌人 · {label}", enemy_count=count, selection=label,
                                query_event=bid, pipe_event=aid, worker_thread=w["ThreadName"],
                                query_completion_thread="RHIInterruptThread", **times))
        overlap = inspection["occlusion_sync_overlap"]
        coverage.append(dict(enemy_count=count, count=inspection["steady_scope_count"],
                             overlap_count=overlap["scopes_with_overlap"],
                             wait_ms=inspection["wait_scope_mean_ms"],
                             overlap_ms=overlap["mean_overlap_ms"],
                             fraction=overlap["time_weighted_fraction"]))
    groups = {r["Variant"]: r for r in read_csv(TOGGLE / "group_summary.csv")}
    runs = read_csv(TOGGLE / "run_summary.csv")
    manifests = read_csv(TOGGLE / "manifest.csv")
    assert len(manifests) == 4 and all(r["Valid"].lower() == "true" for r in manifests)
    metrics = [("VisibilityWaitMs", "可见性等待代理"), ("RenderThreadMs", "Render Thread"),
               ("RHIThreadMs", "RHI Thread"), ("GpuMs", "GPU"), ("FrameAverageMs", "Frame"),
               ("DrawCalls", "RHI Draw Calls")]
    comparison = []
    for key, label in metrics:
        values = {}
        for variant, state in (("OcclusionQueriesOn", "on"), ("OcclusionQueriesOff", "off")):
            assert groups[variant]["Runs"] == "2" and groups[variant]["EnemyCount"] == "0"
            values[state] = float(groups[variant][key])
            per_run = [float(r[key]) for r in runs if r["Variant"] == variant]
            # The saved summary rounds each group after averaging its runs.
            assert len(per_run) == 2 and abs(sum(per_run) / 2 - values[state]) <= 0.051
            values[state + "_runs"] = per_run
        comparison.append(dict(key=key, label=label, unit="次/帧" if key == "DrawCalls" else "ms", **values))
    anchors = [
        ("Renderer/Private/SceneVisibility.cpp", 4722, "Tasks.DynamicMeshElementsPipe->Wait"),
        ("Renderer/Private/SceneVisibility.cpp", 3071, "RHIGetRenderQueryResult"),
        ("D3D12RHI/Private/D3D12Query.cpp", 334, "Query->SyncPoint->Wait()"),
        ("D3D12RHI/Private/D3D12Submission.cpp", 1271, "CompletedFenceValue < Payload->CompletionFenceValue"),
        ("D3D12RHI/Private/D3D12Submission.cpp", 1333, "Query.CopyResultTo"),
        ("D3D12RHI/Private/D3D12Submission.cpp", 1419, "SyncPoint->GraphEvent->DispatchSubsequents()"),
        ("Renderer/Private/SceneVisibility.cpp", 4337, "DispatchSubsequents"),
    ]
    source = []
    for file, line, expected in anchors:
        path = ENGINE / file
        lines = path.read_text(encoding="utf-8-sig").splitlines()
        assert expected in lines[line - 1], (path, line, lines[line - 1])
        source.append(dict(file=file, line=line, text=lines[line - 1].strip()))
    provenance = []
    for path in [TRACE / "task_dependency_audit.md", TRACE / "render_wait_diagnosis.md",
                 TOGGLE / "group_summary.csv", TOGGLE / "run_summary.csv", TOGGLE / "manifest.csv"]:
        provenance.append(dict(path=path.relative_to(ROOT).as_posix(), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    return dict(experiment_date="2026-09-09", samples=samples, coverage=coverage,
                comparison=comparison, source=source, provenance=provenance,
                definitions=dict(timeline="Relative to each selected RT scope start, milliseconds; not GPU execution.",
                                 coverage="Duration-weighted interval overlap, not percentage of GPU busy time.",
                                 comparison="Two independent runs per condition, equal run weight; fixed-view diagnostic, not current baseline."))


def canvas(title, subtitle, height, width=1680):
    image = Image.new("RGB", (width, height), "white")
    d = ImageDraw.Draw(image)
    d.rectangle((65, 54, 73, 105), fill=TEAL)
    d.text((96, 48), title, font=font(43, bold=True), fill=NAVY)
    d.text((67, 120), subtitle, font=font(25), fill=MUTED)
    d.line((66, 173, width-66, 173), fill=GRID, width=2)
    return image, d


def footer(d, height, lines):
    y = height - 48 - 34 * len(lines)
    d.line((66, y-15, 1614, y-15), fill=GRID, width=2)
    for line in lines:
        d.text((68, y), line, font=font(22), fill=MUTED)
        y += 34


def save(image, name):
    path = OUT / name
    image.save(path, optimize=True)
    print(path)


def draw_source():
    image, d = canvas("源码证据：两个事件，逐级解除等待", "E09 · UE 5.5 / D3D12 · 跨线程依赖与完成通知，不是单线程调用栈", 1730)
    nodes = [
        ("1  Render Thread 等事件 A", "网格收集管道尚未完成", ["Tasks.DynamicMeshElementsPipe->Wait(...);"], "SceneVisibility.cpp:4722", GRAY),
        ("2  上游 Worker 等事件 B", "OcclusionCullPipe 先等待历史查询结果", ["RHIGetRenderQueryResult(..., Result, bWait=true);", "Query->SyncPoint->Wait();"], "SceneVisibility.cpp:3064–3071 / D3D12Query.cpp:330–334", PURPLE),
        ("3  GPU 相关批次必须到达完成点", "此处尚无 query / payload / GPU Pass 的运行时一一对应", ["CompletedFenceValue < Payload->CompletionFenceValue"], "D3D12Submission.cpp:1262–1278；不表示等待 GPU 全部工作", ORANGE),
        ("4  RHIInterruptThread 通知事件 B", "fence 达标后处理结果，再唤醒查询等待者", ["Query.CopyResultTo(Query.Target);", "SyncPoint->GraphEvent->DispatchSubsequents();"], "D3D12Submission.cpp:1333 / 1419", TEAL),
        ("5  Worker 继续推进可见性管道", "遮挡 → 渲染相关性 → 动态网格管道收尾", ["ReleaseNumCommands(...)  ->  EmptyFunction()"], "SceneVisibility.cpp:3528–3593 / SceneVisibilityPrivate.h:65–74", TEAL),
        ("6  管道通知事件 A，RT 继续", "不是由查询事件 B 直接唤醒 RT", ["Tasks.DynamicMeshElementsPipe->DispatchSubsequents();"], "SceneVisibility.cpp:4333–4337 → 4722 等待返回", GRAY),
    ]
    y = 205
    for i, (title, detail, code, source, color) in enumerate(nodes):
        d.rectangle((68, y+3, 75, y+170), fill=color)
        d.text((97, y), title, font=font(32, bold=True), fill=NAVY)
        d.text((97, y+48), detail, font=font(26), fill=MUTED)
        for index, line in enumerate(code):
            d.text((97, y+87+index*32), line, font=font(24), fill=NAVY)
        d.text((97, y+160), source, font=font(21), fill=MUTED)
        if i < len(nodes)-1:
            d.line((46, y+177, 46, y+223), fill=GRID, width=3)
            d.polygon([(40,y+215),(52,y+215),(46,y+226)],fill=MUTED)
        y += 232
    footer(d, 1730, ["代码为关键语句节选；省略上下文、分支与参数，不能作为可编译替代实现。",
                     "事件等待/完成来自 TaskTrace；跨管道连接由原始事件、CPU scope 和源码共同核对。"])
    save(image, "e09_source_chain.png")


def draw_timeline(data):
    image, d = canvas("时间线证据：查询通知之后，RT 很快恢复", "E09 · 4 个已审计代表样本 · 各样本以 RT scope 起点为 0，共用毫秒刻度", 1750)
    samples = data["samples"]
    lower = min((s["worker_start"]-s["rt_start"])*1000 for s in samples) - 0.2
    upper = max((s["rt_end"]-s["rt_start"])*1000 for s in samples) + 0.6
    left, right = 380, 1500
    x = lambda ms: left+(ms-lower)/(upper-lower)*(right-left)
    for i, s in enumerate(samples):
        y = 210 + i*326
        origin = s["rt_start"]
        relative = lambda key: (s[key]-origin)*1000
        total = relative("rt_end")
        after = (s["rt_end"]-s["query_completed"])*1000
        d.text((68,y), s["label"], font=font(30,bold=True), fill=NAVY)
        d.text((450,y+4), f"RT scope {total:.4f} ms   ·   通知后至 RT scope 结束 {after:.4f} ms", font=font(24),fill=NAVY)
        for tick in range(0,math.floor(upper)+1,2):
            d.line((x(tick),y+62,x(tick),y+237),fill=GRID,width=1)
            d.text((x(tick)-8,y+245),str(tick),font=font(21),fill=MUTED)
        positions=[y+80,y+137,y+194]
        labels=[f"Worker 等 B · {s['query_event']}","查询完成通知 · 线程 38",f"RT 等 A · {s['pipe_event']}"]
        for py,label in zip(positions,labels):
            d.text((68,py-5),label,font=font(23),fill=NAVY)
        d.rectangle((x(relative("worker_start")),positions[0],x(relative("worker_end")),positions[0]+23),fill=PURPLE)
        q=x(relative("query_completed"))
        d.line((q,positions[0]-14,q,positions[2]+34),fill=ORANGE,width=2)
        d.polygon([(q,positions[1]-2),(q+10,positions[1]+10),(q,positions[1]+22),(q-10,positions[1]+10)],fill=ORANGE)
        d.rectangle((x(0),positions[2],q,positions[2]+23),fill=GRAY)
        d.rectangle((q,positions[2],x(total),positions[2]+23),fill=TEAL)
        a=x(relative("pipe_completed"))
        d.line((a,positions[2]-8,a,positions[2]+31),fill=NAVY,width=2)
        d.text((x(total)+10,positions[2]-6),f"{total:.3f}",font=font(23),fill=NAVY)
        d.text((380,y+282),f"0 ms 对应 Trace 时间 {origin:.7f} s；A 完成标为末端细竖线。",font=font(21),fill=MUTED)
    footer(d,1750,["灰色＋青色为 RT scope；紫色为 Worker 原始等待；橙色为实际 Completed 通知。",
                   "median/P95 是既有脚本选出的代表样本，不是分别计算出的每项分位数；未绘制 GPU 执行时长。",
                   "来源：RenderWaitTaskTrace_20260909 / TaskAudit 原始事件及 render_wait_inspection.json。"])
    save(image,"e09_wait_timeline.png")


def draw_coverage(data):
    image,d=canvas("覆盖证据：约 95% 的 RT 等待与查询同步重叠", "E09 · 捕获区域去除首 1 秒、末 0.5 秒；同线程嵌套校验，重叠区间取并集", 800)
    left,right=380,1390
    maximum=math.ceil(max(c["wait_ms"] for c in data["coverage"]))
    for tick in range(maximum+1):
        x=left+(right-left)*tick/maximum
        d.line((x,225,x,555),fill=GRID,width=1)
        d.text((x-7,565),str(tick),font=font(22),fill=MUTED)
    for i,c in enumerate(data["coverage"]):
        y=245+i*165
        d.text((68,y-3),f"{c['enemy_count']} 敌人",font=font(32,bold=True),fill=NAVY)
        d.text((68,y+48),f"{c['count']} 个 RT scopes",font=font(24),fill=MUTED)
        end=left+(right-left)*c["wait_ms"]/maximum
        overlap=left+(right-left)*c["overlap_ms"]/maximum
        d.rectangle((left,y,overlap,y+43),fill=TEAL)
        d.rectangle((overlap,y,end,y+43),fill=GRAY)
        d.text((end+12,y+3),f"{c['wait_ms']:.3f} ms",font=font(26),fill=NAVY)
        d.text((left,y+62),f"重叠 {c['overlap_ms']:.3f} ms  /  {c['fraction']*100:.2f}%    ·    {c['overlap_count']}/{c['count']} 个 scope 发生重叠",font=font(24),fill=NAVY)
    d.text((1090,611),"每个 RT scope 的平均时长 / ms",font=font(23),fill=MUTED)
    footer(d,800,["青色＝与查询等待的重叠区间；灰色＝其余区间。比例按等待时长加权，不是 GPU 耗时占比。",
                  "来源：0/160 敌人各一份 render_wait_inspection.json；任务身份与源码审计另行验证依赖。"])
    save(image,"e09_overlap_coverage.png")


def draw_toggle(data):
    image,d=canvas("干预证据：关查询后等待消失，提交负担增加", "E10 · 0 敌人，同地图固定镜头 · On → Off → On → Off · 每种条件 2 次独立进程", 1300)
    d.rectangle((70,215,91,235),fill=GRAY)
    d.text((102,204),"查询 On",font=font(25),fill=NAVY)
    d.rectangle((315,215,336,235),fill=TEAL)
    d.text((347,204),"查询 Off",font=font(25),fill=NAVY)
    left,right=420,1290
    timed=[m for m in data["comparison"] if m["unit"]=="ms"]
    maximum=math.ceil(max(max(m["on"],m["off"]) for m in timed)/2)*2
    for tick in range(0,maximum+1,2):
        x=left+(right-left)*tick/maximum
        d.line((x,280,x,884),fill=GRID,width=1)
        d.text((x-8,890),str(tick),font=font(22),fill=MUTED)
    d.text((left,927),"时长 / ms；各轮等权平均，线程时间不可相加",font=font(22),fill=MUTED)
    for index,m in enumerate(timed):
        y=290+index*116
        d.text((68,y+18),m["label"],font=font(27),fill=NAVY)
        for state,dy,color in [("on",0,GRAY),("off",42,TEAL)]:
            end=left+(right-left)*m[state]/maximum
            d.rectangle((left,y+dy,end,y+dy+25),fill=color)
            d.text((end+10,y+dy-7),f"{m[state]:.3f}",font=font(25),fill=NAVY)
        delta=m["off"]-m["on"]
        d.text((1430,y+18),f"{delta:+.3f}",font=font(25),fill=NAVY)
    d.text((1410,249),"Off − On / ms",font=font(22),fill=MUTED)
    m=data["comparison"][-1]
    maximum=math.ceil(max(m["on"],m["off"])/500)*500
    d.text((68,1020),"RHI Draw Calls",font=font(27),fill=NAVY)
    for state,dy,color in [("on",0,GRAY),("off",42,TEAL)]:
        end=left+(right-left)*m[state]/maximum
        d.rectangle((left,1000+dy,end,1025+dy),fill=color)
        d.text((end+10,993+dy),f"{m[state]:.1f}",font=font(25),fill=NAVY)
    for tick in range(0,maximum+1,500):
        x=left+(right-left)*tick/maximum
        d.text((x-12,1080),str(tick),font=font(21),fill=MUTED)
    d.text((1420,1020),f"+{(m['off']/m['on']-1)*100:.1f}%",font=font(27),fill=NAVY)
    d.text((left,1120),"绘制次数 / 帧（独立刻度）",font=font(22),fill=MUTED)
    footer(d,1300,["GPU 均值在记录精度下相同；等待减少 5.095 ms，整帧减少 1.280 ms，二者不等价。",
                   "来源：OcclusionQueryDiagnosis_Checked_20260909/group_summary.csv；历史诊断，不是当前版本基线。"])
    save(image,"e10_query_intervention.png")


if __name__ == "__main__":
    evidence=load_evidence()
    draw_source()
    draw_timeline(evidence)
    draw_coverage(evidence)
    draw_toggle(evidence)
    (OUT/"occlusion_evidence_data.json").write_text(json.dumps(evidence,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print("Verified: four raw task chains, recorded Completed bits, E10 manifests, group/run summaries and seven source anchors.")
