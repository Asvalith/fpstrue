# 代码职责与动作生命周期

面试优先阅读 [INTERVIEW_GUIDE](INTERVIEW_GUIDE.md) 的当前主链和取舍；本文用于追到源码及历史变更，不把不同日期的规则与测试数量拼成当前结论。

## 2026-10-24：本轮收敛后的当前入口

下面各日期是历史记录，失败、测试数量和旧模式说明不能拼接为当前结论。本轮结果统一见 [重构验收](REFACTOR_ACCEPTANCE.md)。

- 普通预览、监听主机和专服只有 `AfpstruePvpGameMode` 一套玩法规则。四张项目地图、两个 HUD 已实际迁移并保存；两个旧 GameMode 蓝图和旧波次配置在确认引用后移入可恢复备份，不再保留空适配类。
- 换弹只有武器组件的权威事务时钟提交弹药。Montage 只负责表现，缺失、播放结束或混合结束均不决定装弹成功；显式取消仍取消当前事务。两个原素材中的旧 ReloadCommit Notify 已移除，旧原生 Notify 类与未使用的超时配置同步删除。
- 武器装备/动作保留公共复制，弹药/备弹只发拥有者；逐发结果使用可靠 ACK，不在快照重复携带没有消费者的 ShotId/Result。两类快照仍由同一权威发布派生，公共表现与弹药确认各自比较版本，避免新 ACK 屏蔽迟到的合法动作。
- 连接缓存凭证不等于本次握手已获接纳。重连和新连接均清除旧接纳编号，握手成功后才能开始项目心跳；同一次握手的重复就绪通知不重新初始化。
- AI 黑板继续使用原生存储与缓存 KeyID；Controller 维护个体上下文，SurroundManager 维护席位与有界候补，Combat 拥有攻击事务。等待不清除失败移动的退避记录，目标位置通知返回后验证原上下文仍有效，避免旧调用栈覆盖新目标。

不因减少类或行数而合并不同状态所有者，也不把不同时间点的必要复核当作重复代码删除。纯表现无需服务器再次批准，生命、弹药、攻击许可和伤害仍由各自服务器所有者提交。

## 2026-10-23：多人唯一入口与固定攻击候补（验收中）

普通预览与服务器统一使用 `AfpstruePvpGameMode`；删除旧单机波次、倒计时胜负和专属 BenchmarkRunner。地图与 HUD 的旧节点需实际迁移、编译，不以类重定向代替资产验收。

黑板按实际资产缓存 KeyID，仍使用 UE 原生存储与批次通知。`Role/Revision` 是真实席位资格，`bHasGoal` 仅代表导航目标可用，两者不互相覆盖。

攻击请求候补固定最多 **8** 人，不包含已取得许可的攻击者，也不限制整个遭遇战的 AI 参与数量。空间候补 `SlotWaitingQueue` 与出手候补 `WaitingAttackers` 明确区分。重复攻击请求保留排位；有名额且队首仍满足真实攻击条件时，由 Combat 同步取许可并立即建立事务，不预授权。完整动作结束／取消／已有 FailSafe 才归还许可；伤害窗口结束只是进入收招。没有增加第二套攻击定时器或状态机。

本节先记录实现契约；实际构建、资产迁移、自动化与网络测试的结果分别验收，不沿用下方历史通过数量作为本轮结论。

## 2026-10-23：行为树按玩法分层，组件仍保持唯一所有权

本轮仅更新 FPS 敌人行为树及对应回归，不引入新的 AI 框架、RPC、互斥锁或第二套更新循环。外层仍使用 `Wait → SampleDecision → Priority Selector`：等待控制决策频率，采样一次性发布黑板，子树表达本轮行动。决策等待不暂停已经提交的移动、动画或攻击事务。

```text
Priority Selector
├── Retreat：有撤退请求，取消攻击并归还资源，然后寻找掩体
├── Combat：当前可见目标；依次维护已有攻击、处理超距、尝试近战、维护站位
├── Investigation：只有有效记忆／声音线索；使用最后已知位置，不使用隐藏玩家实时位置
└── Idle：没有有效目标，停止行动并原地巡视；不是路线巡逻
```

| 模块 | 唯一职责 | 行为树不能重复实现的内容 |
| --- | --- | --- |
| BehaviorTree／Blackboard | 本轮优先级与决策镜像 | 不自行授予槽位、扣血或另维护一份攻击状态 |
| EnemyAIController | 目标与记忆、移动请求归属、朝向、撤退过程 | 不创建第二份槽位占用表或伤害窗口 |
| EnemyCombatComponent | 攻击事务、窗口、同次伤害去重、捕获许可的释放 | 不因树返回成功而认定动画／攻击已完成 |
| SurroundManager | FIFO 席位、攻击容量、共享位置和导航预算 | 不依靠客户端或黑板反向授予资格 |
| SignificanceCoordinator | 更新预算及服务器／本地表现的分档 | 不替代感知、目标选择或攻击准入 |

动作叶子保持即时“提交／维护请求”的语义，不混入常驻 `MoveTo`、第二套 Service 或自动 Abort 生命周期。低血、感知与席位事件继续唤醒既有等待，再进入统一采样；撤退动作调用已有 `ResetCombat → ReleaseSurroundSlot → StopMovement`，不在树节点里再清理一次。已有攻击的维持优先于距离闲置，撤退优先于维持攻击；攻击真相仍来自 Combat，而不是 `AIState` 表现枚举。

可编辑 BT／Blackboard 只升级经过精确识别的默认模板，保留自定义规则；保存前备份，Controller 和 Enemy 蓝图绑定不重存。升级前只读导出确认当前落盘资产的 `Retreating` 正确挂在 Retreat、`Attacking` 正确挂在 Sustain，与截图所示不同；本轮不把截图异常冒充已确认的当前运行缺陷。升级后还要同时检查默认结构、编辑图和重新加载的真实保存资产。

