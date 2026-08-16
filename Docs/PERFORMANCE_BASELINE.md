# FPS Performance Baseline

> 文档身份：项目性能数字与实验边界的唯一权威来源。
>
> 最新结论：2026-08-16 最终矩阵见第 12 节，性能封口与发布边界见第 13 节。第 1～9 节保留早期基线和纹理实验过程，不应用旧数字覆盖最终矩阵。

## 1. 测试目的

记录各阶段的真实性能，后续所有 CPU、GPU、动画、AI 和纹理优化都必须使用相同条件复测，禁止只凭体感描述“性能提升”。

本轮是“现有玩法基线”，用于验证采集流程和发现主要瓶颈。当前波次会让存活敌人数依次达到 5、12、21，因此它还不是最终的固定 10/25/50 敌人压力测试。

## 2. 测试条件

- 日期：2026-07-31
- 引擎：Unreal Engine 5.5.4
- 构建：Development，独立 `-game` 进程
- 地图：`/Game/FactoryDistrict/Maps/Demonstration`
- GameMode：`fpstruegamemode_C`
- 分辨率：1600 x 900，窗口模式
- VSync：关闭
- 帧率上限：关闭
- 光线追踪：开启
- Texture Streaming Pool：1000 MB
- CPU：Intel Core i7-14650HX
- 采样时间：31.61 秒
- 采样帧数：2366
- 工具：UE CSV Profiler，启用 GPU Stats

原始数据：

`E:\ueprojrct\fpstrue_safe2\Saved\Profiling\CSV\Profile(20260731_010055).csv`

运行日志：

`E:\ueprojrct\fpstrue_safe2\Saved\Logs\fpstrue_2.log`

## 3. 核心结果

| 存活敌人 | 样本帧 | 平均 FPS | 平均帧时间 | P95 帧时间 | 近似 1% Low | Game Thread | Render Thread | GPU |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 5 | 399 | 79.6 | 12.556 ms | 14.863 ms | 61.3 FPS | 3.684 ms | 12.202 ms | 10.150 ms |
| 12 | 392 | 78.3 | 12.766 ms | 15.406 ms | 60.2 FPS | 4.294 ms | 12.413 ms | 10.294 ms |
| 21 | 1513 | 72.8 | 13.735 ms | 16.467 ms | 56.6 FPS | 5.503 ms | 13.343 ms | 10.719 ms |

从 5 个敌人增加到 21 个敌人：

- Game Thread 平均时间增加约 49%。
- Render Thread 平均时间增加约 9%。
- GPU 平均时间增加约 6%。
- 平均 FPS 从 79.6 降到 72.8，下降约 9%。
- 当前最长线程是 Render Thread，因此现阶段主要受渲染线程限制，不是纯 AI CPU 瓶颈。

## 4. AI 与角色成本

| 指标 | 5 个敌人 | 12 个敌人 | 21 个敌人 |
| --- | ---: | ---: | ---: |
| TickActors 平均 | 0.157 ms | 0.231 ms | 0.330 ms |
| Animation 平均 | 0.283 ms | 0.394 ms | 0.561 ms |
| CharacterMovement 平均 | 0.446 ms | 0.644 ms | 0.999 ms |
| SkeletalMesh Tick 数 | 7 | 14 | 23 |
| PathFollowing Tick 数 | 5 | 12 | 21 |
| Pathfinding 平均 | 0.001 ms | 0.001 ms | 0.001 ms |

结论：

- CharacterMovement 是当前敌人数量增长时最明显的 Game Thread 成本。
- Animation 与 SkeletalMesh Tick 接近线性增长，适合后续做距离分级和动画更新频率实验。
- Pathfinding 当前不是主要成本，不能为了“优化寻路”盲目重构。
- 当前敌人规模下不需要优先多线程化 AI。

## 5. GPU 与渲染数据

