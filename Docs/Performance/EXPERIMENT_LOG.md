# 多敌人场景性能优化：完整实验报告

本报告只回答三个问题：敌人数增加时成本从哪里来；已实施策略实际减少了什么；敌人成本下降后，整帧为何仍受渲染链限制。结论按实验推进顺序组织，逐轮数据、异常样本、Trace 事件和文件对应关系集中放在[实验附录](../../PERFORMANCE_EVIDENCE.md)。

## 2026-09-29：小装饰物距离剔除与补充 LOD（无整帧收益，已撤回）

**状态：已按用户要求撤回。** 实现和保存重载检查通过，但零敌人验收未确认整帧收益；候选地图、网格和专用 Python 工具已移出 `Content/`、`Tools/`，归档至 `Saved/Profiling/PropVisibility0_20260929/WithdrawnCandidate/`，可以恢复。原始采集仍有效，数据不删除、不改成无效样本；不追加 160 敌人测试。本轮未修改默认地图、敌人、灯光或已验收的画质参数。

### 方案与实现

从当前 `Demonstration_Baked` 创建独立的 `/Game/PerformanceCandidates/PropVisibility_Scene20260929/Demonstration_Props`，不叠加未采用的 Nanite 或已撤回的 ISM 候选。审计的 544 个小件都带碰撞，多数已有 LOD，因此不统一隐藏可能充当掩体的托盘、油桶、混凝土块和堆叠管件，也不重复生成已有 LOD。

- 只对 **146 个静态装饰组件**设置最大绘制距离：57 个电箱、64 个通风管端部、25 个管道弯头。按组件尺寸设置 **120–157 米**，超过 3 米的构件不进入本轮距离规则，已有更短距离不放宽；建筑、主要遮挡物和 10 米长直管保持原状。
- 对原本只有一个 LOD 的 `Pipe_B_90` 创建独立网格副本，25 个组件引用该副本；三层三角形数为 **880 / 439 / 219**，屏幕尺寸阈值为 **1 / 0.15 / 0.06**。LOD0、材质、碰撞 LOD0、原碰撞体及组件碰撞/导航设置保留。
- 当时由 `Tools/OptimizeScenePropVisibility.py` 集中管理名单、距离规则和 LOD 参数，性能采集仍通过统一 Runner。该专用工具现已随候选撤回归档，不是当前工具入口；下文地图路径仅用于对应历史记录。

首轮校验发现新增 LOD 后资产 Bounds 扩展约 **0.991 cm**，停止生成地图。核对 UE 5.5 `StaticMeshBuilder.cpp` 后确认，渲染 Bounds 合并所有 LOD 的包围范围；低精度几何可能略微超出 LOD0。修正检查方式：逐 Section 比较 LOD0 顶点、索引、法线、UV0 和切线签名，再验证资产 Bounds 与三个 LOD 的实际并集一致，并限制扩展不超过资产尺寸的 1%。LOD0 签名完全一致，其他碰撞/材质字段仍严格匹配，没有直接放宽全部几何检查。首次生成的网格可恢复地归档至 `BuildAttempt01/`。

保存重载后，146 个组件的有效绘制距离、引用和状态检查通过；原地图与 7 个源网格的 SHA256 均未改变。距离规则另通过 7 个边界用例，覆盖尺寸上限、距离下限和保留已有更短距离。

### 零敌人验收结果

沿用 `scene-acceptance.json`，仅覆盖进程的候选地图和 `Counts=0`：1600×900，种子 1337，固定镜头，20 秒预热、180 秒采集，无 Trace，不裁剪样本。参考同日早前完成的当前方案采集；两者各一次、时间分开，不作为重复配对的因果或稳定收益证明。

| 指标 | 当前烘焙地图 | 距离剔除＋LOD 候选 | 候选减当前 |
| --- | ---: | ---: | ---: |
| Frame / ms | 11.031 | 11.226 | +0.195 |
| P95 / ms | 12.329 | 12.706 | +0.377 |
| P99 / ms | 13.533 | 14.037 | +0.504 |
| Render Thread / ms | 10.620 | 10.787 | +0.167 |
| RHI Thread / ms | 7.798 | 7.606 | -0.192 |
| GPU / ms | 9.710 | 9.809 | +0.099 |
| 可见性等待代理 / ms | 1.972 | 1.888 | -0.084 |
| RHI Draw Calls | 1868.5 | 1871.5 | +3.0 |
| Shadow Draw Calls | 932.0 | 933.9 | +1.9 |
| ShadowDepths / ms | 0.740 | 0.741 | +0.001 |
| TextureStreaming/WantedMips / MB | 145.695 | 145.695 | 0.000 |

**分析与决定：** RHI 和等待代理下降，但 Frame、尾帧与 GPU 没有同步改善，绘制数量和阴影耗时基本不变。该小件名单在本次固定视角下未体现出值得纳入默认方案的收益；不据此声称距离剔除/LOD 普遍无效，也不将单次帧时上升直接归因于候选。已撤回候选、保留本地可恢复归档和数据供复盘，停止扩大本轮范围，不追加战斗或动态路线验收。

本轮 `Valid=True`、`IsolationValid=True`、`InputIdentityValid=True`，16000 帧全部纳入，CSV 正值 Frame 累计 179.615284 秒，进程退出码 0，VSM 队列溢出和纹理池告警均为 0。固定视角截图未见明显缺件或材质变化；不等同于完成距离切换和 LOD 跳变的动态画面验收。

证据：`Saved/Profiling/PropVisibility_Scene20260929/` 保存审计、构建和重载校验；`Saved/Profiling/PropVisibility0_20260929/` 保存原始 CSV、日志、截图、隔离记录、汇总、`decision.json` 和 `withdrawal.json`。归档的地图、网格和脚本 SHA256 与撤回前一致。默认地图与公共验收配置仍为 `Demonstration_Baked`，没有新增正式性能收益。

## 2026-09-29：静态硬表面 Nanite 转换（绘制计数下降，未采用为默认方案）

**状态：实现与保存重载校验通过；本轮没有确认有效的整帧收益，不修改默认地图，不追加 160 敌人测试。** 候选资产保留供复盘，不能写成已交付的帧率提升；此前已撤回的 ISM 方案仍保持撤回。

### 方案与实现边界

在已验收的 `Demonstration_Baked` 基础上，用 `Tools/Assets/EnableSceneNanite.py` 创建独立地图 `/Game/PerformanceCandidates/Nanite_HardSurface20260929/Demonstration_Nanite`。仅复制白名单网格并切换其静态组件引用，不合并 Actor，不调整灯光、敌人、分辨率或其他画质参数。8 种资产共 454 个组件：托盘 204、通风管端部 64、通风直管 44、混凝土块 45、筒仓 42、油桶 33、车厢 21、建筑框架 1。

本批限定普通静态网格 Actor、静态组件、全部不透明材质，并排除交互/附着、逐实例顶点色及已启用 Nanite 的对象。为避免同时改变光追/复杂碰撞几何，Fallback 保留 100% 三角形、相对误差 0。保存重载核对了组件变换、材质、碰撞/导航与渲染标志，回退 LOD 三角形数、Section 数和碰撞设置一致；包围盒重建最大数值误差约 `0.000031 cm`。原地图及 8 个源网格的文件 SHA256 未变。新建的 8 个网格包合计 857476 字节，不代表运行时显存增量。

转换工具初次被 UE 未暴露的 BodySetup 校验属性拦截；随后因将包围盒浮点重建误差当作严格不一致而停止。修正为仅对 Bounds 容许 0.001 cm 误差、其他字段仍严格匹配。失败生成的单个候选资产移至 `Saved/Profiling/Nanite_HardSurface20260929/BuildAttempt02/`，未混入测试地图，也未覆盖原资产。

### 零敌人验收结果

沿用 `scene-acceptance.json`，仅覆盖本次进程的 `-Counts 0` 与 `-Map`：1600×900、固定镜头、种子 1337、20 秒预热、180 秒采集、无 Trace、不裁剪首尾。参考同日已经完成的零敌人基线，各一次，不是重复配对或稳定收益证明。

| 指标 | 当前烘焙地图 | Nanite 候选 | 候选减当前 |
| --- | ---: | ---: | ---: |
| Frame / ms | 11.031 | 11.008 | -0.023 |
| P95 / ms | 12.329 | 12.313 | -0.016 |
| P99 / ms | 13.533 | 13.849 | +0.316 |
| Render Thread / ms | 10.620 | 10.572 | -0.048 |
| RHI Thread / ms | 7.798 | 7.731 | -0.067 |
| GPU / ms | 9.710 | 9.675 | -0.035 |
| 可见性等待代理 / ms | 1.972 | 2.268 | +0.296 |
| RHI Draw Calls | 1868.5 | 1791.6 | -76.9 |
| Shadow Draw Calls | 932.0 | 911.7 | -20.3 |
| ShadowDepths / ms | 0.740 | 0.747 | +0.007 |
| TextureStreaming/WantedMips / MB | 145.695 | 145.695 | 0.000 |

