// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "fpstrueReloadReentryObserver.generated.h"

class UfpstrueWeaponComponent;

// A reflected receiver is required by OnAmmoChanged's dynamic multicast signature.
// Only automation tests instantiate this transient object; it adds no gameplay API.
UCLASS(Transient, NotBlueprintable)
class UfpstrueReloadReentryObserver : public UObject
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<UfpstrueWeaponComponent> Weapon;
	int32 CallbackCount = 0;
	bool bDisableWeaponOnAmmoChanged = false;

	UFUNCTION()
	void HandleAmmoChanged(int32 CurrentAmmo, int32 MagazineSize, int32 ReserveAmmo);
};
