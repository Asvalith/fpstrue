# FPS 项目架构、性能与 UE 机制说明

> 适用分支：`main`。本文按当前源码核对，不把规划中的功能写成已经实现的成果。
>
> 证据口径分为四类：**源码已实现**、**消融已证明**、**机制生效但收益未证明**、**未来扩展**。本文严格区分四类结论。

## 0. 系统总览

当前项目不是靠一个“大而全”的 AI 框架解决问题，而是把职责拆成几层：

- `Character / EnemyCharacter`：角色身份、移动和对外接口；
- `HealthComponent`：生命值唯一写入者，负责伤害、Clamp 和一次性死亡事件；
- `WeaponComponent / EnemyCombatComponent`：分别管理玩家武器事务和敌人攻击事务；
- `EnemyAIController`：单个敌人的 `Idle / Chase / Attack / Dead` 决策；
- `SurroundManager`：所有敌人共享的槽位、攻击名额、玩家位置快照和 MoveTo 提交预算；
- `SignificanceCoordinator`：统一采样并下发玩法、移动和渲染档位；
- `AnimationSharingCoordinator`：把符合条件的普通敌人接入 UE Animation Sharing。

完整运行链可以压缩成：

```text
输入/Timer/Notify
    -> 角色或 AIController 发出命令
    -> 组件校验并修改唯一状态
    -> UE 移动、导航、动画或伤害系统执行
    -> 委托/蓝图事件通知 HUD、动画、音效和 GameMode
```

项目源码入口：

- `Source/fpstrue/fpstrueCharacter.cpp`
- `Source/fpstrue/fpstrueHealthComponent.cpp`
- `Source/fpstrue/fpstrueWeaponComponent.cpp`
- `Source/fpstrue/fpstrueEnemyAIController.cpp`
- `Source/fpstrue/fpstrueEnemyCombatComponent.cpp`
- `Source/fpstrue/fpstrueSurroundManager.cpp`
- `Source/fpstrue/fpstrueEnemyCharacter.cpp`
- `Source/fpstrue/fpstrueEnemySignificanceCoordinator.cpp`
- `Source/fpstrue/fpstrueEnemyAnimationSharingCoordinator.cpp`

---

## 1. 组件如何通信，如何复用

### 1.1 项目用了三种通信方式

#### 方式一：直接函数调用——用于必须立即执行的一对一命令

玩家输入只把意图交给当前武器，角色本身不扣弹、不做射线，也不写武器状态：

```cpp
// fpstrueCharacter.cpp:312-340
void AfpstrueCharacter::StartWeaponFire()
{
    if (EquippedWeaponComponent != nullptr && !IsDead())
    {
        EquippedWeaponComponent->StartFire();
    }
}

void AfpstrueCharacter::StartReload()
{
    if (EquippedWeaponComponent == nullptr || IsDead() ||
        !EquippedWeaponComponent->CanReload())
    {
        return;
    }

    StopAim();
    StopSprint();
    EquippedWeaponComponent->RequestReload();
}
```

这是命令关系：调用方需要明确知道成功或失败，所以直接调用比广播更清晰。

#### 方式二：动态多播委托——用于一个状态变化通知多个消费者

生命组件收到 UE 伤害事件后统一扣血，再广播变化：

```cpp
// fpstrueHealthComponent.cpp:45-65
if (DamageAmount <= 0.0f || IsDead())
{
    return;
}

CurrentHealth = FMath::Clamp(CurrentHealth - DamageAmount, 0.0f, MaxHealth);
OnDamageReceived.Broadcast(AppliedDamage, DamageCauser, InstigatedBy);
OnHealthChanged.Broadcast(CurrentHealth);

if (IsDead() && !bDeathBroadcast)
{
    bDeathBroadcast = true;
    OnDeath.Broadcast();
}
```

角色在 `BeginPlay` 订阅，在 `EndPlay` 显式解绑：

```cpp
// fpstrueCharacter.cpp:72-79, 96-101
HealthComponent->OnHealthChanged.AddUniqueDynamic(
    this, &AfpstrueCharacter::HandleHealthChanged);
HealthComponent->OnDamageReceived.AddUniqueDynamic(
    this, &AfpstrueCharacter::HandleDamageReceived);
HealthComponent->OnDeath.AddUniqueDynamic(
    this, &AfpstrueCharacter::HandleDeath);

// 绑定后主动拉一次初值，避免错过组件更早的 BeginPlay 广播。
HandleHealthChanged(HealthComponent->GetHealth());
```

`DECLARE_DYNAMIC_MULTICAST_DELEGATE` 依赖 UE 反射，能被蓝图绑定；代价是比原生 C++ 委托重一些，所以适合低频状态事件，不适合每个敌人每帧广播。

#### 方式三：BlueprintImplementableEvent——C++ 决定事实，蓝图负责表现

例如死亡时，C++ 先停止武器和移动，再触发蓝图表现：

```cpp
// fpstrueCharacter.cpp:385-415
if (bDeathEffectsApplied)
{
    return;
}
bDeathEffectsApplied = true;

EquippedWeaponComponent->HandleOwnerDeath();
Movement->StopMovementImmediately();
Movement->DisableMovement();

OnPlayerDeathReported.Broadcast(this); // C++/系统监听者
OnPlayerDied();                        // 蓝图动画、音效、UI 表现
```

设计原则是：

- 必须完成的核心状态不要只放在蓝图事件里；
- C++ 状态修改完成后再通知表现层；
- HUD、音效、动画可以错过或替换，但弹药、生命、死亡和攻击名额不能靠表现层维护。

### 1.2 HealthComponent 为什么真的可复用

`UfpstrueHealthComponent` 不知道 Owner 是玩家还是敌人，也不知道 HUD、AI、GameMode。它只做四件事：

1. 监听 Owner 的 `OnTakeAnyDamage`；
2. 校验并扣除生命值；
3. 保证死亡只广播一次；
4. 提供只读 Getter 和委托。

玩家可以把 `OnHealthChanged` 转发给 HUD，敌人可以把 `OnDeath` 转发给 GameMode。组件不需要修改。这叫复用“能力”，不是复制一份代码。

### 1.3 WeaponComponent 的复用有局限

当前 `UfpstrueWeaponComponent` 直接继承 `USkeletalMeshComponent`，因此把以下两类职责绑在一起了：

- 玩法数据：弹药、射速、动作状态、Hitscan、后坐力；
- 表现载体：武器骨骼网格、Socket、附着关系。

一个 FPS Demo 只有主要枪械时很直接，但武器种类增加后会遇到：

- 枪、火箭筒、手榴弹的发射模型不同；
- 每个蓝图重复一套伤害、弹药和动画参数；
- 运行时状态和静态配置混在组件里；
- 角色当前只保存一个 `EquippedWeaponComponent`，不是完整库存系统。

所以“组件化”不自动等于“完全数据驱动”。当前健康组件复用程度高，武器组件则是面向单个 Demo 的务实实现。

---

## 2. 不永久关闭 Movement / Skeletal Tick，如何做单变量消融

### 2.1 关闭开关到底做了什么

测试脚本提供两组破坏性诊断：

