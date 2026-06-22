// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstrueProjectile.h"
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

//////////////////////////////////////////////////////////////////////////
// AfpstrueCharacter

AfpstrueCharacter::AfpstrueCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
		
	// Create a camera boom so the first person camera can use camera lag.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	CameraBoom->SetRelativeLocation(FVector(-10.f, 0.f, 60.f));
	CameraBoom->TargetArmLength = 0.0f;
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 3.0f;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 3.0f;

	// Create a CameraComponent	
	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FirstPersonCameraComponent->bUsePawnControlRotation = false;

	// Create a mesh component that will be used when being viewed from a '1st person' view (when controlling this pawn)
	Mesh1P = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh1P"));
	Mesh1P->SetOnlyOwnerSee(true);
	Mesh1P->SetupAttachment(FirstPersonCameraComponent);
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->CastShadow = false;
	Mesh1P->SetRelativeLocation(FVector(-30.f, 0.f, -150.f));
	//Tick()
	PrimaryActorTick.bCanEverTick = true;

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));

}

void AfpstrueCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnHealthChanged.AddDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDeath.AddDynamic(this, &AfpstrueCharacter::HandleDeath);
	}

	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

//////////////////////////////////////////////////////////////////////////// Input

void AfpstrueCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}
}

void AfpstrueCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{	
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Move);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Look);

		// Running
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

		// Aiming
		if (AimAction)
		{
			EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartAim);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("AimAction is NULL. Assign IA_Aim in BP_FirstPersonCharacter."));
		}

		// Reloading
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
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// add movement 
		AddMovementInput(GetActorForwardVector(), MovementVector.Y);
		AddMovementInput(GetActorRightVector(), MovementVector.X);
	}
}

void AfpstrueCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

void AfpstrueCharacter::StartSprint()
{
	if (CharacterState == EFPCharacterState::Dead || CharacterState == EFPCharacterState::Reloading || bIsAiming)
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
	if (CharacterState == EFPCharacterState::Dead || CharacterState == EFPCharacterState::Reloading)
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


//添加Tick函数
void AfpstrueCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateCharacterState();

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			1,
			0.0f,
			FColor::Green,
			FString::Printf(
				TEXT("State: %s | Ammo: %d / %d"),
				*GetCharacterStateString(),
				CurrentAmmo,
				ReserveAmmo
			)
		);
	}
}

//换弹药部分
void AfpstrueCharacter::StartReload()
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			2,
			2.0f,
			FColor::Yellow,
			TEXT("Reload key pressed")
		);
	}
	if (!CanReload())
	{
		const FString Reason = GetReloadBlockReason();
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				3,
				2.0f,
				FColor::Red,
				FString::Printf(TEXT("Cannot reload: %s"), *Reason)
			);
		}

		UE_LOG(LogTemplateCharacter, Warning, TEXT("Cannot reload: %s"), *Reason);
		return;
	}

	const bool bWasEmptyReload = CurrentAmmo <= 0;
	const float ActualReloadDuration = bWasEmptyReload ? EmptyReloadDuration : ReloadDuration;

	bIsAiming = false;
	NotifyFireStopped();
	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->NotifyFireStoppedByCharacter();
	}
	CharacterState = EFPCharacterState::Reloading;

	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->NotifyReloadStarted(bWasEmptyReload);
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			3,
			ActualReloadDuration,
			FColor::Orange,
			TEXT("Start Reload")
		);
	}

	UE_LOG(LogTemplateCharacter, Warning, TEXT("Start Reload"));

	GetWorldTimerManager().SetTimer(
		ReloadTimerHandle,
		this,
		&AfpstrueCharacter::FinishReload,
		ActualReloadDuration,
		false
	);
}

void AfpstrueCharacter::FinishReload()
{
	const int32 AmmoNeeded = MagazineSize - CurrentAmmo;
	const int32 AmmoToLoad = FMath::Min(AmmoNeeded, ReserveAmmo);

	CurrentAmmo += AmmoToLoad;
	ReserveAmmo -= AmmoToLoad;


	CharacterState = EFPCharacterState::Idle;
	UpdateCharacterState();

	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->NotifyReloadFinished();
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			3,
			2.0f,
			FColor::Green,
			FString::Printf(TEXT("Finish Reload: Ammo %d / %d"), CurrentAmmo, ReserveAmmo)
		);
	}

	UE_LOG(LogTemplateCharacter, Warning, TEXT("Finish Reload: CurrentAmmo=%d, ReserveAmmo=%d"), CurrentAmmo, ReserveAmmo);
}

