# 配置入口：玩法资产、蓝图默认值与实验预设

波次与敌人近战参数使用小型 Data Asset，其他角色/武器参数保留原蓝图入口；JSON 只管理性能实验，INI 只提供项目级动画素材默认值。没有通用配置管理器，也没有运行时热更新。

## 波次与近战：整组选择一个配置来源

建立或迁移项目资产时，使用以下对应关系：

| 配置类型 | 配置资产路径 | 绑定位置 |
|---|---|---|
| `UfpstrueWaveConfiguration` | `/Game/Config/DA_FPMatchWaves` | `/Game/FirstPerson/Blueprints/gamemode/fpstruegamemode` 的 `WaveConfiguration` |
| `UfpstrueEnemyCombatConfig` | `/Game/Config/DA_FPEnemyCombat` | `/Game/FirstPerson/Blueprints/enemy/enemy_BP` 的 CombatComponent → `CombatConfiguration` |

- **波次资产**接管默认敌人类、显式波次数组、波次间隔和对局时长。每波未指定敌人类时，只回退到同一资产的默认类；空波次或缺失所需敌人类会使开局失败，不会偷偷读取旧蓝图补齐。
- **近战资产**集中攻击距离、伤害、间隔、动画结束保护与刀刃采样参数。CombatComponent 在 `BeginPlay` 整组复制到运行时参数，不在每次攻击时读资产。攻击是否正在进行、当前窗口、已命中对象等仍是各敌人独立状态。
- 两个资产引用未指定时，继续使用现有蓝图字段，保留旧资产兼容性；一旦指定，就在资产中修改相应参数，不再同时修改旧字段。出生点、角色转速、群体槽位与渲染预算不属于这两份资产。
- 迁移时先复制当前实际蓝图值，再绑定资产；保存后重开对局检查生效值。仅新增 C++ 配置类型并不等于已完成资产迁移，必须核对实际关卡使用的 GameMode、敌人蓝图及绑定引用。

本地关卡已通过 `Tools/MigrateGameplayConfiguration.py` 完成复制与绑定，保留原波次数量 `[5, 7, 9]`、90 秒对局时长和全部近战参数。迁移前的两个蓝图备份位于 `Saved/Backups/GameplayConfigurationMigration/`；`Saved/Logs/MigrateGameplayConfiguration.log` 记录保存结果。新开关卡的日志会输出实际配置资产路径，便于确认改的是当前生效的配置。Content 仍不随源码仓库分发，其他环境应在具备对应蓝图后运行迁移脚本。

## 玩法参数：继续在蓝图中编辑

打开实际使用的敌人/AIController 蓝图，进入 Class Defaults 搜索以下属性：

| 所属类 | 属性 | 默认值 | 用途 |
|---|---|---:|---|
| EnemyCharacter | MovementYawRotationRate | 540 度/秒 | CharacterMovement 的转向速度，替换 BeginPlay 写死的数值 |
| EnemyAIController | CombatResponseRangeMultiplier | 1.5 | 有效攻击范围的倍率；此范围内维持战斗响应频率 |
| EnemyAIController | PursuitAcceptanceRangeMultiplier | 0.8 | 共享目标追击的接受半径倍率；仍受 MoveAcceptanceRadius 下限约束 |

转速的统一入口是角色的 MovementYawRotationRate，不能同时在 CharacterMovement 上另改 RotationRate 并期待两处都生效。三个参数保留运行时数值保护，不改变原有决策和转向算法。

AI 使用 `/Game/AI/BP_FPEnemyAIController`，其 `BehaviorTreeAsset` 指向 `/Game/AI/BT_FPEnemy`，配套 Blackboard 为 `/Game/AI/BB_FPEnemy`。在行为树中编辑分支与条件，C++ Task 执行采样、行为和等待；Controller 保留 MoveTo 去重、预算、朝向与攻击许可。决策间隔由树内等待任务消费，不再维护另一套 Controller 决策 Timer。

`Tools/CreateEnemyBehaviorTree.py` 用 UE 编辑器 Python 生成并绑定树、Blackboard 和 Controller 蓝图；重复执行不重建已经编辑过的树。没有自定义树的原生 Controller 仍可使用默认树模板。

武器射速、伤害、弹匣、散布、换弹时长，以及 AI 决策间隔、路径刷新距离、槽位和预算继续在所属蓝图/组件中编辑，不复制到实验 JSON。运行时状态（当前弹药、攻击者集合、换弹提交标记等）不作为默认配置开放。

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

## 性能实验：JSON 只是现有脚本的预设

`Tools/ExperimentProfiles/baseline160.json` 保存 160 敌人、3 轮、15 秒预热、30 秒采集的常规 Baseline 方案。它不代表已通过性能验收，也不改变项目默认画质；ScreenPercentage 为 null，表示不额外覆盖内部渲染比例。

优先级固定为：**显式命令行参数 > JSON 中的值 > 原脚本默认值**。JSON 只接受已有实验字段，不接受任意控制台命令、程序路径或输出目录。

下面在项目根目录的 PowerShell 中执行。先只检查配置，不启动 UE：

换机器运行时，通过 `-ProjectRoot "<项目目录>" -EditorPath "<UE5.5目录>\Engine\Binaries\Win64\UnrealEditor.exe"` 指定本机路径；这两个参数不放入 JSON 实验预设。

```powershell
.\Tools\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\ExperimentProfiles\baseline160.json -ValidateOnly
```

确认生效值后，才执行正式实验（会启动 UE，应先保存并关闭已有编辑器）：

```powershell
.\Tools\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\ExperimentProfiles\baseline160.json -RunName Baseline160_NewRun
```

临时改变采集时长不必编辑脚本：

```powershell
.\Tools\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\ExperimentProfiles\baseline160.json -DurationSeconds 60 -ValidateOnly
```

原有不带 ConfigFile 的调用方式仍然有效。实际运行的 environment.json 记录最终参数、参数来源和输入 JSON 的 SHA256；原来的隔离检查、生成校验和验收失败标记不放宽。

地图指纹跟随实际选择的 Map，不再固定记录默认地图。JSON 使用 `/Game/...` 包路径；命令行还兼容 `.对象名` 和 `?travel` 后缀，计算指纹时取对应的 `.umap` 文件。ValidateOnly 只检查配置，不要求本机存在地图；正式运行会先检查地图文件是否存在，再创建实验记录。

空实验组选项以及 NaN/Infinity 参数会在启动前被拒绝，JSON 与命令行覆盖后的参数均受检查。

## 验证与边界

- C++ Automation 在临时世界检查启动顺序、事件重入、配置来源和玩法边界，不修改实际地图；真实资产绑定及导航生成另用编辑器读取和地图烟测核对。
- `Tools/TestRenderCostConfig.ps1` 验证默认值不变、覆盖优先级、非法输入拒绝和只读试运行；不启动 UE。
- 这次是配置迁移，不是性能优化实验。历史 CSV/Trace 保留，不用这次编译后的二进制冒充旧基线构建。
- 包围槽位、Timer、动画共享 Setup 等仍按现有生命周期初始化；需要热更新时再单独设计，当前通过重开游戏生效。
