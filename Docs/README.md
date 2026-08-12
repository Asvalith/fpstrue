# FPS 文档索引

本文定义每份文档的唯一职责。相同结论只在一个权威文档维护，其他文件通过链接引用。

## 当前权威文档

| 要回答的问题 | 权威文件 | 内容边界 |
| --- | --- | --- |
| 当前架构是什么、面试怎么讲 | [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md) | 当前事实、职责、调用链、设计模式、选型、场景题和面试回答 |
| 现在还要做什么 | [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md) | 唯一活动任务清单和完成标准 |
| 性能数字的原始基线是什么 | [../Source/fpstrue/PERFORMANCE_BASELINE.md](../Source/fpstrue/PERFORMANCE_BASELINE.md) | 固定测试条件、原始指标和数据限制 |

## 专题学习记录

这些文件保存推导过程、历史状态和专题细节，不是第二套当前架构。它们不因主文档已经合并结论而删除：

- [Development_Experience_And_Optimization.md](Development_Experience_And_Optimization.md)：Bug 时间线、实验过程、CPU/纹理数据和停止条件。
- [Health_And_Damage_System.md](Health_And_Damage_System.md)：伤害、死亡、近战结算和生命周期。
- [Enemy_Attack_Window.md](Enemy_Attack_Window.md)：NotifyState、双 Socket Sweep 和攻击去重。
- [UE5_ACTOR_LIFECYCLE_AND_TICK_SOURCE_STUDY.md](UE5_ACTOR_LIFECYCLE_AND_TICK_SOURCE_STUDY.md)：Actor 生命周期与 Tick 源码学习。
- [Portfolio_Technical_Extension_Map.md](Portfolio_Technical_Extension_Map.md)：FPS 与 Co-op 的后续技术扩展，不代表当前已实现。
- [../Source/fpstrue/AI_OPTIMIZATION_DECISION_RECORD.md](../Source/fpstrue/AI_OPTIMIZATION_DECISION_RECORD.md)：AI 改造前的方案与思考记录，其中感知和 EQS 仍属于候选方案。
- [../Source/fpstrue/CPP_BLUEPRINT_BOUNDARY.md](../Source/fpstrue/CPP_BLUEPRINT_BOUNDARY.md)：C++/蓝图迁移时使用过的旧节点清理记录。
- [../Source/fpstrue/PROJECT_PROGRESS.md](../Source/fpstrue/PROJECT_PROGRESS.md)：按日期保留的阶段快照，不作为当前事实入口。
- [../Source/fpstrue/UE5_GAME_FRAMEWORK_TECHNICAL_ROADMAP.md](../Source/fpstrue/UE5_GAME_FRAMEWORK_TECHNICAL_ROADMAP.md)：Gameplay Framework 与图形学学习路线。
- [../Source/fpstrue/FPS_PORTFOLIO_ACCEPTANCE_PLAN.md](../Source/fpstrue/FPS_PORTFOLIO_ACCEPTANCE_PLAN.md)：历史四周验收计划，当前待办以 PROJECT_TASK_CHECKLIST 为准。

## 维护规则

1. 当前实现发生变化时，先更新 FPS_Core_Technical_Summary，再更新任务状态和必要的证据文档。
2. 性能数字只写入 PERFORMANCE_BASELINE；其他文档引用数字并链接来源。
3. 历史方案保留日期和“当时状态”，不能覆盖成当前事实。
4. 专题文档保留学习过程，主文档只保留能用于复习和面试的结论。
5. 当前结论、面试速答和场景题只在主文档维护；学习记录允许保留当时的推导，但不得冒充当前实现。
