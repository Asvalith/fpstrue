// Copyright Epic Games, Inc. All Rights Reserved.
#include "Characters/Player/fpstruePlayerAnimInstance.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#if !UE_BUILD_SHIPPING
#include "UnrealClient.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#endif

void UfpstruePlayerAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
#if !UE_BUILD_SHIPPING
	++PresentationCaptureRevision;
	PendingPresentationCaptures.Reset();
#endif
	SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	// MPC 仅由真实 RAR 第一人称实例使用，不让基类/模板 CDO 强保留旧材质资源；初始化可早于 Possess，不能要求此时已本地控制。
	BindCurrentWeapon();
}

void UfpstruePlayerAnimInstance::NativeUninitializeAnimation()
{
#if !UE_BUILD_SHIPPING
	++PresentationCaptureRevision;
	PendingPresentationCaptures.Reset();
#endif
	if (AfpstrueCharacter* Character = EquipmentSource.Get())
		Character->OnEquippedWeaponChanged.RemoveDynamic(this, &ThisClass::HandleEquipmentChanged);
	EquipmentSource.Reset();
	if (UfpstrueWeaponComponent* Weapon = AnimationWeapon.Get())
	{
		Weapon->OnWeaponFirePerformed.RemoveDynamic(this, &ThisClass::PresentFire);
		Weapon->OnWeaponReloadPlaybackRequested.RemoveDynamic(this, &ThisClass::PresentReload);
	}
	AnimationWeapon.Reset();
	Montage_Stop(0.0f);
	Super::NativeUninitializeAnimation();
}

// 装备完成与首发可能发生在同一次动画 Tick 之前；立即接入事实事件，不等下一帧轮询。
void UfpstruePlayerAnimInstance::HandleEquipmentChanged(UfpstrueWeaponComponent*)
{
	BindCurrentWeapon();
}

// 装备引用和动画实例可能先后就绪；先绑定事件，再补读同一换弹编号的当前进度，避免重连漏播。
void UfpstruePlayerAnimInstance::BindCurrentWeapon()
{
	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(TryGetPawnOwner());
	if (EquipmentSource.Get() != Character)
	{
		if (AfpstrueCharacter* Previous = EquipmentSource.Get())
			Previous->OnEquippedWeaponChanged.RemoveDynamic(this, &ThisClass::HandleEquipmentChanged);
		EquipmentSource = Character;
		if (Character != nullptr) Character->OnEquippedWeaponChanged.AddUniqueDynamic(this, &ThisClass::HandleEquipmentChanged);
	}
	UfpstrueWeaponComponent* Current = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	if (Character != nullptr && !Character->IsLocallyControlled() && GetSkelMeshComponent() == Character->GetMesh1P()) Current = nullptr;
	if (AnimationWeapon.Get() == Current) return;
	if (UfpstrueWeaponComponent* Previous = AnimationWeapon.Get())
	{
		Previous->OnWeaponFirePerformed.RemoveDynamic(this, &ThisClass::PresentFire);
		Previous->OnWeaponReloadPlaybackRequested.RemoveDynamic(this, &ThisClass::PresentReload);
	}
	AnimationWeapon = Current;
	Montage_Stop(0.05f);
	// 停止旧动画可同步触发卸枪/换装；内层已绑定新对象时，旧调用不能再向旧武器添加监听。
	if (!IsValid(Current) || !IsValid(Character) || EquipmentSource.Get() != Character
		|| AnimationWeapon.Get() != Current || Character->GetEquippedWeaponComponent() != Current) return;
	Current->OnWeaponFirePerformed.AddUniqueDynamic(this, &ThisClass::PresentFire);
	Current->OnWeaponReloadPlaybackRequested.AddUniqueDynamic(this, &ThisClass::PresentReload);
	if (Current->IsReloading()) PresentReload(Current->GetActiveReloadId(), Current->IsEmptyReloadPresentation());
	else if (EquipMontage != nullptr && Character != nullptr && !Character->IsDead())
		Montage_Play(EquipMontage, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false);
}

