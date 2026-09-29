#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "fpstrueHealthMutationTestObserver.generated.h"

class UfpstrueHealthComponent;

UCLASS(Transient, NotBlueprintable)
class UfpstrueHealthMutationTestObserver : public UObject
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<UfpstrueHealthComponent> Health;
	bool bResetDuringDamage = false;
	bool bDamageDuringHealthChange = false;
	bool bAlwaysResetDuringHealthChange = false;
	bool bAlternateMaximumDuringHealthChange = false;
	int32 Deaths = 0;
	TArray<float> HealthSnapshots;
	UFUNCTION() void OnDamage(float Amount, AActor* Causer, AController* Instigator);
	UFUNCTION() void OnHealth(float Value);
	UFUNCTION() void OnDeath();
};
