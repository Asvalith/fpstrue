# FPS 蓝图接线与封板清单

> 文档身份：当前工作区唯一活动任务、蓝图接线和发布验收清单。
>
> 同步状态：2026-08-16。C++ 主链、性能证据、完整 Development Editor 编译和全量蓝图编译已经封口；当前只剩实际玩法蓝图接线、删旧节点和 PIE 回归。

稳定架构、调用链、实战问题与方案取舍见 [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md)。测试条件、原始数据和证据见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。本文件只保留编辑器中要做什么、怎样接线以及怎样判定完成。

## 1. 当前执行顺序

1. [x] 审计并清理 `WB_Healthbar` 与 `WB_BaseButton` 两个零引用旧模板资产。
2. [x] 执行 `Compile All Blueprints`；结果为 `0 errors / 0 warnings / 0 failed to load`。
3. [x] 关闭编辑器后执行完整 Development Editor 编译，清除旧 Live Coding 补丁影响。
4. [ ] 按下表完成实际玩法蓝图接线与旧节点清理。
5. [ ] 回归输入、射击、两种换弹、瞄准恢复、敌人移动与攻击、死亡、倒计时、胜负和重启。
6. [ ] 蓝图和玩法通过后再执行 Shipping Cook/Package，并对产物做冒烟测试。

封板阶段不再增加联网、GAS、行为树、EQS、对象池或风格化渲染功能。

### 1.1 蓝图接线交接表

本轮不自动改写现有 `.uasset` 图表。你只需要处理以下接线和删旧节点，Gameplay 状态与 Timer 不再下放到蓝图：

| 资产 | 保留或连接 | 删除或断开 |
| --- | --- | --- |
| `Demonstration` Level Blueprint | 先绑定 `OnGameResult`，再且只调用一次 `StartGameMode`；创建并保存 `ingame` 实例 | 第二条开始游戏链、Level BP 中的波次/倒计时/生成 Timer |
| `BP_FirstPersonCharacter` | `OnAimChanged` 只驱动 FOV、手臂和后处理表现；拾取成功后显示手臂 | 蓝图 IA_Shoot/Reload/Aim 规则、射速 Timer、弹药和换弹状态副本 |
| `BP_Weapon` | 绑定 Fire/Reload/Trace 委托；Reload Commit、Completed、Interrupted 分别调用事务接口 | 旧 `Fireonce`、蓝图自动开火 Timer、蓝图 LineTrace/ApplyDamage、具有第二套伤害的 Projectile |
| `enemy_BP` | BeginPlay 只保留 AnimInstance 初始化；攻击、受击、死亡事件只播表现 | `chase player` Timer、Tick、`AI MoveTo`、`AddMovementInput`、蓝图攻击门禁、死亡 `Destroy Actor`、第二次冲量 |
| `ingame` | 使用一套 HUD 更新路径并确认初始值；当前允许继续保留已有 Text Binding | 同一字段同时使用 Binding 与事件更新、每帧 Tick 更新 UI |
| `winorfail` | 接收一次 `bPlayerWon`，设置文字/颜色并切换 UI 输入模式 | Widget 自己再次绑定 GameResult、以 `self` 调用自身结果事件形成循环 |

当前运行入口已由配置确认：`Demonstration`、`fpstruegamemode`、`/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter`。不要改回同名模板 Pawn。

## 2. 唯一职责边界