// 只在游戏线程采样 UObject；动画图消费这些值，不在动画工作线程访问 Controller、ASC 或可变武器状态。
void UfpstruePlayerAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	BindCurrentWeapon();
	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(TryGetPawnOwner());
	if (Character == nullptr) return;
	const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	const FVector Velocity = Character->GetVelocity();
	const FVector LocalVelocity = Character->GetActorRotation().UnrotateVector(Velocity);
	// 只消除动画快照里的微小校正抖动，不修改真实速度或碰撞；Idle 不继续采样极慢的走路循环。
	const bool bMoving = Velocity.SizeSquared2D() > FMath::Square(AnimationSettings.MovementDeadZone);
	const bool bMovingOnGround = bMoving && Movement != nullptr && Movement->IsMovingOnGround();
	GroundSpeed = bMoving ? Velocity.Size2D() : 0.0f;
	ForwardSpeed = bMoving ? LocalVelocity.X : 0.0f;
	RightSpeed = bMoving ? LocalVelocity.Y : 0.0f;
	bFalling = Movement != nullptr && Movement->IsFalling();
	bJumpRising = bFalling && Velocity.Z > 0.0f;
	if (bWasFalling && !bFalling) LandingRemaining = AnimationSettings.LandingDuration;
	LandingRemaining = FMath::Max(0.0f, LandingRemaining - FMath::Max(0.0f, DeltaSeconds));
	bLanding = LandingRemaining > 0.0f;
	bWasFalling = bFalling;
	bArmed = AnimationWeapon.IsValid() && !Character->IsDead();
	MovementAlpha = bMovingOnGround ? FMath::Clamp(GroundSpeed / FMath::Max(AnimationSettings.FullMovementBlendSpeed, UE_SMALL_NUMBER), 0.0f, 1.0f) : 0.0f;
	LocomotionPlayRate = bMovingOnGround ? FMath::Clamp(GroundSpeed / FMath::Max(AnimationSettings.ReferenceLocomotionSpeed, UE_SMALL_NUMBER), AnimationSettings.MinLocomotionPlayRate, AnimationSettings.MaxLocomotionPlayRate) : 1.0f;
	AimAlpha = FMath::FInterpTo(AimAlpha, bArmed && Character->IsAiming() ? 1.0f : 0.0f, DeltaSeconds, AnimationSettings.AimBlendSpeed);
	// 本地松开移动后退出冲刺姿态；远端没有本地输入，仍以已复制的实际移动判断，不反向取消 GAS。
	const bool bHasMoveInput = !Character->IsLocallyControlled()
		|| (Movement != nullptr && Movement->GetCurrentAcceleration().SizeSquared2D() > UE_KINDA_SMALL_NUMBER);
	const bool bPresentSprint = bArmed && Character->IsSprinting() && bMovingOnGround && bHasMoveInput;
	SprintAlpha = bPresentSprint ? FMath::FInterpTo(SprintAlpha, 1.0f, DeltaSeconds, AnimationSettings.SprintInBlendSpeed)
		: FMath::FInterpConstantTo(SprintAlpha, 0.0f, DeltaSeconds, AnimationSettings.SprintOutBlendSpeed);
	// 远端直接读取 Character 原生 RemoteViewPitch 对应的瞄准方向，不额外复制 Camera 或发送视角 RPC。
	AimPitch = FMath::Clamp(FRotator::NormalizeAxis(Character->GetBaseAimRotation().Pitch),
		-AfpstrueCharacter::ViewPitchLimit, AfpstrueCharacter::ViewPitchLimit);
	if (Character->UsesTemplatePresentation() && GetSkelMeshComponent() == Character->GetMesh()
		&& TurnLeftAnimation != nullptr && TurnRightAnimation != nullptr)
	{
		const float TurnDuration = TurnRightAnimation->GetPlayLength() / TurnRightAnimation->RateScale;
		Character->UpdateBodyFacing(TurnDuration);
		const FFpstrueBodyFacingState& Facing = Character->GetBodyFacingState();
		const bool bStationary = Facing.bInitialized && Facing.bStationary && !bMoving && !bFalling && !Character->IsDead();
		float FeetYaw = Facing.FacingYaw;
		if (bStationary && Facing.TurnDirection != 0)
		{
			bTurnRight = Facing.TurnDirection > 0;
			const UAnimSequence* Turn = bTurnRight ? TurnRightAnimation : TurnLeftAnimation;
			TurnTime = FMath::Clamp((Character->GetBodyFacingServerTime() - Facing.TurnStartedServerTime) * Turn->RateScale,
				0.0f, Turn->GetPlayLength());
			FeetYaw += Turn->ExtractRootMotion(0.0f, TurnTime, false).Rotator().Yaw;
		}
		TurnAlpha = FMath::FInterpConstantTo(TurnAlpha, bStationary && Facing.TurnDirection != 0 ? 1.0f : 0.0f,
			DeltaSeconds, AnimationSettings.TurnBlendSpeed);
		// CMC 对远端 Mesh 的平滑旋转也须抵消；只减 ActorYaw 会让脚部再跟着网络平滑滑转一次。
		const float MeshFacing = (GetSkelMeshComponent()->GetComponentQuat() * Character->GetBaseRotationOffset().Inverse()).Rotator().Yaw;
		RootYawOffset = bStationary ? FRotator::NormalizeAxis(FeetYaw - MeshFacing)
			: FMath::FInterpTo(RootYawOffset, 0.0f, DeltaSeconds, AnimationSettings.YawRecoverySpeed);
		const FVector LowerVelocity = FRotator(0.0f, MeshFacing + RootYawOffset, 0.0f).UnrotateVector(Velocity);
		ForwardSpeed = bMoving ? LowerVelocity.X : 0.0f;
		RightSpeed = bMoving ? LowerVelocity.Y : 0.0f;
		if (APlayerController* PC = Cast<APlayerController>(Character->GetController()); PC && PC->IsLocalController() && PC->PlayerCameraManager)
		{
			// 站立视角相对双脚限幅，迈步转身后范围跟随；移动时仍可连续转身，不永久锁死世界水平角。
			PC->PlayerCameraManager->ViewYawMin = bStationary ? FeetYaw - AfpstrueCharacter::ViewYawLimit : 0.0f;
			PC->PlayerCameraManager->ViewYawMax = bStationary ? FeetYaw + AfpstrueCharacter::ViewYawLimit : 359.999f;
		}
	}
	if (Character->UsesRARPresentation() && GetSkelMeshComponent() != nullptr)
	{
		const float Pitch = FMath::Clamp(AimPitch, -AnimationSettings.BodyAimPitchLimit, AnimationSettings.BodyAimPitchLimit);
		// 身体网格前方是 +Y；UE 的正 Roll 把 +Y 转向 -Z，所以抬头要使用负 Roll。
		AimRotation = FRotator(0.0f, 0.0f, -Pitch * AnimationSettings.BodyAimPitchScale);
		// ik_hand_root 不在脊柱子树中，必须绕同一脊柱支点做同样的旋转和平移，不能只旋转身体留下枪链。
		const FVector AimPivot = GetSkelMeshComponent()->GetSocketTransform(AnimationSettings.SpinePivotBone, RTS_Component).GetTranslation();
		// 枪根自身也有作者动画位移，不能假设它在模型原点；移除上一帧的补偿后再绕支点旋转。
		const FVector GunRoot = GetSkelMeshComponent()->GetSocketTransform(AnimationSettings.GunRootBone, RTS_Component).GetTranslation() - GunAimTranslation;
		GunAimTranslation = AimRotation.Quaternion().RotateVector(GunRoot - AimPivot) + AimPivot - GunRoot;
	}
	else
	{
		// 成套模板由自己的 AimOffset 消费实际俯仰角，不沿用 RAR 的骨链补偿或读取无关骨骼。
		AimRotation = FRotator::ZeroRotator;
		GunAimTranslation = FVector::ZeroVector;
	}
	HitAlpha = FMath::FInterpTo(HitAlpha, 0.0f, DeltaSeconds, AnimationSettings.HitRecoverySpeed);
	HitTime = FMath::Min(HitTime + FMath::Max(0.0f, DeltaSeconds), AnimationSettings.HitDuration);
	if (Character->UsesNativePresentation() && Character->IsLocallyControlled() && GetSkelMeshComponent() == Character->GetMesh1P())
	{
		Character->ApplyCameraPresentation(AimAlpha);
	}
