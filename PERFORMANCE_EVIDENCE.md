# 早期性能实验与消费者消融明细（2026-08-16～09-05）

本文保留早期实验的逐项数据和证据路径，作为历史明细。后续 RVO/Detour、零敌人对照、遮挡查询等待归因、候选策略及 GPU 分辨率诊断统一记录于[分阶段性能实验记录](Docs/Performance/EXPERIMENT_LOG.md)。

以下“当前版本”“新版 Runner”指对应采集时的版本，不代表最新代码或最新已验收基线；第六节是截至 09-05 的待办快照，不能据此认定后续实验尚未执行。

本文只记录能够由现有原始文件复核的结论，并明确区分：

- **定位证据**：说明某类工作量是瓶颈，但关闭后可能破坏玩法，不能直接当优化方案。
- **机制证据**：证明某个开关确实改变了目标消费者，并使对应局部成本同向变化。
- **端到端证据**：证明 Frame、GT、RT、RHI 或 GPU 在相同条件下稳定改善。
- **趋势证据**：来自不同版本或单次运行，只能说明演进方向，不能归因到单个参数。

原始采集文件位于 `Saved/Profiling/`。该目录不提交到 Git，因此本文保留数据摘要和相对路径，完整 CSV、日志、截图及 Trace 需要在本机工程中复核。

## 一、截至 09-05 的阶段结果

### 0. 完整玩法基线迁移验证

新版 Runner 已删除 `RemoveAllWidgets`、玩家无敌、静音/离屏运行和基准专用强制重叠生成。160 个敌人全部生成后，静止玩家会在约 3.7–5.4 秒内死亡，因此 15 秒预热、30 秒采集会被正确判为无效；不能通过关闭伤害来获得一份表面完整的数据。

为验证“生成完成 -> CSV/截图 -> 正常停止”的链路，在所有 `BenchmarkDisable*` 均为 0 的条件下完成了三次零预热、2 秒短采集，共 315 个有效帧：

| 指标 | 三次运行级平均 | 最小值 | 最大值 |
| --- | ---: | ---: | ---: |
| Frame | 15.473 ms | 14.762 ms | 16.114 ms |
| Frame P95 | 17.277 ms | 16.766 ms | 18.076 ms |
| Frame P99 | 17.603 ms | 16.917 ms | 18.476 ms |
| Game Thread | 9.305 ms | 8.748 ms | 10.380 ms |
| Render Thread | 15.464 ms | 14.786 ms | 16.120 ms |
| RHI Thread | 10.026 ms | 9.524 ms | 10.982 ms |
| GPU | 13.964 ms | 13.635 ms | 14.581 ms |

这组数据证明新版完整玩法采集链可以工作，并提示当前关键路径仍偏向 Render Thread；但每次只有约 2 秒、101–110 帧，且紧邻全部敌人生成完成，不能替代 30 秒稳态基线，也不能与旧版 `7.707 ms` 做严格单变量比较。下一步应通过确定性的玩家移动/战斗回放延长存活和采样时间，而不是关闭任何玩法系统。

#### 当前版本0敌人长采样

为观察不包含敌人时的固定渲染底座，使用当前源码在相同地图、1600×900、VSync Off、正常UI/声音/伤害和全部渲染功能开启的条件下，执行了一次0敌人测试：种子1337、预热15秒、采集30秒，共1312帧。日志确认`requested=0 alive=0`、所有破坏性`BenchmarkDisable*`开关均为0，采集正常停止。

| 指标 | Mean | P95 | P99 |
| --- | ---: | ---: | ---: |
| Frame | 22.606 ms | 33.540 ms | 35.769 ms |
| Game Thread | 6.179 ms | 9.134 ms | 12.940 ms |
| Render Thread | 22.107 ms | 34.143 ms | 37.964 ms |
| RHI Thread | 15.006 ms | 23.659 ms | 27.303 ms |
| GPU | 12.047 ms | 13.076 ms | 13.957 ms |

该次运行还有`Visibility Wait`平均5.238 ms、`RenderOther`平均5.573 ms、约1111个Draw Call。它说明即使没有敌人，本轮Render Thread仍可能成为关键路径，但这只是一次独立进程样本，并且落入此前观察到的RT/RHI慢模式。它不能直接与前面的160敌人2秒短采样相减：两者预热、采集长度和进程状态不同，移除敌人也可能改变遮挡与可见内容，更高的RT不能单独确定原因。为缩小这些差异，下面补充同一阶段版本上的三轮0/160配对测试。

原始证据：

```text
Saved/Profiling/PerformanceBaseline0_FullGameplay_20260905/
```

#### 当前版本0/160敌人三轮配对补测

