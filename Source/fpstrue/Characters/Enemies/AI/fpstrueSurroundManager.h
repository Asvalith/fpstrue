// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueSurroundManager.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class UEnvQuery;
struct FEnvQueryResult;
struct FNavigationPath;

// 掩体查询按需异步执行；Pending 不等于找不到掩体。
enum class EFPCoverQueryResult : uint8 { Pending, Found, Failed };

// 席位是长期接近资格，单次攻击许可仍由 ActiveAttackers 单独管理。
enum class EFPEnemyTacticalRole : uint8 { Waiting, Outer, Inner };

struct FFPEnemyTacticalAssignment
{
	EFPEnemyTacticalRole Role = EFPEnemyTacticalRole::Waiting;
	FVector Goal = FVector::ZeroVector;
	uint64 Revision = 0;
};

struct FFPEnemyTacticalDebugSnapshot
{
	int32 InnerCapacity = 0;
	int32 OuterCapacity = 0;
	int32 UsableInnerSlots = 0;
	int32 UsableOuterSlots = 0;
	int32 OccupiedInnerSlots = 0;
	int32 OccupiedOuterSlots = 0;
	int32 QueuedEnemies = 0;
	int32 ActiveAttackers = 0;
	uint64 GrantCount = 0;
	uint64 ReleaseCount = 0;
	uint64 TimeoutCount = 0;
};

// 单个包围槽的静态布局、占用者和 NavMesh 投影缓存。
USTRUCT()
struct FfpstrueSurroundSlot
{
	GENERATED_BODY()

	int32 RingIndex = 0;
	float AngleRadians = 0.0f;
	float Radius = 250.0f;
	TWeakObjectPtr<AfpstrueEnemyCharacter> Occupant;
	FVector ProjectedSlotLocation = FVector::ZeroVector;
	FVector ProjectedApproachLocation = FVector::ZeroVector;
	bool bHasProjectedSlotLocation = false;
	bool bHasProjectedApproachLocation = false;
};

/**
 * 群体 AI 协调模块：共享玩家位置、分配稳定包围槽，并限制并发攻击和每帧 MoveTo 请求。
 *
 * 只保存跨敌人资源，不接管单敌人的行为树决策。敌人使用弱引用参与槽位和预算；玩家位置及NavMesh投影集中缓存，
 * AIController 通过窄接口申请/释放资源，避免各敌人独立维护互相冲突的群体状态。
 */
UCLASS()
class FPSTRUE_API AfpstrueSurroundManager : public AActor
{
	GENERATED_BODY()

public:
	// 创建只由 Timer 更新共享目标和调试绘制的管理器。
	AfpstrueSurroundManager();

	// GameMode 注入当前玩家，并使旧目标相关缓存失效。
	void SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter);
	// AI 停止追击或销毁时归还其包围槽。
	void ReleaseSurroundSlot(AfpstrueEnemyCharacter* Enemy);
	// AIController 请求并发攻击名额，避免所有敌人同时出手。
	bool TryAcquireAttackPermission(AfpstrueEnemyCharacter* Enemy);
	// 攻击结束、死亡或退出时归还攻击名额。
	void ReleaseAttackPermission(AfpstrueEnemyCharacter* Enemy);
	// AIController 提交 MoveTo 前消费 World 共用帧预算；多个玩家的 Manager 不会各增加一份额度。
	bool TryConsumeMoveRequestBudget(bool bCombatPriority);
	// 单次入队，重复决策只读本次分配；无席位时返回稳定外围目标，不退回玩家中心。
	bool GetOrRequestTacticalAssignment(AfpstrueEnemyCharacter* Enemy, FFPEnemyTacticalAssignment& OutAssignment);
	bool HasInnerSlot(AfpstrueEnemyCharacter* Enemy, uint64 Revision = 0) const;
	// 终点和原生 NavMesh 路径分别校验；外围单位绕圈接近，被动入圈时只允许向外恢复。
	bool ResolveTacticalMoveGoal(AfpstrueEnemyCharacter* Enemy, const FFPEnemyTacticalAssignment& Assignment, FVector& OutGoal) const;
	bool IsTacticalPathAllowed(AfpstrueEnemyCharacter* Enemy, const FNavigationPath& Path, uint32 FirstPoint = 0) const;
	// 单位到不了自己的位置时退回候场，保留身份与短退避，防止立即抢回同一坏路径。
	void ReportTacticalMoveFailure(AfpstrueEnemyCharacter* Enemy, uint64 Revision);
	FFPEnemyTacticalDebugSnapshot GetTacticalDebugSnapshot() const;
	// 远距离追击读取共享玩家位置，避免每个 AI 重复采样目标。
	bool GetSharedTargetSnapshot(FVector& OutLocation) const;

	// 只使用调用者已知的威胁位置；首次申请查询，后续读取结果，不在每轮决策重复执行 EQS。
	EFPCoverQueryResult FindOrRequestCover(AfpstrueEnemyCharacter* Enemy, const FVector& KnownThreatLocation, FVector& OutLocation);
	// 撤退结束、死亡、失去控制或换目标时撤销查询及掩体占用。
	void ReleaseCover(AfpstrueEnemyCharacter* Enemy);

	// 清空槽位、攻击者、预算和目标缓存，供 GameMode 结束时调用。
	void ResetManager();

