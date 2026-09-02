# fpstrue

基于 Unreal Engine 5.5 和 C++ 开发的单机 PvE FPS Demo。项目已经形成角色控制、武器战斗、敌人 AI、群体围攻、波次结算和 HUD 的完整玩法闭环，并围绕多敌人场景建立了可重复的性能测试与消融流程。

> 本仓库用于源码与性能分析展示，不包含地图、模型、动画、音频等二进制或第三方 `Content` 资产。
> 因此可以直接阅读和编译 C++ 模块，但若要还原完整画面与关卡，需要自行提供具有合法授权的对应资源。

## 项目内容

- 第一人称移动、跳跃、冲刺、瞄准和武器拾取。
- 自动射击、弹药与换弹、Hitscan 部位伤害、散布和后坐力。
- 玩家与敌人复用生命组件，统一处理受伤、死亡和一次性结算。
- 敌人目标判断、NavMesh 寻路、近战攻击窗口和死亡布娃娃。
- 双环站位、稳定槽位、最多 8 个并发攻击者和 MoveTo 请求限流。
- 三波敌人生成、倒计时、存活数量和胜负事件。
- HUD 通过委托接收生命、弹药和对局状态变化，避免每帧函数绑定。

## 代码结构

| 模块 | 职责 |
| --- | --- |
| `AfpstrueCharacter` | 输入、移动、瞄准、当前武器和玩家死亡协调 |
| `UfpstrueWeaponComponent` | 射击、弹药、换弹事务、散布、后坐力和命中查询 |
| `UfpstrueHealthComponent` | 通用伤害、生命值和幂等死亡事件 |
| `AfpstrueEnemyCharacter` | 敌人表现载体、近战检测、受击和死亡生命周期 |
| `UfpstrueEnemyCombatComponent` | 敌人攻击状态、攻击名额和战斗收口 |
| `AfpstrueEnemyAIController` | Timer 驱动的状态决策、寻路去重和失败退避 |
| `AfpstrueSurroundManager` | 围攻槽位、导航投影、攻击名额和无效引用清理 |
| `AfpstrueGameMode` | 波次、分帧生成、敌人注册、倒计时和胜负 |
| Significance / Animation Sharing Coordinator | 集中计算敌人等级并向移动、动画、阴影和骨骼光追消费者下发策略 |

### 源码快速导航

以下链接可以直接打开核心实现，避免在 `Source/fpstrue/` 中逐个查找：