为了缩小上述单次长采样与160敌人短采样之间的口径差异，又在当前版本补做了三轮0/160敌人交错测试。两组均使用`Demonstration`地图、1600×900、VSync Off、种子1337、完整HUD/声音/伤害和全部正常玩法消费者，且显式设置`t.IdleWhenNotForeground=0`。每次采集2秒；0敌人组预热10秒，160敌人组使用分帧生成完成前约10秒作为自然预热，生成完成后立即采集。这样能够大致对齐两组从地图加载到采集开始的时间，但两组经历的Gameplay过程仍不相同，因此属于配对贡献测试，不是严格实验室A/B。

每组保留3次有效结果，每次约50–55帧。另有1次160敌人运行因为玩家在采集结束前死亡，被Runner明确标记为`Automated benchmark aborted`并排除；没有为补齐样本关闭伤害或修改敌人行为。

| 指标 | 0敌人 | 160敌人 | 160 - 0 |
| --- | ---: | ---: | ---: |
| Frame Mean | 31.593 ms | 32.631 ms | +1.038 ms |
| Frame P95 | 34.231 ms | 34.259 ms | +0.028 ms |
| Game Thread | 9.618 ms | 19.014 ms | +9.396 ms |
| Render Thread | 32.045 ms | 32.748 ms | +0.703 ms |
| RHI Thread | 11.337 ms | 15.653 ms | +4.316 ms |
| GPU | 14.608 ms | 15.645 ms | +1.037 ms |
| CharacterMovement | 0.100 ms | 3.590 ms | +3.490 ms |
| Animation | 0.285 ms | 1.413 ms | +1.128 ms |
| TickActors | 0.185 ms | 1.175 ms | +0.990 ms |
| Significance更新 | 0.014 ms | 0.870 ms | +0.856 ms |
| ShadowDepths | 1.014 ms | 1.503 ms | +0.489 ms |
| Skinned BLAS | 0.055 ms | 0.251 ms | +0.196 ms |
| RHI Draw Calls | 1793 | 2837 | +1044 |
| GPU本地显存 | 3286.7 MB | 3506.0 MB | +219.3 MB |

这组数据支持以下判断：

- 敌人对GT有明确贡献，当前三轮运行级均值增加约9.396 ms。可见局部增量主要包括Movement 3.490 ms、Animation 1.128 ms、TickActors 0.990 ms和Significance集中更新0.856 ms；其余还包含物理等待、组件Tick和Gameplay调用链，不能把GT增量全部归因给AI决策本身。
- 敌人也增加了RHI提交、GPU和资源成本：RHI约增加4.316 ms、Draw Call约增加1044、GPU约增加1.037 ms、显存约增加219 MB。只有5个敌人投射阴影、12个敌人参与骨骼RT时，ShadowDepths和Skinned BLAS仍分别增加约0.489 ms和0.196 ms。
- 总Render Thread只增加约0.703 ms，不表示敌人“没有渲染成本”。0敌人组自身已有约32.045 ms RT底座，当前整帧仍落在RT约32 ms的慢状态；固定场景与线程同步/帧节奏掩盖了部分敌人增量。此时应该同时看RHI、GPU、Draw Call和目标局部事件，而不能只看RT总值。
- 两组Frame Mean和P95几乎相同，说明当前Frame主要被RT慢状态限制，不能据此声称加入160个敌人没有性能影响。0敌人Run 1还出现过一次约451 ms的采集起始异常帧，因此本组P99不用于归因。

即使显式关闭后台Idle和VSync，本批独立进程仍复现了RT约32 ms的慢状态，原因尚未由现有CSV确定。后续若要解释这一慢状态，应对0/160各采一份时间对齐的Insights，检查RT等待、Present/帧节奏、可见性任务和场景提交；不能把它直接归因给敌人或场景建筑。

原始证据：

```text
Saved/Profiling/FullGameplayEnemyContributionAligned_20260905/
```

### 1. 当前已经建立机制证据的策略

当前最可靠的单变量证据来自：

```text
Saved/Profiling/UnverifiedConsumers80_20260831/
```

统一条件为 `Demonstration`、80 个敌人、1600×900、关闭 VSync、种子 1337、预热 10 秒、采集 30 秒；每组独立进程运行三次，组别随机顺序。日志均确认 `requested=80 alive=80`，采集正常停止，VSM 队列溢出和纹理池告警为 0。该组数据由旧版诊断 Runner 采集，当时 HUD、声音和玩家受伤不参与，因此只用于同一旧测试口径下的消费者消融，不作为完整玩法基线。

