# 当前配置入口

只维护多人PvPvE。README是总导航；本页只列正式配置，工具用法见[Tools/README](../Tools/README.md)。已完成的资产生成、迁移和修复代码不再作为日常入口。

| 配置 | 唯一位置 | 运行时所有者 |
| --- | --- | --- |
| 玩法类、地图与连接 | Config/DefaultEngine.ini、DefaultGame.ini；AfpstruePvpGameMode | GameInstance管本机连接；GameMode管权威对局 |
| 玩家与武器表现 | BP_TemplateCharacter、BP_RARCharacter及对应武器蓝图 | 蓝图装配网格、动画、音效与特效 |
| 武器规则 | 正式武器蓝图的WeaponConfiguration，引用DA_FPWeapon_Template或DA_FPWeapon_RAR | WeaponComponent保存独立快照、弹药与换弹事务 |
| 敌人近战 | enemy_BP的CombatConfiguration → DA_FPEnemyCombat | EnemyCombat保存攻击事务和命中去重 |
| 敌人决策 | BP_FPEnemyAIController的BehaviorTreeAsset → BT_FPEnemy，配套BB_FPEnemy | Controller和黑板保存本敌人状态 |
| 包围资格与攻击许可 | SurroundManager的参数 | 每目标管理器拥有队列与许可，黑板只保存镜像 |
| 逻辑与渲染分级 | GameState装配EnemyPerformance协调器 | 服务器决定权威逻辑，本地决定渲染 |
| 动画共享资源默认值 | DefaultGame.ini的[fpstrue.EnemyAnimationSharing] | 当前协调器初始化时读取 |
| HUD版式与刷新 | ingame_network蓝图 | 原生HUD只订阅当前Pawn/武器并提交只读快照 |
| 引擎渲染参数 | DefaultEngine.ini的[ConsoleVariables] | 引擎消费，实验覆盖在测试进程读回核实 |

## 使用规则

- 正式行为树必须显式配置且满足完整黑板schema；没有运行时生成树、旧树升级或缺配置后换一套树的fallback。
- 武器装备时整组校验并读取Data Asset。弹药、编号、队列和播放实例只属于武器实例，不写回共享配置；没有旧Weapon.StartFire/StopFire兼容接口，角色输入统一进入ASC。
- 换弹由服务器时钟唯一提交；动画通知只负责自己的表现边界，不能直接装弹。瞄准、冲刺与动作互斥由GAS管理。
- 移动转向在角色MovementYawRotationRate配置；不要再在CharacterMovement另设一套冲突转速。
- 诊断开关只由Runtime/fpstrueRuntime的进程只读快照解析，默认参数来自正式资产/INI。Shipping不接纳关闭玩法的诊断开关。
- 网络验证入口是RunFpsNetworkMatrix；群体性能入口是RunGroupAIBaseline；CPU/GPU导出统一ExportPerformanceTrace。没有旧单机Profile或LegacyPerformance启动链。
- 网络开发场景统一从GameState进入现有诊断装配。普通双端只用-PvpNormalPair，角色BeginPlay不再执行编辑器console审计。
- 日常配置直接编辑上述资产，重开对局生效；不重跑已完成迁移，也不增加热更新配置管理器。历史迁移与实验在Git历史和既有报告中查阅。
