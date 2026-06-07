// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;

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
	float LineTraceImpulse = 300000.0f;
	/** Damage applied to actors with a HealthComponent */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	float LineTraceDamage = 25.0f;

	/** Make the weapon Fire a Projectile */
	UFUNCTION(BlueprintCallable, Category="Weapon")
	void Fire();

protected:
	/** Ends gameplay for this component. */
	UFUNCTION()
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** The Character holding this weapon*/
	AfpstrueCharacter* Character;
};
