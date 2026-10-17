# 工具入口

工具按用途分组，玩法源码仍按业务模块放在 `Source/fpstrue/`，可调玩法资产集中在 `Content/Config/`。本目录不保存实验输出或资产备份。

| 目录 | 用途 | 常用入口 |
|---|---|---|
| `Gameplay/` | 行为树资产生成/验证、波次/近战/武器配置迁移 | `CreateEnemyBehaviorTree.py`、`VerifyEnemyBehaviorTreeAssets.py`、`MigrateWeaponConfiguration.py` |
| `Networking/` | PvP 构建、功能回归、双客户端网络冒烟与交付证据 | `BuildPvpDelivery.ps1`、`RunPvpSmoke.ps1` |
| `Assets/` | 网格、LOD、纹理、Nanite、VSM 候选审计与样条静态化 | `AuditVSMCandidates.py`、`AuditTextureAssets.py`、`BakeStaticLandscapeSplines.py`、`VerifyBakedLandscapeSplines.py` |
| `Performance/` | 当前性能采集、Trace 导出、CSV/事件分析与脚本回归 | `RunRenderCostMatrix.ps1`、`TestRenderCostConfig.ps1`、`ExportRenderWaitTrace.ps1`、`AnalyzeGpuTimingTrace.py` |
| `Performance/ExperimentProfiles/` | 实验条件、CVar 和读回要求 | `baseline160.json`、`scene-acceptance.json`、`RenderCostCases.psd1` |
| `Performance/Charts/` | 根据既有实验记录生成报告图表 | `render_closeout_charts.py`、`render_occlusion_evidence.py` |
| `LegacyPerformance/` | 冻结的历史启动、汇总和固定数据分析 | 仅用于核对旧实验，不作为新一轮采集入口 |

## 常用操作

以下命令从工程根目录运行。校验不会启动 Unreal，也不会产生新的性能证据：

```powershell
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\TestRenderCostConfig.ps1
powershell -ExecutionPolicy Bypass -File .\Tools\Performance\RunRenderCostMatrix.ps1 -ConfigFile .\Tools\Performance\ExperimentProfiles\baseline160.json -ValidateOnly
```

正式采集使用同一入口，去掉 `-ValidateOnly`；启动前确认预设、地图和独占测试时段。现有基线和画质参数没有因目录整理而变化。

`Gameplay/` 和 `Assets/` 中依赖 `unreal` 的 Python 脚本使用 Unreal Editor 的 Python 环境，不是普通 Python 程序。`Migrate*`、`Create*`、`Configure*`、`Enable*`、`Bake*` 可能修改资产，运行前阅读脚本条件并保留备份；已迁移的武器日常调参直接编辑 Data Asset，不再运行迁移。

`Performance/` 中的 CSV/Trace 分析使用普通 Python；`Charts/` 额外依赖 Pillow 和脚本中指定的微软雅黑字体。UE 自带 Python 不一定包含 Pillow，不需要为运行这些离线图表脚本修改引擎环境。

## 数据与兼容边界

- `Saved/Profiling/`：原始 CSV、Trace 和采集产物。
- `Saved/Automation/`、`Saved/Logs/`：功能测试报告与日志。
- `Saved/Backups/`：配置迁移及资产改动前的备份。
- `Docs/Performance/figures/`：报告图片与证据摘要；图表脚本移动后仍输出到这里。
- 历史日志、manifest 和本地备份中的旧脚本路径保留为历史记录；重新运行时使用本页的新入口。没有在旧位置增加转发脚本。

详细参数及配置选源见 [配置说明](../Docs/CONFIGURATION.md)，历史实验边界见 [归档说明](LegacyPerformance/README.md)。

## PvP 构建与交付

从同一个入口依次执行构建、功能测试和联网检查，任一步失败即停止，日志和结果留在 `Saved/Delivery/<RunId>/`。不会下载引擎、租用主机、修改防火墙，也不会结束其他人打开的编辑器。

