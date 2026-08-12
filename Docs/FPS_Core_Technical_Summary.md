# FPS 项目统一复习主线

> 核对基线：UE 5.5，代码与实验记录截至 2026-08-12。
>
> 本文以当前 C++ 为事实来源。蓝图动画、Timer、音效和后处理属于编辑器资产配置，标记为“蓝图约定”或“需回归验证”。
>
> `AfpstrueTargetDummy` 仅保留为早期测试类，不属于当前正式战斗链路。

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

详细记录见 [Health_And_Damage_System.md](Health_And_Damage_System.md)。

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

详细记录见 [Enemy_Attack_Window.md](Enemy_Attack_Window.md)。

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

### 蓝图需回归验证

- 删除蓝图射速 Timer 和对 Deprecated `Fire` 节点的调用。
- `OnWeaponFirePerformed` 每次只触发一轮枪口、声音、曳光和 Camera Shake 表现。
- 普通换弹和空仓换弹播放正确 Montage，并连接 Commit/Finish/Cancel。
- 换弹开始时 FOV 正确退出瞄准。
- 每个随机攻击 Montage 都设置攻击窗口。
- Montage 完成、混出和中断都通知 C++ 收尾。
- 致死伤害不会被普通受击动画覆盖。
- Camera Shake、声音和后处理不会重复触发。

### 当前已实现并有代码或数据证据

- AIController + NavMesh + `Idle / Chase / Attack / Dead` FSM。
- 双环包围槽位、攻击名额、死亡释放和外圈补位。
- 10/20/40/80/160 敌人固定场景 CPU Baseline，以及最终 100 AI 验收数据。
- Animation URO、不可见对象动画降级、CharacterMovement 距离分级代码。
- 纹理流送定点治理：六张环境纹理限制到 2048，Streaming Current/Target 从 212.27 MB 降至 152.27 MB，下降 28.3%。
- 已区分 Texture Streaming 与 VSM 页面标记告警。

### 已有接口或实现，但仍需最终回归/证据

- HUD、波次、倒计时、胜负和重启的完整编辑器回归。
- CPU 优化后同条件全档 A/B CSV；已有 100 AI 最终验收，但旧 160 AI 对比的镜头/UI 条件不完全一致，不能宣称精确提升百分比。
- 敌人进入攻击范围到真正起手的平均、P95 和最大响应延迟。
- 连续多轮战斗后的对象数量与内存回落曲线。
- 尸体、Niagara、Decal 和后处理的定量 GPU/生命周期报告。

### 尚未实现，不能写进“已完成”

- 行为树、EQS、AI Perception。
- 对象池和自定义内存分配器。
- Toon Shading、自定义 UE Renderer、Global Shader。
- FPS 项目内的多人网络；网络属于独立 Co-op 项目。

## 11. 当前技术债

- 旧武器蓝图仍可能保留射速 Timer，需要删除后做 PIE 回归。
- 换弹 C++ 事务接口已经具备，但 Montage Notify/Completed/Interrupted 尚未在蓝图完成接线；当前 Timer 只能作为兜底。
- 头部骨骼名硬编码为 `head / neck_01`，更换 Skeleton 时需要调整。
- CPU 分级优化已经写入代码，但优化后同条件数据尚未闭环。
- 包围槽位参数目前是工程默认值，仍需要用胶囊半径、攻击距离和拥堵数据解释或再校准。
- 当前中央管理器只维护一名玩家目标，不直接支持多玩家目标选择。
- 当前没有对“剑刃与玩家之间存在墙体”做额外视线遮挡检查，碰撞通道配置必须回归。
- 部分旧 C++ 文件混有非 UTF-8 注释，后续整理编码时必须单独提交，避免污染功能 diff。

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
| PERF-001：性能瓶颈误判 | Profile、线程归因、P95、停止条件 | 10～160 AI 和 100 AI 数据已有 |

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
| 160 AI 无法稳定 60 FPS | 权威基线中 Game Thread 20.733 ms；CharacterMovement 6.920 ms、Animation 3.190 ms、Pathfinding 0.071 ms | 优先做移动 60/30/15Hz 分级、Animation URO、不可见降级和死亡停更，不重写 NavMesh | Baseline 已有；优化后同条件 A/B 待测 |
| 误以为寻路是最大瓶颈 | 只凭系统复杂度猜测，没有看计时 | 用 CSV/Insights 分项后确认 Movement+Animation 更贵 | 已形成数据驱动结论 |
| 编辑器出现 Texture Streaming Pool 告警 | 独立运行预算 1000 MB、Over Budget 0 MB；定位到六张 4K/长边 4K 环境植被纹理 | 不扩大 Pool，只把六张纹理的 Max Texture Size 限制到 2048 | Streaming 212.27 -> 152.27 MB，下降 28.3% |
| `[VSM] 非 Nanite 标记工作队列溢出` | 属于 Virtual Shadow Map 页面标记，最大候选约 512 MB，不是 Texture Streaming Pool | 分开处理 VSM 与纹理流送；检查非 Nanite 大面积阴影投射 Mesh、阴影策略和 GPU Profile | 已分类，VSM 资产级优化未展开 |
| 工业场景材质缺失 | 日志指出 `/Game/FactoryDistrict/Materials/Black` 等引用不完整 | 修复 Redirector/缺失资产或替换材质；这属于资源完整性，不是内存泄漏 | 待资源回归 |
| 担心 Timer、Delegate、Widget、尸体泄漏 | 仅凭内存峰值不能判断泄漏 | 固定多轮 Spawn/Kill/Wait GC，记录 Actor/UObject 数和内存是否回落；Delegate/Timer 在死亡和 EndPlay 清理 | 生命周期代码已有，完整内存曲线待测 |

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

**取舍与遗留风险**：Sphere Sweep 比单点检测更稳，但查询量更高；固定 4 采样仍需和按端点位移自适应采样做 A/B。当前 `SweepMultiByObjectType(ECC_Pawn)` 不会让 WorldStatic 墙体成为阻挡结果，仍存在隔墙伤害风险，下一步应使用专用 Melee Channel 和固定碰撞矩阵验证。

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

**结果与验证边界**：架构、压力场景和 100 AI 数据已经存在。仍需确认 `enemy_BP` 没有旧 Tick/Timer/AI MoveTo，回归绕障、不可达目标、玩家死亡、重新 Possess 和关卡退出；AI 响应延迟与 Move Request 次数尚未量化。

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

**定位过程**：固定关卡、分辨率、VSync、敌人数和采样时长，对 10/20/40/80/160 敌人采集 CSV，并比较 Game Thread、P95、CharacterMovement、Animation、Pathfinding 和 GPU。160 敌人时 Game Thread 约 `20.733 ms`，CharacterMovement `6.920 ms`、Animation `3.190 ms`，Pathfinding仅 `0.071 ms`。

**结论与方案**：瓶颈首先是 Movement、Animation 和 Actor Tick，不是路径算法。优化顺序因此是决策降频/错峰、距离分级、Animation URO、不可见降级和死亡停更；只有 Profile 显示 Nav Query 或同步成为瓶颈时，才考虑更复杂的路径缓存、异步或多线程方案。

**验证结果与诚实边界**：Baseline 和 100 AI 验收数据可用于说明定位方法；旧 160 AI 与优化后 100 AI 的镜头/UI 条件不完全一致，不能声称精确性能提升百分比。后续仍要在同条件下各运行至少三次、取中位数并记录响应延迟和画质/动作正确性。

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

### 18.1 原始 CPU Baseline

以下数字统一引用 `PERFORMANCE_BASELINE.md` 的同一套标准化结果，不混用早期脚本的不同计数口径：

| 敌人数 | Frame Avg ms | Frame P95 ms | Game Avg ms | GPU Avg ms | Animation ms | CharacterMovement ms | Pathfinding ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 12.752 | 15.014 | 3.233 | 10.443 | 0.364 | 0.561 | 0.000 |
| 20 | 13.256 | 15.478 | 4.178 | 11.059 | 0.584 | 0.846 | 0.001 |
| 40 | 14.165 | 15.869 | 6.239 | 12.476 | 0.908 | 1.633 | 0.013 |
| 80 | 15.872 | 17.523 | 11.404 | 14.489 | 1.713 | 3.087 | 0.026 |
| 160 | 20.741 | 23.196 | 20.733 | 16.916 | 3.190 | 6.920 | 0.071 |

160 AI 平均帧率约为：

```text
1000 / 20.741 ≈ 48.2 FPS
```

因此不能说“160 AI 稳定 60 FPS”。80 AI 平均值在预算内，但 P95 超过 16.67 ms，也不能称为稳定 60 FPS。

### 18.2 当前可以说的优化

- 已根据数据实现移动更新 20 米内每帧、20～40 米约 30 Hz、40 米外约 15 Hz。
- 已开启 Animation URO，不可见敌人只更新必要 Montage。
- 攻击窗口内恢复高频更新，避免 Socket Sweep 精度下降。
- 死亡后停止移动更新。
- 100 AI 最终验收中 Frame Avg `15.41 ms`，Frame P95 `16.58 ms`，实际生成的 Enemy 与 AIController 均为 100。
- 六张高占用环境纹理定点限制到 2048，Streaming Current/Target 从 `212.27 MB` 降至 `152.27 MB`，下降 `60 MB / 28.3%`。
- 纹理调整前后 Frame P95 分别为 `16.44/16.58 ms`，因此只宣称资源预算收益，不宣称帧率提升。

### 18.3 当前不能说的结果

- 旧 160 AI 优化前后测试的镜头/UI 条件不完全一致，所以不能把差值包装成精确提升百分比。
- 没有完成响应延迟统计，不能声称降频对攻击反应没有影响。
- 没有完整内存回落曲线，不能声称“零泄漏”。
- 没有实现 Toon，不能把 PostProcess 调色包装成自定义 Shader。
- VSM 非 Nanite 标记队列告警已经和纹理流送问题分开，但尚未完成资产级阴影治理。

详细实验和原始数据见 [Development_Experience_And_Optimization.md](Development_Experience_And_Optimization.md)。

## 19. 项目深挖 28 题：答案与追问

### 19.1 一面：C++ 与 UE 基础

#### Q1. HealthComponent 的 TakeDamage 为什么是虚函数？

先纠正前提：当前 `UfpstrueHealthComponent` 没有定义虚拟 `TakeDamage`，它在 BeginPlay 绑定 Owner 的 `OnTakeAnyDamage`，再进入非虚的内部扣血函数。`AActor::TakeDamage` 才是引擎可重写入口。面试时不能顺着错误前提编故事。

虚函数通过对象中的 vptr 间接查 vtable 实现运行时多态；vptr 属于每个多态对象，vtable 通常由同类对象共享，具体布局和存储区属于 ABI 实现细节。

追问：为什么组件选择订阅 Damage Delegate？因为玩家和敌人可复用相同组件，不要求每种 Actor 都复制扣血逻辑。

#### Q2. TWeakObjectPtr 怎么工作，为什么不用裸指针？

UE 弱对象指针通过 UObject 的对象索引和序列号识别对象，不增加强持有；对象销毁或槽位被复用后，`IsValid()` 会失败。SurroundManager 用它保存敌人，可以避免悬空裸指针，并避免 Manager 的长期引用阻止对象释放。

追问：弱引用仍需要主动释放槽位，因为“最终能识别失效”不等于“业务状态立刻更新”。

#### Q3. Delegate 的绑定对象被销毁会怎样？

UObject 动态委托绑定能够识别失效 UObject，不应再调用已销毁实例；但长期存活的发布者仍可能保留无效绑定或发生重复绑定。项目应在合适的 EndPlay/销毁路径 `RemoveDynamic`，并避免多次 `AddDynamic`。原生 `AddRaw` 不具备 UObject 生命周期保护，风险更高。

#### Q4. Timer 和 Tick 有什么区别？

Timer 不是后台线程；World 的 TimerManager 在 Game Thread 上按到期时间调度回调。Tick 是注册对象几乎每帧收到更新，适合连续模拟；Timer 适合低频决策和延迟任务。项目让 AI 决策按 0.1～1.0 秒分级，但 CharacterMovement 和攻击窗口仍保留必要更新。

追问：Timer 降低总调用次数，却可能让大量同帧到期产生尖峰，所以要加入随机错峰并记录响应延迟。

#### Q5. 为什么变量要加 UPROPERTY？

它让 UHT 生成反射元数据，从而接入编辑器、蓝图、序列化、复制和 UObject GC 引用跟踪。没有 UPROPERTY 的普通 C++ 变量仍能运行，但不会自动获得这些能力。`EditAnywhere`、`BlueprintReadOnly` 和 GC 持有是不同维度，不能混为一谈。

#### Q6. AnimNotifyState 的 Begin/Tick/End 什么时候触发？

Begin 打开攻击窗口，Tick 按动画时间更新刀刃 Sweep，End 关闭窗口。Montage 正常结束或通常中断时引擎会处理活动 Notify State，但销毁 Mesh、突变动画状态和业务取消不能只押在 End 上，因此项目在 Montage 中断、敌人死亡、目标死亡、攻击结束和超时都走统一取消路径。

#### Q7. NavMesh 投影调用什么，失败怎么办？

使用 `UNavigationSystemV1::ProjectPointToNavigation`。失败表示候选点不在当前可用导航数据/查询范围内；分配时跳过该槽位，已分配位置后来失效时重新请求，最后才回退到普通追逐。NavMesh 未构建或世界没有 NavigationSystem 时也必须处理 false，不能把未初始化位置传给 MoveTo。

### 19.2 二面：近战、包围与性能

#### Q8. 双 Socket Sweep 与单点帧间 Sweep 有什么区别？

双 Socket 描述当前帧整段剑刃，但只扫当前线段可能漏掉帧间移动体积；单点帧间 Sweep 能覆盖某个点的运动，却可能漏掉剑刃其他部位。当前实现结合两者：在 WeaponTop/WeaponEnd 之间取 4 个采样点，各自从上一帧扫到当前帧，再补一次当前整段 Sweep。

#### Q9. 如何区分同一挥砍重复命中和下一次挥砍？

`HitActorsThisAttack` 的生命周期是整轮攻击，不是单个窗口。`TryAttackTarget()` 开始时清空集合，`BeginAttackWindow()` 只初始化 Socket 采样，不重置去重状态；命中成功后加入弱引用，后续 Tick 或同一 Montage 的第二个窗口都会跳过。下一轮攻击才重新清空。Combo 分段、网络预测或异步攻击出现后，可再引入显式 AttackSequenceId。

#### Q10. 刀刃会不会穿墙伤人？

当前 `SweepMultiByObjectType` 只查询 `ECC_Pawn`，没有把 WorldStatic 当阻挡，因此在极端情况下存在穿墙命中风险。这是当前明确限制。修复可改为会被世界静态物阻挡的 Trace Channel，按距离处理第一个 Blocking Hit，或在结算前做 Enemy 到 Target 的 LOS 检查。

#### Q11. Montage 中断时要清理什么？

关闭攻击窗口、重置上一帧 Socket 采样、清空本次命中集合、清除攻击结束 Timer、恢复 `bIsAttacking` 和动画优先级、释放攻击 Token，并根据状态重新 Chase。漏掉窗口会继续扣血，漏掉 Token 会阻塞其他敌人，漏掉 Timer 会产生延迟回调。

#### Q12. 为什么是内圈 8、外圈 12？

这是可调工程初值，不是算法常数。250 cm 内圈周长约 1570 cm，8 槽位中心间弧长约 196 cm；430 cm 外圈周长约 2700 cm，12 槽位约 225 cm，给 Character 胶囊和避让留出空间。内圈用于攻击候选，外圈用于等待。最终应结合胶囊直径、攻击接近距离、门宽和卡住率校准。

#### Q13. 两个敌人都想要同一槽位怎么办？

所有分配发生在 Game Thread 的中央 Manager 中。循环找到最近有效空槽后立即写入 Occupant 和 EnemyToSlot；下一个敌人再查询时该槽已占用，不会同时成功。若未来多线程计算候选，提交阶段仍需在主线程或用同步机制做仲裁。

#### Q14. 槽位 NavMesh 投影失败怎么办？

候选分配时直接跳过失败槽位并尝试其他位置；没有可用槽位时返回失败，Controller 可短期回退 MoveToActor/重新调度。不能把投影失败的原始点强行用于移动，否则容易走进桌子、墙体或导航边界。

#### Q15. 两个攻击名额都满了，其他敌人做什么？

没有 Token 的内圈敌人保持自己的槽位，停止反复 MoveTo，并持续面向玩家等待；外圈敌人等待补位。这样减少全员同时攻击、碰撞推挤和路径抖动。未来可以增加侧移/威吓动画，但不应影响权威名额。

#### Q16. 攻击名额会永久占用吗？

不会。控制器保存 Token 获取时间，超过 4 秒未完成攻击会释放；正常完成、中断、HitReact、死亡、目标死亡或离开有效条件也应释放。这是典型“资源租约”设计。

#### Q17. 外圈补位怎么处理？

内圈释放后，Manager 找到离该位置最近的外圈占用者，把它迁入内圈；它原来的外圈槽自然变空，供后续新敌人申请。当前一次只补一个，不递归搬动整圈，避免一次死亡触发大量重新寻路。

#### Q18. 20.733 ms 怎么解释？

60 FPS 帧预算是 16.67 ms。160 AI 时 Game Thread 平均 20.733 ms，Frame 平均 20.741 ms，对应约 48.2 FPS，明确超预算；80 AI 虽平均 15.872 ms，但 P95 17.523 ms，也不能称为稳定 60 FPS。

#### Q19. 为什么 CharacterMovement 比 Pathfinding 贵？

路径不是每帧重算：AI 决策被降频，目的地移动超过 150 cm 或 PathFollowing Idle 才重新提交 MoveTo。CharacterMovement 则对每个活跃 Character 持续做移动积分、地面/碰撞查询和网络友好的移动处理，所以高密度下累计成本更大。数据说明应优化实际热点，而不是凭感觉重写 A*。

#### Q20. 15/30/60 Hz 如何切换？

目标距离小于 20 m 时 TickInterval=0；20～40 m 约 0.033333 s；40 m 外约 0.066667 s。攻击开始强制恢复每帧，确保 Montage 和 Socket 精度。当前按阈值直接切换，依靠 CharacterMovement 的连续速度保持运动；若边界抖动，应增加迟滞区间，而不是继续降频。

#### Q21. 不可见敌人怎么降动画更新？

使用 SkeletalMesh 的 Animation Update Rate Optimization 和 `VisibilityBasedAnimTickOption`。普通状态不可见时只保留必要 Montage；攻击时临时切 `AlwaysTickPoseAndRefreshBones`，否则 Socket 不刷新会导致攻击窗口错误。不能简单依赖“看不见就完全停”，因为刚转入视野和近战判定会出问题。

### 19.3 三面：系统设计和成长

#### Q22. 改成 2～4 人 Co-op 要改什么？

当前 FPS 是单机。联机时伤害、Health、AI 决策、波次和 SurroundManager 必须由服务端权威；客户端只请求开火/表现，血量用 RepNotify，关键表现按条件复制。槽位不能只围绕固定玩家 0，需要服务端选择仇恨目标并为每个玩家维护局部包围组。当前没有在 FPS 中实现这些，只能作为设计方案；网络实作属于独立 Co-op 项目。

#### Q23. 不用中央 Manager 可以吗？

可以。稀疏大世界中，每个敌人基于局部邻居独立选点更易扩展，也减少中央 O(N) 协调；但会产生重复目标和冲突。当前近距离群体战规模有限，全局不变量重要，所以中央式更合适。更大规模可按玩家/网格分区成多个局部 Manager。

#### Q24. 三个管理器耦合如何？加入技能系统会动哪里？

GameMode 负责生成并注入 Player/SurroundManager；AIController 依赖这两个运行时上下文；EnemyCharacter 通过窄接口执行攻击。技能系统应作为 Character/ActorComponent，订阅 Health/状态事件并向伤害入口提交效果，不应直接改槽位容器或 UI。当前主要耦合点是 GameMode 的创建和注入，可进一步抽成接口或 Subsystem，但项目规模下暂不为抽象而抽象。

#### Q25. 如果做 Toon Shading 从哪里切入？

当前未实现。最小方案是材质函数完成 N·L 色阶、阈值高光和 Rim，再用 CustomDepth/Stencil 的 PostProcess 做指定对象描边，逐项用 ProfileGPU/Material Stats 记录成本。修改 BasePass 或自定义 Shading Model 更深入，但会增加引擎源码维护，不适合当前收口版本。

#### Q26. 非科班最大的困难是什么？

回答重点不是自我贬低，而是证据：基础体系需要主动补齐，因此用项目调用链连接 C++、UE 生命周期、数据结构和性能工具；遇到接口无反应时不再只重接节点，而是检查对象类型、事件源、绑定时机和生命周期。不要虚构实习或团队经验。

#### Q27. 如果重做，最想改哪个决定？

可用真实经历回答：早期把追逐直接写在 Enemy 的每帧逻辑里并 MoveTo 玩家中心，功能快但拥堵、职责混合、性能不可量化；后来迁到 AIController + SurroundManager。若重做，会先定义职责、性能指标和测试场景，再实现玩法，减少后期迁移成本。

#### Q28. 没有正式美术/策划协作经历怎么办？

坦诚说明当前主要是独立开发。能提供的团队准备包括：C++ 给蓝图稳定接口、参数暴露而不硬编码表现、Git 小步提交、Bug 复盘、架构/调用链文档。入职后的风险是需求沟通和多人合并经验不足，解决方式是先确认接口与验收标准、拆小提交并主动同步阻塞。

## 20. 面试文档统一题库

下面把之前几份一面、二面、三面和游戏开发面经去重。标记为“项目”的题必须关联本项目；标记为“理论”的题需要单独写小程序或画图验证。

### 20.1 C++ 与 STL 高频题