| 层 | 当前唯一职责 | 禁止重复实现 |
| --- | --- | --- |
| Character | 输入、移动、视角、当前武器引用、玩家死亡协调 | 弹药库存、射速 Timer、Line Trace、换弹结算 |
| WeaponComponent | 弹药、射速、连续射击、Hitscan、换弹事务 | UMG、Montage 选择、输入资产绑定 |
| HealthComponent | 受伤入口、生命 Clamp、一次性死亡广播 | 死亡表现、胜负判断 |
| EnemyCharacter | 近战事务、攻击窗口、Sweep、死亡清理 | 目标搜索、路径决策、蓝图追击 Timer |
| EnemyAIController | `Idle/Chase/Attack/Dead`、MoveTo、决策降频 | 命中检测、伤害表现 |
| SurroundManager | 包围槽位和 Attack Token | 动画、伤害、每敌人局部副本 |
| GameMode | 前置校验、波次、倒计时、敌人注册表、胜负 | HUD 文本、Montage、后处理 |
| 蓝图/UMG | 资产编排、动画、音效、特效、文字和输入模式 | 第二套玩法状态、Timer、伤害或弹药 |

一次性边界已经由 C++ 负责：死亡、换弹提交、敌人注销、攻击命中、Attack Token 和游戏结算都必须幂等。蓝图只调用公开入口，不再保存一份平行状态。

## 3. 运行资产与类默认值

### 3.1 地图和 GameMode

```text
Project Settings / Maps & Modes
-> Editor Startup Map = /Game/FactoryDistrict/Maps/Demonstration
-> Game Default Map = /Game/FactoryDistrict/Maps/Demonstration
-> Default GameMode = fpstruegamemode

Demonstration / World Settings
-> GameMode Override = fpstruegamemode
```

### 3.2 GameMode 参数

```text
Default Pawn Class = /Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter
Enemy Class = /Game/FirstPerson/Blueprints/enemy/enemy_BP
Game Duration = 90
Minimum Spawn Point Count = 4
Total Waves = 3
Base Enemies Per Wave = 5
Enemies Added Per Wave = 2
Surround Manager Class = AfpstrueSurroundManager 或有效蓝图子类
```

项目中另有 `/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter`。它不是当前完整玩家资产，不得重新设为 Default Pawn。

### 3.3 关卡前置条件

- [ ] 至少 4 个 `TargetPoint` 的 **Actor Tags** 包含 `EnemySpawn`，不是 Component Tags。
- [ ] 按 `P` 后，玩家、出生点和主要路径均位于连续绿色 NavMesh 内。
- [ ] `StartGameMode()` 前确认 EnemyClass、玩家 HealthComponent 和 SurroundManager 均有效。
- [ ] Output Log 不出现 `StartGameMode failed:`、错误 AIController 类型或出生失败。

任一前置条件失败时，先读 `StartGameMode failed:` 日志，不从倒计时或 UMG 开始猜。

## 4. 开始游戏、HUD 与结果界面

### 4.1 Demonstration Level Blueprint

当前 Level Blueprint 只做入口适配，不保存波次数量、剩余时间或胜负状态。

```text
Any Key Pressed
-> Do Once
-> 移除主菜单 / 完成镜头切换
-> Create Widget(ingame)
-> Add To Viewport（Target = Create Widget.ReturnValue）
-> Set Input Mode Game Only
-> Get Game Mode
-> Cast To fpstrueGameMode
-> Bind Event to OnGameResult
-> Start GameMode
```

- [ ] `Caps Lock` 若承担退出菜单的现有入口则保留，不与“任意键开始”共用一条事件。
- [ ] 只有菜单淡出或镜头 Blend 确实需要时才保留 `Delay`；它不负责等待 GameMode 创建。
- [ ] `OnGameResult` 必须在 `StartGameMode` 前绑定，避免启动阶段立即失败时错过结果。
- [ ] `StartGameMode` 一局只调用一次，不在 Widget、Character 或 Level Blueprint 其他分支重复调用。
- [ ] 验收后删除 `Print String`；必须保留的诊断节点写明“入口测试用”并默认断开。

### 4.2 当前 HUD 数据方案

当前项目保留已有 UMG Text Binding；本轮不强行改成事件驱动，以免封板前重写 UI。剩余时间绑定函数必须存在白色执行链：

```text
Get_剩余时间文字_Text
-> Get Game Mode
-> Cast To fpstrueGameMode
-> Get Remaining Time（Target = Cast 结果）
-> To Text(Integer)
-> Return Node
```

