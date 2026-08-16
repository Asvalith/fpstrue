# FPS 项目拷打问答

> 项目：`fpstrue_safe2`
> 引擎：Unreal Engine 5.5
> 定位：C++ 单机 PvE FPS 作品集项目
> 范围：以当前源码能够证明的高中频问题为主，不把网络、Co-op 或 GAS 描述成已实现功能。
> 文档身份：面试演练题库；架构事实以 [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md) 为准，性能数字以 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md) 为准。

## 1. 使用规则

### 1.1 证据等级

| 标记 | 含义 | 回答方式 |
| --- | --- | --- |
| 已实现 | 当前源码和工程中存在 | 可以讲调用链、类职责和运行结果 |
| 已验证 | 有固定条件下的日志、CSV 或内存数据 | 可以报告数据，同时说明测试条件 |
| 待验证 | 有代码或方案，但缺少同条件 A/B | 只能讲设计和验证计划 |
| 候选方案 | 尚未实现 | 使用“如果需求扩大，我会……” |
| 知识储备 | 当前项目没有使用 | 不使用“我的项目已经实现” |

### 1.2 项目事实边界

- 当前是单机项目，没有 RPC、属性复制、客户端预测和延迟补偿。
- 当前没有接入 Gameplay Ability System，`Build.cs` 也没有 GameplayAbilities 模块依赖。
- 当前敌人 FSM 是 `Idle/Chase/Attack/Dead`，没有 Patrol 和 Retreat 状态。
- 当前武器使用 Hitscan，项目保留了 Projectile 类型文件不代表主战斗链正在使用实体子弹。
- 当前没有对象池。对象池只能作为大量生成销毁被 Profile 证明为热点后的候选方案。
- 当前不能宣称 160 AI 稳定 60 FPS。最终矩阵中 160 敌人平均约 `35.83 FPS`，Game Thread 已成为明确瓶颈。
- 当前机器上的玩法容量按 40 个活跃敌人约 60 FPS 表述；80/160 是压力档。旧矩阵与当前版本条件不同，不能跨版本计算优化百分比。

### 1.3 回答结构

```text
先给结论
-> 讲当前调用链
-> 解释为什么这样选
-> 说明遇到的边界或 Bug
-> 给出数据、工具或复现方法
-> 最后再谈可扩展方案
```

## 2. 源码地图

| 系统 | 当前责任 | 主要文件 |
| --- | --- | --- |
| 玩家 | 输入、移动、瞄准、武器请求、死亡后的玩家收口 | `fpstrueCharacter.h/.cpp` |
| 武器 | 弹药、开火节奏、换弹事务、散布、Hitscan、后坐力 | `fpstrueWeaponComponent.h/.cpp` |
| 武器配置 | 容量、伤害、射速、散布、后坐力、换弹时间 | `fpstrueWeaponDataAsset.h` |
| 健康 | 扣血、Clamp、伤害事件、一次性死亡事件 | `fpstrueHealthComponent.h/.cpp` |
| 敌人实体 | 目标引用、攻击窗口、近战 Sweep、死亡收口 | `fpstrueEnemyCharacter.h/.cpp` |
| AI 决策 | Timer FSM、MoveTo、状态转换、资源释放 | `fpstrueEnemyAIController.h/.cpp` |
| 群体协调 | 双环槽位、攻击令牌、导航投影 | `fpstrueSurroundManager.h/.cpp` |
| 游戏规则 | 开始、波次、生成、存活数、倒计时、胜负结算 | `fpstrueGameMode.h/.cpp` |
| 蓝图表现 | Montage、受击、死亡、HUD、特效、音效 | C++ 事件对应的蓝图子类和 Widget |

## 3. 高频项目拷打

### Q1. 请用一分钟介绍这个项目

**标准回答：**

这是一个 UE 5.5 C++ 单机 PvE FPS。核心闭环包括第一人称移动与瞄准、武器拾取、自动射击与换弹、Hitscan 部位伤害、共享 HealthComponent、敌人近战、三波生成、90 秒倒计时和胜负结算。

架构上，玩家 Character 只接收输入并转发武器请求，WeaponComponent 持有弹药和武器事务，HealthComponent 统一处理伤害和死亡，EnemyAIController 使用 Timer 驱动的 `Idle/Chase/Attack/Dead` FSM，SurroundManager 通过双环槽位和攻击令牌限制敌人扎堆。C++ 负责规则和状态，蓝图负责 Montage、HUD 和特效表现。

项目的技术重点不是只把功能跑起来，而是处理攻击窗口漏检、重复伤害、换弹被动画打断、死亡和结算重复触发、群体 AI 扎堆以及性能误判。我对 10 到 160 个敌人做过固定场景采样。最终矩阵中 40 敌人约 `61.02 FPS`，80 敌人约 `55.20 FPS`，160 敌人约 `35.83 FPS`；160 敌人时 CharacterMovement 为 `6.188 ms`、Animation 为 `4.448 ms`。结合早期子系统采样中 Pathfinding 远低于这两项的结果，优化优先级放在移动和动画分级，而不是重写寻路。

**不要这样说：**

- “160 个敌人稳定 60 FPS。”数据不支持。
- “已经做了完整 GPU 优化。”目前有 GPU 基线、纹理治理和 VSM 单变量实验，但没有所有渲染 Pass 的逐项优化。
- “做了 Co-op 和 GAS。”当前工程没有这些实现。

### Q2. 你的项目架构如何划分，为什么弹药不放在 Character 里

**结论：** Character 表达“玩家想做什么”，WeaponComponent 决定“武器能不能做以及如何完成”。弹药属于武器运行时状态，所以不能由 Character 和 Weapon 各保存一份。

**详细回答：**

`AfpstrueCharacter` 负责 Enhanced Input、移动、瞄准、冲刺、玩家死亡和当前武器引用。按下开火时只调用 `EquippedWeaponComponent->StartFire()`，换弹时调用 `RequestReload()`。它可以向 UI 提供当前武器快照，但不拥有弹药规则。

