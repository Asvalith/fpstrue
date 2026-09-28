# 附录：性能实验数据、异常与原始证据

本附录是[完整实验报告](Docs/Performance/EXPERIMENT_LOG.md)的数据层，不再复述逐阶段故事。以下所有路径默认相对于工程根目录；`Saved/Profiling/` 保存本机原始采集且被 Git 忽略，公开仓库只携带报告中的关键数值和可复核的文件名。未列出的旧目录不等于删除或判为无效；这里只收录支撑主报告判断的记录。

## A. 判定与统计口径

- 先读每批 `manifest.csv` 的 `Valid`、实际配置、人数和告警，再读 `run_summary.csv`、`group_summary.csv`、逐轮 CSV/日志。不同批次的均值不可直接相减。
- 后期矩阵把首尾帧按各批配置裁剪，先逐轮统计再等权平均；P95/P99 是逐轮分位数的均值。Trace session 秒、CSV 累计 FrameTime 秒和游戏世界时间不是同一个时间轴。
- `Frame` 是整帧指标；GT、RT、RHI、GPU 可重叠。GPU 父子 Pass、CPU Inclusive/Exclusive、不同线程 BLAS 不能相加或互换。
- `Exclusive/RenderThread/EventWait/Visibility` 是 CSV 等待代理；`WaitForGatherDynamicMeshElements` 是 Insights 命名事件。它们方向可比较，但不是相同计量对象。
- `SkinnedGeometryBuildBLAS` 是 CSV GPU 统计，不等于 RHIThread 上 `BuildAccelerationStructure_BottomLevel` 的 CPU 事件。
- 早期消费者消融使用简化玩法诊断 Runner；后期渲染矩阵固定高生命值和视点。任何一类都不能称为正常生命、自由战斗、Shipping 构建的完整验收。

## B. 已使用的实验批次与直接证据

| 主报告问题 | 本机原始目录，均位于 `Saved/Profiling/` | 优先核对 |
| --- | --- | --- |
| 人数增长和清理成本 | `FPS_FinalLOD_20260816/`、`LifecycleCleanup_80_20260816/` | 各 `summary.md`、CSV、日志 |
| 初始调用链及破坏性上界 | `InsightsFirstDiagnosis_20260824/`、`EnemyBottleneckDiagnostics_20260824_MovementFollowup/`、`ProfileGuidedAblation_20260824/` | `Baseline.utrace`、InsightsExport、开关 manifest |
| 历史 AI 降频 | `EnemyOptimizationAblationSeed1337_20260824/` | AllEnabled / NoAIThrottling 各三次 |
| 80 敌人单项消费者 | `UnverifiedConsumers80_20260831/`、`AnimationTierConfirm80_20260831/` | `manifest.csv`、`run_summary.csv`、`group_summary.csv`、逐轮截图/日志 |
| 旧版跨规模与完整玩法迁移 | `CurrentScaleMatrix_Warm_20260830/`、`Current160Insights_20260901/`、`FullGameplayEnemyContributionAligned_20260905/` | 跨版本与短采集限制 |
| 残余敌人阴影/RT、零敌人 | `RVO160_RenderAblation_InputLocked_20260908/`、`RVO_ZeroVs160_20260909/` | 原始单变量与同批 0/160 |
| 查询任务身份和开关 | `RenderWaitTaskTrace_20260909/`、`OcclusionQueryDiagnosis_Checked_20260909/` | `task_dependency_audit.md`、`render_wait_diagnosis.md`、TaskAudit CSV、四轮 manifest |
| HZB/Buffer2 路径与旧矩阵 | `OcclusionCandidatePathCheck_20260909/`、`OcclusionCandidates160_20260910/` | `path_validation.md`、`conclusions.md`；排除受污染 Hardware R3 |
| 分辨率短诊断 | `GpuAttribution_20260916/` | `REPORT.md`；原始门禁未全通过 |
| Buffer2 独立三轮 | `PerformanceCloseout_Buffer2Formal/` | `summary.md`、`group_summary.csv`、manifest |
| Lumen 反射/ScreenProbe 三轮 | `PerformanceCloseout_LumenFormal_R2/` | `summary.md`、`group_summary.csv`、manifest |
| 当前组合三轮 | `PerformanceCloseout_CombinedFormal/` | `summary.md`、`group_summary.csv`、`environment.json` |
| 当前默认 0/160 与内存 | `PerformanceCloseout_FinalDefaults/`、`PerformanceCloseout_MemorySoak180_R2/` | 逐轮 CSV、group_summary、manifest |

