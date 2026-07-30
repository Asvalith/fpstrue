# UE5 游戏框架与技术路线复习文档

最后更新：2026-07-22

> 当前执行基线：`FPS_PORTFOLIO_ACCEPTANCE_PLAN.md`。
>
> 2026-07-22 已冻结本轮范围：只完成单机 FPS 闭环、战斗与动画一致性、敌人 AI、内存/生命周期治理、性能分析和图形表现。本文后半部分的多人网络、额外图形项目、行为树、Global Shader / RDG 等内容仅保留为知识索引，不属于当前实现承诺。

## 1. 文档定位

本文档用于复习 UE5 Gameplay Framework、梳理 `fpstrue` 当前架构，并为当前 FPS 实现提供背景知识。实际范围、顺序和验收要求以 `FPS_PORTFOLIO_ACCEPTANCE_PLAN.md` 为准。

核心原则：

- 只把能够解释、修改和验证的内容写成个人能力。
- 借鉴代码可以作为学习起点，但必须重新梳理调用链并完成独立修改。
- 优先完成一个可玩的项目，再增加 AI、生命周期、性能和图形表现深度。
- 不为了堆技术关键词同时开启过多系统。
- C++ 负责规则和状态，蓝图负责资源配置与表现。

## 2. 岗位目标

主投方向：

1. 游戏引擎开发。
2. 图形渲染开发。
3. UE5 C++ 客户端开发。

保底方向：

1. 游戏客户端开发。
2. 客户端渲染与性能方向。

需要形成的能力组合：

```text
C++ 与数据结构
+ UE Gameplay Framework
+ 游戏系统与状态管理
+ 图形学与 Shader 基础
+ 调试和性能分析
+ 可解释的项目证据
```

## 3. 项目路线总览

| 项目 | 定位 | 当前状态 | 推进顺序 |
| --- | --- | --- | --- |
| UE5 C++ FPS | 主项目，证明商业引擎和客户端能力 | 单机战斗原型 | 第一优先 |
| UE5 多人网络 FPS | FPS 的第二阶段 | 尚未开始 | 单机闭环后 |
| A* / FSM / Behavior Tree 可视化 | 游戏 AI 与算法项目 | 尚未开始 | FPS 第一版后 |
| 通用对象池 | 可视化项目内部基础模块 | 尚未开始 | 有真实使用场景后 |
| OpenGL Shadow Mapping + PCF | 第一个独立图形实验 | 尚未独立重写 | 图形路线第一步 |
| RTIOW 重构与进阶 | CPU 光线追踪学习项目 | 现有代码以参考和借鉴为主 | 从基础重新实现 |
| UE Global Shader / RDG 插件 | UE 渲染进阶项目 | 尚未开始 | OpenGL/Shader 基础后 |

项目数量不等于能力。每个项目只有通过“理解、修改、调试、测量、讲解”后才进入正式作品集。

## 4. UE Gameplay Framework 知识地图

### 4.1 核心对象层级

```text
UObject
└─ AActor
   ├─ APawn
   │  └─ ACharacter
   ├─ AController
   │  ├─ APlayerController
   │  └─ AAIController
   ├─ AGameModeBase
   ├─ AGameStateBase
   └─ APlayerState

UActorComponent
├─ USceneComponent
│  ├─ UPrimitiveComponent
│  ├─ USkeletalMeshComponent
│  └─ UCameraComponent
└─ 自定义 Gameplay Component
```

### 4.2 各框架类的职责

| 类型 | 主要职责 | 当前项目使用情况 |
| --- | --- | --- |
| GameInstance | 跨关卡生命周期、全局数据 | 尚未使用 |
| GameMode | 服务器或单机规则、胜负、生成 | 目前只设置 DefaultPawn |
| GameState | 可复制的比赛状态 | 尚未使用 |
| PlayerController | 玩家输入意图、UI 输入模式、控制 Pawn | 主要使用默认实现 |
| PlayerState | 玩家分数、队伍、可复制玩家数据 | 尚未使用 |
| Pawn/Character | 玩家或 AI 在世界中的实体 | 已使用 Character |
| AIController | AI 决策和导航控制 | 尚未接入 |
| ActorComponent | 可复用 Gameplay 能力 | 已用于 HealthComponent |
| SceneComponent | 有 Transform 的组件 | 武器组件继承 SkeletalMeshComponent |

### 4.3 生命周期复习

```text
Constructor
→ OnConstruction
→ PreInitializeComponents
→ PostInitializeComponents
→ BeginPlay
→ Tick / Timer / Delegate
→ EndPlay
→ UObject GC
```