`UfpstrueWeaponComponent` 负责 `CurrentAmmo/MagazineSize/ReserveAmmo`、`Ready/Firing/Reloading/Disabled` 状态、自动开火 Timer、射速约束、散布、射线和换弹提交。这样开火、UI 和动画都读取同一个事实来源。

`UfpstrueWeaponDataAsset` 保存配置，Component 保存运行时状态。DataAsset 适合复用和调参，但不应该存某一把枪当前还剩多少子弹。

**如果把弹药放回 Character，会出现的问题：**

- 换枪时不知道弹药跟玩家走还是跟武器走。
- Character 与 Weapon 可能分别扣弹，出现双扣或 UI 不一致。
- 新武器类型会继续膨胀 Character。
- 武器拾取、丢弃和复用难以维持状态边界。

### Q3. 从按下开火到敌人扣血，完整调用链是什么

**调用链：**

```text
FireAction Started
-> AfpstrueCharacter::StartWeaponFire
-> UfpstrueWeaponComponent::StartFire
-> CanFire
-> ActionState = Firing
-> Fire
-> 射速检查 + TryConsumeAmmo
-> FireLineTrace / FireSingleLineTrace
-> LineTraceSingleByChannel(ECC_Visibility)
-> 根据 BoneName 选择身体或头部伤害
-> UGameplayStatics::ApplyPointDamage
-> EnemyCharacter::TakeDamage / AActor damage delegate
-> HealthComponent::HandleOwnerTakeAnyDamage
-> ApplyDamageInternal
-> OnDamageReceived / OnHealthChanged / OnDeath
-> EnemyCharacter 受击表现或死亡收口
```

**关键边界：**

- `StartFire()` 不是唯一检查点。自动武器的每一次 `Fire()` 都重新检查状态和弹药。
- `LastAcceptedShotTimeSeconds` 限制最小射击间隔，避免同帧重复输入导致超射速。
- 射线忽略玩家和武器 Owner，减少自击。
- 当前伤害代码仍然 Cast 到 `AfpstrueEnemyCharacter`，这是待优化边界。进一步可使用 Damageable 接口或命中区域组件，使武器不依赖具体敌人类。

### Q4. 为什么使用 Hitscan，而不是 Projectile

**结论：** 当前步枪和霰弹枪强调即时反馈，地图交战距离和玩法规模不需要表现明显飞行时间，因此 Hitscan 是更简单、稳定且符合手感的选择。

**选型理由：**

- 一次 Scene Query 即可得到命中结果，逻辑链短。
- 不需要为每颗子弹创建 Actor、维护生命周期和逐帧运动。
- 不存在高速 Projectile 一帧跨过薄碰撞体的离散穿透问题。
- 单机项目不需要同步大量 Projectile 状态。

**代价：**

- 没有真实飞行时间和弹道下坠。
- 目标无法通过观察飞行轨迹躲避。
- 当前从相机发射，枪口贴墙时可能出现准星能命中但枪口实际被墙挡住的问题。

**枪口遮挡如何扩展：**

先从相机射线获得期望命中点，再从枪口向该点做第二次射线。如果枪口射线先碰到墙，以墙为最终命中点。这样提高空间一致性，但近距离会产生准星和枪口结果差异，需要配合武器抬起或受阻提示。

### Q5. HealthComponent 是怎么设计的，为什么用 Component

**结论：** 玩家、敌人和测试靶都需要相同的生命规则，所以把健康做成不 Tick 的 ActorComponent，Character 只处理死亡后属于自身的行为。

**核心流程：**

1. `BeginPlay()` 绑定 Owner 的 `OnTakeAnyDamage`。
2. `ApplyDamageInternal()` 拒绝非正伤害和已死亡对象。
3. 生命值 Clamp 到 `[0, MaxHealth]`。
4. 依次广播 `OnDamageReceived` 和 `OnHealthChanged`。
5. 首次到达零血时，通过 `bDeathBroadcast` 只广播一次 `OnDeath`。
6. `EndPlay()` 解绑伤害委托。

**为什么死亡行为不全部放在 HealthComponent：**

HealthComponent 只知道健康事实，不应该知道敌人要停 AI、玩家要禁用输入、敌人要变布娃娃或 GameMode 要判负。玩家和敌人订阅 `OnDeath`，分别执行自己的收口逻辑，GameMode 再监听玩家死亡报告处理全局结果。

**进一步扩展护甲：**

可以在进入 HealthComponent 前建立 Damage Pipeline，例如 `RawDamage -> Armor/Resistance -> FinalDamage -> Health`，并使用 DamageType、接口或独立 DefenseComponent。不要把武器、护甲和 UI 判断堆入 `ApplyDamageInternal()`。

### Q6. 如何保证死亡只发生一次

**结论：** 一次性终止状态不能只靠动画或蓝图判断，需要在每一层建立幂等保护。

**当前保护：**

- HealthComponent 使用 `bDeathBroadcast`，每次 ResetHealth 之间只广播一次死亡。
- Player Character 使用 `bDeathHandled`，重复回调直接返回。
- Enemy Character 使用 `bIsDead`，重复死亡不会再次停 AI、改碰撞或广播。
- GameMode 使用 `bGameEnded`，胜负结果只提交一次。
- WeaponComponent 在 Owner 死亡时清开火和换弹 Timer，把状态设为 `Disabled`。

**面试官继续问“为什么这么多层”：**

每一层保护的是不同副作用。HealthComponent 保护领域事件；Character 保护移动、武器和表现收口；GameMode 保护全局结算。不能因为底层通常只广播一次，就让上层重要副作用完全依赖这个假设。

### Q7. 换弹动画被打断后，为什么不能继续开枪

**结论：** Montage 是表现，WeaponComponent 的 ActionState 才是武器逻辑真相。动画被覆盖不能自动把 `Reloading` 改回 `Ready`。

**当前机制：**

- `RequestReload()` 先 `StopFire()`，再进入 `Reloading`。
- `CanFire()` 明确拒绝 `Reloading` 和 `Disabled`。
- `CommitReload()` 使用 `bReloadAmmoCommitted` 保证一次换弹只加一次弹药。
- `ActiveReloadSequence` 标识当前换弹事务，旧 Timer 即使晚到也会因序列号不一致而失效。
- `CancelReload()`、Owner Death 和 EndPlay 都清理 Timer 并使旧事务失效。

