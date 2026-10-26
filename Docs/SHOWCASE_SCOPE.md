# 展示范围与来源

main展示项目源码、方法与可追溯摘要，不是完整资源包。

- Source与开发提交`c9fa4e57fc65fa33b293a897b3cbcdb1b7025016`完全一致；树标识`d2a685198f623cebc43ca9b335406ecd4d11b2d1`。Config、Tools与uproject也同步该提交，没有展示专用玩法。
- 文档及公开JSON只发布职责、方法、指标与校验值。Content、原始日志/Trace、凭据、缓存和机器身份不发布，不改写既有历史。
- 本轮只删除冗余和收敛入口：Editor/Shipping编译通过，35项受影响回归通过（29 clean、6 warned）。未重跑全套压测。
- 既有网络实测对应3c59c4f：首激活故障首流19/19，执行早于可靠重发约198.55ms；该Trace仍有其他拒绝，不作为本轮新成绩。
- 三组固定/自适应对照保留所有失败；发送量减少同时P95上升，严格零拒绝预算未完全满足。弱网渲染横移仍失败，不宣称公网、8人、发布包或商业FPS等级。
- UE负责底层可靠传输、移动预测与GAS生命周期；项目只增加有界业务输入、完整轮次身份和唯一授权/结算，没有自研UDP/RTO或完整物理世界回滚。

按[职责归属](CODE_OWNERSHIP.md)→[实现链路](INTERVIEW_GUIDE.md)→[网络合同](FPS_NETWORK_ACCEPTANCE.md)→[入口清理证据](Testing/entry-consolidation-20261026.json)阅读。