```powershell
# Tools/RunProfileGuidedAblation.ps1:25-26
-BenchmarkDisableCharacterMovementTick
-BenchmarkDisableSkeletalMeshTick
```

Movement 关闭最终执行：

```cpp
// fpstrueEnemyCharacter.cpp:620-623
if (UCharacterMovementComponent* Movement = GetCharacterMovement())
{
    Movement->SetComponentTickEnabled(!bDisableCharacterMovementTick);
}
```

Skeletal Mesh 关闭最终执行：

```cpp
// fpstrueBenchmarkRunner.cpp:256-262
if (BenchmarkConfig.bDisableSkeletalMeshTick)
{
    CharacterMesh->SetComponentTickEnabled(false);
}
```

这两个实验的意义只是回答：**如果这一整类工作完全不存在，成本上界有多大？**

- Movement Tick Off 会让移动、碰撞和路径跟随的下游行为失真；
- Skeletal Mesh Tick Off 会让动画、骨骼 Socket、攻击判定和表现失真；
- 因而不能把它们当成正式优化。

历史诊断数据中：

- Movement Tick 约 `149.5 -> 1`，Movement 局部减少约 `5.103 ms`；
- Skeletal Tick 约 `162 -> 2`，Animation 局部减少约 `3.252 ms`。

它们证明“成本类别很重”，不证明“可直接关掉”。

### 2.2 一个可信的单变量 A/B 应如何跑

例如只证明 Movement 分级：

```text
A：当前默认版本
B：只加 -BenchmarkDisableMovementTiering（让全部敌人恢复每帧 Movement Tick）
```

其余条件必须相同：

- 同一 commit、Development 构建和启动参数；
- 同一地图、RHI、分辨率、Scalability、窗口状态；
- 同一敌人数、随机种子、相机位置与运动路径；
- 同一预热时间、采样时长；
- 每组独立启动进程，至少重复三次；
- A/B 顺序交错，避免温度、着色器和缓存总偏向后一组；
- 同时记录“开关确实改变了消费者数量”和“目标局部耗时确实变化”。

不能只看 FPS。Movement 实验至少记录：

```text
MovementTickCount / frame
CharacterMovement 局部 ms
GameThread ms
FrameTime P50 / P95 / P99
AliveEnemyCount
```

当前 80 敌人三次重复证据是：

| 条件 | Movement Tick/帧 | CharacterMovement |
|---|---:|---:|
| 分级开启 | 65.1 | 1.056 ms |
| 分级关闭 | 81.0 | 1.245 ms |

局部减少约 `0.189 ms`，方向三次一致。这才是“可用策略”的证据。

### 2.3 哪些数不能相加

`Animation -0.408 ms`、`ShadowDepths -0.721 ms` 和 `Skinned BLAS -0.296 ms` 位于不同线程/阶段，可能并行、包含或互相等待，不能直接相加成整帧提升。最终仍要看 Frame、GT、RT、RHI、GPU 的临界路径。

---

## 3. 敌人动画涉及哪些线程，为什么多个线程都有成本

通俗地说，一具骨骼动画不是“在一个线程完整做完”，而是一条流水线：

```text
Game Thread 发起更新并处理玩法事件
    -> Worker Threads 计算动画图、混合和骨骼姿态
    -> Game Thread 在需要时收结果、更新 Socket/碰撞等
    -> Render Thread 准备可见网格和蒙皮/阴影数据
    -> RHI Thread 翻译并提交图形命令
    -> GPU 执行蒙皮、光栅、阴影或光追
```

### 3.1 Game Thread

- 驱动 `USkeletalMeshComponent` 更新；
- 更新 Montage、状态机参数和 AnimNotify；
- 提交并收取并行动画任务；
- 在 Socket、碰撞或渲染需要新姿态时可能等待并行评估结束；
- 本项目攻击 Notify、刀刃 Socket 查询和伤害结算都属于玩法链，最终必须回到 GT。

### 3.2 TaskGraph / Worker Threads

- 动画蓝图代理更新与动画图评估；
- Sequence 解压、Blend、局部到组件空间骨骼变换等可并行部分。

“动画多线程优化”主要是把可并行计算从 GT 分出去，不是把玩法状态随便放到任意线程。UObject 修改、委托、伤害和大多数 Gameplay 仍应在 GT。

### 3.3 Render Thread / RHI Thread / GPU

- RT：处理可见性、Skeletal Mesh Scene Proxy、动态数据、阴影和光追实例；
- RHI：把渲染命令翻译成底层 API 命令并提交；
- GPU：执行实际蒙皮、ShadowDepth、材质 Pass、RT Shader 和加速结构相关工作。

因此 Insights 中 GT、Worker、RT、RHI、GPU 都出现“动画相关”成本，不代表同一段工作被重复做了五遍，而是同一对象走过了五个阶段。整帧瓶颈看最长的依赖链，不是把所有线程时间简单求和。

---

## 4. AI 的 Timer、状态分频、Movement 分级和 MoveTo 去重

### 4.1 一次性 Timer 如何实现动态间隔

默认间隔在 `fpstrueEnemyAIController.h:67-92`：

```cpp
float AttackDecisionInterval = 0.1f;
float ChaseDecisionInterval  = 0.25f;
float FarDecisionInterval    = 0.5f;
float IdleDecisionInterval   = 1.0f;
float FarDecisionDistance    = 3000.0f;
float PathRefreshDistance    = 75.0f;
float FailedMoveRetryDelay   = 0.5f;
```

首次更新随机错峰，之后每次只安排下一次：

```cpp
// fpstrueEnemyAIController.cpp:147-159
void AfpstrueEnemyAIController::StartDecisionTimer()
{
    const float FirstDelay = FMath::FRandRange(
        0.01f,
        FMath::Max(0.01f, ChaseDecisionInterval * SignificanceDecisionMultiplier));
    ScheduleNextDecision(FirstDelay);
}

void AfpstrueEnemyAIController::ScheduleNextDecision(float Delay)
{
    GetWorld()->GetTimerManager().SetTimer(
        DecisionTimerHandle,
        this,
        &AfpstrueEnemyAIController::UpdateAI,
        FMath::Max(0.01f, Delay),
        false); // one-shot
}
```

为什么不使用一个固定循环 Timer？因为攻击、近距离追击、远距离追击和 Idle 的响应要求不同。一次性 Timer 允许本轮观察状态后决定下一轮多久再来。

```cpp
// fpstrueEnemyAIController.cpp:263-288（压缩）
if (bNeedsCombatResponse)
{
    return AttackDecisionInterval;
}
if (!Context.bInChaseRange)
{
    return IdleDecisionInterval * SignificanceDecisionMultiplier;
}
if (Context.DistanceSquared >= FMath::Square(FarDecisionDistance))
{
    return FarDecisionInterval * SignificanceDecisionMultiplier;
}
return ChaseDecisionInterval * SignificanceDecisionMultiplier;
```

关键保护：攻击中或接近攻击范围时固定使用高频间隔，Significance 只放大低风险状态的间隔。

### 4.2 CharacterMovement 更新频率如何单独下发

