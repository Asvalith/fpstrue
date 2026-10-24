// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueSurroundManager.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueEnemyAIController;
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
	// Role/Revision 表示席位身份；共享导航缓存暂不可用时仍保留身份，但不能消费 Goal。
	// 有目标缓存不代表这名敌人的独立路径一定可达，实际路径仍由 Controller 校验。
	bool bHasGoal = false;
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
	int32 QueuedAttackers = 0;
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
	// 仅内圈消费攻击接近点；外圈资格和导航重试只看 ProjectedSlotLocation。
	FVector ProjectedApproachLocation = FVector::ZeroVector;
	bool bHasProjectedSlotLocation = false;
	bool bHasProjectedApproachLocation = false;
};

/**
 * 群体 AI 协调模块：共享玩家位置、分配稳定包围槽，并限制并发攻击和每帧 MoveTo 请求。
 *
 * 只保存跨敌人资源，不接管单敌人的行为树决策。敌人使用弱引用参与槽位和预算；玩家位置及NavMesh投影集中缓存，
 * AIController 通过窄接口申请/释放资源，避免各敌人独立维护互相冲突的群体状态。
 * 服务器 Game Thread 主链：注入目标 → EQS 发布共享布局 → FIFO 授席 → Controller 执行 → 释放并补位。
 * 席位是长期接近资格，ActiveAttackers 是单次出手名额；二者都不由黑板或客户端反向授予。
 * 【设计取舍】FIFO 解决有效等待者的席位晋升顺序，空间槽位解决站哪里；不是一张队列表包办攻击。
 * 内圈持有者仍须另取并发攻击名额，死亡/退出/超时归还后补位；候场者仍感知、受伤，不冻结 AI。
 * 玩家位置只作资源规划快照，Controller 必须先有感知资格才能使用，不能因管理器知情就隔墙追踪。
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
	// CombatComponent 在攻击最终提交前申请并发出手名额；长期内圈席位本身不等于正在攻击。
	bool TryAcquireAttackPermission(AfpstrueEnemyCharacter* Enemy);
	// CombatComponent 完成/中断攻击时归还本次出手名额，不释放长期包围席位。
	void ReleaseAttackPermission(AfpstrueEnemyCharacter* Enemy);
	// AIController 提交 MoveTo 前消费 World 共用帧预算；多个玩家的 Manager 不会各增加一份额度。
	bool TryConsumeMoveRequestBudget(bool bCombatPriority);
	// true 表示请求已登记，目标是否可用另看 bHasGoal；空间参与不受攻击候补容量限制。
	bool GetOrRequestTacticalAssignment(AfpstrueEnemyCharacter* Enemy, FFPEnemyTacticalAssignment& OutAssignment);
	// 校验真实内圈占用；Revision 为 0 时只查当前资格，否则还要求同一授予代际。
	bool HasInnerSlot(AfpstrueEnemyCharacter* Enemy, uint64 Revision = 0) const;
	// 终点和原生 NavMesh 路径分别校验；外围单位绕圈接近，被动入圈时只允许向外恢复。
	bool ResolveTacticalMoveGoal(AfpstrueEnemyCharacter* Enemy, const FFPEnemyTacticalAssignment& Assignment, FVector& OutGoal) const;
	// 检查路径剩余各段不穿越内圈；已在圈内的无资格单位只能向外恢复。
	bool IsTacticalPathAllowed(AfpstrueEnemyCharacter* Enemy, const FNavigationPath& Path, uint32 FirstPoint = 0) const;
	// 单位到不了自己的位置时退回候场，保留身份与短退避，防止立即抢回同一坏路径。
	void ReportTacticalMoveFailure(AfpstrueEnemyCharacter* Enemy, uint64 Revision);
	// 汇总容量、占用、队列与累计授予/回收数，不改变运行中的分配。
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

	// 独立于内圈容量。默认二者均为 8，正常流程不会因本预算排队；并发低于内圈时才发生出手竞争。
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
	friend class FFpstrueSurroundAdmissionTest;
	friend class FFpstrueSurroundAttackQueueTest;
	friend class FFPEnemyBehaviorTreeLifecycleTest;
	friend class FFPEnemyMoveRequestOwnershipTest;
	friend class FFpstrueEnemyAttackPlaybackTest;
	friend class FFpstrueEnemyAttackAssetIntegrationTest;
	friend class FFpstrueFacingToleranceTest;
	friend class FFpstrueEnemySignificanceCombatSampleTest;
	friend class FFpstrueEnemyGameplayTierReasonSnapshotTest;
	friend class FFpstrueEnemyReplicationTieringTest;
	friend class FFpstrueMultiplayerAuthorityPoseTest;
	friend class FFpstrueEnemyCombatLifecycleTest;
	friend class FFpstrueEnemyLowHealthRetreatTest;
	friend class FFpstrueMultiplayerPveBudgetTest;
	// 根据内外环配置创建稳定槽位布局。
	void BuildSlots();
	// Timer 回调：按阈值刷新共享目标位置，并按需请求新的槽位缓存。
	void UpdateSharedTargetSnapshot(bool bForce);
	// EQS 批量投影和筛选槽位、内圈接近点；身份校验通过后才发布。
	void RebuildProjectedSlotCache();
	// 只发布属于当前目标和本轮查询的完整缓存；EQS 回调在游戏线程执行。
	void HandleApproachQueryFinished(TSharedPtr<FEnvQueryResult> Result, uint32 Revision, FVector QueryTarget);
	// 掩体结果还要核对敌人请求身份和几何占用，查询分数不等于已获得占用。
	void HandleCoverQueryFinished(TSharedPtr<FEnvQueryResult> Result, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, uint32 Revision);
	// 移除失效敌人的槽位和攻击名额，防止弱引用表膨胀。
	void CleanupInvalidEntries();
	// 在指定环的可用空位中找二维最近槽位；没有候选时返回 INDEX_NONE，不抢占已有占用。
	int32 FindBestFreeSlot(const FVector& EnemyLocation, int32 RingIndex) const;
	// 先完成补位再通知；无有效目标时仍派发已有的释放通知。
	void PumpTacticalQueue();
	// 提交双向占用映射、授予代际和进度起点，再登记待通知者；由 Pump 保证输入有效。
	void AssignSlot(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, int32 SlotIndex);
	// 撤销参与身份、队列、席位和出手名额；只登记通知，不在移除中递归补位。
	void RemoveParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy);
	// 保留参与者但撤销当前资格，以新顺序排到队尾并设置短退避。
	void RequeueParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy);
	// 移出待通知集合后通知 Controller，回调新增的变更留给下一次派发。
	void NotifyTacticalChanges();
	// 返回身份是否仍登记；布局过期只使 bHasGoal=false，不将真实内圈资格伪装成 Waiting。
	bool BuildAssignment(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, FFPEnemyTacticalAssignment& OutAssignment) const;
	// 只做身份、目标、事务和冷却检查；有许可空位时才另外复核实际距离/朝向/LOS。
	AfpstrueEnemyAIController* GetWaitingAttackerController(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy) const;
	// 只唤醒有效队首的既有 BT；不在归还许可的回调中授予名额或同步开始攻击。
	void WakeNextWaitingAttacker();
	// 用目标环半径加当前敌人胶囊半径构造禁止穿越的边界。
	float GetInnerBoundary(AfpstrueEnemyCharacter* Enemy) const;
	// 调试模式下绘制已投影槽位，并以颜色区分是否占用。
	void DrawDebugSlots();

	// 每个 Manager 只服务一个目标；敌人的参与关系使用弱引用，不持有敌人生命周期。
	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> TargetCharacter;

	FVector CachedTargetLocation = FVector::ZeroVector;
	bool bHasSharedTargetSnapshot = false;
	bool bNeedsNavigationRetry = false;
	double SharedTargetSnapshotTime = 0.0;
	double NextNavigationRetryTime = 0.0;
	double NavigationCacheTime = 0.0;
	FVector PublishedTargetLocation = FVector::ZeroVector;

	TArray<FfpstrueSurroundSlot> SurroundSlots;
	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, int32> EnemyToSlot;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> ActiveAttackers;
	// 固定 8 个单次攻击候补，不含 ActiveAttackers；不是空间槽数或参战 AI 总量上限。
	// InlineAllocator 只决定内联存储，入队仍显式检查硬上限；满员者保留站位/候场目标和正常决策。
	static constexpr int32 MaxWaitingAttackers = 8;
	TArray<TWeakObjectPtr<AfpstrueEnemyCharacter>, TInlineAllocator<MaxWaitingAttackers>> WaitingAttackers;
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
	// 长期内圈晋升顺序，可包含外圈持有者；不与上面的单次攻击候补混用。
	TArray<FTacticalRequest> SlotWaitingQueue;
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
