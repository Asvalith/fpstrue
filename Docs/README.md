# FPS 技术文档总索引

> 同步日期：2026-08-16。
>
> 本页只负责导航和权威边界，不重复维护架构结论、性能数字或任务状态。

## 1. 当前封板状态

- C++ 主链和最终 Development Editor 编译已经完成。
- `10 / 20 / 40 / 80 / 160` 固定规模矩阵、纹理治理、VSM 实验和生命周期回收证据已经封口。
- 当前发布容量按本机约 40 个活跃敌人、约 60 FPS 表述；80/160 是压力档。
- 两个零引用旧模板 Widget 已清理；全量蓝图编译为 `0 errors / 0 warnings / 0 failed to load`。
- 当前 P0 是实际玩法蓝图的 PIE 回归；Shipping Cook/Package 和打包冒烟尚未完成。

活动状态只看 [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md)，精确性能数字只看 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。

## 2. 五份活动文档

| 要回答的问题 | 权威文件 | 唯一职责 |
| --- | --- | --- |
| 当前架构、调用链、真实问题和替代方案是什么 | [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md) | 项目统一技术主线；完整保留问题、定位、修复、验证、取舍和条件变化后的方案 |
| 性能到底达到什么水平 | [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md) | 测试条件、固定矩阵、纹理/VSM/生命周期证据和诚实边界 |
| 现在怎样接蓝图、还剩什么任务 | [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md) | 唯一活动清单；合并接线步骤、回归矩阵和发布门禁 |
| 面试官会怎样追问项目 | [FPS_PROJECT_INTERVIEW_QA.md](FPS_PROJECT_INTERVIEW_QA.md) | 以当前项目为主的高中频拷打和条件扩展题 |
| 文档之间怎样分工 | 本页 | 入口、状态和冲突处理顺序 |

## 3. 推荐阅读路径

### 项目复习

```text
FPS_Core_Technical_Summary
-> 第 13～15 节：架构、设计模式、调用链和方案取舍
-> 第 16～19 节：真实问题、定位过程、修复和替代条件
-> PERFORMANCE_BASELINE：背熟可验证数字
-> FPS_PROJECT_INTERVIEW_QA：闭卷练习
```

### 蓝图封板

```text
PROJECT_TASK_CHECKLIST
-> 修复两个 Widget
-> Compile All Blueprints
-> PIE 完整回归
-> Development Editor Build
-> Shipping Cook/Package
-> 打包产物冒烟
```

### 性能和源码

```text
PERFORMANCE_BASELINE
-> PerformanceEvidence/20260816
-> Learning/UE5_ACTOR_LIFECYCLE_AND_TICK_SOURCE_STUDY
-> Learning/UE5_GAME_FRAMEWORK_TECHNICAL_ROADMAP
```

## 4. 学习记录

这些文件保留学习深度，但不代表当前项目已经实现相应功能：

- [Learning/UE5_ACTOR_LIFECYCLE_AND_TICK_SOURCE_STUDY.md](Learning/UE5_ACTOR_LIFECYCLE_AND_TICK_SOURCE_STUDY.md)：Actor 生命周期、Tick、Timer 和 TaskGraph 源码学习。
- [Learning/UE5_GAME_FRAMEWORK_TECHNICAL_ROADMAP.md](Learning/UE5_GAME_FRAMEWORK_TECHNICAL_ROADMAP.md)：Gameplay Framework、图形学和源码阅读路线。
- [Learning/EXTENSION_TOPICS.md](Learning/EXTENSION_TOPICS.md)：按触发条件整理的 AI、物理、联网、GAS、渲染和多平台扩展入口。

扩展索引只列要点；完整旧学习地图原文保存在 Archive，未删除。

## 5. 历史与完整原始记录

[Archive](Archive/) 保存合并前的专题、实战复盘、AI 决策、旧蓝图接线、阶段快照和历史计划。它们用于追溯“当时遇到什么、为什么这样改”，不能覆盖当前状态。

主文档已经吸收这些记录中的关键内容，包括：

- 问题现象与复现条件；
- 假设、日志、断点、统计命令和排除顺序；
- 根因、最终修复与为什么不用其他方案；
- 修改条件后的替换方案、代价和验证指标；
- 未解决边界和不能夸大的结论。

`PerformanceEvidence/20260816` 是当前证据，不属于历史归档。

## 6. 冲突处理顺序

```text
当前 C++ / 实际蓝图资产
-> 最新编译、日志、CSV、MemReport 和截图
-> PERFORMANCE_BASELINE / PROJECT_TASK_CHECKLIST
-> FPS_Core_Technical_Summary
-> FPS_PROJECT_INTERVIEW_QA
-> Learning 与 Archive
```

## 7. 维护规则

1. 架构、调用链和实战复盘只更新 `FPS_Core_Technical_Summary`。
2. 性能数字只更新 `PERFORMANCE_BASELINE`；其他文件只引用结论。
3. 活动任务和蓝图接线只更新 `PROJECT_TASK_CHECKLIST`。
4. 面试回答更新 `FPS_PROJECT_INTERVIEW_QA`，但事实必须链接回主文档或证据。
5. 学习内容明确标注“当前实现、历史方案、候选方案”，不得把扩展题写成项目成果。
6. 合并前原文放入 Archive，不再从 Source 目录维护第二套文档。
