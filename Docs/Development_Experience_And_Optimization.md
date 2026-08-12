# FPS 开发经历与技术取舍复盘

> 用途：面试中讨论开发过程、Bug、方案比较和后续优化。
>
> 规则：只把代码已经实现并验证过的内容说成“做过”；没有性能数据的内容只能说成“发现的风险、准备验证的方案”。
>
> 当前架构、设计模式和模拟面试的唯一正文见 [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md)。本文只保留经历与证据，避免重复维护。

## 1. 如何讲一个开发经历

不要只讲功能名称。按以下顺序回答：

```text
原始需求
-> 第一版方案
-> 实际出现的问题
-> 比较过的替代方案
-> 最终选择与原因
-> 付出的代价
-> 如何验证
-> 下一步还能怎么改
```

这套结构可以用于攻击检测、换弹、健康组件、AI 更新和尸体性能治理。

## 2. 近战检测从范围判定到通知驱动

### 2.1 第一版：角色前方范围检测

项目仍保留旧入口：

```text
HandleAttackHitNotify
-> PerformMeleeHit
-> 从敌人前方向外做一次 Sphere Sweep
-> 命中 TargetCharacter 后 ApplyDamage
```

优点：

- 实现简单。
- 容易调试攻击距离和半径。
- 不依赖武器骨骼 Socket。

问题：

- 判定区域属于敌人角色，而不是视觉剑刃。
- 剑还没有接触玩家时，玩家可能已经进入前方球形范围。
- 剑已经划过玩家，但单次 Notify 的检测时机不合适时可能漏检。
- 动画更换后，需要重新凭经验调整 `AttackRange / AttackTraceRadius / AttackTraceHeight`。
- 动画与命中在空间上缺乏一致性，难以解释“为什么这一刀算命中”。

### 2.2 “碰撞 + 通知”的准确含义

口头上可以说：

> 我把角色前方的范围伤害改成了动画通知驱动的武器碰撞检测。

技术上更准确的说法是：

> 使用 `AnimNotifyState` 定义攻击有效窗口，并基于剑刃两个 Socket 做连续 Sphere Sweep。

当前实现不是给剑添加一个永久开启的碰撞盒，也不是单纯等待 `OnComponentBeginOverlap`。这里的“碰撞”是主动执行 Collision Query。

### 2.3 比较过的方案

| 方案 | 优点 | 问题 | 结论 |
| --- | --- | --- | --- |
| 角色前方范围 Sweep | 简单、便宜 | 与剑刃轨迹不一致 | 仅保留旧入口 |
| 单点 AnimNotify + 一次 Sweep | 时机比纯 Timer 准确 | 快速动画或低帧率仍可能漏检 | 适合简单攻击 |
| Notify 打开武器 Collision + Overlap | 蓝图直观 | 快速武器可能穿透；重复 Overlap、碰撞层和启停生命周期更复杂 | 当前不采用 |
| AnimNotifyState + Socket 连续 Sweep | 时间窗口和空间轨迹都明确；易调试 | 每个有效更新会执行多次查询 | 当前方案 |

### 2.4 当前实现

Montage 中放置 `Enemy Attack Window`：

```text
NotifyBegin -> BeginAttackWindow
NotifyTick  -> UpdateAttackWindow
NotifyEnd   -> EndAttackWindow
```

Skeleton 上设置：

```text
weapontop
weaponend
```

窗口内：

1. 读取剑刃两端当前世界坐标。
2. 保存上一动画更新的剑刃坐标。
3. 在剑根到剑尖之间生成多个采样点。
4. 每个采样点从上一位置 Sweep 到当前位置。
5. 额外 Sweep 当前帧的完整剑刃线段。
6. 命中当前玩家后调用 `ApplyDamage`。

这样覆盖的是剑刃在两个更新点之间经过的空间，而不只是某一瞬间的位置。

### 2.5 已实现的正确性保护

- 只有攻击窗口激活时才执行剑刃查询。
- 忽略敌人自身。
- 只接受当前 `TargetCharacter`。
- `HitActorsThisAttack` 防止同一目标在连续更新中重复扣血。
- 使用弱引用保存命中 Actor，不延长目标生命周期。
- Notify 正常结束、攻击结束、超时和敌人死亡都会关闭窗口。
- 攻击完成 Timer 是动画回调失效时的兜底。

### 2.6 仍可优化的地方

以下是下一步思路，不能说成已经完成：

1. **命中后提前停止查询**

   当前攻击只允许对玩家结算一次伤害。首次命中后，后续 Notify Tick 仍可能继续 Sweep，只是被命中集合拦住。可以在单段攻击命中后提前结束窗口，减少无意义查询。

2. **整轮攻击共享去重状态（已完成）**

   命中集合现在只在 `TryAttackTarget()` 开始一轮攻击时重置，`BeginAttackWindow()` 不再清空。同一个 Montage 即使存在多个攻击窗口，也不能再次伤害当前目标。若未来支持网络预测、Combo 分段或异步命中，再考虑显式 `AttackSequenceId`，避免把当前简单事务过度设计。

3. **采样数量分级**

   当前 `WeaponTraceSampleCount = 4`。可以根据剑刃长度、动画速度或敌人距离调整采样数，但必须比较漏检率和查询耗时，不能只提高采样。

4. **自定义碰撞通道**

   当前查询使用 `ECC_Pawn`。后续可增加专用 `MeleeDamageable` 通道，减少无关 Pawn 进入结果集，并明确角色 Capsule、Mesh 和其他 Pawn 的响应规则。

5. **墙体遮挡**

   当前武器查询只关心 Pawn。若剑刃可能隔墙命中，需要增加 WorldStatic 阻挡测试，或者设计一个能在首个阻挡面停止的查询规则。

6. **清理旧入口**

   所有 Montage 都迁移并回归验证后，才能删除旧的单点 Notify 范围检测。删除前必须确认资产中没有旧 Notify 引用。

### 2.7 面试口述示例

> 第一版近战是在动画命中点从敌人前方做一次 Sphere Sweep，虽然简单，但范围和剑刃视觉轨迹对不上，而且动画速度变化后容易出现提前命中或漏检。我比较了开启武器碰撞盒和主动 Sweep：Overlap 更直观，但快速武器的穿透、重复事件和碰撞启停会让状态更复杂。最后我用 AnimNotifyState 定义有效时间，再用剑根和剑尖 Socket 做上一更新到当前更新的连续 Sweep，并维护本次攻击的弱引用命中集合。代价是窗口内查询次数增加，所以后续会测试命中后提前停止和采样数分级，而不是盲目增加 Sweep。

## 3. 重复伤害与攻击中断

### 3.1 遇到的问题

攻击窗口会持续多个动画更新。玩家 Capsule 和 Mesh 也可能产生多个命中结果。如果每次结果都直接 `ApplyDamage`，一刀可能扣多次血。

### 3.2 当前解决

```text
HitActorsThisAttack
-> 命中前检查
-> 首次成功 ApplyDamage 后加入集合
-> 后续结果直接忽略
```

同时维护：

```text
bIsAttacking
bAttackWindowActive
bHitTargetThisAttack
bDamageAppliedThisAttack
```

这些变量承担不同职责，不能用一个“正在攻击”布尔值代替全部状态。

### 3.3 已知边界

- 玩家在窗口中死亡后，`TryApplyAttackDamage` 会拒绝继续扣血，但敌人的攻击状态可能等到 Timer 才结束。
- Montage 被替换或中断时，需要确认 Completed、Blend Out、Interrupted 和 Notify End 都能收口。
- 蓝图表现变量 `bAttackMontagePlaying` 不能替代 C++ 的 `bIsAttacking`。

后续可以让玩家死亡事件主动通知正在攻击的敌人收尾，但要先解决敌人与目标之间的订阅和解绑生命周期。

## 4. HealthComponent 的抽取

### 4.1 原始问题

如果玩家和敌人分别保存血量、分别判断死亡，会重复出现：

- 扣血和 Clamp 逻辑。
- 死亡防重入。
- UI 血量事件。
- DamageCauser 和 InstigatedBy 传递。

