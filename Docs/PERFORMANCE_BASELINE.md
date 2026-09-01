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

### 3.2 Significance 特征分层、骨骼 LOD 与渲染消费者

重要性先按特征语义分层，再由各层消费者使用，不能因为某个数据容易取得就跨层复用：

| 层 | 凝练特征 | 当前/未来子特征 | 消费者 | 明确禁止 |
| --- | --- | --- | --- | --- |
| 逻辑 | 玩法关联、交互状态、目标距离 | 威胁、有效目标、角色权重；攻击、受击、卷入事件；敌人到玩法目标的距离 | AI 决策、移动更新、攻击名额、寻路请求 | 相机视锥、屏占比、遮挡不能决定敌人是否攻击 |
| 渲染 | 可见性、距离 | 主/扩张视锥、遮挡、屏占比、最近可见；观察距离 | 骨骼 LOD、动画更新、阴影、骨骼硬件光追、Animation Sharing | 不反向修改感知、寻路和伤害判定 |

敌人仍只向 Significance Manager 注册一次。同一个 UObject 不能以两个规则重复注册，因此单次注册保留为 Gameplay 入口；Coordinator 在同一轮 `Manager->Update` 完成后，对已注册敌人追加一遍只读 Render 评分。两层共享生命周期，但不共享特征、权重或档位：

```text
一次 Enemy 注册
├─ GameplayScore：玩法关联 + 交互状态 + 到玩法目标的距离
│  └─ AI 决策倍率、CharacterMovement Tick
└─ RenderScore：可见性（视锥/屏占比/最近可见）+ 观察距离
   └─ 动画 Tick、Skeletal Mesh 最低 LOD、阴影、骨骼 RT、Animation Sharing
```

Gameplay 层的距离是“敌人到有效玩法目标的二维距离”，不是相机观察距离；攻击、攻击范围和最近交互是硬条件。默认 15 m 内为 `Full`，15-50 m 为 `Reduced`，50 m 外为 `Background`；对应移动 Tick 为每帧、1/30 s、0.05 s，AI 决策间隔倍率为 `1.0 / 1.5 / 2.0`。后续接入威胁、目标选择和角色权重时，与该目标距离共同组成 GameplayScore，但不把视锥加入逻辑公式。镜头背后的近战敌人不会因为不可见而失去追击与攻击响应。

Render 层的距离是“敌人 Mesh Bounds 到观察视点的距离”。逻辑距离和渲染距离可以同时存在，但参考对象、归一化区间、权重和消费者完全独立，不能复制一层的最终 Score 给另一层。

Render 层默认分数为：

```text
Score = 0.45 * Frustum
      + 0.30 * ScreenCoverage
      + 0.15 * RecentFrustum
      + 0.10 * Distance
```

权重会在运行时按总和归一化，不要求人工相加等于 1。视锥使用 Mesh Bounds 与 15% 扩张边界，避免刚出画时立即降级；最近入镜保留 0.5 秒；距离因子在 15-100 m 之间线性衰减；投影半径达到屏幕归一化半径 0.10 时获得完整屏占比分。`Full` 进入/退出阈值为 `0.70 / 0.60`，`Reduced` 进入/退出阈值为 `0.30 / 0.20`，另有 0.5 秒降级延迟和 0.5 秒最短持档时间。升级立即生效，降级需要同时满足滞回和延时，减少 LOD、阴影和动画频率在阈值附近反复切换。

默认 Render 策略为：

| 等级 | 动画 Tick | 最低骨骼 LOD | 普通 Full 名额 |
| --- | ---: | ---: | ---: |
| Full | 每帧 | 0 | 全局最多 12 个 |
| Reduced | 1/30 s | 1 | 不限 |
| Background | 0.05 s | 2 | 不限 |

这张表描述当前实现值，不代表三个数值已经调到最佳。2026-08-31 的 80 敌人独立消融显示：骨骼 LOD 档位确实从关闭组的 `80/0/0` 变为默认组约 `19/29/32`，但 Animation 和动态 BLAS 都没有测到独立收益；动画频率分级的局部 Animation 差异只有约 `0.03 ms`，关闭后 Frame/RT/RHI 反而更低，因此两项暂不写成性能成果。Gameplay Movement 分级则有稳定证据：关闭后 Movement Tick 从约 `65.1` 增至 `81.0`，CharacterMovement 从 `1.056 ms` 增至 `1.245 ms`，说明机制有效，但 `15/50 m` 和 `30/20 Hz` 仍只是待标定初值。

实际 LOD 会按该 Skeletal Mesh 的 LOD 数量 Clamp；当前敌人骨骼资源有 4 个 LOD，因此默认 `0 / 1 / 2` 都可用。攻击中、攻击窗口开启、目标进入攻击范围或 0.75 秒内发生战斗事件时，会通过独立的 Gameplay Animation Protection 立即恢复每帧动画和 LOD0；它不改写 RenderScore、Render 档位、阴影名额或骨骼 RT 名额。攻击 Socket Sweep、受伤、死亡和碰撞结果不因 Render 档位改变。

阴影从固定距离开关改为全局优先队列：只有扩张视锥内、50 m 内且不是 Background 的高分敌人竞争投影名额。当前源码默认 5 个名额；2026-08-29 确定的下一版目标是 `Top 8 Shadow`，尚未实现和 A/B，不能把目标值写成当前收益。玩法保护保证动画和 LOD，不保证镜头外仍投影；这样把近战正确性和阴影画质预算分开。`-BenchmarkEnemyShadowsOff` 仍可完全关闭敌人阴影，`-BenchmarkDisableShadowTiering` 则关闭预算并恢复全部敌人投影。

80 敌人三次消融已证明“限制敌人投影参与”有效：默认 5 个投影敌人，关闭预算后为 80 个；ShadowDepths 从 `1.052 ms` 增至 `1.773 ms`，Shadow Draw Calls 从约 `637` 增至 `1013`。这只能证明预算机制有用，不能证明 5 是最佳名额。

骨骼硬件光追已经接入 Render Significance。默认只有 Render `Full`、100 m 内并位于优先队列前 12 名的敌人保留 `Visible in Ray Tracing`；候选排序只使用主视锥、扩张视锥和 Render 分数。玩法交互保护只恢复动画与骨骼 LOD，不参与阴影或 RT 排名。关闭 RT 只移除硬件光追场景/动态 BLAS 参与，不改变普通光栅可见性。`-BenchmarkDisableEnemyRayTracingTiering` 可关闭分级并恢复全部敌人骨骼 RT，`-BenchmarkEnemyRayTracingOff` 仍用于完全关闭敌人 RT 的对象级消融。

80 敌人三次消融中，默认 RT Visible 为 12，关闭分级后为 80；Skinned Geometry BLAS 从 `0.199 ms` 增至 `0.495 ms`，说明限制骨骼 RT 参与有效。但默认组 `RayTracingBudgetRejected=0`：RT 候选先被 Full Render 限制到最多 12 个，随后再进入同样为 12 的 RT 上限，因此独立 `MaxRayTracingEnemies=12` 尚未产生二次筛选，不能写成已验证参数。

Animation Sharing 插件已经启用，并由 `EnemyAnimationSharingCoordinator` 在运行时创建同骨架共享池。共享状态处理器不再维护第二套枚举，而是直接读取敌人 AI FSM：`Idle` 对应共享待机，`Chase` 且实际存在移动速度时对应共享跑步；已经到达包围槽位的静止 `Chase` 映射回待机。两个状态各有 4 个随机相位 Leader。只有 Render `Reduced / Background`、不在攻击范围、未攻击/受击且不在战斗保护期的敌人可以成为 Follower。攻击、受击、死亡和布娃娃前立即注销并恢复原 AnimBP/Montage/Notify 路径。插件的 Leader Tick 使用 RenderScore，默认阈值 0.20；GameplayScore 不传入插件。`-BenchmarkDisableEnemyAnimationSharing` 可做独立消融。Animation Sharing 优化动画评估和骨骼更新，不会自动合并 Skeletal Mesh Draw Call。