复习重点：

- Constructor 只做默认值和默认子对象创建。
- BeginPlay 适合绑定运行时 Delegate、寻找场景对象和初始化状态。
- Timer 是延迟执行，死亡和 EndPlay 时要考虑清理。
- Actor 销毁不等于普通 C++ `delete`，应理解 UObject 与 GC 生命周期。
- `SetLifeSpan` 是 Actor 延迟销毁机制，不是对象池。

## 5. 当前 FPS 代码架构

当前 C++ 模块约 1971 个物理行：

| 模块 | 物理行数 | 主要职责 |
| --- | ---: | --- |
| Character | 886 | 输入、角色状态、弹药、换弹、死亡 |
| WeaponComponent | 389 | 开火、LineTrace、伤害、武器事件 |
| EnemyCharacter | 303 | 原型追逐、攻击 Timer、死亡 |
| HealthComponent | 102 | 通用血量、受伤、死亡事件 |
| 其他模块 | 291 | 拾取、靶子、Projectile、GameMode |

### 5.1 类职责

#### `AfpstrueCharacter`

负责：

- Enhanced Input 绑定。
- 移动、观察、瞄准和冲刺。
- `Idle / Moving / Reloading / Dead` 状态。
- 当前弹药、备用弹药和换弹。
- 玩家血量变化和死亡响应。
- 保存当前装备的 WeaponComponent 引用。

当前风险：

- 文件较大，调试输出和 Gameplay 状态集中。
- 不应在尚未完全理解前进行大规模重构。
- HUD 接入后应使用事件更新，不要继续增加每帧查询。

#### `UfpstrueWeaponComponent`

负责：

- 接收开火输入生命周期。
- 一次射击尝试。
- Camera LineTrace。
- 散布、后坐力、身体伤害和头部伤害。
- 命中后 `ApplyPointDamage`。
- 命中物理对象后的冲量。
- 向蓝图广播射击、换弹和命中结果。

边界：

- WeaponComponent 不拥有玩家最终生命状态。
- 蓝图表现不能反向决定是否造成伤害。
- 视觉子弹不负责真实命中。

#### `UfpstrueHealthComponent`

负责：

- 保存 `MaxHealth` 和 `CurrentHealth`。
- 监听 Owner 的 `OnTakeAnyDamage`。
- 统一处理 Clamp 扣血。
- 广播 `OnHealthChanged` 和 `OnDeath`。

设计价值：

- 玩家、敌人和 TargetDummy 复用同一生命系统。
- UI、死亡和未来网络同步可以监听统一事件。
- 不需要 Tick。

#### `AfpstrueEnemyCharacter`

当前负责：

- 寻找第 0 个玩家角色。
- Tick 中计算玩家距离。
- 使用 `AddMovementInput` 直线追逐。
- 使用 Timer 实现攻击前摇和攻击结束。
- 玩家进入范围后 `ApplyDamage`。
- 死亡后清理 Timer、停止移动并关闭碰撞。

当前限制：

- 不是正式 NavMesh 寻路。
- 没有 AIController。
- 没有显式完整 FSM。
- `GetPlayerCharacter(0)` 不适合直接扩展为多人逻辑。

#### `UfpstruePickUpComponent`

负责：

- 监听 Sphere Overlap。
- 确认 OtherActor 是玩家 Character。
- 广播 `OnPickUp`。
- 防止重复拾取。

#### `AfpstrueGameMode`

当前只负责设置默认 Pawn。

单机闭环阶段需要增加：

- Match State。
- 敌人总数和剩余敌人数。
- 玩家死亡与敌人死亡监听。
- 胜利、失败和重新开始。

## 6. 核心调用链

### 6.1 输入调用链

```text
Input Mapping Context
→ Enhanced Input Action
→ SetupPlayerInputComponent
→ Character 输入函数
→ 修改移动或 Gameplay 状态
```

排查顺序：

```text
Mapping Context 是否添加
→ Input Action 是否存在
→ 蓝图属性是否赋值
→ EnhancedInputComponent 是否有效
→ BindAction Trigger 是否正确
→ 目标函数是否执行
```

### 6.2 射击调用链

```text
Fire Input
→ WeaponComponent::StartFire
→ 单次 WeaponComponent::Fire
→ Character::TryConsumeAmmo
→ WeaponComponent::FireLineTrace
→ FHitResult
→ ApplyPointDamage
→ HealthComponent
→ HealthChanged / Death
```

表现分支：

