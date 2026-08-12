// Copyright Epic Games, Inc. All Rights Reserved.


#include "fpstrueWeaponComponent.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueWeaponDataAsset.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "TimerManager.h"

#define FPSTRUE_ENABLE_TEST_WEAPON_TRACE_DEBUG 0

namespace
{
constexpr int32 DefaultMagazineSize = 30;
constexpr int32 DefaultReserveAmmo = 90;
constexpr float DefaultReloadDuration = 0.8f;
constexpr float DefaultEmptyReloadDuration = 1.2f;
constexpr float DefaultRoundsPerMinute = 600.0f;

FVector MakeUniformSpreadDirection(const FVector& Forward, float SpreadAngleDegrees)
{
	const FVector AimDirection = Forward.GetSafeNormal();
	if (AimDirection.IsNearlyZero() || SpreadAngleDegrees <= KINDA_SMALL_NUMBER)
	{
		return AimDirection;
	}

	FVector Right;
	FVector Up;
	AimDirection.FindBestAxisVectors(Right, Up);

	const float DiskRadius = FMath::Tan(FMath::DegreesToRadians(SpreadAngleDegrees));
	const float Radius = FMath::Sqrt(FMath::FRand()) * DiskRadius;
	const float Angle = FMath::FRand() * 2.0f * PI;
	const FVector Offset =
		Right * (FMath::Cos(Angle) * Radius)
		+ Up * (FMath::Sin(Angle) * Radius);

	return (AimDirection + Offset).GetSafeNormal();
}
}


UfpstrueWeaponComponent::UfpstrueWeaponComponent()
{
}


void UfpstrueWeaponComponent::StartFire()
{
	if (!CanFire())
	{
		if (bIsEquipped && bWeaponGameplayEnabled && CurrentAmmo <= 0)
		{
			OnWeaponDryFire.Broadcast();
			RequestReload();
		}
		return;
	}

	if (ActionState == EFPWeaponActionState::Firing)
	{
		return;
	}

	SetActionState(EFPWeaponActionState::Firing);
	OnWeaponFireStarted.Broadcast();
	Fire();

	if (IsAutomatic() && ActionState == EFPWeaponActionState::Firing && HasAmmo())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				AutomaticFireTimerHandle,
				this,
				&UfpstrueWeaponComponent::Fire,
				GetConfiguredFireInterval(),
				true
			);
		}
	}
}

void UfpstrueWeaponComponent::StopFire()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutomaticFireTimerHandle);
	}

	const bool bWasFiring = ActionState == EFPWeaponActionState::Firing;
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;

	if (bWasFiring)
	{
		SetActionState(EFPWeaponActionState::Ready);
		OnWeaponFireStopped.Broadcast();
	}
}

void UfpstrueWeaponComponent::Fire()
{
	if (ActionState != EFPWeaponActionState::Firing)
	{
		return;
	}

	UWorld* const World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (!CanFire())
	{
		OnWeaponDryFire.Broadcast();
		StopFire();
		RequestReload();
		return;
	}

	UCameraComponent* Camera = Character->GetFirstPersonCameraComponent();
	if (Camera == nullptr)
	{
		StopFire();
		return;
	}

	const double CurrentTimeSeconds = World->GetTimeSeconds();
	const double FireInterval = GetConfiguredFireInterval();
	if (LastAcceptedShotTimeSeconds >= 0.0
		&& CurrentTimeSeconds - LastAcceptedShotTimeSeconds + KINDA_SMALL_NUMBER < FireInterval)
	{
		return;
	}
	LastAcceptedShotTimeSeconds = CurrentTimeSeconds;

	if (!TryConsumeAmmo())
	{
		OnWeaponDryFire.Broadcast();
		StopFire();
		RequestReload();
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
		&& ActionState != EFPWeaponActionState::Reloading
		&& ActionState != EFPWeaponActionState::Disabled
		&& CurrentAmmo > 0;
}

bool UfpstrueWeaponComponent::CanReload() const
{
	return bIsEquipped
		&& bWeaponGameplayEnabled
		&& Character != nullptr
		&& !Character->IsDead()
		&& ActionState != EFPWeaponActionState::Reloading
		&& ActionState != EFPWeaponActionState::Disabled
		&& CurrentAmmo < MagazineSize
		&& ReserveAmmo > 0;
}

bool UfpstrueWeaponComponent::RequestReload()
{
	if (!CanReload())
	{
		return false;
	}

	StopFire();
	const bool bWasEmptyReload = CurrentAmmo <= 0;
	bReloadAmmoCommitted = false;
	++ActiveReloadSequence;
	SetActionState(EFPWeaponActionState::Reloading);
	OnWeaponReloadStarted.Broadcast(bWasEmptyReload);

	if (UWorld* World = GetWorld())
	{
		FTimerDelegate ReloadDelegate;
		ReloadDelegate.BindUObject(this, &UfpstrueWeaponComponent::HandleReloadTimeout, ActiveReloadSequence);
		World->GetTimerManager().SetTimer(
			ReloadTimerHandle,
			ReloadDelegate,
			GetConfiguredReloadDuration(bWasEmptyReload),
			false
		);
	}

	return true;
}

bool UfpstrueWeaponComponent::CommitReload()
{
	if (ActionState != EFPWeaponActionState::Reloading || bReloadAmmoCommitted)
	{
		return false;
	}

	const int32 AmmoNeeded = MagazineSize - CurrentAmmo;
	const int32 AmmoToLoad = FMath::Min(AmmoNeeded, ReserveAmmo);
	CurrentAmmo += AmmoToLoad;
	ReserveAmmo -= AmmoToLoad;
	bReloadAmmoCommitted = true;
	BroadcastAmmoChanged();
	return true;
}

void UfpstrueWeaponComponent::FinishReload()
{
	if (ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	CommitReload();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}

	SetActionState(EFPWeaponActionState::Ready);
	OnWeaponReloadFinished.Broadcast();
}

void UfpstrueWeaponComponent::CancelReload()
{
	if (ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}

	++ActiveReloadSequence;
	bReloadAmmoCommitted = false;
	SetActionState(EFPWeaponActionState::Ready);
	OnWeaponReloadCanceled.Broadcast();
}

void UfpstrueWeaponComponent::HandleOwnerDeath()
{
	const bool bWasFiring = ActionState == EFPWeaponActionState::Firing;
	const bool bWasReloading = ActionState == EFPWeaponActionState::Reloading;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutomaticFireTimerHandle);
		World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}

	++ActiveReloadSequence;
	bReloadAmmoCommitted = false;
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;
	bWeaponGameplayEnabled = false;
	SetActionState(EFPWeaponActionState::Disabled);

	if (bWasFiring)
	{
		OnWeaponFireStopped.Broadcast();
	}
	if (bWasReloading)
	{
		OnWeaponReloadCanceled.Broadcast();
	}
}

