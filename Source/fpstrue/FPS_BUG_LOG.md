# fpstrue FPS Bug 与故障复盘

最后更新：2026-07-21

## 1. 文档目的

本文档只记录 `fpstrue` 开发过程中真实出现、实际排查或已经确认的故障与技术问题。

记录目标：

- 保存问题现象、根因、解决过程和验证方法。
- 区分“已修复”“部分修复”“临时绕过”和“待处理”。
- 为 README、项目复盘、演示视频和面试问答提供可信材料。
- 避免以后重复踩坑，或把尚未完成的功能误写成已经解决。

## 2. 状态定义

| 状态 | 含义 |
| --- | --- |
| 已修复 | 根因明确，代码或资源已经修正，核心路径已验证 |
| 部分修复 | 主要问题已处理，但仍缺少完整运行回归或边界测试 |
| 临时绕过 | 当前可以继续开发，但底层根因仍然存在 |
| 待处理 | 问题已确认，尚未进入正式修复 |
| 技术债 | 当前实现能够工作，但结构或性能存在明确风险 |

## 3. 问题总表

| ID | 问题 | 分类 | 状态 |
| --- | --- | --- | --- |
| FPS-001 | Enhanced Input Action 未赋值导致按键无响应 | 输入/蓝图配置 | 部分修复 |
| FPS-002 | 射击、换弹、瞄准和死亡状态互相冲突 | Gameplay 状态 | 部分修复 |
| FPS-003 | 连续射击 Timer 与单发射击职责不清 | 武器架构 | 部分修复 |
| FPS-004 | 视觉子弹与真实命中职责混淆 | 射击架构 | 已修复 |
| FPS-005 | 玩家、靶子和敌人血量逻辑重复 | 组件设计 | 已修复 |
| FPS-006 | 死亡后仍可能残留移动、攻击或换弹逻辑 | 生命周期 | 部分修复 |
| FPS-007 | Reload Montage 无法稳定播放 | 动画/蓝图 | 部分修复 |
| FPS-008 | 敌人可以被命中但血痕贴花不显示 | 材质/贴花 | 已修复 |
| FPS-009 | 敌人只能直线追逐，遇到障碍表现异常 | AI | 技术债 |
| FPS-010 | UE 启动时报 DDC 没有可写节点 | 编辑器环境 | 临时绕过 |
| FPS-011 | 错误打开 `fpstrue_safe1` 并产生重复 UE 进程 | 工程管理 | 已修复 |
| FPS-012 | 构建产物、生成文件和未跟踪资源混杂 | Git/构建 | 待处理 |

## 4. 详细记录

### FPS-001：Enhanced Input Action 未赋值

**状态：部分修复**

**现象**

- 冲刺、瞄准或换弹按键没有反应。
- C++ 输入绑定代码存在，但对应函数没有被触发。
- 不同角色蓝图实例可能表现不一致。

**影响**

- 玩家无法稳定使用完整战斗输入。
- 容易误判为 Enhanced Input、Mapping Context 或 C++ 绑定失败。

**根因**

`RunAction`、`AimAction`、`ReloadAction` 是由蓝图配置的 `UInputAction` 引用。C++ 声明和绑定逻辑存在，不代表蓝图已经为属性赋值。

**处理**

- 绑定前检查 Input Action 是否为空。
- 使用 `UE_LOG` 和屏幕信息输出缺失的具体 Action。
- 在 `BP_FirstPersonCharacter` 中检查并配置对应 Input Action。

**验证方法**

1. 启动 PIE。
2. 分别触发冲刺、瞄准和换弹。
3. 确认日志没有 `Action is NULL`。
4. 确认对应角色状态发生变化。

**相关代码**

- `fpstrueCharacter.cpp::SetupPlayerInputComponent`

**剩余风险**

蓝图属性仍然可能在复制角色蓝图或替换 Pawn 后丢失，需要在封版前重新检查默认 Pawn。

### FPS-002：射击、换弹、瞄准和死亡状态冲突

**状态：部分修复**

**现象**

- 换弹期间仍可能收到射击输入。
- 空仓射击、自动换弹和手动换弹可能重复进入换弹流程。
- 瞄准、冲刺和换弹可能同时保持激活。
- 玩家死亡后仍可能残留射击或换弹状态。

