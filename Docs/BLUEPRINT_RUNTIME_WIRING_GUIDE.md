# 蓝图运行闭环接线指南

本文只整理编辑器中必须完成的资产配置和蓝图接线。Gameplay 状态与伤害规则已经由 C++ 持有，蓝图不要再复制第二套 Timer、弹药或伤害逻辑。

## 1. 先确认实际运行资产

项目中存在两个同名玩家蓝图：

```text
/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter
/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter
```

带 `IA_Shoot / IA_reload / IA_Aim / IA_Run`、玩家死亡事件和 `PostProcessComponent` 的完整版本是第二个。打开当前关卡的 World Settings，确认 GameMode Override 指向实际使用的 `fpstruegamemode` 蓝图，再在该 GameMode 的 Class Defaults 中把 Default Pawn Class 明确设置为完整版本。不要只依赖 C++ 构造函数的旧模板路径。

当前关卡资产是：

```text
/Game/FactoryDistrict/Maps/Demonstration
```

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
-> Reload Commit Notify
-> WeaponComponent::CommitReload()

Montage Completed
-> WeaponComponent::FinishReload()

Montage Interrupted
-> WeaponComponent::CancelReload()
```

`CommitReload()`、`FinishReload()` 和 `CancelReload()` 都有状态门禁，重复回调不会重复提交弹药。蓝图不要在 Montage 开始时直接加满弹匣，也不要用 Delay 维护另一份 `IsReloading`。C++ Timer 只负责资源漏 Notify 时的超时恢复。

换弹中开火会被 `WeaponComponent::CanFire()` 拒绝。正确的“不可打断”不是强行禁止 Montage 被覆盖，而是任何输入、动画中断或死亡都不能绕过武器状态机。

## 5. 敌人蓝图与 Montage

`enemy_BP` 的父类必须是 `AfpstrueEnemyCharacter`。删除或断开旧的 Tick 追击、循环 Timer、`AI MoveTo` 和蓝图直接 `ApplyDamage`，移动与攻击决策由 `AfpstrueEnemyAIController` 处理。

攻击接线：

```text
Event OnAttackStarted
-> Random Integer / Select
-> 选择一个正式攻击 Montage
-> Play Montage

Play Montage OnCompleted
Play Montage OnBlendOut
Play Montage OnInterrupted
-> HandleAttackFinishedNotify
```

`FinishAttack()` 在 C++ 中是幂等的，因此多个收尾回调汇入同一个函数是允许的。Timer 仍保留为 Montage 回调缺失时的最长时限兜底。

每个被随机选择的正式攻击 Montage：

1. 在真正有杀伤力的阶段放置 `Add Notify State -> Enemy Attack Window`。
2. 检查 Skeleton 上存在 `weapontop` 和 `weaponend`。
3. 同一 Montage 删除旧 `Enemy Attack Hit`，防止两套伤害入口并存。
4. 不在 Notify 图或敌人蓝图里直接 `ApplyDamage`。

当前旧单点 Notify 仍被 `EnemyWarrior_DoubleLightAttack_InP_Montage` 引用，但该资产不在 `enemy_BP` 的正式随机攻击列表中。先完成资产迁移和保存，再删除旧 C++ 类。

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