| 指标 | 5 个敌人 | 12 个敌人 | 21 个敌人 |
| --- | ---: | ---: | ---: |
| 平均 Draw Calls | 2110 | 2153 | 2218 |
| GPU Lumen Scene Lighting | 1.390 ms | 1.418 ms | 1.438 ms |
| GPU Lumen Screen Probe Gather | 1.884 ms | 1.900 ms | 1.890 ms |
| GPU TSR | 1.581 ms | 1.585 ms | 1.594 ms |
| GPU Shadow Depths | 0.460 ms | 0.465 ms | 0.576 ms |
| 本地 GPU 显存使用 | 2908 MB | 2894 MB | 2896 MB |

当前场景本身约有 2100 个 Draw Calls，远高于敌人新增带来的变化。后续 GPU 优化应先分析场景、阴影、Lumen 和材质，而不是把全部问题归因于 AI。

本次独立运行日志没有出现 Texture Streaming Pool Over Budget。CSV 中 Texture Streaming Pool 为 1000 MB，Wanted Mips 约 204-207 MB。编辑器中曾出现的纹理池警告需要单独复现，不能直接用本轮数据宣称已经修复。

## 6. 已发现的优化候选

1. 整波敌人在同一帧生成，Actor Spawning 最大耗时达到 10.719 ms，适合比较“瞬时生成”和“分帧生成”。
2. 对远距离敌人降低 CharacterMovement 决策与更新频率。
3. 对远距离或不可见敌人启用动画更新率优化，死亡后关闭移动、动画和不必要 Tick。
4. 使用固定敌人数重新测试，隔离波次变化和生成尖峰。
5. 用 Unreal Insights 为 Enemy FSM、槽位更新、MoveTo 请求和攻击窗口增加命名事件。
6. 对场景 Draw Calls、阴影和 Lumen 逐项关闭并复测 GPU 与 Render Thread。

## 7. 数据限制

- 当前是 Development 的独立游戏进程，不是 Shipping 包。
- 本轮只有一次采样，最终简历数据至少需要三次测试并取中位数。
- 5 和 12 个敌人阶段各只有约 5 秒；21 个敌人阶段约 21 秒。
- 当前测试会连续生成三波敌人，不能直接替代固定规模压力测试。
- 近似 1% Low 使用 P99 FrameTime 换算，只用于内部比较。

## 8. 固定规模压力测试

### 8.1 测试方法

为隔离波次增长、生成过程和玩家死亡的干扰，本轮固定运行：

```text
10 / 20 / 40 / 80 / 160 个敌人
```

每档测试均使用：

- 单波次，`EnemiesAddedPerWave = 0`。
- 生成数量与测试档位完全一致。
- 敌人生成稳定后等待 10 秒。
- 开启无敌模式，避免玩家死亡提前终止测试。
- 连续采样约 30 秒。
- 排除 `csvprofile stop` 所在事件帧，避免停止采样命令污染最大帧时间。
- 1600 x 900 窗口模式，VSync 关闭，光线追踪开启。

本轮每档只运行一次，用于确定性能趋势和优化目标；正式写入简历的结果仍需在优化前后各运行三次并取中位数。

### 8.2 核心结果

| 敌人数 | 样本帧 | 平均 FPS | 平均帧时间 | P95 帧时间 | 近似 1% Low | Game Thread | Render Thread | GPU |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 2377 | 78.4 | 12.752 ms | 15.014 ms | 62.1 FPS | 3.233 ms | 12.427 ms | 10.443 ms |
| 20 | 2284 | 75.4 | 13.256 ms | 15.478 ms | 59.9 FPS | 4.178 ms | 12.927 ms | 11.059 ms |
| 40 | 2137 | 70.6 | 14.165 ms | 15.869 ms | 59.3 FPS | 6.239 ms | 13.850 ms | 12.476 ms |
| 80 | 1907 | 63.0 | 15.872 ms | 17.523 ms | 54.1 FPS | 11.404 ms | 15.535 ms | 14.489 ms |
| 160 | 1459 | 48.2 | 20.741 ms | 23.196 ms | 40.6 FPS | 20.733 ms | 8.751 ms | 16.916 ms |

从 10 个敌人增加到 160 个敌人：

