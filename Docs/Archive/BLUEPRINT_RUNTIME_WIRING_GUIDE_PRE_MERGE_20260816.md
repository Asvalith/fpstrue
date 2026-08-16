# 蓝图运行闭环接线指南

> 文档身份：2026-08-16 合并前蓝图接线原文，已归档。当前接线与 PIE 回归只看 [PROJECT_TASK_CHECKLIST.md](../PROJECT_TASK_CHECKLIST.md)。
>
> 当前门禁：先修复 `WB_Healthbar` 与 `WB_BaseButton`，使 `Compile All Blueprints` 达到 0 Error，再按本文回归完整玩法。

本文只整理编辑器中必须完成的资产配置和蓝图接线。Gameplay 状态与伤害规则已经由 C++ 持有，蓝图不要再复制第二套 Timer、弹药或伤害逻辑。

## 1. 先确认实际运行资产

项目中存在两个同名玩家蓝图：

```text
/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter
/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter
```

带 `IA_Shoot / IA_reload / IA_Aim / IA_Run`、玩家死亡事件和 `PostProcessComponent` 的完整版本是第二个。C++ fallback、编辑器启动地图、独立运行地图和全局 GameMode 已统一到这套正式资产。当前关卡的 World Settings 仍应显式指向 `fpstruegamemode`，该 GameMode 的 Class Defaults 也应明确显示完整玩家蓝图，避免以后改项目默认值时关卡行为悄悄变化。

当前关卡资产是：

```text
/Game/FactoryDistrict/Maps/Demonstration
```

第一部分的入口关系固定为：

```text
Project Settings / Maps & Modes
-> Editor Startup Map = Demonstration
-> Game Default Map = Demonstration
-> Default GameMode = fpstruegamemode

Demonstration / World Settings
-> GameMode Override = fpstruegamemode

fpstruegamemode / Class Defaults
-> Default Pawn Class = 完整 BP_FirstPersonCharacter
-> Enemy Class = enemy_BP
-> Game Duration = 90
```

这一部分的架构边界：

| 位置 | 只保留什么 | 不应该出现什么 |
| --- | --- | --- |
| Project Settings | 默认地图和默认 GameMode | 波次、倒计时或敌人引用 |
| World Settings | 当前关卡的 GameMode Override | 重复配置一套玩法规则 |
| `fpstruegamemode` 蓝图 | Default Pawn、EnemyClass 和可调参数 | Tick、生成循环、倒计时蓝图 Timer |
| Demonstration Level Blueprint | 菜单转游戏、创建 HUD、调用一次 `StartGameMode` | 自己生成敌人、维护剩余时间或判断胜负 |
| C++ GameMode | 前置校验、波次、Timer、注册表和胜负 | Montage、HUD 文本和角色表现 |

优化思路是把 Level Blueprint 从“玩法管理器”降为“关卡入口适配器”。Level Blueprint 知道本关卡何时结束菜单演出，但不知道一波有几个敌人、倒计时如何递减，也不保存游戏是否已经结束。规则全部落到 GameMode 后，重新开始会通过新 World 自动得到新状态，换关卡也只需替换数据配置。

开始 PIE 前检查：

- GameMode Override 是 `fpstruegamemode`，父类为 `AfpstrueGameMode`。
- Default Pawn Class 是完整玩家蓝图。
- Enemy Class 是 `/Game/FirstPerson/Blueprints/enemy/enemy_BP`。
- Surround Manager Class 有效；默认可以使用 C++ `AfpstrueSurroundManager`。
- 关卡至少有 4 个 `TargetPoint`，每个 Actor Tags 中包含 `EnemySpawn`。
- `EnemySpawn` 必须写在 Actor Tags，不是 Component Tags。
- `NavMeshBoundsVolume` 覆盖玩家、出生点和主要移动区域，按 `P` 能看到连续绿色区域。

`StartGameMode()` 任一前置条件失败都会立即结束：Enemy Class 未配置、出生点少于 4 个、玩家或 HealthComponent 无效、SurroundManager 创建失败。此时先看 Output Log 中的 `StartGameMode failed:`，不要继续猜 UI 或倒计时节点。

## 2. 开始游戏与 HUD

菜单按任意键后的推荐执行顺序：

```text
Any Key
-> 移除主菜单 Widget
-> Create Widget(ingame)
-> Add To Viewport
-> ingame 初始化并绑定 GameMode / Player / Weapon 事件
-> Set Input Mode Game Only
-> Get Game Mode
-> Cast To fpstrueGameMode
-> Start GameMode
```

当前 `Demonstration` 已包含 `Get Game Mode -> Cast -> Start GameMode`，第一部分只需要整理成一条执行链：

