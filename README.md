# fpstrue · UE5 C++ FPS

基于 Unreal Engine 5.5 和 C++ 的单机 PvE FPS Demo，实现射击、换弹、敌人追击与近战、群体围攻、波次结算和事件驱动 HUD 接口，并围绕 160 敌人场景开展性能分析与优化。

项目重点是完整玩法实现、多敌人更新调度，以及从实验数据追到线程依赖的性能归因。

> 本仓库提供源码、配置、测试脚本和性能记录。地图、模型、动画、音频等二进制及第三方 Content 资产不随仓库分发；还原完整关卡需要配置对应资源。

## 项目亮点

- **组件化玩法**：角色负责输入与组件协调，武器维护射击和换弹事务，生命组件统一处理伤害与死亡，敌人 CombatComponent 维护动画攻击窗口和命中去重。
- **多敌人协作**：状态驱动的 AI 决策 Timer、共享围攻槽位、并发攻击许可、MoveTo 去重、失败退避和帧级请求预算。
- **分开调度玩法与渲染**：玩法按距离与攻击状态分级；渲染按相机信息选择 Full、阴影和骨骼光追参与者，普通敌人通过 Animation Sharing 复用姿态。
- **C++ 算法与生命周期**：预计算优先级键、严格弱序比较、有界 Top-K；弱引用注册表、幂等注销、共享动画交换句柄回调与清理边界。
- **可重复验证**：CSV Profiler、Unreal Insights 与任务依赖追踪结合；脚本记录参数与输入指纹，自动检查实验有效性，代码边界由 UE Automation 回归。

## 已验证的性能结果

下表来自同批启用/关闭策略的重复对照，每组各三次。时间单位为 ms；80 敌人与 160 敌人属于不同实验组。

| 场景 | 指标 | 关闭对应策略 | 启用对应策略 | 降幅 |
| --- | --- | ---: | ---: | ---: |
| 160 敌人 | AI Decision | 0.726 | 0.215 | 70.4% |
| 80 敌人 | Character Movement | 1.245 | 1.056 | 15.2% |
| 80 敌人 | Animation | 1.226 | 0.818 | 33.3% |
| 80 敌人 | GPU ShadowDepths | 1.773 | 1.052 | 40.7% |
| 80 敌人 | GPU Skinned BLAS | 0.495 | 0.199 | 59.8% |
| 80 敌人 | 总 RHI Draw Calls / 帧（阴影策略对照） | 2111.8 | 1655.0 | 21.6% |

这些结果验证了决策降频、移动分级、姿态共享和渲染参与限制的局部收益。历史实验采用简化 HUD、声音和玩家受伤的诊断口径；各项独立消融不相加为整帧收益。采集条件、逐组数据与原始报告索引见[消费者消融明细](PERFORMANCE_EVIDENCE.md)和[分阶段实验记录](Docs/Performance/EXPERIMENT_LOG.md)。

另一项关键发现来自 RT 长等待：零敌人对照、Task Trace、源码和查询开关干预共同表明，一类 `WaitForGatherDynamicMeshElements` 长等待的上游是历史遮挡查询结果同步。这使后续工作从继续削减敌人逻辑，转向 GPU 工作分解和 CPU/GPU 同步验证。具体 GPU 子阶段归因与完整玩法的最终稳定帧率仍在验收中。

## 玩法架构

全部代码位于一个 `fpstrue` Runtime 模块内，按业务职责组织目录。

```mermaid
flowchart TD
    GM[GameMode：波次、生成、注册表与胜负]
    PLAYER[玩家 Character：输入与组件协调]
    WEAPON[WeaponComponent：射击与换弹事务]
    HEALTH[HealthComponent：伤害与死亡]
    AI[EnemyAIController：追击与攻击决策]
    ENEMY[EnemyCharacter：组件与表现协调]
    COMBAT[EnemyCombatComponent：攻击窗口与命中]
    GROUP[SurroundManager：槽位、攻击许可、MoveTo预算]
    HUD[HUD：订阅状态变化]
    GM -->|生成并注入上下文| AI
    GM -->|创建共享实例| GROUP
    AI -->|申请资源| GROUP
    AI -->|控制| ENEMY
    ENEMY --> COMBAT
    PLAYER --> WEAPON
    WEAPON -->|提交命中伤害| HEALTH
    COMBAT -->|提交近战伤害| HEALTH
    HEALTH -->|状态事件| HUD
    WEAPON -->|弹药事件| HUD
    GM -->|对局事件| HUD
```

玩家和敌人各自持有生命组件，不共享血量。Controller 决定何时移动和攻击，CharacterMovement 执行移动；CombatComponent 执行攻击事务，SurroundManager 只协调群体资源。

### 射击与换弹

- 玩家输入经过角色接口交给武器；武器检查状态、射速和弹药，执行 Hitscan，再通过 UE 伤害接口交给目标的生命组件。
- `WeaponTrace` 与普通碰撞分开配置：敌人 Mesh 接收射击查询，胶囊不抢先阻挡该通道。
- 换弹使用 Ready/Firing/Reloading/Disabled 状态与独立提交入口；动画通知提交弹药，结束路径处理重复通知和委托重入，角色死亡时停止武器动作与 Timer。
- HUD 订阅生命、弹药和对局事件；C++ 提供状态变化入口，不在 UI 回调中执行伤害或弹药结算。