- 平均 FPS 从 78.4 降到 48.2，下降约 38.5%。
- Game Thread 从 3.233 ms 增至 20.733 ms，增长约 5.4 倍。
- GPU 从 10.443 ms 增至 16.916 ms，增长约 62%。
- 10～80 个敌人时主要受 Render Thread 或 GPU 限制。
- 160 个敌人时 Game Thread 与平均帧时间几乎相同，瓶颈明确转移到游戏线程。

### 8.3 AI、动画与对象成本

| 敌人数 | TickActors | CharacterMovement | Animation | Pathfinding | CrowdManager | PathFollowing Tick | SkeletalMesh Tick |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 0.216 ms | 0.561 ms | 0.364 ms | 0.000 ms | 0.003 ms | 10 | 12 |
| 20 | 0.324 ms | 0.846 ms | 0.584 ms | 0.001 ms | 0.003 ms | 20 | 22 |
| 40 | 0.579 ms | 1.633 ms | 0.908 ms | 0.013 ms | 0.003 ms | 40 | 42 |
| 80 | 2.022 ms | 3.087 ms | 1.713 ms | 0.026 ms | 0.003 ms | 80 | 82 |
| 160 | 3.649 ms | 6.920 ms | 3.190 ms | 0.071 ms | 0.003 ms | 160 | 162 |

结论：

- `CharacterMovement` 是当前最明确的敌人规模相关 CPU 成本，160 个敌人时约占 Game Thread 的三分之一。
- `Animation` 和 Skeletal Mesh Tick 随敌人数接近线性增长。
- `TickActors` 在 80 个敌人后增幅明显，应继续用 Unreal Insights 拆分具体 Actor Tick。
- `Pathfinding` 和 `CrowdManager` 当前成本很低，不能把主要问题错误归因于 A* 或 NavMesh。
- 优化优先级应为移动更新、动画更新率和 Actor Tick，其次才是寻路算法。

### 8.4 渲染、显存与内存

| 敌人数 | Draw Calls | 本地 GPU 显存 | 进程物理内存 | Wanted Mips | Non-Streaming Mips |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 2099 | 2856 MB | 3722 MB | 212 MB | 129 MB |
| 20 | 2241 | 2915 MB | 3689 MB | 208 MB | 129 MB |
| 40 | 2417 | 2984 MB | 3615 MB | 212 MB | 129 MB |
| 80 | 2820 | 3079 MB | 3694 MB | 213 MB | 129 MB |
| 160 | 3537 | 3171 MB | 3700 MB | 213 MB | 130 MB |

结论：

- Draw Calls 从约 2099 增至 3537，敌人 Skeletal Mesh 和阴影会同步增加渲染成本。
- 本地 GPU 显存增加约 315 MB，但进程物理内存没有随敌人数持续增长，当前采样未表现出明显 CPU 内存泄漏。
- Wanted Mips 基本稳定在 208～213 MB，本轮独立运行没有复现 Texture Streaming Pool 超预算。
- 纹理池警告仍需在编辑器环境单独采样，不能用这组独立运行数据宣称已经修复。

### 8.5 原始证据

CSV 目录：

`E:\ueprojrct\fpstrue_safe2\Saved\Profiling\CSV\FPS_Baseline_20260731`

文件：

```text
fixed_10_run1.csv
fixed_20_run1.csv
fixed_40_run1.csv
fixed_80_run1.csv
fixed_160_run1.csv
```

对应日志：

`E:\ueprojrct\fpstrue_safe2\Saved\Logs\benchmark_fixed_*.log`

### 8.6 下一轮实验

先保持相同地图、画质、分辨率和敌人数量，只修改一个变量：

1. 远距离敌人降低移动决策和路径刷新频率。
2. 远距离或不可见敌人启用动画更新率优化。
3. 死亡敌人停止移动、动画和不必要 Tick。
4. 对 80 和 160 敌人档位重复采样，判断 Game Thread、CharacterMovement 和 Animation 的变化。
5. 确认方案有效后，对优化前后全部档位各运行三次并取中位数。

