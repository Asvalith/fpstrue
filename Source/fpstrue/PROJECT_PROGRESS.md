# fpstrue 项目阶段总结

更新时间：2026-08-12

## 2026-08-12 生命周期边界审计

- HealthComponent 增加每次生命只广播一次死亡的显式标记，并在 Reset 时开启新生命周期。
- 致死伤害不再进入普通 Damaged 蓝图事件，避免受击 Montage 抢占死亡表现。
- GameMode 使用弱引用敌人注册表统一接收 Death/Destroy；只有首次注销会更新存活数，游戏结束同时停止场上 AI。
- 敌人一轮攻击的所有窗口共享命中去重状态，Landed/Missed/Finished 不再因多窗口重复触发。
- Pickup 增加消费门禁；Character、Enemy、TargetDummy 和 HealthComponent 补齐唯一绑定与 EndPlay 解绑。
- `TryAttackTarget()` 收为 AIController 专用入口，防止其他 C++ 调用绕开 SurroundManager Attack Token。
- Development Editor 编译通过；重复 Notify、外部 Destroy、Montage 中断和关卡退出仍需 PIE 行为回归。
- GameMode 分离结算触发器：Character 在处理 `HealthComponent::OnDeath` 后通过 `OnPlayerDeathReported` 向 GameMode 报告，游戏中立即失败；倒计时只在归零时检查玩家仍有生命且未死亡并触发胜利，结算时解绑死亡报告。

## 0. `fps-v1` 当前权威快照

`fps-v1` 是当前作品集单机 FPS Demo 的发布分支。核心调用链已经形成：

```text
Enhanced Input
-> Character / WeaponComponent
-> WeaponDataAsset
-> LineTrace 命中判定
-> Damage / HealthComponent
-> Blueprint 动画、音效、粒子和 UI 表现
```

当前分支在模板基础上已经形成以下工程化模块：

- 通用 WeaponComponent 已支持可选 WeaponDataAsset 配置；Rifle / Shotgun 派生组件已移除。当前未发现已赋值的数据资产，正式武器仍使用组件默认值回退。
- 摄像机 Hitscan 判定与枪口视觉子弹分离，支持伤害、冲量、命中事件和表现层扩展。
- `EnemyAIController + NavMesh + SurroundManager` 的敌人寻路、环绕槽位和攻击令牌系统。
- `GameMode` 驱动的波次生成、敌人计数和胜负流程基础。
- C++ 负责规则与状态，Blueprint 负责动画、音效、粒子、贴花和 UI 的分层边界。

2026-08-05 完成两项针对 `fps-v1` 架构的修复：

- 连续射击扩散改为相机平面圆盘采样；扩散步长、上限和重置时间可由 `WeaponDataAsset` 配置，空引用时使用组件默认值。霰弹枪同一次开火的多颗弹丸共享同一级扩散。
- 敌人有效攻击距离同时考虑敌我胶囊体半径，避免模型已经接触但 Actor 原点距离仍超过 `AttackRange`，从而反复后退或无法进入攻击状态。

当前仍需完成 PIE 回归、HUD 与胜负/重开闭环、固定压力测试和最终录屏证据。迁移地图与美术素材不计作自研图形学成果；多人合作项目位于独立仓库，图形学内容保持为后续加分项。

## 1. 项目定位

`fpstrue` 是一个基于 Unreal Engine 5 的第一人称射击项目。当前目标不是只复刻教程，而是做成一个可以展示工程能力的单机 FPS 原型，并在后期加入 AI、UI、玩法循环和图形渲染亮点。

项目当前方向：

- 第一阶段：完成单机 FPS 基础玩法闭环。
- 第二阶段：加入敌人 AI、生成系统、UI 和关卡目标。
- 第三阶段：加入图形学/渲染展示点，让它从普通 FPS Demo 变成作品集项目。
- 第四阶段：再考虑多人网络，不提前把复杂度拉爆。

当前粗略进度：

- 单机 FPS 玩法 Demo：约 55%。
- 作品集级游戏引擎/图形渲染项目：约 30%。

## 2. 当前代码模块

### 2.1 角色模块：`AfpstrueCharacter`

