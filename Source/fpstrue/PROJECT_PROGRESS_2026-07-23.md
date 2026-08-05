# fpstrue 学习与面试验收记录

记录日期：2026-07-23

本文归档远端分支中独有的阶段记录。当前项目状态以 `PROJECT_PROGRESS.md` 顶部的“当前权威快照”为准。

## 1. 当前已经落地的代码

- `AfpstrueEnemyCharacter` 继承 `ACharacter`，负责敌人的目标、追击、攻击和死亡规则；`enemy_BP` 作为蓝图子类负责模型、动画和表现。
- 玩家与敌人复用 `UfpstrueHealthComponent`，组件监听 Owner 的 `OnTakeAnyDamage`，统一维护生命值并广播 `OnHealthChanged`、`OnDeath`。
- 敌人攻击已经具备距离、冷却、存活状态和攻击中状态检查。
- C++ 通过 `OnAttackStarted` 通知蓝图播放攻击表现。
- `enemy_BP` 已实现 `OnAttackStarted`，并接入两个 In-Place 攻击 Montage；Root Motion 版本暂不进入当前攻击链。
- 攻击命中帧由蓝图动画通知调用 `HandleAttackHitNotify()`，C++ 使用 `bDamageAppliedThisAttack` 保证每次攻击最多结算一次。
- C++ 使用 Sphere Sweep 检测玩家，命中后调用 `ApplyDamage`，最终由玩家的 `HealthComponent` 扣血。
- 攻击结束使用 `AttackFinishTimerHandle` 恢复攻击状态；敌人死亡时清理该 Timer、停止移动并关闭 Capsule 碰撞。
- `HealthComponent` 自身不启用 Tick，属于事件驱动组件。

## 2. 已经理解并能够继续练习的内容

- `Tick` 是逐帧更新入口，`DeltaTime` 表示两帧之间的时间；当前敌人每帧累加攻击冷却并执行目标距离与行为判断。
- 100 个敌人同时 Tick 会放大 Game Thread 开销，后续需要比较 Tick Interval、Timer、错峰和分级更新。
- 自定义 Enemy C++ 类用于补充 `ACharacter` 不具备的敌人规则；蓝图子类用于配置资产和表现，这是 UE 常见的 C++ 与 Blueprint 协作方式。
- 玩家和敌人不共用同一个 Character 类，但通过组件复用生命系统。
- 布娃娃是 Skeletal Mesh 基于 Physics Asset 的物理模拟，不是普通 Montage。
- LOD 降低网格渲染复杂度，Cast Shadow 控制物体是否参与阴影渲染；二者不能替代布娃娃物理优化。
- 死亡后应停用移动和 Tick，而不是销毁 `CharacterMovement` 默认组件。

## 3. 当前仍需接通和验证

- 在攻击 Montage 的实际命中帧添加 Notify，并调用 `HandleAttackHitNotify()`。
- 实机验证一次攻击只扣一次血，挥空不扣血，死亡或攻击中断后不再回调伤害。
- 将蓝图中的立即 `Destroy Actor` 改为合理的延迟回收，否则布娃娃效果和尸体观察时间不足。
- 验证尸体回收前的移动组件、Actor Tick、阴影和物理开销，而不是凭感觉宣称优化有效。
- 当前追击仍是 `AddMovementInput` 直线移动，尚未完成 `AIController + NavMesh`。

## 4. 后续面试官验收方式

以后不能以“项目中存在这段代码”作为掌握证明，必须逐级验收：

1. 不看代码，完整口述 `Tick -> TryAttackTarget -> OnAttackStarted -> AnimNotify -> Sphere Sweep -> ApplyDamage -> HealthComponent`。
2. 解释 `BlueprintImplementableEvent`、`BlueprintCallable`、Delegate、Timer 和 Tick 各自解决什么问题。
3. 解释为什么攻击伤害由 C++ 结算、动画由蓝图播放，以及反过来设计会有什么风险。
4. 解释为什么需要 `bDamageAppliedThisAttack`，并给出重复 Notify、Montage 中断和死亡中断的测试方法。
5. 对比 Line Trace、Sphere Sweep 和纯距离判断在近战检测中的优缺点。
6. 闭卷写出精简版 `CanAttack()`、`HandleAttackHitNotify()` 和生命值扣减逻辑。
7. 面对追问能够指出当前方案缺陷：逐帧 AI、直线追击、Timer 与 Montage 时长耦合、尸体生命周期尚未量化。

掌握等级：

- `L0`：代码存在，但无法解释。
- `L1`：看着代码能够讲清调用链。
- `L2`：不看代码能够解释、修改并排查常见错误。
- `L3`：能够设计对比实验，给出性能数据、方案取舍和回归测试。

当前目标是先把敌人攻击链达到 `L2`，完成性能实验后再达到 `L3`。