| 机制 | 开启时 | 关闭时 | 对应局部成本变化 | 三次方向 | 结论 |
| --- | ---: | ---: | ---: | --- | --- |
| Movement 分级 | 65.1 次 Movement Tick/帧，1.056 ms | 81.0 次/帧，1.245 ms | 开启后减少 0.189 ms，约 15.2% | 三次一致 | 有效 |
| Animation Sharing | 60.6 个 Follower，Animation 0.818 ms | 0 个 Follower，1.226 ms | 开启后减少 0.408 ms，约 33.3% | 三次一致 | 有效 |
| 敌人阴影参与限制 | 5 个投影敌人，ShadowDepths 1.052 ms | 80 个，1.773 ms | 开启后减少 0.721 ms，约 40.7% | 三次一致 | 有效 |
| 敌人骨骼 RT 参与限制 | 12 个 RT Visible，Skinned BLAS 0.199 ms | 80 个，0.495 ms | 开启后减少 0.296 ms，约 59.8% | 三次一致 | 有效 |
| Skeletal LOD | LOD0/1/2+ = 19.4/28.7/31.8 | 80/0/0 | Animation 反而为 0.818/0.805 ms | 无正收益 | 档位生效，收益未建立 |

阴影限制还提供了第二条独立证据：

- Shadow Draw Calls：`1013.4 -> 637.0`，减少约 376.4，约 37.1%。
- 总 RHI Draw Calls：`2111.8 -> 1655.0`，减少约 456.8，约 21.6%。

因此按当前可复核的**局部绝对收益**排序：

1. 敌人阴影参与限制：`0.721 ms ShadowDepths`，同时显著减少 Draw Call。
2. Animation Sharing：`0.408 ms Animation`。
3. 敌人骨骼 RT 参与限制：`0.296 ms Skinned BLAS`。
4. Movement 分级：`0.189 ms CharacterMovement`。

这不是统一线程上的总收益排名：阴影和 BLAS 属于 GPU/渲染工作，Animation Sharing 和 Movement 主要属于 GT；这些局部数值不能直接相加为 Frame 收益。

按目标消费者区分局部结果：

- 当前 GT 局部收益最清楚的是 Animation Sharing，其次是 Movement 分级。
- 当前渲染局部绝对收益最大的是阴影参与限制。
- 按目标子项的相对降幅，骨骼 RT 参与限制对 Skinned BLAS 的降幅最大，约 59.8%。
- 当前没有任何单项能够被严谨地称为“Frame 收益最大”，因为 RT/RHI 存在进程级快慢模式。

### 2. 尚未建立独立收益或最优性的参数

| 参数或机制 | 当前证据 | 判定 |
| --- | --- | --- |
| 动画 Tick 分级：Reduced `1/30 s`、Background `0.05 s` | 关闭后 Animation 仅增加约 0.029 ms；不同进程出现 RT/RHI 快慢模式，开启组 Frame 反而更慢 | 不能作为已验证成果，存在负收益风险 |
| Skeletal LOD | LOD 档位确实改变，但 Animation、Skinned BLAS、骨骼 RT 内存和 Primitives 没有稳定改善 | 只证明实现生效，没有独立净收益 |
| RT 独立 Top 12 | 默认 `RayTracingBudgetRejected=0` | Top 12 没有在 Full Render 之外再次筛人，独立预算未生效 |
| Render 权重 `0.45/0.30/0.15/0.10` | 没有同一提交上的单变量权重 A/B | 未证明最佳 |
| Render 阈值 `0.70/0.60/0.30/0.20` | 没有阈值扫描 | 未证明最佳 |
| Significance 更新间隔 `0.25 s` | 只测到更新本身很便宜，没有响应时间/间隔 A/B | 未证明最佳 |
| Shadow Top 5、Full Top 12 | 消费者上限确实生效，但没有 5/8/12/不限的质量与成本扫描 | 机制有效，数值未证明最佳 |
| 阴影距离 5000、RT 距离 10000 | 没有距离单变量扫描 | 未证明最佳 |
| MoveTo 每帧 8、战斗预留 2 | 没有当前代码下关闭预算的重复 A/B | 已实现，独立收益未建立 |
| 并发攻击者 8 | 主要是围攻节奏参数，没有性能 A/B，也不应只按性能确定 | 玩法参数，不作为性能成果 |

## 二、证据链：从发现瓶颈到验证消费者

### 阶段 A：规模矩阵发现敌人 CPU 扩展问题

早期矩阵：

```text
Saved/Profiling/FPS_FinalLOD_20260816/
```

| 敌人数 | GT | Movement | Animation | AI Decision | MoveTo/帧 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 20 | 5.879 ms | 0.745 ms | 0.977 ms | - | - |
| 80 | 16.106 ms | 3.231 ms | 2.728 ms | - | - |
| 160 | 27.899 ms | 6.188 ms | 4.448 ms | 0.201 ms | 14.565 |

这组数据证明：敌人数增加时，GT、CharacterMovement 和 Animation 明显增长，160 敌人时 GT 已超过 16.67 ms。它能确定优化方向，但每档只有一次运行，不负责证明某个后续参数的净收益。

### 阶段 B：Insights 确认等待链和高成本调用

对应 Trace 与导出：

```text
Saved/Profiling/InsightsFirstDiagnosis_20260824/Baseline.utrace
Saved/Profiling/InsightsFirstDiagnosis_20260824/InsightsExport/
```