| 题目 | 答题锚点 | 与项目的连接 |
| --- | --- | --- |
| 指针和引用 | 指针可空、可改指向且有自身存储；引用必须绑定有效对象且不能改绑 | UE UObject 常用指针；必选依赖可用引用 |
| new/delete 与 malloc/free | 前者是运算符并调用构造/析构，后者只管理原始字节；不能混用 | UE UObject 通常由 NewObject/SpawnActor 和 GC 管理 |
| 虚函数与多态 | vptr/vtable、指针/引用动态分派、虚析构 | UE 生命周期 override；不要声称 HealthComponent 自己的 ApplyDamage 是 virtual |
| 构造/析构中调用虚函数 | 不会分派到尚未构造或已经析构的派生部分 | UE 构造函数只建默认组件，依赖 World 的逻辑放 BeginPlay |
| 多继承内存布局 | 多个多态基类通常有多个 vptr；布局依 ABI；虚继承解决公共基类重复 | 面试画示意图，不把 ABI 细节说成标准强制 |
| 内存对齐 | 成员按对齐要求放置，结构体总大小对齐最宽成员；示例 `char,int,short,double` 常见 64 位结果 24 | 调整成员顺序可减少 padding，但先用 sizeof/alignof 验证 |
| const | 顶层/底层 const、const 成员函数、指针常量区别 | 查询函数如 GetHealth 应 const |
| 深拷贝/浅拷贝/移动 | 资源所有权、Rule of 0/5、move 后源对象有效但状态未指定 | UE 容器移动、避免裸资源所有权 |
| shared_ptr/weak_ptr | 控制块、强弱计数、循环引用、计数安全不等于对象线程安全 | UObject 用 UE 指针体系；SurroundManager 用 TWeakObjectPtr |
| RAII | 构造获取、析构释放，异常和早返回都安全 | 非 UObject 资源、锁和文件句柄；Actor 生命周期由引擎管理 |
| vector/TArray | 连续内存、扩容导致搬迁和迭代器失效；TArray 接入 UE allocator/反射/序列化生态 | 槽位适合 TArray，因为数量固定且遍历多 |
| map/unordered_map | 红黑树 O(logN) 有序；哈希均摊 O(1)、最坏 O(N) | EnemyToSlot 用 TMap 做快速反查，不依赖排序 |
| push_back/emplace_back | emplace 原位构造；已有对象时 push 的 move 可能同样高效 | 不绝对宣传 emplace 永远更快 |
| 四种 cast | static/const/dynamic/reinterpret 的用途和风险 | UE Cast 基于反射类型系统，失败返回 null |
| explicit | 阻止构造函数或转换运算符参与不希望的隐式转换 | 不要和 volatile 混淆 |
| lambda/捕获 | 值/引用捕获生命周期；异步回调不要捕获即将销毁对象裸 this | UE Timer/Async 回调要考虑 UObject 有效性 |
| 完美转发 | 转发引用 + `std::forward` 保留值类别 | 理论题，用小程序验证 |

### 20.2 UE 与工程题

| 题目 | 标准回答框架 |
| --- | --- |
| Actor/Component/Pawn/Character/Controller | Actor 是世界对象；Component 组合能力；Pawn 可被控制；Character 增加胶囊和 CharacterMovement；Controller 负责意图与控制 |
| GameMode/GameState/PlayerState | GameMode 仅服务端规则；GameState 复制全局状态；PlayerState 复制玩家状态；单机项目仍可用 GameMode 管规则 |
| UObject 与 GC | UPROPERTY/TObjectPtr 表达强持有，TWeakObjectPtr 不持有；不要手动 delete UObject；区分 RemoveFromParent、Destroy 和最终 GC |
| UCLASS/UPROPERTY/UFUNCTION/UHT | 宏被 UHT 解析并生成注册代码，接入反射、蓝图、序列化和 GC |
| Delegate 和 Event Dispatcher | 发布订阅、绑定时机、重复绑定、对象销毁和解绑；UI 使用事件而不是 Tick 查询 |
| Tick/Timer/Async | Tick 连续模拟；Timer 主线程低频调度；Async 只做线程安全纯计算，UObject/Actor 修改回主线程 |
| AnimNotify 与 NotifyState | 单点事件 vs 持续窗口；近战检测需要 State；必须有中断兜底 |
| NavMesh/Recast/Detour | Recast 从碰撞体素化并生成多边形导航网格；Detour 在 Polygon Corridor 上寻路并做路径走廊处理；项目只调用上层导航 API |
| RVO/Detour Crowd/包围槽位 | 避让解决局部碰撞，槽位解决战术目标分配，NavMesh 解决全局可达路径 |
| LOD/URO | 几何 LOD 降三角形；动画 URO 降骨骼更新；死亡后停移动/碰撞/无用动画 |
| Unreal Insights/stat/CSV | 先判断 Game/Draw/GPU，再定位函数/系统；固定场景、重复测量、看 Avg/P95 |
| 编译、Live Coding、生成工程文件 | 普通 Build 生成模块；Live Coding 适合函数体小改，不适合 UCLASS 布局/反射大改；工程文件丢失时从 `.uproject` 重生成 |
| 设计模式 | 能指出组件、状态、观察者、更新方法、中央协调器的具体代码，不背模式名 |

### 20.3 操作系统、并发和网络基础

| 题目 | 答题锚点 |
| --- | --- |
| 进程和线程 | 进程是资源隔离单位，线程共享进程地址空间，是调度执行单位；线程切换通常更轻 |
| 栈和堆 | 栈自动生命周期、局部性好；堆动态、灵活但有分配成本和碎片；不要把 C++ 自由存储区与 OS 堆绝对等同 |
| 虚拟内存和页表 | 虚拟地址经页表映射物理页，TLB 缓存；缺页导致高成本；工作集和局部性影响性能 |
| 锁/原子/条件变量 | 原子适合简单共享状态；锁保护复合不变量；条件变量等待条件并配合互斥锁；先避免共享再谈无锁 |
| Game/Render/RHI Thread | Game 生产场景状态，Render 构建渲染命令，RHI 面向图形 API；线程间存在帧流水和同步点 |
| 为什么当前不优先 AI 多线程 | AI 会访问 UObject、NavSystem 和世界状态；线程安全改造成本高，当前热点又是 Movement/Animation，先做降频更合理 |
| TCP 与 UDP | TCP 可靠有序拥塞控制；UDP 数据报、低开销但可靠性自理；实时游戏常基于 UDP 做选择性可靠 |
| RPC/Replication/RepNotify | RPC 表达事件请求，属性复制表达状态；服务端校验伤害；RepNotify 在客户端处理状态变化表现 |
| Authority/Ownership/Relevancy | 服务端权威；Client RPC 需要正确 Owner；Relevancy/NetCullDistance/UpdateFrequency 控制复制范围和频率 |
| Listen Server 与 Dedicated Server | Listen 同时是玩家有主机优势；DS 独立权威更公平但部署运维复杂；当前 FPS 未实现，Co-op 项目单独验证 |

### 20.4 图形学题

| 题目 | 答题锚点 |
| --- | --- |
| 模型到屏幕 | DCC 导出 -> 顶点/索引/UV/法线切线 -> Local/World/View/Clip -> 裁剪 -> 光栅化 -> 像素/材质 -> 深度/混合 -> 后处理 |
| 坐标变换 | 模型、世界、观察、裁剪、NDC 和屏幕空间；齐次坐标支持平移与透视除法 |
| Blinn-Phong | Ambient + max(N·L,0) Diffuse + pow(max(N·H,0),shininess) Specular；UE 实际使用 PBR，金属漫反射接近零、反射颜色来自 BaseColor |
| PBR Metal/Roughness | 能量守恒、微表面 BRDF、Fresnel、NDF、Geometry；Metallic 决定导体/介质，Roughness 控制高光展宽 |
| Shadow Map | 从光源渲染深度，再比较当前点光空间深度；Acne 用 Bias/Normal Bias，Peter Panning 是 Bias 过大，PCF 软化锯齿 |
| Deferred vs Forward | Deferred 先写 GBuffer 再逐光照，动态光多时优势明显；透明和 MSAA 处理受限；Forward 对多采样和透明更自然 |
| LOD | 距离/屏幕尺寸降低几何和材质成本；切换需防跳变；Nanite 是虚拟化几何但并不消除材质、阴影和动画成本 |
| Toon 方案 | 当前未实现；材质函数做色阶/硬高光/Rim，CustomDepth/Stencil 后处理描边，再用 ProfileGPU 定量 |
| 纹理流送与 VSM | Streaming Pool 管纹理 Mip；VSM Physical Page Pool 管虚拟阴影页，两者告警不能混治 |
| 点在三角形内 | 重心坐标；或三条边叉积同号。说明边界点和浮点误差处理 |

### 20.5 算法与现场编码题

#### 反转单链表

迭代维护 `prev/current/next`，O(N) 时间 O(1) 空间；递归先反转后半段，再令 `head->next->next=head`，O(N) 栈空间。现场必须能写可编译代码并处理空链表。

#### 第 K 大元素

QuickSelect 平均 O(N)、最坏 O(N²)，适合原地一次查询；大小为 K 的最小堆 O(N logK)，性能稳定且适合流式数据。先问是否允许修改输入和 K 的大小。

#### 10000 敌人最近目标

不能保证任何结构都“绝不退化”，但可用 Spatial Hash/Uniform Grid：敌人移动时更新所在格；玩家查询所在格并逐圈扩展邻格，用平方距离维护最小值，当下一圈最小可能距离已经大于当前最优时停止。均匀分布下远小于 O(N)，最坏仍可能 O(N)。静态或低频更新可用 k-d tree/BVH/八叉树。

```text
UpdateEnemy(enemy):
    oldCell.remove(enemy)
    newCell = floor(enemy.position / cellSize)
    grid[newCell].add(enemy)

FindNearest(player):
    best = null, bestDist2 = INF
    for ring = 0..maxRing:
        for cell in CellsOnRing(playerCell, ring):
            for enemy in grid[cell]:
                update best by DistSquared
        if MinDistanceToNextRingSquared > bestDist2:
            break
    return best
```

#### Fisher-Yates 均匀洗牌

从 `i=n-1` 到 `1`，在 `[0,i]` 均匀随机选 `j` 并交换。第一个确定的位置有 n 种等概率选择，下一位置有 n-1 种，最终每个排列概率为 `1/n!`。若从全数组每次随便交换，通常不是均匀分布。

```cpp
for (int i = n - 1; i > 0; --i)
{
    int j = RandomInt(0, i);
    Swap(a[i], a[j]);
}
```

#### 射线如何加速

窄阶段做 Ray-Triangle 前先用 BVH/Octree/Uniform Grid 做宽阶段，先排除绝大多数几何。UE LineTrace 使用碰撞场景查询结构，项目层不应每发子弹遍历所有 Actor。

#### 游戏算法补充清单

必须能写或讲：BFS/DFS、堆/TopK、二分、LRU、并查集、拓扑排序、A* 的 `f=g+h`、BVH/AABB、Spatial Hash、FSM。Behavior Tree、EQS、RVO 只需先理解适用场景，不能说已在项目完成。

### 20.6 项目与开放题

| 题目 | 回答结构 |
| --- | --- |
| 30 秒介绍 FPS | UE5 C++ 单机 FPS；组件化射击/健康；NotifyState 近战；AIController+NavMesh+双环包围；160 AI 数据定位 Movement/Animation 热点 |
| 2 分钟项目介绍 | 背景 -> 架构 -> 两个难点 -> 一组数据 -> 一个限制 -> 下一步 |
| 最大 Bug | 选“MoveToActor 扎堆重构”或“Widget 错实例无法移除”，讲现象、断点/日志、对象类型或数据证据、根因和回归 |
| 为什么 UE 不选 Unity | 强调 C++、Gameplay Framework、Navigation、Profiling 和目标岗位；不贬低 Unity |
| 最喜欢的游戏 | 选真正玩过的，按输入反馈、相机、动画、网络或 AI 拆技术模块，避免只讲剧情感受 |
| Git 怎么协作 | 功能分支、小提交、先 pull/rebase 理解冲突、测试后 push；当前个人项目不假装多人协作 |
| 优势与短板 | 优势是自驱、完整问题复盘和量化意识；短板是科班体系与真实团队经验，给出正在补的具体计划 |
| AI 工具怎么用 | 用于查 API、生成候选方案和文档；最终通过源码、编译、运行数据和闭卷讲解取得代码所有权 |

## 21. 统一复习顺序

### 第一轮：项目所有权

1. 画出第 13 节架构图。
2. 不看文档讲第 14 节五条调用链。
3. 打开代码指出每个关键状态、Timer、Delegate 和数据结构的位置。
4. 闭卷重写 HealthComponent、单次射击和槽位分配伪代码。

### 第二轮：Bug 与取舍

1. 从第 16 节选 5 个真实问题，每个控制在 2 分钟。
2. 必须包含：对象 Target 类型、Widget 错实例、攻击窗口重复伤害、MoveToActor 扎堆、160 AI 性能定位。
3. 每个问题都说清“为什么没采用另一个方案”。

### 第三轮：数据和边界

1. 背熟 16.67 ms、20.733 ms、48.2 FPS、6.920/3.190/0.071 ms，并知道它们分别代表 Game Thread、CharacterMovement、Animation 和 Pathfinding。
2. 解释为什么优化 Movement/Animation，不重写 NavMesh。
3. 明确旧 CPU A/B 的条件限制，以及响应延迟、完整内存回落曲线仍未完成。

### 第四轮：模拟面试

- 一面：第 20.1～20.3 节，每次随机 10 题。
- 二面：第 19.2 节逐题深挖，要求画图或写伪代码。
- 图形专项：第 20.4 节，必须能手画渲染管线和 Shadow Map。
- 算法专项：第 20.5 节，每题限时 20 分钟写可运行代码。
- 三面：第 19.3 和 20.6，录音后检查是否夸大事实。

### 详细证据索引

- [Health_And_Damage_System.md](Health_And_Damage_System.md)：Health/伤害/死亡细节。
- [Enemy_Attack_Window.md](Enemy_Attack_Window.md)：NotifyState、双 Socket Sweep 和去重。
- [Development_Experience_And_Optimization.md](Development_Experience_And_Optimization.md)：Bug 时间线、CPU/纹理数据和优化实验。
- [Portfolio_Technical_Extension_Map.md](Portfolio_Technical_Extension_Map.md)：后续扩展，不作为当前已完成事实。

本文件负责“怎么复习和怎么回答”；其他文件只负责“证据在哪里”。

## 22. 面试项目介绍总纲

这一节把项目背景、实现边界、工程验证和后续方向串成一条完整叙事。面试时先讲事实，再接受追问，不要从功能清单开始背。

### 22.0 完整项目介绍与框架结构

#### 项目定位

这是一个使用 UE5、C++ 和 Blueprint 实现的单机第一人称射击 Demo。玩家进入关卡后拾取武器，在限定时间内应对分波生成的近战敌人；系统包含射击与换弹、共享生命组件、敌人近战攻击、AI 导航与显式状态机、群体包围、HUD、胜负结算和重新开始。

项目重点不是继续增加武器和玩法数量，而是围绕两个实际问题深化：

1. 多个近战敌人同时追逐玩家时，怎样避免所有 AI 以玩家中心为同一目标而扎堆，并限制同时攻击人数。
2. 高密度敌人场景中，怎样区分移动、动画、寻路和纹理资源成本，用固定场景和数据验证优化结果。

#### 总体分层

```text
关卡与配置层
TargetPoint / NavMeshBoundsVolume / 敌人蓝图 / 动画与材质资产
                    |
                    v
规则协调层
AfpstrueGameMode ---------------- AfpstrueSurroundManager
波次、倒计时、生成、胜负          包围槽位、攻击令牌、补位与释放
                    |
                    v
AI 与实体执行层
AfpstrueEnemyAIController -------- AfpstrueEnemyCharacter
FSM 决策、MoveTo、转向             近战执行、攻击窗口、受击和死亡
                    |
                    v
战斗组件层
AfpstrueCharacter
  |- UfpstrueWeaponComponent      拾取、射击、弹药、换弹和后坐力
  `- UfpstrueHealthComponent      受伤、血量、死亡和事件广播

敌人和测试靶同样复用 UfpstrueHealthComponent
                    |
                    v
表现层
AnimBlueprint / Montage / AnimNotifyState / UMG / 音效 / 特效 / 后处理
```

这不是严格的传统 MVC，而是借鉴了“规则、数据状态和表现分离”的思路：C++ 对结果负责，蓝图对资产编排和视觉表现负责。`GameMode` 与 `SurroundManager` 是协调者，不直接播放动画；`AIController` 决定去哪里，`EnemyCharacter` 负责怎样执行；组件封装可复用的战斗能力。

#### 核心类与职责

| 类或模块 | 当前职责 | 不负责什么 |
|---|---|---|
| `AfpstrueGameMode` | 游戏开始防重、90 秒倒计时、波次生成、敌人注册表、存活数量、胜负判定、Timer 清理，并向蓝图广播状态 | 不执行单个敌人的移动和攻击，不处理 HUD 样式 |
| `AfpstrueCharacter` | 玩家输入入口、移动/瞄准/冲刺、当前武器引用和死亡协调 | 不保存弹药、射速或换弹事务，不负责关卡波次 |
| `UfpstrueWeaponComponent` | 武器拾取与挂接、Hitscan、随机散布、弹药、普通/空仓换弹、后坐力和伤害分发 | 不绑定玩家输入，不直接管理敌人死亡或 UI 布局 |
| `UfpstrueHealthComponent` | 监听伤害、Clamp 血量、保证死亡只结算一次，并用 Delegate 广播血量和死亡 | 不播放受击动画，不判断攻击是否命中 |
| `AfpstrueEnemyAIController` | Timer 驱动的 `Idle / Chase / Attack / Dead` 决策、导航请求、停止移动和朝向更新 | 不计算最终伤害，不保存玩家 HUD |
| `AfpstrueEnemyCharacter` | 接收 AI 决策、播放攻击接口、管理攻击窗口、双 Socket Sweep、单次攻击去重、受击与死亡清理 | 不集中分配其他敌人的站位 |
| `AfpstrueSurroundManager` | 双环槽位、最近空槽分配、NavMesh 投影、攻击令牌、外圈补位和弱引用清理 | 不代替 Recast/Detour 寻路，不直接移动角色 |
| `UfpstruePickUpComponent` | 接收真实 Overlap Actor、占用一次性消费门禁、调用武器挂接，成功后关闭碰撞并销毁自身 | 不保存武器弹药，不使用固定 Player 0 代替事件中的拾取者 |
| Blueprint / UMG | 动画资产选择、Montage、Notify 时间、音效、特效、镜头和界面表现 | 不作为血量、弹药、AI 和胜负规则的权威来源 |

#### 一局游戏的完整运行链路

```text
1. 关卡加载
   -> World Settings 选择 AfpstrueGameMode 的蓝图子类
   -> 关卡提供 EnemySpawn 标签的 TargetPoint 和 NavMesh 数据

2. 开始游戏
   -> 开始界面完成输入/镜头切换
   -> 蓝图调用 GameMode 的 StartGameMode
   -> GameMode 防止重复开始，初始化 90 秒倒计时和第一波生成

3. 生成敌人
   -> GameMode 收集多个出生点并轮换/随机选择
   -> SpawnActor 生成 EnemyCharacter
   -> AIController Possess 敌人
   -> 注入玩家目标与 SurroundManager 上下文

4. AI 追逐与包围
   -> EnemyAIController 的低频决策 Timer 更新 FSM
   -> 敌人向 SurroundManager 请求空闲槽位
   -> 理论环形位置投影到 NavMesh
   -> AIController 对独立槽位执行 MoveToLocation
   -> 到达攻击区域后申请攻击令牌；未拿到令牌则等待或补位

5. 玩家射击
   -> Enhanced Input
   -> Character / WeaponComponent
   -> 相机方向 LineTraceSingleByChannel
   -> FHitResult 提供命中 Actor、位置、法线和骨骼名称
   -> 根据普通部位或头部计算伤害
   -> ApplyPointDamage
   -> 目标 HealthComponent 更新血量并广播事件
   -> 蓝图播放命中特效、受击动画和 HUD 表现

6. 敌人近战
   -> FSM 进入 Attack
   -> C++ 调用 OnAttackStarted，蓝图播放 Montage
   -> AnimNotifyState Begin 开启攻击窗口
   -> 每帧读取 WeaponTop / WeaponEnd Socket 做连续 Sweep
   -> TSet 记录本轮已命中 Actor，防止同一次挥砍重复扣血
   -> ApplyDamage 到玩家 HealthComponent
   -> Notify End、Montage 中断、敌人死亡或目标死亡均关闭窗口

7. 死亡与结算
   -> HealthComponent 只广播一次死亡
   -> EnemyCharacter 停止移动、攻击窗口和相关 Timer
   -> SurroundManager 释放槽位和攻击令牌
   -> GameMode 从敌人注册表注销；Death/Destroy 只能让同一敌人减少一次存活数
   -> 倒计时结束时玩家存活则成功，玩家死亡则失败
   -> 广播 OnGameResult，蓝图显示结算和重新开始界面
