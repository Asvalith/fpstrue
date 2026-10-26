# UE5 多人 FPS · 战斗同步与群体 AI

基于 UE5.5、C++ 与蓝图独立实现多人 PvPvE 射击，完成装备、射击换弹、伤害死亡、计分重生、敌人战斗与 HUD。主要实践逐发同步与预测校正、群体调度和线程性能分析。

> `main` 是源码展示快照，需要有授权的 Content 资源才能运行；不分发素材、原始日志、凭据或缓存。源码来自开发提交 `c9fa4e5`，Source树一致，没有另写展示玩法。[公开范围](Docs/SHOWCASE_SCOPE.md)与[资源依赖](Docs/ASSET_REQUIREMENTS.md)说明边界。

## 当前入口

| 工作 | 唯一入口 |
| --- | --- |
| 对局规则 | AfpstruePvpGameMode |
| 连接、取消与重连 | UfpstrueMultiplayerGameInstance；离场统一LeavePvp |
| 开火与换弹输入 | Character → ASC/GAS → WeaponComponent唯一执行器 |
| 敌人行为 | 正式BT_FPEnemy/BB_FPEnemy；Controller只执行已配置树 |
| 群体资格与攻击许可 | 每目标SurroundManager |
| 网络开发场景 | GameState → StartPvpNetworkDiagnostics，按参数启动所需场景 |
| 网络验证与交付 | RunFpsNetworkMatrix / BuildPvpDelivery |
| 性能采集与导出 | RunGroupAIBaseline / ExportPerformanceTrace |
| 蓝图核对 | AuditProjectContracts调用同一只读原生合同审计 |

配置见[CONFIGURATION](Docs/CONFIGURATION.md)，具体参数见[工具入口](Tools/README.md)。旧建树、迁移/修复命令、历史单机脚本及无消费者的兼容API已删除，不保留另一套启动链。

## 实现链路

1. **战斗同步**：GAS 处理激活、互斥与取消；武器组件唯一管理弹药与执行队列。本地预测弹药和换弹表现，服务器用完整射击批次进入原生 GAS 条件校验与 Commit，再按真实武器、装备代际、轮次和 ShotId 裁决。输入使用有界不可靠批次，快速裁决窗口配合可靠终结兜底。
2. **网络时间与恢复**：同一探测分别估计动态 RTT/波动与服务器时钟偏移；RTT 只调整有限补发的节拍，服务端独立裁决时效，单个 Timer 调度发送与过期。CMC 负责移动预测/重放，玩家历史形状用于命中查询。断线时保留服务器真实对象，轮换票据后重新占有，离线伤害和死亡继续生效。
3. **群体 AI**：感知进入行为树；每目标管理内外圈资格、FIFO 晋升与固定候补，单次攻击许可独立。共享 EQS 缓存，复用路径、失败退避与 World 帧预算限制重复 MoveTo。
4. **性能分级**：服务器计算玩法重要性并发放逻辑档位；客户端按自己视图决定动画、阴影和光追预算，使用降频、错峰与 Animation Sharing。结合线程 CPU、嵌套任务和渲染等待链定位工作与等待。

## 架构边界

- 只有一个多人 GameMode。GameInstance 管本机连接，GameMode 管权威规则，GameState/PlayerState 发布公共事实。
- C++ 维护规则、状态和生命周期；蓝图负责角色/武器美术装配、动画图、声画资源和 HUD 布局。Data Asset 保存共享配置，运行态留在各自所有者中。
- 开发场景位于 `Testing/`。已删除 Foot IK、AI 世界血条、旧 Profile 采集器及无人消费的公共射击字段；保留唯一 Baseline 入口，没有空兼容接口。
- 网络协议为7，两端需一起重建。Fire 由完整批次触发原生授权，不等待另一条可靠激活；结束与终结兜底仍依赖 UE 可靠传输。没有自研 UDP/RTO，也不保证任意弱网零丢发。

## 已有网络实测（2026-10-26，提交3c59c4f）

| 范围 | 结果 |
| --- | --- |
| Editor / Shipping | 编译退出码均为0 |
| 完整Automation | 98/98，78 clean、20 warned，0 failed/notRun |
| 普通横移 | 34/34接纳，确认P95 41.49ms |
| 弱网横移 | 每端出站100±20ms、5%丢包；66/66接纳，P95 333.09ms |
| 弱网真实断包恢复 | NullRHI，同一客户端进程自动重连；36/36接纳，状态恢复通过 |
| 首激活丢包专项 | 首流19/19接纳，授权等待和漏发均为0；执行早于可靠激活重发到达约198.55ms |
| 过期退款专项 | 单发拒绝与退款、保留后续预测通过 |

| 3轮，每轮160 AI / 双客户端 / 45s | 固定40ms对照 | RTT自适应 |
| --- | --- | --- |
| 预测 / 接纳 / 拒绝 | 1015 / 1013 / 2 | 1020 / 1017 / 3 |
| 每发应用层提交 | 5.617 | 4.955 |
| 客户端出站，时间加权KiB/s | 7.167 | 6.910 |
| 汇总逐发确认P95 / P99，ms | 331.79 / 397.87 | 348.34 / 398.18 |
| 单轮预算通过 | 2 / 3 | 1 / 3 |

本轮自适应每发提交减少约11.8%，客户端平均出站速率降低约3.6%；汇总确认P95增加约16.55ms，P99接近。这是应用层冗余与整条连接的观测，不能等同为UDP重传率或全部网络开销的独占贡献。固定对照仅固定同一发送算法的基础间隔，不是完整旧版算法。 严格零拒绝预算仍未全部通过，不宣称正式容量；每轮成功、失败与原阈值分别保留。

**未通过范围：**弱网＋真实渲染＋横移组合仍失败。首发与下一次补发之间跨两帧约275ms，首发准入年龄301.383ms，超过300ms上限；服务器拒绝且没有错误扣弹。该功能运行没有Trace，未确认帧停顿的具体来源。首激活Trace整轮另有一发过期拒绝，也不算整轮零拒绝。不能把NullRHI通过当作渲染、公网、8人或发布包通过。

[本轮白名单证据](Docs/Testing/fps-adaptive-fire-20261026.json)保留完整逐轮指标、原预算及失败；[历史协议6证据](Docs/Testing/fps-network-20261026.json)保持原样。历史13.28ms与70.4%来自旧场景，未改写成本轮结果。

本轮入口清理通过Editor/Shipping编译及35项受影响回归（29 clean、6 warned）；没有重跑性能或弱网矩阵，以上网络实测保留原提交口径。

## 阅读与运行

- [状态归属与源码阅读顺序](Docs/CODE_OWNERSHIP.md)
- [实现链路与面试讲解](Docs/INTERVIEW_GUIDE.md)
- [唯一多人模式](Docs/MULTIPLAYER_MODE.md) / [配置入口](Docs/CONFIGURATION.md)
- [工具说明](Tools/README.md) / [历史性能证据](PERFORMANCE_EVIDENCE.md)

使用 UE5.5 打开 `fpstrue.uproject`。联机工具位于 `Tools/Networking/`；固定群体基线使用 `Tools/Performance/RunGroupAIBaseline.ps1`。先检查参数与 DryRun，自动测试入口与普通游玩入口分别使用。

资源通过 Git LFS 管理；第三方素材需要对应授权，不授予素材再分发权。