```text
Any Key Pressed
-> Do Once
-> 菜单退出与视角切换
-> Create Widget(ingame) / Add To Viewport
-> Set Input Mode Game Only
-> Get Game Mode
-> Cast To fpstrueGameMode
-> Start GameMode
```

`Delay` 只有在等待菜单淡出或镜头 Blend 时才保留；它不能用于等待 GameMode“准备好”。GameMode 在关卡开始时已经存在。如果没有必须等待的视觉演出，删除这段 Delay，让输入到玩法启动的时序可预测。

临时诊断接线：

```text
Cast Failed
-> Print String("Wrong GameMode")

Start GameMode
-> Is Game Running
-> Branch
   True  -> Print String("Game started")
   False -> 查看 Output Log 中的 StartGameMode failed
```

验收完成后删除或注释这两个 `Print String`，备注“入口测试用”。不要把 Start 后无条件执行的 Print 当作成功证据，因为函数内部前置校验失败时也会返回到蓝图执行链。

HUD 应先绑定事件，再启动 GameMode，这样不会错过 `StartGameMode()` 立即广播的 90 秒、波次 0 和存活数 0。

`ingame` Widget 初始化：

```text
Event Construct
-> Get Game Mode
-> Cast To fpstrueGameMode
-> Bind Event to OnRemainingTimeChanged
-> Bind Event to OnWaveChanged
-> Bind Event to OnAliveEnemyCountChanged
-> Bind Event to OnGameResult
```

事件接线：

- `OnRemainingTimeChanged(RemainingTime)` -> `ToText(Integer)` -> `SetText`。
- `OnWaveChanged(CurrentWave, TotalWaves)` -> `Format Text "{Current}/{Total}"` -> `SetText`。
- `OnAliveEnemyCountChanged(AliveEnemyCount)` -> `ToText(Integer)` -> `SetText`。
- `OnGameResult(bPlayerWon)` -> 显示胜利或失败面板，切到 UI Only，显示鼠标。

不要使用 Text Property Binding 或 Widget Tick 每帧查询 GameMode。重新开始按钮使用 `Open Level` 打开当前关卡，确保新 World 创建新的 GameMode。

## 3. 玩家蓝图

在完整 `BP_FirstPersonCharacter` 的 Class Defaults 中确认输入资产：

```text
DefaultMappingContext
JumpAction / MoveAction / LookAction
FireAction = IA_Shoot
ReloadAction = IA_reload
RunAction = IA_Run
AimAction = IA_Aim
```

C++ 已经绑定 Enhanced Input。蓝图中不要再添加另一套开火、换弹或移动输入 Timer。

表现事件：

- `Event OnAimChanged`：只处理 FOV、手臂位置或准星表现。
- `Event OnWeaponEquipped(WeaponComponent)`：保存当前组件引用，绑定武器 Delegate，并立即读取一次弹药快照刷新 HUD。
- `Event OnPlayerHealthChanged(NewHealth)`：更新血条和血量文字。
- `Event OnPlayerDamaged`：播放受击音效、Camera Shake 和受击后处理 Timeline。
- `Event OnPlayerDied`：停止受击 Timeline，播放死亡后处理和死亡 UI。

玩家死亡的规则已经在 C++ 中执行：只处理一次、停止移动、禁用武器、广播 `OnPlayerDeathReported` 给 GameMode。`OnPlayerDied` 不再判断是否失败，也不要再次调用 GameMode 结算。

后处理 Timeline 建议：受击使用短促的 `0 -> 1 -> 0` 混合权重，死亡使用独立的 `0 -> 1` 并保持。两条 Timeline 必须在死亡时停止竞争同一个 PostProcess Blend Weight。

## 4. 武器蓝图

在 `OnWeaponEquipped` 中对传入的 `WeaponComponent` 绑定：

- `OnAmmoChanged`
- `OnWeaponFirePerformed`
- `OnWeaponDryFire`
- `OnWeaponReloadStarted`
- `OnWeaponReloadFinished`
- `OnWeaponReloadCanceled`
- `OnWeaponTraceFinished`

### 4.1 开火表现

`OnWeaponFirePerformed` 只播放一次枪口火焰、开火音效、射击 Montage、抛壳和 Camera Shake。蓝图不能扣弹药，也不能再次执行 Line Trace。

`OnWeaponTraceFinished`：

```text
TraceStart / TraceTarget
-> 生成曳光或弹道表现

bHit == true
-> 使用 HitResult 的 ImpactPoint / ImpactNormal
-> 生成命中特效、贴花或表面音效
```

### 4.2 换弹事务

