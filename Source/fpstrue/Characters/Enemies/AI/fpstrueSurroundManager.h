// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueSurroundManager.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class UEnvQuery;
struct FEnvQueryResult;

// 掩体查询按需异步执行；Pending 不等于找不到掩体。
enum class EFPCoverQueryResult : uint8 { Pending, Found, Failed };

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
	// 为敌人分配槽位，并返回更靠近玩家的攻击接近点。
	bool GetOrAssignAttackApproachLocation(AfpstrueEnemyCharacter* Enemy, FVector& OutLocation);
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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Attack", meta = (ClampMin = "100.0"))
	float OuterAttackApproachRadius = 220.0f;

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
	friend class FFpstrueSurroundCacheLifecycleTest;
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
	// 按距离和内外环优先级选择最佳空闲槽位。
	int32 FindBestFreeSlot(const FVector& EnemyLocation);
	// 内环空出时把合适的外环敌人提升进来。
	void PromoteOuterOccupantToInnerSlot(int32 InnerSlotIndex);
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
	uint64 MoveRequestBudgetFrame = MAX_uint64;
	int32 MoveRequestsConsumedThisFrame = 0;
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