文件：

- `fpstrueCharacter.h`
- `fpstrueCharacter.cpp`

主要职责：

- 创建第一人称角色胶囊体、摄像机、第一人称手臂 `Mesh1P`。
- 统一处理移动、视角、跳跃、冲刺、瞄准、开火和换弹输入。
- 计算 `Idle / Moving / Dead` 移动与生命状态。
- 保存当前装备引用，并把开火/换弹意图转发给 WeaponComponent。
- Character 管理包含 `IA_Shoot` 的 `IMC_Default`，在 Controller 变化和 EndPlay 时清理映射。
- 协调换弹时停止瞄准和冲刺，死亡时通知武器停用。
- 保留弹药 Getter 作为旧 HUD 的只读兼容转发，不保存弹药副本。

当前状态：

- 基础角色逻辑已成立。
- 瞄准、冲刺和死亡协调位于 Character；换弹和开火动作状态位于 WeaponComponent。
- 角色表现部分主要由蓝图继续完善，例如 FOV 放大、手臂动画、UI。

### 2.2 武器模块：`UfpstrueWeaponComponent`

文件：

- `fpstrueWeaponComponent.h`
- `fpstrueWeaponComponent.cpp`

主要职责：

- 作为枪支核心组件挂在 `BP_Weapon` 上。
- 被拾取后附加到角色 `Mesh1P` 的 `GripPoint`。
- 接收 Character 转发的 `StartFire / StopFire / RequestReload` 命令，不直接依赖 Enhanced Input。
- 独占每把武器实例的 `CurrentAmmo / ReserveAmmo / MagazineSize`。
- 维护 `Ready / Firing / Reloading / Disabled` 动作状态。
- 按 `RoundsPerMinute` 调度自动射击并执行射速门禁。
- 管理 `RequestReload / CommitReload / FinishReload / CancelReload` 换弹事务。
- 处理开火命令：
  - `StartFire()`
  - `StopFire()`
  - `Fire()`
- 使用 LineTrace 作为真实命中判定。
- 保留 Projectile 作为后期备用方案。
- 广播蓝图事件，让蓝图负责表现层。

核心事件：

- `OnWeaponFireStarted`
- `OnWeaponFireStopped`
- `OnWeaponFirePerformed`
- `OnWeaponDryFire`
- `OnWeaponReloadStarted`
- `OnWeaponReloadFinished`
- `OnWeaponTraceFinished`

当前射击链路：

```text
玩家按下左键
↓
WeaponComponent::StartFire()
↓
C++ 立即执行首发，并按配置启动 Timer
↓
WeaponComponent::Fire()
↓
WeaponComponent::TryConsumeAmmo()
↓
LineTrace
↓
ApplyPointDamage
↓
ApplyImpulse
↓
OnWeaponTraceFinished 给蓝图生成曳光弹、弹孔、命中特效
```

当前亮点：

- 命中判定和表现分离。
- C++ 负责真实射击逻辑，蓝图负责动画、声音、特效。
- LineTrace 是主逻辑，视觉子弹 `Bullet_BP` 只是曳光弹表现。
- 已有随机弹道和后坐力参数：
  - `HipFireSpreadAngle`
  - `AimFireSpreadAngle`
  - `RecoilPitch`
  - `RecoilYaw`
  - `AimRecoilMultiplier`

### 2.3 血量模块：`UfpstrueHealthComponent`

文件：

- `fpstrueHealthComponent.h`
- `fpstrueHealthComponent.cpp`

主要职责：

- 维护 `MaxHealth` 和 `CurrentHealth`。
- 监听 `OnTakeAnyDamage`。
- 广播：
  - `OnHealthChanged`
  - `OnDeath`

当前状态：

- 玩家、靶子、敌人都可以复用。
- 武器的 `ApplyPointDamage` 能通过 UE 伤害系统打到这个组件。
- 这是后面敌人 AI、玩家死亡、UI 血条的基础。

### 2.4 靶子模块：`AfpstrueTargetDummy`

文件：

- `fpstrueTargetDummy.h`
- `fpstrueTargetDummy.cpp`

主要职责：