80 敌人三次消融中，默认平均约 61 个 Follower，关闭后为 0；Animation 从 `0.818 ms` 增至 `1.226 ms`，三次方向一致。因此共享机制可以写成已验证的局部 CPU 优化，但不能把受 RT/RHI 运行漂移影响的整帧 FPS 差值归给它。

#### 统一时钟、同代快照与四类 Top-N 预算（2026-08-29 目标方案，待实现/验证）

本轮确定的目标不是让所有玩法数据等待 Significance，而是把 Significance 重构为统一时钟下的派生策略与资源预算系统。血量、位置、目标、攻击、受击、死亡、攻击令牌授予/释放和 AI FSM 转换仍由事件立即更新；Coordinator 只读取这些权威事实，生成更新频率、LOD、动画、阴影、骨骼 RT 和共享策略。紧急升级不等待周期，常规校准和降级才进入统一调度。

四类全局预算的目标口径为：

| 预算 | 目标数量 | 所属层 | 排序输入与所有权 | 当前实现边界 |
| --- | ---: | --- | --- | --- |
| 进攻候选 | Top 8 | 逻辑 | Gameplay 距离、交互状态、可达性与等待时长；令牌和槽位仍由 `SurroundManager` 拥有 | 本轮运行与验收口径固定为 8 |
| Full Render | Top 12 | 渲染 | 视锥、屏占比、最近可见、观察距离；由 Significance Coordinator 排序 | 当前预算已经是 12 |
| Shadow | Top 8 | 渲染 | 独立 Shadow 候选与稳定 Tie-break，不由攻击状态直接抢占 | 当前默认 5，待改为 8 |
| Skeletal RT | Top 12 | 渲染 | 当前先要求进入 Full，再按 RT 条件控制动态骨骼 BLAS 参与 | 当前上限是 12，但未产生独立预算拒绝 |

`Full Render` 与 `Shadow` 当前可以独立筛选；`Skeletal RT` 在现有代码里是 Full 候选的子集，还不是完全独立预算。进入 Full Render 不保证获得阴影，但在 Full 与 RT 上限都为 12 时，RT 上限本身不会继续淘汰对象。玩法攻击权不能使用相机视锥或 RenderScore。攻击、近战和受击只触发 Gameplay Animation Protection，立即恢复动画更新与 LOD0 以保证 Montage/Notify/Sweep 正确性，但不改写 Render 排名。

Coordinator 提供唯一的 `SampleTime / FrameNumber / PolicyGeneration`。调度基准取所有已启用周期的最小值，其他通道使用绝对 `NextDueTime` 或 Dirty 标记按需运行；更理想的实现是每轮直接安排到“下一项最早截止时间”，避免 0.10 秒基准量化 0.25 秒任务时产生 0.20/0.30 秒抖动。建议初始节奏为 Render Sample 0.10 秒、Gameplay Maintenance 0.25 秒、全局预算重排 0.25 秒、统计输出 1.00 秒；生成/传送到近处、进入攻击范围、受击、获得攻击令牌和目标变化走事件式 Urgent Promotion。

同一 `PolicyGeneration` 内必须使用同一代不可变 Snapshot：先在同一时间戳采集全部候选，再计算逻辑/渲染期望状态并稳定排序，最后统一 Diff Commit。采集和排序阶段不修改组件，提交阶段仅在 Desired 与 Applied 不同时调用 TickInterval、`OverrideMinLOD`、`SetCastShadow`、`SetVisibleInRayTracing` 和 Animation Sharing 注册接口。这样既消除遍历顺序偏差，也避免把 Coordinator 提高到更快周期后反复触发 Render State 更新。Benchmark 启动日志必须输出最终生效的 `8 / 12 / 8 / 12`，不能只读取 C++ 声明默认值，防止蓝图或运行配置覆盖造成实验口径漂移。

攻击侧使用“环形槽位 + 8 个进攻令牌 + 等待时间优先队列 + 攻击后轮换”。未获令牌者停留在等待环，获权者才进入攻击接近点；攻击结束者回到队尾，释放时立即唤醒下一名候选，不等待旧 AI Timer。`Top 8` 表示最多 8 个已投入进攻者，不等于 8 个敌人同一帧进入伤害窗口；攻击开始需要错峰，并单独验证同时伤害和动画/Sweep 峰值。

#### 架构与代码体积复查（2026-08-29）

- `RenderSignificanceSample` 只保留视锥、屏占比、最近可见和观察距离；战斗动画保护作为独立正确性信号传递，不再污染 RenderScore、阴影和 RT 排名。
- Animation Sharing 复用敌人 AI FSM，删除重复的 `Idle / Moving` 状态枚举、无外部消费者的转发接口和失效 Handle 扫描；Follower 退出时显式恢复 `bIgnoreLeaderPoseComponentLOD`，把骨骼 LOD 控制权还给项目分级。
- 玩家 `EFPCharacterState` 只是由死亡和速度即时推导的显示枚举，没有状态转换、副作用或源码/资产消费者，已经删除；武器 Action FSM 和敌人 AI FSM 继续保留。
- 玩家和敌人继续复用同一个 `UfpstrueHealthComponent` 类型，各 Actor 只保留不同的死亡表现。玩家在绑定组件委托后主动读取一次初始血量，避免错过组件 BeginPlay 阶段的初始广播。
- 敌人攻击范围、伤害、冷却、动画容错和武器 Sweep 参数已经从 `EnemyCharacter` 收归 `EnemyCombatComponent`；AI 和 Notify 继续通过 Character 的稳定入口调用，不再由组件通过 friend 回读分散配置。
- 阴影分级只由全局 Render Policy 和 Significance Coordinator 管理，删除逐敌人重复的 `bEnableShadowDistanceTiering`；`BenchmarkDisableShadowTiering` 仍通过全局 Policy 完成同一消融。
- 删除无资产/源码消费者的玩家派生状态、三个 GameMode 轮询 Getter、`IsSprinting`、无来源伤害入口和数个共享层转发接口。Blueprint 事件、Notify 回调和 UE 生命周期函数不按静态调用次数误删。
- 核心源码统一使用项目根目录 `.clang-format`：140 列、Tab 缩进、Allman 大括号、保留 include 和中文注释顺序。格式化和死代码清理曾把 `.h/.cpp` 物理行数从 7,791 行降至 6,991 行；随后为全部模块和函数补充“职责 + 调用者/消费者”中文注释，当前为约 7,203 行。物理行数变化不能当作运行时性能收益。

本次骨骼 RT 与 Animation Sharing 增量已通过 UE 5.5 UHT、全部 C++ 编译和 Development Game 链接。Editor 目标也完成 C++ 编译，但链接时发现已有 `UnrealEditor.exe` 占用项目 DLL，因此没有把该次 Editor 链接写成通过。直接运行未 Cook 的 Game 可执行文件会在项目逻辑前因缺失 Global ShaderCodeLibrary 停止；需要关闭现有编辑器后用 `UnrealEditor.exe -game` 做一次真实渲染功能验证，再开始 A/B，当前不能宣称已有性能收益。

代码已通过 UE 5.5 UHT、Development Game 和 Editor 目标编译、链接。独立 NullRHI 运行曾在进入项目逻辑前遇到引擎自带 `WorldGridMaterial` 文件序列化错误，因此没有把 NullRHI 当作功能证据；随后已在真实渲染路径完成 20 敌人固定场景测试。当前结论是“实现、构建和档位生效均已验证，但初版端到端净收益尚未建立”，不能写成已经完成性能优化。

### 3.3 其他策略的采用边界

- 分帧：波次出生已经按 0.05 秒间隔拆分，避免集中 Spawn 峰值。
- Timer：AI FSM 使用一次性 Timer，不开启 AIController Actor Tick；Timer 的价值来自降频，不是天然比 Tick 快。
- `ParallelFor`：不手写并行修改 Actor。显著性只读计算交给插件并行，组件状态仍回到游戏线程顺序更新。
- 碰撞：装饰物和关卡资产应按“不交互则关闭、只查询则 Query Only”的原则逐资产治理；敌人的近战查询、Pawn 阻挡和死亡布娃娃不能被统一关闭。
- 动画：现有实现包含动画 Tick 分级、不可见更新策略、攻击时恢复全速，以及非战斗 Reduced/Background 敌人的 Animation Sharing。Property Access、动画线程代理和 Animation Budget Allocator 尚未作为已实现内容。
- UI：血量、弹药和倒计时已经从 UMG 属性轮询改为 Delegate 事件更新；这条优化独立于敌人显著性分级。
- Draw Call：使用 `stat RHI` 观察 DrawPrimitive/Draw Calls，但复杂骨骼敌人不能直接使用 ISM/HISM 合批代替。

