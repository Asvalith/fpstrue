# UE5 游戏框架与技术路线复习文档

最后更新：2026-07-21

## 1. 文档定位

本文档用于复习 UE5 Gameplay Framework、梳理 `fpstrue` 当前架构，并规划后续多人网络、游戏 AI、渲染和图形学学习路线。

核心原则：

- 只把能够解释、修改和验证的内容写成个人能力。
- 借鉴代码可以作为学习起点，但必须重新梳理调用链并完成独立修改。
- 优先完成一个可玩的项目，再增加网络、AI 和渲染深度。
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
