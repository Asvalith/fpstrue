# FPS 项目技术主线与面试准备

> 项目：`fpstrue_safe2`，UE 5.5，单机 PvE FPS。
>
> 本文以 `Source/fpstrue` 当前源码为事实来源。蓝图只作为动画、UI、声音和特效的表现层；未在源码或资产中验证的内容不会写成已实现。
>
> 性能数字、纹理池和 VSM Non-Nanite 专题见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)，剩余接线与发布门禁见 [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md)。

## 1. 面试定位

### 1.1 30 秒介绍

这是一个 UE 5.5 C++ 单机 PvE FPS。项目完成了第一人称移动与瞄准、武器拾取、自动射击与换弹、Hitscan 部位伤害、共享生命组件、敌人 NavMesh 追击与近战、波次倒计时和胜负结算。

重点不只是功能闭环，而是把规则从蓝图收回 C++：Character 只转发输入，WeaponComponent 持有弹药和武器事务，HealthComponent 统一伤害与一次性死亡，EnemyAIController 使用 Timer 驱动 FSM，GameMode 管理波次和结算。项目还解决了换弹中断、近战漏检和重复扣血、AI 重复寻路、尸体生命周期以及大量敌人的移动和动画成本。

### 1.2 能证明的亮点

1. **职责重构**：玩家、武器、健康、敌人、AI、群体站位和游戏规则分层，删除蓝图中的第二套玩法状态。
2. **时序事务**：开火、换弹、近战、死亡和结算都有状态门禁、清理和幂等保护。
3. **场景查询**：射击使用复杂碰撞 Line Trace；近战使用动画窗口、双 Socket 连续 Sweep 和单次攻击命中集合。
4. **AI 与群体行为**：显式 `Idle / Chase / Attack / Dead` FSM、NavMesh、路径刷新阈值和双环站位。
5. **性能证据**：完成 `10 / 20 / 40 / 80 / 160` 固定规模测试、纹理驻留治理、VSM 单变量实验和尸体回收验证。

### 1.3 不能夸大的边界

- 当前没有联网、RPC、预测、回滚或 GAS。
- 当前没有行为树、EQS、AI Perception、对象池或自建线程池。
- 当前主射击链是 Hitscan；曳光表现不代表实体 Projectile 弹道。
- 当前没有实现 SSAO 专项或风格化渲染管线。
- 40 个活跃敌人约 60 FPS 是当前机器的玩法容量；160 敌人不是稳定 60 FPS。

## 2. 架构与所有权

| 模块 | 当前唯一职责 | 主要源码 |
| --- | --- | --- |
| `AfpstrueCharacter` | Enhanced Input、移动、冲刺、瞄准、当前武器引用、玩家死亡协调 | `fpstrueCharacter.h/.cpp` |
| `UfpstrueWeaponComponent` | 弹药、武器状态、射速 Timer、换弹事务、散布、后坐力、Hitscan | `fpstrueWeaponComponent.h/.cpp` |
| `UfpstruePickUpComponent` | 一次性 Overlap 拾取和消费后自销毁 | `fpstruePickUpComponent.h/.cpp` |
| `UfpstrueHealthComponent` | 统一扣血、Clamp、伤害事件和一次性死亡广播 | `fpstrueHealthComponent.h/.cpp` |
| `AfpstrueEnemyCharacter` | 近战事务、连续 Sweep、受击、Ragdoll 和死亡回收 | `fpstrueEnemyCharacter.h/.cpp` |
| `AfpstrueEnemyAIController` | Timer FSM、目标有效性、MoveTo 和路径刷新 | `fpstrueEnemyAIController.h/.cpp` |
| `AfpstrueSurroundManager` | 双环槽位、NavMesh 投影、无效弱引用清理和内环补位 | `fpstrueSurroundManager.h/.cpp` |
| `AfpstrueGameMode` | 开始校验、分帧生成、敌人注册、倒计时、波次和胜负 | `fpstrueGameMode.h/.cpp` |
| 蓝图/UMG | Montage、相机、声音、特效、贴花、文字和输入模式 | 对应 Blueprint 与 Widget 资产 |

核心原则：**C++ 保存唯一事实，蓝图响应事件做表现。** 蓝图不能再次计算伤害、扣弹、决定死亡或启动第二个追击/射速 Timer。

