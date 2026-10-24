# 历史实验入口（冻结）

本目录保存冻结的历史启动、汇总和固定 CSV 分析脚本，只用于核对旧实验。部分入口依赖已移除的单机 GameMode、AutoBenchmark 或旧地图，不能对当前构建直接运行；历史报告对应的 CSV、Trace、配置和统计口径保持原样。

`RunRenderCostMatrix.ps1` 已从 `Tools/Performance/` 移入本目录并显式拒绝启动，避免删除单机 Runner 后静默生成无效实验。复现旧结果应使用对应历史版本，不在当前多人代码中恢复兼容分支。

当前固定 160 AI 的联网采集入口为 `Tools/Performance/RunGroupAIBaseline.ps1`，保留 Editor/Shipping 客户端、独立服务器、真实人口与消费者门禁、采样身份及结果。`Testing/Benchmarks/fpstrueBenchmarkRunner.*` 仍承载网络 Profile/Baseline，不再包含单机 UActorComponent。本轮入口整理未重跑性能，不将旧结果移称新构建成绩。

`Tools/Performance/ExperimentProfiles` 与历史汇总脚本继续用于理解旧实验条件，不作为当前启动推荐。新增网络实验沿用现有联网入口并明确自己的单因素、场景和验收口径，不复制历史脚本或混合不同构建的数据。