**为什么还保留 Timer：**

Notify 可以提供准确的插匣时刻，但动画资产配置错误、Montage 被中断或没有发出结束事件时，逻辑不能永久卡住。Timer 是恢复兜底，二者最终都调用幂等的 Commit/Finish 接口。

**仍需验证：** 蓝图 Montage 的 Completed、Interrupted 和 Blend Out 必须分别接到正确的 Finish 或 Cancel 路径，并做“换弹中死亡、换枪、暂停、连续按 R”的回归测试。

### Q8. 敌人为什么使用 FSM，而不是行为树

**结论：** 当前敌人只有 `Idle/Chase/Attack/Dead` 四个明确状态，转换关系少，C++ FSM 更直接，也便于把决策频率和性能统计写进 Controller。

**职责划分：**

- EnemyAIController 决定当前应该 Idle、追击还是申请攻击。
- EnemyCharacter 负责移动实体、距离判断、Montage、攻击窗口和受伤死亡。
- SurroundManager 处理多个敌人之间的槽位与攻击资格。

**为什么用 Timer 而不是每帧决策：**

攻击附近使用约 `0.1 s`，普通追击约 `0.25 s`，远距离约 `0.5 s`，Idle 约 `1.0 s`。不同状态对响应速度要求不同，没必要让远距离敌人每帧重复计算。首次 Delay 还会随机错峰，减少同一帧集中决策。

**Timer 不等于多线程：** TimerManager 仍在 Game Thread 调度。它减少调用频率和峰值，不会把 UObject、NavSystem 或 Actor 操作自动移到后台线程。

**什么时候改行为树：** 当行为增加巡逻、调查声音、掩体、撤退、协作技能和可复用子树后，行为树或 StateTree 的可视化组合价值会提高。当前规模强行上行为树会增加资产和调试成本，收益有限。

### Q9. SurroundManager 包围系统如何设计

**问题来源：** 所有敌人直接 `MoveToActor(Player)` 会共享一个目标中心，结果是碰撞、路径和攻击都挤在玩家正前方。

**当前方案：**

- 以玩家为中心建立内环和外环，默认分别为 8 和 12 个槽位。
- 槽位保存环索引、角度、半径和 `TWeakObjectPtr` Occupant。
- `EnemyToSlot` 提供敌人到槽位的反向映射。
- 新敌人优先申请距离自己最近的可导航空槽。
- 内环空出后，可以把合适的外环敌人提升到内环。
- `ActiveAttackers` 是攻击令牌集合，`MaxConcurrentAttackers` 默认限制为 2。
- 敌人死亡、失去目标、停止 AI、令牌超时和游戏结束都必须释放相应资源。

**为什么槽位和令牌是两个概念：**

槽位解决“站在哪里”，令牌解决“谁现在可以攻击”。只做槽位仍可能让整个内环同时出手；只做令牌则仍会发生追击扎堆。

**复杂度与边界：**

当前槽位数很小，线性扫描足够。若扩展到数百槽位，应减少重复导航投影、缓存候选位置或使用空间结构。不能为了理论复杂度提前引入难以维护的数据结构。

### Q10. GameMode 如何管理波次、倒计时和胜负

**当前流程：**

```text
StartGameMode
-> 校验 EnemyClass、TargetPoint、玩家和 HealthComponent
-> 创建 SurroundManager
-> 初始化 CurrentWave / AliveEnemyCount / RemainingTime
-> 绑定玩家死亡
-> 启动 1 秒倒计时 Timer
-> StartNextWave
-> 按 WaveInterval 启动后续波次
```

敌人生成成功后加入 `RegisteredEnemies`，同时绑定死亡和 Destroyed 事件。注销使用 Set 的 Remove 返回值保证重复事件不会重复减少存活数。

玩家在游戏运行中死亡会立即 `FinishGame(false)`；倒计时归零且玩家仍存活则 `FinishGame(true)`。结束时清理倒计时和波次 Timer、解绑玩家死亡、停止所有活动敌人并重置 SurroundManager。

**需要诚实说明的规则：**

- 当前波次按时间间隔推进，不是“清完一波才开始下一波”。如果策划要求清波触发，需要把推进条件改为 `AliveEnemyCount == 0`，并区分当前波敌人与累计存活数。
- 玩家死亡和倒计时归零若在同一帧竞争，当前首次进入 `FinishGame()` 的事件决定结果。更确定的做法是统一 `EvaluateGameResult()`，明确死亡优先或时间优先。

### Q11. 当前性能瓶颈是什么，怎么证明

**结论：** 160 AI 时主要瓶颈转到 Game Thread，规模相关热点首先是 CharacterMovement 和 Animation，不是 Pathfinding。

**最终矩阵的 160 敌人数据：**

| 项目 | 160 敌人数据 |
| --- | ---: |
| 平均 FPS | 35.83 |
| Frame Avg | 27.907 ms |
| Frame P95 | 32.459 ms |
| Game Thread | 27.899 ms |
| Render Thread | 14.182 ms |
| GPU | 14.205 ms |
| CharacterMovement | 6.188 ms |
| Animation | 4.448 ms |

**定位过程：**

1. 固定关卡、分辨率、VSync、敌人数和采样时间。
2. 用 `stat unit` 判断 Game、Draw、GPU 中最长的一侧。
3. 用 CSV Profiler 和 Unreal Insights 拆分 TickActors、CharacterMovement、Animation、Pathfinding 和 GPU。
4. 比较 10/20/40/80/160 敌人的增长趋势，而不是只看一个 FPS 数字。
5. 同条件至少运行三次，报告 Avg 和 P95。

**优化顺序：**

- AI 决策按状态和距离降频并错峰。
- CharacterMovement 使用距离分级更新，攻击时恢复高频。
- SkeletalMesh 开启 URO 和不可见动画降级，攻击窗口保留必要骨骼刷新。
- 死亡后停止 AI、Movement、碰撞和无用更新。
- 最后根据新的 Profile 决定是否继续处理材质、阴影和场景 Draw Call。

