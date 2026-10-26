# 唯一多人玩法入口与迁移边界

项目收敛为多人 PvPvE：玩家可互伤，与 AI 同场交战；不再维护原单机的波次、90 秒生存胜负和死亡即全局失败流程。此次是减少玩法分支，不新增通用框架。

## 当前职责

- `AfpstruePvpGameMode`：唯一服务器玩法入口，管理玩家接纳、出生/重生、计分、敌人人口、每个目标的围攻管理器与离线保留。
- `AfpstruePvpGameState`：复制对局公共事实，并在每端装配 Significance/AnimationSharing 协调器。服务器只管权威策略，客户端只管本端表现。
- `AfpstruePvpPlayerController`：所属端请求、握手和时钟采样；引擎 Playing 状态不等于项目复制就绪。
- `UfpstrueMultiplayerGameInstance`：跨世界连接、取消、有限重试及恢复凭据，不保存或裁决生命、弹药、伤害。
- HUD：读取当前 Pawn/装备/PlayerState，换对象时解绑旧订阅并补读当前快照；不再订阅单机波次与胜负事件。

`RestartPlayer` 是单个角色出生/重生，不表示全局重新开局。地址直连、监听主机与专服入口也不等于 Steam/EOS 匹配服务。

## 断线恢复与预测的边界

断线重连已经由现有连接链处理，不另建一套恢复管理器：GameInstance 发起旅行并有限重试，所属 Controller 提交票据，GameMode 验证并轮换票据，再重新接管服务器保留的 Pawn、装备和 PlayerState。离线角色仍可受伤；离线期间死亡则按正常规则重生，不回到断线前的生命或位置。客户端保存的是地址和票据，不是可信的游戏存档；本机制不覆盖服务器重启或跨服恢复。

预测按业务分别处理：角色移动由 CMC 保存、确认和重放 SavedMove，本项目补充瞄准/冲刺模式位；GAS 负责能力激活与确认/拒绝。武器的客户端弹药显示由权威基准扣除尚未包含的输入，快照水位和逐发 ACK 防止重复扣减或拒绝一发时抹掉后续预测。换弹本地预测播放，服务器接管同一播放实例，仍由唯一权威时钟装弹。已播放的枪声不会反向撤销，客户端不预先提交最终伤害；历史命中查询与客户端预测拒绝也不是同一种回滚。

Fire 的远端授权只有一个入口：服务器先检查批次的拥有者、装备代际、轮次、首发和输入时效，再进入原生 GAS 的 CanActivate/Commit。原生 key-only 的 Fire 激活、结束与释放消息到达后不再驱动业务，其他能力仍走原生流程。普通松开、取消和超时使用完整轮次身份进入同一路由；即使尾发已确认或未产生射击，空 Finish 也能结束自己的轮次并收口预测键。服务器仍在原武器执行器中重排、去重和裁决。

输入最多保留8发、每发最多6次常规发送。补发基础间隔为 `clamp(SRTT + 2×RTTVAR, 20, 60)ms`；无新鲜 RTT 时回退40ms，新输入可按20ms最小间隔顺带旧输入，单个 Timer 安排最近的发送或400ms过期点。客户端不根据 RTT 猜测服务器剩余准入时间并提前停发；75ms缺口处理、原服务端时间窗与可靠终结保持不变。UE 继续负责底层可靠传输，有限输入补发不替代 UDP/RTO，也不保证任意弱网零损失。当前协议为7，旧客户端须一同重建；详细策略、预算与实测口径见 [FPS_NETWORK_ACCEPTANCE](FPS_NETWORK_ACCEPTANCE.md)。

网络身份、恢复准入、预测账本和权威结算保留在 C++，HUD 蓝图读取本地快照并呈现。把布局或绘制迁入蓝图，不表示将网络裁决也迁入蓝图。编译、自动化、最新双端弱网和人工画面检查分别验收，不能用历史通过记录代替当前版本的端到端验证。

## 源码结构与阅读顺序

多人框架按生命周期和状态所有者分文件，不再把七个类放进 `fpstrueMultiplayer.h/.cpp`。类名、模块名和 `/Script/fpstrue.*` 反射路径不变，不增加第二套玩法或兼容聚合头。