#### UI 事件驱动代表帧证据

优化前的 Unreal Insights 代表帧中，`Slate::Tick (Time and Widgets)` / `Slate::DrawWindows` 约为 4.1 ms；当时 Game/GT 为 15.4-18.2 ms。血量、弹药和倒计时改为 Delegate 事件更新后，高密度近战代表帧记录为：54.65 FPS、Frame 18.23 ms、Game/GT 7.46 ms、Draw 17.94 ms、RHI 10.28 ms、GPU 16.72 ms、Draws 1948、Prims 1456.8K。

这组截图说明：在更密集的近战画面中，HUD 不再通过每帧属性绑定持续查询状态，Game/GT 仍保持在 7.46 ms；与优化前代表帧相比，单帧 GT 数值下降约 51.6%-59.0%。由于两张图不是同机位、同敌人数、同持续时间的严格 A/B，该百分比只作为趋势证据，不能替代 CSV 区间平均、P95/P99 和重复实验。当前总帧仍由 Draw/GPU 侧限制，UI 优化收益不能被表述为等比例 FPS 提升。

证据：

- `Docs/PerformanceEvidence/20260825/SmallEnemyUI/SmallEnemy_UI_Baseline.png`
- `Docs/PerformanceEvidence/20260825/SmallEnemyUI/SmallEnemy_UI_EventDriven_CloseCombat_Dense_20260826.png`

#### `stat unit` 的 Input 指标

`stat unit` 中的 `Input` 不是 Enhanced Input 在 Game Thread 上的执行耗时，而是输入采样到对应画面 VBlank/Present 的端到端延迟。UE 5.5 在 `OnSamplingInput` 时记录 `GInputTime`，RHI 在画面提交阶段计算 `GInputLatencyTime = VBlankTime - InputTime`，界面再使用 `0.9 / 0.1` 权重对结果进行平滑。

本轮截图中的 `Input 62-68 ms` 对应约 `18-20 ms` 的帧时间，约为 3-4 帧流水线延迟。它会同时受到 Game Thread、Render Thread、RHI/GPU 队列、帧同步和编辑器 PIE 的影响，不能据此判断输入绑定或 Enhanced Input 本身消耗了几十毫秒。焦点切换、暂停、PIE 初始化或采样时间戳失效时出现的数千万毫秒属于无效样本，必须丢弃。

真实输入 CPU 开销应在 Insights 中查看 `PlayerController` / `EnhancedPlayerInput` 的调用轨道。输入延迟的正式 A/B 则使用独立进程或打包版本、固定机位、关闭 VSync，在相同场景下重复采集稳定区间的平均值和 P95；`r.OneFrameThreadLag 0` 只作为单变量候选组验证，若吞吐或稳定性回退则不保留。当前项目仍以 `Game / Draw / RHIT / GPU` 作为瓶颈判断指标，`Input` 仅作为交互延迟的辅助指标。

本轮 `Profile(20260826_011906).csv` 共 654 个采样帧，进一步区分了输入延迟和输入函数成本：

| 指标 | 平均 | P95 | P99 | 最大 |
| --- | ---: | ---: | ---: | ---: |
| FrameTime | 14.7694 ms | 18.5609 ms | 22.2901 ms | 27.2394 ms |
| GameThreadTime | 4.6934 ms | 5.9484 ms | 6.7818 ms | 100.5643 ms |
| RenderThreadTime | 14.2730 ms | 18.2361 ms | 23.5080 ms | 36.5572 ms |
| GPUTime | 8.3748 ms | 9.3926 ms | 9.6612 ms | 11.1748 ms |
| `Exclusive/GameThread/Input` | 0.0124 ms | 0.0153 ms | 0.0258 ms | 0.0840 ms |
| `Exclusive/GameThread/UI` | 0.2100 ms | 0.2769 ms | 0.4309 ms | 1.1004 ms |
| `Slate/DrawWindows_Private` | 0.0254 ms | 0.0302 ms | 0.0818 ms | 0.1164 ms |

因此，截图中的 `Input 62-68 ms` 不能解释为输入逻辑占用了 62-68 ms。真正的输入 CPU 工作平均只有 0.0124 ms；高 Input 主要是当前帧经过 GT、RT、RHI/GPU 队列后才显示出来。该区间的平均瓶颈在 Render Thread，且约 1954 个平均 Draw Call 使渲染提交和可见性任务继续占据主要预算。

Render Thread 的 CSV 热点为：`RenderOther` 平均 4.16 ms、`EventWait/Visibility` 平均 3.44 ms、`RenderLighting` 平均 1.55 ms；场景统计约有 3450 个动态实例和 2296 个静态实例。这里的 `EventWait/Visibility` 表示 Render Thread 等待并行可见性/动态网格收集任务完成，不等于等待输入，也不能通过删除输入绑定解决。下一步使用固定场景 A/B 临时关闭光追效果，验证动态光追几何是否显著增加渲染提交成本；测试不修改项目默认配置。

### 3.4 Draw / RenderThread / RHIThread 专项定位与治理

#### 3.4.1 为什么转向 Draw 与 RHI

UI 改为事件更新以后，最新 CSV 中 Game Thread 平均为 4.69 ms，而 Render Thread 平均为 14.27 ms，已经非常接近 Frame 平均 14.77 ms；GPU 平均为 8.37 ms。说明这段样本的整帧主要跟随 Render Thread，而不是输入、HUD 或普通玩法代码。

这里必须先统一术语：

- `stat unit` 的 `Draw` 是 CPU Render Thread 的一帧耗时，不是 Draw Call 数量。
- `RHIT` 是 RHI Thread 翻译和提交渲染命令以及等待依赖的时间，不等于 GPU 执行时间。
- 本文中的 `RT` 只表示 Render Thread；硬件 Ray Tracing 始终写成“硬件光追”，避免混淆。
- `FDrawSceneCommand` 是渲染场景命令的上层范围，不是某一个可以直接替换的小函数。必须继续展开调用树才能找到成本来源。

当前状态可分为三层：

| 层级 | 结论 | 状态 |
| --- | --- | --- |
| 瓶颈线程 | 最新 CSV 的 Render Thread 平均 14.27 ms，高于 GT 和 GPU，并贴近 Frame | 已确认 |
| 主要候选 | 可见性和动态网格收集、Mesh Draw Command 提交、动态骨骼光追几何更新 | 已由代表帧定位 |
| 最终治理收益 | 静态实例化、材质 Section 精简和骨骼光追 A/B 的同条件 P95/P99 收益 | 尚待验证 |

#### 3.4.2 从代表帧展开调用链

在稳定战斗区间选择接近 P95、但不是加载、暂停、切换焦点或生成尖峰的帧。代表帧中的 Render Thread 调用链为：

```text
RenderThread Frame / BeginFrame
-> FDrawSceneCommand                  约 14.9-18.8 ms
-> FDeferredShadingSceneRenderer_Render
-> InitViews / VisibilityCommands     约 6.7-8.4 ms
-> ProcessVisibilityTasks             约 6.5-8.2 ms
-> WaitForGatherDynamicMeshElements   约 6.5-8.1 ms
-> WaitUntilTasksComplete / WaitForTasks
```

这条链说明 Render Thread 正在等待并行可见性和动态网格收集任务完成。`WaitForGatherDynamicMeshElements` 不是“线程没事做”，也不是输入延迟；它表示当前帧后续渲染依赖这些任务的结果。父子范围互相包含，不能把 18.8、8.4 和 8.2 ms 再相加。

同一批证据中的 RHIThread 调用链为：

```text
RHI_Translate / FRDGBuilder::Execute
-> Scene
-> RayTracingDynamicGeometry
-> RayTracingDynamicGeometryUpdate    约 5.0-5.1 ms
-> BuildAccelerationStructure_BottomLevel
-> Build                              约 4.0-4.3 ms
```

