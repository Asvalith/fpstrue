# FPS 架构、玩法与基础图形任务清单

本文记录当前工作区的待办项。Gameplay 结果由 C++ 负责，蓝图负责资产编排和表现；只有具备独立状态、生命周期或复用价值的能力才抽成 Component。

## P0：玩法正确性

### 一次性提交边界

- [x] HealthComponent 使用死亡广播标记，保证每次 Reset 之间最多广播一次 `OnDeath`。
- [x] 致死伤害跳过普通 Damaged 表现，只进入 Death 表现，避免两个 Montage 争抢同一 Slot。
- [x] GameMode 使用敌人注册表统一处理 Death/Destroy，同一敌人只允许注销和减少存活数一次。
- [x] `bGameEnded` 保证一局只广播一次胜负结果；结算同时停止场上敌人 AI。
- [x] Character 接收 HealthComponent 的 `OnDeath`，完成自身死亡状态后通过 `OnPlayerDeathReported(this)` 向 GameMode 报告；游戏进行中立即失败，不等待倒计时。
- [x] 倒计时归零只负责检查胜利；生命值大于 0 且未进入死亡状态时 `FinishGame(true)`，不在倒计时中轮询或触发失败。
- [ ] PIE 验证剩余时间内死亡、时间归零时存活、时间归零前致死和死亡/倒计时同帧四种结算路径。
- [x] WeaponComponent 用动作状态、换弹序列号和提交标记保证每轮换弹最多提交一次。
- [x] 射速门禁通过后才扣除一发弹药，并只广播一次真实射击事件。
- [x] PickupComponent 使用消费标记，防止同帧重入导致重复装备或重复广播。
- [x] EnemyCharacter 在整轮攻击开始时清空命中集合；多个攻击窗口共享去重状态，Landed/Missed/Finished 每轮最多各一次。
- [x] SurroundManager 的 Attack Token 使用 `TSet`，重复申请和重复释放保持幂等。
- [ ] PIE 覆盖重复伤害、敌人外部 Destroy、攻击多窗口、Montage 中断、死亡中断和关卡退出。

完成标准：重复输入、Notify、Timer、Death/Destroy 或 EndPlay 的先后顺序不能造成重复扣血、重复加弹、重复计数或重复结算。

### 换弹事务

- [x] 新增 `EFPWeaponActionState::Ready / Firing / Reloading / Disabled`。
- [x] 把弹药、换弹状态和换弹 Timer 从 Character 移入 WeaponComponent。
- [x] 增加换弹序列号和 `bReloadAmmoCommitted`，拒绝旧 Timer，并由状态门禁拒绝已结束换弹的 Notify。
- [ ] 使用 AnimNotify 提交弹药，保证一轮换弹只提交一次。
- [ ] 使用 Montage Completed/Interrupted 收口状态；Timer 仅作超时恢复。
- [x] 换弹开始时统一停止瞄准、冲刺和射击，并广播对应状态变化。
- [x] 明确规则：当前版本换弹期间不可开火，Montage 正常结束后恢复 Ready。
- [ ] 验证普通换弹、空仓换弹、死亡中断、重复按键、长按开火和 Montage 被替换。

完成标准：动画未结束前无法造成射击伤害；动画中断不会加弹；同一轮换弹不会重复结算。

### 群体攻击名额

- [x] 把 `TryAttackTarget()` 收为仅 EnemyAIController 可调用的私有入口，避免其他类绕开 Token。
- [x] 当前运行链只有持有 SurroundManager Attack Token 的敌人可以进入 Attack。
- [x] 攻击结束、超时、死亡、失去目标、GameMode 结算和 UnPossess 时释放 Token。
- [ ] 记录当前攻击者数量，验证不超过 `MaxConcurrentAttackers`。
- [ ] 统一 AttackRange 的含义，明确它是中心距离还是胶囊表面外的武器距离。

完成标准：20 个以上敌人包围时，同时攻击数量稳定受限，没有 Token 泄漏或长期饥饿。

### 通用伤害入口

- [ ] 武器不再只对 `AfpstrueEnemyCharacter` 调用 `ApplyPointDamage`。
- [ ] 允许所有合法 Damageable Actor 进入统一伤害链。
- [ ] 头部判定改为可配置骨骼集合、Physical Material 或命中区域接口。
- [ ] 使用项目专用 Weapon Trace Channel，避免把 `ECC_Visibility` 同时当作视线和子弹规则。
- [ ] 验证 Enemy、TargetDummy、物理物体和不可伤害场景物体。

