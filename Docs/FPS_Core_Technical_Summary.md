# FPS 项目统一复习主线

> 核对基线：UE 5.5，代码、完整编译与性能实验记录截至 2026-08-16。
>
> 本文以当前 C++ 为事实来源。蓝图动画、Timer、音效和后处理属于编辑器资产配置，标记为“蓝图约定”或“需回归验证”。
>
> `AfpstrueTargetDummy` 仅保留为早期测试类，不属于当前正式战斗链路。
>
> 性能数字以 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md) 为唯一来源；活动任务和蓝图接线以 [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md) 为准。

## 0. 这份文档怎么复习

这份文件是 FPS 项目的**唯一复习入口**。其他文档只保存更详细的证据，不再各自形成一套说法。

推荐按四轮复习：

```text
第一轮：先能讲清模块职责和五条调用链
第二轮：再讲近战攻击、群体包围和生命周期三个技术难点
第三轮：用 10/20/40/80/160 AI 数据解释瓶颈、取舍和优化
第四轮：练习 28 道面试追问，严格区分已实现、待验证和仅有方案
```

面试回答统一使用这个结构：

```text
问题现象
-> 定位工具或代码入口
-> 根因
-> 方案与备选方案
-> 为什么这样取舍
-> 验证数据或边界测试
-> 当前仍存在的限制
```

禁止为了显得“高级”扩大事实：没有实现的 Toon、多人权威同步、优化后 A/B 数据，都只能作为后续方案讲。

## 1. 整体职责

```text
Enhanced Input
-> Character：全部玩家输入、移动、视角、当前武器引用和玩家生命值
-> WeaponComponent：弹药、射速、开火/换弹状态、Line Trace、散布和后坐力
-> EnemyCharacter：敌人受伤/死亡、近战攻击窗口、动画和移动性能分级
-> EnemyAIController：Idle/Chase/Attack/Dead FSM、定时决策和路径请求
-> SurroundManager：双环槽位、NavMesh 投影、攻击名额和弱引用治理
-> GameMode：波次生成、倒计时、胜负和 AI 上下文注入
-> HealthComponent：统一扣血、血量限制和死亡广播
-> Blueprint：动画、声音、特效、Camera Shake 和后处理
```

权威规则放在 C++，表现放在蓝图。蓝图不能重新计算弹药、伤害或死亡，否则会形成两套状态。

## 2. 开火与弹药

### 2.1 输入生命周期

角色在 `SetupPlayerInputComponent()` 中一次性绑定 `FireAction`：

1. `Started` 调用 `Character::StartWeaponFire()`。
2. `Completed / Canceled` 调用 `Character::StopWeaponFire()`。
3. Character 只把命令转发给当前 WeaponComponent，不检查或修改弹药。
4. `IMC_Default` 已包含 `IA_Shoot`；没有装备武器时转发函数直接返回，不额外注册重复的武器 Mapping Context。
5. WeaponComponent 不引用 InputAction、MappingContext 或 EnhancedInputComponent。

当前职责拆分：

- `StartFire()`：进入按住开火状态并广播 `OnWeaponFireStarted`。
- `Fire()`：执行一次真实射击尝试。
- `StopFire()`：退出开火状态并广播 `OnWeaponFireStopped`。

### 2.2 连续射击链路

```text
按下开火
-> Character::StartWeaponFire
-> EquippedWeaponComponent::StartFire
-> WeaponActionState = Firing
-> 立即调用一次 Fire
-> Automatic 武器按 RoundsPerMinute 启动 C++ Timer
-> Timer 循环调用 WeaponComponent::Fire
-> 松开、取消、换弹、空仓或死亡
-> C++ 清理 Timer
-> StopFire / OnWeaponFireStopped
```

重要边界：

- `StartFire()` 同时建立开火状态、立即尝试首发，并按武器配置决定是否启动连续射击。
- `Fire()` 仍代表一次射击尝试，但只在 `Firing` 状态中执行，并用时间门禁拒绝快于配置射速的重复调用。
- `StopFire()`、换弹、死亡和 `EndPlay()` 都会清理自动开火 Timer。
- 旧蓝图 `Fire` 节点暂时保留为弃用兼容入口；蓝图应删除自己的射速 Timer，避免双调度源。

### 2.3 单次射击

`UfpstrueWeaponComponent::Fire()` 的顺序：

```text
CanFire
-> 获取 World 和第一人称 Camera
-> 检查本 WeaponComponent 的射速门禁
-> WeaponComponent::TryConsumeAmmo
-> 失败：OnWeaponDryFire + StopFire
-> 成功：OnWeaponFirePerformed
-> FireLineTrace
-> 添加 Pitch / Yaw 后坐力
```

弹药在 Line Trace 之前消耗，所以有效开火即使没有命中也会减少一发子弹。

`CanFire()` 当前检查：

- Character 有效。
- Character 存在 Controller。
- 玩家未死亡。
- 武器已经装备且 Gameplay 已启用。
- WeaponActionState 不是 Reloading 或 Disabled。
- 当前弹匣有子弹。

空仓按下开火会广播 `OnWeaponDryFire` 并尝试进入换弹；真正的扣弹只发生在 WeaponComponent 内。

### 2.4 弹药所有权

每个 `UfpstrueWeaponComponent` 实例拥有自己的运行时弹药：

```text
MagazineSize = 30
CurrentAmmo = 30
ReserveAmmo = 90
```

`TryConsumeAmmo()` 是 WeaponComponent 私有规则入口：

1. `CanFire()` 先统一检查装备、死亡、状态和弹药。
2. 射速门禁拒绝同一帧或过快的重复调用。
3. 接受本次射击后执行 `CurrentAmmo--` 并广播 `OnAmmoChanged`。

Character 不再保存弹药副本。它只保存当前装备引用；现有 `GetCurrentAmmo / GetMagazineSize / GetReserveAmmo` 是兼容旧 HUD 的只读转发，不能作为新的状态所有者。

### 2.5 武器配置扩展边界

> 状态：**数据接口已接入，但未发现已创建并赋值的 WeaponData 资产；当前正式武器走组件默认值回退。**

项目曾验证 Rifle/Shotgun 派生组件的可行性。由于两者唯一差异只是每次开火的射线数量，继续保留派生类会增加类型和蓝图迁移成本。现在统一由 `UfpstrueWeaponComponent` 执行开火；代码支持在赋值 `UfpstrueWeaponDataAsset` 后通过 `WeaponFamily` 与 `PelletsPerShot` 决定单射线或多射线，空引用则使用组件默认参数。

当前关系如下：

```text
UfpstrueWeaponComponent（通用开火和命中规则）
├── Rifle：单条射线
└── Shotgun：按 PelletsPerShot 产生多条射线

UfpstrueWeaponDataAsset（静态配置）
├── 弹匣、备弹和换弹时间
├── 射程、冲量和身体/头部伤害
└── 腰射/瞄准散布、后坐力和霰弹数量

UfpstrueWeaponComponent（每把武器的实例运行状态）
└── CurrentAmmo、ReserveAmmo、ActionState、Timer 和换弹序列

AfpstrueCharacter（玩家协调）
└── 输入意图、移动/瞄准、生命和当前装备引用
```

该方案区分两类数据：

- DataAsset 保存多实例共享、可在编辑器配置的静态参数。
- WeaponComponent 保存当前弹药、换弹和装备状态，不能写回共享 DataAsset。

拾取配置调用链：

```text
OnPickUp
-> WeaponComponent::AttachWeapon(TargetCharacter)
-> WeaponComponent::InitializeRuntimeState
-> 从 WeaponData 初始化本武器的弹匣和备弹
-> SetEquippedWeaponComponent
-> 广播初始 OnAmmoChanged
```

数据驱动的多射线调用链：

```text
WeaponComponent::Fire
-> FireLineTrace
-> GetTraceCount
   ├── Rifle = 1
   └── Shotgun = WeaponData.PelletsPerShot（默认 8）
-> 循环调用 FireSingleLineTrace
-> 每条射线独立计算散布、命中和伤害
```

如果未来确实需要步枪、霰弹枪等多种武器，继续扩展该方案的依据是：

1. **配置与规则分离**：增加武器时优先创建配置资产，不必复制伤害、散布和后坐力代码。
2. **在窄入口隔离差异**：基类保留公共射击链，只在 `GetTraceCount()` 根据数据决定射线数量，避免复制整套射击逻辑。
3. **渐进迁移**：基类支持可选 DataAsset，并保留原参数回退；确认第二种武器确有需求后，再增加真正不同的行为策略。
4. **控制范围**：即使启用，也只解决“单射线/多射线 + 数据配置”，不顺带引入武器工厂、背包或 GAS。

当前正式版边界：

- 蓝图统一使用 `UfpstrueWeaponComponent`，不再替换 Rifle/Shotgun 派生组件。
- 未配置 WeaponData 时继续回退组件默认参数，保持现有蓝图兼容。
- 当前没有正式配置第二把武器，因此不能把数据接口描述为已经完成的多武器玩法。
- 只有出现换弹、射击模式或弹药规则明显不同的武器时，才考虑增加新的行为策略，而不是仅因参数不同创建派生类。

未来启用时需要重点验证：

| 现象 | 根因 | 检查与处理 |
| --- | --- | --- |
| 修改 DataAsset 后参数没有变化 | 蓝图仍使用基类组件，或 WeaponData 没有赋值 | 检查组件真实 Class 和 WeaponData 引用；空引用会按设计回退旧参数 |
| Shotgun 仍只产生一条射线 | WeaponData 未赋值、WeaponFamily 不是 Shotgun，或 PelletsPerShot 配置错误 | 检查组件的 WeaponData 引用与数据资产字段 |
| 拾取后弹药被重置 | 新武器实例第一次 `AttachWeapon` 会执行 `InitializeRuntimeState` | 运行时状态只初始化一次；未来切枪时继续保留各武器实例自己的弹药 |
| 多个武器实例互相修改配置 | 错把运行状态写进共享 DataAsset | DataAsset 只读；CurrentAmmo 等状态只保存在实例对象中 |
| 旧蓝图突然失效 | 不是 DataAsset 的必然结果，优先检查拾取事件 Target、组件类和 AttachWeapon 返回值 | 按 `OnPickUp -> AttachWeapon -> Branch` 逐段断点，不先重接整张蓝图 |

## 3. 换弹

### 3.1 换弹条件

`CanReload()` 在以下情况返回 `false`：

- 已经处于 `Reloading`。
- 已经死亡。
- 当前弹匣已满。
- 备用弹药小于等于 `0`。

因此重复按换弹键、满弹换弹和死亡后换弹都被统一入口拦截。

### 3.2 换弹开始

```text
Reload Input Started
-> Character::StartReload
-> Character::RequestReload
-> WeaponComponent::CanReload
-> Character 停止瞄准和冲刺
-> WeaponComponent::RequestReload
-> StopFire 并清理自动开火 Timer
-> 判断是否为空仓换弹
-> WeaponActionState = Reloading
-> ReloadSequence++ / bReloadAmmoCommitted = false
-> OnWeaponReloadStarted(bWasEmptyReload)
-> 设置带本轮序列号的超时 Timer
```

未配置 WeaponData 时的回退时间：

- 普通换弹：`0.8` 秒。
- 空仓换弹：`1.2` 秒。

蓝图通过 `bWasEmptyReload` 选择对应 Montage。C++ 状态从换弹开始起就拒绝开火，因此 Montage 被表现层打断也不会直接绕过武器规则。

### 3.3 换弹结算

推荐由弹匣插入位置的 AnimNotify 调用 `CommitReload()`：

```cpp
AmmoNeeded = MagazineSize - CurrentAmmo;
AmmoToLoad = Min(AmmoNeeded, ReserveAmmo);
CurrentAmmo += AmmoToLoad;
ReserveAmmo -= AmmoToLoad;
```

`bReloadAmmoCommitted` 保证同一轮只提交一次。Montage 正常完成调用 `FinishReload()`；中断调用 `CancelReload()`。如果蓝图没有回调，带序列号的 C++ Timer 会调用 `FinishReload()` 作为超时恢复，旧 Timer 无权结束新一轮换弹。

正常完成后：

1. 尚未提交时由兜底路径提交一次弹药。
2. `WeaponActionState` 恢复 `Ready`。
3. 广播 `OnWeaponReloadFinished`。

玩家死亡会清理自动开火和换弹 Timer、使序列号失效并把武器置为 `Disabled`，因此死亡后旧回调不会补弹或恢复开火。

### 3.4 当前换弹边界

- C++ 事务接口已经完成，但武器蓝图仍需连接 AnimNotify、Montage Completed 和 Interrupted，完成前只能依赖超时 Timer。
- `CommitReload()` 发生后再中断，弹药不会回滚；这表示弹匣插入时刻是 Gameplay 提交点，Notify 位置必须与动画语义一致。
- 需要在 PIE 覆盖普通换弹、空仓换弹、连续按键、开火中请求换弹、换弹中死亡和 Montage 被替换。

