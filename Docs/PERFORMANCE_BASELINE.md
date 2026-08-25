# FPS 性能优化、纹理驻留与 VSM 专题

> 本文只记录 `fpstrue_safe2` 已实现或已验证的内容。性能结论来自固定矩阵、日志、CSV、MemReport 和截图；未落地的方案只放在“后续边界”中。

## 1. 面试结论

当前项目的性能主线不是“改几个控制台变量”，而是：

```text
固定测试条件
-> 分清 Game Thread、Render Thread、GPU、内存和渲染告警
-> 用 Profile 找主要成本
-> 一次只改一个变量
-> 对比平均值、P95/P99 和功能回归
-> 保留收益，撤销负优化
```

可以证明的结果：

- 完成 `10 / 20 / 40 / 80 / 160` 敌人固定规模矩阵。
- 当前机器上约 40 个活跃敌人可维持约 60 FPS；80 是压力档。最新 160 敌人 Trace 显示 RenderThread 是整帧关键路径，Game Thread 同时受到角色移动、物理同步和动画压力。
- 已在代码中实现 AI 决策降频、路径刷新阈值、移动 Tick 分级、动画可见性策略、阴影距离分级和尸体延迟回收。
- 六张高占用植物纹理的驻留内存降低约 60 MB。
- 已分离 Texture Streaming Pool 与 VSM Non-Nanite 队列问题，并完成多组单变量实验。
- VSM 告警目前是已定位、已验证候选方案但仍有残余的风险，不能表述为彻底修复。

## 2. 固定性能矩阵

测试条件：Development Editor 独立进程、`Demonstration`、1600x900、关闭 VSync、预热 10 秒、采样约 30 秒。

这张矩阵用于说明敌人数增长趋势，来自较早的固定版本。最新线程归因以 2026-08-24 的 Insights Trace 和定向消融为准，不能把两个版本的数据直接拼成优化前后收益。

| 敌人数 | FPS | Frame ms | P95 ms | P99 ms | Game Thread ms | Render Thread ms | GPU ms | CharacterMovement ms | Animation ms | Draw Calls |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 62.84 | 15.914 | 19.706 | 21.244 | 4.299 | 15.150 | 8.785 | 0.429 | 0.650 | 1659 |
| 20 | 61.15 | 16.352 | 19.909 | 21.950 | 5.879 | 15.642 | 9.189 | 0.745 | 0.977 | 1806 |
| 40 | 61.02 | 16.389 | 19.632 | 21.362 | 8.471 | 15.736 | 9.915 | 1.501 | 1.517 | 2088 |
| 80 | 55.20 | 18.115 | 21.076 | 22.547 | 16.106 | 17.355 | 11.591 | 3.231 | 2.728 | 2536 |
| 160 | 35.83 | 27.907 | 32.459 | 34.273 | 27.899 | 14.182 | 14.205 | 6.188 | 4.448 | 3458 |

### 2.1 如何读结果

- `10 -> 40`：帧率仍约 60，Game Thread、移动和动画成本随敌人数增长。
- `80`：Game Thread 与 Render Thread 都接近帧预算，属于压力档。
- `160`：旧矩阵中的 Game Thread 为 27.899 ms，说明 CPU 侧敌人更新已经超出帧预算；最新 Trace 进一步发现 RenderThread 关键路径也超过 GT，不能只写成单一 GT 瓶颈。
- 路径查询不是这组数据中的第一大项；移动、动画和敌人生命周期更值得优先处理。
- 不能只看平均 FPS。P95/P99 用于暴露生成、回收、寻路或资源流送造成的尖峰。

### 2.2 Profile-first 定位与定向验证

最新流程为：

```text
15 秒 Insights Trace
-> Timing 与 Task Graph 缩小范围
-> 排除低占用系统
-> 只验证 CharacterMovement 和 SkeletalMesh 两个候选
-> 每组重复两次并比较关键线程
```

Trace 的 449 个有效 CSV 帧中，Game Thread 均值约 27.61 ms、P95 约 30.07 ms。GT 主要分类为：`EndPhysics` 等待 9.94 ms、CharacterMovement 5.61 ms、其他任务同步等待 4.91 ms、TickActors 4.21 ms、Animation 3.69 ms、SyncBodies 0.96 ms 和 EndOfFrameUpdates 0.76 ms。这里的 Timing 父节点和 CSV 分类存在包含关系，不能直接相加。

