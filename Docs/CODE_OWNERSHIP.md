# 代码职责与动作生命周期

本轮整理针对 2026-09-29 的两次审查摘要，重点是统一状态写入口、明确播放身份和清理责任。第二次摘要提到 51 项审查事项，包含条件风险与设计建议，不是 51 个已复现 Bug；附件中的完整报告链接未提供本地正文，因此不宣称逐项全部修复。保留现有类型、资产路径和有效复习注释，不通过新增一批 Manager 整理架构。

## 1. 从哪里开始读，谁负责修改状态

所有路径相对于 `Source/fpstrue/`。

| 阅读入口 | 唯一业务责任 | 不在这里做什么 |
| --- | --- | --- |
| `Game/fpstrueGameMode` | 对局阶段、生成队列、参与者注册、胜负与停止玩法 | 不解析性能实验命令行，不维护采集阶段 |
| `Characters/Player/fpstrueCharacter` | 输入、ADS/冲刺约束、控制权变化、装备槽的内部维护 | 不对外开放能只改一半装备关系的 setter |
| `Weapons/fpstrueWeaponComponent` | 完整装备/卸下、射击、弹药、换弹身份及结束结果 | 不用“当前正在换弹”推断旧动画通知归属 |
| `Characters/Shared/fpstrueHealthComponent` | 提交生命值变化及依次通知，处理通知期间的重入请求 | 重置生命值不等于复活角色、恢复 AI 或重新装备 |
| `Characters/Enemies/AI/fpstrueEnemyBehaviorTree` | 用本轮快照选择行为，校验 Blackboard 契约 | 不直接修改攻击内部阶段 |
| `Characters/Enemies/AI/fpstrueEnemyAIController` | 目标入口、本轮决策快照、朝向、移动请求与执行结果 | 不把无预算和不可达当成同一种失败 |
| `Characters/Enemies/fpstrueEnemyCombatComponent` | 一次攻击的目标、许可来源、播放身份、窗口、伤害及清理 | 不在结束时反查新 Controller 来猜旧攻击的资源来源 |
| `Characters/Enemies/AI/fpstrueSurroundManager` | 共享目标缓存、导航重试、围攻槽位、攻击及移动请求预算 | 不把“有位置”当成“导航投影也有效” |
| `Characters/Enemies/Performance/` | 全局重要性与预算分配、动画共享注册 | 不直接改玩家输入或武器状态 |
| `Characters/Enemies/fpstrueEnemyCharacter` | 合成当前战斗保护与性能策略，应用自身 Mesh/Movement 设置；死亡表现 | 不自行分配全局 Full/阴影/光追名额 |
| `Runtime/fpstrueRuntimeOptions` | 运行时诊断开关和策略覆盖的只读启动快照 | 不启动测试、不锁输入、不写 CSV |
| `Testing/Benchmarks/` | 实验阶段、测试条件、采集资源及产物校验 | 不成为正常玩法读取配置的依赖 |

GameMode 仍负责创建和挂接 BenchmarkRunner，这是组合入口。正常玩法类只依赖 `Runtime` 参数；`BenchmarkConfig` 复用同一份运行时快照，额外保存预热、时长、人数和输出等采集条件，不重复解析诊断开关。业务埋点保留在业务函数，声明移至 `Runtime/fpstruePerformanceStats.h`。

后续精简以本轮开始时的工作区为基准：`Source/fpstrue` 的 C++/头文件/构建规则共 14,517 → 13,813 行、60 → 54 个文件（含注释和空行）。其中玩法及运行时代码减少 212 行，测试器减少 109 行，移除一次性迁移工具 415 行；回归测试增加 32 行。合并输入绑定与动作重置、波次配置解析、攻击空间采样和采集轮询；保留身份、重入及资源归还检查，补充其职责、时序与清理注释，没有新增管理层。

## 2. 武器：先建立身份，再提交和结束

阅读顺序：`AttachWeapon/DetachWeapon` → `StartFire/Fire` → `RequestReload` → `CommitReloadForTransaction` → `EndReload` → 所有者离场清理。