## 4. Line Trace

### 4.1 射线生成

`FireLineTrace()` 使用第一人称 Camera：

```text
Start = Camera World Location
Forward = Camera Forward Vector
Spread = 瞄准或腰射散布角
ShotDirection = UniformDiskSpread(Forward, Spread)
End = Start + ShotDirection * LineTraceRange
```

当前参数：

- 射程：`10000`。
- 腰射散布：`1.5` 度。
- 瞄准散布：`0.25` 度。

散布方向使用圆盘均匀采样：在准星前方单位平面上生成 `sqrt(Random)` 半径和 `0~2π` 角度的随机偏移，再把 `Forward + Offset` 归一化为射线方向。这样散布面积更接近准星圆内均匀分布，避免直接均匀随机半径造成中心过密。

### 4.2 查询规则

```cpp
FCollisionQueryParams QueryParams;
QueryParams.AddIgnoredActor(Character);
QueryParams.AddIgnoredActor(GetOwner());
QueryParams.bTraceComplex = true;
```

`FCollisionQueryParams` 是本次场景查询的参数集合，不是碰撞体：

- 忽略玩家 Character，避免从相机发出的射线命中自己。
- 忽略武器组件所属 Actor，避免命中枪。
- 使用复杂碰撞查询。

随后执行：

```cpp
LineTraceSingleByChannel(..., ECC_Visibility, QueryParams);
```

场景对象必须正确响应 `Visibility` 通道，否则射线会穿过它。

### 4.3 命中结果

Line Trace 返回一个 `FHitResult`，核心信息包括：

- 命中的 Actor 和 Component。
- `ImpactPoint`。
- `ImpactNormal`。
- `BoneName`。

无论命中与否，C++ 都会广播：

```text
OnWeaponTraceFinished(
    bHit,
    TraceStart,
    TraceEnd,
    TraceTarget,
    HitResult
)
```

蓝图可据此生成曳光、弹孔、粒子和命中声音。真实命中由 Line Trace 决定，视觉子弹不参与伤害判定。

### 4.4 敌人伤害

当前正式规则只对：

```cpp
Cast<AfpstrueEnemyCharacter>(HitResult.GetActor())
```

成功的 Actor 结算伤害。

```text
BoneName == head 或 neck_01 -> 100
其他敌人部位                 -> 40
非 EnemyCharacter           -> 不扣血
```

然后调用 `UGameplayStatics::ApplyPointDamage()`。

因此准确描述是：

> 当前武器只对 `AfpstrueEnemyCharacter` 结算射击伤害，而不是对所有“有 HealthComponent 的对象”自动扣血。

`AfpstrueTargetDummy` 不在当前正式射击伤害链路中，只是早期测试遗留。

### 4.5 物理冲量

物理冲量与敌人伤害是两条独立分支：

```text
HitComponent IsSimulatingPhysics
-> AddImpulseAtLocation
```

所以非敌人物体虽然不扣血，只要开启物理模拟，仍可能被子弹推动。

### 4.6 后坐力

每次有效射击完成后：

- Pitch 使用固定上抬量。
- Yaw 在左右范围中随机。
- 瞄准时乘 `AimRecoilMultiplier`。

当前参数：

```text
RecoilPitch = 1.0
RecoilYaw = 0.4
AimRecoilMultiplier = 0.5
```

## 5. HealthComponent

### 5.1 组件职责

`UfpstrueHealthComponent` 是无 Tick 的通用 ActorComponent：

- 保存 `MaxHealth` 和 `CurrentHealth`。
- 在 BeginPlay 时重置生命值。
- 绑定 Owner 的 `OnTakeAnyDamage`。
- 拒绝负伤害、零伤害和死亡后的重复伤害。
- 将生命值限制在 `[0, MaxHealth]`。

### 5.2 事件顺序

```text
ApplyDamage / ApplyPointDamage
-> Owner::OnTakeAnyDamage
-> HealthComponent::HandleOwnerTakeAnyDamage
-> ApplyDamageInternal
-> OnDamageReceived(AppliedDamage, DamageCauser, InstigatedBy)
-> OnHealthChanged(CurrentHealth)
-> CurrentHealth == 0 时 OnDeath
```

致死伤害会先广播受伤和血量变化，再广播死亡。玩家和敌人的蓝图都必须让死亡表现拥有最高优先级。

### 5.3 当前使用者

- 玩家 Character：监听受伤、血量变化和死亡。
- Enemy Character：监听受伤和死亡。
- TargetDummy：拥有组件，但仅保留为测试类；当前武器不会对它调用 `ApplyPointDamage`。

## 6. 玩家受伤与死亡

玩家绑定：

```text
OnHealthChanged  -> OnPlayerHealthChanged
OnDamageReceived -> OnPlayerDamaged
OnDeath          -> CharacterState = Dead -> OnPlayerDied
```

玩家死亡的 C++ 清理：

- 防止重复死亡。
- 停止冲刺和瞄准。
- 停止射击并通知武器。
- 清理换弹 Timer。
- 停止并禁用移动。
- 调用蓝图死亡事件。

蓝图只播放声音、Camera Shake、后处理和死亡动画，不重新判断死亡。

伤害、死亡与回收中的实战问题见第 19.4 和 19.8 节。

## 7. 敌人近战攻击

### 7.1 当前 AI 事实

当前已经实现 `AfpstrueEnemyAIController`、NavMesh 路径跟随和显式 FSM：

```text
Idle
-> 获取并校验玩家目标
-> Chase
-> 请求 SurroundManager 槽位
-> NavMesh 投影
-> MoveToLocation
-> 到达内圈后申请攻击名额
-> Attack
-> 攻击结束或名额释放后回到 Chase
-> 玩家或敌人死亡时进入 Dead 并停止移动
```

控制器使用一次性 Timer 调度决策，而不是让敌人 Character 每帧做完整 AI 决策：

```text
Attack = 0.1 s
Chase  = 0.25 s
Far    = 0.5 s
Idle   = 1.0 s
```

路径目标移动超过 `150 cm`、当前没有目标或 PathFollowing 进入 Idle 时才重新提交 MoveTo，避免每次决策都重算路径。当前没有使用行为树、EQS 或 AI Perception，不能把它们写成已完成。

### 7.2 群体包围和攻击节奏

`AfpstrueSurroundManager` 使用中央槽位管理：

```text
内圈：8 个槽位，半径 250 cm
外圈：12 个槽位，半径 430 cm
同时攻击：最多 2 个
```

数据结构：

```text
TArray<FSurroundSlot>：稳定保存槽位
TMap<TWeakObjectPtr<Enemy>, int32>：敌人到槽位的快速索引
TSet<TWeakObjectPtr<Enemy>>：当前攻击者集合
```

分配时先找距离敌人最近、空闲并且能够投影到 NavMesh 的内圈槽位，再考虑外圈。敌人死亡或引用失效后释放槽位；内圈释放时，从外圈选择距离该内圈位置最近的占用者补位。攻击名额有 `4 s` 超时，防止卡住的敌人永久占用。

这套系统解决的是“每个敌人应该去哪”，NavMesh 解决“怎么绕障碍到达”，RVO/Detour Crowd 解决“移动中如何局部避让”。三者不是同一个问题。

### 7.3 攻击窗口

```text
C++ CanAttack / TryAttackTarget
-> bIsAttacking = true
-> Blueprint OnAttackStarted
-> 蓝图播放攻击 Montage
-> AnimNotifyState NotifyBegin
-> BeginAttackWindow
-> NotifyTick
-> 读取 weapontop / weaponend
-> 4 个刀刃采样点进行帧间 Sphere Sweep
-> 当前 WeaponTop 到 WeaponEnd 再做一次线段 Sweep
-> 本次攻击命中集合去重
-> ApplyDamage(Player, 10)
-> NotifyEnd / Montage 结束 / Timer 兜底
-> FinishAttack
```

C++ 定义 Notify State 的行为；Montage 资产决定攻击窗口的位置和长度。

最终职责边界：`EnemyAIController` 决定是否攻击，`AnimNotifyState` 只转发动画时间，`EnemyCharacter` 独占 Socket Sweep、整轮去重和攻击收口，`HealthComponent` 独占扣血与一次性死亡，蓝图只选择 Montage 和播放表现。当前只有敌人使用近战，因此不为一个实现者额外引入通用攻击接口或 MeleeComponent。

正式查询保持 `SweepMultiByObjectType(ECC_Pawn)`，随后以 `HitActor == TargetCharacter` 精确过滤，并由 `HitActorsThisAttack` 保证整轮攻击只结算一次。本轮不新增专用近战碰撞通道：当前没有友伤、盾牌、可破坏物或多目标横扫规则，也没有数据证明 Pawn 候选过滤是瓶颈。已知的 WorldStatic 不参与阻挡问题通过薄墙/门框 PIE 用例记录；若实际复现隔墙伤害，优先增加已有通道的视线校验，再根据新增玩法决定是否升级碰撞语义。

旧的 `Enemy Attack Hit` 单点 Notify 只作为资产兼容路径保留。正式 Montage 统一使用 `Enemy Attack Window`，同一 Montage 不能同时连接两套伤害入口。

攻击窗口的漏检、重复命中与替换条件见第 19.4 节。

## 8. Unreal 命名规范

| 前缀 | 含义 |
| --- | --- |
| `A` | 继承自 `AActor` |
| `U` | 继承自 `UObject`，包括 ActorComponent |
| `F` | 普通结构体和多数非 UObject 类型 |
| `E` | 枚举 |
| `I` | Unreal 接口类 |
| `T` | 模板类型 |
| `S` | Slate Widget |
| `b` | 布尔变量 |

前缀由类型体系决定，不是随意添加。例如：

- `AfpstrueCharacter` 继承 `ACharacter`，所以使用 `A`。
- `UfpstrueWeaponComponent` 继承 `USkeletalMeshComponent`，所以使用 `U`。
- `FHitResult` 是普通反射结构体，使用 `F`。
- `EFPCharacterState` 是枚举，使用 `E`。

当前项目类名在前缀后的大小写不完全符合常见 UE 风格，但重命名 UCLASS 会影响蓝图父类和资产引用。冲刺阶段不要为了外观统一直接重命名；若必须重命名，需要 Core Redirect 和完整资产回归。

## 9. Unreal 反射系统

示例：

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay")
USoundBase* FireSound;
```

`UPROPERTY` 不只是“暴露到蓝图”，它让 Unreal 获得这块成员的元数据。根据 Specifier，不同能力包括：

- Details 面板编辑。
- 蓝图读取或写入。
- 序列化和默认值保存。
- UObject 引用的 GC 跟踪。
- 复制、配置或保存系统接入。

常用宏：

```text
UCLASS      -> 注册 UObject 类
USTRUCT     -> 注册结构体
UENUM       -> 注册枚举
UPROPERTY   -> 注册成员属性
UFUNCTION   -> 注册函数
GENERATED_BODY -> 插入生成代码入口
```

构建流程：

```text
头文件包含反射宏
-> Unreal Build Tool 判断是否需要运行 UHT
-> Unreal Header Tool 解析反射声明
-> 生成 .generated.h 和 .gen.cpp
-> C++ 编译器编译项目代码和生成代码
-> 类型与成员注册到 Unreal 反射系统
-> 编辑器、蓝图、序列化和 GC 使用这些元数据
```

规则：

- `xxx.generated.h` 必须是该头文件最后一个 `#include`。
- 没有 `UPROPERTY` 的普通变量仍可被 C++ 使用，但不会自动获得编辑器、蓝图和序列化能力。
- UObject 指针若需要由该对象持有并接受 GC 跟踪，应使用 `UPROPERTY`；UE5 新代码通常优先使用 `TObjectPtr<T>` 表达持有关系。
- `EditAnywhere` 控制编辑器可编辑范围；`BlueprintReadWrite` 控制蓝图读写，它们不是同一件事。

## 10. 当前已确认与未确认

### 代码已确认

- C++ 角色移动状态：`Idle / Moving / Dead`；武器动作状态：`Ready / Firing / Reloading / Disabled`。
- WeaponComponent 独占弹药、换弹事务、射速门禁和自动开火 Timer。
- C++ 单次射击、连续射击调度、散布、Line Trace、敌人伤害和后坐力。
- 忽略玩家与武器 Owner 的查询参数。
- 玩家与敌人 HealthComponent 链路。
- 敌人连续剑刃 Sweep 和单次攻击去重。
- 玩家与敌人死亡后的基础 C++ 清理。
- WeaponComponent 中测试射线、屏幕打印和无效调试属性已经移除；正式 Trace 结果只通过事件交给蓝图表现层。

### 蓝图与发布需回归验证