```

#### 一次性提交与生命周期边界

| 事务 | 唯一提交点 | 防重手段 | 中断/结束清理 |
| --- | --- | --- | --- |
| 一次生命 | `HealthComponent::OnDeath` | `bDeathBroadcast`；Reset 后才重新开放 | EndPlay 解绑 Owner 伤害委托 |
| 一轮换弹 | `CommitReload()` | WeaponActionState、ReloadSequence、`bReloadAmmoCommitted` | Cancel、死亡和 EndPlay 清 Timer 并使旧回调失效 |
| 一发射击 | `TryConsumeAmmo()` 后的 Fire | ActionState、射速时间门禁、弹药检查 | StopFire、换弹、死亡和 EndPlay 清自动开火 Timer |
| 一轮近战 | `TryApplyAttackDamage()` | 整轮攻击共享命中集合和命中标记 | Finish、死亡和 EndPlay 关闭窗口并清 Timer |
| 一个敌人存活记录 | `UnregisterEnemy()` | 弱引用注册表，Remove 成功才允许改计数 | Death、Destroy、GameMode EndPlay 汇入同一入口 |
| 一局胜负 | `FinishGame()` | `bGameEnded`；Character 上报死亡，倒计时只检查胜利 | 解绑玩家死亡报告、清 Gameplay Timer、停止 AI、重置包围资源 |

关键点是把动画 Notify、Timer 和 Delegate 看成可能重复或乱序到达的请求，而不是可信的最终结果。最终状态只在 C++ 的唯一提交点改变；蓝图负责决定表现发生在时间轴的哪里。

HealthComponent 在更新血量后先广播 Damage、再广播 Death，因此 Character 和 EnemyCharacter 会在 Damage 回调里读取最终血量。若该次伤害已经致死，就跳过普通 Damaged 蓝图事件，只执行 Death 表现，避免受击 Montage 与死亡 Montage 在同一个 Slot 上互相替换。

#### 数据流、控制流与表现流

项目中三条流分开处理：

```text
控制流：Input / Timer / FSM / GameMode 调用函数，决定“现在做什么”
数据流：FHitResult、血量、弹药、槽位、攻击令牌和倒计时，决定“当前状态是什么”
表现流：Delegate / Blueprint Event / AnimNotify 驱动动画、UI、音效和特效
```

UI 的目标接线不是每帧查询玩家血量：`HealthComponent` 修改权威数据后广播 `OnHealthChanged`，Character 转发玩家语义事件，UMG 只更新显示。C++ 事件接口已经存在，但最终 Widget 仍需完成“创建时读取快照、随后订阅事件”的 PIE 回归，不能把接口存在说成 UI 已完整验收。

#### 使用的数据结构与设计模式

| 设计 | 项目中的落点 | 解决的问题 |
|---|---|---|
| 组件模式 | `WeaponComponent`、`HealthComponent`、`PickUpComponent` | 将武器、生命和交互能力从 Character 拆开并复用 |
| 状态模式 | Weapon 动作状态、敌人 FSM；Character 只计算移动/死亡语义 | 避免移动状态与换弹、射击等武器事务混在一个枚举里 |
| 观察者模式 | Dynamic Multicast Delegate | Ammo、Health、Wave 等数据源不知道具体订阅者，适合一对多通知 |
| 蓝图扩展点 | `BlueprintImplementableEvent` | C++ 保留规则与触发时机，蓝图子类实现动画、音效和 UI 表现；它不是 Delegate |
| 中央协调者 | `GameMode`、`SurroundManager` | 避免每个敌人独立争抢同一位置和攻击时机 |
| `TSet` | 单次攻击已命中对象集合、活动攻击者 | 平均常数时间查重，避免攻击窗口内重复伤害 |
| `TArray` | 出生点、包围槽位和候选位置 | 数据连续、遍历频繁，规模小且稳定 |
| `TMap` | 敌人与槽位的映射 | 快速查询某个敌人当前占用的槽位 |
| `TWeakObjectPtr` | 槽位占用者和攻击者引用 | 对象销毁后可检测失效，不让管理器错误持有生命周期 |
| Timer + 错峰 | AI 决策、波次和倒计时 | 不让所有规则都在每帧 Tick 中执行，降低同帧峰值 |

#### 当前工程证据与边界

已完成的工程验证包括：

- 固定关卡、固定敌人数和固定采样时长下测试 20 / 40 / 80 / 100 / 160 AI。
- 100 AI 场景 Frame Avg 为 `15.41 ms`，P95 为 `16.58 ms`；主要成本来自 CharacterMovement 和 Animation，而不是 Pathfinding。
- 通过 `stat streaming`、CSV、`ListStreamingTextures`、`MemReport -full`、Size Map 和 Reference Viewer 定位高占用纹理。
- 将 6 张环境/植被纹理的最大尺寸限制为 2048 后，Streaming 占用由 `212.27 MB` 降至 `152.27 MB`，下降 `60 MB`（约 `28.3%`），P95 帧时间没有明显回归。

必须诚实说明的边界：

- 这是单机 FPS，FPS 项目本身没有完成多人同步；RPC、属性复制和 Session 放在独立 Co-op 项目验证。
- VSM Non-Nanite 队列警告与 Texture Streaming Pool 是不同问题，当前只完成归因，没有把 VSM 警告包装成已修复。
- 还没有形成完整的多轮战斗内存回落曲线，因此只描述 Timer、弱引用、死亡清理和对象统计，不宣称解决了内存泄漏。
- 代码已经支持 `WeaponDataAsset + WeaponFamily`，已经放弃 Rifle/Shotgun 派生 WeaponComponent；但 Content 审计没有找到已创建并赋值的 WeaponData 资产，当前正式武器仍走组件默认值回退。创建资产、赋值并删除双配置源仍是待办。

#### 面试中的完整介绍示例

> 这是一个 UE5 C++ 单机 FPS 项目。早期我先完成可运行的射击原型，之后把重点从增加功能转到代码所有权和工程验证。当前结构里，GameMode 管波次、倒计时和胜负；Character 持有 WeaponComponent 与 HealthComponent；AIController 用 Timer 驱动显式 FSM 并发起 NavMesh 移动；EnemyCharacter 执行攻击和死亡；SurroundManager 集中管理站位与攻击并发；蓝图只保留动画、音效、UI 和后处理表现。
>
> 战斗方面，玩家使用相机射线完成 Hitscan，FHitResult 负责传递命中对象、骨骼和表面信息，通过 ApplyPointDamage 保留命中上下文，再进入共享 HealthComponent。敌人近战使用 ApplyDamage，因为当前只需要攻击者、伤害来源和伤害值。近战没有使用全程碰撞，而是在 Montage 的 AnimNotifyState 窗口内，用 WeaponTop 和 WeaponEnd 两个 Socket 连续 Sweep，并使用 TSet 保证同一次挥砍只命中一次；动画中断、敌人死亡和目标死亡都会关闭攻击窗口。
>
> 群体 AI 是项目里最主要的补充。最初所有敌人 MoveToActor 玩家中心，出现扎堆和同时攻击。我增加了双环槽位，按当前位置分配最近空槽，将位置投影到 NavMesh，再由 AIController MoveToLocation；同时用攻击令牌限制并发攻击，敌人死亡时通过弱引用和显式释放归还槽位。这个管理器只分配目标，真正路径仍由 UE 的 Recast/Detour 导航完成。
>
> 最后我建立了固定敌人数的性能基线。100 AI 场景的平均帧时间是 15.41 毫秒，P95 是 16.58 毫秒，数据表明主要成本是 CharacterMovement 和 Animation，而不是寻路。我还用 Streaming 统计、内存报告、Size Map 和 Reference Viewer 定位 6 张环境纹理，在不扩大纹理池的前提下把 Streaming 占用降低了 60 MB。这个过程让我能从“功能能跑”继续讲到职责划分、问题复现、工具定位、方案取舍和量化验证。

### 22.1 项目背景与所有权

项目不是从空目录开始，真实来源是：

```text
UE 第一人称模板
-> 蓝图教程建立可运行的玩法原型
-> 参考 C++ 教程理解 Gameplay Framework 和常用 API
-> 对照 Epic 官方文档确认生命周期、反射、Navigation 和 Profiling
-> 将伤害、状态、AI 决策、波次和群体站位等规则迁入 C++
-> 通过编译、日志、断点、自动压测和数据复测取得代码所有权
```

推荐直接回答：

> 项目早期使用 UE 第一人称模板和蓝图教程完成原型，我不把模板部分包装成原创。后续我按职责拆解 Character、WeaponComponent、HealthComponent、EnemyAIController、SurroundManager 和 GameMode，把会影响状态一致性和可测试性的规则迁到 C++，蓝图保留动画、音效、UI 和后处理表现。我不仅让功能跑通，还记录了对象类型错误、攻击窗口重复命中、GameMode 配置、群体扎堆和性能瓶颈等问题，并用编译、运行日志和 CSV/Insights 数据验证修改。

“独立完成”在这里表示独立整合、重构、调试和验证，不表示所有基础资产、模板代码和 API 都由自己从零发明。

### 22.2 三种时长的项目介绍

#### 30 秒版本

> 这是一个 UE5 C++ 单机 FPS 压力场景。我基于模板原型，将射击、弹药、共享 HealthComponent、敌人近战攻击窗口、AIController/NavMesh 状态机、双环包围和波次规则整理到 C++，蓝图负责动画与 UI 表现。项目的两个重点是群体近战 AI 的站位与攻击节奏，以及 20 到 160 个敌人的 CPU/动画/纹理资源分析。最终 100 AI 固定场景 Frame Avg 15.41 ms、P95 16.58 ms，并完成了 60 MB 的纹理流送预算治理。

#### 2 分钟版本

按以下顺序讲，不要逐条报功能：

```text
背景：模板和教程原型，目标是取得核心代码所有权
架构：C++ 管规则，蓝图管表现
难点一：NotifyState + 双 Socket 帧间 Sweep + 单次攻击去重
难点二：NavMesh 双环槽位 + 攻击名额，解决 MoveToActor 扎堆
难点三：固定敌人数压测，定位 Movement/Animation 而非 Pathfinding
结果：100 AI 的帧时间数据与纹理预算下降 60 MB
限制：没有完整内存回落曲线、Toon Shader 和 FPS 多人同步
下一步：网络项目验证服务端权威、RPC、RepNotify 和 Session
```

#### 15 分钟版本

依次展开第 13 节架构、第 14 节五条调用链、第 16 节三个 Bug、第 18 节一组数据，再讲本节 22.11 和 22.12。每个模块使用统一句式：

```text
需求 -> 初始方案 -> 暴露的问题 -> 定位方法
-> 当前实现 -> 为什么这样取舍 -> 数据/边界 -> 后续方案
```

### 22.3 技术栈与核心原理

| 技术 | 项目中的用途 | 必须理解的原理 |
| --- | --- | --- |
| UE5 / Gameplay Framework | Character、Controller、GameMode、Component 分工 | Pawn 是被控制实体，Controller 负责意图，GameMode 只在当前权威世界管理规则 |
| C++ / Unreal Reflection | 核心状态、组件、事件和蓝图接口 | UHT 处理 UCLASS/UPROPERTY/UFUNCTION，GC 只认识被反射系统跟踪的 UObject 引用 |
| Enhanced Input | 移动、瞄准、开火和换弹 | Character 统一绑定 Input Action 和管理 `IMC_Default`；武器只接收命令，不注册输入上下文 |
| LineTrace / Collision Query | Hitscan、命中部位和伤害入口 | QueryParams 过滤自身；FHitResult 提供 Actor、Component、Bone 和法线 |
| AnimMontage / AnimNotifyState | 近战有效攻击窗口 | 动画只决定判定时机，C++ 决定能否伤害；中断时必须回收窗口状态 |
| AIController / NavMesh | FSM 决策和路径请求 | Recast 构建可行走网格，Detour 在 Poly 图上寻路；局部避障不等于包围站位分配 |
| Timer / Delegate | AI 降频决策和事件驱动 UI/表现 | Timer 仍在 Game Thread 调度；Delegate 降低轮询，但必须治理绑定和对象生命周期 |
| TArray/TSet/TMap/TWeakObjectPtr | 槽位、去重、映射和弱引用 | TSet 适合本次攻击去重；TMap 建敌人到槽位映射；弱引用不延长 UObject 生命周期 |
| Unreal Insights / CSV / stat | CPU、GPU、对象和流送池证据 | 先固定条件建立 Baseline，再根据调用栈/计数定位，最后同条件复测 |

### 22.4 核心调用链速记

详细版本见第 14 节，面试时至少能脱离文档画出以下链路：

```text
射击：Input -> Character::StartWeaponFire -> WeaponComponent::StartFire
     -> Fire（单发提交）-> TryConsumeAmmo -> Camera LineTrace
     -> FHitResult -> ApplyPointDamage
     -> HealthComponent -> Delegate -> Blueprint 表现

换弹：Reload Input -> Character::RequestReload -> WeaponComponent::RequestReload
     -> Montage -> AnimNotify: CommitReload -> Completed/Interrupted
     -> FinishReload/CancelReload -> OnAmmoChanged/ActionStateChanged

敌人近战：FSM Attack -> OnAttackStarted -> Montage
        -> NotifyState Begin/Tick/End -> 双 Socket 帧间 Sweep
        -> TSet 去重 -> ApplyDamage -> 玩家 HealthComponent

群体 AI：GameMode Spawn -> AIController FSM -> RequestSurroundSlot
       -> ProjectPointToNavigation -> MoveToLocation
       -> RequestAttackToken -> Attack/Hold -> 死亡释放槽位和名额

闭环：Start Game -> GameMode Timer -> Wave Spawn/Countdown
    -> Delegate 更新 HUD -> Player Dead 或倒计时结束
    -> OnGameResult -> Result UI / Restart