protected:
	// 构建槽位并按配置启动调试 Timer；共享目标 Timer 在注入玩家时启动。
	virtual void BeginPlay() override;
	// 停止 Timer 并清空所有弱引用状态。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "4", ClampMax = "24"))
	int32 InnerSlotCount = 8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "4", ClampMax = "32"))
	int32 OuterSlotCount = 12;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "100.0"))
	float InnerRadius = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "150.0"))
	float OuterRadius = 430.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Attack", meta = (ClampMin = "50.0"))
	float AttackApproachRadius = 120.0f;

	// 接近迟迟没有进展时归还席位并排到队尾；到位后不按每次挥砍重新排队。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "1.0"))
	float SlotApproachTimeout = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Budget")
	bool bEnableActiveAttackerBudget = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Budget", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxActiveAttackers = 8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Budget")
	bool bEnableMoveRequestBudget = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Budget", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxMoveRequestsPerFrame = 8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Budget", meta = (ClampMin = "0", ClampMax = "16"))
	int32 ReservedCombatMoveRequestsPerFrame = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Navigation")
	FVector NavigationProjectionExtent = FVector(120.0f, 120.0f, 250.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Shared Target", meta = (ClampMin = "0.05"))
	float SharedTargetRefreshInterval = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Shared Target", meta = (ClampMin = "25.0"))
	float SharedTargetMoveThreshold = 200.0f;

	// 位移阈值负责合并频繁变化，最大年龄负责让“小幅移动后停住”最终刷新。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Shared Target", meta = (ClampMin = "0.05"))
	float SharedTargetMaxAge = 0.5f;

	// 导航尚未生成或某些接近点投影失败时，静止目标也需要低频重试。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Navigation", meta = (ClampMin = "0.05"))
	float NavigationRetryInterval = 0.5f;
	// 成功投影也会过期：动态障碍/NavMesh 重建时，静止玩家不能永久复用旧接近点。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Navigation", meta = (ClampMin = "0.05"))
	float NavigationCacheMaxAge = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Debug")
	bool bDrawDebugSlots = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Debug", meta = (ClampMin = "0.1"))
	float DebugDrawInterval = 0.25f;

	// 接近点共享查询；掩体按需查询。候选间距和提交上限独立于 MoveTo 预算。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|EQS", meta = (ClampMin = "50.0"))
	float MinPositionSpacing = 90.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|EQS", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxPendingCoverQueries = 4;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|EQS", meta = (ClampMin = "1", ClampMax = "4"))
	int32 MaxCoverQueriesPerFrame = 1;

