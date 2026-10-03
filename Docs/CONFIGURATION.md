# 配置入口：玩法资产、蓝图默认值与实验预设

波次、敌人近战和武器数值使用小型 Data Asset，其他角色参数与动画表现保留原蓝图入口；JSON 只管理性能实验，INI 只提供项目级动画素材默认值。没有通用配置管理器，也没有运行时热更新。

## 玩法资产：整组选择一个配置来源

建立或迁移项目资产时，使用以下对应关系：

| 配置类型 | 配置资产路径 | 绑定位置 |
|---|---|---|
| `UfpstrueWaveConfiguration` | `/Game/Config/DA_FPMatchWaves` | `/Game/FirstPerson/Blueprints/gamemode/fpstruegamemode` 的 `WaveConfiguration` |
| `UfpstrueEnemyCombatConfig` | `/Game/Config/DA_FPEnemyCombat` | `/Game/FirstPerson/Blueprints/enemy/enemy_BP` 的 CombatComponent → `CombatConfiguration` |
| `UfpstrueWeaponConfig` | `/Game/Config/DA_FPWeapon_RU74` | `/Game/FirstPerson/Blueprints/weapon/BP_Weapon` 的 `TP_Weapon` 组件 → `WeaponConfiguration` |

- **波次资产**接管默认敌人类、显式波次数组、波次间隔和对局时长。每波未指定敌人类时，只回退到同一资产的默认类；空波次或缺失所需敌人类会使开局失败，不会偷偷读取旧蓝图补齐。
- **近战资产**集中攻击距离、伤害、间隔、动画结束保护与刀刃采样参数。CombatComponent 在 `BeginPlay` 整组复制到运行时参数，不在每次攻击时读资产。攻击是否正在进行、当前窗口、已命中对象等仍是各敌人独立状态。
- 波次、近战的资产引用未指定时，继续使用现有蓝图字段，保留旧资产兼容性；一旦指定，就在资产中修改相应参数，不再同时修改旧字段。出生点、角色转速、群体槽位与渲染预算不属于这两份资产。
- 迁移时先复制当前实际蓝图值，再绑定资产；保存后重开对局检查生效值。仅新增 C++ 配置类型并不等于已完成资产迁移，必须核对实际关卡使用的 GameMode、敌人蓝图及绑定引用。

本地关卡已通过 `Tools/Gameplay/MigrateGameplayConfiguration.py` 完成复制与绑定，保留原波次数量 `[5, 7, 9]`、90 秒对局时长和全部近战参数。迁移前的两个蓝图备份位于 `Saved/Backups/GameplayConfigurationMigration/`；`Saved/Logs/MigrateGameplayConfiguration.log` 记录保存结果。新开关卡的日志会输出实际配置资产路径，便于确认改的是当前生效的配置。完整工程需同步对应 Content/LFS 资产；只有 Source 归档的环境，应先取得原资产，再按需要运行迁移脚本。

### 武器：数值在资产，动作和弹药在实例

打开 `/Game/Config/DA_FPWeapon_RU74`，在 `Settings` 中按分类编辑挂点、射速、射线距离/冲量、普通与关键部位伤害、关键骨骼、弹匣/初始备弹、散布、后坐力和换弹兜底，共 25 项。新增武器时复制这个资产，再在对应武器蓝图组件上指定 `WeaponConfiguration`；不需要修改 C++ 或增加配置管理器。

- `BP_Weapon` 已绑定该资产，原有 25 项组件数值字段已移除，不存在“资产与蓝图旧数值谁优先”的双入口。原生未绑定组件使用 `FFPWeaponSettings` 的完整默认值；真实资产测试检查 BP 绑定，防止意外落回默认值。
- 首次成功装备时校验并复制整组参数。编辑器“验证资产”和运行时装备共用合法性检查，拒绝非有限数、越界数值等；装备失败不发布装备关系或初始化弹药。没有读取磁盘的逐帧路径。
- 装备后读取该实例的数值快照；修改共享资产不会中途改变射速、弹匣容量或正在进行的换弹。调参后重新开始对局或生成新武器以生效。卸下再装备同一件武器保留快照与剩余弹药，不补满。
- 当前弹药、动作状态、换弹编号、播放身份、已提交标记与 Timer 仍只属于组件实例。多个武器可以引用同一资产，但不共享这些运行状态。
- `ReloadDuration` / `EmptyReloadDuration` 是兜底计时依据，正常装填仍由带身份的 Notify 提交；超时不补弹。手臂/枪械 Montage、音效和特效仍在蓝图选择，本次不改原播放接线。