本地表现的三项 Top-K 分别是 Full 档、阴影和硬件光追，不是服务器逻辑／渲染各一项，也不是每个 LOD 各一套名额。按最新要求将每视图 Full 默认上限设为 **20**，阴影 **5**、光追 **12** 保持独立配置；光追候选仍须先获得 Full，三项有独立排名历史但并非毫无资格依赖。服务器每目标默认 **8 内圈／12 外圈／8 并发攻击许可**，另有固定 **8 名单次攻击候补（不含正在攻击者）**；空间晋升 FIFO 与攻击候补分别维护，不限制参战 AI 总数，不按每次挥砍归还长期空间席位。多人按目标分别维护，不能用本地 Top-K 数量限制全世界的攻击者。旧报告中的 Full=12 及本轮中途验证过的 Full=8 均保留原始口径，不改写为 20 的验收成绩。

本轮修改前 7 项行为树专项全部通过（6 clean、1 warned），仅作为旧版基线。早期 Editor DLL 占用已经解除，不能继续作为当前阻塞；多人收敛版本的 Editor 首轮编译成功，`Saved/MultiplayerOnly_20261023/Automation_R1/index.json` 实际为 **83 通过、14 失败**。失败记录保留，正在按真实多人参与资格、管理器席位与新生命周期契约修正夹具并回归，不能把登记数量或旧版通过数当作本轮完成。BT／BB 的备份及升级记录保留在 `Saved/BTReview_20261023/`；资产升级、当前 Shipping、完整功能回归与新性能采集分别核验，不将其中一项代替其他验收，也不自动提交／推送。

## 2026-10-21：技术亮点与取舍的源码阅读入口

本轮在现有函数契约之外补充 41 处 `【设计取舍】`，涉及 26 个源码文件；说明为什么采用当前实现、换来了什么、仍有什么限制，不添加新的业务框架或防御层。

| 阅读主题 | 源码入口（相对 `Source/fpstrue/`） | 需要讲清的取舍 |
| --- | --- | --- |
| 射击与换弹 | `Weapons/fpstrueWeaponComponent.*`、`fpstrueCombatAbilities.*` | 准入与执行复核不是重复结算；预测、可靠 ACK、持久属性各有职责；事务编号与动画实例共同拒收旧回调 |
| 伤害与 GAS | `Characters/Shared/fpstrueHealthComponent.*` | AttributeSet 是唯一复制数值源；命中上下文按值保留但不承担攻击去重；有界同线程队列不等于多线程安全 |
| 玩家与动画 | `Characters/Player/fpstrueCharacter.*`、`Shared/fpstrueFootIK.*` | CMC 原生预测只补模式 bit；历史形状查询不回滚整个世界；玩家 IK 的同步求值门禁仍有成本 |
| 群体 AI | `Characters/Enemies/` 的 Character、Performance、AI 与 Combat | 权威逻辑和本地表现分档隔离；六盒体牺牲动画轮廓精度；集中快照仍有遍历；FIFO 席位不是并发出手许可 |
| 对局与配置 | `Game/fpstrueMultiplayer.*`、`Weapons/fpstrueWeaponConfig.h` | 唯一多人 GameMode 管规则，GameState/PlayerState 发公共事实；重连恢复保留状态；共享配置与实例运行态分开 |
| 采集与证据 | `Testing/Benchmarks/fpstrueBenchmarkRunner.*` | 正式基线与诊断分开；wall 减 CPU 不能直接识别等待类型；CSV 超时不等于共享采集器已回收 |

本轮开始前保存已验证的伤害链版本，对全部 55 个跟踪源码文件进行保留字符串的词法对比，执行 token 均未改变。注释核对不代替编译与运行：最终 Editor、Shipping 均重新编译成功，自动化重新运行 **101／101（88 clean、13 warned，0 failed／notRun）**，27 条测试警告与上一轮相同；交付校验器 62 项自测通过。记录在 `Saved/TechnicalTradeoffs_20261021/`。离线报告校验自身不宣称运行新鲜度，实际进程退出及报告由本轮另行核对。

交付范围同时包含此前完成的逻辑收敛、无消费者蓝图接口清理、伤害链精简及配套回归。下方各轮“未提交／推送”是当时状态；本轮统一整理后提交。用户原有 Floor 素材修改不纳入提交，未修改 Co-op。未重新 Cook／Stage，未把本轮编译和功能回归当作弱网、画面或 160 AI 性能基线通过；旧失败记录继续保留。

## 2026-10-21：伤害链的权威结算与本地表现

本轮只整理当前 FPS，不改 Co-op，不新增战斗框架。权威表示最终状态由服务器写入，不表示服务器内部每个普通函数都要经 RPC 往返。

| 环节 | 服务器职责 | 客户端职责／需要的同步 |
| --- | --- | --- |
| 开火 | 准入与执行复核、装备／会话／ShotId 去重、射速和弹药检查、权威射线／历史查询 | 本地预测开火和后坐力；上传必要射击意图，接收最终确认 |
| 命中与扣血 | 部位伤害、近战同次攻击去重、伤害队列、GAS 属性提交、死亡与计分 | 不再次 ApplyDamage；用 GAS 复制属性更新血条和死亡边沿 |
| 命中上下文 | 每次伤害按值保存方向／位置／骨骼，致死时另冻结快照 | 不逐次同步整份上下文；死亡表现状态保留必要的方向／位置／骨骼 |
| 弹道与受击 | 发已确认的瞬时事实；身体姿态影响命中骨骼、Root Motion 或攻击中断时仍保留服务器处理 | 粒子、声音、贴花、镜头反馈和表现去重在本地消费，不反向结算 |
| 死亡收尾 | Health 提交死亡标签并取消能力，角色处理移动／装备／尸体，对局负责计分与重生 | 持久死亡状态支持晚加入；重复通知不能重复掉枪、结算或开启物理 |

已删除的冗余：

