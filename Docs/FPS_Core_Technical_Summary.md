# FPS 项目统一复习主线

> 核对基线：UE 5.5，代码与实验记录截至 2026-08-01。
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
-> Character：角色状态、弹药、换弹、玩家生命值
-> WeaponComponent：武器输入、单次射击、Line Trace、散布和后坐力
-> EnemyCharacter：敌人受伤/死亡、近战攻击窗口、动画和移动性能分级
-> EnemyAIController：Idle/Chase/Attack/Dead FSM、定时决策和路径请求
-> SurroundManager：双环槽位、NavMesh 投影、攻击名额和弱引用治理
-> GameMode：波次生成、倒计时、胜负和 AI 上下文注入
-> HealthComponent：统一扣血、血量限制和死亡广播
-> Blueprint：动画、声音、特效、Camera Shake、后处理和射速 Timer
```

权威规则放在 C++，表现放在蓝图。蓝图不能重新计算弹药、伤害或死亡，否则会形成两套状态。

## 2. 开火与弹药

### 2.1 输入生命周期

武器拾取并附加后，`UfpstrueWeaponComponent::AttachWeapon()`：

1. 将武器附加到玩家 `Mesh1P` 的 `GripPoint`。
2. 把武器组件登记到 Character。
3. 添加武器 Input Mapping Context。
4. 将开火 Action 的 `Started` 绑定到 `StartFire()`。
5. 将 `Completed / Canceled` 绑定到 `StopFire()`。

当前职责拆分：

- `StartFire()`：进入按住开火状态并广播 `OnWeaponFireStarted`。
- `Fire()`：执行一次真实射击尝试。
- `StopFire()`：退出开火状态并广播 `OnWeaponFireStopped`。

### 2.2 当前连续射击链路

```text
按下开火
-> WeaponComponent::StartFire
-> Character::NotifyFireStarted
-> Blueprint OnWeaponFireStarted
-> 蓝图启动射速 Timer
-> Timer 循环调用 WeaponComponent::Fire
-> 松开、取消、换弹、空仓或死亡
-> StopFire / OnWeaponFireStopped
-> 蓝图停止射速 Timer
```

重要边界：

- `StartFire()` 本身不发射子弹。
- 如果蓝图没有在开始事件中调用一次 `Fire()` 并启动 Timer，按下开火不会产生射击。
- 如果蓝图没有在停止事件中清理 Timer，可能在松开、换弹或死亡后继续触发。
- C++ `Fire()` 只代表“一次射击”，不负责自动射速。

### 2.3 单次射击

`UfpstrueWeaponComponent::Fire()` 的顺序：

```text
CanFire
-> 获取 World 和第一人称 Camera
-> Character::TryConsumeAmmo
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
- 玩家不在换弹。

它不检查剩余弹药。弹药不足由 `TryConsumeAmmo()` 负责，并自动请求换弹。

### 2.4 弹药所有权

弹药由 `AfpstrueCharacter` 管理：

```text
MagazineSize = 30
CurrentAmmo = 30
ReserveAmmo = 90
```

`TryConsumeAmmo()`：

