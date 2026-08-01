# FPS 与 Co-op 项目技术延伸地图

更新日期：2026-07-30

## 1. 文档用途

本文记录两个作品集项目中可以自然延伸的技术点：

- `fpstrue_safe2`：单机 FPS、敌人 AI、性能与图形表现。
- `multiplayer`：UE 多人网络、Online Subsystem 与 Co-op 玩法。

本文不是待办堆砌，也不代表所有内容都要实现。每个技术点分为四类：

| 标记 | 含义 |
| --- | --- |
| P0 | 当前投递版必须完成 |
| P1 | 完成 P0 后最值得增加 |
| P2 | 面试需要理解，可以暂不实现 |
| STOP | 当前明确不做，防止范围继续扩大 |

一项能力只有同时具备以下证据，才能写进简历：

```text
代码存在
-> 编译通过
-> 运行验证
-> 有测试或数据
-> 能解释方案、替代方案和边界
```

## 2. 组合定位

### 2.1 FPS 项目

证明：

- UE C++ Gameplay 工程能力。
- C++ 规则层与蓝图表现层的职责拆分。
- 战斗状态、动画时序和对象生命周期治理。
- 多敌人导航、包围和 Game Thread 性能意识。
- GPU、显存和风格化 Shader 基础。

### 2.2 Co-op 项目

证明：

- UE 服务端权威模型。
- Replication、RepNotify 和三类 RPC。
- Actor Ownership、网络角色与安全验证。
- Online Subsystem 和 Session 房间闭环。
- 弱网环境下的同步验证和带宽优化意识。

## 3. FPS 可延伸技术点

### 3.1 Gameplay Framework 与架构

当前连接点：

- Character 管输入意图、角色状态和弹药。
- WeaponComponent 管开火、Line Trace 和武器事件。
- HealthComponent 管统一生命值、伤害和死亡广播。
- EnemyCharacter 管敌人战斗实体和攻击窗口。
- EnemyAIController 管目标、FSM 和导航。
- GameMode 管波次、生成、倒计时和胜负。
- 蓝图负责动画、音效、特效、UI 和资源配置。

可延伸：

- P0：明确 C++ 权威规则与蓝图表现层边界。
- P0：为核心状态画类关系图、状态图和调用链。
- P1：把波次数据改为 DataAsset 或 DataTable 驱动。
- P1：将 UI 更新统一为 Delegate 事件驱动。
- P1：将跨关卡配置移动到 GameInstance。
- P2：将可复制比赛状态放入 GameState，玩家数据放入 PlayerState。
- P2：使用 Gameplay Tags 表达伤害类型和状态互斥。
- STOP：当前不引入完整 GAS，不重构全部现有战斗系统。

面试追问：

- 为什么 GameMode 不适合保存跨关卡状态。
- Controller、Pawn、Character 的职责区别。
- Component 复用与继承复用的取舍。
- Delegate 与 Tick 查询的取舍。

### 3.2 3C 与第一人称手感

当前连接点：

- 移动、跳跃、冲刺、瞄准和 FOV 插值。
- 后坐力、随机弹道和 Weapon Sway。
- 拾取武器后显示手臂和武器。

可延伸：

- P0：保证冲刺、瞄准、射击、换弹和死亡状态不会冲突。
- P1：将后坐力从单次随机值升级为 Curve/DataAsset 参数。
- P1：区分视觉后坐力和真实弹道扩散。
- P1：加入移动、蹲伏、瞄准对散布的参数影响。
- P1：使用 Aim Offset 或 Layered Blend per Bone 分离上下半身。
- P1：使用 IK 修正左手握枪和枪口位置。
- P2：摄像机惯性、落地反馈和不同武器手感配置。
- STOP：不为了展示效果增加大量武器和动画资产。

面试追问：

- 为什么只改变 FOV 不等于完整瞄准系统。
- 后坐力、准星扩散和真实弹道之间的关系。
- Timeline、Curve 和 C++ 插值各适合什么场景。

### 3.3 射击、弹药与换弹

当前连接点：

- Hitscan Line Trace。
- `FHitResult`、骨骼命中和头部伤害。
- 弹药消耗、备用弹药、换弹状态。
- 蓝图表现与 C++ 命中规则分离。

可延伸：

- P0：验证射击、空仓、换弹、死亡和动画中断矩阵。
- P0：弹药 HUD 使用事件更新并保证首次初始化。
- P1：换弹使用 AnimNotify 提交弹药，Timer 只作超时兜底。
- P1：使用本次换弹 ID 或提交标记防止重复结算。
- P1：物理材质驱动水泥、金属和角色命中反馈。
- P1：加入摄像机射线与枪口射线的二次遮挡校验。
- P2：Projectile 与 Hitscan 策略接口。
- P2：异步射线或批量查询只在数据证明需要时采用。
- STOP：当前不增加多武器工厂、背包和复杂装备系统。