- 用于早期验证射击、血量、死亡。
- 挂载 `HealthComponent`。
- 可选择死亡后延迟销毁。

当前状态：

- 已完成早期测试价值。
- 后续会被敌人系统取代，但可以保留作为测试靶。

### 2.5 敌人模块：`AfpstrueEnemyCharacter`

文件：

- `fpstrueEnemyCharacter.h`
- `fpstrueEnemyCharacter.cpp`

当前已经有：

- 敌人角色类。
- `HealthComponent`。
- 自动寻找玩家。
- 在追逐距离内朝玩家移动。
- 在攻击距离内按冷却扣玩家血。
- 受击显示血量。
- 死亡后停止移动、关闭碰撞、可选延迟销毁。

当前不足：

- 现在使用的是 `AddMovementInput` 朝玩家直走，不是真正 NavMesh 寻路。
- 还没有 `EnemyAIController`。
- 还没有正式攻击状态。
- 还没有动画通知控制伤害帧。
- 还没有 RVO 避让。
- 死亡只是逻辑死亡，还没完整接死亡动画和尸体处理。

当前判断：

这是一个“敌人原型”，不是最终 AI 系统。

## 3. 当前蓝图模块

### 3.1 `BP_FirstPersonCharacter`

主要职责：

- 继承 C++ 角色。
- 设置 `Mesh1P` 手臂模型。
- 设置 `PlayerAnim` 动画蓝图。
- 设置 IA 输入资产：
  - 移动
  - 视角
  - 跳跃
  - 冲刺
  - 瞄准
  - 换弹
- 处理瞄准 FOV 放大。

当前状态：

- 已经能切换瞄准状态。
- 已经能影响速度。
- 手臂姿态和武器挂点已经跑通。

### 3.2 `BP_Weapon`

主要职责：

- 包含 `TP_Weapon` 武器组件。
- 包含 `TP_PickUp` 拾取组件。
- 处理拾取后：
  - 调用 `AttachWeapon`
  - 切换手臂 Mesh/AnimBP
  - 保存 `OwningCharacter`
- 处理武器表现：
  - 射击动画
  - 枪支动画
  - 换弹动画
  - 枪声
  - 空枪声
  - 枪口火焰
  - 抛壳
  - 曳光弹
  - 命中声音
  - 命中粒子
  - 弹孔贴花

当前重要经验：

- 手臂动画应播放到 `Mesh1P` 或 `Mesh1P -> Get Anim Instance`。
- 枪支动画应播放到 `TP_Weapon`。
- `Play Montage` 的 Target 是 Skeletal Mesh Component。
- `Montage Play` 的 Target 是 AnimInstance。
- 武器蓝图负责表现层，更适合以后扩展多武器。

### 3.3 `PlayerAnim`

主要职责：

- 第一人称手臂动画蓝图。
- 读取角色速度，驱动 Idle/Move。
- 读取角色瞄准状态，切换瞄准动画。
- 读取鼠标输入，做 Weapon Sway。
- 通过 Slot 播放射击、换弹等 Montage。

当前状态：

- 射击 Montage 已经能播放。
- 换弹 Montage 已经定位到组件/AnimInstance 问题并解决方向。
- 需要继续稳定 Reload / Reload Empty 的手臂动画。

### 3.4 `Bullet_BP`

主要职责：

- 作为视觉曳光弹，不负责真实伤害。
- 从枪口生成。
- 朝 `TraceTarget` 飞行。
- 生命周期或接近目标后销毁。

正确逻辑：

```text
BP_Weapon 收到 OnWeaponTraceFinished
↓
拿到 TraceTarget
↓
从枪口 Socket 生成 Bullet_BP
↓
把 TraceTarget / Speed / Lifetime 传给 Bullet_BP
↓
Bullet_BP Tick 中朝目标移动
↓
接近目标或生命周期结束后销毁
```

当前注意点：

- Bullet 不是命中判定主体。
- Bullet 的目标位置来自 C++ LineTrace。
- Bullet 的速度必须在 Tick 移动公式中使用：

```text
DeltaLocation = Normalize(TargetLocation - ActorLocation) * Speed * DeltaSeconds
```

## 4. 已完成内容