## 9. 纹理驻留资源治理对比

### 9.1 问题与定位边界

编辑器运行高密度敌人场景时曾出现资源相关警告。排查时先区分了两个不同问题：

```text
Texture Streaming
→ 管理可流送纹理的 Mip 驻留和纹理池预算

VSM Non-Nanite Marking Job Queue overflow
→ Virtual Shadow Map 对非 Nanite 网格的阴影标记队列溢出
```

二者都可能出现在复杂场景中，但根因和优化手段不同。本轮实验只验证纹理驻留资源调整，不把仍然存在的 VSM 警告误写成 Texture Streaming Pool 问题。

定位工具：

```text
stat streaming
ListStreamingTextures
MemReport -full
```

定位结果显示，厂区场景中的松树树皮、常春藤图集和松枝图集具有较高的驻留纹理开销，因此选择 6 张植被纹理作为单变量优化对象。

### 9.2 对照条件

优化前后保持以下条件一致：

- 地图：`/Game/FactoryDistrict/Maps/Demonstration`。
- 分辨率：1600 x 900，窗口模式。
- VSync：关闭。
- 固定敌人数：100。
- 预热时间：8 秒。
- 采样时间：30 秒。
- Texture Streaming Pool：1000 MB。
- 使用 `BenchmarkTextureStats` 采集纹理列表与完整内存报告。

优化前日志：

`E:\ueprojrct\fpstrue_safe2\Saved\Logs\texture_pool_baseline_100.log`

优化后日志：

`E:\ueprojrct\fpstrue_safe2\Saved\Logs\fpstrue.log`

优化前 MemReport：

`E:\ueprojrct\fpstrue_safe2\Saved\Profiling\MemReports\Demonstration-WindowsEditor-07.31-21.50.51\Pid37712_Demonstration-WindowsEditor-31-21.50.51.memreport`

优化后 MemReport：

`E:\ueprojrct\fpstrue_safe2\Saved\Profiling\MemReports\Demonstration-WindowsEditor-07.31-22.08.13\Pid31316_Demonstration-WindowsEditor-31-22.08.13.memreport`

### 9.3 实施内容

没有直接扩大纹理池掩盖问题，而是降低不需要原始分辨率的场景植被纹理最大驻留分辨率：

| 纹理 | 优化前 | 优化后 |
| --- | ---: | ---: |
| `PineBark_A` | 2048 x 4096 | 1024 x 2048 |
| `PineBark_N` | 2048 x 4096 | 1024 x 2048 |
| `IvyAtlas_A` | 4096 x 4096 | 2048 x 2048 |
| `IvyAtlas_N` | 4096 x 4096 | 2048 x 2048 |
| `PineBranchAtlas_A` | 4096 x 2048 | 2048 x 1024 |
| `PineBranchAtlas_N` | 4096 x 2048 | 2048 x 1024 |

### 9.4 前后数据

| 指标 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | ---: |
| Streaming Assets 当前/目标占用 | 212.27 MB | 152.27 MB | -60.00 MB（-28.3%） |
| Texture Memory Used | 288.586 MB | 228.906 MB | -59.680 MB（-20.7%） |
| MemReport 纹理驻留总量 | 425.228 MB | 365.549 MB | -59.679 MB（-14.0%） |
| UObject 数量 | 51,948 | 51,954 | 基本不变 |

结论：

- 6 张高占用植被纹理的分辨率约束确实减少了约 60 MB 的纹理驻留开销。
- `Streaming Assets` 下降 28.3%，`Texture Memory Used` 下降 20.7%，该结果可用于简历和项目讲解。
- 对象数量基本不变，说明本次变化来自纹理资源规格，而不是减少运行时对象。
- 两次测试都没有出现真实的 Texture Streaming Pool 超预算，因此准确表述应为“纹理驻留资源治理”，不能写成“修复纹理池溢出”。

### 9.5 尚未完成与禁止夸大