```text
Fire / Trace 结果
→ Dynamic Multicast Delegate
→ BP_Weapon
→ 动画、枪口表现、贴花、曳光弹
```

### 6.3 换弹调用链

```text
Reload Input
→ Character::StartReload
→ CanReload
→ 退出瞄准并停止开火
→ CharacterState = Reloading
→ OnWeaponReloadStarted
→ Reload Timer
→ FinishReload
→ 更新 CurrentAmmo / ReserveAmmo
→ OnWeaponReloadFinished
```

核心约束：

- 死亡不能换弹。
- 换弹中不能重复换弹。
- 满弹匣不能换弹。
- 没有备用弹药不能换弹。
- 换弹期间不能开火。

### 6.4 伤害与死亡调用链

```text
ApplyPointDamage / ApplyDamage
→ Owner::OnTakeAnyDamage
→ HealthComponent::HandleOwnerTakeAnyDamage
→ HealthComponent::ApplyDamage
→ OnHealthChanged
→ CurrentHealth <= 0
→ OnDeath
→ Character / Enemy HandleDeath
```

死亡是系统生命周期边界：

```text
停止开火
→ 清理换弹或攻击 Timer
→ 退出瞄准与冲刺
→ 停止移动
→ 关闭必要碰撞
→ 通知 UI 或 GameMode
```

### 6.5 敌人攻击调用链

```text
Enemy Tick
→ UpdateEnemy
→ 距离检查
→ TryAttackTarget
→ OnAttackStarted
→ AttackDamage Timer
→ 再次确认距离和存活状态
→ ApplyDamage(Player)
→ OnAttackLanded
→ AttackFinish Timer
```

### 6.6 拾取调用链

```text
Sphere Overlap
→ Cast Player Character
→ OnPickUp.Broadcast
→ 武器蓝图处理装备表现
→ Character 保存 WeaponComponent
→ Fire Mapping Context 生效
```

## 7. C++ 与蓝图接口设计

### 7.1 接口类型

当前项目中的“接口”主要包括：

1. Dynamic Multicast Delegate。
2. BlueprintImplementableEvent。
3. BlueprintCallable 命令。
4. BlueprintPure 查询。

这些不完全等于 UE 的 `UInterface`。面试时应称为“蓝图扩展点、事件边界和对外 API”。

### 7.2 当前事件地图

```text
HealthComponent
├─ OnHealthChanged
└─ OnDeath

WeaponComponent
├─ OnWeaponFireStarted
├─ OnWeaponFireStopped
├─ OnWeaponFirePerformed
├─ OnWeaponDryFire
├─ OnWeaponReloadStarted
├─ OnWeaponReloadFinished
└─ OnWeaponTraceFinished

Character
├─ OnAimChanged
├─ OnFireStarted
├─ OnFireStopped
├─ OnPlayerHealthChanged
└─ OnPlayerDied

EnemyCharacter
├─ OnAttackStarted
└─ OnAttackLanded

PickUpComponent
└─ OnPickUp
```

### 7.3 设计原则

```text
C++ 计算结果
→ 广播事件
→ 蓝图消费结果
→ 播放表现
```

正确示例：

- C++ 判断能否射击，蓝图播放枪口表现。
- C++ 计算伤害，蓝图显示血条和受伤反馈。
- C++ 决定死亡，蓝图播放死亡表现。

错误示例：

- 蓝图动画结束后才决定是否扣弹药。
- 粒子是否成功生成决定是否造成伤害。
- UI 每帧反向修改 Character 状态。

### 7.4 接口技术债

- Character 与 WeaponComponent 都存在 Fire Started/Stopped，可能产生重复事件源。
- `OnWeaponTraceFinished` 参数较多，继续扩展时应考虑 `FWeaponHitData`。
- 必须确认每个预留事件是否存在实际蓝图消费者。
- HUD 阶段应新增 `OnAmmoChanged`。
- Match 闭环阶段应新增 `OnMatchStateChanged`。

## 8. 单机 FPS 第一阶段

### 8.1 必须完成

```text
移动、瞄准、射击、换弹
→ 玩家和敌人受伤、死亡
→ 3～5 个敌人
→ HUD
→ 全部敌人死亡则胜利
→ 玩家死亡则失败
→ 重新开始
```

HUD 只使用 UMG 基础控件：

- Crosshair。
- Health ProgressBar。
- Current Ammo / Reserve Ammo Text。
- Remaining Enemy Text。
- Win / Lose Panel。
- Restart Button。

### 8.2 不依赖外部素材的表现