稳定区间约 413 帧。根据导出的 Timer Statistics，换算出的近似每帧成本如下；Timer 可能存在父子包含关系，不能彼此相加：

| 线程/任务 | Trace 中的主要事件 | 近似成本 | 表明什么 |
| --- | --- | ---: | --- |
| Game Thread | `UCharacterMovementComponent_TickComponent` | 约 3.3 ms/帧 | 移动组件是主要敌人 CPU 消费者 |
| Game Thread | `USkeletalMeshComponent_CompleteParallelAnimationEvaluation` | 约 2.8 ms/帧 | 即使动画任务并行，GT仍要完成/等待结果 |
| Game Thread | `GameThreadWaitForTask` | 约 4.7 ms/帧 | GT 中存在明显任务等待，不能只看函数自身耗时 |
| Render Thread | `RayTracing_GatherDynamicRayTracingInstances` | 约 2.35 ms/帧 | 动态光追实例收集是 RT 工作量 |
| Render Thread | `...GatherDynamic...Finish` | 约 1.50 ms/帧 | RT 在等待动态实例收集任务完成 |
| RHI | `RayTracingDynamicGeometryUpdate` | Inclusive 约 8.96 ms/帧 | 动态几何更新形成关键链路 |
| RHI | `BuildAccelerationStructure_BottomLevel` | 总计约 9.88 ms/帧 | BLAS 构建是初始版本的重要成本；与上一项有包含关系 |
| Worker | `RayTracingMeshBatchTask` | 约 2.74 ms/帧 | 动态光追还占用工作线程 |
| Game Thread | `FpstrueEnemy_AttackSweep` | 约 0.008 ms/帧 | 刀具 Sweep 不是当时主要瓶颈 |
| Game Thread | `FpstrueEnemyAI_UpdateAI` | 约 0.08 ms/帧自身耗时 | AI 函数自身不大，频率和下游移动/寻路更重要 |

这份 Insights 证据解释了为什么后续要分别处理 Movement、Animation、动态骨骼 RT 和提交/等待链。它属于早期版本定位证据，不是当前版本参数 A/B。

注意：历史文件 `InsightsLoaded.png` 实际是浏览器画面，不是可用的 Unreal Insights 截图。当前版本已经重新采集 160 敌人 Trace，最终只保留下面这一张可复核的 Insights 界面截图；历史错误截图不作为证据。

![当前版本 160 敌人 Unreal Insights](PerformanceEvidence/UnrealInsights_160Enemies.png)

### 阶段 C：破坏性关闭确定成本上界

证据目录：

```text
Saved/Profiling/EnemyBottleneckDiagnostics_20260824_MovementFollowup/
Saved/Profiling/ProfileGuidedAblation_20260824/
```

| 关闭项 | 目标计数变化 | GT 局部/整体变化 | 正确解释 |
| --- | --- | --- | --- |
| CharacterMovement Tick Off | Movement Tick `149.5 -> 1` | Movement 减少 5.103 ms，GT 减少 13.495 ms | 证明移动及其下游很重，但敌人无法正常移动，不能作为正式方案 |
| PathFollowing Tick Off | PathFollowing Tick `160 -> 0` | Movement 减少 3.792 ms，GT 减少 12.822 ms | 证明路径跟随会驱动大量下游工作，不代表 PathFollowing 函数自身值 12.8 ms |
| SkeletalMesh Tick Off | Skeletal Tick `162 -> 2` | Animation 减少 3.252 ms，GT 减少 9.434 ms | 证明骨骼更新成本上界，但会破坏动画、Socket和攻击表现 |
| Attack Sweep Off | Sweep 变为 0 | 仅约 0.006–0.008 ms | 不是主要优化目标 |
| Pawn Collision Off | 碰撞关闭 | 没有稳定正收益 | 不能据此删除敌人碰撞 |

这一阶段的作用是排优先级：应优化移动、动画和路径请求节奏，而不是优先重写攻击 Sweep。

### 阶段 D：AI 更新节奏的历史单变量证据

证据目录：

```text
Saved/Profiling/EnemyOptimizationAblationSeed1337_20260824/
```

160 敌人、每组三次：

| 指标 | All Enabled | No AI Throttling | AI 降频带来的变化 |
| --- | ---: | ---: | ---: |
| AI Decisions/帧 | 19.521 | 56.741 | 减少 65.6% |
| AI Decision | 0.215 ms | 0.726 ms | 减少 0.511 ms，约 70.4% |
| Move Requests/帧 | 16.063 | 52.897 | 减少 69.6% |
| Game Thread | 28.179 ms | 37.081 ms | 差值 8.902 ms |

Decision Count、Decision Time 和 Move Request Count 三条指标方向一致，说明“按状态/距离降低 AI 更新频率”有效。GT 差值还包含移动、寻路和任务等待的连锁变化，不能把 8.902 ms 全部说成纯 AI 函数收益。

