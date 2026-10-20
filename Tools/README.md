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

### 显式 Editor 测试 DDC

项目提供 `-ddc=fpstrueEditorTestDDC` 专用缓存图，仅在测试进程显式选择时生效，不更改正常游戏／编辑器的默认缓存图，也不修改系统注册表或其他工程配置。该图读取引擎的缓存包，并将既有 `%GAMEDIR%../ddc` 作为可选只读来源；`ReadOnlyDataCachePath` 可显式覆盖此只读目录。新缓存只写入 `LocalDataCachePath` 指定的独立本地目录，未指定时为项目 `Saved/LocalDDC`，不会回写或清理旧 `../ddc`。

例如，单次 Editor 功能测试进程可以同时传入 `-ddc=fpstrueEditorTestDDC -LocalDataCachePath="C:/UEBuild/FPSCache/<RunName>"`，保持历史缓存只读并隔离新写入。实际入口及启动参数应以本轮日志为准；不要据此认定所有测试脚本都已自动使用该图。固定 160 AI 性能入口仍记录自己的缓存参数与预热条件，功能测试缓存复用不等于性能采集已消除冷加载成本，也不能在 A/B 轮次中途切换缓存策略。

## 160 AI 平地基线

群体 AI 采集复用既有平地场地 `/Game/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview55x192`，不再生成土包、坡台或台阶专项图。保持 160 AI 与两个玩家，保留玩家 IK、服务器权威、独立导航、站位缓存、请求去重、World 级限流和失败退避。切换地图后必须重新验证出生点、相机及有效人口并建立新基线；旧地图的结果只保留为历史证据，不能混入新场景成绩。

```powershell
# 只检查计划；实际采集去掉 -DryRun，必须提供 Stage 内部的真实 Shipping EXE。
.\Tools\Performance\RunGroupAIBaseline.ps1 `
  -ShippingClientExecutable '<Stage>/Windows/fpstrue/Binaries/Win64/fpstrue-Win64-Shipping.exe' `
  -Repeat 3 -CaptureScreenshots -PresentMon -DryRun
```

- 固定 160 AI、2 玩家、1600×900 输出、High（2）及实际画质读回；默认使用出生点锚定的固定玩家视点，`-View Overview` 才使用俯视镜头，两者不可混为一个基线。High 的内部分辨率比例另在 `quality` 记录，不宣称原生 100% 渲染。
- **固定玩家视点协议**：每端按角色名查找唯一 `PlayerStartTag=GroupBaseline<Role>` 的 `APlayerStart`，即 `GroupBaselineClient1`／`GroupBaselineClient2`；这是 `PlayerStartTag` 属性，不是普通 Actor Tags。沿作者 Camera 到 Pawn Root 的组件层级累积相对变换，再组合该出生点的变换，生成固定相机并读回 FOV。不采已经被移动／推挤的 Pawn 世界位置，也不把运行中的 SpringArm socket 或 CameraManager 世界位置当锚点；固定视点不等于冻结 Pawn、敌人或真实玩法活动。
- **视点失败关闭**：缺失／重复出生点、无有效相机层级、绝对变换、当前协议不支持的伸臂／偏移或动画 socket、非法变换／FOV均拒绝该轮，不回退成随机视点。日志 `PVP_BASELINE VIEW_ANCHOR` 保存角色、标签、相对变换和世界视点；随后继续检查固定相机及实际视点漂移，不能只看第一张截图。此新协议与旧的运行时 Pawn 视点冻结不视为同一实验条件。
- Editor `-game` 与真实 Shipping 客户端交替各跑三轮；每轮一个渲染端、一个 NullRHI peer，共用相同 Editor DS。全部就绪后预热至少 20 秒、采集约 61 秒；不是 PIE、双渲染压力、公网或 Shipping Server 容量验收。
- Shipping 默认不接受命令行首个 URL。显式基线模式以受限 loopback 引导调用原生 Browse／握手，两个客户端构建均走同一入口；不开放作弊 RPC、任意地图跳转或自动输入。普通连接与重连入口不受影响。
- 结果保存逐帧数据、输入哈希、角色／构建／视点、完整人口计数与服务端真实 AI 活动范围；160 个副本不等于 160 个同时追击或攻击。失败、慢帧和缺失呈现遥测均保留，未完成且没有 `COMPLETE` 的进程即使 exit0 也不能通过。
- 每个角色使用独立控制文件。全部写盘后先退出两个客户端，确认两者均 `COMPLETE` 且自然 exit0，再退出服务器；防止服务器先关闭导致客户端在退出阶段误入默认地图。所有自有进程均需自然 exit0，不能用强杀后的 CSV 冒充有效结果。
- `FrameEndIntervalMs` 是游戏进程帧末回调间隔，含等待；`WorldTickWallMs` 是选定 World 的 Tick 墙钟范围，不能冒充 GT。PresentMon 若被权限拒绝，明确标为无效，不自动提权；它与引擎帧间隔分别报告。
- 平地场地入口为 `-run=fpstrueEnemyUIArena`：仅在场地不存在时创建，已存在则只复用，不在 A/B 中途覆写资产。原工业玩法地图与历史性能产物保留。

