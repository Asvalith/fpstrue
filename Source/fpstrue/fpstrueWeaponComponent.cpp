// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueWeaponComponent.h"
#include "fpstrueCharacter.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"

namespace
{
constexpr float DefaultReloadDuration = 0.8f;
constexpr float DefaultEmptyReloadDuration = 1.2f;
constexpr float DefaultRoundsPerMinute = 600.0f;
constexpr float RecoilRecoveryTickInterval = 1.0f / 60.0f;
constexpr float GaussianSpreadSigmaCount = 3.0f;

// 根据高斯分布生成散布方向，供每发 Hitscan 共用。
FVector MakeGaussianSpreadDirection(const FVector& Forward, float SpreadAngleDegrees)
{
	const FVector AimDirection = Forward.GetSafeNormal();
	if (AimDirection.IsNearlyZero() || SpreadAngleDegrees <= KINDA_SMALL_NUMBER)
	{
		return AimDirection;
	}

	FVector Right;
	FVector Up;
	AimDirection.FindBestAxisVectors(Right, Up);

	const float MaxRadius = FMath::Tan(FMath::DegreesToRadians(SpreadAngleDegrees));
	const float Sigma = MaxRadius / GaussianSpreadSigmaCount;
	const float TruncatedProbability = 1.0f - FMath::Exp(-0.5f * FMath::Square(GaussianSpreadSigmaCount));
	const float Radius = Sigma * FMath::Sqrt(-2.0f * FMath::Loge(1.0f - FMath::FRand() * TruncatedProbability));
	const float Angle = FMath::FRand() * 2.0f * PI;
	const FVector Offset = Right * (FMath::Cos(Angle) * Radius) + Up * (FMath::Sin(Angle) * Radius);

	return (AimDirection + Offset).GetSafeNormal();
}
} // namespace

// ==================== Construction ====================

UfpstrueWeaponComponent::UfpstrueWeaponComponent() {}

// ==================== Equipment ====================

bool UfpstrueWeaponComponent::AttachWeapon(AfpstrueCharacter* TargetCharacter)
{
	if (TargetCharacter == nullptr || TargetCharacter->IsDead())
	{
		return false;
	}

	if (Character != nullptr)
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

	InitializeRuntimeState();
	SetActionState(EFPWeaponActionState::Ready);
	Character->SetEquippedWeaponComponent(this);
	BroadcastAmmoChanged();

	return true;
}

// ==================== Fire System ====================

void UfpstrueWeaponComponent::StartFire()
{
	if (!CanFire())
	{
		if (!HasAmmo() && RequestReload())
		{
			return;
		}

		StopFire();
		return;
	}

	if (ActionState == EFPWeaponActionState::Firing)
	{
		return;
	}

	SetActionState(EFPWeaponActionState::Firing);
	Fire();

	if (ActionState == EFPWeaponActionState::Firing && HasAmmo())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(AutomaticFireTimerHandle, this, &UfpstrueWeaponComponent::Fire,
											  60.0f / DefaultRoundsPerMinute, true);
		}
	}
}