- `MulticastShot` 不再单独发送射线起终点和无消费者的 `PresentationKey`；UE 原生 `FHitResult` 已序列化起终点，保留 `ShotIndex + EquipmentGeneration + Session`。查询完成后、任何伤害回调前固定端点；不改变蓝图弹道事件签名，不新写序列化器。拥有者仍先接收真实命中特效，再抑制已经预测过的枪声／后坐力，避免漏表现。
- 当前多人角色没有旧 `OnPlayerDamaged` 脚本覆盖时，不发送空转的 owner 受击 RPC；按实际脚本实现判断，保留旧单机及合法子类的反馈。瞬时 owner 反馈使用不可靠 RPC，不影响生命或死亡的持久复制。
- 删除不再复制、也没有蓝图引用的旧 `FFPReplicatedHealthSnapshot`／Revision／反射兼容入口，保留已有 `CurrentHealth/MaxHealth` 读口。真实数值仍仅由 GAS AttributeSet 复制；批次结束直接消费，首包必通知，之后同值不重复刷新 HUD／死亡边沿。
- 内置同值 Reset 不再构建／应用无效 GE；外部 GE 即使生命同值仍执行，保留其他修改器与应用回调。删除玩家死亡链重复的 `CancelAllAbilities`，保留退出／控制权变化时的取消。
- 删除敌人受击阶段重复的无条件 `ForceNetUpdate`；生命真实变化、档位变化、攻击／死亡状态仍保留各自必要调度。致死命中不额外发送瞬时受击 RPC，由持久死亡快照驱动表现。重复调度不等于必然发了多份血量包。

必须保留：逐发准入与执行两个时点的复核、近战扣血前锁存命中标志、命中上下文按值排队、致死上下文冻结、死亡副作用去重、回调之间的退出检查。它们分别保护不同生命周期，不作为“重复判断”统一删除。没有改弱网判定时间窗、伤害数值、攻击资格或 ACK 规则，也不以本轮改动宣称带宽／帧率量化收益。

Editor 与 Shipping 编译成功；实际自动化 **101／101（88 clean、13 warned，0 failed／notRun）**，27 条测试警告未增加。原用例补测：无蓝图消费者时实际受击 RPC 分派为零、原生受击仍分派一次；真实蓝图覆盖识别；首包同值／重复同值／单属性／逆序批次；内置同值 Reset 与外部同值 GE 的不同副作用；环境命中／未命中／历史命中的原始端点；重入移动射手不改写旧弹道身份。测试数量和批准名单不变。记录在 `Saved/DamageChainCleanup_20261021/`。

本地 Editor 双客户端严格烟测 `Saved/Networking/PvpSmoke_20261021_185750_82457bc7/Report.json` 通过：主动射击客户端 34 发均执行并终结确认，0 意外拒绝／重复开火表现／缺失最终 ACK，PvE 与硬断重启恢复通过。另一个客户端是观察者，未主动射击；此项不冒充双向交替射击或画面验收。首轮 `PvpSmoke_20261021_185557_1ab6a160` 在客户端启动前失败：启动参数将可写 DDC 指向配置中已作为只读复用的同一目录，造成无可写缓存；原日志／失败报告保留。第二轮只恢复脚本默认独立 `Saved/LocalDDC`，没有改游戏代码、判定阈值或测试门禁。

本轮改变了原生 RPC 参数布局，联机两端必须使用同一重新构建版本；编译成功不代表旧 Cook／Stage 已更新。尚未重新打包、公网／弱网或 160 AI 性能验收，不提交或推送。

## 2026-10-21：删除没有消费者的蓝图接口

通过 UE 原生 `ObjectExporterT3D` 读取当前已保存的 `/Game` 蓝图及关卡对象图（66 个蓝图、16 张地图），再结合 C++ 调用和订阅核对；不是根据 uasset 二进制字符串缺失猜测“没有实现”。引用清单为 `Saved/BlueprintHookCleanup_20261021/BlueprintReferences.json`。

- 删除没有蓝图实现的 `OnWeaponEquipped`、`OnAttackStarted`，以及仅为这两个通知存在的额外调用和复核；原生装备委托继续服务动画与 HUD。
- 删除无订阅的 `OnWeaponReloadStarted`、`OnConnectionStateChanged` 及相关委托类型、广播；换弹播放只走带 `ReloadId` 的命令，连接状态仍由原状态机提交并可查询。
- 删除已完成资产迁移的无参 `CommitReload`／`FinishReload`、角色攻击结束兼容桥及组件无参诊断、无用诊断标志和 `GetReloadStartServerTime`。真正带播放身份的攻击 Notify、换弹事务及服务器计时不变。
- 保留旧单机蓝图已连接的瞄准、血量、受击和死亡事件，以及敌人的攻击播放、受击、死亡事件；`OnPickUp` 也有实际绑定。不是把所有 Blueprint 标记一律删除。
- 原占位兼容测试改为旧 `ReloadId` 的提交／完成／取消不能影响新事务；攻击无身份路径改测真实原生 Notify，批准名单同步更名，不减少测试数量。

Editor／Shipping 编译成功，实际自动化 **101／101（88 clean、13 warned，0 failed／notRun）**；测试警告 27 条，较上轮少 3 条，分别来自已删除的无参攻击诊断；其余用例的警告数不变。交付校验器 62 项自测通过。编译及回归记录同在上述 Saved 目录，未重新 Cook／Stage、未跑本轮性能／公网／弱网验收、未提交或推送。验收后的源文件变更仅修正注释措辞。

全量对象导出虽然 82 项全部完成，但进程返回 1：加载 `CoffeeShopA_P` 触发素材地图重复 `LevelScriptActor` 的 ensure（日志含 53 条错误记录），不能称为“全素材健康检查通过”。该原始失败日志保留，未通过保存或重建场景掩盖；目标玩家／敌人／武器图表没有此加载错误。此次不保存任何蓝图或地图，用户已有的 Floor 修改哈希不变。Co-op 本轮仅只读筛查候选，未修改另一工程。

