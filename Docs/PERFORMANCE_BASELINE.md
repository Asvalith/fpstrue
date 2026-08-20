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
- 当前机器上约 40 个活跃敌人可维持约 60 FPS；80 是压力档，160 已明显受 Game Thread 限制。
- 已在代码中实现 AI 决策降频、路径刷新阈值、移动 Tick 分级、动画可见性策略、阴影距离分级和尸体延迟回收。
- 六张高占用植物纹理的驻留内存降低约 60 MB。
- 已分离 Texture Streaming Pool 与 VSM Non-Nanite 队列问题，并完成多组单变量实验。
- VSM 告警目前是已定位、已验证候选方案但仍有残余的风险，不能表述为彻底修复。

## 2. 固定性能矩阵

测试条件：Development Editor 独立进程、`Demonstration`、1600x900、关闭 VSync、预热 10 秒、采样约 30 秒。

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
- `160`：Game Thread 为 27.899 ms，明显高于 GPU 14.205 ms，首先应治理 CPU 侧敌人更新。
- 路径查询不是这组数据中的第一大项；移动、动画和敌人生命周期更值得优先处理。
- 不能只看平均 FPS。P95/P99 用于暴露生成、回收、寻路或资源流送造成的尖峰。

## 3. 代码中已经实现的优化

### 3.1 AI 决策与寻路

`AfpstrueEnemyAIController` 不启用 Actor Tick，而是使用一次性 Timer 驱动 `Idle / Chase / Attack / Dead` FSM：

- 不同状态和距离使用不同决策间隔。
- 每次决策结束后再安排下一次，便于停止和重设。
- `MoveTo` 只在没有路径目标、目标位移超过 `PathRefreshDistance` 或当前路径空闲时刷新。
- 死亡、UnPossess 和 EndPlay 都清理 Timer、停止移动并释放站位。

这减少了每帧 AI 判断和重复路径请求，但没有实现行为树、EQS 或 AI Perception。

### 3.2 移动、动画与阴影分级

`AfpstrueEnemyCharacter` 根据与玩家的距离调整成本：

- 近距离移动 Tick 间隔为 0；中距离约 0.033 秒；远距离约 0.066 秒。
- 开启 Animation Update Rate Optimization。
- 不可见时使用 `OnlyTickMontagesWhenNotRendered`；进入攻击时恢复完整姿态刷新，避免近战 Socket 位置失真。
- 超过 `ShadowCullDistance` 后关闭动态阴影，当前默认距离为 3000 cm。

这里的取舍是：远处允许响应精度下降，近战攻击窗口仍保证动画与碰撞一致。

### 3.3 出生和死亡生命周期

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
| 为什么 160 敌人先优化 CPU | Game Thread 27.899 ms，明显高于 GPU 14.205 ms；移动和动画随数量增长 |
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
Docs/PerformanceEvidence/20260816
Saved/Logs/FinalPerformanceClosure_Build.log
```

面试时按“现象 -> 指标 -> 假设 -> 单变量实验 -> 结果 -> 是否保留”讲述，不把告警消失等同于性能优化成功。
