// Copyright Epic Games, Inc. All Rights Reserved.


#include "fpstrueWeaponComponent.h"
#include "fpstrueCharacter.h"
#include "fpstrueProjectile.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Animation/AnimInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"


// Sets default values for this component's properties
UfpstrueWeaponComponent::UfpstrueWeaponComponent()
{
	// Default offset from the character location for projectiles to spawn
	MuzzleOffset = FVector(100.0f, 0.0f, 10.0f);
}


void UfpstrueWeaponComponent::StartFire()
{
	if (CanFire() && Character->CanFireWeapon())
	{
		Character->NotifyFireStarted();
		OnWeaponFireStarted.Broadcast();
	}
}

void UfpstrueWeaponComponent::StopFire()
{
	if (Character != nullptr)
	{
		Character->NotifyFireStopped();
	}
	OnWeaponFireStopped.Broadcast();
}
void UfpstrueWeaponComponent::Fire()
{
	if (!CanFire())
	{
		return;
	}

	UWorld* const World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	UCameraComponent* Camera = Character->GetFirstPersonCameraComponent();
	if (Camera == nullptr)
	{
		return;
	}

	// Check whether the character is allowed to fire before consuming ammo.
	if (!Character->CanFireWeapon())
	{
		return;
	}

	// Ammo check before firing
	if (!Character->TryConsumeAmmo())
	{
		return;
	}

	OnWeaponFirePerformed.Broadcast();


	if (bUseLineTrace)
	{
		FireLineTrace(World, Camera);
	}
	else
	{
		FireProjectile(World);
	}

	PlayFireFeedback();
}

bool UfpstrueWeaponComponent::CanFire() const
{
	return Character != nullptr && Character->GetController() != nullptr;
}

void UfpstrueWeaponComponent::FireLineTrace(UWorld* World, UCameraComponent* Camera)
{
	const FVector Start = Camera->GetComponentLocation();
	const FVector Forward = Camera->GetForwardVector();
	const FVector End = Start + Forward * LineTraceRange;

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Character);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bTraceComplex = true;

	const bool bHit = World->LineTraceSingleByChannel(
		HitResult,
		Start,
		End,
		ECC_Visibility,
		QueryParams
	);

	const FVector DebugEnd = bHit ? HitResult.ImpactPoint : End;
	DrawDebugLine(
		World,
		Start,
		DebugEnd,
		bHit ? FColor::Green : FColor::Red,
		false,
		1.0f,
		0,
		0.0f
	);

	if (bHit)
	{
		DrawDebugSphere(
			World,
			HitResult.ImpactPoint,
			8.0f,
			12,
			FColor::Yellow,
			false,
			1.0f
		);

		if (AActor* HitActor = HitResult.GetActor())
		{
			UGameplayStatics::ApplyPointDamage(
				HitActor,
				LineTraceDamage,
				Forward,
				HitResult,
				Character->GetController(),
				GetOwner(),
				nullptr
			);
		}

		if (UPrimitiveComponent* HitComponent = HitResult.GetComponent())
		{
			if (HitComponent->IsSimulatingPhysics())
			{
				HitComponent->AddImpulseAtLocation(Forward * LineTraceImpulse, HitResult.ImpactPoint);
			}
		}

		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				7,
				1.0f,
				FColor::Yellow,
				FString::Printf(TEXT("Hit: %s"), *GetNameSafe(HitResult.GetActor()))
			);
		}
	}
	else if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			7,
			1.0f,
			FColor::Red,
			TEXT("LineTrace missed")
		);
	}
}

void UfpstrueWeaponComponent::FireProjectile(UWorld* World)
{
	// Try and fire a projectile. Kept as a fallback for later projectile-style weapons.
	if (ProjectileClass != nullptr)
	{
		APlayerController* PlayerController = Cast<APlayerController>(Character->GetController());
		if (PlayerController != nullptr && PlayerController->PlayerCameraManager != nullptr)
		{
			const FRotator SpawnRotation = PlayerController->PlayerCameraManager->GetCameraRotation();
			// MuzzleOffset is in camera space, so transform it to world space before offsetting from the character location to find the final muzzle position
			const FVector SpawnLocation = GetOwner()->GetActorLocation() + SpawnRotation.RotateVector(MuzzleOffset);

			//Set Spawn Collision Handling Override
			FActorSpawnParameters ActorSpawnParams;
			ActorSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

			// Spawn the projectile at the muzzle
			World->SpawnActor<AfpstrueProjectile>(ProjectileClass, SpawnLocation, SpawnRotation, ActorSpawnParams);
		}
	}
}

void UfpstrueWeaponComponent::PlayFireFeedback() const
{
	// Try and play the sound if specified
	if (FireSound != nullptr)
	{
		UGameplayStatics::PlaySoundAtLocation(this, FireSound, Character->GetActorLocation());
	}

	// Try and play a firing animation if specified
	if (FireAnimation != nullptr)
	{
		// Get the animation object for the arms mesh
		UAnimInstance* AnimInstance = Character->GetMesh1P()->GetAnimInstance();
		if (AnimInstance != nullptr)
		{
			AnimInstance->Montage_Play(FireAnimation, 1.f);
		}
	}
}

bool UfpstrueWeaponComponent::AttachWeapon(AfpstrueCharacter* TargetCharacter)
{
	Character = TargetCharacter;

	// Check that the character is valid, and has no weapon component yet
	if (Character == nullptr || Character->GetInstanceComponents().FindItemByClass<UfpstrueWeaponComponent>())
	{
		return false;
	}

	// Attach the weapon to the First Person Character
	FAttachmentTransformRules AttachmentRules(EAttachmentRule::SnapToTarget, true);
	AttachToComponent(Character->GetMesh1P(), AttachmentRules, FName(TEXT("GripPoint")));

	// Set up action bindings
	if (APlayerController* PlayerController = Cast<APlayerController>(Character->GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			// Set the priority of the mapping to 1, so that it overrides the Jump action with the Fire action when using touch input
			Subsystem->AddMappingContext(FireMappingContext, 1);
		}

		if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerController->InputComponent))
		{
			// Fire
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &UfpstrueWeaponComponent::StartFire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Triggered, this, &UfpstrueWeaponComponent::Fire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &UfpstrueWeaponComponent::StopFire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Canceled, this, &UfpstrueWeaponComponent::StopFire);
		}
	}

	return true;
}

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// ensure we have a character owner
	if (Character != nullptr)
	{
		// remove the input mapping context from the Player Controller
		if (APlayerController* PlayerController = Cast<APlayerController>(Character->GetController()))
		{
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
			{
				Subsystem->RemoveMappingContext(FireMappingContext);
			}
		}
	}

	// maintain the EndPlay call chain
	Super::EndPlay(EndPlayReason);
}
