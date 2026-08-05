# fpstrue 项目阶段总结与历史记录

更新时间：2026-08-05

> 本节顶部的“当前权威快照”代表 2026-08-05 的真实状态。后续章节保留 2026-06-20 阶段的模块拆解和成长记录；若完成度或范围描述发生冲突，以当前快照、`FPS_BUG_LOG.md`、`PERFORMANCE_BASELINE.md` 和实际代码为准。

## 0. 当前权威快照

### 0.1 项目定位

`fpstrue` 的主价值是可运行、可解释的 UE5 单机 FPS Demo。它用于证明 Gameplay Framework、C++/蓝图分层、动画与命中反馈、AI 原型、生命周期治理和性能排查能力。独立多人玩法位于 `E:\ueprojrct\multiplayer`；图形学是知识与后续加分项，不把迁移来的地图、材质或动画包装成自研图形项目。

### 0.2 已成立的核心链路

```text
Enhanced Input
→ 移动 / 跳跃 / 冲刺 / 瞄准 / 射击 / 换弹
→ WeaponComponent 统一检查开火条件并扣减弹药
→ 摄像机 LineTrace 决定真实命中
→ PointDamage / 物理冲量 / HealthComponent
→ 玩家、敌人或靶子的受伤与死亡生命周期
→ 蓝图播放枪口、枪声、抛壳、曳光弹、命中粒子、贴花和 Montage
```

当前代码和蓝图已经覆盖：

- 保留地图拾枪架构，武器附加到第一人称手臂。
- 单发与长按连发、弹药、普通/空仓换弹、空枪反馈。
- LineTrace Hitscan、头部/身体伤害、物理冲量、曳光表现分离。
- 瞄准/腰射差异、连续射击散布增长、二维随机散布和后坐力。
- 通用 `HealthComponent`，玩家、敌人和 TargetDummy 复用伤害/死亡事件。
- 敌人直线追逐、攻击前摇、伤害窗口、攻击结束、死亡清理与延迟销毁。
- 动画蓝图、Montage、枪支与手臂分组件播放，以及命中材质/贴花排障。
- 一次有固定条件和数据对比的纹理驻留资源治理实验。

### 0.3 最近修复

- 连续射击散布从容易形成直线的单轴表现改为相机右/上平面的圆形随机分布，并随连续射击次数扩张。
- 敌人攻击范围增加胶囊体接触距离下限，进入攻击范围后清除残留速度，修复贴近玩家时后退且无法攻击的问题。对应提交：`78adeaa`。
- 纹理实验将 Streaming Assets 从 `212.27 MB` 降到 `152.27 MB`；这是驻留资源治理，不等同于修复内存泄漏，也不等同于自研渲染系统。

### 0.4 封版前缺口

1. 在 PIE 中保存敌人近距离攻击、连续射击、换弹/死亡互斥的回归证据。
2. 核对 HUD、胜负、重启和输入切换是否形成一局可重复闭环；缺什么只补什么。
3. 保留直线追逐的边界说明，或在确有演示阻塞时再接 AIController/NavMesh；不为了功能列表继续扩楼。
4. 整理必要 `Content / Config / Source`，排除生成目录，完成一次干净编译和启动验证。
5. 录制短演示、整理 README、性能数据、Bug 复盘和面试讲解。

### 0.5 项目边界

- FPS Demo 是游戏客户端求职的主项目之一，目标是稳定、可讲、可修改。
- Co-op 网络 Demo 是独立项目，不把它继续塞回 `fpstrue`。
- 当前没有完成两个独立图形学项目；迁移素材只作为场景和表现资产。
- GAS 计划只属于 Co-op 的进阶分支，不阻塞 FPS 或 Co-op 基础版封版。

## 1. 项目定位

> 以下开始保留 2026-06-20 的阶段性记录，用于复盘项目是如何从模板逐步形成系统边界的。

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
- 处理移动、视角、跳跃、冲刺、瞄准、换弹输入。
- 维护角色状态：
  - `Idle`
  - `Moving`
  - `Reloading`
  - `Dead`
- 管理弹药：
  - `CurrentAmmo`
  - `ReserveAmmo`
  - `MagazineSize`
- 提供武器需要查询的接口：
  - `CanFireWeapon()`
  - `TryConsumeAmmo()`
  - `CanReload()`
  - `IsReloading()`
  - `IsDead()`
  - `IsAiming()`
  - `IsFiring()`
- 接收伤害并进入死亡状态。

当前状态：

- 基础角色逻辑已成立。
- 瞄准、冲刺、换弹、死亡都已经进入 C++ 状态系统。
- 角色表现部分主要由蓝图继续完善，例如 FOV 放大、手臂动画、UI。

### 2.2 武器模块：`UfpstrueWeaponComponent`

文件：

- `fpstrueWeaponComponent.h`
- `fpstrueWeaponComponent.cpp`

主要职责：

- 作为枪支核心组件挂在 `BP_Weapon` 上。
- 被拾取后附加到角色 `Mesh1P` 的 `GripPoint`。
- 添加武器输入映射。
- 处理开火输入：
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
蓝图开始 Timer
↓
蓝图循环调用 WeaponComponent::Fire()
↓
TryConsumeAmmo()
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
- 全部死亡后下一波。
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

该内容后来已迁移为独立的 `E:\ueprojrct\multiplayer` Co-op 项目，不再作为 `fpstrue` 的封版任务。

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

### 9.2 AI 还不是正式寻路

当前敌人 C++ 是原型追逐，不是完整 AI。必须升级，否则遇到障碍物会显得很假。

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