```

### 22.5 技术选型与替代方案

| 当前方案 | 没直接采用的方案 | 取舍依据 |
| --- | --- | --- |
| UE5 + C++/Blueprint 混合 | Unity/C# 或纯 C++/纯蓝图 | 目标是 UE 游戏客户端能力，项目需要 Gameplay Framework、Character、Montage、NavMesh、UMG 和 Insights；C++ 管权威规则，蓝图保留资产迭代，不宣称 UE 对所有项目都更优 |
| ACharacter 作为玩家/敌人实体 | 从 APawn 自建移动 | 当前需要 Capsule、CharacterMovement、骨骼 Mesh 和 Controller/Possess 完整链路；自建 Pawn 会重复实现成熟能力 |
| Camera Hitscan | 实体 Projectile | 当前武器强调即时反馈且无飞行时间；实体弹会增加生命周期、碰撞和网络预测成本 |
| Weapon 作为 SkeletalMeshComponent 子类 | 独立 AWeapon Actor | 当前武器只有一个持有者，网格和射击状态一起附着最直接；出现丢弃物理、独立网络相关性或复杂子组件后应升级为 Actor + WeaponComponent |
| HealthComponent | 在玩家和敌人类里各写一套血量 | 共享组件统一 Clamp、死亡只触发一次和事件契约 |
| 显式 FSM | Behavior Tree / GAS | 当前状态数有限，显式转换更容易闭卷解释和压测；没有为了简历堆框架 |
| NotifyState 攻击窗口 | 武器碰撞全程开启 / 单个命中帧 | 全程碰撞会在起手收招误伤；单帧容易漏过高速刀刃，窗口内 Sweep 更稳定 |
| 双环槽位 + Token | 全员 MoveToActor(Player) | 同目标点导致扎堆、推挤和同时攻击；中央分配能保证唯一占位和受控攻击节奏 |
| Timer 决策 + 距离分级 | 所有 AI 每帧完整决策 | 降低重复判断成本，同时在近战窗口恢复高频保证判定精度 |
| WeaponDataAsset + WeaponFamily | Rifle/Shotgun 各自派生组件 | 当前差异主要是数值与射线数量，数据驱动更少重复；出现真正不同的射击生命周期后再抽策略 |
| GameMode 保存一局规则 | GameInstance/Subsystem 保存全部状态 | 波次、倒计时和胜负随当前 World/Match 生灭；跨关卡持久数据才属于 GameInstance，联机可见状态再进入 GameState |
| SurroundManager 作为每局 Actor | 全局单例或每敌人各自协调 | 槽位与 Token 是当前玩家/关卡的一局资源，显式 Actor 便于创建、重置和调试；开放世界或多玩家再做分区管理器/WorldSubsystem |
| 数据证明后再池化 | 预先实现通用对象池 | 当前 Hitscan 没有高频 Projectile；没有 Spawn/GC 热点证据时池化只会增加复位复杂度 |

### 22.6 如何发现、定位和修复问题

工程排错不是“节点接到能运行”为止。统一流程是：

```text
确认运行的是哪个 .uproject / 地图 / GameMode / 蓝图实例
-> 缩小到输入、对象引用、状态、资源或生命周期层
-> 用编译错误、Output Log、断点、Print/UE_LOG、stat/CSV 找证据
-> 沿调用链检查入口、Target 类型、返回值和清理出口
-> 最小修改
-> 正常路径 + 中断路径 + 死亡/重启路径回归
```

代表性工程问题：

| 现象 | 根因 | 定位与修复 |
| --- | --- | --- |
| 蓝图接口找不到或 Target 报错 | 手中只有父类引用或 self 不是函数所属类型 | 看节点 Target 类型，从真实实例 Cast 或直接使用事件参数，避免 GetPlayerPawn 硬取玩家 0 |
| Widget 调用了 Start 但未淡出/未移除 | 调用的变量不是屏幕上那一个实例，或动画未绑定画布 Render Opacity | 保存 CreateWidget 返回值；验证实例；用动画 Finished 后 RemoveFromParent，不把 CollectGarbage 当 UI 销毁 API |
| 敌人都冲向同一点 | 所有 AI 都 MoveToActor(Player) | Debug NavMesh/目标点并观察路径请求，改为中央槽位分配和 MoveToLocation |
| 攻击重复扣血 | 攻击窗口每帧 Sweep 未按整轮挥砍去重；多窗口重开会清空窗口级集合 | 攻击开始时清空 TSet，整轮所有窗口共享集合；首次命中后拒绝再次伤害，Finish/死亡统一清理 |
| 敌人不生成、倒计时不更新 | 关卡没有使用正确 GameMode，StartGame 未真正进入 C++ | 检查 World Settings Override、GetGameMode Cast、生成点 Tag、EnemyClass 和日志入口 |
| UE 无法启动且 DDC 无可写节点 | Zen/DDC 本地回环通信或缓存目录不可写 | 从日志的 DerivedDataCache 错误入手，检查 ::1 回环、防火墙/代理和缓存目录，而不是误判为普通外网故障 |
| 160 AI 不能稳定 60 FPS | Game Thread 超 16.67 ms，Movement/Animation 累积高 | CSV/Insights 拆分模块，确认 Pathfinding 不是主瓶颈，再做移动/动画频率分级 |
| Texture Streaming Pool 告警 | 高分辨率环境纹理占用预算 | stat streaming + Size Map 定位六张纹理，限制到 2048 后同条件复测，流送占用下降 60 MB |

代码检查必须覆盖：空指针/IsValid、数组边界、Timer 清理、Delegate 重复绑定、状态重复进入、死亡幂等、弱引用失效、Montage 中断和关卡重启。编译成功只能证明语法和链接，不等于行为正确。

### 22.7 技术亮点来自哪些自主补充

模板提供的是移动、基础武器和可运行入口；教程提供的是 API 用法。以下内容才是项目主要亮点，面试时必须讲成“问题驱动的改造”，不能只报功能名称。

| 自主补充 | 最初问题 | 自己的优化思路 | 落地与证据 |
| --- | --- | --- | --- |
| 共享 HealthComponent | 玩家、敌人分别扣血容易形成两套规则，死亡可能重复触发 | 把数值规则、Clamp、死亡幂等和事件契约收进可复用组件 | 玩家/敌人共用组件；沿 ApplyDamage 调用链验证；死亡事件只广播一次 |
| AnimNotifyState 近战窗口 | 全程武器碰撞会在起手/收招误伤，单命中帧可能漏过高速刀刃 | 让动画提供有效时间窗，C++ 在窗口内做双 Socket 帧间 Sweep，并按一次挥砍去重 | WeaponTop/WeaponEnd、TSet 去重、中断/死亡清理；攻击窗口文档和回归用例 |
| 双环包围与攻击名额 | MoveToActor(Player) 让所有敌人扎堆、互推、同时攻击 | 把“路径可达”“局部避障”“战术站位”拆开；中央分配唯一槽位，并限制同时攻击者 | 内圈 8、外圈 12；NavMesh 投影；弱引用占位；Token 释放；25/100 AI 行为验证 |
| AI 决策与更新分级 | AI 数量增大后每帧决策、Movement 和动画成本线性累积 | 先压测找热点，再按距离、可见性和攻击状态分级；关键攻击窗口恢复高频 | 20/40/80/160 Baseline；100 AI 最终 Frame Avg 15.41 ms、P95 16.58 ms |
| 波次与 UI 事件闭环 | 关卡蓝图散落生成、倒计时和 UI 查询，换地图后难复用 | GameMode 管规则和 Timer，Delegate 向蓝图/UMG 广播状态，UI 不每帧轮询 | StartGame、波次、剩余时间、存活数和结果事件；重复开始/重复结算保护 |
| 纹理流送预算治理 | 场景出现 Texture Streaming Pool 告警，直接加 PoolSize 只能掩盖问题 | 先用 stat streaming/Size Map 找真实高占用资源，再限制不必要的 4K 纹理并回归画质 | 六张环境纹理限制到 2048；212.27 MB 降至 152.27 MB，减少 60 MB/28.3% |
| 可重复性能测试入口 | 只看编辑器 FPS 无法比较改动，且容易把镜头差异当成优化 | 固定地图、分辨率、敌人数、预热与采样时间，记录模块计数和原始 CSV | AutoBenchmark、BenchmarkEnemies、CSV 文件、日志错误扫描和 100 AI 冒烟测试 |

判断“是不是自己的亮点”使用五问：

1. 我观察到了什么具体问题？
2. 我如何证明根因，而不是凭感觉猜？
3. 我提出过哪些方案，为什么选当前方案？
4. 我实际修改了哪个职责、状态或数据结构？
5. 修改后用什么数据或边界测试验证，仍有什么限制？

五问答不完整的内容只能算“使用过 API”，不能作为简历核心亮点。

### 22.8 优化方向与场景题

回答性能题时先问场景规模、帧率目标、平台和瓶颈线程，再提出方案。重复的 AI、场景查询、碰撞和物理题已经统一到第 24 章：

- 概念与项目映射：24.1～24.21。
- 必须掌握的复习深度：24.22。
- 完整条件变化题：24.23。
- 陌生题回答模板：24.24。

纹理池与 GPU Pass 的问题仍按第 16、18、22 节的工程证据回答，不混入物理查询题。60 FPS 也不能只报最高帧率；测试必须固定硬件、场景、敌人数、帧率上限和统计时长，并同时报告 Average 与 P95/P99。

当前诚实结果见第 18 节。项目已经证明了定位和治理方法，但不能声称完成“零泄漏”、完整 GPU 优化或 160 AI 稳定 60 FPS。

### 22.9 AI 辅助开发如何说明

AI 在项目中承担的是辅助角色：

- 根据错误日志生成排查假设和检查清单。
- 帮助检索可能相关的 UE API、官方文档关键词和源码入口。
- 提供候选架构、边界条件、场景题和自动测试脚本草案。
- 帮助整理实验数据、技术文档和面试追问。

代码所有权通过以下方式取得：

```text
逐行解释输入、输出、Target、对象生命周期和失败分支
-> 对照官方文档/头文件确认 API 契约
-> 本地 C++ 编译和蓝图编译
-> 真实运行并检查 Output Log
-> 固定条件采集 CSV/Insights 数据
-> 关闭文档后重画调用链、写伪代码和回答取舍
```

不能说“AI 帮我写完了所以就是我的”。推荐回答：

> 我把 AI 当作代码审查和假设生成工具，而不是事实来源。比如对象池、行为树、GAS 和重写 NavMesh 都曾是候选建议，但我根据项目规模和数据没有盲目加入。性能数字来自本机固定场景采集，最终修改必须通过编译、日志和回归验证；无法解释的代码不会放进简历亮点。

### 22.10 项目收获

1. 学会先划分规则所有权和表现边界，避免 C++ 与蓝图维护两套状态。
2. 理解 UE 的对象引用、反射、GC、Timer、Delegate 和 Actor/Component 生命周期会直接决定稳定性。
3. 动画不是单纯表现资源，AnimNotifyState 可以成为 Gameplay 时序契约，但权威规则仍应由 C++ 校验。
4. NavMesh 解决“如何绕路”，局部避障解决“怎么不碰撞”，槽位系统解决“应该去哪里”，三者不能混为一谈。
5. 性能优化必须从数据出发。160 AI 数据显示先处理 Movement/Animation，比重写 Pathfinding 更合理。
6. 能运行、能解释、能复测、能诚实说明限制，才算真正掌握一个教程起点的项目。

### 22.11 再给一段时间会怎么优化

按价值排序，不继续无边界加功能：

#### P0：补工程证据

1. 完成连续多轮战斗的对象数、Timer/Delegate 和内存回落曲线。
2. 用统一镜头和配置重跑 20/40/80/100/160 AI 优化前后 A/B。
3. 统计 AI 决策降频后的平均和最大响应延迟。
4. 修复 VSM 非 Nanite 阴影告警并做画质回归。

#### P1：补图形学最小闭环

实现一个可解释的 Toon Diffuse + Rim + Custom Depth Outline，记录材质指令和 GPU Pass 开销。重点是说明发生在渲染管线哪个阶段，不追求堆满 PBR、SSR、GI 和 Ray Tracing。

#### P2：架构演进

将射击方式抽成 Hitscan/Projectile 策略；将波次配置数据化；只有出现真实 Spawn/GC 热点后才为特效或尸体做专用对象池。Behavior Tree、EQS、GAS 均由新需求触发，不作为第一版装饰。

### 22.12 如何切换到多人网络

当前 FPS 是单机，不能把网络设计方案说成已实现。迁移时按权威边界重构：

```text
客户端输入
-> Server RPC 请求开火/交互
-> 服务端校验状态、弹药、射速、距离和命中
-> 服务端修改 Health/Ammo/GameState
-> Replicated/RepNotify 同步持久状态
-> Multicast 或本地预测播放非权威表现
```

模块变化：

| 单机模块 | 多人迁移 |
| --- | --- |
| Character/WeaponComponent | 客户端只提交意图；服务端校验 Fire/Reload/Pickup；防止客户端直接改弹药和伤害 |
| HealthComponent | 仅服务端扣血；CurrentHealth RepNotify；OnRep 驱动客户端表现 |
| GameMode | 继续只在服务端；波次、胜负和生成保持权威 |
| GameState | 新增可复制的剩余时间、波次、存活敌人数和比赛状态 |
| PlayerState | 保存需要跨 Pawn/重生存在的玩家数据 |
| EnemyAIController/SurroundManager | 只在服务端决策；选择多个玩家中的仇恨目标；客户端只接收必要移动/动画状态 |
| UI | 订阅本地 RepNotify/GameState，不直接查询服务器私有 GameMode |
| Pickup | 服务端验证距离和所有权，成功后复制持有者/可见性，处理两名玩家同时拾取竞争 |

验证顺序：

```text
双人 PIE / LAN
-> Role、Authority、Ownership
-> Variable Replication / RepNotify
-> Server RPC Validation / Multicast
-> Session Create/Find/Join/Destroy
-> 100/200 ms 延迟与丢包模拟
-> Relevancy、NetUpdateFrequency 和带宽分析
```

第一版不承诺完整预测回滚、延迟补偿、无缝重连和自建 Dedicated Server。它们应在服务端权威闭环稳定后作为独立专项，不返工当前单机项目。

### 22.13 面试证据清单

面试前逐项确认能现场打开或口述：

- 架构图和第 14 节五条调用链。
- HealthComponent、WeaponComponent、EnemyAIController、SurroundManager 的关键代码位置。
- 三个完整 Bug：对象 Target/Widget 实例、攻击窗口重复命中、MoveToActor 扎堆。
- 20/40/80/160 AI Baseline 与 100 AI 最终验收数据。
- 纹理流送池前后数据和六张高占用纹理的定位过程。
- 一次编译日志、一次运行日志和一次 CSV/Insights 原始记录。
- 已实现、待回归、仅方案三栏边界。
- 关闭文档后完成 30 秒、2 分钟和 15 分钟项目介绍。

最终判断标准：面试官随机指一个简历关键词时，能够指出代码位置、画出调用链、解释替代方案、讲一个失败路径，并说明用什么证据验证，而不是只复述 API 名称。

### 22.14 七个拷问方向如何落到本项目

面试官不会按文档顺序提问，而会从简历关键词切入，再连续修改条件。每个方向都准备“事实、取舍、证据、边界”四层。

| 方向 | 本项目的主答内容 | 典型深挖 | 必须拿出的证据 |
| --- | --- | --- | --- |
| 架构设计 | 从 Character/蓝图多权威，迁移到 Character、Weapon、Health、AIController、Enemy、SurroundManager、GameMode 分工 | 为什么不是 MVC/MVVM；为什么直接调用、Delegate 和 Blueprint Event 混用 | 第 13 节职责表、第 24 章统一场景题、关键头文件 |
| 技术选型 | Hitscan、显式 FSM、Timer 决策、NotifyState Sweep、中央槽位/Token、DataAsset | 为什么不用 Projectile、BT、EQS、GAS、对象池；何时会换 | 第 15 节和“需求触发升级”表 |
| 核心实现 | 射击、换弹、伤害死亡、敌人近战、群体 AI、比赛结算六条调用链 | 状态在哪改；失败分支怎么返回；中断从哪里清理 | 第 14 节、代码断点位置、Blueprint 接线 |
| 难点解决 | 重复攻击、MoveToActor 扎堆、近距离无法攻击、GameMode 未触发、旧构建缓存 | 如何复现、如何缩小范围、为什么确定是根因 | FPS-001～013、日志、断点和最小修复 |
| 性能优化 | 固定 20/40/80/100/160 AI；Movement/Animation 分级；纹理定点治理 | 是否 CPU/GPU bound；P95；测试条件；画质代价 | CSV/Insights、stat streaming、原始数据路径 |
| 边界反思 | UI 快照、Reload 回调、通用 Damageable、配置双来源、原地重开、VSM 告警 | 哪些已实现、待回归、仅方案；为什么暂时没继续抽象 | 第 10、11、18、22.11 节和任务清单 |
| 游戏理解 | 攻击名额控制可读性与公平性；散布数学正确性服务不同武器手感 | 敌人更强是否等于更好玩；均匀散布是否一定更好 | Token 上限、前摇/窗口/冷却、可配置散布参数 |

#### 架构题的完整回答骨架

```text
第一版能运行，但 Character、武器蓝图和 Timer 都可能修改同一状态
-> 我先定义唯一所有权和提交点，而不是先找设计模式名称
-> Character 只接输入意图；Weapon/Health 组件拥有可复用状态
-> AIController 决策，EnemyCharacter 执行，SurroundManager 管全局约束
-> GameMode 管一局规则，蓝图/UMG 只消费表现事件
-> 通过状态门禁、序列号、注册表和 EndPlay 清理覆盖重复与中断
-> 收益主要是正确性、可维护性和可测量性，不虚构“蓝图转 C++ 提升了 X FPS”
```

#### 游戏理解题不能只谈算法

- Attack Token 的目的不是单纯减少 CPU，而是限制同时攻击人数，让玩家能读懂威胁并获得躲避窗口。
- 双环槽位不是为了让敌人排得整齐，而是减少中心扎堆，同时保留内圈施压和外圈等待的节奏。
- 面积均匀散布是数学正确的基础分布，不代表所有武器都必须均匀。竞技步枪可能使用中心偏置或首发精准，霰弹枪可能使用固定图案、分层采样或边缘衰减。
- AI “更智能”不等于“更好玩”。瞬时反应、全知目标和全员同时攻击会降低公平性；感知延迟、前摇、冷却和并发上限都是玩法参数。

### 22.15 面试官修改条件时如何重新设计

先指出哪条原假设被改变，再说明受影响的权威状态和调用链。不要听到新名词就立刻加入框架。

| 改动条件 | 当前方案哪里失效 | 调整方向 | 仍需验证 |
| --- | --- | --- | --- |
| 单武器变成十种武器 | Character 只有一个引用；WeaponFamily 只覆盖数值和射线数量差异 | 增加 Equipment/Inventory；保留 DataAsset 配置；把 Hitscan/Projectile/Beam 抽成射击策略 | 切枪中断、每把武器弹药、动画层和存档 |
| 武器需要丢弃、交换或独立复制 | SkeletalMeshComponent 与当前 Owner 生命周期绑定过紧 | 改为 AWeapon Actor 持有 Mesh/Pickup/WeaponLogic 组件，Character 只持复制的 EquippedWeapon 引用 | Ownership、Dormancy、物理、拾取竞争和销毁 |
| Hitscan 增加火箭/抛物线 | Camera LineTrace 没有飞行时间和实体生命周期 | 新增 Projectile 策略，服务端生成；出现 Spawn/GC 热点后再做专用池 | 高速穿透、碰撞、爆炸范围、预测 |
| 单目标近战变成横扫多人 | 当前 `TargetCharacter` 和首次命中标记会拒绝其他目标 | 攻击事务改为目标集合；保留每 Actor 一次的 TSet，增加阵营/最大命中数 | 穿墙、友伤、多 Capsule/组件重复命中 |
| 一名玩家变成 2～4 人 Co-op | GameMode/AI 只解析 Player 0；UI 直接依赖本地单机状态 | 服务端权威；GameState/PlayerState；目标选择/仇恨；RPC/RepNotify | 延迟、作弊、拾取竞争、重生和带宽 |
| 100 敌人变成 1000 个单位 | 每单位 ACharacter、Movement、AnimInstance 成本不可线性扩展 | 远距离代理/Significance、MassEntity/MassAI、分区激活；近战实体化 | 视觉一致性、切换抖动、内存和 Crowd 行为 |
| 小竞技场变成开放世界 | 单个 SurroundManager、全图 NavMesh 和集中出生点不合适 | World Partition、Navigation Invoker、按玩家/空间分区的局部 Manager | 跨区迁移、卸载引用、存档和流送峰值 |
| 固定目标变成潜行/听觉 | 直接 Resolve Player 会让 AI 全知 | UAIPerception、刺激记忆、最后已知位置和丢失目标状态 | 视线遮挡、感知延迟、调试可视化 |
| 规则槽位遇到复杂多层地形 | 多个理论槽位投影后可能落在同一狭窄区域或投影失败 | 缓存投影、去重投影结果；必要时 EQS 对可达性、视线和间距评分 | 楼梯、门洞、动态 NavMesh、卡住率 |
| 动态门或障碍频繁变化 | 缓存路径/槽位可能失效 | 监听 Move 完成/失败，按阈值重新投影和请求路径；使用 Nav Link | Repath 频率、失败退避、路径风暴 |
| Reload 在弹匣插入前中断 | 事务尚未 Commit | Cancel，不增加弹药，序列号使旧 Timer 失效 | Montage Interrupted 是否真实接线 |
| Reload 在弹匣插入后中断 | Gameplay 已经 Commit | 当前语义保留已装入弹药，不回滚；若策划要求回滚需记录旧快照 | Notify 位置是否符合视觉语义 |
| Death 和外部 Destroy 连续到达 | 两个事件都可能尝试减少存活数 | 汇入 `UnregisterEnemy()`；只有 TSet Remove 成功才能改计数 | PIE 主动 Destroy 和寿命到期 |
| 玩家死亡与倒计时同帧发生 | 两条路径都可能调用 FinishGame | `bGameEnded` 只允许第一个结果提交；进一步可定义明确优先级 | 同帧顺序测试和策划规则 |
| 关卡内原地重新开始 | 当前 `bGameEnded` 不允许再次 Start；RestartLevel 依赖 World 重建 | 新增 ResetMatch 事务：清敌人/委托/Timer，重置玩家和状态，再开放 Start | 对象数、旧 UI 引用、输入和资源回落 |
| 需要 Replay/联网确定性散布 | `FMath::FRand()` 各端不可复现 | 服务端权威命中，或复制 Seed；使用 `FRandomStream` 和 ShotId | Seed 作弊、浮点差异、重放一致性 |
| UI 创建后初始显示 0 | 事件可能在 Widget 订阅前已经广播 | 创建时先读取 Health/Ammo/GameMode 快照，再订阅增量事件 | 重建 Widget、切 Pawn、解绑旧源 |
| 低端 GPU 出现 VSM Queue Overflow | Texture Pool 调整不能解决阴影标记队列 | ProfileGPU/VSM 统计；治理非 Nanite 阴影投射者、距离、LOD 和死亡敌人阴影 | 画质 A/B、GPU 帧时间和队列警告 |

### 22.16 五组递进式模拟追问

每组按“是什么 → 为什么 → 改条件 → 失败路径 → 证据”推进。练习时先口答，卡住后再看提示。

#### A. 架构优化

1. **你到底优化了什么架构？**
   - 答：不是把蓝图翻译成 C++，而是重新分配状态所有权、通信方向和生命周期收口。
2. **为什么不用 MVC/MVVM？**
   - 答：它们是 UI 主导模式；本项目按 Gameplay Framework 分工。UMG 是 View，但没有正式 ViewModel，不能冒充 MVVM。
3. **为什么 Character 还保存 Weapon 引用，不彻底解耦？**
   - 答：Character 是玩家 Pawn 的聚合根，需要知道当前装备以转发输入和协调死亡；它不拥有武器内部数据。
4. **Delegate、接口和直接调用怎么选？**
   - 答：明确单一接收者且需要返回值用直接调用；未知的一对多观察者用 Delegate；多个类型共享能力契约时才使用 Interface。
5. **怎么证明重构有效？**
   - 答：用单一写入口、重复/中断测试、依赖方向和可独立测量证明；没有蓝图/C++ 前后同条件数据就不声称 FPS 提升。

#### B. 开火、换弹与弹道

1. **开火完整链路是什么？**
   - 答：Input -> Character 意图 -> Weapon 状态/射速/弹药门禁 -> 单发事件 -> LineTrace -> Damage -> Health -> 表现。
2. **为什么不是 WeaponDataAsset 检查子弹？**
   - 答：DataAsset 是共享只读配置；CurrentAmmo 是每把武器实例的运行时状态，属于 WeaponComponent。
3. **为什么圆盘半径要开平方？**
   - 答：圆盘累计面积与 `r^2` 成正比，要让面积概率均匀需 `r = R * sqrt(u)`。
4. **均匀散布一定更好吗？**
   - 答：数学正确不等于玩法最优；应根据武器设计替换概率密度或固定 Pattern，并用命中点热力图验证。
5. **开火和换弹同时输入怎么办？**
   - 答：WeaponActionState 是唯一门禁；Reload 先 StopFire，Firing/Reloading 互斥，死亡转 Disabled。

#### C. 敌人近战与动画中断

1. **为什么用 NotifyState 而不是一次 Notify？**
   - 答：窗口表达有效时间，双 Socket 帧间 Sweep 覆盖高速剑刃路径。
2. **为什么一刀不会扣多次？**
   - 答：整轮攻击开始时清 TSet，所有窗口共享；首次合法目标命中后拒绝再次提交。
3. **同一个 Montage 有两个窗口呢？**
   - 答：窗口 Begin 不再清集合，所以仍属于同一轮攻击；Finish/死亡才清理。
4. **Montage 被受击动画打断呢？**
   - 答：Interrupted/BlendOut 应走统一 Finish/Cancel；C++ Timer 是兜底；死亡会立即关窗口和 Timer。
5. **为什么致死伤害不播放受击 Montage？**
   - 答：Health 已更新后 Damage 回调检查 IsDead，致死时跳过普通 Damaged 事件，只执行 Death 表现。

#### D. 群体 AI

1. **NavMesh 为什么没有解决扎堆？**
   - 答：NavMesh 解决可达路径，不负责每个 Agent 的战术目标分配。
2. **槽位、局部避障和路径搜索分别负责什么？**
   - 答：槽位决定去哪里，Detour 决定怎么绕路，局部避障决定短时如何不碰撞。
3. **Attack Token 泄漏怎么办？**
   - 答：TSet 幂等释放；攻击完成、超时、死亡、失去目标、UnPossess、GameMode 结算都释放。
4. **Timer 降频为什么不会永久变笨？**
   - 答：近战/持 Token 使用 0.1 秒，追击 0.25/0.5 秒，Idle 1 秒；需要另外量化最大响应延迟。
5. **为什么不直接上 Behavior Tree/EQS？**
   - 答：当前四状态和规则槽位已覆盖需求；当任务组合或地形评分复杂度上升时再迁移。

#### E. 性能与资源

1. **100 AI 的 15.41 ms 是什么指标？**
   - 答：固定场景 Frame Avg；同时报告 P95 16.58 ms、测试条件和一次采样局限，不能只报 FPS。
2. **怎么知道 Pathfinding 不是主瓶颈？**
   - 答：CSV/Insights 分解后主要成本在 CharacterMovement 和 Animation，优化优先级由数据决定。
3. **纹理从 212.27 MB 降到 152.27 MB 等于帧率提升吗？**
   - 答：不是。这是 Streaming 资源预算下降 60 MB，P95 基本不变，收益是预算余量而非 CPU/GPU 加速。
4. **为什么不直接增大 PoolSize？**
   - 答：先定位 Wanted/Resident 和单资源占用；增大预算可能掩盖不合理 4K 资产，并不能修复 VSM 队列。
5. **下一步最值得优化什么？**
   - 答：先补多轮内存回落、AI 响应延迟、Reload/UI 回归和 VSM 归因，再依据 Profile 决定 Movement/Animation 或资源治理。

### 22.17 高频场景题与危险回答

| 面试官问题 | 回答主线 | 危险回答 |
| --- | --- | --- |
| “蓝图改 C++ 快了多少？” | 没有同条件数据，不报数字；主要收益是权威状态和可维护性，性能优化另有 CSV 证据 | “C++ 肯定比蓝图快很多” |
| “你用了观察者模式，谁观察谁？” | Weapon/Health/GameMode 是事件源；蓝图/UMG 绑定 Dynamic Multicast；Enemy Blueprint Event 是另一种扩展点 | 把所有 C++ 到蓝图调用都说成 Delegate |
| “新增一把枪要改多少？” | 纯数值/射线数量差异新增 DataAsset；新射击行为才扩展策略，Character 不改 | 为每把枪复制一个 Character 分支 |
| “为什么这么多布尔值？” | 解释每个变量对应事务、窗口、命中和生命周期；能合并的状态归枚举，不能混掉不同语义 | “保险起见多写几个” |
| “敌人越聪明越好吗？” | 公平、可读性、反应窗口优先；Token、前摇和感知延迟都是设计约束 | “让 AI 更快响应、更准就行” |
| “能保证没有内存泄漏吗？” | 只能说明弱引用、解绑和 Timer 清理；要用多轮对象数/内存回落证明，当前不宣称零泄漏 | “用了 UE GC 所以不会泄漏” |
| “这部分是不是 AI 写的？” | 坦诚说明 AI 用于候选方案/排查/草案；用拒绝过度方案、代码解释、编译、日志和数据证明自己掌控 | 编造固定百分比或声称全部独立完成 |
| “你最不满意的代码？” | Weapon 配置双来源、Deprecated Fire、GameMode 混入 Benchmark、UI/Reload 未闭环，并说明优先级 | “目前没有明显问题” |

### 22.18 陌生条件下的现场推导模板

遇到没准备过的场景，不要猜 API。按下面六步推导：

```text
1. 复述新条件：规模、平台、单机/联网、帧率和玩法目标是什么
2. 找权威状态：谁拥有数据，谁有权提交结果
3. 画调用链：入口、直接调用、事件、引擎服务和表现出口
4. 查失败路径：重复、超时、中断、死亡、Destroy、切图和无效引用
5. 比较方案：当前方案何处失效，新抽象解决什么，又增加什么成本
6. 定验证：日志/断点/自动测试/CSV/Insights/画质 A-B，以及停止条件
```

如果没有实际做过，推荐表述：

> 这个条件在当前单机项目中没有真实落地，我不能把方案说成经验。基于现有权威边界，我会先把受影响的状态从 GameMode/本地组件迁到服务端可复制层，再做最小双人 PIE 验证；具体网络预测或带宽结果需要实验后才能下结论。

## 23. 游戏物理、碰撞与客户端面试主线

### 23.1 当前物理边界

项目基于 UE 5.5。UE5 的物理体系是 Chaos；本项目通过 `Engine` 模块提供的 `UWorld`、`UPrimitiveComponent`、`ACharacter` 和 `UCharacterMovementComponent` 等高层接口使用物理能力，没有直接依赖或修改 Chaos 求解器源码。

必须先区分三层：

| 层次 | 回答的问题 | 当前项目实例 |
| --- | --- | --- |
| 场景查询 Query | 轨迹经过哪里、碰到了谁 | 枪械 Line Trace、剑刃 Sphere Sweep、拾取 Overlap |
| 物理模拟 Simulation | 物体受到力后如何运动 | 射击冲量、敌人布娃娃死亡冲量 |
| 玩法规则 Gameplay | 命中后是否扣血、能否重复结算 | `ApplyPointDamage`、`ApplyDamage`、HealthComponent、攻击命中集合 |

`LineTrace` 命中不等于自动扣血，`ApplyDamage` 也不等于产生物理力。当前实现刻意把命中查询、伤害规则和表现/物理响应拆成独立步骤。

当前覆盖情况：

| 笔记中的领域 | 当前状态 | 诚实表述 |
| --- | --- | --- |
| 角色控制 | 已实现 | CharacterMovement 驱动胶囊体角色移动 |
| 碰撞检测 | 已实现 | Line Trace、Sphere Sweep、Overlap 和碰撞通道过滤 |
| 刚体动力学 | 部分实现 | 只对已开启物理模拟的命中组件施加冲量 |
| 布娃娃 | C++ 接口已接入，蓝图需回归 | 蓝图开启 Physics Asset 模拟，C++ 下一帧施加死亡冲量 |
| CCD | 使用相同思想 | 近战用帧间 Sweep 防穿透，不是 Chaos 刚体 CCD |
| 关节 | 间接涉及 | Physics Asset 中的刚体和约束构成布娃娃 |
| 布料、流体、载具、破坏 | 未实现 | 只能作为 Chaos 后续扩展方向，不能写成项目成果 |

### 23.2 角色移动不是自由刚体

玩家和敌人都继承 `ACharacter`，主要碰撞体是 Capsule：

```text
玩家输入
-> Character::Move
-> AddMovementInput 累积方向意图
-> CharacterMovementComponent 消费输入
-> 根据 MovementMode、速度、加速度和重力计算位移
-> 以 Capsule 做 Swept Movement
-> 处理地面、台阶、阻挡、滑动和穿透修正
```

代码入口：

- `fpstrueCharacter.cpp`：玩家 Capsule 半径 `55`、半高 `96`，`Move()` 调用 `AddMovementInput()`。
- `fpstrueEnemyCharacter.cpp`：敌人 Capsule 半径 `42`、半高 `96`，CharacterMovement 设置移动速度和旋转参数。
- 玩家或敌人死亡时调用 `StopMovementImmediately()` 和 `DisableMovement()`，敌人还会关闭 Capsule Collision。

CharacterMovement 属于受约束的运动控制器，不是让整个人物作为自由刚体接受力和力矩。它仍会查询物理场景，但角色位置由运动组件主导，因此控制稳定、适合楼梯和斜坡，也便于以后使用 UE 的网络预测与纠正。

AI 的路径链需要再分一层：

```text
EnemyAIController FSM 决定状态和目标
-> SurroundManager 决定战术槽位
-> NavMesh / PathFollowing 决定可达路径
-> CharacterMovement 执行移动
-> Capsule Collision 处理最终接触
```

NavMesh 解决“从哪里绕过去”，物理碰撞解决“这一小步能不能真正移动”。把两者混成一个系统，会导致对扎堆、不可达和穿墙问题定位错误。

### 23.3 枪械是场景查询，不是弹丸模拟

当前武器采用 Hitscan：

```text
WeaponComponent::Fire
-> 均匀圆盘散布生成 ShotDirection
-> LineTraceSingleByChannel(ECC_Visibility)
-> FHitResult
-> 敌人：读取 BoneName 并 ApplyPointDamage
-> 物理组件：IsSimulatingPhysics 后 AddImpulseAtLocation
-> 蓝图：根据 OnWeaponTraceFinished 生成曳光、弹孔和特效
```

查询参数的作用：

| 参数 | 当前行为 | 原因与代价 |
| --- | --- | --- |
| `AddIgnoredActor(Character)` | 忽略玩家 | 防止相机射线命中自己 |
| `AddIgnoredActor(GetOwner())` | 忽略武器 Owner | 防止枪械模型挡住射线 |
| `bTraceComplex = true` | 使用复杂碰撞查询 | 命中更精细，但通常比简单碰撞更贵 |
| `ECC_Visibility` | 按 Visibility 响应过滤 | 当前可用，但视线与子弹语义耦合 |
| `LineTraceSingle` | 返回第一个阻挡命中 | 适合不穿透的普通 Hitscan |

`FHitResult` 保存命中 Actor、Component、位置、法线和骨骼名。当前头部规则硬编码 `head/neck_01`，能工作但依赖资产命名；正式扩展应改为可配置骨骼集合、Physical Material 或命中区域接口。

Hitscan 的取舍：

| 方案 | 优点 | 缺点 | 当前选择依据 |
| --- | --- | --- | --- |
| Hitscan | 即时、稳定、实现和同步成本低 | 无飞行时间、下坠和途中拦截 | 当前自动步枪与近距离玩法足够 |
| Projectile | 可表现速度、重力、弹道和碰撞过程 | Actor/组件生命周期、CCD、网络同步成本更高 | 暂无明确玩法需求，旧 Projectile 路径已清理 |

如果面试官把条件改成火箭、榴弹或远距离狙击弹，当前方案就不再充分。此时应引入 ProjectileMovement 或真实刚体弹丸，并重新评估碰撞半径、CCD、对象复用、服务端权威和命中表现同步。

### 23.4 近战帧间 Sweep 是手动连续检测

单帧读取剑刃位置会漏掉高速运动：上一帧剑在玩家左侧，下一帧已经到了右侧，两次离散检测都可能没有重叠。当前方案把动画窗口内的剑刃轨迹当成连续体积：

```text
AnimNotifyState NotifyBegin
-> BeginAttackWindow
-> NotifyTick
-> 读取 weapontop / weaponend
-> 在上一帧和当前帧之间插值多个采样
-> 每个采样位置执行 Sphere Sweep
-> 当前剑根到剑尖再执行一次 Sphere Sweep
-> TSet 拒绝同一攻击事务重复伤害
-> NotifyEnd / Montage 中断 / Timer / Death 统一关闭窗口
```

选择 Sphere Sweep 而不是纯 Line Trace，是因为球体给动画误差、低帧率和武器厚度保留容差。代价是半径过大时会产生“空气命中”，所以 `WeaponTraceRadius` 必须按武器尺寸和目标帧率调试。

这与 Sweep-based CCD 的思想一致，但不是 Chaos 刚体 CCD：剑的位置来自骨骼动画 Socket，而不是刚体的线速度和角速度。项目在 Gameplay 层显式构造轨迹，并把检测频率限制在攻击窗口内。

项目没有自行实现 GJK、BVH 或宽相/窄相。代码只向 `UWorld` 提交查询，底层空间加速结构和形状求交由引擎负责。面试时可以解释宽相负责筛掉不可能相交的对象、窄相计算具体接触，但不能声称这些算法是本项目实现的。

### 23.5 Damage 与刚体响应是两条链

射击命中敌人的完整调用链：

```text
LineTrace 命中
-> ApplyPointDamage
-> EnemyCharacter::TakeDamage 保存 ShotDirection、ImpactPoint、BoneName
-> Super::TakeDamage
-> Owner OnTakeAnyDamage
-> HealthComponent Clamp Health 并广播 Damage / Death
-> EnemyCharacter::HandleDeath
```

刚体响应独立执行：

```text
HitComponent->IsSimulatingPhysics()
-> AddImpulseAtLocation(ShotDirection * Strength, ImpactPoint)
```

这样设计有三个结果：

1. 场景刚体可以被推动，但不一定有生命值。
2. 敌人可以扣血，但活着时不需要开启全身刚体模拟。
3. 伤害数值与质量、摩擦、阻尼等物理参数不会意外耦合。

如果未来需要击退，应把“Gameplay 位移”单独建模：角色可使用 LaunchCharacter、Root Motion Source 或受控位移；不要直接假设 Character 胶囊会像自由刚体一样响应 `AddImpulseAtLocation()`。

### 23.6 敌人布娃娃的 C++/蓝图边界

当前敌人死亡流程：

```text
HealthComponent 首次广播 OnDeath
-> EnemyCharacter::HandleDeath 幂等门禁
-> 关闭攻击窗口、Timer、AI 和 CharacterMovement
-> Capsule = NoCollision
-> OnEnemyDeathReported 通知 GameMode
-> BlueprintImplementableEvent OnEnemyDied
-> 蓝图设置 Mesh Collision Profile = Ragdoll
-> 蓝图 Set Simulate Physics(true)
-> C++ SetTimerForNextTick
-> Mesh 已模拟物理时按命中位置和骨骼 AddImpulseAtLocation
```

延迟一帧施加冲量不是随意等待：`OnEnemyDied` 蓝图事件同步执行，蓝图需要先让 Skeletal Mesh 从动画控制切换到 Physics Asset 模拟；下一帧 C++ 再检查 `IsSimulatingPhysics()`。如果蓝图没有开启物理，函数安全返回，冲量不会生效。

Physics Asset 为骨骼建立多个刚体和关节约束。动画阶段由 Animation Pose 主导；全身布娃娃阶段由 Chaos 求解这些刚体与约束。当前没有实现 Physical Animation 或局部布娃娃混合，也没有玩家布娃娃。

碰撞模式需要能说清：

| 模式 | 查询 | 物理模拟 | 典型用途 |
| --- | --- | --- | --- |
| `NoCollision` | 否 | 否 | 死亡后关闭 Capsule |
| `QueryOnly` | 是 | 否 | Trigger、Overlap、只参与 Trace 的对象 |
| `PhysicsOnly` | 否 | 是 | 只参与刚体求解的特殊组件 |
| `QueryAndPhysics` | 是 | 是 | Ragdoll、可推动刚体 |

### 23.7 当前优化和下一步证据

已经存在的成本控制：

- 玩家和敌人使用 Capsule，近战使用 Sphere，避免用高精度 Mesh 做所有动态碰撞。
- Character 和 EnemyCharacter 的 Actor Tick 已关闭；射击只在开火时查询，剑刃 Sweep 只在攻击窗口执行。
- AI 决策使用分级 Timer，路径目标变化达到阈值后才重新提交 MoveTo。
- 冲量前检查 `IsSimulatingPhysics()`，未启用物理的对象不会进入这条响应。
- 死亡关闭敌人 Capsule 和移动，避免尸体继续作为活 Pawn 参与移动碰撞。

尚缺的性能证据：

| 待验证项 | 工具与指标 | 目标 |
| --- | --- | --- |
| Trace/Sweep 成本 | Unreal Insights、Collision/SceneQuery 事件、固定攻击次数 | 区分查询数量和单次复杂查询成本 |
| 复杂碰撞取舍 | `bTraceComplex` A/B、固定靶场命中准确率 | 证明是否需要复杂查询 |
| 布娃娃预算 | 10/25/50 尸体 CPU Frame、Physics 时间、对象数 | 决定最大活动布娃娃数量 |
| 尸体生命周期 | 多轮波次内存和对象回落 | 验证 LifeSpan、引用和物理状态能回收 |
| 碰撞矩阵 | Weapon/Melee/Pawn/WorldStatic 固定用例 | 防止改 Profile 后漏命中或误命中 |

布娃娃数量上升后的候选策略：限制同时活动数量、远距离关闭模拟、静止后休眠、降低 Physics Asset 刚体/约束数量、停止后切换为静态姿势。没有同条件数据前，不能声称这些方案已经带来性能提升。

Substepping 和固定物理时间步主要改善真实刚体模拟的稳定性，会增加求解次数。当前核心枪械是瞬时查询、近战是手动 Sweep，不应在没有刚体稳定性问题时盲目开启并宣称可以修复所有漏判。

### 23.8 七个客户端拷问方向如何落到物理系统

| 拷问方向 | 本项目回答主线 |
| --- | --- |
| 架构设计 | Query 负责命中，Damage/Health 负责规则，Blueprint/Chaos 负责表现和刚体响应 |
| 技术选型 | 枪械选 Hitscan；近战选 NotifyState + Sphere Sweep；角色选 CharacterMovement 而非自由刚体 |
| 核心实现 | 能从输入讲到 LineTrace/FHitResult/ApplyPointDamage，也能从攻击窗口讲到帧间 Sweep/TSet |
| 难点与调试 | 漏命中看 Socket 轨迹和采样；枪打不着看碰撞通道、Profile、忽略对象和 FHitResult |
| 性能优化 | 减少查询频率、简化碰撞体、限制布娃娃数量，并用 Insights 做 A/B |
| 边界反思 | Visibility 语义耦合、硬编码骨骼、蓝图布娃娃未自动验收、缺少物理预算数据 |
| 游戏理解 | Trace 半径、击退、尸体停留和弹丸方案最终服务于命中手感、反馈清晰度和公平性 |

### 23.9 高频变体题

本节原有变体题已合并到 24.23，不再维护第二套答案。对应主题包括高速剑漏判、霰弹多射线、实体弹道、独立 Weapon Channel、护甲命中区、批量 Ragdoll、多人权威和可破坏场景。

### 23.10 官方源码与文档入口

- UE 5.5 `UCharacterMovementComponent`：`Engine/Source/Runtime/Engine/Classes/GameFramework/CharacterMovementComponent.h`。
- UE 5.5 `UMovementComponent`：`Engine/Source/Runtime/Engine/Classes/GameFramework/MovementComponent.h`。
- UE 5.5 `UPhysicsAsset`：`Engine/Source/Runtime/Engine/Classes/PhysicsEngine/PhysicsAsset.h`。
- Epic 文档：[Physics in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/physics-in-unreal-engine)。
- Epic 文档：[Traces with Raycasts](https://dev.epicgames.com/documentation/en-us/unreal-engine/traces-with-raycasts-in-unreal-engine)。
- Epic API：[UCharacterMovementComponent 5.5](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/GameFramework/UCharacterMovementComponent?application_version=5.5)。
- Epic API：[UPhysicsAsset 5.5](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/PhysicsEngine/UPhysicsAsset?application_version=5.5)。

## 24. 客户端统一专题：查询、碰撞、目标搜索、伤害与物理优化

### 24.1 面试官真正考察什么

只会说“调用 LineTrace”不够。客户端面试通常继续追问：

```text
输入数据是什么
-> 候选对象怎么产生
-> 如何过滤和选择
-> 查询复杂度与更新频率
-> 谁拥有权威状态
-> 重复、死亡、切图和多人条件如何收口
-> 用什么数据证明方案足够
```

回答一个系统时，至少覆盖五层：

| 层次 | 要说明的内容 |
| --- | --- |
| 技术概念 | 碰撞体、宽相/窄相、空间索引、Scene Query、感知和伤害事件 |
| 当前实现 | 真实类、调用链、数据结构和生命周期 |
| 技术选型 | 为什么当前选择比候选方案更适合现在的规模和玩法 |
| 失败边界 | 哪些新条件会使方案失效，升级后又引入什么成本 |
| 验证证据 | Debug Draw、日志、Gameplay Debugger、Insights、固定用例和复杂度 |

### 24.2 “查询敌人”其实是四类问题

项目中容易被统称为“查询”的操作，语义并不相同：

| 查询类型 | 问题 | 当前项目 |
| --- | --- | --- |
| 目标获取 | AI 应该关注谁 | GameMode 注入 Player 0，AIController 失效时兜底解析 |
| 空间/战术查询 | AI 应该站哪里 | Surround 槽位 + NavMesh 投影 |
| 路径查询 | 如何到达目标 | `MoveToLocation`、PathFollowing、NavMesh |
| 命中查询 | 这次攻击碰到了谁 | 枪械 Line Trace、近战 Sphere Sweep |

把它们拆开后，才能正确定位问题：看不到玩家是感知/目标获取问题，绕不过墙是路径问题，扎堆是目标位置分配问题，剑穿过玩家没有伤害是命中查询问题。

### 24.3 当前敌人目标获取：注入固定玩家

当前调用链：

```text
GameMode::StartGameMode
-> GetPlayerCharacter(this, 0)
-> 保存 PlayerCharacter
-> SpawnEnemyAtPoint
-> EnemyController::InitializeCombatContext(PlayerCharacter, SurroundManager)
-> AIController 保存 TargetCharacter
-> EnemyCharacter 保存同一 TargetCharacter 供距离、朝向和攻击使用
```

AIController 的 `ResolveTarget()` 仍会在目标无效时调用 `GetPlayerCharacter(this, 0)`，但这是兜底路径，不是每次决策都遍历世界。`IsTargetUsable()` 当前只检查引用存在且玩家未死亡。

选择直接注入的依据：

- 当前是单机、单玩家、竞技场波次玩法，敌人设计上可以一直知道玩家目标。
- 获取和读取目标引用是 `O(1)`，不会让每个敌人在每次决策里搜索所有 Pawn。
- GameMode 是敌人生成和本局玩家引用的装配点，生成后立即注入依赖，调用链容易调试。
- 固定目标让当前 FSM、Surround 槽位和性能压测保持确定性。

当前代价：

- Player 0 假设无法直接支持 2～4 人 Co-op。
- AI 是“全知”的，没有视野遮挡、听觉、刺激记忆和最后已知位置。
- 目标替换、重生或重新 Possess 时，需要显式刷新上下文。
- `ResolveTarget()` 和 `ResolveSurroundManager()` 的全局兜底查询会隐藏装配错误；规模增大后应改为明确的 TargetProvider/WorldSubsystem 或注册表。

### 24.4 常见敌人搜索方案及取舍

设世界中有 `N` 个 Actor，空间查询返回 `K` 个候选。复杂度是用于比较的模型，UE 内部结构和最坏情况仍由场景分布决定。

| 方案 | 典型复杂度 | 优点 | 缺点 | 适用条件 |
| --- | --- | --- | --- | --- |
| 已知引用/依赖注入 | 获取 `O(1)` | 最稳定、无重复搜索 | 目标固定、装配依赖明显 | 当前单玩家敌群 |
| `GetAllActorsOfClass` 后遍历 | `O(N)`，并产生结果数组 | 原型最快、逻辑直观 | 每帧调用浪费大，世界越大越差 | 编辑器工具、低频初始化，不用于高频决策 |
| 自维护 `TArray/TSet` 注册表 | 更新由 Spawn/Death 驱动；选择 `O(N)` | 不扫描无关 Actor，生命周期明确 | 需要注册、注销和失效清理 | 单位类型明确、中等规模 |
| Sphere Overlap / Object Query | 平均只返回邻近候选，近似 `O(log N + K)`；最坏仍可能退化 | 利用引擎空间加速、天然按范围裁剪 | 仍需阵营、死亡、视线和距离二次过滤 | 爆炸、近战范围、局部索敌 |
| AI Perception | 事件驱动获取视听/伤害刺激 | 支持遗忘、最后感知位置和感知调试 | 配置和状态更多，不等于目标评分 | 潜行、巡逻、听觉和非全知 AI |
| EQS | 生成候选后执行多个 Test 并评分 | 适合掩体、射击位、可达性和视线综合评分 | 查询成本和调试复杂度更高 | 复杂地形和战术选点 |
| Uniform Grid/Spatial Hash | 分布均匀时接近访问相关 Cell + `K` | 动态对象更新简单、局部搜索快 | CellSize 敏感，密集 Cell 会退化 | 大量同尺度单位、子弹和邻域查询 |
| QuadTree/Octree/BVH | 平衡时约 `O(log N + K)` | 支持不同空间尺度和范围裁剪 | 动态更新、平衡和内存成本更高 | 大世界、稀疏分布、复杂空间范围 |

不能把 `GetAllActorsOfClass` 一概说成“不能用”。低频初始化和编辑器工具中它足够简单；问题是把 `O(N)` 世界扫描复制到每个 AI 的 Tick 或高频 Timer。

也不能把 Overlap 结果直接当最终目标。标准流程应该是：

```text
Broad Candidate Query
-> 引用有效
-> 阵营/Team 过滤
-> 存活和可伤害过滤
-> 距离/角度/高度差
-> 可选 Line of Sight
-> 可选 Nav Reachability
-> 目标评分和迟滞
-> 缓存最终 Target
```

### 24.5 目标评分、稳定性和仇恨

多目标条件下，不应简单每帧选择最近者。可定义高分优先：

```text
Score = Wd * (1 - Distance / MaxDistance)
      + Wa * Dot(Forward, DirectionToTarget)
      + Wt * Threat
      + Wv * IsVisible
      + Wh * LowHealthPreference
      - Ws * SwitchPenalty
