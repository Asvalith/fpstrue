# 2026-08-24 Profile-first 性能诊断证据

## 1. 流程

本轮不从预设开关开始。先在同一版本、同一地图、160 个敌人、固定随机种子 `1337` 下录制 15 秒 Unreal Insights Trace，去除采集首尾各约 1 秒后，通过 Timing、Task Graph、RHI 和 GPU 事件缩小范围。随后只保留三个实验组，每组重复两次并随机执行顺序：

```text
Baseline
CharacterMovementTickOff
SkeletalMeshTickOff
```

关闭组件 Tick 只用于估算成本上限，不是最终玩法方案。

## 2. Trace 定位

### Game Thread

`Baseline.csv` 中筛除无效帧后保留 449 帧，Game Thread 均值为 27.61 ms，中位数为 27.16 ms，P95 为 30.07 ms。下表列出 GT 中需要关注的全部主要分类。CSV 分类和 Timing 父节点存在包含关系，不能把每一行直接相加。

| 分类 | 平均值 | P95 | 判断 |
| --- | ---: | ---: | --- |
| EndPhysics 等待 | 9.94 ms | 12.06 ms | 最大同步等待，必须沿 Task Graph 找被等待任务和 Worker 占用 |
| CharacterMovement | 5.61 ms | 6.46 ms | 最大可直接治理的玩法执行成本 |
| 其他 EventWait | 4.91 ms | 8.22 ms | 主要出现在帧末同步；属于等待，不等同于 GT 自身算法执行 |
| TickActors | 4.21 ms | 4.74 ms | Actor/Component Tick 汇总项，与 Movement、Animation 有包含关系 |
| Animation | 3.69 ms | 4.19 ms | 骨骼更新、并行动画完成和 AnimBP 更新 |
| SyncBodies | 0.96 ms | 1.15 ms | 物理结果回写与组件同步 |
| EndOfFrameUpdates | 0.76 ms | 0.93 ms | Primitive/组件帧末更新 |
| PostPhysics 等待 | 0.22 ms | 0.47 ms | 次要同步等待 |
| QueueTicks | 0.17 ms | 0.22 ms | Tick 入队成本，当前不是主要瓶颈 |
| UI | 0.17 ms | 0.24 ms | 当前不是主要瓶颈 |
| Physics 自身 | 0.16 ms | 0.23 ms | GT 直接物理执行较低，主要问题是等待和 Movement 触发的任务 |
| TimerManager | 0.09 ms | 0.25 ms | Timer 调度不是主要瓶颈 |
| RenderAssetStreaming | 0.08 ms | 0.13 ms | GT 流送更新不是当前主要瓶颈 |
| AI 决策 | 0.08 ms | - | 已降频，不是本 Trace 的主要成本 |
| PathFollowing | 0.11 ms | - | 当前不是主要瓶颈 |
| 攻击 Sweep | 0.007 ms | - | 当前不是主要瓶颈 |

#### P95 代表帧

Timing 稳定区间中选择 `FEngineLoop::Tick` 的 P95 附近帧，而不是最大异常帧：

```text
42.7878396 s -> 42.8234201 s
FEngineLoop::Tick = 35.581 ms
```

该帧主要调用链为：

```text
FEngineLoop::Tick 35.581 ms
├─ UWorld_Tick 24.674 ms
│  ├─ TickCompletionEvents 10.231 ms
│  │  └─ ExecuteTask：集中执行 CharacterMovement 等组件 Tick
│  ├─ TickCompletionEvents 13.267 ms
│  │  └─ WaitUntilTasksComplete
│  │     ├─ WaitForTasks 9.362 ms
│  │     └─ ExecuteTask 3.887 ms
│  └─ 其他 Tick Group
├─ FViewport_Draw 1.008 ms
└─ GameThreadWaitForTask 8.923 ms
```

`WaitForTasks` 位于 `EndPhysics` Tick Group。相同 9.362 ms 时间窗内，两条 Foreground Worker 主要在执行 `RayTracingMeshBatchTask`、`FRDGBuilder::ProcessAsyncSetupQueue`、`SetupShadowDepthView` 和阴影收集任务，而可见的 `PhysicsParallelForWithContext` 约为 0.155 ms。这说明 GT 的 EndPhysics 阻塞不仅是“物理计算很慢”，还受到共享 Worker 队列占用和任务完成时序影响；不能把整段等待都归因于 AI、碰撞或 Chaos 本体。

因此本轮 GT 治理优先级为：

1. `CharacterMovement` 及其碰撞、物理同步链。
2. 骨骼动画更新与并行动画完成阶段。
3. `EndPhysics` 的任务依赖和 Foreground Worker 竞争。
4. `SyncBodies` 与帧末 Primitive 更新。

AI 决策、寻路、攻击 Sweep、Timer、UI 和 Streaming 已由 Profile 直接排除为第一优先级，不再用逐项关闭的方式盲查。

#### Timing 截图证据

完整帧统计负责确定 P95，Timing 截图负责展示调用链。截图中的高耗时帧高于 P95，但两处都复现了相同的等待结构，因此用于验证热点是否稳定存在，而不是代替分位数统计。

| 证据 | 单帧信息 | 可见调用链 | 用途 |
| --- | --- | --- | --- |
| `Insights_GT_HighFrame_42.448s.png` | CSV 201，Frame/GT 约 40.4 ms | `UWorld_Tick 28.1 ms -> TickCompletionEvents 18.1 ms -> WaitForTasks 14 ms` | 代表性高耗时帧，展示完整 GT 与 RHI 上下文 |
| `Insights_GT_TailFrame_42.859s.png` | 中间帧 CSV 213，Frame 43.6 ms | `UWorld_Tick 29.8 ms -> TickCompletionEvents 18.6 ms -> WaitForTasks 14.7 ms`，帧末另有约 9 ms 等待 | 尾部帧，确认峰值时仍是同一类任务等待 |

