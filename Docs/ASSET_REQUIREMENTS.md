# 资源依赖与蓝图契约

## 可以直接审阅

UE5.5 C++ 规则、模块/构建配置、测试源码、采集方法和白名单证据。启用的 GameplayAbilities、AnimationSharing、SignificanceManager 等是 UE 插件，不是项目自研框架。

## 运行前必须恢复

- 默认关卡及其导航、碰撞和 WorldSettings；唯一玩法类为 AfpstruePvpGameMode，不再恢复旧单机波次规则。
- 玩家/武器/敌人的 Mesh、AnimBP、Montage、音效及 HUD 蓝图；资源须具备独立授权。
- `/Game/FirstPerson/AI/BT_FPEnemy`、`BB_FPEnemy` 与当前 C++ Task/Key 契约；布局可从 `Tools/Gameplay/` 的生成/验证代码审阅。
- 武器/近战 Data Asset 与角色实际引用。共享参数和运行态不能混存；六个原生命中盒不需要专用凸包资源。
- 画质、静态样条烘焙等历史性能场景所需资源。缺失时不能把另一张空图的帧率代替原结果。

资产路径出现在源码或配置中不意味着该资源包含在 main。项目自己的蓝图也可能引用受授权约束的动画/模型，因此没有按文件名推断可再分发。完整资产集成、画面和性能测试只能在恢复一致资源后执行。

工具的 Create/Migrate/Bake 会改变资产，不能为了让测试通过而在未知资源集上盲目运行。先检查只读契约，再生成/迁移、编译并核对真实消费者。
