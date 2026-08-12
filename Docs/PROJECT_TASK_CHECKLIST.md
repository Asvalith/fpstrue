# FPS 架构、玩法与基础图形任务清单

本文记录当前工作区的待办项。Gameplay 结果由 C++ 负责，蓝图负责资产编排和表现；只有具备独立状态、生命周期或复用价值的能力才抽成 Component。

## P0：玩法正确性

### 换弹事务

- [ ] 新增 `EFPWeaponActionState::Ready / Firing / Reloading / Disabled`。
- [ ] 把弹药、换弹状态和换弹 Timer 从 Character 移入 WeaponComponent。
- [ ] 增加 `ReloadSequenceId` 和 `bReloadAmmoCommitted`，拒绝旧 Timer 和旧 Notify。
- [ ] 使用 AnimNotify 提交弹药，保证一轮换弹只提交一次。
- [ ] 使用 Montage Completed/Interrupted 收口状态；Timer 仅作超时恢复。
- [ ] 换弹开始时统一停止瞄准、冲刺和射击，并广播对应状态变化。
- [ ] 明确规则：当前版本换弹期间不可开火，Montage 正常结束后恢复 Ready。
- [ ] 验证普通换弹、空仓换弹、死亡中断、重复按键、长按开火和 Montage 被替换。

完成标准：动画未结束前无法造成射击伤害；动画中断不会加弹；同一轮换弹不会重复结算。

### 群体攻击名额

- [ ] 删除或限制无 Token 的直接攻击入口。
- [ ] 只有持有 SurroundManager Attack Token 的敌人可以进入 Attack。
- [ ] 攻击结束、超时、死亡、失去目标和 UnPossess 时释放 Token。
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

- [ ] Character 只保留移动、视角、输入意图、当前装备引用和角色死亡协调。
- [ ] 拆分 LocomotionState 与 WeaponActionState，避免移动状态和武器状态混用。
- [ ] 移除每帧测试文字；状态变化改为事件驱动。
- [ ] 在 EndPlay/Controller 变化时清理默认 Input Mapping Context。
- [ ] 检查重新 Possess、重新开始和窗口失焦后的输入状态。

### WeaponComponent

- [ ] 由 WeaponComponent 统一拥有弹药、射速、连续射击、换弹和开火冷却。
- [ ] 把蓝图自动开火 Timer 移到 C++，蓝图只响应单发、空仓、换弹和命中事件。
- [ ] 保存并移除 Enhanced Input Binding Handle，避免输入组件长期积累绑定。
- [ ] 统一 WeaponData 与组件默认参数，避免同一参数存在两套权威值。
- [ ] 在“子类多态”和“WeaponFamily + DataAsset”中选择一种主要武器策略。
- [ ] 当前 Rifle/Shotgun 只有射线数量差异时优先数据驱动；行为真正分化后再保留子类。

### HealthComponent

- [ ] 保持为独立 Component。
- [ ] 补充可靠的初始 Health 快照，确保 UI 在绑定后立即获得当前值。
- [ ] 为 MaxHealth 增加合法范围限制。
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

- [ ] 把 AutoBenchmark、CSV、纹理和内存命令移出正式 GameMode。
- [ ] 检查 Character、Weapon、Enemy、GameMode 的 Timer 与 Delegate 清理路径。
- [ ] 清理 enemy_BP 中残留的旧 Tick、Timer、AI MoveTo 和手写追击节点。
- [ ] 确认所有攻击 Montage 只使用一种命中入口；旧单点 Notify 与 AttackWindow 不在同一攻击中重复结算。
- [ ] 确认 Projectile 路径无资产引用后，清理旧 Projectile 类、include 和重定向。
- [ ] 缓存 Surround 槽位的导航投影，只在玩家移动超过阈值或 NavMesh 变化时刷新。

## P2：基础图形学与材质

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
- [ ] Gameplay 修改至少覆盖正常、重复输入、中断、死亡和重新开始。
- [ ] 图形修改使用相同关卡、机位、分辨率和质量设置做 A/B 对比。
- [ ] 记录 CPU Frame、GPU Frame、主要 Pass、Texture Pool 和对象数量。
- [ ] 未采集的数据写成“预期效果”，不填写虚构提升比例。
- [ ] 完成 P0 后再进行大规模组件重构和风格化封版。