**诚实边界：** 最终矩阵证明的是当前容量和瓶颈迁移，不是某个优化项的净收益。当前没有同版本逐项关闭 LOD、URO 和 Movement 分级的全档 A/B，不能把旧矩阵和新矩阵直接相减。

### Q12. 这个项目最难的问题是什么

可以从两个方向回答，面试时选择与岗位最相关的一条。

**方向一，群体近战正确性：**

难点不只是让敌人追到玩家，而是同时解决站位、攻击并发、动画窗口、低帧率漏检、单次攻击重复扣血和死亡中断。最终把职责拆为 AIController 决策、SurroundManager 仲裁、EnemyCharacter 执行、AnimNotifyState 定义有效窗口，并用帧间 Sphere Sweep 与命中集合保证正确性。

**方向二，性能瓶颈误判：**

敌人多时直觉容易怪 NavMesh。早期子系统采样中 Pathfinding 约 `0.071 ms`，明显低于 CharacterMovement 与 Animation；最终矩阵又确认后两项随敌人数持续增长。真正困难的是固定测试条件、把帧时间拆到系统、接受直觉错误，并让优化顺序服从证据。

**回答重点：** 难点必须包含现象、错误假设、定位工具、最终方案、数据和仍未解决的边界，不能只说“这个系统代码很多”。

### Q13. 项目使用了哪些设计模式

**可以明确回答的模式：**

| 模式或思想 | 项目中的使用 | 价值 |
| --- | --- | --- |
| Component | HealthComponent、WeaponComponent、PickUpComponent | 组合可复用能力，缩小 Character |
| Observer | 动态多播委托通知伤害、死亡、弹药、波次和 UI | 规则层不直接依赖具体 Widget 或特效 |
| State | Weapon ActionState、Enemy AI FSM、GameMode 运行/结束状态 | 明确互斥和终止边界 |
| Manager/Coordinator | SurroundManager | 集中仲裁稀缺槽位和攻击令牌 |
| Data-Driven | WeaponDataAsset、WaveConfig | 把配置与运行时状态分离 |
| Command-like input forwarding | Character 把输入意图转给 Weapon | 输入拥有者与武器规则解耦 |
| Template Method / Blueprint hook | C++ 规则完成后调用 BlueprintImplementableEvent | C++ 保证规则，蓝图扩展表现 |

**不要强行套模式：** SurroundManager 是由 GameMode 创建和持有的 Actor，不是全局 Singleton；当前没有对象池，也没有完整 Strategy 工厂体系。面试中解释职责和代价比罗列模式名称更重要。

## 4. 中频项目拷打

### Q14. 随机弹道为什么使用 `sqrt(random)`

**结论：** 要在圆盘面积上均匀采样，半径不能直接线性随机，而应使用 `r = R * sqrt(U)`。

**原因：** 半径为 `r` 的圆面积是 `PI * r^2`。如果希望样本落入任意面积的概率与面积成正比，需要让 `r^2` 均匀，因此 `r` 应为均匀随机数的平方根。若直接使用 `r = R * U`，同样数量的样本被分配到越来越大的外环面积，视觉结果会偏向圆心。

**当前实现：**

```text
Radius = sqrt(FRand()) * DiskRadius
Angle = FRand() * 2PI
Offset = Right * Radius * cos(Angle) + Up * Radius * sin(Angle)
Direction = normalize(Forward + Offset)
```

WeaponComponent 在此基础上叠加：

- 腰射或瞄准基础散布。
- 连续射击次数乘以 SpreadStep。
- MaxContinuousSpread 上限。
- 普通 `StopFire()` 会立即清零连续射击计数，当前没有渐进恢复曲线。

**继续追问“这是严格圆锥均匀吗”：** 当前实现是准星切平面或垂直靶面面积均匀，不是严格的球面立体角均匀。严格圆锥采样应让 `cos(theta)` 在 `[cos(MaxAngle), 1]` 上均匀。当前角度只有几度，两者差异很小。

**继续追问“为什么不用 VRandCone”：** `VRandCone` 本身没有数学错误，UE 官方将其定义为圆锥内均匀随机单位向量。项目改成圆盘采样，是把设计目标从立体角均匀改为准星覆盖面积均匀。

**继续追问“游戏枪械一定要面积均匀吗”：** 不一定。中心偏置、环形散布、固定 Pattern、二维高斯和截断高斯都可能是有意设计。真实弹着群通常中心更密，不能说均匀采样天然更符合真实枪械。本项目保留当前实现，其他模型只作为条件变化题。

### Q15. 部位伤害怎么实现，有什么扩展问题

**当前实现：** Hitscan 获得 `FHitResult` 后读取 `BoneName`。`head` 和 `neck_01` 使用头部伤害，其余使用身体伤害，再通过 `ApplyPointDamage` 传递命中位置、方向和 Instigator。

**优点：** 实现简单，能够直接利用 SkeletalMesh 的命中骨骼，不需要为每个部位额外维护 Actor。

**问题：**

- 骨骼名称是硬编码字符串，换模型或改骨架后容易失效。
- 目前只有头和身体两档，无法自然表达手臂缴械、腿部减速等效果。
- WeaponComponent Cast 到具体 EnemyCharacter，形成了武器对敌人类型的依赖。

**扩展方案：**

定义 HitZone 配置，将 BoneName、区域标签、伤害倍率和附加效果映射到 DataAsset 或命中区域组件；武器只产生基础伤害上下文，目标根据自身骨架解释命中区域。若需要更稳定的竞技判定，可使用专用 Hitbox，但要控制碰撞体数量和查询过滤。

### Q16. 敌人近战如何避免低帧率漏检和重复扣血

**结论：** 攻击范围由动画窗口控制，窗口内使用剑刃前后帧位置做连续 Sphere Sweep，一次攻击再用集合去重。

**调用链：**