- [ ] 使用 `To Text(Integer)`，不能使用 `To Text(Object)`。
- [ ] Cast 节点不能只连数据引脚而没有执行引脚。
- [ ] `Get Remaining Time.Target` 不能是 Widget 的 `self`。
- [ ] 开局第一帧显示 90，而不是等待下一次 Timer 后才从 89 开始。
- [ ] 弹药和生命 UI 在 Widget 创建后立即读取一次快照，不等第一次开火或受伤。

事件驱动 UMG 是发布后的可选重构：只有准备统一 HUD 生命周期时，才改为“先绑定 Delegate、读取快照、再启动 GameMode”，并一次性移除所有 Text Binding，不能两套方案并存。

### 4.3 结果界面

`OnGameResult` 只在 Level Blueprint 绑定一次：

```text
OnGameResult(bPlayerWon)
-> Create Widget(winorfail)
-> 设置创建出的实例变量 bPlayerWon
-> Add To Viewport（Target = Create Widget.ReturnValue）
-> Remove From Parent(ingame 实例)
-> Get Player Controller
-> Set Show Mouse Cursor(true)
-> Set Input Mode UI Only
```

`winorfail` 的 `Event Construct` 只根据 `bPlayerWon` 设置标题、说明和颜色。它不能再次调用或绑定 `OnGameResult`，否则会形成递归或重复界面。

- [ ] 胜利显示成功文案和颜色，失败显示失败文案和颜色。
- [ ] 重新开始使用 `Open Level` 打开当前关卡，创建新的 World 与 GameMode。
- [ ] 结果出现后不能再移动、开枪或让敌人继续攻击。

## 5. 玩家和武器蓝图

### 5.1 输入与瞄准

完整玩家蓝图的 Class Defaults：

```text
DefaultMappingContext
JumpAction / MoveAction / LookAction
FireAction = IA_Shoot
ReloadAction = IA_reload
RunAction = IA_Run
AimAction = IA_Aim
```

C++ 已绑定 Enhanced Input。蓝图不得再维护开火 Timer 或第二套 IA 事件。

- [ ] Aim `Started/Triggered` 进入瞄准，`Completed/Canceled` 必须退出瞄准。
- [ ] 退出瞄准后 FOV、手臂位置、灵敏度和准星均恢复。
- [ ] 换弹、死亡和武器禁用会强制退出瞄准。

### 5.2 武器装备和表现 Delegate

`OnWeaponEquipped(WeaponComponent)` 保存的是组件实例引用。随后绑定：

- `OnAmmoChanged`
- `OnWeaponFirePerformed`
- `OnWeaponDryFire`
- `OnWeaponReloadStarted`
- `OnWeaponReloadFinished`
- `OnWeaponReloadCanceled`
- `OnWeaponTraceFinished`

绑定后立即读取一次当前弹药快照。`FinishReload/CancelReload/CommitReload` 的 Target 必须是这个 WeaponComponent，不能接 TP Weapon 的 SkeletalMesh。

### 5.3 开火表现

```text
OnWeaponFirePerformed
-> 播放手臂/武器射击 Montage
-> 枪口火焰、音效、抛壳、Camera Shake

OnWeaponTraceFinished
-> TraceStart / TraceTarget 生成曳光表现
-> bHit 为 true 时用 ImpactPoint / ImpactNormal 生成命中特效、贴花和表面音效
```

蓝图不能再次扣弹药、执行 Line Trace 或调用 ApplyDamage。曳光弹 Actor 只是 Hitscan 的可视化：从枪口移动到 `TraceTarget`，LifeSpan 可按 `Distance / CosmeticSpeed` 计算并 Clamp；它不拥有 Gameplay 伤害。

