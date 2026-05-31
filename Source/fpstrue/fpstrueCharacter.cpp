// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueCharacter.h"
#include "fpstrueProjectile.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "Engine/LocalPlayer.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

//////////////////////////////////////////////////////////////////////////
// AfpstrueCharacter

AfpstrueCharacter::AfpstrueCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
		
	// Create a CameraComponent	
	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(GetCapsuleComponent());
	FirstPersonCameraComponent->SetRelativeLocation(FVector(-10.f, 0.f, 60.f)); // Position the camera
	FirstPersonCameraComponent->bUsePawnControlRotation = true;

	// Create a mesh component that will be used when being viewed from a '1st person' view (when controlling this pawn)
	Mesh1P = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh1P"));
	Mesh1P->SetOnlyOwnerSee(true);
	Mesh1P->SetupAttachment(FirstPersonCameraComponent);
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->CastShadow = false;
	Mesh1P->SetRelativeLocation(FVector(-30.f, 0.f, -150.f));
	//自定义：开启每帧调用Tick()函数
	PrimaryActorTick.bCanEverTick = true;

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

	CharacterState = EFPCharacterState::Reloading;

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			3,
			ReloadDuration,
			FColor::Orange,
			TEXT("Start Reload")
		);
	}

	UE_LOG(LogTemplateCharacter, Warning, TEXT("Start Reload"));

	GetWorldTimerManager().SetTimer(
		ReloadTimerHandle,
		this,
		&AfpstrueCharacter::FinishReload,
		ReloadDuration,
		false
	);
}

void AfpstrueCharacter::FinishReload()
{
	const int32 AmmoNeeded = MagazineSize - CurrentAmmo;
	const int32 AmmoToLoad = FMath::Min(AmmoNeeded, ReserveAmmo);

	CurrentAmmo += AmmoToLoad;
	ReserveAmmo -= AmmoToLoad;

	// 换弹已经结束，先退出 Reloading 状态
	CharacterState = EFPCharacterState::Idle;

	// 然后根据当前速度修正成 Idle 或 Moving
	UpdateCharacterState();

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