```text
TryAttackTarget
-> bIsAttacking = true
-> OnAttackStarted 播放 Montage
-> AnimNotifyState::NotifyBegin
-> BeginAttackWindow，保存 weapontop/weaponend
-> NotifyTick
-> UpdateAttackWindow
-> 上一帧剑刃段到当前帧剑刃段的补充采样和 Sweep
-> TryApplyAttackDamage
-> ApplyDamage
-> NotifyEnd / Finish Notify / Finish Timer
-> EndAttackWindow / FinishAttack
```

**防重复机制：**

- `HitActorsThisAttack` 记录本次攻击已经处理的 Actor。
- `bHitTargetThisAttack` 保证当前单目标攻击不会重复命中。
- 旧单点 Notify 只保留兼容性；挥空不会阻止后续连续刀刃窗口继续检测。
- `FinishAttack()`、死亡和 EndPlay 都清理窗口、集合和 Timer。

**极低帧率仍漏检怎么办：** 根据剑刃位移和旋转增加有限子采样，或适当扩大 Sweep 半径。采样数必须设上限，并记录每帧 Sweep 数和 Game Thread 成本。判定更宽会增加“空气刀”风险，需要与动画和碰撞体一起 A/B。

### Q17. 敌人寻路使用 A* 还是 NavMesh

**结论：** 项目使用 UE NavigationSystem 和 `MoveToActor/MoveToLocation`。NavMesh 表示可行走空间，路径搜索通常在导航图上完成；项目没有自行实现 A*。

**当前配合方式：**

- SurroundManager 计算相对槽位后先投影到 NavMesh。
- AIController 向槽位或攻击接近点提交 Move 请求。
- 只有目标变化超过 `PathRefreshDistance` 或当前没有有效移动请求时才刷新，避免每次决策都重复 MoveTo。
- 生成点复用时使用 `GetRandomReachablePointInRadius` 获得附近可达位置。

**大量敌人时为什么不先优化 A*：** 160 AI 基线中 Pathfinding 只有 `0.071 ms`，远低于 CharacterMovement 与 Animation。当前数据不支持重写寻路。若以后 Nav Query 成为热点，再考虑降低路径刷新频率、缓存共享路径走廊、分帧提交或使用更适合大规模群体的方案。

### Q18. 倒计时开始了但没有敌人，如何定位

**回答顺序：**

1. 确认当前关卡实际使用 `AfpstrueGameMode` 或其蓝图子类。
2. 在 `StartGameMode()` 入口、Cast Success 和 Cast Failed 设置断点或日志。
3. 检查 GameMode 的 `EnemyClass` 是否配置。
4. 检查至少四个 TargetPoint 是否带精确的 `EnemySpawn` Actor Tag。对象名字叫 EnemySpawn 不等于拥有该 Tag。
5. 查看 `StartGameMode failed`、`SpawnActor failed` 和每次生成成功日志。
6. 检查 Spawn Transform、碰撞处理和出生点是否位于有效位置。
7. 敌人生成后，再检查 `AIControllerClass`、`AutoPossessAI = PlacedInWorldOrSpawned` 和 NavMesh。

**诊断原则：** “没有 Actor”“Actor 生成但没有 Controller”“有 Controller 但没有路径”“有路径但被碰撞卡住”是四个不同层级。必须先观察对象是否存在，再进入 AI 和导航调试。

### Q19. 大量敌人时 Draw Call 怎么处理

**现有证据：** 最终矩阵中 10 敌人 Draw Calls 约 `1659`，160 敌人约 `3458`。场景本身已有较高的固定提交成本，敌人的 SkeletalMesh、材质 Pass 和阴影又随数量增加。

**定位顺序：**

1. 用 `stat unit` 区分 Game、Draw 和 GPU 瓶颈。
2. 用 `stat RHI`、ProfileGPU 和 RenderDoc/Insights 检查 Draw Call、Pass 和状态切换。
3. 区分场景静态物、敌人 SkeletalMesh、阴影、材质槽和透明对象的贡献。
4. 逐项关闭阴影、Lumen 或高成本材质做同机位 A/B。

**可选方案：**

- 重复静态场景物使用 ISM/HISM，并尽量共享材质。
- 合并不必要的材质槽，减少每个 SkeletalMesh 的 Section 数。
- 远距离敌人降低阴影质量、关闭不必要的 Cast Shadow 或改用代理表现。
- 对大量同骨架角色评估 Animation Sharing、Mesh Merge 或简化代理，但要衡量动画自由度和内存。
- 使用可见性、距离和遮挡剔除减少真正提交的对象。

**不能混淆：** ISM/HISM 适合重复 StaticMesh，不能直接替代每个敌人的独立骨骼动画。Nanite 也不意味着自动消除所有 Draw Call、材质和阴影成本。

### Q20. 大量敌人 Spawn/Destroy 会不会造成 GC 卡顿，要不要对象池

**结论：** Actor Destroy 后不会等同于 C++ 立即释放全部 UObject 内存；UE GC 会在之后根据 UObject 引用可达性回收。大量短生命周期对象可能增加分配、注册、销毁和 GC 工作，但是否需要对象池必须先测。

**当前生命周期治理：**

- GameMode 用 `TWeakObjectPtr` 注册敌人，不延长敌人生命周期。
- 敌人死亡和 Destroyed 两条路径都可注销，但 Set Remove 保证只处理一次。
- AIController、EnemyCharacter、WeaponComponent 和 HealthComponent 在 Stop/Death/EndPlay 中清 Timer 和 Delegate。
- 敌人死亡后停止 Movement、碰撞和 AI，并按 LifeSpan 销毁。
- 80 敌人统一死亡后等待 35 秒，Enemy Actor 与 GameMode 注册数由 80 回到 0，UObject 由 50,763 降到 49,876；这证明当前回收链可完成，但不能代替连续多波次长期审计。

**强引用与弱引用：** `TObjectPtr` 配合 UPROPERTY 表达 UObject 强引用；`TWeakObjectPtr` 不阻止对象被回收，使用前必须检查有效性。弱引用解决悬空和生命周期延长问题，不等于自动清除容器中的无效条目。

**对象池何时值得做：** Insights、`obj list`、MemReport 或 GC 日志显示波次边界存在明显 Spawn/GC 尖峰，而且该尖峰影响 P95/P99 时再做。回池必须重置 Health、AI State、Timer、Delegate、Montage、碰撞、Movement、槽位、攻击令牌和材质状态。只隐藏 Actor 会保留内存和错误状态，甚至可能更慢。

