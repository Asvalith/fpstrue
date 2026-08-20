# FPS 封板清单

> 本文只记录剩余工作和验收门禁。架构说明见 [FPS_Core_Technical_Summary.md](FPS_Core_Technical_Summary.md)，性能结论见 [PERFORMANCE_BASELINE.md](PERFORMANCE_BASELINE.md)。

## 当前状态

- C++ 核心闭环已完成：武器、伤害、敌人 AI/近战、波次、倒计时和胜负。
- 固定敌人规模、纹理驻留、VSM 实验和尸体回收已有证据。
- 剩余重点是蓝图接线回归和发布包验收，不再增加新系统。

## P0：蓝图回归

- [ ] `Demonstration` 使用正确的 `fpstrueGameMode` 和玩家 Pawn。
- [ ] 进入游戏前完成 HUD/结果事件绑定，再调用 `StartGameMode()`。
- [ ] `BP_Weapon` 只响应三个表现事件，不再保存弹药、伤害或射速状态。
- [ ] 换弹：开始事件播放双手/武器 Montage；Completed 调 `FinishReload`；Interrupted 调 `CancelReload`；弹匣插入处触发 Reload Commit Notify。
- [ ] 射击：场景命中播放贴花、声音和粒子；敌人命中不播放混凝土贴花。
- [ ] `enemy_BP` 使用 `fpstrueEnemyAIController`，动态生成自动 Possess；无旧追击 Timer/Tick/AI MoveTo。
- [ ] 攻击 Montage 包含 AttackWindow NotifyState，Socket 为 `weapontop` / `weaponend`。
- [ ] `OnEnemyDamaged` 只做存活受击表现；`OnEnemyDied` 只做死亡表现，不再 Destroy Actor。
- [ ] 瞄准按下/松开视口、FOV 和手臂表现均能恢复。
- [ ] UI 的生命、弹药、倒计时、波次和存活数首次值正确，后续由事件更新。

## P0：玩法验收

- [ ] 移动、跳跃、冲刺、瞄准、拾枪、开火、停止开火和换弹完整可用。
- [ ] 换弹期间不能开火；换弹完成只提交一次弹药；中断后状态恢复。
- [ ] 头部/身体伤害正确；死亡只触发一次。
- [ ] 敌人能生成、寻路、绕障、靠近攻击；单次攻击不会重复扣血。
- [ ] 玩家死亡立即失败；时间到且玩家仍存活才胜利；结果界面只创建一次。
- [ ] 三波生成、敌人计数和 90 秒倒计时正确。
- [ ] 敌人死亡后保留尸体反馈，并在约 30 秒由 C++ LifeSpan 回收。
- [ ] Output Log 无 Blueprint Runtime Error、Accessed None 和循环报错。

## P0：构建与发布

- [ ] 关闭 Live Coding 后执行完整 Development Editor 编译，避免旧热重载补丁干扰。
- [ ] 编译所有改动过的蓝图并保存，修复重定向器和失效资产引用。
- [ ] Windows Shipping Cook/Package 成功。
- [ ] 在打包版本完成一次从主菜单到胜负界面的全流程 Smoke Test。
- [ ] 打包版本复查输入、NavMesh、UI、声音、特效和结果重开。

## P1：证据收口

- [ ] 为当前最终蓝图版本补一轮 `10 / 20 / 40 / 80 / 160` 快速回归；若代码和场景成本未变，可沿用已有详细矩阵并注明日期。
- [ ] 补纹理调整前后相同机位截图，确认可接受的画质差异。
- [ ] 保留一次 VSM 告警复现截图和诊断输出，说明残余风险而不是宣称彻底修复。
- [ ] 保存最终打包日志、运行截图和短演示视频。

## 停止线

封板前不新增联网、GAS、行为树、EQS、对象池、自建线程池或新图形功能。只有 P0 回归发现阻断问题时才修改 C++；其他内容进入后续项目。
