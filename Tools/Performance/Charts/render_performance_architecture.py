"""Render the README architecture diagram with Pillow and a CJK font.

Run from any directory; --font and --bold-font support other font installations.
This exports documentation only and does not launch Unreal or change assets.
"""

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
INK = "#18384B"
MUTED = "#536D7E"
TEAL = "#087F8C"
LINE = "#94AAB7"


def render(output: Path, font_path: Path, bold_path: Path) -> None:
    image = Image.new("RGB", (1800, 1870), "white")
    draw = ImageDraw.Draw(image)

    def text(x, y, value, size=28, color=INK, bold=False, anchor="lt"):
        font = ImageFont.truetype(str(bold_path if bold else font_path), size)
        draw.text((x, y), value, font=font, fill=color, anchor=anchor)

    def box(x, y, w, h, title, lines=(), fill="#F1F7F8"):
        draw.rounded_rectangle((x, y, x + w, y + h), radius=14,
                               fill=fill, outline="#D6E2E8", width=2)
        content = [(title, 31, True)] + [(line, 27, False) for line in lines]
        first_y = y + h / 2 - (len(content) - 1) * 23
        for i, (value, size, bold) in enumerate(content):
            font = ImageFont.truetype(str(bold_path if bold else font_path), size)
            if draw.textlength(value, font=font) > w - 38:
                raise ValueError(f"Label does not fit: {value}")
            text(x + w / 2, first_y + i * 46, value, size,
                 INK if bold else MUTED, bold, "mm")

    def arrow(points, color=LINE):
        draw.line(points, fill=color, width=3, joint="curve")
        x, y = points[-1]
        px, py = points[-2]
        angle = math.atan2(y - py, x - px)
        head = [(x, y)]
        for offset in (-0.48, 0.48):
            head.append((x - 14 * math.cos(angle + offset),
                         y - 14 * math.sin(angle + offset)))
        draw.polygon(head, fill=color)

    def heading(y, number, title):
        text(60, y, number, 29, TEAL, True)
        text(124, y, title, 31, INK, True)

    draw.rectangle((60, 52, 69, 106), fill=TEAL)
    text(92, 51, "性能优化架构", 46, bold=True)
    text(62, 123, "运行时调度 · 离线资源处理 · 引擎渲染 · 自动化验证", 28, MUTED)
    draw.line((60, 180, 1740, 180), fill="#DCE6EB", width=2)

    heading(220, "01", "运行时：在游戏线程决定更新与渲染资格")
    box(555, 285, 690, 88, "GameMode：策略配置与存活敌人注册表")
    arrow([(900, 373), (900, 410)])
    box(555, 410, 690, 112, "SignificanceCoordinator", ["集中采样 → 预计算优先级 → 统一分配预算"])
    arrow([(900, 522), (900, 558), (455, 558), (455, 595)])
    arrow([(900, 522), (900, 558), (1345, 558), (1345, 595)])
    box(60, 595, 790, 112, "Gameplay：玩家距离与战斗保护", ["SignificanceManager 评分 → 决策倍率 / 更新间隔"])
    box(950, 595, 790, 112, "Render：相机快照与 Top-K", ["Full / 阴影 / 骨骼光追名额分别选择"])
    arrow([(455, 707), (455, 750)])
    arrow([(1345, 707), (1345, 750)])
    box(60, 750, 790, 150, "行为树 / Movement / 群体资源", ["自适应等待 · MoveTo 去重 / 退避", "共享目标与站位 · 攻击许可 · 请求预算"])
    box(950, 750, 790, 150, "EnemyCharacter + Animation Sharing", ["LOD / 动画 / 阴影 / 光追资格按需应用", "Idle / Moving 姿态复用 · 状态变化时更新"])
    text(62, 934, "战斗保护恢复必要更新，不额外授予阴影 / 光追名额；为 UMG 提供事件驱动刷新接口。", 27, MUTED)

    heading(1010, "02", "场景与渲染：资产处理、引擎参数各自独立")
    box(60, 1080, 790, 150, "Tools/Assets：离线处理与核验", ["静态样条烘焙 · 材质 / LOD / Bounds 核验", "适用资产 Nanite · 纹理尺寸与 Mip 流送治理"], "#F5F7FA")
    box(950, 1080, 790, 150, "DefaultEngine.ini：正式渲染默认值", ["硬件查询 Buffer2 · Lumen 反射下采样 2", "TSR History 150"], "#F5F7FA")
    arrow([(455, 1230), (455, 1270), (900, 1270), (900, 1310)])
    arrow([(1345, 1230), (1345, 1270), (900, 1270), (900, 1310)])
    # Enemy mesh eligibility also contributes to the engine rendering workload.
    arrow([(1740, 825), (1770, 825), (1770, 1355), (1470, 1355)])
    box(330, 1310, 1140, 90, "引擎渲染链：Render Thread / RHI / GPU")
    text(62, 1435, "Buffer2 延后消费查询结果；不等于查询本身执行更快。", 27, MUTED)

    heading(1510, "03", "验证：功能正确性与实景性能分别检查")
    boxes = [
        (60, "实验预设与外部脚本", ["独立进程 / 参数读回"]),
        (490, "BenchmarkRunner", ["准备 / 预热 / 采集"]),
        (920, "CSV / Trace / 校验", ["消费者数量 / 线程计时"]),
        (1350, "汇总与方案取舍", ["Frame / P95 / P99"]),
    ]
    for x, title, lines in boxes:
        box(x, 1580, 390, 122, title, lines, "#F5F7FA")
    for x in (450, 880, 1310):
        arrow([(x, 1641), (x + 40, 1641)])
    text(62, 1750, "UE Automation 验证状态与生命周期；性能报告保留 RT、RHI、GPU 和绘制计数。", 27, MUTED)
    text(62, 1802, "边界：项目 Render 预算在 Game Thread；离线工具、引擎参数与采集流程不由协调器接管。", 27, MUTED)

    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, optimize=True)
    print(f"Saved {output} ({image.width} x {image.height})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "PerformanceEvidence/PerformanceArchitecture.png")
    parser.add_argument("--font", type=Path, default=Path("C:/Windows/Fonts/msyh.ttc"))
    parser.add_argument("--bold-font", type=Path, default=Path("C:/Windows/Fonts/msyhbd.ttc"))
    args = parser.parse_args()
    render(args.output, args.font, args.bold_font)