- [ ] 单击只消耗一发，长按遵守射速，空仓只触发 DryFire。
- [ ] 枪口表现和 Trace 回调每发各执行一次。
- [ ] 未命中时曳光到最大射程终点，不能停在世界原点或形成无限长线。
- [ ] 蓝图不再 Spawn 一个具有第二套伤害碰撞的真实 Projectile。

### 5.4 换弹事务

```text
OnWeaponReloadStarted(bWasEmptyReload)
-> 根据 bWasEmptyReload 选择普通/空仓手臂 Montage
-> 同时播放对应武器 Montage

手臂 Montage 的弹匣插入帧
-> Reload Commit Notify
-> Current WeaponComponent.CommitReload

手臂 Montage OnCompleted
-> Current WeaponComponent.FinishReload

手臂 Montage OnInterrupted
-> Current WeaponComponent.CancelReload
```

只有手臂 Montage 的代理回调负责事务收口，武器 Montage 只做同步表现。不要把 `OnBlendOut` 接到 `FinishReload`，也不要在 Montage 开始或弹匣离手时直接加弹。

- [ ] 普通换弹和空仓换弹都在正确插匣帧提交弹药。
- [ ] 换弹未完成时开火被 `CanFire()` 拒绝，不能打断后继续射击。
- [ ] Montage 被其他动画替换、玩家死亡或重复按 R 时不会加弹或永久卡在 Reloading。
- [ ] 一轮换弹即使重复收到 Notify/Completed 也只提交一次。

## 6. 敌人蓝图与 Montage

### 6.1 类默认值和 BeginPlay

```text
Parent Class = AfpstrueEnemyCharacter
AI Controller Class = fpstrueEnemyAIController
Auto Possess AI = Placed in World or Spawned
```

BeginPlay 只保留动画初始化：

```text
BeginPlay
-> Mesh / Get Anim Instance
-> Cast To enemy_anim
-> 保存 As Enemy Anim
-> 初始化速度和动画
```

删除旧 `Set Timer by Function Name(chase player)` 时，不要一起删除 AnimInstance 初始化执行链。追击由 C++ AIController 自动启动，蓝图中不保留 Tick、循环 Timer、`AI MoveTo`、`AddMovementInput` 或手写 Chase。

### 6.2 攻击动画

```text
OnAttackStarted
-> Random Integer / Select
-> Play Montage

Play Montage.OnCompleted
Play Montage.OnInterrupted
-> HandleAttackFinishedNotify
```

不需要一个永远为 True 的 Branch，也不需要蓝图 `bIsAttackPlaying` 作为第二套门禁。C++ Attack 状态、冷却和 Attack Token 决定是否能发起攻击，`FinishAttack()` 保证完成、中断和超时重复回调仍然幂等。

每个正式攻击 Montage：

1. 在真正有杀伤力的帧段放置 `Enemy Attack Window` NotifyState。
2. 确认 Skeleton 有 `weapontop` 和 `weaponend` Socket。
3. 删除同一 Montage 中旧的单点 `Enemy Attack Hit`。
4. 不在 Notify、AnimBP 或 enemy_BP 中直接 ApplyDamage。

近战继续使用 `ECC_Pawn` 对象查询、`TargetCharacter` 精确过滤、双 Socket 帧间 Sphere Sweep 和整轮命中集合。本轮不新增专用碰撞通道。

- [ ] 起手和收招不扣血，有效窗口只扣一次。
- [ ] 低帧率、高速挥砍和多个窗口不漏检、不重复扣血。
- [ ] Montage 完成、中断、死亡和游戏结束均关闭窗口并释放 Token。
- [ ] 薄墙/门框回归无隔墙命中；若复现，再在提交伤害前增加现有 Visibility/LOS 校验。

### 6.3 受伤与死亡表现

```text
OnEnemyDamaged
-> 随机播放受击 Montage

OnEnemyDied
-> 停止 Montage
-> Capsule = NoCollision
-> Mesh Collision Profile = Ragdoll
-> Mesh Collision Enabled = Query And Physics
-> Mesh Set Simulate Physics(true)
-> 死亡音效/特效
```