动画与活体命中代理使用同一套基线入口，不另建测试场景：

```powershell
# 只改变专服线程门禁，奇偶轮交换 Reference/Parallel 顺序；先用 -DryRun 核对。
.\Tools\Performance\RunGroupAIBaseline.ps1 -BuildKinds Editor -Repeat 3 `
  -ServerAnimationThreads Reference,Parallel -EnemyHitProxies Reference `
  -OutputRoot C:/UEBuild/FPSBaselines -RunName AnimationThreads160_AB -DryRun
```

- `-ServerAnimationThreads Reference` 为默认历史启动方式，不额外加线程参数；`Parallel` 仅给同一个 Editor DS 增加 `-useperfthreads`，不强制修改动画实例、CVar、Root Motion 或玩法档位。两模式可放在同一运行中交替对照，汇总按客户端构建和线程模式分别分组，不混合成绩。
- `-EnemyHitProxies Reference` 显式保留旧骨骼命中策略；`Simple` 测试简化活体命中代理。比较代理时固定线程模式、复制策略、场景及其他条件，用不同运行名保存证据；该选择在三端显式传入，旧 Shipping 包不能冒充新版本。
- `-OutputRoot` 可将日志、UserDir和帧数据移到空间充足的目录，默认仍为项目 `Saved/Profiling`。输出不覆盖既有目录，固定160 AI、两玩家、原画质与活动校验不变。
- 每个角色的摘要补充 START／STOP 边界快照：实际 `FApp` 门禁、动画 CVar、实例并行资格、姿态策略及物理计数。共享 PhysicsAsset 的 Body／Shape／Constraint 数仅在边界按资产去重读取，是配置数量；Mesh 有效 BodyInstance、创建的物理状态与命中代理另列，不能互相冒充。这些门禁也不证明任务实际在Worker执行，仍需独立Trace确认。
- Editor／Shipping仅指客户端构建，服务器仍是相同的Editor DS；不将此结果写成Shipping独立服务器性能。逐帧CSV列与既有schema-2口径保持不变，动画／物理边界快照位于计时窗口之外；显式 Trace／线程 CPU／节拍逐帧诊断的额外开销另行记录。

节拍与遮挡对照复用上述入口，**默认不改变旧启动条件**：