完成标准：TargetDummy 和敌人共用 HealthComponent 伤害链，武器代码不依赖具体敌人类。

## P1：职责与状态治理

### Character

- [x] Character 只保留移动、视角、输入意图、当前装备引用和角色死亡协调；弹药 Getter 仅作兼容转发。
- [x] 拆分 LocomotionState 与 WeaponActionState，避免移动状态和武器状态混用。
- [x] 关闭 Character Tick；状态变化改为事件驱动。
- [x] 在 EndPlay/Controller 变化时停止开火并清理默认 Input Mapping Context。
- [ ] 检查重新 Possess、重新开始和窗口失焦后的输入状态。

### WeaponComponent

- [x] 由 WeaponComponent 统一拥有弹药、射速、连续射击、换弹和开火冷却。
- [x] 把自动开火 Timer 移到 C++，蓝图只响应单发、空仓、换弹和命中事件。
- [x] 移除 WeaponComponent 的 Enhanced Input 依赖，由 Character 统一绑定并转发开火命令。
- [ ] 创建并赋值正式 WeaponData 资产，再移除组件默认参数回退，避免同一参数存在两套权威值。
- [x] 代码层选择 `WeaponFamily + DataAsset` 作为武器配置策略；资产创建与赋值尚未完成。
- [x] 当前 Rifle/Shotgun 只有射线数量差异时使用数据驱动；行为真正分化后再增加策略或子类。

### HealthComponent

- [x] 保持为独立 Component。
- [ ] 补充可靠的初始 Health 快照，确保 UI 在绑定后立即获得当前值。
- [x] 为 MaxHealth 增加编辑器 Clamp 和运行时最小值限制。
- [ ] 评估事件是否需要 PreviousHealth、MaxHealth、DamageType 和击杀上下文。
- [ ] 验证 Reset 后再次死亡仍只广播一次当前生命周期的死亡事件。

### UI 与 PlayerController

- [ ] 新增 C++ PlayerController 管理主菜单、游戏 HUD、暂停界面、光标和 Input Mode。
- [ ] UMG 创建后先读取一次 Health、Ammo、Time、Wave 和 AliveEnemyCount 快照。
- [ ] 后续通过 Delegate 更新，不使用 Widget Tick 或 Text Property Binding 查询核心数据。
- [ ] 补齐弹药、波次、存活敌人、倒计时、胜负和重新开始显示。
- [ ] GameMode 继续保存单机对局规则；将来联机时再把可复制状态移入 GameState。

完成标准：HUD 无每帧 Getter 绑定，开始游戏时所有字段立即显示正确初始值。

## P2：组件候选与边界

### 适合抽成 Component

- [ ] 多个敌人类型需要复用近战窗口时，抽取 `UMeleeAttackComponent`。
- [ ] `UMeleeAttackComponent` 管冷却、攻击事务、Socket Sweep、单次攻击去重和中断清理。
- [ ] 游戏设计需要视野、听觉和丢失目标时，在 AIController 添加 `UAIPerceptionComponent`。
- [ ] 出现武器切换、背包和多弹药类型时再新增 `UInventoryComponent`。
- [ ] 出现门、钥匙、按钮等多种交互时再新增玩家 `UInteractionComponent` 与 `IInteractable`。

### 不建议现在抽成 Component

- [ ] Sprint、Aim 和基础移动继续留在 Character。
- [ ] 波次、倒计时和胜负继续留在 GameMode。
- [ ] SurroundManager 保持中央协调 Actor，不给每个敌人复制一套槽位系统。
- [ ] 性能距离分级暂留 EnemyCharacter/AIController，出现多个 Actor 家族复用后再抽象。
- [ ] 不为当前单机项目引入 GAS、Behavior Tree 或复杂服务层来替代已经可控的简单 FSM。

### 生命周期与开发工具

- [x] 停止跟踪 `Binaries / Intermediate / Saved / .sln` 生成文件，并完成一次重新生成 BuildRules 的全量 Development Editor 编译。
- [ ] 把 AutoBenchmark、CSV、纹理和内存命令移出正式 GameMode。
- [x] 检查 Character、Weapon、Enemy、Health、TargetDummy、GameMode 的 C++ Timer 与 Delegate 清理路径。
- [ ] 清理 enemy_BP 中残留的旧 Tick、Timer、AI MoveTo 和手写追击节点。
- [ ] 确认所有攻击 Montage 只使用一种命中入口；旧单点 Notify 与 AttackWindow 不在同一攻击中重复结算。
- [ ] 确认 Projectile 路径无资产引用后，清理旧 Projectile 类、include 和重定向。
- [ ] 缓存 Surround 槽位的导航投影，只在玩家移动超过阈值或 NavMesh 变化时刷新。

