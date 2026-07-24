# 敌人近战攻击窗口与连续剑刃检测

## 1. 目标

敌人的武器碰撞不能在整段动画中持续造成伤害。系统将近战命中拆成三层：

1. Montage 上的 `Enemy Attack Window` 决定攻击有效时间。
2. C++ 使用武器 Socket 的连续 Sweep 判断空间接触。
3. 本次攻击的已命中 Actor 集合保证同一目标只结算一次。

最终链路：

```text
Enemy 状态允许攻击
-> OnAttackStarted
-> 蓝图播放攻击 Montage
-> Enemy Attack Window Begin
-> Enemy Attack Window Tick
-> Socket 连续 Sphere Sweep
-> ApplyDamage
-> OnTakeAnyDamage
-> HealthComponent
-> 玩家受伤、血量变化或死亡事件
-> Enemy Attack Window End
-> 清理攻击窗口状态
```

## 2. C++ 如何知道帧的位置

C++ 不读取 Montage 的帧号，也不保存动画时间。

开发者在 Montage 的 Notify 轨道上放置 `Enemy Attack Window`，并拖动它的左右边界。UE 动画系统在播放到这些时间点时自动调用：

```text
进入窗口左边界 -> NotifyBegin -> BeginAttackWindow
处于窗口范围内 -> NotifyTick  -> UpdateAttackWindow
离开窗口右边界 -> NotifyEnd   -> EndAttackWindow
```

因此，帧的位置属于动画资产；检测和伤害规则属于 C++。

## 3. 剑刃双 Socket

骨骼资产在 `weapon` 骨骼上设置两个 Socket，直接描述剑刃线段：

```text
WeaponTraceStartSocketName = weapontop
WeaponTraceEndSocketName   = weaponend
WeaponTraceRadius          = 8
WeaponTraceSampleCount     = 4
```

C++ 每次动画更新直接读取两个 Socket 的世界坐标，不再依赖单个手部 Socket 和局部偏移。调试线应该与视觉剑刃重合。

## 4. 连续 Sweep

窗口开始时记录剑根和剑尖的世界坐标。

窗口内每次动画更新：

1. 读取当前剑根和剑尖。
2. 在剑根到剑尖之间生成多个采样点。
3. 每个采样点从上一帧位置 Sweep 到当前帧位置。
4. 再 Sweep 当前帧的整条剑刃线段。
5. 命中玩家后调用 `ApplyDamage`。
6. 将玩家加入 `HitActorsThisAttack`，避免后续帧重复扣血。
7. 保存当前坐标，供下一次动画更新使用。

连续 Sweep 覆盖了两帧之间经过的空间，比单帧 Overlap 更不容易在快速挥剑或低帧率下漏检。

## 5. 状态归属

`AnimNotifyState` 只转发动画事件，不保存敌人的运行时状态。

以下数据由每个 `AfpstrueEnemyCharacter` 实例独立保存：

```text
bAttackWindowActive
bHasPreviousWeaponSample
PreviousWeaponBase
PreviousWeaponTip
HitActorsThisAttack
```

原因是同一个 Montage 和 NotifyState 资产可能被多个敌人同时播放。

## 6. 编辑器操作

1. 编译 C++ 并重新启动 UE。
2. 打开一个敌人攻击 Montage。
3. 在 Notify 轨道右键。
4. 选择 `Add Notify State -> Enemy Attack Window`。
5. 将窗口左边界放到剑开始具有杀伤力的位置。
6. 将窗口右边界放到剑离开杀伤区域的位置。
7. 删除同一 Montage 中旧的单点 `Enemy Attack Hit`，避免两套检测入口并存。
8. 在 `enemy_BP` 类默认值中启用 `bDrawAttackTrace`。
9. 播放动画，观察调试线是否从 `weapontop` 连到 `weaponend`。
10. 必要时在骨骼编辑器中调整两个 Socket 的位置，或调整检测半径。

每一个随机攻击 Montage 都需要独立设置攻击窗口。

## 7. 生命周期清理

以下情况都会关闭或取消攻击窗口：

- NotifyState 正常结束。
- Montage 完成或中断后结束攻击。
- 攻击超时 Timer 触发。
- 敌人死亡。

取消时清空上一帧坐标和已命中集合，防止状态残留到下一次攻击。

## 8. 验收矩阵

| 场景 | 预期 |
| --- | --- |
| 起手时贴着玩家 | 不扣血 |
| 有效窗口内剑刃接触玩家 | 扣一次血 |
| 收招时贴着玩家 | 不扣血 |
| 玩家在窗口前离开 | 不扣血 |
| 玩家在窗口内进入剑刃轨迹 | 扣一次血 |
| 同时命中玩家 Capsule 和 Mesh | 只扣一次血 |
| 敌人在窗口内死亡 | 立即停止检测 |
| 低帧率下快速挥剑 | 连续 Sweep 仍可命中 |

## 9. 面试描述

初版使用动画命中帧触发一次 Sphere Sweep，结构简单但快速挥剑时存在漏检风险。进阶版改为 AnimNotifyState 定义攻击有效窗口，使用 `weapontop` 和 `weaponend` 的世界坐标构造剑刃线段，并对上一帧到当前帧的轨迹进行多点 Sphere Sweep。每个敌人维护本次攻击的弱引用命中集合，解决多组件重复命中和多帧重复伤害，同时在 Montage 中断、攻击超时和敌人死亡时统一清理状态。