稳定区间选择 `42.7878396-42.8234201 s` 的 P95 附近帧进行调用树检查：`FEngineLoop::Tick` 为 35.581 ms，`UWorld_Tick` 为 24.674 ms；其中一段 `TickCompletionEvents -> WaitUntilTasksComplete -> WaitForTasks` 阻塞 9.362 ms，帧末另有 8.923 ms 的 `GameThreadWaitForTask`。相同时间窗内 Foreground Worker 被动态光追 Mesh Batch、阴影设置和 RDG 异步任务占用，说明 EndPhysics 等待同时受到任务依赖和共享 Worker 竞争影响，不能把整段时间简单写成“Chaos 计算”或“AI 成本”。

Timing 截图另外保留了两处高耗时帧：`42.448 s` 附近 CSV 201 的 40.4 ms 帧，以及 `42.859 s` 附近 CSV 213 的 43.6 ms 帧。两者都展开到 `UWorld_Tick -> TickCompletionEvents -> WaitUntilTasksComplete -> WaitForTasks`，对应等待约 14.0 ms 和 14.7 ms，证明该热点不是单个异常尖峰。第二张图的 `48.6 ms` 是跨帧选区，不作为单帧数据引用。截图见 `Docs/PerformanceEvidence/20260824/Insights_GT_HighFrame_42.448s.png` 和 `Insights_GT_TailFrame_42.859s.png`。

可直接治理的第一层是 CharacterMovement、碰撞/物理同步和骨骼动画；任务等待需要沿 Task Graph 检查被等待任务。AI 决策约 0.08 ms、PathFollowing 约 0.11 ms、攻击 Sweep 约 0.007 ms，Timer、UI 和 Streaming 也都低于 0.2 ms，因此没有继续穷举关闭这些低占用模块。

| 组别 | Frame | GT | RT | GPU | EndPhysics | Movement | Animation |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 38.06 ms | 30.81 ms | 36.88 ms | 13.98 ms | 13.18 ms | 5.55 ms | 3.50 ms |
| CharacterMovementTickOff | 32.96 ms | 16.19 ms | 32.49 ms | 12.46 ms | 7.39 ms | 0.07 ms | 2.60 ms |
| SkeletalMeshTickOff | 27.49 ms | 21.38 ms | 26.63 ms | 11.17 ms | 10.06 ms | 5.12 ms | 0.24 ms |

Render/RHI 进一步显示动态骨骼光追几何是重要成本：RHIThread 的 Bottom-Level Acceleration Structure 构建约 9.86 ms，RenderThread 的动态光追实例收集约 2.35 ms，GPU 的 Skinned Geometry BLAS 约 1.43 ms。阴影深度约 2.78 ms，Lumen Lighting 与 Screen Probe Gather 分别约 1.56 ms 和 1.39 ms。复杂建筑会参与阴影和 Lumen，但当前不能把它写成第一根因。

关闭组件 Tick 是诊断上限，不是最终方案。可落地方向是限制完整频率移动与动画的并发数量、使用动画预算和骨骼 LOD，并单独评估敌人是否必须进入硬件 Ray Tracing 场景。完整证据见 `Docs/PerformanceEvidence/20260824/README.md`。

## 3. 代码中已经实现的优化

### 3.1 AI 决策与寻路

`AfpstrueEnemyAIController` 不启用 Actor Tick，而是使用一次性 Timer 驱动 `Idle / Chase / Attack / Dead` FSM：

- 不同状态和距离使用不同决策间隔。
- 每次决策结束后再安排下一次，便于停止和重设。
- `MoveTo` 只在没有路径目标、目标位移超过 `PathRefreshDistance` 或当前路径空闲时刷新。
- 死亡、UnPossess 和 EndPlay 都清理 Timer、停止移动并释放站位。

这减少了每帧 AI 判断和重复路径请求，但没有实现行为树、EQS 或 AI Perception。

### 3.2 Significance Manager 统一更新分级