```

候选先通过硬条件过滤，再评分。硬条件负责“是否合法”，评分负责“合法目标中选哪个”。两者混在一个公式里会让不可见、死亡或不同阵营对象仅因距离很近而获胜。

常见稳定策略：

- 设置切换阈值：新目标分数必须明显高于当前目标才切换。
- 设置最短锁定时间，避免两个相近目标之间每帧抖动。
- 缓存上次可见位置和感知时间，支持搜索/丢失状态。
- 同分时用稳定 ID 或固定规则打破平局，便于重放和调试。
- 仇恨表只存必要上下文，Actor 销毁时用弱引用或事件注销。

当前项目没有多目标评分和仇恨系统。因为只有一个玩家，引入这些结构不会改善玩法，只会增加状态和测试面；Co-op 或诱饵/召唤物出现后才有需求依据。

### 24.6 AI Perception、EQS、NavMesh 和碰撞的边界

```text
AI Perception：我感知到了谁、从哪里、是什么刺激、多久后忘记
Target Selector：多个已知对象中选择谁
EQS：候选 Actor/位置中哪个评分最高
NavMesh：从当前位置到目标位置有没有可走路径
CharacterMovement/Collision：这一帧的实际移动是否被阻挡
```

`UAIPerceptionComponent` 是刺激监听者，可以接收视觉、听觉和伤害刺激并产生更新事件。它不自动决定最终攻击目标，也不自动寻路。

EQS 使用 Generator 产生候选，Context 提供参照，Tests 对距离、视线、可达性等条件过滤和评分。它适合回答“哪个射击位最好”，不是替代视觉/听觉感知的系统。

当前项目选择固定目标 + 规则槽位，是因为玩法不需要潜行和复杂掩体评分。升级顺序应由需求触发：

```text
需要视听和丢失目标
-> AI Perception + Target Memory