面试追问：

- 为什么射线从摄像机发出，而视觉子弹从枪口发出。
- 如何忽略玩家和武器自身。
- `ECC_Visibility` 与自定义 Trace Channel 的取舍。
- 换弹中死亡、Montage 中断和 Timer 到期如何避免重复提交。

### 3.4 健康、伤害与生命周期

当前连接点：

- 玩家和敌人共用 HealthComponent。
- 血量变化、受伤和死亡 Delegate。
- 死亡只触发一次。
- Timer、攻击窗口和移动清理。

可延伸：

- P0：完成死亡和关卡重启后的 Timer、Delegate、Actor 数量审计。
- P0：连续波次后确认对象数和内存能回落。
- P1：伤害上下文记录 DamageCauser、Instigator 和 DamageType。
- P1：用弱引用保存 AI 目标和集中管理器中的敌人。
- P1：Niagara 使用引擎内置 Pool，Decal 设置数量和寿命上限。
- P1：数据证明 Spawn/Destroy 或 GC 尖峰后再实现 Enemy Object Pool。
- P2：护甲、持续伤害和不同受击方向。
- P2：通用对象池接口 `Acquire/Release/Reset`。
- STOP：不写自定义 UObject 内存分配器。

对象池必须说明：

```text
收益：减少 Spawn/Destroy 和 GC 尖峰
代价：增加常驻内存和状态重置复杂度
风险：Timer、Delegate、AI、动画或碰撞残留
```

### 3.5 近战攻击窗口

当前连接点：

- Montage 驱动攻击动画。
- AnimNotifyState 定义有效攻击窗口。
- `weapontop/weaponend` 双 Socket 连续 Sphere Sweep。
- 单次攻击命中集合防止重复扣血。
- 动画中断、死亡和目标死亡关闭窗口。

可延伸：

- P0：验证低帧率下连续 Sweep 不漏检。
- P0：验证攻击中死亡、HitReact 和 Montage 中断。
- P1：记录上一帧和当前帧 Socket 轨迹，避免高速武器穿透。
- P1：不同攻击 Montage 使用独立伤害、半径和硬直参数。
- P1：调试绘制攻击轨迹与命中对象集合。
- P2：Root Motion 攻击位移与 NavMesh 的协调。
- P2：攻击令牌与攻击窗口的超时兜底。

面试追问：

- 单点 Notify 与 NotifyState 的区别。
- Queued 与 Branching Point 的时序和成本取舍。
- 为什么不能让武器碰撞全程造成伤害。

### 3.6 AIController、FSM 与 NavMesh

当前连接点：

- Idle、Chase、Attack、Dead 显式 FSM。
- AIController 定时决策。
- NavMesh 和 MoveTo。
- 包围槽位、NavMesh 投影和攻击名额。
- 敌人持续面向玩家。

可延伸：

- P0：10/25/50 个敌人的决策、导航和响应延迟测试。
- P0：目标死亡、不可达、攻击中断和卡住边界验证。
- P0：玩家移动时包围点低频更新，不每帧重新寻路。
- P1：多圈包围与内外圈补位。
- P1：RVO 与 Detour Crowd 二选一并做对比。
- P1：AIPerception 视觉、听觉和最后已知位置。
- P1：EQS 只用于复杂地形下的候选包围点或掩体选择。
- P1：动态障碍和 Nav Link Proxy。
- P2：Recast 体素化、区域、轮廓、凸多边形和细节网格。
- P2：Detour 的 Poly A*、路径走廊和 Funnel 平滑。
- P2：Behavior Tree、EQS 与 FSM 的适用边界。
- P2：大量远距离 Agent 使用 Mass Entity 的条件。
- STOP：当前不同时实现行为树、EQS、RVO、Detour Crowd 和 Mass。

面试追问：

- NavMesh 解决路径，避障解决局部冲突，包围系统解决目标分配。
- 为什么 MoveToActor 会导致敌人朝同一点聚集。
- 为什么降低决策频率会降低 CPU，但增加响应延迟。
- 为什么不能简单关闭所有 Tick。

### 3.7 波次、生成与胜负

当前连接点：

- GameMode 管理波次、出生点、倒计时和结果广播。
- 多出生点和分批生成。

可延伸：