- 装备/卸下同时维护双方引用、实际附着及动作清理。装备事件返回后再次确认关系，监听者立即卸下时不误报成功。
- 换弹开始创建 `ReloadId`；蓝图收到 `OnWeaponReloadPlaybackRequested(ReloadId, bWasEmptyReload)` 后，通过 `PlayReloadMontage` 明确提交 Mesh 和 Montage。外部播放只允许通过 `BindReloadMontage` 显式登记，不再比较播放前后的列表猜归属。
- `FFPActionPlayback` 统一保存动作编号、Mesh、AnimInstance、Montage 和实例 ID，武器与近战复用身份校验，仍各自拥有业务状态和清理责任。
- 原生换弹 Notify 核对播放实例；正常结束、取消、播放失败、超时、禁用收敛到 `EndReload`。自然结束等待本次绑定的手臂和枪械播放全部结束；中断停止同一事务持有的播放，不停止无关实例。
- `OnWeaponReloadEnded` 报告原因及是否已经装弹。取消不回滚已提交弹药；超时只解除动作锁，不伪造装填成功。`OnWeaponReloadStarted` 只作观察通知，不再承担动画播放命令。
- `ActionRevision` 继续处理同步委托重入；操作 ID 和 Montage 实例处理延迟通知，两者不互相替代。
- 控制权变化只中断旧输入事务，不永久禁用装备；转换期间拒绝委托重新发起动作。角色离场完整结束独立 Pickup Actor 上的武器组件，不能依赖角色组件销毁连带清理。
- 自动射击按上一发的剩余冷却安排 Timer；后坐力恢复按实际经过时间积分；已经发生的射击继续报告事实，不因回调结束动作而漏报。

### 蓝图入口与兼容边界

无参数 `CommitReload/FinishReload` 与 `HandleAttackFinishedNotify` 只保留弃用反射符号，调用会诊断并拒绝，不再把旧通知包装成当前动作。`CancelReload` 表示“现在取消当前动作”，仍可作为用户命令；异步取消必须保存发起时的 ID，使用 `CancelReloadForTransaction`。

项目已迁移 `BP_Weapon` 的 reload 图和 `enemy_BP` 的攻击图：保留动画选择、普通/空仓素材、受击和死亡表现，移除无身份完成回调。一次性迁移与图审计工具已从 Source 删除，不再随项目编译；源码恢复包位于 `Saved/Backups/ActionPlaybackMigration/CompletedMigrationTools_20260929.zip`。迁移记录、原资产备份和真实蓝图回归测试保留。

武器仍是一次性拾取/禁用生命周期；本轮没有扩展通用背包、掉落、切枪或对象池。枪口是否额外做遮挡查询也属于独立玩法规则，没有随本轮重构改变。

## 3. AI：决策数据、执行结果与攻击资源分开

阅读顺序：GameMode 注入上下文 → Controller 采样 → Blackboard 发布 → BT 选择 → Controller 执行移动或请求 Combat → Combat 结束。

- Controller 是决策快照的写入入口，Blackboard 是供树观察的发布数据，AIState 是行为状态，不让三处各自决策。发布多个键时暂停并恢复通知，避免观察者看到半轮更新；拒绝不兼容对象键和逐敌人键的实例同步。
- 移动结果区分 `Moving/Arrived/Deferred/Unreachable/InvalidContext`，保存真实 RequestID 与提交版本处理同步回调和旧完成。去重包含三维位置、接受半径及过滤器；围攻与追击各自维护失败退避，不能交替覆盖一条失败记录。
- 共享目标缓存将位置版本、成功导航结果有效期和失败重试分开，目标未移动也会到期重投影。选槽验证最终返回的攻击接近点；释放槽位先核对占用者弱引用身份，旧失效项不清掉新占用者。
- 攻击开始保存目标及许可来源，经 `OnAttackPlaybackRequested(AttackId)` → `PlayAttackMontageForAttack` 绑定明确的播放实例。自然结束完成攻击，中断取消，播放失败立即清理；不取“第一个活动 Montage”猜新攻击。
- StopAI 和上下文替换先禁止接收新战斗命令，再停止行为树、移动和 Montage，避免停止委托重入重新占用许可。结束清理统一处理窗口、Timer、许可和动画保护。
- 开始攻击检查高度和环境遮挡；刀刃采样先按环境阻挡截断有效轨迹，再处理 Pawn 命中。同一攻击多个窗口仍只提交一次目标伤害。
- `SampleAttackReach` 为一次 AI 决策同时提供距离、有效攻击范围和可达性；正式提交攻击仍重新检查，不能用旧快照越过环境或状态变化。