```powershell
# 只查看计划，不启动进程或创建输出。
.\Tools\Networking\BuildPvpDelivery.ps1 -EngineRoot 'E:\program\ue554\UE_5.5' -Mode Editor -PlanOnly

# 当前安装版引擎可执行：Editor、Game Development/Shipping 编译，功能回归，正常/受损网络双客户端检查。
.\Tools\Networking\BuildPvpDelivery.ps1 -EngineRoot 'E:\program\ue554\UE_5.5' -Mode Editor

# 支持 Server 的 UE 5.5 构建环境：Development 客户端与独立服务器打包，再用实际打包程序联网验证。
.\Tools\Networking\BuildPvpDelivery.ps1 -EngineRoot '<支持Server的UE5.5目录>' -Mode Delivery -RequireClean
```

- `Editor` 模式使用编辑器的服务器模式，不证明独立服务器包可交付。`Delivery` 模式先检查 Server 构建能力和空间；不支持时明确失败，不自动降级。默认要求输出盘至少有 50 GB 可用空间，此为保守门槛，不是实际容量保证。
- `Delivery -IncludeShipping` 另外构建和 Cook 正式包。Shipping 已移除自动输入和性能消融参数；它不运行开发版冒烟逻辑，所以报告只记录构建/打包结果，不伪称正式包已通过运行验收。
- 功能回归默认要求至少 75 项成功，无失败、未执行或进行中项；带警告的成功单独记录。当前本机 `C:/UEBuild/FPS/RARAnimationCompletion_20261016/FramingVerifiedRegression2/index.json` 为 63 项无警告、12 项带警告，失败、未执行和进行中均为 0；包含 RAR 资源接线、三 Mesh 动作/换弹取消、中途进度补齐及死亡后迟到附着的隐藏检查，不代替真实联网画面验收。正常及受损网络检查均需服务器命中、换弹、死亡、重生，以及双端状态证据。深度门禁默认开启，还检查专用服务器身份、真实位移、双方时钟、同发号预测/接受/ACK/历史命中及表现不重复。受损条件为每个发送方向延迟 100 ms、波动 20 ms、丢包 2%，不是测得的 RTT。
- 打包冒烟要求 Development Server/Game 的直接可执行文件及匹配构建记录，随后检查进程实际运行配置。默认使用无画面模式；`RunPvpSmoke -RenderedClients` 则让客户端使用真实 D3D12 离屏渲染，服务器仍无画面。它检查当前相机、武器附着、HUD 和敌人副本，保存初始双端及恢复后的 1280×720 PNG 并验证标记、身份、文件与尺寸；自动门禁不替代人工观感检查、命中公平性、人数容量或长时间稳定性验收。
- 多人 AI 门禁另外核对专服仅更新 Gameplay 重要性，两个客户端及恢复客户端实际应用各自表现预算。共享跟随者数量单独记录，近距离战斗中的敌人保持独立动画是正常情况，不强迫为测试进入共享池。
- `DeliveryReport.json` 记录源码提交、工作区是否有未提交修改、引擎版本、步骤、自动化统计、实际包程序哈希和符号位置；符号复制到本次输出目录，避免下次编译覆盖。带本地修改的结果可用于开发验证，但不能声称仅凭远端提交即可复现。输出不进入源码仓库；脚本不删除历史包。
- `.github/workflows/pvp-delivery.yml` 仅允许在受信开发分支手动运行。需自行注册标签 `ue55-server` 的 Windows 自托管执行机，并设置 `UE_ROOT`；仓库中的工作流文件不代表执行机已配置或 CI 已通过。CI 取得 Git LFS 实体后检查干净检出。日志/报告默认仅保留在执行机，显式选择后才上传 GitHub Artifact；公开仓库的产物不应视为私密，日志可能包含本机路径。工作流不上传第三方资源包或符号文件。

独立服务器首先是“没有本地玩家的独立游戏进程”，可以和两个客户端运行在同一台电脑。远程部署是下一层验收：把同一版本服务器包放在另一台机器，客户端连接其地址，检查进出、战斗、死亡重生和日志。只有实际完成后才能记录跨机通过；公网部署另需确认端口、安全及费用。

### Win64 客户端与 Linux Server 成对构建

`LinuxPackage` 使用同一个 UE 5.5 源码引擎构建 Win64 Game 和 Linux Server；需要已安装的 v23 Linux 交叉编译工具链。`CookEngineRoot` 可指定兼容的同版本 Editor 负责资源 Cook，但不能拿它的安装版 Game 程序替换源码引擎客户端，也不通过覆盖 NetVersion 掩盖版本差异。