**分析与决定：** 总绘制计数下降约 4.1%，但整帧、RT、RHI、GPU 只出现很小的变化，不能宣称取得稳定整帧收益；ShadowDepths 未下降。这说明本批对象的绘制计数减少，没有在本次固定视角采集中转化为明显的关键路径改善，不证明 Nanite 对其他资产或视角无收益。P99 增加不能直接归因于 Nanite：日志也记录了采集期间的 DDC 后台维护，且两次顺序采集不是多轮统计结论。不继续扩大转换范围或追加其他开关测试。

`Valid=True`、`IsolationValid=True`、`InputIdentityValid=True`；16317 帧全部纳入，CSV 正值 Frame 累计 179.616 秒，进程退出码 0。VSM 队列溢出和纹理池告警均为 0，不代表历史告警触发路线已经治理完成。固定视角前后截图未见明显缺件或材质替换，未新增动态画面验收。

证据：`Saved/Profiling/Nanite_HardSurface20260929/build.json`、`verification.json` 保存资产与校验记录；`Saved/Profiling/NaniteHardSurface0_20260929/` 保存原始 CSV、截图、日志、环境指纹、汇总和 `decision.json`。默认地图和公共验收配置仍为 `Demonstration_Baked`。

## 2026-09-29：ISM 候选零敌人筛选（无效优化方案，已撤回）

**状态：无效优化方案，已撤回。** 未获得整帧收益，不进入正式配置或后续验收。候选地图已移出 `Content/`，专用生成脚本已移出 `Tools/`，一并归档至本次证据目录的 `WithdrawnCandidate/`，可以恢复。这里标记的是优化方案无效；原始采集本身有效，`manifest.csv` 中的 `Valid=True` 保留，不篡改实验数据。

目的：验证已经生成的“514 个重复静态构件 → 56 个 ISM 渲染组件”是否实际减少场景提交成本。只切换本次进程的地图为 `StaticInstances_Scene20260928V2/Demonstration_Instanced`，不改默认地图、敌人或画质。沿用刚完成的当前方案零敌人采集作参考，同为 1600×900、20 秒预热、180 秒采集、固定视角、无 Trace，各 1 次；游戏 DLL、DefaultEngine.ini 和 DefaultGame.ini 的 SHA256 一致。

| 指标 | 当前烘焙地图 | ISM 候选 | 候选减当前 |
| --- | ---: | ---: | ---: |
| Frame / ms | 11.031 | 11.124 | +0.093 |
| P95 / ms | 12.329 | 12.350 | +0.021 |
| P99 / ms | 13.533 | 13.322 | -0.211 |
| Render Thread / ms | 10.620 | 10.697 | +0.077 |
| RHI Thread / ms | 7.798 | 7.717 | -0.081 |
| GPU / ms | 9.710 | 9.809 | +0.099 |
| 可见性等待代理 / ms | 1.972 | 2.124 | +0.152 |
| RHI Draw Calls | 1868.5 | 1980.5 | +112.0 |
| Shadow Draw Calls | 932.0 | 1002.8 | +70.8 |
| TextureStreaming/WantedMips / MB | 145.695 | 215.164 | +69.469 |

**分析与决定：** RHI 和 P99 有小幅下降，但 Frame、Render Thread、GPU 没有同步改善，Draw Calls 反而增加约 6%，WantedMips 也上升。这一轮没有达到“整帧有收益再进行 160 敌人回归”的条件，因此不采用该候选，不追加 160 敌人测试，不继续扩大实例化范围。单次顺序采集不证明长期必然回退，也不证明所有 ISM 方案无效；它足以说明本候选不能作为已验证的优化成果。

检查生成脚本及原保存重载校验记录：514 个原组件的可见性、投影和光追标志已关闭，碰撞/导航保留，56 个替代 ISM 不额外创建碰撞/导航几何；未发现脚本漏关原组件的相应渲染标志。此次固定视角截图未见明显构件缺失、错位或材质变化；这不代替动态、多视点验收。当前没有逐 Draw/Pass 的捕获证据，不能进一步断言增加的绘制来自哪个对象或剔除步骤。

采集结果 `Valid=True`、`IsolationValid=True`、`InputIdentityValid=True`；VSM 溢出和纹理池超预算告警均为 0，16146 帧全部纳入统计。当前默认地图和验收预设仍为 `Demonstration_Baked`；撤回前后已核对其文件与默认配置未变。基线：`Saved/Profiling/SceneAcceptance_20260929_PrelaunchFixed/`；候选证据：`Saved/Profiling/ISMValidation_0_20260929_0049/`，包含原始 CSV、截图、日志、隔离记录和汇总。该目录的 `decision.json` 记录撤回决定，`WithdrawnCandidate/` 保存退出项目资产目录的候选地图及原生成工具，归档文件 SHA256 与撤回前一致。

## 2026-09-29：统一采集入口实测（当前方案复测，非新增优化收益）

使用 `Tools/Performance/ExperimentProfiles/scene-acceptance.json`：已验收的 `Demonstration_Baked`、1600×900、0/160 敌人各 1 次、20 秒预热与 180 秒采集、固定视角、随机种子 1337、玩家测试生命值 1000000、不启用 Trace、不裁剪首尾。读回确认硬件查询开启、HZB=0、Buffer2、反射下采样=2、TSR History=150；未测试或启用新的 ISM 地图。

| 指标 | 0 敌人 | 160 敌人 |
| --- | ---: | ---: |
| Frame / ms | 11.031 | 13.276 |
| FPS（1000 / 平均 Frame） | 90.66 | 75.33 |
| P95 / ms | 12.329 | 14.817 |
| P99 / ms | 13.533 | 16.349 |
| Game Thread / ms | 2.793 | 8.335 |
| Render Thread / ms | 10.620 | 12.218 |
| RHI Thread / ms | 7.798 | 10.123 |
| GPU / ms | 9.710 | 11.825 |
| 可见性等待代理 / ms | 1.972 | 2.739 |
| RHI Draw Calls | 1868.5 | 2574.5 |
| 纳入统计的 CSV 帧数 | 16283 | 13530 |

两组 `Valid=True`、`IsolationValid=True`、`InputIdentityValid=True`，游戏进程均正常退出；VSM 非 Nanite 队列溢出与纹理池超预算告警均为 0。160 敌人组结束时存活数为 160，采集开始的投影/光追主 Mesh 数为 5/12。CSV 正值 Frame 累计时长分别为 179.615 / 179.618 秒，与配置定时器的 180 秒采集窗口口径不同。

结论：统一配置后的实际采集链路可用，当前方案仍主要受渲染侧限制。此轮不是修改画质或实例化的 A/B，不新增优化收益结论；固定视角单次采集也不替代动态玩法与多次重复验证。

本轮同时修复两处测试工具兼容问题：Windows PowerShell 5.1 序列化 `Get-Content` 附带的文件系统元数据会长时间占用资源，改为读取纯 INI 字符串；中文汇总脚本补 UTF-8 BOM，避免旧 PowerShell 按系统编码误读。前两次启动前诊断没有启动游戏，不计入样本。实际两组原始数据有效；汇总编码修复后仅重跑离线汇总，没有重跑游戏。新增对应回归后，56 项配置检查通过。

证据目录保留当时运行名：`Saved/Profiling/SceneAcceptance_20260929_PrelaunchFixed/`。其中 `environment.json` 保存配置与指纹，`manifest.csv` 保存有效性和告警，`run_summary.csv` 保存上述完整指标，`summary.md` 为自动汇总，两组子目录包含原始 CSV、截图、启动日志及进程隔离记录。

## 2026-09-28 追加候选：重复静态构件实例化（历史实现记录；09-29 已标记无效并撤回）

目标只针对场景绘制提交，不修改敌人、灯光布局或已保留的画质参数。先按网格、材质、渲染属性和 50 米空间网格分组，再排除交互蓝图、动态/附着对象、实例顶点色、负缩放、特殊材质，以及不支持现成 ISM 着色器的对象。446 个独特的烘焙样条网格不在本轮范围。

- 在独立候选地图中，将 **514 个重复构件的渲染表示归入 56 个 ISM 组件**，主要是托盘、混凝土块、油桶和管件。原 Actor 与碰撞组件保留，继续承担碰撞、导航和对象引用；这不是删除 514 个 Actor，也不是已测得减少 458 次 Draw Call。
- 保存重载后验证了实例数量、世界变换、材质、渲染属性和原有碰撞/导航策略；514 个实例全部通过，变换检查最大误差约 `1.82e-12 cm`。共享网格和材质资产未修改。
- 初版候选的根组件变换重载不一致，被校验拦截。改为单位变换的拥有者并在添加实例前固定组件 Mobility 后复验通过；失败地图归档在 `Saved/Profiling/StaticInstances_Scene20260928/Discarded/`，不用于测试或默认入口。
- 实验时的候选地图为 `/Game/PerformanceCandidates/StaticInstances_Scene20260928V2/Demonstration_Instanced`，现已从 Content 撤回。原 `Tools/OptimizeStaticMeshInstances.py` 同步撤回，二者归档于 `Saved/Profiling/ISMValidation_0_20260929_0049/WithdrawnCandidate/`。原清单与验证结果仍保留在 `Saved/Profiling/StaticInstances_Scene20260928V2/build.json`，其中的路径用于历史复盘，不是当前可运行入口。
- 曾启动一次零敌人参考采集，但与另一个项目的 UE 自动化进程重叠，`IsolationValid=False`、`Valid=False`。记录保留在 `Saved/Profiling/StaticInstances_Before0_20260928/`，**不作为收益证据**，候选尚未测性能。
- 当时用户决定：**先保留候选，暂不测性能。** 默认地图和 `baseline160.json` 仍指向已验收的 `Demonstration_Baked`，没有启用此次 ISM 候选，也没有新增 FPS/RT/GPU 收益结论。