1. 换弹中或死亡时拒绝开火。
2. `CurrentAmmo <= 0` 时调用 `RequestReload()`，不产生射击。
3. 其他情况执行 `CurrentAmmo--` 并返回成功。

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
-> CanReload
-> 判断是否为空仓换弹
-> 选择 ReloadDuration / EmptyReloadDuration
-> bIsAiming = false
-> 停止开火状态并通知武器
-> CharacterState = Reloading
-> WeaponComponent::NotifyReloadStarted
-> OnWeaponReloadStarted(bWasEmptyReload)
-> 设置 Reload Timer
```

当前时间：

- 普通换弹：`0.8` 秒。
- 空仓换弹：`1.2` 秒。

蓝图通过 `bWasEmptyReload` 选择对应动画和表现。弹药结算的权威入口仍是 C++ Timer，不是 AnimNotify。

### 3.3 换弹结算

Timer 到期调用 `FinishReload()`：

```cpp
AmmoNeeded = MagazineSize - CurrentAmmo;
AmmoToLoad = Min(AmmoNeeded, ReserveAmmo);
CurrentAmmo += AmmoToLoad;
ReserveAmmo -= AmmoToLoad;
```

随后：

1. 临时将状态设为 `Idle`。
2. `UpdateCharacterState()` 根据当前水平速度切换为 `Idle` 或 `Moving`。
3. 广播 `OnWeaponReloadFinished`。

玩家死亡时会清理 `ReloadTimerHandle`，因此换弹中死亡不会在之后补充弹药。

### 3.4 当前换弹边界

- 换弹结算依赖固定 Timer，Montage 时长必须与 C++ 时长一致。
- 当前 `StartReload()` 直接将 `bIsAiming` 设为 `false`，但不调用 `OnAimChanged(false)`；蓝图需在换弹开始事件中自行恢复 FOV，或未来统一由 C++ 广播。
- 如果以后改用 AnimNotify 结算弹药，必须保证 Timer 只作为兜底，并增加“本次换弹只结算一次”的保护，不能让 Timer 和 Notify 同时加弹。

## 4. Line Trace

### 4.1 射线生成

`FireLineTrace()` 使用第一人称 Camera：

```text
Start = Camera World Location
Forward = Camera Forward Vector
Spread = 瞄准或腰射散布角
ShotDirection = VRandCone(Forward, Spread)
End = Start + ShotDirection * LineTraceRange
```

当前参数：

- 射程：`10000`。
- 腰射散布：`1.5` 度。
- 瞄准散布：`0.25` 度。

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

- C++ 角色状态：`Idle / Moving / Reloading / Dead`。
- C++ 弹药和换弹 Timer。
- C++ 单次射击、散布、Line Trace、敌人伤害和后坐力。
- 忽略玩家与武器 Owner 的查询参数。
- 玩家与敌人 HealthComponent 链路。
- 敌人连续剑刃 Sweep 和单次攻击去重。
- 玩家与敌人死亡后的基础 C++ 清理。

### 蓝图需回归验证

- 开火开始时立即调用一次 `Fire()`，之后才按射速循环。
- 所有停止路径都清理蓝图射速 Timer。
- 普通换弹和空仓换弹动画与 C++ 时长一致。
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

- 连续射击的射速调度仍依赖蓝图 Timer，需要防止重复 Timer。
- 换弹使用固定 C++ Timer，动画与结算可能因播放速率或中断而不同步。
- 头部骨骼名硬编码为 `head / neck_01`，更换 Skeleton 时需要调整。
- CPU 分级优化已经写入代码，但优化后同条件数据尚未闭环。
- 包围槽位参数目前是工程默认值，仍需要用胶囊半径、攻击距离和拥堵数据解释或再校准。
- 当前中央管理器只维护一名玩家目标，不直接支持多玩家目标选择。
- 当前没有对“剑刃与玩家之间存在墙体”做额外视线遮挡检查，碰撞通道配置必须回归。
- 部分旧 C++ 文件混有非 UTF-8 注释，后续整理编码时必须单独提交，避免污染功能 diff。

## 12. 面试自检

1. 为什么 `StartFire()` 不直接完成一次射击？
2. 蓝图射速 Timer 与 C++ 单次射击分别负责什么？
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
| Character | 玩家状态、弹药、换弹、生命值入口 | 不计算命中特效 | 显式状态、组件模式 |
| WeaponComponent | 输入绑定、一次射击、Trace、后坐力 | 不拥有 UI 状态 | 组件模式、单一职责 |
| HealthComponent | 血量限制、伤害和死亡广播 | 不播放动画 | 组件模式、观察者模式 |
| EnemyCharacter | 攻击窗口、Socket Sweep、受击和死亡执行 | 不做全局站位分配 | 状态保护、每次攻击命中集合 |
| EnemyAIController | FSM、定时决策、MoveTo 请求和转向 | 不播放攻击表现 | 状态模式、更新方法 |
| SurroundManager | 双环槽位、攻击名额、补位和弱引用清理 | 不做 NavMesh 路径搜索 | 中央协调器、TArray/TMap/TSet |
| GameMode | 开局、倒计时、波次、出生、胜负、上下文注入 | 不保存跨关卡持久数据 | 服务端规则入口、Timer |
| UMG/蓝图 | HUD、动画、声音、后处理和参数配置 | 不重新计算伤害、弹药和死亡 | 观察者模式、表现层 |

### 13.2 设计模式不是装饰词

- **组件模式**：武器和生命值从 Character 中拆出，复用逻辑并降低类体积。
- **状态模式**：AI 的 `Idle/Chase/Attack/Dead` 和玩家的 `Idle/Moving/Reloading/Dead` 让非法转换有统一入口。
- **观察者模式**：Health、Ammo、Wave、Time 和 Result 通过动态多播委托驱动 UI，不让 Widget 每帧查询。
- **更新方法**：AI 用按状态和距离分级的一次性 Timer 调度，CharacterMovement 也按距离降频。
- **中央协调器**：SurroundManager 维护“一个槽位只能属于一个敌人、攻击者不超过上限”这类全局不变量。
- **弱引用治理**：Manager 不应因为保存敌人引用而阻止敌人被回收。
- **对象池暂缓**：当前 Hitscan 没有实体子弹；只有 Spawn/Destroy 或 GC 数据证明是瓶颈时，才池化特效、贴花或敌人。

## 14. 五条必须闭眼讲出的调用链

### 14.1 玩家射击

```text
Enhanced Input
-> WeaponComponent::StartFire
-> 蓝图射速 Timer 调用 Fire
-> Character::TryConsumeAmmo
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
-> CanReload
-> 停止开火和瞄准
-> CharacterState = Reloading
-> OnWeaponReloadStarted
-> C++ Timer
-> FinishReload 只结算一次
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
-> Notify End / Montage 中断 / 死亡 / 4 秒兜底
-> 关闭窗口并释放名额
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
-> 玩家死亡或倒计时/波次条件满足
-> OnGameResult
-> 胜负 UI / Restart Level
```

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
| 同一次挥砍重复扣血 | Notify Tick 多帧命中同一玩家 | 每次 BeginAttackWindow 清空 `HitActorsThisAttack`，成功命中后加入 TSet | 已实现 |
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
| 160 AI 无法稳定 60 FPS | CSV 中 Game Thread 20.76 ms；CharacterMovement 6.919 ms、Animation 3.190 ms、Pathfinding 0.686 ms | 优先做移动 60/30/15Hz 分级、Animation URO、不可见降级和死亡停更，不重写 NavMesh | Baseline 已有；优化后 A/B 待测 |
| 误以为寻路是最大瓶颈 | 只凭系统复杂度猜测，没有看计时 | 用 CSV/Insights 分项后确认 Movement+Animation 更贵 | 已形成数据驱动结论 |
| 编辑器出现 Texture Streaming Pool 告警 | 独立运行预算 1000 MB、Over Budget 0 MB；定位到六张 4K/长边 4K 环境植被纹理 | 不扩大 Pool，只把六张纹理的 Max Texture Size 限制到 2048 | Streaming 212.27 -> 152.27 MB，下降 28.3% |
| `[VSM] 非 Nanite 标记工作队列溢出` | 属于 Virtual Shadow Map 页面标记，最大候选约 512 MB，不是 Texture Streaming Pool | 分开处理 VSM 与纹理流送；检查非 Nanite 大面积阴影投射 Mesh、阴影策略和 GPU Profile | 已分类，VSM 资产级优化未展开 |
| 工业场景材质缺失 | 日志指出 `/Game/FactoryDistrict/Materials/Black` 等引用不完整 | 修复 Redirector/缺失资产或替换材质；这属于资源完整性，不是内存泄漏 | 待资源回归 |
| 担心 Timer、Delegate、Widget、尸体泄漏 | 仅凭内存峰值不能判断泄漏 | 固定多轮 Spawn/Kill/Wait GC，记录 Actor/UObject 数和内存是否回落；Delegate/Timer 在死亡和 EndPlay 清理 | 生命周期代码已有，完整内存曲线待测 |

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

| 敌人数 | Frame Avg ms | Frame P95 ms | Game Avg ms | Game P95 ms | GPU Avg ms | Animation ms | CharacterMovement ms | Pathfinding ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 20 | 13.27 | 15.48 | 4.20 | 5.10 | 11.06 | 0.584 | 0.846 | 0.016 |
| 40 | 14.18 | 15.87 | 6.26 | 7.61 | 12.48 | 0.908 | 1.633 | 0.184 |
| 80 | 15.89 | 17.52 | 11.43 | 12.89 | 14.49 | 1.712 | 3.087 | 0.329 |
| 160 | 20.77 | 23.21 | 20.76 | 23.25 | 16.92 | 3.190 | 6.919 | 0.686 |

160 AI 平均帧率约为：

```text
1000 / 20.77 = 48.1 FPS
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

