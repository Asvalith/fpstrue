// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AnimationSharingTypes.h"
#include "fpstrueEnemyPerformance.generated.h"

// 渲染档位只描述表现成本，不替代敌人的追击、攻击等玩法状态。
UENUM(BlueprintType)
enum class EFPEnemyRenderSignificanceTier : uint8
{
	Full,
	Reduced,
	Background
};

/**
 * 敌人全局渲染显著性策略，单机由 GameMode、多人由本端 Coordinator 配置并校验和消费。
 *
 * 四个权重会在运行时归一化，不要求手工相加为 1。Gameplay Significance 仍按玩法目标距离和交互状态独立计算，
 * 本结构只控制渲染评分、档位、LOD、动画、阴影和硬件光追预算。
 */
USTRUCT(BlueprintType)
struct FPSTRUE_API FFPEnemyRenderSignificancePolicy
{
	GENERATED_BODY()

	//功能开关(消融实验使用)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableRenderTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableSkeletalLOD = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableAnimationTickTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableShadowBudget = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableRayTracingBudget = true;


	//评分输入：评分计算中使用的视锥扩展、最近可见宽限、屏占比满分半径和距离范围
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ExpandedFrustumMargin = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.0"))
	float RecentFrustumGraceSeconds = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.001"))
	float ScreenRadiusForFullScore = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.0"))
	float NearDistance = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "1.0"))
	float FarDistance = 10000.0f;


	//评分权重：主视锥、屏占比、最近可见、距离。归一化后总和为 1。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float FrustumWeight = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float ScreenCoverageWeight = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float RecentFrustumWeight = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float DistanceWeight = 0.10f;

	// 战斗只恢复必要动画和 LOD，不提高渲染评分，也不额外分配阴影或光追名额。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Animation Protection", meta = (ClampMin = "0.0"))
	float CombatPriorityGraceSeconds = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FullEnterThreshold = 0.70f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FullExitThreshold = 0.60f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReducedEnterThreshold = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReducedExitThreshold = 0.20f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0"))
	float DemotionDelaySeconds = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0"))
	float MinimumTierHoldSeconds = 0.50f;


	// 预算：Full Render、阴影和硬件光追的最大敌人数量，以及阴影和光追的最大距离
	// 只给同一视锥层级内的上轮名额持有者加分；三种预算各自记录，不改变原始评分或自然档位。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BudgetRetentionMargin = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	// 每本地视图的画面预算，容纳围攻不同玩家的候选；不授予或限制每目标的服务器攻击名额。
	int32 MaxFullRenderEnemies = 20;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	int32 MaxShadowCastingEnemies = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0.0"))
	float ShadowMaxDistance = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	int32 MaxRayTracingEnemies = 12;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0.0"))
	float RayTracingMaxDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "0"))
	int32 FullMinLOD = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "0"))
	int32 ReducedMinLOD = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "0"))
	int32 BackgroundMinLOD = 2;
};

// Coordinator 每轮从玩家相机生成一次，所有敌人通过 const 引用读取同一份值类型快照。
struct FFPEnemyRenderViewContext
{
	//相机位置、旋转、水平FOV、宽高比和采样时刻
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	float HorizontalFOVDegrees = 90.0f;
	float AspectRatio = 16.0f / 9.0f;
	float TimeSeconds = 0.0f;
};

// 纯渲染采样结果：不得加入攻击、受击、威胁等玩法状态。
struct FFPEnemyRenderSignificanceSample
{
	//一个敌人一份的采样结果
	float Score = 0.0f;
	float FrustumFactor = 0.0f;
	float ScreenCoverageFactor = 0.0f;
	float RecentFrustumFactor = 0.0f;
	float DistanceFactor = 0.0f;
	float Distance = MAX_flt;
	bool bInPrimaryFrustum = false;
	bool bInExpandedFrustum = false;
};

/**
 * 本轮相机和策略的纯值派生数据。Coordinator 只构造一次，所有敌人复用相机坐标轴、投影参数和评分常量。
 * 不保存 Actor/组件引用，也不缓存跨轮结果；玩家转镜、FOV 或策略改变后，下一轮会重新构造。
 * EvaluateBounds 保留原有球形 Bounds 评分公式，不增加距离早退、不改变 Top-K 优先级或玩法档位。
 */