void UfpstrueWeaponComponent::StopFire()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutomaticFireTimerHandle);
	}

	if (ActionState == EFPWeaponActionState::Firing)
	{
		SetActionState(EFPWeaponActionState::Ready);
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
		if (!HasAmmo() && RequestReload())
		{
			return;
		}

		StopFire();
		return;
	}

	UCameraComponent* Camera = Character->GetFirstPersonCameraComponent();
	if (Camera == nullptr)
	{
		StopFire();
		return;
	}

	const double CurrentTimeSeconds = World->GetTimeSeconds();
	const double FireInterval = 60.0 / DefaultRoundsPerMinute;
	if (LastAcceptedShotTimeSeconds >= 0.0 && CurrentTimeSeconds - LastAcceptedShotTimeSeconds + KINDA_SMALL_NUMBER < FireInterval)
	{
		return;
	}
	LastAcceptedShotTimeSeconds = CurrentTimeSeconds;

	if (!TryConsumeAmmo())
	{
		if (!RequestReload())
		{
			StopFire();
		}
		return;
	}

	OnWeaponFirePerformed.Broadcast();
	FireLineTrace(World, Camera);

	if (APlayerController* PlayerController = Cast<APlayerController>(Character->GetController()))
	{
		ApplyRecoil(PlayerController);
	}

	//最后一发完成命中和表现后再退出射击状态，避免Firing残留
	if (!HasAmmo() && !RequestReload())
	{
		StopFire();
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

void UfpstrueWeaponComponent::FireLineTrace(UWorld* World, UCameraComponent* Camera)
{
	const double CurrentTimeSeconds = World->GetTimeSeconds();
	if (LastShotTimeSeconds < 0.0 || CurrentTimeSeconds - LastShotTimeSeconds > SpreadResetDelay)
	{
		ConsecutiveShotCount = 0;
	}

	const float ContinuousSpreadAngle = FMath::Clamp(ConsecutiveShotCount * ContinuousFireSpreadStep, 0.0f, MaxContinuousFireSpreadAngle);
	const float SpreadAngle = (Character->IsAiming() ? AimFireSpreadAngle : HipFireSpreadAngle) + ContinuousSpreadAngle;
	LastShotTimeSeconds = CurrentTimeSeconds;
	++ConsecutiveShotCount;

	FireSingleLineTrace(World, Camera, SpreadAngle);
}

void UfpstrueWeaponComponent::FireSingleLineTrace(UWorld* World, UCameraComponent* Camera, float SpreadAngle)
{
	const FVector Start = Camera->GetComponentLocation();
	const FVector Forward = Camera->GetForwardVector();
	const FVector ShotDirection = SpreadAngle > 0.0f ? MakeGaussianSpreadDirection(Forward, SpreadAngle) : Forward;
	const FVector End = Start + ShotDirection * LineTraceRange;

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Character);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bTraceComplex = true;

	const bool bHit = World->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, QueryParams);

	const FVector TraceTarget = bHit ? HitResult.ImpactPoint : End;
	OnWeaponTraceFinished.Broadcast(bHit, Start, End, TraceTarget, HitResult);

	if (bHit)
	{
		if (AActor* HitActor = HitResult.GetActor())
		{
			const FString HitBoneName = HitResult.BoneName.ToString().ToLower();
			const bool bHeadShot = HitBoneName == TEXT("neck_01") || HitBoneName == TEXT("head");
			const float DamageToApply = bHeadShot ? LineTraceHeadDamage : LineTraceDamage;

			UGameplayStatics::ApplyPointDamage(HitActor, DamageToApply, ShotDirection, HitResult, Character->GetController(), GetOwner(),
											   nullptr);

			if (!HitActor->IsA<ACharacter>())
			{
				if (UPrimitiveComponent* HitComponent = HitResult.GetComponent())
				{
					if (HitComponent->IsSimulatingPhysics())
					{
						HitComponent->AddImpulseAtLocation(ShotDirection * LineTraceImpulse, HitResult.ImpactPoint);
					}
				}
			}
		}
	}
}

// ==================== Reload System ====================

bool UfpstrueWeaponComponent::RequestReload()
{
	if (!CanReload())
	{
		return false;
	}

	StopFire();
	const bool bWasEmptyReload = CurrentAmmo <= 0;
	InvalidateReloadTransaction();
	SetActionState(EFPWeaponActionState::Reloading);
	const float ReloadDuration = bWasEmptyReload ? DefaultEmptyReloadDuration : DefaultReloadDuration;
	ScheduleReloadTimeout(FMath::Max(ReloadDuration, ReloadFailSafeDuration));
	OnWeaponReloadStarted.Broadcast(bWasEmptyReload);

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
	InvalidateReloadTransaction();
	SetActionState(EFPWeaponActionState::Ready);
}

void UfpstrueWeaponComponent::InvalidateReloadTransaction()
{
	++ActiveReloadSequence;
	bReloadAmmoCommitted = false;
}

void UfpstrueWeaponComponent::ScheduleReloadTimeout(float DurationSeconds)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	FTimerDelegate ReloadDelegate;
	ReloadDelegate.BindUObject(this, &UfpstrueWeaponComponent::HandleReloadTimeout, ActiveReloadSequence);
	World->GetTimerManager().SetTimer(ReloadTimerHandle, ReloadDelegate, FMath::Max(0.01f, DurationSeconds + ReloadCompletionGracePeriod),
									  false);
}

void UfpstrueWeaponComponent::HandleReloadTimeout(int32 ReloadSequence)
{
	if (ReloadSequence != ActiveReloadSequence || ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	FinishReload();
}

// ==================== Owner / Component Lifecycle ====================

void UfpstrueWeaponComponent::HandleOwnerDeath()
{
	ResetWeaponRuntimeState();
	InvalidateReloadTransaction();
	SetActionState(EFPWeaponActionState::Disabled);
}

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetWeaponRuntimeState();
	InvalidateReloadTransaction();

	if (Character != nullptr)
	{
		Character->ClearEquippedWeaponComponent(this);
	}

	SetActionState(EFPWeaponActionState::Disabled);
	Character = nullptr;
	Super::EndPlay(EndPlayReason);
}

