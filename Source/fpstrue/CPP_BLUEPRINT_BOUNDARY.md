# FPS C++ 与蓝图职责边界

## C++ 负责

- Enhanced Input 绑定、移动、视角、冲刺、瞄准和换弹规则。
- 武器拾取、附着、装备唯一性、开火输入启用和 PickupComponent 回收。
- 射击、散布、后坐力、LineTrace、命中部位和伤害结算。
- 弹药消耗、换弹结算、死亡限制和 `OnAmmoChanged` 数据广播。
- 玩家与敌人的 HealthComponent、受伤、死亡和生命周期清理。
- 敌人 AIController、NavMesh 移动、FSM、包围槽位和攻击名额。
- 攻击窗口、剑刃 Sweep、单次攻击去重和 ApplyDamage。
- GameMode 的开始、倒计时、波次、敌人生成和胜负判定。

## 蓝图保留

- Animation Blueprint、BlendSpace、Montage 和动画资产选择。
- AnimNotify/AnimNotifyState 在动画时间轴上的位置配置。
- 枪口火焰、命中特效、声音、镜头震动和后处理表现。
- UMG 布局、控件样式、动画以及对 C++ 事件的显示响应。
- 地图资产、TargetPoint、NavMeshBoundsVolume 和可编辑参数配置。

## 武器拾取后的调用链

```text
PickupComponent overlap
-> C++ 查找同一 Actor 上的 WeaponComponent
-> WeaponComponent::AttachWeapon
-> 检查角色存活、未装备其他武器、当前武器未重复装备
-> 附着到 Mesh1P 的 GripPoint
-> Character 保存武器引用并显示手臂
-> 添加开火 Input Mapping Context
-> 广播 OnAmmoChanged
-> 调用 OnWeaponEquipped 供蓝图播放动画表现
-> 禁用并销毁 PickupComponent
```

## 蓝图需要清理的旧节点

- 删除 `OnPickUp -> AttachWeapon`：装备已经由 C++ 完成。
- 删除拾取时手动显示手臂：装备成功后 C++ 自动显示。
- 删除拾取组件的碰撞关闭和销毁：C++ 已统一处理。
- 删除蓝图中的弹药加减、换弹结算、伤害和死亡判断。
- 删除敌人的旧 Tick、Timer、AI MoveTo 和手写追逐节点。

蓝图可以继续在 `OnPickUp` 或 `OnWeaponEquipped` 中设置动画类、播放声音和特效，但不得反向修改装备、弹药或生命状态。

## UI 接线原则

- 初始化时通过 Character 的 Getter 读取一次当前值。
- 后续绑定 `OnAmmoChanged`、HealthComponent 的事件和 GameMode 的事件更新控件。
- 不使用 Widget Tick 或每帧属性绑定查询核心数据。