- 全量蓝图编译已经达到 `0 errors / 0 warnings / 0 failed to load`；继续回归实际运行蓝图中的射击、普通/空仓换弹、瞄准恢复、攻击窗口、死亡表现和 UI 初始值。
- 确认 `OnWeaponFirePerformed` 每次只触发一轮枪口、声音、曳光和 Camera Shake 表现。
- 确认 Montage 的 Commit、Completed、Blend Out 和 Interrupted 出口都只完成一次状态收尾。
- 完成 Shipping Cook/Package，并在打包产物中验证启动、输入、战斗、AI、胜负和退出。

### 当前已实现并有代码或数据证据

- AIController + NavMesh + `Idle / Chase / Attack / Dead` FSM。
- 双环包围槽位、攻击名额、死亡释放和外圈补位。
- 10/20/40/80/160 敌人最终固定矩阵：40 敌人约 61 FPS，80 敌人约 55 FPS，160 敌人约 35.83 FPS。
- Animation URO、不可见对象动画降级、CharacterMovement 距离分级代码。
- 纹理流送定点治理：六张环境纹理限制到 2048，Streaming Current/Target 从 212.27 MB 降至 152.27 MB，下降 28.3%。
- 80 敌人生命周期回收：等待 35 秒后 Enemy Actor 和 GameMode 注册数从 80 回到 0，UObject 减少 887。
- 已区分 Texture Streaming 与 VSM 页面标记告警；正式矩阵 Texture Pool 警告为 0，VSM 在 80 敌人档保留单次但可复现的残留风险。

### 已有接口或实现，但仍需最终回归/证据

- HUD、波次、倒计时、胜负和重启的最终编辑器回归。
- 当前最终矩阵已经建立容量基线，但没有同一代码版本下逐项关闭 LOD/URO/Movement 分级的全档 A/B，不能把新旧矩阵差异宣称为优化收益。
- 敌人进入攻击范围到真正起手的平均、P95 和最大响应延迟。
- 连续多轮战斗的长期对象曲线；当前只有一次 80 敌人统一死亡与 35 秒回收证据。
- Niagara、Decal 和后处理的分项 GPU 成本报告。

### 尚未实现，不能写进“已完成”

- 行为树、EQS、AI Perception。
- 对象池和自定义内存分配器。
- Toon Shading、自定义 UE Renderer、Global Shader。
- FPS 项目内的多人网络；网络属于独立 Co-op 项目。

## 11. 当前技术债

- 蓝图全量编译阻塞已经清除；完整玩法蓝图仍需做一次统一 PIE 回归，清理运行时残留旧节点和双权威入口。
- AutoBenchmark、CSV、纹理和内存命令仍位于正式 GameMode，发布前宜迁移到开发测试模块或明确 Shipping 编译边界。
- 头部骨骼名硬编码为 `head / neck_01`，更换 Skeleton 时需要调整。
- 80 敌人压力下仍有 CharacterMovement `Max iterations 8 hit` 警告；不应直接提高迭代次数掩盖拥堵和大步长问题。
- VSM Non-Nanite Marking Queue 在多组 80 敌人测试中各出现 1 次，当前接受残留风险，但不能表述为彻底修复。
- 包围槽位参数目前是工程默认值，仍需要用胶囊半径、攻击距离和拥堵数据解释或再校准。
- 当前中央管理器只维护一名玩家目标，不直接支持多玩家目标选择。
- 当前没有对“剑刃与玩家之间存在墙体”做额外视线遮挡检查，碰撞配置必须回归。

## 12. 面试自检

1. 为什么 `StartFire()` 要负责首发和自动射击 Timer，而 `Fire()` 仍只表示一次射击尝试？
2. 为什么射速门禁和自动射击调度必须与弹药一起由 WeaponComponent 持有？
3. 为什么弹药应在 Line Trace 前消耗？
4. 空仓射击如何自动进入换弹？
5. 为什么换弹中死亡不会在 Timer 到期后加弹？
6. `FCollisionQueryParams` 和 Collision Channel 分别控制什么？
7. 为什么同时忽略 Character 和 Weapon Owner？
8. 为什么当前 TargetDummy 不会被正式武器扣血？
9. `ApplyPointDamage` 如何进入 HealthComponent？
10. 致死伤害的三个事件按什么顺序广播？
11. `UPROPERTY` 与 `BlueprintReadWrite` 各解决什么问题？
12. 为什么 `generated.h` 必须放在最后一个 include？
13. AIController、NavMesh 和 SurroundManager 分别解决什么问题？
14. 连续剑刃 Sweep 如何避免低帧率漏检？
15. 当前实现中哪些是 C++ 权威状态，哪些只是蓝图表现状态？

---

## 13. 架构设计总览

### 13.1 分层和职责

项目不是严格的 Web MVC，而是采用与 MVC 相似的“规则、执行、表现”分离：

```text
输入与表现层
Enhanced Input / UMG / Animation Blueprint / Montage / Niagara / Sound
        |
        v
角色与组件层
Character / WeaponComponent / HealthComponent / EnemyCharacter
        |
        v
决策与协调层
EnemyAIController / SurroundManager / GameMode
        |
        v
引擎服务
Damage Framework / TimerManager / NavigationSystem / PathFollowing / GC
```

| 模块 | 负责什么 | 不负责什么 | 核心数据结构/模式 |
| --- | --- | --- | --- |
| Character | 移动、视角、输入意图、当前装备引用、生命值入口和死亡协调 | 不保存弹药或武器动作状态 | 角色聚合根、组件模式 |
| WeaponComponent | 弹药、射速、连续射击、换弹事务、Trace 和后坐力 | 不拥有 HUD 或 Montage 表现 | 组件模式、状态模式、单一职责 |
| HealthComponent | 血量限制、伤害和死亡广播 | 不播放动画 | 组件模式、观察者模式 |
| EnemyCharacter | 攻击窗口、Socket Sweep、受击和死亡执行 | 不做全局站位分配 | 状态保护、每次攻击命中集合 |
| EnemyAIController | FSM、定时决策、MoveTo 请求和转向 | 不播放攻击表现 | 状态模式、更新方法 |
| SurroundManager | 双环槽位、攻击名额、补位和弱引用清理 | 不做 NavMesh 路径搜索 | 中央协调器、TArray/TMap/TSet |
| GameMode | 开局、倒计时、波次、出生、胜负、上下文注入 | 不保存跨关卡持久数据 | 服务端规则入口、Timer |
| UMG/蓝图 | HUD、动画、声音、后处理和参数配置 | 不重新计算伤害、弹药和死亡 | 观察者模式、表现层 |

### 13.2 设计模式不是装饰词

- **组件模式**：武器和生命值从 Character 中拆出，复用逻辑并降低类体积。
- **状态模式**：AI 使用 `Idle/Chase/Attack/Dead`；玩家移动只计算 `Idle/Moving/Dead`；武器用 `Ready/Firing/Reloading/Disabled` 管动作互斥。
- **观察者模式**：Health、Ammo、Wave、Time 和 Result 通过动态多播委托驱动 UI，不让 Widget 每帧查询。
- **更新方法**：AI 用按状态和距离分级的一次性 Timer 调度，CharacterMovement 也按距离降频。
- **中央协调器**：SurroundManager 维护“一个槽位只能属于一个敌人、攻击者不超过上限”这类全局不变量。
- **弱引用治理**：Manager 不应因为保存敌人引用而阻止敌人被回收。
- **对象池暂缓**：当前 Hitscan 没有实体子弹；只有 Spawn/Destroy 或 GC 数据证明是瓶颈时，才池化特效、贴花或敌人。

### 13.3 设计模式总账

模式名称必须对应真实职责。下面区分“项目已实现”“依赖 UE 框架”和“条件触发后再引入”，避免为了回答面试而硬套 GoF 名词。

| 类别 | 模式或工程方法 | 当前参与者 | 解决的问题 | 实现边界与代价 |
| --- | --- | --- | --- | --- |
| 已实现 | 组件模式（Component） | Weapon、Health、PickUp Component | 把可复用能力从 Character 拆出，以组合替代玩家/敌人重复实现 | 这是 UE ActorComponent 组合，不是数据导向 ECS；组件间仍要控制依赖方向 |
| 已实现 | 显式状态机（State/FSM） | Weapon 的 `Ready/Firing/Reloading/Disabled`；AI 的 `Idle/Chase/Attack/Dead` | 把互斥动作和合法转换集中为状态门禁 | 当前用枚举、Guard 和集中转换实现，没有为每个状态创建多态类；状态继续增加时才考虑 StateTree/状态对象 |
| 已实现 | 观察者（Observer） | Weapon/Health/GameMode 动态多播委托；Blueprint/UMG 订阅 | 权威数据源不依赖具体 HUD、音效或特效接收者 | 绑定有生命周期和反射开销；顺序相关的规则不能依赖多个监听者完成 |
| 已实现 | 中介者/中央协调（Mediator） | SurroundManager；GameMode 负责比赛编排 | 维护槽位唯一性、攻击并发和一局规则，避免多个敌人点对点争抢 | Manager 会成为集中依赖点；开放世界/多人需要按空间或玩家拆分 |
| 已实现 | 更新方法（Update Method） | AI 决策循环、距离分级更新、波次与倒计时 Timer | 将重复更新集中，并允许降频、错峰和按状态改变频率 | Timer 仍在 Game Thread，不等于异步或多线程；高频动画窗口仍需逐帧采样 |
| 已实现 | 类型对象/数据驱动（Type Object） | WeaponDataAsset + WeaponFamily | 让武器数值和同类行为参数脱离 Character/派生类 | DataAsset 是共享配置，不保存 CurrentAmmo；当前默认值回退造成双配置源技术债 |
| 已实现 | 适配器（Adapter） | Reload Notify、AttackHit Notify、AttackWindow NotifyState | 把 UE 动画回调签名转换为 Weapon/Enemy 的窄业务接口 | Adapter 只传递时间信号，不能绕过 C++ 状态门禁直接提交伤害或弹药 |
| 已实现 | 注册表（Registry，工程模式） | GameMode 的 ActiveEnemies 弱引用集合 | 统一存活计数，并让 Death 与 Destroy 汇入同一个注销入口 | 不是对象所有者；必须清理失效弱引用，Remove 成功后才能修改计数 |
| 已实现 | 事务与幂等提交（工程模式） | ReloadSequence、命中集合、`bDeathBroadcast`、`bGameEnded` | 让重复输入、Timer、Notify、Death/Destroy 只能提交一次结果 | 每个事务都要明确开始、Commit、Cancel 和 EndPlay；这是正确性设计，不是 GoF 模式 |
| 已实现 | 运行时依赖注入（Setter/Context Injection） | GameMode 生成敌人后注入 Player Target 与 SurroundManager | AI 不需要在每次决策中全局搜索依赖，测试时可替换上下文 | 当前由 GameMode 手工装配；规模增大后可用 WorldSubsystem 或明确接口管理服务发现 |
| UE 提供 | 生命周期模板与 Hook | BeginPlay、EndPlay、Possess、BlueprintImplementableEvent | 引擎固定生命周期骨架，项目覆写特定步骤；C++ 给蓝图表现扩展点 | Blueprint Event 是虚拟 Hook，不是多播 Delegate，也不等于完整 Template Method 自研框架 |
| UE 提供 | 对象构造/工厂服务 | `SpawnActor` + `TSubclassOf<EnemyCharacter>` | GameMode 按配置类创建运行时敌人 | 使用了引擎工厂能力，但项目没有自建 Abstract Factory 层，不应包装成自研工厂模式 |
| 条件触发 | 策略模式（Strategy） | 未来 Hitscan/Projectile/Beam 或不同散布模型 | 在不修改 Character 的情况下替换射击算法 | 当前 WeaponFamily 分支尚不是独立 Strategy；行为只有少量差异时先保持数据驱动 |
| 条件触发 | 对象池（Object Pool） | 未来高频 Projectile、Decal、Effect 或尸体 | 降低已被 Profile 证明的 Spawn/Destroy、分配或 GC 峰值 | 复位、引用泄漏和容量管理有成本；当前没有足够热点证据 |

### 13.4 面试时容易套错的模式

| 追问 | 准确回答 |
| --- | --- |
| “用了 Component，所以这是 ECS 吗？” | 不是。当前是 UObject/ActorComponent 的面向对象组合；没有 ECS 的连续组件存储、System 批处理和实体 ID 数据布局。 |
| “用了 enum，能叫 State 模式吗？” | 可以说采用了状态机思想与集中转换，但不是 GoF 的类多态 State 实现。当前四个状态用枚举更低成本。 |
| “SurroundManager 是 Singleton 吗？” | 不是。它是随 World/Match 创建和销毁的 Actor 实例；这正好避免全局静态生命周期和多世界冲突。 |
| “DataAsset 是 Flyweight 吗？” | 它有共享只读配置的 Flyweight 特征，但本项目主要意图是 Type Object/数据驱动；运行时弹药绝不能写回共享资产。 |
| “输入回调是不是 Command 模式？” | 目前只是命令式窄接口，没有把请求封装成可排队、撤销或重放的 Command 对象。需要 Replay/预测时才值得升级。 |
| “C++ 调蓝图就是 Observer 吗？” | 只有可被多个对象订阅的 Multicast Delegate 符合观察者语义；BlueprintImplementableEvent 是子类 Hook。 |
| “SpawnActor 是你的 Factory 模式吗？” | 它是 UE 提供的构造服务。GameMode 只选择 `TSubclassOf` 并调用它，没有自建工厂层。 |
| “为什么没有 MVC/MVVM？” | Gameplay Framework 还要处理输入、Actor 生命周期、AI、导航和动画时序。UMG 是 View，但当前没有正式 ViewModel。 |