### 4.1 武器基础

- 自动拾取武器。
- 武器附加到第一人称手臂。
- 保留拾取架构，没有退回模板式“出生自带枪”。
- 左键开火。
- 支持单点和长按连发。
- 支持弹药扣减。
- 支持弹匣和备用弹药。
- 支持换弹。
- 支持空仓换弹和普通换弹区分。
- 支持空枪声音事件。

### 4.2 射击判定

- C++ LineTrace。
- 忽略自己和武器 Owner。
- 使用 `ECC_Visibility`。
- 命中后 `ApplyPointDamage`。
- 命中物理对象后施加冲量。
- Debug Trace 已可关闭。
- 随机弹道。
- 后坐力。

### 4.3 射击表现

- 射击动画。
- 射击音效。
- 枪口火焰。
- 抛壳。
- 曳光弹。
- 命中音效。
- 命中粒子。
- 弹孔贴花。
- 水泥/墙面命中特效。

### 4.4 角色动作

- 行走。
- 冲刺切换。
- 瞄准切换。
- 瞄准减速。
- 摄像机 FOV 放大。
- 手臂 Weapon Sway。
- 手臂和枪支分离播放动画。

### 4.5 伤害系统

- 通用 HealthComponent。
- 玩家血量。
- 靶子血量。
- 敌人血量。
- 死亡事件。

### 4.6 敌人原型

- 敌人能找到玩家。
- 敌人能朝玩家移动。
- 敌人靠近后能扣玩家血。
- 敌人能被打掉血。
- 敌人死亡后停止移动。

## 5. 还未完成内容

### 5.1 敌人 AI

优先级最高。

需要完成：

- NavMesh 寻路追逐。
- 敌人移动状态。
- 攻击状态。
- 攻击动画。
- 动画通知决定伤害帧。
- 受击反馈。
- 死亡动画。
- 死亡后关闭攻击和 AI。
- 多敌人避让。

### 5.2 玩家死亡系统

需要完成：

- 玩家死亡后禁用输入。
- 停止射击和换弹。
- Game Over UI。
- 重开游戏。

### 5.3 UI

需要完成：

- 准星。
- 血量。
- 当前弹药/备用弹药。
- 换弹提示。
- 空枪提示。
- 受伤反馈。
- 击杀数。
- 波次。
- Game Over。

### 5.4 生成和玩法循环

需要完成：

- Enemy Spawn Point。
- Enemy Spawner。
- Wave Manager。
- 每波敌人数。
- 原计划为全部死亡后下一波；当前实现改为按 `WaveInterval` 推进波次，敌人死亡只更新注册表存活数。
- 胜利/失败条件。

### 5.5 图形学/渲染亮点

后期加入，不要现在打断玩法闭环。

候选方向：

- 自定义后处理描边。
- 受击红屏/暗角。
- 武器/敌人高亮扫描。
- Niagara 风格化命中特效。
- Render Target 小地图或扫描器。
- 材质驱动的弹孔/血痕变化。
- 简单非真实感渲染或局部 stylized lighting。

### 5.6 多人网络

最后做。

原因：

- 当前蓝图表现、武器事件、AI、UI 都还在快速变化。
- 提前网络同步会让调试成本暴涨。
- 单机闭环稳定后再拆 ServerFire、Multicast、RepNotify 更合理。

## 6. 当前最重要的架构原则

### 6.1 C++ 做规则，蓝图做表现

C++ 负责：

- 是否能开火。
- 是否能换弹。
- 弹药扣减。
- LineTrace 命中。
- 伤害。
- 冲量。
- 状态。

蓝图负责：

- 动画。
- 音效。
- 特效。
- UI。
- 素材替换。
- 调试表现。

### 6.2 武器表现放在 `BP_Weapon`

原因：

- 后面会有多把武器。
- 每把武器的枪声、火焰、弹壳、动画、弹道表现都不一样。
- 放在角色蓝图会让角色越来越臃肿。

### 6.3 LineTrace 是真实子弹，Bullet_BP 是视觉子弹

真实逻辑：

```text
LineTrace 决定命中、伤害、冲量。
```

视觉逻辑：