```text
OnWeaponReloadStarted(bWasEmptyReload)
-> 按 bWasEmptyReload 选择普通或空仓 Montage
-> Play Montage

Montage 的弹匣插入帧
-> Add Notify -> Reload Commit（`UfpstrueAnimNotify_ReloadCommit`）
-> WeaponComponent::CommitReload()

Montage Completed
-> WeaponComponent::FinishReload()

Montage Interrupted
-> WeaponComponent::CancelReload()
```

`CommitReload()`、`FinishReload()` 和 `CancelReload()` 都有状态门禁，重复回调不会重复提交弹药。不要把 `OnBlendOut` 接到 `FinishReload()`，Blend Out 早于 Montage 真正完成，会提前解锁开火。蓝图不要在 Montage 开始时直接加满弹匣，也不要用 Delay 维护另一份 `IsReloading`。当前 `Play Montage` 代理节点使用 `OnCompleted/OnInterrupted` 收口，C++ 的 5 秒 Timer 只做回调丢失时的安全恢复。

换弹中开火会被 `WeaponComponent::CanFire()` 拒绝。正确的“不可打断”不是强行禁止 Montage 被覆盖，而是任何输入、动画中断或死亡都不能绕过武器状态机。

## 5. 敌人蓝图与 Montage

`enemy_BP` 的父类必须是 `AfpstrueEnemyCharacter`。删除或断开旧的 Tick 追击、循环 Timer、`AI MoveTo` 和蓝图直接 `ApplyDamage`，移动与攻击决策由 `AfpstrueEnemyAIController` 处理。

在 `enemy_BP -> Class Defaults -> Pawn` 中确认：

```text
AI Controller Class = fpstrueEnemyAIController
Auto Possess AI = Placed in World or Spawned
```

动态生成后 GameMode 会在 Controller 缺失时调用 `SpawnDefaultController()`；如果蓝图覆盖成其他 Controller，Output Log 会明确报告实际类型。

攻击接线：

```text
Event OnAttackStarted
-> Random Integer / Select
-> 选择一个正式攻击 Montage
-> Play Montage

Play Montage OnCompleted
Play Montage OnInterrupted
-> HandleAttackFinishedNotify
```

`FinishAttack()` 在 C++ 中是幂等的，因此完成和中断回调汇入同一个函数是允许的。不要用 `OnBlendOut` 提前结束攻击状态。当前 `Play Montage` 代理节点使用完成和中断回调收口，Timer 使用 5 秒安全时限兜底。攻击冷却从本轮攻击真正结束后开始计算。

每个被随机选择的正式攻击 Montage：

1. 在真正有杀伤力的阶段放置 `Add Notify State -> Enemy Attack Window`。
2. 检查 Skeleton 上存在 `weapontop` 和 `weaponend`。
3. 同一 Montage 删除旧 `Enemy Attack Hit`，防止两套伤害入口并存。
4. 不在 Notify 图或敌人蓝图里直接 `ApplyDamage`。

旧单点 `Enemy Attack Hit` 仅保留兼容性。它不再会因为一次挥空而阻止后续刀刃窗口检测，但正式攻击 Montage 仍应只使用 `Enemy Attack Window`，避免两条检测路径并存。

敌人表现事件：

- `OnAttackLanded`：播放命中音效或轻量反馈。
- `OnAttackMissed`：可选挥空反馈。
- `OnAttackFinished`：只做表现清理。
- `OnEnemyDamaged`：随机播放受击 Montage，不判断死亡。致死伤害不会进入这个普通受击事件。
- `OnEnemyDied`：停止 Montage，Mesh 切 `Ragdoll` Profile，开启 Query And Physics 与 `Set Simulate Physics(true)`，再播放死亡音效/特效。

死亡时 C++ 已停止 AI、移动和攻击窗口，并关闭 Capsule；蓝图负责 Mesh 的布娃娃表现。Physics Asset 无效时，`Set Simulate Physics` 不会得到正确尸体效果，需要在 Physics Asset Editor 修复，而不是在死亡事件里重复加逻辑。

## 6. 近战查询设置

不新增专用近战碰撞通道。当前 C++ 使用 `SweepMultiByObjectType(ECC_Pawn)`，忽略攻击者自身，只对当前 `TargetCharacter` 提交伤害，并按整轮攻击去重。

编辑器只需保证：

- 玩家 Capsule 的 Object Type 为 Pawn，且 Query 可用。
- 敌人 Skeleton Socket 名称与 C++ 默认值一致。
- 攻击窗口外不启用任何第二套武器伤害碰撞。
- `bDrawAttackTrace` 只在调试时开启，验收后关闭。

薄墙和门框属于当前已知边界测试。若复现隔墙命中，先记录用例，再在 C++ 伤害提交前增加现有 Visibility/LOS 校验；本轮不改项目碰撞通道。