## 3. C++ 与蓝图边界

当前武器只保留三个蓝图表现事件：

```text
OnWeaponFirePerformed
OnWeaponReloadStarted(bool bWasEmptyReload)
OnWeaponTraceFinished(bool bHit, TraceStart, TraceEnd, TraceTarget, HitResult)
```

蓝图仅可调用两个换弹收口接口：

```text
FinishReload
CancelReload
```

`StartFire`、`StopFire`、`RequestReload`、`CommitReload`、弹药和状态查询是 C++ 侧接口。Character 通过只读转发函数向现有 HUD 提供弹药值，但不保存弹药副本。

敌人蓝图表现入口为：

```text
OnAttackStarted
OnEnemyDamaged
OnEnemyDied
```

GameMode 向 UI 广播剩余时间、波次、存活敌人数和游戏结果。UI 必须先绑定事件，再调用 `StartGameMode()`，否则会错过初始快照。

## 4. 核心调用链

### 4.1 开火、散布与伤害

```text
IA_Shoot Started
-> Character::StartWeaponFire
-> WeaponComponent::StartFire
-> CanFire
-> ActionState = Firing
-> Fire
-> RPM 时间门禁
-> TryConsumeAmmo
-> OnWeaponFirePerformed（蓝图播放双手和武器动画）
-> FireLineTrace
-> OnWeaponTraceFinished（蓝图生成命中特效）
-> ApplyPointDamage
-> EnemyCharacter::TakeDamage
-> HealthComponent::HandleOwnerTakeAnyDamage
-> OnDamageReceived / OnHealthChanged / OnDeath
```

自动武器由 WeaponComponent 的 Timer 重复调用私有 `Fire()`；松开输入、换弹、死亡和 `EndPlay()` 都会清理 Timer。

散布实现严格按当前代码描述：

```cpp
MaxRadius = tan(SpreadAngle)
Sigma = MaxRadius / 3
TruncatedProbability = 1 - exp(-0.5 * 3 * 3)
Radius = Sigma * sqrt(-2 * log(1 - FRand() * TruncatedProbability))
Angle = FRand() * 2 * PI
Direction = normalize(Forward + Right * x + Up * y)
```

当前采用二维截断高斯采样，使弹着点在准星中心附近概率更高，向边缘逐渐衰减。`SpreadAngle` 仍表示最大偏角，代码将该边界设为 `3σ`，使用截断分布的反函数直接采样，因此不会产生超出最大散布角的离群弹道，也不会因简单 Clamp 在边界堆积样本。连续射击会增加散布角，停火超过 `SpreadResetDelay` 后重置。

当前命中规则：

- 使用 `ECC_Visibility` 和复杂碰撞查询。
- 忽略玩家与武器 Owner。
- `head`、`neck_01` 使用头部伤害，其余使用身体伤害。
- 对射线命中的 Actor 提交点伤害，由目标自身决定是否处理伤害。
- 非 Character 物理组件才添加冲量；角色受击表现由角色自身处理，避免双冲量。

### 4.2 换弹事务

```text
IA_Reload Started
-> Character::RequestReload
-> StopAim + StopSprint
-> WeaponComponent::RequestReload
-> StopFire
-> ActionState = Reloading
-> ReloadSequence++
-> 启动 Fail-safe Timer
-> OnWeaponReloadStarted
-> 蓝图同时播放双手与枪械 Montage
-> Reload Commit AnimNotify
-> UfpstrueAnimNotify_ReloadCommit::Notify
-> WeaponComponent::CommitReload
-> Montage Completed -> FinishReload
-> Montage Interrupted -> CancelReload
```

事务边界：

- `bReloadAmmoCommitted` 保证一次换弹只转移一次弹药。
- `ActiveReloadSequence` 让旧 Timer 不能提交新一轮换弹。
- `FinishReload()` 会调用一次 `CommitReload()` 作为漏 Notify 的兜底。
- Reloading 状态下 `CanFire()` 为 false，因此开火不能打断换弹并继续扣弹。
- 死亡会清除 Reload Timer 并把武器设为 `Disabled`。

### 4.3 敌人 FSM 与寻路

```text
OnPossess
-> 获取 Enemy、SurroundManager 和目标
-> 随机化首次决策延迟
-> 一次性 Timer 调用 UpdateAI
-> 每轮按状态重新安排下次 Timer
-> Idle / Chase / Attack / Dead
```

