// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueSurroundManager.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;

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
 * 只保存跨敌人资源，不接管单敌人FSM。敌人使用弱引用参与槽位和预算；玩家位置及NavMesh投影集中缓存，
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
	// AIController 提交 MoveTo 前消费本帧预算，战斗请求可使用预留名额。
	bool TryConsumeMoveRequestBudget(bool bCombatPriority);
	// 为敌人分配槽位，并返回更靠近玩家的攻击接近点。
	bool GetOrAssignAttackApproachLocation(AfpstrueEnemyCharacter* Enemy, FVector& OutLocation);
	// 远距离追击读取共享玩家位置，避免每个 AI 重复采样目标。
	bool GetSharedTargetSnapshot(FVector& OutLocation) const;

	// 清空槽位、攻击者、预算和目标缓存，供 GameMode 结束时调用。
	void ResetManager();

protected:
	// 构建槽位并启动共享目标和调试 Timer。
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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Debug")
	bool bDrawDebugSlots = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Debug", meta = (ClampMin = "0.1"))
	float DebugDrawInterval = 0.25f;

private:
	// 根据内外环配置创建稳定槽位布局。
	void BuildSlots();
	// Timer 回调：按阈值刷新共享目标位置。
	void RefreshSharedTargetSnapshot();
	// 更新缓存位置，并同步重建全部槽位的 NavMesh 投影。
	void UpdateSharedTargetSnapshot(bool bForce);
	// 批量把原始槽位和接近点投影到 NavMesh。
	void RebuildProjectedSlotCache();
	// 为尚未占槽的敌人选择并记录一个可用槽位。
	bool RequestSurroundSlot(AfpstrueEnemyCharacter* Enemy);
	// 移除失效敌人的槽位和攻击名额，防止弱引用表膨胀。
	void CleanupInvalidEntries();
	// 按距离和内外环优先级选择最佳空闲槽位。
	int32 FindBestFreeSlot(const FVector& EnemyLocation);
	// 内环空出时把合适的外环敌人提升进来。
	void PromoteOuterOccupantToInnerSlot(int32 InnerSlotIndex);
	// 根据玩家位置、槽位角度和半径计算未投影位置。
	FVector CalculateRawSlotLocation(const FfpstrueSurroundSlot& Slot, float RadiusOverride = -1.0f) const;
	// 调试模式下绘制槽位、占用关系和攻击接近点。
	void DrawDebugSlots();

	// 跨 Actor 的长期引用需要进入反射系统；Transient 表明缓存不会被序列化到资产或存档。
	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> TargetCharacter;

	FVector CachedTargetLocation = FVector::ZeroVector;
	bool bHasSharedTargetSnapshot = false;

	// TArray 适合连续遍历；TMap 保存“敌人到槽位”的映射；TSet 只表达唯一攻击者集合。
	TArray<FfpstrueSurroundSlot> SurroundSlots;
	// 弱指针作为键时可能留下失效键，因此 CleanupInvalidEntries 必须负责清理。
	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, int32> EnemyToSlot;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> ActiveAttackers;
	uint64 MoveRequestBudgetFrame = MAX_uint64;
	int32 MoveRequestsConsumedThisFrame = 0;
	FTimerHandle DebugDrawTimerHandle;
	FTimerHandle SharedTargetTimerHandle;
};
