# 配置入口：玩法资产、蓝图默认值与实验预设

项目只维护多人 PvPvE 规则，唯一玩法 GameMode 为 `AfpstruePvpGameMode`。默认游戏和 Dedicated Server 均指向该原生类；关卡覆盖与旧蓝图接线仍须经过实际资产迁移验证，类重定向不等于蓝图节点自动修复。敌人近战和武器数值使用小型 Data Asset，其他角色参数与动画表现保留原蓝图入口；JSON 只管理性能实验。配置在既有生命周期读取，不逐帧访问磁盘。没有通用配置管理器或运行时热更新。

旧单机波次、倒计时胜负与 `UfpstrueBenchmarkRunner` 组件入口不再属于当前玩法；历史报告保留原场景与构建口径，不移称当前多人验收。迁移范围见[唯一多人入口与迁移边界](MULTIPLAYER_MODE.md)。

## 玩法资产：整组选择一个配置来源

### EQS 与低血撤退

包围与接近点参数仍在 `SurroundManager` 的蓝图默认值：内外环数量、半径、接近半径、目标刷新间隔和缓存期限。EQS 模板在运行时只创建一次，候选生成与环境筛选集中在 `AI/fpstrueEnemyPositionQuery.h/.cpp`；每次请求传入位置快照，不读取隐藏玩家的实时位置。`MinPositionSpacing` 限制已发布接近点及掩体之间的距离；`MaxPendingCoverQueries` 默认 4，`MaxCoverQueriesPerFrame` 默认 1。`DefaultGame.ini` 的 `EnvQueryManager` 将 EQS 测试时间片设为 0.5 ms；这是候选之间的预算，不是每次导航调用的硬性耗时上限。

Controller 蓝图的 `AI|Retreat` 管理低血比例（默认 0.30）、撤退总期限（6 秒）、到位停留（1.5 秒）、重复触发冷却（8 秒）和决策间隔（0.25 秒）。生命值通知只唤醒现有行为树，不新增 Tick。首次/等待掩体保留紧急频率，取得移动目标后的维护乘以已有 Significance 倍率，同时不超过撤退期限。没有回血系统；持续低血不反复触发查询，只有冷却后的新伤害可以开启下一轮撤退。

建立或迁移项目资产时，使用以下对应关系：

| 配置类型 | 配置资产路径 | 绑定位置 |
|---|---|---|
| `UfpstrueEnemyCombatConfig` | `/Game/Config/DA_FPEnemyCombat` | `/Game/FirstPerson/Blueprints/enemy/enemy_BP` 的 CombatComponent → `CombatConfiguration` |
| `UfpstrueWeaponConfig` | `/Game/Config/DA_FPWeapon_RU74` | `/Game/FirstPerson/Blueprints/weapon/BP_Weapon` 的 `TP_Weapon` 组件 → `WeaponConfiguration` |

- **近战资产**集中攻击距离、伤害、间隔、动画结束保护与刀刃采样参数。CombatComponent 在 `BeginPlay` 整组复制到运行时参数，不在每次攻击时读资产。攻击是否正在进行、当前窗口、已命中对象等仍是各敌人独立状态。
- 近战资产未指定时使用现有蓝图字段；指定后在资产中修改，不再同时修改旧字段。出生点、角色转速、群体槽位与渲染预算不属于近战配置。多人敌人口目标与补充间隔由 `AfpstruePvpGameMode` 的 PvE 配置管理，不再读取单机波次资产。
- 迁移时先复制当前实际蓝图值，再绑定资产；保存后重开对局检查生效值。仅新增 C++ 配置类型并不等于已完成资产迁移，必须核对实际关卡使用的 GameMode、敌人蓝图及绑定引用。

历史 `MigrateGameplayConfiguration.py` 曾同时迁移三波 `[5, 7, 9]`、90 秒单机规则和近战配置。该记录不是当前多人配置入口，不重新运行旧脚本恢复已移除的玩法。迁移前备份与日志继续保留，完整工程仍需同步当前 Content/LFS 资产。

### 武器：数值在资产，动作和弹药在实例

打开 `/Game/Config/DA_FPWeapon_RU74`，在 `Settings` 中按分类编辑挂点、射速、射线距离/冲量、普通与关键部位伤害、关键骨骼、弹匣/初始备弹、散布、控制后坐力、视觉后坐力和换弹兜底。新增武器时复制这个资产，再在对应武器蓝图组件上指定 `WeaponConfiguration`；不需要修改 C++ 或增加配置管理器。

