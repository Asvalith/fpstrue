// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstrueWeaponComponent.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "Engine/LocalPlayer.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);


AfpstrueCharacter::AfpstrueCharacter()
{
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
		
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	CameraBoom->SetRelativeLocation(FVector(-10.f, 0.f, 60.f));
	CameraBoom->TargetArmLength = 0.0f;
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 3.0f;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 3.0f;

	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FirstPersonCameraComponent->bUsePawnControlRotation = false;

	Mesh1P = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh1P"));
	Mesh1P->SetOnlyOwnerSee(true);
	Mesh1P->SetupAttachment(FirstPersonCameraComponent);
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->CastShadow = false;
	Mesh1P->SetRelativeLocation(FVector(-30.f, 0.f, -150.f));
	PrimaryActorTick.bCanEverTick = false;

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));

}

void AfpstrueCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnHealthChanged.AddUniqueDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDeath);
	}

	Mesh1P->SetVisibility(EquippedWeaponComponent != nullptr, true);
	Mesh1P->SetHiddenInGame(EquippedWeaponComponent == nullptr, true);
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void AfpstrueCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopWeaponFire();
	RemoveInputMappingContexts();
	if (HealthComponent != nullptr)
	{
		HealthComponent->OnHealthChanged.RemoveDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDamageReceived.RemoveDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueCharacter::HandleDeath);
	}
	Super::EndPlay(EndPlayReason);
}


void AfpstrueCharacter::NotifyControllerChanged()
{
	StopWeaponFire();
	RemoveInputMappingContexts();
	Super::NotifyControllerChanged();

	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			BoundInputSubsystem = Subsystem;
			ApplyInputMappingContexts();
		}
	}
}

void AfpstrueCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{	
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Move);

		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Look);

		if (FireAction)
		{
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartWeaponFire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &AfpstrueCharacter::StopWeaponFire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Canceled, this, &AfpstrueCharacter::StopWeaponFire);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("FireAction is NULL. Assign IA_Shoot in BP_FirstPersonCharacter."));
		}

		if (RunAction)
		{
			EnhancedInputComponent->BindAction(RunAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartSprint);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("RunAction is NULL. Assign IA_Run in BP_FirstPersonCharacter."));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(15, 5.0f, FColor::Red, TEXT("RunAction is NULL. Assign IA_Run in BP_FirstPersonCharacter."));
			}
		}

		if (AimAction)
		{
			EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartAim);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("AimAction is NULL. Assign IA_Aim in BP_FirstPersonCharacter."));
		}

		if (ReloadAction)
		{
			EnhancedInputComponent->BindAction(
				ReloadAction,
				ETriggerEvent::Started,
				this,
				&AfpstrueCharacter::StartReload
			);

			UE_LOG(LogTemplateCharacter, Warning, TEXT("ReloadAction bound successfully."));
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("ReloadAction is NULL. Assign IA_reload in BP_FirstPersonCharacter."));
		}
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error, TEXT("'%s' Failed to find an Enhanced Input Component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}


void AfpstrueCharacter::Move(const FInputActionValue& Value)
{
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		AddMovementInput(GetActorForwardVector(), MovementVector.Y);
		AddMovementInput(GetActorRightVector(), MovementVector.X);
	}
}

void AfpstrueCharacter::Look(const FInputActionValue& Value)
{
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

void AfpstrueCharacter::StartSprint()
{
	if (IsDead() || IsReloading() || bIsAiming)
	{
		return;
	}

	bIsSprinting = !bIsSprinting;
	GetCharacterMovement()->MaxWalkSpeed = bIsSprinting ? SprintSpeed : WalkSpeed;

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			15,
			0.8f,
			bIsSprinting ? FColor::Green : FColor::White,
			FString::Printf(TEXT("%s: %.0f"), bIsSprinting ? TEXT("Sprint") : TEXT("Walk"), GetCharacterMovement()->MaxWalkSpeed)
		);
	}
}

void AfpstrueCharacter::StopSprint()
{
	bIsSprinting = false;
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

void AfpstrueCharacter::StartAim()
{
	if (EquippedWeaponComponent == nullptr
		|| IsDead()
		|| IsReloading())
	{
		return;
	}

	bIsAiming = !bIsAiming;
	OnAimChanged(bIsAiming);

	if (bIsAiming)
	{
		bIsSprinting = false;
		GetCharacterMovement()->MaxWalkSpeed = AimWalkSpeed;
	}
	else
	{
		GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(16, 0.8f, bIsAiming ? FColor::Cyan : FColor::White, bIsAiming ? TEXT("Aim: true") : TEXT("Aim: false"));
	}
}
void AfpstrueCharacter::StopAim()
{
	const bool bWasAiming = bIsAiming;
	bIsAiming = false;

	if (bWasAiming)
	{
		OnAimChanged(false);
		GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(16, 0.8f, FColor::White, TEXT("Aim: false"));
	}
}


void AfpstrueCharacter::StartReload()
{
	RequestReload();
}

void AfpstrueCharacter::StartWeaponFire()
{
	if (EquippedWeaponComponent != nullptr && !IsDead())
	{
		EquippedWeaponComponent->StartFire();
	}
}

void AfpstrueCharacter::StopWeaponFire()
{
	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->StopFire();
	}
}

