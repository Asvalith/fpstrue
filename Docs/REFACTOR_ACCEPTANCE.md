# 两项目逻辑收敛与验收（2026-10-24）

本轮只保留现有玩法，不新增通用框架。每个运行状态有唯一写入者；客户端表现、服务器裁决和测试夹具不是三套玩法实现。

## 基线

- FPS：`e05f216` 加已有未提交的多人迁移、AI 黑板/槽位修改。保留全部既有改动；`Floor_400x400.uasset` 是用户改动，不修改。
- Co-op：`f6ad97f`，开始时工作区干净。
- 既有测试与性能报告仅作历史证据，不视为本轮通过。

## 执行清单

- [x] FPS：共享目标回调失效、移动失败退避及生命周期边界；20 项 AI 回归已通过。
- [x] FPS：本次握手接纳与缓存凭证分离；监听入口阶段统一。
- [x] FPS：换弹统一服务器时钟，删除动画结算的旧单机分支及两处资产 Notify。
- [x] FPS：迁移四张地图和两个 HUD 的旧 GameMode 接线，编译并核对剩余引用。
- [x] Co-op：重开请求准入、失败回复和恢复结果闭环。
- [x] Co-op：攻击时序、失控取消、攻击距离单一来源。
- [x] Co-op：合并同形成本 GE 与失败提示末端，迁移旧类型引用。
- [x] 两项目：蓝图资产与 C++ 契约检查、Editor/Shipping 原生编译。
- [x] 两项目：完整自动化；FPS 99/99，Co-op 174/174。
- [ ] 两项目：正常/弱网联机核心链路回归。
- [x] 整理实际职责、删除清单、验证结果与未覆盖边界；未通过的门禁不勾成完成。

## 验收原则

不以文件变短代替规则收敛；不删除不同时间点的必要复核。资产修改先备份，确认引用后再删。原始失败日志保留，测试修复不得降低生产断言。未运行、失败和通过分别记录；没有人工通关或连续画面验收时，不宣称这两项完成。

## 本轮实际证据

最终文档收尾又运行两项目 Editor/Shipping 增量编译，四个目标均 up to date、退出 0；日志分别在两项目的 `Saved/FinalInterview_20261024/`。收尾仅整理当前说明与既有证据，没有把完整自动化或联机专项重新计作一轮新运行。

- Co-op：`Saved/Refactor_20261024/BuildEditor_R4.log`、`BuildShipping_R1.log` 编译成功；`Automation_R3/index.json` 为 174 clean、0 warning、0 failed/notRun。
- Co-op：`Contracts_R4.json` 静态审计 42 条，43 个所查包磁盘内容不变，0 错误/警告；`AssetsValidate_R1.log`、`AIAssetsValidate_R1.log` 均 success=1。范围限于所列项目资产，不是全部第三方素材健康证明。
- FPS：最终 `BuildEditor_R6.log`、`BuildShipping_R2.log` 编译成功；`Contracts_R2.log` 实际编译 23 个蓝图、1 行为树和 1 黑板，0 加载/编译失败，没有保存包。该静态审计为 R5；R6 只删除无消费者私有字段并纠正测试夹具，完整自动化重新加载并回归实际资产。
- FPS：`RetiredReferences_R3.log` 冷读四张地图均为原生 PvpGameMode、两个 HUD 均为原生 PvpIngameWidget；0 读取或契约失败。R2 的 Registry 空结果处理错误已修复，原失败日志保留。
- FPS：最终 `Automation_R3/index.json` 为 86 clean + 13 warned、0 failed/notRun / 99。R2 的 1 项远端换弹失败源于夹具没有真实发布网络身份却硬写代际，已改为真实装备发布并保持原四条表现断言、补充前置身份断言；不放宽生产拒旧规则。受众拆分专项验证消费逻辑，不替代真实双端传输。
- FPS：`BuildPvpDelivery.ps1 -SelfTest` 62 项通过，源注册与批准清单均为 99 项；此工具自测不算作 UE 或联机测试。
- 5 个退休资产已移出 Content：两个旧 GameMode 蓝图、DA_FPMatchWaves、winorfail 和 mainmenushake。备份在 `Saved/Backups/RetiredGameplay_20261024/`，保留校验一致的文件；仍被素材场景引用的菜单/暂停和 HUD 布局不删。