需要从多个玩家选择威胁
-> Candidate Registry + Target Scoring/Threat

需要复杂地形最佳位置
-> EQS 或自定义评分查询
```

### 24.7 碰撞体和常见窄相算法

| 碰撞体 | 常见判定思路 | 优点 | 项目对应 |
| --- | --- | --- | --- |
| Sphere | 圆心距离平方 `<= (r1+r2)^2` | 旋转无关、最便宜 | 近战 Sweep 半径、调试球 |
| AABB | 三个轴上的投影区间都重叠 | 计算简单、适合宽相包围盒 | 引擎空间加速中的常见包围形式，不是项目手写 |
| OBB | 在对象自身方向轴上做分离轴测试 | 比 AABB 更贴合旋转物体 | 武器/箱体的精细近似候选 |
| Capsule | 点/线段到线段距离与半径比较 | 适合人体，边缘连续 | 玩家和敌人的主碰撞体 |
| Convex Hull | SAT 或 GJK 支持映射 | 能表示不规则凸物体 | 复杂刚体的简单碰撞候选 |
| Triangle Mesh | 射线/形状与三角形求交，通常配合 BVH | 几何精确 | `bTraceComplex` 场景查询；不适合所有动态接触 |

高频算法概念：

- **SAT**：如果能找到一个投影轴使两个凸体区间不重叠，则两者分离；常用于 Box/Convex 判断。
- **GJK**：利用 Minkowski Difference 和 Support Mapping 判断两个凸体是否相交或求距离。
- **EPA**：常在 GJK 已确认相交后继续估计穿透深度和法线。
- **Ray-AABB Slab**：计算射线进入和离开各轴区间的时间范围，用于快速射线裁剪。
- **Segment Distance**：胶囊和剑刃 Sweep 可归结为线段/点之间最近距离与半径判断的组合。

这些是面试常考原理，不是当前项目自行实现的算法。项目提交 UE Scene Query，Chaos/Engine 负责底层加速和求交。

### 24.8 宽相、窄相和空间索引

一次碰撞检测通常分为：

```text
Broad Phase
-> 用 AABB、BVH、Grid、Sweep and Prune 等结构排除大多数不可能相交对象
-> 产生少量 Potential Pairs

Narrow Phase
-> 对具体形状执行 Sphere/Capsule/SAT/GJK/Triangle 等精确测试
-> 生成 Hit/Overlap/Contact 信息
```

如果 `N` 个对象全部两两检测，朴素复杂度为 `O(N^2)`。宽相的价值是利用空间局部性，让窄相只处理少量候选对。空间结构不是越复杂越好：动态对象频繁移动时，还要计算插入、更新、重建和缓存失效成本。

面试中讨论空间结构时要给出场景：

| 场景 | 优先考虑 |
| --- | --- |
| 规则竞技场、大量同尺度动态单位 | Uniform Grid / Spatial Hash |
| 大世界、对象尺寸差异大、分布稀疏 | Octree/BVH 或引擎现有空间索引 |
| 静态关卡射线查询 | 预构建层次结构、BVH 类结构 |
| 少量对象或低频查询 | 线性数组往往更简单，测到瓶颈再升级 |

### 24.9 UE Scene Query 选择表

| 维度 | 选项 | 如何选择 |
| --- | --- | --- |
| 几何 | Line / Box / Sphere / Capsule | 线适合 Hitscan；Sweep 适合有体积或防穿透；Overlap 只看当前位置重叠 |
| 结果数 | Single / Multi | 只要第一个阻挡物用 Single；爆炸、横扫和穿透候选用 Multi |
| 过滤语义 | ByChannel / ByObjectType / ByProfile | 从“攻击是什么”选 Channel；从“要找什么对象”选 Object；复用完整矩阵选 Profile |
| 响应 | Ignore / Overlap / Block | Ignore 不返回；Overlap 报告但不阻挡；Block 作为阻挡命中 |
| 几何精度 | Trace Complex false/true | 简单碰撞成本低；复杂碰撞精细但更贵，需资产和性能 A/B |
| 时间 | 同步 / 异步 | Gameplay 需要本帧权威命中时常用同步；大量非即时查询才评估异步和结果延迟 |

当前实际选择：

- 枪械：`LineTraceSingleByChannel(ECC_Visibility)`，复杂查询，忽略玩家和武器 Owner。
- 近战：`SweepMultiByObjectType(ECC_Pawn)`，Sphere 形状，再用 `HitActor == TargetCharacter` 精确过滤。
- 拾取：Sphere BeginOverlap，成功消费后关闭 Overlap 和 Collision 防止重入。
- 敌人死亡：关闭 Capsule Collision，避免尸体继续阻挡活 Pawn。

当前枪械通道的技术债是把 `Visibility` 同时用作视线和子弹语义。出现“玻璃挡视线但不挡子弹”或不同材质穿透规则后，应建立 Weapon Trace Channel 和碰撞响应矩阵。

### 24.10 离散检测、Sweep 与 CCD

离散检测只比较时刻 `t0` 和 `t1` 的位置。高速物体可能在两个时刻都不重叠，却在中间穿过目标，这就是 tunneling。

常见处理：

| 方法 | 思路 | 取舍 |
| --- | --- | --- |
| 增加采样/减小时间步 | 让离散位置更密 | 简单但成本线性增加，仍不能数学保证 |
| Sweep/Shape Cast | 检测起点到终点扫过的体积 | 适合武器、角色和运动查询 |
| Sweep-based CCD | 求运动体积的首次碰撞时间 | 准确但更贵，角运动处理复杂 |
| Speculative CCD | 扩张预测包围范围提前建立接触 | 可覆盖角运动，可能产生提前/幽灵接触 |

项目近战采用 NotifyState + 上一帧/当前帧 Socket 插值 + Sphere Sweep，是 Gameplay 层的手动连续检测。它解决动画武器漏判，但不等同于给 Chaos 刚体开启 CCD。

### 24.11 当前伤害方案的完整分层

伤害系统建议拆成七步：

```text
1. Detection：LineTrace / Sweep 找到命中
2. Eligibility：阵营、可伤害、目标合法性、无敌状态
3. Calculation：基础伤害、部位、护甲、距离衰减、暴击
4. Submission：ApplyPointDamage / ApplyDamage / ApplyRadialDamage
5. State Commit：HealthComponent Clamp 并提交 CurrentHealth
6. Semantic Events：Damaged / HealthChanged / Death
7. Presentation：Montage、音效、特效、UI、Ragdoll
```

当前枪械链：

```text
LineTrace
-> Cast<AfpstrueEnemyCharacter>
-> BoneName 判定 head/neck_01
-> ApplyPointDamage(HitResult, ShotDirection, Instigator, Causer)
-> EnemyCharacter::TakeDamage 保存命中方向、位置和骨骼
-> Super::TakeDamage / OnTakeAnyDamage
-> HealthComponent::ApplyDamageInternal
-> OnDamageReceived -> OnHealthChanged -> 首次 OnDeath
```

当前近战链：

```text
Sphere Sweep(ECC_Pawn)
-> HitActor 必须等于 TargetCharacter
-> 玩家必须存活
-> bHitTargetThisAttack + TSet 防重复
-> ApplyDamage(AttackDamage, EnemyController, Enemy)
-> Player OnTakeAnyDamage
-> HealthComponent
```

枪械选择 `ApplyPointDamage`，因为头部判定、死亡冲量和命中特效需要 `FHitResult`、方向、位置和骨骼。近战当前只需要攻击者、来源和数值，所以使用 Generic `ApplyDamage`；如果以后需要格挡方向、命中骨骼和硬直，应切换 PointDamage 或项目自己的 `FDamageContext`。

### 24.12 伤害入口方案比较

| 方案 | 优点 | 风险 | 当前结论 |
| --- | --- | --- | --- |
| 武器直接调用 `HealthComponent::ApplyDamage` | 最短、容易理解 | 武器依赖具体组件，绕开 Actor Damage、Instigator、Causer 和 DamageType | 不作为正式跨 Actor 入口 |
| UE `ApplyDamage/Point/Radial` | 引擎原生、上下文明确、Actor 可统一接收 | 默认接口较轻，复杂护甲/Buff 需自行扩展 | 当前主入口 |
| `FindComponentByClass<Health>` | 不要求具体 Actor 类型 | 仍耦合组件类型；每次动态查找不必要 | 可作迁移手段，不是能力契约 |
| `IDamageable` 接口 | 表达“该对象可受伤”，支持不同 Actor 实现 | 接口过大容易复制 Health 规则 | 适合做合法性/上下文契约，实际扣血仍统一进入 Damage Framework/Health |
| 自定义 `FDamageContext` | 可携带武器、元素、部位、标签、ShotId、距离等 | 需要定义序列化、版本和验证规则 | 多伤害类型出现后渐进引入 |
| `UDamageType` 子类 | 低成本区分 Bullet/Melee/Explosion 类型 | 主要描述类型，不适合存每次命中的可变运行数据 | 当前传 `nullptr`，需要差异响应时使用 |
| GAS Gameplay Effect | Attribute、Tag、Buff、预测和网络能力完整 | 学习、资产、调试和接入成本高 | 当前简单 FPS 不引入；复杂技能/状态/联网才评估 |

当前最明显的问题是枪械先 Cast 成 `AfpstrueEnemyCharacter`，导致同样拥有 HealthComponent 的 TargetDummy 不会受到枪械伤害。渐进改造应是：

```text
LineTrace HitActor
-> 通用 Damageable/Team 合法性检查
-> HitZone Resolver 计算部位倍率
-> ApplyPointDamage(HitActor, ...)
-> 各 Actor 通过统一 Damage/Health 链响应
```

不能简单删除 Cast 后对所有 Actor 扣血；还必须定义阵营、`CanBeDamaged`、不可破坏场景物、友伤和命中区域规则。

### 24.13 HealthComponent 为什么独立

HealthComponent 负责：

- 唯一保存 `CurrentHealth/MaxHealth`。
- 拒绝非正伤害和死亡后伤害。
- Clamp 到合法范围。
- 计算实际扣除值并广播事件。
- `bDeathBroadcast` 保证每次 Reset 生命周期只死亡一次。
- 在 `EndPlay` 解绑 Owner 的 `OnTakeAnyDamage`。

Character、Enemy 和 TargetDummy 都可以复用同一数值规则，而各自处理不同死亡语义。Component 是状态拥有者，Actor 是角色语义协调者，蓝图是表现扩展点。

它目前没有处理护甲、护盾、抗性、治疗、DamageType 和阵营。继续把这些全塞进 HealthComponent 也会膨胀；较合理的扩展是让 Damage Calculation/Policy 在提交前产生最终伤害，Health 只提交生命值变化。

### 24.14 数据结构、C++ 与生命周期考点

| 问题 | 当前选择 | 原因 |
| --- | --- | --- |
| 一次攻击去重 | `TSet<TWeakObjectPtr<AActor>>` | 平均 `O(1)` 查询/插入；弱引用不延长 Actor 生命周期 |
| 敌人注册 | `TSet<TWeakObjectPtr<Enemy>>` | Death/Destroy 汇入同一 Remove，只有成功移除才更新计数 |
| 槽位 | `TArray` + `TMap` | Array 保持稳定槽位顺序，Map 快速从敌人查索引 |
| 当前目标 | 受反射跟踪的 UObject 指针 | AI 生命周期内需要稳定访问；UnPossess/StopAI 主动清空 |
| 距离判断 | 可优先使用 DistSquared | 只比较阈值时避免开平方；需要真实距离或 UI 时再开方 |
| 目标高度 | 当前战斗距离使用 2D | 地面近战忽略 Z 更稳定；飞行/多层场景需要 3D 或路径距离 |

客户端代码不只考虑“指针会不会为空”，还要考虑：Actor Pending Kill/已销毁、关卡切换、UnPossess、重生、Timer 延迟回调和 Delegate 未解绑。缓存引用减少搜索成本，但必须有失效和刷新策略。

### 24.15 性能与线程边界

高频查询优化顺序：

```text
先减少调用次数
-> 再减少候选数量
-> 再简化单次查询
-> 最后才更换空间结构或异步化
```

项目已经采用：

- GameMode 生成时注入目标，不让每个敌人每帧搜索世界。
- AI 按状态使用 `0.1/0.25/0.5/1.0 s` 决策间隔。
- Move 目标变化超过阈值或 PathFollowing Idle 才重新提交路径。
- 近战 Sweep 只在攻击窗口运行。
- 攻击 Token 限制同时攻击者，也间接限制高频攻击查询数量。

进一步验证要记录：每帧 Trace/Sweep 次数、返回候选 `K`、AI Decision Count、Move Request Count、Game Thread/Physics/Navigation 时间和 P95，而不是只看平均 FPS。

同步 Scene Query 的结果本帧可用，适合权威命中，但大量调用会占用 Game Thread/物理查询时间。异步查询引入至少一个结果等待和对象失效问题，更适合非即时、可容忍延迟的环境检测；不要为了“异步更高级”改变枪械命中语义。

UE Gameplay UObject 通常由 Game Thread 管理。即使物理系统内部并行，也不能随意在工作线程修改 Actor、Component、TSet 或广播 Gameplay Delegate；并行任务应产出纯数据，再回到安全线程提交状态。

### 24.16 调试顺序与工具

敌人“看不见/不追/打不到”按层排查：

```text
1. Target：TargetCharacter 是否有效、是否死亡
2. FSM：当前 State、距离和 Timer 是否更新
3. Tactical：是否获得槽位/Attack Token
4. Nav：目标是否投影、Move 请求和 PathFollowing 状态
5. Collision：Capsule/Profile/Channel 是否阻挡
6. Hit Query：Trace 起终点、半径、Socket 和 FHitResult
7. Damage：Cast/Interface、ApplyDamage 返回、OnTakeAnyDamage、Health
8. Presentation：Gameplay 已成功后再查 Montage/VFX/UI
```

常用证据：

- `DrawDebugLine/Sphere/Capsule`：确认查询几何，不用肉眼猜动画。
- 输出 Actor、Component、ObjectType、BoneName、ImpactPoint 和响应结果。
- Gameplay Debugger：查看 AI、NavMesh 和 Perception；感知调试需真正启用 AIPerception。
- Unreal Insights/CSV：统计决策、Move Request、Trace/Sweep 和帧耗时。
- 固定测试关卡：无遮挡、墙体、边缘距离、高低差、多目标、死亡和销毁分别验证。

### 24.17 高频面试题速答

| 问题 | 回答核心 |
| --- | --- |
| 为什么不用 Tick 搜玩家？ | 当前玩家在生成时注入，读取 `O(1)`；Tick 全图搜索把 `O(N)` 复制到每个 AI |
| `GetAllActorsOfClass` 能不能用？ | 能用于低频初始化/工具；不适合大量 AI 高频查询 |
| Overlap 与 Trace 有什么区别？ | Overlap 查询当前位置体积重叠；Trace/Sweep 查询一段轨迹，Sweep 有体积 |
| Channel 和 Object Query 怎么选？ | Channel 从“这次查询是什么”出发；Object 从“要找哪类对象”出发 |
| Single 和 Multi 怎么选？ | 第一个阻挡命中用 Single；范围、多目标和穿透候选用 Multi |
| 为什么角色用 Capsule？ | 人体近似、旋转稳定、楼梯/滑动连续，计算比复杂网格简单 |
| 为什么用距离平方？ | 只比较阈值时避免 `sqrt`；需要真实距离值时再计算长度 |
| 宽相和窄相分别做什么？ | 宽相减少候选对，窄相对具体形状求精确接触 |
| GJK 是什么？ | 基于 Support Mapping/Minkowski Difference 的凸体相交或距离算法；项目未手写 |
| 高速剑为什么漏判？ | 离散采样发生 tunneling；用帧间 Sweep/自适应采样覆盖轨迹 |
| AI Perception 和 EQS 区别？ | Perception 产生感知事实；EQS 对候选位置/Actor 做测试评分 |
| NavMesh 能解决扎堆吗？ | 不能；NavMesh 管路径，槽位/目标分配管战术位置，局部避障管短时碰撞 |
| 为什么枪用 PointDamage？ | 需要 HitResult、方向、位置和 Bone；Generic Damage 不携带具体撞击上下文 |
| 为什么不直接改血量？ | 会绕开统一 Damage、来源上下文、Clamp、死亡幂等和表现事件 |
| Interface 和 Component 怎么分？ | Interface 表达能力契约；Component 拥有可复用状态和生命周期 |
| 为什么不用 GAS？ | 当前只有直接生命伤害；没有复杂 Attribute/Effect/Tag/预测需求，收益不覆盖成本 |
| 多人时谁做命中？ | 服务端权威提交 Damage；客户端可预测表现，需处理延迟、回溯和防作弊 |
| 怎么证明优化有效？ | 固定场景记录调用次数、候选 K、Game/Physics/Nav 时间和 P95，做同条件 A/B |
| Line Trace 一定比 Sphere Sweep 快吗？ | 线的窄相通常更简单，但总成本仍由次数、候选、Complex、Single/Multi 和结果处理决定 |
| Trace Complex 要不要关闭？ | 用当前资产做命中准确率和成本 A/B；需要骨骼/Hit Zone 精度时不能凭感觉关闭 |
| 异步查询为什么不直接用于枪械？ | 当前命中需要本帧权威结果；异步会增加延迟、过期对象和状态顺序问题 |
| 空间树一定比数组快吗？ | 少量或低频对象时维护索引可能更贵；规模、动态更新频率和查询模式共同决定 |

### 24.18 一分钟回答模板

> 当前是单玩家波次 FPS，所以敌人目标不做每帧空间搜索：GameMode 在生成敌人时把 Player 和 SurroundManager 注入 AIController，目标读取是常数成本；Controller 只在引用失效时兜底解析。AI Perception、EQS 和 NavMesh 分别负责感知、候选评分和路径，它们不是同一个问题，当前没有潜行和复杂地形评分需求，因此没有为了框架而引入。
>
> 战斗检测与伤害也分层：枪械用 Camera LineTrace 得到 FHitResult，再用 PointDamage 保留骨骼、方向和位置；近战只在 NotifyState 攻击窗口做双 Socket 帧间 Sphere Sweep，并用 TSet 保证一次攻击只结算一次。HealthComponent 监听 OnTakeAnyDamage，统一 Clamp、事件顺序和死亡幂等。当前技术债是枪械仍 Cast 具体 Enemy、头部骨骼硬编码且复用 Visibility 通道；下一步会引入通用 Damageable/HitZone 契约和专用 Weapon Channel，并用固定碰撞矩阵与 Insights 数据验证，而不是直接上 GAS 或复杂空间树。

### 24.19 官方资料入口

- Epic API：[UGameplayStatics 5.5](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/Kismet/UGameplayStatics?application_version=5.5)：`ApplyDamage`、`ApplyPointDamage`、`ApplyRadialDamage`。
- Epic API：[UAIPerceptionSystem 5.5](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/AIModule/Perception/UAIPerceptionSystem?application_version=5.5)。
- Epic 文档：[AI Perception](https://dev.epicgames.com/documentation/unreal-engine/ai-perception-in-unreal-engine)。
- Epic 文档：[Environment Query System](https://dev.epicgames.com/documentation/en-us/unreal-engine/environment-query-system-in-unreal-engine)。

### 24.20 场景查询与碰撞性能优化

#### 24.20.1 Scene Query 成本从哪里来

一次查询的成本不是只有“射线和三角形求交”：

```text
提交查询
-> 构造过滤参数和忽略列表
-> Broad Phase 访问空间加速结构
-> 产生 Candidate Shapes
-> Narrow Phase 做精确形状求交
-> 过滤 Ignore / Overlap / Block
-> 填充一个或多个 FHitResult
-> Gameplay 遍历结果、Cast、伤害和事件
-> 可选 Debug Draw / Log / VFX
```

可以用下面的近似模型分析，不把它当引擎内部精确公式：

```text
TotalCost ≈ QueryCount
          * (BroadPhaseCandidates
             + NarrowPhaseShapeCost
             + ReturnedHitCount
             + GameplayPostProcess)
          + Allocation/Debug/SyncCost