void AfpstrueCharacter::HandleHealthChanged(float NewHealth)
{
	OnPlayerHealthChanged(NewHealth);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			10,
			1.5f,
			FColor::Cyan,
			FString::Printf(TEXT("Player Health: %.0f"), NewHealth)
		);
	}
}

void AfpstrueCharacter::HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	if (HealthComponent != nullptr && HealthComponent->IsDead())
	{
		return;
	}

	OnPlayerDamaged(DamageAmount, DamageCauser, InstigatedBy);
}

void AfpstrueCharacter::HandleDeath()
{
	if (bDeathHandled)
	{
		return;
	}

	bDeathHandled = true;
	bIsSprinting = false;

	const bool bWasAiming = bIsAiming;
	bIsAiming = false;
	if (bWasAiming)
	{
		OnAimChanged(false);
	}

	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->HandleOwnerDeath();
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

	OnPlayerDeathReported.Broadcast(this);
	OnPlayerDied();

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			11,
			3.0f,
			FColor::Red,
			TEXT("Player Dead")
		);
	}
}

bool AfpstrueCharacter::IsReloading() const
{
	return EquippedWeaponComponent != nullptr && EquippedWeaponComponent->IsReloading();
}

bool AfpstrueCharacter::HasAmmo() const
{
	return EquippedWeaponComponent != nullptr && EquippedWeaponComponent->HasAmmo();
}

bool AfpstrueCharacter::IsDead() const
{
	return bDeathHandled || (HealthComponent != nullptr && HealthComponent->IsDead());
}

float AfpstrueCharacter::GetCurrentHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
}

float AfpstrueCharacter::GetMaxHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetMaxHealth() : 0.0f;
}

float AfpstrueCharacter::GetHealthNormalized() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealthNormalized() : 0.0f;
}

bool AfpstrueCharacter::CanFireWeapon() const
{
	return EquippedWeaponComponent != nullptr && EquippedWeaponComponent->CanFire();
}

void AfpstrueCharacter::RequestReload()
{
	if (EquippedWeaponComponent == nullptr || !EquippedWeaponComponent->CanReload())
	{
		return;
	}

	StopAim();
	StopSprint();
	EquippedWeaponComponent->RequestReload();
}

void AfpstrueCharacter::SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent)
{
	if (WeaponComponent == nullptr || EquippedWeaponComponent == WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent = WeaponComponent;
	Mesh1P->SetHiddenInGame(false, true);
	Mesh1P->SetVisibility(true, true);
	OnWeaponEquipped(WeaponComponent);
}

void AfpstrueCharacter::ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != WeaponComponent)
	{
		return;
	}

	StopWeaponFire();
	EquippedWeaponComponent = nullptr;
	Mesh1P->SetVisibility(false, true);
	Mesh1P->SetHiddenInGame(true, true);
}

void AfpstrueCharacter::ApplyInputMappingContexts()
{
	UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get();
	if (Subsystem == nullptr)
	{
		return;
	}

	if (DefaultMappingContext != nullptr)
	{
		Subsystem->AddMappingContext(DefaultMappingContext, 0);
	}
}

void AfpstrueCharacter::RemoveInputMappingContexts()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
	{
		if (DefaultMappingContext != nullptr)
		{
			Subsystem->RemoveMappingContext(DefaultMappingContext);
		}
	}

	BoundInputSubsystem.Reset();
}

int32 AfpstrueCharacter::GetCurrentAmmo() const
{
	return EquippedWeaponComponent != nullptr ? EquippedWeaponComponent->GetCurrentAmmo() : 0;
}

int32 AfpstrueCharacter::GetMagazineSize() const
{
	return EquippedWeaponComponent != nullptr ? EquippedWeaponComponent->GetMagazineSize() : 0;
}

int32 AfpstrueCharacter::GetReserveAmmo() const
{
	return EquippedWeaponComponent != nullptr ? EquippedWeaponComponent->GetReserveAmmo() : 0;
}

bool AfpstrueCharacter::IsFiring() const
{
	return EquippedWeaponComponent != nullptr && EquippedWeaponComponent->IsFiring();
}

EFPCharacterState AfpstrueCharacter::GetCharacterState() const
{
	if (IsDead())
	{
		return EFPCharacterState::Dead;
	}

	return GetVelocity().SizeSquared2D() > 1.0f
		? EFPCharacterState::Moving
		: EFPCharacterState::Idle;
}