武器复制只派生两类受众快照：公共装备/动作给相关观察者，弹药/备弹给拥有者。逐发结果只有既有可靠 ClientShotResult，不再在快照重复携带无消费者的最终 ShotId/Result。两个快照由同一权威提交生成；不增加第二处扣弹、Timer 或 Manager。公共表现版本与弹药确认版本分开，避免新 ACK 把迟到的合法换弹表现挡掉。此改动要求双方更新同一版本，不可直接使用旧 Cook/Stage 声称已交付。

## 当前联机结果与仍未闭环项

Co-op：`Saved/Refactor_20261024/Network_Combat_R2/`、`Network_CombatLifecycle_R1/`、`Network_CombatExpansion_R1/`、`Network_CombatReconnect_R1/` 各 Normal/Harsh 一轮，**8/8 完整通过**。同一磁盘模块与运行前输入清单，24 个 UE peer 正常退出、0 强杀、0 报告错误。Harsh 是三端 PktLag=200、Variance=50、Loss=5，不当作实测 RTT；本轮均为 CombatTest 专项图、NullRHI，覆盖战斗、取消、冲刺/弹反/救援及存活/倒地/死亡恢复，不等于正式地图画面或通关。最初 Combat_R1 的宿主中断和不完整报告原样保留，不计入八轮通过。

FPS 均为本机 Editor DS、双独立客户端、NullRHI，不是新打包运行或连续画面验收：

| 当前报告（Saved/Networking/） | 结果 | 说明 |
| --- | --- | --- |
| PvpSmoke_20261024_111853_8628bb5d | Normal 通过 | 34 接受/34 最终 ACK、无重复/漏确认，原 Pawn/PlayerState 与资源恢复通过 |
| PvpSmoke_20261024_112020_b13ff3ee | Harsh 政策专项未覆盖 | 36 全接受，未触发该专项要求的 stale 边界；重连未启动 |
| PvpSmoke_20261024_112150_f8dda48c | Harsh 严格模式失败 | 42 预测=36 接受+6 过期拒绝，42 最终 ACK，0 未结账本；零拒绝门禁未满足，重连未启动 |

Harsh 两轮均为每方向 PktLag=100、Variance=20、Loss=5，不称为 RTT。六次拒绝均发生在准入而非执行队列，Age 255～394ms 超过当时 233.321ms 窗口；其请求构造到拒绝的同机日志时间与 Age 差不超过约 2.84ms，已有多次时钟同步。因此证据支持确实迟到后按策略拒收，不支持“首次没同步导致数百毫秒估时错误”的结论。拒绝没有改弹药、备弹或执行编号，所有终结确认均到达，观察端无他人 ACK。

这不等于严格弱网体验通过，也不足以区分可靠重传、发送/接收调度和并行测试争用。保留两种不同契约的原报告，不随机反复重跑、不放宽生产时间窗。后续需独占机器，把 NetTrace 包/bunch 与调度记录按 Shot 身份关联，明确延迟发生在哪一段；MovingHit 本轮未请求，不能称已修复。烟测按原脚本验证身份后强停自有 UE，只有脚本返回状态，不能写为 UE 自然退出 0。

## 群体 AI：先验证减量，再考虑计算内核

### 最后一次线程/模块复核（2026-10-24）

当前讲解入口见 [INTERVIEW_GUIDE](INTERVIEW_GUIDE.md)。本轮最后一次诊断与下方早先的无 Trace A/B 分开，不能直接相减。

`FinalModules_20261024_Simple_Trace_R1`：当前 Simple 六原生查询盒，Editor 专服 + 两客户端，其中 Client1 为 DX12 1600×900、RTX 5060 Laptop，Client2 为 NullRHI；人口一直为 160 AI / 2 玩家，预热后约 62 秒。服务端 1865 完整帧、渲染端 5578 完整帧，以分布时钟锚点匹配全部帧序列验证唯一对齐区间。独占采集时段没有并行的编译、源码扫描或另一个试玩。