玩法 Significance 把敌人分为 `Full / Reduced / Background`，然后只修改 Movement 组件自己的 Tick Interval：

```cpp
// fpstrueEnemyCharacter.cpp:291-320（压缩）
float MovementTickInterval = 0.0f; // 0 表示每帧
if (!IsAttacking() && bEnableMovementUpdateTiering)
{
    switch (SignificanceTier)
    {
    case EFPEnemySignificanceTier::Reduced:
        MovementTickInterval = MidRateMovementTickInterval; // 1/30 s
        break;
    case EFPEnemySignificanceTier::Background:
        MovementTickInterval = FarRateMovementTickInterval; // 0.05 s
        break;
    default:
        break;
    }
}
GetCharacterMovement()->SetComponentTickInterval(MovementTickInterval);
```

所以三个时钟不是一个东西：

- AI Timer：多久重新做一次离散决策；
- PathFollowing / CharacterMovement Tick：已有路径怎样逐帧跟随和移动；
- Significance Coordinator：多久重新计算资源档位。

它们共享档位结果，但不会全部等 0.25 秒后才一起执行。

### 4.3 MoveTo 去重、预算与失败退避

```cpp
// fpstrueEnemyAIController.cpp:396-434（压缩）
const bool bSameGoal =
    bCombatPriority == bLastMoveGoalWasCombatPriority &&
    FVector::DistSquared2D(GoalLocation, LastMoveGoal) <
        FMath::Square(PathRefreshDistance);

if (bSameGoal && bHasMoveGoal)
{
    return; // 已经沿同一目标的有效路径移动
}

if (bSameGoal && World->GetTimeSeconds() < NextMoveRetryTime)
{
    return; // 上次失败，尚未到重试时间
}

if (SurroundManager != nullptr &&
    !SurroundManager->TryConsumeMoveRequestBudget(bCombatPriority))
{
    return; // 保留旧路径，下轮再尝试，不急停
}

const EPathFollowingRequestResult::Type Result = MoveToLocation(
    GoalLocation, AcceptanceRadius,
    true, true, !bCombatPriority, false, nullptr, true);

LastMoveGoal = GoalLocation;
bHasMoveGoal = Result != EPathFollowingRequestResult::Failed;
NextMoveRetryTime = bHasMoveGoal
    ? 0.0f
    : World->GetTimeSeconds() + FailedMoveRetryDelay;
```

通俗解释：敌人手里已经有一张“去 A 点的路线图”。玩家只移动了几十厘米，就不要撕掉路线图重新算；只有目标变化足够大，才申请一张新路线图。即使本帧申请名额用完，也继续走旧路线，而不是突然站住。

---

## 5. Timer 与 Tick 的区别

| 对比 | Tick | Timer |
|---|---|---|
| 触发 | 启用时每帧进入 Tick 系统 | 到期后由 `FTimerManager` 在 GT 触发 |
| 时间参数 | 直接拿 `DeltaTime` | 指定延时/周期，精度受帧边界限制 |
| 调度 | 有 TickGroup、依赖、Interval | one-shot 或 looping；不创建新线程 |
| 适用 | 连续移动、相机、物理、每帧表现 | 冷却、低频轮询、延迟事件、稀疏 AI 决策 |
| 开销 | 每帧参与调度；函数可很轻也可很重 | Timer Manager 也要维护和检查，不是零成本 |

两个高频误区：

1. Timer 不是后台线程，它的回调默认仍在 Game Thread；
2. 把每帧 Tick 改成 0.01 秒 Timer，在 60 FPS 下仍接近每帧，而且大量 Timer 同时到期仍会形成尖峰。

本项目选择 Timer 的原因不是“Timer 天生比 Tick 快”，而是 AI 决策是离散任务，可以安全地按状态减少执行次数；连续的 CharacterMovement 仍由组件 Tick 完成。

---

## 6. 行为树、黑板、当前 FSM 和 Significance 的关系

### 6.1 最通俗的解释

- **行为树**：AI 的流程图，决定下一步做巡逻、追击、找掩体还是攻击；
- **黑板**：某一个 AI 的记事本，保存目标、最后看到的位置、是否受伤等事实；
- **Significance**：资源调度分数，决定谁值得更高频更新、阴影、RT 或独立动画；
- **SurroundManager**：全局交通管理员，决定谁占哪个位置、谁能攻击、本帧还能交几次路线请求。

所以 Significance 不是黑板。一个是“预算优先级”，一个是“AI 决策数据仓库”。

### 6.2 当前为什么没有必要强行引入行为树

当前单体决策只有 `Idle / Chase / Attack / Dead` 四个主要状态，复杂点其实在全局协调：攻击名额、20 个槽位、MoveTo 预算和共享玩家位置。行为树不会自动解决这些全局问题，仍然要调用 SurroundManager。

现在强行引入会产生两套真相：

- C++ `AIState` 一套；
- Blackboard Key、Decorator、Task 状态又一套。

还必须正确实现 Task Abort：如果攻击 Task 被打断，要关闭攻击窗口、恢复动画档位、清 Timer、归还攻击名额，否则更容易泄漏状态。

### 6.3 当前方案的局限

- 分支增加后，`UpdateAI` 会逐渐膨胀；
- 缺少行为树可视化、Decorator 和运行时调试；
- 不适合复杂的巡逻、听觉调查、丢失目标搜索、掩体、远近武器切换；
- 当前攻击名额先到先得，没有 Blackboard/Utility AI 那样显式候选排序和公平队列。

合理演进是：当敌人行为真的增加时，用行为树表达高层策略，但继续让 CombatComponent 管攻击事务，让 SurroundManager 管全局预算；不要为了简历关键词重写稳定的四状态逻辑。

---

## 7. Significance 算法和 UE 底层

### 7.1 项目 Gameplay Significance

每个敌人向 UE `USignificanceManager` 注册一次，不是每个敌人拥有一个 Manager：

```cpp
// fpstrueEnemyCharacter.cpp:189-210（压缩）
Manager->RegisterObject(
    this, TEXT("Enemy"),
    [](USignificanceManager::FManagedObjectInfo* Info,
       const FTransform& Viewpoint)
    {
        const auto* Enemy = Cast<AfpstrueEnemyCharacter>(Info->GetObject());
        if (!IsValid(Enemy) || Enemy->IsDead()) return 0.0f;

        const float Distance = FVector::Dist2D(
            Enemy->GetActorLocation(), Viewpoint.GetLocation());
        return 1.0f / (1.0f + Distance);
    },
    USignificanceManager::EPostSignificanceType::Sequential,
    [](auto* Info, float, float NewValue, bool bUnregister)
    {
        if (!bUnregister)
        {
            Cast<AfpstrueEnemyCharacter>(Info->GetObject())
                ->ApplySignificance(NewValue);
        }
    });
```

分数只是距离的单调映射；攻击中或已进入攻击范围再强制 Full，避免低频破坏近战响应。当前 Gameplay 分数没有血量、威胁、角色类型等权重，也不负责选 Top 8 攻击者。

### 7.2 二维距离

```text
d² = (x₂-x₁)² + (y₂-y₁)²
```

