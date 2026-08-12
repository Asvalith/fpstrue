// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "fpstrueCharacter.generated.h"

class UCameraComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UInputComponent;
class UInputMappingContext;
class USkeletalMeshComponent;
class USpringArmComponent;
class UfpstrueHealthComponent;
class UfpstrueWeaponComponent;
class AfpstrueCharacter;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerDeathReported, AfpstrueCharacter*, DeadPlayer);

UENUM(BlueprintType)
enum class EFPCharacterState : uint8
{
	Idle       UMETA(DisplayName = "Idle"),
	Moving     UMETA(DisplayName = "Moving"),
	// Kept hidden so existing Blueprint enum pins can load; weapon state now owns reloading.
	Reloading  UMETA(Hidden),
	Dead       UMETA(DisplayName = "Dead")
};

UCLASS(config=Game)
class FPSTRUE_API AfpstrueCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AfpstrueCharacter();
	virtual void BeginPlay() override;

	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void RequestReload();

	void SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent);
	void ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent);

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasEquippedWeapon() const { return EquippedWeaponComponent != nullptr; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	UfpstrueWeaponComponent* GetEquippedWeaponComponent() const { return EquippedWeaponComponent; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsReloading() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool CanFireWeapon() const;

	// Compatibility getters forward to the equipped weapon; Character stores no ammo state.
	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetCurrentAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetMagazineSize() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetReserveAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsFiring() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsAiming() const { return bIsAiming; }

	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetCurrentHealth() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthNormalized() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	UfpstrueHealthComponent* GetHealthComponent() const { return HealthComponent; }

	UPROPERTY(BlueprintAssignable, Category = "Health|Events")
	FOnPlayerDeathReported OnPlayerDeathReported;

	UFUNCTION(BlueprintPure, Category = "State")
	EFPCharacterState GetCharacterState() const;

	UFUNCTION(BlueprintPure, Category = "Movement")
	bool IsSprinting() const { return bIsSprinting; }

	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void NotifyControllerChanged() override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;

	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void StartWeaponFire();
	void StopWeaponFire();
	void StartReload();
	void StartSprint();
	void StopSprint();
	void StartAim();
	void StopAim();

	UFUNCTION()
	void HandleHealthChanged(float NewHealth);

	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION()
	void HandleDeath();

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnAimChanged(bool bNewIsAiming);

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnWeaponEquipped(UfpstrueWeaponComponent* WeaponComponent);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerHealthChanged(float NewHealth);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDied();

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> Mesh1P;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FirstPersonCameraComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> FireAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> ReloadAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> RunAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> AimAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UfpstrueHealthComponent> HealthComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float WalkSpeed = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float SprintSpeed = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float AimWalkSpeed = 120.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	bool bIsSprinting = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon", meta = (AllowPrivateAccess = "true"))
	bool bIsAiming = false;

	UPROPERTY(Transient)
	TObjectPtr<UfpstrueWeaponComponent> EquippedWeaponComponent;

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;

	void ApplyInputMappingContexts();
	void RemoveInputMappingContexts();

	bool bDeathHandled = false;
};
