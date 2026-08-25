// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "fpstrueEnemyCharacter.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyAIController;
class AfpstrueEnemyCharacter;
class UfpstrueHealthComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEnemyDeathReported, AfpstrueEnemyCharacter*, DeadEnemy);

enum class EFPEnemySignificanceTier : uint8
{
	Full,
	Reduced,
	Background
};

UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueEnemyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AfpstrueEnemyCharacter();

	UFUNCTION(BlueprintCallable, Category = "Combat")
	void HandleAttackFinishedNotify();

	void BeginAttackWindow();

	void UpdateAttackWindow();

	void EndAttackWindow();

	bool IsDead() const { return bIsDead; }

	bool IsAttacking() const { return bIsAttacking; }

	bool IsTargetInAttackRange() const;

	float GetChaseRange() const { return ChaseRange; }

	float GetAttackRange() const { return AttackRange; }

	float GetDistanceToTarget2D() const;

	void SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter);

	void FaceTarget();

	void ApplyBenchmarkDiagnosticOverrides(
		bool bDisableAttackSweep,
		bool bDisablePawnCollision,
		bool bDisableCharacterMovementTick
	);

	UPROPERTY(BlueprintAssignable, Category = "AI")
	FOnEnemyDeathReported OnEnemyDeathReported;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
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
	float ChaseRange = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackRange = 230.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackDamage = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackInterval = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float AttackAnimationDuration = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.1"))
	float AttackFailSafeDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float AttackCompletionGracePeriod = 0.1f;

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
	float DestroyDelay = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	bool bDestroyOnDeath = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "15000.0"))
	float DeathImpulseStrength = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeathImpulseUpwardBias = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Hit Reaction", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float HitReactionImpulseStrength = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance")
	bool bEnableMovementUpdateTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FullRateMovementDistance = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementDistance = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementTickInterval = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateMovementTickInterval = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateAnimationTickInterval = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateAnimationTickInterval = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "1.0"))
	float ReducedDecisionIntervalMultiplier = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "1.0"))
	float BackgroundDecisionIntervalMultiplier = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance")
	bool bEnableShadowDistanceTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float ShadowCullDistance = 5000.0f;

	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDied();

private:
	friend class AfpstrueEnemyAIController;

	bool TryAttackTarget();
	bool PerformMeleeHit();
	bool GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const;
	void SweepWeaponSegment(const FVector& TraceStart, const FVector& TraceEnd);
	bool TryApplyAttackDamage(AActor* HitActor);
	void CancelAttackWindow();
	void ScheduleAttackFinish(float DurationSeconds);
	void FinishAttack();
	bool CanAttack() const;
	void SetAttackAnimationPriority(bool bHighPriority);
	void RegisterWithSignificanceManager();
	void UnregisterFromSignificanceManager();
	void ApplySignificance(float Significance);
	void ApplySignificanceTier(EFPEnemySignificanceTier NewTier);
	void ApplySignificanceIntervals();
	void ApplyHitReactionImpulse();
	void ApplyDeathImpulse();

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter;

	float LastAttackTime = 0.0f;
	bool bIsDead = false;
	bool bIsAttacking = false;
	bool bHitTargetThisAttack = false;
	bool bAttackWindowActive = false;
	bool bHasPreviousWeaponSample = false;
	bool bDisableAnimationOptimizationsForBenchmark = false;
	bool bDisableAttackSweepForBenchmark = false;
	bool bRegisteredWithSignificanceManager = false;
	EFPEnemySignificanceTier SignificanceTier = EFPEnemySignificanceTier::Full;

	FVector PreviousWeaponBase = FVector::ZeroVector;
	FVector PreviousWeaponTip = FVector::ZeroVector;
	FVector LastDamageDirection = FVector::ForwardVector;
	FVector LastDamageLocation = FVector::ZeroVector;
	FName LastDamageBoneName = NAME_None;
	TSet<TWeakObjectPtr<AActor>> HitActorsThisAttack;

	FTimerHandle AttackFinishTimerHandle;
};