地面战斗判断通常用 `DistSquared2D`，忽略楼梯、小坡和角色中心高度差。只做阈值比较时用：

```cpp
FVector::DistSquared2D(A, B) <= FMath::Square(MaxDistance)
```

这样避免 `sqrt`。如果要生成 `1/(1+d)` 这类连续分数，才需要实际距离。

### 7.3 三维距离

```text
d² = dx² + dy² + dz²
```

飞行敌人、立体迷宫、楼层之间应使用 `DistSquared`。二维和三维都是 `O(1)`；三维只是多一次减法和乘加。`sqrt` 也仍是 `O(1)`，但常数更大。复杂度只描述规模增长，不代表所有 `O(1)` 一样快。

### 7.4 视锥近似

项目用角色包围球做保守视锥测试，代码在 `fpstrueEnemyCharacter.cpp:335-365`：

1. `ToEnemy = BoundsCenter - CameraLocation`；
2. 点乘相机 Forward/Right/Up，得到相机坐标中的前、横、纵距离；
3. 用 `tan(FOV/2)` 把横纵距离归一化；
4. 把包围球投影半径加到边界，避免身体中心刚出屏幕就错误判不可见；
5. 再用扩大 Margin 得到扩展视锥。

核心近似：

```cpp
const float X = HorizontalDistance / (ForwardDistance * TanHalfHorizontalFov);
const float Y = VerticalDistance   / (ForwardDistance * TanHalfVerticalFov);
const float Rx = BoundsRadius / (ForwardDistance * TanHalfHorizontalFov);
const float Ry = BoundsRadius / (ForwardDistance * TanHalfVerticalFov);

const bool bInFrustum =
    ForwardDistance + BoundsRadius > 0.0f &&
    FMath::Abs(X) <= 1.0f + Margin + Rx &&
    FMath::Abs(Y) <= 1.0f + Margin + Ry;
```

这是粗粒度预算判断，不是严格裁剪，也没有做遮挡查询。因此：

- 在视锥里不等于真正可见，可能被墙挡住；
- 屏占比是包围球半径近似，不是逐像素覆盖率；
- 优点是每个敌人常数成本低，不需要 GPU readback 或大量射线。

### 7.5 屏幕占比近似

物体投影大小与半径成正比、与前向距离和 `tan(FOV/2)` 成反比：

```text
projectedRadius ≈ sphereRadius / (forwardDistance * tan(FOV/2))
```

项目取横向和纵向投影半径的较大值，再除以“满分半径”并 Clamp 到 `[0,1]`。这可用于排序，但不能解释为真实屏幕像素百分比。

### 7.6 复杂度与可以继续优化的常数

项目 Render Coordinator 对 N 个敌人采样是 `O(N)`，之后稳定排序是 `O(N log N)`，预算下发是 `O(N)`。N=160 时通常合理。

当前每个敌人都会重复构造 `FRotationMatrix` 和计算 `tan(FOV/2)`。这些其实属于同一相机快照，可以预计算到 ViewContext；这是降低常数，不改变大 O。

### 7.7 UE 5.5 Significance Manager 源码机制

已核对本机 UE 5.5 源码：

`E:/program/ue554/UE_5.5/Engine/Plugins/Runtime/SignificanceManager/Source/SignificanceManager/Private/SignificanceManager.cpp`

关键过程：

1. `RegisterObject` 为对象创建 `FManagedObjectInfo`，放入对象表、平铺数组和按 Tag 分组数组（约 202-240 行）；
2. `Update` 复制当前对象数组，避免计算期间注册/注销破坏遍历（494-497 行）；
3. `ParallelFor` 并行调用每个对象的评分函数（499-507 行）；
4. `Sequential` 类型的 Post 回调在并行评分完成后顺序执行（509-513 行）；
5. 每个 Tag 的对象按分数 `StableSort`（518-520 行）；
6. 多 Viewpoint 时，默认降序策略取该对象对各观察点的最大重要性。

所以本项目选择 `Sequential` Post 的原因是：评分可以只读并行，下发 `SetComponentTickInterval`、LOD、阴影等 UObject 状态必须回到顺序阶段。需要注意，评分函数本身也应保持只读、线程安全，不能在里面修改 Actor。

这里还有一个值得主动说明的源码边界：当前项目的评分 Lambda 在 `ParallelFor` 中直接读取 `Enemy->GetActorLocation()` 和死亡状态。Manager 的 `Update` 从 GT 发起并等待并行任务结束，因此当前单机流程里通常能得到稳定快照，但 UE 并不保证所有 UObject API 都可在任意工作线程安全调用。若要进一步工程化，可以先在 GT 为所有敌人采集只含位置、死亡标志的 POD 快照，再让 Worker 只读取快照；或者在规模较小时直接采用串行自定义评分。

引擎 Manager 内部用 `TMap<TObjectPtr<UObject>, ...>` 保存注册对象，并通过 `AddReferencedObjects` 参与 GC 引用收集。换句话说，注册不是弱观察关系；项目必须在敌人死亡/退出世界时调用 `UnregisterObject`，不能只等弱指针自然失效。

引擎 Manager 的总体复杂度约为：

```text
O(N * V + Σ Ntag log Ntag)
```

其中 V 是观察点数量。本项目通常只有一个玩家观察点。

---

## 8. 四个预算是否冲突

当前默认值：

- 8 个并发攻击名额；
- 8 个内环 + 12 个外环槽位；
- 每帧最多 8 个新 MoveTo 请求；
- 其中 2 个额度为槽位/战斗优先请求预留。

它们不直接冲突，因为约束的资源和时间尺度不同：

| 机制 | 约束对象 | 生命周期 |
|---|---|---|
| 20 个槽位 | 空间目标 | 敌人长期持有，直到离开/死亡/释放 |
| 8 个攻击名额 | 攻击事务 | 一次攻击期间持有 |
| 8 个 MoveTo | 新路径提交 | 每帧清零 |
| 2 个预留 | 8 个中的优先额度 | 同一帧内有效，不是额外 2 个 |

预算实现：

```cpp
// fpstrueSurroundManager.cpp:282-298（压缩）
if (MoveRequestBudgetFrame != GFrameCounter)
{
    MoveRequestBudgetFrame = GFrameCounter;
    MoveRequestsConsumedThisFrame = 0;
}

const int32 Total = 8;
const int32 Reserved = 2;
const int32 Limit = bCombatPriority ? Total : Total - Reserved;
if (MoveRequestsConsumedThisFrame >= Limit) return false;
++MoveRequestsConsumedThisFrame;
return true;
```

但存在四个设计边界：

1. 8 个攻击者与 8 个内环只是数值碰巧相同，代码没有强制“只有内环才能攻击”；
2. 攻击名额目前是先申请先得，不是 Significance 排序后的 Top 8；
3. 只有 20 个槽位，更多敌人会退化为追共享目标，仍可能在外围拥堵；
4. `bCombatPriority` 当前也用于槽位接近请求，名字更准确地说是“槽位/战斗优先”，不完全等价于“已获得攻击权限”。