| 观察，ms/帧 | 服务端 | 渲染客户端 |
| --- | ---: | ---: |
| 平均帧间隔 | 33.160 | 11.074 |
| P95 / P99 帧间隔 | 35.997 / 37.688 | 12.610 / 13.956 |
| GT 实际 CPU（GetThreadTimes） | 12.165 | 8.521 |
| 引擎 unit RT / RHI / GPU | 不适用 | 8.027 / 8.938 / 6.973 |
| 引擎节拍 Idle | 20.699 | 0.000 |

服务端实际上限为 30 Hz。调用树 CMC 为 3.262 ms、TickFlush 为 2.370 ms、骨骼组件 Tick 为 1.635 ms；GT 物理启动/完成 Tick 为约 1.375/0.766 ms，子项变换同步不能再重复相加。Sig 协调更新 0.028 ms，EQS 位置校验 0.010 ms，不支持先换空间索引/并行评分内核。

客户端 GT 的骨骼渲染数据更新是当前显著消费者：MeshObject Update 约 1.284 ms、Calc SkelMesh Bounds 0.596 ms；RT 的动态网格收集等待入口约 1.683 ms；RHI 的 GPUSkinCache 范围约 2.922 ms、Submission_Wait 约 1.066 ms。这些分别是线程上的 inclusive scope，不相加为整帧。GT 的 WaitUntilTasksComplete 也包含实际执行的嵌套任务，不是同数值的纯阻塞。

客户端首尾 133/136 个共享姿态跟随者，RenderTier 为 20/67/73 → 20/54/86，Trace 有 Worker 动画事件；说明本地分级与共享正在工作。专服 ShouldUseThreadingForPerformance=false，未见对应 Worker 动画事件，不能因为 a.ParallelAnimUpdate=1 就称已并行。服务器首尾 156 个 Gameplay Full 的主要原因是距离，不等于客户端渲染与共享策略一起失效；完整权威姿态仅 9 → 6 个。

原始数据在 `Saved/Profiling/FinalModules_20261024_Simple_Trace_R1/`，Trace 在 `C:/UEBuild/FPSDiagnostics/FinalModules_20261024_Simple_Trace_R1/Editor_R1/`；可随仓库检查的摘要见 [Final160Diagnostic_20261024.json](Final160Diagnostic_20261024.json)。UE5.5 的全 scope 导出混入 GPU timeline 别名，代表帧分析仅排除明确的 GPU 元数据，保留全部 CPU/缺元数据作用域并记录数量。初次筛选失败的局部输出保留，最终客户端分析只采用 `MedianCPUOnly_R2/WaitAnalysis.json`。

本次为单轮带 Trace 的 Editor 诊断，不是重复正式基线、Shipping 运行、硬件采样调用栈或 OS Ready/Blocked 归因。不能把它的等待入口直接说成旧构建的历史遮挡瓶颈，也不据此宣传新版性能提升比例。

### 最后一次槽位管理器检查

- 当前每目标 8 内圈 + 12 外圈是稳定空间槽位；FIFO 存晋升次序，反向表存归属。死亡/退出清占用，不环形删除数组。单次攻击候补上限 8 与空间候场名单不是一个队列；后者并未限制为 8。
- 明确移动失败回收当前席位，退避 1 秒后重新排队；8 秒无进展也回收；版本检查避免旧路径结果释放新席位。采集首尾两管理器的超时计数 2→14、1→12，即本段新增 23 次兜底回收，不代表已完成动态障碍玩法验收。
- **仍需修正**：外圈 Kind0 只做导航投影，未做内圈同等级的胶囊站立/点间距检查；失败重排没有排除坏槽/敌人-槽位组合。几何不可站、个体不可达和临时拥堵要分别处理，不能一律全局删槽或无限换人重试。
- 现有 `fpstrue.AI.Surround` 5 项最终通过，4 clean + 1 warned、0 failed/notRun。警告为攻击队列夹具的无骨架/无渲染数据，不隐去。队列用例固定导航输出，证明归属/FIFO/回收边界，不证明墙角/窄路能站。
- 首次专项启动在测试执行前因默认 Zen/DDC 无可写节点失败；改用已有项目测试缓存图后完成。两个原始日志与报告均保留，不修改生产缓存策略。本轮检查未改玩法代码。

已有：黑板缓存 KeyID、间隔决策和事件唤醒、共享站位查询、同目标移动去重、到位复用、失败退避、World 寻路预算；Significance 使用冻结位置、复用近战采样、插件并行评分，相机派生值每轮一次，三个本地资源预算独立分配。不要重复实现这些功能。