```text
Bullet_BP 从枪口飞向 TraceTarget，只负责让玩家看到曳光弹。
```

这个设计适合 FPS：

- 命中即时。
- 表现可控。
- 后期网络同步更轻。

## 7. 项目亮点

### 7.1 保留拾取架构

没有使用模板的“出生自带枪”，而是保留：

```text
地图上有武器
↓
玩家走近拾取
↓
武器附加到手臂
↓
武器开始绑定输入
```

这是比模板更真实的 FPS 架构。

### 7.2 武器系统分层清晰

当前结构：

```text
Character：角色状态、弹药、换弹、瞄准、死亡
WeaponComponent：开火、LineTrace、伤害、事件广播
BP_Weapon：动画、音效、特效、弹孔、曳光弹
Bullet_BP：视觉子弹
```

这说明项目已经不是简单蓝图堆节点，而是有系统边界。

### 7.3 命中反馈链完整

现在已经有：

```text
枪口火焰
枪声
抛壳
曳光弹
命中音效
命中粒子
弹孔
物理冲量
血量扣减
```

这已经接近一个 FPS 武器系统的完整反馈链。

### 7.4 C++ 和蓝图协作

项目体现了 UE 常见工程方式：

- C++ 提供稳定规则和接口。
- 蓝图连接素材和表现。
- 动画蓝图处理姿态。
- 组件系统复用通用能力。

### 7.5 已具备后续扩展基础

当前系统可以继续扩展：

- 多武器。
- 不同射速。
- 不同后坐力。
- 不同散布。
- 不同换弹动画。
- AI 敌人。
- UI。
- 网络同步。

## 8. 下一阶段执行顺序

### 阶段 A：敌人 AI 闭环

目标：

```text
敌人能追、能打、能死。
```

任务：

1. 创建或完善敌人蓝图，继承 `AfpstrueEnemyCharacter`。
2. 设置敌人模型和动画蓝图。
3. 地图放置 `NavMeshBoundsVolume`。
4. 将当前直线追逐升级为导航移动。
5. 设置攻击距离和攻击冷却。
6. 攻击动画通过 AnimNotify 扣玩家血。
7. 玩家射击敌人，敌人受伤死亡。

### 阶段 B：UI

目标：

```text
玩家知道自己血量、弹药、敌人击杀数和当前游戏状态。
```

任务：

1. 弹药 UI。
2. 玩家血量 UI。
3. 准星。
4. 受击反馈。
5. 击杀数。
6. Game Over。

### 阶段 C：生成与波次

目标：

```text
游戏从单个敌人测试变成有节奏的战斗。
```

任务：

1. SpawnPoint。
2. EnemySpawner。
3. WaveManager。
4. 每波敌人数量。
5. 过关/失败条件。

### 阶段 D：渲染展示点

目标：

```text
让项目具备图形学作品集价值。
```

任务：

1. 选择一个主渲染主题。
2. 做一个能截图展示的效果。
3. 写清楚原理。
4. 和玩法结合，而不是单独堆效果。

## 9. 当前风险

### 9.1 蓝图复杂度开始升高

`BP_Weapon` 已经负责很多表现逻辑。后续要注意：

- 用函数整理重复节点。
- 给蓝图加注释框。
- 每个事件只做一种事。
- 不要让表现逻辑反过来控制核心规则。

### 9.2 AI 寻路仍需编辑器验证

敌人 C++ 已从 `Tick + AddMovementInput` 改为 `EnemyAIController + MoveToActor`。目前 C++ 编译通过，但还需要清理 `enemy_BP` 中旧追击节点，并在关卡中配置 `NavMeshBoundsVolume` 后做运行验证。

### 9.3 UI 还没接入

没有 UI，项目看起来仍像调试工程。UI 是让它像游戏的关键一步。

### 9.4 渲染亮点还未开始

作为图形/引擎方向项目，后面必须补一个能解释原理的渲染模块。

## 10. 短期结论

项目现在不是“什么都没做”，而是已经完成了 FPS 最难的一块之一：武器系统。

当前真正的下一步不是继续抠枪，而是推进：