```

因此优化不能只盯着某一个 API。减少查询次数、让碰撞过滤更早排除无关对象、使用更简单的形状、减少返回结果和关闭调试输出都可能有效，最终优先级由 Profile 决定。

#### 24.20.2 当前项目查询清单与理论频率

| 查询 | 触发时机 | 当前参数 | 理论查询量 | 已知风险 |
| --- | --- | --- | --- | --- |
| Rifle Hitscan | 每次被射速门禁接受的射击 | `600 RPM`、1 条复杂 Line Trace | 持续开火约 `10 query/s` | `ECC_Visibility` 语义耦合；复杂查询成本待测 |
| Shotgun Hitscan 能力 | 每次射击 | 默认配置候选为 8 pellets | 若同为 600 RPM，代码能力可达 `80 query/s` | 当前没有正式 WeaponData 资产，不能说玩法已启用 |
| Melee Weapon Sweep | NotifyState 每次 NotifyTick | 4 个剑刃采样 + 当前剑根到剑尖 | 默认 `5 sweep/tick/attacker` | Multi 返回所有 Pawn；WorldStatic 不阻挡 |
| Melee Fallback Sweep | 单点攻击 Notify | 1 个较大 Sphere Multi Sweep | 每次 Notify 1 次 | 若与窗口路径并存会重复查询，必须保证资产只选一种入口 |
| Pickup Overlap | 有对象进入 Sphere 时 | BeginOverlap | 事件驱动，不是每帧主动查询 | 消费后已关闭 Collision，重入边界已治理 |
| Nav Projection/Path | 槽位和 Move 目标变化时 | 阈值刷新 | 由 AI 状态和移动目标决定 | 属于导航查询，仍与 Game Thread 帧预算竞争 |

近战默认并发攻击者上限为 2。若两个敌人在 60 FPS 下同时处于攻击窗口：

```text
5 sweeps/tick * 60 ticks/s * 2 attackers = 600 sweeps/s
```

这是上限估算，不代表实测耗时：NotifyTick 频率受动画更新、可见性和实际窗口长度影响。但它说明近战比单玩家步枪更值得增加 Query 计数器，并验证 `SampleCount + 1` 的收益是否覆盖成本。

#### 24.20.3 优化顺序：先数量，再范围，再精度

推荐按以下顺序推进，每一步都做同条件 A/B：

```text
0. 先记录 QueryCount、HitCount 和耗时
1. 删除重复入口和无效查询
2. 降低不需要逐帧运行的查询频率
3. 用 Channel/Object/Profile 在宽相后尽早过滤候选
4. Single 代替不必要的 Multi，允许 Early Out
5. Simple Collision 代替不必要的 Complex/Mesh 查询
6. 按速度/距离自适应 Sweep 采样
7. 对允许延迟的查询做错峰、时间切片或异步
8. 数据仍证明是瓶颈时，再改变空间结构或系统架构
```

不要先做微小对象复用，却保留每帧几十次本来不需要的 Multi Sweep。调用次数和候选规模通常比几个栈参数的构造更值得先检查。

#### 24.20.4 碰撞过滤就是性能设计

过滤越晚，越多无关对象会进入结果数组和 Gameplay 循环。

当前枪械：

```text
ByChannel(ECC_Visibility)
-> 引擎根据各组件对 Visibility 的响应筛选
-> 第一个 Blocking Hit 返回
```

当前近战：

```text
ByObjectType(ECC_Pawn)
-> 返回 Sweep 轨迹里的所有 Pawn
-> Gameplay 再检查 HitActor == TargetCharacter
```

近战的正确性和性能问题是同一个根：它只查询 Pawn，WorldStatic 不在对象集合中，所以墙不会成为阻挡结果；附近其他 Enemy Pawn 又可能进入 Multi 结果，最后才被目标指针过滤。

更合适的候选改造：

```text
新增 MeleeTrace Channel
Player Pawn：Block
WorldStatic/需要挡刀的门：Block
Enemy Pawn：Ignore
攻击者自身：Ignored Actor
-> SphereSweepSingleByChannel
-> 第一个阻挡对象是玩家才结算伤害
```

这样同时解决穿墙命中、无关敌人候选和 Multi 结果遍历。是否真的改成 Single 还要确认横扫多人、友伤和可破坏物规则；当前单目标近战适合，未来多目标横扫则需保留 Multi 并按距离/阻挡关系处理。

枪械应建立独立 WeaponTrace Channel，而不是继续复用 Visibility：

| 对象 | Visibility | WeaponTrace 示例 |
| --- | --- | --- |
| 实墙 | Block | Block |
| 透明玻璃 | 可能 Block | 由设计决定 Block/穿透 |
| 纯视觉粒子/Trigger | Ignore | Ignore |
| 玩家第一人称手臂 | 可能可见 | Ignore |
| 敌人 Hit Zone | Block | Block |

Channel 不是越多越好。每个新 Channel 都需要资产 Profile、蓝图默认值和回归矩阵；只在现有语义确实不能表达玩法时增加。

#### 24.20.5 Simple 与 Complex 查询取舍

`bTraceComplex = true` 让枪械查询复杂碰撞几何。它可能提高模型表面命中精度，但不应直接等同于“头部判定一定更正确”。Skeletal Mesh 的 Physics Asset、骨骼身体和项目 Collision 设置也可以提供部位信息，必须用当前资产验证。

| 方案 | 成本与特点 | 适用 |
| --- | --- | --- |
| Simple Collision | 少量 Box/Sphere/Capsule/Convex，通常更便宜稳定 | 移动、阻挡、近战容差、常规 Gameplay |
| Complex Collision | 更接近渲染三角面，命中细致 | 精确弹孔、静态场景表面、确有需求的射击判定 |
| Physics Asset Bodies | 按骨骼设置基础形状，可返回身体/骨骼语义 | 角色命中区域、布娃娃 |
| 独立 Hit Zone Components | Gameplay 语义最明确，可独立配置倍率 | 竞技射击、护甲和弱点，但维护组件更多 |

建议做四组固定靶场测试：正面头部、身体边缘、遮挡边缘和远距离小目标。分别记录 Simple/Complex 的命中一致性、BoneName/HitZone 正确率和查询耗时；没有准确率证据就不能只因“Complex 更精确”长期保留。

#### 24.20.6 Single、Multi 和结果后处理

选择原则：

- `Single`：只关心第一个阻挡对象，可以尽早结束；当前普通枪械最合适。
- `Multi`：需要范围内多个对象、穿透候选、爆炸或横扫多人；必须控制返回数量和去重。
- `OverlapMulti`：只需要当前位置范围候选，不需要轨迹上的首次接触点。

Multi 查询后的常见成本和错误：

- `TArray<FHitResult>` 可能分配和写入多个结果。
- 同一 Actor 的多个 Component 可能产生多个 Hit，需要按 Actor/HitZone 明确去重语义。
- 如果要处理墙体阻挡，必须按 Blocking Hit 和距离顺序截断，不能无条件伤害所有结果。
- Gameplay Cast、接口调用和事件广播也会随结果数增长。

当前近战的 `TSet<TWeakObjectPtr<AActor>>` 负责“一次攻击每个 Actor 最多一次”，但当前还有 `bHitTargetThisAttack`，因此实际上是单目标首次命中后整轮停止伤害。未来横扫多人时应移除全局首次目标门禁，保留按 Actor 的 TSet。

#### 24.20.7 自适应近战采样

当前固定 `WeaponTraceSampleCount = 4`，每个 NotifyTick 总查询数为：

```text
QueriesPerTick = Clamp(SampleCount, 2, 8) + 1
```

固定采样容易出现两个问题：剑移动很慢时多查，剑移动很快时仍可能采样不足。可以按本帧最大端点位移自适应：

```text
MaxTravel = max(
    Distance(PreviousBase, CurrentBase),
    Distance(PreviousTip, CurrentTip)
)

StepLength = WeaponTraceRadius * CoverageFactor
SampleCount = Clamp(ceil(MaxTravel / StepLength), MinSamples, MaxSamples)
```

这种方案把采样密度与实际运动距离和碰撞半径联系起来。仍需设置最大值避免瞬移或动画跳帧导致单帧查询爆炸；Teleport/极端 DeltaTime 可直接取消当前窗口或走一次保守的大 Sweep。

另一个方案是扫掠整个剑刃的 Box/Capsule，而不是多个球形线段。查询次数可能下降，但旋转武器的姿态、宽度和角运动近似更复杂。应比较准确率和总成本，不以查询次数单一指标决定。

#### 24.20.8 查询频率、Significance 与错峰

即时战斗查询和环境认知查询的时效要求不同：

| 查询 | 延迟容忍度 | 调度策略 |
| --- | --- | --- |
| 玩家本次射击命中 | 几乎不能延迟 | 同步、本帧提交结果 |
| 攻击窗口剑刃命中 | 需跟动画时序一致 | NotifyTick，同步；用并发 Token 控制总量 |
| AI 目标有效性/距离 | 可容忍 0.1～1 秒 | 状态分级 Timer、错峰 |
| AI 视线更新 | 通常可容忍短延迟 | Perception 更新或分帧查询，近处高频远处低频 |
| 远处环境/掩体预查询 | 可容忍更多延迟 | 时间切片、缓存，必要时评估异步 |

假设给 100 个敌人每个都做 10 Hz LOS Line Trace，就是 `1000 traces/s`。这不一定不可接受，但必须先问：远处 Idle 敌人是否真的需要 100 ms 级视线响应？可以按距离、战斗状态、是否在屏幕附近和最近刺激时间分层，并给 Timer 加随机初始偏移，避免同一帧集中提交。

缓存也有边界：可以缓存 Target 引用、上次可见时间、路径目标和静态投影结果；不能长期缓存移动目标的碰撞命中而不设置位置/时间失效条件。

#### 24.20.9 同步、异步与批处理

同步查询优点是结果立即可用，调用链简单，适合伤害权威；缺点是当前线程要等查询结束。异步查询可以把可容忍延迟的工作移出立即路径，但增加：

- Query Handle 与下一帧结果读取。
- Actor 在结果返回前死亡或销毁的校验。
- 结果顺序、过期和取消策略。
- Gameplay 状态仍需回到安全线程提交。

当前不应把玩家 Hitscan 改成异步，因为会改变开火、扣弹、命中反馈和伤害提交的时序。更合理的异步候选是远距离 AI 环境采样或非关键预查询，而且必须先证明同步查询是瓶颈。

批处理的思路是集中同类查询、共享调度和预算，而不是简单把多个 API 调用包在一个函数里。若将来出现大量弹片/霰弹，可评估统一 Shot Query Batch、限制每帧最大查询数量和合并表现事件；具体是否减少底层成本仍需测量。

#### 24.20.10 内存、参数和 Debug 成本

`FCollisionQueryParams`、`FCollisionObjectQueryParams` 和 `FCollisionShape` 是小型查询描述，当前在栈上构造。没有 Profile 证据前，不应为了省几个构造就把它们改成复杂的共享可变状态。

更值得注意：

- Multi 查询结果数组的元素数量和分配。
- 每个 Hit 后的 FString 骨骼名转换、Cast、Delegate 和 VFX。
- Persistent Debug Draw 会产生线段/球体并占用渲染和内存。
- 高频 `UE_LOG` 和屏幕消息会扭曲测试帧时间。
- `bReturnPhysicalMaterial` 等额外结果只在真正需要时开启。

当前武器 Debug Trace 已通过编译宏关闭，敌人 Debug Trace 默认关闭。正式 Profile 必须维持关闭状态；调试截图和性能数据不能混用同一轮采样。

#### 24.20.11 项目应增加的统计点

已有 `STAT_fpstrueAIDecisionCount` 和 `STAT_fpstrueAIMoveRequestCount`。场景查询还应增加：

```text
WeaponLineTraceCount
WeaponTraceHitCount
MeleeSweepCount
MeleeSweepHitCount
MeleeReturnedHitCount
ActiveAttackWindowCount
RagdollActiveCount
```

并给枪械查询添加可识别的 `SCENE_QUERY_STAT(WeaponTrace)`；敌人已有 `EnemyMeleeTrace/EnemyWeaponTrace` 标签。计数器回答“调用了多少次”，Insights Scope/Scene Query 事件回答“花了多久”，两者需要一起看。

建议派生指标：

```text
HitRate = HitCount / QueryCount
AverageReturnedHits = ReturnedHitCount / MultiQueryCount
QueriesPerAcceptedShot = WeaponTraceCount / AcceptedShotCount
SweepsPerAttack = MeleeSweepCount / AttackStartedCount
MoveRequestsPerDecision = MoveRequestCount / DecisionCount
```

如果查询量下降但漏判率上升，不能算优化成功。

#### 24.20.12 固定测试矩阵

| 用例 | 验证正确性 | 记录性能 |
| --- | --- | --- |
| 枪打实墙后的敌人 | 墙先 Block，敌人不受伤 | 每发 1 次 Single Trace |
| 玻璃/铁网/Trigger | 按 Weapon Channel 设计响应 | 候选数和命中结果稳定 |
| 头部/身体边缘 | HitZone/Bone 判定正确 | Simple/Complex A/B |
| 剑高速横跨玩家 | 不漏判、不重复扣血 | Sweeps/Attack 和窗口耗时 |
| 剑隔墙划过玩家 | 墙体阻挡 | 无额外 LOS 补查或明确记录补查成本 |
| 两个敌人同时攻击 | 最多两个窗口活跃 | 查询峰值、P95 和错峰情况 |
| 25/50 敌人等待 | 非攻击敌人不做武器 Sweep | Decision/Move/Trace 数量 |
| 10/25/50 布娃娃 | 尸体不阻挡活 Pawn | Physics Frame、对象数和内存回落 |

每轮固定关卡、机位、敌人数、动画、帧率上限和统计时长，报告 Average 与 P95。CPU 查询优化不能只用 FPS 表述，因为 GPU Bound 场景下 FPS 可能完全不变。

### 24.21 Chaos 物理模拟性能：六类方案的原理与取舍

#### 24.21.1 先区分 Scene Query 与 Physics Simulation

截图中的 LOD、休眠、并行和时间步主要优化 **持续物理模拟**；前一章的 Trace 次数、Single/Multi 和查询过滤主要优化 **场景查询**。二者共享碰撞几何和过滤配置，但成本来源不同：

```text
Scene Query
-> 外部主动提交 LineTrace / Sweep / Overlap
-> 返回 FHitResult
-> 成本随查询次数、候选形状和结果数增长

Physics Simulation
-> Chaos 每个物理步更新 Active Rigid Bodies
-> 碰撞对、约束、接触、积分和回写
-> 成本随 Active Bodies、Contact Pairs、Constraints、Solver Iterations 和 Steps 增长
```

当前项目中：

- 玩家和活敌人主要由 CharacterMovement 驱动，不是自由 Chaos 刚体。
- 枪械 Line Trace 和剑刃 Sphere Sweep 是 Scene Query。
- 命中的可模拟场景物体和敌人 Ragdoll 才进入刚体模拟。
- 因此物理模拟优化的首要对象是尸体 Ragdoll 和未来可推动/可破坏物，不是把所有 Trace 改成休眠。

一个用于讨论的近似模型：

```text
SimulationCost ≈ PhysicsStepCount
               * (ActiveBodyIntegration
                  + BroadPhasePairUpdate
                  + NarrowPhaseContacts
                  + ConstraintSolveIterations)
               + Game/Physics Thread Sync
               + CollisionEventDispatch
```

六类优化分别减少其中不同项，不能互相替代。

#### 24.21.2 Physics LOD：按重要性减少模拟质量

物理 LOD 不是单纯的模型几何 LOD，而是按距离、可见性和 Gameplay 重要性降低：

- 模拟对象数量。
- 刚体与关节数量。
- 碰撞精度和碰撞响应范围。
- 更新频率或是否继续模拟。
- 是否产生 Hit/Overlap/Wake 等事件。

适合当前项目的 Ragdoll 分级：

| Tier | 条件示例 | 模拟策略 |
| --- | --- | --- |
| Tier 0 近景关键尸体 | 刚死亡、镜头附近、仍可能被射击 | 全身 Ragdoll、QueryAndPhysics、接收冲量 |
| Tier 1 中景尸体 | 已稳定、不是视线焦点 | 允许自动 Sleep，减少不必要事件；仍可被强碰撞唤醒 |
| Tier 2 远景/超预算尸体 | 距离远或活动 Ragdoll 已超过预算 | 冻结最终姿势，关闭物理模拟；按玩法保留 QueryOnly 或 NoCollision |
| Tier 3 无意义尸体 | 超过寿命、切波或不可见区域 | Destroy/LifeSpan，释放 Actor、Mesh、Physics Bodies 和引用 |

进入 Tier 2 时如果直接 `SetSimulatePhysics(false)`，Skeletal Mesh 可能回到动画姿势或产生跳变。正式方案要先验证 Pose Snapshot、骨骼变换保留或专用静态尸体表现，再关闭模拟。

LOD 的关键不是距离一个条件，还应考虑：

- 是否在镜头内或屏幕占比。
- 是否刚受到冲量、仍在高速运动。
- 是否会影响玩家路径或掩体。
- 是否可被再次射击、爆炸或网络玩家观察。
- 当前活动 Ragdoll 的全局预算。

项目已有的 `UpdatePerformanceTier()` 只调整敌人 CharacterMovement Tick Interval，动画也按可见性降级；这属于移动/动画 Significance，不等于已经完成刚体 Ragdoll LOD。

#### 24.21.3 休眠：从 Active Body 集合中退出

Chaos 刚体常见状态可以这样理解：

| 状态 | 是否求解运动 | 能否被唤醒 | 语义 |
| --- | --- | --- | --- |
| Dynamic Awake | 是 | 已活跃 | 每个物理步积分并参与接触/约束 |
| Sleeping | 大部分运动求解暂停 | 可以，受力或碰撞可唤醒 | 暂时静止但仍保留刚体语义 |
| Kinematic | 不由力积分 | 由 Gameplay/动画移动 | 移动平台、动画驱动物体 |
| Static | 否 | 通常不作为动态体唤醒 | 固定场景几何 |
| Simulation Disabled | 不再参与动态模拟 | 需显式重新启用 | 冻结/剔除，不等同于 Sleep |

UE5.5 可用接口包括：

```cpp
PrimitiveComponent->PutRigidBodyToSleep(BoneName);
PrimitiveComponent->WakeRigidBody(BoneName);
SkeletalMeshComponent->PutAllRigidBodiesToSleep();
SkeletalMeshComponent->WakeAllRigidBodies();
SkeletalMeshComponent->IsAnyRigidBodyAwake();
```

`FBodyInstance` 还暴露 `SleepFamily` 和 `CustomSleepThresholdMultiplier`；阈值越积极，物体越早休眠，但可能让缓慢滚动或摇摆提前停止。

Sleep 与关闭模拟的区别：

- Sleep 保留刚体和碰撞状态，新的有效碰撞或力可能唤醒。
- 关闭模拟更省，但不会自然响应新力；需要显式恢复，并处理姿势/Transform 同步。
- Physics Field 的 Sleep/Disable 适合 Geometry Collection 或区域化物理控制，但当前项目没有破坏系统，不需要为了尸体引入 Field。

当前项目尚未实现尸体休眠治理。候选实现应以“连续一段时间 `IsAnyRigidBodyAwake() == false`”或速度低于阈值为依据，而不是死亡后固定延迟立刻冻结；玩家刚把尸体击飞时冻结会明显破坏反馈。

#### 24.21.4 简化碰撞体：降低窄相与约束成本

碰撞几何越复杂，单对窄相测试、接触点生成和内存通常越贵：

```text
Sphere
-> Capsule / Box
-> 少量 Convex
-> 多 Convex / 大量 Physics Asset Bodies
-> Triangle Mesh / 高复杂度组合
```

这不是所有场景的绝对排序，但适合作为 Gameplay 碰撞设计起点。

当前项目的正确使用：

- 玩家和敌人用 Capsule 负责移动阻挡，不让 Skeletal Mesh 三角面承担角色移动。
- 近战用 Sphere Sweep 给武器轨迹提供厚度和低帧率容差。
- Ragdoll 由 Physics Asset 的基础形状近似骨骼身体。
- 枪械当前使用 Complex Line Trace，这属于需要 A/B 验证的精确查询，不应扩散到所有动态碰撞。

Physics Asset 优化重点：

- 删除对轮廓和 Gameplay 没有贡献的小骨骼 Body。
- 用 Capsule/Box/Sphere 代替复杂 Convex。
- 减少不必要的 Constraint 和过紧的角度限制。
- 检查 Body 互相碰撞矩阵，避免相邻骨骼产生无意义自碰撞。
- 只对关键 Body 产生碰撞/Hit Event。
- 用近景落地稳定性、穿插率和 Physics 时间验证，不能只看 Body 数量。

减少 Body 会降低姿态自然度和命中区域精度。若头部/身体伤害依赖 Physics Asset Body，必须同时验证 HitResult Bone/Body 语义，避免性能优化改变伤害规则。

#### 24.21.5 分层检测：碰撞矩阵同时优化正确性和性能

碰撞过滤决定哪些对象有机会形成查询候选或模拟接触对：

```text
Collision Enabled
-> Object Type
-> Trace Channel / Collision Profile
-> Ignore / Overlap / Block
-> 可选 Actor Ignore / Team / Gameplay Filter
```

针对模拟，最重要的是减少无意义 **Body Pair**：

- Ragdoll 忽略活 Pawn，可以减少尸体与每个敌人胶囊之间的推挤；当前默认 `Ragdoll` Profile 已对 Pawn 使用 Ignore。
- 只需要被 Trace 命中的组件使用 QueryOnly，不必进入 Physics Solve。
- 只需要刚体接触、无需射线命中的特殊对象可评估 PhysicsOnly。
- Trigger 使用 Overlap/QueryOnly，不应模拟刚体接触。
- 死亡 Capsule 已设为 NoCollision，避免它和 Ragdoll Mesh 同时形成两套碰撞。

针对 Scene Query，则是专用 Weapon/Melee Channel、Single/Multi 和早期过滤。分层不是手写一棵检测树；UE 底层已经有空间加速结构，项目层主要负责提供准确的过滤语义。

每次改 Profile 都要验证双向响应。A 对 B 的最终结果取双方中更宽松的响应：任一方 Ignore 则忽略，Block 与 Overlap 组合得到 Overlap，只有双方都 Block 才真正阻挡。只改其中一个蓝图而不检查另一方，容易出现编辑器里“看起来设了 Block”但运行结果仍不符合预期。

#### 24.21.6 并行计算：不是“打开 GPU”

UE5.5 `UPhysicsSettings` 提供 Chaos Threading Model、`bTickPhysicsAsync` 和 `AsyncFixedTimeStepSize` 等设置。物理求解器可以通过 Task Graph 或专用线程并行执行，但并行化有三个前提：

1. Profile 证明物理求解或 Game/Physics 同步是瓶颈。
2. 工作量足够大，能够覆盖任务调度、缓冲和同步开销。
3. Gameplay 能接受物理结果和碰撞回调的时序变化。

并行不能减少总工作量，只是在多个核心上调度工作；过多碰撞对、Body 和 Constraint 仍然需要求解。小型场景中，线程切换和同步成本可能抵消收益。

线程边界：

```text
Game Thread
-> 提交 Actor/Component 状态和物理命令