迁移原样保留蓝图实际数值（600 发/分钟、30 发弹匣、90 发备弹等）。原包与 25 项完整导出位于 `Saved/Backups/WeaponConfigurationMigration/`。`Tools/Gameplay/MigrateWeaponConfiguration.py` 分为旧模块 `Export`、新模块 `Migrate`、新进程 `Verify` 三种模式；原备份和已存在配置禁止覆盖。**当前项目已迁移，日常调参直接编辑资产，不重跑迁移。** 旧版本升级必须先在删除旧属性前导出，并同时交付新配置资产、已绑定蓝图和新代码；仅更新源码不会替旧包迁移数值。

迁移验证：新进程逐项读回 25 项数值与原导出一致；Development Editor / Game 均编译通过，43 项自动化测试通过（0 失败、0 未运行）。其中新增配置合法性、独立快照与重新装备回归，真实 `BP_Weapon` 测试同时验证配置绑定及普通/空仓 Montage 换弹。报告在 `Saved/Automation/WeaponConfiguration/`，日志在 `Saved/Logs/WeaponConfig*.log`；NullRHI 功能回归不代替画面验收或完整打包测试。

## 其余玩法参数：继续在蓝图中编辑

打开实际使用的敌人/AIController 蓝图，进入 Class Defaults 搜索以下属性：

| 所属类 | 属性 | 默认值 | 用途 |
|---|---|---:|---|
| EnemyCharacter | MovementYawRotationRate | 540 度/秒 | CharacterMovement 的转向速度，替换 BeginPlay 写死的数值 |
| EnemyAIController | CombatResponseRangeMultiplier | 1.5 | 有效攻击范围的倍率；此范围内维持战斗响应频率 |
| EnemyAIController | PursuitAcceptanceRangeMultiplier | 0.8 | 共享目标追击的接受半径倍率；仍受 MoveAcceptanceRadius 下限约束 |

转速的统一入口是角色的 MovementYawRotationRate，不能同时在 CharacterMovement 上另改 RotationRate 并期待两处都生效。三个参数保留运行时数值保护，不改变原有决策和转向算法。

AI 使用 `/Game/FirstPerson/AI/BP_FPEnemyAIController`，其 `BehaviorTreeAsset` 指向 `/Game/FirstPerson/AI/BT_FPEnemy`，配套 Blackboard 为 `/Game/FirstPerson/AI/BB_FPEnemy`。在行为树中编辑分支与条件，C++ Task 执行采样、行为和等待；Controller 保留 MoveTo 去重、预算、朝向与攻击许可。决策间隔由树内等待任务消费，不再维护另一套 Controller 决策 Timer。

`Tools/Gameplay/CreateEnemyBehaviorTree.py` 用 UE 编辑器 Python 生成并绑定树、Blackboard 和 Controller 蓝图；重复执行不重建已经编辑过的树。没有自定义树的原生 Controller 仍可使用默认树模板。

AI 决策间隔、路径刷新距离、槽位和预算继续在所属蓝图/组件中编辑，不复制到实验 JSON。运行时状态（当前弹药、攻击者集合、换弹提交标记等）不作为默认配置开放。

### 动画播放：明确编号，不读取“当前动作”替代旧回调身份

- `BP_Weapon` 的 reload 图由 `OnWeaponReloadPlaybackRequested` 进入，将该事件的 `ReloadId` 传给手臂和枪械两次 `PlayReloadMontage`。普通/空仓的原素材选择保留；装填继续由原生 `ReloadCommit` Notify 提交，原生实例结束回调收尾。
- `enemy_BP` 的攻击图由 `OnAttackPlaybackRequested` 进入，将 `AttackId` 和选中的 Montage 传给 Combat 的 `PlayAttackMontageForAttack`。受击动画不再调用无参数攻击完成接口。
- 无参数 `CommitReload`、`FinishReload`、`HandleAttackFinishedNotify` 已弃用并拒绝执行；`OnWeaponReloadStarted` / `OnAttackStarted` 仅供观察，不负责播放。自定义异步回调必须保存发起时的编号。
- 一次性迁移已经完成，迁移与图审计 Commandlet 已从 Source 删除；原资产及工具源码备份保留在 `Saved/Backups/ActionPlaybackMigration/`，不属于正常玩法构建内容。
- 两个目标蓝图均为 Git LFS 跟踪资产，代码与资产应一起交付；只有 Source 归档的环境必须同步已迁移资产，只更新 DLL 不会自动重接旧蓝图。自动化中的两个真实资产测试不允许用缺失资产跳过来冒充通过。

