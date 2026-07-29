// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

AfpstrueEnemyCharacter::AfpstrueEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	AIControllerClass = AfpstrueEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 96.0f);

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.AddDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageReceived.AddDynamic(this, &AfpstrueEnemyCharacter::HandleDamageReceived);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = MoveSpeed;
	}

	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds();
	}
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

bool AfpstrueEnemyCharacter::TryAttackTarget()
{
	if (!CanAttack())
	{
		return false;
	}

	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds();
	}

	CancelAttackWindow();
	bIsAttacking = true;
	bDamageAppliedThisAttack = false;
	bHitTargetThisAttack = false;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}

	OnAttackStarted();

	GetWorldTimerManager().SetTimer(
		AttackFinishTimerHandle,
		this,
		&AfpstrueEnemyCharacter::FinishAttack,
		AttackAnimationDuration,
		false
	);

	return true;
}

void AfpstrueEnemyCharacter::HandleAttackHitNotify()
{
	if (bIsDead || !bIsAttacking || bDamageAppliedThisAttack)
	{
		return;
	}

	bDamageAppliedThisAttack = true;
	bHitTargetThisAttack = PerformMeleeHit();
	if (!bHitTargetThisAttack && !bAttackWindowActive)
	{
		OnAttackMissed();
	}
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
	HitActorsThisAttack.Reset();
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

	const bool bMissedTarget = !bHitTargetThisAttack;
	CancelAttackWindow();

	if (bMissedTarget)
	{
		OnAttackMissed();
	}
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
	if (HitActor == nullptr || HitActor != TargetCharacter || TargetCharacter->IsDead())
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
	bDamageAppliedThisAttack = true;
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
	HitActorsThisAttack.Reset();
}

void AfpstrueEnemyCharacter::FinishAttack()
{
	if (!bIsAttacking)
	{
		return;
	}

	EndAttackWindow();
	bIsAttacking = false;
	GetWorldTimerManager().ClearTimer(AttackFinishTimerHandle);
	OnAttackFinished(bHitTargetThisAttack);
}


bool AfpstrueEnemyCharacter::IsTargetInAttackRange() const
{
	if (TargetCharacter == nullptr)
	{
		return false;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	return HorizontalToTarget.Size() <= AttackRange;
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
	bDamageAppliedThisAttack = true;
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
	}

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	OnEnemyDeathReported.Broadcast(this);
	OnEnemyDied();

	if (bDestroyOnDeath)
	{
		SetLifeSpan(DestroyDelay);
	}
}
