# UE5 多人 FPS · 战斗同步与群体 AI 优化

个人项目，基于 UE5.5、C++ 和蓝图实现多人 PvPvE 射击：角色控制、拾取装备、射击换弹、伤害死亡、计分重生、敌人战斗与 HUD。重点是让联机战斗、群体战术和性能分级各有明确的状态归属。

> `main` 是源码展示快照，不是开箱即玩的完整工程：不分发地图、蓝图、模型、动画等 Content 资产，也不上传原始日志、凭据或缓存。下述实测来自具备授权资源的开发环境。源码取自 `fb1b7a3`，没有另写一套展示玩法；[公开范围](Docs/SHOWCASE_SCOPE.md)与[资源依赖](Docs/ASSET_REQUIREMENTS.md)说明可审阅和可运行的边界。

## 三条实现主线

1. **多人战斗**：GAS 管开火、换弹、瞄准和冲刺的动作互斥；武器组件唯一维护弹药和射击/换弹事务。客户端预测自身表现，逐发输入带装备代际、会话和 ShotId，服务器校验、结算并确认；公共装备/动作与仅拥有者弹药分开复制。
2. **群体 AI**：感知进入行为树，向目标玩家的围攻管理器申请资格；固定内外圈站位、FIFO 晋升与单次攻击许可分开。共享 EQS 选点，Controller 消费目标；通过路径复用、失败退避和 World 级预算限制重复 MoveTo。
3. **性能定位与分级**：服务器按玩家距离和战斗状态应用玩法档位；客户端独立分配模型、阴影和光追预算，接入动画降频及 Animation Sharing。使用完整帧对齐、线程 CPU、嵌套任务和渲染等待链区分执行工作与等待结果。

## 架构与配置

- C++ 维护规则、唯一状态及生命周期；蓝图选择动画、音效、特效、UMG 版式和可编辑行为树，不再保留另一套伤害或换弹结算。
- 武器与近战 Data Asset 保存共享配置；弹药、生命、槽位等运行状态留在各自所有者中，不写回配置。
- 只有 `AfpstruePvpGameMode` 一套玩法。普通预览、Listen 与 Dedicated Server 不再分别维护单机波次规则。
- ASC 复用生命组件；生命数值归 AttributeSet，弹药归武器组件。没有为了使用 GAS 把所有计数器搬进属性集。

## 当前验证（2026-10-24）

| 范围 | 实际结果 |
| --- | --- |
| Editor / Shipping | 原生编译通过；不等于新发布包运行通过 |
| 完整 Automation | 99/99，86 clean、13 warned，0 failed/notRun |
| 槽位最终专项 | 5/5，4 clean、1 warned；夹具骨骼警告保留 |
| 正常双客户端 | 射击确认与硬断恢复通过 |
| 严格弱网 | 未通过零拒绝门禁；迟到请求均有最终确认，不宣传弱网问题全部解决 |
| 最新 160 AI Trace | Editor 专服 + 双客户端；渲染端 1600×900，平均帧间隔 11.07 ms；专服 GT 实际 CPU 12.16 ms/帧，30 Hz 节拍下帧间隔 33.16 ms |

最新性能是一次约 62 秒的带 Trace 诊断，含采集开销，不是重复正式基线或 Shipping 容量证明。当前仍有外圈站立检查、失败槽位重复分配和严格弱网归因待完善。历史 13.28 ms、70.4% 等数据仅属于其原实验，不与本轮混用。

直接核对：[逐项自动化摘要](Docs/Testing/automation-20261024.json)、[正常/弱网门禁](Docs/Testing/network-20261024.json)、[160 AI 线程与模块摘录](Docs/Testing/160ai-20261024.json)。均保留原报告 SHA-256，不以登记数量代替实际通过结果。

## 阅读与运行

- [面试讲解：背景 → 架构 → 三条主链 → 取舍与证据](Docs/INTERVIEW_GUIDE.md)
- [当前验收、失败记录与后续清单](Docs/REFACTOR_ACCEPTANCE.md)
- [唯一多人模式与资产迁移](Docs/MULTIPLAYER_MODE.md)
- [资源依赖与蓝图契约](Docs/ASSET_REQUIREMENTS.md) / [配置入口](Docs/CONFIGURATION.md)
- [开发、联机与性能工具](Tools/README.md)
- [历史实验报告](Docs/Performance/EXPERIMENT_LOG.md) / [历史实验数据](PERFORMANCE_EVIDENCE.md)

源码入口是 `fpstrue.uproject`。恢复授权资产和蓝图接线后，联网构建/烟测使用 `Tools/Networking/`，固定群体负载使用 `Tools/Performance/RunGroupAIBaseline.ps1`。缺少 Content 时默认地图和资产集成测试无法运行；工具 DryRun 也不构成实测。切换协议版本后需两端一起重建，旧 Cook/Stage 不能代表当前源码。

当前展示快照不含 Content；第三方素材须独立获得授权，不因能看到源码就获得素材使用或再分发权。未进行历史重写，其他分支或旧提交的资源不属于本快照的分发承诺。