### 渲染预算的作用范围

- 阴影与光追名额按敌人分配，应用于自身 Mesh 及 ChildActor 组件拥有的 Mesh；只收紧资产原有资格，不把原本关闭的效果打开。
- 普通 Attach 的独立 Actor 不归入敌人预算。若附件应随敌人一起受预算管理，使用本 Actor 的 Mesh 组件或 ChildActor 组件。
- 动态添加、移除或替换受管组件后调用角色的 `RefreshRenderBudgetMeshes`；集中采样使用缓存，不每轮扫描附属对象。
- 纳管期间阴影/光追标志由预算统一写入；刷新只更新组件集合，不把其他逻辑临时修改的标志重新当成资产默认值。长期资格在资产中设置并重新开始对局。
- 死亡时、死亡蓝图回调结束后都会刷新并关闭受管 Mesh 的阴影/光追；死亡后延迟添加附件也需调用刷新。预算不隐藏尸体，也不关闭其布娃娃物理。
- CSV 的 `ShadowOwners` / `RayTracingOwners` 是至少一个受管 Mesh 开启对应标志的敌人数；`ShadowMeshes` / `RayTracingMeshes` 是组件数。`ShadowCasters` / `RayTracingVisible` 保持旧版主 Mesh 口径，不能当作全场景的光追实例或三角形统计。

## 动画素材：项目默认值 + 现有蓝图覆盖

原来构造函数中的两条素材路径已移到 `Config/DefaultGame.ini`：

```ini
[fpstrue.EnemyAnimationSharing]
IdleAnimation=/Game/EnemyWarriorAnimPack/Animations/InPlace/Misc/EnemyWarrior_Idle_InP.EnemyWarrior_Idle_InP
MovingAnimation=/Game/EnemyWarriorAnimPack/Animations/InPlace/Movement/EnemyWarrior_Running_Forward_InP.EnemyWarrior_Running_Forward_InP
```

这里只是现有软引用属性的**构造默认值**。GameMode 蓝图中 EnemyAnimationSharingCoordinator 组件的 IdleAnimation / MovingAnimation 仍可覆盖，不覆盖时使用项目默认值。更换具体敌人素材，优先在组件属性中选择资源，便于编辑器验证引用。

- 修改 INI 后重启编辑器，保证类默认对象重新初始化；修改蓝图后保存并重新开始游戏。
- 不每帧读磁盘，也不承诺游戏中改文件立即生效。
- 配置缺失、素材加载失败或骨架不一致，沿用已有校验并回退到独立 AnimBP；此时动画共享收益会消失，需要查看日志。
- 新素材打包时必须确认已被 Cook；仅把资源路径写进文本不能保证该资源进入安装包。
- INI 只提供动画软引用默认值，与上面的玩法 Data Asset、行为树资产各自独立。

## 性能实验：一个采集入口，配置集中管理

新的性能采集统一使用 `Tools/Performance/RunRenderCostMatrix.ps1`，不再复制一份启动脚本来增加实验。

| 要修改的内容 | 唯一维护位置 |
| --- | --- |
| 人数、预热、时长、轮次、地图、实验组选项 | `Tools/Performance/ExperimentProfiles/*.json` |
| 公共默认值、分辨率、Trace 通道、隔离与超时设置 | `Tools/Performance/ExperimentProfiles/RenderCostCases.psd1` 的 `Defaults` / `Capture` |
| 每个实验的开关、CVar、消费者数量与分辨率限制 | 同文件的 `Variants`；CVar 同时生成启动命令和读回检查 |
| 运行时诊断开关与策略覆盖的唯一解析入口 | `Source/fpstrue/Runtime/fpstrueRuntimeOptions.h/.cpp` |
| 预热、时长、人数、输出等采集条件 | `Source/fpstrue/Testing/Benchmarks/fpstrueBenchmarkConfig.h/.cpp`；复用运行时快照，不重复解析上述开关 |
| 游戏内等待生成、预热、采集、校验与退出 | `Source/fpstrue/Testing/Benchmarks/fpstrueBenchmarkRunner.h/.cpp` |

业务埋点仍留在对应业务函数，声明集中在 `Source/fpstrue/Runtime/fpstruePerformanceStats.h`；正常玩法不依赖 `Testing/Benchmarks` 的参数实现。正式玩法默认值继续由 `Config/` 和玩法配置资产管理，不混入实验配置。`ReadRenderCostConfig.ps1` 只负责读取、校验和生成命令，`SummarizeRenderCostMatrix.ps1` 只负责汇总已有采集结果。

