# 游戏开发面试基础同步学习计划

最后更新：2026-07-21

## 1. 文档目的

本文档将游戏开发面试中的 C++、STL、数据结构、工程基础与 `fpstrue` 项目开发绑定，避免项目和八股分开学习。

学习目标不是背出固定答案，而是达到四层掌握：

```text
能定义
→ 能解释原理
→ 能结合项目
→ 能写代码或验证
```

## 2. 附件内容范围

附件正文实际覆盖：

1. C++：多态、对象布局、内存、类型转换、智能指针、关键字、拷贝与移动、内联和宏。
2. STL 与数据结构：容器实现、复杂度、Allocator、排序、TopK、哈希、栈、队列和链表。
3. 工程问题：编译链接、静态库与动态库、数据复制、对象池、单例和工厂。

附件开头提到了操作系统、计算机组成和网络，但当前文件正文没有包含这些章节。它们需要单独补充，不能认为已经覆盖。

## 3. 阅读附件时必须纠正的内容

这篇材料适合作为问题清单，不应把全部答案原样背诵。

### 3.1 C++ 与内存

- 堆和栈大小不是固定的 `4 GB` 与 `1 MB`，实际限制由平台、进程配置和运行环境决定。
- `new/delete` 描述的是 C++ 自由存储接口，常由堆实现，但概念不应完全等同。
- `new[]` 必须与 `delete[]` 配对，`new` 必须与 `delete` 配对；不匹配属于未定义行为，不能因为元素是基础类型就混用。
- `weak_ptr` 解决的是 `shared_ptr` 循环所有权导致的资源无法释放，不应称为线程“死锁”。
- `shared_ptr` 的控制块支持多个不同 `shared_ptr` 实例并发共享所有权，但不自动保护被指向对象，也不保证同一个 `shared_ptr` 实例的无锁并发读写安全。
- `volatile` 不能替代原子变量、锁或内存同步原语。
- C++11 只保证函数局部静态变量的初始化线程安全，不保证单例对象后续成员操作线程安全。
- `delete this` 风险很高，不应作为常规设计手段。

### 3.2 STL 与容器

- `vector` 的增长倍率由标准库实现决定，不能回答成标准强制两倍扩容。
- `map/set` 通常使用节点式红黑树，不是“基于链表”，节点存在指针和分配开销。
- `list` 的单点插入删除是 O(1)，但查找位置、内存分配和缓存不友好可能使实际性能较差。
- 哈希桶数量使用质数是一种实现策略，不是所有 `unordered_map` 的标准要求。
- `vector::clear()` 不保证释放 capacity。
- `shrink_to_fit()` 是非强制请求，标准不保证一定缩容。
- 与空临时 `vector` 交换通常可以释放容量，但需要考虑迭代器失效和分配器行为。
- `emplace_back` 不保证永远比 `push_back` 快；传入已经构造好的对象时仍可能发生移动。

### 3.3 工程问题

- `memcpy` 适合按字节复制可安全按位复制的数据，不能随意复制含虚函数、资源所有权或非平凡成员的 C++ 对象。
- `std::copy` 要求目标范围已经具备足够空间，或使用正确的插入迭代器。
- 单例和工厂不是看到“全局唯一”或“创建对象”就必须使用，先判断依赖、生命周期和测试成本。
- 对象池只有在对象频繁创建销毁且测量显示分配成本明显时才有价值。

## 4. 八股与 FPS 项目的映射

| FPS 模块 | 同步学习内容 | 项目中的验证点 |
| --- | --- | --- |
| Character / Enemy 继承 | 多态、虚函数、override、对象布局 | `ACharacter` 生命周期函数和虚调用 |
| `Cast<AfpstrueCharacter>` | `static_cast`、`dynamic_cast`、UE Cast 与反射 | 拾取与敌人目标获取 |
| HealthComponent | RAII、生命周期、观察者模式、组合优于继承 | Delegate 绑定与死亡广播 |
| WeaponComponent | 组合、职责边界、接口与事件 | Character 与 Weapon 解耦 |
| `FHitResult` 与参数 | 值语义、引用、const、拷贝与移动 | 命中数据如何传递给事件 |
| Timer | 对象生命周期、悬空回调、异步边界 | 死亡时清理攻击和换弹 Timer |
| Dynamic Delegate | 函数指针、多态、观察者模式 | C++ 广播，蓝图消费 |
| TArray / vector | 连续内存、扩容、缓存局部性、迭代器失效 | 敌人列表、贴花列表、对象集合 |
| TMap / unordered_map | 哈希、冲突、复杂度 | 表面类型或配置表 |
| Object Pool | 分配器、碎片、生命周期、Reset | 后续可视化节点或临时效果 |
| UE Module / DLL | 预处理、编译、汇编、链接、动态库 | UBT、UHT、Editor Module |
| 多人网络 | 线程/进程、TCP/UDP、序列化、同步 | 第二阶段单独学习 |

## 5. 每次开发的同步教学流程

以后每实现一个功能，按以下顺序学习：

### 开发前：三个概念问题