### 13.5 架构优化的演进过程

第一版的问题不是“蓝图一定慢”，而是 Character、武器蓝图、Timer 和敌人实体都可能修改 Gameplay 状态。重构先建立四条不变量：

```text
一个状态只有一个权威拥有者
一个 Gameplay 结果只有一个提交点
命令沿窄接口向下传递，结果通过事件向外通知
正常、失败、中断、死亡和 EndPlay 都必须能收口
```

迁移顺序按风险和依赖推进：

1. 抽出 HealthComponent，统一 Clamp、伤害事件和每次生命的死亡幂等。
2. 将弹药、射速、自动开火和换弹事务移入 WeaponComponent，Character 只转发输入意图。
3. 将敌人攻击改为 NotifyState 时序、双 Socket 帧间 Sweep 和整轮攻击去重。
4. 新建 EnemyAIController，以 Timer 驱动显式 FSM；SurroundManager 管槽位和 Attack Token。
5. GameMode 收口波次、倒计时、胜负和敌人注册表，将 Death/Destroy 汇入同一注销入口。
6. 最后加入 WeaponDataAsset、统计点和固定条件实验，避免在职责尚未稳定时引入大框架。

渐进迁移让每一步都保留可运行闭环，代价是过渡期存在 Deprecated 蓝图入口和配置回退。旧蓝图迁移完成后必须继续删除兼容路径。

通信方式按关系选择：

| 通信方式 | 当前例子 | 选择条件 | 不能承担什么 |
| --- | --- | --- | --- |
| 直接调用 | Character 调 StartFire；AIController 请求 Enemy 攻击 | 接收者唯一，需要立即返回成功/失败 | 不适合未知的一对多监听者 |
| Dynamic Multicast Delegate | Ammo、Health、Wave、Time、Result | 权威状态改变后通知多个表现对象 | 不提交 Gameplay 结果，不依赖监听顺序 |
| BlueprintImplementableEvent | OnAttackStarted、OnEnemyDied、OnPlayerDied | C++ 规则固定，蓝图子类补表现 | 不是多播观察者，也不适合返回权威结果 |
| AnimNotify/NotifyState | Reload Commit、AttackWindow Begin/Tick/End | 动画时间轴决定“何时请求” | 可能重复、缺失或中断，不能独占状态权威 |
| UE Damage Framework | ApplyPointDamage/ApplyDamage -> OnTakeAnyDamage | 统一伤害入口并传递来源、控制者和命中上下文 | 当前头部规则仍依赖 Enemy 类型，通用 Damageable 尚未完成 |

## 14. 五条必须闭眼讲出的调用链

### 14.1 玩家射击

```text
Enhanced Input
-> Character::StartWeaponFire
-> EquippedWeaponComponent::StartFire
-> WeaponActionState = Firing
-> 首发 Fire + C++ 射速 Timer
-> WeaponComponent::TryConsumeAmmo
-> Camera LineTrace
-> FHitResult / BoneName
-> ApplyPointDamage
-> Enemy HealthComponent
-> OnHealthChanged / OnDamageReceived / OnDeath
-> 蓝图受击或死亡表现
```

### 14.2 玩家换弹

```text
Reload Input
-> Character::StartReload
-> Character::RequestReload
-> 停止瞄准和冲刺
-> WeaponComponent::RequestReload
-> 停止开火并进入 Reloading
-> OnWeaponReloadStarted
-> AnimNotify: CommitReload 只提交一次
-> Montage Completed: FinishReload
-> Montage Interrupted: CancelReload
-> C++ 序列 Timer 仅作超时恢复
-> OnWeaponReloadFinished
```

### 14.3 敌人近战

```text
AIController 获得攻击名额
-> EnemyCharacter::TryAttackTarget
-> Blueprint OnAttackStarted 播放 Montage
-> AnimNotifyState Begin
-> BeginAttackWindow
-> Notify Tick 驱动刀刃帧间 Sweep
-> HitActorsThisAttack 去重
-> ApplyDamage(Player)
-> HealthComponent 扣血
-> Notify End / Finish Notify / AttackAnimationDuration Timer / 死亡
-> 关闭攻击窗口；AIController 在攻击结束或 4 秒 Token 租约超时后释放名额
```

### 14.4 群体 AI

```text
GameMode 生成 Enemy
-> 注入 Player 和 SurroundManager
-> AIController 定时 UpdateAI
-> RequestSurroundSlot
-> ProjectPointToNavigation
-> MoveToLocation
-> PathFollowing
-> 内圈到位后 RequestAttackToken
-> Attack 或原地面向玩家等待
```

### 14.5 游戏闭环和 UI

```text
UI/关卡确认开始
-> GameMode::StartGameMode
-> 倒计时 + 波次 Timer
-> Spawn Enemy
-> OnWaveChanged / OnAliveEnemyCountChanged / OnRemainingTimeChanged
-> UMG 更新
-> 玩家死亡或倒计时归零
-> OnGameResult
-> 胜负 UI / Restart Level
```

胜负规则由两个独立事件触发：HealthComponent 判定死亡后通知 Character，Character 完成武器、移动和死亡状态清理，再广播 `OnPlayerDeathReported(this)`；GameMode 收到当前玩家的报告后立即 `FinishGame(false)`，与剩余时间无关。只有倒计时归零时才调用 GameMode 的 `IsPlayerAlive()` 检查胜利，玩家对象和 HealthComponent 有效、生命值大于 0 且未进入死亡状态才 `FinishGame(true)`。倒计时不逐秒轮询失败，也不会在归零分支主动提交失败。敌人是否全部死亡只影响存活数量显示，不直接决定本局胜负。

## 15. 设计依据与未采用方案

### 15.1 为什么用 Hitscan

当前武器强调即时射击反馈，使用 Camera LineTrace 成本低、结果稳定。没有采用实体 Projectile，是因为项目没有弹道下坠、飞行时间和预测需求；为了“显得复杂”增加 Projectile 只会扩大生命周期和碰撞问题。未来新增慢速榴弹或狙击弹道时再提供第二种策略。

### 15.2 为什么伤害规则放 C++、表现放蓝图

伤害、弹药、死亡和攻击窗口要求唯一权威，适合 C++；Montage、音效、后处理和 UI 需要快速调参，适合蓝图。纯蓝图能完成 Demo，但更容易出现重复扣血、Timer 残留和两套状态；全部 C++ 又会提高表现迭代成本。

### 15.3 为什么近战用 AnimNotifyState + 帧间 Sweep

全程武器碰撞会在贴近、起手和收招时误伤；单个 Notify 只有一个命中时刻，快速动画或低帧率可能漏检。NotifyState 给出明确攻击窗口，双 Socket 和帧间 Sweep 覆盖剑刃移动路径，`TSet` 保证同一挥砍对同一目标只结算一次。

### 15.4 为什么不是每个敌人 MoveToActor(Player)

同一目标点会导致扎堆、推挤和重复路径刷新。SurroundManager 把“目标是谁”进一步拆成“每个敌人应到哪个位置”，AIController 改用 `MoveToLocation`。NavMesh 只负责可达路径，不会自动产生包围战术。

### 15.5 为什么是中央管理器而不是完全分布式

槽位唯一性和攻击人数上限是全局约束，集中管理最容易保证一致性。完全分布式会让多个敌人同时选中同一点，并需要冲突仲裁。中央方案的代价是单点依赖和多玩家扩展成本；大世界可改成按玩家或空间分区的多个局部 Manager。

### 15.6 为什么 Timer 决策而不是每帧 Tick

近距离 Attack 需要较快响应，远距离 Chase 不需要 60 次/秒重算。按状态使用 0.1/0.25/0.5/1.0 秒间隔并加入错峰，可以减少峰值。不能把所有 Tick 直接关闭，因为 CharacterMovement、Montage、Socket Sweep 等执行仍依赖引擎更新；优化的是“决策频率”，不是盲目停掉所有更新。

### 15.7 为什么暂时不用行为树、EQS、GAS 和对象池

- FSM 状态数量有限，已经能表达追击、攻击和死亡；行为树不是当前瓶颈。
- 确定性环形槽位便于调试和压测；复杂地形再考虑 EQS 候选点评分。
- 当前战斗没有复杂 Buff、技能取消和网络预测，GAS 会增加学习和排查成本。
- Hitscan 没有实体子弹，运行时数据也尚未证明 Spawn/Destroy 是主瓶颈，因此不强行池化。

## 16. 真实问题、定位过程与修复记录

本节按实际发生顺序保留完整问题台账；第 19 节只从中提炼面试高频案例和条件变化后的替换方案，两节不分别维护第二套项目事实。

以下内容按“现象 -> 定位 -> 根因 -> 修复 -> 学到什么”记录。只有标为“已验证”的内容才能在面试中作为完成事实。

### 16.0 深挖范围与证据规则

本章不追求 Bug 数量。只有下面六类与当前项目强相关、又能覆盖游戏客户端核心能力的问题展开复盘；其余编辑器操作和蓝图接线问题保留在简表中：

| 深挖问题 | 主要考察点 | 当前证据 |
| --- | --- | --- |
| FPS-002/003：战斗状态与连续射击多权威 | 状态所有权、Timer、输入生命周期、幂等 | C++ 已编译；蓝图旧 Timer 待清理 |
| FPS-007：换弹 Montage 被打断 | 动画时序、事务提交、中断恢复 | C++ 事务接口完成；Notify/结束回调待接线 |
| COMBAT-001：近战重复命中与低帧率漏判 | AnimNotifyState、Scene Query、去重和连续检测 | C++ 已实现；边界矩阵待 PIE |
| FPS-006：死亡与胜负结算 | 生命周期、事件顺序、一次性结算 | C++ 已实现；同帧竞争和 UI 待 PIE |
| FPS-009：直追玩家和群体扎堆 | AIController、NavMesh、群体协调、降频 | 架构和压力数据已有；旧蓝图清理待确认 |
| PERF-001：性能瓶颈误判 | Profile、线程归因、P95、停止条件 | 最终 10/20/40/80/160 矩阵与生命周期证据已有 |

证据等级统一为：`代码已实现`、`蓝图已接线`、`PIE 已验收`、`已有量化数据`。风险分析和面试场景题不能写成项目真实事故；编译通过也不能替代行为验证。

### 16.1 工程环境和项目识别

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| UE 启动报 `InstalledDerivedDataBackendGraph` 无可写节点 | 日志指向 DDC/Zen；v2rayN 代理或规则拦截 `::1` 本地通信，编辑器无法连接本地 Zen | 代理规则让 `::1/localhost` 直连，并允许 `UnrealEditor.exe`、打包时的 `UnrealEditor-Cmd.exe`；`-DDC-ForceMemoryCache` 只作应急诊断 | 已定位；能讲 DDC、Zen 和 loopback，不应说成公网网络故障 |
| 多个 safe2/safe3/safe4 项目内容不一致 | 核对 `.uproject`、Source、Content 和修改时间，发现打开的副本不同 | 统一以 safe2 为主项目，每次启动先确认绝对路径和当前 Map | 已处理；说明工程副本需要单一事实来源 |
| Visual Studio 显示 UE5、项目和 ModuleRules “未找到” | `.sln` 缓存指向旧引擎或旧项目路径，不代表资产丢失 | 关闭 IDE，右键 `.uproject` 重新生成工程文件，再用正确 UE 版本打开 | 可复现的工程生成问题 |

### 16.2 GameMode、关卡和波次

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| 剩余时间有值但敌人不生成 | World Settings 的 GameMode、EnemyClass、带 `EnemySpawn` Tag 的 TargetPoint、StartGameMode 调用链逐项检查 | 关卡覆盖为 C++ GameMode 的正确蓝图子类，设置 EnemyClass 和出生点 Tag，并在自定义 `gamestart` 后调用 StartGameMode | 已实现，仍需最终打包回归 |
| 事件在蓝图里找不到 | 打开的蓝图父类不是修改后的 C++ GameMode，或热重载没有刷新反射信息 | 确认父类、完整编译后重开编辑器；必要时重新生成项目文件 | 已形成排查流程 |
| 敌人只在一个点扎堆生成 | 随机选择允许连续命中同一 TargetPoint，波次内没有分散策略 | 收集并打乱出生点，至少覆盖多个不同点；生成后由 SurroundManager 分流 | 生成分布已改进 |
| 敌人生成过慢、下一波等待过长 | SpawnInterval/WaveInterval 是调度参数，不是寻路问题 | 将生成间隔调整到约 0.5～1 秒、波次间隔约 5 秒，并回归峰值 | 参数化完成 |
| `Target` 不知道是什么 | 蓝图函数节点的 Target 是“在哪个对象实例上调用”，不是目标点坐标 | GameMode 函数接 GameMode 实例；MoveTo 的目标位置来自 TargetPoint Transform | 已理解并记录类型语义 |