- P0：验证重复开始、重复结算和 Timer 清理。
- P0：验证所有出生点位于 NavMesh。
- P1：波次配置数据化。
- P1：出生点按与玩家距离、视线和可达性筛选。
- P1：加入生成预算，限制单帧 Spawn 数量。
- P1：用敌人死亡事件维护存活数量，不每帧遍历。
- P2：固定随机种子实现可复现 Benchmark。
- P2：GameState 承载多人可复制波次状态。

### 3.8 UI 与输入生命周期

当前连接点：

- 主界面、游戏内 HUD、暂停/结果界面。
- 血量、弹药和倒计时接口。
- 输入模式切换、Widget 创建和移除。

可延伸：

- P0：HUD 不使用 Tick 和每帧属性绑定。
- P0：Widget 创建一次、持有明确引用、正确移除。
- P0：验证关卡重启后不重复创建和绑定。
- P1：统一使用 PlayerController 管理输入模式和光标。
- P1：创建 C++ Widget 基类，只暴露状态和命令给 UMG。
- P2：UE MVVM 或 CommonUI。
- STOP：不增加复杂主菜单、设置菜单和大量 UI 动画。

面试追问：

- UI Only、Game and UI、Game Only 的区别。
- PlayerController 与 Pawn 在输入链路中的职责。
- 为什么 `Get All Widgets Of Class` 能工作但不应成为常规架构。

### 3.9 CPU 性能

当前连接点：

- AIController 使用低频 Timer 决策。
- EnemyCharacter 关闭旧 Tick 追逐。
- 包围系统和波次生成适合压力测试。

可延伸：

- P0：建立 10/25/50 敌人固定场景。
- P0：记录 Game、Draw、GPU 与总帧时间。
- P0：Unreal Insights 标记 AI 决策、攻击和生成函数。
- P0：比较每帧 Tick、固定间隔、错峰和距离分级。
- P1：动画 Update Rate Optimization。
- P1：远处/不可见 Mesh 降低动画更新，死亡后关闭 Mesh Tick。
- P1：碰撞查询数量和更新频率统计。
- P1：限制单帧生成数量，降低波次开始尖峰。
- P2：Task Graph 只处理纯数据计算，结果回到 Game Thread。
- P2：CSV Profiler 和自动化 Benchmark。
- STOP：没有数据前不引入多线程和复杂 Job System。

### 3.10 内存、GC 与资源管理

可延伸：

- P0：`stat memory`、`memreport -full` 和 UObject/Actor 数量。
- P0：连续战斗、等待释放、再次战斗的内存曲线。
- P0：Size Map 与 Reference Viewer 定位资源引用。
- P0：检查 Timer、Delegate、Widget、Niagara、Decal 和尸体。
- P1：区分 `TObjectPtr`、`TWeakObjectPtr`、软引用和裸指针。
- P1：大数组使用 `Reserve`，避免可预测增长中的重复扩容。
- P1：对象池设置预热、软上限、硬上限和关卡结束释放策略。
- P1：资源使用软引用和 Asset Manager 异步加载。
- P2：GC Cluster、增量 GC 和 UObject 生命周期。
- STOP：不对 UObject 使用普通 `new/delete`，不自定义 allocator。

### 3.11 GPU、显存与 Shader

当前连接点：

- 项目曾出现 Texture Streaming Pool 超预算。
- 场景存在 VSM 非 Nanite Page Pool 溢出提示。
- 已有受伤后处理、Niagara、Decal 和阴影。

可延伸：

- P0：`stat streaming` 记录 Required/Used/Pool Baseline。
- P0：Size Map 定位大纹理。
- P0：调整 Max Texture Size、LOD Bias、Texture Group 和 Never Stream。
- P0：优化前后记录显存、画质和 Streaming Pool 数据。
- P0：区分 Texture Pool 与 VSM Page Pool，不混成一个问题。
- P0：`stat gpu` 与 ProfileGPU 建立 GPU Baseline。
- P1：Toon Diffuse、硬边 Specular 和 Fresnel Rim Light。
- P1：Custom Depth/Stencil 后处理描边。
- P1：Shader Complexity 和 Quad Overdraw 检查特效成本。
- P1：限制 Decal、Niagara、透明材质和阴影数量。
- P1：比较效果开关前后 GPU 时间。
- P2：Shadow Mapping、PCF、Bias、Shadow Acne 和 Peter Panning。
- P2：延迟渲染、GBuffer、前向渲染和动态光源成本。
- P2：Lumen、Nanite、VSM 的适用边界。
- STOP：当前不改 UE BasePass，不写 RDG/RHI/Global Shader。
- STOP：当前不做完整 PBR、GI、SSR、硬件光追和体积光合集。

## 4. Co-op 可延伸技术点

### 4.1 网络角色与服务端权威

当前连接点：

