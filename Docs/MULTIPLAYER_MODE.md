# 唯一多人玩法入口与迁移边界

项目收敛为多人 PvPvE：玩家可互伤，与 AI 同场交战；不再维护原单机的波次、90 秒生存胜负和死亡即全局失败流程。此次是减少玩法分支，不新增通用框架。

## 当前职责

- `AfpstruePvpGameMode`：唯一服务器玩法入口，管理玩家接纳、出生/重生、计分、敌人人口、每个目标的围攻管理器与离线保留。
- `AfpstruePvpGameState`：复制对局公共事实，并在每端装配 Significance/AnimationSharing 协调器。服务器只管权威策略，客户端只管本端表现。
- `AfpstruePvpPlayerController`：所属端请求、握手和时钟采样；引擎 Playing 状态不等于项目复制就绪。
- `UfpstrueMultiplayerGameInstance`：跨世界连接、取消、有限重试及恢复凭据，不保存或裁决生命、弹药、伤害。
- HUD：读取当前 Pawn/装备/PlayerState，换对象时解绑旧订阅并补读当前快照；不再订阅单机波次与胜负事件。

`RestartPlayer` 是单个角色出生/重生，不表示全局重新开局。地址直连、监听主机与专服入口也不等于 Steam/EOS 匹配服务。

## 配置与资产

`DefaultEngine.ini` 的普通游戏与 Dedicated Server 默认 GameMode 均指向 `/Script/fpstrue.fpstruePvpGameMode`。旧原生 GameMode 名称只作类重定向，不保留第二套规则或空函数适配层。

默认配置无法覆盖关卡自己的 WorldSettings、LevelScript 或 Widget 事件图。删除旧类前后需核对旧 GameMode 蓝图、地图开局节点、`ingame`/`winorfail` 的旧委托与调用，完成重定向、清除旧规则接线及真实蓝图编译。保留 `ingame_network` 和必要版式来源，不因删除单机模式而删除联网 HUD、暂停/菜单外观或全部原素材。类能加载不代表旧函数节点已经迁移成功。

`DA_FPMatchWaves` 属于退出的单机配置，不替代多人敌人人口目标。敌人近战和武器 DataAsset、正式行为树/黑板、动画素材继续沿用实际绑定，不复制配置或引入新的配置管理器。

## 性能采集不随单机删除而降级

当前固定 160 AI 的 Editor/Shipping 客户端对照继续使用 `Tools/Performance/RunGroupAIBaseline.ps1`。保留 `StartPvpPerformanceProfile`、`StartPvpBaselineClient`、网络世界的真实人口/行为/画质门禁，以及 CSV/Trace、OS 线程 CPU、引擎 unit、物理与网络消费者统计；不同口径不相加为整帧耗时。

删除的是依赖旧单机 GameMode 的 `UfpstrueBenchmarkRunner` 组件与专属用例，不是整个 `Testing/Benchmarks/fpstrueBenchmarkRunner.*` 文件。该文件仍承载网络 Profile/Baseline。旧 `RunRenderCostMatrix`/`AutoBenchmark` 及单机视觉循环属于历史入口，不作为当前运行命令；原始 CSV、Trace、冻结报告和对应版本仍保留。

## 验收口径

源码、默认配置和文档修改不等于资产迁移或运行验收。需要分别记录默认地图冷加载、实际蓝图编译、正常/弱网双端玩法、死亡重生、断线恢复和 160 AI 基线的结果；失败与未运行不能写成通过。历史单机或其他构建的通过不自动迁移到本次多人收敛版本。