### Q21. 纹理池警告和 VSM Non-Nanite Queue Overflow 是一回事吗

**结论：** 不是。Texture Streaming 管纹理 Mip 驻留预算；VSM Non-Nanite Queue Overflow 属于 Virtual Shadow Map 对非 Nanite 阴影投射几何的页面标记工作，二者的统计和修复路径不同。

**纹理治理结果：**

- 使用 `stat streaming`、`ListStreamingTextures`、`MemReport -full`、Size Map 和 Reference Viewer 定位资源。
- 将六张不需要 4K 的环境与植被纹理限制到 2048。
- Streaming 占用从约 `212.27 MB` 降至 `152.27 MB`，减少约 `60 MB`，约 `28.3%`。
- Texture Memory Used 从 `288.586 MB` 降至 `228.906 MB`。
- P95 没有明显变化，因此收益是资源预算余量，不是帧率提升。

**VSM 当前结论：** 已完成根因分类、页覆盖诊断、首批大面积资产治理、敌人阴影距离分级，以及粗页开关、动态粗页阈值和阴影半径阈值的单变量实验。`IncludeInCoarsePages = 0` 虽消除该次警告，却让平均 FPS 从 `55.19` 降到 `45.79`，因此没有固化。80 敌人实验仍会单次复现队列警告，当前是接受残留风险，不能宣称彻底修复。

### Q22. UI 为什么初始值可能是 0，为什么当前仍保留文字绑定

**结论：** C++ 已提供变化事件，长期方案应是“初始快照 + 增量事件”；但当前封板版本的剩余时间仍沿用已有 UMG Text Binding。本轮优先保证运行闭环，不把计划中的事件驱动重构冒充为已完成。

**已有事件接口：**

- HealthComponent：`OnHealthChanged`。
- WeaponComponent：`OnAmmoChanged` 和武器状态事件。
- GameMode：`OnRemainingTimeChanged`、`OnWaveChanged`、`OnAliveEnemyCountChanged`、`OnGameResult`。

**初始显示为 0 的原因：** Event 方案中，Widget 可能错过创建前的第一次广播；当前 Text Binding 方案中，常见根因是 Cast 没有接入白色执行链、Target 误接 `self`，或使用了 `ToText(Object)`。

**当前修复：** 绑定函数执行 `Get Game Mode -> Cast -> Get Remaining Time -> ToText(Integer) -> Return`。发布后若统一改为事件驱动，应先读取 `GetCurrentHealth/GetRemainingTime/GetCurrentAmmo` 快照，再订阅变化事件，并一次性删除旧 Binding，不能两套更新路径并存。

### Q23. 当前架构还有哪些问题

**高优先级问题：**

1. **命中对象耦合。** WeaponComponent 直接 Cast EnemyCharacter，应该抽象 Damageable/HitZone 契约。
2. **同帧胜负不确定。** 玩家死亡和时间归零的先后可能决定结果，应统一结果评估入口和优先级。
3. **初始快照时序。** HealthComponent 在 Owner 监听者绑定前可能已广播初始化生命，需要明确 Snapshot 流程。
4. **编译通过不等于玩法通过。** 两个零引用旧模板 Widget 已清理且全量蓝图编译为 0 Error，但仍要统一回归换弹中断、死亡、UI、胜负和重启。
5. **缺少同版本逐项 A/B。** 最终五档矩阵已经建立当前容量，但 LOD/URO/Movement 分级没有逐项关闭后的全档对照。
6. **VSM 仍有残留。** 已完成归因和三组单变量实验，但 80 敌人档仍可能出现单次队列警告。

**中期扩展：**

- 若武器种类增多，进一步定义通用武器接口和装备容器。
- 若敌人行为复杂，评估 StateTree/Behavior Tree 和数据驱动状态配置。
- 若频繁生成成为热点，再引入可验证的对象池。
- 若需要联机，重新划分服务器权威状态和客户端表现，不能只给现有函数加 Replicated。

### Q24. 如果继续优化，你会先做什么

**回答：** 我不会先选听起来更高级的技术，而是先补齐证据缺口。

1. 先完成实际玩法蓝图回归，再执行 Shipping 打包和产物冒烟测试；全量蓝图编译已经通过。
2. 增加 AI Decision Count、Move Request Count、攻击令牌等待时间和攻击响应延迟统计。
3. 如需证明优化收益，在当前版本逐项关闭 Movement/Animation 分级，按 10/20/40/80/160 重跑至少三次并报告中位数、P95 和 P99。
4. 对远距离 Movement/Animation 分级做画质、攻击窗口和行为正确性回归。
5. 继续按页覆盖排名治理大面积非 Nanite 阴影投射物，并用 ProfileGPU 验证 Shadow/VSM 成本。
6. 做连续多波次对象数量与内存回落审计，确认是否真的需要对象池。

**停止条件：** 达到目标平台帧预算后停止继续牺牲动画响应和画质。优化目标是满足预算，不是让所有统计数字无限降低。

## 5. 高频扩展与图形追问

本节同时包含条件扩展和图形追问。网络、GAS 与 SSAO 不是当前项目成果；角色蓝图后处理是当前实现，但修改 Renderer 只属于进一步方案。回答时必须逐项标明边界。

### Q25. 如果把当前 FPS 改成联机，最先改什么

**回答：** 首先划分权威边界，而不是先给所有变量加 Replicated。

- Server 权威保存生命、弹药、射速、命中、敌人 AI、波次和胜负。
- Owning Client 立即播放镜头、后坐力、枪口和部分开火表现，减少输入延迟。
- 客户端通过 Server RPC 提交开火意图，服务器验证射速、弹药、起点、方向和时间戳。
- Health 等持续状态使用属性复制和 OnRep；一次性、瞬态表现按可靠性选择 RPC 或本地重建。
- GameMode 只在服务器存在，客户端需要展示的波次、时间和结果应迁移到 GameState；玩家持久状态根据语义进入 PlayerState。
- Hitscan 竞技需求可能需要服务器历史快照和延迟补偿，但要限制回溯窗口并处理高延迟公平性。