- NetMode、LocalRole、RemoteRole 和 Authority 调试。
- 客户端拥有的 Character 可以调用 Server RPC。

可延伸：

- P0：双人 Listen Server PIE 验证角色分配。
- P0：所有玩法状态由服务端修改。
- P1：区分 Listen Server、Dedicated Server 和 Standalone。
- P1：记录服务器玩家与远程客户端玩家的角色矩阵。
- P2：Dedicated Server 构建和部署。

### 4.2 属性复制与 RepNotify

当前连接点：

- `ReplicatedUsing` 网络计数。
- `DOREPLIFETIME` 注册。
- 服务端修改，客户端 `OnRep` 更新。

可延伸：

- P0：完成双窗口一致性验证。
- P1：血量、交互状态、钥匙和胜负状态使用 RepNotify。
- P1：使用复制条件减少不必要同步。
- P1：状态与瞬时事件分离，持久状态不用 Multicast 代替。
- P2：Fast Array Serializer 用于动态列表。
- P2：Dormancy 与唤醒。

### 4.3 Server、Client 与 Multicast RPC

当前连接点：

- Server RPC 与 Validation。
- 服务端生成复制 Actor。

可延伸：

- P0：Multicast 同步全局短暂表现。
- P0：Client RPC 向指定拥有者发送结果。
- P0：Ownership 验证和失败案例。
- P0：说明 Reliable 与 Unreliable 的选择。
- P1：RPC 参数范围验证、调用频率限制和状态验证。
- P1：不让客户端直接提交伤害结果，只提交操作意图。
- P2：网络攻击面、作弊和重放攻击基础。

正确选择：

```text
持久状态 -> Replication / RepNotify
客户端意图 -> Server RPC
所有人瞬时表现 -> NetMulticast RPC
指定玩家提示 -> Client RPC
```

### 4.4 Actor Ownership、Relevancy 与生命周期

可延伸：

- P0：解释 PlayerController、Pawn、Owner 和 Owning Connection。
- P0：验证未拥有 Actor 上的 Server RPC 为什么被丢弃。
- P1：设置 Owner 后的 RPC 调用权限。
- P1：NetCullDistanceSquared、NetUpdateFrequency 和 MinNetUpdateFrequency。
- P1：远距离 Actor 降频或停止复制。
- P2：Replication Graph。
- P2：关卡切换和晚加入客户端的 Actor 状态恢复。

### 4.5 Online Subsystem 与 Session

当前连接点：

- GameMode 中已有直接 `ServerTravel(?listen)` 与 IP `ClientTravel` 原型。

可延伸：

- P0：把房间生命周期移入自定义 GameInstance。
- P0：Create、Find、Join、Destroy Session。
- P0：保存并清理异步 DelegateHandle。
- P0：处理重复创建、失败回调和按钮 Busy 状态。
- P0：Null OSS 完成本地验证。
- P1：Steam OSS、Presence 和 Lobby。
- P1：房间名、人数、Ping 和地图信息。
- P1：退出房间后返回菜单。
- P2：邀请、好友列表和平台账号。
- STOP：当前不自建账号、匹配和大厅后端。

### 4.6 Co-op 玩法闭环

可延伸：

- P0：多人连接、协作机关、钥匙、共享胜利。
- P0：交互由客户端请求、服务器验证、状态复制。
- P0：晚加入玩家能读取当前机关和胜负状态。
- P1：多人压力板、搬运物、门和钥匙的 Ownership 规则。
- P1：服务器权威的拾取和重复交互保护。
- P2：断线后状态恢复。
- STOP：不先增加复杂技能、背包和战斗系统。

### 4.7 移动、预测与延迟

可延伸：

- P0：使用网络模拟测试 100 ms、200 ms 和丢包。
- P0：记录移动、交互和状态同步在弱网下是否正确。
- P1：理解 CharacterMovement 自带客户端预测与服务端校正。
- P1：模拟对象使用插值而不是直接跳变。
- P1：记录 Server Correction 和网络平滑表现。
- P2：自定义移动的 SavedMove。
- P2：射击游戏服务器回滚和延迟补偿。
- STOP：当前 Co-op 第一版不实现完整预测回滚。

### 4.8 网络性能与诊断

可延伸：

- P0：使用 `stat net`、Network Insights 和网络模拟命令。
- P0：记录每个 Actor 的更新频率和带宽。
- P1：比较 RepNotify 与重复 Multicast。
- P1：距离相关性、休眠和更新频率优化。
- P1：批量状态使用 Fast Array。
- P2：Replication Graph 支持大量 Actor。
- P2：服务器 CPU 与网络带宽预算。

### 4.9 联机自动化与工程流程

可延伸：