这组实验早于当前最终架构，因此适合证明设计方向，不用于宣称当前版本仍精确节省 8.902 ms。当前默认参数为：攻击 0.1 s、追击 0.25 s、远距离 0.5 s、Idle 1.0 s；Reduced/Background 再乘 1.5/2.0。具体数值没有逐项扫描，不能说最优。

### 阶段 E：当前消费者单变量消融

当前最强证据目录：

```text
Saved/Profiling/UnverifiedConsumers80_20260831/
Saved/Profiling/AnimationTierConfirm80_20260831/
```

每个当前消融组都有 CSV、日志和同次进程截图。Run1 的直接对应关系如下，Run2/Run3 延续相同目录命名：

| 组别 | CSV | 日志 | 截图 |
| --- | --- | --- | --- |
| 默认 | `AllEnabled_Run1/AllEnabled_Run1.csv` | `AllEnabled_Run1/Benchmark_UnverifiedConsumers80_20260831_AllEnabled_Run1.log` | `AllEnabled_Run1/AllEnabled_Run1.png` |
| Movement Off | `NoMovementTiering_Run1/NoMovementTiering_Run1.csv` | `NoMovementTiering_Run1/Benchmark_UnverifiedConsumers80_20260831_NoMovementTiering_Run1.log` | `NoMovementTiering_Run1/NoMovementTiering_Run1.png` |
| Sharing Off | `NoAnimationSharing_Run1/NoAnimationSharing_Run1.csv` | `NoAnimationSharing_Run1/Benchmark_UnverifiedConsumers80_20260831_NoAnimationSharing_Run1.log` | `NoAnimationSharing_Run1/NoAnimationSharing_Run1.png` |
| Shadow Off | `NoShadowTiering_Run1/NoShadowTiering_Run1.csv` | `NoShadowTiering_Run1/Benchmark_UnverifiedConsumers80_20260831_NoShadowTiering_Run1.log` | `NoShadowTiering_Run1/NoShadowTiering_Run1.png` |
| RT Tier Off | `NoRayTracingTiering_Run1/NoRayTracingTiering_Run1.csv` | `NoRayTracingTiering_Run1/Benchmark_UnverifiedConsumers80_20260831_NoRayTracingTiering_Run1.log` | `NoRayTracingTiering_Run1/NoRayTracingTiering_Run1.png` |
| Skeletal LOD Off | `NoSkeletalLOD_Run1/NoSkeletalLOD_Run1.csv` | `NoSkeletalLOD_Run1/Benchmark_UnverifiedConsumers80_20260831_NoSkeletalLOD_Run1.log` | `NoSkeletalLOD_Run1/NoSkeletalLOD_Run1.png` |
| Animation Tick Off | `NoAnimationTiering_Run1/NoAnimationTiering_Run1.csv` | `NoAnimationTiering_Run1/Benchmark_UnverifiedConsumers80_20260831_NoAnimationTiering_Run1.log` | `NoAnimationTiering_Run1/NoAnimationTiering_Run1.png` |

汇总关系为：`manifest.csv` 校验运行完整性，`run_summary.csv` 保留每次结果，`group_summary.csv` 汇总三次均值，`comparison_vs_all_enabled.csv` 计算关闭后差值。局部结论仍回查每次 Run，避免组均值掩盖方向不一致。

#### Movement 分级

日志证明开关生效：

- 默认快照：`movementFull=16 movementMid=28 movementFar=36`。
- 关闭组：`movementFull=80 movementMid=0 movementFar=0`。
- CSV：默认 Movement Tick 平均 65.1/帧，关闭后恒为 81.0/帧。
- CSV：CharacterMovement 三次默认约 `1.030–1.090 ms`，关闭后三次约 `1.210–1.311 ms`，区间不重叠。

因此可以证明 Movement 分级有效。当前 Reduced 约 30 Hz、Background 约 20 Hz，但本实验只比较“分级/全速”，没有证明 30/20 Hz 是最佳频率。

#### Animation Sharing

日志证明开关生效：

- 默认 `running=1`，约 60.6 个 Follower；关闭组明确记录 `ablation=1`，Follower 为 0。
- 默认 SkeletalMesh Tick Enabled 快照约 17；关闭 Sharing 后为 80。
- CSV：默认 Animation 三次为 `0.806/0.813/0.836 ms`；关闭后三次为 `1.217/1.227/1.235 ms`，完全不重叠。

这是当前最可靠的 GT 局部优化证据。它证明复用机制有效，不证明 `idleLeaders=4`、`movingLeaders=4` 和阈值 0.20 是最佳配置。

#### 阴影参与限制

日志证明 `features[shadow=1] -> features[shadow=0]`，投影敌人 `5 -> 80`。

三次原始结果：

- 默认 ShadowDepths：`1.049/1.052/1.055 ms`。
- 关闭限制：`1.750/1.754/1.814 ms`。
- 默认 Shadow Draw Calls：约 `622–666`。
- 关闭限制：约 `947–1103`。