`HitActorsThisAttack` 的生命周期就是一次攻击。`BeginAttackWindow` 清空集合，命中成功后加入弱引用，后续 Tick 命中同一 Actor 直接跳过；下一次攻击重新清空，所以能够再次造成伤害。攻击 ID 是可扩展方案，但当前单敌人不并发多个攻击窗口，集合生命周期已经足够。

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

#### Q18. 20.76 ms 怎么解释？

60 FPS 帧预算是 16.67 ms。160 AI 时 Game Thread 平均 20.76 ms，Frame 平均 20.77 ms，对应约 48.1 FPS，明确超预算；80 AI 虽平均 15.89 ms，但 P95 17.52 ms，也不能称为稳定 60 FPS。

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

1. 背熟 16.67 ms、20.76 ms、48.1 FPS、6.919/3.190/0.686 ms。
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
| Enhanced Input | 移动、瞄准、开火和换弹 | Input Action 与 Mapping Context 解耦，武器附加后再注册武器输入 |
| LineTrace / Collision Query | Hitscan、命中部位和伤害入口 | QueryParams 过滤自身；FHitResult 提供 Actor、Component、Bone 和法线 |
| AnimMontage / AnimNotifyState | 近战有效攻击窗口 | 动画只决定判定时机，C++ 决定能否伤害；中断时必须回收窗口状态 |
| AIController / NavMesh | FSM 决策和路径请求 | Recast 构建可行走网格，Detour 在 Poly 图上寻路；局部避障不等于包围站位分配 |
| Timer / Delegate | AI 降频决策和事件驱动 UI/表现 | Timer 仍在 Game Thread 调度；Delegate 降低轮询，但必须治理绑定和对象生命周期 |
| TArray/TSet/TMap/TWeakObjectPtr | 槽位、去重、映射和弱引用 | TSet 适合本次攻击去重；TMap 建敌人到槽位映射；弱引用不延长 UObject 生命周期 |
| Unreal Insights / CSV / stat | CPU、GPU、对象和流送池证据 | 先固定条件建立 Baseline，再根据调用栈/计数定位，最后同条件复测 |