- 尚缺相同机位、相同光照下的优化前后截图，不能宣称“画质无损”。
- 进程物理内存从 3624.34 MB 变化到 3679.15 MB，受到编辑器、分配器和采集时机影响，不能宣称总进程内存下降。
- 两次运行均存在 `VSM Non-Nanite Marking Job Queue overflow`，这是独立的阴影问题，本轮没有修复。
- UObject 数量对照不足以证明不存在泄漏；Timer、Delegate、Widget、Niagara、Decal 和尸体仍需通过连续波次与等待 GC 的实验验证。

### 9.6 面试表达

```text
我先用 stat streaming、ListStreamingTextures 和 MemReport -full
区分纹理流送占用与 VSM 阴影警告，再定位厂区植被中的高占用纹理。
我没有直接扩大纹理池，而是在相同地图、100 敌人和相同采样时长下，
调整 6 张远景植被纹理的最大驻留分辨率。
复测后 Streaming Assets 从 212.27 MB 降到 152.27 MB，
Texture Memory Used 从 288.59 MB 降到 228.91 MB。
目前还缺同机位画质回归截图，VSM 警告也属于另一条待办，
所以我不会把这次结果描述成已经解决了全部显存或阴影问题。
```

## 10. 封板前运行时优化实现

### 10.1 已实现

- 敌人 Actor 和 AIController 不使用每帧 Tick；AI 使用一次性 Timer，并按攻击、追击、远距和空闲状态选择 `0.1 / 0.25 / 0.5 / 1.0` 秒决策间隔。
- `MoveTo` 仅在目标位置变化超过阈值或路径已经空闲时刷新，避免重复提交相同导航请求。
- 敌人 Skeletal Mesh 开启 Update Rate Optimization，并按可见性和攻击优先级调整骨骼更新策略。
- CharacterMovement 按目标距离使用全速、约 30 Hz 和约 15 Hz 三档组件更新频率；攻击状态恢复全速更新。
- 敌人死亡时停止 AI、Movement 和攻击 Timer，关闭 Capsule，进入 Ragdoll，并通过 `SetLifeSpan` 统一回收；C++ 默认尸体保留时间由 300 秒收紧为 30 秒。
- 波次生成由同帧循环改为默认每 `0.05` 秒生成一个敌人的队列。结束游戏或 EndPlay 时清理生成 Timer 和队列。
- 高密度场景重复使用出生点时，NavMesh 随机采样半径按复用次数平方根扩大，最大限制为 2000 cm；失败日志记录出生点、复用次数和采样半径。

### 10.2 为什么这样修改

- 原始测试中整波生成的 Actor Spawning 峰值达到 `10.719 ms`，分帧生成用于削平单帧组件注册、动画初始化和 Controller 创建成本。
- 100/160 敌人测试表明 CharacterMovement 和 Animation 明显高于 Pathfinding，因此优先降低远距离更新频率，不重写 NavMesh 或引入复杂 AI 框架。
- 300 秒 Ragdoll 会让物理体、骨骼网格和阴影在多波次中长期累积，30 秒默认值更符合当前 90 秒玩法时长。
- 扩大高密度生成半径用于减少出生点附近空间耗尽导致的 Spawn 失败，不使用 `AlwaysSpawn` 强行制造重叠敌人。

### 10.3 当前证据边界

上述代码已经完成 Development Editor 编译，并在全新独立进程中完成 `10 / 20 / 40 / 80 / 160` 五档固定规模复测。每档均达到目标存活数量、完成 10 秒预热和约 30 秒 CSV 采样，并保留运行日志与截图。

本轮证据已经覆盖：

1. 固定规模敌人生成、AI 决策、Movement、Animation、GPU 和纹理数据。
2. Texture Streaming Pool 在五档测试中均未出现超预算警告。
3. VSM Non-Nanite 队列在正式矩阵中只在 80 敌人档出现 1 次；后续多组 80 敌人实验也各出现 1 次，因此属于低速率但可复现的残留风险。
4. 高密度档存在 CharacterMovement `Max iterations 8 hit` 警告，说明拥挤接触和移动模拟仍是进一步扩容时的治理重点。