## C. 早期实验的逐轮对应关系

### 80 敌人消费者消融

统一条件：`Demonstration`、80 敌人、1600×900、VSync Off、seed 1337、预热 10 秒、采集 30 秒；每组独立运行三次，旧诊断 Runner 简化 HUD、声音和玩家伤害。数据主目录为 `Saved/Profiling/UnverifiedConsumers80_20260831/`。例如默认 Run1 对应 `AllEnabled_Run1/AllEnabled_Run1.csv`、同目录的 Benchmark 日志及 `AllEnabled_Run1.png`；关闭组同样使用组名＋Run 序号，Run2/3 类推。

| 关闭组目录前缀 | 日志中的消费者变化 | 默认三次目标时间 | 关闭后三次目标时间 | 判定 |
| --- | --- | --- | --- | --- |
| `NoMovementTiering` | Movement Full 16→80；Tick/帧 65.1→81.0 | Movement 均值 1.056 | 1.245 | 三次方向一致 |
| `NoAnimationSharing` | Follower 约 60.6→0；Mesh Tick Enabled 约 17→80 | Animation 0.806/0.813/0.836 | 1.217/1.227/1.235 | 三次区间不重叠 |
| `NoShadowTiering` | 投影敌人 5→80 | ShadowDepths 1.049/1.052/1.055 | 1.750/1.754/1.814 | 三次区间不重叠；Shadow Draw Calls 同向 |
| `NoRayTracingTiering` | RT Visible 12→80 | Skinned BLAS 0.195/0.200/0.202 | 0.473/0.506/0.506 | 三次区间不重叠 |
| `NoSkeletalLOD` | LOD 约 19/29/32→80/0/0 | Animation 0.818 | 0.805 | 档位生效，收益未建立 |
| `NoAnimationTiering` | 关闭动画 Tick 分级 | Animation 0.795 | 0.824 | 仅约 0.029 ms；整帧受 RT/RHI 快慢状态干扰 |

阴影限制使 Shadow Draw Calls 1013.4→637.0、总 RHI Draw Calls 2111.8→1655.0。默认组 `RayTracingBudgetRejected=0`，所以“RT 参与限制有效”不等于“独立 RT Top 12 上限再次筛掉候选”。具体预算值、频率、权重和阈值未做最优性扫描。

### 历史 AI 降频

`EnemyOptimizationAblationSeed1337_20260824/`：160 敌人，AllEnabled / NoAIThrottling 各三次。Decisions/帧 19.521/56.741，DecisionTime 0.215/0.726 ms，MoveRequests/帧 16.063/52.897，GT 28.179/37.081 ms。该批属于 Timer 版本，不作为当前 C++＋蓝图行为树版本的独立收益。

### 跨版本与完整玩法的失效样本

`CurrentScaleMatrix_Warm_20260830/` 的 160 敌人单次 GT 7.707、RT 30.411、Frame 30.899 ms；早期 `FPS_FinalLOD_20260816/` 的 GT 27.899 ms。两端不同版本且各一次，只能作为敌人侧成本下降的历史趋势，不能算成单参数 A/B 或稳态 60 FPS 证明。

`FullGameplayEnemyContributionAligned_20260905/` 中 0/160 各三次短采集，恢复 HUD、声音、伤害；两组经历的 Gameplay 过程不同，采集约 2 秒且有一次 160 敌人死亡被排除。那批 0/160 Frame 为 31.593/32.631、GT 9.618/19.014、RT 32.045/32.748 ms，出现 RT 慢状态。它不是后期固定高生命值 30 秒矩阵的对照组，不能与后期数值拼接。

## D. RT 遮挡查询等待：原始事件而非同名猜测

`RenderWaitTaskTrace_20260909/` 的 0/160 Trace 分别附 `TimingExport/` 和 `TaskAudit/`。代表样本：