struct FPSTRUE_API FFPEnemyRenderScoringContext
{
	// 由本轮相机及已校验策略预计算公共投影参数，后续每个敌人只提供自己的 Bounds。
	FFPEnemyRenderScoringContext(const FFPEnemyRenderViewContext& ViewContext, const FFPEnemyRenderSignificancePolicy& Policy);

	// LastPrimaryFrustumTime 是调用者独占的历史值；纯数学计算不访问 World 或可变 UObject。
	FFPEnemyRenderSignificanceSample EvaluateBounds(const FVector& BoundsOrigin, float BoundsRadius,
		float& LastPrimaryFrustumTime) const;

private:
	FVector ViewLocation;
	FVector ForwardAxis;
	FVector RightAxis;
	FVector UpAxis;
	float TimeSeconds;
	float HorizontalTangent;
	float VerticalTangent;
	float ExpandedFrustumMargin;
	float RecentGraceSeconds;
	float ScreenRadiusForFullScore;
	float NearDistance;
	float DistanceRange;
	float FrustumWeight;
	float ScreenCoverageWeight;
	float RecentFrustumWeight;
	float DistanceWeight;
	float WeightSum;
};

/**
 * 单轮渲染预算使用的不可变排序键。
 *
 * 排序优先级依次为主视锥、扩展视锥、评分、TieBreakId。TieBreakId 只在其余字段完全相同时生效，
 * 用于保证同一批候选的结果确定。若以后需要把相近评分视为同档，应在生成键之前显式量化评分，
 * 不能在比较器中使用 IsNearlyEqual；“近似相等”不具备传递性，会破坏排序算法要求的严格弱序。
 */

struct FFPEnemyRenderPriorityKey
{
	float Score = 0.0f;
	uint32 TieBreakId = 0;
	bool bInPrimaryFrustum = false;
	bool bInExpandedFrustum = false;
};

/** 按渲染重要性从高到低排列候选；同一视锥层级内，无效评分落到有效评分之后。 */
struct FFPEnemyRenderPriorityLess
{
	// 纯值比较，不读取 Actor；无效分数排到末尾，最终用对象编号打破完全相同的优先级。
	bool operator()(const FFPEnemyRenderPriorityKey& Left, const FFPEnemyRenderPriorityKey& Right) const
	{
		if (Left.bInPrimaryFrustum != Right.bInPrimaryFrustum)
		{
			return Left.bInPrimaryFrustum;
		}
		if (Left.bInExpandedFrustum != Right.bInExpandedFrustum)
		{
			return Left.bInExpandedFrustum;
		}
		const float LeftScore = FMath::IsFinite(Left.Score) ? FMath::Clamp(Left.Score, 0.0f, 1.0f) : -1.0f;
		const float RightScore = FMath::IsFinite(Right.Score) ? FMath::Clamp(Right.Score, 0.0f, 1.0f) : -1.0f;
		if (LeftScore != RightScore)
		{
			return LeftScore > RightScore;
		}
		return Left.TieBreakId < Right.TieBreakId;
	}
};

/**
 * 在进入堆之前构造本预算的临时键，保持比较器严格弱序，不在比较中使用近似相等。
 * 共同缩放后分数仍在 [0,1]，避免高分持有者加成被比较器的 Clamp 截掉；原始 Key 不被改写。
 * 主/扩展视锥优先级保持原样，新视野中的对象不会被旧名额的分数加成挡住。
 */
FORCEINLINE FFPEnemyRenderPriorityKey FPEnemyRenderBudgetPriorityKey(
	const FFPEnemyRenderPriorityKey& Key, bool bWasHolder, float RetentionMargin)
{
	FFPEnemyRenderPriorityKey BudgetKey = Key;
	const float SafeMargin = FMath::IsFinite(RetentionMargin) ? FMath::Clamp(RetentionMargin, 0.0f, 1.0f) : 0.0f;
	if (SafeMargin > 0.0f && FMath::IsFinite(Key.Score))
	{
		BudgetKey.Score = (FMath::Clamp(Key.Score, 0.0f, 1.0f) + (bWasHolder ? SafeMargin : 0.0f)) / (1.0f + SafeMargin);
	}
	return BudgetKey;
}