- `BP_Weapon` 已绑定该资产，原有 25 项组件数值字段已移除，不存在“资产与蓝图旧数值谁优先”的双入口。原生未绑定组件使用 `FFPWeaponSettings` 的完整默认值；真实资产测试检查 BP 绑定，防止意外落回默认值。
- 首次成功装备时校验并复制整组参数。编辑器“验证资产”和运行时装备共用合法性检查，拒绝非有限数、越界数值等；装备失败不发布装备关系或初始化弹药。没有读取磁盘的逐帧路径。
- 装备后读取该实例的数值快照；修改共享资产不会中途改变射速、弹匣容量或正在进行的换弹。调参后重新开始对局或生成新武器以生效。卸下再装备同一件武器保留快照与剩余弹药，不补满。
- 弹药、换弹编号、播放身份、已提交标记与表现 Timer 属于武器实例，动作标签和连射任务属于角色 ASC 与 GA。多个武器可以引用同一资产，但不共享这些运行状态。
- `VisualRecoil` 只叠加最终镜头画面，不写入控制方向或命中射线所用的 Camera 组件，也不修改动画。`VisualRecoilScale` 控制整体强度（0 为关闭），`AimVisualRecoilMultiplier` 控制瞄准倍率；`VisualRecoilRotation` 是以度为单位的 Pitch/Yaw/Roll 峰值，`VisualRecoilFOV` 是短促视野脉冲，`VisualRecoilDuration` 是回正总时长。每发重触发同一个 Shake，死亡、卸下或失去控制时仅清理武器自身的效果。旧配置资产加载时使用这五个新增字段的结构体默认值，无需重新迁移或重接蓝图。
- `ReloadDuration` / `EmptyReloadDuration` 决定多人服务器装填时长；客户端动画通知不能直接补弹。手臂/枪械 Montage、音效和特效仍在蓝图选择。

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
- 无参数 `CommitReload`、`FinishReload`、`HandleAttackFinishedNotify` 的旧资产迁移已完成，现已删除这些无调用占位接口。没有消费者的 `OnWeaponReloadStarted` / `OnAttackStarted` 也不再保留。自定义异步回调必须保存发起时的编号，原生 Notify 则校验实际播放实例。
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

这里只是现有软引用属性的**构造默认值**。多人 GameState 装配 EnemyAnimationSharingCoordinator；组件的 IdleAnimation / MovingAnimation 可覆盖项目默认值。更换敌人素材时核对实际 GameState 组件引用，不再编辑已退出的单机 GameMode 组件。

- 修改 INI 后重启编辑器，保证类默认对象重新初始化；修改蓝图后保存并重新开始游戏。
- 不每帧读磁盘，也不承诺游戏中改文件立即生效。
- 配置缺失、素材加载失败或骨架不一致，沿用已有校验并回退到独立 AnimBP；此时动画共享收益会消失，需要查看日志。
- 新素材打包时必须确认已被 Cook；仅把资源路径写进文本不能保证该资源进入安装包。
- 此处 `DefaultGame.ini` 的动画配置段只提供软引用默认值，与玩法 Data Asset、行为树资产及引擎渲染配置各自独立。

引擎渲染默认值另由 [DefaultEngine.ini](../Config/DefaultEngine.ini) 的 `[ConsoleVariables]` 管理，当前包含查询缓冲 `r.NumBufferedOcclusionQueries=2`、反射下采样 `r.Lumen.Reflections.DownsampleFactor=2` 和 TSR 历史缓冲 `r.TSR.History.ScreenPercentage=150`。实验案例可在测试进程中显式覆盖并读回校验，不修改玩法配置资产。

## 性能实验：多人基线与历史记录分开

当前固定 160 AI、Editor/Shipping 客户端对照入口为 `Tools/Performance/RunGroupAIBaseline.ps1`。服务端身份、地图、画质、人数、预热与采样窗口按每轮清单核对，不能把 Editor DS 自动称为 Shipping Server。

- 网络世界由 GameState 调用 `StartPvpBaseline`，只有显式 `PvpBaseline` 才启动固定场景采集；旧血条 `PvpProfile` 和相关入口已删除。
- `Testing/Benchmarks/fpstrueBenchmarkRunner.*` 仍保留网络 Profile/Baseline、CSV/Trace 与线程/物理/网络消费者验证，不再包含依赖旧单机 GameMode 的采集组件。
- 正式玩法开关仍由 `Runtime/fpstrueRuntime.*` 统一解析；采集参数不写回蓝图或玩法 DataAsset。
- `Tools/Performance/ExperimentProfiles`、`RunRenderCostMatrix`、`AutoBenchmark`、`scene-acceptance.json` 记录旧单机实验口径，不作为当前运行命令。保留原始数据与报告，复现须使用对应历史版本。
- 场景资源转换工具属于离线资产工作，不是性能采集入口；未经单因素对照，不把转换本身写成收益。

只认本轮身份匹配、门禁有效的实测结果。网络参数是模拟注入条件，不是实测 RTT；World wall-time、OS on-core CPU、引擎 unit 和带宽各自解释，不相加为总帧时间。

## 验证与边界

- C++ Automation 在临时世界检查启动顺序、事件重入、配置来源和玩法边界，不修改实际地图；真实资产绑定及导航生成另用编辑器读取和地图烟测核对。
- 历史 `TestRenderCostConfig.ps1` 的配置检查不启动 UE，也不能证明已退出的单机入口还能运行。
- 这次是配置迁移，不是性能优化实验。历史 CSV/Trace 保留，不用这次编译后的二进制冒充旧基线构建。
- 包围槽位、Timer、动画共享 Setup 等仍按现有生命周期初始化；需要热更新时再单独设计，当前通过重开游戏生效。
