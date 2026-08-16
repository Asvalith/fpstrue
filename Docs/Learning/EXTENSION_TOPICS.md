# FPS 条件扩展索引

> 文档身份：发布后的学习与替换方案索引，不代表当前项目已经实现。
>
> 使用原则：先描述新增条件和当前方案为何失效，再选最小替代方案，最后给出验证指标。完整历史推导保存在 [Portfolio_Technical_Extension_Map_FULL_20260730.md](../Archive/Portfolio_Technical_Extension_Map_FULL_20260730.md)。

## 1. AI 与多敌人

| 触发条件 | 候选方案 | 主要代价 | 验证指标 |
| --- | --- | --- | --- |
| 需要视野、听觉、最后已知位置 | `UAIPerceptionComponent` + 目标记忆 | 感知事件和目标切换状态增加 | 感知延迟、候选数量、切换抖动 |
| 槽位无法处理掩体或多层地形 | EQS 生成并评分站位 | Query 成本和缓存失效更复杂 | Query 时间、可达率、评分稳定性 |
| 行为出现并行、组合和可复用子树 | Behavior Tree 或 StateTree | 迁移和调试成本 | 决策耗时、节点数量、行为可解释性 |
| 数百个完整 Character 成为瓶颈 | Animation Budget、Significance、降频或代理 LOD | 远处表现降级 | Game Thread、Anim、Movement、视觉误差 |
| 需要 500+ 简化单位 | Mass/数据导向模拟 | 现有 Actor 交互要重构 | 活跃实体数、CPU、内存、Draw Call |

当前项目的 Timer FSM、NavMesh、双环槽位和 Attack Token 在 40 个活跃敌人的发布容量内继续成立，不因框架名称升级而重写。

## 2. 武器、碰撞与物理

- 多武器、多弹药和背包：增加 InventoryComponent 与 WeaponData 资产，先定义装备所有权和切换事务。
- Projectile 武器：只有弹速、重力、提前量和飞行时间成为玩法时，才从 Hitscan 增加 Projectile 策略；曳光表现不等于真实 Projectile。
- 通用伤害对象：用 Damageable 接口、Physical Material 或 Hit Zone 解耦具体敌人类。
- 独立 Weapon Trace Channel：只有 Visibility 同时承担视线、交互和子弹规则并产生冲突时再建立。
- 近战隔墙：先在现有伤害提交前加 LOS 校验；只有规则确实不同才新增通道。
- 大量尸体：按 `Full Ragdoll -> Sleep -> Frozen Pose -> Destroy` 分级，并由数量、距离、可见性和 Awake 状态控制预算。

验证必须同时覆盖命中正确性、Scene Query 时间、物理时间、重复伤害和碰撞矩阵，不能只看动画效果。

## 3. 生命周期、异步加载与对象池

- 波次生成尖峰：先分帧生成，再用软引用和 AssetManager 预加载；不要自建线程池调用 UObject、SpawnActor 或蓝图。
- GC 尖峰：先证明对象数量和 GC 时间相关，再调整回收节奏、引用关系或缓存策略。
- 对象池：只有 Spawn/Destroy P95 已成为瓶颈，且对象可以完整 Reset 时才引入；必须验证 Timer、Delegate、AIController、Health、碰撞和 UI 引用均无残留。
- 资源异步：后台阶段只做线程安全数据和 IO，UObject 创建、Actor Spawn 和场景写操作回到 Game Thread。
- Travel/重启：检查 Timer、Delegate、Task、软/弱引用以及全局单例是否跨 World 残留。

## 4. 联网与 GAS

- 当前单机 FPS 不需要帧同步；若改联机，采用服务器权威的状态同步。
- GameMode 仍只存在服务器；可复制的时间、波次和结果迁移到 GameState，玩家长期状态放 PlayerState。
- 移动使用 CharacterMovement 自带预测与校正；射击验证瞄准方向、射速、弹药、距离和遮挡。
- Hitscan 高延迟场景再评估服务器回溯；记录延迟、丢包、误判和作弊边界。
- 只有复杂属性、Buff、技能组合、Tag 门禁和预测需求出现时才引入 GAS。
- GAS 深挖重点：ASC Owner/Avatar、CanActivate/Commit/Activate、PredictionKey、GE/ExecCalc、AttributeSet、GameplayCue。

## 5. 渲染与多平台

- 后处理：当前只实现饱和度、对比度、色彩偏移、暗角和 Blend Weight；SSAO 是引擎概念追问，不写成项目成果。
- 风格化：Toon Diffuse、Rim Light、Custom Depth/Stencil Outline 逐项 A/B，分别记录 GPU 增量。
- VSM：继续用页覆盖、粗页和非 Nanite 投影者定位；不要用扩大队列或全局禁用掩盖根因。
- GPU Driven Rendering：只有 Draw Call、可见性提交或大量静态实例成为主要瓶颈后再评估。
- 软光栅遮挡：适用于 CPU 提交和遮挡效率问题，需要验证误剔除、遮挡收益和移动相机稳定性。
- 多平台：使用 DeviceProfile、Scalability、纹理/骨骼最小 LOD、输入抽象和 PSO 缓存；每个平台单独记录 CPU、GPU、内存和画质边界。

## 6. 源码阅读优先级

1. `Character.cpp`、`PlayerController.cpp`、`GameMode.cpp`：对应当前 Gameplay Framework 和输入/Possess/规则边界。
2. `ActorReplication.cpp`、`PlayerState.cpp`：仅在第二个联机项目中作为高优先级。
3. `TextureStreaming.cpp`：对应当前纹理池治理实战。
4. `ObjectMacros.h`、Delegate、容器和弱引用：对应反射、事件与生命周期。
5. Deferred Renderer 与 PostProcess：用于解释 Base Pass、Lighting、后处理和 VSM，不把源码阅读写成已完成渲染功能。
6. GAS 源码：只对第二个 GAS 项目列为最高优先级，不反向强加给单机 FPS。

## 7. 扩展题回答模板

1. **条件变化**：面试官修改了什么规模、平台、网络或玩法约束。
2. **当前失效点**：现方案在哪个边界开始不成立。
3. **最小替代**：先给能解决问题的最小增量，不直接堆框架。
4. **代价与风险**：复杂度、内存、延迟、一致性或表现损失。
5. **验证**：固定场景、单变量 A/B、P95/P99、对象数和正确性回归。