## 11. 2026-06-22 命中贴花踩坑记录

### 11.1 问题现象

武器 LineTrace 已经能够命中敌人，`Cast To enemy_BP` 也能成功输出 `Hit Enemy`，墙体周围可以正常生成弹孔贴花，但是敌人身上没有血痕贴花。

### 11.2 已排除的问题

- 不是 `OnWeaponTraceFinished` 没触发：命中音效和粒子能正常播放。
- 不是 LineTrace 没打到敌人：`Hit Actor` 能 Cast 到 `enemy_BP`。
- 不是打到胶囊体：打印 `Hit Component` 后确认是 `CharacterMesh0`。
- 不是贴花材质本身完全失效：墙体弹孔贴花可以显示。
- 不是 `Receives Decals` 没开：敌人 Mesh 已确认打开接收贴花。

### 11.3 真正原因

敌人 Mesh 使用的材质没有响应贴花。

组件上的 `Receives Decals` 只表示这个 Mesh 允许接收贴花；真正决定贴花能不能画到表面上的，是材质里的 `Decal Response`。

如果材质的 `Decal Response` 是 `None`，即使：

```text
LineTrace 命中 Mesh
Cast 成功
Spawn Decal 节点执行
Receives Decals 已开启
```

贴花仍然不会显示。

### 11.4 正确检查路径

```text
enemy_BP
↓
CharacterMesh0
↓
Materials / 材质
↓
打开材质实例
↓
打开 Parent 父材质
↓
搜索 Decal Response / 贴花响应
↓
不能是 None
```

推荐设置：

```text
Decal Response = Color Normal Roughness
```

至少也要：

```text
Decal Response = Color
```

### 11.5 当前命中反馈正确结构

```text
OnWeaponTraceFinished
↓
Break Hit Result
↓
Hit Actor
↓
Cast To enemy_BP
```

敌人命中分支：

```text
Cast Success
↓
Spawn Decal Attached
```

关键参数：

```text
Decal Material = Blood Decal
Attach to Component = CharacterMesh0 / enemy_BP Mesh
Location = Impact Point
Rotation = Rotation From X Vector(Impact Normal)
Location Type = Keep World Position
Attach Point Name = 先空着
Decal Size = 适当放大测试
Life Span = 10
```

非敌人命中分支：

```text
Cast Failed
↓
Spawn Decal Attached
```

用于墙体、地面、箱子等普通表面的弹孔、命中音效、命中粒子。

### 11.6 这次学到的关键点

- `Hit Actor` 判断命中了谁。
- `Hit Component` 判断具体打中了哪个组件。
- `Receives Decals` 是 Mesh 层面的开关。
- `Decal Response` 是材质层面的开关。
- 墙体能显示贴花，不代表敌人材质也能显示贴花。
- 调试命中反馈时，先打印 `Hit Actor`、`Hit Component`、`Cast Success`，再查材质。

## 12. 下一阶段敌人系统任务

### 12.1 敌人被攻击掉血

目标：

```text
玩家射击敌人
↓
LineTrace 命中 enemy_BP
↓
ApplyPointDamage
↓
HealthComponent 扣血
↓
敌人播放受击反馈
```

需要确认：

- `enemy_BP` 是否挂载 `HealthComponent`。
- 武器 C++ 的 `ApplyPointDamage` 是否能打到当前敌人 Actor。
- 敌人蓝图是否监听血量变化或伤害事件。
- 敌人死亡后是否停止移动、停止攻击、关闭碰撞。

### 12.2 敌人受攻击动画

推荐做法：

```text
敌人受伤事件
↓
根据命中方向或随机选择 HitReact 动画
↓
播放受击 Montage
```

当前先做简化版：

```text
只要敌人被击中
↓
播放一个 HitReact Montage
```

后续再扩展：

```text
正面受击
背后受击
左侧受击
右侧受击
重击硬直
死亡动画
```

### 12.3 敌人攻击动画

目标：

```text
敌人接近玩家
↓
进入攻击距离
↓
停止移动或降低移动
↓
播放攻击动画
↓
动画通知帧触发伤害
```

关键点：

- 不要一进入攻击距离就立刻扣血。
- 应该由攻击动画中的 `AnimNotify` 决定真正伤害帧。
- 这样玩家看到刀挥到身上时才掉血，反馈更合理。

### 12.4 敌人伤害检测

简化版：

```text
AnimNotify_AttackHit
↓
判断玩家是否仍在攻击距离内
↓
ApplyDamage 给玩家
```

推荐先不用复杂碰撞盒，先用距离判断：

```text
Distance(Enemy, Player) <= AttackRange
```

后续再升级为：

```text
刀具碰撞盒
球形检测
扇形攻击范围
```

### 12.5 玩家死亡系统

目标：

```text
玩家 Health <= 0
↓
CharacterState = Dead
↓
禁用移动、开火、瞄准、换弹
↓
停止敌人继续伤害
↓
显示 Game Over UI
↓
允许重开
```

需要完成：

- 玩家死亡状态接入 UI。
- 死亡后禁止输入。
- 死亡后停止武器 Timer。
- 敌人攻击逻辑检查玩家是否已死亡。
- Game Over 界面。

### 12.6 建议执行顺序

```text
1. 敌人扣血验证
2. 敌人受击动画
3. 敌人死亡动画/死亡状态
4. 敌人攻击动画
5. 攻击动画 Notify 扣玩家血
6. 玩家死亡系统
7. Game Over UI
```