这说明动态骨骼网格进入硬件光追场景后，需要更新并构建 Bottom-Level Acceleration Structure。它是 RHI 侧明确候选，但目前只能表述为“已定位的重要成本”，不能仅凭一帧断言它就是全部 Draw 瓶颈。

证据：

- `Docs/PerformanceEvidence/20260826/DrawRHI/DrawRHI_RepresentativeFrame.png`
- `Docs/PerformanceEvidence/20260826/DrawRHI/Visibility_CallTree.png`

**三帧优化基准（2026-08-26）**

| 代表帧 | 选中帧 | `FDeferredShadingSceneRenderer_Render` | `VisibilityCommands` | `WaitForGatherDynamicMeshElements` | `RayTracingDynamicGeometryUpdate` | BLAS `Build` |
|---|---:|---:|---:|---:|---:|---:|
| A | 19.5 ms | 15.0 ms | 7.0 ms | 6.8 ms | 4.5 ms | 4.0 ms |
| B | 19.7 ms | 14.7 ms | 5.3 ms | 5.1 ms | 4.8 ms | 4.2-4.3 ms |
| C | 25.0 ms | 19.8 ms | 8.0 ms | 7.8 ms | 5.2 ms | 4.5 ms |

三帧中重复出现了两条稳定的高占用链路：RenderThread 的 `InitViews -> VisibilityCommands -> ProcessVisibilityTasks -> WaitForGatherDynamicMeshElements`，以及 RHIThread 的 `RayTracingDynamicGeometryUpdate -> BuildAccelerationStructure_BottomLevel`。这说明当前基准不是单个偶发尖峰，后续 A/B 应优先验证动态网格收集、可见性处理和动态光追几何更新。

读取这些数据时必须遵守两个边界：表中节点是嵌套的包含时间，父子节点不能相加；`WaitForTasks` 表示当前线程在等待依赖任务完成，它是定位调用链的等待点，不等于根因本身。三帧只用于建立定位基准，尚不能单独证明成本全部来自敌人，也不能作为优化收益结论。

新增证据：

- `Docs/PerformanceEvidence/20260826/DrawRHI/Baseline_A_Frame19_5ms.png`
- `Docs/PerformanceEvidence/20260826/DrawRHI/Baseline_B_Frame19_7ms.png`
- `Docs/PerformanceEvidence/20260826/DrawRHI/Baseline_C_Frame25_0ms.png`

#### 3.4.3 工程化执行流程

第一步是冻结测试条件，而不是先改参数：

1. 使用同一 Development 独立进程、同一地图、1600x900、关闭 VSync 和相同画质档。
2. 固定玩家位置、视角、敌人数、敌人随机种子和战斗阶段；先用 20 敌人观察基础提交成本，再用 80 敌人检查随角色数量增长的斜率。
3. 进入场景后预热 5-10 秒，避开资源加载和首帧 Shader/PSO 创建。
4. 每组录制 5-10 秒稳定区间并重复三次，保留平均值、P95、P99，不用单张 `stat unit` 截图替代区间数据。
5. 每次只改变一个候选因素；功能、机位或画质变化都意味着该组不能直接与基线比较。

建议同时记录：

```text
stat unit
stat RHI
stat SceneRendering
stat InitViews
r.MeshDrawCommands.LogDynamicInstancingStats 1
```

Unreal Insights 中重点读取 `RenderThread`、`RHIThread`、`GPU`、`Frames` 和 `Task Graph`。`r.MeshDrawCommands.LogDynamicInstancingStats 1` 只输出下一帧动态实例化统计，用来判断相同渲染状态的 Mesh Draw Command 是否成功合并，不能单独证明端到端收益。

第二步才是根据证据建立三个假设：

```text
假设 A：大量可见 Primitive 和动态网格收集使 VisibilityCommands 成本过高
假设 B：重复静态物体、过多材质 Section 或状态差异增加 Draw Command 数量
假设 C：骨骼敌人进入硬件光追场景，持续产生动态几何和 BLAS 更新
```

第三步按风险从低到高做单变量实验。

#### 3.4.4 静态场景合批与可见性治理

优先处理重复且不移动的场景物体，例如托盘、路灯、围栏、管道小件和相同装饰件：

1. 在场景中按 Static Mesh、材质、Mobility、阴影和碰撞配置筛选完全相同的重复对象。
2. 确认对象不保存独立 Gameplay 状态，不需要单独交互、独立移动或独立销毁。
3. 同类重复物改为 ISM/HISM、Foliage 或 PCG 实例化；按空间区域拆组，避免一个跨越全地图的大组件让整组一直可见。
4. 对远距离厂房模块使用 HLOD 或小范围 Merge Actor。按建筑或遮挡单元合并，不能把整张地图合成一个巨大网格。
5. 设置合理的 Cull Distance / Max Draw Distance，并检查 Bounds。小物体远距离仍参与可见性和阴影没有收益。
6. 不移动的环境组件保持 `Static`。错误的 `Movable` 会进入动态 Primitive 路径，增加更新、阴影和动态网格收集成本。

采用 HISM 的原因不是“所有对象都能变成一个 Draw Call”，而是减少 Actor/Component 数量，并让相同 Mesh、材质和渲染状态的实例共享提交。不同材质、不同渲染状态、不同 LOD 或不同 Vertex Factory 仍可能拆成多个 Draw Command。

Actor Merge 也不等于自动消除所有 Draw Call。合并后的网格如果仍有多个材质 Section，每个可见 Section 仍可产生独立提交；合并范围过大还会损害遮挡剔除、LOD、光照和碰撞粒度。因此每次合并后必须同时比较 Draw Calls、`VisibilityCommands`、Render Thread P95 和视觉/碰撞结果。

#### 3.4.5 材质 Section 与 Mesh Draw Command

当前检查过的 `Building_TypeC_C` 资产包含 11 个材质槽。材质槽不一定全部在每个 LOD 中产生可见 Section，但它是 Draw Command 数量偏高的明确审计对象。

治理顺序为：

1. 在 Static Mesh Editor 检查各 LOD 的 Section 数，而不只看材质槽名称。
2. 删除未被任何 Section 使用的槽；把确实可以共享材质的 Section 合并。
3. 只有在纹理分辨率、UV、材质参数和画质允许时才使用材质图集，不能为了少一个 Draw Call 盲目扩大纹理和显存。
4. 尽量复用同一 Material Instance Parent 和兼容的渲染状态；透明、双面、不同 Blend Mode 和不同 Static Switch 会破坏合批条件。
5. 使用 `r.MeshDrawCommands.LogDynamicInstancingStats 1` 和 `stat RHI` 检查引擎是否已经把兼容命令动态实例化，避免重复手工优化已经被合并的对象。

UE 5.5 本地源码中 `r.MeshDrawCommands.DynamicInstancing` 默认开启，它只会合并 Vertex Factory 支持且渲染状态兼容的可见 Mesh Draw Command。最终目标不是追求“场景只有几个 Draw Call”，而是在不破坏剔除和资源预算的前提下减少无意义的独立提交。

#### 3.4.6 骨骼敌人与硬件光追 A/B

活体敌人需要 Skeletal Mesh、Montage、骨骼命中和布娃娃，不能用 ISM/HISM 直接替换。Animation Sharing 或动画预算可以减少动画评估成本，但不会自动把多个 Skeletal Mesh 合成一个 Draw Call，因此必须把“动画优化”和“渲染提交优化”分开描述。

针对 RHI 证据，只做临时单变量验证：

```text
基线：r.RayTracing.Geometry.SkeletalMeshes 1
候选：r.RayTracing.Geometry.SkeletalMeshes 0
```

该 CVar 在 UE 5.5 的 `SkeletalMesh.cpp` 中用于控制骨骼网格是否进入硬件光追效果。它适合验证动态骨骼 BLAS 成本，但不代表最终必须关闭。`r.RayTracing.Geometry.SupportSkeletalMeshes` 是只读启动配置，不能拿来做运行时切换。

测试时保持敌人数、机位、动画、灯光和画质一致，比较：