`OnEnemyDamaged` 不判断死亡；致死伤害由 C++ 直接进入 `OnEnemyDied`，避免受击和死亡 Montage 竞争。命中冲量由 C++ 在物理启用后延迟提交，蓝图不要再加第二次冲量。

- [ ] enemy_BP 中不存在 `Destroy Actor`、短 LifeSpan 或死亡 Delay 销毁分支。
- [ ] 有效 Physics Asset 已绑定，物理 Body 的 Collision Enabled 可接受查询和物理。
- [ ] 尸体不再阻挡活角色，受击冲量不过大，不会直接飞离场景。
- [ ] 当前有效尸体保留时间为 30 秒；GameMode 的敌人注册数在死亡时下降，Actor 在回收时销毁。
- [ ] `OnEnemyDied` 只执行一次，死亡后不再追击、攻击或播放普通受击。

## 7. 已解决的蓝图编译阻塞

### 7.1 现象与难点

`Compile All Blueprints` 最初报告 26 个错误，全部集中在 `/Game/FPS/UI/HealthBar/WB_Healthbar` 和 `/Game/FPS/UI/Buttons/WB_BaseButton`。前者依赖已经不存在的模板 `AC_Health`，后者依赖已经不存在的 `BI_PlayerController`、枚举、材质和声音。错误表面上像当前 HealthComponent、玩家或 UI 架构损坏，直接重接会把已经舍弃的模板系统重新引入工程。

### 7.2 判断与取舍

- Asset Registry 同时检查硬引用、软引用和管理引用，两个资产的 Referencer 均为 0。
- 两者不属于当前 `Demonstration -> ingame/WinOrFail -> fpstruegamemode` 运行链，也不被当前玩家、武器或敌人资产引用。
- 因此选择删除零引用旧模板，而不是伪造 `AC_Health`、`BI_PlayerController` 及其配套资源来让废弃资产勉强编译。
- 删除前保存原始 `.uasset` 备份并校验 SHA-256，避免判断错误时无法恢复。

### 7.3 证据与结果

```text
审计：Saved/Profiling/BlueprintCleanupAudit.json
删除记录：Saved/Profiling/BlueprintCleanupRemoval.json
原资产备份：Saved/BlueprintCleanupBackup_20260816/
全量编译日志：Saved/Logs/CompileAllBlueprints_BlueprintCleanup.log
结果：0 errors / 0 warnings / 0 blueprints that failed to load
```

清理测试射线与屏幕输出后的复验：

```text
Development Editor：Saved/Logs/BlueprintDispatchCleanup_Build.log
Compile All Blueprints：Saved/Logs/CompileAllBlueprints_BlueprintDispatchCleanup.log
结果：0 errors / 0 warnings / 0 blueprints that failed to load
```

这一步只证明所有现存蓝图可编译，不替代 PIE 时序、对象实例和表现回归。

### 7.4 条件变化后的替代方案

如果以后确实需要这两套模板 UI，应从备份恢复到隔离分支，再把它们迁移到当前 `HealthComponent`、实际 PlayerController 和当前样式资产；不能把缺失的旧模板依赖整套重新塞回主项目。若新 UI 已有真实 Referencer，则不再采用删除方案，而应按当前接口逐节点迁移并补初始化快照测试。

## 8. PIE 完整回归矩阵

按顺序执行，前一项失败先修前一项：

