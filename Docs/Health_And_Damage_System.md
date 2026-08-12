# 3. 健康、伤害与死亡系统

> 本文以 `fpstrue_safe2` 当前代码为准。蓝图表现部分依据当前编辑器实现记录，提交前仍需逐项回归验证。

## 3.0 职责划分

系统不采用“蓝图检测、C++ 扣血、蓝图再判断死亡”的三套并行规则，而是保持一条权威链路：

```text
命中方生成 Damage
-> 被命中 Actor 收到 OnTakeAnyDamage
-> HealthComponent 统一扣血
-> 广播受伤、血量变化和死亡委托
-> Character / Enemy 更新权威 Gameplay 状态
-> 蓝图播放动画、声音、Camera Shake 和后处理
```

职责如下：

- `UfpstrueHealthComponent`：保存生命值、扣血、限制生命值范围、判断死亡并广播委托。
- `AfpstrueEnemyCharacter`：敌人攻击检测、攻击状态、死亡后的攻击与移动清理。
- `AfpstrueCharacter`：玩家战斗状态、死亡后的输入、射击、换弹和移动清理。
- 蓝图：动画、声音、镜头晃动、后处理和其他视觉反馈。

这里所说的 `OnEnemyDamaged`、`OnPlayerDamaged` 等是 C++ 暴露给蓝图的事件钩子，不是 UE 的 `UInterface` 类型。

## 3.1 敌人受到玩家射击

### 3.1.1 当前 C++ 链路

```text
WeaponComponent 执行 Line Trace
-> 获得 FHitResult
-> Cast<AfpstrueEnemyCharacter>
-> 根据 HitResult.BoneName 计算伤害
-> ApplyPointDamage
-> Enemy::OnTakeAnyDamage
-> HealthComponent::ApplyDamageInternal
-> OnDamageReceived
-> OnHealthChanged
-> 生命值为 0 时 OnDeath
```

当前规则：

- 命中骨骼名为 `head` 或 `neck_01`：伤害 `100`。
- 命中敌人其他部位：伤害 `40`。
- 命中 Actor 不能转换为 `AfpstrueEnemyCharacter`：不结算生命值伤害。
- 非敌人物体如果启用了物理模拟，仍可以接收射击冲量。

因此准确表述不是“自动判断是不是人”，而是“当前武器只对 `AfpstrueEnemyCharacter` 结算射击伤害”。

### 3.1.2 HealthComponent 的事件顺序

`ApplyDamageInternal` 的广播顺序是：

```text
OnDamageReceived
-> OnHealthChanged
-> OnDeath（仅生命值已经归零）
```

致死伤害也会先触发受伤事件，再触发死亡事件。蓝图必须让死亡状态拥有更高优先级：

1. `OnEnemyDamaged` 先检查 `IsDead()`，已经死亡则不再播放普通受击动画。
2. `OnEnemyDied` 中断普通受击和攻击 Montage，再进入死亡表现。
3. 不允许死亡表现结束后重新回到受击、攻击或移动状态。

### 3.1.3 敌人受击蓝图状态

推荐流程：

```text
Event OnEnemyDamaged
-> IsDead?
   -> True：返回
   -> False：继续
-> bHitReactionPlaying?
   -> True：根据设计忽略、重播或换成更高优先级反应
   -> False：继续
-> 先设置 bHitReactionPlaying = true
-> Play Montage
-> Completed / Blend Out / Interrupted
-> bHitReactionPlaying = false
```

必须先设置状态，再播放 Montage，避免同一帧内的第二次伤害绕过状态检查。

`bHitReactionPlaying` 只控制表现层，不负责生命值计算。当前设计希望普通受击不打断攻击 Montage，因此状态优先级应为：

```text
Dead > Attack Montage > Hit Reaction > Chase / Idle
```

敌人攻击期间受到普通伤害时仍然扣血，但跳过普通受击 Montage；致死伤害仍由 `OnEnemyDied` 立即覆盖攻击表现。未来如果需要“受击硬直可以打断攻击”，必须增加显式的 `InterruptAttack` 规则，同时关闭攻击窗口并清理 C++ 攻击状态，不能仅靠播放另一个 Montage 偶然覆盖当前动画。

## 3.2 敌人死亡

