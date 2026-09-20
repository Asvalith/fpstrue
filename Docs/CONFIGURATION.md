# 配置入口：少写死，不增加架构层

本次只在原有组件、AIController 和测试脚本内增加配置入口；没有新增 Manager、Data Asset、通用配置系统或热更新。旧属性名、蓝图覆盖、玩法流程和注释保留。默认数值没有调优或改变。

## 玩法参数：继续在蓝图中编辑

打开实际使用的敌人/AIController 蓝图，进入 Class Defaults 搜索以下属性：

| 所属类 | 属性 | 默认值 | 用途 |
|---|---|---:|---|
| EnemyCharacter | MovementYawRotationRate | 540 度/秒 | CharacterMovement 的转向速度，替换 BeginPlay 写死的数值 |
| EnemyAIController | CombatResponseRangeMultiplier | 1.5 | 有效攻击范围的倍率；此范围内维持战斗响应频率 |
| EnemyAIController | PursuitAcceptanceRangeMultiplier | 0.8 | 共享目标追击的接受半径倍率；仍受 MoveAcceptanceRadius 下限约束 |

转速的统一入口是角色的 MovementYawRotationRate，不能同时在 CharacterMovement 上另改 RotationRate 并期待两处都生效。三个参数保留运行时数值保护，不改变原有决策和转向算法。

没有 AIController 蓝图时，可创建现有原生类的蓝图子类，并在敌人蓝图的 AI Controller Class 中选择它；默认原生 Controller 仍可继续使用。

射速、伤害、弹匣、散布、换弹时长、决策间隔、路径刷新距离、槽位和预算等原本已有可编辑属性，不再复制到 JSON 或增加一套参数容器。运行时状态（当前弹药、攻击者集合、换弹提交标记等）不作为默认配置开放。

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
- 路径值与迁移前相同，未修改任何 `.uasset`。

## 性能实验：JSON 只是现有脚本的预设

`Tools/ExperimentProfiles/baseline160.json` 保存 160 敌人、3 轮、15 秒预热、30 秒采集的常规 Baseline 方案。它不代表已通过性能验收，也不改变项目默认画质；ScreenPercentage 为 null，表示不额外覆盖内部渲染比例。

优先级固定为：**显式命令行参数 > JSON 中的值 > 原脚本默认值**。JSON 只接受已有实验字段，不接受任意控制台命令、程序路径或输出目录。

下面在项目根目录的 PowerShell 中执行。先只检查配置，不启动 UE：

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

- C++ 使用原有 Unreal Automation 临时世界测试，不改地图或资产；新增配置读取和转速覆盖检查，保留转向与换弹回归。
- `Tools/TestRenderCostConfig.ps1` 验证默认值不变、覆盖优先级、非法输入拒绝和只读试运行；不启动 UE。
- 这次是配置迁移，不是性能优化实验。历史 CSV/Trace 保留，不用这次编译后的二进制冒充旧基线构建。
- 包围槽位、Timer、动画共享 Setup 等仍按现有生命周期初始化；需要热更新时再单独设计，当前通过重开游戏生效。