两个零引用旧 UI 模板资产已清理，全量蓝图编译已经达到 `0 errors / 0 warnings / 0 failed to load`。Shipping 打包和打包后冒烟测试仍延后到玩法蓝图回归之后；这不影响本节的 Development 性能结论，也不能据此宣称发布验收已经完成。

## 11. VSM Non-Nanite 队列诊断

### 11.1 已确认的根因范围

- `VSM Non-Nanite Marking Job Queue overflow` 在 10 敌人基线中、敌人生成前已经出现，因此厂区场景资源是主因，敌人和尸体阴影只是运行时放大器。
- UE 5.5 的标记着色器会把覆盖大量 VSM 页面的非 Nanite 实例放入共享的大实例任务队列；队列容量耗尽后触发该警告。这不是 Texture Streaming Pool，也不是 Nanite Streaming Pool 溢出。
- 首次 Asset Registry 审计曾确认以下四个大场景网格为 `NaniteEnabled=False`：
  - `/Game/FactoryDistrict/Meshes/Building_TypeD_A`
  - `/Game/FactoryDistrict/Meshes/Pipes_Stack_A`
  - `/Game/FactoryDistrict/Meshes/Pipes_Stack_B`
  - `/Game/FactoryDistrict/Meshes/Pipes_Stack_C`
- `Building_TypeD_A` 是大面积建筑网格，包含多个材质槽。后续治理已经逐项保存并复查候选资产；这份列表只保留为根因定位记录，不能继续当作当前未处理清单。此类资产启用 Nanite 时仍必须验证透明材质、碰撞、Fallback Mesh 和画面，不能批量盲开。

### 11.2 工具与命令边界

| 工具或命令 | 正确用途 | 不能证明的内容 |
|---|---|---|
| `r.Nanite 0` | 全局关闭 Nanite 做 A/B，观察问题是否随更多网格进入非 Nanite 路径而变化 | 不能作为修复；当前警告本来就来自非 Nanite 路径 |
| `stat Nanite` | 查看 Nanite CPU 统计组 | 不能直接给出哪个非 Nanite Primitive 覆盖了最多 VSM 页面 |
| `NaniteStats VirtualShadowMaps` | 查看 Nanite/VSM GPU 统计 | 不能替代具体 Primitive 诊断 |
| Unreal Insights | 对比 GPU、RHI、Shadow/VSM Pass 时间和 CPU 提交成本 | 只有 Pass 成本，没有自动给出问题资产名单 |
| `r.Shadow.Virtual.NonNanite.NumPageAreaDiagSlots 16` | 启用覆盖页面最多的非 Nanite Primitive 诊断 | 仅用于诊断，不应作为 Shipping 配置 |
| `r.Shadow.Virtual.NonNanite.LargeInstancePageAreaThreshold 1` | 降低诊断阈值，配合日志输出 Actor 和组件名称 | 阈值过低会产生大量日志，只用于短时固定机位测试 |

`r.Shadow.Virtual.NonNanite.MaxCulledInstanceAllocationSize` 对应另一条可见实例缓冲区溢出，不是本项目的 Marking Job Queue，不能用扩大该数值掩盖问题。

### 11.3 治理顺序

1. 固定地图、机位、分辨率和光照，启用页覆盖诊断，记录排名靠前的 Actor 与组件。
2. 优先处理大面积、Opaque、静态的建筑和管线网格；逐个启用 Nanite，并验证材质、碰撞、Fallback Mesh 和视觉结果。
3. 对纯装饰或远景组件关闭不必要的动态阴影，设置合理的距离裁剪；对必须保留非 Nanite 的资源补齐 LOD，并收紧 Bounds。
4. 以 `r.Shadow.Virtual.NonNanite.IncludeInCoarsePages 0` 做单变量 A/B。若 Shadow/VSM 成本明显下降，再检查体积雾和前向半透明是否仍正确，确认后才考虑固化配置。
5. 对远距离敌人和死亡尸体按距离关闭阴影，避免多波次持续增加非 Nanite 动态阴影成本。
6. 每次只修改一类资源，复测警告、GPU Frame、ShadowDepths/VSM Pass 和画面截图。