**影响**

- 弹药数量和动画表现不同步。
- 重复 Timer 可能导致多次回调。
- 蓝图表现状态与 C++ 规则状态不一致。

**根因**

早期实现主要依赖多个布尔值和输入事件，缺少统一状态入口与互斥检查。

**处理**

- 增加 `Idle / Moving / Reloading / Dead` 角色状态。
- 使用 `CanReload()`、`CanFireWeapon()` 和 `TryConsumeAmmo()` 统一入口。
- 换弹开始时退出瞄准并停止开火。
- 死亡时清理 Reload Timer、停止移动并通知武器停止射击。
- 空仓射击通过统一入口请求换弹。

**验证方法**

1. 换弹过程中持续按住开火，确认不消耗弹药、不产生伤害。
2. 满弹匣、无备用弹药、死亡状态下分别尝试换弹。
3. 空仓持续开火，确认只进入一次有效换弹流程。
4. 换弹过程中死亡，确认换弹不会在死亡后完成。

**相关代码**

- `fpstrueCharacter.cpp::StartReload`
- `fpstrueCharacter.cpp::CanReload`
- `fpstrueCharacter.cpp::TryConsumeAmmo`
- `fpstrueCharacter.cpp::HandleDeath`

**剩余风险**

尚未完成系统化 PIE 回归，特别是长按开火、空仓换弹和死亡同时发生的边界组合。

### FPS-003：连续射击职责不清

**状态：部分修复**

**现象**

- `StartFire`、`StopFire` 和 `Fire` 的语义一度混合。
- 蓝图 Timer 与 C++ 调用可能重复触发射击。
- 停止射击或换弹时，表现 Timer 可能没有同步停止。

**影响**

- 射速不可控。
- 单次输入可能消耗多发弹药。
- 死亡或换弹后仍可能出现枪口表现。

**根因**

连续射击的“输入生命周期”“射速调度”和“单次射击规则”没有明确分层。

**处理**

- `StartFire()` 只负责进入开火状态并广播开始事件。
- `Fire()` 只负责一次射击尝试。
- `StopFire()` 只负责退出开火状态并广播停止事件。
- 弹药、换弹和死亡限制由 Character 与 WeaponComponent 的统一检查负责。

**验证方法**

1. 单击开火只消耗一发弹药。
2. 长按开火按照设定射速消耗弹药。
3. 松开输入后立即停止。
4. 换弹、死亡和空仓时确认射击 Timer 停止。

**相关代码**

- `fpstrueWeaponComponent.cpp::StartFire`
- `fpstrueWeaponComponent.cpp::Fire`
- `fpstrueWeaponComponent.cpp::StopFire`

**剩余风险**

射速 Timer 仍需要结合 `BP_Weapon` 做一次完整检查，确认没有第二套重复调度逻辑。

### FPS-004：视觉子弹与真实命中职责混淆

**状态：已修复**

**现象**

- 视觉子弹从枪口飞行，而真实准星射线从摄像机出发，两条路径可能不一致。
- 如果让视觉子弹负责伤害，近距离、帧率变化或高速目标会造成命中差异。
- 曳光弹速度或生命周期参数未正确使用时，会停留或无法到达目标。

**影响**

- 玩家看到的轨迹与实际伤害不一致。
- Hitscan 武器失去即时命中特性。

**根因**

早期没有明确区分 Gameplay 命中判定与纯视觉表现。

**处理**

- 摄像机 LineTrace 负责真实命中、伤害和物理冲量。
- `FHitResult` 或射线终点作为 `TraceTarget`。
- `Bullet_BP` 从枪口飞向 `TraceTarget`，只负责曳光表现。
- 视觉子弹不再决定伤害结果。

**验证方法**

1. 关闭视觉子弹后，射击和伤害仍然正常。
2. 开启视觉子弹后，曳光轨迹朝向 LineTrace 目标。
3. 调整视觉子弹速度不会改变伤害发生时间。

**相关代码**

- `fpstrueWeaponComponent.cpp::FireLineTrace`
- `BP_Weapon`
- `Bullet_BP`