| 功能 | 头文件 | 实现文件 | 重点入口 |
| --- | --- | --- | --- |
| 玩家输入、移动与组件协调 | [fpstrueCharacter.h](Source/fpstrue/fpstrueCharacter.h) | [fpstrueCharacter.cpp](Source/fpstrue/fpstrueCharacter.cpp) | `SetupPlayerInputComponent`、`StartWeaponFire`、`StartReload`、`HandleDeath` |
| 玩家武器、射击与换弹 | [fpstrueWeaponComponent.h](Source/fpstrue/fpstrueWeaponComponent.h) | [fpstrueWeaponComponent.cpp](Source/fpstrue/fpstrueWeaponComponent.cpp) | `StartFire`、`FireLineTrace`、`RequestReload`、`CommitReload` |
| 玩家与敌人复用生命组件 | [fpstrueHealthComponent.h](Source/fpstrue/fpstrueHealthComponent.h) | [fpstrueHealthComponent.cpp](Source/fpstrue/fpstrueHealthComponent.cpp) | `HandleOwnerTakeAnyDamage`、`ApplyDamageInternal` |
| 单敌人状态决策与寻路 | [fpstrueEnemyAIController.h](Source/fpstrue/fpstrueEnemyAIController.h) | [fpstrueEnemyAIController.cpp](Source/fpstrue/fpstrueEnemyAIController.cpp) | `UpdateAI`、`GetNextDecisionInterval`、`MoveToGoal` |
| 敌人攻击事务与刀刃检测 | [fpstrueEnemyCombatComponent.h](Source/fpstrue/fpstrueEnemyCombatComponent.h) | [fpstrueEnemyCombatComponent.cpp](Source/fpstrue/fpstrueEnemyCombatComponent.cpp) | `TryAttackTarget`、`UpdateAttackWindow`、`SweepWeaponSegment`、`FinishAttack` |
| 攻击动画有效窗口 | [fpstrueAnimNotifyState_AttackWindow.h](Source/fpstrue/fpstrueAnimNotifyState_AttackWindow.h) | [fpstrueAnimNotifyState_AttackWindow.cpp](Source/fpstrue/fpstrueAnimNotifyState_AttackWindow.cpp) | `NotifyBegin`、`NotifyTick`、`NotifyEnd` |
| 群体槽位与全局请求预算 | [fpstrueSurroundManager.h](Source/fpstrue/fpstrueSurroundManager.h) | [fpstrueSurroundManager.cpp](Source/fpstrue/fpstrueSurroundManager.cpp) | `RequestSurroundSlot`、`TryAcquireAttackPermission`、`TryConsumeMoveRequestBudget` |
| 敌人角色、LOD与渲染策略落地 | [fpstrueEnemyCharacter.h](Source/fpstrue/fpstrueEnemyCharacter.h) | [fpstrueEnemyCharacter.cpp](Source/fpstrue/fpstrueEnemyCharacter.cpp) | `ApplySignificance`、`EvaluateRenderSignificance`、`ApplyRenderSignificanceSettings`、`HandleDeath` |
| Significance集中采样和预算分配 | [fpstrueEnemySignificanceCoordinator.h](Source/fpstrue/fpstrueEnemySignificanceCoordinator.h) | [fpstrueEnemySignificanceCoordinator.cpp](Source/fpstrue/fpstrueEnemySignificanceCoordinator.cpp) | `Update`、候选排序、Full/Shadow/RT预算下发 |
| Animation Sharing适配 | [fpstrueEnemyAnimationSharingCoordinator.h](Source/fpstrue/fpstrueEnemyAnimationSharingCoordinator.h) | [fpstrueEnemyAnimationSharingCoordinator.cpp](Source/fpstrue/fpstrueEnemyAnimationSharingCoordinator.cpp) | `BuildRuntimeSetup`、`RefreshEnemyRegistration`、`SuspendEnemy` |
| 波次、注册表与模块装配 | [fpstrueGameMode.h](Source/fpstrue/fpstrueGameMode.h) | [fpstrueGameMode.cpp](Source/fpstrue/fpstrueGameMode.cpp) | `StartGame`、`SpawnNextQueuedEnemy`、`RegisterEnemy`、`FinishGame` |
| Benchmark命令行配置 | [fpstrueBenchmarkConfig.h](Source/fpstrue/fpstrueBenchmarkConfig.h) | [fpstrueBenchmarkConfig.cpp](Source/fpstrue/fpstrueBenchmarkConfig.cpp) | 消融参数解析与策略覆盖 |
| Benchmark执行和消费者计数 | [fpstrueBenchmarkRunner.h](Source/fpstrue/fpstrueBenchmarkRunner.h) | [fpstrueBenchmarkRunner.cpp](Source/fpstrue/fpstrueBenchmarkRunner.cpp) | `BeginBenchmark`、`StartCapture`、`ApplyDiagnosticOverrides` |
| 项目性能埋点 | [fpstruePerformanceStats.h](Source/fpstrue/fpstruePerformanceStats.h) | — | AI、生成、攻击Sweep的Stat与CSV指标声明 |

性能结论和截图入口：

- [完整实验与消融证据](PERFORMANCE_EVIDENCE.md)
- [160敌人Unreal Insights截图](PerformanceEvidence/UnrealInsights_160Enemies.png)
- [性能测试脚本目录](Tools/)

核心玩法状态由 C++ 维护；蓝图和 UMG 通过委托、Blueprint 事件及只读查询完成动画、音效、特效和界面表现。

## 多敌人性能策略

项目使用 `stat`、CSV Profiler 和 Unreal Insights 按 Game Thread、Render Thread、RHI Thread 与 GPU 分层定位问题。当前已经实现：

