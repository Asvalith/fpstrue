// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "fpstrueCharacter.generated.h"

class UInputComponent;
class USkeletalMeshComponent;
class UCameraComponent;
class USpringArmComponent;
class UInputAction;
class UInputMappingContext;
class UfpstrueHealthComponent;
class UfpstrueWeaponComponent;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FFPAmmoChanged,
	int32, CurrentAmmo,
	int32, MagazineSize,
	int32, ReserveAmmo
);

UENUM(BlueprintType)
enum class EFPCharacterState : uint8
{
	Idle        UMETA(DisplayName = "Idle"),
	Moving      UMETA(DisplayName = "Moving"),
	Reloading   UMETA(DisplayName = "Reloading"),
	Dead        UMETA(DisplayName = "Dead")
};

UCLASS(config=Game)
class AfpstrueCharacter : public ACharacter
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category=Mesh, meta = (AllowPrivateAccess = "true"))
	USkeletalMeshComponent* Mesh1P;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FirstPersonCameraComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputMappingContext* DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	UInputAction* JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	class UInputAction* LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* ReloadAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* RunAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* AimAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Health, meta = (AllowPrivateAccess = "true"))
	UfpstrueHealthComponent* HealthComponent;
	
public:

	AfpstrueCharacter();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FFPAmmoChanged OnAmmoChanged;

protected:
	void Move(const FInputActionValue& Value);

	void Look(const FInputActionValue& Value);

	void StartReload();

	void StartSprint();
	void StopSprint();
	void StartAim();
	void StopAim();

	void FinishReload();

	bool CanReload() const;

	FString GetReloadBlockReason() const;

	void UpdateCharacterState();

	FString GetCharacterStateString() const;

	void BroadcastAmmoChanged();

	UFUNCTION()
	void HandleHealthChanged(float NewHealth);

	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION()
	void HandleDeath();

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnAimChanged(bool bNewIsAiming);

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnFireStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnFireStopped();

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnWeaponEquipped(UfpstrueWeaponComponent* WeaponComponent);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerHealthChanged(float NewHealth);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDied();

protected:

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 MagazineSize = 30;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 CurrentAmmo = 30;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 ReserveAmmo = 90;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	float ReloadDuration = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	float EmptyReloadDuration = 1.2f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	EFPCharacterState CharacterState = EFPCharacterState::Idle;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float WalkSpeed = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float SprintSpeed = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float AimWalkSpeed = 120.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
	bool bIsSprinting = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
	bool bIsAiming = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
	bool bIsFiring = false;

	FTimerHandle ReloadTimerHandle;

	UPROPERTY(Transient)
	UfpstrueWeaponComponent* EquippedWeaponComponent = nullptr;

protected:
	virtual void NotifyControllerChanged() override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;

public:

	bool IsReloading() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetCurrentHealth() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthNormalized() const;

	UFUNCTION(BlueprintPure, Category = "State")
	EFPCharacterState GetCharacterState() const { return CharacterState; }

	bool HasAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool CanFireWeapon() const;

	bool TryConsumeAmmo();

	void NotifyFireStarted();

	void NotifyFireStopped();


	void RequestReload();

	void SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent);

	void ConfigureAmmoFromWeapon(
		int32 InMagazineSize,
		int32 InReserveAmmo,
		float InReloadDuration,
		float InEmptyReloadDuration
	);

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasEquippedWeapon() const { return EquippedWeaponComponent != nullptr; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetCurrentAmmo() const { return CurrentAmmo; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetMagazineSize() const { return MagazineSize; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetReserveAmmo() const { return ReserveAmmo; }
	UFUNCTION(BlueprintPure, Category = "Health")
	UfpstrueHealthComponent* GetHealthComponent() const { return HealthComponent; }

	UFUNCTION(BlueprintPure, Category = "Movement")
	bool IsSprinting() const { return bIsSprinting; }
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsAiming() const { return bIsAiming; }
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsFiring() const { return bIsFiring; }
	
	
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

};