### 3.2.1 当前 C++ 已实现

`AfpstrueEnemyCharacter::HandleDeath()` 已经负责：

- 使用 `bIsDead` 防止重复执行死亡逻辑。
- 关闭攻击窗口并清空本次攻击检测状态。
- 将 `bIsAttacking` 设为 `false`。
- 清理 `AttackFinishTimerHandle`。
- 将移动速度设为 `0`，立即停止并禁用移动。
- 关闭 Capsule 碰撞。
- 调用蓝图事件 `OnEnemyDied`。
- `bDestroyOnDeath` 开启时，通过 `SetLifeSpan(DestroyDelay)` 延迟销毁 Actor。

`DestroyDelay` 当前 C++ 默认值是 `300` 秒，蓝图类默认值可能覆盖它，封版前需要核对。

死亡只执行一次有两层保护：

1. `HealthComponent` 在已经死亡时拒绝继续扣血。
2. `EnemyCharacter::HandleDeath` 使用 `bIsDead` 再次防重入。

### 3.2.2 蓝图只负责死亡表现

死亡表现应在以下两种方案中选择一种，不能让两套系统同时完整驱动骨骼：

**方案 A：死亡动画**

- 播放死亡 Montage。
- Montage 混出后进入 Animation Blueprint 的 `Dead` 状态或保持最终死亡姿势。
- 不应混出后重新回到 Idle / Locomotion。

**方案 B：布娃娃**

- 为 Mesh 设置合适的 Ragdoll 碰撞配置。
- 开启 `Simulate Physics`，让 Physics Asset 驱动骨骼。
- 不再依赖普通死亡动画持续驱动全身骨骼。

布娃娃就是 Skeletal Mesh 的物理身体和约束接管骨骼运动。它需要有效的 Physics Asset。给 Mesh 开启全身物理模拟后，再用空的 `Play Animation` 资产“消除脚步声”是脆弱且冗余的做法；脚步声应通过停止相关 Montage、停止音频或确保死亡后不再触发脚步 AnimNotify 解决。

### 3.2.3 为什么不要销毁 CharacterMovement

蓝图中 `Destroy Component` 的 Target 如果连接到 `CharacterMovement`，销毁的就是角色移动组件，而不是整个敌人。

当前 C++ 已执行：

```text
StopMovementImmediately
DisableMovement
```

因此无需销毁 `CharacterMovement`。直接销毁组件会让其他代码和蓝图持有失效引用，增加重启、复用和调试难度。后续若有性能证据，可以关闭组件 Tick，而不是破坏组件结构。

### 3.2.4 尸体性能治理

正确标题是“敌人尸体性能优化”，不是“玩家丧尸帧数优化”。

建议顺序：

1. 死亡瞬间停止 AI、攻击、移动与 Capsule 碰撞。
2. 需要布娃娃时只在短时间内开启物理模拟。
3. 布娃娃稳定后关闭物理模拟和不必要的碰撞。
4. 暂停动画或关闭 Skeletal Mesh Tick。
5. 根据画面需求关闭 `Cast Shadow`。
6. 最终使用 `SetLifeSpan` 销毁整个敌人 Actor。

注意：

- `Cast Shadow = false` 是停止投射阴影，不叫“设置阴影 LOD”。
- 强制使用更低 LOD 只降低渲染成本，不能代替物理和动画 Tick 的治理。
- 只有 Skeletal Mesh 资产真实存在多个 LOD 时，强制 LOD 才有意义；`LOD 0` 是最高质量，编号越大通常越低模。
- 不要在没有 Unreal Insights、`stat game` 或 `stat anim` 数据时堆叠所有优化开关。

## 3.3 敌人攻击与玩家受伤

### 3.3.1 攻击发起

当前 C++ 流程：

```text
Tick -> UpdateEnemy
-> CanAttack
-> TryAttackTarget
-> bIsAttacking = true
-> OnAttackStarted
-> 蓝图选择并播放攻击 Montage
-> AttackFinishTimer 作为超时兜底
```

`bIsAttacking` 是 C++ 的权威 Gameplay 状态。蓝图中的 Montage 播放布尔值应命名为 `bAttackMontagePlaying` 一类的表现状态，避免误认为它控制实际攻击规则。

