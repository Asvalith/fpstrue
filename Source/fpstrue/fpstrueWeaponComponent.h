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
	/** Projectile class to spawn */
	UPROPERTY(EditDefaultsOnly, Category=Projectile)
	TSubclassOf<class AfpstrueProjectile> ProjectileClass;
	/** Sound to play each time we fire */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Gameplay)
	USoundBase* FireSound;
	
	/** AnimMontage to play each time we fire */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Gameplay)
	UAnimMontage* FireAnimation;

	/** Gun muzzle's offset from the characters location */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=Gameplay)
	FVector MuzzleOffset;

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

	
	/** Use LineTrace as the main fire mode. Projectile remains as a fallback. */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	bool bUseLineTrace = true;
	/** Linetrace length*/
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceRange = 10000.0f;

	/** Impulse applied to physics objects hit by LineTrace */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceImpulse = 10000.0f;
	/** Damage applied to actors with a HealthComponent */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceDamage = 25.0f;

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
	void FireProjectile(UWorld* World);

	/** The Character holding this weapon*/
	AfpstrueCharacter* Character;
};