局部成本与 Draw Call 都三次同向且区间不重叠，因此“限制敌人阴影参与数量”是强证据。Top 5 只代表当前测试值，仍需结合动态转身、近距离攻击和阴影跳变做 5/8/12 的画质回归。

#### 骨骼 RT 参与限制

日志证明 `features[rt=1] -> features[rt=0]`，RT Visible `12 -> 80`。

- 默认 Skinned BLAS：`0.195/0.200/0.202 ms`。
- 关闭限制：`0.473/0.506/0.506 ms`。

三次方向稳定，说明减少参与动态骨骼 RT 的敌人数能够降低 BLAS 成本。

但是默认组 `RayTracingBudgetRejected=0`。Full Render 上限和 RT 上限都为 12，流程实际是：

```text
80 个敌人
-> Full Render 最多 12
-> RT 候选最多 12
-> RT 上限也是 12
-> 独立 RT Top 12 没有再次拒绝对象
```

所以已证明的是“敌人 RT 参与限制”有效，尚未证明独立 `MaxRayTracingEnemies=12` 有效。验证独立预算时应暂时让 Full 候选超过 12，再比较 RT 8/12/不限，并要求 `RayTracingBudgetRejected` 在限额组大于 0。

#### Skeletal LOD

日志和 CSV 证明 LOD 分配确实从约 `19/29/32` 变成 `80/0/0`，但目标成本没有建立：

- Animation：默认 0.818 ms，关闭 LOD 0.805 ms。
- Skinned BLAS：默认约 0.199 ms，关闭约 0.193 ms。
- Animation Parallel Task、SkeletalMesh Tick、RT Geometry Resident Size 没有稳定改善。
- RHI Primitives 有较大运行波动，敌人即时位置也不同，不能归因给 LOD。

因此只能说“LOD 已接入并改变档位”，不能说“LOD 已获得性能收益”。后续应补充固定姿态/固定距离的渲染专项场景，记录每个 LOD 的顶点/骨骼规模、GPUSkinCache 或等价 Skinned Mesh 成本，并配合画质截图。

#### 动画 Tick 分级

四次紧配对确认结果：

| 指标 | 默认启用 | 关闭动画 Tick 分级 | 关闭后差值 |
| --- | ---: | ---: | ---: |
| Animation | 0.795 ms | 0.824 ms | +0.029 ms |
| GT | 6.729 ms | 6.558 ms | -0.171 ms |
| Frame | 28.535 ms | 24.049 ms | -4.486 ms |
| RT | 28.067 ms | 23.568 ms | -4.499 ms |
| RHI | 25.148 ms | 20.598 ms | -4.550 ms |
| GPU | 9.048 ms | 9.622 ms | +0.574 ms |

局部 Animation 只节省约 0.03 ms；Frame 差异主要来自 RT/RHI 快慢状态，与目标消费者不对应。关闭组四次 Frame Mean 又在约 `18.85–28.00 ms` 之间波动。因此动画 Tick 分级当前只能标记为“净收益未建立”，不能把 4.486 ms 解释成该参数的负收益，0.029 ms 差值也不足以确认稳定收益。

## 三、CSV、Insights、日志、截图和消融的对应关系

| 要回答的问题 | 消融/对照 | CSV | Insights | 日志 | 截图 | 证据等级 |
| --- | --- | --- | --- | --- | --- | --- |
| 最初 GT 为什么随敌人数上升 | 20/80/160 规模矩阵 | `FPS_FinalLOD_20260816/CSV/` | `InsightsFirstDiagnosis_20260824/Baseline.utrace` 与导出 | 对应运行日志 | 各规模截图 | 定位证据，强 |
| Movement 是否是高成本类别 | Movement Tick Off、PathFollowing Tick Off | `EnemyBottleneckDiagnostics_20260824_MovementFollowup/` | 初始 Trace 中 Movement/Wait 事件 | manifest 与诊断快照 | 每组截图 | 成本上界，强；不是可用方案 |
| Skeletal Tick 是否昂贵 | SkeletalMesh Tick Off | `ProfileGuidedAblation_20260824/` | 初始 Trace 的动画完成事件 | manifest 与诊断快照 | 每组截图 | 成本上界，强；不是可用方案 |
| AI 降频是否减少工作 | NoAIThrottling | `EnemyOptimizationAblationSeed1337_20260824/` | 没有同组 A/B Trace | 日志确认开关与 Alive | 每组截图 | 历史机制证据，较强 |
| Movement 分级是否有效 | NoMovementTiering | `UnverifiedConsumers80_20260831/` | 无当前配对 Trace | 80 Full 与 Tick 快照 | 同机位动态截图 | 当前机制证据，强 |
| Animation Sharing 是否有效 | NoAnimationSharing | 同上 | 无当前配对 Trace | Follower `约61 -> 0` | 同机位动态截图 | 当前机制证据，强 |
| 阴影限制是否有效 | NoShadowTiering | 同上 | 无当前配对 Trace | Shadow `5 -> 80` | 同机位截图，只能人工检查明显画面错误 | 当前机制证据，强 |
| RT 参与限制是否有效 | NoRayTracingTiering | 同上 | 初始 Trace 解释 BLAS 来源，但不是本组 A/B | RT Visible `12 -> 80` | 同机位截图 | 当前局部证据，强 |
| Skeletal LOD 是否有效 | NoSkeletalLOD | 同上 | 无当前配对 Trace | LOD `约19/29/32 -> 80/0/0` | 动态截图无法精确比较 LOD | 只证明生效，收益不足 |
| 动画 Tick 分级是否有效 | NoAnimationTiering | `AnimationTierConfirm80_20260831/` | 无当前配对 Trace | feature 开关与快照 | 同机位动态截图 | 净收益未建立 |
| 当前多敌人 GT 扩展 | 20/80/160 当前矩阵 | `CurrentScaleMatrix_Warm_20260830/CSV/` | `Current160Insights_20260901/Baseline.utrace` 与导出 | Alive 与消费者快照 | `PerformanceEvidence/UnrealInsights_160Enemies.png` | 当前趋势证据；规模矩阵每档仅一次，Trace 为另一次 160 敌人诊断运行 |