### 16.3 蓝图对象类型、接口和 Target 错误

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| `Mesh1P`、`IsAiming` 节点提示当前蓝图 self 不是 Character | 武器蓝图中的 `self` 是 Weapon Actor，不能调用 Character 成员 | 使用 `OnPickUp` 事件输出的 `Pick Up Character`，或保存强类型 OwningCharacter，再作为 Target | 已验证；这是对象实例和静态类型的典型问题 |
| 找不到 `Pick Up Character` | 它不是全局节点，而是 `OnPickUp` 事件参数 | 从事件输出引脚直接使用，不能空白搜索同名变量 | 已确认 |
| Cast 后才能看到 Widget 自定义事件 | 变量原本静态类型是 `UserWidget`，编译器不知道子类 `mainmenu` 的函数 | 最好把变量直接声明为 mainmenu 类型；已有通用引用时 Cast，并用 Cast 输出作为 Target | 已验证；Cast 改变可见接口，不会创建新对象 |
| 调用 `Start` 报 Target=self 错误 | 在关卡蓝图里 `self` 是 LevelScriptActor，不是 mainmenu Widget | `CreateWidget` 返回值保存为 mainmenu 引用，调用 Start 时连接该实例 | 已修复 |
| `SetInputMode_GameOnly` 连续警告需要有效 PlayerController | PlayerController 引脚为空、对象失效，或流程在不合适的上下文反复执行 | 每条输入模式链显式使用同一个 `GetPlayerController(0)` 返回值，并在调用前 IsValid | 已定位 |
| Disable/Enable Input 接不上 Character | 节点同时需要 Actor Target 和 PlayerController；把 Controller 接到了 Actor Target 或用 Level self | Actor Target 接 GetPlayerPawn/Character，PlayerController 接 GetPlayerController | 已理解 |

### 16.4 主菜单、UMG 和输入生命周期

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| 开局仍能控制玩家 | `Game and UI` 仍允许游戏接收输入；DisableInput 的 Target 又可能不是 Pawn | 菜单阶段用 UI Only 或显式禁用 Pawn；开始后切 Game Only 并恢复 Pawn 输入 | 已形成正确模型 |
| Any Key 重复触发或 DoOnce 后完全不执行 | DoOnce 的位置和重置关系掩盖了输入流；第一按键也可能被 UI/InputMode 消耗 | 使用明确的 `bGameStarted` 状态或 UI 按钮事件，开始成功后立刻切换输入模式并移除菜单 | 已定位；布尔状态比隐藏 Gate 更易调试 |
| Widget 创建了但没有显示 | CreateWidget、保存引用、AddToViewport 的顺序错误，或创建了重复实例 | 保持 `CreateWidget -> 保存返回值 -> AddToViewport`，只创建一次 | 已修复 |
| `RemoveFromParent(self)` 没有移除画面，GetAllWidgets 后才成功 | Start 调用在一个 Widget 实例上，但画面里显示的是另一个重复创建的实例；Remove 的 Target 不是顶层实例 | `GetAllWidgetsOfClass` 只作为定位重复实例的诊断；正式修复是只创建一次并持有准确实例引用 | 已定位；不要把全局枚举当正式架构 |
| 手动 `Collect Garbage` | RemoveFromParent 只移出视口，GC 要等没有强引用后按引擎时机执行；强制 GC 会造成卡顿 | 移除视口后清理持有引用，让 UE GC 自然处理 | 已纠正 |
| 有事件输出但淡出不可见 | 检查 PlayAnimation 的动画资产引脚、动画是否真正修改根画布 Render Opacity、播放方向、时长和是否在移除前完成 | 将淡出动画绑定根容器，使用动画 Finished 回调或与真实动画长度一致的 Delay 后再 Remove | 需最终视觉回归 |
| Health Bar 函数连接报错 | `GetCurrentHealth` 的 Target 是 Character，而误把 HealthComponent 或未经执行的非纯 Cast 接入；纯绑定函数又没有执行白线 | 更推荐 Construct 时缓存 Character/HealthComponent，通过 OnHealthChanged 主动更新 Percent，避免每帧属性绑定和反复 Cast | 事件接口已有，UI 待回归 |

### 16.5 武器、动画和战斗

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| 枪支无法拾取 | 迁移部分逻辑到 C++ 后，蓝图事件链没有把 `Pick Up Character` 同时传给 AttachWeapon 和手臂显示 | AttachWeapon 成功后，以事件传入的 Character 为 Target 显示 Mesh1P；不要 GetPlayerPawn 硬取玩家 0 | 已修复/需最终回归 |
| 开局就显示手臂 | Mesh1P 默认可见，显示逻辑放在 BeginPlay 而不是拾取成功点 | 默认隐藏，只有 AttachWeapon 返回 true 后显示，并 Propagate to Children | 设计已明确 |
| 攻击事件触发但 Montage 不播放 | 逐项检查蓝图是否继承正确 C++ Enemy、事件是否被覆盖、Montage/Skeleton/Slot、Mesh Animation Mode 和播放节点 Target | 保持 C++ 发起、蓝图只播表现；用 Print/断点先验证事件，再验证 Montage 返回值和 AnimBP Slot | 已形成诊断链 |
| 随机攻击看起来只播放一种 | 检查 Select 的四个资产是否实际不同、Random Index 是否每次执行、各 Montage 是否共享相同动画或 Slot | 先打印随机 Index 和资产名，再排查动画资源，而不是先改随机算法 | 需资产回归 |
| 敌人全程贴着玩家也扣血 | 武器碰撞一直开启，没有动作语义 | 用 AnimNotifyState 定义攻击窗口，窗口外不做伤害 Sweep | 已修复 |
| 同一次挥砍重复扣血 | Notify Tick 多帧命中，或同一 Montage 多个窗口重复重置去重集合 | 只在 `TryAttackTarget()` 开始整轮攻击时清空 `HitActorsThisAttack`；所有窗口共享集合 | C++ 已实现，PIE 多窗口回归待完成 |
| 低帧率或高速挥砍漏检 | 只检查当前帧 Socket 点，剑刃可能跨过目标 | 4 个刀刃采样点做上一帧到当前帧 Sweep，再补当前 base-to-tip Sweep | 已实现 |
| Montage 中断、死亡后仍可能伤害 | Notify End 不是唯一可靠清理路径，Timer/状态可能残留 | Montage 中断、攻击结束、敌人死亡、目标死亡和超时都调用统一取消/收尾 | 已实现，边界待完整矩阵回归 |
| NotifyState 加入后资产保存异常 | 需要区分 C++ Notify 类未编译/热重载、资产只读、Skeleton/Montage 不兼容等原因 | 完整编译、重开编辑器、确认资产可写和 Notify 类可加载；没有最终日志时不虚构根因 | 曾遇到，根因证据需补 |

### 16.6 AI、移动与群体行为

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| 所有敌人只冲玩家中心并扎堆 | 所有人执行 MoveToActor(Player)，目标完全相同 | 双环槽位 + NavMesh 投影 + MoveToLocation + 攻击名额 | 已实现，是核心亮点 |
| 远处敌人不主动追击 | 旧逻辑有 ChaseRange/目标解析限制，且出生后上下文不稳定 | GameMode 生成后注入 Player/Manager；Controller 必要时再解析兜底 | 已实现 |
| 敌人背对玩家移动或攻击 | Movement 朝向速度方向与显式 LookAt 规则冲突 | 使用 Controller Desired Rotation，并在决策和等待攻击时 FaceTarget | 已实现/需群体回归 |
| 两个敌人争同一槽位 | 如果由敌人独立随机选择会冲突 | Manager 在 Game Thread 串行检查 Occupant 并立即写入 TMap/TArray | 已实现 |
| 槽位落进墙、桌子或 NavMesh 外 | 几何环形位置不保证可走 | `ProjectPointToNavigation`；分配时跳过失败槽位，运行时失败则重新请求或回退追击 | 已实现基本兜底 |
| 敌人拿到攻击名额却卡住 | 名额可能永久占用 | 只允许内圈申请，设置 4 秒超时；攻击结束、中断、死亡、离距都释放 | 已实现 |
| 内圈敌人死亡后外圈不补位 | 只释放槽位但没有候补迁移 | 选择离空内圈最近的外圈占用者提升；原外圈位置自然空出 | 已实现 |

### 16.7 性能、内存和渲染告警

| 问题 | 定位与根因 | 解决方案 | 状态/可讲价值 |
| --- | --- | --- | --- |
| 160 AI 无法稳定 60 FPS | 最终矩阵中 Game Thread 27.899 ms；CharacterMovement 6.188 ms、Animation 4.448 ms，仍是主要扩容成本 | 已落地移动全速/30/15Hz 分级、Animation URO、不可见降级和死亡停更，不重写 NavMesh | 五档复测完成；缺少当前版本逐项开关 A/B，不能宣称单项提升比例 |
| 误以为寻路是最大瓶颈 | 只凭系统复杂度猜测，没有看计时 | 用 CSV/Insights 分项后确认 Movement+Animation 更贵 | 已形成数据驱动结论 |
| 编辑器出现 Texture Streaming Pool 告警 | 独立运行预算 1000 MB、Over Budget 0 MB；定位到六张 4K/长边 4K 环境植被纹理 | 不扩大 Pool，只把六张纹理的 Max Texture Size 限制到 2048 | Streaming 212.27 -> 152.27 MB，下降 28.3% |
| `[VSM] 非 Nanite 标记工作队列溢出` | 属于 Virtual Shadow Map 页面标记，不是 Texture Streaming Pool | 已完成页覆盖诊断、首批大面积资产治理、敌人阴影距离分级和三组单变量实验 | 80 敌人档单次但可复现；保留默认路径并接受残留，不宣称彻底消除 |
| 工业场景材质缺失 | 日志指出 `/Game/FactoryDistrict/Materials/Black` 等引用不完整 | 修复 Redirector/缺失资产或替换材质；这属于资源完整性，不是内存泄漏 | 待资源回归 |
| 担心 Timer、Delegate、Widget、尸体泄漏 | 仅凭进程工作集峰值不能判断泄漏 | 80 敌人 Spawn/Kill/等待 35 秒，记录 Actor、注册表、UObject、Movement、Animation 和工作集 | Enemy/注册数回到 0，UObject 减少 887；分配器保留页面，因此不宣称工作集立即下降 |

### 16.8 历史 Bug 编号与完整复盘

本节合并原 `FPS_BUG_LOG.md` 的历史记录。编号用于追踪开发过程中真实出现的问题；状态表示目前证据边界，不因文档合并而自动视为完成。

| ID | 问题 | 分类 | 当前状态 |
| --- | --- | --- | --- |
| FPS-001 | Enhanced Input Action 未赋值导致按键无响应 | 输入/蓝图配置 | 部分修复 |
| FPS-002 | 射击、换弹、瞄准和死亡状态互相冲突 | Gameplay 状态 | C++ 已修复，蓝图待回归 |
| FPS-003 | 连续射击 Timer 与单发射击职责不清 | 武器架构 | C++ 已修复，蓝图旧 Timer 待删除 |
| FPS-004 | 视觉子弹与真实命中职责混淆 | 射击架构 | 已修复 |
| FPS-005 | 玩家、靶子和敌人血量逻辑重复 | 组件设计 | 已修复 |
| FPS-006 | 死亡后仍可能残留移动、攻击或换弹逻辑 | 生命周期 | 部分修复 |
| FPS-007 | Reload Montage 无法稳定播放 | 动画/蓝图 | C++ 事务接口已完成，蓝图待接线 |
| FPS-008 | 敌人可以被命中但血痕贴花不显示 | 材质/贴花 | 已修复 |
| FPS-009 | 敌人只能直线追逐，遇到障碍表现异常 | AI | 部分修复 |
| FPS-010 | UE 启动时报 DDC 没有可写节点 | 编辑器环境 | 临时绕过 |
| FPS-011 | 打开错误工程副本并产生重复 UE 进程 | 工程管理 | 已修复 |
| FPS-012 | 构建产物、生成文件和未跟踪资源混杂 | Git/构建 | 已清理索引并通过全量构建，待提交 |
| FPS-013 | 敌人近距离后退并且无法攻击 | AI/碰撞距离 | 代码已修复，等待 PIE 回归 |

#### FPS-001：Enhanced Input Action 未赋值

**现象**：冲刺、瞄准、开火或换弹没有响应。C++ 已执行输入绑定，但角色蓝图中的 `RunAction`、`AimAction`、`FireAction` 或 `ReloadAction` 引用为空，或默认映射上下文没有对应按键。