原生 Attack Window 和 `Enemy Attack Finished` Notify 校验播放身份。攻击播放实例自身也负责结束回调，不依赖受击 Montage 代为结束攻击。自动化区分原生逻辑和真实项目蓝图入口；NullRHI 下通过不等于刀刃轨迹、LOD 保骨和运动画面已经验收。

## 4. 表现与对局：合成设置，清理自己持有的状态

- 战斗保护和动画降频在 Character 的应用入口合成，统一处理 TickInterval、VisibilityBasedAnimTickOption 与 URO。受击先退出共享、解除已排入的 Mesh Tick 冷却，再发受击表现；局部保护到期会自行恢复，不等待协调器。死亡布娃娃不继续参与存活档位调度。
- 阴影和光追候选先确认资产确实有对应资格，再竞争配额；战斗保护不赠送这两类名额。
- CSV 区分策略 `MinLOD*` 和 CPU 预测 `PredictedLOD*`；旧 `LOD*` 列为兼容保留最小 LOD 约束口径，不能当成实际 GPU 各 Pass 使用的 LOD。
- AnimationSharing 同 World 停止后可复用自己创建的 Manager；不接管其他 Manager，也不在骨架不兼容时强行重用。
- Health 先提交本次结果，按伤害、血量、死亡顺序完成通知，再处理重入请求。方向、位置和骨骼随伤害记录值拷贝入队，死亡冻结实际击杀记录，后来的命中不能覆盖它。
- 无数值变化的 Reset 不广播。每条同步通知链最多接纳 128 项；超限的新请求在入队前拒绝并告警，已接纳请求照序处理。不通过下一帧转发把死循环隐藏成永久任务。
- GameMode 同时处理玩家死亡和直接离场；拒绝缺失正确 Controller 的生成实例；生成循环明确每帧上限。对局结束先拆开注册集合再调用外部清理，避免回调修改正在遍历的数组。
- `GetWaveConfig` 一次解析本轮数量和类型；敌人只在 BeginPlay 后登记，死亡直接注销，EndPlay 覆盖销毁及关卡移除，不再重复订阅 OnDestroyed。

## 5. 测试器：一次采集有自己的阶段和资源

`Queued → Preparing → WarmingUp → StartingCapture → Capturing → Flushing → Finished`

- 每个排队动作保存 Timer 句柄并校验 RunId，取消使旧回调失效。
- 锁定输入、镜头、对局覆盖和诊断组件设置时记录所有权；仅恢复本轮持有的资源。正式自动试验采用一进程一轮，避免重复 Start 继承上一轮 World 负载。
- 消融在预热前应用；未指定固定人数也记录就绪人数，并在采集中检查真实消费者、人数变化与避让配置。
- CSV 确认实际开始，结束等待写入结果；Trace 校验启动、连接结束和非空产物。不再以发出命令或固定等待一秒代替成功证明。
- 业务结果和采集资源释放分开：清理超时进入隔离状态，保留迟到 CSV 启动的清理回调与进程占用，直到真实写盘结束，不能提前宣布 Profiler 空闲。
- 预热使用 World Timer；进入采集启动后由 0.25 秒 CoreTicker 独占推进，避免两套轮询重复采样，并能在 World 暂停时处理超时。结果区分 `Completed/InvalidSample/Failed/Cancelled/Unsupported`；独立试验进程通过退出码报告失败，编辑器/PIE 不关闭宿主。