`OnAttackStarted` 被调用前，C++ 已经执行：

```text
bIsAttacking = true
bDamageAppliedThisAttack = false
bHitTargetThisAttack = false
停止当前移动
```

所以蓝图不需要、也不能再次设置 C++ 的 `bIsAttacking`。蓝图只设置自己的 `bAttackMontagePlaying`，用于防止重复播放或表现层重入。

### 3.3.2 C++ 如何知道攻击帧

C++ 不保存“第几帧造成伤害”。项目定义了 `UfpstrueAnimNotifyState_AttackWindow`，但攻击窗口在 Montage 时间轴中的起止位置由编辑器资产决定。

```text
进入窗口 -> NotifyBegin -> BeginAttackWindow
窗口期间 -> NotifyTick  -> UpdateAttackWindow
离开窗口 -> NotifyEnd   -> EndAttackWindow
```

每个攻击 Montage 都必须独立放置一个 `Enemy Attack Window`。不能只给其中一个随机动画添加窗口。

### 3.3.3 Notify 与 Notify State

- `AnimNotify`：单个时间点触发一次，适合脚步声、枪口火焰、单次弹药结算。
- `AnimNotifyState`：拥有持续时间，提供 Begin、Tick、End，适合近战有效窗口、持续无敌或持续特效。

本项目使用 Notify State，因为剑刃在一段时间内持续移动，单帧检测在低帧率或高速挥剑时更容易漏检。

### 3.3.4 Queued 与 Branching Point

- `Queued`：异步排队执行，开销较低，但允许轻微的帧误差。
- `Branching Point`：同步执行，时间更精确，但开销更高。

攻击窗口属于 Gameplay 关键事件，可以使用 Branching Point，但不能机械地把所有通知都改成 Branching Point。若多个 Branching Point 在同一时刻重叠，可能出现其中一个不触发的警告；Montage 无法保存时应先检查重叠通知、资产只读状态和保存日志，而不是反复强制保存。

当前连续 Sweep 已经覆盖相邻动画更新之间的剑刃轨迹。封版前应分别测试 Queued 和 Branching Point，并以命中稳定性和性能数据决定最终配置。

### 3.3.5 剑刃连续命中检测

骨骼资产中已经设置：

```text
weapontop
weaponend
```

攻击窗口开始时记录剑刃两端位置。窗口每次更新时：

1. 读取两个 Socket 的当前世界坐标。
2. 在剑根到剑尖之间生成多个采样点。
3. 对每个采样点从上一帧位置 Sweep 到当前帧位置。
4. 再 Sweep 当前帧的完整剑刃线段。
5. 只接受当前玩家 `TargetCharacter`。
6. 调用 `ApplyDamage`，当前敌人基础伤害为 `10`。

这不是“武器碰撞全程开启”。只有 Montage 的攻击窗口处于激活状态时才检测。

### 3.3.6 如何避免重复扣血

每个敌人实例维护：

```text
TSet<TWeakObjectPtr<AActor>> HitActorsThisAttack
```

同一 Actor 在本轮攻击首次命中后会加入集合，后续动画更新或同一 Montage 的其他攻击窗口再次扫到它时不再扣血。

必须满足：

- 只在 `TryAttackTarget()` 开始一轮新攻击时重置集合。
- `BeginAttackWindow()` 只初始化 Socket 采样，不重置整轮攻击的去重集合。
- 同一轮攻击可以有多个采样窗口，但默认仍只允许同一目标结算一次。
- 不要在同一 Montage 中同时保留旧的单点 `Enemy Attack Hit` 和新的 `Enemy Attack Window`。
- 如果一个攻击被设计为多段伤害，应显式设计“伤害段”，不能依靠重复创建窗口偶然实现。

当前代码把 `HitActorsThisAttack` 的生命周期绑定到整轮攻击：`TryAttackTarget()` 重置，`FinishAttack()` 或死亡清理。这样即使同一 Montage 存在多个 NotifyState 窗口，也不会重新开放对同一目标的伤害。未来只有在 Combo 分段、网络预测或异步攻击需要区分多次合法提交时，才引入显式 `AttackSequenceId` 或 Damage Segment。

### 3.3.7 攻击窗口关闭条件

当前已经处理：

