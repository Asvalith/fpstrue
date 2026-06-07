// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "fpstrueHealthComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHealthChanged, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDeath);

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UfpstrueHealthComponent();

	UFUNCTION(BlueprintCallable, Category="Health")
	void ApplyDamage(float DamageAmount);

	UFUNCTION(BlueprintCallable, Category="Health")
	void ResetHealth();

	UFUNCTION(BlueprintPure, Category="Health")
	float GetHealth() const { return CurrentHealth; }

	UFUNCTION(BlueprintPure, Category="Health")
	float GetMaxHealth() const { return MaxHealth; }

	UFUNCTION(BlueprintPure, Category="Health")
	bool IsDead() const { return CurrentHealth <= 0.0f; }

	UPROPERTY(BlueprintAssignable, Category="Health")
	FOnHealthChanged OnHealthChanged;

	UPROPERTY(BlueprintAssignable, Category="Health")
	FOnDeath OnDeath;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Health")
	float MaxHealth = 100.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Health")
	float CurrentHealth = 100.0f;
};