### 4.2 当前取舍

把通用规则放进无 Tick 的 `UfpstrueHealthComponent`：

```text
Owner::OnTakeAnyDamage
-> ApplyDamageInternal
-> OnDamageReceived
-> OnHealthChanged
-> OnDeath
```

玩家和敌人只处理各自死亡后的 Gameplay 状态，蓝图只处理表现。

### 4.3 为什么暂时不增加 Damageable Interface

当前正式射击对象只有 `AfpstrueEnemyCharacter`，武器明确 Cast 敌人。`TargetDummy` 只是测试遗留。

此时强行增加通用 Damageable Interface 会扩大抽象面，却没有新的正式对象需要它。等到可破坏物、多个敌人基类或不同伤害接收者出现时，再比较：

- 通过 `HealthComponent` 判断可受伤。
- 通过 `IDamageable` 表达能力。
- 通过统一敌人基类约束。

这是“避免过早抽象”的取舍，不是不会写接口。

### 4.4 事件顺序带来的 Bug

致死伤害的顺序是：

```text
OnDamageReceived
-> OnHealthChanged
-> OnDeath
```

所以最后一击可能先触发普通受击表现，再触发死亡表现。当前由 Character 和 EnemyCharacter 的 C++ Damage 回调读取已经更新的 Health；如果该次伤害已经致死，就不再调用普通 Damaged 蓝图事件，只让 Death 表现继续。表现优先级是：

```text
Dead > Attack Montage > Hit Reaction > Movement
```

## 5. 开火与换弹状态治理

### 5.1 为什么拆开 StartFire 和 Fire

当前结构：

- `StartFire()`：响应按下，进入 `Firing`，立即首发，并按 `RoundsPerMinute` 启动 C++ Timer。
- `Fire()`：完成一次受射速门禁保护的弹药消耗、Line Trace 和后坐力。
- `StopFire()`：响应松开、取消、换弹、空仓和死亡，并清理 C++ Timer。
- 蓝图只消费 `OnWeaponFirePerformed`，播放单发动画、声音和特效。

这样单次射击规则不会和“按住多久”混在一起。

### 5.2 为什么把射速调度移入 WeaponComponent

旧蓝图 Timer 的优点是调参直观，但实际出现了三个权威源：Character 保存开火/弹药状态、WeaponComponent 执行射击、蓝图决定射速。它带来的问题包括：

- 重复启动 Timer 可能形成双倍射速。
- 松开、换弹和死亡任一路径漏清理时仍会调用 `Fire()`。
- 换弹动画、Character 布尔值和蓝图 Timer 可能互相不同步。

本轮把弹药、射速、连续射击、换弹状态和两个 Timer 全部移到 WeaponComponent；Enhanced Input 绑定及 Mapping Context 生命周期统一收回 Character。Character 只提交意图并保存当前装备引用，WeaponComponent 不再依赖输入系统类型。

优化效果不是“凭空提高 FPS”，而是建立单一权威：

- 开火间隔由 `RoundsPerMinute` 唯一计算，并有时间门禁拒绝过快重复调用。
- 换弹、死亡和 `EndPlay()` 都能在同一对象内清理射击 Timer。
- 每把武器实例保存自己的弹药，为以后切枪保留正确的所有权边界。
- 蓝图不再参与 Gameplay 射速计算，只响应结果事件。

### 5.3 换弹结算的取舍

当前换弹使用 C++ 事务接口：

```text
Character::RequestReload
-> WeaponComponent::RequestReload
-> ActionState = Reloading / ReloadSequence++
-> AnimNotify 调用 CommitReload
-> Montage Completed 调用 FinishReload
-> Montage Interrupted 调用 CancelReload
-> 带序列号的 Timer 只作超时恢复
```

`bReloadAmmoCommitted` 保证一轮换弹只搬运一次弹药，序列号拒绝旧 Timer，状态门禁拒绝已经结束的换弹回调。动画 Notify 丢失时 Timer 仍能恢复，但正式蓝图接线完成前还不能算 Montage 闭环验收。

这里采用的语义是：弹匣插入 Notify 是弹药提交点，Montage 回调决定事务正常完成或取消，Timer 只保证异常情况下不会永久卡在 `Reloading`。蓝图下一步必须补齐三个回调并完成中断矩阵测试。

## 6. AI 与 Game Thread

### 6.1 改造前事实

敌人仍在 Character Tick 中执行：

```text
距离计算
-> 追逐判断
-> 攻击范围判断
-> 转向或 AddMovementInput
```

还没有 AIController、NavMesh、行为树或显式 FSM。

### 6.2 2026-07-29 C++ AIController 改造记录

本次改造先处理 C++ 结构，不直接宣称 AI 完整闭环。蓝图旧节点和关卡 NavMesh 仍需要在编辑器中清理和验证。

优化原因：

1. **绕障能力**

   `AddMovementInput` 只能把敌人推向玩家方向，遇到墙体和障碍物时不会规划路线。改为 `AIController::MoveToActor()` 后，可以把移动交给 NavMesh。

2. **减少逐帧决策**

   旧实现每个敌人每帧执行目标检查、距离计算、状态判断和移动输入。敌人数量增加后，这部分会直接放大 Game Thread 压力。新版 AIController 关闭自身 Tick，用 Timer 按固定间隔做决策。

3. **明确状态边界**

   旧版 `Idle / Chase / Attack / Dead` 主要由距离、`bIsAttacking` 和 `bIsDead` 隐式组合出来。新版新增 `EFPEnemyAIState`，由 AIController 显式维护状态，后续更容易调试、显示和扩展。

4. **拆分角色和控制器职责**

   `AfpstrueEnemyCharacter` 保留生命、攻击窗口、Sweep、伤害和蓝图表现事件。`AfpstrueEnemyAIController` 负责目标选择、状态切换和导航移动，避免敌人角色类继续堆 AI 决策。

本次 C++ 变化：

- `Build.cs` 增加 `AIModule`、`NavigationSystem` 和 `GameplayTasks`。
- 新增 `AfpstrueEnemyAIController`。
- 新增 `EFPEnemyAIState::Idle / Chase / Attack / Dead`。
- AIController 使用 `DecisionInterval = 0.2f` 的 Timer 做决策。
- Chase 状态调用 `MoveToActor(TargetCharacter)`。
- Attack 状态停止移动、面向玩家，并调用敌人已有攻击入口。
- Dead 状态停止移动并清理 AI 决策 Timer。
- `AfpstrueEnemyCharacter` 关闭自身 Tick，删除旧追击职责。
- `AfpstrueEnemyCharacter` 默认设置 AIController，并允许场景放置和动态生成敌人自动 Possess。

保留内容：

- 攻击 Montage 仍由蓝图事件 `OnAttackStarted` 驱动。
- 攻击有效窗口仍由 `AnimNotifyState` 调用 `BeginAttackWindow / UpdateAttackWindow / EndAttackWindow`。
- 剑刃检测仍使用 `weapontop / weaponend` 双 Socket 和连续 Sphere Sweep。
- 伤害链仍是 `ApplyDamage -> HealthComponent`。
- `OnAttackLanded / OnAttackMissed / OnAttackFinished / OnEnemyDamaged / OnEnemyDied` 继续作为蓝图表现入口。

验证状态：

- Development Editor 编译通过。
- `UnrealEditor-fpstrue.dll` 已重新生成。
- 源码扫描确认敌人类中不再存在旧 `Tick()` 追击和 `AddMovementInput()` 追逐。

剩余验证：

- 清理 `enemy_BP` 中旧 Tick、Timer、AI MoveTo 或手写追击节点。
- 关卡添加并调整 `NavMeshBoundsVolume`。
- 确认 `EnemySpawn` 点位于绿色 NavMesh 区域。
- PIE 验证敌人在复杂障碍物场景中的绕行。
- 用 10、25、50 敌人场景记录 Game Thread 和 AI 决策频率。

### 6.3 2026-07-31 AI 与 GameMode 架构收口

