// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "fpstrueReloadReentryObserver.generated.h"

class UfpstrueWeaponComponent;
class AfpstrueCharacter;

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
	TWeakObjectPtr<AfpstrueCharacter> Player;
	bool bUnequipOnEquipped = false;
	int32 FireEventCount = 0;
	int32 ReloadEndCount = 0;
	EFPReloadEndReason LastReloadEndReason = EFPReloadEndReason::Completed;
	bool bLastReloadCommitted = false;
	bool bRestartReloadOnEnd = false;
	bool bFireOnReloadEnd = false;
	bool bReloadRestartAccepted = false;
	TWeakObjectPtr<UAnimInstance> ReloadAnimInstance;
	TWeakObjectPtr<UAnimMontage> ReloadMontage;
	bool bAmmoChangedWhileMontageActive = false;

	UFUNCTION()
	void HandleAmmoChanged(int32 CurrentAmmo, int32 MagazineSize, int32 ReserveAmmo);
	UFUNCTION()
	void HandleEquippedWeaponChanged(UfpstrueWeaponComponent* EquippedWeapon);
	UFUNCTION()
	void HandleFirePerformed();
	UFUNCTION()
	void HandleReloadEnded(int32 ReloadId, EFPReloadEndReason Reason, bool bAmmoCommitted);
	UFUNCTION()
	void HandleReloadPlaybackRequested(int32 ReloadId, bool bWasEmptyReload);
};