- P0：一键启动 Listen Server 和多个客户端。
- P0：固定的双人冒烟测试清单。
- P1：自动检查创建、搜索、加入、退出和重新创建。
- P1：记录服务端和客户端分离日志。
- P1：打包版本跨进程、跨机器 LAN 测试。
- P2：Dedicated Server 自动构建和部署。

## 5. 两个项目共同的 C++ 延伸点

- P0：对象模型、构造析构、虚函数和多态。
- P0：值、指针、引用、`const` 和生命周期。
- P0：UE GC 与 RAII 的边界。
- P0：`TArray/TMap/TSet` 的复杂度和迭代器失效。
- P0：强引用、弱引用、软引用与循环依赖。
- P0：Timer、Delegate 和异步回调的生命周期。
- P1：Move、完美转发、智能指针和 STL allocator 基础。
- P1：锁、原子、条件变量和 Game Thread 语义。
- P1：缓存局部性、数据布局和批处理。
- P2：Task Graph、AsyncTask 和线程安全的纯数据任务。
- STOP：不为了简历单独写没有实际使用场景的内存池。

## 6. 调试、源码与工具链延伸点

- P0：Visual Studio/UBT/UHT/Live Coding 的区别。
- P0：Editor Target、Game Target 和 Packaging 的区别。
- P0：断点、调用栈、日志类别和断言。
- P0：Unreal Insights CPU Trace。
- P0：ProfileGPU、Shader Complexity、Quad Overdraw。
- P0：Memory Insights、memreport、Size Map 和 Reference Viewer。
- P0：Network Insights 与网络模拟。
- P1：阅读一条 Replication、MoveTo、Session 和 Damage 源码链。
- P1：自动化构建和冒烟测试脚本。
- P2：BuildGraph、CI 和平台打包。

## 7. 热更新延伸点

只作为知识储备，不阻塞当前项目：

- P2：配置热更新与远程参数。
- P2：Pak/IoStore Chunk、Manifest 和差分补丁。
- P2：CDN、断点续传、Hash/签名校验和失败回滚。
- P2：Asset Manager、软引用和异步资源加载。
- P2：脚本层热更新与原生 C++ 更新的边界。
- STOP：当前不实现完整商业热更新系统。

## 8. 定量工程证据

每一个优化至少保留：

```text
问题现象
-> 固定测试条件
-> Baseline
-> 工具定位
-> 修改方案
-> 优化后数据
-> 副作用和取舍
-> 回归结果
```

当前最值得形成的数据：

1. FPS 10/25/50 敌人 Game Thread、总帧时间和响应延迟。
2. FPS AI Timer、错峰和距离分级前后数据。
3. FPS 连续波次前后 Actor/UObject 数量和内存回落。
4. FPS Texture Streaming Pool 优化前后显存与画质。
5. FPS 风格化效果逐项开启后的 GPU 时间。
6. Co-op 100/200 ms 延迟和丢包下的移动、交互与状态一致性。
7. Co-op 不同 NetUpdateFrequency/Relevancy 下的带宽与平滑度。

## 9. 最终优先级

### 当前 P0

```text
Co-op P6-P13 双人 PIE 验证
-> Multicast / Client RPC / Ownership
-> GameInstance + Session 闭环
-> 2 至 4 人联机与弱网测试
-> FPS CPU / 内存 / Texture Pool Baseline
-> 一项 AI 优化数据
-> 一项 GPU / 风格化效果数据
-> Release、README、视频和项目问答
```

### P0 完成后最多选择三个 P1

推荐：

1. FPS AIPerception 或 RVO/Detour Crowd 对比，二选一。
2. FPS Toon + Outline 及 GPU 成本。
3. Co-op Relevancy/NetUpdateFrequency 优化。

### 当前停止项

```text
完整 GAS
复杂行为树 + EQS + Mass 全家桶
完整背包与任务系统
自定义 UE Renderer / RDG / RHI
完整 PBR / GI / SSR / 光追合集
自定义内存分配器
完整商业热更新系统
自建登录、匹配和 Dedicated Server 后端
```

## 10. 面试所有权检查

每个写入简历的关键词都必须能够回答：

1. 它解决了什么真实问题。
2. 为什么选择这个方案。
3. 替代方案是什么，为什么没选。
4. 调用链和对象生命周期是什么。
5. 如何验证正确性。
6. 性能或内存代价是什么。
7. 遇到过什么 Bug，如何定位。
8. 如果重做，会改什么。

无法回答以上问题的技术点，只保留在本文作为学习方向，不写进简历。

## 11. 实际项目落地关注点

教程通常只验证“功能能够运行”，实际项目还要考虑：