## 2026-10-20：最终注释与阅读约定

本轮检查当前 FPS 的 55 个 C++／构建源码文件，覆盖玩法、网络、AI、动画／性能、自动化测试和编辑器工具；不改引擎、第三方素材或蓝图图表。沿用下方六条主链，不新增管理器或状态层。

- **模块说明**：先说明谁调用、流程顺序、谁持有状态，以及不负责什么。
- **函数说明**：声明处给调用契约；复杂实现再说明计算、输出、副作用和必要时序。声明与实现共用同一契约，不为凑数重复两遍。
- **边界说明**：明确服务器裁决与本地表现、Game Thread 与快照计算、原始计时与统计结论；不把开关开启或候选入选写成已经执行或已经提速。
- **工具说明**：区分只读审计、显式 Apply／保存、驱动真实输入的验收。创建资源的 helper 不自动代表安全覆盖或已做备份哈希校验。
- **复习方法**：先读正常流程和唯一状态所有者，再看委托、动画完成、网络晚包等边界；不逐行背 Guard、Revision 或类名。

已删除同义重复、过时和误挂说明；补齐内联查询、RPC、代理、队列内部函数及测试 helper。注释审核对 1,644 个声明／实现候选做交叉匹配，未发现缺口；这是辅助静态检查，不冒充完整 C++ AST 或蓝图审计。55 文件去注释后的词法 token 与本轮开始逐项一致，未修改玩法条件、网络协议、Guard 或执行顺序。报告在 `C:/UEBuild/FPSCommentReview_20261020_R1/CommentsAudit.json`。

验收：48 个源码文件有注释差异，其余保留已有准确说明；独立注释补丁为 `CommentsOnly.patch`。Editor／Shipping 编译成功；本轮 `Automation_R1/index.json` **101／101（87 clean、14 warned，0 failed／notRun）**，30 条测试警告及完整日志 100 条警告与上一轮归一临时编号后完全一致。未运行会保存资源的 Editor 命令、未改素材、未重新 Cook／Stage、未提交或推送；上轮弱网失败仍保留，本轮不冒充弱网问题修复。

## 2026-10-20：逻辑收敛，按六条主链阅读

本轮只收敛现有流程，不增加框架。先读正常流程，只有跨越委托、蓝图事件或异步完成点时，再看身份和生命周期保护。

| 主链 | 正常流程 | 唯一提交者 |
| --- | --- | --- |
| 开火 | 输入 → Fire GA → 请求准入 → 有界队列 → 执行复核 → Fire | 武器组件扣弹、执行命中；服务器裁决 |
| 换弹 | Reload GA → 武器创建 ReloadId → 动画／计时完成 → 提交或取消 | 武器组件维护换弹事务与弹药 |
| 生命 | UE 伤害／Instant GE → 同线程排队 → AttributeSet → 通知 | AttributeSet 存数值，生命组件组织通知 |
| 单体 AI | 感知 → 黑板／行为树 → Controller → 移动／攻击组件 | Controller 执行决策，不自行授予群体资格 |
| 群体 AI | 申请 → FIFO 等待 → 槽位分配 → 释放／补位 → 统一通知 | SurroundManager 管席位和攻击预算 |
| 对局 | 初始化 → 生成并注册 → 运行 → 结算 → 清理 | GameMode 管阶段及参与者注册 |

具体删减：Fire 不再另存“已松开”，释放统一进入 EndAbility；射击共用装备归属和时间窗校验，但保留准入、执行两个时点；包围槽释放不重复通知，目标失效也由队列出口排空通知；清除误挂、过时和重复注释。未按文件大小拆分组件。

只保留能说清用途的保护：

- `ActivationRevision`：CommitAbility 等同步回调可以取消并重启同一能力；`IsActive` 无法区别新旧激活。
- `ReloadId`／装备代际：旧动画完成或旧网络输入不能修改新事务。
- `bEnding`／EndPlay：退出阶段不接受新工作，先解绑再触发取消回调。
- 生命队列：当前伤害的通知先结束，再处理监听者追加的伤害／重置；不是多线程队列，128 次上限仍保留。
- Death 与 EndPlay：两个合理入口共用一次注册表移除，不是两套注销实现。

验证补充在原有用例中，不增加重复测试框架：非有限射击时间及非法身份、死亡回调重置、伤害通知中销毁 Owner 后丢弃待处理事务、目标离场后的槽位待通知集合排空。后者不等同于 Controller 通知次数验收。

原生接口暴露面检查未发现蓝图可直接写生命镜像、弹药、装备指针或槽位账本；合法命令仍经过各自所有者。此结论不是逐个蓝图资产图的完整接线审计，也不意味着引擎原生 C++ 接口被封死。

本轮证据：`C:/UEBuild/FPSLogicCleanup_20261020_R1/`。Editor／Shipping 编译均成功，`Automation_R1/index.json` **101／101（87 clean、14 warned，0 failed／notRun）**；新增断言包括在内，测试警告 30 条，完整日志警告 100 条与上一轮归一临时编号后逐条一致。批准名单未增减。旧性能 A/B 暂停，不能把本轮逻辑修改混入碰撞体单因素对比。

- 正常本地双客户端：`Saved/Networking/PvpSmoke_20261020_204622_4442bc07/Report.json` 严格通过，34 发全部执行并确认，覆盖 PvE 和硬断恢复。
- 弱网：`PvpSmoke_20261020_204749_ffce457f` **严格失败**，每方向额外延迟 100 ms、波动参数 20 ms、丢包 2%。38 发均收到最终 ACK，36 发接受，ShotId 2／3 在准入时因年龄 404.930／304.983 ms 超过当时 232.062 ms 窗口而被拒绝；`Deep.NoUnexpectedShotRejections=false`，180 秒超时，未进入重连。没有漏最终 ACK 或重复表现，但不能据此称弱网体验通过。保留失败、不扩大窗口、不盲目重跑；时间判断本轮为等价抽取，归因仍需隔离对照，不能直接断言与本轮无关。

