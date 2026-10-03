# 工具入口

工具按用途分组，玩法源码仍按业务模块放在 `Source/fpstrue/`，可调玩法资产集中在 `Content/Config/`。本目录不保存实验输出或资产备份。

| 目录 | 用途 | 常用入口 |
|---|---|---|
| `Gameplay/` | 行为树资产生成/验证、波次/近战/武器配置迁移 | `CreateEnemyBehaviorTree.py`、`VerifyEnemyBehaviorTreeAssets.py`、`MigrateWeaponConfiguration.py` |
| `Assets/` | 网格、LOD、纹理、Nanite、VSM 候选审计与样条静态化 | `AuditVSMCandidates.py`、`AuditTextureAssets.py`、`BakeStaticLandscapeSplines.py`、`VerifyBakedLandscapeSplines.py` |
| `Performance/` | 当前性能采集、Trace 导出、CSV/事件分析与脚本回归 | `RunRenderCostMatrix.ps1`、`TestRenderCostConfig.ps1`、`ExportRenderWaitTrace.ps1`、`AnalyzeGpuTimingTrace.py` |
| `Performance/ExperimentProfiles/` | 实验条件、CVar 和读回要求 | `baseline160.json`、`scene-acceptance.json`、`RenderCostCases.psd1` |
| `Performance/Charts/` | 根据既有实验记录生成报告图表 | `render_closeout_charts.py`、`render_occlusion_evidence.py` |
| `LegacyPerformance/` | 冻结的历史启动、汇总和固定数据分析 | 仅用于核对旧实验，不作为新一轮采集入口 |

## 常用操作

以下命令从工程根目录运行。校验不会启动 Unreal，也不会产生新的性能证据：

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\TestRenderCostConfig.ps1
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\Performance\ExperimentProfiles\baseline160.json -ValidateOnly
```

正式采集使用同一入口，去掉 `-ValidateOnly`；启动前确认预设、地图和独占测试时段。现有基线和画质参数没有因目录整理而变化。

`Gameplay/` 和 `Assets/` 中依赖 `unreal` 的 Python 脚本使用 Unreal Editor 的 Python 环境，不是普通 Python 程序。`Migrate*`、`Create*`、`Configure*`、`Enable*`、`Bake*` 可能修改资产，运行前阅读脚本条件并保留备份；已迁移的武器日常调参直接编辑 Data Asset，不再运行迁移。

`Performance/` 中的 CSV/Trace 分析使用普通 Python；`Charts/` 额外依赖 Pillow 和脚本中指定的微软雅黑字体。UE 自带 Python 不一定包含 Pillow，不需要为运行这些离线图表脚本修改引擎环境。

## 数据与兼容边界

- `Saved/Profiling/`：原始 CSV、Trace 和采集产物。
- `Saved/Automation/`、`Saved/Logs/`：功能测试报告与日志。
- `Saved/Backups/`：配置迁移及资产改动前的备份。
- `Docs/Performance/figures/`：报告图片与证据摘要；图表脚本移动后仍输出到这里。
- 历史日志、manifest 和本地备份中的旧脚本路径保留为历史记录；重新运行时使用本页的新入口。没有在旧位置增加转发脚本。

详细参数及配置选源见 [配置说明](../Docs/CONFIGURATION.md)，历史实验边界见 [归档说明](LegacyPerformance/README.md)。