**根因**：`UInputAction` 属性由蓝图实例配置。C++ 声明和 `BindAction` 存在，并不代表资产引用已经赋值；复制或替换 Pawn 后也可能再次丢失。

**处理与验证**：绑定前输出缺失 Action，在 `BP_FirstPersonCharacter` 默认值中补齐引用。PIE 中分别触发三种输入，确认没有 `Action is NULL`，并检查对应状态变化。封版前还要重新检查关卡实际使用的 Default Pawn。

#### FPS-002/003：战斗状态与连续射击多权威

**现象**：换弹期间仍可能收到射击输入；空仓自动换弹与手动换弹重复进入；蓝图 Timer 和 C++ 调用可能重复发射；停止射击、换弹或死亡后，旧回调仍可能到达。

**难点**：输入按下/释放、自动射击 Timer、射速门禁、弹药消费、换弹和动画表现发生在不同时间点。如果 Character、WeaponComponent 和 BP_Weapon 各保存一份布尔状态，单看某个函数都可能正确，但组合后会出现双重权威和延迟回调。

**定位过程**：沿 `Enhanced Input -> Character -> WeaponComponent -> Timer -> Fire -> Delegate -> BP_Weapon` 画调用链；分别记录一次按下产生的 Accepted Shot、弹药变化和表现事件数量；检查蓝图是否仍创建射速 Timer。由此把“动画重复”与“Gameplay 真正重复射击”分开。

**根因**：早期输入生命周期、射速调度和单发规则没有分层，Character 与 WeaponComponent 同时保存武器状态，蓝图又保留第二套自动射击 Timer。

**方案取舍**：

| 方案 | 结果 | 取舍 |
| --- | --- | --- |
| Character 管全部射击 | 入口集中，但角色继续持有武器内部状态 | 不采用 |
| 蓝图 Timer 调度、C++ 扣弹 | 上手快，但规则跨蓝图/C++，中断难收口 | 仅作为历史原型 |
| WeaponComponent 统一状态和 Timer | 状态、弹药和射速在同一所有者内 | 当前采用 |

**最终处理**：Character 只转发 `StartFire/StopFire/RequestReload` 意图并协调移动动作；WeaponComponent 独占弹药、`Ready/Firing/Reloading/Disabled`、射速门禁和 Timer。`StartFire()` 表示输入生命周期开始，`Fire()` 表示一次受门禁保护的射击尝试，弹药只在射击被接受后消费一次；死亡统一进入 `Disabled` 并清理 Timer。

**结果与验证边界**：Development Editor 编译通过，状态所有权已经收口。仍需删除 BP_Weapon 旧射速 Timer，并在 PIE 覆盖单击、长按、松开、空仓、换弹和死亡；在这些验证完成前只能说“C++ 多权威已治理”，不能说完整射击链已经封版。

#### FPS-004：视觉子弹与真实命中混淆

**现象**：枪口发出的视觉子弹与摄像机准星射线可能不重合，调整视觉子弹速度还可能改变命中表现。

**根因**：Gameplay 命中判定与曳光表现没有分开。

**处理**：摄像机 Hitscan LineTrace 决定命中、伤害和冲量；`FHitResult` 或射线终点作为 `TraceTarget`；视觉子弹只从枪口飞向该目标，不决定伤害结果。

**验证**：关闭视觉子弹后伤害仍正常；开启后轨迹朝向 TraceTarget；修改视觉速度不改变伤害发生时间。由此确认“规则产生结果，表现消费结果”的边界。

#### FPS-005：血量逻辑重复

**现象**：Player、TargetDummy 和 Enemy 分别维护生命值、受伤和死亡，规则逐渐不一致。

**根因**：早期为快速验证射击，在各 Actor 内重复实现生命值逻辑。

**处理**：抽取 `UfpstrueHealthComponent`，监听 Owner 的 `OnTakeAnyDamage`，统一 Clamp、死亡判断以及 `OnHealthChanged / OnDamageReceived / OnDeath` 广播。

**验证**：使用同一伤害入口攻击不同 Actor，确认生命值统一扣除且死亡只广播一次。当前枪械仍对具体 Enemy 类型有依赖，后续应改为通用可伤害契约，才能让 TargetDummy 完整复用链路。

#### FPS-006：死亡既是生命周期边界，也是失败结算入口

**现象**：早期死亡只表现为生命值归零，玩家仍可能瞄准、射击或换弹；敌人攻击 Timer 和碰撞仍可能保留。另一个规则问题是：失败不能等倒计时结束才判断，而胜利只能在倒计时归零时检查玩家是否仍存活。

**难点**：HealthComponent 负责确认死亡，Character 负责停止自身能力，GameMode 负责对局结果。把这些职责全部塞进 HealthComponent 会让通用组件依赖具体游戏规则；只让 GameMode 每秒轮询生命值又会产生失败延迟，并增加死亡与倒计时同帧时的竞争。

**定位过程**：分别画出 `Damage -> HealthComponent -> Character Death` 和 `Countdown -> GameMode` 两条链，检查谁最先知道玩家死亡、谁有权广播最终结果，以及重复死亡/重复倒计时回调是否可能二次结算。

**最终处理**：HealthComponent 用死亡标记保证每次 Reset 之间只广播一次；Character 用 `bDeathHandled` 幂等停止瞄准、武器和 CharacterMovement，然后广播 `OnPlayerDeathReported(this)`；GameMode 使用 `AddUniqueDynamic` 订阅，游戏运行中收到死亡立即 `FinishGame(false)`。倒计时只在归零且 `IsPlayerAlive()` 时 `FinishGame(true)`，`bGameEnded` 保护胜负只提交一次；结算同时清 Timer、解绑死亡事件、停止敌人并重置 SurroundManager。

**方案取舍**：死亡采用事件驱动而不是每秒轮询；胜利仍由时间条件触发，不把“玩家暂时存活”误当作提前胜利。GameMode 保存单机规则，将来联机时才迁移可复制状态到 GameState/服务器权威层。

**结果与验证边界**：C++ 结算链已实现。仍需 PIE 覆盖剩余时间内死亡、时间归零时存活、归零前致死和死亡/倒计时同帧四种顺序，并验证 UI、输入模式和重新开始；在此之前不能宣称完整游戏结束流程已验收。

#### FPS-007：Reload Montage 无法稳定播放

**现象**：C++ 已进入换弹状态，但第一人称手臂没有稳定播放 Montage；普通换弹和空仓换弹表现不一致，或 Montage 被开火动画打断后仍能继续射击。

**难点**：Montage 是可中断的表现时间轴，弹药增加是一次性 Gameplay 提交。若直接在固定 Timer 到期时加弹，动画速度、被替换、死亡和切枪都会使画面与规则失配；若只依赖 Notify，又必须处理重复 Notify、Notify 未到达和中断恢复。

**定位顺序**：先确认请求是否进入 `Reloading`，再确认蓝图监听的是当前 WeaponComponent，随后检查第一人称手臂 Mesh、AnimInstance、Slot 和 Montage 返回值，最后区分 Completed 与 Interrupted。不能在 Montage 尚未播放时先修改 Timer 长度掩盖资产接线问题。

**候选方案**：

| 方案 | 优点 | 主要问题 | 定位 |
| --- | --- | --- | --- |
| 固定 Timer 到期加弹 | 简单，不依赖动画资产 | 动画变速/中断后失配 | 只作超时兜底 |
| Montage 结束时加弹 | 生命周期一致 | 弹匣在动画末尾才变化，无法表达插匣关键帧 | 负责收尾，不负责唯一提交点 |
| AnimNotify 提交 + Completed/Interrupted 收尾 | 规则与关键帧一致，能区分完成和取消 | 需要幂等和超时恢复 | 目标正式方案 |

**当前实现**：WeaponComponent 提供 `RequestReload / CommitReload / FinishReload / CancelReload`。`ActiveReloadSequence` 让旧 Timer 失效，`bReloadAmmoCommitted` 保证一轮最多加弹一次，状态门禁拒绝换弹期间开火，Owner 死亡会清 Timer、递增序列并进入 `Disabled`。

**当前边界**：C++ 事务接口已经完成，但蓝图 Notify、Montage Completed/Interrupted 尚未全部接线，Timer 仍承担兜底完成。因此该问题仍是 P0，必须测试普通/空仓换弹、重复按键、长按开火、Montage 被替换和死亡中断后才能标记完成。

#### COMBAT-001：近战多帧重复命中与低帧率漏判

**现象**：武器碰撞持续开启会让敌人贴住玩家时不断扣血；只在 NotifyTick 检查当前 Socket 又会在低帧率或高速挥砍时跨过玩家；同一 Montage 的多个窗口还可能重复清空命中集合。

**难点**：动画负责“什么时候允许命中”，Scene Query 负责“刀刃经过哪里”，Gameplay 负责“这一轮是否已经结算”。三个问题不能由一个布尔值或一次 Overlap 同时解决。

**定位过程**：使用 AttackId/当前攻击状态、窗口 Begin/Tick/End 日志和 Debug Sweep 区分“同一次攻击的多帧命中”“两个真实攻击”和“查询漏判”；将去重集合的重置点从窗口开始移动到 `TryAttackTarget()` 的整轮攻击开始。

**最终处理**：AnimNotifyState 只定义有效窗口；记录 `weapontop/weaponend` 上一帧和当前帧位置，默认沿刀刃取 4 个采样点做帧间 Sphere Sweep，再补当前 base-to-tip Sweep；`TSet<TWeakObjectPtr<AActor>>` 和 `bHitTargetThisAttack` 保证整轮攻击最多对目标提交一次伤害。攻击结束、中断、死亡和超时统一关闭窗口并释放攻击 Token。

**取舍与遗留风险**：Sphere Sweep 比单点检测更稳，但查询量更高；固定 4 采样仍需和按端点位移自适应采样做 A/B。当前 `SweepMultiByObjectType(ECC_Pawn)` 不会让 WorldStatic 墙体成为阻挡结果，所以隔墙用例仍需验证。本轮不增加专用近战通道；若问题实际复现，先增加现有可见性/视线校验，玩法出现盾牌、阵营或可破坏物后再评估更细的碰撞矩阵。

**结果与验证边界**：连续检测和整轮去重已在 C++ 实现；多窗口 Montage、中断、低帧率、高速挥砍、隔墙和死亡中断仍需 PIE 验收。未完成这些用例前不能说“近战碰撞完全可靠”。

#### FPS-008：敌人血痕贴花不显示

**现象**：LineTrace、Cast、声音和粒子都成功，墙面弹孔正常，但敌人 `CharacterMesh0` 没有血痕。

**定位过程**：打印 Hit Actor 和 Hit Component，确认命中 SkeletalMesh 而不是 Capsule；确认 Mesh 开启 `Receives Decals`；使用墙面弹孔排除 Spawn 节点和 Decal Material 本身。

**根因与修复**：敌人父材质 `Decal Response` 为 `None`。改为 `Color` 或 `Color Normal Roughness`，使用 `Spawn Decal Attached` 附着到 Hit Component，并设置有限 Life Span。

**验证**：静止敌人可见血痕；移动和旋转后贴花随 Mesh；墙面仍走弹孔分支；Life Span 到期后自动清理。

#### FPS-009：敌人直线追逐

**现象**：旧 EnemyCharacter 使用 `AddMovementInput` 朝玩家移动，遇到障碍不会绕行，Idle/Chase/Attack/Dead 由距离和布尔值隐式表达。

**难点**：问题不只是“缺少 NavMesh”。目标选择、状态决策、路径执行、战术站位和攻击并发是五个不同职责；直接把 `MoveToActor(Player)` 换成另一个节点仍会让所有敌人竞争同一个位置。

**定位过程**：先用导航可视化验证可达区域，再观察每个敌人的目标位置和 Move Request。所有 AI 目标都等于玩家中心，说明扎堆来自战术目标相同；性能数据又显示 Pathfinding 远低于 CharacterMovement 和 Animation，不能把群体成本归咎于 A*。

**方案取舍**：Behavior Tree、EQS 和 AI Perception 都是候选，但当前目标固定为单玩家，没有潜行、听觉和复杂掩体评分需求。项目先采用可调试的 C++ FSM、Timer 决策和确定性双环槽位；复杂条件出现时再升级感知和候选评分。

**最终处理**：`AfpstrueEnemyAIController` 管理 `Idle/Chase/Attack/Dead`，Timer 驱动决策，NavMesh MoveTo 执行路径；GameMode 生成后注入 Player 和 SurroundManager；Manager 分配双环槽位、NavMesh 投影和有限攻击 Token，敌人只处理自身攻击窗口与受伤死亡。

**结果与验证边界**：架构、压力场景和最终五档矩阵已经存在。仍需确认 `enemy_BP` 没有旧 Tick/Timer/AI MoveTo，回归绕障、不可达目标、玩家死亡、重新 Possess 和关卡退出；AI 响应延迟与 Move Request 次数尚未量化。