**复盘结论**

Gameplay 结果必须由稳定、可测试的 C++ 命中链负责，视觉对象只消费结果，不反向控制规则。

### FPS-005：血量逻辑重复

**状态：已修复**

**现象**

玩家、TargetDummy 和 Enemy 都需要血量、受伤和死亡逻辑，继续分别实现会产生重复代码。

**影响**

- 不同 Actor 的伤害规则容易不一致。
- UI、死亡和后续网络同步难以使用统一接口。

**根因**

早期使用测试靶快速验证射击，没有先抽象通用生命值边界。

**处理**

- 新增 `UfpstrueHealthComponent`。
- 组件监听 Owner 的 `OnTakeAnyDamage`。
- 统一维护 `MaxHealth`、`CurrentHealth` 和死亡判断。
- 通过 `OnHealthChanged`、`OnDeath` 向角色和蓝图广播结果。

**验证方法**

1. 使用同一武器攻击 TargetDummy、Enemy 和 Player。
2. 确认伤害均通过 HealthComponent 扣除。
3. 确认血量到零后只广播一次死亡事件。

**相关代码**

- `fpstrueHealthComponent.h`
- `fpstrueHealthComponent.cpp`

### FPS-006：死亡后残留逻辑

**状态：部分修复**

**现象**

- 玩家死亡后仍可能保持瞄准、射击或换弹状态。
- 敌人死亡后攻击伤害 Timer 仍可能回调。
- 死亡 Actor 仍可能移动或参与碰撞。

**影响**

- 尸体继续攻击或移动。
- 玩家死亡后仍然造成伤害。
- 延迟回调访问已经失效的 Gameplay 状态。

**根因**

死亡最初只改变血量，没有被视为需要统一终止其他系统的生命周期边界。

**处理**

- Player 死亡后进入 `Dead` 状态，停止瞄准、开火和移动。
- 清理玩家 Reload Timer。
- Enemy 死亡后清理攻击伤害与攻击结束 Timer。
- 禁用敌人移动和 Capsule 碰撞。
- 销毁使用 `SetLifeSpan`，避免立即删除造成表现链中断。

**验证方法**

1. 在敌人攻击前摇期间击杀敌人，确认玩家不再受到延迟伤害。
2. 玩家换弹期间死亡，确认弹药不会在死亡后增加。
3. 玩家死亡后确认无法继续移动和造成伤害。

**相关代码**

- `fpstrueCharacter.cpp::HandleDeath`
- `fpstrueEnemyCharacter.cpp::HandleDeath`

**剩余风险**

Game Over UI、输入模式切换和重新开始尚未接入，因此完整死亡闭环仍未完成。

### FPS-007：Reload Montage 无法稳定播放

**状态：部分修复**

**现象**

- C++ 已进入换弹状态，但第一人称手臂没有播放 Reload Montage。
- 普通换弹与空仓换弹表现可能不一致。

**影响**

- 规则层已经完成换弹，视觉层没有反馈。
- 容易误判为换弹输入或 Timer 没有执行。

**可能根因**

- Montage 播放在错误的 SkeletalMesh 或 AnimInstance。
- Animation Blueprint 没有配置对应 Slot。
- 蓝图监听了错误的 Weapon 事件或组件实例。

**已做处理**

- 将问题定位到 Mesh、AnimInstance 和 Slot 链路。
- 区分普通换弹与空仓换弹事件。
- WeaponComponent 广播换弹开始和结束事件，由蓝图负责表现。

**待验证**

1. 检查第一人称手臂实际使用的 AnimInstance。
2. 检查 Montage Slot 与 AnimGraph Slot 名称一致。
3. 分别测试普通换弹、空仓换弹和中途死亡。

### FPS-008：敌人血痕贴花不显示

**状态：已修复**

**现象**

- LineTrace 能命中敌人。
- `Cast To enemy_BP` 成功。
- 命中声音和粒子正常。
- 墙体弹孔能够显示。
- 敌人 `CharacterMesh0` 上没有血痕贴花。

**排除过程**

