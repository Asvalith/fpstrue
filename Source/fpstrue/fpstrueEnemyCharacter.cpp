// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueCharacter.h"
#include "fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
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

	const FVector ToTarget = TargetCharacter->GetActorLocation() - GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	const float DistanceToTarget = HorizontalToTarget.Size();

	if (DistanceToTarget > ChaseRange)
	{
		return;
	}

	if (DistanceToTarget <= AttackRange)
	{
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

	UGameplayStatics::ApplyDamage(
		TargetCharacter,
		AttackDamage,
		GetController(),
		this,
		nullptr
	);

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

bool AfpstrueEnemyCharacter::CanAttack() const
{
	return TargetCharacter != nullptr
		&& TargetCharacter->GetHealthComponent() != nullptr
		&& !TargetCharacter->GetHealthComponent()->IsDead()
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
	bIsDead = true;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->DisableMovement();
	}

	SetActorEnableCollision(false);

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