- Notify State 正常结束。
- 攻击完成或攻击超时 Timer 触发。
- 敌人死亡。

仍需补全或回归验证：

- Montage 的 `Completed`、`Blend Out` 和 `Interrupted` 都应调用 C++ 的攻击结束入口，不能只修改蓝图布尔值。
- 玩家在攻击窗口期间死亡时，应立即结束敌人攻击或由玩家死亡事件通知敌人停止；当前代码会拒绝继续扣血，但可能等到攻击 Timer 才完成状态清理。
- 蓝图切换 Montage、停止 Montage 或重新播放相同 Montage 时，必须确认 Notify End 和 C++ 超时兜底都能收口。

推荐将 Montage 的三个结束执行口统一连接到 `HandleAttackFinishedNotify()`。该函数和内部状态保护可以防止重复结束。

### 3.3.8 攻击蓝图的准确流程

当前蓝图应按以下顺序组织：

```text
Event OnAttackStarted
-> IsDead?
   -> True：返回
   -> False：继续
-> bAttackMontagePlaying?
   -> True：返回，防止同一攻击重复播放
   -> False：继续
-> Set bAttackMontagePlaying = true
-> Random Integer in Range
-> Select 对应攻击 Montage
-> Play Montage（Starting Position = 0）
-> Completed / Blend Out / Interrupted
-> Set bAttackMontagePlaying = false
-> HandleAttackFinishedNotify
```

注意：

- Branch 的条件不能使用未连接的常量 `True`，应实际读取 `IsDead()` 和 `bAttackMontagePlaying`。
- `Play From Start` 是 Timeline 的输入名称。攻击动画使用的是 `Play Montage`；`Starting Position = 0` 表示从 Montage 开始位置播放。
- 每个随机候选 Montage 都必须添加自己的 `Enemy Attack Window`。
- `Completed`、`Blend Out`、`Interrupted` 都要恢复表现状态；C++ 的 `FinishAttack()` 具有状态保护，可以拒绝重复收尾。
- 如果 `Play Montage` 因 Mesh、Slot 或资产配置错误而未成功播放，C++ 的 `AttackFinishTimer` 仍会在超时后结束攻击，避免永久卡在攻击状态。

### 3.3.9 受击蓝图与攻击状态的协同

普通受击表现应先检查死亡，再检查攻击：

```text
Event OnEnemyDamaged
-> IsDead?
   -> True：返回，等待 OnEnemyDied
   -> False：继续
-> IsAttacking?
   -> True：只保留扣血，不播放普通受击 Montage
   -> False：继续
-> bHitReactionPlaying?
   -> True：返回
   -> False：继续
-> Set bHitReactionPlaying = true
-> Play Hit Reaction Montage
-> Completed / Blend Out / Interrupted
-> Set bHitReactionPlaying = false
```

这里检查的 `IsAttacking()` 来自 C++。蓝图中的 `bAttackMontagePlaying` 只是表现状态，不能替代它判断 Gameplay 是否仍在攻击。

### 3.3.10 原笔记逐句纠错

| 原笔记 | 修正后的准确表述 |
| --- | --- |
| C++ 写关键帧 | C++ 定义 `UAnimNotifyState` 的 Begin、Tick、End 行为；攻击窗口在 Montage 时间轴中的位置和长度由编辑器资产设置。 |
| 攻击检测关键帧 | 不是单个“检测关键帧”，而是一段攻击有效窗口。窗口内由 `NotifyTick` 驱动连续剑刃 Sweep。 |
| 在蒙太奇通知中添加通知状态 | 正确。应使用 `Add Notify State -> fpstrueAttackWindow`，并覆盖剑刃真正具有杀伤力的时间段。 |
| 关键帧范围内持续检测碰撞 | 基本正确，但当前实现不是等待物理碰撞事件，而是主动对 `ECC_Pawn` 执行连续 Sphere Sweep 查询。 |
| 设置两个插槽 | 正确。当前 C++ 从敌人 Character Mesh 读取 `weapontop` 和 `weaponend` 的世界坐标。如果以后武器改为独立 Mesh，读取对象也必须改为武器 Mesh。 |
| 这个写成接口 | 更准确地说，C++ 暴露 `BlueprintCallable` 函数和 `BlueprintImplementableEvent` 事件钩子；它们不是 UE `UInterface`。攻击窗口目前由自定义 Notify State 自动调用，不需要蓝图每帧手动调用。 |
| 蓝图中如果为是，Set 为正在攻击 | C++ 在触发 `OnAttackStarted` 前已经将权威状态 `bIsAttacking` 设为真。蓝图只能设置表现变量 `bAttackMontagePlaying`。 |
| 播放攻击动画 From Start | Montage 使用 `Play Montage` 并令 `Starting Position = 0`；`Play From Start` 是 Timeline 的术语。 |
| 受击状态之前检查是否正在攻击 | 正确，但要明确规则：攻击期间仍然扣血，只跳过普通受击 Montage；死亡必须打断攻击。 |
| 已排队和分支点 | `Queued` 异步、开销较低但允许轻微帧误差；`Branching Point` 同步、精度更高但开销更高，并要避免同一时刻重叠。 |