// ==================== Action State / Rules ====================

void UfpstrueWeaponComponent::SetActionState(EFPWeaponActionState NewState)
{
	if (ActionState == NewState)
	{
		return;
	}

	ActionState = NewState;
}

bool UfpstrueWeaponComponent::IsOperational() const
{
	return IsValid(Character) && ActionState != EFPWeaponActionState::Disabled && !Character->IsDead();
}

bool UfpstrueWeaponComponent::CanFire() const
{
	return IsOperational() && Character->GetController() != nullptr && ActionState != EFPWeaponActionState::Reloading && CurrentAmmo > 0;
}

bool UfpstrueWeaponComponent::CanReload() const
{
	return IsOperational() && ActionState != EFPWeaponActionState::Reloading && CurrentAmmo < MagazineSize && ReserveAmmo > 0;
}

// ==================== Recoil System ====================

void UfpstrueWeaponComponent::ApplyRecoil(APlayerController* PlayerController)
{
	if (PlayerController == nullptr)
	{
		return;
	}

	const float RecoilMultiplier = Character != nullptr && Character->IsAiming() ? AimRecoilMultiplier : 1.0f;
	const float PitchKick = -RecoilPitch * RecoilMultiplier;
	const float YawKick = FMath::FRandRange(-RecoilYaw, RecoilYaw) * RecoilMultiplier;
	const float NewPitch = FMath::Clamp(AccumulatedRecoilPitch + PitchKick, -MaxAccumulatedRecoilPitch, 0.0f);
	const float NewYaw = FMath::Clamp(AccumulatedRecoilYaw + YawKick, -MaxAccumulatedRecoilYaw, MaxAccumulatedRecoilYaw);

	PlayerController->AddPitchInput(NewPitch - AccumulatedRecoilPitch);
	PlayerController->AddYawInput(NewYaw - AccumulatedRecoilYaw);
	AccumulatedRecoilPitch = NewPitch;
	AccumulatedRecoilYaw = NewYaw;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(RecoilRecoveryTimerHandle, this, &UfpstrueWeaponComponent::UpdateRecoilRecovery,
										  RecoilRecoveryTickInterval, true, RecoilRecoveryDelay);
	}
}

void UfpstrueWeaponComponent::UpdateRecoilRecovery()
{
	UWorld* World = GetWorld();
	APlayerController* PlayerController = Character != nullptr ? Cast<APlayerController>(Character->GetController()) : nullptr;
	if (World == nullptr || PlayerController == nullptr || Character->IsDead())
	{
		ClearRecoilState();
		return;
	}

	const float RecoverySpeed = RecoilRecoverySpeed;
	const float NewPitch = FMath::FInterpConstantTo(AccumulatedRecoilPitch, 0.0f, RecoilRecoveryTickInterval, RecoverySpeed);
	const float NewYaw = FMath::FInterpConstantTo(AccumulatedRecoilYaw, 0.0f, RecoilRecoveryTickInterval, RecoverySpeed);

	PlayerController->AddPitchInput(NewPitch - AccumulatedRecoilPitch);
	PlayerController->AddYawInput(NewYaw - AccumulatedRecoilYaw);
	AccumulatedRecoilPitch = NewPitch;
	AccumulatedRecoilYaw = NewYaw;

	if (FMath::IsNearlyZero(AccumulatedRecoilPitch, KINDA_SMALL_NUMBER) && FMath::IsNearlyZero(AccumulatedRecoilYaw, KINDA_SMALL_NUMBER))
	{
		ClearRecoilState();
	}
}

void UfpstrueWeaponComponent::ClearRecoilState()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RecoilRecoveryTimerHandle);
	}

	AccumulatedRecoilPitch = 0.0f;
	AccumulatedRecoilYaw = 0.0f;
}

// ==================== Runtime Helpers ====================

void UfpstrueWeaponComponent::ResetWeaponRuntimeState()
{
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		TimerManager.ClearTimer(AutomaticFireTimerHandle);
		TimerManager.ClearTimer(ReloadTimerHandle);
	}

	ClearRecoilState();
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;
	LastAcceptedShotTimeSeconds = -1.0;
}

void UfpstrueWeaponComponent::InitializeRuntimeState()
{
	if (bRuntimeStateInitialized)
	{
		return;
	}

	MagazineSize = FMath::Max(1, MagazineSize);
	CurrentAmmo = MagazineSize;
	ReserveAmmo = FMath::Max(0, StartingReserveAmmo);
	bRuntimeStateInitialized = true;
}

void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	OnAmmoChanged.Broadcast(CurrentAmmo, MagazineSize, ReserveAmmo);
}
