// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueCharacter.h"
#include "fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

AfpstrueEnemyCharacter::AfpstrueEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 96.0f);

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.AddDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = MoveSpeed;
	}

	TargetCharacter = Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
}

void AfpstrueEnemyCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	TimeSinceLastAttack += DeltaTime;

	if (!bIsDead)
	{
		UpdateEnemy();
	}
}

void AfpstrueEnemyCharacter::UpdateEnemy()
{
	if (TargetCharacter == nullptr)
	{
		TargetCharacter = Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
	}

	if (TargetCharacter == nullptr || TargetCharacter->IsDead())
	{
		return;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	const float DistanceToTarget = HorizontalToTarget.Size();

	if (bIsAttacking)
	{
		if (!HorizontalToTarget.IsNearlyZero())
		{
			SetActorRotation(HorizontalToTarget.GetSafeNormal().Rotation());
		}
		return;
	}

	if (DistanceToTarget > ChaseRange)
	{
		return;
	}

	if (IsTargetInAttackRange())
	{
		if (!HorizontalToTarget.IsNearlyZero())
		{
			SetActorRotation(HorizontalToTarget.GetSafeNormal().Rotation());
		}
		TryAttackTarget();
		return;
	}

	if (!HorizontalToTarget.IsNearlyZero())
	{
		MoveTowardTarget(HorizontalToTarget.GetSafeNormal());
	}
}
void AfpstrueEnemyCharacter::MoveTowardTarget(const FVector& DirectionToTarget)
{
	AddMovementInput(DirectionToTarget, 1.0f, true);
	SetActorRotation(DirectionToTarget.Rotation());
}

void AfpstrueEnemyCharacter::TryAttackTarget()
{
	if (!CanAttack())
	{
		return;
	}

	TimeSinceLastAttack = 0.0f;
	bIsAttacking = true;
	bDamageAppliedThisAttack = false;

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
}

void AfpstrueEnemyCharacter::HandleAttackHitNotify()
{
	if (bIsDead || !bIsAttacking || bDamageAppliedThisAttack)
	{
		return;
	}

	// Each attack animation gets exactly one authoritative damage opportunity.
	bDamageAppliedThisAttack = true;
	PerformMeleeHit();
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

			const float AppliedDamage = UGameplayStatics::ApplyDamage(
				TargetCharacter,
				AttackDamage,
				GetController(),
				this,
				nullptr
			);

			bHitPlayer = AppliedDamage > 0.0f;
			if (bHitPlayer)
			{
				OnAttackLanded();
			}
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

void AfpstrueEnemyCharacter::FinishAttack()
{
	bIsAttacking = false;
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
	return TargetCharacter != nullptr
		&& TargetCharacter->GetHealthComponent() != nullptr
		&& !TargetCharacter->GetHealthComponent()->IsDead()
		&& !bIsDead
		&& !bIsAttacking
		&& IsTargetInAttackRange()
		&& TimeSinceLastAttack >= AttackInterval;
}

void AfpstrueEnemyCharacter::HandleDeath()
{
	if (bIsDead)
	{
		return;
	}

	bIsDead = true;
	bIsAttacking = false;
	bDamageAppliedThisAttack = true;
	GetWorldTimerManager().ClearTimer(AttackFinishTimerHandle);

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

	if (bDestroyOnDeath)
	{
		SetLifeSpan(DestroyDelay);
	}
}