- 通过日志确认命中事件已经触发。
- 打印 `Hit Actor`，确认命中敌人。
- 打印 `Hit Component`，确认命中 `CharacterMesh0`，不是 Capsule。
- 确认 Mesh 已开启 `Receives Decals`。
- 使用墙面弹孔验证 Decal Material 和 Spawn 节点本身有效。

**根因**

敌人 Mesh 的父材质 `Decal Response` 为 `None`。`Receives Decals` 只是 Mesh 层面的接收开关，材质仍然必须响应贴花。

**处理**

- 将敌人材质的 `Decal Response` 改为 `Color` 或 `Color Normal Roughness`。
- 使用 `Spawn Decal Attached`。
- 将贴花附着到 `Hit Component`，位置使用 Impact Point，方向来自 Impact Normal。
- 设置有限 Life Span，避免贴花无限累积。

**验证方法**

1. 射击静止敌人，确认血痕出现。
2. 射击后移动或旋转敌人，确认血痕随 SkeletalMesh 移动。
3. 射击墙面，确认仍使用普通弹孔分支。
4. 等待 Life Span，确认贴花自动消失。

**复盘结论**

排查 UE 贴花时必须逐层检查：事件、Actor、Component、Mesh 接收开关、材质响应和附着方式，不能只看 Spawn 节点是否执行。

### FPS-009：敌人直线追逐

**状态：技术债**

**现象**

- 敌人通过 `AddMovementInput` 朝玩家当前位置移动。
- 遇到墙体或障碍物时不会主动绕路。
- Idle、Chase、Attack 和 Dead 主要由距离判断与布尔值隐式表达。

**影响**

- 场景稍复杂就会出现敌人卡墙。
- 状态切换难以观察和扩展。
- 不适合作为正式多人或复杂 AI 基础。

**原因**

当前 EnemyCharacter 是为了尽快验证玩家受伤和敌人死亡链而实现的原型。

**后续方案**

- 第一阶段改为显式 FSM。
- 当前项目允许继续保留直线追逐，避免依赖外部动画和复杂 AI 资产。
- 只有场景确实需要绕障时，再接入 AIController 与 NavMesh。

**相关代码**

- `fpstrueEnemyCharacter.cpp::UpdateEnemy`
- `fpstrueEnemyCharacter.cpp::MoveTowardTarget`

### FPS-010：UE DDC 没有可写节点

**状态：临时绕过**

**错误信息**

```text
Unable to use default cache graph 'InstalledDerivedDataBackendGraph'
because there are no writable nodes available.
```

**现象**

- 仅启动 Unreal Editor，不打开项目也会 Fatal Error。
- Zen Server 能够启动并监听端口，但 UE 无法访问服务。
- 日志反复出现无法访问 `[::1]:8558`。

**已确认信息**

- `E:\ueprojrct\ddc` 可写。
- Zen 数据目录位于 E 盘，不是磁盘空间不足。
- Zen 进程能够启动。
- 系统 IPv6 Loopback 访问异常，UE 无法连接 Zen HTTP 服务。
- Installed DDC Graph 在 Zen 失效后没有其他可写节点。

**临时处理**

使用文件缓存回退图启动：

```text
UnrealEditor.exe "E:\ueprojrct\fpstrue\fpstrue.uproject" -ddc=InstalledNoZenLocalFallback
```

日志确认：

```text
Local: Using data cache path E:/ueprojrct/ddc: Writable
ZenShared: Disabled because Host is set to 'None'
```

**验证结果**

- Unreal Editor 能进入 `fpstrue` 工程初始化。
- DDC Fatal Error 不再出现。
- 首次切换缓存图需要重新编译部分 Shader，启动时间明显增加。

**剩余风险**

- Zen 与 IPv6 Loopback 的底层问题尚未永久修复。
- 从普通项目浏览器或不带参数的快捷方式启动，仍可能重新使用默认 Zen Graph。
- 当前必须保留 NoZen 启动方式或创建明确的项目启动入口。

### FPS-011：打开错误工程副本并重复编译

**状态：已修复**

**现象**