```text
敌人 AI 闭环
↓
玩家血量 UI
↓
敌人生成和波次
↓
玩法目标
↓
渲染亮点
```

只要敌人 AI 和 UI 跑通，`fpstrue` 就会从“武器测试项目”变成“可玩的 FPS 原型”。

## 11. 学习与面试验收记录（2026-07-23）

### 11.1 当前已经落地的代码

- `AfpstrueEnemyCharacter` 继承 `ACharacter`，负责敌人的攻击、伤害和死亡规则；`AfpstrueEnemyAIController` 负责目标与追击状态，`enemy_BP` 负责模型、动画和表现。
- 玩家与敌人复用 `UfpstrueHealthComponent`，组件监听 Owner 的 `OnTakeAnyDamage`，统一维护生命值并广播 `OnHealthChanged`、`OnDeath`。
- 敌人攻击已经具备距离、冷却、存活状态和攻击中状态检查。
- C++ 通过 `OnAttackStarted` 通知蓝图播放攻击表现。
- `enemy_BP` 已实现 `OnAttackStarted`，并接入两个 In-Place 攻击 Montage；Root Motion 版本暂不进入当前攻击链。
- 攻击 Montage 使用 `Enemy Attack Window` NotifyState 驱动剑刃连续 Sphere Sweep，`HitActorsThisAttack` 和 `bHitTargetThisAttack` 保证一轮攻击最多结算一次。
- C++ 对 `weapontop/weaponend` 两个 Socket 之间的刀刃轨迹跨帧补采样，命中后调用 `ApplyDamage`，最终由玩家的 `HealthComponent` 扣血。
- 攻击完成或中断由 Montage 回调调用 `HandleAttackFinishedNotify()`；`AttackFinishTimerHandle` 只做丢失回调时的超时恢复。敌人死亡时清理 Timer、攻击窗口、移动和 Capsule 碰撞。
- `HealthComponent` 自身不启用 Tick，属于事件驱动组件。

### 11.2 已经理解并能够继续练习的内容

- 旧版敌人追击依赖 `Tick` 和 `AddMovementInput`，100 个敌人同时 Tick 会放大 Game Thread 开销。
- 新版 C++ 将敌人决策移到 `AfpstrueEnemyAIController`，使用 Timer 定时更新状态，并通过 `MoveToActor` 交给 NavMesh 处理移动。
- 自定义 Enemy C++ 类用于补充 `ACharacter` 不具备的敌人规则；蓝图子类用于配置资产和表现，这是 UE 常见的 C++ 与 Blueprint 协作方式。
- 玩家和敌人不共用同一个 Character 类，但通过组件复用生命系统。
- 布娃娃是 Skeletal Mesh 基于 Physics Asset 的物理模拟，不是普通 Montage。
- LOD 降低网格渲染复杂度，Cast Shadow 控制物体是否参与阴影渲染；二者不能替代布娃娃物理优化。
- 死亡后应停用移动和 Tick，而不是销毁 `CharacterMovement` 默认组件。

### 11.3 当前仍需接通和验证

- 在正式攻击 Montage 的有效挥砍区间添加 `Enemy Attack Window` NotifyState，并删除同一 Montage 中旧的单点 `Enemy Attack Hit`。
- 实机验证一次攻击只扣一次血，挥空不扣血，死亡或攻击中断后不再回调伤害。
- 将蓝图中的立即 `Destroy Actor` 改为合理的延迟回收，否则布娃娃效果和尸体观察时间不足。
- 验证尸体回收前的移动组件、Actor Tick、阴影和物理开销，而不是凭感觉宣称优化有效。
- `enemy_BP` 中旧追击 `Tick`、Timer、AI MoveTo 或手写移动节点仍需清理。
- 关卡还需要放置和调整 `NavMeshBoundsVolume`，确认 `EnemySpawn` 点位于绿色导航区域。

### 11.4 后续面试官验收方式

以后不能以“项目中存在这段代码”作为掌握证明，必须逐级验收：

