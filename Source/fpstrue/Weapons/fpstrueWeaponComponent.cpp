// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueCollisionChannels.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"

namespace
{
constexpr float RecoilRecoveryTickInterval = 1.0f / 60.0f;
constexpr float GaussianSpreadSigmaCount = 3.0f;

/*
 * 玩家武器的核心状态与射击实现。
 * 组件拥有弹药、开火/换弹互斥状态和后坐力恢复；角色只转发输入，动画通过 Notify 提交换弹，
 * 蓝图事件负责枪口、音效等表现，因此 Gameplay 结算不依赖某个蓝图节点是否执行。
 *
 * 主要调用链：
 *   输入 -> Character::StartWeaponFire -> StartFire -> Fire -> Hitscan -> ApplyPointDamage
 *   输入 -> Character::RequestWeaponReload -> RequestReload -> 动画 Notify::CommitReload -> FinishReload
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

// 构造默认的关键骨骼集合；可随具体武器蓝图和目标骨架覆盖，不把素材名称写死在射击流程中。
UfpstrueWeaponComponent::UfpstrueWeaponComponent()
{
	// 这里只提供当前 Mannequin 的默认骨骼约定；武器蓝图可针对其他目标骨架覆盖名单。
	CriticalHitBones = {FName(TEXT("neck_01")), FName(TEXT("head"))};
}

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

	USkeletalMeshComponent* TargetMesh = TargetCharacter->GetMesh1P();
	if (TargetMesh == nullptr || GripSocketName.IsNone() || !TargetMesh->DoesSocketExist(GripSocketName))
	{
		UE_LOG(LogTemp, Error, TEXT("AttachWeapon failed: character %s does not provide socket/bone %s."), *GetNameSafe(TargetCharacter),
			   *GripSocketName.ToString());
		return false;
	}

	FAttachmentTransformRules AttachmentRules(EAttachmentRule::SnapToTarget, true);
	if (!AttachToComponent(TargetMesh, AttachmentRules, GripSocketName))
	{
		return false;
	}

	// 所有外部操作成功后才提交角色引用和运行时状态，失败路径不会留下半装备状态。
	Character = TargetCharacter;
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
	// StartFire 只提交 Ready -> Firing；首次和后续每一发的有效性统一由 Fire 校验。
	if (ActionState != EFPWeaponActionState::Ready)
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
											  60.0f / FMath::Max(RoundsPerMinute, 1.0f), true);
		}
	}
}

void UfpstrueWeaponComponent::StopFire()
{
	// 停止自动射击 Timer，并且只把正在开火的状态恢复为 Ready，避免覆盖 Reloading/Disabled。
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
	// 每发射击的提交顺序固定为“校验 -> 扣弹 -> 表现/伤害”，保证一次请求最多结算一次弹药。
	if (ActionState != EFPWeaponActionState::Firing)
	{
		return;
	}

	UWorld* const World = GetWorld();
	if (World == nullptr)
	{
		StopFire();
		return;
	}

	// 空弹匣在解析射击依赖前直接转入统一换弹入口；无备弹时停止连射。
	if (!HasAmmo())
	{
		if (!RequestReload())
		{
			StopFire();
		}
		return;
	}

	// 射击入口先校验持有者，再读取相机与控制器。
	AfpstrueCharacter* OwningCharacter = Character.Get();
	if (!IsValid(OwningCharacter) || OwningCharacter->IsDead() || OwningCharacter->GetController() == nullptr)
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
	const double FireInterval = 60.0 / FMath::Max(static_cast<double>(RoundsPerMinute), 1.0);
	if (LastAcceptedShotTimeSeconds >= 0.0 && CurrentTimeSeconds - LastAcceptedShotTimeSeconds + KINDA_SMALL_NUMBER < FireInterval)
	{
		return;
	}
	LastAcceptedShotTimeSeconds = CurrentTimeSeconds;

	// 弹药校验通过后提交扣弹，再广播 HUD 更新。
	--CurrentAmmo;
	BroadcastAmmoChanged();

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

void UfpstrueWeaponComponent::FireLineTrace(UWorld* World, UCameraComponent* Camera)
{
	// 根据瞄准状态和连续射击次数计算散布，并在同一入口完成单射线查询。
	AfpstrueCharacter* OwningCharacter = Character.Get();
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

	// 从相机发出射击专用射线，返回第一个阻挡命中，并据骨骼名称结算点伤害。
	const FVector Start = Camera->GetComponentLocation();
	const FVector Forward = Camera->GetForwardVector();
	const FVector ShotDirection = SpreadAngle > 0.0f ? MakeGaussianSpreadDirection(Forward, SpreadAngle) : Forward;
	const FVector End = Start + ShotDirection * LineTraceRange;

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(OwningCharacter);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bTraceComplex = true;

	// WeaponTrace 与相机可见性解耦：透明表现、交互射线和玩家子弹可以分别配置响应。
	const bool bHit = World->LineTraceSingleByChannel(HitResult, Start, End, FpstrueCollisionChannels::WeaponTrace, QueryParams);

	const FVector TraceTarget = bHit ? HitResult.ImpactPoint : End;
	OnWeaponTraceFinished.Broadcast(bHit, Start, End, TraceTarget, HitResult);

	// 只有查询真正命中后才读取目标、骨骼和物理组件；未命中只广播弹道终点供表现层使用。
	if (bHit)
	{
		if (AActor* HitActor = HitResult.GetActor())
		{
			const FName HitBoneName = HitResult.BoneName;
			const bool bCriticalHit = CriticalHitBones.Contains(HitBoneName);
			const float DamageToApply = bCriticalHit ? LineTraceHeadDamage : LineTraceDamage;

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

/*
 * Request 负责进入 Reloading 并通知蓝图播放动画；Commit 在装填帧转移弹药，但仍保持 Reloading。
 * 动画正常结束调用 Finish，中断调用 Cancel：Finish 会补交缺失的 Commit，Cancel 不会补交或回滚。
 * 两个动画出口都未触发时，唯一的兜底 Timer 调用 Finish，防止状态永久卡住。
 *
 * Delegate 在当前调用栈同步执行，监听者可能取消换弹、死亡或开始下一次换弹。
 * 因此开始事件之前必须先设置状态和 Timer；Finish 在提交事件返回后还要确认事务序号未变。
 */

bool UfpstrueWeaponComponent::IsOperational() const
{
	// 汇总武器最基础的可用条件，供换弹等规则复用，不在多个入口重复判断角色生命周期。
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	return IsValid(OwningCharacter) && ActionState != EFPWeaponActionState::Disabled && !OwningCharacter->IsDead();
}

bool UfpstrueWeaponComponent::CanReload() const
{
	// 换弹必须同时满足：武器可用、当前不在换弹、弹匣未满且仍有备弹。
	return IsOperational() && ActionState != EFPWeaponActionState::Reloading && CurrentAmmo < MagazineSize && ReserveAmmo > 0;
}

bool UfpstrueWeaponComponent::RequestReload()
{
	// Request 只开启换弹事务，不立刻搬运弹药；真正提交点由动画 Notify 决定，使数值变化与装填动作对齐。
	if (!CanReload())
	{
		return false;
	}

	StopFire();
	const bool bWasEmptyReload = CurrentAmmo <= 0;
	++ReloadSequence;
	bReloadAmmoCommitted = false;
	ActionState = EFPWeaponActionState::Reloading;
	// 先建立本次兜底，再广播；监听者同步 Finish/Cancel 时才能一并清除正确的 Timer。
	const float SelectedReloadDuration = bWasEmptyReload ? EmptyReloadDuration : ReloadDuration;
	// 超时 Timer 是动画 Notify 的容错，不替代正常 Notify；动画链断开时仍能结束 Reloading。
	// 本次请求只有这一处设置 Timer，超时通过 FinishReload 补交弹药并结束流程。
	if (UWorld* World = GetWorld())
	{
		const float Timeout = FMath::Max(0.01f, FMath::Max(SelectedReloadDuration, ReloadFailSafeDuration) + ReloadCompletionGracePeriod);
		World->GetTimerManager().SetTimer(ReloadTimerHandle, this, &UfpstrueWeaponComponent::FinishReload, Timeout, false);
	}
	OnWeaponReloadStarted.Broadcast(bWasEmptyReload);

	// true 表示请求曾被接纳；监听者可能已同步结束换弹，广播之后不再写回状态或重设计时器。
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
	// 先标记已提交再通知 HUD，防止监听者重入 CommitReload 导致重复装弹。
	bReloadAmmoCommitted = true;
	BroadcastAmmoChanged();
	return true;
}

void UfpstrueWeaponComponent::FinishReload()
{
	// 正常结束与超时共用此入口；仅结束进入本函数时的那次换弹。
	if (ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	const uint32 FinishingReloadSequence = ReloadSequence;
	CommitReload();
	// Commit 的同步广播可能已中断/禁用武器，或开启新一轮换弹，不能覆盖其状态和 Timer。
	if (ActionState != EFPWeaponActionState::Reloading || ReloadSequence != FinishingReloadSequence)
	{
		return;
	}

	ResetReloadState();
	ActionState = EFPWeaponActionState::Ready;
}

void UfpstrueWeaponComponent::CancelReload()
{
	// 取消只收尾：装填帧之前不补弹，装填帧之后保留已转移的弹药。
	if (ActionState != EFPWeaponActionState::Reloading)
	{
		return;
	}

	ResetReloadState();
	ActionState = EFPWeaponActionState::Ready;
}

void UfpstrueWeaponComponent::ResetReloadState()
{
	// 所有出口成对清 Timer 和提交标记；序号保留到下次 Request 递增，以识别委托重入的新事务。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}
	bReloadAmmoCommitted = false;
}

// ==================== Recoil System ====================

void UfpstrueWeaponComponent::ApplyRecoil(APlayerController* PlayerController)
{
	// 本发后坐力先累加到受限范围，再启动固定频率的恢复 Timer；瞄准时使用较小倍率。
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
	// 每次 Timer 回调只恢复一小步，并把相邻两次累计值的差量写入 Controller 视角。
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
	// 停止恢复 Timer 并归零累计量，供恢复完成、角色死亡和组件退出共同调用。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RecoilRecoveryTimerHandle);
	}

	AccumulatedRecoilPitch = 0.0f;
	AccumulatedRecoilYaw = 0.0f;
}

// ==================== 生命周期结束与公共清理 ====================

void UfpstrueWeaponComponent::HandleOwnerDeath()
{
	// 玩家死亡属于强制中断：停止连射、取消换弹和后坐力恢复，再把武器置为 Disabled。
	ResetWeaponRuntimeState();
	ActionState = EFPWeaponActionState::Disabled;
}

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 组件退出前清理所有 Timer，并通知角色解除装备关系，避免留下跨生命周期的回调和悬空关系。
	ResetWeaponRuntimeState();

	if (AfpstrueCharacter* OwningCharacter = Character.Get())
	{
		OwningCharacter->ClearEquippedWeaponComponent(this);
	}

	ActionState = EFPWeaponActionState::Disabled;
	Character.Reset();
	Super::EndPlay(EndPlayReason);
}

void UfpstrueWeaponComponent::ResetWeaponRuntimeState()
{
	// 死亡和 EndPlay 共用同一清理入口；调用者随后禁用武器，角色引用由 EndPlay 单独解除。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutomaticFireTimerHandle);
	}

	ResetReloadState();
	ClearRecoilState();
	ConsecutiveShotCount = 0;
	LastShotTimeSeconds = -1.0;
	LastAcceptedShotTimeSeconds = -1.0;
}

void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	// 弹药状态只由 WeaponComponent 写入；角色、HUD 和蓝图通过该委托读取同一份结果。
	OnAmmoChanged.Broadcast(CurrentAmmo, MagazineSize, ReserveAmmo);
}