### 敌人追击与近战

- 每个 AI 用首次错峰的一次性 Timer 安排下一轮决策，根据战斗、追击、远距和空闲状态调整间隔。
- MoveTo 先检查目标变化与失败退避，再申请全局帧级预算；现有路径由导航、PathFollowing 和 CharacterMovement 继续执行。
- 攻击先申请群体许可，再检查停稳与朝向，经角色接口启动 CombatComponent；AnimNotifyState 开启和关闭刀刃 Sweep 窗口，单次攻击命中去重。
- 正常结束、保护 Timer 和死亡清理共用攻击收口路径，归还许可、清除窗口与定时任务。

## 性能优化架构

```mermaid
flowchart TD
    GM[GameMode：策略配置与存活敌人注册表]
    CO[SignificanceCoordinator：集中采样]
    GM --> CO
    CO -->|玩家 Transform| GP[Gameplay：距离评分与攻击保护]
    GP --> AI[AI：下一轮决策间隔]
    GP --> MOVE[Movement：组件更新间隔]
    CO -->|相机快照| SAMPLE[Render：视锥、投影大小、距离与历史]
    SAMPLE --> SELECT[预计算优先级键：Full、光追、阴影 Top-K]
    SELECT --> APPLY[EnemyCharacter：应用档位与参与资格]
    APPLY --> MESH[SkeletalMesh：LOD、动画、阴影与光追]
    APPLY --> SHARE[AnimationSharingCoordinator：注册与退出]
    SHARE --> PLUGIN[UE Animation Sharing：复用 Idle / Moving 姿态]
    PROTECT[战斗动画保护：攻击、近战范围、近期交互]
    PROTECT -->|恢复必要动画与LOD| MESH
    PROTECT -->|退出共享| SHARE
```

### 1. 玩法分级与战斗保护

Gameplay 不读取相机可见性，玩家背后的敌人仍能追击和攻击。UE SignificanceManager 按玩家二维距离评分，角色转换为 Full/Reduced/Background 并下发 AI 与 Movement 更新节奏。

正在攻击或目标进入攻击范围时，Gameplay 保持 Full；战斗附近 AI 使用独立短间隔。攻击入口直接恢复 Movement 和必要骨骼更新配置。受击会刷新动画保护并退出共享；它不直接重排 AI Timer。

### 2. 渲染资格与 Top-K

每轮相机信息只采样一次，敌人样本进入连续候选数组。选择阶段只比较预计算的数值键：主视锥、扩展视锥、评分、对象 ID。比较器处理非有限评分，评分完全相等时才使用 ID，避免近似相等破坏严格弱序。

Full、骨骼光追和阴影分别使用有界 Top-K 堆，替代全部候选排序。堆顶保留当前入选者中优先级最低的一项，新候选更优时替换；三次选择复用同一个内联堆。单项选择需要线性扫描和至多 `O(log K)` 的单次调整，额外选择空间为 `O(K)`。

资格仍有明确依赖：Full 从自然 Full 档位中选择；骨骼光追从最终 Full 且满足距离的对象中选择；阴影要求扩展视锥、距离和非 Background 档位。战斗动画保护独立执行，不额外分配阴影或光追名额。自然渲染档位使用升降双阈值、降档延迟与最短保持时间。

### 3. 动画共享与生命周期

非 Full Render、无战斗保护且存活的普通敌人，可按骨架资格进入 Idle/Moving 共享池。攻击、受击和死亡时退出，恢复独立动画；每个敌人的 AI、移动、血量和伤害判定仍独立维护。

GameMode 用弱引用集合管理存活敌人，死亡、销毁和 EndPlay 共用幂等注销入口，集中更新前清除失效条目。Animation Sharing 区分待交付和已注册句柄；失败注册释放待交付记录，允许重试。

UE 插件注销采用 `RemoveAtSwap`，会同步通知被交换角色的新句柄。回调只保存本地句柄，实际注销在回调外完成，避免重入正在调整的插件数组；退出共享时恢复骨骼 LOD 控制。

### 4. 配置与实验控制

- 玩法参数、更新间隔和渲染预算由现有蓝图可编辑属性提供。
- 共享动画默认软引用放在项目 INI，组件蓝图可以覆盖。
- JSON 只保存实验预设，优先级为显式命令行参数 > JSON > 脚本默认值。
- Runner 负责规模准备、预热、CSV/Trace 采集和有效性检查；脚本保存最终参数、来源和输入指纹。

配置入口与使用方式见 [CONFIGURATION.md](Docs/CONFIGURATION.md)。

## 实验进展与证据