- 使用 Engine Basic Shapes 补充场景或敌人表示。
- 使用 UMG Text、Border、ProgressBar，不依赖 UI 图片包。
- 使用程序化材质颜色表示敌人状态。
- 使用 Post Process Material 实现受伤暗角和低血量去饱和。
- 使用 CustomDepth/Stencil 实现简单描边。
- 不把音效、复杂粒子、外部动画作为验收条件。

### 8.3 当前不加入

- 多人网络。
- GAS。
- Behavior Tree。
- 通用对象池。
- 多武器系统。
- 波次和复杂生成系统。
- UE Global Shader。
- 自定义 Renderer。

## 9. 多人网络第二阶段

单机闭环稳定后，再将 FPS 升级为最小多人网络项目。

### 9.1 必学概念

- Authority。
- Ownership。
- Role 与本地控制判断。
- Server RPC。
- Client RPC。
- NetMulticast RPC。
- Replicated Property。
- RepNotify。
- Character Movement Replication。
- 服务端权威伤害。

### 9.2 当前代码需要调整的位置

```text
CurrentAmmo / ReserveAmmo
→ 需要决定复制方式

CharacterState / Health / Death
→ 需要服务端权威并复制结果

WeaponComponent::FireLineTrace
→ 不能直接由客户端决定伤害

Enemy GetPlayerCharacter(0)
→ 不能作为多人目标选择方案

蓝图连续开火 Timer
→ 需要明确客户端输入与服务端射速验证
```

### 9.3 最小网络验收

- 两个客户端可以进入同一关卡。
- 移动正常复制。
- 客户端发送开火请求，服务端验证并判定伤害。
- Health 与 Death 同步。
- 弹药和换弹结果同步。
- 不做大厅、匹配、Steam、反作弊和延迟补偿。

## 10. 游戏 AI 与系统可视化路线

将以下内容合并为一个独立项目：

```text
A* 寻路
+ FSM
+ Behavior Tree
+ Object Pool
```

### 10.1 A* 可视化

- 可编辑网格与障碍。
- 显示 Open Set、Closed Set 和最终路径。
- 优先队列实现 Open Set。
- 支持 Manhattan 与 Euclidean 启发函数。
- 比较 Dijkstra 与 A* 扩展节点数量。

### 10.2 FSM

```text
Idle
→ Patrol
→ Chase
→ Attack
→ Return
```

复习重点：

- 状态进入、更新和退出。
- 状态切换条件。
- 防止同一帧重复切换。
- FSM 与 Agent 数据如何解耦。

### 10.3 Behavior Tree

在 FSM 完成后加入：

- Selector。
- Sequence。
- Condition。
- Action。
- Running / Success / Failure。

不要一开始就同时实现 FSM 和 Behavior Tree。

### 10.4 Object Pool

对象池不单独作为项目。只有在可视化节点、临时标记或 Agent 存在大量重复创建时加入。

必须记录：

- 分配次数。
- 峰值对象数量。
- Pool 命中率。
- 使用前后耗时。
- Reset 和生命周期规则。

## 11. 图形学路线

### 11.1 OpenGL Shadow Mapping + PCF

这是第一个应该独立重写的 GPU 图形实验。

```text
场景几何
→ Light View / Projection
→ Shadow Depth Pass
→ Depth Texture
→ Camera Lighting Pass
→ Light Space Depth Compare
→ Bias
→ 3x3 PCF
```

必须实现：

- 一个平面和多个 Cube。
- Shadow Map 深度纹理。
- 深度图调试视图。
- 固定 Bias。
- 基于法线的 Bias。
- 无 PCF 与 3x3 PCF 对比。

必须解释：

- 世界空间到光源裁剪空间的变换。
- Shadow Acne。
- Peter Panning。
- Shadow Map 分辨率。
- PCF 采样数量与开销。

### 11.2 RTIOW 重构路线

现有 Path Tracer 代码以参考和借鉴为主，先作为学习资料保留，不直接视为已掌握项目。

重新实现顺序：

```text
Vec3
→ Ray
→ Sphere Intersection
→ Hittable
→ Camera
→ Lambertian
→ Metal
→ Dielectric
→ AABB
→ BVH
→ Multithreading
```

进阶内容必须放在基础重写之后：

- 面积光源采样。
- PDF 重要性采样。
- Russian Roulette。
- SAH BVH。
- 多线程性能对比。

### 11.3 UE Global Shader / RDG 插件

只有在能够解释 OpenGL Shader、Framebuffer、Texture 和 Pass 后再开始。

最小项目：

```text
UE Plugin
→ Global Shader 注册
→ .usf Shader
→ RDG Compute Pass
→ Dispatch
→ 输出 Render Target
→ 材质或 UMG 显示
```

