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

	UFUNCTION(BlueprintPure, Category = "AI")
	EFPEnemyAIState GetAIState() const { return AIState; }

	UFUNCTION(BlueprintCallable, Category = "AI")
	void StopAI();

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.05"))
	float DecisionInterval = 0.2f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float MoveAcceptanceRadius = 75.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "25.0"))
	float PathRefreshDistance = 150.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "25.0"))
	float SlotArrivalTolerance = 100.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.5"))
	float AttackTokenTimeout = 4.0f;

private:
	void StartDecisionTimer();
	void ClearDecisionTimer();
	void UpdateAI();
	void SetAIState(EFPEnemyAIState NewState);
	void MoveToGoal(const FVector& GoalLocation);
	void ReleaseSurroundResources(bool bReleaseSlot);
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
	bool bHasMoveGoal = false;
	bool bHasAttackToken = false;
	bool bObservedAttackInProgress = false;
	float AttackTokenAcquiredTime = 0.0f;
	FTimerHandle DecisionTimerHandle;
};
