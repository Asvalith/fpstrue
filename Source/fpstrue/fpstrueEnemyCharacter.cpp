// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueHealthComponent.h"
#include "fpstruePerformanceStats.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "SignificanceManager.h"

DEFINE_STAT(STAT_fpstrueAttackSweepTime);
DEFINE_STAT(STAT_fpstrueAttackSweepCount);
DEFINE_STAT(STAT_fpstrueSweepReturnedHitCount);
DEFINE_STAT(STAT_fpstrueAttackWindowUpdateCount);
CSV_DEFINE_CATEGORY(fpstrueCombat, true);

AfpstrueEnemyCharacter::AfpstrueEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	AIControllerClass = AfpstrueEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 96.0f);

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->bEnableUpdateRateOptimizations = true;
		CharacterMesh->VisibilityBasedAnimTickOption =
			EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	}

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();
	SetActorTickEnabled(false);

	if (FParse::Param(FCommandLine::Get(), TEXT("BenchmarkDisableMovementTiering")))
	{
		bEnableMovementUpdateTiering = false;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("BenchmarkDisableShadowTiering")))
	{
		bEnableShadowDistanceTiering = false;
	}
	bDisableAnimationOptimizationsForBenchmark = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisableAnimationOptimizations")
	);

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (bDisableAnimationOptimizationsForBenchmark)
		{
			CharacterMesh->bEnableUpdateRateOptimizations = false;
			CharacterMesh->VisibilityBasedAnimTickOption =
				EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
		if (!bEnableShadowDistanceTiering)
		{
			CharacterMesh->SetCastShadow(true);
		}

		CharacterMesh->SetSimulatePhysics(false);
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDamageReceived);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = MoveSpeed;
		Movement->bOrientRotationToMovement = false;
		Movement->bUseControllerDesiredRotation = true;
		Movement->RotationRate = FRotator(0.0f, 540.0f, 0.0f);
	}
	bUseControllerRotationYaw = false;

	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds() - AttackInterval;
	}

	RegisterWithSignificanceManager();
}

void AfpstrueEnemyCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromSignificanceManager();
	CancelAttackWindow();
	GetWorldTimerManager().ClearTimer(AttackFinishTimerHandle);
	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageReceived.RemoveDynamic(this, &AfpstrueEnemyCharacter::HandleDamageReceived);
	}

	Super::EndPlay(EndPlayReason);
}

float AfpstrueEnemyCharacter::TakeDamage(
	float DamageAmount,
	FDamageEvent const& DamageEvent,
	AController* EventInstigator,
	AActor* DamageCauser)
{
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		const FPointDamageEvent* PointDamageEvent = static_cast<const FPointDamageEvent*>(&DamageEvent);
		LastDamageDirection = PointDamageEvent->ShotDirection.GetSafeNormal();
		LastDamageLocation = PointDamageEvent->HitInfo.ImpactPoint;
		LastDamageBoneName = PointDamageEvent->HitInfo.BoneName;
	}
	else if (DamageCauser != nullptr)
	{
		LastDamageDirection = (GetActorLocation() - DamageCauser->GetActorLocation()).GetSafeNormal();
		LastDamageLocation = GetActorLocation();
		LastDamageBoneName = NAME_None;
	}

	if (LastDamageDirection.IsNearlyZero())
	{
		LastDamageDirection = GetActorForwardVector() * -1.0f;
	}

	if (LastDamageLocation.IsNearlyZero())
	{
		LastDamageLocation = GetActorLocation();
	}

	return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
}

float AfpstrueEnemyCharacter::GetDistanceToTarget2D() const
{
	if (TargetCharacter == nullptr)
	{
		return MAX_flt;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	return FVector(ToTarget.X, ToTarget.Y, 0.0f).Size();
}

void AfpstrueEnemyCharacter::SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter)
{
	TargetCharacter = NewTargetCharacter;
}