09-29 已恢复并完成上述候选的零敌人筛选，结果见本报告开头。未获得整帧收益，已按用户要求标记为无效优化并撤回；不再安排该候选的 160 敌人回归，也不保留为待验收事项。

## 2026-09-28 更新：场景优化已落地，停止扩展候选

**review 同步说明：** 已同步已验收方案的代码、脚本和默认参数，保留复习注释及完整资产。原地图在切回 `review` 后与此前发布时的校验值不同，因此不覆盖它；本分支改为默认打开已验收的 `Demonstration_Baked`，`baseline160.json` 使用相同地图。下文应用回原地图的描述是此前验收时的历史记录。此次为版本同步与入口校验，没有新增性能实验或收益数据。

同步时还补齐了旧 `enemy_BP` 到现有 `BP_FPEnemyAIController` 的绑定，未重建行为树。Development Editor 编译通过，31 项配置检查通过，20 项 UE Automation 复验通过（其中 5 项带警告，失败 0）。报告位于 `Saved/Automation/ReviewSync20260928_Final/`，绑定前的蓝图备份位于 `Saved/ReviewSync20260928/`。

本轮不重查遮挡等待链、不改敌人。围绕零敌人的实例准备与 TSR 热点，完成轨道/道砟静态化和 TSR 历史缓冲调优。按最新约定，取消逐项三轮重复测试，方案完成后统一验收。

### 实现与检查

- 224 段轨道、222 段道砟的**变形后几何**烘焙为普通静态网格，已应用到原 `Demonstration` 地图。材质、各级 LOD、阴影和光追参与保留；原样条改为仅承担碰撞/导航，新渲染组件使用 `NoCollision`，不修改敌人代码。
- TSR `r.TSR.History.ScreenPercentage=150` 已写入默认配置；输出分辨率、内部渲染比例、Buffer2、反射下采样等其他策略不变。
- 446 个构件、894 个 LOD 的变换、材质、LOD 阈值和三角形数量检查通过；变形几何包围范围最大误差约 0.000015 cm。保存重载检查通过。新增网格资产约 63.9 MiB，不是没有资源代价的优化。
- 修正了两个转换细节：仅设置 `CollisionEnabled` 会被原碰撞 Profile 在重载时恢复，因此持久化 `NoCollision`；新建静态网格的远级 LOD 默认会再次简化，因此显式保留已提取的各级几何。
- 灯光拥有者检查未发现启用的 Actor Tick，项目 C++ 也未发现逐帧重复设置灯光参数的入口。本轮不强改光照逻辑，不扩展 Lumen 候选。

### 统一验收结果

UE 5.5.4，1600×900，种子 1337，固定玩家/镜头；0/160 敌人分别预热 20 秒、采集 180 秒，各一轮，无 Task Trace。玩家临时高生命，伤害与敌人行为仍运行。测试对象是静态化候选地图；通过后用同一替换逻辑应用回原地图，并检查重载后的 446 个替换对象。

| 指标（ms） | 0 敌人 | 160 敌人 |
| --- | ---: | ---: |
| Frame | 11.077 | 13.271 |
| P95 / P99 | 12.446 / 13.774 | 14.675 / 15.667 |
| Game Thread | 2.786 | 8.544 |
| Render Thread | 10.649 | 11.972 |
| RHI Thread | 7.792 | 10.209 |
| GPU | 9.728 | 11.839 |
| GPU TSR | 1.256 | 1.311 |

两轮 manifest 均有效，VSM 队列和纹理池告警均为 0；160 敌人结束时仍存活 160，玩家有正常受伤。此前同二进制短 Trace 参考值为：0 敌人 Frame/RT/RHI/GPU = 11.918/11.561/9.032/10.381 ms；160 敌人 = 14.893/12.618/12.005/12.697 ms。**参考值与本轮的采集时长、Trace 开关不同，不包装为新的三轮同条件 A/B。** 本轮保留完整样本；采集起点的截图停顿单列为采集扰动，不偷偷删除。

此前样条光追 On/Off 短测中，RT 实例准备 1.708→0.367 ms、Frame 11.721→10.952 ms，只用于确认优化方向，不能算作烘焙收益。最终配置没有关闭样条光追。未完成的 `TSRVerify0_20260928` 三轮矩阵已按用户要求中止，不作为三轮证据。

### 分工与收尾边界

| 工作 | 负责方 | 状态 / 交付 |
| --- | --- | --- |
| 构件转换、LOD/材质/碰撞检查、默认参数落地、回退备份 | Codex | 已完成；保留烘焙和核验脚本 |
| 0/160 自动采集、帧时/尾帧/消费者/告警核验 | Codex | 已完成；原始 CSV、截图、日志和 manifest 保留 |
| 新方案快速转镜时围栏细线、武器拖影、反射/阴影观感 | 用户 | 待最终确认；此前旧方案手动验收不替代本次 TSR 变更的画质判断 |
| 用户发现具体异常后的单点修正或回退 | Codex | 仅针对明确反馈处理，不重开全面调查 |

不宣称全程锁定 60 FPS、不宣称 VSM 所有路线的溢出已修复，也不宣称查询生产端的 query/fence 与 GPU Pass 已精确归因。本轮自动结果支持保留方案；运动画质仍单独验收。不再增加光照、Lumen 或敌人侧优化任务。

证据：`Saved/Profiling/SceneOptimizationAcceptance_20260928/`；资产核验与落地记录：`Saved/Profiling/SplineBake_Tracks20260928/`。原地图备份为该目录的 `Backup/Demonstration.umap`；恢复此备份并将 TSR History 改回 200 即可回退本轮场景/画质修改。试制资产已移至该目录的 `DiscardedCandidates`，可恢复；正式烘焙资产仍保留在 `Content/PerformanceCandidates/SplineBake_Tracks20260928/Meshes/`。

## 截至 2026-09-26 的阶段性结论（以下为历史实验主线）

在固定地图、1600×900、160 敌人的三轮平衡顺序对照中，当前组合配置相对原配置：平均帧时间 **14.955 → 14.279 ms**，P95 **16.415 → 15.696 ms**，P99 **17.254 → 16.409 ms**，约 **66.87 → 70.03 FPS**。RT 从 14.689 降至 13.160 ms，GPU 从 13.321 降至 12.704 ms；RHI 从 10.597 升至 11.333 ms。组合包含遮挡查询缓冲数 1→2 和 Lumen 反射下采样因子 1→2，不能把组合整帧收益分别归给其中任意一项，也不能相加单项实验的收益。

另一批当前默认配置的 0/160 敌人对照中，0 敌人平均帧时间仍有 **11.868 ms**，160 敌人为 **14.296 ms**。敌人增加了 GT、绘制提交和 GPU 工作，但零敌人也存在显著场景底座。Task Trace、任务完成事件和查询开关干预已将一类 RT 长等待追到**历史硬件遮挡查询结果的 GPU fence 同步**；目前没有把具体 query/payload/fence 与某个 GPU Pass 直接绑定，不能声称某一项 Lumen 或阴影 Pass 单独造成全部等待。

这些数值来自 Development Editor 独立进程下的固定玩家/镜头成本测试。为了完成长采样，玩家生命在这类实验中临时覆盖为 1000000；HUD、声音、伤害路径仍运行，但这不是正常生命、自由移动的完整玩法验收，也不是 Shipping 构建成绩。报告中的性能数值对应各批保存的二进制与输入指纹，不自动代表以后重新编译的结果。

## 实验口径

后期对照统一使用 UE 5.5.4、`/Game/FactoryDistrict/Maps/Demonstration`、1600×900、VSync Off、固定随机种子 1337；独立进程预热后采集。每批以自己的 manifest 校验实际敌人数、策略参数、进程隔离、输入指纹与采集完整性；不同批次的绝对帧时间不直接作单变量比较。

平均、P95、P99 分别报告。GT、RT、RHI 与 GPU 可并行且跨帧流水，不能相加；父子 Timer 不能相加。CSV 的可见性等待是稳定代理，不等于某个 Insights scope 的精确时长。早期诊断曾简化 HUD、声音和玩家受伤，仅用于当时同口径的局部 A/B；后期固定生命测试也不能冒充完整玩法基线。单次短测只筛选路径，不用于宣布正式收益。

## 阶段一：先确认人数增长带来的成本

**问题。** 多敌人场景掉帧，但先改 AI、动画还是渲染没有依据。

**实验。** 早期在同地图分别运行 20、80、160 敌人；再用 Insights 和破坏性关闭开关检查移动、PathFollowing、骨骼 Tick、攻击 Sweep 的成本上界。每档规模测试只有一次，关闭开关会破坏玩法，二者都不能当作最终方案的收益。