1. 不看代码，完整口述 `AIController Timer FSM -> TryAttackTarget -> OnAttackStarted -> Montage -> Attack Window -> Sphere Sweep -> ApplyDamage -> HealthComponent`。
2. 解释 `BlueprintImplementableEvent`、`BlueprintCallable`、Delegate、Timer 和 Tick 各自解决什么问题。
3. 解释为什么攻击伤害由 C++ 结算、动画由蓝图播放，以及反过来设计会有什么风险。
4. 解释为什么同时需要 `HitActorsThisAttack` 与 `bHitTargetThisAttack`，并给出跨帧重复命中、Montage 中断和死亡中断的测试方法。
5. 对比 Line Trace、Sphere Sweep 和纯距离判断在近战检测中的优缺点。
6. 闭卷写出精简版 `CanAttack()`、`HandleAttackHitNotify()` 和生命值扣减逻辑。
7. 面对追问能够指出当前方案缺陷：蓝图资产仍需完成统一接线、近战尚未增加遮挡校验、尸体生命周期和攻击查询成本仍需量化。

掌握等级：

- `L0`：代码存在，但无法解释。
- `L1`：看着代码能够讲清调用链。
- `L2`：不看代码能够解释、修改并排查常见错误。
- `L3`：能够设计对比实验，给出性能数据、方案取舍和回归测试。

当前目标是先把敌人攻击链达到 `L2`，完成性能实验后再达到 `L3`。

### 11.5 AIController 改造记录（2026-07-29）

本次目标是把敌人 AI 从角色自身的逐帧追击逻辑中拆出来，减少 `AfpstrueEnemyCharacter` 的职责，并为后续 NavMesh、状态调试和性能测试留出结构。

改造前的问题：

- 敌人每帧在 `Tick()` 中查找目标、计算距离、判断攻击并用 `AddMovementInput` 直线追逐。
- 遇到墙体或障碍时不会主动绕行。
- `Idle / Chase / Attack / Dead` 由距离和布尔值隐式表达，不方便观察和扩展。
- 敌人数量上来后，每个敌人每帧做决策会放大 Game Thread 压力。

本次 C++ 已完成：

- 新增 `AfpstrueEnemyAIController`。
- 新增 `EFPEnemyAIState`：`Idle / Chase / Attack / Dead`。
- AIController 关闭 Tick，使用一次性 Timer 调度决策；攻击、追击、远距和空闲分别采用 `0.1 / 0.25 / 0.5 / 1.0` 秒更新间隔。
- Chase 状态使用 `MoveToActor()`，后续依赖关卡 NavMesh 绕障。
- Attack 状态停止移动、面向玩家，并调用敌人已有的 `TryAttackTarget()`。
- 战斗接近使用独立的 `CombatMoveAcceptanceRadius = 15`，避免把 `AttackRange` 直接传给寻路后叠加胶囊体半径，导致敌人在攻击范围外提前停止。
- `AfpstrueEnemyCharacter` 默认设置 `AIControllerClass = AfpstrueEnemyAIController::StaticClass()`。
- `AutoPossessAI` 设置为 `PlacedInWorldOrSpawned`，支持场景放置和动态生成敌人。
- 移除敌人自身的旧 `Tick()`、`UpdateEnemy()`、`MoveTowardTarget()` 和 `AddMovementInput` 追逐职责。
- 保留原有攻击窗口、剑刃 Sweep、`ApplyDamage -> HealthComponent`、受伤/死亡和攻击蓝图事件。
- 攻击状态由 C++ 独占，Montage 回传实际时长并在完成或中断时收尾；Timer 仅负责丢失动画回调时的恢复。

已验证：

- Development Editor 编译通过。
- `UnrealEditor-fpstrue.dll` 已重新生成。
- 源码扫描确认敌人类中不再存在旧追击 `Tick()` 和 `AddMovementInput()`。

还未完成：

- 打开 `enemy_BP`，确认 AI Controller Class 和 Auto Possess AI。
- 删除敌人蓝图中的旧追击 Tick、Timer、AI MoveTo 或手写移动节点。
- 在关卡中配置 `NavMeshBoundsVolume`。
- 确认 `EnemySpawn` TargetPoint 落在绿色 NavMesh 上。
- PIE 验证 Idle、Chase、Attack、Dead 状态切换，以及玩家死亡、敌人死亡后的 AI 停止行为。
