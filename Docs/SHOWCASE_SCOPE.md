# 展示范围与来源

main展示项目源码、方法与可追溯摘要，不是完整资源包。

- Source与开发提交`3c59c4fe2a440dce3cbf3cbdbd25680f50c69990`完全一致；树标识`60d2b6c1649ef9d8942091692f21b1f8d1ee3d1d`。Config、Tools与uproject也同步该提交，没有展示专用玩法。
- 文档及公开JSON只发布职责、方法、指标与校验值。Content、原始日志/Trace、凭据、缓存和机器身份不发布，不改写既有历史。
- 完整98项自动化与Editor/Shipping通过；普通/弱网横移、NullRHI弱网同进程断包恢复及过期退款通过。
- 首激活真实丢包专项首流19/19接纳，执行早于可靠激活重发约198.55ms；带Trace整轮另有过期拒绝，不认证容量。
- 三组固定/自适应对照保留所有失败；发送量减少同时P95上升，严格零拒绝预算未完全满足。弱网渲染横移仍失败，不宣称公网、8人、发布包或商业FPS等级。
- UE负责底层可靠传输、移动预测与GAS生命周期；项目只增加有界业务输入、完整轮次身份和唯一授权/结算，没有自研UDP/RTO或完整物理世界回滚。

按[职责归属](CODE_OWNERSHIP.md)→[实现链路](INTERVIEW_GUIDE.md)→[网络合同](FPS_NETWORK_ACCEPTANCE.md)→[本轮证据](Testing/fps-adaptive-fire-20261026.json)阅读。
