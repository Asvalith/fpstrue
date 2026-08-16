// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class APlayerController;
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

	UfpstrueWeaponDataAsset* GetWeaponData() const { return WeaponData; }

	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);

	void StartFire();
	void StopFire();

	bool RequestReload();

	bool CommitReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void FinishReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void CancelReload();

	bool IsEquipped() const { return bIsEquipped; }

	bool IsReloading() const { return ActionState == EFPWeaponActionState::Reloading; }

	bool IsFiring() const { return ActionState == EFPWeaponActionState::Firing; }

	bool HasAmmo() const { return CurrentAmmo > 0; }

	bool CanFire() const;

	bool CanReload() const;

	EFPWeaponActionState GetActionState() const { return ActionState; }

	int32 GetCurrentAmmo() const { return CurrentAmmo; }

	int32 GetMagazineSize() const { return MagazineSize; }

	int32 GetReserveAmmo() const { return ReserveAmmo; }

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFirePerformed;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponReloadEvent OnWeaponReloadStarted;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryDelay = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil", meta = (ClampMin = "0.1"))
	float RecoilRecoverySpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float MaxAccumulatedRecoilPitch = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float MaxAccumulatedRecoilYaw = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float ReloadFailSafeDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.0"))
	float ReloadCompletionGracePeriod = 0.1f;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void Fire();
	bool TryConsumeAmmo();
	void InitializeRuntimeState();
	void SetActionState(EFPWeaponActionState NewState);
	void ScheduleReloadTimeout(float DurationSeconds);
	void HandleReloadTimeout(int32 ReloadSequence);
	void FireLineTrace(UWorld* World, UCameraComponent* Camera);
	void FireSingleLineTrace(UWorld* World, UCameraComponent* Camera, float SpreadAngle);
	void ApplyRecoil(APlayerController* PlayerController);
	void UpdateRecoilRecovery();
	void ClearRecoilState();
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
	float GetConfiguredRecoilRecoveryDelay() const;
	float GetConfiguredRecoilRecoverySpeed() const;
	float GetConfiguredMaxAccumulatedRecoilPitch() const;
	float GetConfiguredMaxAccumulatedRecoilYaw() const;
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
	float AccumulatedRecoilPitch = 0.0f;
	float AccumulatedRecoilYaw = 0.0f;
	FTimerHandle AutomaticFireTimerHandle;
	FTimerHandle ReloadTimerHandle;
	FTimerHandle RecoilRecoveryTimerHandle;
};