### 11.4 当前完成边界

当前已经完成根因分类、引擎源码机制确认、页覆盖诊断、首批大面积场景资产治理、敌人阴影距离分级和三组单变量实验：

- `IncludeInCoarsePages = 0`：80 敌人档不再出现队列警告，但平均 FPS 从 `55.19` 降到 `45.79`，Render Thread 从 `17.378 ms` 增至 `20.690 ms`。性能回退明显，因此不固化。
- `DynamicCoarsePagePixelThreshold = 32 / 64`：两档仍各出现 1 次队列警告，不能解决问题。
- `r.Shadow.RadiusThreshold = 0.02 / 0.03`：两档仍各出现 1 次队列警告，不能解决问题。

项目配置只保留 `r.Shadow.Virtual.Enable=1`，没有写入实验性全局 CVar。正式五档矩阵共出现 1 次 VSM 队列警告；多组 80 敌人后续实验也各出现 1 次。当前只能证明警告没有形成连续刷屏或崩溃，但它仍可复现；缺少治理前后的多次同条件统计，不能虚构“频率下降”的结论。

封板决策是保留默认渲染路径，接受当前单次但可复现的警告风险，不通过扩大队列或全局关闭粗页掩盖问题。若未来扩大到常态 80 个以上同屏敌人，应继续按诊断排名拆分大面积非 Nanite 阴影投射物，并对固定机位运行多次 A/B 取中位数。

## 12. 2026-08-16 最终固定规模矩阵

### 12.1 测试条件

- Development Editor 独立进程。
- 地图：`/Game/FactoryDistrict/Maps/Demonstration`。
- 分辨率：1600 x 900，VSync 关闭。
- 每档预热 10 秒，采样约 30 秒。
- 固定敌人数：`10 / 20 / 40 / 80 / 160`。
- 采样期间玩家无敌，排除玩家死亡和波次切换干扰。
- 每档使用独立进程，成功达到目标敌人数后才开始采样。

### 12.2 核心结果

| 敌人数 | 平均 FPS | 平均帧时间 | P95 | P99 | Game Thread | Render Thread | GPU | CharacterMovement | Animation | Draw Calls |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 62.84 | 15.914 ms | 19.706 ms | 21.244 ms | 4.299 ms | 15.150 ms | 8.785 ms | 0.429 ms | 0.650 ms | 1659 |
| 20 | 61.15 | 16.352 ms | 19.909 ms | 21.950 ms | 5.879 ms | 15.642 ms | 9.189 ms | 0.745 ms | 0.977 ms | 1806 |
| 40 | 61.02 | 16.389 ms | 19.632 ms | 21.362 ms | 8.471 ms | 15.736 ms | 9.915 ms | 1.501 ms | 1.517 ms | 2088 |
| 80 | 55.20 | 18.115 ms | 21.076 ms | 22.547 ms | 16.106 ms | 17.355 ms | 11.591 ms | 3.231 ms | 2.728 ms | 2536 |
| 160 | 35.83 | 27.907 ms | 32.459 ms | 34.273 ms | 27.899 ms | 14.182 ms | 14.205 ms | 6.188 ms | 4.448 ms | 3458 |

### 12.3 结论与发布容量

- 当前机器上的稳定玩法容量可按 **40 个活跃敌人、约 60 FPS** 表述。
- 80 敌人是压力档，平均约 55 FPS；160 敌人是极限档，Game Thread 已成为明确瓶颈。
- 从 10 增至 160 敌人时，CharacterMovement 从 `0.429 ms` 增至 `6.188 ms`，Animation 从 `0.650 ms` 增至 `4.448 ms`，两者仍是扩容优先级。
- Pathfinding 不是主要成本；不能为了展示复杂度而优先重写 NavMesh 或自建线程池。
- Wanted Mips 在五档约为 `105 MB`，所有档位 Texture Pool 警告均为 0。
- 80 敌人档出现 1 次 VSM 队列警告；其余四档为 0。结合后续 80 敌人实验，结论是单次但可复现的残留，而非彻底修复。
- 与 2026-07-31 的旧矩阵相比，本轮整体帧率更低，不能把 LOD 和运行时治理描述成已经带来净性能提升。当前版本增加了更完整的 AI、动画、生成和生命周期逻辑，后续如需做优化收益对比，必须从当前版本重新建立单变量前后基线。