测试器增加了低频有效性检查，新旧测试器自身开销不完全相同，后续性能比较要用相同版本重新采集，不能把历史数据与本轮构建直接相减。独立随机流和生成布局记录尚未补入，本轮不声称跨不同执行顺序完全复现随机场景。

## 6. 验证边界

本轮验证分别记录编译、功能 Automation 和脚本配置检查；不把 NullRHI 功能回归写成画面或 GPU 性能验收。完整玩家 AnimBP 姿态、动态 NavMesh 变化、实际刀刃/碰撞和运动画面仍需实景验证。

真实资产测试覆盖范围：武器从 `BP_Weapon` 的 `RequestReload` 入口经过真实蓝图选材，使用项目 Mesh/Montage 推进世界动画 Tick，验证装填 Notify、两个播放结束及弹药守恒；为隔离移动动画，夹具使用原生 AnimInstance，不声称验证完整 PlayerAnim 姿态。敌人保留项目 Mesh/AnimBP，从 `TryAttackTarget` 经蓝图选材，检查具体播放实例的自然结束与中断。二者都不由测试再次 Bind 来修复漏接的蓝图。

2026-09-29 最终验证：

| 检查 | 结果 | 记录 |
| --- | --- | --- |
| Development Editor / Development Game 编译 | 均通过 | UnrealBuildTool 构建输出 |
| `Automation RunTests fpstrue.` | 精简后重新运行：41 项全部通过，0 失败、0 未运行；25 项无警告、16 项带夹具或故障注入警告 | `Saved/Automation/CodeCleanup_20260929_Final/index.json` |
| 两份实际蓝图迁移与新进程审计 | 编译、保存、重新加载通过；ID 接线正确，旧换弹完成/取消节点及受击误接攻击完成节点已移除 | `Saved/ActionPlaybackAudit_Before.txt`、`Saved/ActionPlaybackAudit_After.txt` |
| `Tools/Performance/TestRenderCostConfig.ps1` | 56 项通过 | 脚本控制台输出，无 UE 性能压测 |
| 差异格式检查 | `git diff --check` 通过 | 保留现有行尾策略，未整仓格式化 |

上一轮回归修复了 UnPossess 先于 EndPlay 导致换弹误报普通取消，以及后坐力测试的 Pending Timer 时序。本轮新增测试验证了真实蓝图播放链、无身份回调拒绝、停止回调重入、双策略退避、伤害队列终止与上下文、受击解除实际 Tick 冷却、迟到 CSV 启动及暂停 watchdog。最终结果来自修复后的完整重跑，不用进程退出码代替每项断言结果。

资产恢复点：`Saved/Backups/ActionPlaybackMigration/BP_Weapon_20260929_190514.uasset` 与 `Saved/Backups/ActionPlaybackMigration/20260929_190547/enemy_BP.uasset`。只保存两个目标蓝图；动画素材和其他表现图未批量改动。迁移前加载旧节点时产生弃用警告，迁移后的新进程蓝图编译汇总为 0 错误、0 警告。

Top-K 驻留规则、Gameplay 迟滞、异步资源预加载以及进一步拆分 EnemyCharacter 属于后续设计选择，没有为了关闭审查条目一次性扩大实现。

## 7. 本轮没有扩展的内容

- W05 射击事实的不可变结果快照、W07 原地失去/重新获得拾取资格后的重试，没有作为本轮播放链迁移的一部分改造。
- 输入映射共享租约、多个重叠攻击窗口、混合导航代理和波次失败政策，需要先确定业务支持范围，未新增通用管理框架。
- Top-K 名单时间稳定性、共享回退统计和共同视图预计算，不能仅凭审查建议宣称性能收益；未重开性能实验。
- CSV 离线工具已有统计列、数值解析及裁剪检查；最低帧数、采样时长容差、Trace 采样区域完整性和跨轮实际负载 manifest 尚未补齐。真实 CSV 迟到启动清理测试不能代替这些验收。

本轮收敛的是明确身份、唯一写入口、停止/清理顺序和新增机制的组合回归，不是将审查中所有条件风险都转换为额外状态机。