所以没有死锁式冲突，但参数并非理论最优，仍需要玩法观感和消融扫描。

---

## 9. TSet、哈希、UE 容器、弱指针和 GC

### 9.1 为什么 ActiveAttackers 是这个类型

```cpp
TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> ActiveAttackers;
```

它同时表达三个需求：

- `TSet`：同一敌人不能重复占两个名额；
- 哈希查找：平均 `O(1)` 判断、加入和移除；
- `TWeakObjectPtr`：Manager 不拥有敌人的生命期，敌人销毁后引用可失效。

如果用 `TArray`，每次 `Contains/Remove` 是 `O(N)`；但这里最多 8 个攻击者，实际上 `TArray` 也可能因连续内存和更低常数更快。选择 `TSet` 主要是语义清晰和自动唯一，不能机械地说哈希容器一定更快。

### 9.2 TArray / TSet / TMap

| 容器 | 内存/顺序 | 查找 | 适用 |
|---|---|---|---|
| `TArray<T>` | 连续、顺序稳定、Cache 友好 | 索引 O(1)，按值 O(N) | 高频遍历、小规模数据、需要顺序 |
| `TSet<T>` | 哈希集合、无稳定顺序、元素唯一 | 平均 O(1)，最坏 O(N) | 唯一成员与频繁 Contains |
| `TMap<K,V>` | 哈希键值表、无稳定顺序 | 平均 O(1)，最坏 O(N) | 键到值映射，如敌人到槽位 |

哈希过程是：计算 Hash -> 定位桶 -> 桶内用 `==` 确认。发生碰撞、哈希差或扩容重哈希时，常数会增大；极端情况下所有键落在一起就是 O(N)。所以“平均 O(1)”绝不等于“最坏 O(1)”。

### 9.3 TWeakObjectPtr

弱指针大致保存 UObject 的对象索引和序列号，不阻止 GC。对象销毁或对象槽位被新对象复用时，序列号能帮助它判断旧引用失效。

```cpp
TWeakObjectPtr<AfpstrueCharacter> Character;

if (AfpstrueCharacter* Owner = Character.Get())
{
    // 只在当前 GT 调用栈临时使用这个裸指针，不跨帧保存。
}
```

弱指针不会自动从 `TSet/TMap` 中删除，所以项目在预算满或生命周期结束时仍要清理无效条目。

### 9.4 TObjectPtr 与裸指针

- `TObjectPtr<T>`：用于 UObject 成员的强引用，通常与 `UPROPERTY` 搭配，使 GC、序列化、编辑器和引用追踪看得见；
- `TWeakObjectPtr<T>`：观察关系，不拥有对象；
- `T*` 裸指针：不表达所有权，适合函数参数、返回值或当前调用栈的临时访问，跨帧保存容易悬空。

`nullptr` 只说明地址是否为零，不能证明某个 UObject 尚未 Pending Kill。跨帧 UObject 访问应使用 `IsValid` 或弱指针解析。

### 9.5 UObject GC 与悬空指针

UE 对 UObject 主要采用可达性标记/清扫：从 Root、反射可见的强引用、`AddReferencedObjects` 等入口开始标记；不可达对象后续进入销毁与回收。

Actor 通常调用 `Destroy()`，先触发 `EndPlay/Destroyed` 并退出世界，C++ 内存不是当场就能随意复用。游戏清理应放在 `EndPlay`，不要等 C++ 析构函数。

悬空指针是“地址还在，但那个对象已经销毁或内存已复用”。普通 C++ 裸指针无法自动发现；`TWeakObjectPtr` 能通过 UE 对象表和序列号发现失效。

---

## 10. 为什么预算按人数，而不是直接按毫秒/显存

这里有三种不同的“预算”：

- 攻击名额是玩法并发限制，本质就应该按事务/人数；
- MoveTo 数量是路径提交 CPU 工作的廉价代理；
- 阴影、RT 人数是渲染成本的代理，不是真实 GPU 毫秒预算。

当前敌人模型基本同质，按人数有四个优点：便宜、稳定、可预测、容易验证。它的缺点是默认每个敌人成本近似相同；以后混入 Boss、精英、不同骨骼和材质后就不准确。

可扩展为加权 Token：

```cpp
int32 GetShadowCostUnits(const FEnemyRenderDesc& Enemy)
{
    if (Enemy.bBoss)  return 4;
    if (Enemy.bElite) return 2;
    return 1;
}
```

也可以基于历史移动平均做自适应毫秒预算，但逐对象计时会引入采样开销、噪声和档位振荡。对当前 Demo，先以人数建立稳定证据，比伪精确的动态毫秒预算更合理。

---

## 11. 武器系统怎样数据驱动，如何扩展手榴弹

### 11.1 当前实现

当前武器以组件默认属性配置：RPM、Magazine、Reserve、Damage、TraceDistance、Spread、Recoil 和 Reload 时间。射击顺序为：

```text
Character 输入
 -> WeaponComponent::StartFire
 -> CanFire + 射速时间戳二次校验
 -> TryConsumeAmmo
 -> 射线/部位伤害
 -> 后坐力
 -> 委托驱动表现与 HUD
```

换弹事务：

```text
RequestReload
 -> ActionState=Reloading
 -> 动画 Notify 调 CommitReload
 -> bReloadAmmoCommitted 防重复搬弹
 -> FinishReload 恢复 Ready
 -> 若 Notify 丢失，由 Fail-safe Timer 调 FinishReload
```

当前局限必须诚实说明：它没有 Reload Sequence/Generation。`bReloadAmmoCommitted` 能挡住同一轮重复 Notify，但如果旧动画的迟到 Notify 恰好落入下一轮 Reloading 状态，理论上可能提交新一轮换弹。更严格实现应给每轮事务分配序号，并让 Notify 携带或校验序号。

### 11.2 推荐的数据驱动拆分（未来扩展，不是当前已实现）

```cpp
UENUM(BlueprintType)
enum class EWeaponExecutionType : uint8
{
    Hitscan,
    Projectile,
    Throwable
};

UCLASS(BlueprintType)
class UWeaponDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditDefaultsOnly) FName WeaponId;
    UPROPERTY(EditDefaultsOnly) EWeaponExecutionType ExecutionType;
    UPROPERTY(EditDefaultsOnly) TSoftObjectPtr<USkeletalMesh> Mesh;
    UPROPERTY(EditDefaultsOnly) int32 MagazineSize = 30;
    UPROPERTY(EditDefaultsOnly) float Damage = 20.0f;
    UPROPERTY(EditDefaultsOnly) float RoundsPerMinute = 600.0f;
    UPROPERTY(EditDefaultsOnly) TSubclassOf<AActor> ProjectileClass;
};
```

静态配置放 `UPrimaryDataAsset`；运行时实例只保存弹药、冷却、动作状态。枪械执行使用策略对象或小型子类，不用一个巨型 `switch` 塞进 WeaponComponent。

### 11.3 手榴弹完整链路（扩展示例）