#if !UE_BUILD_SHIPPING
	if (Character->UsesNativePresentation())
	{
		// 同时采样拥有者视模和观察者身体；远端可见动作必须由真实 AnimGraph 消费，不能仅检查复制布尔值。
		if (AimAlpha > 0.9f) CapturePresentation(TEXT("aim"));
		if (SprintAlpha > 0.9f) CapturePresentation(TEXT("sprint"));
		if (bJumpRising) CapturePresentation(TEXT("jump"));
		if (GroundSpeed > 50.0f) CapturePresentation(TEXT("move"));
		if (bArmed && GroundSpeed < 3.0f && !IsAnyMontagePlaying()) CapturePresentation(TEXT("holding"));
	}
#endif
}

// 源材质用动画曲线决定视模独立 FOV；求值完成后只由本地手臂更新本世界实例，远端材质保持 World 模式。
void UfpstruePlayerAnimInstance::NativePostEvaluateAnimation()
{
	Super::NativePostEvaluateAnimation();
	const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(TryGetPawnOwner());
	if (Character == nullptr || !Character->UsesRARPresentation() || !Character->IsLocallyControlled()
		|| GetSkelMeshComponent() != Character->GetMesh1P() || ViewmodelFOVCollection == nullptr) return;
	const float AuthoredFOV = GetCurveValue(AnimationSettings.ViewmodelFOVCurve) + GetCurveValue(AnimationSettings.ViewmodelFOVAddCurve);
	// 过渡或空曲线不允许送出 0/NaN，避免材质投影退化；不改源 MPC 资产，也不改游戏相机 FOV。
	const float FOV = FMath::IsFinite(AuthoredFOV) && AuthoredFOV > 5.0f && AuthoredFOV < 170.0f ? AuthoredFOV : AnimationSettings.DefaultViewmodelFOV;
	PresentViewmodelProjection(FOV);
}