### 目标搜索与感知治理

- [x] GameMode 在敌人生成时注入 Player 和 SurroundManager，AI 决策不使用每帧 `GetAllActorsOfClass` 或全 Pawn 扫描。
- [x] 区分目标获取、战术槽位、NavMesh 路径和攻击命中查询，不把四类问题集中到 EnemyCharacter Tick。
- [ ] PIE 验证玩家死亡、重新 Possess、重新开始和目标引用失效后的 AI Idle/Stop/重新注入路径。
- [ ] 只有玩法需要视野、听觉、最后已知位置和丢失目标时，才接入 `UAIPerceptionComponent` 和目标记忆。
- [ ] 只有出现多玩家、诱饵或召唤物时，才增加 Candidate Registry、Team Filter、Threat Score 和切换迟滞。
- [ ] 只有规则槽位无法处理复杂掩体/多层地形时，才引入 EQS 对候选位置做可达性、视线和距离评分。
- [ ] 记录 AI Decision、Target Resolve、Move Request、Nav Query 和候选数量，避免用框架名称代替性能证据。

完成标准：能区分“目标是谁、站在哪里、怎么到达、是否命中”四条链；新条件出现时按需求升级，不用高频全图搜索掩盖上下文装配错误。

### 物理与碰撞治理

- [x] 区分场景查询、物理模拟和 Gameplay 伤害：Trace/Sweep 决定命中，HealthComponent 决定扣血，Impulse 只作用于模拟刚体。
- [x] 玩家和敌人使用 Capsule + CharacterMovement；枪械使用 Line Trace；近战使用攻击窗口内的帧间 Sphere Sweep。
- [x] 敌人死亡先关闭移动和 Capsule，再由蓝图开启 Ragdoll，C++ 下一帧检查物理状态并施加命中冲量。
- [ ] 在敌人蓝图回归 `OnEnemyDied -> Ragdoll Profile -> Set Simulate Physics(true)`，验证没有 Physics Asset 时的失败表现。
- [x] 近战封版保留 `ECC_Pawn` 对象查询、`TargetCharacter` 精确过滤和整轮命中去重，不新增专用近战碰撞通道。
- [ ] 枪械是否建立独立 Weapon Trace Channel 单独评估，不与近战查询绑定迁移。
- [ ] 用可配置骨骼集合、Physical Material 或 Hit Zone 替换头部骨骼名硬编码。
- [ ] 记录 10/25/50 个活动 Ragdoll 的 CPU Physics、Frame P95、对象数和内存回落，确定尸体数量预算。
- [ ] 对 `bTraceComplex`、Sphere 半径和帧间采样数做固定靶场 A/B，同时记录准确率和 Scene Query 成本。
- [ ] 为射击、近战、Overlap、死亡切换和物理冲量建立碰撞测试矩阵，覆盖 Ignore/Overlap/Block 和 NoCollision/QueryOnly/QueryAndPhysics。
- [ ] 增加 WeaponLineTrace、MeleeSweep、ReturnedHit、ActiveAttackWindow 和 RagdollActive 计数器，并为 Weapon Trace 添加 Scene Query 统计标签。
- [ ] 记录默认攻击窗口实际 NotifyTick 数，计算并实测 `SampleCount + 1` 次 Sweep 的每轮查询量。
- [ ] 使用薄墙、门框和动态门回归近战隔墙问题；若复现，优先增加现有 Visibility/LOS 校验。
- [ ] 用剑刃端点位移和 TraceRadius 推导自适应采样数，与固定 4 采样做漏判率/查询量 A/B。
- [ ] 在 Debug Draw、屏幕消息和高频日志关闭条件下采集 Scene Query 数据。
- [ ] 建立 Ragdoll `Full -> Sleeping -> Frozen -> Destroyed` 分级策略，按距离、可见性、Awake 状态和全局预算转换。
- [ ] 验证 `PutAllRigidBodiesToSleep / IsAnyRigidBodyAwake / WakeAllRigidBodies`，区分休眠与关闭物理模拟。
- [ ] 审计敌人 Physics Asset 的 Body、Constraint、相邻骨骼自碰撞和事件生成，保留质量 A/B 截图。
- [ ] 验证冻结 Ragdoll 前后的 Pose 保留，避免 `SetSimulatePhysics(false)` 后尸体回弹到动画姿势。
- [ ] 记录 Active/Awake Rigid Body、Contact/Constraint、Physics 时间和 Game/Physics 同步等待。
- [ ] 只有 Profile 证明 Physics/Sync 是瓶颈后，才评估 Chaos Threading Model 或 Async Physics；不把 GPU 当通用开关。
- [ ] 只有 Ragdoll 抖动、约束不稳或低帧率穿透可复现后，才对 MaxPhysicsDelta/Substep 做 30/60/120 FPS A/B。
- [ ] 按主文档 24.23 的统一场景题完成口述复盘：先分类问题，再给最小方案、代价和验证指标。
- [ ] PIE 实测隔墙近战、高速挥砍、50 敌人同时死亡、重复子步回调和 Character 击退五个代表场景。