截图边界：自动脚本固定了地图、分辨率和相机方向，但 AI 在预热期间持续移动和攻击，所以不同进程中的敌人位置与姿态并不完全相同。截图用于证明场景、机位、敌人数和功能没有明显失效，不是逐像素性能 A/B。若要比较 LOD、阴影跳变等画质，应另建固定角色位置和固定姿态的静态画质场景。

版本边界：当前 manifest 没有记录 Git commit、显卡驱动和完整 Scalability/RHI 配置。日志能够证明敌人数、种子、策略开关和主要预算，但还不能把运行二进制与某个提交做完全追溯绑定。所有现有结果也来自 Development Editor 独立进程，不等同于 Shipping 构建性能。

## 四、历史规模结果与跨版本比较边界

当前热缓存矩阵：

```text
Saved/Profiling/CurrentScaleMatrix_Warm_20260830/
```

| 敌人数 | Frame | P95 | GT | RT | RHI | GPU | Movement | Animation |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 20 | 20.359 ms | 33.290 ms | 4.532 ms | 19.985 ms | 16.568 ms | 8.479 ms | 0.382 ms | 0.706 ms |
| 80 | 14.023 ms | 25.712 ms | 6.273 ms | 13.650 ms | 10.772 ms | 8.691 ms | 1.275 ms | 0.881 ms |
| 160 | 30.899 ms | 37.975 ms | 7.707 ms | 30.411 ms | 27.593 ms | 9.039 ms | 1.636 ms | 0.814 ms |

在该次 Development Editor 样本中，160 个敌人的 Game Thread 平均为 7.707 ms；整帧受 Render/RHI 链路限制，未稳定达到 60 FPS。

20、80、160 的 Frame 和 RT 不呈单调关系，说明存在进程级渲染波动。该矩阵每档只有一次，因此主要证明“当前策略的消费者数量受控”和“GT 没有随敌人数线性爆炸”，不能用来证明 80 敌人比 20 敌人更快。

为确认当时 160 敌人的稳态限制，随后使用相同地图、分辨率、敌人数和随机种子，预热 15 秒后另采集 10 秒 Trace。它同样属于旧版诊断口径，不是新版完整玩法基线：

```text
Saved/Profiling/Current160Insights_20260901/Baseline.utrace
Saved/Profiling/Current160Insights_20260901/InsightsExport/
PerformanceEvidence/UnrealInsights_160Enemies.png
```

该次 CSV 有 704 个有效采样值：Frame P95 为 15.280 ms，GT P95 为 9.280 ms，RT P95 为 14.830 ms，RHI P95 为 12.490 ms，GPU P95 为 9.420 ms。`Exclusive/RenderThread/EventWait/Visibility` P95 约 4.650 ms，说明当前样本更接近 Render/RHI 提交和可见性任务等待限制，GT 和纯 GPU 着色都不是稳态首要限制。

早期 160 敌人与当前 160 敌人的趋势对照为：

| 指标 | 早期 | 当前 | 变化 |
| --- | ---: | ---: | ---: |
| GT | 27.899 ms | 7.707 ms | -72.4% |
| Movement | 6.188 ms | 1.636 ms | -73.6% |
| Animation | 4.448 ms | 0.814 ms | -81.7% |
| AI Decision | 0.201 ms | 0.039 ms | -80.6% |
| MoveTo/帧 | 14.565 | 0.109 | -99.3% |
| Frame | 27.907 ms | 30.899 ms | +10.7% |