- `-ServerTickRate` 默认不传值，保留本次引擎／NetDriver 的继承配置；显式传入 30 或 60 时，只覆盖该轮 Editor DS 的 `NetServerMaxTickRate`，Case 增加 `_ServerTick30/60`。`t.MaxFPS=0` 不取消 DS 的 NetDriver 限速；不要传 0 冒充不限速，UE5.5 会把该值限制为 1 Hz。先比较相同构建的 30／60 独立诊断，不将节拍改变算成代码优化收益，也不混入旧基线。
- 每端摘要 `pacingDiagnostics.start/stop` 保存 live NetDriver 目标、固定时间／平滑配置和安全的引擎频率重读。`GetMaxTickRate` 内含故障注入副作用，仅精确 `UGameEngine` 且 `CauseHitches=0`、`t.UnsteadyFPS=0` 时调用；它是以记录的 FApp delta 在边界／帧尾重新查询，不是已截获引擎 `UpdateTimeAndHandleMaxTickRate` 内部的 `GivenMaxTickRate`。普通基线只读边界；独立诊断另写按 global frame 精确配对的 `.pacing.csv`，记录原始 FApp delta／idle／overshoot，不改变旧 `frames.csv` 列。Idle 不是 OS 调度分类，不能把剩余时间冒充 CPU 执行。
- `-ClientOcclusion Inherited` 为默认，不追加渲染参数。`Buffer3` 只给唯一渲染客户端加本轮 `r.NumBufferedOcclusionQueries=3` ini 覆盖；`HZB` 只加 `r.HZBOcclusion=1`，二者不能作为同一轮选项叠加。Case、experiment、result、summary 记录模式与独立名称，不改生产 `Config/`，服务器和 NullRHI peer 不受该选项影响。
- 渲染端必须读回 `r.NumBufferedOcclusionQueries`、`r.HZBOcclusion`、`r.OneFrameThreadLag`、`r.RHICmdBypass`；覆盖值未实际生效即失败，尤其不能认为 Shipping 收到命令行就一定应用。先用 Editor 同场景各三轮做单因素对照，Shipping 后续只确认选中的候选；不同模式／构建不能合并成一个性能均值。
- Buffer3 增加遮挡历史延迟，要复验快速转头、出掩体和显隐跳变；HZB 结果更保守，可能多画物体，且其 readback lock／RHI 提交仍可能等待。不能预告它“消除 GPU 等待”，也不能靠增加流水线延迟隐藏瓶颈。需要同时看平均帧、RT／RHI scope、实际画面与固定活动人口。
- `-ContinuousFloorChecks` 是地面缓存的反向消融对照，默认关闭，不改资产配置。显式开启时只给本轮 DS 传 `-PvpBaselineAlwaysCheckFloor`，等全部 160 个敌人和两玩家就绪后、预热前一次性设置敌人 CMC 的 `bAlwaysCheckFloor=true`，不在生成阶段或采集循环每帧写入。Case 增加 `_FloorReference`，清单和结果记录该选项，并严格核对服务器 START／STOP 的 `movement.alwaysCheckFloorEnemies` 为 160；默认固定场景应为 0。必须与同一六凸包构建、相同节拍及渲染参数的默认组比较，不拿旧两代理 B3 当地面缓存对照，不改变正常游戏入口。
- `-CpuDiagnostic -BuildKinds Editor` 是**不依赖 Trace 的独立 CPU 诊断**，允许 `-Repeat 3`；不与 Trace／Scheduler／PresentMon 混用。只有 DS 与渲染端增加已有原生 `-PvpBaselineCpuAccounting`，读取 Windows `GetThreadTimes` 并写相同 schema-2 `frames.csv` 和独立 `pacing.csv`，无 `.utrace`、命名事件、引擎 CSV profiler 或 ETL，不触发专属于 Trace 的 C 盘 5 GiB 守卫。直接采样和 pacing 的开销仍在，Case 增加 `_CpuDiagnostic`，`cpu-diagnostic-manifest.json`、清单及汇总明确 `DiagnosticOnly=true`／`FormalBaseline=false`，不能混进正式 B1。
- `-ServerTraceOnly` 必须与 `-DiagnosticTrace -DiagnosticTraceOnly` 同用，仅为 DS 写新 Trace；渲染端仍采 CPU／pacing，但没有客户端 Trace，不用于证明其 Worker／GPU 调用链。默认仍采双端，Case、清单和逐角色记录注明实际 `TraceRoles`，5 GiB 磁盘守卫不降低。
- 160 AI 验收同时要求所有敌人的行为树持续运行。服务器 START／STOP 另记录各目标管理器的内外环容量、可用／占用席位、等候人数、攻击许可及累计授予／回收／超时；客户端应无权威管理器。外环持有者仍排内环队列，不能将外环与队列相加当总人数。边界观测不申请席位、不触发补位，也不能替代逐次补位与死亡／超时的功能回归。
- 地面缓存与连续查询对照应在**同一六凸包构建**各做三轮、交错调用并使用独立运行名；`-Repeat 3` 只重复当前组，不自动切换地面模式。两组都加 `-CpuDiagnostic`，只有参考组再加 `-ContinuousFloorChecks`。先比较 native GT／World 的有效 CPU 累计量、每帧均值和实际移动／攻击人口；汇总 `MeanServerGTFrameCpuMs`／`MeanServerWorldCpuMs` 仅在启用 CPU 采样时有值，否则为 null。服务器 FrameEnd 与 FApp idle 只作节拍上下文，不用帧长变化冒充执行成本收益；无 OS 调度数据，仍不输出 Ready／Blocked 分类或 GPU／Worker 时间。