void AfpstrueEnemyCharacter::FaceTarget()
{
	if (TargetCharacter == nullptr)
	{
		return;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	if (!HorizontalToTarget.IsNearlyZero())
	{
		SetActorRotation(HorizontalToTarget.GetSafeNormal().Rotation());
	}
}

void AfpstrueEnemyCharacter::RegisterWithSignificanceManager()
{
	if (bRegisteredWithSignificanceManager || GetWorld() == nullptr)
	{
		return;
	}

	USignificanceManager* Manager = USignificanceManager::Get(GetWorld());
	if (Manager == nullptr)
	{
		return;
	}

	Manager->RegisterObject(
		this,
		TEXT("Enemy"),
		[](USignificanceManager::FManagedObjectInfo* ObjectInfo, const FTransform& Viewpoint)
		{
			const AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(ObjectInfo->GetObject());
			if (!IsValid(Enemy) || Enemy->IsDead())
			{
				return 0.0f;
			}

			const float Distance = FVector::Dist2D(
				Enemy->GetActorLocation(),
				Viewpoint.GetLocation()
			);
			return 1.0f / (1.0f + Distance);
		},
		USignificanceManager::EPostSignificanceType::Sequential,
		[](USignificanceManager::FManagedObjectInfo* ObjectInfo, float, float NewSignificance, bool bUnregister)
		{
			AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(ObjectInfo->GetObject());
			if (!bUnregister && IsValid(Enemy))
			{
				Enemy->ApplySignificance(NewSignificance);
			}
		}
	);
	bRegisteredWithSignificanceManager = true;
}

void AfpstrueEnemyCharacter::UnregisterFromSignificanceManager()
{
	if (!bRegisteredWithSignificanceManager)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (USignificanceManager* Manager = USignificanceManager::Get(World))
		{
			Manager->UnregisterObject(this);
		}
	}
	bRegisteredWithSignificanceManager = false;
}

void AfpstrueEnemyCharacter::ApplySignificance(float Significance)
{
	if (bIsDead)
	{
		return;
	}

	const float FullRateThreshold = 1.0f / (1.0f + FullRateMovementDistance);
	const float MidRateThreshold = 1.0f / (1.0f + MidRateMovementDistance);
	const bool bRequiresFullRate = bIsAttacking || bAttackWindowActive || IsTargetInAttackRange();

	EFPEnemySignificanceTier NewTier = EFPEnemySignificanceTier::Background;
	if (bRequiresFullRate || Significance >= FullRateThreshold)
	{
		NewTier = EFPEnemySignificanceTier::Full;
	}
	else if (Significance >= MidRateThreshold)
	{
		NewTier = EFPEnemySignificanceTier::Reduced;
	}

	if (bEnableShadowDistanceTiering)
	{
		if (USkeletalMeshComponent* CharacterMesh = GetMesh())
		{
			const float ShadowThreshold = 1.0f / (1.0f + ShadowCullDistance);
			const bool bShouldCastShadow = Significance >= ShadowThreshold;
			if (CharacterMesh->CastShadow != bShouldCastShadow)
			{
				CharacterMesh->SetCastShadow(bShouldCastShadow);
			}
		}
	}

	ApplySignificanceTier(NewTier);
}

void AfpstrueEnemyCharacter::ApplySignificanceTier(EFPEnemySignificanceTier NewTier)
{
	if (bIsDead)
	{
		return;
	}

	SignificanceTier = NewTier;
	ApplySignificanceIntervals();

	if (AfpstrueEnemyAIController* EnemyAIController = Cast<AfpstrueEnemyAIController>(GetController()))
	{
		float DecisionMultiplier = 1.0f;
		switch (SignificanceTier)
		{
		case EFPEnemySignificanceTier::Reduced:
			DecisionMultiplier = ReducedDecisionIntervalMultiplier;
			break;

		case EFPEnemySignificanceTier::Background:
			DecisionMultiplier = BackgroundDecisionIntervalMultiplier;
			break;

		case EFPEnemySignificanceTier::Full:
		default:
			break;
		}
		EnemyAIController->SetSignificanceDecisionMultiplier(DecisionMultiplier);
	}
}

void AfpstrueEnemyCharacter::ApplySignificanceIntervals()
{
	if (bIsDead)
	{
		return;
	}

	float MovementTickInterval = 0.0f;
	float AnimationTickInterval = 0.0f;
	if (!bIsAttacking && bEnableMovementUpdateTiering)
	{
		switch (SignificanceTier)
		{
		case EFPEnemySignificanceTier::Reduced:
			MovementTickInterval = MidRateMovementTickInterval;
			AnimationTickInterval = MidRateAnimationTickInterval;
			break;

		case EFPEnemySignificanceTier::Background:
			MovementTickInterval = FarRateMovementTickInterval;
			AnimationTickInterval = FarRateAnimationTickInterval;
			break;

		case EFPEnemySignificanceTier::Full:
		default:
			break;
		}
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetComponentTickInterval(MovementTickInterval);
	}

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->SetComponentTickInterval(
			bDisableAnimationOptimizationsForBenchmark ? 0.0f : AnimationTickInterval
		);
	}
}