第二张图中的蓝色 `48.6 ms` 是框选的时间区间，覆盖了帧边界，不能写成单帧耗时。文档引用的是区间内 CSV 213 的 `43.6 ms` 帧。两张图同时显示 RHI 侧动态骨骼光追几何和 BLAS 构建，但当前治理范围只收口 Game Thread；RHI 结果作为已知后续方向记录，不在本轮继续扩展。

### Render Thread 与 Worker

| 事件 | 每帧时间 | 含义 |
| --- | ---: | --- |
| RenderThread Critical Path | 32.27 ms | 当前整帧主要 CPU 渲染路径 |
| RenderThread WaitForTasks | 13.28 ms | 等待可见性、场景更新和渲染 Worker |
| UpdateAllPrimitiveSceneInfos | 11.17 ms inclusive | 更新动态 Primitive 并发起相关任务 |
| ShadowDepths | 2.81 ms exclusive | 阴影深度提交 |
| Gather Dynamic RayTracing Instances | 2.35 ms exclusive | 收集动态光追实例 |
| Worker ShadowDepths | 2.78 ms exclusive | 并行阴影任务 |
| Worker RayTracingMeshBatchTask | 2.72 ms exclusive | 动态光追 Mesh Batch |

### RHI Thread

| 事件 | 每帧时间 |
| --- | ---: |
| BuildAccelerationStructure BottomLevel | 9.86 ms exclusive |
| SubmissionQueue Process | 3.56 ms exclusive |
| ShadowDepths | 2.77 ms exclusive |
| SkinnedGeometryBuildBLAS | 1.49 ms exclusive |

项目启用了 Ray Tracing，160 个持续变形的骨骼敌人会更新动态光追几何并构建 BLAS。这是本轮 RHI 侧最明确的热点。

### GPU

GPU 总时间约 13.67 ms，低于 RenderThread Critical Path，因此当前不是整帧第一瓶颈。主要 Pass 为：

| GPU Pass | 每帧时间 |
| --- | ---: |
| Deferred Lighting | 3.30 ms inclusive |
| ShadowDepths | 2.78 ms |
| Lumen Scene Lighting | 1.56 ms |
| Skinned Geometry Build BLAS | 1.43 ms |
| Lumen Screen Probe Gather | 1.39 ms |
| Post Processing | 1.05 ms inclusive |
| Base Pass | 0.72 ms |
| Nanite VisBuffer | 0.36 ms |

复杂建筑会参与可见性、阴影、Lumen 和 Base Pass，但这份 Trace 不支持把“复杂建筑”写成第一根因。若要归因到具体场景资产，还需要 Primitive Stats、GPU Visualizer 或 RenderDoc 的逐资产证据。

## 3. 定向消融结果

| 组别 | Frame | GT | RT | GPU | EndPhysics | Movement | Animation |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 38.06 ms | 30.81 ms | 36.88 ms | 13.98 ms | 13.18 ms | 5.55 ms | 3.50 ms |
| CharacterMovementTickOff | 32.96 ms | 16.19 ms | 32.49 ms | 12.46 ms | 7.39 ms | 0.07 ms | 2.60 ms |
| SkeletalMeshTickOff | 27.49 ms | 21.38 ms | 26.63 ms | 11.17 ms | 10.06 ms | 5.12 ms | 0.24 ms |

结论：

1. `CharacterMovement` 及其物理同步是 Game Thread 第一成本来源。关闭后 GT 减少约 14.62 ms，`EndPhysics` 等待减少约 5.78 ms。
2. 骨骼更新是第二来源，并同时影响 GT、RenderThread 和动态光追几何。关闭后 GT 减少约 9.43 ms，RT 减少约 10.25 ms。
3. 当前 160 敌人场景的整帧关键路径还受 RenderThread 限制，GT 同时明显超出 16.67 ms 预算；本轮专项只收口 GT，GPU 仍有余量。
4. 两个关闭组改变了玩法和画面，只能用于确认因果和收益上限，不能直接作为上线配置。

## 4. 后续治理边界

- 移动：限制同时以完整频率运行 `CharacterMovement` 的敌人数，远距离降低频率但仍保留追击；结合碰撞简化和更新预算验证功能误差。
- 动画：使用 URO、可见性更新策略、骨骼 LOD 和远距离骨骼裁剪；进入攻击范围时恢复完整姿态，保证 Socket Sweep 准确。
- Ray Tracing：先 A/B 判断该 Demo 是否必须让全部敌人进入硬件光追场景，再评估远距离敌人的 Ray Tracing 可见性和 Lumen 模式，不能直接全局关闭后宣称完成优化。
- 阴影：继续采用敌人距离分级和逐资产审计；VSM Non-Nanite 告警属于单独专题。

## 5. 原始证据

```text
Saved/Profiling/InsightsFirstDiagnosis_20260824/Baseline.utrace
Saved/Profiling/InsightsFirstDiagnosis_20260824/InsightsExport
Saved/Profiling/ProfileGuidedAblation_20260824
```

本目录中的 `Scene_Baseline_160.png` 是 Trace 录制时的游戏场景图，不是 Unreal Insights Timing 截图。三张消融场景图只证明测试条件和功能状态；线程归因以 `.utrace`、导出表和后续保存的 P95 Timing 截图为准。

最终面试证据应优先展示两张 `Insights_GT_*.png`，再用 `GT_Profile_Summary.csv` 说明分位数和成本排序；不要用场景截图证明线程瓶颈。
