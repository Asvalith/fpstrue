// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueCharacter.h"
#include "fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
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
		HealthComponent->OnHealthChanged.AddDynamic(this, &AfpstrueEnemyCharacter::HandleHealthChanged);
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
		UpdateEnemy(DeltaTime);
	}
}

void AfpstrueEnemyCharacter::UpdateEnemy(float DeltaTime)
{
	if (TargetCharacter == nullptr)
	{
		TargetCharacter = Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
	}

	if (TargetCharacter == nullptr)
	{
		return;
	}

	if (TargetCharacter->IsDead())
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

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}

	OnAttackStarted();

	GetWorldTimerManager().SetTimer(
		AttackDamageTimerHandle,
		this,
		&AfpstrueEnemyCharacter::ApplyAttackDamage,
		AttackDamageDelay,
		false
	);

	GetWorldTimerManager().SetTimer(
		AttackFinishTimerHandle,
		this,
		&AfpstrueEnemyCharacter::FinishAttack,
		AttackAnimationDuration,
		false
	);
}

void AfpstrueEnemyCharacter::ApplyAttackDamage()
{
	if (bIsDead || TargetCharacter == nullptr || TargetCharacter->IsDead())
	{
		return;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	const float DistanceToTarget = HorizontalToTarget.Size();

	if (!IsTargetInAttackRange())
	{
		return;
	}

	UGameplayStatics::ApplyDamage(
		TargetCharacter,
		AttackDamage,
		GetController(),
		this,
		nullptr
	);

	OnAttackLanded();

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			12,
			0.8f,
			FColor::Orange,
			FString::Printf(TEXT("%s Attack Player: %.0f"), *GetName(), AttackDamage)
		);
	}
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

void AfpstrueEnemyCharacter::HandleHealthChanged(float NewHealth)
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			13,
			1.0f,
			FColor::Purple,
			FString::Printf(TEXT("%s Health: %.0f"), *GetName(), NewHealth)
		);
	}
}

void AfpstrueEnemyCharacter::HandleDeath()
{
	if (bIsDead)
	{
		return;
	}

	bIsDead = true;
	bIsAttacking = false;
	GetWorldTimerManager().ClearTimer(AttackDamageTimerHandle);
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

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			14,
			DestroyDelay,
			FColor::Red,
			FString::Printf(TEXT("%s Dead"), *GetName())
		);
	}

	if (bDestroyOnDeath)
	{
		SetLifeSpan(DestroyDelay);
	}
}