- 项目浏览器打开了 `E:\ueprojrct\fpstrue_safe1\fpstrue.uproject`，不是当前主工程。
- 同时存在两组 UnrealEditor 与 ShaderCompileWorker。
- 两个工程各启动 12 个 Shader Worker，共计 24 个，导致首次启动明显变慢。

**根因**

- Recent Projects 中保留了备份副本。
- 从项目浏览器选择工程时没有核对绝对路径。

**处理**

- 核对 UnrealEditor 进程 CommandLine。
- 正常关闭 `fpstrue_safe1` 对应进程。
- 使用正确的主工程绝对路径直接启动。

**验证方法**

- 进程命令行指向 `E:\ueprojrct\fpstrue\fpstrue.uproject`。
- 编辑器窗口标题显示 `fpstrue`。
- 只保留一组属于主工程的 Shader Worker。

**复盘结论**

备份应该使用日期化压缩包或只读归档，不应长期保留多个名称相近、可以直接启动的活跃工程副本。

### FPS-012：构建产物与未跟踪资源混杂

**状态：待处理**

**现象**

- `Binaries`、`Intermediate`、UHT 生成文件和缓存文件出现在工作区改动中。
- `Content/UI`、`Content/enemy/Demo` 和部分项目文档未被 Git 跟踪。
- 源码时间与已有 Editor DLL 时间曾不一致，容易加载旧模块。

**影响**

- 无法快速判断哪些是人工修改，哪些是 UE 自动生成。
- 只提交 Source 可能遗漏实际依赖的蓝图或 Content。
- 复制或恢复工程时可能出现“本机可用，其他目录不可用”。

**待处理方案**

- 明确项目需要版本控制的 `Source / Config / Content / Plugins / .uproject`。
- 检查 `.gitignore` 是否正确排除可再生成目录。
- 对 `.uasset`、`.umap` 和大型二进制资源继续使用 Git LFS。
- 在提交前列出所有未跟踪资源，确认哪些是项目依赖，哪些只是未使用素材。
- 完成一次干净目录恢复和重新编译测试。

**安全要求**

在恢复验证完成前，不删除现有工程、备份副本或缓存目录。清理必须建立在明确路径、可恢复备份和验证结果之上。

## 5. 当前未闭环事项

以下内容是已确认缺口，不应在简历中写成已经完成：

- Reload 与 Empty Reload 动画完整回归。
- HUD：准星、血量、当前/备用弹药。
- 玩家死亡后的 Game Over 与输入模式切换。
- 胜利、失败和重新开始。
- 显式 Enemy FSM。
- NoZen 启动方式的固定入口。
- Git 工作区与必要 Content 的封版整理。

## 6. 面试复盘优先级

### 第一优先：血痕贴花不显示

价值：展示 UE 命中链、组件、材质与贴花系统的分层排查能力。

讲述顺序：

```text
现象
→ 验证事件触发
→ 验证 Hit Actor
→ 验证 Hit Component
→ 验证 Receives Decals
→ 定位 Decal Response
→ 改为 Spawn Decal Attached
→ 验证随敌人移动和生命周期
```

### 第二优先：换弹与射击状态冲突

价值：展示状态互斥、统一入口、Timer 生命周期和 Gameplay 规则设计。

讲述重点：

- 为什么不能只依赖多个布尔值。
- 为什么换弹、死亡、空仓必须经过统一检查。
- 为什么死亡时要主动清理异步 Timer。

### 第三优先：LineTrace 与视觉子弹分离

价值：展示 Gameplay 判定与表现层解耦。

讲述重点：

- 摄像机射线负责玩家瞄准结果。
- 枪口曳光只负责视觉连贯性。
- 视觉对象的速度和帧率不能影响真实伤害。

### 环境补充：DDC/Zen 启动故障

价值：展示日志分析、进程检查、路径权限和最小化绕过方案。

注意：面试中应明确说明当前是 NoZen 文件缓存绕过方案，不能声称已经永久修复 IPv6/Zen 根因。

## 7. 后续记录模板

新问题统一使用以下格式追加：

```text
### FPS-XXX：问题名称

状态：
首次发现日期：
最后验证日期：

现象：
影响：
复现步骤：
排除过程：
根因：
处理：
验证方法：
剩余风险：
相关文件或提交：
```