| 敌人数 | GT | CharacterMovement | Animation |
| ---: | ---: | ---: | ---: |
| 20 | 5.879 | 0.745 | 0.977 |
| 80 | 16.106 | 3.231 | 2.728 |
| 160 | 27.899 | 6.188 | 4.448 |

160 敌人时，关闭 Movement Tick 使目标 Movement 耗时减少 5.103 ms；关闭 SkeletalMesh Tick 使 Animation 减少 3.252 ms。攻击 Sweep 的自身耗时约 0.006～0.008 ms。这说明应优先减少**必须执行的移动与动画工作量**，而不是先重写刀刃查询；关闭 Tick 本身不能交付，因为移动、动画、Socket 和战斗表现会失效。

**下一步。** 保留玩法，用更新节奏、共享和渲染参与限制替代整类关闭。

## 阶段二：减少重复 AI 决策，但不把历史 Timer 结果套到当前行为树

**动机。** AI 决策函数本身不是最重的局部函数，但它会触发后续寻路与移动请求；应验证减少重复决策是否真的减少下游提交。

**实验与发现。** 历史 Timer 版本在 160 敌人下做 AllEnabled / NoAIThrottling 各三次。启用按战斗状态与距离降频后，AI Decisions 从 56.741 降至 19.521 次/帧，Decision 耗时从 0.726 降至 0.215 ms，Move Requests 从 52.897 降至 16.063 次/帧；GT 组均值为 37.081→28.179 ms。决策和请求三项同向，证明减少无效更新有意义；GT 差值包含寻路、移动和等待的连锁变化，不能全部归功于 AI 函数。

当前架构改用 C++ 任务＋可编辑行为树：一轮等待、一次目标与距离采样，再由 Selector 选择攻击、站位、包围或追击。Controller 负责 MoveTo 去重、失败退避和预算；没有另开一套 Controller 决策 Timer。**历史 Timer 消融证明策略方向，不是当前行为树版本的性能 A/B**。行为树迁移后的 BrainComponent 调度成本仍需在最终版本中复测。

**下一步。** 不凭“换成行为树”宣称加速，而对 Movement、动画、阴影和骨骼光追分别看消费者是否真的减少。

## 阶段三：80 敌人消费者消融，建立局部收益

**动机。** AI 更新变少不等于整个角色便宜；Movement、动画、投影和光追是不同消费者，需要分别验证。

**实验。** 80 敌人、固定地图和种子，默认组与单项关闭组各三次独立进程。该批使用早期简化玩法的诊断 Runner；结果仅用于同批局部成本对照。

| 单项机制 | 默认消费者/耗时 | 关闭机制后 | 已验证的局部减少 |
| --- | --- | --- | ---: |
| Movement 分级 | 65.1 Tick/帧；1.056 ms | 81.0 Tick/帧；1.245 ms | 0.189 ms |
| Animation Sharing | 约 60.6 Follower；0.818 ms | 0 Follower；1.226 ms | 0.408 ms |
| 敌人投影限制 | 5 个投影；ShadowDepths 1.052 ms | 80 个；1.773 ms | 0.721 ms |
| 敌人骨骼 RT 限制 | 12 个 RT Visible；Skinned BLAS 0.199 ms | 80 个；0.495 ms | 0.296 ms |

四项目标局部耗时均三次同向，但分属不同消费者和线程，**不能相加为整帧收益**。阴影限制另使 Shadow Draw Calls 1013.4→637.0、总 RHI Draw Calls 2111.8→1655.0。Skeletal LOD 确实改变了分配档位，却未测到稳定的 Animation 或 BLAS 正收益；动画 Tick 分级的独立净收益也未建立。Full、阴影和 RT 名额是当前配置，不是通过完整画质扫描证明的通用最优值；在该批中 RT 上限没有在 Full 候选之外再次拒绝对象。

**下一步。** 敌人自身工作已被压下，但整帧未随局部收益等量下降，于是转查 RT/RHI 的实际限制，而不是继续把所有时间记在 GT 上。

## 阶段四：为什么局部变快，整帧仍慢

**观察。** 历史规模矩阵中，优化后的 160 敌人 GT 曾记录 7.707 ms，但同次 Frame 为 30.899 ms、RT 为 30.411 ms。它与早期 27.899 ms GT 是跨版本趋势，不是单变量提速曲线。独立进程还出现过 RT/RHI 快慢两种运行状态，因此只比较平均 FPS 会误判消费者策略。

**口径修正。** 恢复正常 HUD、声音、伤害和死亡后，固定站立玩家的长采样不能成立；短采集只证明流程能启动，不是稳态基线。后期成本诊断改为高生命值、固定输入/镜头，保留其余玩法路径；这解决了长时间测量的可重复性，但仍不等于交付玩法的正常生命回归。

**0/160 对照。** 在当前默认配置的同批单次运行中：

| 指标 | 0 敌人 | 160 敌人 | 160−0 |
| --- | ---: | ---: | ---: |
| Frame | 11.868 | 14.296 | +2.428 |
| GT | 2.448 | 7.806 | +5.358 |
| RT | 11.547 | 13.061 | +1.514 |
| RHI | 9.069 | 11.359 | +2.290 |
| GPU | 10.335 | 12.733 | +2.398 |
| RHI Draw Calls | 1872.1 | 2566.0 | +693.9 |
| Shadow Draw Calls | 936.3 | 996.7 | +60.4 |

同批差值说明敌人确实有成本，也说明场景本身有明显底座。线程增量仍不能相加解释 Frame 增量；本批各只有一轮，不能替代多镜头、正常生命的最终验收。

**下一步。** 既然零敌人也有长 RT 等待，必须沿任务依赖找等待对象。

## 阶段五：把 RT 长等待追到历史遮挡查询

**初始误判。** Insights 显示 RT 停在 `WaitForGatherDynamicMeshElements`，容易把整段等待说成“敌人 Skeletal Mesh 收集太慢”。

**证据链。** 0/160 敌人 Task Trace 中，RT 等待动态网格管道完成；上游 `OcclusionCullPipe` Worker 同期在 `SyncPoint_Wait` 等待。被等待 GraphEvent 的原始 Completed 事件来自 `RHIInterruptThread`；本机 UE 5.5 源码对应 `WaitForLastOcclusionQuery → RHIGetRenderQueryResult(bWait=true) → Query SyncPoint`，GPU fence 达标后复制结果并通知 CPU。零敌人 1366 个等待 scope 中，1365 个与查询同步等待重叠；160 敌人 1199/1199 个重叠。时间加权重叠约为 95.76%/95.24%，表示这批样本中的时间重叠，**不是敌人、Occlusion Pass 或 GPU 的成本归属比例**。

**干预。** 在 0 敌人同镜头下，查询开启/关闭各两次。关闭后可见性等待代理 5.137→0.042 ms、RT 13.256→9.040 ms，但 RHI 8.189→9.542 ms、Draw Calls 1860.6→2310.4。等待消失的同时更多对象被提交，所以不采用全局关闭查询作为交付方案。

**结论。** 这类 RT 长等待的关键链包含历史硬件遮挡查询的 GPU 结果同步。已有记录未把具体 query、payload、fence 与某个 GPU Pass 逐一关联；不能说某一项 GPU Pass 自身执行了 5～8 ms，也不能据等待位置给敌人 Mesh 定责。

## 阶段六：替换查询策略是否比等待更划算

**问题。** 硬件查询导致同步，但直接关闭又增加提交量；因此比较保留剔除能力的 HZB 和查询缓冲数 2。

**路径门禁。** HZB 短测确实进入 HZB 结果映射分支：原 `WaitForGatherDynamicMeshElements` 平均由 4.304 降至 0.798 ms，然而 `STAT_MapHZBResults` 出现平均 14.092 ms 的 Inclusive scope，其中大量时间落在 RT 等 RHI 的子事件。说明等待可能换了位置，不能把原 scope 变短当成整帧提速。

**重复对照。** 后续三组各三次的旧矩阵发现一轮 Hardware 对照被外部进程污染，不能使用自动生成的 9/9 汇总。其余两个有效配对中，HZB 的 Frame 分别增加 1.000 和 6.329 ms，因此不采用 HZB；该结论只针对这些固定场景样本，不声称 HZB 在所有地图都更慢。

另做独立、平衡顺序的 Buffer2 三轮对照：Hardware Frame 14.977→Buffer2 14.677 ms，P95 16.379→16.115 ms，RT 14.711→13.430 ms，但 RHI 10.543→11.693 ms。它改善了固定镜头整帧与等待，却把部分工作/时序压力转给 RHI。当前默认已设置 Buffer2；快速转镜、遮挡变化和多视点视觉回归尚未完成。

## 阶段七：缩小 GPU 候选，不凭局部 Pass 宣布整帧成功

**探索。** 一次分辨率短测把内部 `ScreenPercentage` 显式设为 50，GPUTime 和 RT 等待下降，但两轮原始门禁均未全部通过，且 P99 几乎不变。它只提示照明、反射与 TSR 值得细分，不作为画质方案或正式收益。