```text
职责是否清楚
异常能否恢复
对象能否正确释放
性能是否满足预算
弱网和异步是否可靠
打包后是否仍然成立
其他成员是否能继续维护
```

以下内容是每个功能完成后的验收清单，不代表继续增加功能范围。

### 11.1 架构与职责

1. 一个状态只能有一个权威写入者，避免 C++、蓝图、动画蓝图和 UI 同时修改。
2. 明确职责：

```text
Character：角色输入、移动和角色状态
Weapon：射击、换弹和弹药
HealthComponent：生命值与死亡判定
AIController：AI决策和导航请求
GameMode：单局规则、波次和胜负
GameState：需要共享的对局状态
Widget：只显示和发送用户意图
```

3. 不依赖不同 Actor 的 `BeginPlay` 调用顺序，跨对象依赖应通过显式初始化、接口或 Delegate 建立。
4. 区分配置数据与运行状态。可复用参数放入 DataAsset、DataTable 或蓝图默认值，运行状态留在实例中。
5. 只暴露必要接口，不把所有成员都设成 `BlueprintReadWrite`。
6. 修改 C++ 接口时检查旧蓝图、默认值和序列化资产是否仍然兼容。
7. 可复用业务逻辑不要长期留在关卡蓝图中，否则换地图、多人协作和版本合并都困难。

### 11.2 Gameplay 与状态一致性

1. 输入只是“请求”，不等于动作一定成功。执行前必须检查状态、资源、冷却和目标。
2. Gameplay 状态是权威，动画负责表现和提供时序信号，不能反过来让动画成为唯一状态来源。
3. AnimNotify 可能因 Montage 中断、跳段、低帧率或对象死亡而未按预期触发，必须有结束回调或超时兜底。
4. Timer、Delegate 和异步回调执行时，要重新验证对象是否有效、当前状态是否仍允许操作。
5. 伤害、换弹结算、死亡和胜负判定要具备幂等性，重复调用不能产生二次扣血、重复加弹或重复结算。
6. 时间相关逻辑使用 `DeltaSeconds`、Timer 或明确的固定更新间隔，不依赖机器帧率。
7. 必须覆盖暂停、切换关卡、窗口失焦、角色死亡和重新开始时的状态清理。
8. Shipping 版本不能依赖调试节点、屏幕日志或仅编辑器存在的对象才能正常运行。

### 11.3 射击与战斗

1. 摄像机射线负责瞄准，但还要检查枪口到目标之间是否被近处墙体遮挡。
2. 视觉弹道、枪口方向和权威命中结果必须一致，避免“画面打中但逻辑未命中”。
3. 骨骼名称、碰撞体和 Physical Material 会随资产变化，命中部位逻辑不能依赖未经验证的硬编码。
4. 高频射击、低帧率、快速切换输入和换弹中断下，弹药仍要只结算一次。
5. 近战 Sweep 要验证低帧率和高速动画下是否漏检，并使用本次攻击命中集合避免重复伤害。
6. 明确 `HitReact / Reload / Attack / Dead` 的中断优先级，不让多个 Montage 相互覆盖后留下错误状态。
7. 保存 `Instigator / DamageCauser / DamageType`，便于击杀统计、友军判断、日志和后续网络校验。
8. 音效、Niagara 或贴花创建失败时，不能影响伤害结算主链。

### 11.4 AI 与导航

1. NavMesh 来源是场景碰撞。替换地图、修改碰撞或移动障碍后必须重新检查导航覆盖。
2. 敌人出生点和包围点要投影到 NavMesh，并处理投影失败。
3. `MoveTo` 可能因目标不可达、路径失效或控制器未就绪而失败，需要重试、换点或返回安全状态。
4. 必须测试动态障碍、窄门、楼梯、不同 Agent Radius 和多人拥堵。
5. 三类职责不能混淆：

```text
包围槽位：决定每个敌人应该去哪
NavMesh：计算如何绕过静态障碍到达目标
RVO / Detour Crowd：处理移动过程中的局部避让
```

6. 不要让所有 AI 每帧重新请求路径；只有目标位移超过阈值、路径失效或更新间隔到达时才重算。
7. 根据距离、可见性和战斗状态降低远处 AI 的决策与动画更新频率。
8. 优化后同时记录 CPU 成本和响应延迟，不能只追求更低耗时而让近距离战斗变迟钝。
9. 压力测试要固定地图、出生点、敌人数、运行时长和观察视角，才能比较前后数据。
10. 如果 AI 将来运行在多人服务端，还要额外评估服务器 Game Thread、同步频率和带宽。

### 11.5 对象池、GC 与生命周期