本次修改的目标是降低职责耦合、减少隐式依赖，并为后续压测保留可替换入口。它不是一次有数据支撑的性能优化，因此当前只能描述为“架构改造并完成编译验证”。

#### 6.3.1 拆分 AI 决策管线

改造前，`UpdateAI()` 同时处理目标恢复、死亡检查、攻击状态、攻击令牌、包围槽位和导航请求。功能能够运行，但函数过长，修改一个分支容易影响其他状态，也不利于逐段打断点和统计耗时。

改造后保留同一个 Timer 入口，将决策拆为：

```text
PrepareDecisionContext
-> HandleActiveAttack
-> HandleAttackToken
-> HandleSurroundMovement
-> 最终 MoveToActor 兜底
```

优化原因：

- 每个阶段只有一个明确的提前退出条件，状态优先级更容易检查。
- 攻击、令牌和移动职责分开后，可以单独增加日志、Trace 或测试，而不用继续扩大主函数。
- 所有状态仍通过 `SetAIState()` 写入，避免在多个分支直接修改枚举。
- 没有改动 `OnAttackStarted`、攻击窗口或伤害接口，现有动画蓝图无需重新接线。

方案取舍：

- 没有立即把每个状态写成独立 UObject 状态类。当前只有四个状态，使用状态对象会增加对象生命周期、跳转和调试成本。
- 没有引入行为树或 EQS。现阶段的难点是包围、攻击名额和性能证据，不是增加另一套决策框架。
- 本次拆分主要提升可维护性，必须经过 Unreal Insights 前后对比后才能声称降低了 CPU 时间。

#### 6.3.2 显式注入战斗上下文

改造前，AIController 在运行时通过全局查询恢复玩家和包围管理器。查询可以作为容错，但会隐藏对象之间的依赖，也依赖 World 中恰好只有一个匹配对象。

改造后，GameMode 生成敌人后调用：

```text
InitializeCombatContext(PlayerCharacter, SurroundManager)
```

优化原因：

- GameMode 本来就拥有本局玩家、敌人和包围管理器，是组装这些对象关系的合适位置。
- AIController 的目标来源变得明确，减少正常流程对 `GetActorOfClass` 一类全局搜索的依赖。
- `ResolveTarget()` 和 `ResolveSurroundManager()` 仍作为旧关卡放置敌人或初始化异常时的兼容兜底。
- 该入口仅供 C++ 组装使用，没有新增必须在蓝图中填写的引脚。

#### 6.3.3 波次配置与旧蓝图兼容

改造前，每波数量只能由：

```text
BaseEnemiesPerWave + WaveIndex * EnemiesAddedPerWave
```

计算，且所有波次共用一个 `EnemyClass`。这适合原型，但后续加入不同敌人类型或固定压力测试数量时需要改代码。

改造后增加可选的 `WaveConfigs`：

```text
WaveConfigs 非空
-> 每波读取 EnemyClass 和 EnemyCount

WaveConfigs 为空
-> 继续使用 TotalWaves、BaseEnemiesPerWave、
   EnemiesAddedPerWave 和原 EnemyClass
```

优化原因：

- 区分配置数据和运行状态，GameMode 仍只负责规则与调度。
- 固定的 10/25/50/100 敌人实验可以直接配置，减少为测试反复改代码。
- 空数组自动走旧逻辑，现有 GameMode 蓝图默认值和关卡调用不需要迁移。

#### 6.3.4 本次验证

- 未改动任何 `.uasset` 或关卡蓝图。
- 未重命名已有 `UFUNCTION`、Delegate 和可编辑属性。
- Unreal Header Tool 反射代码生成通过。
- `fpstrueEditor Win64 Development` 编译和 DLL 链接通过。
- 仍需 PIE 回归：开始游戏、三波生成、AI 包围、攻击、死亡、胜负与重新开始。

#### 6.3.5 仍可优化的空间

按优先级记录，只有测试证明问题存在后才实施：

1. **P0：行为回归和失败路径**

   记录 `MoveTo` 的结果，覆盖目标不可达、NavMesh 投影失败、控制器未 Possess 和攻击被中断；失败后应换点、重试或回到 Idle，不能持续重复请求。

2. **P0：AI 决策错峰**

   当前敌人在接近时间生成时，`DecisionInterval` 相同，Timer 可能在同一帧集中触发。为首次决策加入小范围随机延迟，再用 Insights 验证峰值是否下降。

3. **P0：距离与战斗状态分级**

   近距离攻击保持高频，中距离追逐降频，远距离待机进一步降频。必须同时记录 Game Thread 和攻击响应延迟，避免只优化耗时却让战斗迟钝。

4. **P0：分批生成**

   当前一波敌人在同一帧循环 Spawn，敌人数较多时可能形成 Spawn、组件注册和动画初始化尖峰。先记录尖峰，再决定通过 Timer/队列分帧生成，而不是直接池化。

5. **P1：状态进入与退出动作**

   如果状态继续增加，可让 `SetAIState()` 统一执行进入/退出副作用，例如停止移动、释放攻击令牌和清理攻击窗口。当前状态较少，先避免过度抽象。

6. **P1：波次数据资产化**

   当波次还需要出生间隔、敌人类型权重和难度参数时，再把 `WaveConfigs` 迁移到 DataAsset/DataTable；当前结构体数组已经足够，不提前增加资产层。

7. **P1：生命周期审计**

   检查 GameMode、敌人死亡 Delegate、Timer、SurroundManager 槽位和攻击令牌在死亡、重启、切关时是否全部释放，并记录对象数和内存是否回落。

8. **P2：行为树、EQS、Mass 或多线程**

   只有现有 FSM 难以表达行为、复杂地形需要位置评分，或性能数据证明当前方案达到瓶颈时再引入。它们不是为了增加技术名词而添加。

### 6.4 第一批低风险优化

这些需要通过 10、25、50、100 敌人场景验证：

1. **使用距离平方**

   用 `SizeSquared()` 与范围平方比较，避免每次开方。当前一次更新中还存在重复计算攻击距离的机会。

2. **死亡后关闭 Actor Tick**

   当前死亡 Tick 仍会被调度，只是在函数内跳过 `UpdateEnemy()`。关闭 Tick 可以减少大量尸体的调度成本。

3. **分级更新**

   - 近距离、正在攻击：高频。
   - 中距离追逐：较低频。
   - 远距离或不可见：低频或暂停。

4. **错峰更新**

   不让所有敌人在同一帧执行感知和状态判断，降低 Game Thread 峰值。

5. **事件代替查询**

   HUD、死亡、血量和攻击完成使用 Delegate；不在 Tick 中反复读取不变状态。

### 6.5 为什么不能直接关闭所有 Tick

- 近距离攻击需要及时转向和响应。
- 降低更新频率会引入可测量的响应延迟。
- 所有敌人在同一固定 Timer 上更新仍可能形成周期峰值。
- 动画、移动组件和 NavMesh 还有独立更新成本，仅关闭 Actor Tick 不代表敌人没有成本。

### 6.6 为什么当前不优先多线程

大量 UE Actor、Component、World Query 和 Gameplay 状态属于 Game Thread 语义。直接把敌人逻辑扔到工作线程会带来 UObject 访问、同步和结果回写问题。

当前更合理的顺序是：

```text
减少工作量
-> 降低更新频率
-> 错峰
-> 事件驱动
-> 测量剩余瓶颈
-> 再判断是否需要 TaskGraph / Mass / 并行计算
```

## 7. 死亡对象与生命周期

### 7.1 当前已经做的

- 玩家死亡清理 Reload Timer。
- 敌人死亡清理 Attack Timer。
- 敌人死亡关闭攻击窗口。
- 玩家死亡停止射击和移动。
- 敌人死亡禁用移动和 Capsule。
- `SetLifeSpan` 负责最终销毁敌人。

### 7.2 下一步尸体优化

按阶段处理，不在死亡瞬间同时堆满节点：

