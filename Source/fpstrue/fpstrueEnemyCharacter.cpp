// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

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

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
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
}

void AfpstrueEnemyCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
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

void AfpstrueEnemyCharacter::UpdatePerformanceTier(float DistanceToTarget)
{
	if (!bEnableMovementUpdateTiering || bIsDead || bIsAttacking)
	{
		return;
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		float TickInterval = 0.0f;
		if (DistanceToTarget >= MidRateMovementDistance)
		{
			TickInterval = FarRateMovementTickInterval;
		}
		else if (DistanceToTarget >= FullRateMovementDistance)
		{
			TickInterval = MidRateMovementTickInterval;
		}

		Movement->SetComponentTickInterval(TickInterval);
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

void AfpstrueEnemyCharacter::HandleAttackHitNotify()
{
	if (bIsDead || !bIsAttacking || bHitTargetThisAttack)
	{
		return;
	}

	PerformMeleeHit();
}

void AfpstrueEnemyCharacter::HandleAttackFinishedNotify()
{
	if (bIsAttacking)
	{
		FinishAttack();
	}
}

bool AfpstrueEnemyCharacter::SetAttackPresentationDuration(float DurationSeconds)
{
	if (bIsDead || !bIsAttacking || DurationSeconds <= 0.0f)
	{
		return false;
	}

	ScheduleAttackFinish(DurationSeconds);
	return true;
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
	OnAttackLanded();
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
	const bool bHitTarget = bHitTargetThisAttack;
	bIsAttacking = false;
	SetAttackAnimationPriority(false);
	GetWorldTimerManager().ClearTimer(AttackFinishTimerHandle);
	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds();
	}

	if (!bHitTarget)
	{
		OnAttackMissed();
	}
	OnAttackFinished(bHitTarget);
	HitActorsThisAttack.Reset();
}

void AfpstrueEnemyCharacter::SetAttackAnimationPriority(bool bHighPriority)
{
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->VisibilityBasedAnimTickOption = bHighPriority
			? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			: EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	}

	if (bHighPriority)
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			Movement->SetComponentTickInterval(0.0f);
		}
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

	OnEnemyDamaged(DamageAmount, DamageCauser, InstigatedBy);
}

void AfpstrueEnemyCharacter::HandleDeath()
{
	if (bIsDead)
	{
		return;
	}

	bIsDead = true;
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