- Render Thread 与 RHIThread 的平均、P95、P99；
- `RayTracingDynamicGeometryUpdate` 和 Bottom-Level Build；
- GPUTime、阴影、反射和画面差异；
- 敌人是否仍满足项目要求的反射、阴影和命中表现。

如果关闭骨骼光追后 RHI/RT 没有稳定改善，就回到 Visibility 和 Section 数量继续排查；如果有明显改善，则再选择“敌人不参与硬件光追”“使用 Ray Tracing LOD Bias”或“保留高画质档、在性能档关闭”的产品方案，而不是直接修改全局默认值。

#### 3.4.7 验收标准与回退规则

每个候选方案必须满足：

| 类别 | 验收内容 |
| --- | --- |
| 性能 | Render Thread P95/P99 稳定下降，RHIThread 或 Draw Calls 与假设方向一致 |
| 稳定性 | 三次重复结果超出测量噪声，不依赖某一个最好看的瞬时帧 |
| 画面 | 材质、阴影、反射、LOD 和遮挡切换无明显退化 |
| 玩法 | 碰撞、拾取、射击命中、敌人动画和死亡布娃娃正常 |
| 资源 | 合批没有用更大的纹理、过大的 Bounds 或额外常驻资源换取表面收益 |

如果告警消失但 RT/GPU 回退，或者 Draw Calls 减少却因 Bounds 过大导致更多物体常驻可见，都属于负优化，应撤销。RHIThread 本身通常是上游场景提交和动态几何的结果，不通过随机修改线程或队列 CVar 治理。

这轮 Draw/RHI 专项的正确表述是：已经利用 CSV 和 Insights 把瓶颈缩小到 Render Thread 的可见性/动态网格收集，以及 RHI 的动态骨骼光追几何更新；静态实例化、材质 Section 和骨骼光追仍需完成同条件 A/B 后，才能写入“已获得收益”。

### 3.5 Significance A/B 验收方案

使用同版本、同地图、同机位、同随机种子和相同敌人数各运行三次，并采用 AB/BA 交错顺序：

```text
候选组：默认启用 Significance Manager
基线组：-BenchmarkDisableEnemySignificance
```

总开关只能证明整套系统的净收益；定位每个子策略时使用独立开关：

```text
骨骼 LOD Off：-BenchmarkDisableEnemySkeletalLOD
Render 动画分级 Off：-BenchmarkDisableEnemyAnimationTiering
阴影预算 Off：-BenchmarkDisableShadowTiering
骨骼 RT 分级 Off：-BenchmarkDisableEnemyRayTracingTiering
Animation Sharing Off：-BenchmarkDisableEnemyAnimationSharing
Render 三档 Off：-BenchmarkDisableEnemyRenderTiering
```

CSV 的 `fpstrueSignificance` 分类记录 `GameplayFull/Reduced/Background`、`RenderFull/Reduced/Background`、`LOD0/1/2Plus`、`ShadowCasters`、`RayTracingVisible`、`AnimationSharingFollowers`、各类预算拒绝数，以及 `MeanFrustum/Screen/Recent/DistanceFactor`。统一时钟版本还必须记录 `SampleTime / PolicyGeneration`、Top 8 进攻候选、Top 12 Full Render、Top 8 Shadow、Top 12 RT 的占用/换手、策略实际执行间隔、晋升/降级次数、Diff Apply 次数、攻击队列长度与等待 P50/P95/P99。只有先证明同一代快照和对应消费者确实发生变化，才能比较 FPS、Frame/GT/RT/GPU 的平均值与 P95/P99、Movement、Animation、AI Decision、ShadowDepths、动态 BLAS 和 Draws。

权重和阈值可通过命令行单变量覆盖，无需修改资产：`EnemySigFrustumWeight`、`EnemySigScreenWeight`、`EnemySigRecentWeight`、`EnemySigDistanceWeight`、`EnemySigFullEnter/Exit`、`EnemySigReducedEnter/Exit`、`EnemySigFullBudget`、`EnemySigShadowBudget` 等。每轮只改变一个权重或阈值，并同时记录四个因子均值、三档人数、视觉错误和端到端 P95/P99；不能只按平均 FPS 反向拟合权重。

性能之外必须回归：镜头外近战敌人仍寻路和攻击，快速转身时 LOD/动画不突变，攻击窗口不漏判，武器 Socket Sweep 正常，阴影切换无明显闪烁，死亡后能注销并按 30 秒回收。只有候选组尾帧改善且行为无回退，才保留该方案。

统一时钟先做功能验收，再做性能 A/B：事件触发的攻击、受击、令牌接棒必须立即生效；周期策略在同一 `PolicyGeneration` 内使用同代 Snapshot；状态不变时组件写入计数应接近 0。随后以相同权重和四类固定预算比较初版单一 0.25 秒方案与统一调度方案，并单独比较 0.25 / 0.10 秒，不允许在周期实验中同时调整预算或权重。

### 3.6 出生和死亡生命周期

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
| `stat unit` 的 Draw 是 Draw Call 数吗 | 不是。Draw 是 CPU Render Thread 帧时间；Draw Call 数应结合 `stat RHI`、SceneRendering 和 Mesh Draw Command 统计查看 |
| `FDrawSceneCommand` 高说明什么 | 它说明场景渲染提交阶段处于关键路径，仍要展开到 InitViews、Visibility、动态网格收集、材质 Section、阴影或光追几何，不能把上层范围当作根因 |
| `WaitForGatherDynamicMeshElements` 为什么高 | Render Thread 在等并行可见性和动态网格收集任务完成。它是任务依赖结果，需检查参与任务、可见 Primitive 和动态组件数量，不是输入或空等 |
| 为什么不把敌人直接改成 HISM | 活体敌人需要独立骨骼姿态、Montage、命中、移动和布娃娃；HISM 适合相同静态网格实例，不能保持这些独立 Gameplay 行为 |
| Animation Sharing 能否降低 Draw Call | 主要降低动画评估和骨骼更新，不能自动合并不同 Skeletal Mesh 的渲染提交；动画和 Draw 必须分别验证 |
| 为什么检查材质 Section | 一个 Actor 或一个合并网格仍可因多个可见 Section 提交多次；减少 Actor 数量不等于减少相同数量的 Draw Command |
| 为什么测试骨骼硬件光追 | Insights 已看到 `RayTracingDynamicGeometryUpdate` 和 Bottom-Level Build 约 4-5 ms；关闭骨骼光追的单变量实验可以验证成本上限，再决定画质分级，而不是直接永久关闭 |
| 为什么不能直接合并整个场景 | 大 Bounds 会损害视锥和遮挡剔除，整组更容易常驻可见，同时破坏 LOD、碰撞和光照粒度；应按建筑或遮挡单元合并 |
| 为什么不用平均 FPS 作为唯一指标 | 平均值会隐藏生成、回收、寻路和流送尖峰，需要 P95/P99 |
| Timer 一定比 Tick 快吗 | 不是；收益来自降低不必要的调用频率，Timer 过密同样有成本 |
| LOD 解决什么 | 降低远处网格、骨骼和动画成本；不能替代 AI、碰撞和生命周期治理 |
| 为什么不能直接扩大纹理池 | 可能掩盖资产预算问题并挤占其他显存，先找 Wanted Mips 和高驻留资产 |
| VSM 告警为什么不是纹理池 | 两者资源、统计和治理路径不同；一个是纹理 Mip 驻留，一个是虚拟阴影标记工作 |
| 为什么撤销 Coarse Pages 修改 | 它消除了告警但 FPS 和 Render Thread 明显回退，属于负优化 |
| 尸体回收是否证明没有内存泄漏 | 证明 Actor/UObject 与更新成本下降；仍需长时间多波次和 Insights/LLM 证据 |

## 7. 后续边界

只保留与当前瓶颈直接相关的候选项：