### 3.3.11 从攻击到玩家扣血的完整链路

```text
C++ CanAttack
-> C++ TryAttackTarget
-> C++ bIsAttacking = true
-> Blueprint Event OnAttackStarted
-> Blueprint Play Montage
-> Montage 进入 Enemy Attack Window
-> NotifyBegin 调用 BeginAttackWindow
-> NotifyTick 调用 UpdateAttackWindow
-> C++ 读取 weapontop / weaponend
-> 连续 Sphere Sweep
-> TryApplyAttackDamage
-> HitActorsThisAttack 去重
-> ApplyDamage(Player)
-> Player::OnTakeAnyDamage
-> HealthComponent::ApplyDamageInternal
-> OnDamageReceived / OnHealthChanged
-> 玩家死亡时 OnDeath
-> Character 转发受伤事件；死亡时更新权威状态
-> Blueprint Event OnPlayerDamaged / OnPlayerDied
-> 播放声音、Camera Shake、后处理或死亡表现
-> NotifyEnd、Montage 回调或 Timer 关闭攻击
```

## 3.4 玩家健康系统

### 3.4.1 玩家与敌人共用组件

玩家和敌人都挂载 `UfpstrueHealthComponent`，它不关心拥有者是玩家还是敌人，只负责通用生命值规则。

玩家 Character 在 `BeginPlay` 绑定：

```text
HealthComponent::OnHealthChanged
-> Character::HandleHealthChanged
-> Blueprint Event OnPlayerHealthChanged

HealthComponent::OnDamageReceived
-> Character::HandleDamageReceived
-> Blueprint Event OnPlayerDamaged

HealthComponent::OnDeath
-> Character::HandleDeath
-> Blueprint Event OnPlayerDied
```

所以蓝图可用的三个表现入口是：

- `OnPlayerHealthChanged(float NewHealth)`：HUD 和血量显示。
- `OnPlayerDamaged(float DamageAmount, DamageCauser, InstigatedBy)`：受击表现。
- `OnPlayerDied()`：死亡表现。

### 3.4.2 玩家死亡的 C++ 清理

玩家死亡由 C++ 将 `CharacterState` 切换为 `Dead`，并执行：

- 防止重复死亡。
- 关闭冲刺和瞄准。
- 停止射击。
- 通知武器停止射击。
- 清理换弹 Timer。
- 停止并禁用移动。
- 调用蓝图事件 `OnPlayerDied`。

蓝图不应再次决定玩家是否死亡，只根据事件播放表现。

### 3.4.3 玩家受伤蓝图表现

推荐流程：

```text
Event OnPlayerDamaged
-> IsDead?
   -> True：交给 OnPlayerDied
   -> False：继续
-> 只播放一个 Camera Shake
-> 播放受伤声音
-> Damage Timeline: Play From Start
-> Timeline Update
-> 设置受伤 Post Process Blend Weight
```

检查项：

- `Get Player Camera Manager(0)` 的 Return Value 必须连接到 Camera Shake 的 Player Camera Manager 输入。
- 不要连续调用两个功能相同的 Camera Shake。
- `Play Montage` 的 Skeletal Mesh Component 输入必须连接正确 Mesh。

### 3.4.4 后处理与 Timeline

如果 Timeline 直接控制 `Post Process Blend Weight`：