Cook 显式使用 UAT 的 `-VersionCookedContent`，保存带字段名的属性和版本信息。仅版本号相同仍不足以证明资源兼容：定制源码引擎可能给 `UPrimitiveComponent` 等基础类增加反射属性，默认无版本属性序列化的字段编号会与安装版 Cooker 不同，导致打包程序加载崩溃。该选项解决这类属性布局差异，不保证自定义原生 `Serialize` 格式兼容；最终仍必须加载真实地图并执行联网门禁。不修改全局序列化开关或跳过网络版本检查。

```powershell
# 先检查前置条件和计划；实际构建时去掉 -PlanOnly。
.\Tools\Networking\BuildPvpDelivery.ps1 `
  -Mode LinuxPackage `
  -EngineRoot '<UE5.5源码引擎目录>' `
  -CookEngineRoot '<同版本可运行Editor的引擎目录>' `
  -LinuxToolchainRoot '<v23_clang-18.1.0-rockylinux8目录>' `
  -OutputRoot '<独立输出目录>' `
  -ServerDebugInfo Full -MinimumTests 75 -MaxParallelActions 4 -Port 7797 -PlanOnly
```

- `ServerDebugInfo` 支持 `Full`、`LineTablesOnly`、`None`，默认 `Full`；不要为了省空间静默丢失符号。报告记录所选策略，产出 `.debug` 时核对 ELF build-id；客户端 PDB 和服务端符号只保留在内部构建目录，不随公开源码或游戏包上传。
- `MaxParallelActions` 默认 4，仅约束本轮构建，并关闭本轮 UBA 执行器；不修改共享引擎配置或系统分页文件，避免高并发编译挤占其他工程的内存。
- 输出隔离为 `Packages/Win64` 和 `Packages/LinuxServer`，并核对可执行文件、构建收据、哈希及符号。`LinuxPackage` 成功只表示构建/Cook/Stage/Archive 通过；Windows 上不会运行 Linux ELF，不能据此宣称联机通过。
- `-IncludeShipping` 仅用于 Windows `Delivery`，不能与当前 Development 成对 Linux 构建混用。磁盘门槛与源码/工具链检查失败会停止，不自动下载引擎或删除旧产物。

### 远端 Linux Server 与本机双客户端验收

先由部署流程把完整 Linux 包放到独立目录，目录内应有 `Engine/`、`fpstrue/`、已 Cook 内容及 `fpstrue/Binaries/Linux/fpstrueServer` 和相邻 `.target`。准备已有的普通运行用户、已核对主机指纹的 SSH 配置及仅用于 FPS 的 UDP 7797；不要占用 Co-op 的 7777 或停止它的服务。以下地址和路径均为占位示例，不包含真实服务器或私钥。

```powershell
# DryRun 不进行 SSH、不创建输出、不启动进程；填入实际构建信息后先检查计划。
.\Tools\Networking\RunPvpSmoke.ps1 `
  -ExecutionMode Packaged -ServerMode RemoteLinux `
  -ClientExecutable '<本次Win64包>\fpstrue\Binaries\Win64\fpstrue.exe' `
  -ServerAddress 'game-server.example' -Port 7797 `
  -SshTarget 'root@game-server.example' `
  -SshKeyPath '<已授权私钥的本地路径>' `
  -SshKnownHostsPath '<已核对的known_hosts文件>' `
  -RemotePackageRoot '/srv/fpstrue/pvp/build-id' `
  -RemoteRunUser 'fpstrue-ds' `
  -RemoteServerSha256 '<本次LinuxServer的64位SHA256>' `
  -TimeoutSeconds 180 -DryRun
```

去掉 `-DryRun` 才执行验收；弱网另加 `-PacketLagMs 100 -PacketLagVarianceMs 20 -PacketLossPercent 2`。SSH 管理登录与游戏运行用户是不同职责，服务器进程不能以 root 运行。

