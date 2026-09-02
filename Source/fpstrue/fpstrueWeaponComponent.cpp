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
// 语法复习：FName 适合反复比较的标识符；集中构造可避免每次命中都创建临时 FString 并执行 ToLower。
const FName GripPointSocketName(TEXT("GripPoint"));
const FName NeckBoneName(TEXT("neck_01"));
const FName HeadBoneName(TEXT("head"));

/*
 * 玩家武器的核心状态与射击实现。
 * 组件拥有弹药、开火/换弹互斥状态和后坐力恢复；角色只转发输入，动画通过 Notify 提交换弹，
 * 蓝图事件负责枪口、音效等表现，因此 Gameplay 结算不依赖某个蓝图节点是否执行。
 *
 * 主要调用链：
 *   输入 -> Character::StartWeaponFire -> StartFire -> Fire -> Hitscan -> ApplyPointDamage
 *   输入 -> Character::StartReload -> RequestReload -> 动画 Notify::CommitReload -> FinishReload
 *   Notify 丢失 -> ReloadTimeout -> FinishReload；主动中断 -> CancelReload，确保武器不会永久卡在 Reloading。
 *
 * ActionState、CurrentAmmo、ReserveAmmo 和后坐力累计量都只由本组件写入；Character/HUD 通过只读接口和
 * Delegate 观察结果，从结构上避免蓝图、角色和武器各保存一份可变状态。
 */

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

// ==================== Equipment ====================

bool UfpstrueWeaponComponent::AttachWeapon(AfpstrueCharacter* TargetCharacter)
{
	// 装备是一次性事务：任何前置条件失败都不修改双方状态，挂接成功后才提交角色引用和运行时弹药。
	if (TargetCharacter == nullptr || TargetCharacter->IsDead())
	{
		return false;
	}

	if (AfpstrueCharacter* ExistingCharacter = Character.Get())
	{
		return ExistingCharacter == TargetCharacter;
	}

	if (TargetCharacter->HasEquippedWeapon())
	{
		return false;
	}

	Character = TargetCharacter;

	FAttachmentTransformRules AttachmentRules(EAttachmentRule::SnapToTarget, true);
	if (!AttachToComponent(TargetCharacter->GetMesh1P(), AttachmentRules, GripPointSocketName))
	{
		Character.Reset();
		return false;
	}

	// 成功挂接只会发生一次，直接初始化本实例的运行时弹药。
	MagazineSize = FMath::Max(1, MagazineSize);
	CurrentAmmo = MagazineSize;
	ReserveAmmo = FMath::Max(0, StartingReserveAmmo);
	ActionState = EFPWeaponActionState::Ready;
	TargetCharacter->SetEquippedWeaponComponent(this);
	BroadcastAmmoChanged();

	return true;
}

// ==================== Fire System ====================

void UfpstrueWeaponComponent::StartFire()
{
	// StartFire 只负责进入持续开火流程；每发是否允许执行仍由 Fire/CanFire 再次检查，处理期间状态变化。
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

	ActionState = EFPWeaponActionState::Firing;
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
		ActionState = EFPWeaponActionState::Ready;
	}
}

void UfpstrueWeaponComponent::Fire()
{
	// 每发射击的提交顺序固定为“校验 -> 扣弹 -> 射线/伤害 -> 表现事件”，保证一次请求最多结算一次弹药。
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

	// 语法复习：弱指针解析为局部裸指针后，只在当前 Game Thread 调用栈内临时使用，不保存到下一帧。
	AfpstrueCharacter* OwningCharacter = Character.Get();
	if (OwningCharacter == nullptr)
	{
		StopFire();
		return;
	}

	UCameraComponent* Camera = OwningCharacter->GetFirstPersonCameraComponent();
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

	if (APlayerController* PlayerController = Cast<APlayerController>(OwningCharacter->GetController()))
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
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	if (OwningCharacter == nullptr)
	{
		return;
	}

	const double CurrentTimeSeconds = World->GetTimeSeconds();
	if (LastShotTimeSeconds < 0.0 || CurrentTimeSeconds - LastShotTimeSeconds > SpreadResetDelay)
	{
		ConsecutiveShotCount = 0;
	}

	const float ContinuousSpreadAngle = FMath::Clamp(ConsecutiveShotCount * ContinuousFireSpreadStep, 0.0f, MaxContinuousFireSpreadAngle);
	const float SpreadAngle = (OwningCharacter->IsAiming() ? AimFireSpreadAngle : HipFireSpreadAngle) + ContinuousSpreadAngle;
	LastShotTimeSeconds = CurrentTimeSeconds;
	++ConsecutiveShotCount;

	FireSingleLineTrace(World, Camera, SpreadAngle);
}