同场景的线程诊断使用独立单轮，不重建或覆盖上述三轮基线：

```powershell
.\Tools\Performance\RunGroupAIBaseline.ps1 -BuildKinds Editor -Repeat 1 `
  -DiagnosticTrace -DiagnosticTraceOnly -RunName '<新的纯英文运行名>'
```

- `-DiagnosticTraceOnly` 是本机已经验证的入口：仅给服务器和渲染客户端加 CPU／Frame／Task／Log Trace、客户端另加 GPU 和 `-statnamedevents`；不要求开启统计 HUD。非渲染 peer 不额外采 Trace。Trace 默认保存到 `C:/UEBuild/FPSDiagnostics/<RunName>/`，C 盘低于 5 GiB 时停止并保留无效证据。
- 必须显式加 `-DiagnosticTraceOnly`。未加该选项的实验路径会请求有限原生 CSV 并等待完整写盘，但本机真实启动已失败，**尚不能作为有效入口推荐**；不使用无限 CSV 后直接退出伪造完整文件。Shipping 诊断和重复轮次在入口拒绝。
- 带 Trace／命名事件的运行存在额外成本，原生摘要中旧的“未启用 Trace”文字不适用，外层 `diagnostic-manifest.json` 明确覆盖。只有全序列核实后的完整帧窗口才用于模块归因，不能直接套用 GFrameCounter 与 Insights 序号，或把加载、预热及退出计时混进排行。
- 本机 UE5.5 不支持 `TimingInsights.ExportFrames`。已验证的方法是导出 `FEngineLoop::Tick` 连续事件，与原始帧末间隔匹配唯一偏移后，用 `-startTime/-endTime` 导出窗口统计。该版本统计导出即使筛选 GT 也仍含 GPU 行，须结合 `Timers.csv` 类型识别；嵌套 Scope、并行线程和 GPU 时间不能相加。

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
- 功能回归固定核对 `BuildPvpDelivery.ps1` 中批准的必跑名单，不仅检查成功数量；缺项、重名、失败、未执行、进行中、明细与汇总不符均不能通过，带警告的成功单独记录。删除专项功能后须同步审核注册项与批准名单并重新运行，旧的 83／87／89 项记录保留为对应版本历史，不能替代当前版本验收。正常及受损网络检查均需服务器命中、换弹、死亡、重生，以及双端状态证据。深度门禁默认开启，还检查专用服务器身份、真实位移、双方时钟、逐发预测/接受或拒绝/ACK/历史命中、表现不重复及历史内存预算；明确拒绝单独统计，不能当作碰撞命中。默认受损条件为每个发送方向延迟 100 ms、波动 20 ms、丢包 2%，不是测得的 RTT；压力场景另按报告中的参数区分。
- 打包冒烟要求 Development Server/Game 的直接可执行文件及匹配构建记录，随后检查进程实际运行配置。默认使用无画面模式；`RunPvpSmoke -RenderedClients` 则让客户端使用真实 D3D12 离屏渲染，服务器仍无画面。它检查当前相机、武器附着、HUD 和敌人副本，保存初始双端及恢复后的 1280×720 PNG 并验证标记、身份、文件与尺寸；自动门禁不替代人工观感检查、命中公平性、人数容量或长时间稳定性验收。
- 多人 AI 门禁另外核对专服仅更新 Gameplay 重要性，两个客户端及恢复客户端实际应用各自表现预算。共享跟随者数量单独记录，近距离战斗中的敌人保持独立动画是正常情况，不强迫为测试进入共享池。
- 普通入口的远端动作验收使用 `Networking/RunFpsVisualAcceptance.ps1`：两台真实渲染客户端各自走到补给并通过服务器 Overlap 拾枪，再用 CMC 行走会合，交换开火、换弹、瞄准及冲刺姿态。不是烟测自动发枪、直接装备或传送。默认隔离 AI 为 0；`-FullGameplay -EnemyPopulation 1` 另外要求沿实际导航接敌、真实射杀 AI、玩家交火死亡和自动重生，以及双端死亡即时/0.8 秒稳定截图。`-Round 2 -FirstClient 2 -PacketLagMs 100 -PacketLagVarianceMs 20 -PacketLossPercent 2` 可复验交换顺序的中档弱网。功能档为 720p/30FPS/关闭光追与运动模糊，VSM 仅将中档物理页容量提至 1024，真实溢出仍使门禁失败；不能用它声称默认画质性能或实际键盘绑定已验收。PNG 和真实 Montage/附着记录必须同时具备，仍需人工查看关键帧。
- 上下身转向连续复验可单独加 `-FacingReview -FacingStartServerTime 130 -TimeoutSeconds 230`（不要与 `-FullGameplay` 同轮）：保留以上全部动作门禁，两端准备好后按同一服务器时间轮流输入小幅转头、左右大幅转向和上下俯仰，每端要求至少 40 张连续远端图。日志记录实际骨骼朝向、脚部位置、转身快照、网络 Mesh 平滑及相机方向；捕获短开火仍须有真实播放进度和 Slot 权重。130 秒是本机热启动复验示例，冷启动较慢时应显式延后共同起时并留足超时，不得跳过缺失画面或改写失败报告。
- 保留玩家脚部 IK 与 `ABP_PlayerFootIK`，不保留敌人脚部 IK、专属资源和优化分支。玩家专项沿用 `Networking/RunFpsFootIKAcceptance.ps1 -Scenario Facing`，检查双端行走、冲刺、停步和转身，并保留普通入口的原门禁；原有 `Enemy` 专项不再作为当前功能或验收要求。
- 玩家 IK 以当前输入姿态查地，保留真实鞋底／趾部标定、支撑权重和最终逐点防穿透，不移动 Actor／CMC 或改上半身动作图。该后处理类的 `CanRunParallelWork()` 固定返回 `false`，首次初始化、离地和落地都不依赖上一轮贴地资格决定线程许可；世界查询保留 Game Thread 断言。UE5.5 的主图／后处理共用 Mesh 求值门禁，因此安装此后处理的玩家身体 Mesh 主图与后处理求值均同步，不能写成“只有后处理同步、同一 Mesh 主图仍完整并行”。敌人未安装该玩家 IK，全局／敌人动画并行开关不因这项修复而关闭。初始化、起跳／落地和死亡契约检查不替代 `-useperfthreads` 下真实玩家生成的多进程验收。历史蒙皮、滑步与贴地报告保留于原 `Saved` 目录，不能当作删除专项功能后的新版本验收，也不能宣称零滑步、全地图适配或公网通过。
- 原单机完整流程使用 `Networking/RunFpsStandaloneVisual.ps1 -Case Both`：保留原地图、LevelScript AnyKey 开局、原 UMG 胜败/重新开始按钮。胜利仅使用显式临时 6 秒/1 敌人验收配置，失败由正常 CMC 接敌和真实 AI 伤害触发，不写生命或直接调用结算；重开必须是同地图的新世界和满血状态。它不会替换原菜单/HUD，报告不能代替截图观感、物理键盘绑定或声音验收。
- 严格网络体验仍默认要求 `Deep.NoUnexpectedShotRejections`。可另显式运行 `RunPvpSmoke.ps1 -WeakPolicyCorrectness`：只在有真实 stale 样本、其准入或执行年龄超过对应预算、拒绝未扣弹/进入命中执行、逐发最终 ACK 和弹药吻合、动作状态收尾后，继续验证硬断恢复；未来时间、身份、射速等错误不豁免。该模式的报告单独命名，旧零拒绝门禁仍可为 false，不能将政策正确性称为零拒绝体验或传输优化完成。没有 stale 的样本标为未覆盖，而不是这个边界通过。
- `Networking/MeasureFpsShotLatency.ps1 -RunDirectory <本轮证据目录>` 只读汇总预测/接受/拒绝/最终 ACK 缺失及同机 UTC 预测到 ACK 的 P50/P95/max；逐发区分准入和执行时的年龄、预算、服务器 Ping、最近客户端时钟样本。旧日志没有的字段保留 null；连接累计丢包不等于已定位每发的重传，不能靠批量到达就认定 HOL。
- `MemoryAccounting` 只度量历史形状数组的堆容量。专服至少两个角色须分配有效且有界的历史、视模主动画和 Tick 关闭；客户端不应分配权威历史。后处理实例可保留但不求值，不把这块缓存、对象计数或一次启动日志写成整进程 RAM/显存峰值测试。`-MovingHit` 仅用于开发专项，以零散布隔离几何链路，要求实际横移目标的服务器命中和复制血量降低，不修改正式配置资产或冒充命中率统计。
- `DeliveryReport.json` 记录源码提交、工作区是否有未提交修改、引擎版本、步骤、自动化统计、实际包程序哈希和符号位置；符号复制到本次输出目录，避免下次编译覆盖。带本地修改的结果可用于开发验证，但不能声称仅凭远端提交即可复现。输出不进入源码仓库；脚本不删除历史包。
- `.github/workflows/pvp-delivery.yml` 仅允许在受信开发分支手动运行。需自行注册标签 `ue55-server` 的 Windows 自托管执行机，并设置 `UE_ROOT`；仓库中的工作流文件不代表执行机已配置或 CI 已通过。CI 取得 Git LFS 实体后检查干净检出。日志/报告默认仅保留在执行机，显式选择后才上传 GitHub Artifact；公开仓库的产物不应视为私密，日志可能包含本机路径。工作流不上传第三方资源包或符号文件。

独立服务器首先是“没有本地玩家的独立游戏进程”，可以和两个客户端运行在同一台电脑。远程部署是下一层验收：把同一版本服务器包放在另一台机器，客户端连接其地址，检查进出、战斗、死亡重生和日志。只有实际完成后才能记录跨机通过；公网部署另需确认端口、安全及费用。

### 自动化流程的边界检查

先确认检查工具本身不会误报，再运行游戏测试。下面三条命令不启动 Unreal 或服务器，不进行 SSH，也不产生新的游戏通过记录：

```powershell
.\Tools\Networking\BuildPvpDelivery.ps1 -SelfTest
.\Tools\Networking\RunPvpSmoke.ps1 -SelfTest
.\Tools\Performance\TestRenderCostConfig.ps1
```

- 构建入口的自测构造缺项、重复项、失败明细、错误计数、空报告和过期报告等反例；正式回归复用同一判定函数。批准名单是当前规则回归的最低合同，不代表所有玩法或网络情况都已覆盖；新增、删除或有意更名用例时须同步维护名单。
- 网络入口的自测检查逐发账本和场景判定。没有最终 ACK、裁决冲突、重复执行、孤立记录不能冒充完整射击链；空观察端不冒充主动完成端。未启用的专项与已通过专项分开记录。
- 配置自测检查参数类型、范围、配置优先级、地图缺失、参数读回及无效样本边界。它证明采集配置检查有效，不证明当前多人性能达标。

已有游戏用例按以下边界组织，不以用例数量代替覆盖结论：

| 领域 | 现有自动检查重点 | 仍需另外验收 |
|---|---|---|
| 武器与 GAS | 换弹取消、超时、旧通知、同能力重启、回调重入、配置异常 | 真实按键、输入焦点、双端动作观感 |
| 网络战斗 | 权威修改、逐发身份、历史时间与几何边界、状态清理 | 当前构建的真实弱网、移动命中与双向主动战斗 |
| 敌人 AI | 失去目标、停止与退出、寻路失败、请求归属、槽位占用、EQS 缺导航 | 多人场景下真实追击、拥堵、容量和动画质量 |
| 生命周期与恢复 | 死亡取消、资源归还、旧重试作废、恢复状态与 HUD 重绑定 | 原进程自动重试、超期恢复及完整画面恢复 |
| 资产与表现 | 骨架与挂点、动画配置、武器附着、死亡物理与迟到状态 | 双渲染客户端、落地姿态和人工画面检查 |
| 采集流程 | 停止与取消、迟到采集回调、暂停看门狗、样本有效性 | 当前多人性能预算、长时间内存趋势与自动退化告警 |

完整验收顺序是：工具自测 → 同构建功能回归 → 正常/弱网联网 → 按需进行渲染、打包与公网验收。某一层通过不能替代后一层；本轮没有启用的专项必须保留“未执行”范围。历史报告可离线检查结构，但不能因此升级成当前版本验收。

当前仍有明确边界：网络运行已有超时检查，CI 整个任务限时 180 分钟，但本地编译/Cook 尚无逐阶段超时；带警告用例会列出名称和数量，新增警告仍需审查。工具自测通过不会消除现有弱网或恢复功能失败，也不会自动批准性能基线。

### Win64 客户端与 Linux Server 成对构建

`LinuxPackage` 使用同一个 UE 5.5 源码引擎构建 Win64 Game、Linux Server 和负责 Cook 的 Editor；需要已安装的 v23 Linux 交叉编译工具链。`CookEngineRoot` 默认等于 `EngineRoot`，显式传入时也必须是同一解析后的引擎根目录。不能仅凭版本号／CompatibleCL 相同就混用安装版 Cooker 与定制源码运行时，也不通过覆盖 NetVersion 掩盖差异。

Cook 显式使用 UAT 的 `-VersionCookedContent`，保存带字段名的属性和版本信息，但它不能修正 Shader／View Uniform Buffer 布局或自定义原生 `Serialize` 差异。本轮实际捕获过定制源码 Game＋官方 Cooker 在首次绘制时的 View UB 崩溃，因此不同根目录在构建和输出创建前即拒绝；不能靠序列化选项或相同版本号绕过。源码引擎须先有可正常构建的配套 Editor；本机该配套 Editor 尚有大量过期引擎产物，完整重建与 Linux／公网运行未在本轮完成。最终仍必须加载真实地图并执行联网门禁。

```powershell
# 先检查前置条件和计划；实际构建时去掉 -PlanOnly。
.\Tools\Networking\BuildPvpDelivery.ps1 `
  -Mode LinuxPackage `
  -EngineRoot '<UE5.5源码引擎目录>' `
  -LinuxToolchainRoot '<v23_clang-18.1.0-rockylinux8目录>' `
  -OutputRoot '<独立输出目录>' `
  -ServerDebugInfo Full -MinimumTests 83 -MaxParallelActions 4 -Port 7797 -PlanOnly
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

- 2026-10-17 最新本地正常网络 `PvpSmoke_20261017_161027_a0630d65` 通过全部开启门槛：执行端 34 发均有唯一 Accepted/ACK，未决计数归零；服务器每角色形状缓存 67,584 字节、主视模动画/Tick 关闭，两个客户端历史容量为 0。此轮显式关闭重连，不能用于新版本重连验收。移动命中专项 `PvpSmoke_20261017_160303_9354d3e3` 通过，其零散布测试范围另见报告。
- 同日弱网 `PvpSmoke_20261017_160253_dd8db00c` 未通过严格射击门槛：38 发都有终结 ACK，但 2 发超时拒绝，不能算无漏判或所有射击成功。真实 D3D12 双客户端 `PvpSmoke_20261017_160809_3b7b7e97` 出现显存超预算和卡顿，已主动结束，未通过画面验收；不隐藏失败、不复用旧版公网通过结论。

- 新增 PvPvE／恢复的本地 Editor 正常网络报告 `PvpSmoke_20261016_120700_7860b3c5/Report.json` 与模拟弱网报告 `PvpSmoke_20261016_120855_c0c7753d/Report.json` 均通过全部默认门禁。两轮真实离线伤害均使生命 90→80，恢复保持 80、各自原弹药与成绩，三端 Pawn/PlayerState NetGUID 相同；不要求跨端 FName 相同。弱网为每发送方向额外延迟 100 ms、波动参数 20 ms、丢包 2%，不是测得的 RTT。此结果不代替最新 Linux 包、公网、原进程自动重试或画面验收。该版本的 66 项规则回归也已通过（40 项无警告、26 项带警告）。

- 成对构建报告 `20261016_020904_233d4919/DeliveryReport.json`：Editor、Win64 Game、Linux Server 编译及双端打包通过，60 项自动化成功（37 项无警告、23 项带警告）。构建报告的 `RuntimeVerified=false` 保留原意，运行结果由下述独立报告证明，不回填伪造构建阶段证据。
- 公网打包验收 `PvpSmoke_20261016_022020_a991b9ac/Report.json`（正常）与 `PvpSmoke_20261016_022105_08b0d1f7/Report.json`（模拟弱网）均通过。使用本次成对构建的 Win64 和 LinuxServer 包；远端 Linux 包全部 45 个文件与本次本地归档逐文件 SHA256 对比一致，部署记录为 `Saved/Networking/Deployment_20261016_020904_233d4919.json`。实际程序及双方收据的哈希记录在运行报告中；未覆盖 NetVersion。
- 两轮均为两个 Win64 客户端连接远端普通用户运行的 Linux Dedicated Server。弱网为每发送方向额外延迟 100 ms、波动 20 ms、丢包 2%；不是测得的 RTT，也没有量化误差上界。每轮由一个客户端执行战斗、另一个观察复制，报告中未分配给该角色的单项可为 `false`，整体必须满足同一执行端和同一观察端的组合门禁。
- 测试后 FPS 临时服务和 UDP 7797 监听已退出；原 Co-op 服务保持运行。未配置常驻 FPS 服务。本地 Editor 报告 `PvpSmoke_20261016_012337_0c1c8408`、`PvpSmoke_20261016_012512_774e7f81` 与失败尝试继续保留；符号、原始日志和云端访问资料保持内部保存。上述结果不等于画面、容量或长时稳定性验收。