- 根据目标距离和战斗状态调整 AI 决策与移动更新节奏。
- 将渲染可见性与玩法重要性分开评估，限制高成本渲染消费者数量。
- 普通敌人接入 Animation Sharing，减少重复动画求值。
- 敌人分帧生成，死亡后延迟回收，避免集中生成和长期尸体累积。
- 使用固定地图、分辨率、随机种子、预热时间和采样时间运行自动矩阵及单变量消融。

在 Development Editor、`Demonstration` 地图、1600×900、关闭 VSync、固定随机种子并完成预热的当前样本中，160 个敌人的 Game Thread 平均耗时为 `7.707 ms`。该数值只描述当前测试环境下的游戏线程开销，不代表整帧已经稳定达到 60 FPS。

### 历史趋势对照（非严格 A/B）

下面的数据来自早期版本与当前版本的两轮 160 敌人测试。两轮都使用 `Demonstration` 地图、1600×900、独立 Development Editor 进程、关闭 VSync，并采样约 30 秒；但它们**不是严格的单变量 A/B 实验**：测试二进制不同，预热时间由 10 秒调整为 15 秒，当前版本才固定随机种子 `1337`，且每个版本只有一次运行。Render Thread / RHI Thread 还存在明显的运行级波动。因此，这组数据只用于展示项目演进趋势，不能把全部差值归因于 Significance 或某一个具体优化，也不能据此声称整体性能提升了同样比例。

| 整体指标 | 早期版本 | 当前版本 | 变化 |
| --- | ---: | ---: | ---: |
| Frame | 27.907 ms | 30.899 ms | +10.7% |
| Game Thread | 27.899 ms | 7.707 ms | -72.4% |
| Render Thread | 14.182 ms | 30.411 ms | +114.4% |
| GPU | 14.205 ms | 9.039 ms | -36.4% |

| 敌人相关指标 | 早期版本 | 当前版本 | 变化 |
| --- | ---: | ---: | ---: |
| Character Movement | 6.188 ms | 1.636 ms | -73.6% |
| Animation | 4.448 ms | 0.814 ms | -81.7% |
| Tick Actors | 5.211 ms | 0.682 ms | -86.9% |
| AI Decision | 0.201 ms | 0.039 ms | -80.6% |
| MoveTo 提交/帧 | 14.565 | 0.109 | -99.3% |
| Draw Calls | 3458 | 1786 | -48.4% |

这组对照支持的结论是：敌人移动、动画、AI 和提交频率的 CPU 扩展成本明显下降；但整体 Frame 没有同步改善，当前关键路径转移到了 Render Thread / RHI Thread。

## 环境与运行

源码编译要求：

- Unreal Engine 5.5。
- Visual Studio 2022，并安装“使用 C++ 的游戏开发”和对应 Windows SDK。

可先生成 Visual Studio 项目文件并编译 `fpstrueEditor` 的 Development Editor 配置。完整开发环境中的编辑器与游戏默认地图为：

```text
/Game/FactoryDistrict/Maps/Demonstration
```

公开仓库未分发该地图及其依赖资产；上述路径仅用于说明源码与配置的原始运行入口。完整开发环境进入地图后，由关卡或 UI 调用 `Start GameMode` 启动正式波次流程。

## 性能测试

完整的实验分层、原始证据对应关系、有效参数与未验证参数见 [PERFORMANCE_EVIDENCE.md](PERFORMANCE_EVIDENCE.md)。

规模矩阵：

```powershell
.\Tools\RunPerformanceMatrix.ps1 -Counts 20,80,160 -WarmupSeconds 10 -DurationSeconds 30 -BenchmarkSeed 1337
```

80 敌人消费者消融：

```powershell
.\Tools\RunEnemyOptimizationAblation.ps1 -EnemyCount 80 -RunsPerGroup 3 -UnverifiedConsumersOnly
```

脚本默认使用本机 UE 与项目绝对路径；换机器运行前需要修改脚本顶部的 `$ProjectRoot` 和 `$Editor`。运行结果写入 `Saved/Profiling/`，该目录不提交到 Git。

## 仓库目录

```text
Config/                         项目、输入和默认地图配置
Source/fpstrue/                 C++ Runtime 模块
Tools/                          性能采集、消融和资产审计脚本
PerformanceEvidence/            可公开的性能截图
```