- 2026-08-30 决定暂缓环境 Draw Call 合批，不再把 ISM/HISM、全场景合并或 HLOD 资产改造作为当前主线。Draw Calls 仍保留为诊断指标，但下一阶段优先验证现有 Significance、骨骼 LOD、动画分级、Animation Sharing、阴影和骨骼 RT 预算在多敌人下的净收益与稳定性。
- 当 Spawn/Destroy 成为实测尖峰时，再评估对象池；当前未实现。
- 骨骼最低 LOD 已接入 Render Significance，但收益尚待独立 A/B；更高敌人数仍可继续评估骨骼裁剪、Animation Budget Allocator 或 Animation Sharing，不能用 ISM/HISM 替代仍需蒙太奇和骨骼命中的活体敌人。
- VSM 继续采用逐资产治理，不把全局 CVar 调参包装成架构优化。
- 发布包仍要在目标画质档和固定硬件上重新建立 CPU、GPU、显存和内存预算。

## 8. 实验台账与执行计划

### 8.1 已完成工作时间线

下表按“完成日期”记录已经落地或已经采集的工作。`已验证` 表示数据和功能都足以支撑当前结论；`方向性证据` 表示开关生效或成本方向明确，但重复性还不足以做产品决策；`未建立收益` 表示实现已经生效，但端到端性能没有超过噪声。

| 完成日期 | 阶段 | 完成内容 | 主要测试指标 | 当前结论与状态 | 证据 |
| --- | --- | --- | --- | --- | --- |
| 2026-08-16 | 规模与生命周期基线 | 完成 `10/20/40/80/160` 敌人矩阵、VSM Coarse Pages A/B、80 敌人死亡回收 | FPS、Frame P95/P99、GT、RT、GPU、Movement、Animation、Draw Calls、Actor/UObject、VSM 告警 | 规模曲线、负优化回退和生命周期回收均已验证；矩阵属于旧版本，只用于趋势 | `Saved/Profiling/FPS_FinalLOD_20260816`、`Docs/PerformanceEvidence/20260816` |
| 2026-08-20 | 纹理驻留 | 审计高驻留植物纹理并限制六张纹理的最大 Mip | Streaming Assets、Texture Memory、MemReport Resident Texture、Pool 告警 | 纹理驻留减少约 60 MB；未声称画质无损，已验证 | `Saved/Profiling/TextureRegionAudit_20260820` |
| 2026-08-22 | 移动分级 | 完成 CharacterMovement 距离分级 A/B | Frame/GT/RT/GPU、Movement、路径刷新、移动 Tick 数、敌人行为 | 分级机制落地；不同版本数据不与当前 20 敌人基线直接拼接 | `Saved/Profiling/MovementTierAB_20260822` |
| 2026-08-24 | Profile-first 定位 | 160 敌人固定种子 Insights Trace，并完成 Movement Tick Off、Skeletal Mesh Tick Off 定向消融 | Frame/GT/RT/GPU、EndPhysics、Movement、Animation、TickActors、SyncBodies、RT/RHI/GPU 光追和阴影事件 | RT 是整帧关键路径；Movement 和骨骼更新是 GT 可治理大项；关闭组只代表收益上限，已验证 | `Docs/PerformanceEvidence/20260824` |
| 2026-08-25 至 08-26 | UI、Input 与 Draw/RHI 调用链 | UI 改为事件驱动；区分输入 CPU 成本和 Input 延迟；展开 Visibility 与动态骨骼 BLAS 调用链 | Input CPU、UI、Frame/GT/RT/GPU、Draw Calls、RenderOther、Visibility Wait、`WaitForGatherDynamicMeshElements`、动态光追更新、BLAS Build | Input CPU 不是瓶颈；可见性/动态网格收集和动态骨骼光追是明确候选，已验证到调用链 | `Docs/PerformanceEvidence/20260825/SmallEnemyUI`、`Docs/PerformanceEvidence/20260826/DrawRHI` |
| 2026-08-27 | 全局骨骼光追消融 | 20 敌人下执行 Baseline 与 `r.RayTracing.Geometry.SkeletalMeshes 0`，每组两次 | Frame/GT/RT/GPU、Visibility、RenderOther、RayTracing Dynamic Geometry/Scene、RHI Draw Calls | Frame 均值 `24.953 -> 24.155 ms`，约节省 `0.798 ms`；GPU 约节省 `0.154 ms`，但运行间波动较大，只作为方向性证据 | `Saved/Profiling/SkeletalRayTracingAB_20260827` |
| 2026-08-27 至 08-28 | 敌人对象级光追/阴影消融 | 单独关闭敌人 `Visible in Ray Tracing` 与敌人投影，并验证快照状态 | Frame/GT/RT/GPU、RT Dynamic Geometry/Scene、ShadowDepths/Projection、Shadow Draw Calls、RHI Draw Calls/Primitives | 对象开关已证明生效；短采样出现明显运行漂移，现有 FPS 差值不作为最终收益，需严格复测 | `Saved/Profiling/EnemyRenderOffGroups_20260828` |
| 2026-08-28 | 分层 Significance 初版 | 完成 Gameplay/Render 双层评分、骨骼最低 LOD、动画 Tick 分级、阴影名额和 CSV 统计，并通过 Game/Editor 构建 | 三档人数、LOD 人数、Shadow Casters、四项因子、预算拒绝数、UpdateTime、Frame/GT/RT/GPU、Movement、Animation、Draw/Shadow | 实现和生效已验证；20 敌人为 `12/4/4`、LOD `12/4/4`、阴影 3。Frame `24.953 -> 25.427 ms`，P95/P99略改善，均值近似中性，净收益未建立 | `Saved/Profiling/SignificanceLayeredLOD_20260828/Matched20_analysis.md` |
| 2026-08-28 | 骨骼 RT 与 Animation Sharing 接入 | RT 名额接入纯 RenderScore；Reduced/Background 非战斗敌人接入 Idle/Moving 共享；补充独立消融和状态计数 | `RayTracingVisible`、`RayTracingBudgetRejected`、`AnimationSharingFollowers`、`GameplayAnimationProtection`、动态 BLAS、Animation、Frame/GT/RT/RHIT/GPU | UHT、C++ 编译、Development Game 链接和真实渲染运行已通过；160 敌人快照有 140 个 Sharing Follower，独立性能结果见 2026-08-31 消融行 | `Source/fpstrue/fpstrueEnemySignificanceCoordinator.cpp`、`Source/fpstrue/fpstrueEnemyAnimationSharingCoordinator.cpp`、`Saved/Profiling/CurrentScaleMatrix_Warm_20260830` |
| 2026-08-29 | 统一时钟与 Top-N 策略设计 | 确定逻辑/渲染分层、同代 Snapshot、两阶段 Diff Commit，以及 Top 8 进攻、Top 12 Full Render、Top 8 Shadow、Top 12 RT；攻击等待环和即时接棒纳入同一方案 | `PolicyGeneration`、四类预算占用/换手、实际更新间隔、Diff Apply、攻击等待 P95/P99、Frame/GT/RT/RHIT/GPU | 进攻名额已改为 8，20/80/160 快照中的实际攻击数为 3/7/8；统一时钟和 Top 8 Shadow 尚未完成，当前运行 Shadow 预算仍为 5 | 本节、复习手册 4.8.10-4.8.13 与 `Saved/Profiling/CurrentScaleMatrix_Warm_20260830` |
| 2026-08-29 | 当前构建 0/20 敌人快速复核 | 重编译 Editor Target；先跑 `0 -> 20`，发现首进程冷启动污染，再补跑 `20 -> 0` 反序热缓存组 | Frame/GT/RT/RHIT/GPU、Draw Calls、Primitives、Visibility、Shadow、动态 RT、Movement、Animation、策略快照和告警 | 反序热缓存组方向合理，可作快速归因；只有一组有效配对，不能替代三次 AB/BA 正式验收 | `Saved/Profiling/EnemyContributionAB_20260829_OnePair`、`Saved/Profiling/EnemyContributionAB_20260829_ReversePair` |
| 2026-08-30 | 当前版本多敌人规模复核 | 冷启动诊断后，以热缓存、固定种子和相同机位执行 `20/80/160` 敌人矩阵 | Frame P95/P99、GT、RT、RHIT、GPU、Movement、Animation、Draw Calls、三档/LOD/阴影/RT/Sharing 人数 | 消费者封顶和 CPU 次线性增长已验证；20/80/160 分别为 49.12/71.31/32.36 FPS，但 RT/RHIT 非单调漂移明显，单次矩阵不能作为精确规模曲线或 Significance 净收益 | `Saved/Profiling/CurrentScaleMatrix_Warm_20260830` |
| 2026-08-31 | 80 敌人消费者消融 | 默认与 Movement、骨骼 LOD、动画分级、Animation Sharing、阴影、骨骼 RT 单变量关闭组各三次；动画分级另做四次紧配对 | 目标消费者计数、Movement、Animation、ShadowDepths/Draw、Skinned BLAS、Frame/GT/RT/RHI/GPU P95/P99 | Movement、Sharing、限制阴影参与、限制骨骼 RT 参与有稳定局部证据；骨骼 LOD 无独立收益；动画分级存在负收益风险；具体阈值与 Top-N 未证明最优 | `Saved/Profiling/UnverifiedConsumers80_20260831/analysis.md` |

