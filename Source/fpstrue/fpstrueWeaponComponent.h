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

	/** MappingContext */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	class UInputMappingContext* FireMappingContext;

	/** Fire Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	class UInputAction* FireAction;

	/** Sets default values for this component's properties */
	UfpstrueWeaponComponent();

	/** Attaches the actor to a FirstPersonCharacter */
	UFUNCTION(BlueprintCallable, Category="Weapon")
	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);
	/** Linetrace length*/
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceRange = 10000.0f;

	/** Impulse applied to physics objects hit by LineTrace */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceImpulse = 10000.0f;
	/** Damage applied to enemy body hits. */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceDamage = 40.0f;

	/** Damage applied when LineTrace hits an enemy head bone. */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceHeadDamage = 100.0f;

	/** Random bullet spread when hip firing, in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread")
	float HipFireSpreadAngle = 1.5f;

	/** Random bullet spread while aiming, in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Spread")
	float AimFireSpreadAngle = 0.25f;

	/** Upward camera kick applied after each shot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float RecoilPitch = 1.0f;

	/** Random left/right camera kick applied after each shot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float RecoilYaw = 0.4f;

	/** Multiplier applied to recoil while aiming. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon|Recoil")
	float AimRecoilMultiplier = 0.5f;

	/** Draw LineTrace debug lines and hit messages. Disabled for normal gameplay. */
	UPROPERTY(EditAnywhere, Category = "Weapon|Debug")
	bool bShowDebugTrace = false;

	/** Make the weapon Fire a Projectile */
	UFUNCTION(BlueprintCallable, Category="Weapon")
	void Fire();

	/*Add fire VFX interface*/
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

protected:
	/** Ends gameplay for this component. */
	UFUNCTION()
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void StartFire();
	void StopFire();
	bool CanFire() const;
	void FireLineTrace(UWorld* World, UCameraComponent* Camera);

	/** The Character holding this weapon*/
	AfpstrueCharacter* Character;
};