两轮均为 Editor NullRHI 本地功能验证，不是 Shipping 打包运行、画面、公网或 160 AI 性能验收。本轮未重新 Cook／Stage。代码差异单独冻结为 `C:/UEBuild/FPSLogicCleanup_20261020_R1/LogicCleanup.patch`（不包含之前未提交的性能观察器）；没有提交或推送。

## 2026-10-20：敌人命中改用原生盒体

六个命中部位直接使用 `UBoxComponent`，保留组件名、`EnemyHitProxy` 标签、根节点相对位置／尺寸、通道继承和部位伤害映射；删除专用 StaticMesh 硬引用、缩放换算、缺网格校验和 `BuildEnemyHitHull` 生成分支。不新增自己的几何求交或物理系统。

盒体只在服务器启用 QueryOnly，客户端不创建权威查询。活体旧骨骼物理退出；攻击／受击／Root Motion 的必要姿态规则不变；死亡先关闭六盒，再恢复原 PhysicsAsset 布娃娃。原死亡资产和 PvP 玩家的历史命中几何不在删除范围内。

`SM_EnemyHitConvex.uasset` 已移出 `Content`，可从 `Saved/Backups/NativeBoxHits_20261020/` 恢复，原文件 SHA256 为 `3BA5AFE3D9C3433AD3C4117870EBF8404696A793D3DD9647058C7F47BB632D90`。不删除历史 Cook／Stage 和冻结报告；下面旧六凸包报告不代表当前六盒体性能。

验收同时检查真实 `enemy_BP` 直接加载／实例化和显式蓝图编译，不只测原生类；AssetRegistry 扫描活动项目包的硬／软依赖。新基线观察只读已有 BodySetup，逐敌人六盒、代理 Convex 为零，不能靠聚合总数掩盖错误组件。

本轮证据目录为 `C:/UEBuild/FPSBoxHits_20261020/`。首轮回归 `Automation_R1` 在显式蓝图编译时触发 UE 重实例化断言：前序 `FTestWorldWrapper` 只销毁世界而不立即 GC，重实例化试图在已退出的旧世界生成 Actor。保留原失败日志；测试在创建本轮世界之前完成 GC 隔离，继续验证真实编译，不关闭重实例化、不屏蔽失败，也不修改生产代码绕过此断言。

Editor／Shipping 编译通过，补全编译前实例覆盖和测试 GC 隔离后，`Automation_R2/index.json` **101／101（87 clean、14 warned、0 failed／notRun）**。六盒生命周期用例零警告／错误；30 条测试期警告及完整日志 100 条警告均与上一版原生复用回归归一临时对象编号后逐条一致，没有新增缺包或蓝图类型警告。批准清单仍为 101，工具自测 62 项通过；离线验证的 `FreshnessChecked=false` 不冒充运行时新鲜度证明。

验证源码差异为 `ValidatedSource.patch`，SHA256 `64EF11FB1B5C973C110734273C309B63F5EE4722914E22A1CD9903846E29BBA7`；Editor DLL／Shipping EXE SHA256 分别为 `8FA0886380D31C693825023CE669741E76983953AF24A3949EF3946B2AB06ABF`／`AFA9EB6EB05605F53015B57D34533ACE83A6498F7C00ABF33CF9B7376C0FE235`。未重做 Cook／Stage，旧打包文件不包含当前替换。

`NativeBox160_20261020_R1` 实际集成通过：服务器 START／STOP 各 960 Box／0 Convex，客户端零权威查询／物理状态，160 棵行为树持续就绪；两个目标管理器仍各 8 内环／12 外环占位，三个进程自然退出。正常移动靶烟测通过；标准弱网（不含移动靶）亦通过，36 发完整执行确认，含 PvE／硬断恢复。附加“弱网＋移动靶”首发 PvP 未命中、后续未进入，失败保留且未修改网络判定刷绿。逐轮结果见[实验日志](Performance/EXPERIMENT_LOG.md)，不把单轮诊断当作新性能基线。

## 2026-10-20：优先复用 UE 原生能力

本轮基于 `842b472` 检查生产模块，目标是删掉重复工作，而不是为减行数改变协议、生命周期或战斗规则。没有新增生产源文件、管理框架、Protobuf 或替代网络层。

| 原重复实现 | 当前处理 | 保留的业务边界 |
| --- | --- | --- |
| HUD 在每次 DrawHUD 查询 Pawn 并判断是否重绑 | 原生 `OnPossessedPawnChanged`；初始化补读，EndPlay 解绑 | 晚建 HUD、换 Pawn、旧武器通知隔离 |
| AttributeSet 回调自行扫描生命组件 | 原生 `GetOwningAbilitySystemComponent()`；删除生命组件空复制覆写 | AttributeSet 仍是唯一复制数值来源，兼容通知顺序不变 |
| 射击 Timer 先 Clear 再 Set；低帧后坐力同帧补调 | 同句柄直接使用原生 SetTimer 替换；后坐力开启 `bMaxOncePerFrame` | 空队列／取消仍清理，恢复按实际游戏时间积分，射击节拍不变 |
| 手读 GameDefaultMap 配置键 | 原生 `UGameMapsSettings`，明确请求 Client 地图 | PvP 覆盖优先、包路径校验，不接纳 URL 附加选项 |
| 离线保留与断线状态两个同值字段 | 合并为既有 `bDisconnected` | 保留原 Pawn／PlayerState、认领后清标志、到期释放 |
| 外圈已不用攻击接近点，却仍查询和据此重试 | Outer 只生成／消费站位；Inner 保留三个接近候选 | 默认攻击 EQS 候选 80→44；重试看真实消费者，FIFO／版本／软边界不变 |
| 玩家脚部查询重复过滤、重复类型转换 | 复用现有支撑查询入口，收窄已知玩家类型 | 仍用原生世界查询和 `AnimationCore::SolveTwoBoneIK`；探针、计数、支撑选择不变 |