| 样本 | RT `WaitForGatherDynamicMeshElements` | Worker `OcclusionCullPipe → SyncPoint_Wait` | Worker 等待的 GraphEvent 完成 | RT 结束 |
| --- | --- | --- | --- | --- |
| 0 敌人 median | 33.8660498–33.8711035 s | 33.8657843–33.8708915 s | Task 1410954，33.8708814 s，RHIInterruptThread | 33.8711035 s |
| 160 敌人 median | 46.4001061–46.4054233 s | 46.3998766–46.4052144 s | Task 2496165，46.4050694 s，RHIInterruptThread | 46.4054233 s |

两个 median 和两个 P95 样本均核对了原始 WaitId→TaskId、任务 Completed 和线程身份。整个捕获区间内，0/160 的 RT scope 分别为 1366/1199 次，与查询 SyncPoint 等待重叠的时间加权比例为 95.76%/95.24%。这些数值来自同线程嵌套及区间交集；手动 pipe/empty callback 不会完整体现为 TaskTrace 自动 prerequisite 边，因果链还结合 UE 5.5 本机源码核对。完整 TaskId 与源码位置在该批 `task_dependency_audit.md`，不能从本表进一步推断具体 GPU Pass。

`OcclusionQueryDiagnosis_Checked_20260909/` 的四轮 On→Off→On→Off 查询开关实验：CSV 可见性等待 5.137→0.042 ms、RT 13.256→9.040 ms、RHI 8.189→9.542 ms、Draw Calls 1860.6→2310.4、Frame 13.532→12.252 ms。关闭的代价是提交增加，不能把 5.095 ms 等待缩短直接当成 5.095 ms 整帧节省，也不能采用全局关闭查询。

历史 Insights 截图仅展示早期等待现场，不承担 TaskId 与 GPU fence 的直接证明：

![早期 160 敌人 Unreal Insights 样本](PerformanceEvidence/UnrealInsights_160Enemies.png)

## E. 查询与 GPU 候选的有效/无效数据

### HZB 路径和正式取舍

`OcclusionCandidatePathCheck_20260909/path_validation.md`：短测中 `WaitForGatherDynamicMeshElements` Baseline/Buffer2/HZB 平均 4.304/2.925/0.798 ms；HZB 独有的 `STAT_MapHZBResults` 出现 195 次，Inclusive 平均 14.092 ms，其中 119 次 Map 内显式 RT→RHI 等待。该短测只通过“切换路径”的门禁，不作净收益。

`OcclusionCandidates160_20260910/` 共九次原始运行，但 Hardware R3 与外部进程重叠，后验排除；不能引用自动生成的 9/9 平均。有效同轮配对中，HZB 比 Hardware 的 Frame 分别增加 1.000/6.329 ms，拒绝作为本场景默认策略。Buffer2 当时只有两个有效配对、尾帧并非全同向，随后另做独立三轮。

### Buffer2、反射和 ScreenProbe 的正式三轮

下表各行只在自身目录内对照；单变量结果不能相加，也不能与组合批次相减计算交互项。

| 批次 | 参数变化 | 基线 Frame / P95 / GPU | 候选 Frame / P95 / GPU | 判定 |
| --- | --- | --- | --- | --- |
| `PerformanceCloseout_Buffer2Formal/` | BufferedQueries 1→2 | 14.977 / 16.379 / 13.366 | 14.677 / 16.115 / 13.114 | 固定镜头小幅改善；RHI 10.543→11.693 |
| `PerformanceCloseout_LumenFormal_R2/` | Reflections DS 1→2 | 14.934 / 16.335 / 13.317 | 14.480 / 15.868 / 12.793 | 进入组合验证 |
| `PerformanceCloseout_LumenFormal_R2/` | ScreenProbe DS 16→32 | 14.934 / 16.335 / 13.317 | 14.966 / 16.912 / 13.302 | 整帧无益，P95 回退 |
| `PerformanceCloseout_CombinedFormal/` | Buffer2＋Reflections DS2 | 14.955 / 16.415 / 13.321 | 14.279 / 15.696 / 12.704 | 固定镜头组合收益；P99 17.254→16.409 |

