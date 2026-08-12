// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueHealthComponent.h"

UfpstrueHealthComponent::UfpstrueHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// TODO: Replay the initial snapshot after owner listeners bind; this broadcast currently happens too early.
	ResetHealth();

	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.AddDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}
}

void UfpstrueHealthComponent::ApplyDamage(float DamageAmount)
{
	ApplyDamageInternal(DamageAmount, nullptr, nullptr);
}

void UfpstrueHealthComponent::ApplyDamageInternal(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	if (DamageAmount <= 0.0f || IsDead())
	{
		return;
	}

	const float PreviousHealth = CurrentHealth;
	CurrentHealth = FMath::Clamp(CurrentHealth - DamageAmount, 0.0f, MaxHealth);
	const float AppliedDamage = PreviousHealth - CurrentHealth;

	OnDamageReceived.Broadcast(AppliedDamage, DamageCauser, InstigatedBy);
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

float UfpstrueHealthComponent::GetHealthNormalized() const
{
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}

void UfpstrueHealthComponent::HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType, AController* InstigatedBy, AActor* DamageCauser)
{
	ApplyDamageInternal(Damage, DamageCauser, InstigatedBy);
}