**Lumen 单变量对照。** 160 敌人各三轮、平衡顺序：

| 候选 | Baseline Frame / P95 | 候选 Frame / P95 | 决定 |
| --- | --- | --- | --- |
| Reflections DownsampleFactor 1→2 | 14.934 / 16.335 | 14.480 / 15.868 | 固定镜头有收益，进入组合验证 |
| ScreenProbeGather DownsampleFactor 16→32 | 14.934 / 16.335 | 14.966 / 16.912 | 整帧无收益、P95 回退，不采用 |

这解释了“局部降低采样成本”不必然缩短玩家看到的整帧；ScreenProbe 不是继续重复调参的优先方向。

**组合验证。** Buffer2＋反射下采样 2 相对两项均为原值的三轮平衡顺序对照：

| 指标 | 原组合 | 当前组合 |
| --- | ---: | ---: |
| Frame / FPS | 14.955 ms / 66.87 | 14.279 ms / 70.03 |
| P95 / P99 | 16.415 / 17.254 | 15.696 / 16.409 |
| GT / RT / RHI / GPU | 7.593 / 14.689 / 10.597 / 13.321 | 7.610 / 13.160 / 11.333 / 12.704 |
| 可见性等待代理 | 5.778 | 4.213 |
| GPU Lumen Reflections | 1.128 | 0.649 |
| RHI Draw Calls | 2566.0 | 2581.1 |

Frame 平均节省 0.676 ms、P95 节省 0.719 ms。Draw Calls 未得到实质下降，RHI 反而上升；这次成功不是“合批省了提交”。P99 在固定成本测试中低于 16.67 ms，但余量仅约 0.26 ms，不能据此宣称完整玩法稳定 60 FPS。

## 阶段八：当前底座与内存检查

**当前默认一次性 0/160 复测。** 使用已经写入项目默认配置的 Buffer2 和反射下采样 2：0 敌人 Frame 11.868、P95 13.063、P99 13.831 ms；160 敌人 Frame 14.296、P95 15.731、P99 16.400 ms。160 敌人 GPU 较大的独立统计包括 ScreenProbeGather 1.938、TSR 1.599、LumenSceneLighting 1.377、ShadowDepths 1.149 ms；Skinned BLAS 0.219 ms。不能把这些 GPU 子项或其父项相加成总帧时。

**内存。** 独立的 160 敌人约 180 秒单轮驻留检查（使用当批原配置，不与当前默认的帧时直接相减）中，PhysicalUsedMB 首/末约 3624/3643，GPU 本地使用约 3691/3692 MB；期间峰值约 3655/3724 MB。WantedMips 约 148～153 MB，manifest 无纹理池或 VSM 队列告警。该窗口未见持续快速增长或纹理池告警；一轮固定场景不足以证明长局无泄漏，更不能把提高纹理池容量当作已验证的 FPS 优化。

## 已完成、未采用与收尾边界

| 状态 | 项目 | 判断依据 |
| --- | --- | --- |
| 局部收益已验证 | AI 历史降频、Movement 分级、Animation Sharing、敌人阴影与骨骼 RT 参与限制 | 同口径重复消融，目标消费者与局部时间同向 |
| 当前配置已有固定镜头净收益 | Buffer2＋Lumen 反射下采样 2 | 三轮平衡顺序组合对照，Frame/P95/P99 改善；RHI 增加 |
| 不采用 | 全局关闭遮挡、HZB 候选、ScreenProbe 下采样 32 | 提交/等待转移或整帧无益；保留原始失败记录 |
| 尚未验收 | 当前行为树版本性能、快速转镜及多视点画质、正常生命的完整玩法长局、跨硬件/Shipping | 现有固定成本采集不能替代这些条件 |
| 可选深挖，不是重跑旧实验 | 查询对应 GPU Pass 的精确生产链 | 现有 Trace 缺 query/payload/fence 到 Pass 的直接关联 |

**后续只做新的判定，不重做已经得到答案的测试。** 若以 60 FPS 交付为目标，先对当前配置做动态镜头和正常玩法回归，再针对零敌人也存在的场景绘制、阴影与 GPU 大项做单变量 A/B。若要回答“具体哪个 Pass 让历史查询结果晚到”，需补同一 query 的提交、GPU 队列和 fence 关联证据；不能从当前帧最大的 GPU Pass 猜测。实验通过条件始终是整帧与尾帧、消费者计数、画质/玩法和运行完整性同时成立。

## 2026-10-20：联网 160 AI 命中代理候选、线程对照与验收边界

**状态：查询响应修正后的 Simple B3 已完成 Editor／Shipping 客户端各三轮，共六个有效 case，作为本轮当前基线；Reference 命中体内部的线程 A/B 另有三轮、共六个有效 case。Simple B2 后续发现查询响应遗漏及 Sight 遮挡回归，全部降为 `PreQueryResponseFix` 修正前诊断，不作为最终验收或等价收益。修后正常本地双渲染及硬断恢复通过，但视觉质量尚未完整验收；弱网旧失败未重跑，不能宣称弱网闭环通过。** 原始报告的 `Valid=true` 表示采集协议通过，不抹去数据，也不等于候选玩法语义正确。各批次独立保留，不拼接历史单机、旧视点协议或修正前 Simple 数字。

### 实现及正确性边界

- 活体敌人改为根组件驱动的“头部球体＋身体胶囊”两个 `QueryOnly` 命中代理；不读取全身骨骼位置。头／身体射线分别归一为 `head`／`spine_03`，沿用爆头、伤害上下文及死亡冲量入口。近远距离使用一致几何规则，玩家 PvP 历史形状回溯保持原样。
- 这是**统一的近似命中模型**，不再精确匹配挥动四肢、低头等动画轮廓；“保留爆头身份”不等于与旧骨骼几何完全等价。没有用散布或距离掩盖这项玩法取舍，也没有新增敌人历史姿态回溯。
- 修正前 Simple B2 采集边界均确认服务器 160 个敌人共 **320 个启用查询的代理**，旧活体 Mesh 的物理状态／有效 BodyInstance 均为 **0**；客户端代理启用查询数为 **0**。保留完整 PhysicsAsset 资产供死亡恢复，而不是同时维护新旧两套活体形状。Reference 配置快照保留 **2880 个有效骨骼 BodyInstance（160×18）**、代理查询数为 0；此处只比较实际配置数量，不据此推算时间节省比例。减少 Body 不代表已保留原 Mesh 的其他查询响应，正是本轮后续审计发现的缺口。
- Simple 候选不再一律为了被射击而刷新完整姿态；需要的攻击、受击及 Root Motion 保护仍保留，死亡前补求值并恢复原 PhysicsAsset 布娃娃。客户端表现预算不决定服务器命中几何。修正前 Simple B2 的服务器边界快照均为 Gameplay Full 160／Reduced 0／Background 0，**不能用这批成绩证明中远档降频收益**；完整姿态保护资格是边界状态，不是全窗口实际求值次数。
- Simple B2 专用服务器显式使用 `-useperfthreads`；Reference 命中体内部线程 A/B 另比较是否添加该参数。快照记录项目／实例并行资格及 CVars，但**资格为真不等于已经证明 Worker 实际派发或关键路径收益**。相机坐标轴、FOV 系数和评分公共因子每轮复用，原评分与 Top-K 规则保持。

### 保留真实失败，修复线程安全边界

**另一个真实反例：查询几何简化不能静默删掉查询通道。** B2 中 Reference 的追击数量范围为 19–51，而修正前 Simple 为 143–152。实际 `enemy_BP.uasset`（SHA256 `DE84B167E871D24AA416AAE4FBE18E9F2125D1844949744D80F5FD260783F08D`）的 Mesh 保存的是 Custom 响应，仅把 Pawn／Vehicle 设为 Ignore，其余默认 Block，因此 Visibility／Camera 实际为 Block；根胶囊则显式忽略 Visibility／Camera。原生 Sight 使用复杂 Visibility 射线，近战遮挡也使用该通道。仅启用 WeaponTrace 的新代理意外让敌人失去原有查询遮挡，这是功能回归，不是性能收益。活动差异不能全部按这条原因定量归属，但已经足以否定“相同感知条件”的假设。

修复保持两个简化形状、不重建旧活体 Mesh 物理：在退出旧 Mesh 物理前按值复制原查询响应容器和 ObjectType，保留既有 Pawn 忽略／WeaponTrace 阻挡契约，其余通道继承作者配置。客户端代理仍 NoCollision，Overlap 事件／导航影响／模拟仍关闭。扩展既有 `SimpleHitProxyLifecycle`，用真实复杂 Visibility 射线验证另一个 AI 能遮挡、作者 Ignore 时不误遮挡，并核对 Camera Overlap 等自定义响应及实际蓝图。**恢复通道语义不意味着两种几何的轮廓或逐帧感知结果完全等价**。修后完整回归 `Automation_QueryResponse` 再次为 92 通过（79 clean＋13 warned，0 failed／notRun），不是沿用修前报告。