// 依据本角色瞄准状态播放单发蒙太奇；只负责表现，不扣弹或重新提交命中。
void UfpstruePlayerAnimInstance::PresentFire()
{
	const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(TryGetPawnOwner());
	UfpstrueWeaponComponent* Weapon = AnimationWeapon.Get();
	if (Character == nullptr || Character->IsDead() || Weapon == nullptr || Weapon->IsReloading()) return;
	UAnimMontage* Montage = Character->IsAiming() && AimedFireMontage != nullptr ? AimedFireMontage.Get() : FireMontage.Get();
	const float PlayedLength = Montage != nullptr ? Montage_Play(Montage, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false) : 0.0f;
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeVisual")))
		UE_LOG(LogTemp, Display, TEXT("PVP_FIRE_PRESENT Pawn=%s Remote=%d Mesh=%s Montage=%s Length=%.4f Played=%.4f BlendIn=%.4f BlendOut=%.4f"),
			*Character->GetName(), !Character->IsLocallyControlled(), *GetNameSafe(GetSkelMeshComponent()), *GetNameSafe(Montage),
			Montage ? Montage->GetPlayLength() : 0.0f, PlayedLength, Montage ? Montage->BlendIn.GetBlendTime() : 0.0f, Montage ? Montage->BlendOut.GetBlendTime() : 0.0f);
	CapturePresentation(TEXT("fire"));
#else
	(void)PlayedLength;
#endif
}

// 选择普通/空仓蒙太奇并匹配事务时长；播放实例仍登记到武器的同一 ReloadId。
void UfpstruePlayerAnimInstance::PresentReload(int32 ReloadId, bool bWasEmpty)
{
	UfpstrueWeaponComponent* Weapon = AnimationWeapon.Get();
	UAnimMontage* Montage = bWasEmpty ? EmptyReloadMontage.Get() : ReloadMontage.Get();
	if (Weapon == nullptr || Montage == nullptr) return;
	const float Duration = Weapon->GetReloadDuration();
	const float PlayRate = Duration > KINDA_SMALL_NUMBER ? Montage->GetPlayLength() / Duration : 1.0f;
	// 统一播放身份、结束和取消仍由武器事务管理；本实例不提交弹药，也不结束 GAS。
	if (Weapon->PlayReloadMontage(ReloadId, GetSkelMeshComponent(), Montage, PlayRate))
	{
#if !UE_BUILD_SHIPPING
		CapturePresentation(bWasEmpty ? TEXT("reload_empty") : TEXT("reload"));
#endif
	}
}

