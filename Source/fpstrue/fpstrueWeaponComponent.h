// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class UCameraComponent;
class UWorld;

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

UCLASS(Blueprintable, BlueprintType, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class FPSTRUE_API UfpstrueWeaponComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	class UInputMappingContext* FireMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	class UInputAction* FireAction;

	UfpstrueWeaponComponent();

	UFUNCTION(BlueprintCallable, Category="Weapon")
	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float RecoilPitch = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float RecoilYaw = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float AimRecoilMultiplier = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Weapon|Debug")
	bool bShowDebugTrace = false;

	UFUNCTION(BlueprintCallable, Category="Weapon")
	void Fire();
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
	FWeaponTraceEvent OnWeaponTraceFinished;

	void NotifyReloadStarted(bool bWasEmptyReload);
	void NotifyReloadFinished();
	void NotifyFireStoppedByCharacter();
	void HandleOwnerDeath();

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsEquipped() const { return bIsEquipped; }

protected:
	UFUNCTION()
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void StartFire();
	void StopFire();
	bool CanFire() const;
	void FireLineTrace(UWorld* World, UCameraComponent* Camera);

	UPROPERTY(Transient)
	AfpstrueCharacter* Character = nullptr;

	bool bIsEquipped = false;
	bool bWeaponGameplayEnabled = false;
};