private:
	friend class FFpstrueMultiplayerPveTargetLifecycleTest;
	friend class FFpstrueMultiplayerPveAuthorityTest;
	friend class FFpstrueSurroundCacheLifecycleTest;
	friend class FFpstrueSurroundTacticalQueueTest;
	friend class FFpstrueSurroundTacticalBoundaryTest;
	friend class FFpstrueEnemyCombatLifecycleTest;
	friend class FFpstrueEnemyLowHealthRetreatTest;
	friend class FFpstrueMultiplayerPveBudgetTest;
	// 根据内外环配置创建稳定槽位布局。
	void BuildSlots();
	// Timer 回调：按阈值刷新共享目标位置。
	// 更新缓存位置，并同步重建全部槽位的 NavMesh 投影。
	void UpdateSharedTargetSnapshot(bool bForce);
	// 批量把原始槽位和接近点投影到 NavMesh。
	void RebuildProjectedSlotCache();
	// 只发布属于当前目标和本轮查询的完整缓存；EQS 回调在游戏线程执行。
	void HandleApproachQueryFinished(TSharedPtr<FEnvQueryResult> Result, uint32 Revision, FVector QueryTarget);
	// 掩体结果还要核对敌人请求身份和几何占用，查询分数不等于已获得占用。
	void HandleCoverQueryFinished(TSharedPtr<FEnvQueryResult> Result, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, uint32 Revision);
	// 移除失效敌人的槽位和攻击名额，防止弱引用表膨胀。
	void CleanupInvalidEntries();
	int32 FindBestFreeSlot(const FVector& EnemyLocation, int32 RingIndex) const;
	void PumpTacticalQueue();
	void AssignSlot(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, int32 SlotIndex);
	void RemoveParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy);
	void RequeueParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy);
	void NotifyTacticalChanges();
	bool BuildAssignment(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, FFPEnemyTacticalAssignment& OutAssignment) const;
	float GetInnerBoundary(AfpstrueEnemyCharacter* Enemy) const;
	// 根据玩家位置、槽位角度和半径计算未投影位置。
	// 此生成规则集中在 EQS Generator，不再保留第二个手写选点实现。
	// 调试模式下绘制已投影槽位，并以颜色区分是否占用。
	void DrawDebugSlots();

	// 语法复习：跨 Actor 的长期引用需要进入反射系统；Transient 表明缓存不会被序列化到资产或存档。
	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> TargetCharacter;

	FVector CachedTargetLocation = FVector::ZeroVector;
	bool bHasSharedTargetSnapshot = false;
	bool bNeedsNavigationRetry = false;
	double SharedTargetSnapshotTime = 0.0;
	double NextNavigationRetryTime = 0.0;
	double NavigationCacheTime = 0.0;
	FVector PublishedTargetLocation = FVector::ZeroVector;

	// 语法复习：TArray 适合连续遍历；TMap 保存“敌人到槽位”的映射；TSet 只表达唯一攻击者集合。
	TArray<FfpstrueSurroundSlot> SurroundSlots;
	// 语法复习：弱指针作为键时可能留下失效键，因此 CleanupInvalidEntries 必须负责清理。
	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, int32> EnemyToSlot;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> ActiveAttackers;
	struct FTacticalParticipant
	{
		uint64 RequestSequence = 0;
		uint64 AssignmentRevision = 0;
		FVector WaitingOffset = FVector::ZeroVector;
		FVector LastProgressLocation = FVector::ZeroVector;
		double LastProgressTime = 0.0;
		double RetryAfter = 0.0;
	};
	struct FTacticalRequest
	{
		TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy;
		uint64 Sequence = 0;
	};
	// Participants 是唯一请求身份账本；FIFO 不再配一张重复的去重集合。
	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, FTacticalParticipant> Participants;
	TArray<FTacticalRequest> WaitingQueue;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> PendingTacticalNotifications;
	uint64 NextRequestSequence = 0;
	uint64 NextAssignmentRevision = 0;
	uint64 TacticalGrantCount = 0;
	uint64 TacticalReleaseCount = 0;
	uint64 TacticalTimeoutCount = 0;
	bool bPumpingTacticalQueue = false;
	// 模板仅建立一次，参数使用请求快照；运行中不修改 EQS 已缓存的模板。
	UPROPERTY(Transient)
	TObjectPtr<UEnvQuery> ApproachQueryTemplate;
	UPROPERTY(Transient)
	TObjectPtr<UEnvQuery> CoverQueryTemplate;
	int32 ApproachQueryId = INDEX_NONE;
	uint32 PositionQueryRevision = 0;
	double ApproachQueryStartedTime = 0.0;

	struct FCoverRequest
	{
		FVector ThreatLocation = FVector::ZeroVector;
		FVector Location = FVector::ZeroVector;
		int32 QueryId = INDEX_NONE;
		uint32 Revision = 0;
		EFPCoverQueryResult State = EFPCoverQueryResult::Pending;
		double StartedTime = 0.0;
	};
	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, FCoverRequest> CoverRequests;
	uint32 NextCoverRevision = 0;
	int32 PendingCoverQueries = 0;
	uint64 CoverQueryBudgetFrame = MAX_uint64;
	int32 CoverQueriesStartedThisFrame = 0;
	FTimerHandle DebugDrawTimerHandle;
	FTimerHandle SharedTargetTimerHandle;
};