void AfpstrueEnemyCharacter::ApplyBenchmarkDiagnosticOverrides(
	bool bDisableAttackSweep,
	bool bDisablePawnCollision,
	bool bDisableCharacterMovementTick)
{
	bDisableAttackSweepForBenchmark = bDisableAttackSweep;

	if (bDisablePawnCollision)
	{
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetComponentTickEnabled(!bDisableCharacterMovementTick);
	}
}

bool AfpstrueEnemyCharacter::TryAttackTarget()
{
	if (!CanAttack())
	{
		return false;
	}

	CancelAttackWindow();
	HitActorsThisAttack.Reset();
	bIsAttacking = true;
	bHitTargetThisAttack = false;
	SetAttackAnimationPriority(true);
	FaceTarget();

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}

	ScheduleAttackFinish(FMath::Max(AttackAnimationDuration, AttackFailSafeDuration));
	OnAttackStarted();

	return true;
}

void AfpstrueEnemyCharacter::HandleAttackFinishedNotify()
{
	if (bIsAttacking)
	{
		FinishAttack();
	}
}

void AfpstrueEnemyCharacter::BeginAttackWindow()
{
	if (bIsDead || !bIsAttacking)
	{
		return;
	}

	CancelAttackWindow();

	FVector CurrentWeaponBase;
	FVector CurrentWeaponTip;
	if (!GetWeaponBladeSegment(CurrentWeaponBase, CurrentWeaponTip))
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("%s cannot start attack window: sockets '%s' and '%s' must both exist."),
			*GetName(),
			*WeaponTraceStartSocketName.ToString(),
			*WeaponTraceEndSocketName.ToString()
		);
		return;
	}

	bAttackWindowActive = true;
	bHasPreviousWeaponSample = true;
	PreviousWeaponBase = CurrentWeaponBase;
	PreviousWeaponTip = CurrentWeaponTip;
}

void AfpstrueEnemyCharacter::UpdateAttackWindow()
{
	if (!bAttackWindowActive || bIsDead || !bIsAttacking)
	{
		return;
	}

	INC_DWORD_STAT(STAT_fpstrueAttackWindowUpdateCount);
	CSV_CUSTOM_STAT(
		fpstrueCombat,
		AttackWindowUpdateCount,
		1,
		ECsvCustomStatOp::Accumulate
	);

	FVector CurrentWeaponBase;
	FVector CurrentWeaponTip;
	if (!GetWeaponBladeSegment(CurrentWeaponBase, CurrentWeaponTip))
	{
		CancelAttackWindow();
		return;
	}

	if (!bHasPreviousWeaponSample)
	{
		PreviousWeaponBase = CurrentWeaponBase;
		PreviousWeaponTip = CurrentWeaponTip;
		bHasPreviousWeaponSample = true;
		return;
	}

	if (bDisableAttackSweepForBenchmark)
	{
		PreviousWeaponBase = CurrentWeaponBase;
		PreviousWeaponTip = CurrentWeaponTip;
		return;
	}

	const int32 SampleCount = FMath::Clamp(WeaponTraceSampleCount, 2, 8);
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const float Alpha = static_cast<float>(SampleIndex) / static_cast<float>(SampleCount - 1);
		const FVector PreviousSample = FMath::Lerp(PreviousWeaponBase, PreviousWeaponTip, Alpha);
		const FVector CurrentSample = FMath::Lerp(CurrentWeaponBase, CurrentWeaponTip, Alpha);
		SweepWeaponSegment(PreviousSample, CurrentSample);
	}

	SweepWeaponSegment(CurrentWeaponBase, CurrentWeaponTip);

	PreviousWeaponBase = CurrentWeaponBase;
	PreviousWeaponTip = CurrentWeaponTip;
}

void AfpstrueEnemyCharacter::EndAttackWindow()
{
	if (!bAttackWindowActive)
	{
		return;
	}

	CancelAttackWindow();
}

bool AfpstrueEnemyCharacter::PerformMeleeHit()
{
	UWorld* World = GetWorld();
	if (World == nullptr || TargetCharacter == nullptr || TargetCharacter->IsDead())
	{
		return false;
	}

	const FVector TraceStart = GetActorLocation() + FVector(0.0f, 0.0f, AttackTraceHeight);
	const FVector TraceEnd = TraceStart + GetActorForwardVector() * AttackRange;

	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EnemyMeleeTrace), false, this);
	QueryParams.AddIgnoredActor(this);

	TArray<FHitResult> HitResults;
	const bool bHitAnyPawn = World->SweepMultiByObjectType(
		HitResults,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(AttackTraceRadius),
		QueryParams
	);

	bool bHitPlayer = false;
	if (bHitAnyPawn)
	{
		for (const FHitResult& HitResult : HitResults)
		{
			if (HitResult.GetActor() != TargetCharacter)
			{
				continue;
			}

			bHitPlayer = TryApplyAttackDamage(TargetCharacter);
			break;
		}
	}

	if (bDrawAttackTrace)
	{
		const FColor DebugColor = bHitPlayer ? FColor::Green : FColor::Red;
		DrawDebugLine(World, TraceStart, TraceEnd, DebugColor, false, 1.0f, 0, 2.0f);
		DrawDebugSphere(World, TraceStart, AttackTraceRadius, 16, DebugColor, false, 1.0f);
		DrawDebugSphere(World, TraceEnd, AttackTraceRadius, 16, DebugColor, false, 1.0f);
	}

	return bHitPlayer;
}