### 22.4 核心调用链速记

详细版本见第 14 节，面试时至少能脱离文档画出以下链路：

```text
射击：Input -> WeaponComponent::Fire -> TryConsumeAmmo
     -> Camera LineTrace -> FHitResult -> ApplyDamage
     -> HealthComponent -> Delegate -> Blueprint 表现

换弹：Reload Input -> CanReload -> StartReload -> Montage
     -> AnimNotify/结算点 -> FinishReload -> OnAmmoChanged

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
| Camera Hitscan | 实体 Projectile | 当前武器强调即时反馈且无飞行时间；实体弹会增加生命周期、碰撞和网络预测成本 |
| HealthComponent | 在玩家和敌人类里各写一套血量 | 共享组件统一 Clamp、死亡只触发一次和事件契约 |
| 显式 FSM | Behavior Tree / GAS | 当前状态数有限，显式转换更容易闭卷解释和压测；没有为了简历堆框架 |
| NotifyState 攻击窗口 | 武器碰撞全程开启 / 单个命中帧 | 全程碰撞会在起手收招误伤；单帧容易漏过高速刀刃，窗口内 Sweep 更稳定 |
| 双环槽位 + Token | 全员 MoveToActor(Player) | 同目标点导致扎堆、推挤和同时攻击；中央分配能保证唯一占位和受控攻击节奏 |
| Timer 决策 + 距离分级 | 所有 AI 每帧完整决策 | 降低重复判断成本，同时在近战窗口恢复高频保证判定精度 |
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
| 攻击重复扣血 | 攻击窗口每帧 Sweep 未按一次挥砍去重 | Begin 清空 TSet，Tick 只伤害未命中对象，End/中断/死亡统一清理 |
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

回答性能题时先问场景规模、帧率目标、平台和瓶颈线程，再提出方案：

| 场景题 | 回答主线 |
| --- | --- |
| 100 个敌人同时寻路怎么优化 | 固定场景 Baseline；决策降频/错峰；避免重复 MoveTo；距离分级；动画 URO；先测 Movement、Animation、Pathfinding 各自成本 |
| AI 降频会不会变笨 | 会引入响应延迟；近战和可见近距离保持高频，远距离低频；记录平均/最大响应延迟而不是只看 FPS |
| 死亡对象怎么降成本 | StopMovement、停止 AI/Timer、关闭不必要碰撞和动画更新、延迟销毁；验证对象数和内存是否回落 |
| 要不要做对象池 | 先看 SpawnActor、Destroy、GC 尖峰和对象复位复杂度；优先池化高频、同构、可完整 Reset 的对象 |
| 纹理池超预算怎么办 | 先定位实际高占用纹理，再调 MaxTextureSize/LODGroup/NeverStream；不能只增大 PoolSize 掩盖资源问题 |
| GPU 慢怎么定位 | stat unit 判断 GPU bound，ProfileGPU 找 Pass，Shader Complexity/Quad Overdraw 找材质与透明叠加，再逐项开关复测 |
| 60 FPS 的标准是什么 | 平均值和 P95/P99 都要接近 16.67 ms；不能只报一次最高 FPS；测试条件和硬件必须写明 |

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