`Tools/Performance/ExperimentProfiles/baseline160.json` 保存 160 敌人、3 轮、15 秒预热、30 秒采集的常规 Baseline 方案。它不代表已通过性能验收，也不改变项目默认画质；ScreenPercentage 为 null，表示不额外覆盖内部渲染比例。

`scene-acceptance.json` 保存当前收尾验收条件：0/160 敌人，各 1 次，20 秒预热、180 秒采集、无 Trace。它仍使用已验收的 `Demonstration_Baked`；ISM 候选已因无整帧收益标记为无效并撤回，不作为默认地图或待验收事项。其他候选若另行获准测试，显式覆盖 `-Map`，不要修改公共基线。

`Tools/Assets/EnableSceneNanite.py` 是离线资产转换工具，不是另一个性能采集入口：白名单集中在脚本的 `MESH_NAMES`；创建独立候选地图和网格副本，保留原资产，支持 `--verify` 保存后复核。09-29 的 `Nanite_HardSurface20260929` 已完成 8 种网格、454 个组件转换，但零敌人采集没有确认有效整帧收益，未采用为默认方案，也未安排 160 敌人复测。详细结果见性能实验记录。

优先级固定为：**显式命令行参数 > JSON 中的值 > 配置表 Defaults**。JSON 只接受已有实验字段，不接受任意控制台命令、程序路径或输出目录。

下面在项目根目录的 PowerShell 中执行。先只检查配置，不启动 UE：

换机器运行时，通过 `-ProjectRoot "<项目目录>" -EditorPath "<UE5.5目录>\Engine\Binaries\Win64\UnrealEditor.exe"` 指定本机路径；这两个参数不放入 JSON 实验预设。

```powershell
.\Tools\Performance\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\Performance\ExperimentProfiles\baseline160.json -ValidateOnly
```

确认生效值后，才执行正式实验（会启动 UE，应先保存并关闭已有编辑器）：

```powershell
.\Tools\Performance\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\Performance\ExperimentProfiles\baseline160.json -RunName Baseline160_NewRun
```

临时改变采集时长不必编辑脚本：

```powershell
.\Tools\Performance\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\Performance\ExperimentProfiles\baseline160.json -DurationSeconds 60 -ValidateOnly
```

原有不带 ConfigFile 的调用方式仍然有效，保留旧地图默认值；当前验收应显式选择上述预设。ValidateOnly 同时展示实验定义及最终控制台命令，不启动 UE、不创建实验记录。实际运行的 environment.json 记录最终参数、参数来源、实验定义及输入 JSON 的 SHA256；配置表、读取器、采集脚本与输入 JSON 也纳入前后指纹校验。原来的隔离检查、生成校验和验收失败标记不放宽。

地图指纹跟随实际选择的 Map，不再固定记录默认地图。JSON 使用 `/Game/...` 包路径；命令行还兼容 `.对象名` 和 `?travel` 后缀，计算指纹时取对应的 `.umap` 文件。ValidateOnly 只检查配置，不要求本机存在地图；正式运行会先检查地图文件是否存在，再创建实验记录。

空实验组选项、未知实验名以及 NaN/Infinity 参数会在启动前被拒绝，JSON 与命令行覆盖后的参数均受检查。要求默认内部分辨率的 TSR/样条实验不能同时传入 ScreenPercentage 覆盖；缺失读回或最后一次读回不符均不能通过实验校验。

13 个历史 `Run*.ps1` 已从 `Tools/` 移入 `Tools/LegacyPerformance/`，保留原参数、注释及旧输出格式，只供查阅历史实验。配套汇总脚本也归入该目录，已有 CSV、Trace 与统计口径不变。历史配置不自动迁移为新的基线，避免混用旧地图、离屏采集或不同验收规则。

## 验证与边界

- C++ Automation 在临时世界检查启动顺序、事件重入、配置来源和玩法边界，不修改实际地图；真实资产绑定及导航生成另用编辑器读取和地图烟测核对。
- `Tools/Performance/TestRenderCostConfig.ps1` 验证默认值不变、覆盖优先级、非法输入拒绝和只读试运行；不启动 UE。
- 这次是配置迁移，不是性能优化实验。历史 CSV/Trace 保留，不用这次编译后的二进制冒充旧基线构建。
- 包围槽位、Timer、动画共享 Setup 等仍按现有生命周期初始化；需要热更新时再单独设计，当前通过重开游戏生效。
