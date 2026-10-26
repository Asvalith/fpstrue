# FPS 状态归属与源码阅读顺序

只维护一套多人 PvPvE 规则。普通预览、监听主机、专服共用玩法；差异在于本端是否有权写状态、是否需要绘制。

## 谁负责什么

| 所有者 | 唯一状态与入口 | 消费者 |
| --- | --- | --- |
| GameInstance | 本机连接、旅行、重试、恢复票据 | Controller 握手，界面显示连接结果 |
| GameMode | 接纳、出生/重生、计分、人口、离线对象保留 | GameState/PlayerState 发布结果 |
| Controller / Character | 输入、占有、组件协调；CMC 处理移动 | GAS、武器、动画和 HUD |
| ASC / Ability | 激活、动作互斥、预测键、取消与任务寿命 | Weapon 执行一次实际业务 |
| WeaponComponent | 装备代际、权威弹药、射击队列、换弹时钟 | 私有弹药快照、公共动作、一次性表现 |
| Health / AttributeSet | 生命数值、伤害事务与死亡通知 | 角色死亡、计分、HUD；没有 AI 血条 |
| EnemyAIController / Blackboard | 目标记忆、移动请求和本轮决策镜像 | 行为树只选择并提交行动 |
| SurroundManager | 每目标站位、晋升顺序、攻击容量 | Controller 消费位置；Combat 取还攻击许可 |
| EnemyCombat | 攻击事务、命中窗口和同次伤害去重 | 动画表现，窗口结束后继续收招 |
| EnemyPerformance | 权威玩法预算与本地渲染预算分别计算 | 决策、移动、动画、阴影及光追消费者 |

Data Asset 只存共享配置。生命、弹药、队列、播放实例等运行状态不能写回配置。黑板是决策镜像，不能反向成为槽位或攻击许可的所有者。

## C++ 与蓝图边界

- C++：规则、状态写入、复制、任务寿命、事件订阅与解绑。
- 蓝图：角色/武器的美术装配，动画图与播放资源，音效、粒子、HUD 布局、样式和实际刷新图。
- 正式资源：`BP_TemplateCharacter`、`BP_RARCharacter`、对应武器蓝图、`ingame_network` 与敌人 BT/Blackboard。旧单机角色/武器、Foot IK、AI 血条及空兼容接口已退役。
- HUD 只消费本地快照。没有逐帧查找世界，也没有额外 Canvas 布局兜底。
- `Testing/` 保存测试、场景驱动和采集；正式玩法不通过测试开关取得伤害授权。GameMode 的开发场景成员实现在 `Testing/Networking/fpstrueGameModeSmoke.cpp`，仍调用原有规则。

## 战斗链：输入、执行、确认分开

```text
输入 → 本地 GAS 预测激活/互斥 → 预测表现和弹药
     → 有界不可靠批次 → 服务器校验完整身份 → 原生 GAS 授权/Commit
     → 武器重排/准入 → 按射速执行
     → 唯一扣弹/命中/伤害 → 最近裁决快速窗口 + 可靠终结兜底
     → 拥有者按权威快照水位消账；同一发只终结一次
```

一次射击由真实武器、装备代际、开火轮次和 ShotId 标识；服务器还检查预测键的当前连接归属。Fire 只由完整批次进入原生授权，晚到的原生 key-only 控制不再激活或结束它；其他能力不改。快照和回执各自控制版本，不能重复扣弹或退款。

普通松开保留已接纳尾发；死亡、换枪、过期分别收尾。可靠 Finish 携带武器、装备代际、轮次和首发，即使尾部为空也可关闭自己的轮次。当前装备/连接保留一个新鲜度记录，防止旧轮次复活；最多8项短期关闭记录只处理已授权尾发，两者不另存伤害结果。正常批次和 Finish 最终共用武器执行链。

换弹以服务器时钟唯一提交弹药。客户端先播放，服务器确认接管原播放实例；动画通知不能装弹。结束旧动作时先释放旧身份再通知，监听者可以同步开始新动作，旧调用栈不能清掉新标记。

## 网络时间与命中边界

Controller 复用同一探测计算动态 SRTT 与绝对残差平滑 RTTVAR；时钟偏移只采纳质量合格的样本。乱序、重复、未知与过期样本不能刷新估计。武器用 `clamp(SRTT + 2×RTTVAR, 20, 60)ms` 安排有界补发，无新鲜样本回退40ms；新输入可按20ms最小间隔顺带旧输入，单个 Timer 同时管理发送与400ms过期。RTT 只调整发送节拍，客户端不猜测服务器剩余时效并提前停发；服务端独立校验输入时间，客户端上报不构成信任依据。

CMC 继续负责拥有者移动预测/重放。玩家历史命中只查询历史形状，不搬动真实物理世界；模拟玩家副本使用时间戳对应的快照插值。Root Motion、动态基座和动态遮挡的限制见 [网络合同](FPS_NETWORK_ACCEPTANCE.md)。没有自研 UDP、可靠重传或完整世界回滚。

## 群体链：资格、目标点和出手分开

每个目标玩家维护 8 内圈、12 外圈、FIFO 空间晋升；单次攻击另有容量和固定 8 名候补。槽位数组保留稳定身份，释放时清占用，不能删除数组元素让所有敌人换位。拿到资格不代表有可达目标点，更不代表获准攻击。

位置失败或接近超时归还资源、退避后排队。服务器在 Game Thread 顺序分配，无额外 Server RPC 或锁。黑板缓存 KeyID，仍使用 UE 原生存储。服务器计算玩法重要性；每个客户端按自己视图计算渲染预算，两者不混用。

## 建议阅读

1. `Game/fpstrueMultiplayerGameInstance.*` 与 `fpstruePvpGameMode.*`：连接与规则所有权。
2. `Weapons/fpstrueCombatAbilities.*`、`fpstrueWeaponComponent.*`：动作生命周期与唯一执行。
3. `Characters/Player/`：CMC 扩展、历史命中与动画输入。
4. `Characters/Enemies/AI/`、Combat、Performance：个体、群体和预算。
5. `Testing/Automation/` 与 [验收记录](REFACTOR_ACCEPTANCE.md)：成功、失败和未覆盖边界分别看。

目录均相对 `Source/fpstrue/`。历史成绩不能直接视为当前构建通过；具体测量条件见 [FPS_NETWORK_ACCEPTANCE](FPS_NETWORK_ACCEPTANCE.md) 与 [PERFORMANCE_EVIDENCE](../PERFORMANCE_EVIDENCE.md)。

## 入口清理（2026-10-26）

运行时只使用正式行为树资产和完整黑板；删除动态建树/旧schema/升级入口。连接取消统一LeavePvp；测试输入改在Testing中经过真实ASC，武器不暴露测试专用StartFire/StopFire。普通双端驱动迁入现有Networking文件，由GameState的诊断入口启动；角色不再通过console启动它。已完成资产迁移器及旧单机工具删除；当前工具入口统一见[Tools/README](../Tools/README.md)。
