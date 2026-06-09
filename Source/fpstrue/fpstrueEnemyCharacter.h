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

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandleHealthChanged(float NewHealth);

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
	float DestroyDelay = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	bool bDestroyOnDeath = false;

private:
	void UpdateEnemy(float DeltaTime);
	void MoveTowardTarget(const FVector& DirectionToTarget);
	void TryAttackTarget();
	bool CanAttack() const;

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter;

	float TimeSinceLastAttack = 0.0f;
	bool bIsDead = false;
};