完成标准：能用数据证明碰撞查询和 Ragdoll 成本；Profile 或通道调整不会导致漏命中、误命中、重复伤害或尸体继续阻挡活角色。

## P3：基础图形学与材质

### PBR 材质基础

- [ ] 建立环境、角色和武器 Master Material，并通过 Material Instance 调参。
- [ ] Base Color 使用 sRGB；Normal、Roughness、Metallic、AO 使用线性数据。
- [ ] Roughness、Metallic、AO 可按项目约定打包为 ORM，记录每个通道含义。
- [ ] 禁止把高光和阴影直接烘进 Base Color，避免与动态光照重复。
- [ ] 为常用材质提供 BaseColor Tint、Roughness、Normal Strength 和 UV Tiling 参数。

### 法线贴图

- [ ] 导入时确认 Texture Compression 为 Normalmap，关闭 sRGB。
- [ ] 检查 DirectX 法线方向；凹凸反转时确认绿色通道设置。
- [ ] 使用切线空间法线，不直接把普通 RGB 颜色贴图接入 Normal。
- [ ] 使用 `FlattenNormal` 或等价方式控制强度，不直接乘法破坏法线归一化。
- [ ] 在武器、墙面、地面和敌人近景各选择一个样本做有/无法线 A/B 截图。

完成标准：细节随光照方向正确变化，没有反凹凸、接缝闪烁或远距离噪点。

### UV、纹理与流送

- [ ] 检查 UV 拉伸、接缝和主要资产 Texel Density。
- [ ] 检查 MipMap、LOD、Texture Group 与 Max Texture Size。
- [ ] 继续保持纹理池无 Over Budget，不通过盲目扩大 Pool 掩盖问题。
- [ ] 使用 `stat streaming`、`ListStreamingTextures` 和固定视角记录前后数据。

### 光照、阴影和后处理

- [ ] 校准 Directional Light、Sky Light、曝光和阴影，先保证中性可读性。
- [ ] 固定或限制自动曝光范围，避免室内外亮度跳变掩盖材质问题。
- [ ] 检查大面积非 Nanite 阴影投射物，继续治理 VSM Non-Nanite Job Queue Overflow。
- [ ] 小型远景物体和远距离敌人按层级关闭不必要动态阴影。
- [ ] 用 `ProfileGPU` 对比 ShadowDepths、Lumen、PostProcess 和 BasePass。
- [ ] Bloom、AO、Color Grading、Vignette 分项启用并保存开关前后截图。

### 风格化效果

- [ ] Toon Diffuse：使用可调明暗阈值或 Ramp，避免完全丢失材质层次。
- [ ] Rim Light：使用 Fresnel、颜色、强度和宽度参数，并限制暗部过曝。
- [ ] Outline：使用 Custom Depth/Stencil，区分敌人、可交互物和普通场景。
- [ ] 为描边增加距离或屏幕宽度控制，避免远处线条过粗。
- [ ] 分别测量 Toon、Rim、Outline 的 GPU 增量，不把美术变化描述成性能优化。

## 验收与记录

- [ ] 每项修改记录问题、原因、方案、取舍、验证方法和结果。
- [ ] 对主文档 16.0 的六个核心难点逐项保留源码、蓝图、日志或性能证据，不把风险分析写成真实事故。
- [ ] 至少选择换弹、近战和群体 AI 三个问题完成闭卷复盘：现象、假设、根因、方案取舍、结果和遗留边界。
- [ ] Gameplay 修改至少覆盖正常、重复输入、中断、死亡和重新开始。
- [ ] 图形修改使用相同关卡、机位、分辨率和质量设置做 A/B 对比。
- [ ] 记录 CPU Frame、GPU Frame、主要 Pass、Texture Pool 和对象数量。
- [ ] 未采集的数据写成“预期效果”，不填写虚构提升比例。
- [ ] 完成 P0 后再进行大规模组件重构和风格化封版。