bool UfpstrueWeaponComponent::TryConsumeAmmo()
{
	if (CurrentAmmo <= 0)
	{
		return false;
	}

	--CurrentAmmo;
	BroadcastAmmoChanged();
	return true;
}

void UfpstrueWeaponComponent::InitializeRuntimeState()
{
	if (bRuntimeStateInitialized)
	{
		return;
	}

	MagazineSize = WeaponData != nullptr ? FMath::Max(1, WeaponData->MagazineSize) : DefaultMagazineSize;
	CurrentAmmo = MagazineSize;
	ReserveAmmo = WeaponData != nullptr ? FMath::Max(0, WeaponData->StartingReserveAmmo) : DefaultReserveAmmo;
	bRuntimeStateInitialized = true;
}

void UfpstrueWeaponComponent::SetActionState(EFPWeaponActionState NewState)
{
	if (ActionState == NewState)
	{
		return;
	}

	ActionState = NewState;
	OnWeaponActionStateChanged.Broadcast(ActionState);
}

void UfpstrueWeaponComponent::HandleReloadTimeout(int32 ReloadSequence)
{
	if (ReloadSequence != ActiveReloadSequence || ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	FinishReload();
}

void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	OnAmmoChanged.Broadcast(CurrentAmmo, MagazineSize, ReserveAmmo);
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
	const FVector ShotDirection = SpreadAngle > 0.0f
		? MakeUniformSpreadDirection(Forward, SpreadAngle)
		: Forward;
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

		// TODO: Use a generic damageable contract; hit-zone rules should not require the enemy character class.
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
	if (WeaponData != nullptr && WeaponData->WeaponFamily == EFPWeaponFamily::Shotgun)
	{
		return FMath::Max(1, WeaponData->PelletsPerShot);
	}

	return 1;
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

float UfpstrueWeaponComponent::GetConfiguredReloadDuration(bool bEmptyReload) const
{
	if (WeaponData != nullptr)
	{
		return bEmptyReload ? WeaponData->EmptyReloadDuration : WeaponData->ReloadDuration;
	}

	return bEmptyReload ? DefaultEmptyReloadDuration : DefaultReloadDuration;
}

float UfpstrueWeaponComponent::GetConfiguredFireInterval() const
{
	const float RoundsPerMinute = WeaponData != nullptr
		? FMath::Max(1.0f, WeaponData->RoundsPerMinute)
		: DefaultRoundsPerMinute;
	return 60.0f / RoundsPerMinute;
}

bool UfpstrueWeaponComponent::IsAutomatic() const
{
	return WeaponData == nullptr || WeaponData->bAutomatic;
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
	InitializeRuntimeState();
	SetActionState(EFPWeaponActionState::Ready);
	Character->SetEquippedWeaponComponent(this);
	BroadcastAmmoChanged();

	return true;
}

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutomaticFireTimerHandle);
		World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}

	if (Character != nullptr)
	{
		Character->ClearEquippedWeaponComponent(this);
	}

	bWeaponGameplayEnabled = false;
	bIsEquipped = false;
	SetActionState(EFPWeaponActionState::Disabled);
	Character = nullptr;
	Super::EndPlay(EndPlayReason);
}