先回答该模块涉及的三个基础问题。例如实现 HealthComponent 前：

1. 继承与组合有什么区别？
2. Delegate 和虚函数回调有什么区别？
3. UObject 生命周期为什么不能用普通 `delete` 管理？

### 开发中：定位项目证据

在代码中指出：

- 数据由谁拥有。
- 对象由谁创建。
- 谁触发函数。
- 谁消费结果。
- 生命周期在哪里结束。

### 开发后：最小实验

使用独立小程序或安全分支验证一个原理：

- 打印构造、拷贝、移动和析构顺序。
- 观察 `vector` 扩容后的 capacity 和地址变化。
- 构造 `shared_ptr` 循环引用，再用 `weak_ptr` 打破。
- 输出结构体 `sizeof` 与成员偏移，验证内存对齐。
- 故意遗漏 Timer 清理，观察死亡后的回调。

### 当天结束：口述验收

每个主题必须能够完成：

```text
30 秒定义
→ 2 分钟原理
→ 2 分钟项目结合
→ 1 个追问题
```

## 6. 第一阶段必学八股

这些内容直接对应当前 FPS，应优先掌握。

### P0-1：多态与虚函数

必须会：

- 编译时多态与运行时多态。
- 虚函数、虚表、虚表指针的基本思想。
- `override` 的作用。
- 为什么多态基类通常需要虚析构函数。
- UE 中 `BeginPlay`、`Tick` 等虚函数如何被引擎调用。

项目结合：

- `AfpstrueCharacter::Tick` 覆盖 `ACharacter::Tick`。
- `AfpstrueEnemyCharacter::BeginPlay` 覆盖父类生命周期函数。
- 当前蓝图事件与 C++ 虚函数不是同一种机制。

### P0-2：对象生命周期与内存

必须会：

- 栈对象、动态对象、静态存储期。
- 构造与析构顺序。
- RAII。
- 内存泄漏、悬空指针和重复释放。
- UObject GC 与普通 C++ 所有权的差异。

项目结合：

- HealthComponent 由 `CreateDefaultSubobject` 创建。
- Character 保存带 `UPROPERTY` 的 UObject 引用。
- Actor 使用 `Destroy` 或 `SetLifeSpan`，不是 `delete`。
- Timer 回调必须考虑 Owner 死亡。

### P0-3：类型转换

必须会：

- `static_cast`、`dynamic_cast`、`const_cast`、`reinterpret_cast`。
- 向上转换和向下转换。
- RTTI 与类型安全。
- UE `Cast<T>` 依赖 UObject 反射系统。

项目结合：

- PickUpComponent 将 OtherActor 转为 Player Character。
- Enemy 获取玩家后进行 UE Cast。
- 不要把 UE UObject 指针随意使用 `reinterpret_cast`。

### P0-4：智能指针与 UE 指针

必须会：

- `unique_ptr`、`shared_ptr`、`weak_ptr`。
- 所有权、引用计数和循环所有权。
- 拷贝与移动对智能指针的影响。
- 智能指针不自动保证被指向对象线程安全。

项目结合：

- UObject 通常不使用 `std::shared_ptr` 管理。
- UE 中还需要区分裸 UObject 指针、`TObjectPtr`、`TWeakObjectPtr`、`TSharedPtr`。
- `TSharedPtr` 主要用于非 UObject 类型和工具代码。

### P0-5：拷贝、移动与引用

必须会：

- 左值、右值和引用折叠的基础概念。
- 拷贝构造、移动构造和赋值。
- 深拷贝与浅拷贝。
- `const T&`、`T&&` 和按值传参的选择。

项目结合：

- `FHitResult` 是否按值、引用或 const 引用传递。
- Delegate 参数过多时，是否封装为数据结构。
- UObject 不应按照普通值对象随意拷贝。

### P0-6：容器与复杂度

必须会：

- vector/TArray 连续内存和扩容。
- list 的节点与缓存问题。
- map/set 与 unordered_map 的实现差异。
- 查找、插入、删除的平均与最坏复杂度。
- 迭代器和引用失效。

项目结合：

- 敌人列表优先考虑 TArray，而不是因为频繁删除就直接选链表。
- 配置表可以使用 TMap，但要说明键类型与查找需求。
- 删除死亡敌人时考虑 Remove、RemoveSwap 与顺序要求。

### P0-7：编译与链接

必须会：

```text
预处理
→ 编译
→ 汇编
→ 链接
```

还要理解：

- 声明与定义。
- 头文件重复包含与 include guard。
- 静态库和动态库。
- 链接错误与编译错误的区别。
- ABI 与导出宏的基础概念。

项目结合：

- `FPSTRUE_API` 的作用。
- `.Build.cs` 声明模块依赖。
- UHT 生成反射代码。
- UBT 负责编译 UE Target。
- 源码更新但 Editor DLL 未重建时可能加载旧模块。

## 7. 第二阶段八股

### P1-1：STL 与 Allocator

- Allocator 的职责。
- 小对象频繁分配的问题。
- 内部碎片与外部碎片。
- Pool、Free List 和 Slab 的基本思想。
- 标准库实现细节不能当作标准保证。