CharacterMovement 已提供成熟的移动预测与服务器校正能力，但当前工程没有做联机验收，不能把上述设计说成已完成。RPC 的执行还取决于 Actor 所有权和 Owning Connection，而不是函数上写了 `Server` 就一定能远程执行。

### Q26. 为什么当前没有使用 GAS，什么时候值得引入

**回答：** 当前只有简单生命、枪械、近战和少量互斥状态，现有 Component + FSM 已能清楚表达规则。引入 GAS 会增加 ASC、AttributeSet、Ability、Effect、Tag、Cue 和网络预测的学习与资产成本，对当前封板目标收益不足。

当需求发展到以下规模时，GAS 的价值会明显提高：

- 多个角色共享大量主动和被动技能。
- 冷却、消耗、Buff/Debuff、叠层、免疫和标签阻断关系复杂。
- 技能需要统一预测、服务器确认和 GameplayCue 表现。
- Character 会重生，但属性和能力需要由 PlayerState 持久保存。

迁移时不会把 WeaponComponent 整体机械替换。可以先把 Health/Ammo 等资源映射为 Attribute，把 Reload/Fire/Skill 抽成 Ability，把伤害与 Buff 抽成 GameplayEffect，再决定哪些原有武器逻辑保留为底层执行器。

官方资料中，GAS 的核心是 AbilitySystemComponent、GameplayAbility、AttributeSet、GameplayEffect、GameplayTag 和 GameplayCue 的协作；它支持复制与预测，但这些能力仍需要正确的 Owner/Avatar、执行策略和生命周期设计。

### Q27. 为什么把 PostProcessComponent 创建在角色蓝图中

**当前事实：** `PostProcessComponent` 创建在 `BP_FirstPersonCharacter` 蓝图，不在 Character C++ 中。C++ 的 HealthComponent 和 Character 决定受伤/死亡并发出事件，蓝图用 Timeline 修改 `BlendWeight`，播放受伤或死亡视觉反馈。

**选择理由：** 这是单机本地玩家的表现逻辑，需要和 Camera Shake、声音、动画一起快速调参。Gameplay 规则不应依赖饱和度、对比度、色彩偏移、暗角或景深的具体数值。

**继续追问“为什么不用关卡 Volume”：** 关卡 Volume 适合环境区域和全局调色；受伤效果不应因为玩家离开某个空间而消失。若改为分屏、观战或联网，应重新评估 CameraComponent 或 PlayerCameraManager，不能默认 Character 上的组件天然只影响拥有者视图。

**继续追问“两个 Timeline 同时写 BlendWeight”：** 最后写入者会覆盖前者。受伤脉冲与死亡保持应拆成独立层，或死亡时停止受伤 Timeline 并独占权重。

### Q28. 饱和度、对比度、色彩偏移、暗角和景深在哪个阶段完成

**回答：** 游戏先完成深度/Base Pass、GBuffer 和光照，再进入后处理链。DOF 根据 Scene Depth 和镜头参数模糊 Scene Color；饱和度、对比度与色彩偏移属于调色/Tonemapper 链；暗角属于较晚的屏幕镜头效果。UE 可能按版本和平台把多个效果合并到同一 RDG Pass，准确成本应查看 ProfileGPU，而不是只背一张固定顺序图。

Timeline 在 Game Thread 更新混合权重，渲染器汇总相机、组件和 Volume 的最终 View 设置，GPU 才执行全屏处理。Timeline 不是 Shader。

### Q29. 项目实现 SSAO 了吗，为什么仍然要会讲

**结论：** 没有实现或验收，不能列为项目成果。它属于图形学后期追问。

SSAO 使用当前屏幕的 Scene Depth 与 GBuffer Normal 估计局部环境光遮蔽，通常在最终调色前参与环境/间接光观感。它只能看到屏幕已有信息，因此会遇到屏幕边缘消失、屏幕外遮挡缺失、薄物体厚度未知和 Halo 等问题。Radius、Intensity 和 Quality 会在接触层次、伪影与 GPU 成本之间取舍。

如果要求加入项目，先做固定机位 `Off/On`、`Visualize Ambient Occlusion` 和 ProfileGPU A/B，再检查当前 Lumen 配置是否已经提供相关遮蔽，避免重复加深和重复付费。

### Q30. UE 的 GC 和 C++ 析构是什么关系

**回答：** 普通 C++ 对象由作用域、RAII 和明确所有权管理；UObject 由 UE 的引用图和 GC 管理，两者不能混用。Actor 调用 `Destroy` 后先退出 World 生命周期并执行 `EndPlay`，不是立刻 `delete`；失去可达强引用后，才由后续 GC 完成 UObject 销毁和内存回收。

项目中的 Timer、Delegate、AI 槽位、攻击窗口和 GameMode 注册必须在 Death/Stop/`EndPlay` 中幂等清理，不能等析构函数。成员 UObject 强持有使用 `UPROPERTY` + `TObjectPtr`，观察关系使用 `TWeakObjectPtr` 并在使用前检查有效性。手动 `CollectGarbage` 不是关闭 Widget 或销毁尸体的常规方案，因为会带来停顿。

### Q31. 波次生成是否应该自建线程池并异步加载

**回答：** 不先自建线程池。资源预取应优先使用 `UAssetManager`、`FStreamableManager` 和软引用，让 UE 的异步加载系统处理 IO；加载完成后回到安全线程，再用 Timer 分批 `SpawnActor`。Worker Thread 只处理纯数据，不能任意修改 UObject、World 或碰撞场景。

当前基线没有证明波次切换存在加载尖峰，高敌人数的主要成本是 CharacterMovement、Animation 和 VSM，因此异步加载不会直接解决当前瓶颈。只有 Insights 显示 Game Thread 等待 IO、首次资源加载或组件创建影响 P95/P99 时，才把敌人类、Montage、Niagara 和音效改成软引用预取，并处理取消、失败回退和 Handle 生命周期。