1. 先用数据证明 Spawn/Destroy 或 GC 存在尖峰，再决定是否池化。
2. 对象池会用常驻内存换取更稳定的帧时间，需要预热数量、软上限和硬上限。
3. 对象回池前必须重置：

```text
Timer 与 Delegate
AI目标和状态
动画与 Montage
物理和碰撞
可见性和材质参数
Owner、Instigator 与临时引用
```

4. 池管理器可用强引用持有池内对象；临时目标、敌人占位和异步观察者优先使用弱引用。
5. 池化对象不能继续走 `SetLifeSpan` 或普通 `Destroy` 流程。
6. Niagara 优先使用引擎自带池化，不重复制造一套粒子池。
7. Widget、Decal、Niagara、音频、尸体和武器都要有明确的创建者、失效条件和清理位置。
8. 切换地图或重新开始后，旧 World 不能被 Singleton、Delegate 或异步任务继续引用。
9. `Collect Garbage` 不是常规的泄漏修复手段；应先消除不应存在的强引用和残留回调。

### 11.6 CPU 性能

1. 先用 `stat unit` 判断瓶颈在 Game、Draw 还是 GPU，再选择优化方向。
2. 使用毫秒而不是只看 FPS，因为毫秒可以直接对应帧预算。
3. 除平均值外，至少记录最大值、P95/P99 或尖峰帧，避免平均帧时间掩盖卡顿。
4. 把 Tick 改成 Timer 只代表降低更新频率，不代表自动消除成本；大量 Timer 也需要测量。
5. 错峰更新要验证更新是否真正分散到不同帧，而不是同一时刻集中触发。
6. 性能优化后必须回归正确性、响应延迟、路径行为和动画表现。
7. 工作线程不要直接读写 UObject、Actor 或进行依赖 World 的查询。
8. 多线程只处理纯数据计算，结果回到 Game Thread 后再操作 UE 对象。
9. Debug Draw、频繁日志和 Trace 应有开关，Shipping 版本不能保留高频调试成本。

### 11.7 GPU、Shader 与显存

1. 先确定目标硬件、分辨率、画质档和 GPU 帧预算。
2. 前后对比必须固定分辨率、Scalability、视角、场景和采样时间。
3. Shader 不只看节点数量，还要看指令数、纹理采样、动态分支和 Permutation 数量。
4. 半透明 Niagara、Decal、后处理和大面积特效要重点检查 Overdraw。
5. 描边要测试动态物体、遮挡、远距离、抗锯齿和不同分辨率下的稳定性。
6. Toon 色阶和高光要检查 TAA/TSR 下的闪烁与时间稳定性。
7. 优先使用 Material Instance 复用 Master Material，避免大量相似母材质增加编译和维护成本。
8. Texture Streaming Pool 溢出不能只靠扩大 Pool 解决，还要查：

```text
Required Pool
Max Texture Size
LOD Bias
Texture Group
压缩格式
Never Stream
重复或未使用纹理
```

9. 修改纹理规格后必须检查近景质量、UI 清晰度和镜头切换时的流送抖动。
10. 区分 Texture Pool、VSM Page Pool、Nanite Streaming 等不同预算，不能把所有显存警告当成同一问题。
11. 为低端设备准备 Scalability 或关闭高成本效果的降级路径。

### 11.8 网络权威与安全

1. 客户端只提交操作意图，服务端负责验证并修改权威状态。
2. 不直接相信客户端传来的伤害、位置、弹药、资源数量或胜利结果。
3. Server RPC 至少校验：

```text
调用者是否拥有该对象
目标是否有效且在合理范围
调用频率是否超过限制
当前状态和资源是否允许
```

4. 高频且允许丢失的表现消息可用 Unreliable；低频且必须到达的关键请求才用 Reliable。
5. 过多 Reliable RPC 会造成队头阻塞，不能把“可靠”理解为“没有代价”。
6. 持久状态使用 Replication/RepNotify；瞬时、非关键表现才考虑 Multicast。
7. Multicast 不会自动恢复晚加入客户端的历史状态，晚加入依赖当前权威属性同步。
8. Client RPC 只能发给拥有该 Actor 的客户端，调用前要确认 Ownership 链。
9. 重复 RPC、重传或延迟到达时，关键操作要用状态检查、序列号或幂等设计避免重复执行。
10. 服务端日志应能关联玩家、连接、RPC、目标对象和状态变化。

### 11.9 Session、OSS 与跨平台

