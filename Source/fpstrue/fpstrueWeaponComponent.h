// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class UCameraComponent;
class UfpstrueWeaponDataAsset;
class UWorld;

UENUM(BlueprintType)
enum class EFPWeaponActionState : uint8
{
	Ready      UMETA(DisplayName = "Ready"),
	Firing     UMETA(DisplayName = "Firing"),
	Reloading  UMETA(DisplayName = "Reloading"),
	Disabled   UMETA(DisplayName = "Disabled")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FWeaponFireEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWeaponReloadEvent, bool, bWasEmptyReload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWeaponActionStateChanged, EFPWeaponActionState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FFPAmmoChanged,
	int32, CurrentAmmo,
	int32, MagazineSize,
	int32, ReserveAmmo
);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(
	FWeaponTraceEvent,
	bool, bHit,
	FVector, TraceStart,
	FVector, TraceEnd,
	FVector, TraceTarget,
	FHitResult, HitResult
);

UCLASS(Blueprintable, BlueprintType, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueWeaponComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:
	UfpstrueWeaponComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Config")
	TObjectPtr<UfpstrueWeaponDataAsset> WeaponData;

	UFUNCTION(BlueprintPure, Category = "Weapon|Config")
	UfpstrueWeaponDataAsset* GetWeaponData() const { return WeaponData; }

	UFUNCTION(BlueprintCallable, Category = "Weapon")
	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);

	void StartFire();
	void StopFire();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool RequestReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool CommitReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void FinishReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void CancelReload();

	// Compatibility entry for existing Blueprints. Input cadence is now owned by this component.
	UFUNCTION(BlueprintCallable, Category = "Weapon", meta = (DeprecatedFunction, DeprecationMessage = "Remove Blueprint fire timers and let WeaponComponent own fire cadence."))
	void Fire();

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsEquipped() const { return bIsEquipped; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsReloading() const { return ActionState == EFPWeaponActionState::Reloading; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsFiring() const { return ActionState == EFPWeaponActionState::Firing; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasAmmo() const { return CurrentAmmo > 0; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool CanFire() const;

	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	bool CanReload() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	EFPWeaponActionState GetActionState() const { return ActionState; }

	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentAmmo() const { return CurrentAmmo; }

	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetMagazineSize() const { return MagazineSize; }

	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetReserveAmmo() const { return ReserveAmmo; }

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FFPAmmoChanged OnAmmoChanged;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponActionStateChanged OnWeaponActionStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFireStarted;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFireStopped;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFirePerformed;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponDryFire;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponReloadEvent OnWeaponReloadStarted;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponReloadFinished;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponReloadCanceled;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponTraceEvent OnWeaponTraceFinished;

	void HandleOwnerDeath();

	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceRange = 10000.0f;

	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceImpulse = 10000.0f;

	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceDamage = 40.0f;

	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceHeadDamage = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread")
	float HipFireSpreadAngle = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread")
	float AimFireSpreadAngle = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float ContinuousFireSpreadStep = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float MaxContinuousFireSpreadAngle = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadResetDelay = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float RecoilPitch = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float RecoilYaw = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float AimRecoilMultiplier = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Weapon|Debug")
	bool bShowDebugTrace = false;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool TryConsumeAmmo();
	void InitializeRuntimeState();
	void SetActionState(EFPWeaponActionState NewState);
	void HandleReloadTimeout(int32 ReloadSequence);
	void BroadcastAmmoChanged();
	void FireLineTrace(UWorld* World, UCameraComponent* Camera);
	void FireSingleLineTrace(UWorld* World, UCameraComponent* Camera, float SpreadAngle);
	int32 GetTraceCount() const;

	float GetConfiguredTraceRange() const;
	float GetConfiguredTraceImpulse() const;
	float GetConfiguredBodyDamage() const;
	float GetConfiguredHeadDamage() const;
	float GetConfiguredSpreadAngle(bool bAiming) const;
	float GetConfiguredContinuousFireSpreadStep() const;
	float GetConfiguredMaxContinuousFireSpreadAngle() const;
	float GetConfiguredSpreadResetDelay() const;
	float GetConfiguredRecoilPitch() const;
	float GetConfiguredRecoilYaw() const;
	float GetConfiguredAimRecoilMultiplier() const;
	float GetConfiguredReloadDuration(bool bEmptyReload) const;
	float GetConfiguredFireInterval() const;
	bool IsAutomatic() const;

	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> Character;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|State", meta = (AllowPrivateAccess = "true"))
	EFPWeaponActionState ActionState = EFPWeaponActionState::Disabled;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo", meta = (AllowPrivateAccess = "true"))
	int32 MagazineSize = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo", meta = (AllowPrivateAccess = "true"))
	int32 CurrentAmmo = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo", meta = (AllowPrivateAccess = "true"))
	int32 ReserveAmmo = 0;

	bool bIsEquipped = false;
	bool bWeaponGameplayEnabled = false;
	bool bRuntimeStateInitialized = false;
	bool bReloadAmmoCommitted = false;
	int32 ActiveReloadSequence = 0;
	int32 ConsecutiveShotCount = 0;
	double LastShotTimeSeconds = -1.0;
	double LastAcceptedShotTimeSeconds = -1.0;
	FTimerHandle AutomaticFireTimerHandle;
	FTimerHandle ReloadTimerHandle;
};
