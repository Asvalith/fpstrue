# FPS Performance Baseline

## 1. 测试目的

记录优化前的真实性能，后续所有 CPU、GPU、动画、AI 和纹理优化都必须使用相同条件复测，禁止只凭体感描述“性能提升”。

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