1. Create、Find、Join、Destroy Session 都是异步操作，完成前 UI 按钮要进入 Busy 状态。
2. 保存每次绑定返回的 `DelegateHandle`，成功、失败、取消和 Shutdown 时都要解绑。
3. 已存在 Session 时先 Destroy，等待完成回调后再 Create，不能连续同步调用。
4. 搜索结果可能过期，Join 失败要显示原因并允许重新搜索。
5. Join 成功后通过 OSS 解析连接地址，不硬编码公网地址。
6. Session 生命周期放在 GameInstance 或专用 Subsystem；GameMode 只存在于服务端当前关卡。
7. Null、Steam 和其他平台 OSS 的行为不同，不能只验证其中一种。
8. PIE、Standalone、Packaged、LAN 和真实互联网环境应分别验证。
9. 本机多窗口成功不能证明 NAT、账号、邀请、断线重连和跨网络连接已完成。
10. 客户端与服务端版本不兼容时，应拒绝连接或执行明确兼容策略。

### 11.10 弱网与同步体验

1. 分别测试延迟、抖动、丢包和带宽限制，不能只测固定延迟。
2. 同时检查“状态是否正确”和“画面是否平滑”，二者不是同一问题。
3. 角色移动、物理物体、交互、战斗和 UI 状态需要不同同步策略。
4. 能区分：

```text
插值：平滑显示已收到的状态
预测：本地提前执行，之后接受服务端校正
回滚：恢复历史状态并重新模拟
```

5. 观察校正时的瞬移、抖动、穿透和输入延迟。
6. 降低 NetUpdateFrequency 能节省带宽，但会增加表现延迟，需要记录取舍数据。
7. 晚加入客户端依靠复制后的当前状态恢复场景，不能依赖过去已经发完的 Multicast。
8. Travel、断线和重新连接过程中，异步回调必须重新验证 World、PlayerController 和 Session。

### 11.11 UI 与用户体验

1. UI 初始化不能假设 Pawn、PlayerState 或网络属性已经就绪。
2. 创建 UI 时先主动读取一次初始值，再绑定 Delegate 接收后续变化。
3. 打开和关闭界面时成对设置 Input Mode、鼠标光标和 Focus。
4. 防止 Widget 重建后重复绑定 Delegate，销毁或移除时要解绑。
5. `RemoveFromParent` 只移出视口，不等于立即 GC；还要清除外部强引用和回调。
6. 验证不同 DPI、宽高比、安全区、键鼠和手柄输入。
7. Session 等异步操作要显示 Busy、成功和失败反馈，避免用户重复点击。
8. UI 不负责权威 Gameplay 计算，只显示状态和提交请求。

### 11.12 资源、打包与发布

1. 编辑器中能运行，不代表 Development 或 Shipping 包可运行。
2. 动态加载资产需要 Soft Reference、Asset Manager 或明确 Cook 规则，避免打包后丢失。
3. 定期检查 Redirector、Missing Reference、路径大小写和未使用资源。
4. 大型二进制资产使用 Git LFS；不提交 Binaries、Intermediate、Saved 和 DDC。
5. Release 冒烟测试至少覆盖：

```text
启动
进入关卡
完整战斗
胜负与重启
退出
多人创建、搜索、加入和离开
```

6. SaveGame、配置格式和网络协议都应有版本概念。
7. Packaged Build 要能保留必要日志，便于定位启动、崩溃、网络和性能问题。
8. 热更新方案必须考虑清单、校验、回滚、版本兼容和失败恢复，不能只证明文件可以下载。

### 11.13 团队协作与可维护性

1. 一个提交只解决一个问题，避免同时混入代码、资产迁移和大规模格式化。
2. 提交说明写清“改了什么、为什么改”，不要只写 `update` 或 `fix`。
3. 蓝图使用稳定命名、注释分组和清晰执行流，避免超长连线与隐藏副作用。
4. 修改公共接口后同步检查调用方、文档和回归测试。
5. 蓝图和地图是二进制资产，难以自动合并，团队中要减少多人同时编辑。
6. README 要记录构建环境、启动方式、测试命令、已知限制和复现步骤。
7. 仓库不能包含账号密钥、Token、个人绝对路径和仅本机存在的调试资产。
8. 教程或 AI 辅助生成的代码必须经过逐行审查、运行验证和项目化修改，确保能够独立解释。

### 11.14 每项功能的实际验收问题

完成一个功能后，至少回答：

1. 正常流程是否完整。
2. 重复调用是否安全。
3. 对象中途销毁后回调是否安全。
4. 暂停、重启或切换关卡后是否残留状态。
5. 低帧率和大量对象下是否仍然正确。
6. 弱网、晚加入或断线时是否仍然一致。
7. 资产缺失、导航失败或异步失败时如何恢复。
8. Development 与 Shipping 行为是否一致。
9. 能否通过日志、断点、Insights 或网络模拟定位问题。
10. 出现线上问题时是否有关闭、降级或回滚方案。