以下不是应删除的“轮子”：GAS 自定义 TargetData 的薄 `NetSerialize`、SavedMove 两个移动模式位、逐发身份与最终 ACK、命中历史、动作播放实例身份、重连安全票据、围攻 FIFO／预算／请求版本。这些补充项目独有语义，底层传输、预测、Timer、BT、感知、导航、EQS、Significance 和 AnimationSharing 已使用 UE 原生能力。

不直接把自适应 DecisionWait 换成普通 Wait：当前还需要初始错峰、动态间隔和事件提前唤醒；也不把多 Mesh 换弹桥替换成只管单 AnimInstance 的任务。没有语义等价证据就不删除，避免用更隐蔽的状态错误换取少几行代码。

本轮构建／回归证据目录为 `C:/UEBuild/FPSNativeReuse_20261020/`。Editor／Shipping 首轮编译通过；`Automation_R1/index.json` 首轮 **101／101（87 clean、14 warned、0 failed／notRun）**，注册名单未增减，测试警告 30 条及完整日志警告与上一轮归一临时对象编号后完全相同。批准清单工具自测 62 项通过。新增断言覆盖真实占有事件、晚建 HUD、解绑、原生 ASC 获取、地图选择、离线保留、低帧恢复及内外圈真实目标，不以手动调用掩盖事件接线。

- 正常本地双 NullRHI：`Saved/Networking/PvpSmoke_20261020_190323_df956f2f/Report.json` 严格通过，活跃端 34 发完整执行确认，移动命中、PvE、死亡重生及硬断恢复通过。
- 模拟弱网：`PvpSmoke_20261020_190451_55f43c0a` 严格通过，活跃端 36 发完整执行确认；每发送方向额外 100 ms、波动参数 20 ms、丢包 2%。两轮均无异常拒绝、漏最终 ACK 或重复表现，不代表公网、160 AI 弱网、原进程自动重连或画面验收。
- 160 AI 集成诊断：`C:/UEBuild/FPSBaselines/NativeReuse160_20261020_R1/results.json` 有效，固定两玩家、160 AI，1851 个完整服务器采样帧，283 次人口检查中行为树始终 160 棵就绪。START／STOP 的两个 Manager 均有 8 个可用且占用的内圈、12 个可用且占用的外圈，STOP 攻击许可分别 4／3，未超配额；外圈不依赖废弃接近点。客户端 Full Render 12、共享姿态 143，三个进程均自然 exit0。本次明确 `DiagnosticOnly=true`、`FormalBaseline=false`，是单轮 CPU 采样的集成验证，不是新的三轮性能收益或 160 个同时攻击者验收。

验证源码差异保存在 `ValidatedSource.patch`，SHA256 `0BCC06C0E36DF66B2674D8831D0D6AAF13BFB0D1E7B4D6DFBF61ED69B8CE2135`；Editor DLL／Shipping EXE 的 SHA256 分别为 `18A0861B05D3C191B3F8EF086E417433AA27FB819D1422A9684D96D97D191DEF`／`21E0578FA1DA0154476BFCBCDA6C8E7E5A07B22561E942439EF889DE4468CA45`。本轮未重做素材 Cook／Stage；旧包不代表这些新代码。

此前 `Tactical160_20261020_B1` 的三轮性能数据属于冻结的 `842b472`，不能直接移称本轮精简后的性能收益；新增断言也不是新增玩法功能。

以下保留 2026-09-29 的历史整理和验收记录。

本轮整理针对 2026-09-29 的两次审查摘要，重点是统一状态写入口、明确播放身份和清理责任。第二次摘要提到 51 项审查事项，包含条件风险与设计建议，不是 51 个已复现 Bug；附件中的完整报告链接未提供本地正文，因此不宣称逐项全部修复。保留现有类型、资产路径和有效复习注释，不通过新增一批 Manager 整理架构。

## 1. 从哪里开始读，谁负责修改状态

所有路径相对于 `Source/fpstrue/`。

| 阅读入口 | 唯一业务责任 | 不在这里做什么 |
| --- | --- | --- |
| `Game/fpstrueMultiplayer` | 连接与就绪、玩家接纳／出生／重生、计分、敌人人口与断线保留；按类划分唯一所有者 | GameInstance 不裁决伤害，GameMode 不执行客户端渲染或维护采集阶段 |
| `Characters/Player/fpstrueCharacter` | 能力输入、标签派生的速度与表现、控制权变化、装备槽的内部维护 | 不重复声明 GA 的动作互斥，不对外开放能只改一半装备关系的 setter |
| `Weapons/fpstrueCombatAbilities` | 开火、换弹、瞄准和冲刺的激活、互斥、取消与持续任务 | 不另存弹药、不接受客户端命中或伤害结果 |
| `Weapons/fpstrueWeaponComponent` | 拾取入口、完整装备/卸下、单发射击、弹药、换弹身份及结束结果 | 不维护第二套动作状态机，不用“当前正在换弹”推断旧动画通知归属 |
| `Characters/Shared/fpstrueHealthComponent` | 提交生命值变化及依次通知，处理通知期间的重入请求 | 重置生命值不等于复活角色、恢复 AI 或重新装备 |
| `Characters/Enemies/AI/fpstrueEnemyBehaviorTree` | 用本轮快照选择行为，校验 Blackboard 契约 | 不直接修改攻击内部阶段 |
| `Characters/Enemies/AI/fpstrueEnemyAIController` | 目标入口、本轮决策快照、朝向、移动请求与执行结果 | 不把无预算和不可达当成同一种失败 |
| `Characters/Enemies/fpstrueEnemyCombatComponent` | 一次攻击的目标、许可来源、播放身份、窗口、伤害及清理 | 不在结束时反查新 Controller 来猜旧攻击的资源来源 |
| `Characters/Enemies/AI/fpstrueSurroundManager` | 共享目标缓存、导航重试、围攻槽位、攻击及移动请求预算 | 不把“有位置”当成“导航投影也有效” |
| `Characters/Enemies/AI/fpstrueEnemyPositionQuery` | EQS 候选生成、导航及环境筛选、掩体路径验证 | 不分配槽位、不授予攻击许可、不执行移动或伤害 |
| `Characters/Enemies/Performance/fpstrueEnemyPerformance` | 全局重要性与预算分配、动画共享注册；两个协调器保持各自职责 | 不直接改玩家输入或武器状态 |
| `Characters/Enemies/fpstrueEnemyCharacter` | 合成当前战斗保护与性能策略，应用自身 Mesh/Movement 设置；死亡表现 | 不自行分配全局 Full/阴影/光追名额 |
| `Runtime/fpstrueRuntime` | 共享播放身份与碰撞通道；诊断开关和策略覆盖的只读启动快照 | 不拥有具体动作状态、不启动测试、不锁输入、不写 CSV |
| `Testing/Benchmarks/` | 实验阶段、测试条件、采集资源及产物校验 | 不成为正常玩法读取配置的依赖 |