```text
死亡瞬间
-> 停止 AI、攻击和移动
-> 需要时短暂开启 Ragdoll
-> 布娃娃稳定后停止物理
-> 关闭不必要碰撞、动画 Tick 和阴影
-> SetLifeSpan 销毁 Actor
```

需要比较：

- 保留布娃娃与停止物理后的 Physics 时间。
- Mesh Tick 开关前后的 Animation 时间。
- Cast Shadow 开关前后的 Shadow Pass。
- 连续战斗后 Actor/Object 数量能否回落。

### 7.3 为什么暂时不做通用对象池

对象池会引入重置协议：

- Timer、Delegate 和状态必须全部清空。
- Niagara、Decal、碰撞和动画必须恢复初始值。
- UObject/Actor 的生命周期语义更复杂。

如果 Spawn/Destroy 没有成为热点，池化可能只增加 Bug。先测 `SpawnActor`、GC、对象数量和内存回落，再决定是使用 UE 自带 Niagara 池、限制数量，还是池化特定高频对象。

## 8. 动画成本

100 个敌人时，HealthComponent 本身无 Tick，通常不是主要成本；Skeletal Mesh、Anim Blueprint、物理和阴影更值得测量。

优化顺序：

1. 精简 Anim Blueprint Event Graph。
2. 远处敌人降低动画更新频率。
3. 不可见敌人使用合适的 Visibility Based Anim Tick Option。
4. 死亡并稳定后暂停动画更新。
5. 根据数据选择 URO 或 Animation Budget Allocator。

Animation Budget Allocator 可以按显著性限制整体动画预算，但它需要更换/配置 Budgeted Skeletal Mesh Component，属于有侵入性的系统升级，不应在封版前未经实验直接接入。

## 9. GPU 与表现

### 9.1 命中特效

利用现有 `FHitResult` 驱动表现，不再额外做一条射线：

```text
ImpactPoint / ImpactNormal
-> Niagara
-> Decal
-> Sound
-> Physical Surface 分类
```

可优化：

- 限制同时存在的 Niagara 和 Decal 数量。
- 为特效设置明确生命周期。
- 高频射击时降低远距离或不可见特效。
- 只有 Spawn/Destroy 成本成为热点后才启用对应池化。

### 9.2 后处理

- 受伤 Timeline 结束后 Blend Weight 回到 `0`。
- 死亡后处理不能和受伤 Timeline 同时写同一个权重。
- 不使用时避免保留持续全屏 Pass。
- 比较后处理开关前后的 GPU 时间，而不是只看总 FPS。

### 9.3 阴影和 LOD

- LOD 主要降低几何和部分骨骼成本，不能代替物理、动画 Tick 和生命周期清理。
- 关闭尸体阴影可能降低 Shadow Pass，但必须保留画面验收。
- 强制 LOD 前先确认 Skeletal Mesh 实际存在多个 LOD。

### 9.4 纹理流送与显存预算

项目已经出现 `Texture Streaming Pool Over Budget` 警告，因此把纹理流送作为一项需要定量验证的优化目标，而不是直接提高 Pool 大小掩盖问题。

验证流程：

```text
stat streaming 建立 Baseline
-> 记录 Texture Pool、Required Pool 和超预算量
-> 使用 Size Map 定位高占用纹理及其引用来源
-> 检查 Max Texture Size、LOD Bias、Texture Group 和 Never Stream
-> 每次只修改一类纹理配置
-> 重新测量显存、Required Pool、超预算量和画面质量
```

优先处理：

- 尺寸明显超过实际屏幕占用的纹理。
- UI、角色、武器和环境纹理是否使用了合适的 Texture Group。
- 没有必要常驻但启用了 `Never Stream` 的纹理。
- 重复或意外被关卡引用的高分辨率资源。

验收数据：

- 固定地图、视角、分辨率和画质下的 Pool Size、Required Pool 与 Over Budget。
- 优化前后的显存占用和纹理清晰度截图。
- 是否出现模糊、频繁换入换出或近景纹理质量下降。

只有确认硬件预算确实不足且纹理规格合理后，才考虑调整 `r.Streaming.PoolSize`。在完成 Baseline 和复测前，只能表述为“发现显存预算风险并设计了验证方案”。

## 10. 调试代码本身也是成本

当前 Character Tick 每帧输出状态和弹药调试文字。少量对象时方便，但性能测试时会污染 Game Thread 数据。

正式 Benchmark 前应：

- 用调试开关包住屏幕输出和 DrawDebug。
- Shipping/Development Profile 场景保持同一配置。
- 不在一次对比中同时修改多个系统。

## 11. 优化验证方法

### 11.1 固定场景

敌人数量：

```text
10 / 25 / 50 / 100
```

每组使用相同：

- 地图和出生点。
- 玩家路径。
- 战斗时间。
- 特效开关。
- 分辨率和画质。

### 11.2 记录指标

- Game Thread 时间。
- Enemy Tick / AI 函数耗时。
- 每秒 AI 更新次数。
- 平均与最大响应延迟。
- Animation 时间。
- Physics 时间。
- GPU Frame、Shadow、Translucency、Post Process。
- Texture Pool、Required Pool、Over Budget 和显存占用。
- 初始、峰值、等待回收后的内存和对象数量。

### 11.3 实验顺序

```text
Baseline
-> 单独改变一项
-> 重复采样
-> 比较平均值和峰值
-> 检查玩法与画面回归
-> 保留或撤销方案
```

工具：

- Unreal Insights。
- `stat game`、`stat anim`、`stat memory`、`stat gpu`。
- ProfileGPU。
- `stat streaming`、Size Map、Reference Viewer。
- Memory Insights / `memreport`。
- Shader Complexity、Quad Overdraw。

没有数据前只能说“识别了风险并设计了实验”，不能说“性能提升了多少”。

## 12. 可以真实讲的经历

### 已实现，可以展开

- 将前方范围伤害演进为 AnimNotifyState + Socket 连续 Sweep。
- 使用本次攻击命中集合解决连续更新和多组件重复扣血。
- 用 HealthComponent 统一玩家和敌人的扣血与死亡广播。
- 拆分输入开始、蓝图射速调度和 C++ 单次射击。
- 换弹、死亡和攻击 Timer 的生命周期清理。
- C++ 权威状态与蓝图表现事件的职责拆分。

### 已发现但尚未完成，只能作为后续思考

- 命中后提前停止 Sweep。
- AttackSequenceId 和多窗口保护。
- 专用伤害碰撞通道与墙体阻挡。
- C++ 射速 Timer。
- AI 分级更新和错峰。
- 死亡后关闭 Actor/Skeletal Mesh Tick。
- Animation Budget Allocator。
- 特效数量预算和 GPU 数据。
- 基于证据的特定对象池。

## 13. 追问准备

1. 为什么没有直接使用武器 Overlap？
2. 连续 Sweep 的查询次数增加了多少，准备怎么测？
3. 为什么同一攻击的命中集合使用弱引用？
4. 多段攻击如何避免被“一次攻击只能命中一次”限制？
5. Montage 中断时如何保证 Notify End 和 C++ 状态收口？
6. 为什么 HealthComponent 不需要 Tick？
7. 为什么暂时不抽象 Damageable Interface？
8. 蓝图 Timer 和 C++ Timer 的取舍是什么？
9. 降低 AI 更新频率会产生什么玩法代价？
10. 为什么不直接把 AI 放到多线程？
11. 为什么 Skeletal Mesh 比 HealthComponent 更值得优化？
12. 什么证据出现后才值得做对象池？

## 14. 关联文档