bool AfpstrueEnemyCharacter::GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const
{
	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr
		|| !CharacterMesh->DoesSocketExist(WeaponTraceStartSocketName)
		|| !CharacterMesh->DoesSocketExist(WeaponTraceEndSocketName))
	{
		return false;
	}

	OutBladeBase = CharacterMesh->GetSocketLocation(WeaponTraceStartSocketName);
	OutBladeTip = CharacterMesh->GetSocketLocation(WeaponTraceEndSocketName);
	return true;
}

void AfpstrueEnemyCharacter::SweepWeaponSegment(const FVector& TraceStart, const FVector& TraceEnd)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemy_AttackSweep);
	CSV_SCOPED_TIMING_STAT(fpstrueCombat, AttackSweepTime);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAttackSweepTime);
	INC_DWORD_STAT(STAT_fpstrueAttackSweepCount);
	CSV_CUSTOM_STAT(fpstrueCombat, AttackSweepCount, 1, ECsvCustomStatOp::Accumulate);

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EnemyWeaponTrace), false, this);
	QueryParams.AddIgnoredActor(this);

	TArray<FHitResult> HitResults;
	const bool bHitAnyPawn = World->SweepMultiByObjectType(
		HitResults,
		TraceStart,
		TraceEnd,
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(WeaponTraceRadius),
		QueryParams
	);

	INC_DWORD_STAT_BY(STAT_fpstrueSweepReturnedHitCount, HitResults.Num());
	CSV_CUSTOM_STAT(
		fpstrueCombat,
		SweepReturnedHitCount,
		HitResults.Num(),
		ECsvCustomStatOp::Accumulate
	);

	if (bDrawAttackTrace)
	{
		const FColor DebugColor = bHitAnyPawn ? FColor::Green : FColor::Yellow;
		DrawDebugLine(World, TraceStart, TraceEnd, DebugColor, false, 0.1f, 0, 1.5f);
		DrawDebugSphere(World, TraceStart, WeaponTraceRadius, 8, DebugColor, false, 0.1f);
		DrawDebugSphere(World, TraceEnd, WeaponTraceRadius, 8, DebugColor, false, 0.1f);
	}

	if (!bHitAnyPawn)
	{
		return;
	}

	for (const FHitResult& HitResult : HitResults)
	{
		TryApplyAttackDamage(HitResult.GetActor());
	}
}

bool AfpstrueEnemyCharacter::TryApplyAttackDamage(AActor* HitActor)
{
	if (HitActor == nullptr
		|| HitActor != TargetCharacter
		|| TargetCharacter->IsDead()
		|| bHitTargetThisAttack)
	{
		return false;
	}

	const TWeakObjectPtr<AActor> WeakHitActor(HitActor);
	if (HitActorsThisAttack.Contains(WeakHitActor))
	{
		return false;
	}

	const float AppliedDamage = UGameplayStatics::ApplyDamage(
		HitActor,
		AttackDamage,
		GetController(),
		this,
		nullptr
	);

	if (AppliedDamage <= 0.0f)
	{
		return false;
	}

	HitActorsThisAttack.Add(WeakHitActor);
	bHitTargetThisAttack = true;
	return true;
}

void AfpstrueEnemyCharacter::CancelAttackWindow()
{
	bAttackWindowActive = false;
	bHasPreviousWeaponSample = false;
	PreviousWeaponBase = FVector::ZeroVector;
	PreviousWeaponTip = FVector::ZeroVector;
}

void AfpstrueEnemyCharacter::ScheduleAttackFinish(float DurationSeconds)
{
	GetWorldTimerManager().SetTimer(
		AttackFinishTimerHandle,
		this,
		&AfpstrueEnemyCharacter::FinishAttack,
		FMath::Max(0.01f, DurationSeconds + AttackCompletionGracePeriod),
		false
	);
}

