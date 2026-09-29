# 历史实验入口（冻结）

本目录的 13 个 `Run*.ps1` 从 `Tools/` 移入，保留原参数、注释、执行方式和输出格式，用于核对历史实验；不作为新的测试入口。部分脚本使用旧地图、硬编码机器路径或离屏渲染，不能直接与当前基线混用。旧报告对应的 CSV、Trace 和汇总脚本均未改动。

当前统一使用 `Tools/RunRenderCostMatrix.ps1`：

- `Tools/ExperimentProfiles/*.json`：人数、预热、时长、地图和实验组选项。
- `Tools/ExperimentProfiles/RenderCostCases.psd1`：共用采集设置、实验 CVar 与对应验收条件。
- `Source/fpstrue/Testing/Benchmarks/`：采集配置与生命周期；运行时诊断参数及埋点声明位于 `Source/fpstrue/Runtime/`。

新增实验不要复制历史脚本。先在配置表中登记实验条件，再由统一入口采集；新增玩法消融若需要不同验收条件，应明确实现该条件，不能套用渲染实验的正常战斗校验。