```text
输入按下 -> 进入 Prepare/Cooking -> 播放拉栓动画
输入释放/Notify -> 服务器或单机权威端生成 GrenadeProjectile
 -> ProjectileMovement 负责飞行
 -> SphereCollision 负责碰撞/反弹
 -> Fuse Timer 到期
 -> bExploded 幂等保护
 -> ApplyRadialDamage
 -> 广播爆炸 FX/SFX
 -> Destroy
```

核心边界：

- 数量只扣一次；
- Cooking 时死亡/切武器要明确取消还是掉落；
- 爆炸必须幂等；
- Projectile 的 Owner/Instigator 正确，避免误伤判定错误；
- 联机时由服务器 Spawn 和结算伤害，客户端只预测投掷表现。

---

## 12. 敌人寻路的完整过程

### 12.1 项目层

```text
AI Decision Timer 到期
 -> 计算距离/攻击范围
 -> 读稳定槽位或共享玩家位置
 -> MoveToGoal 做目标阈值去重
 -> SurroundManager 检查本帧提交额度
 -> AAIController::MoveToLocation
```

### 12.2 UE 5.5 引擎层

已核对本机源码：

- `AIModule/Private/AIController.cpp:586`：`MoveToLocation`；
- `AIController.cpp:725-737`：构建查询、查路、发请求；
- `AIController.cpp:760-765`：把路径交给 `PathFollowingComponent`；
- `Navigation/PathFollowingComponent.cpp:352`：`RequestMove`；
- `PathFollowingComponent.cpp:676-689`：组件 Tick 跟随路径；
- `PathFollowingComponent.cpp:1088-1131`：计算当前路径段并调用 `RequestPathMove/RequestDirectMove`；
- `Engine/Private/Components/CharacterMovementComponent.cpp:1576`：Movement Tick；
- `CharacterMovementComponent.cpp:3822-3922`：消费请求、加速/制动并进入移动物理。

完整解释：

1. `MoveToLocation` 把目标、AcceptanceRadius、是否允许部分路径等封装成 `FAIMoveRequest`；
2. NavigationSystem 根据 NavData 和 QueryFilter 找一条 `FNavPath`；
3. PathFollowingComponent 持有请求和路径，决定当前追哪一个路径段；
4. 每次 PathFollowing Tick 计算期望方向/速度；
5. CharacterMovement 接收移动请求，计算加速度、制动、地面移动和碰撞，最终修改 Actor Transform；
6. 到达、失败、路径无效或被新请求打断时，PathFollowing 返回结果。

所以频繁 `MoveTo` 的问题不只是一个函数调用：它可能中止旧请求、重新投影目标、重新查询路径并重建跟随状态。项目把去重和预算放在 `MoveToLocation` 之前，避免无意义地进入引擎链。

局限是 `MoveToLocation` 当前走同步查路路径，密集同帧提交会产生尖峰；预算能摊开提交，但没有降低所有路径的稳态跟随成本。

---

## 13. 玩家位置怎样共享，是否不如 Blackboard

GameMode 把唯一玩家目标注入 SurroundManager。Manager 每 0.25 秒检查一次，玩家二维位移超过 200uu 才更新快照并批量重投影槽位：

```cpp
// fpstrueSurroundManager.cpp:76-95
const FVector Current = TargetCharacter->GetActorLocation();
const bool bMovedEnough = FVector::DistSquared2D(
    Current, CachedTargetLocation) >= FMath::Square(SharedTargetMoveThreshold);

if (!bForce && bHasSharedTargetSnapshot && !bMovedEnough)
{
    return;
}

CachedTargetLocation = Current;
bHasSharedTargetSnapshot = true;
RebuildProjectedSlotCache();
```

每个 Controller 只读同一个值：

```cpp
// fpstrueEnemyAIController.cpp:361-371
FVector SharedGoal;
if (SurroundManager->GetSharedTargetSnapshot(SharedGoal))
{
    MoveToGoal(SharedGoal, PursuitAcceptanceRadius, false);
    return;
}
MoveToGoal(TargetCharacter->GetActorLocation(), MoveAcceptanceRadius, false);
```

这不是“性能不及 Blackboard”。Blackboard 默认属于每个 AIController，各自保存一份 Key；如果 160 个 AI 的 Service 都写 PlayerLocation，仍然是 160 份更新。当前 Manager 是一次读取、一次批量投影、N 个轻量读取，更适合全局共享数据和预算。

Blackboard 的优势是可视化、Decorator/Service/Task 集成和行为树调试，而不是天然更快。

当前共享方案的边界：

- 快照可能落后 0.25 秒，并且小于 200uu 的移动不会触发重投影；
- 目标高速移动时旧路径会短暂滞后；
- 只有一个玩家目标；
- 20 个槽位每次刷新最多做约 40 次 NavMesh 投影，可能形成批量小尖峰；
- 近战范围判断仍读实时玩家位置，避免缓存误差破坏攻击正确性。

---

## 14. Animation Sharing 共享了什么

### 14.1 项目层资格

项目只允许以下敌人加入共享：非 Full Render、非攻击/近期战斗保护、非死亡、非布娃娃。攻击时退出共享，继续用自己的 Montage、Notify 和 Socket。

状态处理器把现有 AI 状态和实际速度映射为两种共享姿态：

```cpp
// fpstrueEnemyAnimationSharingCoordinator.cpp:31-49（压缩）
const EFPEnemyAIState AIState = EnemyController
    ? EnemyController->GetAIState()
    : EFPEnemyAIState::Idle;

const bool bMoving =
    AIState == EFPEnemyAIState::Chase &&
    InActor->GetVelocity().SizeSquared2D() > FMath::Square(10.0f);

OutState = static_cast<int32>(
    bMoving ? EFPEnemyAIState::Chase : EFPEnemyAIState::Idle);
```

运行时 Setup 创建 Idle/Moving 各 4 个随机 Leader（默认值），Follower 在这些 Leader 之间复用姿态。

### 14.2 插件真正共享的内容

共享的是“某个动画状态的骨骼姿态计算结果”。插件内部让多个 Follower Mesh 跟随一个 Leader Pose，因此不用每个敌人都独立解压、混合并评估相同 Idle/Run 动画。

没有共享：

- Actor Transform；
- AIController 与 AIState 所有权；
- CharacterMovement、碰撞和 NavPath；
- Health/CombatComponent；
- 攻击 Montage、Notify、伤害和布娃娃。

### 14.3 为什么关闭 Sharing 后 Animation 反而更高

实验不是“关掉一个功能应该更快”，而是比较两种执行模型：

- 开启：约 60.6 个 Follower 复用少量 Leader 的姿态，Animation 约 0.818 ms；
- 关闭：Follower=0，约 80 个敌人各自独立评估，Animation 约 1.226 ms。

关闭插件虽然省掉 Manager 和状态映射开销，却恢复了更多重复动画评估，所以净成本更高。局部减少约 `0.408 ms`，三次方向一致，说明共享节省大于管理开销。

但这不是“所有动画共享都一定有效”。敌人状态越离散、Montage 越多、Leader 数越多，复用率越低；频繁进出共享也有切换成本。

---

## 15. FPS 代表什么

FPS 是每秒最终呈现的帧数。对稳定帧时间：

```text
FPS ≈ 1000 / FrameTime(ms)
```

