// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "fpstrueEnemyAIController.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;

UENUM(BlueprintType)
enum class EFPEnemyAIState : uint8
{
	Idle   UMETA(DisplayName = "Idle"),
	Chase  UMETA(DisplayName = "Chase"),
	Attack UMETA(DisplayName = "Attack"),
	Dead   UMETA(DisplayName = "Dead")
};

UCLASS()
class FPSTRUE_API AfpstrueEnemyAIController : public AAIController
{
	GENERATED_BODY()

public:
	AfpstrueEnemyAIController();

	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void StopAI();

	void InitializeCombatContext(
		AfpstrueCharacter* NewTargetCharacter,
		AfpstrueSurroundManager* NewSurroundManager
	);

	void ApplyBenchmarkPathFollowingTickOverride(bool bDisablePathFollowingTick);

	void SetSignificanceDecisionMultiplier(float NewMultiplier);

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float AttackDecisionInterval = 0.1f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float ChaseDecisionInterval = 0.25f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float FarDecisionInterval = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float IdleDecisionInterval = 1.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.0"))
	float FarDecisionDistance = 3000.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float MoveAcceptanceRadius = 75.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float CombatMoveAcceptanceRadius = 15.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "25.0"))
	float PathRefreshDistance = 150.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "25.0"))
	float SlotArrivalTolerance = 100.0f;

private:
	void StartDecisionTimer();
	void ScheduleNextDecision(float Delay);
	void ClearDecisionTimer();
	void UpdateAI();
	float GetNextDecisionInterval() const;
	bool PrepareDecisionContext();
	bool HandleActiveAttack();
	bool HandleAttackApproach();
	bool HandleSurroundMovement();
	bool HandleSharedPursuit();
	void UpdateFacingTarget();
	void SetAIState(EFPEnemyAIState NewState);
	void MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius);
	void ReleaseSurroundSlot();
	AfpstrueCharacter* ResolveTarget() const;
	AfpstrueSurroundManager* ResolveSurroundManager() const;
	bool IsTargetUsable(const AfpstrueCharacter* Target) const;

	UPROPERTY()
	AfpstrueEnemyCharacter* ControlledEnemy = nullptr;

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter = nullptr;

	UPROPERTY()
	AfpstrueSurroundManager* SurroundManager = nullptr;

	EFPEnemyAIState AIState = EFPEnemyAIState::Idle;
	FVector LastMoveGoal = FVector::ZeroVector;
	uint32 LastSharedTargetGeneration = 0;
	bool bHasMoveGoal = false;
	bool bDisableDecisionThrottlingForBenchmark = false;
	float SignificanceDecisionMultiplier = 1.0f;
	FTimerHandle DecisionTimerHandle;
};
