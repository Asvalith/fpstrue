# 工具入口

当前玩法只维护多人 PvPvE。工具按职责保留一个主入口，不在正常启动时生成、迁移或修复资产。

| 工作 | 入口 | 边界 |
| --- | --- | --- |
| 网络功能与容量验证 | `Networking/RunFpsNetworkMatrix.ps1` | 调用同一 `RunPvpSmoke.ps1` 执行分项；正常、弱网、退款、恢复和容量分别验收 |
| 编译与打包交付 | `Networking/BuildPvpDelivery.ps1` | 管构建和批准测试名单，复用网络功能脚本 |
| 群体 AI 性能采集 | `Performance/RunGroupAIBaseline.ps1` | 同一联网场景通过参数切换对照，不复制启动脚本 |
| CPU / GPU Trace 导出 | `Performance/ExportPerformanceTrace.ps1` | 同一导出循环、窗口和校验；GPU 用 `-IncludeGpuEvents` 按需加入 |
| 普通入口双端表现检查 | `Networking/RunFpsVisualAcceptance.ps1` | 真实行走拾枪与生产输入，区别于烟测自动装备 |
| 蓝图与 C++ 合同核对 | `Gameplay/AuditProjectContracts.py` | 调用 Editor 只读审计，不修改或保存 Content |

`Assets/` 保留网格、LOD、纹理审计及显式资源处理工具。`Performance/Charts/` 从既有记录生成报告图；`ExperimentProfiles/` 保存旧实验条件，均不是当前游戏采集入口。

## 计划与执行

```powershell
# 只显示计划，不启动 UE 或产生新通过记录。
.\Tools\Networking\RunFpsNetworkMatrix.ps1 -Suite Functional -Profiles Normal,Weak -Repeat 1 -DryRun
.\Tools\Networking\BuildPvpDelivery.ps1 -Mode Editor -PlanOnly

# 实际采集需要本轮 Stage 内部的真实 Shipping EXE。
.\Tools\Performance\RunGroupAIBaseline.ps1 `
  -ShippingClientExecutable '<Stage>/Windows/fpstrue/Binaries/Win64/fpstrue-Win64-Shipping.exe' `
  -Repeat 3 -CaptureScreenshots -PresentMon -DryRun
```

网络分项参数由 `RunPvpSmoke.ps1` 维护，矩阵复用它，不新增转发脚本。`-MovingHit` 是零散布移动命中专项；`-WeakPolicyCorrectness` 只验证真实过期拒绝、单发退款和恢复，不能代替默认零意外拒绝门禁。`-ReconnectMode TransportOutage` 验证同进程断包和自动重连，进程重启恢复单独记录。

`BuildPvpDelivery.ps1 -Mode Delivery` 需要支持 Server 的配套引擎，`LinuxPackage` 另需同一源码引擎和交叉编译工具链。构建、Cook、真实运行、跨机和公网分别验收。远端检查使用已有部署及明确的 SSH/程序身份，不配置主机或防火墙。

纯离线检查不启动游戏：

```powershell
.\Tools\Networking\BuildPvpDelivery.ps1 -SelfTest
.\Tools\Networking\RunFpsNetworkMatrix.ps1 -SelfTest
.\Tools\Networking\RunPvpSmoke.ps1 -SelfTest
```

批准测试名单以构建脚本为准。缺失、重名、失败、未执行或明细不一致均不能通过；带警告的成功单独记录，不用固定数量掩盖未覆盖内容。

## 性能与离线分析

基线复用已存在地图 `/Game/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview55x192`，默认160 AI、两个玩家、1600×900、High（2）。三端校验真实人口、行为、构建、固定玩家视点和画质读回；不在 A/B 之间重建地图、冻结敌人或换场景。人数、视点、线程、命中代理、地面查询和遮挡参数均属于显式独立对照，不能混池宣称收益。

正式重复采集、`-CpuDiagnostic` 和 `-DiagnosticTrace -DiagnosticTraceOnly` 分别记录。Trace/命名事件有额外成本，不能并入无 Trace 正式基线；诊断只选需要的构建与角色，不重复整套运行。`-DryRun` 可核对完整参数与输出计划。

Insights CSV 统一由 `ExportPerformanceTrace.ps1` 导出。当前基线使用独立验证的完整帧时间窗 `-StartTime/-EndTime`；旧 Trace 有明确区域时才用 `-Region`。`-IncludeGpuEvents` 加入 `GpuEvents.csv` 和 `GpuStatistics.csv`，复用同一源 Trace、窗口、超时和文件校验。先结束采集，再离线导出。

分析工具各自处理不同证据，不启动游戏：

- `AnalyzeRenderWaitTrace.py`：CPU 嵌套范围、Task 依赖和帧对齐；范围时长不是 OS Ready/Blocked。
- `AnalyzeGpuTimingTrace.py`：分 GPU 时间线分析事件，父子范围不重复相加；CPU 等待重叠不能单独证明 GPU 因果。
- `analyze_csv_ab.py`、`AnalyzeResolutionProbe.py`：既有 CSV 的逐帧/分辨率对照，保留失败和无效样本。
- `Networking/MeasureFpsShotLatency.ps1`：逐发预测、裁决、确认与补发；应用提交次数不是 UDP 数据报数。
- `Networking/AnalyzeFpsNetworkTrace.py`：读取 `fp.ExportNetworkTrace` 原生包/Bunch，只比较同端时钟，保留业务关联限制。

## 资产与历史

正式 BT、Blackboard、Controller 和 Data Asset 是唯一配置来源，日常调整直接编辑正式资产。旧树生成/升级、单机转多人、首次配置拷贝、已完成的导航备份和表现迁移入口已删除，不保留空命令或兼容包装。核对资产使用只读合同审计；批量处理新资源才显式运行对应 `Assets/` 工具并保存备份。

`LegacyPerformance/` 和旧单机 RenderCost 配置/自测/汇总脚本已删除。原始 CSV、Trace、历史配置、图表和报告不变；历史脚本从对应 Git 版本读取，不恢复到当前启动链。文档引用旧实验名称不表示当前仍可执行。

- `Saved/Profiling/`、`Saved/Networking/`、`Saved/Automation/`：原始采集、功能结果及失败记录，不进入源码仓库。
- `Saved/Backups/`：已发生的资源/配置迁移备份，删除工具不删除备份。
- `Docs/Performance/figures/`：公开报告图与证据摘要。
- `-ddc=fpstrueEditorTestDDC` 只影响显式测试进程；旧缓存只读，新写入由 `-LocalDataCachePath` 指定。普通编辑器/游戏缓存不变，A/B 中途不切换缓存策略。

配置、实现和验收边界分别见[配置说明](../Docs/CONFIGURATION.md)、[网络合同](../Docs/FPS_NETWORK_ACCEPTANCE.md)与[验收记录](../Docs/REFACTOR_ACCEPTANCE.md)。历史通过不自动升级为当前构建的性能、公网或发布包通过。