以下路径相对于 `Source/fpstrue/`，每组均有同名 `.h/.cpp`：

| 阅读入口 | 负责的链路 |
| --- | --- |
| [Game/fpstrueMultiplayerGameInstance](../Source/fpstrue/Game/fpstrueMultiplayerGameInstance.cpp) | 本地连接/离场 → 地图旅行 → 握手与复制就绪 → 有限重试；跨世界保存地址和票据 |
| [Game/fpstruePvpPlayerController](../Source/fpstrue/Game/fpstruePvpPlayerController.cpp) | 拥有者握手 RPC、离场和时钟采样；将请求交给 GameMode，不自行恢复角色 |
| [Game/fpstruePvpGameMode](../Source/fpstrue/Game/fpstruePvpGameMode.cpp) | 唯一服务器接纳、生成/重生、PvE 人口与目标管理器、离线保留和到期清理 |
| [Game/fpstruePvpGameState](../Source/fpstrue/Game/fpstruePvpGameState.cpp) | 对局公共事实、计分视图与各端独立性能协调器 |
| [Game/fpstruePvpPlayerState](../Source/fpstrue/Game/fpstruePvpPlayerState.cpp) | 玩家本局成绩与离线标记，死亡换 Pawn 不重置成绩 |
| [UI/fpstruePvpHUD](../Source/fpstrue/UI/fpstruePvpHUD.cpp) | HUD 订阅当前 Pawn/武器，Widget 显示快照；两者同属本地界面链，保留在同一组文件 |

连接实现内部只共用 `Game/fpstrueConnectionHelpers.h` 的无状态票据格式检查和诊断身份读取；它不保存数据，不增加管理器或新的握手入口。调用者直接包含所需类头，不依赖聚合头间接导入所有框架类。

开发场景成员实现在 `Testing/Networking/fpstrueGameModeSmoke.cpp`；正式 GameMode 留下清晰的生成、重生和恢复流程，没有增加第二个规则对象。开发验收保持显式开关及 Shipping 边界。

## 配置与资产

`DefaultEngine.ini` 的普通游戏与 Dedicated Server 默认 GameMode 均指向 `/Script/fpstrue.fpstruePvpGameMode`。旧原生 GameMode 名称只作类重定向，不保留第二套规则或空函数适配层。

默认配置无法覆盖关卡自己的 WorldSettings、LevelScript 或 Widget 事件图。删除旧类前后需核对旧 GameMode 蓝图、地图开局节点、`ingame`/`winorfail` 的旧委托与调用，完成重定向、清除旧规则接线及真实蓝图编译。保留 `ingame_network` 和必要版式来源，不因删除单机模式而删除联网 HUD、暂停/菜单外观或全部原素材。类能加载不代表旧函数节点已经迁移成功。

`DA_FPMatchWaves` 属于退出的单机配置，不替代多人敌人人口目标。敌人近战和武器 DataAsset、正式行为树/黑板、动画素材继续沿用实际绑定，不复制配置或引入新的配置管理器。

## 性能采集不随单机删除而降级

当前固定 160 AI 的 Editor/Shipping 客户端对照继续使用 `Tools/Performance/RunGroupAIBaseline.ps1`。保留 `StartPvpBaseline`、`StartPvpBaselineClient`、网络世界的真实人口/行为/画质门禁，以及 CSV/Trace、OS 线程 CPU、引擎 unit、物理与网络消费者统计；不同口径不相加为整帧耗时。

删除的是旧单机 Benchmark 组件、AI 血条 Profile 及其专属工具；`Testing/Benchmarks/fpstrueBenchmarkRunner.*` 只保留当前网络 Baseline。旧单机视觉循环不作为当前运行命令；原始 CSV、Trace、冻结报告和对应版本仍保留。

## 验收口径

源码、默认配置和文档修改不等于资产迁移或运行验收。需要分别记录默认地图冷加载、实际蓝图编译、正常/弱网双端玩法、死亡重生、断线恢复和 160 AI 基线的结果；失败与未运行不能写成通过。历史单机或其他构建的通过不自动迁移到本次多人收敛版本。