效果可以很简单：

- 灰度转换。
- Sobel 边缘检测。
- 简单模糊。
- 程序化噪声。
- Game of Life。

重点是能够解释：

- Shader 参数绑定。
- Game Thread 与 Render Thread。
- RDG Resource 生命周期。
- Compute Thread Group。
- UAV、SRV 和 Render Target。

## 12. 闭卷重写路线

闭卷重写不等于背诵 UE API。可以查官方文档和编译错误，但不能复制原函数实现。

### 12.1 第一轮：HealthComponent

目标：

```text
保存血量
→ 监听伤害
→ Clamp
→ 广播血量变化
→ 广播死亡
```

验收：20～60 分钟内独立实现核心版本。

### 12.2 第二轮：弹药与换弹

独立实现：

- `CanReload()`。
- `TryConsumeAmmo()`。
- `StartReload()`。
- `FinishReload()`。

验收：能够覆盖死亡、换弹中、满弹匣和无备用弹药。

### 12.3 第三轮：Hitscan

独立实现：

```text
Camera Start / Direction
→ Trace End
→ Ignore Owner
→ LineTrace
→ FHitResult
→ Point Damage
```

验收：调整伤害、射程和碰撞通道后仍能说明结果。

### 12.4 第四轮：生命周期清理

独立实现玩家和敌人死亡：

- 清理 Timer。
- 停止移动。
- 禁止射击。
- 禁用必要碰撞。
- 防止重复死亡。

### 12.5 重写方法

```text
阅读一个模块
→ 关闭原文件
→ 中文写输入/输出/边界条件
→ 写伪代码
→ 独立实现
→ 编译修正
→ 与原代码对比
→ 记录遗漏
→ 24 小时后再次实现
```

不要在主分支删除原代码。学习重写应使用独立分支或独立练习目录。

## 13. 项目掌握标准

一个功能只有同时满足以下条件，才可以写进简历：

- 能画出调用链。
- 能指出数据所有者。
- 能指出触发者和消费者。
- 能解释生命周期和清理位置。
- 能说明为什么这样设计。
- 能独立修改一个需求。
- 能讲一个真实 Bug。
- 能完成运行验证。

不要求记住：

- 每个 include。
- UE API 的全部参数顺序。
- Delegate 宏的完整拼写。
- 模板生成代码。

必须真正掌握：

- 规则放在哪里。
- 状态由谁维护。
- 事件在哪里广播。
- 蓝图在哪里消费。
- Timer 在哪里结束。
- Actor 死亡后哪些系统必须停止。

## 14. 游戏框架复习问题

### UObject 与 Actor

1. UObject 和普通 C++ 对象有什么不同？
2. Actor 与 Component 的职责边界是什么？
3. 为什么不能直接 `delete Actor`？
4. Constructor、BeginPlay 和 Tick 分别适合做什么？
5. `UPROPERTY` 与 GC 有什么关系？

### Gameplay Framework

6. Pawn、Character、Controller 的关系是什么？
7. PlayerController 为什么不应该保存所有角色状态？
8. GameMode 和 GameState 有什么区别？
9. PlayerState 适合保存什么？
10. GameInstance 的生命周期是什么？

### Component 与事件

11. Health 为什么适合做 ActorComponent？
12. Delegate 和直接函数调用有什么区别？
13. BlueprintImplementableEvent 的用途是什么？
14. BlueprintCallable 和 BlueprintPure 有什么区别？
15. 什么时候应该使用真正的 `UInterface`？

### 输入与状态

16. Enhanced Input 的 Mapping Context、Input Action 和 Trigger 是什么？
17. 为什么换弹和射击需要统一状态检查？
18. Timer 与 Tick 应该如何选择？
19. 为什么死亡时要清理 Timer？
20. 多个布尔状态会产生什么问题？

### 射击与伤害

21. Hitscan 和 Projectile 有什么区别？
22. 为什么从 Camera 做 LineTrace，而视觉子弹从 Muzzle 生成？
23. `ApplyDamage` 与 `ApplyPointDamage` 有什么区别？
24. `FHitResult` 包含哪些重要信息？
25. 头部伤害如何判断？

### 网络预备

26. Authority 和 Ownership 有什么区别？
27. 为什么不能让客户端直接决定伤害？
28. Server RPC 和 Multicast 分别解决什么问题？
29. RepNotify 适合更新什么状态？
30. 当前 `FireLineTrace` 如果网络化，应如何拆分？

### 大厂关注：架构与工程所有权