当前由 GameState 启动网络 Profile／Baseline，不再创建单机 BenchmarkRunner 组件。正常玩法类只依赖 `Runtime` 参数；保留的 `FFPBenchmarkConfig` 只服务旧网络 Profile 的人数／种子覆盖，Baseline 在原实现中持有自己的采集参数。业务埋点保留在业务函数，声明集中在 `Runtime/fpstrueRuntime.h`。下段源码行数是 2026-09-29 的历史整理结果，不是本轮删减统计。

后续精简以本轮开始时的工作区为基准：`Source/fpstrue` 的 C++/头文件/构建规则共 14,517 → 13,813 行、60 → 54 个文件（含注释和空行）。其中玩法及运行时代码减少 212 行，测试器减少 109 行，移除一次性迁移工具 415 行；回归测试增加 32 行。合并输入绑定与动作重置、波次配置解析、攻击空间采样和采集轮询；保留身份、重入及资源归还检查，补充其职责、时序与清理注释，没有新增管理层。

## 2. 武器：先建立身份，再提交和结束

阅读顺序：`Character` 输入 → ASC 与四个 GA → `AttachWeapon/DetachWeapon`、单发 `Fire`、`BeginAbilityReload` → `CommitReloadForTransaction` → `EndReload` → 所有者离场清理。

- 装备/卸下同时维护双方引用、实际附着及动作清理。装备事件返回后再次确认关系，监听者立即卸下时不误报成功。
- 换弹开始创建 `ReloadId`；蓝图收到 `OnWeaponReloadPlaybackRequested(ReloadId, bWasEmptyReload)` 后，通过 `PlayReloadMontage` 明确提交 Mesh 和 Montage。外部播放只允许通过 `BindReloadMontage` 显式登记，不再比较播放前后的列表猜归属。
- `FFPActionPlayback` 统一保存动作编号、Mesh、AnimInstance、Montage 和实例 ID，武器与近战复用身份校验，仍各自拥有业务状态和清理责任。
- 原生换弹 Notify 核对播放实例；正常结束、取消、播放失败、超时、禁用收敛到 `EndReload`。自然结束等待本次绑定的手臂和枪械播放全部结束；中断停止同一事务持有的播放，不停止无关实例。
- `OnWeaponReloadEnded` 报告原因及是否已经装弹。取消不回滚已提交弹药；超时只解除动作锁，不伪造装填成功。播放命令只走携带 `ReloadId` 的 `OnWeaponReloadPlaybackRequested`，不再另设无人订阅的开始通知。
- GA 的 `ActivationRevision` 与武器 `EquipmentRevision` 核对同步回调后的激活及装备身份；`ReloadId` 和 Montage 实例拒绝延迟旧通知，两者不互相替代。网络动作枚举仅为标签派生的展示结果。
- 控制权变化只中断旧输入事务，不永久禁用装备；转换期间拒绝委托重新发起动作。角色离场完整结束独立 Pickup Actor 上的武器组件，不能依赖角色组件销毁连带清理。
- Fire GA 按上一发的剩余冷却安排 `WaitDelay`，能力结束即清理任务；后坐力恢复按实际经过时间积分。已经发生的射击继续报告事实，不因回调结束动作而漏报。
- 单发提交时保存表现激活编号与序号，伤害和弹道监听者即使同步重启开火，旧发多播也只使用自己的身份，不读取新一轮计数。

### 蓝图入口与兼容边界

确认已保存蓝图没有引用后，无参数 `CommitReload/FinishReload` 与角色的 `HandleAttackFinishedNotify` 兼容符号已删除。组件只接收携带真实播放身份的原生攻击完成通知，不把旧通知包装成当前动作。`CancelReload` 表示“现在取消当前动作”，仍可作为用户命令；异步取消必须保存发起时的 ID，使用 `CancelReloadForTransaction`。

项目已迁移 `BP_Weapon` 的 reload 图和 `enemy_BP` 的攻击图：保留动画选择、普通/空仓素材、受击和死亡表现，移除无身份完成回调。一次性迁移与图审计工具已从 Source 删除，不再随项目编译；源码恢复包位于 `Saved/Backups/ActionPlaybackMigration/CompletedMigrationTools_20260929.zip`。迁移记录、原资产备份和真实蓝图回归测试保留。

武器仍是一次性拾取/禁用生命周期；本轮没有扩展通用背包、掉落、切枪或对象池。枪口是否额外做遮挡查询也属于独立玩法规则，没有随本轮重构改变。

## 3. AI：决策数据、执行结果与攻击资源分开