bool AfpstrueCharacter::CanReload() const
{
	if (CharacterState == EFPCharacterState::Reloading)
	{
		return false;
	}
	if (CharacterState == EFPCharacterState::Dead)
	{
		return false;
	}
	if (CurrentAmmo >= MagazineSize)
	{
		return false;
	}
	if (ReserveAmmo <= 0)
	{
		return false;
	}
	return true;
}

FString AfpstrueCharacter::GetReloadBlockReason() const
{
	if (CharacterState == EFPCharacterState::Reloading)
	{
		return TEXT("already reloading");
	}
	if (CharacterState == EFPCharacterState::Dead)
	{
		return TEXT("character is dead");
	}
	if (CurrentAmmo >= MagazineSize)
	{
		return TEXT("magazine is full");
	}
	if (ReserveAmmo <= 0)
	{
		return TEXT("no reserve ammo");
	}
	return TEXT("unknown");
}

void AfpstrueCharacter::UpdateCharacterState()
{
	if (CharacterState == EFPCharacterState::Dead)
	{
		return;
	}

	if (CharacterState == EFPCharacterState::Reloading)
	{
		return;
	}

	const FVector Velocity = GetVelocity();
	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);

	if (HorizontalVelocity.SizeSquared() > 1.0f)
	{
		CharacterState = EFPCharacterState::Moving;
	}
	else
	{
		CharacterState = EFPCharacterState::Idle;
	}
}

FString AfpstrueCharacter::GetCharacterStateString() const
{
	switch (CharacterState)
	{
	case EFPCharacterState::Idle:
		return TEXT("Idle");
	case EFPCharacterState::Moving:
		return TEXT("Moving");
	case EFPCharacterState::Reloading:
		return TEXT("Reloading");
	case EFPCharacterState::Dead:
		return TEXT("Dead");
	default:
		return TEXT("Unknown");
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

void AfpstrueCharacter::HandleDeath()
{
	if (CharacterState == EFPCharacterState::Dead)
	{
		return;
	}

	CharacterState = EFPCharacterState::Dead;
	bIsSprinting = false;

	const bool bWasAiming = bIsAiming;
	bIsAiming = false;
	if (bWasAiming)
	{
		OnAimChanged(false);
	}

	NotifyFireStopped();
	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->NotifyFireStoppedByCharacter();
	}

	GetWorldTimerManager().ClearTimer(ReloadTimerHandle);

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

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
	return CharacterState == EFPCharacterState::Reloading;
}

bool AfpstrueCharacter::HasAmmo() const
{
	return CurrentAmmo > 0;
}

bool AfpstrueCharacter::IsDead() const
{
	return CharacterState == EFPCharacterState::Dead;
}

bool AfpstrueCharacter::CanFireWeapon() const
{
	return CharacterState != EFPCharacterState::Dead
		&& CharacterState != EFPCharacterState::Reloading
		&& CurrentAmmo > 0;
}

bool AfpstrueCharacter::TryConsumeAmmo()
{
	// 换弹期间不能开火
	if (IsReloading())
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				6,
				1.0f,
				FColor::Red,
				TEXT("Cannot fire: reloading")
			);
		}

		return false;
	}

	// 死亡状态不能开火，当前只是预留
	if (CharacterState == EFPCharacterState::Dead)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				6,
				1.0f,
				FColor::Red,
				TEXT("Cannot fire: dead")
			);
		}

		return false;
	}

	// 没有子弹时，自动请求换弹
	if (CurrentAmmo <= 0)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				6,
				1.0f,
				FColor::Red,
				TEXT("No ammo: auto reload")
			);
		}

		RequestReload();
		return false;
	}

	// 成功消耗一发子弹
	CurrentAmmo--;

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			6,
			0.5f,
			FColor::White,
			FString::Printf(
				TEXT("Fire: Ammo %d / %d | Reserve %d"),
				CurrentAmmo,
				MagazineSize,
				ReserveAmmo
			)
		);
	}

	return true;
}

void AfpstrueCharacter::NotifyFireStarted()
{
	if (bIsFiring || CharacterState == EFPCharacterState::Dead || CharacterState == EFPCharacterState::Reloading)
	{
		return;
	}

	bIsFiring = true;
	OnFireStarted();
}

void AfpstrueCharacter::NotifyFireStopped()
{
	if (!bIsFiring)
	{
		return;
	}

	bIsFiring = false;
	OnFireStopped();
}
void AfpstrueCharacter::RequestReload()
{
	StartReload();
}

void AfpstrueCharacter::SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent)
{
	EquippedWeaponComponent = WeaponComponent;
}