首次启用服务端性能线程后，正常联网试跑在玩家 FootIK 的世界查询处触发 `IsInGameThread()` 断言，证据保留于 [失败服务器日志](/E:/ueprojrct/fpstrue_safe2/Saved/Networking/PvpSmoke_20261020_101650_737c5fce/Server.log)。原因是引擎可以在 `Proxy::PreUpdate` 前读取并行资格，不能依赖上一轮贴地状态动态切换门禁。

修复为玩家 FootIK `CanRunParallelWork()` 始终返回 false，地面资格只控制是否查询、不再控制线程许可；初始化、空中、落地前及死亡阶段都保持同一同步契约。该约束会使安装后处理的玩家身体同步求值，但不修改敌人或全局并行开关。未初始化实例测试使用合法的 `USkeletalMeshComponent` Outer。失败试跑不删除，也不计为通过样本。

### 功能与联网验收

| 验收 | 实际结果 | 不能据此宣称 |
| --- | --- | --- |
| 通道修正前冻结代码自动化回归 | **92 项通过：79 clean、13 warned、0 failed、0 notRun** | 不是 92 个公网／画面场景，也不是无告警；没有覆盖后来发现的查询响应回归 |
| 通道修正后完整自动化回归 `Automation_QueryResponse` | **92 项通过：79 clean、13 warned、0 failed、0 notRun**；扩展的 `SimpleHitProxyLifecycle` 与 `SightOcclusionAndFacing` 均成功 | 覆盖真实 complex Visibility 射线与查询响应，不证明两种命中几何轮廓完全等价，也不是画面验收 |
| 先前正常本地双客户端、PvE 与硬断恢复 | 通过；Client1 34 次预测对应 34 次 Accepted，缺失终结 ACK／重复表现／意外拒绝均为 0；硬停客户端进程后重启恢复 | 此轮客户端为 NullRHI；没有验证渲染画面恢复，也不是自动断线重试验收；Accepted 不等于每发都几何命中 |
| 修后正常本地双渲染、PvE 与硬断恢复 `112023_d4535a13` | **StrictExperience 通过**；主动端为 **Client2**，34 次预测／34 次 Accepted，缺失终结 ACK／重复表现／意外拒绝均为 0；`RenderedRecoveryVerified=true` | Client1 为观察端而非第二个完成 34 发的主动端；不是双端对称操作、自动重试、公网或弱网验收；`VisualQualityReviewed=false` |
| 弱网双渲染旧样本 | **失败，修后尚未重跑**；出方向 100±20 ms、2% 丢包，首发在准入处因 `AgeMs=645.522 > AllowedAgeMs=300.000` 被拒绝，随后等待证据超时 | 后续战斗、死亡、重连及渲染恢复未被完整覆盖，不能把“拒绝有 ACK”算作弱网玩法通过，也不能用新的正常网络通过覆盖旧失败 |

修后正常双渲染使用 Development Editor 专服＋两个 D3D12 offscreen 客户端，工业玩法图，未注入延迟或丢包。硬停 Client2 后重启，同一 Pawn／PlayerState 网络身份保留；离线保留时血量 **90**，恢复时为 **80**，没有回滚离线受伤；弹药 **27**、Kills 1／Deaths 0 保留。恢复 PNG 已生成，场景、持枪视模、HUD 27／59 与敌人可见，但截图仍显示 **Preparing Shaders (9)**，敌人材质暂呈灰色。因此这是渲染恢复证据，不是完整视觉预热就绪或连续移动／射击／换弹动画质量通过。Client2 预测至终结 ACK 的同客户端单调时钟 P50／P95／Max 为 **51.807／79.383／251.618 ms**，不是网络 RTT，也不是隔离出的传输耗时。

证据：[冻结回归索引](/C:/UEBuild/FPSAcceptance_20261020/Automation_Frozen/index.json)、[修后完整回归索引](/C:/UEBuild/FPSAcceptance_20261020/Automation_QueryResponse/index.json)、[先前 NullRHI 正常联网报告](/E:/ueprojrct/fpstrue_safe2/Saved/Networking/PvpSmoke_20261020_102428_709f075a/Report.json)、[修后正常双渲染报告](/E:/ueprojrct/fpstrue_safe2/Saved/Networking/PvpSmoke_20261020_112023_d4535a13/Report.json)、[恢复画面](/E:/ueprojrct/fpstrue_safe2/Saved/Networking/PvpSmoke_20261020_112023_d4535a13/Visual/Client2Resume/restored.png)、[弱网双渲染失败报告](/E:/ueprojrct/fpstrue_safe2/Saved/Networking/PvpSmoke_20261020_103025_9bae05a8/Report.json)。延迟注入值是配置方向口径，不直接当作测得 RTT；本轮没有扩大服务器允许年龄以放过失败样本。

### 修正前 Simple B2：PreQueryResponseFix，保留数据而非最终基线

**以下两张帧时表与网络表全部属于 `PreQueryResponseFix`。** `Simple160Final_20261020_B2` 名称保留原样，但文件名中的 Final 不赋予最终验收资格。该版代理只保留 WeaponTrace 响应，遗漏原 Mesh 的其他查询响应；Sight 遮挡与 AI 活动分布发生变化，不能把它与 Reference 的差值全部算作动画／物理等价优化收益。

该批使用扁平的 `EnemyUI160_ArenaOverview55x192`，160 AI、两个连接、种子 1337、20 秒预热及至少 60 秒采集；仅 Client1 DX12 渲染，Client2 和服务器为 NullRHI。输出 1600×900，Scalability 各级 2、ScreenPercentage 87、FOV 90、VSync Off、`t.MaxFPS=0`；保留当前引擎特性默认值。本地三个进程竞争同一台机器资源，不是三台独立硬件。

地图重建后为两个客户端指定不同的作者 `PlayerStartTag`，并保留地图作者相机；B2 实际使用 **Player** 视点，而非俯视相机，六轮 Client1 位置／旋转均为 `(-350,-900,164)`／`(0,90,0)`。旧 `AnimationThreads160_AB_20261020` 第三个 case 因 `Camera changed between repeated/build cases` 作废，前两个旧 case 也不与 B2 拼接成完整对照；不从旧协议提取均值补新表。

下面的客户端数值为 `OnEndFrame` 间隔，含整条本地执行与节拍影响，**不是纯 CPU 执行时间**。服务器 World 列为该 World Tick 的墙钟范围，包含依赖与等待，不是 on-core 时间、不是完整服务器帧间隔，也不能用 `1000/World` 推导服务器 FPS／承载能力。Editor 与 Shipping 行只切换客户端构建，**服务器始终是同一个 Development Editor 专用服务器，不是 Shipping Server**。各轮 `Valid=true`，进程自然退出；本轮无性能 Trace。

#### PreQueryResponseFix：Editor 客户端，三轮独立结果

| 轮次 | 客户端平均帧间隔 / ms | 客户端 P95 / ms | 客户端 P99 / ms | 客户端 ＞16.667 ms / % | 服务器 World 均值 / ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| R1 | 18.928 | 25.775 | 29.949 | 71.962 | 11.500 |
| R2 | 20.451 | 28.249 | 31.570 | 83.526 | 11.732 |
| R3 | 18.377 | 23.462 | 27.958 | 70.894 | 11.341 |
| 三轮等权均值 | **19.252** | **25.828** | **29.826** | **75.461** | **11.524** |

#### PreQueryResponseFix：Shipping 客户端，三轮独立结果

| 轮次 | 客户端平均帧间隔 / ms | 客户端 P95 / ms | 客户端 P99 / ms | 客户端 ＞16.667 ms / % | 服务器 World 均值 / ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| R1 | 10.346 | 14.970 | 18.671 | 2.510 | 11.725 |
| R2 | 10.790 | 15.605 | 19.377 | 3.432 | 11.912 |
| R3 | 11.899 | 17.929 | 21.398 | 8.518 | 11.826 |
| 三轮等权均值 | **11.012** | **16.168** | **19.815** | **4.820** | **11.821** |

均值行的 P95／P99 是**各轮分位数的算术平均**，不是合并六轮或三轮全部帧后重新计算的分位数。Shipping R3 的 P95 已超过 16.667 ms，不能宣称锁定 60 FPS。该批画质、视点和人口门禁一致，但查询响应回归使其不能作为最终玩法验收或等价优化基线；不与 9 月单机成绩或 10 月旧协议直接相减。服务器完整 FrameEnd 的三轮均值另为 Editor 客户端组 **46.395 ms**、Shipping 客户端组 **46.419 ms**，不可用上表约 11.5～11.8 ms 的 World 范围代替。

### PreQueryResponseFix：同窗口网络计量

| 客户端构建 | 服务器总出方向 / B/s | 服务器总出包 / packet/s | Client1 入方向 / B/s |
| --- | ---: | ---: | ---: |
| Editor 三轮等权均值 | 23735.128 | 43.554 | 11867.804 |
| Shipping 三轮等权均值 | 22183.231 | 43.083 | 11094.123 |