- [ ] 打开 `Demonstration`，确认运行 Pawn、GameMode、EnemyClass 和 NavMesh。
- [ ] 按任意键只开始一次；倒计时立即显示 90，并每秒减 1。
- [ ] 第一波按 5 个敌人生成，后续波次数量按配置递增；日志无 Spawn/Controller 错误。
- [ ] 敌人绕障追击，靠近后按 Token 限制攻击，不静止、不漂移、不持续顶飞玩家。
- [ ] 单击、连射、空仓、普通换弹、空仓换弹、换弹中开火全部符合状态机规则。
- [ ] Aim 按下进入、松开退出；开火、换弹和死亡不会留下错误 FOV。
- [ ] 身体和头部命中进入同一 HealthComponent 链，倍率正确；不可伤害物体不扣血。
- [ ] 近战有效窗口只伤害一次，攻击中断和死亡会清理窗口。
- [ ] 玩家在剩余时间内死亡立即失败；不会等待倒计时归零。
- [ ] 时间归零且玩家仍存活时胜利；死亡与归零同帧仍只结算一次。
- [ ] 结果界面文案、颜色、鼠标与 UI Only 正确，游戏输入被禁用。
- [ ] 重新开始后时间、波次、敌人数、Health、Ammo、输入和 UI 全部重新初始化。
- [ ] 尸体保留约 30 秒后回收，死亡时存活敌人计数立即下降。
- [ ] 关闭 Debug Draw、屏幕消息、高频日志和测试 Print 后再次回归。

需要专门覆盖的边界：重复输入、Montage 中断、旧 Notify、敌人外部 Destroy、玩家死亡、游戏结算、EndPlay 和重新打开关卡。

## 9. 编译、打包与冒烟

### 9.1 Development Editor

- [ ] 关闭 Unreal Editor，避免 Live Coding 占用 DLL。
- [ ] 清理旧热重载补丁后执行完整 `fpstrueEditor Win64 Development` 编译。
- [ ] 编译结果为成功或 `Target is up to date`，无 LNK1104、MSB3073 或 UHT 错误。
- [ ] 重新打开正确的 `E:\ueprojrct\fpstrue_safe2\fpstrue.uproject`，确认没有打开旧项目。

### 9.2 Shipping Cook/Package

- [ ] `Compile All Blueprints` 为 0 Error 后才开始 Cook/Package。
- [ ] 修复所有 Missing Package、Invalid Reference、Cook Error；普通材质使用标记警告按实际平台验证处理。
- [ ] 打包目录不混入旧构建产物，并记录引擎版本、提交和配置。

### 9.3 打包产物冒烟

- [ ] 启动和退出正常。
- [ ] 菜单、输入模式、移动和瞄准正常。
- [ ] 射击、换弹、命中、受伤和死亡正常。
- [ ] AI 生成、寻路、近战和尸体回收正常。
- [ ] 倒计时、波次、胜负、结果 UI 和重新开始正常。
- [ ] 日志无新的 Error、ensure 或持续刷屏 Warning。

## 10. 已完成的性能封口

- [x] 使用 `10 / 20 / 40 / 80 / 160` 固定矩阵完成独立进程采样。
- [x] 保存 CSV、日志、清单和截图，证据位于 [PerformanceEvidence/20260816](PerformanceEvidence/20260816/README.md)。
- [x] 80 敌人统一死亡后，Enemy Actor 与 GameMode 注册数由 80 回到 0。
- [x] Texture Streaming 五档均无超预算警告；六张高驻留植被纹理减少约 60 MB 驻留量。
- [x] 完成 VSM 粗页、动态页阈值和阴影半径单变量实验，没有保留造成回退的全局 CVar。
- [x] 近距离敌人保留动态阴影，远距离按阈值关闭；骨骼 LOD 资产已经建立。
- [x] 当前发布容量按约 40 个活跃敌人、约 60 FPS 陈述；80/160 是压力档。
- [x] 保留 VSM 单次可复现残留和高密度 CharacterMovement 迭代警告，不宣称彻底消除。

性能结论和精确数字只引用 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)，本清单不复制第二份矩阵。

## 11. 条件触发后的替换方案

以下不是当前发布任务，只保留扩展入口：