### 本轮固定 160 AI 的实际诊断

使用现有 `RunGroupAIBaseline.ps1`，Editor 专服、一个 DX12 渲染客户端和一个 NullRHI 对照客户端；人口始终 160 AI / 2 玩家，预热后约 60 秒采集。两组输入指纹一致，均 Valid=true，六个自有进程自然退出 0，未强停。结果分别在 `Saved/Profiling/Refactor160_20261024_Reference_Cpu_A1/` 与 `Refactor160_20261024_Simple_Cpu_B1/`。

| 均值，ms | Reference 回退对照 | Simple 六盒体策略 |
| --- | ---: | ---: |
| 服务器帧间隔，墙钟 | 43.240 | 33.151 |
| 服务器 World Tick，含依赖等待的墙钟 | 42.627 | 13.873 |
| 服务器 Game Thread 帧内实际 CPU | 41.091 | 13.572 |
| 服务器 World 内 Game Thread 实际 CPU | 40.599 | 12.562 |
| 服务器 TickFlush 原生计时 | 3.493 | 2.799 |
| 引擎报告的节拍 Idle | 0.020 | 18.696 |
| 渲染客户端帧间隔，墙钟 | 9.993 | 9.471 |

两组实际 NetServerMaxTickRate 均为 30Hz。Simple 的约 33ms 帧间隔含约 18.7ms 节拍空闲，不代表仍有 33ms 业务工作，也不能把 Idle 称为线程依赖等待。Reference 的高 GT CPU 说明不能沿用旧报告的“大量睡眠”判断；实际 CPU 仍可能包含任务执行或忙等，未采调用栈/调度时不做更细归因。

Reference 实测每敌 18 个骨骼 Body/形状和 17 项约束登记，160 个 Mesh 共 2880 个有效 Body；独立六盒组件虽存在，但未启用查询。Simple 实测 960 个原生查询盒、无凸包；撤去活体 Mesh 骨骼物理，完整权威姿态保护在结束快照为 10 个而非 Reference 的 160 个。约束登记数不等于实际模拟约束数。服务器 Gameplay Full 的距离原因在 Simple 结束快照覆盖 160 个单位，说明玩法 Full 与“所有单位都必须完整骨骼姿态”已经解耦，不是所有 LOD 一起失效。

`bEnableSimpleHitProxies` 原生默认 true；Reference 是本次命令行显式回退，不可把其成本冒充正式 Simple 默认值。客户端原生 GT/RT/RHI/GPU 均值分别为 Reference 9.942/7.177/7.507/6.691，Simple 7.940/6.968/7.321/6.601，线程/设备工作会重叠，不能相加为帧时间；NullRHI 端缺失的单位指标是 null，不是 0。

这只是各一轮的 CpuDiagnostic 探索，不是三次重复的正式基线或 Shipping 运行。两组 AI 活动分布随模拟推进不同，不能当等工作量微基准；Simple 期间另有约 3 秒只读源码检索，已记录为潜在干扰。不能将差值独立归因于盒体形状、动画或单个模块，也不据此宣传稳定降幅。没有新增采集框架或改动生产配置。

### 当前仍需定位的工作

1. 候场 offset 默认约 610～1030 cm，而 Gameplay Full 距离为 1500 cm。大量候场者可能仍是 Full，决策降频并不等于移动降频。先统计候场人数、移动比例、MoveTo/失败重试、CMC 与地面查询的实际成本，不凭队列身份直接关闭感知或碰撞。
2. 固定同一个 Simple 后，才用 ContinuousFloorChecks 反向消融地面复用；本轮未做第三组，不借前两组推导地面检测的独立贡献。活体根盒体是命中精度取舍，后续仍要补射击/近战边界与连续画面验收，不能只看 CPU。

SoA、SIMD、空间哈希和对象池暂不增加：评分已并行，20 个槽位的扫描不是 RVO 邻居扫描，固定存活人口也不能证明创建/销毁是热点。只有真实热点与对照数据支持时，才挑一个计算内核优化；CPU ParallelFor/SIMD 不称为 GPU 优化。旧耗时不作为当前结论，墙钟 scope 也不等于纯 CPU 执行时间。