### 8.2 当前比较基线

后续实验统一使用当前 Development 独立进程、`Demonstration`、1600x900、`-NoVSync`、固定机位、随机种子 `1337`、硬件光追和骨骼光追开启。短期归因测试采用预热 10 秒、采样 30 秒、每组三次，并随机化组别执行顺序；需要与 2026-08-27 的历史组直接对照时，另外保留同口径的预热 5 秒、采样 5 秒复现组，不能混合计算。

当前初版 Significance 的 20 敌人匹配结果为：

| 指标 | 2026-08-27 Baseline | 2026-08-28 初版 | 差值 |
| --- | ---: | ---: | ---: |
| FPS | 40.08 | 39.33 | -0.75 |
| Frame Mean | 24.953 ms | 25.427 ms | +0.474 ms |
| Frame P95 | 32.439 ms | 31.878 ms | -0.561 ms |
| Frame P99 | 34.676 ms | 34.468 ms | -0.208 ms |
| GT Mean | 4.699 ms | 4.763 ms | +0.065 ms |
| RT Mean | 24.702 ms | 25.201 ms | +0.499 ms |
| GPU Mean | 8.335 ms | 8.381 ms | +0.046 ms |
| RHI Draw Calls | 1899.4 | 1894.3 | -5.1 / -0.27% |
| RHI Primitives | 1.224 M | 1.260 M | +2.96% |

这组数据说明当前 39 FPS 主要受 RT/RHI 提交链限制，而不是 GT、GPU 或 Significance 评分本身。2026-08-28 初版 Significance 更新平均约 `0.0435 ms/次`，当时采用单一 0.25 秒周期；该数值早于统一时钟、Top 8 Shadow 和攻击队列目标方案，只能作为历史成本参考。在没有拆清各消费者收益前，不降低 Full 名额、不调整权重，也不以牺牲近战和可见敌人质量换取平均 FPS。

### 8.3 下一轮执行顺序

计划日期是执行窗口，不用来提前声明结果。每完成一项，就把状态、实际命令行、三次结果和证据目录回填到本节。

| 计划日期 | ID | 实验目的 | 单变量组 | 核心指标 | 完成/通过条件 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-08-28 | P0 | 冻结当前口径 | 记录构建、地图、分辨率、画质、种子、预热、采样、命令行 | 元数据、敌人 Alive、VSync、目标帧率、告警 | 所有后续组使用同一二进制和配置；不改权重与预算 | 已完成 |
| 2026-08-28 | E1 | 分离环境固定成本与 20 个敌人的增量 | `0 Enemy`、`20 Enemy`，各三次 | Frame/GT/RT/RHIT/GPU 平均与 P95/P99；Draw Calls、Primitives、Visibility Wait、RenderOther、Shadow、RT Dynamic Geometry/Scene；Movement、Animation | 三个有效配对完成；原 Run3 整对因环境进程级异常排除并补跑。输出 `20-0` 增量，没有修改画质 | 已完成 |
| 策略实现后 | E2A | 验证统一时钟、同代快照和四类 Top-N 预算 | 当前新策略 Default、`-BenchmarkDisableEnemySignificance`；固定 Top 8/12/8/12、固定 20 敌人，各三次 | E1 全部指标；`PolicyGeneration`、实际周期、四类预算占用/换手、Diff Apply、攻击等待与接棒延迟 | 事件即时生效；同轮数据同代；无变化不写组件；总策略净收益超过噪声且无玩法/画质回退 | 待实现/待执行 |
| E2A 通过后 | E2B | 拆分 Render Significance 各消费者的净收益 | Default、Skeletal LOD Off、Animation Tiering Off、Shadow Tiering Off、Skeletal RT Tiering Off、Animation Sharing Off，固定 20 敌人，各三次 | E1 全部指标；三档/LOD/阴影/RT/Sharing 人数、UpdateTime、Movement、Animation、Shadow Draws、动态 BLAS | 每个开关必须改变对应消费者计数；逐项记录净收益与画质/动作回退，不以降低预算代替策略验证 | 待执行 |
| 2026-08-30 | E3 | 检查档位抖动和任务等待 | 当前默认 20 敌人录制稳定区间 Insights；补充档位升降、LOD 改变、阴影切换、动画间隔改变计数 | RT/RHI Critical Path、Task Graph、`WaitForGatherDynamicMeshElements`、四类状态切换次数及 P95/P99 | 区分稳定成本与频繁状态切换；若发生抖动，先修滞回/状态写入，再调权重 | 待执行 |
| 2026-08-29 | E4 | 审计环境 Draw Call 来源 | 0 敌人固定机位；Primitive Stats、Mesh Draw Command 动态实例化统计、资产/组件/材质 Section 清单 | 按 Static Mesh 的实例数、组件数、材质 Section、Mobility、阴影、Cull Distance、Draw Calls | 保留历史证据和诊断口径，不再为当前版本修改资产或实施合批 | 已暂缓 |
| 2026-09-01 | E5 | 验证一次真实环境合批 | E4 Baseline 与一个局部 HISM/ISM 或小范围 HLOD 候选，各三次 | RHI Draw Calls、BasePass/Prepass/Shadow Draws、RT/RHIT、Visibility P95/P99、Primitives、Bounds、显存、视觉/碰撞 | 仅在以后恢复 Draw Call 专项时执行 | 已暂缓 |
| 2026-08-30 | E6 | 当前策略规模复核 | 当前默认策略下 `20/80/160 Enemy`，固定镜头、种子、分辨率和采样时长 | FPS、Frame/GT/RT/RHIT/GPU P95/P99、Movement、Animation、Draw Calls、Significance 消费者人数 | 验证消费者封顶与 CPU 规模趋势；运行级 RT/RHIT 非单调，后续正式曲线需每档三次随机顺序 | 已完成一轮 |

**E1 完成结果（2026-08-28）**

| 指标 | 0 Enemy | 20 Enemy | 敌人增量 |
| --- | ---: | ---: | ---: |
| FPS | 49.96 | 43.44 | -6.52 |
| Frame Mean / P95 | 20.014 / 23.952 ms | 23.020 / 29.113 ms | +3.006 / +5.161 ms |
| GT Mean | 2.289 ms | 4.243 ms | +1.953 ms |
| RT Mean | 19.418 ms | 22.555 ms | +3.138 ms |
| RHIT Mean | 17.267 ms | 19.960 ms | +2.694 ms |
| GPU Mean | 7.422 ms | 8.620 ms | +1.198 ms |
| 有效 RHI Draw Calls | 1648.8 | 1859.5 | +210.7 / +12.8% |
| 有效 RHI Primitives | 647.9 K | 1355.4 K | +707.5 K / +109.2% |
| GPU ShadowDepths | 0.740 ms | 1.001 ms | +0.261 ms |

`0 Enemy` 仍包含环境、玩家、武器、灯光和游戏框架，它不是“只有静态网格”。但 0 敌人 RT 已为 `19.42 ms`，有效 Draw Call 仍占 20 敌人组的约 88.7%，证明固定渲染底座已经超过 60 FPS 预算；20 敌人再增加约 `3.01 ms` Frame、`211` Draw Call 和 `707.5 K` Primitives。当前必须并行治理环境提交和敌人消费者，不能把低 FPS 全归因于敌人，也不能只降低敌人质量预算。完整质检、异常样本说明和计数口径见 `Saved/Profiling/EnemyContributionAB_20260828_CurrentSignificance/analysis.md`。

**E1 当前构建快速复核（2026-08-29，仅一组有效配对）**