组合批次采用 OriginalRenderPolicy / OptimizedRenderPolicy 各三次平衡顺序，30 秒采集、预热 15 秒、裁剪首 1 秒末 0.5 秒。两组的协调器采样人数均为 160/5 投影/12 RT；可见性等待代理 5.778→4.213 ms，RT 14.689→13.160，RHI 10.597→11.333，GPU LumenReflections 1.128→0.649，RHI Draw Calls 2566.0→2581.1。Draw Calls 近似持平，不得宣传成“合批提速”。`environment.json` 保存配置、二进制与地图指纹；当前 `Config/DefaultEngine.ini` 已设 Buffer2 和反射 DS2，但快速转镜视觉回归尚未完成。

### 当前默认的 0/160 对照

`PerformanceCloseout_FinalDefaults/` 各一轮，预热 15 秒、采集 30 秒、裁剪首 1 秒末 0.5 秒：

| 指标 | 0 敌人 | 160 敌人 |
| --- | ---: | ---: |
| Frame Mean / P95 / P99 | 11.868 / 13.063 / 13.831 | 14.296 / 15.731 / 16.400 |
| GT / RT / RHI / GPU | 2.448 / 11.547 / 9.069 / 10.335 | 7.806 / 13.061 / 11.359 / 12.733 |
| 可见性等待代理 | 2.970 | 4.210 |
| RHI Draw Calls / Shadow Draw Calls | 1872.1 / 936.3 | 2566.0 / 996.7 |
| GPU ScreenProbeGather / TSR | 1.754 / 1.524 | 1.938 / 1.599 |
| GPU LumenSceneLighting / ShadowDepths / Skinned BLAS | 1.285 / 0.761 / 0.042 | 1.377 / 1.149 / 0.219 |

这批是当前默认配置的底座分解，不能与上一行三轮组合批次混成六轮“最终成绩”。查询缓冲和反射参数已生效，0 敌人仍有约 1872 RHI Draw Calls 和 936 Shadow Draw Calls，因此后续场景绘制/阴影实验应先在零敌人视角确认对象与 Pass，而不是把全部成本归给敌人。

### 内存驻留

`PerformanceCloseout_MemorySoak180_R2/`：160 敌人，预热 20 秒、采集 180 秒，单轮 manifest 有效；该轮使用自身的原配置，不作为当前组合配置的帧时对照。CSV 中 `PhysicalUsedMB` 首/末 3624.29/3643.43，范围 3624.29～3655.08；`GPUMem/LocalUsedMB` 首/末 3691.34/3691.97，峰值 3723.97；`TextureStreaming/WantedMips` 约 147.79～152.65 MB。manifest 的 TexturePoolWarnings/VSMQueueOverflows 均为 0。只支持“该固定窗口未见快速单调增长和告警”，不支持“无泄漏”或“纹理池设置已经最优”。

## F. 已知异常与禁止复用的数字

| 原记录 | 问题 | 处理 |
| --- | --- | --- |
| HZB/Hardware/Buffer2 九轮旧自动汇总 | Hardware R3 与外部进程重叠 | 保留原始文件，仅用有效配对；不引用污染后的组均值 |
| `GpuAttribution_20260916/` 的 SP50 短诊断 | 原始两轮未通过全部门禁，实际内部分辨率未核实，P99 几乎不变 | 仅用于选择照明/TSR 候选，不作为正式分辨率收益 |
| 完整玩法 160 敌人固定站立长采集 | 正常生命下中途死亡 | 判无效；短测只验证采集链路 |
| 80 敌人动画 Tick 分级 | 局部 Animation 只差约 0.029 ms，RT/RHI 运行状态波动 | 净收益未建立，不宣传 Frame 改善 |
| 80 敌人 Skeletal LOD | LOD 档位改变，但目标成本不同向 | 只记机制生效，不宣传提速 |
| 早期 160 GT 27.899 与后期 7.707 | 跨版本、不同口径、各一次 | 历史趋势，不能作为单变量收益 |
| 最新单次 0/160 与三轮组合对照 | 批次与重复数不同 | 分别报告，不拼成一组平均 |
| 当前行为树与历史 Timer AI 降频 | 驱动实现已经迁移 | 历史证据只证明降低决策频率的设计方向 |

附录与主报告共同构成项目的两份维护文档：主报告只改变结论时更新，附录在新增有效实验时补入原始运行、门禁和计算依据。历史失败文件、被排除样本及大型 Trace 不清理，以便复核和避免重复测试。
