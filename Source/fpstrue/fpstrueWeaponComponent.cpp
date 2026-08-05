// Copyright Epic Games, Inc. All Rights Reserved.


#include "fpstrueWeaponComponent.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueWeaponDataAsset.h"
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

#define FPSTRUE_ENABLE_TEST_WEAPON_TRACE_DEBUG 0


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
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;
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
		const float RecoilMultiplier = Character->IsAiming() ? GetConfiguredAimRecoilMultiplier() : 1.0f;
		const float Pitch = GetConfiguredRecoilPitch();
		const float Yaw = GetConfiguredRecoilYaw();
		PlayerController->AddPitchInput(-Pitch * RecoilMultiplier);
		PlayerController->AddYawInput(FMath::FRandRange(-Yaw, Yaw) * RecoilMultiplier);
	}

}

bool UfpstrueWeaponComponent::CanFire() const
{
	return bIsEquipped
		&& bWeaponGameplayEnabled
		&& Character != nullptr
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
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;
	OnWeaponFireStopped.Broadcast();
}

void UfpstrueWeaponComponent::HandleOwnerDeath()
{
	bWeaponGameplayEnabled = false;
	StopFire();
}

void UfpstrueWeaponComponent::FireLineTrace(UWorld* World, UCameraComponent* Camera)
{
	const double CurrentTimeSeconds = World->GetTimeSeconds();
	if (LastShotTimeSeconds < 0.0
		|| CurrentTimeSeconds - LastShotTimeSeconds > GetConfiguredSpreadResetDelay())
	{
		ConsecutiveShotCount = 0;
	}

	const float ContinuousSpreadAngle = FMath::Clamp(
		ConsecutiveShotCount * GetConfiguredContinuousFireSpreadStep(),
		0.0f,
		GetConfiguredMaxContinuousFireSpreadAngle());
	const float SpreadAngle = GetConfiguredSpreadAngle(Character->IsAiming()) + ContinuousSpreadAngle;
	LastShotTimeSeconds = CurrentTimeSeconds;
	++ConsecutiveShotCount;

	const int32 TraceCount = FMath::Max(1, GetTraceCount());
	for (int32 TraceIndex = 0; TraceIndex < TraceCount; ++TraceIndex)
	{
		FireSingleLineTrace(World, Camera, SpreadAngle);
	}
}

void UfpstrueWeaponComponent::FireSingleLineTrace(UWorld* World, UCameraComponent* Camera, float SpreadAngle)
{
	const FVector Start = Camera->GetComponentLocation();
	const FVector Forward = Camera->GetForwardVector();
	FVector ShotDirection = Forward;
	if (SpreadAngle > 0.0f)
	{
		const float SpreadRadius = FMath::Tan(FMath::DegreesToRadians(SpreadAngle));
		const FVector2D SpreadOffset = FMath::RandPointInCircle(SpreadRadius);
		ShotDirection = (
			Forward
			+ Camera->GetRightVector() * SpreadOffset.X
			+ Camera->GetUpVector() * SpreadOffset.Y).GetSafeNormal();
	}
	const FVector End = Start + ShotDirection * GetConfiguredTraceRange();

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

#if FPSTRUE_ENABLE_TEST_WEAPON_TRACE_DEBUG
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
#endif

	if (bHit)
	{
#if FPSTRUE_ENABLE_TEST_WEAPON_TRACE_DEBUG
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
#endif

		if (AfpstrueEnemyCharacter* HitEnemy = Cast<AfpstrueEnemyCharacter>(HitResult.GetActor()))
		{
			const FString HitBoneName = HitResult.BoneName.ToString().ToLower();
			const bool bHeadShot = HitBoneName == TEXT("neck_01") || HitBoneName == TEXT("head");
			const float DamageToApply = bHeadShot ? GetConfiguredHeadDamage() : GetConfiguredBodyDamage();

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
				HitComponent->AddImpulseAtLocation(ShotDirection * GetConfiguredTraceImpulse(), HitResult.ImpactPoint);
			}
		}

#if FPSTRUE_ENABLE_TEST_WEAPON_TRACE_DEBUG
		if (bShowDebugTrace && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				7,
				1.0f,
				FColor::Yellow,
				FString::Printf(TEXT("Hit: %s"), *GetNameSafe(HitResult.GetActor()))
			);
		}
#endif
	}
#if FPSTRUE_ENABLE_TEST_WEAPON_TRACE_DEBUG
	else if (bShowDebugTrace && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			7,
			1.0f,
			FColor::Red,
			TEXT("LineTrace missed")
		);
	}
#endif
}

int32 UfpstrueWeaponComponent::GetTraceCount() const
{
	return 1;
}