### 12.4 原始证据

```text
Saved/Profiling/FPS_FinalLOD_20260816/
  CSV/
  Logs/
  Screenshots/
  manifest.csv
  summary.csv
  summary.md

Saved/Profiling/VSM_CoarsePages_AB_20260816/
Saved/Profiling/VSM_DynamicThreshold_20260816/
Saved/Profiling/VSM_RadiusThreshold_20260816/
```

关键截图另存于 `Docs/PerformanceEvidence/20260816`，用于代码仓库中的固定证据索引。

### 12.5 80 敌人生命周期回收

测试链路：

```text
80 个活跃敌人采样 10 秒
-> 对全部敌人调用 ApplyDamage
-> HealthComponent 提交死亡
-> EnemyCharacter 停止 AI、Movement、攻击 Timer 和 Capsule
-> Ragdoll + SetLifeSpan(30s)
-> 等待 35 秒
-> 再采样 5 秒
```

| 状态 | Enemy Actor | FPS | Frame | Game Thread | Render Thread | GPU | Movement | Animation | UObject |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 活体 | 80 | 44.96 | 22.242 ms | 18.110 ms | 19.392 ms | 10.528 ms | 3.705 ms | 2.915 ms | 50,763 |
| 回收后 | 0 | 68.51 | 14.596 ms | 5.444 ms | 14.082 ms | 7.843 ms | 0.106 ms | 0.266 ms | 49,876 |

结果：

- 35 秒后敌人 Actor 与 GameMode 注册数均由 `80` 回到 `0`，证明死亡后的 LifeSpan 和注销链能够完成回收。
- UObject 数量减少 `887`，Movement 与 Animation 成本接近回到场景基线。
- 本轮 VSM 队列和 Texture Pool 警告均为 0；活体阶段仍有 41 条 CharacterMovement 最大迭代警告。
- MemReport 的进程物理内存从 `3616.00 MB` 增至 `3676.34 MB`。分配器会保留已提交页面，MemReport 本身也会扰动工作集，因此不能用工作集没有立即下降推翻 Actor/UObject 回收，也不能宣称总进程内存下降。

原始证据：

```text
Saved/Profiling/LifecycleCleanup_80_20260816/
```

## 13. 性能封口与发布验收边界

### 13.1 已完成

- 纹理驻留资源治理有前后 MemReport 数据，约减少 60 MB 纹理驻留开销。
- 固定规模五档性能矩阵、日志、CSV 和截图完整。
- 80 敌人死亡后等待 35 秒的生命周期测试完成，Actor、注册数和 UObject 均有回落证据。
- 敌人 LOD、动画更新率、移动分级、阴影距离和死亡回收已进入当前版本。
- VSM 根因、引擎机制、资产排名与三个全局 CVar 方案均有实测证据。
- 实验性 VSM 配置没有写入项目，避免为了消除警告引入更大的渲染回退。
- 生命周期测试的临时命令行入口已经移除，正式源码重新完成 Development Editor 编译；构建日志为 `Saved/Logs/FinalPerformanceClosure_Build.log`。

### 13.2 暂缓项

- 实际玩法蓝图仍需完成 PIE 回归，确认编译通过的节点在真实对象和时序下行为正确。
- PIE 回归通过后再执行 Shipping Cook/Package。
- Shipping 包生成后再执行 10 敌人启动、输入、射击、AI、胜负和自动退出冒烟测试。

因此当前状态是：**性能证据与蓝图编译已经封口，发布验收尚未封口。** 发布验收剩余项是玩法回归、Shipping 打包和产物冒烟，不再与性能治理混在一起。