状态选择：

- 目标无效或超出 ChaseRange：`Idle` 并停止移动。
- 已在攻击或进入攻击距离：`Attack`，停止移动并面向玩家。
- 已分配站位：投影到 NavMesh 后 `MoveToLocation`。
- 无可用站位时：回退 `MoveToActor(TargetCharacter)`。
- 死亡、UnPossess、EndPlay：停止移动、清 Timer、释放槽位。

AIController 不 Tick。决策间隔根据攻击、追击、远距和空闲状态变化；`MoveToGoal()` 只在目标位移超过 `PathRefreshDistance`、没有旧目标或路径空闲时提交新请求。

SurroundManager 当前只有双环槽位和内环补位，**没有 Attack Token 或攻击名额系统**。靠近并满足冷却的敌人可以立即攻击，这是当前代码事实。

### 4.4 近战攻击窗口

```text
AI TryAttackTarget
-> bIsAttacking = true
-> OnAttackStarted（蓝图播放 Montage）
-> AnimNotifyState Begin：BeginAttackWindow
-> AnimNotifyState Tick：UpdateAttackWindow
-> AnimNotifyState End：EndAttackWindow
-> weapontop / weaponend 双 Socket
-> 前后动画帧之间插值采样并 Sphere Sweep
-> TryApplyAttackDamage
-> ApplyDamage
-> 玩家 HealthComponent
-> Montage/Notify 结束或 Fail-safe Timer -> FinishAttack
```

防漏检与防重：

- `WeaponTraceSampleCount` 默认 4，在刀刃上进行多点采样。
- 保存上一动画帧 Socket 位置，在前后帧之间 Sweep，降低低帧率穿透。
- `HitActorsThisAttack` 和 `bHitTargetThisAttack` 保证单次攻击只扣一次血。
- 窗口、Montage、Timer 或死亡都进入统一清理路径。
- `HandleAttackHitNotify()` 保留单次范围 Sweep 作为兼容入口，但正式高精度链路是 AnimNotifyState。

### 4.5 生命、死亡与胜负

HealthComponent 不 Tick。Owner 收到 `OnTakeAnyDamage` 后：

```text
拒绝非正伤害和已死亡对象
-> Clamp(CurrentHealth - Damage)
-> OnDamageReceived
-> OnHealthChanged
-> 首次到 0：OnDeath
```

一次性保护分层存在：

- HealthComponent 的 `bDeathBroadcast` 保护领域死亡事件。
- Character 的 `bDeathHandled` 保护输入、移动和武器收口。
- EnemyCharacter 的 `bIsDead` 保护 AI、Ragdoll 和回收副作用。
- GameMode 的 `bGameEnded` 保护全局结算。

玩家死亡链：

```text
HealthComponent::OnDeath
-> Character::HandleDeath
-> 禁止移动、禁用武器、退出瞄准
-> OnPlayerDeathReported
-> GameMode::HandlePlayerDied
-> FinishGame(false)
```

胜利只在倒计时到 0 且玩家仍存活时提交。失败不等待倒计时，玩家死亡立即触发。`FinishGame()` 清理倒计时、波次和生成 Timer，停止敌人并广播一次 `OnGameResult`。

敌人死亡后停止 AI 和 Movement，关闭 Capsule，Mesh 切换 `Ragdoll`、开启重力与物理，下一帧施加受控冲量，并按当前默认 `DestroyDelay = 30s` 使用 `SetLifeSpan()` 回收。

### 4.6 波次生成

`StartGameMode()` 先校验 EnemyClass、`EnemySpawn` TargetPoint、玩家 HealthComponent 和 SurroundManager。通过后广播初始 `90` 秒、波次和存活数，再启动倒计时和第一波。

生成链：

```text
StartNextWave
-> SpawnCurrentWave
-> 打乱出生点
-> 默认每 0.05 秒 SpawnNextQueuedEnemy
-> NavMesh 随机可达点 + ProjectPointToNavigation
-> SpawnActor
-> 必要时 SpawnDefaultController
-> InitializeCombatContext
-> RegisterEnemy
```