- 启动前使用严格 SSH 主机校验，核对远端 ELF 哈希、Linux Development Server 收据、已 Cook 内容、普通用户、空闲端口和可用内存；不接受 Editor、Game、Shipping 或启动器冒充该 Server。
- 每轮创建独立的 systemd 临时服务和日志目录，限制 900M 内存及测试时限；只停止本轮的精确服务名，取回服务器日志和该服务日志，不使用 `pkill`，不改防火墙、不创建账号、不部署包。SSH 失败仍受远端最长运行时间保护。
- 两个本机客户端连接 `ServerAddress:Port`。报告和日志保留在 `Saved/Networking/PvpSmoke_<RunId>/`；记录客户端程序和双方原始构建收据的实际 SHA256，打包程序仍需 Development 运行日志。`Editor` 客户端也可用于排查，但不能代替成对打包程序的最终验收。
- 深度门禁按完整发号身份关联预测表现与 Accepted ACK，再用 ShotId 和 FireTime 对应服务器历史部位命中记录；同一客户端的相同身份只能播放一次。默认还检查真实 AI 攻击、玩家反击及敌人死亡，并硬停本轮 Client2；确认服务器保留角色后，立即使用同一私有用户目录启动新进程，让冷启动与真实离线战斗并行。仍要求同一身份的实际离线伤害严格早于服务器恢复，再核对三端非零 NetGUID、血量、弹药、成绩及新时钟。其他在线玩家在离线受伤专项中被移出感知范围；不改变正式 AI 选敌规则，也不直接制造伤害。`IncludePve` 和 `IncludeReconnect` 可显式关闭用于排查，关闭后的报告不证明对应功能。
- 进程重启恢复不等于原进程的自动重试已验收，报告中的 `AutomaticRetryTransportVerified` 单独标明。私有用户目录包含重连票据，不要整体分享或上传 `Saved/Networking/`；对外只导出脱敏报告。通过前及停止进程前再次核对存活状态与最新日志，停止后查验 Fatal、FAIL 和明确 OOM；本轮主动停止产生的 SIGTERM 143 不当作运行崩溃。位移检查证明实际移动与复制发生，不是误差上界、延迟公平性、画面、容量或长时稳定性的证明。

分版本证据（下列公网 PvP 批次不包含随后新增的多人 AI 与断线恢复）：

- 新增 PvPvE／恢复的本地 Editor 正常网络报告 `PvpSmoke_20261016_120700_7860b3c5/Report.json` 与模拟弱网报告 `PvpSmoke_20261016_120855_c0c7753d/Report.json` 均通过全部默认门禁。两轮真实离线伤害均使生命 90→80，恢复保持 80、各自原弹药与成绩，三端 Pawn/PlayerState NetGUID 相同；不要求跨端 FName 相同。弱网为每发送方向额外延迟 100 ms、波动参数 20 ms、丢包 2%，不是测得的 RTT。此结果不代替最新 Linux 包、公网、原进程自动重试或画面验收。该版本的 66 项规则回归也已通过（40 项无警告、26 项带警告）。

- 成对构建报告 `20261016_020904_233d4919/DeliveryReport.json`：Editor、Win64 Game、Linux Server 编译及双端打包通过，60 项自动化成功（37 项无警告、23 项带警告）。构建报告的 `RuntimeVerified=false` 保留原意，运行结果由下述独立报告证明，不回填伪造构建阶段证据。
- 公网打包验收 `PvpSmoke_20261016_022020_a991b9ac/Report.json`（正常）与 `PvpSmoke_20261016_022105_08b0d1f7/Report.json`（模拟弱网）均通过。使用本次成对构建的 Win64 和 LinuxServer 包；远端 Linux 包全部 45 个文件与本次本地归档逐文件 SHA256 对比一致，部署记录为 `Saved/Networking/Deployment_20261016_020904_233d4919.json`。实际程序及双方收据的哈希记录在运行报告中；未覆盖 NetVersion。
- 两轮均为两个 Win64 客户端连接远端普通用户运行的 Linux Dedicated Server。弱网为每发送方向额外延迟 100 ms、波动 20 ms、丢包 2%；不是测得的 RTT，也没有量化误差上界。每轮由一个客户端执行战斗、另一个观察复制，报告中未分配给该角色的单项可为 `false`，整体必须满足同一执行端和同一观察端的组合门禁。
- 测试后 FPS 临时服务和 UDP 7797 监听已退出；原 Co-op 服务保持运行。未配置常驻 FPS 服务。本地 Editor 报告 `PvpSmoke_20261016_012337_0c1c8408`、`PvpSmoke_20261016_012512_774e7f81` 与失败尝试继续保留；符号、原始日志和云端访问资料保持内部保存。上述结果不等于画面、容量或长时稳定性验收。