/**
 * 有界 Top-K 堆中的轻量条目。CandidateIndex 指回本轮连续候选数组，排序键按值保存，
 * 避免堆比较期间再访问 Actor、组件或分散内存。
 */
struct FFPEnemyRenderTopKEntry
{
	int32 CandidateIndex = INDEX_NONE;
	FFPEnemyRenderPriorityKey PriorityKey;
};

/** 让堆顶始终保存当前 Top-K 中优先级最低的条目，便于新候选在 O(log K) 内替换它。 */
struct FFPEnemyRenderTopKWorstFirst
{
	// 反转高优先级比较，使堆顶成为最容易被新候选替换的条目。
	bool operator()(const FFPEnemyRenderTopKEntry& Left, const FFPEnemyRenderTopKEntry& Right) const
	{
		return FFPEnemyRenderPriorityLess{}(Right.PriorityKey, Left.PriorityKey);
	}
};

/**
 * 把候选并入一个容量为 MaxCount 的 Top-K 堆。
 *
 * 扫描 N 个候选时总复杂度为 O(N log K)，额外空间为 O(K)。输出堆不保证有序，
 * 但其中保存的集合与按 FFPEnemyRenderPriorityLess 完整排序后取前 K 个完全一致。
 */
template <typename AllocatorType>
FORCEINLINE void FPEnemyRenderTopKInsert(TArray<FFPEnemyRenderTopKEntry, AllocatorType>& TopKHeap, int32 MaxCount,
	int32 CandidateIndex, const FFPEnemyRenderPriorityKey& PriorityKey)
{
	if (MaxCount <= 0 || CandidateIndex == INDEX_NONE)
	{
		return;
	}

	const FFPEnemyRenderTopKEntry Entry{CandidateIndex, PriorityKey};
	const FFPEnemyRenderTopKWorstFirst WorstFirst;
	// 名额未满直接入堆；满额后只替换比当前最差条目更重要的候选。
	if (TopKHeap.Num() < MaxCount)
	{
		TopKHeap.HeapPush(Entry, WorstFirst);
		return;
	}

	if (FFPEnemyRenderPriorityLess{}(Entry.PriorityKey, TopKHeap[0].PriorityKey))
	{
		TopKHeap.HeapPopDiscard(WorstFirst, EAllowShrinking::No);
		TopKHeap.HeapPush(Entry, WorstFirst);
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.



class AfpstrueEnemyCharacter;
class UfpstrueEnemyAnimationSharingCoordinator;

/**
 * 显著性调度：GameState 启动 Timer → GT 采样快照 → 插件更新玩法评分 → 本端分配表现预算 → GT 应用。
 * 服务器玩法只看有效玩家位置及战斗保护；客户端相机只决定自己的 LOD、动画、阴影和 RT。
 * 候选、预算持有者和策略归本组件，不复制评分/名额，也不授予 AI 目标或攻击资格。
 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueEnemySignificanceCoordinator : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建由 Timer 驱动、无组件 Tick 的协调器。
	UfpstrueEnemySignificanceCoordinator();

	// ==================== 生命周期 ====================

	// 多人 GameState 在每个 World 启动本地协调器：服务器更新玩法，客户端只分配自己的表现预算。
	void StartMultiplayer(UfpstrueEnemyAnimationSharingCoordinator* InSharingCoordinator);
	// 停止集中更新 Timer；GameState 结束和组件销毁都会调用。
	void Stop();
	// 只读诊断用于自动化验证启动、发现副本和采样端职责，不参与玩法判断。
	bool IsRunning() const { return bRunning; }
	// 上轮实际发现的存活敌人数，不代表所有注册 Actor 数。
	int32 GetTrackedEnemyCount() const { return LastTrackedEnemyCount; }
	// 上轮构建的服务器玩法视点数；插件有效时才用于评分，不是相机视锥数量。
	int32 GetGameplayViewpointCount() const { return LastGameplayViewpointCount; }
	// 每次进入有效更新递增，便于验证 Timer 是否持续运行。
	uint32 GetUpdateSerial() const { return UpdateSerial; }
	// 只在采到有效本地相机并执行表现预算更新时递增。
	uint32 GetRenderUpdateSerial() const { return RenderUpdateSerial; }
	// 多人模式是否曾处理非空表现候选；只读启动诊断，不代表当前可见性。
	bool HasRenderedEnemies() const { return bReportedRenderUpdate; }

protected:
	// 组件退出时清理 Timer。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 多人协调器各端独立启停；下方 RenderPolicy 只控制本端表现，不复制某个客户端的相机或预算。
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Multiplayer")
	bool bEnableMultiplayerSignificance = true;
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Multiplayer", meta = (ClampMin = "0.1"))
	float MultiplayerUpdateInterval = 0.25f;
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Multiplayer")
	FFPEnemyRenderSignificancePolicy MultiplayerRenderPolicy;

private:
	friend class FFpstrueMultiplayerSignificanceCoordinatorTest;
	friend class FFpstrueMultiplayerClientPerformanceTest;
	friend class FFpstrueEnemyRenderBudgetStabilityTest;
	// 周期间复用容量；Actor 指针只在当前 Update 的同步采样/应用阶段使用。
	struct FEnemyRenderCandidate
	{
		AfpstrueEnemyCharacter* Enemy = nullptr;
		FFPEnemyRenderSignificanceSample Sample;
		FFPEnemyRenderPriorityKey PriorityKey;
		EFPEnemyRenderSignificanceTier NaturalTier = EFPEnemyRenderSignificanceTier::Background;
		EFPEnemyRenderSignificanceTier AssignedTier = EFPEnemyRenderSignificanceTier::Background;
		bool bCloseCombatProtection = false;
		bool bGameplayAnimationProtection = false;
		bool bShadowEligible = false;
		bool bRayTracingEligible = false;
		bool bShouldCastShadow = false;
		bool bShouldBeVisibleInRayTracing = false;
	};
	TArray<FEnemyRenderCandidate> CandidateBuffer;
	// 仅保存上轮实际名额的弱引用；每轮按当前资格重建，不能保活敌人或跨网络端共享。
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> FullBudgetHolders;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> ShadowBudgetHolders;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> RayTracingBudgetHolders;

	// ==================== 集中更新与策略校验 ====================

	// 一次完成 Gameplay 更新、Render 采样、Top-K 预算选择、组件应用和 CSV 统计。
	void Update();
	// 只消费本轮纯值样本与本预算历史，不写 Actor、Gameplay 档位或网络策略。
	void AllocatePresentationBudgets(int32& FullBudgetDowngradeCount, int32& ShadowBudgetRejectedCount,
									 int32& RayTracingBudgetRejectedCount);
	// 预算选完后统一应用并读回实际结果；统计不参与下一轮决策。
	void ApplyAndRecordCandidates(int32 FullBudgetDowngradeCount, int32 ShadowBudgetRejectedCount,
								  int32 RayTracingBudgetRejectedCount);
	// 修正权重、阈值、距离和名额的非法配置。
	void SanitizePolicy(FFPEnemyRenderSignificancePolicy& Policy);

	TWeakObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> AnimationSharingCoordinator;
	FFPEnemyRenderSignificancePolicy ActiveRenderPolicy;
	bool bRunning = false;
	bool bSharingStartAttempted = false;
	bool bReportedPopulatedUpdate = false;
	bool bReportedRenderUpdate = false;
	bool bReportedSharingFollowers = false;
	int32 LastTrackedEnemyCount = 0;
	int32 LastGameplayViewpointCount = 0;
	uint32 UpdateSerial = 0;
	uint32 RenderUpdateSerial = 0;
	FTimerHandle UpdateTimerHandle;
};

// Copyright Epic Games, Inc. All Rights Reserved.



class AfpstrueEnemyCharacter;
class UAnimationSharingManager;
class UAnimationSharingSetup;
class UAnimSequence;
class USkeleton;

/**
 * Animation Sharing 的原生状态处理器。
 * 共享层读取实际速度区分 Idle / Chase 表现状态，客户端不依赖不存在的 AIController；行为选择仍由行为树负责。攻击、受击和死亡会先退出共享系统，
 * 继续由敌人原有 AnimBP、Montage、Notify 和布料/物理链处理。
 */
UCLASS()
class FPSTRUE_API UfpstrueEnemyAnimationSharingStateProcessor final : public UAnimationSharingStateProcessor
{
	GENERATED_BODY()

public:
	// 根据实际速度把敌人映射到共享待机或跑步状态，不读取客户端的 AIController。
	virtual void ProcessActorState_Implementation(int32& OutState, AActor* InActor, uint8 CurrentState, uint8 OnDemandState,
												  bool& bShouldProcess) override;
	// 告诉插件共享状态复用 EFPEnemyAIState 枚举，而不另建一套动画状态编号。
	virtual UEnum* GetAnimationStateEnum_Implementation() override;
};

/**
 * Animation Sharing 的唯一项目侧登记入口；World Manager 生命周期仍由引擎管理。
 * Render Significance 决定谁可进入共享池以及共享 Leader 是否值得更新，
 * Gameplay Significance 不读取视锥，也不依赖本组件。
 * 单机/客户端创建兼容池后按资格增删 Follower；联网权威端不使用共享姿态代替判定姿态。
 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueEnemyAnimationSharingCoordinator final : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建不参与逐帧 Tick 的共享动画协调器。
	UfpstrueEnemyAnimationSharingCoordinator();

	// ==================== 生命周期与 Follower 接口 ====================

	// GameMode 在首个敌人生成前调用，创建本 World 唯一共享池。
	void Start(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass);
	// GameMode 结束时注销全部 Follower 并停止接纳敌人。
	void Stop();
	// Render Significance 应用档位后调用，决定敌人加入、保留或退出共享池。
	void RefreshEnemyRegistration(AfpstrueEnemyCharacter* Enemy);
	// 攻击、受击和死亡前立即退出共享池，恢复独立 AnimBP 与 LOD 控制。
	void SuspendEnemy(AfpstrueEnemyCharacter* Enemy);

	// CSV Benchmark 读取当前 Follower 数量。
	int32 GetRegisteredEnemyCount() const { return RegisteredActorHandles.Num(); }

protected:
	// World 退出时注销 Follower 并释放 Manager/Setup/Skeleton 强引用，不手动销毁引擎 World 池。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ==================== 共享池配置 ====================

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing")
	bool bEnableAnimationSharing = true;

	// 项目 INI 提供默认素材；当前 GameMode 蓝图的组件属性可覆盖，不需要修改 C++。
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing")
	TSoftObjectPtr<UAnimSequence> IdleAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing")
	TSoftObjectPtr<UAnimSequence> MovingAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing", meta = (ClampMin = "1"))
	int32 IdleRandomizedInstances = 4;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing", meta = (ClampMin = "1"))
	int32 MovingRandomizedInstances = 4;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LeaderTickSignificanceThreshold = 0.20f;

private:
	// 自动化回归在临时世界中构造插件登记，覆盖真实 UnregisterActor 的 swap 句柄回调。
	friend class FFpstrueAnimationSharingStopTest;
	friend class FFpstrueEnemyCombatLifecycleTest;

	// ==================== Setup 与 Handle 维护 ====================

	// 从敌人骨架和待机/移动动画构建插件所需的运行时 Setup。
	bool BuildRuntimeSetup(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass);
	// 保存插件返回的 Actor Handle，后续用于更新 Leader Tick 显著性。
	void HandleActorHandleUpdated(int32 NewHandle, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy);

	// ==================== 运行时状态 ====================

	UPROPERTY(Transient)
	TObjectPtr<UAnimationSharingManager> SharingManager;

	UPROPERTY(Transient)
	TObjectPtr<UAnimationSharingSetup> RuntimeSetup;

	UPROPERTY(Transient)
	TObjectPtr<USkeleton> SharingSkeleton;

	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, uint32> RegisteredActorHandles;
	// UE 5.5 同步注册调用内的待交付集合；防止重复登记，无回调的失败请求在调用返回后清除。
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> PendingActorRegistrations;
	bool bRunning = false;
};