旧方案同时存在敌人自身距离分级和 GameMode 固定名额预算。固定名额组虽然降低了移动、动画 Tick 数量，但 160 敌人实测没有形成端到端收益，并出现部分敌人等待或响应失真的问题，因此没有继续叠加第三套规则。

当前改为一个统一入口：

```text
GameMode 每 0.25 秒提交玩家视点
-> Significance Manager 并行计算敌人与视点的距离重要度
-> Sequential 回调进入 EnemyCharacter
-> 统一设置 AI 决策倍率、CharacterMovement Tick、骨骼动画 Tick 和远距离阴影
```

分级使用四个清晰的距离边界：5 m 内为 `Full`，5-10 m 为 `Reduced`，10-100 m 为 `Background`，超过 100 m 才退出追击。50 m 以后关闭动态阴影。UE 代码中的对应厘米值为 `500 / 1000 / 5000 / 10000`。默认参数为：

| 等级 | 移动 Tick | 动画 Tick | AI 决策间隔倍率 |
| --- | ---: | ---: | ---: |
| Full（0-5 m） | 每帧 | 每帧 | 1.0 |
| Reduced（5-10 m） | 0.05 s | 0.05 s | 1.5 |
| Background（10-100 m） | 0.10 s | 0.10 s | 2.0 |

攻击中、攻击窗口开启或目标已经进入攻击范围的敌人强制使用 `Full`，攻击决策间隔不参与降频。这样 10-100 m 的敌人只降低反应精度，不会关闭寻路、停止攻击或改变伤害结果。超过 50 m 可关闭敌人动态阴影；碰撞响应、攻击 Sweep、受伤和死亡逻辑不由显著性分级修改。

Significance Manager 内部可并行执行只读的重要度计算，但 UObject、组件 Tick 和阴影状态只能在 `Sequential` 回调中修改。敌人在 BeginPlay 注册、首次死亡和 EndPlay 注销，避免延迟回收的尸体继续参与评分，也避免管理器持有失效对象。

本次接入替换了固定名额预算，不与其叠加。代码已落地，但完整构建和同条件 A/B 尚未完成，因此当前只能表述为“统一了更新分级入口”，不能宣称已经获得性能收益。

### 3.3 其他策略的采用边界

- 分帧：波次出生已经按 0.05 秒间隔拆分，避免集中 Spawn 峰值。
- Timer：AI FSM 使用一次性 Timer，不开启 AIController Actor Tick；Timer 的价值来自降频，不是天然比 Tick 快。
- `ParallelFor`：不手写并行修改 Actor。显著性只读计算交给插件并行，组件状态仍回到游戏线程顺序更新。
- 碰撞：装饰物和关卡资产应按“不交互则关闭、只查询则 Query Only”的原则逐资产治理；敌人的近战查询、Pawn 阻挡和死亡布娃娃不能被统一关闭。
- 动画：现有实现包含动画 Tick 分级、不可见更新策略和攻击时恢复全速。Property Access、动画线程代理和 Animation Budget Allocator 尚未作为已实现内容。
- UI：血量、弹药和倒计时已经从 UMG 属性轮询改为 Delegate 事件更新；这条优化独立于敌人显著性分级。
- Draw Call：使用 `stat RHI` 观察 DrawPrimitive/Draw Calls，但复杂骨骼敌人不能直接使用 ISM/HISM 合批代替。

### 3.4 Significance A/B 验收方案

使用同版本、同地图、同机位、同随机种子和相同敌人数各运行至少两次：

```text
候选组：默认启用 Significance Manager
基线组：-BenchmarkDisableEnemySignificance
```

记录 FPS、Frame/GT/RT/GPU 的平均值与 P95/P99，Movement、Animation、AI Decision 数量和耗时，以及移动/骨骼 Tick 数。性能之外必须回归：远处敌人仍寻路，靠近后能及时追击和攻击，攻击窗口不漏判，死亡后能注销并按 30 秒回收。只有候选组尾帧改善且行为无回退，才保留该方案。

### 3.5 出生和死亡生命周期

`AfpstrueGameMode` 将单波生成拆成 0.05 秒间隔的队列，避免同一帧集中 Spawn。出生点使用 NavMesh 可达采样，并限制单个敌人的尝试次数。