#### FPS-010：DDC 没有可写节点

**错误**：`InstalledDerivedDataBackendGraph` 报告没有可写节点，编辑器在未打开项目时也可能 Fatal。

**定位**：缓存目录可写、Zen 进程存在，但本机 `::1:8558` loopback 访问异常，默认 Installed DDC Graph 在 Zen 失效后没有其他可写节点。

**临时处理**：通过 `-ddc=InstalledNoZenLocalFallback` 使用文件缓存回退，并从日志确认本地路径 Writable、ZenShared Disabled。

**剩余风险**：这不是永久修复。普通快捷方式仍可能回到默认 Zen Graph，首次切换缓存还会触发 Shader 重建。正式处理应检查代理规则、IPv6 loopback 和本机防火墙。

#### FPS-011：打开错误工程副本

**现象**：编辑器打开名称相近的 safe1/safe2 副本，同时出现两组 UnrealEditor 和 ShaderCompileWorker，源码、DLL 与蓝图状态不一致。

**根因**：Recent Projects 保留备份副本，启动时没有核对 `.uproject` 绝对路径。

**处理与验证**：检查进程 CommandLine、窗口标题、项目根目录和当前 Map，只保留正确工程进程。备份应改为日期化压缩包或只读归档，避免多个可直接运行的活动副本。

#### FPS-012：构建产物与项目资源混杂

**现象**：`Binaries`、`Intermediate`、UHT 文件和缓存进入工作区，同时蓝图、Content 或文档存在未跟踪项，提交范围难以判断。

**风险**：只提交 Source 可能遗漏真实依赖的资产；提交全部改动又会带入可再生成文件和本机缓存；旧 DLL 还可能掩盖源码是否真正编译。

**治理方案**：版本控制聚焦 `.uproject / Source / Config / Content / Plugins / Docs`，通过 `.gitignore` 排除 `Binaries / Intermediate / Saved`；提交前逐项检查未跟踪资源，大型资产继续使用 Git LFS，并最终做一次干净目录恢复和 Development Editor 编译。

**实际复盘（2026-08-12）**：本地切换到最新 `fps-v1` 后，UBT 读取了仓库中被跟踪的旧 `Intermediate/Build/BuildRules`。旧规则没有包含当前 `Build.cs` 中的 `AIModule` 依赖，导致全量编译报 `AIController.h` 找不到；旧模块 DLL 又曾让增量构建误报 `Target is up to date`。执行 UBT Clean、删除旧 BuildRules 缓存并从 Git 索引移除 50 个 `Binaries / Intermediate / Saved / .sln` 生成文件后，UBT 重新生成了包含 `AIModule` 路径的响应文件，34 个编译和链接动作全部通过。

**安全边界**：恢复验证完成前不直接删除现有工程或备份。生成文件只从 Git 索引移除，本地编译产物继续保留；未跟踪资产仍需逐项确认来源、用途和授权，不能使用 `git add .` 批量提交。

#### FPS-013：敌人近距离后退并且无法攻击

**现象**：敌人视觉上已经贴近玩家，却继续调整路径或后退，攻击状态不能稳定触发。

**根因**：旧逻辑只比较两个 Actor 原点之间的距离与 `AttackRange`。敌我胶囊体可能先发生接触，导航无法继续缩短中心距离，但范围判断仍返回 false。

**处理**：保留 `EnemyAIController / NavMesh / SurroundManager` 架构，有效攻击距离取配置 `AttackRange` 与“敌方胶囊半径 + 玩家胶囊半径 + 5 cm 容差”的较大值，不修改已有蓝图公开接口。

**验证边界**：PIE 中从不同方向接近静止和移动玩家，确认胶囊接触后能稳定进入 Attack，同时检查双环槽位和攻击令牌没有被绕过。不同胶囊尺寸、根运动和攻击动画仍需分别校准。

#### PERF-001：把“敌人多时变慢”误判成寻路瓶颈

**现象**：敌人数量增加后帧率下降，直觉上容易把复杂的 NavMesh/Pathfinding 当作主要成本，并提出重写寻路或多线程 AI。

**难点**：帧率是 Game、Render、GPU 中最长线程的最终结果，平均 FPS 还会掩盖尖峰。AI 逻辑、CharacterMovement、Animation、PathFollowing 和渲染会随敌人数同时增长，仅凭画面无法归因。

**定位过程**：固定关卡、分辨率、VSync、敌人数和采样时长，对 10/20/40/80/160 敌人采集 CSV，并比较 Game Thread、P95、CharacterMovement、Animation 和 GPU。最终 160 敌人档 Game Thread 为 `27.899 ms`，CharacterMovement 为 `6.188 ms`，Animation 为 `4.448 ms`。早期基线中 Pathfinding 仅 `0.071 ms`，最终矩阵没有重复记录该项，因此只用于支持“寻路不是首要嫌疑”，不跨版本计算提升比例。

**结论与方案**：瓶颈首先是 Movement、Animation 和 Actor Tick，不是路径算法。优化顺序因此是决策降频/错峰、距离分级、Animation URO、不可见降级和死亡停更；只有 Profile 显示 Nav Query 或同步成为瓶颈时，才考虑更复杂的路径缓存、异步或多线程方案。

**验证结果与诚实边界**：最终五档矩阵可用于说明当前容量和瓶颈归因，但缺少当前版本逐项关闭 LOD/URO/Movement 分级的全档 A/B，不能声称精确性能提升百分比。后续若验证单项收益，应在同条件下各运行至少三次、取中位数并记录响应延迟和画质/动作正确性。

#### 当前未闭环事项

1. 在武器蓝图连接换弹 AnimNotify、Montage Completed/Interrupted，并覆盖中断矩阵。
2. 删除武器蓝图旧自动开火 Timer 和对 Deprecated `Fire` 节点的调用。
3. 清理 `enemy_BP` 中旧 Tick、Timer 和移动节点并完成 NavMesh 回归。
4. 完成 Game Over、重新开始、血量、弹药、波次和倒计时 UI 验收。
5. 验证代理/loopback 修复后的 Zen DDC 正常启动路径。
6. 整理 Git LFS 和资产清单，完成干净恢复测试。

## 17. 统一定位方法

### 17.1 “接口无反应”五步法

```text
1. 事件源是否真的执行：断点 / Print / UE_LOG
2. 当前对象是谁：打印 GetName 和 Class，确认 self/Target
3. 静态类型是否包含接口：变量类型、Cast 输出、父类是否正确
4. 绑定发生在什么时候：BeginPlay/Construct 前后、是否重复绑定
5. 对象是否仍有效：IsValid、是否已 Remove/Destroy/GC
```

### 17.2 “蓝图跑不通”五步法

```text
编译器红字的完整文本
-> 检查红色节点 Target 类型
-> 检查执行白线是否真正到达
-> 检查纯节点是否依赖无效对象
-> 检查资产引用、父类和当前打开的项目副本
```

### 17.3 “性能不好”五步法

```text
固定场景和敌人数
-> stat unit 判断 Game/Draw/GPU
-> CSV/Insights 找具体系统
-> 一次只改一个变量
-> 至少三次重复测量 Avg/P95
-> 做玩法、画质和生命周期回归
```

### 17.4 “内存增长”五步法

```text
记录初始对象数和内存
-> 多轮生成/击杀/特效
-> 等待正常销毁和 GC
-> 再记录对象数、引用和内存
-> 区分缓存、高水位和真正不可回落的泄漏
```

## 18. 定量证据和诚实边界

### 18.1 2026-08-16 最终固定规模矩阵

以下数字统一引用 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md) 第 12 节。早期 100 AI 和旧矩阵只保留在归档复盘中，不作为当前容量结论。

| 敌人数 | 平均 FPS | Frame Avg | P95 | Game Thread | Render Thread | GPU | Movement | Animation |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 62.84 | 15.914 ms | 19.706 ms | 4.299 ms | 15.150 ms | 8.785 ms | 0.429 ms | 0.650 ms |
| 20 | 61.15 | 16.352 ms | 19.909 ms | 5.879 ms | 15.642 ms | 9.189 ms | 0.745 ms | 0.977 ms |
| 40 | 61.02 | 16.389 ms | 19.632 ms | 8.471 ms | 15.736 ms | 9.915 ms | 1.501 ms | 1.517 ms |
| 80 | 55.20 | 18.115 ms | 21.076 ms | 16.106 ms | 17.355 ms | 11.591 ms | 3.231 ms | 2.728 ms |
| 160 | 35.83 | 27.907 ms | 32.459 ms | 27.899 ms | 14.182 ms | 14.205 ms | 6.188 ms | 4.448 ms |

当前机器上的发布容量按 **40 个活跃敌人约 60 FPS** 表述；80 是压力档，160 是极限档。P95 均高于 16.67 ms，因此不能表述为“全程稳定 60 FPS”。

### 18.2 当前可以说的优化

- 已根据数据实现移动更新 20 米内每帧、20～40 米约 30 Hz、40 米外约 15 Hz。
- 已开启 Animation URO，不可见敌人只更新必要 Montage。
- 攻击窗口内恢复高频更新，避免 Socket Sweep 精度下降。
- 死亡后停止移动更新。
- 最终五档矩阵均达到目标敌人数；从 10 到 160，Movement 与 Animation 的增长证明它们仍是扩容优先级，Pathfinding 不是主要成本。
- 六张高占用环境纹理定点限制到 2048，Streaming Current/Target 从 `212.27 MB` 降至 `152.27 MB`，下降 `60 MB / 28.3%`。
- 纹理调整前后 Frame P95 分别为 `16.44/16.58 ms`，因此只宣称资源预算收益，不宣称帧率提升。
- 80 敌人统一死亡后等待 35 秒，Enemy Actor 和 GameMode 注册数由 80 回到 0，UObject 减少 887。

### 18.3 当前不能说的结果

- 当前版本没有逐项关闭 LOD、URO、Movement 分级的同版本全档 A/B，所以不能把新旧矩阵差值包装成精确提升百分比。
- 没有完成响应延迟统计，不能声称降频对攻击反应没有影响。
- 只有一次 80 敌人回收证据，没有连续多波次长期曲线，不能声称“零泄漏”。
- 没有实现 Toon，不能把 PostProcess 调色包装成自定义 Shader。
- VSM 非 Nanite 标记队列在 80 敌人实验中仍会单次复现，当前是接受残留风险，不是彻底修复。

详细实验和原始数据见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。

## 19. 实战问题、解决思路与替换条件

本节不重复第 16 节的全部时间线，只保留“现象 -> 根因 -> 当前修复 -> 条件变化时怎样替换”的高信号版本。

这一章只保留项目真正遇到过的问题。每个问题都按“复现、定位、修复、验证、条件变化”组织，避免只记最终 API。

### 19.1 接口执行了，但 GameMode 没有开始

```text
现象：按任意键后菜单退出，但倒计时为 0、敌人没有生成
-> 在蓝图执行线上加断点和临时 Print
-> 确认执行已经到达 GameStart
-> 检查 Get GameMode 的实际类型和 Cast Failed
-> 查看 StartGameMode failed 日志，而不是继续改 UI
```

实际根因可能来自不同层：关卡 World Settings 没有覆盖正确 GameMode、Default Pawn 指向同名旧蓝图、EnemyClass 未配置、`EnemySpawn` 写成 Component Tag、出生点不足 4 个，或 SpawnPoint 不在 NavMesh 上。最终做法是让 C++ `StartGameMode()` 负责前置校验并输出失败原因，关卡蓝图只调用一次入口。

**验证**：倒计时从 90 开始，日志出现 Wave 和 Spawn 记录，存活数与场上敌人数一致。

**条件变化**：若以后有多个关卡，不复制 Level Blueprint 规则；将波次和敌人类下沉到数据配置，GameMode 保持统一入口。若改成联网，GameMode 仍只在服务器存在，客户端展示数据迁移到 GameState。

### 19.2 蓝图节点存在，但 Target 类型错误

项目多次遇到 Mesh、WeaponComponent、Widget Class、Widget Instance 和 GameMode 被混用。典型表现是节点报 Warning、运行时 `Accessed None`，或看似执行但状态没有变化。

定位顺序：

1. 先读节点副标题中的 `Target is ...`。
2. 沿蓝色对象线回溯对象的实际类型和创建位置。
3. 对可能为空的对象使用 `IsValid`，但不把它当作修复类型错误的办法。
4. Widget 必须保存 `Create Widget` 的 Return Value；Component 函数必须传当前 WeaponComponent，不能传枪 Mesh。
5. 修复后删除临时 Print，并重新打开蓝图确认旧节点没有保留失效签名。

**条件变化**：当跨蓝图引用越来越多时，优先让创建者保存实例并通过初始化参数或事件分发传递，不升级为全局 `GetAllActorsOfClass` 搜索。

### 19.3 换弹动画被打断后仍能开枪