当前源码重新编译 Editor Target 后先执行 `0 Enemy -> 20 Enemy`。首个 0 敌人进程的 Frame Mean 为 `23.032 ms`，第二个 20 敌人进程反而为 `14.078 ms`，RT/RHIT 也整体反向；同机位截图一致，因此该对判定为首次进程的 PSO/DDC/资源预热顺序污染，不用于敌人收益结论。保留原始目录，不能挑选其“更好看”的一侧参与平均。

随后执行反序 `20 Enemy -> 0 Enemy`，两组都在前一对完成后运行，可作为本轮热缓存快速归因：

| 指标 | 0 Enemy | 20 Enemy | `20 - 0` |
| --- | ---: | ---: | ---: |
| 等效 FPS（`1000 / Frame Mean`） | 91.47 | 85.47 | -6.01 |
| Frame Mean / P95 / P99 | 10.932 / 13.488 / 13.996 ms | 11.701 / 14.510 / 15.419 ms | +0.768 / +1.023 / +1.423 ms |
| GT Mean | 2.042 ms | 3.413 ms | +1.372 ms |
| RT Mean | 10.666 ms | 11.423 ms | +0.757 ms |
| RHIT Mean | 7.435 ms | 7.873 ms | +0.438 ms |
| GPU Mean / P95 / P99 | 7.226 / 7.395 / 7.481 ms | 8.317 / 9.517 / 9.952 ms | +1.090 / +2.122 / +2.471 ms |
| Visibility Wait Mean | 3.212 ms | 3.716 ms | +0.504 ms |
| RHI Draw Calls | 1593.7 | 1818.4 | +224.7 / +14.1% |
| RHI Primitives | 627.4 K | 1321.4 K | +694.0 K / +110.6% |
| GPU ShadowDepths | 0.717 ms | 0.975 ms | +0.258 ms |
| GPU RT Dynamic Geometry | 0.1600 ms | 0.1640 ms | +0.0039 ms |
| CharacterMovement / Animation | 0.082 / 0.208 ms | 0.401 / 0.603 ms | +0.319 / +0.396 ms |

20 敌人组最终 `alive=20`，初始生成有 4 次可恢复重试，VSM 队列溢出和纹理池告警均为 0。采样开始快照为 Movement `4/16/0`、Render `12/3/5`、LOD `13/3/4`、活动攻击者 `4`、Shadow `5`、RT Visible `11`、Animation Sharing Followers `7`。启动日志确认当前渲染预算仍为 `12/5/12`；日志没有输出配置的攻击预算，因此 `attacking=4` 只能证明该时刻有 4 个活动攻击者，不能证明目标 Top 8 已实现。结合源码默认 `MaxActiveAttackers=4`，下一版必须先实现并记录有效预算 `8/12/8/12`，再进入 E2A。

这组快速复核说明当前 20 敌人的稳定增量同时落在 GT 的 Movement/Animation、RT 的 Visibility/提交以及 GPU 阴影上；平均动态 RT 增量在这一次配对中很小，但它不是 RT 单变量消融，不能推翻既有骨骼 RT A/B。由于本轮只有一组有效热缓存配对，只用于检查当前构建方向和策略是否生效，不替代三次 AB/BA 的正式结论。

环境 Draw Call 合批已经暂缓。80 敌人 E2B 首轮已经完成：保留有稳定局部证据的 Movement、Animation Sharing、阴影参与限制和骨骼 RT 参与限制；骨骼 LOD、动画频率分级、独立 RT Top 12 和具体权重/阈值不写成已验证优化。下一步先用 Insights 解释动画 TickInterval 与 RT/RHI 快慢运行状态的关系，再决定回退还是重做动画分级；只有机制通过后才对距离、频率和 Top-N 做三点扫描。

### 8.4 每组固定采集指标

| 类别 | 必采指标 | 目的 |
| --- | --- | --- |
| 帧结果 | FPS、Frame Mean/P95/P99/Max | 判断平均吞吐和尾帧，不用单张截图代替区间 |
| 线程关键路径 | GT、RT、RHIT、GPU Mean/P95/P99 | 判断真正限制帧率的线程；CPU 与 GPU 优化不能混写 |
| 提交与可见性 | RHI Draw Calls、Primitives、BasePass/Prepass/Shadow Draw Calls、RenderOther、Visibility Wait | 验证合批、剔除和动态网格收集是否真的改善 |
| Insights 深挖 | InitViews、VisibilityCommands、`WaitForGatherDynamicMeshElements`、Task Graph、RHI 提交 | CSV 只能分类时，用调用链确定被等待任务和关键路径 |
| 硬件光追 | RayTracing Dynamic Geometry/Scene、动态实例收集、Bottom-Level AS Build | 区分普通光栅提交与动态骨骼 BLAS 成本 |
| 敌人 CPU | CharacterMovement、Animation、AI Decision、EndPhysics、SyncBodies、移动/骨骼 Tick 数 | 判断敌人增量和分级消费者的真实 CPU 收益 |
| Significance 状态 | `SampleTime/PolicyGeneration`、Gameplay/Render 三档、Top 8 进攻、Top 12 Full Render、Top 8 Shadow、Top 12 RT、LOD0/1/2+、Sharing、预算占用/换手、UpdateTime、实际周期、Diff Apply 与状态切换次数 | 证明同轮数据同代、策略改变了谁，并确认没有重复写组件 |
| 攻击调度 | QueueLength、TokenUtilization、Wait P50/P95/P99/Max、GrantToAttack、AttackStarts/100ms、有效伤害窗口并发、令牌超时/泄漏 | 验证 8 个进攻者没有造成贴脸站桩、饥饿、同时爆发或 AI 接棒长尾 |
| 资源与异常 | Working Set、GPU/Texture Memory、UObject、Spawn Failures、VSM/Texture Pool 告警 | 防止用内存、对象泄漏或异常状态换取表面帧率 |
| 功能与画质 | 同机位截图、LOD Pop、阴影闪烁、反射缺失、近战响应、Socket Sweep、死亡/布娃娃/回收 | 性能通过但玩法或画质回退时必须撤销 |

### 8.5 单次实验记录模板

```text
日期 / ID：
构建与变更：
地图 / 分辨率 / 画质 / VSync：
敌人数 / 种子 / 机位 / 战斗阶段：
预热 / 采样 / 重复次数 / 执行顺序：
唯一自变量：
状态生效证据：
Frame / GT / RT / RHIT / GPU（Mean/P95/P99）：
Draw Calls / Primitives / Visibility / Shadow / RT Geometry：
Movement / Animation / AI / Significance 状态：
告警、画质和玩法回归：
结论：保留 / 撤销 / 证据不足需复测
证据目录：
```

## 9. 证据位置

```text
Saved/Profiling/FPS_FinalLOD_20260816
Saved/Profiling/VSM_CoarsePages_AB_20260816
Saved/Profiling/VSM_DynamicThreshold_20260816
Saved/Profiling/VSM_RadiusThreshold_20260816
Saved/Profiling/LifecycleCleanup_80_20260816
Saved/Profiling/InsightsFirstDiagnosis_20260824
Saved/Profiling/ProfileGuidedAblation_20260824
Saved/Profiling/SkeletalRayTracingAB_20260827
Saved/Profiling/EnemyRenderOffGroups_20260828
Saved/Profiling/SignificanceLayeredLOD_20260828
Saved/Profiling/EnemyContributionAB_20260828_CurrentSignificance
Saved/Profiling/EnemyContributionAB_20260828_Replacement
Saved/Profiling/EnemyContributionAB_20260829_OnePair
Saved/Profiling/EnemyContributionAB_20260829_ReversePair
Saved/Profiling/CurrentScaleMatrix_Warm_20260830
Docs/PerformanceEvidence/20260816
Docs/PerformanceEvidence/20260824
Docs/PerformanceEvidence/20260825/SmallEnemyUI
Docs/PerformanceEvidence/20260826/DrawRHI
Saved/Logs/FinalPerformanceClosure_Build.log
```

面试时按“现象 -> 指标 -> 假设 -> 单变量实验 -> 结果 -> 是否保留”讲述，不把告警消失等同于性能优化成功。