敌人死亡后：

```text
停止 AI、移动和攻击事务
-> Capsule NoCollision
-> Mesh 进入 Ragdoll / QueryAndPhysics
-> 延迟施加死亡冲量
-> 30 秒 LifeSpan 回收
```

蓝图不应再执行第二次 `DestroyActor`，否则会破坏尸体反馈和 C++ 的统一生命周期。

80 敌人生命周期验证：

| 阶段 | 敌人 Actor | FPS | Game Thread ms | Movement ms | Animation ms | UObject |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 存活 | 80 | 44.96 | 18.110 | 3.705 | 2.915 | 50,763 |
| 回收后约 35 秒 | 0 | 68.51 | 5.444 | 0.106 | 0.266 | 49,876 |

这能证明 Actor 和主要更新成本被回收；不能据此声称进程物理内存立即下降，因为 UE 分配器可能保留内存页。

## 4. 纹理驻留治理

### 4.1 先分清问题

Texture Streaming Pool 管理普通纹理 Mip 的驻留预算。典型排查工具：

```text
stat streaming
ListStreamingTextures
MemReport -full
```

它与 Nanite Streaming Pool、VSM 物理页和 VSM Non-Nanite 标记队列不是同一套资源。看到 `Texture Streaming Pool Over Budget` 时，不能通过扩大 VSM 队列解决；看到 VSM 队列告警时，也不能只调整 `r.Streaming.PoolSize`。

### 4.2 本项目实际修改

依据 `ListStreamingTextures` 和 MemReport，调整六张植物纹理的最大驻留分辨率：

| 纹理组 | 调整前 | 调整后 |
| --- | --- | --- |
| PineBark A/N | 2048x4096 | 1024x2048 |
| IvyAtlas A/N | 4096x4096 | 2048x2048 |
| PineBranchAtlas A/N | 4096x2048 | 2048x1024 |

同地图、100 敌人、Pool 1000 MB、预热 8 秒、采样 30 秒：

| 指标 | 调整前 | 调整后 | 变化 |
| --- | ---: | ---: | ---: |
| Streaming Assets | 212.27 MB | 152.27 MB | -60.00 MB / -28.3% |
| Texture Memory Used | 288.586 MB | 228.906 MB | -59.680 MB / -20.7% |
| MemReport Resident Texture | 425.228 MB | 365.549 MB | -59.679 MB / -14.0% |

准确说法是“降低了纹理驻留和预算压力”。两次测试都没有复现真正的 Pool Over Budget，而且尚缺同机位画质 A/B 截图，因此不能写成“已经修复纹理池溢出且画质无损”。

## 5. VSM Non-Nanite 队列告警

### 5.1 它是什么

Virtual Shadow Maps 需要为投射阴影的非 Nanite 几何建立标记工作。大范围覆盖阴影页、数量很多或频繁变化的非 Nanite Primitive 会增加这条工作队列压力。

它不是：

- Texture Streaming Pool 超预算；
- Nanite Streaming Pool 超预算；
- 普通 Draw Call 数量的同义词；
- 单纯把 `MaxCulledInstanceAllocationSize` 调大就能解决的问题。

本项目在低敌人数基线中也出现过告警，因此 FactoryDistrict 的大体积非 Nanite 场景资源是主要怀疑对象；敌人和尸体会进一步放大压力。首轮审计关注过 `Building_TypeD_A`、`Pipes_Stack_A/B/C` 等大范围资产。

### 5.2 已做诊断

```text
r.Nanite 0
stat Nanite
NaniteStats VirtualShadowMaps
Unreal Insights 的 GPU / RHI / VSM 轨道
r.Shadow.Virtual.NonNanite.NumPageAreaDiagSlots 16
r.Shadow.Virtual.NonNanite.LargeInstancePageAreaThreshold 1
```

`r.Nanite 0` 只用于 A/B 判断问题是否与 Nanite/VSM 路径有关，不能作为最终修复。

### 5.3 单变量实验

