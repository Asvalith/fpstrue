// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AnimationSharingTypes.h"
#include "fpstrueEnemyPerformance.generated.h"

UENUM(BlueprintType)
// 渲染档位只描述表现成本，不替代敌人的追击、攻击等玩法状态。
enum class EFPEnemyRenderSignificanceTier : uint8
{
	Full,
	Reduced,
	Background
};

/**
 * 敌人全局渲染显著性策略，由 GameMode 配置、Coordinator 校验和消费。
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
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	int32 MaxFullRenderEnemies = 12;

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
 * 单轮渲染预算使用的不可变排序键。
 *
 * 排序优先级依次为主视锥、扩展视锥、评分、TieBreakId。TieBreakId 只在其余字段完全相同时生效，
 * 用于保证同一批候选的结果确定。若以后需要把相近评分视为同档，应在生成键之前显式量化评分，
 * 不能在比较器中使用 IsNearlyEqual；“近似相等”不具备传递性，会破坏排序算法要求的严格弱序。
 */

//排序键
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



class AfpstrueGameMode;
class AfpstrueEnemyCharacter;
class UfpstrueEnemyAnimationSharingCoordinator;

/** 敌人显著性调度模块：集中采样所有敌人，通过有界 Top-K 分配 LOD、动画、阴影和骨骼 RT 预算。 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueEnemySignificanceCoordinator : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建由 Timer 驱动、无组件 Tick 的协调器。
	UfpstrueEnemySignificanceCoordinator();

	// ==================== 生命周期 ====================

	// GameMode 保留可编辑配置作为兼容层，Coordinator 独占评分、预算分配和应用流程。
	void Start(AfpstrueGameMode* InGameMode);
	// 多人 GameState 在每个 World 启动本地协调器：服务器更新玩法，客户端只分配自己的表现预算。
	void StartMultiplayer(UfpstrueEnemyAnimationSharingCoordinator* InSharingCoordinator);
	// 停止集中更新 Timer；GameMode 结束和组件销毁都会调用。
	void Stop();
	// 只读诊断用于自动化验证启动、发现副本和采样端职责，不参与玩法判断。
	bool IsRunning() const { return bRunning; }
	int32 GetTrackedEnemyCount() const { return LastTrackedEnemyCount; }
	int32 GetGameplayViewpointCount() const { return LastGameplayViewpointCount; }
	uint32 GetUpdateSerial() const { return UpdateSerial; }
	uint32 GetRenderUpdateSerial() const { return RenderUpdateSerial; }
	bool HasRenderedEnemies() const { return bReportedRenderUpdate; }

protected:
	// 组件退出时清理 Timer。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 多人配置属于本端表现策略；不把服务器相机或某个客户端的预算复制给其他玩家。
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Multiplayer")
	bool bEnableMultiplayerSignificance = true;
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Multiplayer", meta = (ClampMin = "0.1"))
	float MultiplayerUpdateInterval = 0.25f;
	UPROPERTY(EditDefaultsOnly, Category = "Performance|Multiplayer")
	FFPEnemyRenderSignificancePolicy MultiplayerRenderPolicy;

private:
	friend class FFpstrueMultiplayerSignificanceCoordinatorTest;
	friend class FFpstrueMultiplayerClientPerformanceTest;
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

	// ==================== 集中更新与策略校验 ====================

	// 一次完成 Gameplay 更新、Render 采样、Top-K 预算选择、组件应用和 CSV 统计。
	void Update();
	// 预算选完后统一应用并读回实际结果；统计不参与下一轮决策。
	void ApplyAndRecordCandidates(int32 FullBudgetDowngradeCount, int32 ShadowBudgetRejectedCount,
								  int32 RayTracingBudgetRejectedCount);
	// 修正权重、阈值、距离和名额的非法配置。
	void SanitizePolicy(FFPEnemyRenderSignificancePolicy& Policy);

	TWeakObjectPtr<AfpstrueGameMode> GameMode;
	TWeakObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> AnimationSharingCoordinator;
	FFPEnemyRenderSignificancePolicy ActiveRenderPolicy;
	bool bPolicyInitialized = false;
	bool bMultiplayerMode = false;
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
 * Animation Sharing 的唯一运行时所有者。
 * Render Significance 决定谁可进入共享池以及共享 Leader 是否值得更新，
 * Gameplay Significance 不读取视锥，也不依赖本组件。
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
	// World 退出时释放共享 Manager、运行时 Setup 和 Skeleton 引用。
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