## 7. 当天验收顺序

按以下顺序测试，前一项失败时先修前一项：

1. 打开 `Demonstration`，确认 World Settings 的 GameMode 和 Default Pawn。
2. 按 `P` 检查 NavMesh，确认至少 4 个带 `EnemySpawn` Actor Tag 的 TargetPoint 在绿色区域。
3. PIE 后按任意键，确认 `Start GameMode` 执行，倒计时立即显示 90 并每秒减 1。
4. Output Log 出现 `Wave 1/3 started` 和 `Enemy spawned`，场上生成第一波敌人。
5. 敌人绕障追击，进入近战范围后播放 Montage；起手和收招不扣血，有效窗口只扣一次。
6. 射击、空仓、连续射击、普通换弹、空仓换弹和换弹中开火逐项测试。
7. 玩家死亡时立即失败，即使倒计时仍有剩余；倒计时归零且玩家仍存活时胜利。
8. 胜负后敌人停止 AI，重新开始后计时、波次、UI 和武器状态全部初始化。
9. 关闭所有 Debug Draw、Print String 和测试屏幕消息，再保存蓝图与关卡。

## 8. 最小故障定位

| 现象 | 第一检查点 |
| --- | --- |
| `Start GameMode` 后时间仍为 0 | Widget 是否绑定了实际 GameMode 实例；绑定是否发生在 Start 之前 |
| 倒计时开始但没有敌人 | Output Log；EnemyClass；至少 4 个 `EnemySpawn` Actor Tag；SpawnActor 日志 |
| 敌人生成但不移动 | NavMesh；AIControllerClass；Auto Possess AI；旧蓝图 AI 节点是否冲突 |
| 敌人攻击但不扣血 | Montage 是否有 Attack Window；Socket 是否存在；玩家 Capsule 是否为 Pawn |
| 一刀扣多次 | 同一 Montage 是否还保留旧 Enemy Attack Hit 或蓝图 ApplyDamage |
| 换弹被打断后还能开枪 | Interrupted 是否接 CancelReload；是否还有蓝图自建开火 Timer/弹药变量 |
| 死亡后仍追击或攻击 | 是否使用正确 C++ 父类；旧 Tick/Timer/AI MoveTo 是否仍在运行 |

## 9. 2026-08-14 最终接线补充

本节只追加当天实际确认过的接线边界，不替换前面的完整说明。

### 9.1 敌人 BeginPlay

删除旧 `Set Timer by Function Name(chase player)` 后，不能把它前后的动画初始化执行线一起删除。最终保留：

```text
BeginPlay
-> Mesh / Get Anim Instance
-> Cast To enemy_anim
-> 保存 As Enemy Anim
-> 初始化速度和动画
```

追击由 `fpstrueEnemyAIController` 自动启动，不需要蓝图调用 Chase：

```text
AI Controller Class = fpstrueEnemyAIController
Auto Possess AI = Placed in World or Spawned
```

### 9.2 剩余时间文字绑定

当前项目保留原有 UMG Text Binding，不再额外绑定 `OnRemainingTimeChanged`。绑定函数必须让 Cast 位于白色执行路径中：

```text
Get_剩余时间文字_Text
-> Cast To fpstrueGameMode
-> Return Node
```

数据连接：

```text
Get Game Mode.ReturnValue -> Cast.Object
Cast.As fpstrueGameMode -> Get Remaining Time.Target
Get Remaining Time.ReturnValue -> To Text(Integer) -> Return Value
```

不要使用 `ToText(Object)`；不要创建 `TextRenderComponent::SetText`。如果改成手动 SetText，Target 必须是设计器中勾选了 Is Variable 的实际 TextBlock/RichTextBlock。

### 9.3 换弹 Montage

```text
OnWeaponReloadStarted
-> 同时播放手臂和武器换弹 Montage

Reload Commit Notify
-> Current WeaponComponent.CommitReload

手臂 Montage Completed
-> Current WeaponComponent.FinishReload

手臂 Montage Interrupted
-> Current WeaponComponent.CancelReload
```

组件 Target 不能接 TP Weapon Mesh。武器 Montage 可以并行播放，但只选择一条权威完成回调提交 `FinishReload/CancelReload`，避免两个 Montage 竞争收口。

### 9.4 结果界面

```text
OnGameResult(bPlayerWon)
-> Create Widget(winorfail)
-> 设置创建实例的 bPlayerWon
-> Add To Viewport（Target = ReturnValue）
-> Remove ingame
-> Set Input Mode UI Only
-> Show Mouse Cursor = true
```

`winorfail` 内只读取结果并更新文字，不再次调用 `OnGameResult`，避免递归和无限循环。
