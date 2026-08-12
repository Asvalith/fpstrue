// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "fpstrueEnemyCharacter.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class UfpstrueHealthComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEnemyDeathReported, AfpstrueEnemyCharacter*, DeadEnemy);

UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueEnemyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AfpstrueEnemyCharacter();

	UFUNCTION(BlueprintCallable, Category = "Combat")
	void HandleAttackHitNotify();

	UFUNCTION(BlueprintCallable, Category = "Combat")
	void HandleAttackFinishedNotify();

	UFUNCTION(BlueprintCallable, Category = "Combat|Attack Window")
	void BeginAttackWindow();

	UFUNCTION(BlueprintCallable, Category = "Combat|Attack Window")
	void UpdateAttackWindow();

	UFUNCTION(BlueprintCallable, Category = "Combat|Attack Window")
	void EndAttackWindow();

	UFUNCTION(BlueprintPure, Category = "AI")
	bool IsDead() const { return bIsDead; }

	UFUNCTION(BlueprintPure, Category = "AI")
	bool IsAttacking() const { return bIsAttacking; }

	UFUNCTION(BlueprintPure, Category = "AI")
	bool IsTargetInAttackRange() const;

	UFUNCTION(BlueprintPure, Category = "AI")
	float GetChaseRange() const { return ChaseRange; }

	UFUNCTION(BlueprintPure, Category = "AI")
	float GetAttackRange() const { return AttackRange; }

	UFUNCTION(BlueprintPure, Category = "AI")
	float GetDistanceToTarget2D() const;

	UFUNCTION(BlueprintCallable, Category = "AI")
	void SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter);

	UFUNCTION(BlueprintCallable, Category = "AI")
	void FaceTarget();

	void UpdatePerformanceTier(float DistanceToTarget);

	bool TryAttackTarget();

	UPROPERTY(BlueprintAssignable, Category = "AI")
	FOnEnemyDeathReported OnEnemyDeathReported;

protected:
	virtual void BeginPlay() override;
	virtual float TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	UFUNCTION()
	void HandleDeath();

	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	UfpstrueHealthComponent* HealthComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float MoveSpeed = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float ChaseRange = 20000.0f;

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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace")
	FName WeaponTraceStartSocketName = TEXT("weapontop");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace")
	FName WeaponTraceEndSocketName = TEXT("weaponend");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (ClampMin = "1.0"))
	float WeaponTraceRadius = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (ClampMin = "2", ClampMax = "8"))
	int32 WeaponTraceSampleCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Debug")
	bool bDrawAttackTrace = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float DestroyDelay = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	bool bDestroyOnDeath = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0"))
	float DeathImpulseStrength = 70000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeathImpulseUpwardBias = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance")
	bool bEnableMovementUpdateTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FullRateMovementDistance = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementDistance = 4000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementTickInterval = 0.033333f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateMovementTickInterval = 0.066667f;

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackLanded();

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackMissed();

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackFinished(bool bHitTarget);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDied();

private:
	bool PerformMeleeHit();
	bool GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const;
	void SweepWeaponSegment(const FVector& TraceStart, const FVector& TraceEnd);
	bool TryApplyAttackDamage(AActor* HitActor);
	void CancelAttackWindow();
	void FinishAttack();
	bool CanAttack() const;
	void SetAttackAnimationPriority(bool bHighPriority);
	void ApplyDeathImpulse();

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter;

	float LastAttackTime = 0.0f;
	bool bIsDead = false;
	bool bIsAttacking = false;
	bool bDamageAppliedThisAttack = false;
	bool bHitTargetThisAttack = false;
	bool bAttackWindowActive = false;
	bool bHasPreviousWeaponSample = false;

	FVector PreviousWeaponBase = FVector::ZeroVector;
	FVector PreviousWeaponTip = FVector::ZeroVector;
	FVector LastDamageDirection = FVector::ForwardVector;
	FVector LastDamageLocation = FVector::ZeroVector;
	FName LastDamageBoneName = NAME_None;
	TSet<TWeakObjectPtr<AActor>> HitActorsThisAttack;

	FTimerHandle AttackFinishTimerHandle;
};