void UfpstrueWeaponComponent::FireSingleLineTrace(UWorld* World, UCameraComponent* Camera, float SpreadAngle)
{
	AfpstrueCharacter* OwningCharacter = Character.Get();
	if (OwningCharacter == nullptr)
	{
		return;
	}

	const FVector Start = Camera->GetComponentLocation();
	const FVector Forward = Camera->GetForwardVector();
	const FVector ShotDirection = SpreadAngle > 0.0f ? MakeGaussianSpreadDirection(Forward, SpreadAngle) : Forward;
	const FVector End = Start + ShotDirection * LineTraceRange;

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(OwningCharacter);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bTraceComplex = true;

	const bool bHit = World->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, QueryParams);

	const FVector TraceTarget = bHit ? HitResult.ImpactPoint : End;
	OnWeaponTraceFinished.Broadcast(bHit, Start, End, TraceTarget, HitResult);

	if (bHit)
	{
		if (AActor* HitActor = HitResult.GetActor())
		{
			const FName HitBoneName = HitResult.BoneName;
			const bool bHeadShot = HitBoneName == NeckBoneName || HitBoneName == HeadBoneName;
			const float DamageToApply = bHeadShot ? LineTraceHeadDamage : LineTraceDamage;

			UGameplayStatics::ApplyPointDamage(HitActor, DamageToApply, ShotDirection, HitResult, OwningCharacter->GetController(), GetOwner(),
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
	// Request 只开启换弹事务，不立刻搬运弹药；真正提交点由动画 Notify 决定，使数值变化与装填动作对齐。
	if (!CanReload())
	{
		return false;
	}

	StopFire();
	const bool bWasEmptyReload = CurrentAmmo <= 0;
	bReloadAmmoCommitted = false;
	ActionState = EFPWeaponActionState::Reloading;
	const float ReloadDuration = bWasEmptyReload ? DefaultEmptyReloadDuration : DefaultReloadDuration;
	ScheduleReloadTimeout(FMath::Max(ReloadDuration, ReloadFailSafeDuration));
	OnWeaponReloadStarted.Broadcast(bWasEmptyReload);

	return true;
}

bool UfpstrueWeaponComponent::CommitReload()
{
	// bReloadAmmoCommitted 是单次提交保护：同一轮换弹的重复 Notify 只会第一次搬运弹药。
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
	ActionState = EFPWeaponActionState::Ready;
}

void UfpstrueWeaponComponent::CancelReload()
{
	// 所有中断路径统一恢复 Ready/Disabled 并清 Timer，防止换弹动画中断后留下不可开火状态。
	if (ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}
	bReloadAmmoCommitted = false;
	ActionState = EFPWeaponActionState::Ready;
}

void UfpstrueWeaponComponent::ScheduleReloadTimeout(float DurationSeconds)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	World->GetTimerManager().SetTimer(ReloadTimerHandle, this, &UfpstrueWeaponComponent::FinishReload,
									  FMath::Max(0.01f, DurationSeconds + ReloadCompletionGracePeriod), false);
}

// ==================== Owner / Component Lifecycle ====================

void UfpstrueWeaponComponent::HandleOwnerDeath()
{
	// 玩家死亡属于强制中断：停止连射、取消换弹和后坐力恢复，再把武器置为 Disabled。
	ResetWeaponRuntimeState();
	ActionState = EFPWeaponActionState::Disabled;
}

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetWeaponRuntimeState();

	if (AfpstrueCharacter* OwningCharacter = Character.Get())
	{
		OwningCharacter->ClearEquippedWeaponComponent(this);
	}

	ActionState = EFPWeaponActionState::Disabled;
	Character.Reset();
	Super::EndPlay(EndPlayReason);
}

bool UfpstrueWeaponComponent::IsOperational() const
{
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	return IsValid(OwningCharacter) && ActionState != EFPWeaponActionState::Disabled && !OwningCharacter->IsDead();
}

bool UfpstrueWeaponComponent::CanFire() const
{
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	return IsOperational() && OwningCharacter != nullptr && OwningCharacter->GetController() != nullptr &&
		   ActionState != EFPWeaponActionState::Reloading && CurrentAmmo > 0;
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

	const AfpstrueCharacter* OwningCharacter = Character.Get();
	const float RecoilMultiplier = OwningCharacter != nullptr && OwningCharacter->IsAiming() ? AimRecoilMultiplier : 1.0f;
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
	AfpstrueCharacter* OwningCharacter = Character.Get();
	APlayerController* PlayerController =
		OwningCharacter != nullptr ? Cast<APlayerController>(OwningCharacter->GetController()) : nullptr;
	if (World == nullptr || PlayerController == nullptr || OwningCharacter->IsDead())
	{
		ClearRecoilState();
		return;
	}

	const float NewPitch = FMath::FInterpConstantTo(AccumulatedRecoilPitch, 0.0f, RecoilRecoveryTickInterval, RecoilRecoverySpeed);
	const float NewYaw = FMath::FInterpConstantTo(AccumulatedRecoilYaw, 0.0f, RecoilRecoveryTickInterval, RecoilRecoverySpeed);

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
	// EndPlay 和异常收口复用同一重置入口，Timer 与运行时弱引用在这里成对清理。
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		TimerManager.ClearTimer(AutomaticFireTimerHandle);
		TimerManager.ClearTimer(ReloadTimerHandle);
	}

	ClearRecoilState();
	bReloadAmmoCommitted = false;
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;
	LastAcceptedShotTimeSeconds = -1.0;
}

void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	OnAmmoChanged.Broadcast(CurrentAmmo, MagazineSize, ReserveAmmo);
}