这是不同二进制、不同预热条件、每边一次的历史趋势，不是严格 A/B。它能说明敌人侧 GT 扩展成本显著下降，同时也明确显示整体 Frame 没有同步改善。

## 五、尚未完成独立量化的机制

### HUD 事件驱动

已有多敌人 CSV 来自旧版 Benchmark Runner：它在采集前移除了 HUD，因此这些历史数据不能证明 HUD 事件驱动节省了多少 GT。当前 Runner 已禁止移除 Widget，并在正式基线中保留声音、玩家受伤/死亡和全部正常消费者；完整玩法基线需重新采集后才能替换历史数据。若要单独量化 HUD 改造，仍应在不影响其他玩法的前提下比较 Binding 与 Event Driven 两组，并记录 `Exclusive/GameThread/UI`、Slate Prepass/Paint 和属性读取次数。

### 分帧生成

Benchmark 等待全部敌人生成完毕、再预热 10–15 秒才开始采集，所以稳态 CSV 不包含生成尖峰。现有代码和 Spawn Interval 只能证明功能实现，不能证明尖峰降低。需要比较“同帧生成”和“0.05 s 分批生成”的 Frame Max/P99、单敌人 Spawn Time 和波次完成时间。

### MoveTo 请求预算

当前版本 MoveTo 提交/帧很低，但现有证据主要是跨版本趋势，且没有 `NoMoveToBudget` 当前重复组。路径目标去重、失败退避和每帧预算可能共同贡献，尚不能拆出 `MaxMoveRequestsPerFrame=8` 的独立收益。

### 尸体回收

`LifecycleCleanup_80_20260816` 中，80 个敌人清理后 Enemy Actor 变为 0、Movement/Animation/Tick 工作显著下降，UObject 数减少 887；进程物理内存由 3616 MB 增至 3676.34 MB，没有立即下降。分配器保留内存是可能解释，但未通过分配跟踪确认。该记录另有 41 条 CharacterMovement 最大迭代次数告警。它证明生命周期清理停止持续工作，不证明立即归还物理内存或长期无泄漏，也不是当前版本的严格性能 A/B。

## 六、截至 09-05 的补证计划快照

以下保留当时的计划，不代表最新执行顺序。后续已完成的诊断、被拒绝的候选和剩余计划见[分阶段性能实验记录](Docs/Performance/EXPERIMENT_LOG.md)。

按证据价值排序：

1. **当前 AI A/B**：当前提交下运行 AllEnabled/NoAIThrottling 各 5 次，补 Decision、MoveRequest、GT P95/P99 和消费者快照。
2. **MoveTo 独立 A/B**：增加 SameGoalSkipped、RetrySkipped、BudgetRejected、Submitted、Failed/AlreadyAtGoal/Successful；比较预算开关，不再只看提交数。
3. **阴影预算扫描**：Top 5/8/12/不限，记录 ShadowDepths、Shadow Draw Calls、GPU、固定画质场景截图。机制已有效，本轮用于选择质量/成本折中，不是重新证明存在成本。
4. **RT 独立预算扫描**：先让 Full 候选超过 12，再测 RT 8/12/不限，要求 `RayTracingBudgetRejected>0`，记录 Skinned BLAS、动态几何、RT/RHI 和画质。
5. **Skeletal LOD 专项**：固定角色距离与姿态，记录实际 LOD、顶点/骨骼规模、Skinned Mesh/GPUSkinCache/BLAS成本；当前动态战斗场景不足以判断。
6. **动画 Tick 分级处理**：在解释 RT/RHI 快慢模式前不再调参；如果继续保留，必须做当前 Trace 配对并确认不会破坏攻击窗口和近距离动画。
7. **当前 Insights 配对**：至少为 AllEnabled、NoAnimationSharing、NoShadowTiering、NoRayTracingTiering 各采一份相同区间 Trace，使当前 CSV 的局部变化能在调用链中闭环。
8. **补齐运行元数据**：manifest 写入 Git commit、UE版本、Build Configuration、RHI、分辨率、Scalability、CPU/GPU/驱动和完整命令行；缺失列输出 `N/A`，不能默认为 0。

## 七、结论与证据边界

规模矩阵、Insights 与破坏性关闭共同定位了移动、动画完成/等待以及动态骨骼光追成本。保留玩法的策略分别控制 AI/移动更新频率、姿态复用、阴影与骨骼光追参与数量。

重复消融中，Movement 分级、Animation Sharing、阴影参与限制和骨骼 RT 参与限制均具备“消费者数量改变、目标局部成本三次同向变化”的证据。Skeletal LOD 和动画 Tick 分级没有建立独立净收益，具体权重与 Top-N 数值也未完成最优性验证。

27.899 ms 与 7.707 ms 是跨版本 GT 趋势，不能作为 Significance 的单变量收益，也不能据此判定整帧稳定达到 60 FPS。