> 本节标记为“大厂关注”，不是因为问题一定以原句出现，而是因为大厂面试通常会沿着数据所有权、模块职责、方案取舍和实际验证继续追问。

31. 当前项目中 Character、WeaponComponent、HealthComponent、EnemyCharacter、AIController 和 GameMode 分别拥有什么数据？
32. 为什么生命值和武器逻辑适合拆成 Component，而不是全部放进 Character？
33. C++ 与蓝图的职责边界是什么？为什么动画和 UI 表现可以保留在蓝图中？
34. Delegate、直接函数调用和 Tick 查询各适合什么场景？
35. UI 为什么应监听状态变化事件，而不是每帧读取 Character？
36. 当前哪些蓝图节点只是表现层，哪些节点仍然承担了不该承担的规则？
37. 如果需求改为增加第二种武器，当前架构需要修改哪些类？
38. 如果重新实现一次，哪些教程或模板逻辑应该保留，哪些应该替换？
39. 如何证明一个模块已经由自己掌握，而不只是能够运行？
40. 如何用调用链、独立修改、Bug 复盘和测试结果证明代码所有权？

### 大厂关注：CPU、动画与 AI 性能

41. 为什么必须先建立 Baseline，再声称完成了优化？
42. `stat unit` 和 Unreal Insights 分别解决什么问题？
43. 10、25、50、100 个敌人的固定压力测试需要控制哪些变量？
44. AI 每帧决策、固定时间间隔决策和错峰更新的成本与响应延迟有什么区别？
45. 为什么不能为了省 CPU 简单关闭所有敌人的 Tick？
46. 为什么近距离战斗敌人需要更高更新频率，远距离敌人可以降频？
47. Animation Update Rate Optimization、Visibility Based Anim Tick 和 LOD 分别减少什么成本？
48. 敌人死亡后应停止哪些移动、动画、碰撞、Timer 和 AI 更新？
49. 为什么先做距离分级和更新降频，而不是直接把 AI 放到多线程？
50. 优化方案自身的调度、分支和缓存开销如何测量？

### 大厂关注：内存、资源与生命周期

51. UObject、Actor、Component、强引用、弱引用和 GC 之间是什么关系？
52. Timer、Delegate、AI 目标、Widget、Niagara、Decal 和尸体分别在哪里创建与清理？
53. 如何通过 `stat memory`、`memreport`、Actor/Object 数量和连续波次实验判断是否泄漏？
54. 为什么内存没有立即下降不一定代表泄漏？
55. Texture Streaming Pool 超预算代表什么？如何使用 `stat streaming` 和 Size Map 定位资源？
56. Max Texture Size、LOD Bias、Texture Group 和 Never Stream 分别会怎样影响显存与画质？
57. 什么情况下对象池能减少 Spawn/Destroy 尖峰？什么情况下会增加状态残留风险？
58. 当前 Hitscan 项目为什么不需要“子弹对象池”？
59. 为什么对象池必须由性能数据驱动，而不能仅作为简历关键词？
60. 如何验证关卡重启和多轮战斗后没有残留对象或重复绑定？

### 大厂关注：多人网络

61. NetMode、Role、Authority 和 Ownership 各自描述什么？
62. 为什么伤害、拾取和胜负判定应该由服务端保持权威？
63. Server RPC、Client RPC、Multicast RPC 和 RepNotify 的选型依据是什么？
64. Reliable RPC 为什么不能滥用？高频状态为什么更适合属性同步？
65. Actor Ownership 如何决定客户端能否调用 Server RPC？
66. 房间创建、搜索、加入、销毁为什么适合放在 GameInstance 或其子系统中？
67. 如何在 100ms、200ms 延迟及丢包环境下验证交互没有重复结算或状态分叉？
68. NetUpdateFrequency、Dormancy 和 Relevancy 分别怎样影响带宽和响应速度？
69. Listen Server 与 Dedicated Server 的架构和部署成本有什么区别？
70. 当前 Co-op 为什么先完成服务端权威、RPC 和 Session，再决定是否实现延迟补偿？

#### 多人拾取与第一/第三人称显示追问记录

> 状态：多人设计方案，尚未在 `fpstrue` 中实现，简历中不能写成已完成功能。

单机版本可以直接使用 `OnPickUp` 事件提供的 `PickUpCharacter`。多人版本不能使用 `GetPlayerPawn(0)` 代替它，因为每台机器上的索引 0 通常只代表该机器的本地玩家，不一定是实际拾取者。

推荐的服务端权威链路：