- `0` 表示不混合该后处理。
- `1` 表示完全混合该后处理。
- 受伤脉冲应是 `0 -> 1 -> 0`，即“弱 -> 强 -> 弱”，不是“由强及弱再变强”。
- `Play` 从当前播放位置继续。
- `Play From Start` 每次从 `0` 重新开始，适合连续受击时重新触发反馈。
- `Reverse` 从当前位置倒放。
- `Reverse From End` 从时间轴末尾倒放。
- `Update` 在播放期间持续输出曲线值。
- `Finished` 只在时间轴自然到达终点时触发，`Stop` 不等于自然结束。

关键帧插值：

- `Auto`：自动计算平滑切线，适合柔和的受伤淡入淡出。
- `Linear`：匀速变化，转折更直接。
- `Constant`：保持前一个值直到下一个关键帧，形成跳变。
- `User / Break`：手动控制曲线切线。

受伤和死亡若共用同一个 Post Process Component，会发生两个 Timeline 同时写 `Blend Weight` 的竞争。应选择：

1. 使用两个独立组件：`DamagePostProcess` 和 `DeathPostProcess`；或
2. 共用一个组件，但 `OnPlayerDied` 必须先停止 Damage Timeline，再由 Death Timeline 独占控制。

死亡 Timeline 通常是 `0 -> 1` 后保持最终值，不应在死亡后又被受伤 Timeline 拉回 `0`。

## 3.5 当前完成度

### 已由代码确认

- 玩家射击对敌人按头部和身体计算 `100 / 40` 伤害。
- 玩家和敌人共用 HealthComponent。
- 受伤、血量变化、死亡三类委托链路。
- 敌人死亡防重入、停止攻击、清理攻击 Timer、禁用移动和延迟销毁。
- 玩家死亡状态、停止射击、清理换弹 Timer 和禁用移动。
- AnimNotifyState 攻击窗口。
- `weapontop / weaponend` 连续剑刃 Sweep。
- 本次攻击命中集合去重。
- 敌人死亡后拒绝继续造成伤害。

### 需要在编辑器中回归验证

- 所有随机攻击 Montage 都包含正确长度的攻击窗口。
- 攻击 Montage 的所有结束和中断出口都通知 C++ 收尾。
- 受击 Montage 的状态在 Completed、Blend Out、Interrupted 后都能恢复。
- 致死伤害不会先播放普通受击再错误覆盖死亡表现。
- Camera Shake 没有重复播放。
- 受伤和死亡后处理不会争抢同一 Blend Weight。
- 布娃娃、死亡动画和脚步声音不会同时残留。

### 尚未形成性能证据

- 布娃娃停止物理前后的 Game Thread / Physics 时间。
- 关闭尸体动画 Tick 前后的 Anim 时间。
- 关闭尸体阴影前后的 GPU Shadow Pass 时间。
- 不同尸体数量下的帧时间和内存回落。

## 3.6 面试自检问题

1. 为什么 HealthComponent 不直接播放动画？
2. 为什么致死伤害会先触发 Damage，再触发 Death？
3. 为什么 `bIsDead` 和 HealthComponent 的死亡判断都需要保留？
4. 为什么不能整段攻击动画一直开启武器伤害？
5. 连续 Sweep 比单帧 Sphere Sweep 解决了什么问题？
6. 为什么命中集合使用弱引用？
7. Notify 和 Notify State 的适用场景分别是什么？
8. Queued 和 Branching Point 的精度、性能取舍是什么？
9. Montage 中断后为什么仍需要 C++ Timer 兜底？
10. 为什么不应该销毁 CharacterMovement 组件？
11. 布娃娃、动画 Tick、阴影和 LOD 分别消耗哪部分性能？
12. 受伤 Timeline 与死亡 Timeline 同时写 Blend Weight 会发生什么？

## 3.7 官方资料

- [Animation Notifies](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-notifies-in-unreal-engine)
- [Editing Timelines](https://dev.epicgames.com/documentation/en-us/unreal-engine/editing-timelines-in-unreal-engine)
- [Physics Driven Animation](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-driven-animation-in-unreal-engine)
- [Skeletal Mesh LODs](https://dev.epicgames.com/documentation/unreal-engine/skeletal-mesh-lods-in-unreal-engine)
