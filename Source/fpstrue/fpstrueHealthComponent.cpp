// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueHealthComponent.h"

UfpstrueHealthComponent::UfpstrueHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	ResetHealth();
}

void UfpstrueHealthComponent::ApplyDamage(float DamageAmount)
{
	if (DamageAmount <= 0.0f || IsDead())
	{
		return;
	}

	CurrentHealth = FMath::Clamp(CurrentHealth - DamageAmount, 0.0f, MaxHealth);
	OnHealthChanged.Broadcast(CurrentHealth);

	if (IsDead())
	{
		OnDeath.Broadcast();
	}
}

void UfpstrueHealthComponent::ResetHealth()
{
	CurrentHealth = MaxHealth;
	OnHealthChanged.Broadcast(CurrentHealth);
}