重复使用出生点时，采样半径按 `sqrt(reuseCount + 1)` 扩大，最大 2000 cm；单个敌人最多尝试 8 次。连续失败达到阈值会停止队列并记录错误，不会无限重试。

## 5. 关键设计取舍

| 问题 | 当前选择 | 原因 | 条件变化后的替代 |
| --- | --- | --- | --- |
| 弹药放哪里 | WeaponComponent | 弹药随武器实例，避免 Character 与 Weapon 双份状态 | 多武器时增加 Inventory/Equipment 协调层 |
| 规则还是表现 | C++ 规则，蓝图表现 | C++ 可测试、可复用；蓝图适合资产编排 | 只有纯表现参数留在蓝图 |
| Hitscan 或 Projectile | Hitscan | 即时手感、一次查询、无 Projectile 生命周期成本 | 弹速、下坠、提前量成为玩法时增加 Projectile 策略 |
| FSM 或行为树 | Timer FSM | 当前只有四个状态，调用链短且可解释 | 出现感知记忆、并行任务、可复用子树后迁移 BT/StateTree |
| AI Tick 或 Timer | 一次性 Timer | 决策不需要每帧执行，且可按距离降频 | 高频反应仍只提高局部状态频率 |
| 近战检测 | AnimNotifyState + Sweep | 检测窗口与动画时序一致，连续 Sweep 降低漏检 | 复杂武器形状可增加多段几何或专用 Hit Zone |
| 立即 Destroy 或延迟回收 | Ragdoll + 30 秒 LifeSpan | 保留死亡反馈，同时控制多波次累积 | 大量尸体时分级为 Ragdoll、Sleep、Frozen Pose、Destroy |

## 6. 实战问题与解决过程

| 现象 | 根因定位 | 当前修复 | 复查入口 |
| --- | --- | --- | --- |
| 输入正常但无法开火 | `FireAction` 未赋值、Mapping Context 错误或武器未 Attach | C++ 统一绑定 IA；空资产输出明确日志；Attach 成功后设置当前武器 | `SetupPlayerInputComponent`、`AttachWeapon` |
| 换弹被打断后还能开枪 | 动画、弹药转移和状态结束没有事务边界 | Reloading 门禁、Commit Notify、Completed/Interrupted 分流、Sequence 与 Fail-safe | `RequestReload` 到 `CancelReload` |
| 敌人站着不动 | 蓝图 AIController Class/自动 Possess、NavMesh 或旧追击 Timer 冲突 | C++ AIController + SpawnDefaultController + NavMesh；删除蓝图追击 Timer/Tick | `SpawnEnemyAtPoint`、`OnPossess` |
| 靠近后敌人弹开 | Capsule 拥挤、重复移动源或过强冲量 | 单一 C++ MoveTo 源；受击冲量与死亡冲量分离；敌人命中不再追加通用物理冲量 | EnemyCharacter 冲量与 Weapon Trace |
| 近战漏检或重复扣血 | 单点 Notify、低帧率跨越、同窗口多次命中 | 双 Socket、跨帧 Sweep、命中集合、攻击状态和结束兜底 | `UpdateAttackWindow`、`TryApplyAttackDamage` |
| 倒计时是 0、结果事件无响应 | UI 在初始广播后才绑定，或 Delegate Target 接成 self | 先创建/绑定 HUD 和结果事件，再 `StartGameMode`；Target 使用 Cast 结果 | GameMode 初始广播与 Level BP |
| 尸体立即消失或长期累积 | 蓝图 Destroy 与 C++ LifeSpan 重复，或旧默认 300 秒 | 删除蓝图 Destroy；C++ 统一 30 秒 LifeSpan | `EnemyCharacter::HandleDeath` |
| 敌人出现混凝土贴花 | Trace 表现没有按 HitActor 分类 | C++ 只广播 HitResult；蓝图对 Enemy 与场景表面分支播放不同表现 | `OnWeaponTraceFinished` 蓝图 |

排查统一顺序：**确认运行对象和类 -> 看输入/事件是否进入 -> 检查 Target 与状态门禁 -> 检查 Timer/生命周期 -> 最后再改算法。**

## 7. 项目中实际使用的设计思想