```text
客户端按下交互键
→ ServerTryPickup(PickupActor)
→ 服务器验证 RPC 调用者拥有该 Character
→ 服务器验证距离、碰撞、武器是否仍可拾取
→ 服务器检查 Character 当前是否已有武器
→ 第一个合法请求设置 bPickedUp / EquippedWeapon
→ 服务器设置武器 Owner 并完成装备
→ EquippedWeapon 通过 RepNotify 复制
→ 各客户端在 OnRep_EquippedWeapon 中更新附件和表现
```

两个玩家同时拾取同一武器时：

```text
服务器串行处理请求
→ 第一个合法请求把 bPickedUp 设为 true
→ 第二个请求再次验证时失败
→ 避免两个客户端各自认为拾取成功
```

第一人称与第三人称显示应拆开：

- 本地拥有者：显示 `Mesh1P` 和第一人称武器。
- 其他客户端：显示第三人称角色 Mesh 和第三人称武器。
- 第一人称组件使用 `Only Owner See`。
- 第三人称组件使用 `Owner No See`。
- `OnRep_EquippedWeapon` 负责持久装备状态；一次性的拾取音效或粒子可以使用本地表现或受控 Multicast。
- UI 只在拥有该 Character 的客户端监听装备/弹药变化。

多人拾取的关键取舍：

- 持久状态优先使用复制属性和 RepNotify，不依赖一次性 Multicast。
- 客户端只提交拾取意图，不能直接决定武器归属。
- 服务器必须验证距离和可用状态，不能信任客户端传入结果。
- 服务器 Attach 的复制武器 Actor 可以同步附件关系；第一人称专用 Mesh 仍由拥有者本地处理。
- 玩家死亡、断线、丢弃武器和重生时必须明确清理 Owner、附件和装备引用。

面试口述主线：

> 单机版直接消费重叠事件中的 `PickUpCharacter`。多人化后，我不会用 `GetPlayerPawn(0)`，而是让拥有该 Character 的客户端发送 Server RPC，由服务器验证距离和武器状态并决定归属。装备引用使用 RepNotify 复制，拥有者显示第一人称手臂和武器，其他客户端显示第三人称模型。两个玩家同时拾取时，由服务器上的 `bPickedUp` 状态保证只有第一个合法请求成功。

### 大厂关注：图形学与 GPU

71. 一个模型从局部空间到最终屏幕像素经历哪些坐标变换和管线阶段？
72. Lambert、Blinn-Phong 和 PBR 分别解决什么问题？
73. 前向渲染和延迟渲染在光源数量、透明物体、带宽和 MSAA 上有什么取舍？
74. Shadow Mapping 的基本原理是什么？Shadow Acne 和 Peter Panning 如何产生？
75. Toon Diffuse、硬边高光、Rim Light 和描边分别应放在什么阶段实现？
76. 后处理材质为什么不会修改 Gameplay 状态和网络同步结果？
77. `stat gpu`、ProfileGPU、Shader Complexity、Quad Overdraw 和 Material Stats 各自看什么？
78. 如何按照 Baseline、单项效果开启、组合效果开启的顺序测量 GPU 成本？
79. 为什么只调整材质参数不能宣称自己实现了 Shader？
80. 如何用源码、材质图、自定义 HLSL、截图和 GPU 数据证明图形效果的实现边界？

### 大厂关注：C++、源码与 AI 辅助开发

81. C++ 对象模型、虚函数、内存布局、RAII、移动语义和智能指针如何对应当前项目？
82. TArray、TSet、TMap 的底层特点分别适合哪些项目数据？
83. 迭代器失效、容器扩容和缓存局部性可能在哪些项目代码中出现？
84. Game Thread、Render Thread、RHI Thread 和 GPU 之间如何并行？
85. 为什么阅读官方文档后还要跟一条 UE 源码调用链？
86. 阅读源码时如何从公开 API 进入实现，而不是无目标浏览整个引擎？
87. AI 辅助生成的代码如何通过闭卷重写、独立修改、边界测试和口述追问完成所有权审查？
88. 为什么不能把 AI 生成量、教程完成量或代码行数直接当成工程能力？
89. Git 提交如何体现单一意图、真实迭代、Bug 修复和性能实验？
90. 简历上的每个数字、优化和技术名词需要保存哪些可验证证据？

### 未来几个月完成清单

以下内容按优先级推进，不要求全部阻塞首次投递。

#### P0：首次投递前必须完成