这些是各轮独立 START／STOP 的 `UNetConnection` 累计差值速率，服务器总量包含两个连接及引擎协议／ACK／PacketOverhead 计量，不是 NIC 抓包、纯 AI Payload 或每 Actor 带宽；各端窗口独立，不能强求跨端字节完全相等。仍使用 Tiered 复制策略，并保持 160 AI 相关性，未借裁剪客户端副本数量换成绩。该版 Editor 三轮追击人口范围 **143～152**、攻击 **8～17**、移动 **11～73**；Shipping 三轮追击 **144～152**、攻击 **8～16**、移动 **16～55**，始终 160 个控制器。活动分布明显不同于下述 Reference 命中体，因此低流量也不能单独解释为网络调度收益；不是 160 个单位同时攻击的容量证明。

### Reference 命中体内部线程 A/B：B2 三轮、六个 case 门禁通过

`AnimationThreads160_AB_20261020_B2` 始终固定 `EnemyHitProxies=Reference` 与 `EnemyReplication=Tiered`，服务器和两个客户端均为 Development Editor。唯一显式线程条件是服务器是否增加 `-useperfthreads`：Reference 不加，Parallel 增加；不强制改动画 CVars、实例标志或命中几何。顺序为 R1 Reference→Parallel、R2 Parallel→Reference、R3 Reference→Parallel。沿用新版作者出生点相对相机协议、160 AI、两个玩家、种子 1337、20 秒预热及完整采集窗口，各角色自然 exit0，六个 case 均 `Valid=true`，无强杀。

| 轮次 | 服务器线程模式 | 服务器 World 均值 / ms | 服务器 FrameEnd 均值 / ms | Client1 FrameEnd 均值 / ms | 服务器总出方向 / B/s |
| --- | --- | ---: | ---: | ---: | ---: |
| R1 | Reference | 47.998 | 48.569 | 20.908 | 58261.455 |
| R1 | Parallel | 41.687 | 42.227 | 21.487 | 59428.287 |
| R2 | Parallel | 42.807 | 43.367 | 21.677 | 56072.112 |
| R2 | Reference | 44.454 | 44.990 | 21.066 | 57937.860 |
| R3 | Reference | 45.699 | 46.256 | 21.903 | 57601.146 |
| R3 | Parallel | 40.914 | 41.453 | 20.657 | 59278.955 |
| 三轮等权均值 | **Reference** | **46.050** | **46.605** | **21.292** | **57933.487** |
| 三轮等权均值 | **Parallel** | **41.803** | **42.349** | **21.274** | **58259.785** |

服务器 World 三轮范围为 Reference **44.454～47.998 ms**、Parallel **40.914～42.807 ms**；完整 FrameEnd 范围分别为 **44.990～48.569 ms**、**41.453～43.367 ms**。World 观察均值差 **4.247 ms**，各轮方向相同；客户端 FrameEnd 均值仅差约 **0.019 ms**，不能把服务端改进包装成客户端帧率收益。上述 World 仍是墙钟范围而非 on-core 时间，FrameEnd 包含等待与节拍，两者不互相替代。

| 活动与配置 | Reference 线程组 | Parallel 线程组 |
| --- | --- | --- |
| 控制器与存活敌人数 | 全窗口均为 160 | 全窗口均为 160 |
| 追击人口范围 | 19～51 | 20～46 |
| 攻击人口范围 | 4～17 | 5～16 |
| 移动人口范围（速度＞5 cm/s） | 0～36 | 0～22 |
| 各边界有效骨骼 BodyInstance | 2880 | 2880 |
| 各边界启用的简化命中代理 | 0 | 0 |
| 各边界完整权威姿态资格 | 160 | 160 |
| `FApp::ShouldUseThreadingForPerformance()` | false | true |

六轮 Client1 视点签名相同：位置 `(-350,-900,164)`、旋转 `(0,90,0)`、FOV 90；画质签名唯一，输出 1600×900、High 2、ScreenPercentage 87。各轮输入指纹门禁通过，但真实追击／攻击／移动人口不是逐帧完全一致。**这是同 Reference 命中方案中的线程门禁对照，不把全部差值归因于动画 Worker 执行，更不与有查询响应回归的 Simple B2 相减。** `-useperfthreads` 影响性能线程门禁，不只对应一段动画函数；实例资格和 FApp 读回不等于实际 Worker 派发证明。

网络均值分别为 Reference **83.918 packet/s**、Client1 入方向 **28967.896 B/s**，Parallel **86.119 packet/s**、Client1 入方向 **29133.994 B/s**。均属引擎连接计量，不作为独立网络优化结论。

### 查询响应修正后 Simple B3：当前联网 160 AI 基线

`Simple160Final_20261020_B3` 使用修正后构建及独立新 Stage，未复用 B2 的结果。六个 case 均 `Valid=true`，18 个角色进程全部自然 exit0、无强制终止。顺序为 R1 Editor→Shipping、R2 Shipping→Editor、R3 Editor→Shipping；固定 `EnemyHitProxies=Simple`、`EnemyReplication=Tiered`，仅服务器增加 `-useperfthreads`。**Editor／Shipping 标签只区分客户端，服务器始终是同一 Development Editor 专用服务器，不是 Shipping Server 容量测试。**

人口／地图／视点协议与线程 A/B B2 相同：扁平 `EnemyUI160_ArenaOverview55x192`、160 AI、2 个玩家、种子 1337、20 秒预热和至少 60 秒采集。Client1 为唯一 DX12 渲染端，Client2 与服务器 NullRHI；本机三个进程共享硬件。六轮视点签名唯一：位置 `(-350,-900,164)`、旋转 `(0,90,0)`、FOV 90；输出 1600×900、各 Scalability 2、ScreenPercentage 87、VSync Off、`t.MaxFPS=0`，无动态分辨率。不把正常双渲染 smoke 的截图或开销混入这组性能数据。

| 客户端／轮次 | 客户端 FrameEnd 均值 / ms | 客户端 P95 / ms | 客户端 P99 / ms | 客户端 ＞16.667 ms / % | 服务器 World 均值 / ms | 服务器 FrameEnd 均值 / ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Editor R1 | 15.769 | 26.118 | 30.511 | 45.489 | 10.924 | 41.296 |
| Editor R2 | 21.774 | 29.174 | 33.403 | 91.597 | 11.863 | 46.578 |
| Editor R3 | 21.439 | 28.609 | 33.113 | 91.962 | 12.813 | 46.448 |
| **Editor 三轮等权均值** | **19.660** | **27.967** | **32.342** | **76.350** | **11.867** | **44.774** |
| Shipping R1 | 11.240 | 17.302 | 20.838 | 6.796 | 12.116 | 44.631 |
| Shipping R2 | 11.481 | 16.678 | 19.331 | 5.032 | 13.132 | 46.436 |
| Shipping R3 | 8.780 | 13.641 | 17.165 | 1.294 | 10.458 | 40.098 |
| **Shipping 三轮等权均值** | **10.500** | **15.874** | **19.111** | **4.374** | **11.902** | **43.722** |

均值行 P95／P99 仍为各轮分位数的算术平均，不是合并帧后的分位数。Editor 客户端三轮均值范围 **15.769～21.774 ms**，Shipping **8.780～11.481 ms**，存在明显轮间波动，不能只取最好一轮或宣称稳定 60 FPS；Shipping R1／R2 的 P95 仍超过 16.667 ms，三轮 P99 全部超过。服务器 World 范围分别为 Editor 组 **10.924～12.813 ms**、Shipping 组 **10.458～13.132 ms**；完整 FrameEnd 范围分别为 **41.296～46.578 ms**、**40.098～46.436 ms**。World 是含依赖等待的选定 World Tick 墙钟范围，FrameEnd 是完整端帧间隔；**不能用约 11.9 ms 的 World 换算服务器 FPS，或把两列之差直接判为睡眠／调度等待。** 独立 Trace 才能进一步分解。

| 活动与边界配置 | Editor 客户端组三轮 | Shipping 客户端组三轮 |
| --- | --- | --- |
| 全窗口敌人／控制器／玩家 | 160／160／2 | 160／160／2 |
| 追击人口范围 | 54～92 | 54～96 |
| 攻击人口范围 | 7～17 | 9～17 |
| 移动人口范围（速度＞5 cm/s） | 3～41 | 3～42 |
| 每端各 START／STOP 的 Simple 敌人数 | 160 | 160 |
| 服务器旧 Mesh：有效 BodyInstance／物理状态／碰撞启用数 | 0／0／0 | 0／0／0 |
| 服务器代理：组件／查询启用／物理状态数 | 320／320／320 | 320／320／320 |
| 两客户端各自代理：组件／查询启用／物理状态数 | 320／0／0 | 320／0／0 |

36 个角色边界快照一致符合代理／人口门禁；原 PhysicsAsset 仍配置 18 bodies／18 shapes／17 constraints、引用数 160，这些是资产配置，**不是活跃 Chaos Shape 数**。服务器完整权威姿态保护资格在六轮边界为 **8～15**，Gameplay 分布出现 Full 157～160、Reduced 0～3、Background 0，不能据此证明大规模中远档或 160 单位同时攻击的性能。两个客户端仅负责本地表现：Client1 的共享姿态 follower 边界为 136～143，Client2 为 135～141，不承担权威查询。所有端敌人主动画并行资格 160、后处理实例数 0；服务器 FApp 性能线程门禁为 true、强制线程标志为 true，客户端强制标志为 false。**这些读回仅证明配置与资格，不替代实际 Worker 事件证据。**