| 条件变化 | 替换或增量方案 | 必须验证 |
| --- | --- | --- |
| 需要视野、听觉和丢失目标 | AI Perception + 目标记忆 | 感知延迟、候选数量、切换稳定性 |
| 槽位无法处理掩体/多层空间 | EQS 生成与评分候选点 | Query 时间、可达率、缓存命中率 |
| AI 行为出现复杂并行和组合 | Behavior Tree/StateTree | 决策成本、调试可见性、迁移风险 |
| 需要 500+ 简化单位 | Mass/数据导向模拟、动画预算 | 活跃代理数、CPU、Draw Call、表现降级 |
| Spawn/Destroy 成为 P95 尖峰 | 分批生成、软引用预加载；复用收益明确后再对象池 | Spawn P95、GC、重置正确性、内存常驻 |
| 多武器/背包/多弹药 | InventoryComponent + 数据资产 | 所有权、切换事务、存档边界 |
| 多种交互物复用 | InteractionComponent + `IInteractable` | 查询频率、焦点切换、失效引用 |
| 改为联机 | GameState/PlayerState、服务器权威、RPC/复制 | 延迟、丢包、晚加入、作弊边界 |
| 复杂属性、Buff 和技能 | GAS | ASC 所有权、预测、GE/Tag 复制成本 |
| 多平台发布 | DeviceProfile、Scalability、输入抽象、PSO 缓存 | 各档帧时间、内存、画质和输入 |
| 渲染成为主要瓶颈 | 遮挡剔除、GPU Driven、阴影/材质分级 | GPU Pass、Draw Call、可见性错误 |

详细扩展问题只保留在学习索引，不写成当前已实现。

## 12. 最小故障定位

| 现象 | 第一检查点 | 已验证思路 |
| --- | --- | --- |
| Start 后时间仍为 0 | 实际 GameMode、Cast Target、绑定函数执行链 | 先验证对象实例，再验证 Getter，不用无条件 Print 当成功证据 |
| 倒计时开始但没敌人 | `StartGameMode failed`、EnemyClass、`EnemySpawn` Actor Tag、NavMesh | 按前置校验顺序排除，不从 Spawn 表现猜 |
| 敌人生成但不移动 | AIControllerClass、Auto Possess AI、NavMesh、旧 Chase 节点 | 只保留 C++ Controller 一条决策链 |
| 敌人靠近被弹开 | Capsule/Movement 碰撞、目标槽位、旧 AddMovementInput | 区分路径、避障、物理接触和攻击距离 |
| 攻击不扣血或重复扣血 | Attack Window、Socket、Pawn Query、旧 Hit Notify | 一种命中入口、整轮去重、结束统一清理 |
| 换弹后不能开火或重复加弹 | Component Target、Commit Notify、Completed/Interrupted | 事务状态、唯一提交点、超时只兜底 |
| 枪或动画实例为空 | 实际 Mesh、Animation Mode、Anim Class、GetAnimInstance Target | 先恢复资产引用，再处理表现节点 |
| UI 初始值为 0 | 创建时机、实际实例、初始快照或 Text Binding Cast | 初始化和增量更新分开验证 |
| 尸体立刻消失 | 蓝图 Destroy/LifeSpan、C++回收时间、GameMode 注销 | 区分“存活计数注销”和“Actor 销毁” |
| 纹理池或 VSM 警告 | 先分类 Texture Streaming 与 Shadow VSM | 单变量实验，不用扩大预算掩盖根因 |

完整问题现象、假设、根因、修复、验证和替代条件见主文档第 16、17、19 节；本表只用于现场排查。

## 13. 完成定义

项目只有同时满足以下条件才算封板：

- [x] 两个零引用旧 UI 模板已清理，`Compile All Blueprints` 为 0 Error。
- [ ] PIE 回归矩阵通过并保存必要截图/日志。
- [x] 完整 Development Editor 编译通过。
- [ ] Shipping Cook/Package 通过。
- [ ] 打包产物冒烟通过，无新致命日志。
- [ ] 文档中的“已完成、已测量、候选方案”边界与实际证据一致。