阅读顺序：真实感知记录有效玩家 → Controller 选择目标、从唯一多人 GameMode 取得该目标的 Manager → Controller 采样并发布 Blackboard → BT 选择 → Controller 执行移动或请求 Combat → Combat 结束。停止后的上下文注入不重启 AI；只有明确的新 Possess 生命周期启动行为逻辑。

- Controller 是决策快照的写入入口，Blackboard 是供树观察的发布数据，AIState 是行为状态，不让三处各自决策。发布多个键时暂停并恢复通知，避免观察者看到半轮更新；拒绝不兼容对象键和逐敌人键的实例同步。
- 移动结果区分 `Moving/Arrived/Deferred/Unreachable/InvalidContext`，保存真实 RequestID 与提交版本处理同步回调和旧完成。去重包含三维位置、接受半径及过滤器；围攻与追击各自维护失败退避，不能交替覆盖一条失败记录。
- 共享目标缓存将位置版本、成功导航结果有效期和失败重试分开，目标未移动也会到期重投影。选槽验证最终返回的攻击接近点；释放槽位先核对占用者弱引用身份，旧失效项不清掉新占用者。
- 攻击开始保存目标及许可来源，经 `OnAttackPlaybackRequested(AttackId)` → `PlayAttackMontageForAttack` 绑定明确的播放实例。自然结束完成攻击，中断取消，播放失败立即清理；不取“第一个活动 Montage”猜新攻击。
- StopAI 和上下文替换先禁止接收新战斗命令，再停止行为树、移动和 Montage，避免停止委托重入重新占用许可。结束清理统一处理窗口、Timer、许可和动画保护。
- 开始攻击检查高度和环境遮挡；刀刃采样先按环境阻挡截断有效轨迹，再处理 Pawn 命中。同一攻击多个窗口仍只提交一次目标伤害。
- `SampleAttackReach` 为一次 AI 决策同时提供距离、有效攻击范围和可达性；正式提交攻击仍重新检查，不能用旧快照越过环境或状态变化。
- EQS 共享接近查询按目标位移与缓存期限刷新；异步结果核对查询编号、版本及目标快照后才发布。具体敌人的占位与许可仍由 SurroundManager 管理，不因接入 EQS 改成逐敌人重复查询。
- 低血通知唤醒行为树；撤退先取消攻击、归还许可和站位，再按已知威胁位置申请掩体。初次响应和等待查询保持及时，已有目标后的维护才按 Gameplay Significance 降频；超时、无掩体、移动失败、目标遗忘和离场都释放查询与掩体占用。撤退不增加回血，也不读取已丢失玩家的实时位置。

原生 Attack Window 和 `Enemy Attack Finished` Notify 校验播放身份。攻击播放实例自身也负责结束回调，不依赖受击 Montage 代为结束攻击。自动化区分原生逻辑和真实项目蓝图入口；NullRHI 下通过不等于刀刃轨迹、LOD 保骨和运动画面已经验收。

## 4. 表现与对局：合成设置，清理自己持有的状态

- 战斗保护和动画降频在 Character 的应用入口合成，统一处理 TickInterval、VisibilityBasedAnimTickOption 与 URO。受击先退出共享、解除已排入的 Mesh Tick 冷却，再发受击表现；局部保护到期会自行恢复，不等待协调器。死亡布娃娃不继续参与存活档位调度。
- 阴影和光追候选先确认资产确实有对应资格，再竞争配额；战斗保护不赠送这两类名额。
- CSV 区分策略 `MinLOD*` 和 CPU 预测 `PredictedLOD*`；旧 `LOD*` 列为兼容保留最小 LOD 约束口径，不能当成实际 GPU 各 Pass 使用的 LOD。
- AnimationSharing 同 World 停止后可复用自己创建的 Manager；不接管其他 Manager，也不在骨架不兼容时强行重用。
- Health 先提交本次结果，按伤害、血量、死亡顺序完成通知，再处理重入请求。方向、位置和骨骼随伤害记录值拷贝入队，死亡冻结实际击杀记录，后来的命中不能覆盖它。
- 无数值变化的 Reset 不广播。每条同步通知链最多接纳 128 项；超限的新请求在入队前拒绝并告警，已接纳请求照序处理。不通过下一帧转发把死循环隐藏成永久任务。
- 多人 GameMode 分别处理玩家死亡／重生和直接离场／短时保留，死亡不结束整个多人世界；敌人人口通过单一 Timer 分批补充，不再运行单机波次。
- 敌人 BeginPlay 后登记，死亡后按活体过滤安排人口补充并保留尸体，EndPlay 解除登记。死亡和直接离场不能漏清理，也不能让尸体误计为活体；旧 `GetWaveConfig` 已删除。

## 5. 网络采集：玩法状态与实验资源分开

当前入口是进程外的 `RunGroupAIBaseline.ps1` 和进程内的 `StartPvpPerformanceProfile`／`StartPvpBaselineClient`；旧单机组件的阶段枚举、场景覆盖和专属测试已删除，不在多人流程中保留空壳。

- 一进程一轮，等待真实参与者、AI 人口及固定视点就绪，再预热和采集；START／STOP 记录角色、策略和消费者读回，不以启动成功代替有效样本。
- 网络 Baseline 保留线程 CPU、unit、物理／复制统计与逐敌人快照。父子和并行模块耗时不可直接求和，墙钟减线程 CPU 也不等于精确阻塞时间。
- 旧网络 Profile 仍保留独立 CSV 租约：确认实际开始，停止后等待真实落盘；取消或超时不提前释放仍在启动／写盘的采集器，World 消失后由 CoreTicker 完成收尾。
- 进程外脚本校验 Trace／CSV／状态文件与输入身份，独立进程退出码报告失败；旧配置、历史数据和分析脚本保留，但不恢复 `AutoBenchmark` 兼容分支。

本轮没有重跑 160 AI 性能；旧结果须保留原构建与采集口径，不能与迁移后构建直接相减。功能回归也不证明新版采集门禁、画面或 GPU 性能通过。

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