- 30 FPS：33.33 ms；
- 60 FPS：16.67 ms；
- 120 FPS：8.33 ms。

但平均 FPS 会掩盖尖峰。10 帧 10ms 加 1 帧 100ms，平均值可能仍好看，玩家却能明显感到卡顿。因此性能结论应优先报告 FrameTime 的平均、P95、P99、1% Low，并说明是否受 VSync、帧率上限和编辑器影响。

UE 的 GT、RT、RHI 和 GPU 是流水并行的，整帧通常受最慢阶段及等待链限制，不是简单 `GT+RT+RHI+GPU`。所以 `GT=7.71 ms` 只说明 GT 在该样本中具备 60 FPS 预算余量，不代表整体一定达到约 129 FPS。

---

## 16. 状态约束、边界和幂等保护

### 16.1 玩家动作

- 死亡、换弹、瞄准时不能开始冲刺；
- 开始瞄准会取消冲刺；
- 开始换弹先取消瞄准和冲刺；
- 死亡统一停止开火、取消换弹/后坐力恢复、禁用移动；
- 角色和 HealthComponent 都有死亡幂等保护，防止多次表现和资源清理。

### 16.2 武器

- `CanFire`：Owner 有效且未死、武器非 Disabled、Controller 存在、非 Reloading、弹匣有弹；
- `CanReload`：Owner 有效、非 Reloading、弹匣未满、备弹大于 0；
- 射击既受 Timer 周期约束，又用 `LastAcceptedShotTimeSeconds` 二次检查射速；
- 扣弹发生在射线和伤害前，一次请求最多消费一次；
- Reload Commit 用布尔值防重复 Notify，Fail-safe Timer 处理 Notify 丢失；
- 当前缺少跨换弹事务序列号，是已知边界。

### 16.3 敌人攻击

`CanStartAttack` 同时要求：目标存在且未死、敌人未死、当前不在攻击、距离满足、冷却完成。

攻击开始顺序：

```text
先建立 bIsAttacking 和单次命中状态
 -> 提高动画/LOD优先级
 -> 停止当前移动
 -> 启动失败保护 Timer
 -> 通知蓝图播放攻击动画
```

攻击窗口由 AnimNotifyState 控制。窗口内保存刀刃 Base/Tip 上一帧和当前帧位置，插值 2~8 个点并 Sweep 连续轨迹。只接受真正的玩家目标；`bHitTargetThisAttack` 保证一次攻击只扣一次血。Notify 结束和 Fail-safe Timer 都汇入同一个幂等 `FinishAttack`，在那里关闭窗口并归还攻击名额。

### 16.4 敌人死亡

死亡路径必须回收：Animation Sharing、Significance 注册、攻击窗口、攻击名额、AI Timer、PathFollowing、槽位、Movement 和 Capsule，然后才切 Ragdoll、发死亡事件并设置 LifeSpan。任何一处漏清都可能留下幽灵攻击者或槽位占用。

---

## 17. 阴影、骨骼 RT、BLAS、Render/RHI 怎样继续优化

### 17.1 当前已经证明的部分

80 敌人三次重复结果：

| 策略 | 开启 | 关闭 | 局部变化 |
|---|---:|---:|---:|
| 阴影参与限制 | 5 个投影敌人，ShadowDepths 1.052 ms | 80 个，1.773 ms | -0.721 ms |
| 骨骼 RT 参与限制 | 12 个 RT Visible，Skinned BLAS 0.199 ms | 80 个，0.495 ms | -0.296 ms |

这证明“限制敌人参与者”有效。尚未证明：

- Shadow Top 5 是最佳画质/性能点；
- 独立 RT Top 12 预算有效，因为当前 RT 候选先要求进入 Full，二者上限同为 12，已有样本 `RayTracingBudgetRejected=0`；
- 所有剩余 RT 成本都来自场景建筑。

### 17.2 BLAS 是什么

BLAS 是 Bottom-Level Acceleration Structure，保存单个几何对象的光追空间加速结构。静态网格可以长期复用；骨骼网格顶点随动画变化，通常需要更新动态几何并重建/更新 BLAS，因此大量敌人会出现 `RayTracingDynamicGeometryUpdate`、`SkinnedGeometryBuildBLAS` 等链路。

### 17.3 下一步实验，不先猜答案

1. 固定相机和画质，测 0/80/160 敌人，判断动态骨骼 RT 随敌人数的斜率；
2. 环境资源按建筑、附件、植被、灯光分组消融，确认场景贡献；
3. 让 Full 候选大于 12，再扫 RT 8/12/不限，并要求 `RayTracingBudgetRejected>0`；
4. 扫 Shadow 5/8/12/不限，记录画质截图、ShadowDepths、Shadow Draw Calls、GPU P95；
5. 检查 Skeletal Mesh 三角形、Section/材质槽、RT LOD 和是否真的需要进反射/阴影；
6. 检查环境对象 Mobility、WPO、动态变换和 VSM 缓存失效；静态建筑不应该因为“看起来是建筑”就自动断言为动态成本；
7. 对重复且无需独立交互的静态附件再评估 ISM/HISM；整栋建筑不要盲目合并，以免降低剔除粒度。

可用的质量降级手段：远处敌人无实时阴影或用 Capsule/Blob 替代；只让关键敌人进入 RT；降低远处骨骼/几何复杂度和材质 Section；减少不必要 Movable、WPO、材质 Pass 和 Scene Proxy 更新。

---

## 18. 内存优化：当前做了什么，下一步怎么做

当前没有完整的 Memory Trace 或消融，因此不能在简历中声称“内存优化取得明确收益”。已有的只是工程卫生：

- Manager 用弱引用保存非拥有关系；
- 敌人死亡后 LifeSpan 回收，避免尸体无限累积；
- 候选数组会 Reserve/复用部分容量；
- 动画资产以 `TSoftObjectPtr` 配置，但 Coordinator 启动时 `LoadSynchronous`，只能避免构造时硬加载，不等于异步流送；
- Animation Sharing 的主要已证收益是 CPU Animation 时间，不是内存。

正式内存分析流程：

1. 用 Unreal Insights Memory Trace、`memreport -full`、`stat memory`、`stat streaming` 和 LLM 建基线；
2. 记录 20/80/160 敌人的稳态快照，以及反复生成/死亡后的保留量；
3. 检查 UObject 数是否只升不降，定位 Timer、委托、强引用或缓存泄漏；
4. 检查 Texture Mip、Skeletal Mesh LOD、Animation Compression、Niagara、Decal 和 Dynamic Material Instance；
5. 检查每帧临时 `TArray/TMap` 分配，使用 `Reserve`、复用缓冲区；
6. 热循环优先使用连续 `TArray`，减少指针追逐，关注结构体大小、对齐和 Cache Line；
7. 对大资源使用 AssetManager/Soft Reference 和异步加载，不在战斗尖峰 `LoadSynchronous`。

对象池不是默认答案：Actor/Component 状态很复杂，必须证明 Spawn/Destroy 是瓶颈，并完整重置 Timer、委托、碰撞、AI、动画和网络状态后再使用。

