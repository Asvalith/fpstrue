"""Render the three formal performance A/B summaries as copyable PNG figures."""

from __future__ import annotations

import csv
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
PROFILING = ROOT / "Saved" / "Profiling"

NAVY = "#183249"
MUTED = "#647b8d"
GRID = "#dce5eb"
GRAY = "#9bafbd"
TEAL = "#007f87"
ORANGE = "#ba6b2c"
BACKGROUND = "#ffffff"

FONT_REGULAR = Path("C:/Windows/Fonts/msyh.ttc")
FONT_BOLD = Path("C:/Windows/Fonts/msyhbd.ttc")


def font(size: int, *, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_BOLD if bold else FONT_REGULAR), size)


def rows(batch: str) -> dict[str, dict[str, str]]:
    path = PROFILING / batch / "group_summary.csv"
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        result = {row["Variant"]: row for row in csv.DictReader(handle)}
    if not result or any(row["EnemyCount"] != "160" or row["Runs"] != "3" for row in result.values()):
        raise ValueError(f"Expected three-run 160-enemy groups: {path}")
    return result


def render(
    filename: str,
    title: str,
    subtitle: str,
    data: dict[str, dict[str, str]],
    variants: list[tuple[str, str, str]],
    metrics: list[tuple[str, str]],
    note: str,
    source: str,
) -> None:
    width = 1680
    plot_left, plot_right = 590, 1450
    chart_top = 315
    group_step = len(variants) * 43 + 33
    chart_bottom = chart_top + len(metrics) * group_step
    height = chart_bottom + 176
    image = Image.new("RGB", (width, height), BACKGROUND)
    draw = ImageDraw.Draw(image)

    title_font = font(47, bold=True)
    subtitle_font = font(27)
    legend_font = font(27)
    label_font = font(28)
    value_font = font(27)
    axis_font = font(23)
    note_font = font(22)

    draw.rectangle((70, 62, 80, 116), fill=TEAL)
    draw.text((103, 52), title, fill=NAVY, font=title_font)
    draw.text((72, 140), subtitle, fill=MUTED, font=subtitle_font)
    draw.line((70, 202, width - 70, 202), fill=GRID, width=2)

    legend_x = 85
    for _, legend, color in variants:
        draw.ellipse((legend_x, 240, legend_x + 13, 253), fill=color)
        draw.text((legend_x + 24, 224), legend, fill=MUTED, font=legend_font)
        legend_x += 75 + draw.textlength(legend, font=legend_font)

    maximum = max(float(data[key][column]) for key, _, _ in variants for column, _ in metrics)
    scale_max = max(5, int(math.ceil(maximum / 5.0) * 5))
    for tick in range(0, scale_max + 1, 5):
        x = plot_left + (plot_right - plot_left) * tick / scale_max
        draw.line((x, chart_top - 7, x, chart_bottom - 17), fill=GRID, width=1)
        tick_label = str(tick)
        tick_width = draw.textlength(tick_label, font=axis_font)
        draw.text((x - tick_width / 2, chart_bottom - 7), tick_label, fill=MUTED, font=axis_font)

    for index, (column, label) in enumerate(metrics):
        y0 = chart_top + index * group_step
        label_y = y0 + (len(variants) * 43 - 31) / 2
        draw.text((72, label_y), label, fill=NAVY, font=label_font)
        for variant_index, (key, _, color) in enumerate(variants):
            value = float(data[key][column])
            bar_y = y0 + variant_index * 43
            bar_end = plot_left + (plot_right - plot_left) * value / scale_max
            draw.rectangle((plot_left, bar_y, bar_end, bar_y + 26), fill=color)
            draw.text((bar_end + 13, bar_y - 8), f"{value:.3f}", fill=NAVY, font=value_font)
        if index < len(metrics) - 1:
            separator_y = y0 + group_step - 18
            draw.line((72, separator_y, width - 70, separator_y), fill=GRID, width=1)

    footer_y = chart_bottom + 68
    draw.line((70, footer_y - 22, width - 70, footer_y - 22), fill=GRID, width=2)
    draw.text((72, footer_y), note, fill=MUTED, font=note_font)
    draw.text((72, footer_y + 43), source, fill=MUTED, font=note_font)

    destination = OUT / filename
    image.save(destination, optimize=True)
    print(destination)


def main() -> None:
    buffer_data = rows("PerformanceCloseout_Buffer2Formal")
    render(
        "10_buffer2_formal.png",
        "查询缓冲正式对照：RT 缩短，RHI 上升",
        "160 敌人；硬件查询 Buffer1 / Buffer2 各三次，平衡顺序独立进程；单位 ms",
        buffer_data,
        [("HardwareQueries", "Buffer1", GRAY), ("BufferedQueries2", "Buffer2", TEAL)],
        [
            ("FrameAverageMs", "Frame"),
            ("FrameP95Ms", "P95"),
            ("FrameP99Ms", "P99"),
            ("RenderThreadMs", "RT"),
            ("RHIThreadMs", "RHI"),
            ("VisibilityWaitMs", "可见性等待代理"),
        ],
        "结论：固定镜头整帧有益；RHI 同期升高，快速转镜画质尚未验收。",
        "来源：PerformanceCloseout_Buffer2Formal/group_summary.csv；各轮等权均值。",
    )

    lumen_data = rows("PerformanceCloseout_LumenFormal_R2")
    render(
        "11_lumen_formal.png",
        "GPU 单变量：反射有效，ScreenProbe 无净收益",
        "160 敌人；每项相对同组 Baseline，各三次平衡顺序对照；单位 ms",
        lumen_data,
        [
            ("Baseline", "Baseline", GRAY),
            ("LumenReflectionsDS2", "Reflections 下采样 2", TEAL),
            ("LumenScreenProbeDS32", "ScreenProbe 下采样 32", ORANGE),
        ],
        [
            ("FrameAverageMs", "Frame"),
            ("FrameP95Ms", "P95"),
            ("FrameP99Ms", "P99"),
        ],
        "结论：反射候选进入组合验证；ScreenProbe 的 Frame、P95、P99 均未改善。",
        "来源：PerformanceCloseout_LumenFormal_R2/group_summary.csv；单变量结果。",
    )

    combined_data = rows("PerformanceCloseout_CombinedFormal")
    render(
        "12_combined_formal.png",
        "组合验证：整帧改善，但不能把收益跨线程相加",
        "160 敌人；原策略与 Buffer2＋反射下采样 2 各三次，平衡顺序；单位 ms",
        combined_data,
        [
            ("OriginalRenderPolicy", "原策略", GRAY),
            ("OptimizedRenderPolicy", "当前组合", TEAL),
        ],
        [
            ("FrameAverageMs", "Frame"),
            ("FrameP95Ms", "P95"),
            ("FrameP99Ms", "P99"),
            ("RenderThreadMs", "RT"),
            ("RHIThreadMs", "RHI"),
            ("GpuMs", "GPU"),
            ("VisibilityWaitMs", "可见性等待代理"),
        ],
        "结论：Frame / 尾帧下降、RHI 上升；不证明某个 GPU Pass 导致查询等待。",
        "来源：PerformanceCloseout_CombinedFormal/group_summary.csv；独立组合 A/B。",
    )


if __name__ == "__main__":
    main()