### Q32. 多敌人交互是不是项目的优化亮点

**回答：** 它不是一个孤立算法，而是综合压力场景和治理链。敌人数量增加会同时放大 AI 决策、NavMesh 跟随、CharacterMovement、包围与攻击竞争、骨骼动画、阴影、Ragdoll、GameMode 注册和 GC 回落问题。

当前项目已经使用 Timer FSM、距离决策分级、MoveTo/NavMesh、SurroundManager 槽位、Movement 降频、URO、可见性动画策略、四级骨骼 LOD、远距离阴影关闭和死亡生命周期清理。正确亮点是：用 `10/20/40/80/160` 固定矩阵定位瓶颈，再按系统治理并回归正确性；不是简单地说“支持很多敌人”。对象池、异步加载和 Animation Budget Allocator 仍是由数据触发的后续方案。

## 6. 模拟面试追问链

### 6.1 武器链

```text
为什么选 Hitscan
-> LineTrace 使用什么 Channel
-> 为什么从相机发射
-> 枪口贴墙怎么办
-> 怎么判断爆头
-> 换骨架后怎么办
-> 自动开火如何限制射速
-> 换弹被打断怎么办
-> 改成联机后谁有权扣弹和判定命中
```

### 6.2 AI 链

```text
为什么使用 FSM
-> 为什么不用 Tick
-> Timer 是否在多线程
-> NavMesh 和 A* 是什么关系
-> 为什么所有敌人会扎堆
-> 槽位和攻击令牌分别解决什么
-> 敌人死亡如何释放资源
-> 160 AI 为什么不先重写寻路
-> 1000 AI 时当前架构哪里先失效
```

### 6.3 生命周期链

```text
伤害如何进入 HealthComponent
-> 为什么 OnDamaged 不判断死亡
-> 如何保证死亡一次
-> 死亡时要清哪些 Timer 和 Delegate
-> GameMode 如何立即判负
-> 时间归零与死亡同帧怎么办
-> 敌人 Destroyed 和 Died 都通知时为何不重复计数
-> 多轮后内存不回落如何定位
```

### 6.4 性能链

```text
如何判断 CPU 还是 GPU 瓶颈
-> 为什么看帧时间而不是只看 FPS
-> 为什么还要 P95/P99
-> 160 AI 哪些模块最贵
-> 如何证明不是 Pathfinding
-> Timer 降频有什么响应代价
-> URO 为什么不能在攻击时完全停骨骼刷新
-> Draw Call 增加来自哪里
-> 纹理减少 60 MB 为什么不等于帧率提升
```

### 6.5 后处理链

```text
PostProcessComponent 在哪里创建
-> 为什么属于角色蓝图表现层
-> Timeline 与 BlendWeight 谁在 CPU、谁在 GPU
-> 多个组件和 Volume 如何混合
-> DOF、调色、Tonemapper 和暗角是什么顺序
-> SSAO 为什么需要 Depth 与 Normal
-> SSAO 为什么不是遮挡剔除
-> 如何用 ProfileGPU 和固定机位验证成本
-> 什么时候使用 Post Process Material
-> 什么时候才值得修改 Renderer/RDG Pass
```

## 7. 面试前必背数据

| 数据 | 正确表述 |
| --- | --- |
| 60 FPS 预算 | 每帧约 `16.67 ms` |
| 40 敌人 | `61.02 FPS`，Frame Avg `16.389 ms`，当前玩法容量口径 |
| 80 敌人 | `55.20 FPS`，Frame Avg `18.115 ms`，P95 `21.076 ms` |
| 160 敌人 | `35.83 FPS`，Frame Avg `27.907 ms`，P95 `32.459 ms` |
| 160 敌人 Game Thread | `27.899 ms` |
| 160 敌人 CharacterMovement | `6.188 ms` |
| 160 敌人 Animation | `4.448 ms` |
| Draw Calls | `1659 -> 3458`，从 10 增至 160 敌人 |
| Streaming 占用 | `212.27 MB -> 152.27 MB`，减少约 60 MB/28.3% |
| Texture Memory Used | `288.586 MB -> 228.906 MB` |
| 生命周期回收 | 80 个 Enemy Actor 和注册数回到 0，UObject 减少 887 |
| VSM 残留 | 正式矩阵仅 80 敌人档出现 1 次；后续 80 敌人实验仍可单次复现 |

## 8. 参考入口

### 项目证据

- `Docs/PERFORMANCE_BASELINE.md`
- `Docs/FPS_Core_Technical_Summary.md`
- `Docs/PROJECT_TASK_CHECKLIST.md`（当前 C++/蓝图边界与接线）
- `Docs/Archive/AI_OPTIMIZATION_DECISION_RECORD.md`（历史决策原文）
- `Source/fpstrue/fpstrueCharacter.cpp`
- `Source/fpstrue/fpstrueWeaponComponent.cpp`
- `Source/fpstrue/fpstrueHealthComponent.cpp`
- `Source/fpstrue/fpstrueEnemyAIController.cpp`
- `Source/fpstrue/fpstrueEnemyCharacter.cpp`
- `Source/fpstrue/fpstrueSurroundManager.cpp`
- `Source/fpstrue/fpstrueGameMode.cpp`

### UE 官方资料

- [Gameplay Ability System Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/understanding-the-unreal-engine-gameplay-ability-system)
- [Remote Procedure Calls](https://dev.epicgames.com/documentation/unreal-engine/remote-procedure-calls-in-unreal-engine)
- [Actor Role and Remote Role](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-role-and-remote-role-in-unreal-engine)
- [Using Gameplay Abilities](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-gameplay-abilities-in-unreal-engine)
- [Post Process Effects](https://dev.epicgames.com/documentation/unreal-engine/post-process-effects-in-unreal-engine)
- [Color Grading and the Filmic Tonemapper](https://dev.epicgames.com/documentation/en-us/unreal-engine/color-grading-and-the-filmic-tonemapper-in-unreal-engine)
- [Ambient Occlusion](https://dev.epicgames.com/documentation/unreal-engine/ambient-occlusion?application_version=4.27)
- [FMath::VRandCone](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FMath/VRandCone)