void AfpstrueEnemyCharacter::FinishAttack()
{
	if (!bIsAttacking)
	{
		return;
	}

	EndAttackWindow();
	bIsAttacking = false;
	SetAttackAnimationPriority(false);
	GetWorldTimerManager().ClearTimer(AttackFinishTimerHandle);
	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds();
	}

	HitActorsThisAttack.Reset();
}

void AfpstrueEnemyCharacter::SetAttackAnimationPriority(bool bHighPriority)
{
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (bDisableAnimationOptimizationsForBenchmark)
		{
			CharacterMesh->VisibilityBasedAnimTickOption =
				EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
		else
		{
			CharacterMesh->VisibilityBasedAnimTickOption = bHighPriority
				? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
				: EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
		}

		if (bHighPriority)
		{
			CharacterMesh->SetComponentTickInterval(0.0f);
		}
	}

	if (bHighPriority)
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			Movement->SetComponentTickInterval(0.0f);
		}
	}
	else
	{
		ApplySignificanceIntervals();
	}
}


bool AfpstrueEnemyCharacter::IsTargetInAttackRange() const
{
	if (TargetCharacter == nullptr)
	{
		return false;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);

	const float EnemyRadius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float TargetRadius = TargetCharacter->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float MinimumReachableDistance = EnemyRadius + TargetRadius + 5.0f;
	const float EffectiveAttackRange = FMath::Max(AttackRange, MinimumReachableDistance);

	return HorizontalToTarget.Size() <= EffectiveAttackRange;
}

bool AfpstrueEnemyCharacter::CanAttack() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	return TargetCharacter != nullptr
		&& TargetCharacter->GetHealthComponent() != nullptr
		&& !TargetCharacter->GetHealthComponent()->IsDead()
		&& !bIsDead
		&& !bIsAttacking
		&& IsTargetInAttackRange()
		&& World->GetTimeSeconds() - LastAttackTime >= AttackInterval;
}

void AfpstrueEnemyCharacter::HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	if (HealthComponent != nullptr && HealthComponent->IsDead())
	{
		return;
	}

	ApplyHitReactionImpulse();
	OnEnemyDamaged(DamageAmount, DamageCauser, InstigatedBy);
}

void AfpstrueEnemyCharacter::ApplyHitReactionImpulse()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (Movement == nullptr || HitReactionImpulseStrength <= 0.0f || Movement->MovementMode == MOVE_None)
	{
		return;
	}

	FVector HitDirection(LastDamageDirection.X, LastDamageDirection.Y, 0.0f);
	if (!HitDirection.Normalize())
	{
		return;
	}

	Movement->AddImpulse(HitDirection * HitReactionImpulseStrength, true);
}

void AfpstrueEnemyCharacter::HandleDeath()
{
	if (bIsDead)
	{
		return;
	}

	bIsDead = true;
	UnregisterFromSignificanceManager();
	CancelAttackWindow();
	bIsAttacking = false;
	HitActorsThisAttack.Reset();
	SetAttackAnimationPriority(false);
	GetWorldTimerManager().ClearTimer(AttackFinishTimerHandle);

	if (AfpstrueEnemyAIController* EnemyAIController = Cast<AfpstrueEnemyAIController>(GetController()))
	{
		EnemyAIController->StopAI();
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = 0.0f;
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
		Movement->SetComponentTickEnabled(false);
	}

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->SetComponentTickInterval(0.0f);
		CharacterMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetEnableGravity(true);
		CharacterMesh->SetSimulatePhysics(true);
	}

	OnEnemyDeathReported.Broadcast(this);
	OnEnemyDied();
	GetWorldTimerManager().SetTimerForNextTick(this, &AfpstrueEnemyCharacter::ApplyDeathImpulse);

	if (bDestroyOnDeath)
	{
		SetLifeSpan(DestroyDelay);
	}
}

void AfpstrueEnemyCharacter::ApplyDeathImpulse()
{
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr || !CharacterMesh->IsSimulatingPhysics())
	{
		return;
	}

	CharacterMesh->SetEnableGravity(true);
	CharacterMesh->WakeAllRigidBodies();

	const FVector ImpulseDirection =
		(LastDamageDirection + FVector::UpVector * DeathImpulseUpwardBias).GetSafeNormal();
	if (ImpulseDirection.IsNearlyZero())
	{
		return;
	}

	CharacterMesh->AddImpulseAtLocation(
		ImpulseDirection * FMath::Clamp(DeathImpulseStrength, 0.0f, 15000.0f),
		LastDamageLocation,
		LastDamageBoneName
	);
}
