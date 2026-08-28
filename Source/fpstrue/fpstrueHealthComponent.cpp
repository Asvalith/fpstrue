// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueHealthComponent.h"

UfpstrueHealthComponent::UfpstrueHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// Blueprint 覆盖的 MaxHealth 到 BeginPlay 才最终可用；Owner 会在绑定委托后主动读取初始快照。
	ResetHealth();

	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.AddUniqueDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}
}

void UfpstrueHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.RemoveDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}

	Super::EndPlay(EndPlayReason);
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

	if (IsDead() && !bDeathBroadcast)
	{
		bDeathBroadcast = true;
		OnDeath.Broadcast();
	}
}

void UfpstrueHealthComponent::ResetHealth()
{
	MaxHealth = FMath::Max(1.0f, MaxHealth);
	CurrentHealth = MaxHealth;
	bDeathBroadcast = false;
	OnHealthChanged.Broadcast(CurrentHealth);
}

float UfpstrueHealthComponent::GetHealthNormalized() const
{
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}

void UfpstrueHealthComponent::HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType,
													   AController* InstigatedBy, AActor* DamageCauser)
{
	ApplyDamageInternal(Damage, DamageCauser, InstigatedBy);
}