Physics Tasks / Physics Thread
-> 求解刚体、接触和约束

结果缓冲/同步
-> Game Thread 读取 Transform、处理回调和 Gameplay 事件
```

不能在任意 Physics/Worker Thread 直接修改 UObject、HealthComponent、TSet 或广播 Gameplay Delegate。并行任务应产出纯数据，再在安全线程提交权威状态。

截图中的“GPU 加速”不适合直接套用到本项目。Chaos 某些领域和其他引擎技术可能使用 GPU，但 UE5 常规 Gameplay 刚体、角色和 Scene Query 没有一个可靠的“全部转 GPU”按钮。当前应先减少查询、活动 Ragdoll 和碰撞对，再评估 Chaos 线程模型。

#### 24.21.7 时间步：稳定性、CPU 和 Gameplay 时序的三方取舍

物理模拟用时间步 `DeltaTime` 积分运动。大时间步意味着每秒求解次数少，但误差、穿透、约束抖动和高速失稳风险更高；小时间步更稳定，但 CPU 成本上升。

UE5.5 关键设置：

| 设置 | 作用 | 风险 |
| --- | --- | --- |
| `MaxPhysicsDeltaTime` | 限制单次物理步允许的最大 Delta | 设置过小且帧率太低时，物理可能表现为慢于真实时间 |
| `bSubstepping` | 将一个渲染帧的物理时间拆成多个小步 | 稳定性提高，但 CPU 和内部记账增加 |
| `MaxSubstepDeltaTime` | 单个子步最大时长 | 越小通常越稳定，也越容易产生更多子步 |
| `MaxSubsteps` | 每帧最多拆多少步 | 防止卡帧后产生不可控的物理追赶成本 |
| `bTickPhysicsAsync` | 在异步线程按独立物理节奏更新 | 结果缓冲、时序和实验性功能需要验证 |
| `AsyncFixedTimeStepSize` | 异步物理固定步长 | 提高节奏可预测性，不等于跨平台绝对确定性 |

Substepping 的重点是“用更多小步换稳定性”，不是性能优化。它可能改善 Ragdoll 抖动和复杂 Physics Asset，但会增加 CPU；碰撞回调还可能在一个游戏帧中积累多个子步事件，因此 Damage、Overlap 和生命周期入口仍需幂等。

截图所说“稳定允许时增大步长”在通用模拟器中成立，但在本项目不能直接照抄：

- 枪械 Line Trace 是瞬时 Scene Query，不靠 Substep。
- 剑刃漏判由动画采样和帧间 Sweep 处理，不靠 Chaos 时间步。
- CharacterMovement 有自己的移动 Tick/子步逻辑，不应和 Ragdoll 求解器设置混为一谈。
- 只有实测 Ragdoll 抖动、约束爆炸或低帧率穿透时，才调 Substep/MaxDelta，并做 30/60/120 FPS 与卡帧用例。

#### 24.21.8 六类方案映射到当前项目

| 方向 | 当前已有 | 下一步 | 优先级 |
| --- | --- | --- | --- |
| Physics LOD | CharacterMovement/动画有距离分级；尸体有 LifeSpan | 建立活动 Ragdoll 预算和 Full/Sleep/Frozen/Destroy 状态 | 高 |
| 休眠 | Chaos 可自动休眠；代码尚无尸体治理 | 统计 Awake 时间，稳定后 Sleep；超预算再冻结 | 高 |
| 简化碰撞体 | Capsule、Sphere、Physics Asset 基础形状 | 审计 Physics Asset Body/Constraint、自碰撞和 Complex Trace | 高 |
| 分层检测 | Pawn/Ragdoll Profile、死亡 Capsule NoCollision | 专用 Weapon/Melee Channel 和完整碰撞矩阵 | 最高 |
| 并行计算 | 引擎可用 Task Graph/异步物理 | 先用 Insights 找 Physics/Sync 热点，再评估设置 | 低，数据触发 |
| 时间步 | 当前配置未发现项目级覆写 | 仅在 Ragdoll 稳定性问题出现后做 Substep A/B | 低，问题触发 |

当前最划算的路线不是先改线程或时间步，而是：

```text
碰撞通道与 Query 计数器
-> Ragdoll 活动数量/清理预算
-> Sleep/Freeze 状态治理
-> Physics Asset 简化
-> Profile 仍显示 Physics 热点
-> 再评估 Async/Substep/Solver 设置
```

#### 24.21.9 物理性能验证指标

需要同时记录数量、时间和质量：

| 维度 | 指标 |
| --- | --- |
| 数量 | Active/Awake Rigid Bodies、Ragdoll 数、Contact Pairs、Constraint 数、Trace/Sweep 数 |
| 时间 | Game Thread、Physics/Chaos、Task Graph、同步等待、Frame Average/P95 |
| 内存 | Physics Asset/Body、尸体 Actor 数、多轮波次后的对象和内存回落 |
| 稳定性 | 抖动、穿透、爆炸飞散、Pose 跳变、低帧率行为 |
| Gameplay | 命中率、穿墙、尸体阻挡、冲量反馈和唤醒是否正确 |

工具组合：

- Unreal Insights：观察 Game Thread、Physics/TaskGraph 和同步区间。
- Chaos Visual Debugger：录制并逐帧检查刚体、碰撞和子步状态。
- Physics Asset Editor：审计 Body、Constraint、Collision 和模拟稳定性。
- CSV/自定义 Stat：记录 RagdollActive/Awake、Trace/Sweep 和攻击窗口数量。
- 固定关卡 A/B：10/25/50 尸体，30/60/120 FPS，近景/远景和有无冲量。

#### 24.21.10 高频面试追问

| 问题 | 回答重点 |
| --- | --- |
| 休眠和关闭物理有什么区别？ | Sleep 可被碰撞/力唤醒并保留刚体；Disable/关闭模拟更省但需显式恢复和处理姿势 |
| 物理 LOD 只看距离吗？ | 还看可见性、速度、Gameplay 交互、网络观察者和全局预算 |
| 为什么简化碰撞体能优化？ | 降低窄相求交、接触点、Body Pair 和约束成本；但要验证命中与姿态质量 |
| 碰撞层怎样影响性能？ | 越早 Ignore 无关 Pair，越少进入窄相、求解和事件；也是正确性规则 |
| 多线程为什么可能没变快？ | 工作量小、任务粒度差或同步等待抵消并行收益；并行不减少总计算 |
| Chaos 能直接切 GPU 吗？ | 常规 Gameplay 刚体/查询没有通用 GPU 开关；先减少工作量并 Profile 线程模型 |
| 增大时间步为什么危险？ | 求解次数下降但积分误差、穿透和约束不稳定增加 |
| Substepping 是性能优化吗？ | 主要是稳定性优化，以更多 CPU/记账换更小物理步 |
| 为什么项目不先开 Substep？ | 当前核心是 Query 和 CharacterMovement；只有 Ragdoll 稳定性证据才值得承担成本 |
| 怎么判断 Ragdoll 应冻结？ | 持续休眠/低速、距离与可见性、交互需求和全局预算共同决定 |

#### 24.21.11 UE5.5 源码入口

- `Engine/Source/Runtime/Engine/Classes/PhysicsEngine/PhysicsSettings.h`：Substep、Async Physics、Max Physics Delta。
- `Engine/Source/Runtime/Engine/Classes/PhysicsEngine/BodyInstance.h`：SleepFamily、Sleep Threshold、CCD 和 Body Collision。
- `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`：Sleep/Wake、CCD、Simulate Physics 和 Collision 接口。
- `Engine/Source/Runtime/Engine/Classes/Components/SkeletalMeshComponent.h`：全身刚体 Sleep/Wake 和 Physics Asset 模拟。
- Epic 文档：[Physics Sub-Stepping](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-sub-stepping-in-unreal-engine)。
- Epic 文档：[Physics Settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-settings-in-the-unreal-engine-project-settings)。
- Epic 文档：[Chaos Physics](https://dev.epicgames.com/documentation/unreal-engine/physics-in-unreal-engine)。

### 24.22 复习深度分级

面试准备不要求把每个算法都学到实现求解器。按下面三层控制投入：

| 层级 | 必须掌握的程度 | 内容 |
| --- | --- | --- |
| A：必须闭眼讲 | 能结合代码画调用链、比较方案、处理新条件 | Query/Simulation 区别；Line/Sweep/Overlap；Single/Multi；Channel/Object/Profile；Simple/Complex；伤害链；Ragdoll LOD/Sleep；Substep 取舍 |
| B：知道原理 | 能用一两句话解释作用，不冒充项目实现 | 宽相/窄相；AABB/Grid/BVH；SAT/GJK；Physics Thread；固定/可变时间步 |
| C：按岗位选学 | 只有面试官继续深入时展开 | GJK/EPA 数学推导、Chaos Solver 源码、约束求解器、跨平台确定性和网络物理预测 |

判断标准不是“背了多少名词”，而是遇到场景能否回答：

```text
这是 Query 还是持续 Simulation 问题
-> 工作量由次数、候选、Body、Pair、Constraint 还是 Step 决定
-> 当前最小改动是什么
-> 正确性/画质/延迟代价是什么
-> 用什么固定条件验证
```

### 24.23 统一场景题：查询、碰撞、AI 与物理性能

#### 场景 1：100 个敌人每帧搜索玩家，Game Thread 变慢

**判断**：高频目标搜索和 AI 更新问题，不是刚体求解问题。

**回答主线**：

1. 先统计每帧 `GetAllActorsOfClass`、距离检查和 LOS Trace 数。
2. 单玩家目标由 GameMode/Subsystem 生成时注入，缓存引用，失效时更新。
3. 多玩家时维护候选注册表，按阵营/距离粗筛，再按威胁评分。
4. 感知按事件或状态分级 Timer 更新，远处敌人降频并错峰。
5. 用 Decision Count、Target Resolve、LOS Trace、Game Thread P95 验证。

**危险回答**：“换成八叉树就好了。”空间树不解决每个敌人都在每帧重复决策的问题。

#### 场景 2：敌人隔着墙挥刀仍能伤到玩家

**判断**：查询过滤和阻挡语义错误。

**当前根因**：`SweepMultiByObjectType(ECC_Pawn)` 只返回 Pawn，墙体不是候选，也就不能挡住伤害。

**回答主线**：建立 MeleeTrace Channel，让 WorldStatic 和 Player Block、同阵营 Enemy Ignore；单目标攻击用 SphereSweepSingle 获取第一个阻挡对象，只有玩家才结算。若设计是横扫多人，则 Multi 结果按距离和 Blocking Hit 截断。

**验证**：薄墙、门框、玩家贴墙、敌人贴墙、动态门分别回归。

#### 场景 3：低帧率时剑穿过玩家却不扣血

**判断**：离散动画采样造成 tunneling，不等同于 Chaos 刚体 CCD。

**回答主线**：记录上一帧与当前帧的 Sword Base/Tip，用 Sphere Sweep 覆盖帧间轨迹；采样数按端点位移/TraceRadius 自适应，并设上限。比较漏判率、Sweep/Attack 和 P95，而不是无条件扩大半径。

**危险回答**：“把 Tick 开得更快。”帧率下降时 Tick 本身也变少，而且会增加全局成本。

#### 场景 4：50 个敌人同时死亡后 CPU 飙升

**判断**：活动 Ragdoll Body、Constraint、Contact Pair 和同步成本上升。

**回答主线**：

1. 记录 Active/Awake Ragdoll、Physics 时间和 Contact/Constraint。
2. 建立全局活动 Ragdoll 预算。
3. 刚死亡近景保持全模拟；稳定后 Sleep；远景或超预算冻结 Pose 并关闭模拟；超时 Destroy。
4. 简化 Physics Asset Body/Constraint 和自碰撞。
5. Ragdoll Ignore Pawn，避免尸体与整群活敌人形成大量 Pair。

**危险回答**：“打开 GPU 物理”或“直接全部 Destroy”。前者没有通用开关，后者可能破坏死亡反馈。

#### 场景 5：布娃娃不断抖动，偶尔穿地

**判断**：可能是 Physics Asset、质量比、Constraint、碰撞体穿插、低帧率时间步或求解精度问题。

**回答顺序**：先在 Physics Asset Editor 检查初始穿插、Body 和 Constraint；再检查缩放、质量、Collision 和大冲量；只有问题随低帧率明显加重时，才评估 Max Physics Delta/Substep。Substep 用更多 CPU 换稳定，不能作为第一反应。

#### 场景 6：枪械命中很准，但大量自动武器时 Trace 成本升高

**判断**：Scene Query 次数和复杂查询成本，不是持续刚体模拟。

**回答主线**：统计 Accepted Shot 与 Weapon Trace，确认没有蓝图/C++ 双 Timer；普通枪用 Single；建立 Weapon Channel 早期过滤；对 Simple/Complex 做命中准确率 A/B；霰弹批量控制表现事件。即时命中通常保持同步，不为了异步而改变伤害时序。

#### 场景 7：Projectile 高速飞行穿过薄墙

**判断**：实体高速运动的离散碰撞问题。

**回答主线**：先确认 ProjectileMovement 是否使用 Swept Movement；增加合理碰撞半径；只对高速关键物体启用 CCD；必要时限制最大速度或使用子步。Hitscan 武器不存在这个实体穿透问题，不应把两条方案混讲。

#### 场景 8：优化物理后 FPS 没变化

**判断**：可能是 GPU Bound、Physics 原本不是瓶颈，或收益被同步/其他系统覆盖。

**回答主线**：先用 `stat unit`/Insights 判断 Game、Draw、GPU；报告 Physics 时间、Query Count 和 P95，不只报 FPS。优化可以增加预算余量而不改变 GPU Bound 场景的最终帧率。

#### 场景 9：开启 Substepping 后一次碰撞触发多次回调

**判断**：多个物理子步的回调在游戏帧末排队处理，属于时序和幂等问题。

**回答主线**：Damage/Overlap 不能直接把“每个回调”当一次 Gameplay 事务；使用 AttackId、命中 TSet、消费标记或状态机保证一次动作只提交一次。记录回调时间和 Body，确认不是两个真实碰撞。

#### 场景 10：远处物理物体很多，但大部分静止

**判断**：大量 Body 是否仍 Awake、是否产生无意义 Pair/事件，以及 Actor/Component 本身成本。

**回答主线**：允许自动 Sleep，按 Significance 对远处对象关闭事件、简化碰撞或冻结；静态装饰不要开启 Simulate Physics；开放世界还要结合分区加载。只有需要重新交互的对象才保留唤醒路径。

#### 场景 11：开启 Async Physics 后偶发读取旧 Transform

**判断**：Game Thread 与 Physics Thread 使用缓冲结果，读取和回调时序发生变化。

**回答主线**：明确权威发生在哪个 Tick Phase，不在任意工作线程修改 UObject；Gameplay 提交使用安全同步点；对延迟一帧不可接受的逻辑继续走同步路径。用 Insights 找同步等待，不能假设异步一定更快。

#### 场景 12：对象池复用一个物理 Actor 后自己飞走

**判断**：上一次生命周期的线速度、角速度、Sleep/Wake、碰撞、Timer 或 Delegate 没有复位。

**回答主线**：Pool Acquire 时重置 Transform、Linear/Angular Velocity、Collision/Profile、Simulate Physics、Sleep 状态和 Gameplay 标记；Release 时解绑事件并停止模拟。对象池降低 Spawn/Destroy，但增加复位正确性成本，必须由 Profile 证明需要。

#### 场景 13：角色被爆炸击退，直接对 Capsule AddImpulse 没效果

**判断**：ACharacter 由 CharacterMovement 驱动，不是自由刚体。

**回答主线**：Gameplay 击退用 `LaunchCharacter`、Root Motion Source 或自定义 Movement Mode；死亡后切 Ragdoll 才对 Skeletal Mesh Body 施加冲量。不要把 Character Capsule 与 Ragdoll Mesh 当同一个运动权威。

#### 场景 14：多人游戏里客户端显示命中，但服务端没扣血

**判断**：网络权威和延迟问题，不只是 Collision Profile。

**回答主线**：服务端权威执行/验证 Trace 和 Damage；客户端即时播放预测表现；需要时基于时间戳做服务器回溯。物理对象还要选择服务器权威复制或 Networked Physics 模式。当前单机项目没有实现这部分，只能作为迁移方案。

#### 场景 15：100 个敌人同时 MoveTo，寻路和移动出现尖峰

**判断**：需要拆开路径请求、路径跟随、CharacterMovement 和动画成本，不能把它们统称为“AI 慢”。

**回答主线**：先用 Insights/CSV 分离 Nav Query、Move Request、Movement 和 Animation；避免目标未变化时重复发起 MoveTo；按距离分级决策和移动更新并错峰；共享的战术位置由 SurroundManager 分配，但每个 Agent 仍由 NavMesh 生成可达路径。若动态障碍导致频繁重算，再评估局部避障、路径刷新阈值和 Navigation Invoker。

#### 场景 16：霰弹枪一次发射 20 条射线，开火出现尖峰

**判断**：Scene Query 数量、Multi/Complex 参数和每个命中的表现后处理共同放大成本。

**回答主线**：一次开火只提交一次弹药和表现事务；20 条射线使用统一参数并统计 WeaponTrace 数；按 Actor/Bone 聚合伤害规则；Decal、Niagara 和声音设置数量预算；用固定散布验证命中分布、查询时间和表现事件数量。不能为了省查询直接把 20 条独立弹丸错误地合成一次伤害。

#### 场景 17：子弹需要飞行时间、下坠和被玩家看见

**判断**：Hitscan 的瞬时查询语义已经不满足玩法，需要迁移为 Projectile，而不是给 LineTrace 加一个延迟扣血。

**回答主线**：ProjectileMovement 使用 Swept Movement；高速关键弹丸评估 CCD/子步；定义碰撞体、伤害权威、最大寿命、穿透/反弹和对象池复位；多人条件下由服务端生成或验证并设计客户端预测表现。对象池是否需要由 Spawn/Destroy/GC 数据决定。

#### 场景 18：墙能挡视线，但设计要求部分墙不能挡子弹

**判断**：`ECC_Visibility` 同时承担视觉和武器语义，Collision Channel 设计耦合。

**回答主线**：建立专用 Weapon Trace Channel，在材质/Actor Profile 中分别配置 Visibility 与 Weapon 响应；门、玻璃、薄板和不可穿透墙建立固定矩阵。若还需要穿透厚度和能量衰减，则在首次命中后做有限次数的出口查询，而不是无界 Multi Trace。

#### 场景 19：不同护甲、骨骼和材质需要不同伤害

**判断**：命中查询负责提供 `FHitResult`，伤害计算需要独立的 Hit Zone/材质规则，不能继续堆骨骼名分支。

**回答主线**：用 Physical Material、Hit Zone 数据或 Damageable 接口把命中上下文转换为倍率/抗性；HealthComponent 仍只负责最终 Clamp、事件和死亡幂等。配置需要有默认分支，防止未知骨骼导致零伤害或异常倍率。

#### 场景 20：加入可破坏墙体后，导航、伤害和性能都受影响

**判断**：这是 Geometry Collection、碰撞、导航更新、生命周期和持久化的跨系统问题。

**回答主线**：先定义破坏是否纯表现、是否改变通路、碎片能否伤人以及保存多久；只让关键近景对象进入高质量 Chaos 模拟；碎片按预算 Sleep/Freeze/Destroy；需要改变可达区域时再触发受控 NavMesh 更新。验证破坏前后路径、碰撞矩阵、Active Body 数和 Physics P95。

#### 场景 21：AI 降频后，敌人反应明显变笨

**判断**：优化破坏了玩法响应预算，说明更新频率没有按重要性分级。

**回答主线**：近距离、可见、正在攻击或刚受击的敌人保持高频；远距离不可见敌人低频并错峰；目标丢失、受伤和进入攻击范围等关键事件立即唤醒一次决策。除了 Game Thread，还要记录平均/P95/最大响应延迟和错误攻击率。

### 24.24 场景题统一回答模板

```text
1. 先分类：Scene Query、Physics Simulation、Navigation、Animation 还是 Network
2. 量化：次数、候选、Active Bodies、Pairs、Constraints、Steps、线程等待
3. 查正确性：Channel/Profile、生命周期、权威状态、回调顺序
4. 最小改动：先降频/过滤/预算，再改算法/线程/时间步
5. 说代价：精度、延迟、表现、可交互性、维护复杂度
6. 定验证：固定关卡、边界用例、Avg/P95、质量指标和回归矩阵
```

如果题目没有给规模、帧率和玩法要求，应主动问或声明假设。没有数据时说“我会先采集这些指标”，不要直接给出虚构的性能结论。