- **Component**：Health、Weapon、PickUp 按可复用职责拆分。
- **Observer**：动态多播委托把规则事件发送给 GameMode、UI 和蓝图表现。
- **State**：Weapon ActionState、Enemy FSM 和一次性死亡状态控制非法转换。
- **Coordinator**：GameMode 协调全局规则，SurroundManager 协调群体站位。
- **Command/Adapter 倾向**：Character 把输入意图转发给 Weapon，而不实现武器规则。

不要把每个函数都强行命名为设计模式。面试时应先讲职责和问题，再说明模式只是对现有结构的概括。

## 8. 高频面试追问

这部分只列回答核心，详细内容直接回到前文调用链，避免维护第二份答案。

| 问题 | 回答必须覆盖 |
| --- | --- |
| 为什么弹药不在 Character | 状态所有权、换枪语义、避免双扣与 UI 不一致 |
| 从输入到扣血的调用链 | Enhanced Input、Weapon 状态/RPM、Trace、PointDamage、Health |
| 为什么用 Hitscan | 手感、查询成本、生命周期；同时说明枪口遮挡和飞行时间代价 |
| `sqrt(random)` 为什么存在 | 圆盘面积随半径平方增长；说明当前并非严格均匀立体角 |
| 换弹为什么不能只等 Montage 完成 | Commit 时刻、动画中断、Fail-safe、幂等与旧 Timer 失效 |
| 死亡为什么多层防重 | 每层保护不同副作用：领域事件、角色收口、全局结算 |
| 为什么 AI 不用 Tick | 决策频率低于渲染帧；按状态和距离调度，路径请求有阈值 |
| 为什么暂时不用行为树/EQS | 当前状态少、站位规则明确；说明迁移触发条件 |
| 近战如何防漏检和重复伤害 | 动画窗口、跨帧 Sweep、多点采样、命中集合 |
| NavMesh 和 A* 是什么关系 | 项目调用高层 MoveTo/NavMesh；底层路径算法由引擎负责，不自称手写 A* |
| GameMode 如何判胜负 | 死亡立即失败；时间到且存活才胜利；`bGameEnded` 防重复 |
| 项目最难问题是什么 | 选择换弹事务、近战窗口或多敌人性能，按现象、定位、方案、验证回答 |
| 当前最大技术债 | 武器伤害依赖 Enemy 类、HUD 初始快照时序、Shipping 发布验收 |

## 9. 扩展考点

只掌握核心边界，不写成当前成果：

- **联机**：服务器权威状态同步；GameMode 留在服务器，复制数据进入 GameState/PlayerState；射击验证射速、弹药、方向、距离和遮挡。
- **GAS**：只有复杂属性、Buff、Tag、技能组合和预测需求出现时引入；核心是 ASC Owner/Avatar、Ability 激活/Commit、GE、Attribute 和 PredictionKey。
- **行为树/EQS/感知**：适合感知记忆、掩体评分、并行行为和可复用子树；必须衡量 Query 成本和调试复杂度。
- **对象池与异步加载**：先用 Profile 证明 Spawn/Destroy 或 IO 是瓶颈；UObject 创建和 Actor Spawn 仍回到 Game Thread。
- **渲染**：理解 Deferred Base Pass、Lighting、VSM 和 Post Process 顺序；当前项目后处理是蓝图表现，不把 SSAO 写成自研功能。
- **多平台**：DeviceProfile、Scalability、纹理/骨骼最小 LOD、输入抽象和 PSO 缓存；每个平台独立建立预算。

## 10. 代码入口与证据

面试前优先打开：

```text
Source/fpstrue/fpstrueCharacter.cpp
Source/fpstrue/fpstrueWeaponComponent.cpp
Source/fpstrue/fpstrueAnimNotify_ReloadCommit.cpp
Source/fpstrue/fpstrueHealthComponent.cpp
Source/fpstrue/fpstrueEnemyCharacter.cpp
Source/fpstrue/fpstrueAnimNotifyState_AttackWindow.cpp
Source/fpstrue/fpstrueEnemyAIController.cpp
Source/fpstrue/fpstrueSurroundManager.cpp
Source/fpstrue/fpstrueGameMode.cpp
```

证据层级：

```text
当前 C++ / 实际蓝图资产
-> 最新构建、CSV、日志、MemReport 和截图
-> 本文与性能专题
-> Learning 和 Archive
```

回答时始终区分：**已实现、已验证、待回归、候选方案、知识储备**。