void UfpstrueWeaponComponent::ApplyWeaponConfiguration(AfpstrueCharacter* TargetCharacter) const
{
	if (WeaponData == nullptr || TargetCharacter == nullptr)
	{
		return;
	}

	TargetCharacter->ConfigureAmmoFromWeapon(
		WeaponData->MagazineSize,
		WeaponData->StartingReserveAmmo,
		WeaponData->ReloadDuration,
		WeaponData->EmptyReloadDuration
	);
}

float UfpstrueWeaponComponent::GetConfiguredTraceRange() const
{
	return WeaponData != nullptr ? WeaponData->TraceRange : LineTraceRange;
}

float UfpstrueWeaponComponent::GetConfiguredTraceImpulse() const
{
	return WeaponData != nullptr ? WeaponData->TraceImpulse : LineTraceImpulse;
}

float UfpstrueWeaponComponent::GetConfiguredBodyDamage() const
{
	return WeaponData != nullptr ? WeaponData->BodyDamage : LineTraceDamage;
}

float UfpstrueWeaponComponent::GetConfiguredHeadDamage() const
{
	return WeaponData != nullptr ? WeaponData->HeadDamage : LineTraceHeadDamage;
}

float UfpstrueWeaponComponent::GetConfiguredSpreadAngle(bool bAiming) const
{
	if (WeaponData != nullptr)
	{
		return bAiming ? WeaponData->AimFireSpreadAngle : WeaponData->HipFireSpreadAngle;
	}

	return bAiming ? AimFireSpreadAngle : HipFireSpreadAngle;
}

float UfpstrueWeaponComponent::GetConfiguredContinuousFireSpreadStep() const
{
	return WeaponData != nullptr ? WeaponData->ContinuousFireSpreadStep : ContinuousFireSpreadStep;
}

float UfpstrueWeaponComponent::GetConfiguredMaxContinuousFireSpreadAngle() const
{
	return WeaponData != nullptr ? WeaponData->MaxContinuousFireSpreadAngle : MaxContinuousFireSpreadAngle;
}

float UfpstrueWeaponComponent::GetConfiguredSpreadResetDelay() const
{
	return WeaponData != nullptr ? WeaponData->SpreadResetDelay : SpreadResetDelay;
}

float UfpstrueWeaponComponent::GetConfiguredRecoilPitch() const
{
	return WeaponData != nullptr ? WeaponData->RecoilPitch : RecoilPitch;
}

float UfpstrueWeaponComponent::GetConfiguredRecoilYaw() const
{
	return WeaponData != nullptr ? WeaponData->RecoilYaw : RecoilYaw;
}

float UfpstrueWeaponComponent::GetConfiguredAimRecoilMultiplier() const
{
	return WeaponData != nullptr ? WeaponData->AimRecoilMultiplier : AimRecoilMultiplier;
}

bool UfpstrueWeaponComponent::AttachWeapon(AfpstrueCharacter* TargetCharacter)
{
	if (TargetCharacter == nullptr || TargetCharacter->IsDead())
	{
		return false;
	}

	if (bIsEquipped)
	{
		return Character == TargetCharacter;
	}

	if (TargetCharacter->HasEquippedWeapon())
	{
		return false;
	}

	Character = TargetCharacter;

	FAttachmentTransformRules AttachmentRules(EAttachmentRule::SnapToTarget, true);
	if (!AttachToComponent(Character->GetMesh1P(), AttachmentRules, FName(TEXT("GripPoint"))))
	{
		Character = nullptr;
		return false;
	}

	bIsEquipped = true;
	bWeaponGameplayEnabled = true;
	ApplyWeaponConfiguration(TargetCharacter);
	Character->SetEquippedWeaponComponent(this);

	if (APlayerController* PlayerController = Cast<APlayerController>(Character->GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			if (FireMappingContext != nullptr)
			{
				Subsystem->AddMappingContext(FireMappingContext, 1);
			}
		}

		if (FireAction != nullptr)
		{
			if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerController->InputComponent))
			{
				EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &UfpstrueWeaponComponent::StartFire);
				EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &UfpstrueWeaponComponent::StopFire);
				EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Canceled, this, &UfpstrueWeaponComponent::StopFire);
			}
		}
	}

	return true;
}

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Character != nullptr)
	{
		if (APlayerController* PlayerController = Cast<APlayerController>(Character->GetController()))
		{
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
			{
				if (FireMappingContext != nullptr)
				{
					Subsystem->RemoveMappingContext(FireMappingContext);
				}
			}
		}
	}

	bWeaponGameplayEnabled = false;
	bIsEquipped = false;
	Character = nullptr;
	Super::EndPlay(EndPlayReason);
}