### P1-2：排序、TopK 与堆

- 快速排序、归并排序和堆排序。
- 平均、最坏复杂度与稳定性。
- TopK 的小根堆方案。
- `std::sort` 采用实现相关的混合策略，常见实现是 introsort。

项目结合：

- A* Open Set 使用优先队列。
- 敌人威胁排序或最近目标选择。

### P1-3：哈希表

- 哈希函数。
- 拉链法与开放寻址。
- Load Factor。
- Rehash。
- 平均 O(1) 与最坏 O(n)。

项目结合：

- Surface Type 到反馈配置的映射。
- Entity ID 或节点状态查询。

### P1-4：设计模式

优先理解项目中真实出现的模式：

- Component：组合能力。
- Observer：Delegate。
- State：Character 与 Enemy 状态。
- Factory：统一创建对象时再使用。
- Pool：频繁复用对象时再使用。

不要为了面试强行加入 Singleton、Factory 或 Pool。

## 8. 第三阶段必须补充的基础

附件未覆盖，但游戏客户端和引擎岗位通常会考察。

### 操作系统

- 进程与线程。
- 用户态与内核态。
- 虚拟内存、页表和缺页。
- 栈、堆与内存映射。
- 互斥锁、读写锁、自旋锁和条件变量。
- 死锁四条件。
- 原子操作与内存序基础。

### 计算机组成

- Cache 层级与局部性。
- Cache Line 与 False Sharing。
- 指令流水线和分支预测。
- SIMD 基础。
- CPU 与 GPU 架构差异。

### 网络

- TCP 与 UDP。
- 三次握手与四次挥手。
- 可靠传输、拥塞控制和重传。
- 延迟、抖动和丢包。
- 游戏同步、快照、插值和预测。
- UE RPC、Replication 与底层网络概念的关系。

### 图形学

- GPU 渲染管线。
- 坐标空间与矩阵变换。
- 深度测试、模板测试和混合。
- 光照模型与 BRDF 基础。
- Shadow Mapping、Bias 与 PCF。
- Forward 与 Deferred。
- Shader、Render Target、GBuffer。

## 9. 学习节奏

每天不追求覆盖大量题目，固定完成：

```text
20 分钟：概念和正确答案
20 分钟：结合当前项目找代码证据
20 分钟：小实验或手写
10 分钟：闭卷口述
```

每周完成一次混合复习：

- 10 道 C++。
- 5 道 STL/数据结构。
- 5 道 UE Gameplay Framework。
- 3 道项目调用链。
- 2 道 Bug 复盘。

## 10. 与当前开发顺序同步

| 当前开发内容 | 同步八股 |
| --- | --- |
| HealthComponent 复习 | 生命周期、RAII、组合、Delegate |
| Weapon Fire/Trace | 引用、const、对象职责、数据传递 |
| Character Reload/Death | 状态模式、Timer 生命周期、边界条件 |
| HUD 接入 | Observer、MVC/MVVM 基础、事件驱动 |
| Enemy FSM | 多态、状态模式、优先队列预备 |
| GameMode 闭环 | Gameplay Framework、所有权与生命周期 |
| 多人网络 | 进程/线程、TCP/UDP、序列化、Replication |
| Object Pool | Allocator、Free List、碎片、缓存局部性 |
| OpenGL Shadow | 图形管线、矩阵、深度、采样 |
| UE Global Shader | 编译链接、线程模型、GPU 资源生命周期 |

## 11. 掌握判定

每个问题分为四级：

| 等级 | 标准 |
| --- | --- |
| L1 | 能给出基本定义 |
| L2 | 能解释底层原理和常见陷阱 |
| L3 | 能结合 `fpstrue` 指出代码位置 |
| L4 | 能写实验、修改需求并回答追问 |

第一批面试前要求：

- P0 内容至少达到 L2。
- 与 FPS 直接相关的内容达到 L3。
- Health、射击、换弹、死亡和 Delegate 达到 L4。

## 12. 回答模板

面试回答不要只背定义，使用以下结构：

```text
一句话定义
→ 核心原理
→ 优点与代价
→ 项目中的使用位置
→ 一个边界或 Bug
```

示例：为什么使用 Delegate？

```text
Delegate 是一种事件通知机制。
在项目中 HealthComponent 不直接依赖 HUD 或 Character 蓝图，
而是在血量变化和死亡时广播事件。
这样规则层与表现层解耦，但代价是调用关系更隐式，
所以需要管理绑定生命周期并记录事件消费者。
我们曾在死亡流程中检查并清理 Timer，避免死亡后继续触发异步逻辑。
```

## 13. 复习资料边界

- 附件用于收集高频问题，不作为唯一正确答案来源。
- C++ 语言规则以当前标准和可靠参考为准。
- STL 的具体增长倍率、树结构和 Allocator 策略要区分“标准保证”与“实现细节”。
- UE 问题必须回到当前引擎版本、项目代码和运行验证。
- 不确定的答案不强行背诵，应标记并通过代码实验或官方资料确认。