| 客户端构建 | 服务器总出方向 / B/s | 服务器总出包 / packet/s | Client1 入方向 / B/s |
| --- | ---: | ---: | ---: |
| Editor 三轮等权均值 | 44946.659 | 59.593 | 22467.889 |
| Shipping 三轮等权均值 | 46640.805 | 62.888 | 23323.816 |

网络仍为各角色独立 START／STOP 的连接累计差值速率，包括两个服务器连接的协议／ACK／PacketOverhead，不是纯 AI Payload／NIC／每 Actor 流量。修后 Simple 的追击与攻击人口不与 Reference 或修正前 Simple 逐帧相同，两个简化形状也不等价于原全骨骼轮廓，因此**本节给出当前构建基线，不把跨批差值全部归因动画多线程、物理维护或复制调度**，不据流量下降单独声称网络优化完成。

证据：[修后 Simple B3 六轮结果](/C:/UEBuild/FPSBaselines/Simple160Final_20261020_B3/results.json)、[修后 Simple B3 汇总](/C:/UEBuild/FPSBaselines/Simple160Final_20261020_B3/summary.json)、[修后 Simple B3 输入协议与指纹](/C:/UEBuild/FPSBaselines/Simple160Final_20261020_B3/experiment.json)。B2 修正前数据继续保留在上文，不被 B3 覆盖，也不参与本节均值。

### 后续证据门禁与原始来源

查询响应修正后的 Simple 已重新构建，修后 92 项回归与 B3 六个性能 case 均通过各自门禁；修正前 B2 只作发现回归的诊断记录。独立 Trace 的实际 Worker 与执行／等待证据见下节，不拿资格开关或 Body 数量代替计时。当前仍未完成修后弱网重跑、完整视觉预热及连续操作画面验收，不能把本轮固定视点基线视为所有玩法和网络条件的最终通过。

修正前诊断：[Simple B2 六轮结果](/C:/UEBuild/FPSBaselines/Simple160Final_20261020_B2/results.json)、[Simple B2 汇总](/C:/UEBuild/FPSBaselines/Simple160Final_20261020_B2/summary.json)、[Simple B2 输入协议与指纹](/C:/UEBuild/FPSBaselines/Simple160Final_20261020_B2/experiment.json)。同 Reference 命中体内部线程对照：[线程 A/B 六轮结果](/C:/UEBuild/FPSBaselines/AnimationThreads160_AB_20261020_B2/results.json)、[线程 A/B 汇总](/C:/UEBuild/FPSBaselines/AnimationThreads160_AB_20261020_B2/summary.json)、[线程 A/B 输入协议与指纹](/C:/UEBuild/FPSBaselines/AnimationThreads160_AB_20261020_B2/experiment.json)。巨大的原始日志、CSV 和 Trace 保留本机证据目录，不随本节提交。

### 修后独立 Trace：实际 Worker 与执行／等待分离

`Simple160Trace_20261020_B3` 使用相同 Simple＋Parallel 场地、160 AI 和两个玩家，单独增加 CPU／Task／Frame 命名 Trace 及 Windows `GetThreadTimes` 统计，不与无 Trace 的 B3 均值混合。完整原始帧序列与 `FEngineLoop::Tick` 事件逐一匹配，唯一有效完整窗口为 **52.9459111–114.980772 秒，共 1339 帧**；不沿用历史 1555 帧分母。原生调用树 `Count=1340`、`NumFrames=1339`，保留该计数差异而不冒充帧数；另核对 1339 个完整 Tick 事件的累计时长与调用树根相差约 0.7 微秒，分母始终使用经过验证的 1339 帧。

- 四条实际线程 `Foreground Worker #0/#1`、`Background Worker #0/#1` 均出现原生带 `_WorkerThread` 后缀的动画事件，其中 `SkeletalComponentAnimEvaluate_WorkerThread` 为 **17,914 次**，`ProxyUpdateAnimation_WorkerThread` 同为 17,914 次。原生后缀根据 `IsInGameThread()` 选择，事件同时带实际线程名，因此不是仅凭配置判断。合计 109,669 个动画子事件含多层嵌套，**不当作独立动画次数**，也不宣称 160 个 AI 每帧都完整求值或已把 Event Graph 全搬到 Worker；没有逐对象归属所有事件。
- 完整引擎 Tick scope 均值 **46.328 ms**；同窗口帧末配对墙钟 **46.329 ms**、GameThread 累计 CPU **14.073 ms**。World 配对墙钟 **13.969 ms**、累计 CPU **12.836 ms**。线程被调度的 CPU 比例约为整帧 30.38%、World 91.89%，不能把 World 内主要执行成本和整帧节拍等待混为一谈。
- `Game thread idle time` 在 `Game thread tick wait time` 下，均值 **31.737 ms**；本机引擎 `UEngine::UpdateTimeAndHandleMaxTickRate` 的同名统计包围节拍等待。它解释了这次整帧中明确的大块空闲，但本轮未读回实际动态最大 TickRate，不能只凭 BaseEngine 的默认 30 就断言全部节拍来源或 Windows 超睡原因。
- 所有 `WaitUntilTasksComplete` 合计 Inclusive 约 **8.64 ms/帧**，Exclusive 约 **0.71 ms/帧**；大部分包含嵌套任务工作，不能称 8.64 ms 纯阻塞。没有 OS 调度 ETL，仍不能把“墙钟减线程 CPU”完全拆成阻塞与 Ready／抢不到 CPU。

| 当前 GameThread 埋点范围 | 均值 / ms 每完整帧 | 解释 |
| --- | ---: | --- |
| `Char Tick` | 4.369 | 移动调用链的 Inclusive，含子工作 |
| `NetBroadcastTickTime` | 3.889 | 复制调用链的 Inclusive，不是纯包大小开销 |
| `SkinnedMeshComp Tick` | 1.801 | 所有对应 GT 上下文合计，包含玩家 IK 等子项 |
| 玩家 `FpstrueFootIK_Solve` | 0.506 | 2678 次，即每帧合计两次；嵌在动画中，不再加到上一行 |
| GT Physics Start＋End 两段 | 0.413 | 不包含全部 Worker 物理成本，不能当整机物理耗时 |
| `UpdateKinematicBonesToAnim` | 0.070 | 2678 次；不是恢复 160 个敌人的旧活体骨骼物理 |
| `EnemySignificanceCoordinator` | 0.050 | 集中调度不是当前主要执行热点 |

这些行并非互斥分区，**不能相加**；GT 的各 Exclusive 节点则已检查与根 Inclusive 闭合。当前应继续盯移动、复制及客户端实际绘制依赖，而不是凭历史 14.41 ms 再宣布当前 Chaos 或玩家 IK 为主瓶颈。该次诊断存在命名事件、线程 CPU 查询和 Trace 成本；不据它与旧版本、不同活动分布的诊断相减来计算纯收益。这里是引擎埋点调用树，不是 Windows 采样调用栈，也未完成当前客户端 GPU／RHI 的新归因。

证据：[Trace 窗口校准](/C:/UEBuild/FPSDiagnostics/Simple160Trace_20261020_B3_Analysis/CaptureWindow.json)、[当前分析](/C:/UEBuild/FPSDiagnostics/Simple160Trace_20261020_B3_Analysis/CurrentAnalysis.json)、[GT 调用树](/C:/UEBuild/FPSDiagnostics/Simple160Trace_20261020_B3_Analysis/GTCalltree.csv)、[实际 Worker 事件](/C:/UEBuild/FPSDiagnostics/Simple160Trace_20261020_B3_Analysis/WorkerAnimationEvents.csv)、[诊断服务器摘要](/C:/UEBuild/FPSBaselines/Simple160Trace_20261020_B3/Editor_Parallel_R1/Server.summary.json)。源 Trace 位于 `C:/UEBuild/FPSDiagnostics/Simple160Trace_20261020_B3/Editor_Parallel_R1/`，采集及离线 Insights 均正常退出，独占窗口已释放给 Co-op。

### 最终构建身份与提交范围

修后 Editor DLL SHA256 为 `9A3A4A260ADC35D840F06209597960DA02C244FF20BF227488ECDA0216BAA0E6`，实际 `Stage_QueryResponse/Windows/fpstrue/Binaries/Win64/fpstrue-Win64-Shipping.exe` 为 `6EC48133960A46ED4321F84EA86D69C7A78FA0F817FB67D1C0BBF22456B86D2B`。两目标编译通过；仅原生逻辑改变、UHT 无生成变更，重新 Stage 使用同一既有 Cook 数据，旧包与旧日志均保留。不是新 Linux Server／公网配对验收。

最终离线工具复验为交付 62 项、网络解析 65 项、逐发计时解析 22 项及 Render-wait 层级／边界自测通过。功能冗余检查保留生命周期、重入、射击准入与执行期二次校验，不为减少行数删除必要保护。提交包含已授权的敌人 IK／复杂地形专项撤除及对应平地场地、实现、测试、工具和文档；用户原有 `Content/StarterContent/Architecture/Floor_400x400.uasset` 改动明确排除并保留在工作树。
