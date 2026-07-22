// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "fpstrueEnemyCharacter.generated.h"

class AfpstrueCharacter;
class UfpstrueHealthComponent;

UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueEnemyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AfpstrueEnemyCharacter();

	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "Combat")
	void HandleAttackHitNotify();

	UFUNCTION(BlueprintPure, Category = "AI")
	bool IsDead() const { return bIsDead; }

	UFUNCTION(BlueprintPure, Category = "AI")
	bool IsAttacking() const { return bIsAttacking; }

	UFUNCTION(BlueprintPure, Category = "AI")
	bool IsTargetInAttackRange() const;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandleDeath();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	UfpstrueHealthComponent* HealthComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float MoveSpeed = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float ChaseRange = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackRange = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackDamage = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackInterval = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackAnimationDuration = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Trace", meta = (ClampMin = "0.0"))
	float AttackTraceRadius = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Trace")
	float AttackTraceHeight = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Debug")
	bool bDrawAttackTrace = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float DestroyDelay = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	bool bDestroyOnDeath = true;

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackLanded();

private:
	void UpdateEnemy();
	void MoveTowardTarget(const FVector& DirectionToTarget);
	void TryAttackTarget();
	bool PerformMeleeHit();
	void FinishAttack();
	bool CanAttack() const;

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter;

	float TimeSinceLastAttack = 0.0f;
	bool bIsDead = false;
	bool bIsAttacking = false;
	bool bDamageAppliedThisAttack = false;

	FTimerHandle AttackFinishTimerHandle;
};
