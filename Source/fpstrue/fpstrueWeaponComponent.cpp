// Copyright Epic Games, Inc. All Rights Reserved.


#include "fpstrueWeaponComponent.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueProjectile.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"


// Sets default values for this component's properties
UfpstrueWeaponComponent::UfpstrueWeaponComponent()
{
}


void UfpstrueWeaponComponent::StartFire()
{
	if (!CanFire())
	{
		return;
	}

	Character->NotifyFireStarted();
	OnWeaponFireStarted.Broadcast();
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

	// Ammo, reload, and death gates live in the character; this is the single shot attempt.
	if (!Character->TryConsumeAmmo())
	{
		OnWeaponDryFire.Broadcast();
		StopFire();
		return;
	}

	OnWeaponFirePerformed.Broadcast();
	FireLineTrace(World, Camera);

	if (APlayerController* PlayerController = Cast<APlayerController>(Character->GetController()))
	{
		const float RecoilMultiplier = Character->IsAiming() ? AimRecoilMultiplier : 1.0f;
		PlayerController->AddPitchInput(-RecoilPitch * RecoilMultiplier);
		PlayerController->AddYawInput(FMath::FRandRange(-RecoilYaw, RecoilYaw) * RecoilMultiplier);
	}

}

bool UfpstrueWeaponComponent::CanFire() const
{
	return Character != nullptr
		&& Character->GetController() != nullptr
		&& !Character->IsDead()
		&& !Character->IsReloading();
}

void UfpstrueWeaponComponent::NotifyReloadStarted(bool bWasEmptyReload)
{
	OnWeaponReloadStarted.Broadcast(bWasEmptyReload);
}

void UfpstrueWeaponComponent::NotifyReloadFinished()
{
	OnWeaponReloadFinished.Broadcast();
}

void UfpstrueWeaponComponent::NotifyFireStoppedByCharacter()
{
	OnWeaponFireStopped.Broadcast();
}

void UfpstrueWeaponComponent::FireLineTrace(UWorld* World, UCameraComponent* Camera)
{
	const FVector Start = Camera->GetComponentLocation();
	const FVector Forward = Camera->GetForwardVector();
	const float SpreadAngle = Character->IsAiming() ? AimFireSpreadAngle : HipFireSpreadAngle;
	const FVector ShotDirection = SpreadAngle > 0.0f
		? FMath::VRandCone(Forward, FMath::DegreesToRadians(SpreadAngle))
		: Forward;
	const FVector End = Start + ShotDirection * LineTraceRange;

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

	const FVector TraceTarget = bHit ? HitResult.ImpactPoint : End;
	OnWeaponTraceFinished.Broadcast(bHit, Start, End, TraceTarget, HitResult);

	if (bShowDebugTrace)
	{
		DrawDebugLine(
			World,
			Start,
			TraceTarget,
			bHit ? FColor::Green : FColor::Red,
			false,
			1.0f,
			0,
			0.0f
		);
	}

	if (bHit)
	{
		if (bShowDebugTrace)
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
		}

		if (AfpstrueEnemyCharacter* HitEnemy = Cast<AfpstrueEnemyCharacter>(HitResult.GetActor()))
		{
			const FString HitBoneName = HitResult.BoneName.ToString().ToLower();
			const bool bHeadShot = HitBoneName == TEXT("neck_01") || HitBoneName == TEXT("head");
			const float DamageToApply = bHeadShot ? LineTraceHeadDamage : LineTraceDamage;

			UGameplayStatics::ApplyPointDamage(
				HitEnemy,
				DamageToApply,
				ShotDirection,
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
				HitComponent->AddImpulseAtLocation(ShotDirection * LineTraceImpulse, HitResult.ImpactPoint);
			}
		}

		if (bShowDebugTrace && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				7,
				1.0f,
				FColor::Yellow,
				FString::Printf(TEXT("Hit: %s"), *GetNameSafe(HitResult.GetActor()))
			);
		}
	}
	else if (bShowDebugTrace && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			7,
			1.0f,
			FColor::Red,
			TEXT("LineTrace missed")
		);
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
	Character->SetEquippedWeaponComponent(this);

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