第一版问题来自 Gameplay 状态依赖动画是否还在播放，且手臂 Montage、武器 Montage、Delay 和蓝图布尔值都可能结束换弹，形成多权威入口。

最终事务：

```text
StartReload
-> WeaponActionState = Reloading
-> 广播 OnWeaponReloadStarted
-> 蓝图并行播放手臂和武器 Montage
-> 弹匣插入帧 ReloadCommit Notify -> CommitReload
-> 权威手臂 Montage Completed -> FinishReload
-> 权威手臂 Montage Interrupted -> CancelReload
-> C++ Timeout 只处理回调丢失
```

`CommitReload` 使用序列和状态门禁保证只转移一次弹药；`CanFire()` 在 Reloading 时拒绝开火；死亡会进入 Disabled 并清 Timer。不能把 `OnBlendOut` 当完成，因为 Blend Out 可能早于 Montage 真正结束。

**验证矩阵**：普通换弹、空仓换弹、Commit 前中断、Commit 后中断、连续按 R、换弹中开火、换弹中死亡和重新开始。

**条件变化**：整匣换弹继续使用一次 Commit；逐发装填需要每个 Notify 提交一发，并增加可中断的 Reload Policy，不能复用单次整匣提交语义。

### 19.4 近战漏检、重复扣血和动画中断

单帧 Notify 在低帧率下可能错过接触，全程武器碰撞又会在起手和收招误伤。最终使用 `AnimNotifyState` 给出有效时间窗，由 EnemyCharacter 在窗口内执行双 Socket 连续 Sweep：

```text
上一帧 WeaponTop/WeaponEnd
-> 当前帧 WeaponTop/WeaponEnd
-> 沿刀刃长度采样
-> 对各采样点执行帧间 Sphere Sweep
-> 追加当前刀刃线段 Sweep
-> 只接受 TargetCharacter
-> HitActorsThisAttack 整轮去重
```

窗口关闭、Montage Completed、Montage Interrupted、攻击 Timeout、攻击者死亡和目标死亡都汇入幂等收尾。正式 Montage 只保留 Attack Window，旧单点 Hit Notify 不与它并存。

**验证矩阵**：窗口外不伤害、窗口内只伤害一次、挥空、低帧率、多个窗口、Montage 中断、敌人死亡、玩家死亡和薄墙门框。

**条件变化**：出现友军、盾牌、可破坏物或多目标横扫后，再引入团队过滤、Damageable/HitZone 接口或专用碰撞语义；当前单目标近战没有必要增加通道和抽象。

### 19.5 敌人不移动、扎堆或靠近后弹开

“不移动”先检查 AIControllerClass、Auto Possess AI、NavMesh 和旧蓝图追击 Timer；“扎堆和弹开”则不是同一个问题。NavMesh 解决可达路径，CharacterMovement/RVO 处理局部移动，SurroundManager 处理战术目标位置。

项目最终把所有敌人 `MoveToActor(Player)` 改成双环槽位：内圈 8、外圈 12、同时攻击最多 2。管理器用弱引用分配槽位和攻击令牌，AIController 只在目标变化超过阈值或路径空闲时重新提交 MoveTo。

**验证**：放置敌人和动态生成敌人都能自动 Possess；绕障可达；内圈占满后进入外圈；敌人死亡释放槽位；玩家死亡后全部停止；没有旧 `chase player` Timer 并行写移动。

**条件变化**：

- 行为组合变复杂：FSM 迁移到 StateTree/Behavior Tree。
- 站位需要掩体、视线和地形评分：使用 EQS 生成候选点，但仍由管理器仲裁稀缺名额。
- 多玩家：目标选择和槽位集合按目标分组，不能继续假设唯一玩家。
- 数百个简化单位：评估 Mass/轻量 Agent，而不是继续堆完整 Character。

### 19.6 波次生成尖峰与出生失败

原实现一帧循环生成整波敌人，组件注册、SkeletalMesh、AnimInstance、Controller 和导航初始化集中在同一帧；高密度时固定小半径也无法容纳胶囊体。

修复方案：

```text
StartWave
-> 打乱出生点
-> 建立待生成队列
-> 每 0.05 秒生成一个
-> 重复使用出生点时按 sqrt(复用次数) 扩大 NavMesh 采样半径
-> 最大半径限制 2000 cm
-> Game End / EndPlay 清队列和 Timer
```

保留 `AdjustIfPossibleButDontSpawnIfColliding`，不使用 `AlwaysSpawn` 把生成失败变成角色重叠和解穿透。

**条件变化**：只有 Insights 证明 Spawn/GC 尖峰仍是 P95/P99 热点时才做对象池；对象池必须完整重置 Health、AI、Timer、Delegate、Montage、碰撞、Ragdoll、槽位和材质状态。

### 19.7 纹理池预算与 VSM 队列被误认为同一问题

Texture Streaming 管 Mip 驻留预算；VSM Non-Nanite Marking Queue 管非 Nanite 阴影投射物的页面标记任务。两者不能用同一个 CVar 处理。

纹理治理链：

```text
stat streaming / ListStreamingTextures / MemReport
-> Size Map 与 Reference Viewer 找到六张高占用环境纹理
-> Max Texture Size 限制到 2048
-> 保持 PoolSize 不变复测
```

Streaming Current/Target 从 `212.27 MB` 降到 `152.27 MB`，减少 `60 MB / 28.3%`；P95 基本不变，所以这是资源预算收益，不是帧率收益。

VSM 治理完成了页覆盖诊断、首批大面积资产检查、敌人阴影距离分级和三组单变量实验。关闭粗页包含虽然消除该次警告，却造成明显 Render Thread 回退，因此没有固化。最终结论是 80 敌人档仍可能单次复现，接受残留风险而不扩大队列掩盖问题。

**条件变化**：若目标变成常态 80 个以上同屏，继续按页覆盖排名拆分或转换大面积阴影投射物，并对同一机位多次 A/B 取中位数；不能盲目全局启用 Nanite 或关闭阴影。

### 19.8 尸体立即消失、长期保留与生命周期证据

死亡事实只提交一次，C++ 停止 AI、Movement、攻击 Timer 和 Capsule，蓝图负责 Ragdoll。尸体默认 `SetLifeSpan(30s)`，避免原 300 秒值让骨骼、物理和阴影跨多轮累积。

80 敌人生命周期实验中，等待 35 秒后 Enemy Actor 与 GameMode 注册数由 80 回到 0，UObject 减少 887，Movement 和 Animation 接近场景基线。进程工作集没有同步下降不等于 Actor 未回收，因为分配器可以保留已提交页面，MemReport 本身也会扰动工作集。

**条件变化**：需要尸体长期保留时，优先冻结布娃娃、关闭碰撞/动画/阴影或替换为静态代理；只有连续波次证明 Spawn/GC 是热点时再池化。

## 20. 渲染、物理与生命周期边界

### 20.1 Scene Query 与物理模拟

- Hitscan 是 `LineTrace` 场景查询，不创建真实子弹刚体。
- 近战是动画窗口内的 `Sphere Sweep`，不是全程武器 Overlap。
- `ApplyDamage/ApplyPointDamage` 是 Gameplay 伤害；Impulse、Ragdoll 和碰撞响应是物理表现，二者不应互相代替。
- CharacterMovement 使用胶囊体和受约束移动，不等于自由刚体模拟。
- 高频查询先控制频率、范围、过滤和错峰，再考虑并行；Worker Thread 不能直接安全访问 World 和 UObject。

### 20.2 当前后处理事实

`PostProcessComponent` 位于完整玩家蓝图，Timeline 在 Game Thread 更新 Blend Weight，渲染器再把相机、组件和 Volume 的设置合并到 View。已使用的表现包括饱和度、对比度、色彩偏移、暗角和景深；SSAO 只是知识储备，不能写成已实现。

选择蓝图是因为这些属于本地玩家表现和快速调参，不参与伤害或死亡判定。若以后支持分屏、观战或联网，需要迁移到 Camera/PlayerCameraManager 并明确只影响拥有者视图。

### 20.3 UObject 与 Actor 生命周期

```text
Gameplay Stop/Death
-> 清 Timer、Delegate、AI、槽位和攻击窗口
-> Destroy / SetLifeSpan 结束 World 生命周期
-> EndPlay 做幂等注销
-> 失去强引用
-> 后续 GC 回收 UObject
```

普通 C++ 资源用 RAII；UObject 不使用普通 `delete`。强持有通常使用 `UPROPERTY` + `TObjectPtr`，观察关系使用 `TWeakObjectPtr` 并在访问前检查有效性。析构函数不承担需要 World 上下文的 Gameplay 清理。

## 21. 条件扩展要点

以下内容是候选方案，不是当前成果：

| 条件变化 | 候选方案 | 引入前必须证明 |
| --- | --- | --- |
| AI 需要巡逻、搜索、听觉和丢失目标 | AI Perception + 扩展 FSM/StateTree | 当前目标注入无法表达刺激历史 |
| 站位需要掩体、视线和地形评分 | EQS | 固定双环不能满足地形需求 |
| 技能、Buff、免疫和叠层复杂 | GAS | Component/FSM 已出现跨系统组合爆炸 |
| 改为多人 FPS | GameState/PlayerState、RPC、Replication、预测与服务器回溯 | 明确服务器权威、延迟模型和作弊边界 |
| 波次首次加载造成明显卡顿 | Soft Reference + AssetManager 异步预取 | Insights 显示 IO 或首次资源创建影响 P95/P99 |
| Spawn/Destroy 与 GC 成为热点 | 专用对象池 | 连续波次存在可复现尖峰，且能定义完整 Reset 协议 |
| 角色数量进入数百 | Significance、Animation Sharing、Mass/轻量 Agent | CharacterMovement 与骨骼成本超过预算 |
| 多平台发布 | Device Profile、Scalability、输入映射、PSO 预热 | 每个平台有独立帧、内存和画质预算 |
| 遮挡提交成为主要瓶颈 | HZB/软件遮挡或更强实例剔除 | Render Thread/Draw/GPU 数据确认可见性成本 |
| 大量实例需要 GPU 侧生成和裁剪 | GPU Driven Rendering | CPU 提交和实例管理确为主要瓶颈 |
| 需要风格化画面 | Toon Diffuse、Rim、Outline | 先定义风格目标并做 BasePass/PostProcess 成本 A/B |

升级原则：先描述新增需求，再指出当前方案在哪个约束下失效，最后选择最小能解决问题的机制。不能为了名词更高级而替换已经足够的实现。

## 22. 项目陈述与证据清单

### 22.1 30 秒版本

> 这是一个 UE5 C++ 单机 PvE FPS。我把教程原型中的战斗、生命、AI 和对局规则重构为 Character、WeaponComponent、HealthComponent、EnemyAIController、SurroundManager 和 GameMode 的明确职责，蓝图只负责 Montage、UI、音效和后处理。主要难点是 NotifyState 双 Socket Sweep 的近战正确性、双环槽位与攻击令牌的群体行为，以及 10 到 160 敌人的性能和生命周期治理。最终矩阵中 40 敌人约 61 FPS、80 敌人约 55 FPS、160 敌人约 35.83 FPS，并有纹理预算下降 60 MB 和 80 敌人回收证据。

### 22.2 必须能打开的证据

- Character、WeaponComponent、HealthComponent、EnemyAIController、EnemyCharacter、SurroundManager 和 GameMode 的关键代码。
- 射击、换弹、近战、群体 AI、死亡和胜负六条调用链。
- 至少三个完整问题：错误 Target/实例、换弹事务、近战漏检或 MoveTo 扎堆。
- [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md) 的最终五档矩阵、VSM 单变量实验和生命周期数据。
- [PerformanceEvidence/20260816/README.md](PerformanceEvidence/20260816/README.md) 的关键截图。
- [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md) 中的发布阻塞与回归结果。

### 22.3 不能夸大的边界

- FPS 项目没有多人同步、GAS、行为树、EQS、对象池或自定义 Renderer。
- 后处理调色不是 Toon Shader，SSAO 未在项目中实现。
- 40 敌人约 60 FPS 是当前机器和固定场景口径，不是所有硬件保证。
- 当前矩阵证明容量和瓶颈，不证明每个优化项的净收益。
- VSM 队列仍有单次可复现残留，不能写成彻底解决。
- 蓝图全量编译已经通过；发布验收仍需完成 PIE 回归、Shipping 打包和产物冒烟测试。

## 23. 文档入口

- [README.md](README.md)：文档地图与阅读顺序。
- [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)：唯一性能数字来源。
- [PROJECT_TASK_CHECKLIST.md](PROJECT_TASK_CHECKLIST.md)：蓝图接线、当前任务和发布验收。
- [FPS_PROJECT_INTERVIEW_QA.md](FPS_PROJECT_INTERVIEW_QA.md)：项目高中频追问。
- `Learning/`：源码学习和扩展要点，不代表已实现。
- `Archive/`：历史计划和阶段记录，不覆盖当前状态。