- FPS：完成可玩闭环、AIController/NavMesh/FSM 边界验收、攻击与生命周期回归测试。
- FPS：完成 10/25/50 敌人的 CPU Baseline，并验证一次 AI 决策降频或错峰更新。
- FPS：完成 Animation URO、死亡后停止无用更新等至少一项有前后数据的优化。
- FPS：完成 Texture Streaming Pool 排查，记录纹理显存、画质和参数取舍。
- FPS：完成连续波次内存实验，检查 Timer、Delegate、Widget、Niagara、Decal 和尸体生命周期。
- Co-op：完成 Authority、Ownership、Replication、RepNotify、Server/Client/Multicast RPC 的运行验证。
- Co-op：完成 Create/Find/Join/Destroy Session 和至少双客户端联机测试。
- 图形：完成一个真正由自己实现的 Shader 或后处理效果，并记录 GPU 增量成本。
- 基础：能够口述 C++ 对象模型、内存与 STL，操作系统线程/虚拟内存，以及基础渲染管线。
- 算法：完成 Hot 100 第一轮，并补 A*、堆、图搜索、空间划分等游戏相关算法。
- 交付：Release 包、演示视频、README、架构图、调用链、性能表和真实 Bug 复盘。

#### P1：首次投递后一个月继续深化

- 对 Co-op 进行 100ms/200ms 延迟、抖动和丢包测试，修复重复结算与表现不同步。
- 学习并验证 Relevancy、NetUpdateFrequency、Dormancy 和基础带宽分析。
- 扩充 FPS 固定压力场景到 100 个敌人，完成 CPU、动画和内存的完整曲线。
- 对比 Tick、Timer、固定更新间隔、错峰更新和距离分级方案。
- 补全图形学基础：坐标变换、光照模型、Shadow Mapping、前向与延迟渲染。
- 选择一到两条 UE 源码调用链，形成“公开 API → 引擎实现 → 项目应用”的技术笔记。
- 继续进行闭卷重写：HealthComponent、Hitscan、换弹状态、RPC/RepNotify 小实验。

#### P2：后续两到三个月按证据选做

- 如果 Spawn/Destroy 数据证明存在明显尖峰，再为敌人或短生命周期特效设计专用对象池。
- 如果网络射击需要公平性，再实现受控范围的服务端时间戳与延迟补偿实验。
- 如果技能和状态复杂度确实增长，再单独学习 GAS 的 ASC、Ability、AttributeSet 和 GameplayEffect。
- 如果现有动画无法满足瞄准表现，再补 BlendSpace、Aim Offset、分层混合和基础 IK。
- 如果图形方向反馈良好，再深入 Custom HLSL、Shadow Mapping + PCF 或 UE Shader 插件。
- Dedicated Server、预测回滚、复杂 EQS、多线程 AI 和通用热更新不作为当前默认承诺，只有岗位要求和时间允许时再进入。

每一项技术完成后必须留下：

```text
问题或需求
→ Baseline / 原方案
→ 备选方案与取舍
→ 实现和关键调用链
→ 测试环境与数据
→ Bug 和边界条件
→ 复测结果
→ 能够独立修改与口述
```

## 15. 当前复习顺序

```text
第一步：HealthComponent
→ 第二步：Weapon Fire / LineTrace
→ 第三步：Character Ammo / Reload / Death
→ 第四步：Enemy Attack / Death
→ 第五步：Delegate 与蓝图消费者
→ 第六步：GameMode / HUD / Match State
→ 第七步：多人网络基础
→ 第八步：AI 可视化与图形项目
```

每完成一步，必须产出：

- 一张调用链图。
- 一份伪代码。
- 一次独立修改。
- 一个验证记录。
- 三到五道口述问题。

## 16. 相关文档

- `PROJECT_PROGRESS.md`：当前功能和蓝图进度。
- `FPS_BUG_LOG.md`：真实 Bug、环境故障和排查记录。
- `RESUME_ENGINE_EARLY_2026-07.md`：简历草稿，内容必须随真实完成度更新。
- `ENGINE_EARLY_SPRINT_2026-07-17_2026-07-30.md`：阶段冲刺计划，仅作为时间安排参考。

## 17. 最终路线原则

```text
先理解现有 FPS
→ 完成单机可玩闭环
→ 形成可讲解的接口与 Bug 证据
→ 再做最小多人网络
→ 独立完成 A* / FSM 可视化
→ 从零实现 OpenGL Shadow Mapping
→ 重构 RTIOW 基础
→ 最后进入 UE Global Shader / RDG
```

路线是否有效，不由项目数量判断，而由以下问题判断：

> 能否解释、能否修改、能否调试、能否测量、能否诚实说明自己的贡献。