- [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md)
- [Health_And_Damage_System.md](Health_And_Damage_System.md)
- [Enemy_Attack_Window.md](Enemy_Attack_Window.md)
- [Unreal Insights 官方文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-in-unreal-engine)
- [Animation Optimization 官方文档](https://dev.epicgames.com/documentation/unreal-engine/animation-optimization-in-unreal-engine)
- [Animation Budget Allocator 官方文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-budget-allocator-in-unreal-engine)

## 15. 项目工程证据总账

本节只记录本项目已经发生的事实。所有条目使用以下状态：

- **已实现**：代码或蓝图已经存在，并通过基本运行验证。
- **已测量**：存在可追溯的原始数据，测试条件能够说明。
- **待验证**：方案已经落地，但还没有同条件前后数据。
- **未实现**：只能作为候选方案，不能写入简历成果。

| 模块 | 职责和功能设计 | 关键数据结构 | 设计方式 | 当前状态 |
| --- | --- | --- | --- | --- |
| Character 与 WeaponComponent | Character 管输入意图、移动和当前装备引用；WeaponComponent 管拾取、射击、弹药、换弹及命中查询；蓝图负责动画和表现 | `EFPWeaponActionState`、`FTimerHandle`、动态多播委托 | 组件模式、状态模式、观察者模式 | C++ 已实现，仍需换弹 Montage 回归 |
| HealthComponent | 玩家和敌人复用同一套扣血、血量变化和死亡广播；组件本身不需要 Tick | 当前/最大血量、受伤/血量/死亡委托 | 组件模式、观察者模式 | 已实现 |
| EnemyCharacter 近战 | Montage 的 `AnimNotifyState` 控制攻击窗口；`WeaponTop/WeaponEnd` 双 Socket 连续 Sweep；每次攻击只结算一次 | `TSet<TWeakObjectPtr<AActor>>`、`TArray<FHitResult>`、攻击结束 Timer | 状态约束、事件驱动、集合去重 | 已实现 |
| EnemyAIController | Timer 驱动 Idle/Chase/Attack/Dead 决策；NavMesh 和 MoveTo 负责路径执行；角色只处理自身战斗行为 | `EFPEnemyAIState`、决策 Timer、玩家和管理器引用 | 状态模式、更新方法 | 已实现；已有固定数量性能数据，响应延迟仍待量化 |
| SurroundManager | 分配内外圈包围槽位、攻击名额和死亡释放，避免所有敌人争抢玩家中心点 | `TArray<FSurroundSlot>`、`TMap<TWeakObjectPtr<Enemy>, int32>`、`TSet<TWeakObjectPtr<Enemy>>` | 中央协调器、弱引用生命周期管理 | 已实现；公平性和复杂地形仍需专项验收 |
| GameMode | 统一管理波次、出生点、倒计时、存活数和胜负；敌人注册表统一接收 Death/Destroy；生成后向 AI 注入上下文 | `TArray<FfpstrueWaveConfig>`、`TSet<TWeakObjectPtr<Enemy>>`、多个 Timer 和状态委托 | 规则协调器、观察者模式、幂等事务 | 已实现；原地重开尚未实现 |
| UMG | 通过 Health、Ammo、Wave、RemainingTime 和 Result 事件更新显示，避免把业务状态放进 Widget | 动态多播委托、Widget 实例引用 | 观察者模式、表现层分离 | 接口已实现，最终 UI 闭环待回归 |
| 纹理流送 | 处理工业场景的流送预算，而不是单纯扩大 Pool | Streaming Pool、Wanted/Resident Mips、纹理组和单纹理设置 | 基于证据的资源预算 | 六张纹理定点治理已测量，减少 60 MB；VSM 告警仍是独立待办 |

### 15.1 当前核心调用链

```text
输入
-> Character 状态检查
-> WeaponComponent 射击或换弹
-> LineTrace / ApplyDamage
-> HealthComponent 结算
-> Character 或 Enemy 响应状态
-> Delegate 通知 UMG 和蓝图表现
```

```text
GameMode 启动波次
-> 从带标签的出生点分批生成 Enemy
-> AIController 获得玩家和 SurroundManager
-> Timer 触发 FSM 决策
-> SurroundManager 分配独立槽位
-> NavMesh / MoveToLocation 执行路径
-> 获得攻击名额
-> Montage / AnimNotifyState 打开攻击窗口
-> Sphere Sweep 命中玩家
```

## 16. 真实问题与优化台账

| 编号 | 现象 | 定位方式和根因 | 已做修改 | 前后数据 | 状态 |
| --- | --- | --- | --- | --- | --- |
| AI-001 | 旧敌人直接追玩家，远距离和有障碍时行为不稳定 | 运行观察和代码检查；追击、决策和角色移动耦合，没有稳定的 NavMesh 路径执行边界 | 拆出 AIController，改为 Timer 驱动 FSM 和 NavMesh MoveTo | 只有当前 Baseline，优化后待测 | 待验证 |
| AI-002 | 多个敌人向玩家中心同一点移动，产生堆叠、拥堵和重复 MoveTo | 观察所有 AI 的目标位置；根因是所有敌人共享同一个 `MoveToActor(Player)` 目标 | 增加环形槽位、NavMesh 投影、攻击名额和死亡释放 | 行为已跑通；拥堵数量和 MoveTo 次数待测 | 待验证 |
| AI-003 | 敌人数量增加后 Game Thread 成本快速增长 | CSV Profiler 分解 Game、Animation、CharacterMovement 和 Pathfinding | 已增加 AI 决策统计点；下一步只做决策降频/错峰、距离分级、动画 URO 和死亡停更中有数据支持的项 | 见第 17 节 | 已测量 Baseline |
| COMBAT-001 | 连续 Sweep 或多窗口可能在同一次挥砍中重复命中同一 Actor | 攻击窗口日志、断点和命中结果检查；一次攻击会跨多帧产生多个命中结果 | 去重集合改为整轮攻击共享，只在 `TryAttackTarget()` 开始时重置 | C++ 已编译；PIE 多窗口/重复 Notify 回归待完成 | 已实现/待回归 |
| LIFE-001 | Montage 中断或死亡后仍可能保留攻击窗口、Timer 或回调 | 沿中断和死亡路径检查状态、Timer 与 Delegate 生命周期 | `EndAttackWindow()`、Timer 清理、Delegate 解绑、死亡与结算幂等保护 | C++ 已编译；完整中断矩阵和内存回落待测 | 已实现/待回归 |
| TEX-001 | 编辑器中曾出现 Texture Streaming Pool 超预算提示 | 用固定路线独立运行 CSV 复测；运行时 `WantedMips` 约 206～213 MB，远低于 1000 MB 预算，未复现运行时超池 | 未盲目扩大 Pool，也未全局降低纹理质量；将编辑器瞬时常驻资源与运行时预算分开处理 | 见第 23 节 | 运行时已排除 |

## 17. 高密度 AI 原始 Baseline

原始文件：

```text
Saved/Profiling/CSV/FPS_Baseline_20260731/
```

提取规则：

- 使用 `fixed_10/20/40/80/160_run1.csv`。
- 只统计 `ActorCount/fpstrueEnemyCharacter` 达到目标数量后的帧。
- 当前每档只有一次运行，因此这是 Baseline，不是最终结论。
- 测试结果还不能称为“优化后提升”，必须在相同场景、分辨率、路线和时长下复测。

| 敌人数 | 有效帧 | Frame Avg ms | Frame P95 ms | Game Avg ms | Game P95 ms | GPU Avg ms | Animation ms | CharacterMovement ms | Pathfinding ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 2377 | 12.752 | 15.014 | 3.233 | - | 10.443 | 0.364 | 0.561 | 0.000 |
| 20 | 2284 | 13.256 | 15.478 | 4.178 | - | 11.059 | 0.584 | 0.846 | 0.001 |
| 40 | 2137 | 14.165 | 15.869 | 6.239 | - | 12.476 | 0.908 | 1.633 | 0.013 |
| 80 | 1907 | 15.872 | 17.523 | 11.404 | - | 14.489 | 1.713 | 3.087 | 0.026 |
| 160 | 1459 | 20.741 | 23.196 | 20.733 | - | 16.916 | 3.190 | 6.920 | 0.071 |

当前数据能够支持的结论：

1. 80 个敌人时平均帧时间仍低于 16.67 ms，但 P95 已超过 16.67 ms，不能称为“稳定 60 FPS”。
2. 160 个敌人时 Game Thread 平均 20.733 ms，已经成为主要限制。
3. `CharacterMovement + Animation` 从 10 个敌人的约 0.925 ms 增长到 160 个敌人的约 10.109 ms，是比 Pathfinding 更明确的优化对象。
4. Pathfinding 成本随数量上升，但当前数据不支持“寻路是最大瓶颈”的说法。
5. 下一轮应优先验证距离分级更新、动画 URO、死亡停更和决策错峰，而不是先上多线程或重写寻路算法。

## 18. 两个技术锚点与停止条件

项目只继续深入两个方向：

### 18.1 高密度群体 AI

完成标准：

1. 固定场景完成 10/20/40/80/100/160 敌人的同条件复测。
2. 至少完成一组有证据的 CPU 优化，并保存修改前后的 CSV 或 Insights Trace。
3. 同时记录 Game Thread、P95、Animation、CharacterMovement、Pathfinding、决策次数和响应延迟。
4. 验证目标死亡、敌人死亡、路径不可达、槽位释放和攻击名额不会残留。
5. 能解释性能收益和响应延迟之间的取舍。

达到以上标准后，不再增加行为树、EQS、复杂小队战术、AI 多线程或自定义寻路。

### 18.2 纹理池治理

完成标准：

1. 用 `stat streaming` 保存修改前的 Pool、Wanted、Resident、NonStreaming 数据。
2. 用 Size Map 和 Reference Viewer 找到实际高占用纹理及引用来源。
3. 根据画面用途调整 Max Texture Size、LOD Bias、Texture Group 和不必要的 Never Stream。
4. 在相同镜头路线下复测，确认警告消失或预算恢复，并记录画质影响。
5. Pool Size 只有在资源治理后仍有明确硬件预算依据时才允许调整。

达到以上标准后，不再继续扩展复杂显存系统或修改 UE 底层流送代码。

两个锚点完成后，只进行：

- 回归测试和打包。
- README、架构图、数据图表和 Bug 复盘。
- 演示视频和项目问答。
- 对照代码补齐 C++、UE、图形学和性能基础。

不再增加 GAS、背包、复杂行为树、EQS、通用对象池、自定义分配器或新玩法系统。

## 19. 每次修改后的文档规则

每次优化必须在同一次工作中补全：

```text
问题现象和复现步骤
-> 修改前原始数据路径
-> 定位工具和关键证据
-> 根因
-> 具体代码/资源操作
-> 修改后同条件数据
-> 性能收益、玩法代价和适用边界
```

没有前后数据时写“待验证”，不能写“显著优化”。原始 CSV、Trace、截图和日志必须保留路径，文档只保存摘要和结论。

## 20. 两个锚点的具体优化方案

### 20.1 高密度群体 AI

#### 当前已经完成，不重复改造

- Enemy Actor 已关闭自身 Tick。
- AIController 已使用 `DecisionInterval = 0.2s` 的 Timer 驱动决策。
- `PathRefreshDistance = 150cm`，目标位置没有明显变化时不会重复请求路径。
- 敌人死亡后会清理决策 Timer、停止 AI、关闭移动和 Capsule 碰撞。
- 包围槽位和攻击名额已经限制所有敌人同时挤向玩家中心。

因此下一步不重写 FSM、NavMesh 或包围算法，只处理 Baseline 暴露出的成本。

#### AI-O1：决策分级和错峰

当前所有存活敌人固定每 0.2 秒同时运行同一套决策。修改为按距离和状态选择下一次决策间隔：

| 层级 | 建议条件 | 初始间隔 | 目的 |
| --- | --- | ---: | --- |
| 近距离战斗 | 攻击范围附近、持有攻击名额、正在攻击 | 0.10s | 保证攻击响应 |
| 中距离追击 | 正在向内外圈槽位移动 | 0.25s | 保持追击稳定 |
| 远距离追击 | 距离玩家较远但仍在 ChaseRange | 0.50s | 降低无意义决策 |
| Idle | 超出 ChaseRange 或目标无效 | 1.00s | 只做低频重新检查 |

具体操作：

1. 将固定循环 Timer 改成每次决策结束后重新注册一次性 Timer。
2. 第一次触发增加 `0~当前间隔` 的随机延迟，使大量敌人的决策分散到不同帧。
3. 保留近距离高频更新，不能把所有敌人统一降到 0.5 秒。
4. 记录每秒 `STAT_fpstrueAIDecisionCount`、Game Thread 和攻击响应延迟。

预期取舍：

- 平均和尖峰 CPU 成本下降。
- 最坏响应延迟接近对应层级的决策间隔。
- 如果远处敌人出现明显停顿，先缩短远距离间隔，而不是撤销整个分级。

#### AI-O2：动画更新分级

Baseline 中 Animation 从 10 人的 0.364 ms 增长到 160 人的 3.190 ms。

具体操作：

1. 为敌人 Skeletal Mesh 开启 Animation Update Rate Optimization。
2. 不可见敌人改用基于可见性的动画 Tick 策略。
3. 只有确认 Skeletal Mesh 资产具有可用 LOD 后，才测试远距离强制较低 LOD。
4. 死亡表现结束后停止 Mesh 动画 Tick；尸体保留期间不继续更新 AnimBlueprint。
5. 对比 `stat anim`、CSV 的 `Exclusive/GameThread/Animation` 和 `Ticks/SkeletalMeshComponent`。

不能直接做的事：

- 不能让近距离正在攻击的敌人停止 Montage 更新。
- 不能在没有 Mesh LOD 的情况下直接强制 LOD。
- 不能只看 FPS，必须确认攻击通知和死亡动画没有丢失。

#### AI-O3：CharacterMovement 分级实验

Baseline 中 CharacterMovement 从 10 人的 0.561 ms 增长到 160 人的 6.920 ms，是当前最明确的 CPU 成本。

具体操作：

1. 保持近距离敌人的 CharacterMovement 每帧更新。
2. 仅对远距离敌人测试较大的 Movement Tick Interval，例如 0.05s，再根据画面决定是否尝试 0.10s。
3. 当敌人进入近距离、攻击或受击状态时立即恢复每帧更新。
4. 若蓝图启用了 RVO，分别测量始终开启与只对近距离移动者开启的成本和拥堵差异。
5. 记录移动平滑度、到达时间、卡住数量、Game Thread 和 CharacterMovement 时间。

这是一项实验，不预先认定一定保留。出现明显抖动、穿透或响应问题就撤销。

#### AI-O4：边界与响应测试

固定测试：

- 玩家死亡：所有 AI 停止 MoveTo 和攻击。
- 敌人死亡：释放槽位、攻击名额和 Timer。
- 路径不可达：MoveTo 失败后低频重试，不能每帧重新请求。
- 攻击 Montage 中断：关闭攻击窗口并释放攻击名额。
- 玩家快速移动：只有目标点超过刷新阈值才重新寻路。
- 160 敌人连续运行：槽位和攻击名额数量不持续增长。

响应延迟定义：

```text
敌人第一次进入可攻击距离的时间
-> OnAttackStarted 实际触发时间
```

记录平均值、P95 和最大值。CPU 下降但攻击延迟不可接受，优化仍然失败。

#### AI 测试顺序

```text
A0 当前 Baseline
-> A1 只加决策分级和错峰
-> A2 在 A1 上加动画 URO
-> A3 单独评估远距离 Movement Tick Interval
-> 选择效果和正确性都通过的组合
```

每组测试 10/20/40/80/100/160 敌人，每档至少运行三次。使用同一地图、出生点、镜头路线、分辨率和运行时长。

### 20.2 纹理池治理

#### TEX-O1：建立可复现 Baseline

在纹理池警告能够稳定出现的场景和镜头路线中记录：

```text
stat streaming
stat RHI
stat unit
```

同时保存：

- Texture Pool、Wanted Mips、Resident Mips、NonStreaming Mips。
- 本地 GPU 已用显存和预算。
- 警告出现的时间和镜头位置。
- 固定近、中、远三个画质对比截图。

不要先修改 `r.Streaming.PoolSize`，否则会掩盖真实资源问题。

#### TEX-O2：判断是哪一种超预算

```text
Wanted Mips 明显超过 Pool
-> 可流送纹理分辨率或纹理组预算不合理

NonStreaming Mips 很高
-> 检查 Never Stream、UI、无 Mip 纹理和其他常驻资源

Pool 正常但本地显存很高
-> 问题可能来自 VSM、Lumen、Render Target 或其他 GPU 资源，
   不能误判为 Texture Streaming
```

#### TEX-O3：定位真实高占用资产

1. 对当前地图使用 Size Map，按资源大小排序。
2. 对前十个纹理使用 Reference Viewer，确认它们为何被地图或材质引用。
3. 在 Texture Editor 检查 Imported Size、Displayed Size、Max Texture Size、LOD Bias、Texture Group、Mip Gen Settings 和 Never Stream。
4. 建立表格记录资源路径、修改前大小、屏幕用途和决定。

#### TEX-O4：按使用场景治理

优先处理：

- 小型道具仍使用 4K/8K 纹理。
- 远景或重复环境资产使用了不必要的高分辨率法线和遮罩。
- 普通世界纹理被错误放入 UI、Character 等高优先级纹理组。
- 本可流送的环境纹理开启了 Never Stream。
- 纹理没有正常生成 Mip。

可用操作：

- 设置合理的 Max Texture Size。
- 调整单纹理 LOD Bias。
- 修正 Texture Group。
- 恢复合理的 Mip Gen Settings。
- 关闭没有依据的 Never Stream。
- 清理确实未使用或错误引用的资源。

不做全项目统一降清晰度。角色、武器、近景重点物体和 UI 必须分别评估。

#### TEX-O5：同条件复测和画质回归

沿完全相同的镜头路线再次记录：

- Texture Pool、Wanted、Resident、NonStreaming。
- GPU 本地显存。
- GPU Time 和 Frame Time。
- 警告是否再次出现。

对比近、中、远截图，检查：

- 近景是否明显模糊。
- 法线细节是否丢失。
- 移动中是否出现明显 Mip 跳变。
- 斜视角地面是否闪烁。

只有资源治理后仍然超预算，并且目标显卡显存预算有明确余量时，才考虑最终调整 Pool Size。

#### 纹理治理完成标准

```text
相同路线不再出现超预算警告
+ Wanted/Resident/NonStreaming 数据可解释
+ 前十个高占用资产都有处理决定
+ 固定截图没有不可接受的画质退化
+ 保存修改前后数据和资产清单
```

## 21. 当前实验记录

### 21.1 A1：AI 决策分级与错峰

实验目的：

```text
验证在不修改 FSM、NavMesh、包围槽位、攻击窗口和动画逻辑的前提下，
通过降低非关键 AI 的决策频率并分散首次触发时间，
能否降低高密度敌人场景中的 Game Thread 平均成本和尖峰。
```

唯一修改变量：

- 固定 `0.2s` 循环 Timer 改为一次性 Timer，每次触发时根据当前状态重挂下一次决策。
- 攻击中、持有攻击名额或接近攻击范围：`0.10s`。
- 中距离追击：`0.25s`。
- 距离玩家超过 `3000cm`、但仍在 ChaseRange 内：`0.50s`。
- Idle、目标无效或超出 ChaseRange：`1.00s`。
- 第一次决策增加 `0.01s ~ 当前决策间隔` 的随机延迟，使大量敌人的更新错开。

保留不变：

- `PathRefreshDistance = 150cm`。
- 包围槽位和攻击名额算法。
- `MoveToLocation` / `MoveToActor` 的选择。
- CharacterMovement、动画更新、RVO、资源和画面设置。

实现位置：

- `Source/fpstrue/fpstrueEnemyAIController.h`
- `Source/fpstrue/fpstrueEnemyAIController.cpp`

构建验证：

```text
2026-07-31
UE 5.5 Development Editor 构建成功。
当前只证明实现可编译，性能收益待同条件复测。
```

复测矩阵：

```text
10 / 20 / 40 / 80 / 100 / 160 个敌人
每档至少 3 次
场景、出生点、镜头路线、分辨率和采样时长与 A0 Baseline 相同
```

重点指标：

- Frame Avg / P95。
- Game Thread Avg / P95。
- `STAT_fpstrueAIDecisionCount`。
- `STAT_fpstrueAIDecisionTime`。
- `STAT_fpstrueAIMoveRequestCount`。
- Animation、CharacterMovement、Pathfinding。
- 敌人进入攻击距离到 `OnAttackStarted` 的平均、P95 和最大延迟。

通过条件：

```text
高敌人数下 Game Thread 或 P95 有可重复下降
+ 决策调用次数符合分级预期
+ 近距离攻击、追击和转向无明显停顿
+ 玩家/敌人死亡、攻击中断、不可达路径、槽位和攻击名额回收全部正常
```

回退条件：

```text
连续三次复测没有稳定收益
或出现不可接受的追击停顿、攻击延迟、路径重试异常
→ 回退对应间隔，不能为了漂亮数据牺牲玩法正确性。
```

## 22. 常规 CPU 优化落地

### 22.1 为什么做

160 敌人 Baseline 中：

```text
CharacterMovement = 6.920 ms
Animation = 3.190 ms
Pathfinding = 0.071 ms
```

因此优先处理移动和动画更新，不重写 NavMesh，也不为了简历强行加入 AI 多线程。

### 22.2 修改内容

1. 敌人骨骼网格开启 Animation Update Rate Optimization。
2. 不可见敌人只继续处理 Montage，避免无意义的完整骨骼刷新。
3. CharacterMovement 按目标距离分级：
   - 20 米内：每帧更新。
   - 20～40 米：约 30 Hz。
   - 40 米外：约 15 Hz。
4. 攻击开始时临时恢复每帧移动和完整骨骼刷新，保证 Montage、Socket 和攻击窗口精度。
5. 敌人死亡后停止 CharacterMovement 更新，保留蓝图已有的死亡表现和销毁流程。

实现位置：

- `Source/fpstrue/fpstrueEnemyCharacter.h`
- `Source/fpstrue/fpstrueEnemyCharacter.cpp`
- `Source/fpstrue/fpstrueEnemyAIController.cpp`

构建验证：

```text
2026-07-31
UE 5.5 Development Editor 构建成功。
功能回归和优化后 CSV 待同条件运行验证。
```

适用边界：

- 当前项目是单机高密度近战 AI，远距离降频以视觉可接受为前提。
- 攻击窗口内不能降低骨骼更新频率，否则武器 Socket Sweep 可能漏检。
- 如果复测出现远处移动抖动，应先调高 15/30 Hz 阈值，而不是继续降低频率。

## 23. 100 AI 与纹理资源封版实验

### 23.1 实验目的与条件

本轮只回答三个问题：

1. 压测数据是否真的对应 100 个敌人。
2. 工业场景的纹理流送是否超出预算，主要资源是谁。
3. 限制明显过大的环境纹理后，显存余量和帧时间怎样变化。

固定条件：

```text
地图：/Game/FactoryDistrict/Maps/Demonstration
分辨率：1600 x 900
垂直同步：关闭
敌人数：100
镜头：固定
预热：8 秒
采集：30 秒
```

原始证据：

```text
优化前 CSV：Saved/Profiling/CSV/Profile(20260731_215052).csv
优化后 CSV：Saved/Profiling/CSV/Profile(20260731_220814).csv
优化前日志：Saved/Logs/texture_pool_baseline_100.log
优化后日志：Saved/Logs/fpstrue.log
MemReport：Saved/Profiling/MemReports/Demonstration-WindowsEditor-07.31-21.50.51
MemReport：Saved/Profiling/MemReports/Demonstration-WindowsEditor-07.31-22.08.13
```

日志与 CSV 中同时记录到：

```text
fpstrueEnemyCharacter = 100
fpstrueEnemyAIController = 100
```

因此结果不是空场景或仅配置了 `BenchmarkEnemies=100`，而是实际生成并由 Controller 驱动的 100 个敌人。

### 23.2 100 AI 最终运行数据

| 指标 | 优化前 | 资源优化后 |
| --- | ---: | ---: |
| Frame Avg | 15.23 ms | 15.41 ms |
| Frame P95 | 16.44 ms | 16.58 ms |
| Game Avg | 10.87 ms | 11.31 ms |
| GPU Avg | 13.82 ms | 13.97 ms |
| Render Avg | 14.88 ms | 15.06 ms |
| CharacterMovement Avg | 2.289 ms | 2.241 ms |
| Animation Avg | 1.475 ms | 1.666 ms |
| Pathfinding Avg | 0.021 ms | 0.022 ms |
| AI Decision Avg | 0.073 ms | 0.081 ms |

结论：

- 最终 Frame 平均约 `15.41 ms`，约为 `64.9 FPS`。
- Frame P95 约 `16.58 ms`，刚好位于 60 FPS 的 `16.67 ms` 预算内。
- 100 AI 下 Pathfinding 不是瓶颈，CharacterMovement 和 Animation 明显更贵。
- 两次帧时间差异属于运行波动，纹理规格调整没有带来可证明的帧率收益。

### 23.3 纹理定位与修改

运行时资源列表定位到六张分辨率明显偏大的环境植被纹理：

```text
IvyAtlas_A          4096 x 4096
IvyAtlas_N          4096 x 4096
PineBranchAtlas_A   4096 x 2048
PineBranchAtlas_N   4096 x 2048
PineBark_A          2048 x 4096
PineBark_N          2048 x 4096
```

没有扩大 `r.Streaming.PoolSize`，也没有设置全局 Mip Bias。只对这六张环境纹理设置：

```text
Max Texture Size = 2048
```

修改前保存了原始资产备份，并在相同 100 AI 场景中重新采集。

### 23.4 纹理优化前后数据

| 指标 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | ---: |
| Texture Pool Budget | 1000 MB | 1000 MB | 不变 |
| Streaming Current/Target | 212.27 MB | 152.27 MB | -60.00 MB |
| Pool Occupancy | 21% | 15% | -6 个百分点 |
| Over Budget | 0 MB | 0 MB | 不变 |
| GPU Local Used（辅助指标） | 2952.92 MB | 2911.52 MB | 约 -41.4 MB |

流送纹理占用下降：

```text
(212.27 - 152.27) / 212.27 = 28.3%
```

这项优化的价值是增加纹理池和显存余量，降低更高分辨率、长时间编辑会话和更多资源同时加载时的风险。它不应被描述为“FPS 提升了 X%”。

### 23.5 Texture Streaming 与 VSM 的区别

最终独立运行中：

```text
Streaming Current/Target <= 212.27 MB
Pool Budget = 1000 MB
Over Budget = 0 MB
```

因此没有复现运行时 Texture Streaming Pool 超预算。历史编辑器提示可能受到同时打开地图、资产预览和编辑器资源常驻的影响。

运行中仍出现：

```text
[VSM] Non-Nanite Marking Job Queue overflow
```

它属于 Virtual Shadow Map 的非 Nanite 阴影标记队列，不是纹理流送池。可能方向包括检查大面积非 Nanite 阴影投射物、远距离/死亡敌人的动态阴影以及 VSM 页面成本。本轮只完成问题分类，不把它伪装成已解决，也不通过扩大 Texture Pool 处理它。

### 23.6 面试表达与停止条件

可以这样描述：

> 我先用固定 100 AI 场景和 `stat streaming`、CSV、`ListStreamingTextures`、`MemReport -full` 建立基线。最终运行没有超出 1000 MB 流送预算，但定位到六张 4K/长边 4K 的环境植被纹理。保持 Pool 不变，只把它们的 Max Texture Size 限制到 2048，复测后 Streaming Current/Target 从 212.27 MB 降到 152.27 MB，下降 60 MB、约 28.3%；Frame P95 基本不变，说明这是资源预算优化，不是帧率优化。同时我区分了 Texture Streaming 告警和 VSM 阴影队列告警，避免用错误参数掩盖问题。

停止条件：

```text
100 AI 实际生成得到日志证明
+ Frame P95 位于 16.67 ms 预算内
+ Texture Pool 无 Over Budget
+ 六张高占用纹理完成定点治理
+ 优化后资源占用下降且帧时间没有明显退化
-> FPS 性能与纹理资源实验封版，不再增加新系统。
```

## 24. 弹道散布均匀采样优化

### 24.1 问题来源

武器散布的目标不是“随机偏一下枪口”，而是在准星附近形成可控、稳定、符合玩家预期的命中分布。当前项目使用 Camera LineTrace 作为真实命中，散布方向会直接影响命中概率、腰射手感、霰弹类武器的覆盖范围，以及后续准星扩散表现。

如果直接在极坐标中让半径 `r` 在 `[0, R]` 上均匀随机，视觉上会出现中心更密、边缘更稀的问题。原因是圆盘外圈面积更大，但每个半径区间获得的样本数相同，单位面积上的点数就不一致。拒绝采样可以保证均匀，但每次都要先在正方形里采样再丢弃圆外点，命中计算中没有必要引入这种额外循环。

### 24.2 优化思路

采用圆盘均匀采样：

```text
theta = Random(0, 2π)
radius = sqrt(Random(0, 1)) * SpreadRadius
x = cos(theta) * radius
y = sin(theta) * radius
```

`sqrt(Random)` 的意义是让半径分布跟圆面积匹配。圆面积随 `r^2` 增长，因此先对面积比例做均匀随机，再开平方得到半径，才能让单位面积内的样本密度更接近一致。

在项目里，`SpreadAngle` 仍然保持原来的角度配置含义。实现时把角度转换为准星前方单位平面上的圆盘半径：

```text
SpreadRadius = tan(SpreadAngle)
ShotDirection = Normalize(Forward + Right * x + Up * y)
```

这样蓝图和 DataAsset 中已有的腰射、瞄准散布角不用重新配置，真实命中仍然走原来的 `LineTraceSingleByChannel`。

### 24.3 修改位置

实现位置：

- `Source/fpstrue/fpstrueWeaponComponent.cpp`
- `UfpstrueWeaponComponent::FireSingleLineTrace`
- `MakeUniformSpreadDirection`

本次把原先的方向随机：

```text
FMath::VRandCone(Forward, SpreadAngle)
```

替换为：

```text
MakeUniformSpreadDirection(Forward, SpreadAngle)
```

保留内容：

- Camera LineTrace 命中链路不变。
- 头部和身体差异伤害不变。
- 连续射击散布叠加不变。
- 瞄准时散布降低不变。
- 调试射线仍通过测试宏控制，默认不显示。

### 24.4 预期效果

这次优化主要改善“弹道分布质量”，不是 CPU 性能优化。

预期变化：

- 腰射散布在准星圆范围内更均匀。
- 不会因为半径采样错误导致中心异常密集。
- 霰弹多射线时，每颗弹丸覆盖圆盘面积更自然。
- 保持原有角度参数，避免重新调一整套武器数值。

当前还没有保存命中点热力图或固定随机种子 A/B 数据，因此不能写成“命中率提升 X%”。更准确的表述是：弹道散布采样方式已从通用圆锥随机改为面积均匀圆盘采样，分布正确性更容易解释和验证。

### 24.5 验收方法

建议后续用固定场景验证：

```text
固定相机位置
固定靶墙距离
关闭后坐力或固定输入
连续射击 500~1000 发
记录 HitResult.ImpactPoint
对比命中点热力图
```

判断标准：

- 点云应覆盖准星圆范围。
- 中心和边缘不应出现明显非预期密度偏差。
- 瞄准模式点云半径应明显小于腰射。
- 连续射击叠加散布时，点云半径应随 `ConsecutiveShotCount` 增大并受 `MaxContinuousFireSpreadAngle` 限制；停止射击超过 `SpreadResetDelay` 后应恢复基础散布。

## 25. 架构内容已合并

架构重构过程、权威所有权、通信方式、设计模式和条件变化题已经统一到 [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md) 第 13、15、22 章。本文件只保留开发经历、Bug、实验过程和原始优化证据，避免同一架构结论维护两份。
