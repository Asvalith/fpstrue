# FPS C++ 与蓝图职责边界

## C++ 负责

- Character 统一进行 Enhanced Input 绑定、Mapping Context 生命周期管理以及移动、视角、冲刺、瞄准、开火和换弹意图转发。
- 武器拾取、附着、装备唯一性和 PickupComponent 回收。
- 射击、散布、后坐力、LineTrace、命中部位和伤害结算。
- WeaponComponent 内的弹药消耗、射速调度、换弹事务、死亡限制和 `OnAmmoChanged` 数据广播。
- 玩家与敌人的 HealthComponent、受伤、死亡和生命周期清理。
- 敌人 AIController、NavMesh 移动、FSM、包围槽位和攻击名额。
- 攻击窗口、剑刃 Sweep、单次攻击去重和 ApplyDamage。
- GameMode 的开始、倒计时、波次、敌人生成和胜负判定。

## 蓝图保留

- Animation Blueprint、BlendSpace、Montage 和动画资产选择。
- AnimNotify/AnimNotifyState 在动画时间轴上的位置配置。
- 换弹 Montage 的 Completed/Interrupted 回调，以及换弹提交 Notify 的时间点。
- 枪口火焰、命中特效、声音、镜头震动和后处理表现。
- UMG 布局、控件样式、动画以及对 C++ 事件的显示响应。
- 地图资产、TargetPoint、NavMeshBoundsVolume 和可编辑参数配置。

## 一次性事务边界

- 死亡、伤害、弹药、存活数和胜负结果以 C++ 状态为准，蓝图事件只消费结果。
- `OnDeath` 每次生命只广播一次；`ResetHealth()` 开启下一次生命事务。
- 致死伤害不会再调用普通 Damaged 蓝图表现，死亡 Montage 具有更高的生命周期优先级。
- 一轮换弹只有 `CommitReload()` 可以转移弹药，重复 Notify、旧 Timer 和已经结束的回调都会被拒绝。
- 一次有效射击只扣一发弹药，并只触发一次 `OnWeaponFirePerformed`。
- 同一轮敌人攻击的所有 AnimNotifyState 窗口共享命中集合；窗口重开不能再次伤害同一目标。
- GameMode 同时监听敌人 Death 和 Destroy，但只有首次从注册表移除时才更新存活数。
- `FinishGame()` 只结算一次，并停止敌人 AI、倒计时、波次 Timer 和攻击资源。
- `HealthComponent::OnDeath` 先进入 Character；Character 完成死亡清理后通过 `OnPlayerDeathReported` 向 GameMode 报告，这是唯一的运行中失败触发器。倒计时归零是胜利触发器，仅在玩家生命值大于 0 且未死亡时判胜。蓝图只根据 `OnGameResult` 显示结果。
- BeginPlay 使用唯一绑定，EndPlay 显式解绑 Delegate 并清理所属 Timer。

## 武器拾取后的调用链

```text
PickupComponent overlap
-> C++ 查找同一 Actor 上的 WeaponComponent
-> WeaponComponent::AttachWeapon
-> 检查角色存活、未装备其他武器、当前武器未重复装备
-> 附着到 Mesh1P 的 GripPoint
-> Character 保存武器引用并显示手臂
-> Character 已持有 DefaultMappingContext；无武器时开火输入为空操作
-> 广播 OnAmmoChanged
-> 调用 OnWeaponEquipped 供蓝图播放动画表现
-> 禁用并销毁 PickupComponent
```

## 蓝图需要清理的旧节点

- 在实际使用的 `BP_FirstPersonCharacter` 设置 `FireAction = IA_Shoot`；`IMC_Default` 已包含该 Action。
- `BP_Weapon` 不再配置 FireAction 或 FireMappingContext，也不再直接绑定玩家输入。
- `IMC_Weapons` 与 `IMC_Default` 重复映射 `IA_Shoot`，当前退出运行链路但保留资产，后续确认无引用后再决定是否清理。
- 删除 `OnPickUp -> AttachWeapon`：装备已经由 C++ 完成。
- 删除拾取时手动显示手臂：装备成功后 C++ 自动显示。
- 删除拾取组件的碰撞关闭和销毁：C++ 已统一处理。
- 删除蓝图中的弹药加减、换弹结算、伤害和死亡判断。
- 删除蓝图中的自动开火 Timer，以及由 `OnWeaponFireStarted` 再调用 `Fire` 的旧链路。
- `OnWeaponFirePerformed` 只播放单发枪口火焰、声音、曳光和 Camera Shake。
- `OnWeaponReloadStarted` 播放 Montage；弹匣插入 Notify 调用 `CommitReload`；Completed 调用 `FinishReload`；Interrupted 调用 `CancelReload`。
- 删除敌人的旧 Tick、Timer、AI MoveTo 和手写追逐节点。

蓝图可以继续在 `OnPickUp` 或 `OnWeaponEquipped` 中设置动画类、播放声音和特效，但不得反向修改装备、弹药或生命状态。

## UI 接线原则

- 初始化时从 Character 获取当前 WeaponComponent，再读取一次弹药快照；Character Getter 仅用于旧蓝图兼容。
- 后续绑定 WeaponComponent 的 `OnAmmoChanged`、HealthComponent 的事件和 GameMode 的事件更新控件。
- 不使用 Widget Tick 或每帧属性绑定查询核心数据。