| 实验 | FPS | Frame ms | Render Thread ms | 告警 | 结论 |
| --- | ---: | ---: | ---: | ---: | --- |
| 默认 `IncludeInCoarsePages=1` | 55.19 | 18.121 | 17.378 | 1 | 基线 |
| `IncludeInCoarsePages=0` | 45.79 | 21.838 | 20.690 | 0 | 告警消失但性能回退，不采用 |

另外测试了 Dynamic Coarse Page Pixel Threshold `32 / 64` 和 Shadow Radius Threshold `0.02 / 0.03`，告警仍出现一次，不能证明解决问题。

最终保留引擎默认，只在配置中保留：

```ini
r.Shadow.Virtual.Enable=1
```

没有为了隐藏告警而全局关闭 Coarse Pages，也没有盲目扩大队列。固定矩阵中该告警在 80 敌人档出现一次，其他四档为 0；后续 80 档复测仍能各复现一次，所以状态应写为“残余风险已记录”。

### 5.4 正确治理顺序

1. 用诊断输出定位覆盖页数最大的 Primitive。
2. 对适合的 opaque 静态网格逐资产评估 Nanite，回归材质、碰撞和 fallback。
3. 对不重要或远距离对象分级关闭动态阴影，避免全局一刀切。
4. 对必须保留的非 Nanite 对象检查 Bounds、LOD、实例数量和移动性。
5. 每次只改一个变量，保留相同机位截图、GPU/RT 数据和告警计数。

## 6. 高频追问

| 问题 | 回答核心 |
| --- | --- |
| 160 敌人的第一瓶颈在哪里 | 最新 Trace 中 RT Critical Path 约 32.27 ms，高于 GT 27.39 ms 和 GPU 13.67 ms；GT 内部第一来源是 CharacterMovement 与 EndPhysics 等待，RT/RHI 主要受动态骨骼光追几何、阴影和 Lumen 影响 |
| 为什么不用平均 FPS 作为唯一指标 | 平均值会隐藏生成、回收、寻路和流送尖峰，需要 P95/P99 |
| Timer 一定比 Tick 快吗 | 不是；收益来自降低不必要的调用频率，Timer 过密同样有成本 |
| LOD 解决什么 | 降低远处网格、骨骼和动画成本；不能替代 AI、碰撞和生命周期治理 |
| 为什么不能直接扩大纹理池 | 可能掩盖资产预算问题并挤占其他显存，先找 Wanted Mips 和高驻留资产 |
| VSM 告警为什么不是纹理池 | 两者资源、统计和治理路径不同；一个是纹理 Mip 驻留，一个是虚拟阴影标记工作 |
| 为什么撤销 Coarse Pages 修改 | 它消除了告警但 FPS 和 Render Thread 明显回退，属于负优化 |
| 尸体回收是否证明没有内存泄漏 | 证明 Actor/UObject 与更新成本下降；仍需长时间多波次和 Insights/LLM 证据 |

## 7. 后续边界

只保留与当前瓶颈直接相关的候选项：

- 当 Spawn/Destroy 成为实测尖峰时，再评估对象池；当前未实现。
- 更高敌人数需要动画预算分配、骨骼 LOD/骨骼裁剪、碰撞和 AI 分层更新，不能用 ISM/HISM 替代仍需蒙太奇和骨骼命中的活体敌人。
- VSM 继续采用逐资产治理，不把全局 CVar 调参包装成架构优化。
- 发布包仍要在目标画质档和固定硬件上重新建立 CPU、GPU、显存和内存预算。

## 8. 证据位置

```text
Saved/Profiling/FPS_FinalLOD_20260816
Saved/Profiling/VSM_CoarsePages_AB_20260816
Saved/Profiling/VSM_DynamicThreshold_20260816
Saved/Profiling/VSM_RadiusThreshold_20260816
Saved/Profiling/LifecycleCleanup_80_20260816
Saved/Profiling/InsightsFirstDiagnosis_20260824
Saved/Profiling/ProfileGuidedAblation_20260824
Docs/PerformanceEvidence/20260816
Docs/PerformanceEvidence/20260824
Saved/Logs/FinalPerformanceClosure_Build.log
```

面试时按“现象 -> 指标 -> 假设 -> 单变量实验 -> 结果 -> 是否保留”讲述，不把告警消失等同于性能优化成功。