---

## 19. UE 基础知识补全

### 19.1 UObject、Actor、Component 生命周期

#### UObject

构造/默认值 -> `PostInitProperties` -> 加载对象可能 `PostLoad` -> 使用 -> `BeginDestroy` -> `FinishDestroy` -> 内存回收。

构造函数阶段不应假设 World、Controller 或其他 Actor 已存在。`NewObject` 创建 UObject，不能对 UObject 使用普通 `delete`。

#### Actor

大致为：

```text
Constructor
 -> OnConstruction（编辑器中可能多次）
 -> Pre/Initialize/PostInitializeComponents
 -> BeginPlay
 -> Tick/Timer/Event
 -> EndPlay
 -> Destroyed
 -> 后续 GC
```

Timer、委托、攻击名额、槽位和外部注册应在 `EndPlay` 释放，因为析构太晚，也不适合访问 World。

#### ActorComponent

由 Actor 创建/拥有，经历注册、InitializeComponent、BeginPlay、EndPlay。组件适合封装可复用能力，但必须明确谁拥有状态、谁负责绑定和解绑。

### 19.2 Timer、委托和反射

- Timer：`UWorld::GetTimerManager()` 管理，回调仍在 GT；保存 `FTimerHandle` 便于取消；
- 动态委托：通过 UFUNCTION/反射支持蓝图与序列化，适合低频通知；
- 原生委托：纯 C++、开销更低，不暴露蓝图；
- `UCLASS/USTRUCT/UENUM/UFUNCTION/UPROPERTY`：由 UHT 生成反射元数据，支持 GC、编辑器、蓝图、序列化、RPC；
- 反射不是为了“实时调参”才存在，它也是 UE 对象系统和工具链的基础。

### 19.3 AIController、PathFollowing、CharacterMovement

- AIController：拥有 Pawn 的决策和 Move 请求入口；
- NavigationSystem/NavData：计算可行路径；
- PathFollowingComponent：保存路径、判断当前段和到达条件；
- CharacterMovement：把期望输入变成速度、加速度、碰撞和最终位移。

关闭 AIController 的决策 Timer，不代表已有 PathFollowing/Movement 停止；关闭 CharacterMovement Tick，则即便路径存在，角色也无法正常消费移动请求。

### 19.4 动画线程与 GT 同步点

动画图可以并行评估，但 Gameplay Notify、UObject 修改和很多组件交互仍回到 GT。当 GT 需要最新姿态用于 Socket、渲染提交或物理时，可能等待 Worker 完成，这就是 Insights 中的 `Wait`/`CompleteParallelAnimationEvaluation` 类同步成本。优化可以减少任务数量、复杂度或提高复用率，但不能通过“再开一个线程”消除依赖。

### 19.5 Render Thread、RHI Thread、GPU

- GT：世界状态、AI、动画调度、组件和渲染状态提交；
- RT：把场景状态变成渲染图、可见列表、Draw/Dispatch；
- RHI：将 UE 渲染命令转译为 D3D12/Vulkan 等 API 命令并提交；
- GPU：真正执行图形、计算和光追工作。

GT 高通常优化逻辑数量与频率；RT/RHI 高看 Primitive、Section、Draw 提交、动态几何和同步；GPU 高看分辨率、材质、阴影、后处理、带宽和 RT Shader。必须先定位线程，不要用同一套药。

### 19.6 容器、内存布局和 Cache

CPU 读取内存以 Cache Line 为单位。`TArray` 连续遍历通常预取友好；`TSet/TMap` 的哈希和稀疏节点访问可能产生更多 Cache Miss。对于 8 个元素，线性扫描的 TArray 可能比哈希更快；对于需要唯一语义和频繁增删的集合，TSet 更清晰。选容器要看访问模式、规模和测量，不只背复杂度。

---

## 20. 网络项目相关八股

> 当前 `main` 是单机 FPS 源码，Co-op 项目代码不在这个工作区，因此下面是 UE 正确机制，不把示例冒充成本分支的源码。

### 20.1 服务器权威

客户端发送“我想拾取/交互/开火”的意图；服务器校验所有权、距离、当前状态和是否已结算，然后修改唯一权威状态。客户端不能直接决定“我已经获得钥匙”或“敌人已经死亡”。

### 20.2 属性复制与 OnRep

```cpp
UPROPERTY(ReplicatedUsing=OnRep_KeyState)
EKeyState KeyState;

UFUNCTION()
void OnRep_KeyState(); // 客户端根据新状态更新表现
```

服务器状态改变后通过网络复制到相关客户端；`OnRep` 适合状态驱动表现。服务器自身通常要直接调用相同表现更新函数，因为服务器设置属性时不一定走客户端式 OnRep 路径。

### 20.3 RPC

- `Server` RPC：客户端向自己拥有对象的服务器端发送意图；
- `Client` RPC：服务器通知某个拥有者；
- `NetMulticast`：服务器向相关客户端广播瞬时表现，不能替代持久状态复制；
- `Reliable` 只给少量、必须到达且不能丢的消息；高频输入/表现滥用 Reliable 会堵塞通道。

服务器 RPC 必须再次校验，RPC 不是信任边界。

### 20.4 CharacterMovement 网络同步

本地拥有角色是 Autonomous Proxy，会预测移动并发送输入/Move；服务器权威模拟并校正；其他客户端看到 Simulated Proxy 插值。不要再用每帧可靠 RPC 手动同步角色位置。

### 20.5 GameMode、GameState、PlayerState

- GameMode：只存在服务器，管理规则和结算；
- GameState：复制给所有客户端的全局比赛状态；
- PlayerState：跨 Pawn、面向所有人的玩家状态；
- PlayerController：服务器与拥有客户端存在，适合所有者通信；
- Session/OnlineSubsystem：负责找房、建房、加入，不负责进房后的 Gameplay 属性复制。

玩家退出时，服务器要清除区域人数、钥匙占用、委托、弱/强引用和等待队列，不能只依赖客户端 UI 消失。

---

## 21. 结论与证据边界

当前证据支持：

- 我通过调用链和破坏性消融确认 Movement 与动画是早期 GT 主要成本类别；
- 正式方案没有关闭玩法，而是减少决策/移动/寻路提交频率，并用 Animation Sharing 复用普通敌人姿态；
- Movement 分级和 Animation Sharing 已有三次同向局部证据；
- 敌人阴影参与限制与骨骼 RT 参与限制分别降低了 ShadowDepths 与 Skinned BLAS；
- 当前 160 敌人样本 GT 平均约 7.71 ms，但整帧转为更受 Render/RHI 限制。

当前证据不支持：

- 所有参数已经调到最优；
- 槽位、攻击名额和 MoveTo 预算各自已有独立毫秒收益；
- LOD 和 Animation Tick 分级已经证明净收益；
- RT Top 12 已经独立生效；
- 剩余渲染成本确定全部来自建筑；
- 项目已经完成系统性内存优化。

最好的面试表达不是“我用了很多优化词”，而是：我能指出每个策略控制了哪个消费者、为什么不破坏正确性、证据能证明到哪一步、还有什么没有证明。
