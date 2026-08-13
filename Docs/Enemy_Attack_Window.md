# 敌人近战攻击窗口与连续剑刃检测

## 1. 当前封板架构

近战系统按“决策、时序、执行、生命”分层：

```text
EnemyAIController
-> 判断 Idle / Chase / Attack / Dead，申请攻击名额
-> EnemyCharacter::TryAttackTarget()
-> OnAttackStarted（蓝图选择并播放 Montage）
-> AnimNotifyState_AttackWindow
   -> NotifyBegin：BeginAttackWindow()
   -> NotifyTick：UpdateAttackWindow()
   -> NotifyEnd：EndAttackWindow()
-> EnemyCharacter 读取 weapontop / weaponend
-> 帧间多点 Sphere Sweep
-> 本轮攻击命中集合去重
-> ApplyDamage(Player)
-> Player HealthComponent 扣血并判断死亡
-> 玩家蓝图播放受击或死亡表现
```

职责边界：

| 模块 | 负责 | 不负责 |
| --- | --- | --- |
| `EnemyAIController` | 是否追击、是否攻击、导航和攻击名额 | 动画关键帧、Sweep、伤害数值 |
| `AnimNotifyState_AttackWindow` | 把 Montage 时间轴的 Begin/Tick/End 转发给敌人 | 保存攻击状态、查找目标、直接扣血 |
| `EnemyCharacter` | 攻击事务、Socket 轨迹、Sweep、整轮去重、中断和死亡清理 | 玩家血量规则、全局站位分配 |
| `HealthComponent` | Clamp、伤害事件和一次性死亡 | 判断武器是否命中、播放 Montage |
| 蓝图 | 选择 Montage、受击/死亡/音效/特效表现 | 决定是否允许重复伤害、保存权威攻击状态 |

目前只有敌人使用近战，因此 NotifyState 直接转发给 `AfpstrueEnemyCharacter`。现在增加通用接口或 MeleeComponent 只会多出一层调用和资产迁移成本。等玩家近战、不同近战 Actor 或可复用陷阱至少出现第二个实现者时，再提取通用合同。

## 2. 为什么使用 NotifyState

全程开启武器碰撞会在起手和收招阶段误伤；单个 Notify 只提供一个离散命中时刻，快速挥剑或低帧率下可能漏过玩家。`AnimNotifyState` 提供持续有效窗口，动画资产决定窗口位置，C++ 决定窗口内能否造成伤害。

```text
进入窗口左边界 -> NotifyBegin -> BeginAttackWindow
窗口持续期间   -> NotifyTick  -> UpdateAttackWindow
离开窗口右边界 -> NotifyEnd   -> EndAttackWindow
```

NotifyState 资产会被多个敌人共享，所以不能把运行时状态放进 NotifyState 对象。每个敌人的状态保存在自身实例中：

```text
bIsAttacking
bAttackWindowActive
bHasPreviousWeaponSample
PreviousWeaponBase / PreviousWeaponTip
HitActorsThisAttack
bHitTargetThisAttack
```

## 3. 连续剑刃 Sweep

骨骼资产的 `weapon` 骨骼上使用两个 Socket 表示剑刃线段：

```text
WeaponTraceStartSocketName = weapontop
WeaponTraceEndSocketName   = weaponend
WeaponTraceRadius          = 8
WeaponTraceSampleCount     = 4
```

攻击窗口开始时保存剑根和剑尖位置。每次动画更新时：

1. 读取当前剑根和剑尖世界坐标。
2. 沿剑刃生成 4 个采样点。
3. 每个采样点从上一更新位置 Sweep 到当前位置。
4. 补一次当前剑根到剑尖的 Sphere Sweep。
5. 只接受当前 `TargetCharacter`，并用 `HitActorsThisAttack` 去重。
6. 保存当前位置，供下一次更新使用。

这属于 Gameplay 层的连续轨迹查询，思想上接近 Sweep-based CCD，但不是 Chaos 刚体 CCD。剑刃位置来自骨骼动画 Socket，查询只在攻击窗口中执行。

## 4. 碰撞查询决定

当前正式实现保留：

```text
SweepMultiByObjectType(ECC_Pawn)
-> Ignore 攻击者自身
-> 只接受 HitActor == TargetCharacter
-> 本轮攻击命中集合去重
```

本轮不新增专用近战碰撞通道，原因是：

1. 当前玩法只有一个玩家目标，命中后还会按 `TargetCharacter` 精确过滤。
2. 同时攻击数量受攻击名额限制，尚无数据证明 Pawn 候选过滤是瓶颈。
3. 新通道需要同步修改项目配置、角色 Capsule、Mesh、敌人、门和关卡资产，会扩大今天封板的回归范围。
4. 当前没有友伤、盾牌、可破坏物或多目标横扫规则，专用语义暂时没有实际消费者。