| 阶段 | 解决的问题 | 结果与后续 |
| --- | --- | --- |
| 规模测试与调用链分析 | 敌人增加后，哪些工作随规模增长 | 优先定位移动与动画，区分局部计算和任务等待 |
| 消费者重复对照 | 哪些策略确实减少了工作 | 确认决策、移动、动画共享、阴影与骨骼 RT 的局部收益 |
| 零敌人与任务依赖追踪 | 敌人成本下降后，RT 为什么仍等待 | 追到历史遮挡查询结果同步，转向场景渲染与同步链 |
| 查询策略和 GPU 工作分解 | 缩短等待能否改善整帧 | HZB 因整帧回退未采用；Buffer2 与分辨率诊断继续验收 |

- [分阶段实验记录](Docs/Performance/EXPERIMENT_LOG.md)：实验动机、条件、结果、决策和后续方向。
- [消费者消融与历史数据明细](PERFORMANCE_EVIDENCE.md)：逐组数据和原始报告索引。
- [Unreal Insights 截图](PerformanceEvidence/UnrealInsights_160Enemies.png)：历史 160 敌人样本，仅对应当次采集。

![160 敌人场景的 Unreal Insights 历史采样](PerformanceEvidence/UnrealInsights_160Enemies.png)

完整 CSV、日志与 Trace 保存在本机 `Saved/Profiling/`，公开记录保留摘要和文件索引。失败实验及成本转移同样保留，用于解释策略取舍。

## 源码导航

| 功能 | 主要实现 |
| --- | --- |
| 玩家控制 | [Character](Source/fpstrue/Characters/Player/fpstrueCharacter.cpp) |
| 武器、射击与换弹 | [WeaponComponent](Source/fpstrue/Weapons/fpstrueWeaponComponent.cpp) |
| 通用伤害与生命 | [HealthComponent](Source/fpstrue/Characters/Shared/fpstrueHealthComponent.cpp) |
| 敌人 AI 与路径请求 | [EnemyAIController](Source/fpstrue/Characters/Enemies/fpstrueEnemyAIController.cpp) |
| 攻击事务与命中窗口 | [EnemyCombatComponent](Source/fpstrue/Characters/Enemies/fpstrueEnemyCombatComponent.cpp) |
| 围攻槽位与共享预算 | [SurroundManager](Source/fpstrue/Characters/Enemies/fpstrueSurroundManager.cpp) |
| 角色表现与性能档位 | [EnemyCharacter](Source/fpstrue/Characters/Enemies/fpstrueEnemyCharacter.cpp) |
| Top-K 与预算分配 | [SignificanceCoordinator](Source/fpstrue/Characters/Enemies/fpstrueEnemySignificanceCoordinator.cpp)、[优先级键与堆](Source/fpstrue/Characters/Enemies/fpstrueEnemySignificance.h) |
| 动画共享接入 | [AnimationSharingCoordinator](Source/fpstrue/Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.cpp) |
| 波次、注册表与胜负 | [GameMode](Source/fpstrue/Game/fpstrueGameMode.cpp) |
| 测试与性能采集 | [Automation](Source/fpstrue/Testing/Automation/)、[Benchmarks](Source/fpstrue/Testing/Benchmarks/)、[Tools](Tools/) |

## 构建与验证

环境：Unreal Engine 5.5、Visual Studio 2022、“使用 C++ 的游戏开发”工作负载及 Windows SDK。生成 Visual Studio 项目文件后，编译 `fpstrueEditor` 的 Development Editor 配置。

完整开发环境的地图入口为 `/Game/FactoryDistrict/Maps/Demonstration`，由关卡或 UI 调用 `StartGameMode` 开始正式波次。公开源码需要自行配置地图、角色蓝图与资源引用。

UE Automation 测试组为 `fpstrue.`，可在 Session Frontend → Automation 中运行，或使用：

```powershell
& "<UE5.5目录>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<项目目录>\fpstrue.uproject" -Unattended -NullRHI -NoSplash -NoSound -DDC-ForceMemoryCache '-ExecCmds=Automation RunTests fpstrue.' '-TestExit=Automation Test Queue Empty' -Log
```

8 项自动化测试覆盖朝向与配置、射击结算、换弹重入、严格弱序、Top-K 等价性，以及共享注销和交换句柄。NullRHI 测试验证代码边界，画面与实景性能另做关卡回归。

配置脚本另有 29 项检查，无须启动 UE：

```powershell
.\Tools\TestRenderCostConfig.ps1
.\Tools\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\ExperimentProfiles\baseline160.json -ValidateOnly
```

正式采集前按[配置说明](Docs/CONFIGURATION.md)检查引擎路径、地图和生效参数。固定场景成本采集与正常生命条件下的玩法基线分别记录；Top-K 的算法验证不代替实景性能 A/B。

## 目录结构

```text
Config/                     引擎、输入、碰撞与默认资源配置
Source/fpstrue/
  Game/                     波次、生成、注册表与结算
  Characters/
    Player/                 玩家控制
    Enemies/                AI、近战、围攻、显著性与动画共享
    Shared/                 生命组件与碰撞通道
  Weapons/                  射击、换弹、拾取与动画通知
  Testing/
    Automation/             玩法、算法与生命周期回归
    Benchmarks/             性能配置、采集与埋点
Tools/                      实验采集、分析与配置校验
Docs/Performance/           分阶段实验记录
PerformanceEvidence/        可公开的性能截图
```