void UfpstruePlayerAnimInstance::PresentHit()
{
	HitAlpha = 1.0f;
	HitTime = 0.0f;
}

#if !UE_BUILD_SHIPPING
// 仅显式有画面冒烟时记录真实客户端视口；普通玩法、NullRHI 测试和 Shipping 都不触发截图。
void UfpstruePlayerAnimInstance::CapturePresentation(const TCHAR* Phase)
{
	const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(TryGetPawnOwner());
	if (Character == nullptr || Character->GetNetMode() != NM_Client || !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeVisual"))
		|| FScreenshotRequest::IsScreenshotRequested()) return;
	const bool bLocalViewmodel = Character->IsLocallyControlled() && GetSkelMeshComponent() == Character->GetMesh1P();
	const bool bObservedBody = !Character->IsLocallyControlled() && GetSkelMeshComponent() == Character->GetMesh()
		&& FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeObserve"));
	if (!bLocalViewmodel && !bObservedBody) return;
	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), Directory) || Directory.IsEmpty()) return;
	const FString Path = FPaths::Combine(Directory, (bObservedBody ? TEXT("remote_") : TEXT("")) + FString(Phase) + TEXT(".png"));
	const FName PhaseName(Phase);
	if (IFileManager::Get().FileExists(*Path) || PendingPresentationCaptures.Contains(PhaseName)) return;
	UfpstrueWeaponComponent* Weapon = AnimationWeapon.Get();
	if (Weapon == nullptr || GetWorld() == nullptr) return;
	PendingPresentationCaptures.Add(PhaseName);
	const bool bReload = PhaseName == TEXT("reload") || PhaseName == TEXT("reload_empty");
	UAnimMontage* ExpectedAction = bReload ? (PhaseName == TEXT("reload_empty") ? EmptyReloadMontage.Get() : ReloadMontage.Get())
		: PhaseName == TEXT("fire") ? (Character->IsAiming() && AimedFireMontage ? AimedFireMontage.Get() : FireMontage.Get()) : nullptr;
	// 极短开火 Montage 也要在自动 BlendOut 前截帧；固定 100ms 在 200ms 动画上可能已经错过有效实例。
	const float ActionCaptureTime = ExpectedAction ? FMath::Min(ExpectedAction->GetPlayLength() * 0.35f,
		FMath::Max(0.01f, ExpectedAction->GetPlayLength() - ExpectedAction->BlendOut.GetBlendTime()) * 0.4f) : 0.15f;
	const float Delay = bReload ? FMath::Max(0.05f, Weapon->GetReloadDuration() * 0.45f)
		: PhaseName == TEXT("fire") ? 0.01f : ExpectedAction ? FMath::Clamp(ActionCaptureTime, 0.01f, 0.1f) : 0.15f;
	const uint32 Revision = PresentationCaptureRevision;
	const int32 ReloadId = Weapon->GetActiveReloadId();
	const TWeakObjectPtr<AfpstrueCharacter> ExpectedPawn(const_cast<AfpstrueCharacter*>(Character));
	const TWeakObjectPtr<UfpstrueWeaponComponent> ExpectedWeapon(Weapon);
	FTimerHandle CaptureTimer;
	// 截取动画中段而不是收到事件的首帧；只在显式开发验收启动，旧动画实例/旧换弹不能迟到截图。
	GetWorld()->GetTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateWeakLambda(this,
		[this, Path, PhaseName, bReload, Revision, ReloadId, ExpectedPawn, ExpectedWeapon,
			ExpectedAction = TWeakObjectPtr<UAnimMontage>(ExpectedAction), bObservedBody]()
		{
			if (Revision != PresentationCaptureRevision) return;
			PendingPresentationCaptures.Remove(PhaseName);
			const AfpstrueCharacter* Pawn = ExpectedPawn.Get();
			UfpstrueWeaponComponent* CurrentWeapon = ExpectedWeapon.Get();
			if (!IsValid(Pawn) || Pawn->IsDead() || !IsValid(CurrentWeapon)
				|| Pawn->GetEquippedWeaponComponent() != CurrentWeapon || TryGetPawnOwner() != Pawn
				|| GetSkelMeshComponent() == nullptr || GetSkelMeshComponent()->GetAnimInstance() != this) return;
			if (bReload && (!CurrentWeapon->IsReloading() || CurrentWeapon->GetActiveReloadId() != ReloadId)) return;
			if (PhaseName == TEXT("aim") && (!Pawn->IsAiming() || AimAlpha < 0.99f)) return;
			if (PhaseName == TEXT("sprint") && !Pawn->IsSprinting()) return;
			if (PhaseName == TEXT("holding") && (!bArmed || IsAnyMontagePlaying())) return;
			if (PhaseName == TEXT("move") && GroundSpeed <= 50.0f) return;
			if (ExpectedAction.IsValid() && !Montage_IsPlaying(ExpectedAction.Get()))
			{
				UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_CAPTURE_SKIP Phase=%s Remote=%d Expected=%s Length=%.4f Position=%.4f BlendTime=%.4f Reason=ExpectedMontageNotPlaying"),
					*PhaseName.ToString(), bObservedBody, *GetNameSafe(ExpectedAction.Get()), ExpectedAction->GetPlayLength(),
					Montage_GetPosition(ExpectedAction.Get()), Montage_GetBlendTime(ExpectedAction.Get()));
				return;
			}
			// 网络事件发生在动画更新前，短开火取首个已求值的有效帧；仍要求真实播放位置和 Slot 权重，不能拿空图通过截图门禁。
			if (PhaseName == TEXT("fire") && ExpectedAction.IsValid())
			{
				const float SlotWeight = ExpectedAction->SlotAnimTracks.Num() > 0 ? GetSlotMontageLocalWeight(ExpectedAction->SlotAnimTracks[0].SlotName) : 0.0f;
				const float Position = Montage_GetPosition(ExpectedAction.Get());
				if (SlotWeight <= 0.01f || Position <= 0.0f)
				{
					UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_CAPTURE_SKIP Phase=fire Remote=%d SlotWeight=%.4f Position=%.4f Reason=PoseNotEvaluated"), bObservedBody, SlotWeight, Position);
					return;
				}
				UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_FIRE_POSE Remote=%d Position=%.4f SlotWeight=%.4f UROEnabled=%d"), bObservedBody, Position, SlotWeight, GetSkelMeshComponent()->bEnableUpdateRateOptimizations);
			}
			USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
			UAnimInstance* GunAnimation = CurrentWeapon->GetAnimInstance();
			UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_ACTION Phase=%s Remote=%d Pawn=%s Mesh=%s Anim=%s Montage=%s Position=%.4f BodyHidden=%d GunVisible=%d GunHidden=%d GunOwnerOnly=%d GunParent=%s GunSocket=%s Speed=%.2f Aim=%.3f Sprint=%.3f GunMontage=%s Hand=%s Gun=%s"),
				*PhaseName.ToString(), bObservedBody, *Pawn->GetName(), *GetNameSafe(Mesh), *GetClass()->GetPathName(),
				*GetNameSafe(ExpectedAction.Get()), ExpectedAction.IsValid() ? Montage_GetPosition(ExpectedAction.Get()) : 0.0f,
				Mesh->bHiddenInGame, CurrentWeapon->IsVisible(), CurrentWeapon->bHiddenInGame, CurrentWeapon->bOnlyOwnerSee,
				*GetNameSafe(CurrentWeapon->GetAttachParent()), *CurrentWeapon->GetAttachSocketName().ToString(),
				GroundSpeed, AimAlpha, SprintAlpha, *GetNameSafe(GunAnimation ? GunAnimation->GetCurrentActiveMontage() : nullptr),
				*Mesh->GetSocketLocation(TEXT("hand_r")).ToCompactString(), *CurrentWeapon->GetComponentLocation().ToCompactString());
			if (!FScreenshotRequest::IsScreenshotRequested() && !IFileManager::Get().FileExists(*Path))
				FScreenshotRequest::RequestScreenshot(Path, true, false, false);
		}), Delay, false);
}
#endif