已知边界是 `ECC_Pawn` 查询不会让 `WorldStatic` 成为阻挡结果。今天必须用薄墙和门框做 PIE 验收。如果确实出现隔墙伤害，先在结算前增加现有可见性查询或目标视线校验；只有玩法增加盾牌、阵营、可破坏物或复杂阻挡矩阵时，才重新评估专用通道。

## 5. 一次攻击事务与清理

`HitActorsThisAttack` 在 `TryAttackTarget()` 开始整轮攻击时清空，在 `FinishAttack()` 或死亡时回收。`BeginAttackWindow()` 不能清空集合，否则同一 Montage 的多个窗口会让同一目标再次扣血。

以下路径都必须收口：

- NotifyState 正常结束：关闭当前窗口和上一采样位置。
- Montage Completed/Interrupted：蓝图调用 `HandleAttackFinishedNotify()`。
- `AttackAnimationDuration` 超时：Timer 兜底调用 `FinishAttack()`。
- 敌人死亡或 EndPlay：关闭窗口、清 Timer、停止 AI 和移动。
- 玩家死亡：后续伤害由目标死亡门禁拒绝，AI 转入停止状态。

攻击期间临时使用 `AlwaysTickPoseAndRefreshBones`，避免不可见动画降级造成 Socket 不刷新；攻击结束后恢复普通动画更新策略。

## 6. 旧 Notify 的退役规则

`fpstrueAnimNotify_EnemyAttackHit` 和 `PerformMeleeHit()` 是旧的单点兼容路径。正式随机攻击 Montage 使用 `Enemy Attack Window`，不能在同一 Montage 同时保留旧 `Enemy Attack Hit`。

当前旧 Notify 仍被 `EnemyWarrior_DoubleLightAttack_InP_Montage` 资产引用。删除源码前必须先在编辑器中移除该 Notify、保存资产并执行引用检查；否则会留下失效类引用。完成资产迁移前保留代码，但不把它接入 `enemy_BP` 的正式攻击选择。

## 7. 蓝图配置

1. `enemy_BP` 的父类必须是 `AfpstrueEnemyCharacter`。
2. `OnAttackStarted` 只选择并播放一个正式攻击 Montage。
3. 每个正式 Montage 放置一个或多个 `Enemy Attack Window`，窗口覆盖剑刃真正有杀伤力的阶段。
4. Montage 的 Completed 和 Interrupted 都调用 `HandleAttackFinishedNotify()`。
5. 蓝图不能在攻击事件中直接 `ApplyDamage`，也不能重新开启武器碰撞盒做第二套伤害。
6. `OnAttackLanded/Missed/Finished` 只播放反馈或收尾表现。
7. 调试时临时打开 `bDrawAttackTrace`；验收后关闭，并备注为测试开关。

更完整的接线顺序见 [BLUEPRINT_RUNTIME_WIRING_GUIDE.md](BLUEPRINT_RUNTIME_WIRING_GUIDE.md)。

## 8. 验收矩阵

| 场景 | 预期 |
| --- | --- |
| 起手或收招时贴着玩家 | 不扣血 |
| 有效窗口内剑刃接触玩家 | 一轮攻击只扣一次血 |
| 同时扫到玩家 Capsule 和 Mesh | 只扣一次血 |
| 同一 Montage 有多个攻击窗口 | 同一目标仍只扣一次血 |
| Montage 正常完成或被打断 | 攻击状态、窗口和 Timer 全部回收 |
| 敌人在窗口内死亡 | 立即停止查询和移动 |
| 低帧率下快速挥剑 | 帧间 Sweep 仍能覆盖轨迹 |
| 玩家与敌人隔着薄墙或门框 | 当前方案必须实测并记录结果 |

## 9. 面试表述

初版使用动画命中帧触发一次 Sphere Sweep，结构简单，但快速挥剑时存在漏检风险。最终用 AnimNotifyState 定义攻击有效时间，用 `weapontop/weaponend` 构造剑刃线段，再对上一更新到当前更新的轨迹做多点 Sphere Sweep。整轮攻击共享弱引用命中集合，解决多组件、多帧和多窗口重复伤害。NotifyState 只负责时序，EnemyCharacter 持有权威攻击状态，HealthComponent 持有权威生命状态；Montage 中断、超时和死亡都有独立收口路径。当前保留 `ECC_Pawn` 是基于单目标规则和封板成本的主动取舍，不把未出现的复杂碰撞语义提前做成项目配置。
