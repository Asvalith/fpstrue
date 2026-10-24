// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueFootIK.h"
#include "Kismet/GameplayStatics.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/GameStateBase.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "Game/fpstrueMultiplayer.h"
#include "Runtime/fpstrueRuntime.h"
#include "Net/UnrealNetwork.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#if !UE_BUILD_SHIPPING
#include "UnrealClient.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#endif

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

// 阅读顺序：动画事实消费 -> CMC/SavedMove 模式上下文 -> 权威命中历史 -> Character 生命周期/输入/装备/死亡。
// Character 只协调已有状态所有者：GAS 管动作，武器管弹药与换弹，AttributeSet 管生命；动画不反向授权玩法。

// ==================== 共用玩家动画表现 ====================

UfpstruePlayerAnimInstance::UfpstruePlayerAnimInstance()
{
	// 工具首次生成资源前允许为空；生成后的真实角色门禁要求成套资源全部存在，不使用错误骨架兜底。
	const auto FindMontage = [](const TCHAR* Path)
	{
		return LoadObject<UAnimMontage>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
	};
	FireMontage = FindMontage(TEXT("/Game/FirstPerson/Animations/RAR/AM_RAR_Fire.AM_RAR_Fire"));
	AimedFireMontage = FindMontage(TEXT("/Game/FirstPerson/Animations/RAR/AM_RAR_FireAimed.AM_RAR_FireAimed"));
	ReloadMontage = FindMontage(TEXT("/Game/FirstPerson/Animations/RAR/AM_RAR_Reload.AM_RAR_Reload"));
	EmptyReloadMontage = FindMontage(TEXT("/Game/FirstPerson/Animations/RAR/AM_RAR_ReloadEmpty.AM_RAR_ReloadEmpty"));
	EquipMontage = FindMontage(TEXT("/Game/FirstPerson/Animations/RAR/AM_RAR_Equip.AM_RAR_Equip"));
}

void UfpstruePlayerAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
#if !UE_BUILD_SHIPPING
	++PresentationCaptureRevision;
	PendingPresentationCaptures.Reset();
#endif
	SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	// MPC 仅由真实 RAR 第一人称实例使用，不让基类/模板 CDO 强保留旧材质资源；初始化可早于 Possess，不能要求此时已本地控制。
	const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(TryGetPawnOwner());
	if (Character != nullptr && Character->UsesRARPresentation() && Character->GetNetMode() != NM_DedicatedServer
		&& GetSkelMeshComponent() == Character->GetMesh1P() && ViewmodelFOVCollection == nullptr)
	{
		ViewmodelFOVCollection = LoadObject<UMaterialParameterCollection>(nullptr,
			TEXT("/Game/InfimaGames/ArtCore/MaterialLibrary/MPC_IG_FOV.MPC_IG_FOV"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	}
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
	const bool bMoving = Velocity.SizeSquared2D() > FMath::Square(3.0f);
	const bool bMovingOnGround = bMoving && Movement != nullptr && Movement->IsMovingOnGround();
	GroundSpeed = bMoving ? Velocity.Size2D() : 0.0f;
	ForwardSpeed = bMoving ? LocalVelocity.X : 0.0f;
	RightSpeed = bMoving ? LocalVelocity.Y : 0.0f;
	bFalling = Movement != nullptr && Movement->IsFalling();
	bJumpRising = bFalling && Velocity.Z > 0.0f;
	if (bWasFalling && !bFalling) LandingRemaining = 0.15f;
	LandingRemaining = FMath::Max(0.0f, LandingRemaining - FMath::Max(0.0f, DeltaSeconds));
	bLanding = LandingRemaining > 0.0f;
	bWasFalling = bFalling;
	bArmed = AnimationWeapon.IsValid() && !Character->IsDead();
	MovementAlpha = bMovingOnGround ? FMath::Clamp(GroundSpeed / 160.0f, 0.0f, 1.0f) : 0.0f;
	LocomotionPlayRate = bMovingOnGround ? FMath::Clamp(GroundSpeed / 400.0f, 0.05f, 2.0f) : 1.0f;
	AimAlpha = FMath::FInterpTo(AimAlpha, bArmed && Character->IsAiming() ? 1.0f : 0.0f, DeltaSeconds, 12.0f);
	// 本地松开移动后退出冲刺姿态；远端没有本地输入，仍以已复制的实际移动判断，不反向取消 GAS。
	const bool bHasMoveInput = !Character->IsLocallyControlled()
		|| (Movement != nullptr && Movement->GetCurrentAcceleration().SizeSquared2D() > UE_KINDA_SMALL_NUMBER);
	const bool bPresentSprint = bArmed && Character->IsSprinting() && bMovingOnGround && bHasMoveInput;
	SprintAlpha = bPresentSprint ? FMath::FInterpTo(SprintAlpha, 1.0f, DeltaSeconds, 8.0f)
		: FMath::FInterpConstantTo(SprintAlpha, 0.0f, DeltaSeconds, 10.0f);
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
			DeltaSeconds, 10.0f);
		// CMC 对远端 Mesh 的平滑旋转也须抵消；只减 ActorYaw 会让脚部再跟着网络平滑滑转一次。
		const float MeshFacing = (GetSkelMeshComponent()->GetComponentQuat() * Character->GetBaseRotationOffset().Inverse()).Rotator().Yaw;
		RootYawOffset = bStationary ? FRotator::NormalizeAxis(FeetYaw - MeshFacing)
			: FMath::FInterpTo(RootYawOffset, 0.0f, DeltaSeconds, 15.0f);
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
		const float Pitch = FMath::Clamp(AimPitch, -75.0f, 75.0f);
		// 身体网格前方是 +Y；UE 的正 Roll 把 +Y 转向 -Z，所以抬头要使用负 Roll。
		AimRotation = FRotator(0.0f, 0.0f, -Pitch * 0.6f);
		// ik_hand_root 不在脊柱子树中，必须绕同一脊柱支点做同样的旋转和平移，不能只旋转身体留下枪链。
		const FVector AimPivot = GetSkelMeshComponent()->GetSocketTransform(TEXT("spine_03"), RTS_Component).GetTranslation();
		// 枪根自身也有作者动画位移，不能假设它在模型原点；移除上一帧的补偿后再绕支点旋转。
		const FVector GunRoot = GetSkelMeshComponent()->GetSocketTransform(TEXT("ik_hand_root"), RTS_Component).GetTranslation() - GunAimTranslation;
		GunAimTranslation = AimRotation.Quaternion().RotateVector(GunRoot - AimPivot) + AimPivot - GunRoot;
	}
	else
	{
		// 成套模板由自己的 AimOffset 消费实际俯仰角，不沿用 RAR 的骨链补偿或读取无关骨骼。
		AimRotation = FRotator::ZeroRotator;
		GunAimTranslation = FVector::ZeroVector;
	}
	HitAlpha = FMath::FInterpTo(HitAlpha, 0.0f, DeltaSeconds, 12.0f);
	HitTime = FMath::Min(HitTime + FMath::Max(0.0f, DeltaSeconds), 2.0f);
	if (Character->UsesNativePresentation() && Character->IsLocallyControlled() && GetSkelMeshComponent() == Character->GetMesh1P())
	{
		UCameraComponent* Camera = Character->GetFirstPersonCameraComponent();
		Camera->SetFieldOfView(FMath::Lerp(90.0f, 70.0f, AimAlpha));
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
	const float AuthoredFOV = GetCurveValue(TEXT("Field Of View Viewmodel")) + GetCurveValue(TEXT("Field Of View Viewmodel Add"));
	// 过渡或空曲线不允许送出 0/NaN，避免材质投影退化；不改源 MPC 资产，也不改游戏相机 FOV。
	const float FOV = FMath::IsFinite(AuthoredFOV) && AuthoredFOV > 5.0f && AuthoredFOV < 170.0f ? AuthoredFOV : 100.0f;
	UKismetMaterialLibrary::SetScalarParameterValue(this, ViewmodelFOVCollection, TEXT("Field Of View"), FOV);
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

namespace
{
	// 预测数据只改变 SavedMove 的分配类型，仍复用 UE 的丢包恢复、ACK、时间戳和位置校正协议。
	class FNetworkPredictionData_Client_fpstrue final : public FNetworkPredictionData_Client_Character
	{
	public:
		// 保留 CMC 原生预测缓存初始化，只替换本项目移动记录的分配类型。
		explicit FNetworkPredictionData_Client_fpstrue(const UCharacterMovementComponent& Movement)
			: FNetworkPredictionData_Client_Character(Movement) {}
		// 每条历史移动创建带两个动作模式位的记录，仍交给原生合并/ACK/重放流程。
		virtual FSavedMovePtr AllocateNewMove() override { return MakeShared<FSavedMove_fpstrueCharacter>(); }
	};
}

void FSavedMove_fpstrueCharacter::Clear()
{
	Super::Clear();
	bSavedAiming = bSavedSprinting = false;
}

uint8 FSavedMove_fpstrueCharacter::GetCompressedFlags() const
{
	return Super::GetCompressedFlags() | (bSavedAiming ? FLAG_Custom_0 : 0) | (bSavedSprinting ? FLAG_Custom_1 : 0);
}

bool FSavedMove_fpstrueCharacter::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* Character, float MaxDelta) const
{
	const auto* Other = static_cast<const FSavedMove_fpstrueCharacter*>(NewMove.Get());
	return Other != nullptr && bSavedAiming == Other->bSavedAiming && bSavedSprinting == Other->bSavedSprinting
		&& Super::CanCombineWith(NewMove, Character, MaxDelta);
}

void FSavedMove_fpstrueCharacter::SetMoveFor(ACharacter* Character, float InDeltaTime, const FVector& Accel,
	FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(Character, InDeltaTime, Accel, ClientData);
	if (const auto* Movement = Cast<UfpstrueCharacterMovementComponent>(Character->GetCharacterMovement()))
	{
		bSavedAiming = Movement->IsAimMovementRequested();
		bSavedSprinting = Movement->IsSprintMovementRequested();
	}
}

void FSavedMove_fpstrueCharacter::PrepMoveFor(ACharacter* Character)
{
	Super::PrepMoveFor(Character);
	if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(Character->GetCharacterMovement()))
	{
		// ★【重放边界】不重新激活 GA，不刷新 UI，只恢复这段历史移动实际使用的模式。
		Movement->bMoveAiming = bSavedAiming;
		Movement->bMoveSprinting = bSavedSprinting;
	}
}

float UfpstrueCharacterMovementComponent::GetMaxSpeed() const
{
	const auto* Character = Cast<AfpstrueCharacter>(CharacterOwner);
	if (Character == nullptr || !(IsMovingOnGround() || IsFalling()) || IsCrouching()) return Super::GetMaxSpeed();
	return bMoveAiming ? Character->AimWalkSpeed : (bMoveSprinting ? Character->SprintSpeed : Character->WalkSpeed);
}

FNetworkPredictionData_Client* UfpstrueCharacterMovementComponent::GetPredictionData_Client() const
{
	if (ClientPredictionData == nullptr)
		const_cast<UfpstrueCharacterMovementComponent*>(this)->ClientPredictionData = new FNetworkPredictionData_Client_fpstrue(*this);
	return ClientPredictionData;
}

void UfpstrueCharacterMovementComponent::SetMovementIntent(bool bAiming, bool bSprinting)
{
	const uint8 NewMode = bAiming ? 1 : (bSprinting ? 2 : 0);
	if (CharacterOwner != nullptr && CharacterOwner->HasAuthority() && NewMode != GrantedMode && GrantedMode != 0)
	{
		// 能力 RPC 与移动包不同时到达：只给刚结束的已授权模式一个有界旧 Move 区间，不接受任意客户端速度。
		const auto* ServerData = GetPredictionData_Server_Character();
		FRecentMovementGrant& Grant = RecentGrants[NextGrant++ % UE_ARRAY_COUNT(RecentGrants)];
		Grant.Mode = GrantedMode;
		Grant.LastAllowedClientTime = ServerData->CurrentClientTimeStamp + FMath::Min(ServerData->MaxMoveDeltaTime, 0.125f);
		Grant.ExpiresAt = GetWorld()->GetTimeSeconds() + 0.25;
	}
	GrantedMode = NewMode;
	bMoveAiming = bAiming;
	bMoveSprinting = !bAiming && bSprinting;
}

void UfpstrueCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);
	bool bAim = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
	bool bSprint = (Flags & FSavedMove_Character::FLAG_Custom_1) != 0;
	const auto* Character = Cast<AfpstrueCharacter>(CharacterOwner);
	if (Character != nullptr && Character->HasAuthority())
	{
		const auto* ASC = Character->GetHealthComponent();
		const uint8 RequestedMode = bAim ? 1 : (bSprint ? 2 : 0);
		const bool bHardBlocked = !Character->CanAcceptGameplayInput() || ASC == nullptr
			|| ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading)
			|| (bAim && !Character->HasEquippedWeapon());
		bool bAuthorized = RequestedMode == 0 || RequestedMode == GrantedMode;
		const FCharacterNetworkMoveData* Move = GetCurrentNetworkMoveData();
		if (!bAuthorized && !bHardBlocked && Move != nullptr)
		{
			const double Now = GetWorld()->GetTimeSeconds();
			for (const FRecentMovementGrant& Grant : RecentGrants)
			{
				const float AheadOfEnd = Move->TimeStamp - Grant.LastAllowedClientTime;
				if (Grant.Mode == RequestedMode && Now <= Grant.ExpiresAt && AheadOfEnd <= 0.0f && AheadOfEnd >= -0.25f)
				{
					bAuthorized = true;
					break;
				}
			}
		}
		// GAS 尚未授权的意图保守拒绝，由 UE 校正；这里绝不补激活能力或修改权威标签。
		if (bHardBlocked || !bAuthorized || (bAim && bSprint)) bAim = bSprint = false;
		// 不能靠清掉客户端 Aim 位绕开当前服务器的瞄准减速；合法的旧 Sprint 已受上面的历史时间界限约束。
		if (!bHardBlocked && GrantedMode == 1 && !bSprint) bAim = true;
	}
	bMoveAiming = bAim;
	bMoveSprinting = !bAim && bSprint;
}

bool UfpstrueCharacterMovementComponent::ClientUpdatePositionAfterServerUpdate()
{
	const bool bReplayed = Super::ClientUpdatePositionAfterServerUpdate();
	// 最后一个旧 Move 不能覆盖现在的真实模式，尤其是已被服务器拒绝或因换弹取消的冲刺。
	// 重放中的碰撞/落地回调也可能改变能力；读取最新派生状态，而不是覆盖回调已经提交的新状态。
	const auto* Character = Cast<AfpstrueCharacter>(CharacterOwner);
	bMoveAiming = IsValid(Character) && Character->bIsAiming;
	bMoveSprinting = IsValid(Character) && !bMoveAiming && Character->bIsSprinting;
	return bReplayed;
}

// ==================== 权威身体历史与纯查询命中 ====================

// 形状样本在骨骼最终刷新后最多 60 Hz 写入固定环；传送、死亡或拓扑变化断代。
// 【设计取舍】只回查 PvP 玩家历史几何，不暂移真实 Pawn/Chaos，避免伤害回调看到回滚中的世界。
// 环境遮挡仍查当前碰撞，动态掩体历史未被重建；这是有界命中补偿，不是全世界回滚。

void AfpstrueCharacter::ResetHitHistory(bool bReleaseMemory)
{
	HitHistoryCount = HitHistoryNext = 0;
	++HitHistoryEpoch;
	for (FHitHistoryFrame& Frame : HitHistory)
	{
		if (bReleaseMemory) Frame.Shapes.Empty();
		else Frame.Shapes.Reset();
	}
	if (bReleaseMemory) RecordedPhysicsAsset.Reset();
}

SIZE_T AfpstrueCharacter::GetHitHistoryAllocatedBytes() const
{
	SIZE_T Bytes = 0;
	for (const FHitHistoryFrame& Frame : HitHistory) Bytes += Frame.Shapes.GetAllocatedSize();
	return Bytes;
}

int32 AfpstrueCharacter::GetHitHistoryReservedShapeCount() const
{
	int32 Shapes = 0;
	for (const FHitHistoryFrame& Frame : HitHistory) Shapes += Frame.Shapes.Max();
	return Shapes;
}

SIZE_T AfpstrueCharacter::GetHitHistoryShapeStride()
{
	return sizeof(FHistoricalShape);
}

bool AfpstrueCharacter::TeleportTo(const FVector& DestLocation, const FRotator& DestRotation, bool bIsATest, bool bNoCheck)
{
	const bool bMoved = Super::TeleportTo(DestLocation, DestRotation, bIsATest, bNoCheck);
	if (bMoved && !bIsATest) ResetHitHistory();
	return bMoved;
}

void AfpstrueCharacter::RecordServerHitHistory()
{
	if (!HasAuthority() || !CanMaintainEquipment() || GetWorld() == nullptr) return;
	USkeletalMeshComponent* BodyMesh = GetMesh();
	UPhysicsAsset* Asset = BodyMesh != nullptr ? BodyMesh->GetPhysicsAsset() : nullptr;
	if (Asset == nullptr)
	{
		if (HitHistoryCount > 0 || GetHitHistoryAllocatedBytes() > 0) ResetHitHistory(true);
		RecordedPhysicsAsset.Reset();
		if (!bLoggedUnsupportedHitGeometry)
			UE_LOG(LogTemplateCharacter, Warning, TEXT("%s has no PhysicsAsset; historical player hits are disabled, not replaced by the movement capsule."), *GetName());
		bLoggedUnsupportedHitGeometry = true;
		return;
	}
	if (RecordedPhysicsAsset.Get() != Asset)
	{
		ResetHitHistory();
		RecordedPhysicsAsset = Asset;
		// 只在物理资产变更时建立容量；32 个循环帧按真实有效形状预留，不按上限各占 64 条。
		int32 ShapeCapacity = 0;
		for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
		{
			if (Body == nullptr || BodyMesh->GetBoneIndex(Body->BoneName) == INDEX_NONE) continue;
			for (const FKBoxElem& Shape : Body->AggGeom.BoxElems) ShapeCapacity += CollisionEnabledHasQuery(Shape.GetCollisionEnabled()) ? 1 : 0;
			for (const FKSphereElem& Shape : Body->AggGeom.SphereElems) ShapeCapacity += CollisionEnabledHasQuery(Shape.GetCollisionEnabled()) ? 1 : 0;
			for (const FKSphylElem& Shape : Body->AggGeom.SphylElems) ShapeCapacity += CollisionEnabledHasQuery(Shape.GetCollisionEnabled()) ? 1 : 0;
		}
		ShapeCapacity = FMath::Min(ShapeCapacity, MaxHistoricalShapes);
		for (FHitHistoryFrame& HistoryFrame : HitHistory) HistoryFrame.Shapes.Empty(ShapeCapacity);
	}
	const double Now = GetWorld()->GetTimeSeconds();
	if (HitHistoryCount > 0)
	{
		const FHitHistoryFrame& Last = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
		const double Elapsed = Now - Last.ServerTime;
		// SetActorLocation 一类未走 TeleportTo 的不连续位移也不允许插值出一条虚构的身体轨迹。
		const double AllowedDistance = FMath::Max(200.0, GetCharacterMovement()->GetMaxSpeed() * FMath::Max(Elapsed, 0.0) * 3.0 + 40.0);
		if (Elapsed < 0.0 || FVector::DistSquared(Last.ActorLocation, GetActorLocation()) > FMath::Square(AllowedDistance)) ResetHitHistory();
		// 世界时间存为 float；只给采样间隔 0.1ms 数值容差，避免正常 60Hz 被误判成尚未到时而隔帧记录。
		else if (Elapsed + 0.0001 < 1.0 / 60.0) return;
	}
	FHitHistoryFrame& Frame = HitHistory[HitHistoryNext];
	Frame.ServerTime = Now;
	Frame.ActorLocation = GetActorLocation();
	FRotator ViewRotation;
	GetActorEyesViewPoint(Frame.ViewOrigin, ViewRotation);
	Frame.Shapes.Reset();
	bool bUnsupported = false;
	for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
	{
		if (Body == nullptr) continue;
		const int32 BoneIndex = BodyMesh->GetBoneIndex(Body->BoneName);
		if (BoneIndex == INDEX_NONE) continue;
		FTransform BoneToWorld = BodyMesh->GetBoneTransform(BoneIndex);
		const FVector Scale = BoneToWorld.GetScale3D();
		BoneToWorld.SetScale3D(FVector::OneVector);
		const auto AddShape = [&](const FTransform& Local, const FVector& Extent, uint8 Type)
		{
			if (Frame.Shapes.Num() >= MaxHistoricalShapes) { bUnsupported = true; return; }
			FHistoricalShape& Shape = Frame.Shapes.AddDefaulted_GetRef();
			const FTransform Pose = Local * BoneToWorld;
			Shape.Rotation = Pose.GetRotation();
			Shape.Location = Pose.GetLocation();
			Shape.Extent = Extent;
			Shape.Bone = Body->BoneName;
			Shape.Type = Type;
		};
		for (const FKBoxElem& Element : Body->AggGeom.BoxElems)
		{
			if (!CollisionEnabledHasQuery(Element.GetCollisionEnabled())) continue;
			const FKBoxElem Scaled = Element.GetFinalScaled(Scale, FTransform::Identity);
			AddShape(Scaled.GetTransform(), FVector(Scaled.X, Scaled.Y, Scaled.Z) * 0.5, 0);
		}
		for (const FKSphereElem& Element : Body->AggGeom.SphereElems)
		{
			if (!CollisionEnabledHasQuery(Element.GetCollisionEnabled())) continue;
			const FKSphereElem Scaled = Element.GetFinalScaled(Scale, FTransform::Identity);
			AddShape(Scaled.GetTransform(), FVector(Scaled.Radius, 0.0, 0.0), 1);
		}
		for (const FKSphylElem& Element : Body->AggGeom.SphylElems)
		{
			if (!CollisionEnabledHasQuery(Element.GetCollisionEnabled())) continue;
			const FKSphylElem Scaled = Element.GetFinalScaled(Scale, FTransform::Identity);
			AddShape(Scaled.GetTransform(), FVector(Scaled.Radius, 0.0, Scaled.Length * 0.5), 2);
		}
		// 不用凸体的包围盒冒充精确碰撞；更换 PhysicsAsset 后应补支持或改成明确的球/盒/胶囊。
		bUnsupported |= Body->AggGeom.ConvexElems.Num() > 0 || Body->AggGeom.TaperedCapsuleElems.Num() > 0;
	}
	if ((bUnsupported || Frame.Shapes.IsEmpty()) && !bLoggedUnsupportedHitGeometry)
	{
		UE_LOG(LogTemplateCharacter, Warning, TEXT("%s historical hit geometry: %d supported shapes (limit %d); unsupported convex/tapered or excess shapes are NOT approximated."),
			*GetName(), Frame.Shapes.Num(), MaxHistoricalShapes);
		bLoggedUnsupportedHitGeometry = true;
	}
	if (HitHistoryCount > 0)
	{
		const auto& Last = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
		bool bSameTopology = Last.Shapes.Num() == Frame.Shapes.Num();
		for (int32 I = 0; bSameTopology && I < Frame.Shapes.Num(); ++I)
			bSameTopology = Last.Shapes[I].Bone == Frame.Shapes[I].Bone && Last.Shapes[I].Type == Frame.Shapes[I].Type;
		if (!bSameTopology) { HitHistoryCount = 0; ++HitHistoryEpoch; }
	}
	HitHistoryNext = (HitHistoryNext + 1) % HitHistoryCapacity;
	HitHistoryCount = FMath::Min(HitHistoryCount + 1, HitHistoryCapacity);
}

bool AfpstrueCharacter::FindHitHistoryFrames(double ServerTime, const FHitHistoryFrame*& Older,
	const FHitHistoryFrame*& Newer, float& Alpha) const
{
	Older = Newer = nullptr;
	Alpha = 0.0f;
	if (!HasAuthority() || !CanMaintainEquipment() || HitHistoryCount == 0 || !FMath::IsFinite(ServerTime)) return false;
	const double Now = GetWorld()->GetTimeSeconds();
	if (ServerTime < Now - MaxHitHistorySeconds || ServerTime > Now + 0.025) return false;
	const int32 Oldest = (HitHistoryNext + HitHistoryCapacity - HitHistoryCount) % HitHistoryCapacity;
	const FHitHistoryFrame& First = HitHistory[Oldest];
	const FHitHistoryFrame& Last = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
	if (ServerTime < First.ServerTime - UE_DOUBLE_SMALL_NUMBER || ServerTime > Last.ServerTime + 0.025) return false;
	if (ServerTime >= Last.ServerTime) { Older = Newer = &Last; return true; }
	for (int32 I = 1; I < HitHistoryCount; ++I)
	{
		const FHitHistoryFrame& Next = HitHistory[(Oldest + I) % HitHistoryCapacity];
		if (Next.ServerTime >= ServerTime)
		{
			Older = &HitHistory[(Oldest + I - 1) % HitHistoryCapacity];
			Newer = &Next;
			Alpha = static_cast<float>((ServerTime - Older->ServerTime) / (Next.ServerTime - Older->ServerTime));
			return true;
		}
	}
	Older = Newer = &First;
	return true;
}

bool AfpstrueCharacter::GetHistoricalViewOrigin(double ServerTime, FVector& OutOrigin) const
{
	const FHitHistoryFrame* A;
	const FHitHistoryFrame* B;
	float Alpha;
	if (!FindHitHistoryFrames(ServerTime, A, B, Alpha))
	{
		// NetDispatch 可早于本帧骨骼采样。仅射手视点允许夹到上一帧已记录位置，不给目标命中盒外推。
		// 空历史/Teleport 断代不能借旧位置；也不将一帧宽限扩展成接受任意未来或过旧请求。
		if (!HasAuthority() || !CanMaintainEquipment() || GetWorld() == nullptr || HitHistoryCount == 0 || !FMath::IsFinite(ServerTime)) return false;
		const double Now = GetWorld()->GetTimeSeconds();
		const FHitHistoryFrame& Latest = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
		const double FrameTolerance = FMath::Min(0.05, FMath::Max(0.0, static_cast<double>(GetWorld()->GetDeltaSeconds())) + 0.0001);
		if (ServerTime < Latest.ServerTime || ServerTime > Now + 0.025 || ServerTime < Now - MaxHitHistorySeconds
			|| ServerTime - Latest.ServerTime > FrameTolerance || Now - Latest.ServerTime > FrameTolerance) return false;
		OutOrigin = Latest.ViewOrigin;
		return true;
	}
	OutOrigin = FMath::Lerp(A->ViewOrigin, B->ViewOrigin, Alpha);
	return true;
}

bool AfpstrueCharacter::TraceHistoricalHit(double ServerTime, const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	OutHit = FHitResult(Start, End);
	const FHitHistoryFrame* A;
	const FHitHistoryFrame* B;
	float Alpha;
	if (Start.ContainsNaN() || End.ContainsNaN() || !FindHitHistoryFrames(ServerTime, A, B, Alpha)) return false;
	const FVector Ray = End - Start;
	if (Ray.IsNearlyZero()) return false;
	double Best = 1.0 + UE_DOUBLE_SMALL_NUMBER;
	for (int32 I = 0; I < A->Shapes.Num(); ++I)
	{
		const auto& Older = A->Shapes[I];
		const auto& Newer = B->Shapes[I];
		const FTransform Pose(FQuat::Slerp(Older.Rotation, Newer.Rotation, Alpha).GetNormalized(),
			FMath::Lerp(Older.Location, Newer.Location, Alpha));
		const FVector Extent = FMath::Lerp(Older.Extent, Newer.Extent, Alpha);
		const FVector Origin = Pose.InverseTransformPositionNoScale(Start);
		const FVector Delta = Pose.InverseTransformVectorNoScale(Ray);
		double T = 2.0;
		FVector Normal = FVector::ZeroVector;
		const auto Sphere = [&](const FVector& Center, double Radius)
		{
			const FVector Offset = Origin - Center;
			const double C = Offset.SquaredLength() - Radius * Radius;
			const double Dot = FVector::DotProduct(Offset, Delta);
			const double LengthSquared = Delta.SquaredLength();
			const double Discriminant = Dot * Dot - LengthSquared * C;
			if (Discriminant < 0.0) return;
			const double Hit = C <= 0.0 ? 0.0 : (-Dot - FMath::Sqrt(Discriminant)) / LengthSquared;
			if (Hit >= 0.0 && Hit <= 1.0 && Hit < T)
			{
				T = Hit;
				Normal = (Origin + Delta * T - Center).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, -Delta.GetSafeNormal());
			}
		};
		if (Older.Type == 0)
		{
			double Near = 0.0, Far = 1.0;
			FVector NearNormal = -Delta.GetSafeNormal();
			for (int32 Axis = 0; Axis < 3 && Near <= Far; ++Axis)
			{
				if (FMath::Abs(Delta[Axis]) < UE_DOUBLE_SMALL_NUMBER)
				{
					if (FMath::Abs(Origin[Axis]) > Extent[Axis]) Near = 2.0;
					continue;
				}
				double Enter = (-Extent[Axis] - Origin[Axis]) / Delta[Axis];
				double Exit = (Extent[Axis] - Origin[Axis]) / Delta[Axis];
				const double Sign = Delta[Axis] > 0.0 ? -1.0 : 1.0;
				if (Enter > Exit) Swap(Enter, Exit);
				if (Enter > Near) { Near = Enter; NearNormal = FVector::ZeroVector; NearNormal[Axis] = Sign; }
				Far = FMath::Min(Far, Exit);
			}
			if (Near >= 0.0 && Near <= Far && Near <= 1.0) { T = Near; Normal = NearNormal; }
		}
		else if (Older.Type == 1) Sphere(FVector::ZeroVector, Extent.X);
		else
		{
			// Capsule 是 Z 轴线段与半径球的 Minkowski 和；圆柱侧面与两端球分别取最近交点。
			const double Radius = Extent.X, Half = Extent.Z;
			const FVector NearestAxis(0.0, 0.0, FMath::Clamp(Origin.Z, -Half, Half));
			if ((Origin - NearestAxis).SquaredLength() <= Radius * Radius) { T = 0.0; Normal = -Delta.GetSafeNormal(); }
			const double AA = Delta.X * Delta.X + Delta.Y * Delta.Y;
			const double BB = Origin.X * Delta.X + Origin.Y * Delta.Y;
			const double CC = Origin.X * Origin.X + Origin.Y * Origin.Y - Radius * Radius;
			const double Discriminant = BB * BB - AA * CC;
			if (AA > UE_DOUBLE_SMALL_NUMBER && Discriminant >= 0.0)
			{
				const double Hit = (-BB - FMath::Sqrt(Discriminant)) / AA;
				const FVector Point = Origin + Delta * Hit;
				if (Hit >= 0.0 && Hit <= 1.0 && Hit < T && FMath::Abs(Point.Z) <= Half)
				{
					T = Hit;
					Normal = FVector(Point.X, Point.Y, 0.0).GetSafeNormal();
				}
			}
			Sphere(FVector(0.0, 0.0, Half), Radius);
			Sphere(FVector(0.0, 0.0, -Half), Radius);
		}
		if (T < Best && T <= 1.0)
		{
			Best = T;
			const FVector Point = Start + Ray * T;
			OutHit = FHitResult(const_cast<AfpstrueCharacter*>(this), GetMesh(), Point, Pose.TransformVectorNoScale(Normal).GetSafeNormal());
			OutHit.bBlockingHit = true;
			OutHit.Time = static_cast<float>(T);
			OutHit.Distance = static_cast<float>(Ray.Length() * T);
			OutHit.TraceStart = Start;
			OutHit.TraceEnd = End;
			OutHit.BoneName = Older.Bone;
		}
	}
	return OutHit.bBlockingHit;
}

bool AfpstrueCharacter::TraceHistoricalHit(UWorld* World, const AfpstrueCharacter* Shooter, double ServerTime,
	const FVector& Start, const FVector& End, FHitResult& OutHit)
{
	OutHit = FHitResult(Start, End);
	if (World == nullptr || World->GetNetMode() == NM_Client || Start.ContainsNaN() || End.ContainsNaN()
		|| !FMath::IsFinite(ServerTime) || ServerTime < World->GetTimeSeconds() - MaxHitHistorySeconds
		|| ServerTime > World->GetTimeSeconds() + 0.025) return false;
	// 与普通武器射线保持相同的复杂碰撞约定，不能在联网回溯时穿过仅复杂碰撞的掩体。
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FpstrueHistoricalCover), true);
	if (Shooter != nullptr) Params.AddIgnoredActor(Shooter);
	// 当前玩家的 Mesh 不能提前挡住另一名玩家的历史位置；墙和其他环境仍按当前碰撞参与遮挡。
	for (TActorIterator<AfpstrueCharacter> It(World); It; ++It) Params.AddIgnoredActor(*It);
	World->LineTraceSingleByChannel(OutHit, Start, End, FpstrueCollisionChannels::WeaponTrace, Params);
	for (TActorIterator<AfpstrueCharacter> It(World); It; ++It)
	{
		if (*It == Shooter) continue;
		FHitResult Candidate;
		if (It->TraceHistoricalHit(ServerTime, Start, End, Candidate)
			&& (!OutHit.bBlockingHit || Candidate.Time < OutHit.Time)) OutHit = Candidate;
	}
	return OutHit.bBlockingHit;
}

/*
 * 玩家角色是“输入与表现协调层”，不拥有武器弹药和生命值这两类业务状态：
 *
 *   Enhanced Input -> Character 提交输入 -> ASC/GameplayAbility 管理动作 -> WeaponComponent 执行射击/换弹事务
 *   ApplyDamage -> HealthComponent 修改生命值 -> Delegate 回到 Character -> 蓝图/HUD 表现
 *
 * 角色维护当前装备关系，并从能力标签派生瞄准/冲刺显示；武器保存弹药和换弹事务，HealthComponent 保存生命状态。
 * 本类不启用逐帧 Tick；连射由能力的 WaitDelay 任务推进，换弹超时和后坐力恢复由武器 Timer 管理。
 * GAS 当前流程：输入只提交能力按下/松开，动作标签为唯一决策状态；本类只投影标签为速度和蓝图表现。
 */

// ==================== 默认组件与生命周期 ====================

// 构造角色的胶囊体、第一人称相机、手臂网格和可复用生命组件；构造阶段不访问运行时世界。
AfpstrueCharacter::AfpstrueCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UfpstrueCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	//胶囊体
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
	// 胶囊只保留移动碰撞职责；玩家子弹的部位判定不能先撞到外层移动胶囊。
	GetCapsuleComponent()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);
	bReplicates = true;
	SetReplicateMovement(true);
	// 第三人称身体只给其他玩家看；成套身体由蓝图配置，旧原生身体在初始化时按需补齐，不替换原第一人称动画资产。
	GetMesh()->SetRelativeLocationAndRotation(FVector(0.0, 0.0, -96.0), FRotator(0.0, -90.0, 0.0));
	GetMesh()->SetOwnerNoSee(true);
	// 服务器也需要真实身体物理状态；存活时仍是查询体，不提前启用布娃娃。
	GetMesh()->bEnablePhysicsOnDedicatedServer = true;

	//弹簧臂创建、挂载、位置、长度
	//**构造函数中创建Actor默认组件
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	CameraBoom->SetRelativeLocation(FVector(-10.f, 0.f, 60.f));
	// 第一人称相机臂长为 0，不需要弹簧臂碰撞回缩。
	CameraBoom->TargetArmLength = 0.0f;
	//旋转跟随
	CameraBoom->bUsePawnControlRotation = true;
	//平滑延迟效果（位置、旋转延迟跟随）
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 3.0f;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 3.0f;

	//相机创建、挂载、**关闭自身控制**
	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FirstPersonCameraComponent->bUsePawnControlRotation = false;

	//网格体创建、可见性、挂载、关闭动静态阴影投射、
	Mesh1P = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh1P"));
	Mesh1P->SetRelativeLocation(FVector(-30.f, 0.f, -150.f));
	Mesh1P->SetOnlyOwnerSee(true);
	Mesh1P->SetupAttachment(FirstPersonCameraComponent);
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->CastShadow = false;
	Mesh1P->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);

	//禁用角色自身 Tick：输入、碰撞、动画通知等事件驱动角色逻辑；Movement、相机和 Mesh 组件仍各自更新。
	PrimaryActorTick.bCanEverTick = false;
	//自定义健康组件
	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueCharacter::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	Super::CalcCamera(DeltaTime, OutResult);
	// 模板原设全局 0.0001 cm；这里只为原生动画表现使用 1 cm，保留近处手部且不把极小近裁面扩散到其他相机。
	if (UsesNativePresentation()) OutResult.PerspectiveNearClipPlane = 1.0f;
}

// 单机蓝图的 AutoPossessPlayer=Player0 会在第二次生成时抢走第一个玩家的 Pawn；网络只允许 GameMode 显式占有。
void AfpstrueCharacter::PreInitializeComponents()
{
	// 蓝图组件覆盖已完成后再提供旧原生角色兜底；原生动画表现已有成套 AnimClass，不再把无用 ABP_Manny 永久加入 Root。
	if (!UsesNativePresentation() && GetMesh()->GetSkeletalMeshAsset() == nullptr)
	{
		// 普通 LoadObject 由实际组件持有；不通过构造器静态查找器 AddToRoot，也不覆盖蓝图已配置的身体。
		if (USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")))
			GetMesh()->SetSkeletalMesh(BodyMesh);
	}
	if (!UsesNativePresentation() && GetMesh()->GetAnimClass() == nullptr)
	{
		if (UClass* BodyAnimation = LoadClass<UAnimInstance>(nullptr,
			TEXT("/Game/InfimaGames/ArtCore/Mannequin/Animations/ABP_Manny.ABP_Manny_C"))) GetMesh()->SetAnimInstanceClass(BodyAnimation);
	}
	if (GetNetMode() == NM_DedicatedServer)
	{
		// 保留骨骼和握枪插槽供装备校验；专服只更新第三人称权威身体，不求值不可见的第一人称手臂。
		Mesh1P->SetAnimInstanceClass(nullptr);
		// 主 AnimClass 清空并不清理网格自带的后处理实例；显式停求值，不修改共享骨骼资产或声称实例已释放。
		Mesh1P->SetDisablePostProcessBlueprint(true);
		Mesh1P->PrimaryComponentTick.bStartWithTickEnabled = false;
		Mesh1P->SetComponentTickEnabled(false);
	}
	if (GetNetMode() != NM_Standalone)
	{
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE POSSESSION_DEFAULT Name=%s AutoPlayer=%d AutoAI=%d"),
				*GetName(), static_cast<int32>(AutoPossessPlayer.GetValue()), static_cast<int32>(AutoPossessAI));
#endif
		AutoPossessPlayer = EAutoReceiveInput::Disabled;
		AutoPossessAI = EAutoPossessAI::Disabled;
	}
	Super::PreInitializeComponents();
}

// 订阅生命事件并推送初始快照，再按当前装备显示手臂、应用基础移动速度。
void AfpstrueCharacter::BeginPlay()
{
	Super::BeginPlay();
	if (UsesTemplatePresentation()) UfpstrueFootIKAnimInstance::Install(this);
	if (GetNetMode() == NM_DedicatedServer)
	{
		// 组件初始化/自动激活可能重启 Tick；在完整初始化之后再次关闭纯视模工作。
		Mesh1P->SetComponentTickEnabled(false);
	}
	if (HealthComponent != nullptr)
	{
		HealthComponent->InitAbilityActorInfo(this, this);
		// 本地预测、服务器确认和取消都读取同一组标签，不在输入和 RPC 中重复修改姿态。
		// 由 GA 激活/结束后刷新，不在引擎 PreActivate 的标签广播期间调用外部蓝图。
		if (HasAuthority())
		{
			// 能力随当前 Pawn 授予一次；重生创建新 Pawn，不把旧生命的能力和预测带过去。
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueFireAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Fire), this));
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueReloadAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Reload), this));
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueAimAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Aim), this));
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueSprintAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Sprint), this));
		}
	}

	if (HealthComponent != nullptr)
	{
		//订阅事件（受攻击、掉血、死亡）
		HealthComponent->OnHealthChanged.AddUniqueDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDeath);
		// Component 的 BeginPlay 早于 Owner；绑定后主动同步一次初始快照，避免 HUD 错过首次广播。
		HandleHealthChanged(HealthComponent->GetHealth());
	}

	// 本地手臂与远端身体使用不同投影；最终可见性由下方当前装备关系决定。
	if (bUseRARPresentation && GetNetMode() != NM_DedicatedServer)
	{
		// 只创建各 Mesh 自己的动态材质；第三人称不能套第一人称的屏幕投影变形。
		GetMesh()->SetScalarParameterValueOnMaterials(TEXT("Field Of View Alpha"), 0.0f);
		Mesh1P->SetScalarParameterValueOnMaterials(TEXT("Field Of View Alpha"), 1.0f);
	}
	if (bUseTemplatePresentation && GetNetMode() != NM_DedicatedServer)
	{
		// 模板第一人称使用完整 Manny 蒙皮；只隐藏本地视模的头和腿，不改远端身体或命中骨骼。
		Mesh1P->HideBoneByName(TEXT("head"), PBO_None);
		Mesh1P->HideBoneByName(TEXT("thigh_l"), PBO_None);
		Mesh1P->HideBoneByName(TEXT("thigh_r"), PBO_None);
	}
	const bool bHasWeapon = EquippedWeaponComponent != nullptr;
	Mesh1P->SetHiddenInGame(!bHasWeapon, true);
	// 初始化基础移动速度；后续只在冲刺和瞄准状态切换时修改。
	RefreshMovementAbilityState();
	if (GetNetMode() != NM_Standalone)
	{
		// 联网规则视点与服务器 EyesViewPoint 对齐，不让弹簧臂的平滑滞后改变准星方向或射线起点。
		// 视觉后坐力/CameraShake 仍由表现层执行；单机保留原相机延迟配置。
		CameraBoom->bEnableCameraLag = false;
		CameraBoom->bEnableCameraRotationLag = false;
		CameraBoom->SetRelativeLocation(FVector(0.0f, 0.0f, BaseEyeHeight));
		GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		GetMesh()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		GetMesh()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
		// 【设计取舍】视模投影和隐藏只服务本地镜头；第三人称身体同时服务观察与权威命中，不能拿视模替代。
		// 专服关闭第一人称 Tick，但身体仍付出必要的骨骼刷新/历史采样成本，不能套用客户端不可见剔除。
		GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		// 不能把客户端不可见裁剪/URO 的旧姿态当作服务器当前命中形状。
		if (HasAuthority()) GetMesh()->bEnableUpdateRateOptimizations = false;
		// 移动碰撞只由胶囊负责；子弹查询 PhysicsAsset 身体形状，不能让外层胶囊提前吃掉头部命中。
		GetCapsuleComponent()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);
		Mesh1P->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);
		if (HasAuthority())
		{
			BoneHistoryHandle = GetMesh()->RegisterOnBoneTransformsFinalizedDelegate(
				FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &ThisClass::RecordServerHitHistory));
			RecordServerHitHistory();
		}
		if (IsDead()) HandleDeath();
#if !UE_BUILD_SHIPPING
	#if WITH_EDITOR
		// 显式实机诊断不依赖键鼠或自动开火；等真实装备/动画复制就绪后，读取本端实际世界。
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpPresentationAudit")) || FParse::Param(FCommandLine::Get(), TEXT("PvpPickupWalk")))
		{
			FTimerHandle AuditHandle;
			GetWorldTimerManager().SetTimer(AuditHandle, FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (GEngine != nullptr && !bEndingPlay)
				{
					if (FParse::Param(FCommandLine::Get(), TEXT("PvpPresentationAudit"))) GEngine->Exec(GetWorld(), TEXT("fp.AuditRARWorld"));
					if (IsLocallyControlled() && FParse::Param(FCommandLine::Get(), TEXT("PvpPickupWalk"))) GEngine->Exec(GetWorld(), TEXT("fp.AuditPickupWalk"));
				}
			}), 5.0f, false);
		}
	#endif
		// ★【重要边界】[发布隔离] 自动输入只编入开发/测试版本；Shipping 不响应测试命令行。
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PAWN_BEGIN Name=%s Authority=%d Replicates=%d HealthRep=%d"),
				*GetName(), HasAuthority(), GetIsReplicated(), HealthComponent != nullptr && HealthComponent->GetIsReplicated());
			UE_LOG(LogTemp, Display, TEXT("PVP_MEMORY Pawn=%s NetMode=%d HistoryBytes=%llu ReservedShapes=%d ShapeStride=%llu ViewmodelAnim=%d ViewmodelTick=%d ViewmodelPostProcess=%d"),
				*GetName(), static_cast<int32>(GetNetMode()), static_cast<unsigned long long>(GetHitHistoryAllocatedBytes()),
				GetHitHistoryReservedShapeCount(), static_cast<unsigned long long>(GetHitHistoryShapeStride()),
				Mesh1P->GetAnimInstance() != nullptr, Mesh1P->IsComponentTickEnabled(), Mesh1P->GetPostProcessInstance() != nullptr);
			GetWorldTimerManager().SetTimer(NetworkSmokeTimer, this, &AfpstrueCharacter::RunPvpSmokeStep, 0.05f, true);
		}
#endif
	}
}

// 角色移动仍走 CharacterMovement；这里只复制角色级姿态和装备，生命由 HealthComponent 复制。
void AfpstrueCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstrueCharacter, EquippedWeaponComponent);
	// 拥有者从 GAS 标签预测和纠正；其他客户端只需要服务器的姿态显示镜像。
	DOREPLIFETIME_CONDITION(AfpstrueCharacter, bIsAiming, COND_SimulatedOnly);
	DOREPLIFETIME_CONDITION(AfpstrueCharacter, bIsSprinting, COND_SimulatedOnly);
	DOREPLIFETIME(AfpstrueCharacter, BodyFacingState);
}

float AfpstrueCharacter::GetBodyFacingServerTime() const
{
	const AGameStateBase* State = GetWorld()->GetGameState();
	return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

// 由服务器身体动画的既有更新驱动；胶囊、输入和 CMC 的移动预测仍完全沿用原通路。
void AfpstrueCharacter::UpdateBodyFacing(float TurnDuration)
{
	if (!HasAuthority() || !UsesTemplatePresentation()) return;
	const bool bStationary = !IsDead() && GetCharacterMovement()->IsMovingOnGround() && GetVelocity().SizeSquared2D() <= FMath::Square(3.0f);
	bool bChanged = false;
	if (!BodyFacingState.bInitialized || BodyFacingState.bStationary != bStationary)
	{
		BodyFacingState.bInitialized = true;
		BodyFacingState.bStationary = bStationary;
		BodyFacingState.FacingYaw = GetActorRotation().Yaw;
		BodyFacingState.TurnDirection = 0;
		bChanged = true;
	}
	const float Now = GetBodyFacingServerTime();
	if (bStationary)
	{
		if (BodyFacingState.TurnDirection != 0 && Now - BodyFacingState.TurnStartedServerTime >= TurnDuration)
		{
			BodyFacingState.FacingYaw = FRotator::NormalizeAxis(BodyFacingState.FacingYaw + BodyFacingState.TurnDirection * 90.0f);
			BodyFacingState.TurnDirection = 0;
			bChanged = true;
		}
		const float Difference = FMath::FindDeltaAngleDegrees(BodyFacingState.FacingYaw, GetActorRotation().Yaw);
		if (BodyFacingState.TurnDirection == 0 && FMath::Abs(Difference) > TurnTriggerYaw)
		{
			BodyFacingState.TurnDirection = Difference > 0.0f ? 1 : -1;
			BodyFacingState.TurnStartedServerTime = Now;
			bChanged = true;
		}
	}
	if (bChanged) ForceNetUpdate();
}

// 客户端先绑定装备事件，再读取弹药快照；武器快照较晚到达时还会补发弹药变化。
void AfpstrueCharacter::OnRep_EquippedWeapon()
{
	// 生命快照可以先于装备引用到达；迟到的装备不能把死亡角色的第一人称手臂重新打开。
	Mesh1P->SetHiddenInGame(IsDead() || EquippedWeaponComponent == nullptr || !IsLocallyControlled(), true);
	OnEquippedWeaponChanged.Broadcast(EquippedWeaponComponent);
	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->RefreshNetworkEquipment();
	}
}

// 先应用服务器互斥结果再通知本地动画，远端不操作本机 HUD。
void AfpstrueCharacter::OnRep_MovementModifiers()
{
	ApplyMovementSpeed();
#if !UE_BUILD_SHIPPING
	if (GetNetMode() == NM_Client && !IsLocallyControlled()
		&& FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
	{
		// 接收端只验证服务器派生的姿态与步速，不从远端显示镜像激活能力。
		if (bIsSprinting && !bIsAiming && FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, SprintSpeed))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_REMOTE_SPRINT PASS Pawn=%s Speed=%.1f"),
				*GetName(), GetCharacterMovement()->MaxWalkSpeed);
		if (bIsAiming && !bIsSprinting && FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, AimWalkSpeed))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_REMOTE_AIM PASS Pawn=%s Speed=%.1f"),
				*GetName(), GetCharacterMovement()->MaxWalkSpeed);
	}
#endif
	if (bPresentedAiming == bIsAiming) return;
	const bool bPresentLocally = GetNetMode() == NM_Standalone || IsLocallyControlled();
	if (bPresentLocally && bIsAiming)
	{
		// ★【回调边界】新 Aim 的标签先于 Spec 活跃计数建立；取消旧 Sprint 的结束回调可能在中间刷新。
		// 这时只派生镜像和速度，等 Aim Activate 再广播，蓝图才能安全取消已经完整建立的能力。
		const FGameplayAbilitySpec* Aim = HealthComponent != nullptr
			? HealthComponent->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass()) : nullptr;
		if (Aim == nullptr || !Aim->IsActive()) return;
	}
	bPresentedAiming = bIsAiming;
	if (bPresentLocally) OnAimChanged(bIsAiming);
}

// 沿用原生命组件作为唯一 ASC，外部 GE、能力及调试工具都从同一接口取得它。
UAbilitySystemComponent* AfpstrueCharacter::GetAbilitySystemComponent() const
{
	return HealthComponent;
}

// ADS/冲刺可先在本地反馈，但服务端重新校验装备、死亡和换弹，不直接接受客户端速度。
// 上述校验由对应 GA 完成；本接口只读取 GAS 已成立的状态，不接纳客户端布尔值。
void AfpstrueCharacter::RefreshMovementAbilityState()
{
	const bool bNewAiming = HealthComponent != nullptr && HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming);
	const bool bNewSprinting = HealthComponent != nullptr && HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting);
	const bool bChanged = bIsAiming != bNewAiming || bIsSprinting != bNewSprinting;
	bIsAiming = bNewAiming;
	bIsSprinting = bNewSprinting;
	// ★【状态边界】先更新全部镜像和速度，再通知蓝图；表现不能构造另一套动作状态。
	OnRep_MovementModifiers();
	if (bChanged && HasAuthority())
	{
		ForceNetUpdate();
#if !UE_BUILD_SHIPPING
		if (HealthComponent != nullptr && GetNetMode() != NM_Standalone
			&& FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		{
			// 服务端从真实能力标签核对镜像与步速，不能仅凭客户端日志证明网络请求已生效。
			const bool bAimTag = HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming);
			const bool bSprintTag = HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting);
			if (bSprintTag && !bAimTag && bIsSprinting && !bIsAiming
				&& FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, SprintSpeed))
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_MOVEMENT PASS=Sprint Pawn=%s Speed=%.1f"),
					*GetName(), GetCharacterMovement()->MaxWalkSpeed);
			if (bAimTag && !bSprintTag && bIsAiming && !bIsSprinting
				&& FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, AimWalkSpeed))
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_MOVEMENT PASS=Aim Pawn=%s Speed=%.1f"),
					*GetName(), GetCharacterMovement()->MaxWalkSpeed);
		}
#endif
	}
}

// 退出关卡、切换地图或 Actor 被销毁都会进入这里；先停止外部回调，再交给基类释放 Actor。
void AfpstrueCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// WeaponComponent 属于拾取 Actor，不能只停开火后等待玩家自动销毁它。
	bEndingPlay = true;
	if (BoneHistoryHandle.IsValid()) GetMesh()->UnregisterOnBoneTransformsFinalizedDelegate(BoneHistoryHandle);
	// Actor::Destroy 后可能仍等待 GC；退出玩法时即释放历史帧的堆内存，不让尸体保留旧缓存。
	ResetHitHistory(true);
#if !UE_BUILD_SHIPPING
	ReleaseNetworkSmokeMovementKey();
	GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
#endif
	ClearEquippedWeaponComponent(EquippedWeaponComponent);
	if (HealthComponent != nullptr) HealthComponent->CancelAllAbilities();
	RefreshMovementAbilityState();
	RemoveInputMappingContexts();

	if (HealthComponent != nullptr)
	{
		// 显式解绑，避免组件销毁顺序变化时继续回调正在退出的角色。
		HealthComponent->OnHealthChanged.RemoveDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDamageReceived.RemoveDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueCharacter::HandleDeath);
	}
	Super::EndPlay(EndPlayReason);
}

// ==================== 控制器切换与输入绑定 ====================

// 控制权移交：归还旧视角限制/输入 -> 取消旧持续动作 -> 刷新 ASC ActorInfo -> 安装新本地输入映射。
void AfpstrueCharacter::NotifyControllerChanged()
{
	// 放弃控制权时释放本角色的视角范围，不把最后一次站立限角遗留给旁观相机。
	if (APlayerController* Previous = Cast<APlayerController>(PreviousController); UsesNativePresentation() && Previous != Controller
		&& Previous && Previous->IsLocalController() && (Previous->GetPawn() == nullptr || Previous->GetPawn() == this) && Previous->PlayerCameraManager)
	{
		const APlayerCameraManager* Defaults = Previous->PlayerCameraManager->GetClass()->GetDefaultObject<APlayerCameraManager>();
		Previous->PlayerCameraManager->ViewPitchMin = Defaults->ViewPitchMin;
		Previous->PlayerCameraManager->ViewPitchMax = Defaults->ViewPitchMax;
		Previous->PlayerCameraManager->ViewYawMin = Defaults->ViewYawMin;
		Previous->PlayerCameraManager->ViewYawMax = Defaults->ViewYawMax;
	}
#if !UE_BUILD_SHIPPING
	ReleaseNetworkSmokeMovementKey();
#endif
	TGuardValue<bool> InputTransitionGuard(bOwnerInputTransition, true);
	if (HealthComponent != nullptr) HealthComponent->CancelAllAbilities();
	// Controller 变化时先清理旧控制器留下的持续输入状态。
	if (EquippedWeaponComponent != nullptr) EquippedWeaponComponent->InterruptOwnerInput();
	StopJumping();
	RefreshMovementAbilityState();
	RemoveInputMappingContexts();
	Super::NotifyControllerChanged();
	// ASC 的本地控制器信息随占有迁移刷新，不能一直使用组件 BeginPlay 时的空控制器。
	if (HealthComponent != nullptr) HealthComponent->InitAbilityActorInfo(this, this);
	RefreshMovementAbilityState();

	// Controller 变化后重新取得 LocalPlayer 的 Enhanced Input 子系统；弱引用只记录归属，不拥有它。
	APlayerController* PlayerController = Cast<APlayerController>(Controller);
	// 限制真实控制视角而非只夹住动画；鼠标、后坐力都经 CameraManager 同一入口，水平仍可连续转身。
	if (UsesNativePresentation() && PlayerController != nullptr && PlayerController->IsLocalController()
		&& PlayerController->PlayerCameraManager != nullptr)
	{
		PlayerController->PlayerCameraManager->ViewPitchMin = -ViewPitchLimit;
		PlayerController->PlayerCameraManager->ViewPitchMax = ViewPitchLimit;
	}
	UEnhancedInputLocalPlayerSubsystem* Subsystem = PlayerController != nullptr
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()) : nullptr;
	BoundInputSubsystem = Subsystem;
	if (Subsystem != nullptr && DefaultMappingContext != nullptr)
	{
		// 只记录自己新加的映射；移交/销毁时不能删除其他系统早已安装的相同 Context。
		AppliedMappingContext = DefaultMappingContext.Get();
		bAddedMappingContext = !Subsystem->HasMappingContext(DefaultMappingContext);
		if (bAddedMappingContext) Subsystem->AddMappingContext(DefaultMappingContext, 0);
	}
}

// 输入绑定注册：连续轴使用 Triggered，普通按住动作成对处理 Started/Completed，切换和命令只响应 Started。
void AfpstrueCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (EnhancedInputComponent == nullptr)
	{
		UE_LOG(LogTemplateCharacter, Error,
			   TEXT("%s (%s): Enhanced Input Component is required by this character."), *GetName(), *GetClass()->GetPathName());
		return;
	}

	// 所有可配置 InputAction 使用同一校验和日志格式；单个资产缺失不会阻止其他动作完成绑定。
	const auto IsActionAssigned = [this](const UInputAction* Action, const TCHAR* PropertyName)
	{
		if (Action != nullptr)
		{
			return true;
		}

		UE_LOG(LogTemplateCharacter, Error, TEXT("%s (%s): %s is not assigned."), *GetName(), *GetClass()->GetPathName(), PropertyName);
		return false;
	};
	// 为单个触发阶段绑定处理函数；校验只影响该 Action，不中断其他输入注册。
	const auto BindInput = [this, EnhancedInputComponent, &IsActionAssigned](const UInputAction* Action, const TCHAR* Name,
		ETriggerEvent Trigger, auto Handler)
	{
		if (IsActionAssigned(Action, Name))
		{
			EnhancedInputComponent->BindAction(Action, Trigger, this, Handler);
		}
	};
	// 按住动作同时绑定正常松开和取消，防止输入条件撤销后留下持续动作。
	const auto BindHeldInput = [this, EnhancedInputComponent, &IsActionAssigned](const UInputAction* Action, const TCHAR* Name,
		auto Start, auto Stop)
	{
		if (!IsActionAssigned(Action, Name)) return;
		EnhancedInputComponent->BindAction(Action, ETriggerEvent::Started, this, Start);
		for (ETriggerEvent Release : {ETriggerEvent::Completed, ETriggerEvent::Canceled})
		{
			EnhancedInputComponent->BindAction(Action, Release, this, Stop);
		}
	};

	// 连续轴输入：Triggered 在触发条件满足期间逐帧调用；方向键一直按住时，即使值不变也持续移动。
	BindInput(MoveAction, TEXT("MoveAction"), ETriggerEvent::Triggered, &AfpstrueCharacter::Move);
	BindInput(LookAction, TEXT("LookAction"), ETriggerEvent::Triggered, &AfpstrueCharacter::Look);

	// 按住型输入：Completed 与 Canceled 都结束动作，覆盖 Trigger 条件变化和输入被撤销。
	BindHeldInput(JumpAction, TEXT("JumpAction"), &ACharacter::Jump, &ACharacter::StopJumping);
	BindHeldInput(FireAction, TEXT("FireAction"), &AfpstrueCharacter::StartWeaponFire, &AfpstrueCharacter::StopWeaponFire);
	BindHeldInput(AimAction, TEXT("AimAction"), &AfpstrueCharacter::StartAim, &AfpstrueCharacter::StopAim);

	// 切换型输入：SprintAction 每次按下切换一次，不在松开时自动停止。
	BindInput(SprintAction, TEXT("SprintAction"), ETriggerEvent::Started, &AfpstrueCharacter::ToggleSprint);

	// 单次命令：换弹只提交能力请求；单机由身份化动画/显式完成，联网由服务器计时完成。
	BindInput(ReloadAction, TEXT("ReloadAction"), ETriggerEvent::Started, &AfpstrueCharacter::RequestWeaponReload);
}

// 只移除本角色实际添加的映射，保留其他系统拥有的同名 Context。
void AfpstrueCharacter::RemoveInputMappingContexts()
{
	// Controller 更换或角色退出时成对移除本角色添加的映射，并清空不拥有对象生命周期的弱引用。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
	{
#if !UE_BUILD_SHIPPING
		if (bNetworkSmokeInjectingMove)
		{
			Subsystem->StopContinuousInputInjectionForAction(MoveAction);
			bNetworkSmokeInjectingMove = false;
		}
#endif
		if (bAddedMappingContext && AppliedMappingContext.IsValid())
		{
			Subsystem->RemoveMappingContext(AppliedMappingContext.Get());
		}
	}
	// 清空弱引用，防止下一次 Controller 切换误用旧子系统。
	BoundInputSubsystem.Reset();
	AppliedMappingContext.Reset();
	bAddedMappingContext = false;
}

// ==================== 移动与视角输入 ====================

// 接收连续移动轴并提交前后、左右方向输入，实际位移与碰撞由 CharacterMovement 处理。
void AfpstrueCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr && CanAcceptGameplayInput())
	{
		//X:左右，Y:前后
		AddMovementInput(GetActorForwardVector(), MovementVector.Y);
		AddMovementInput(GetActorRightVector(), MovementVector.X);
	}
}

// 将连续视角输入交给控制器；无控制权、死亡或退出期间不改变控制旋转。
void AfpstrueCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr && CanAcceptGameplayInput())
	{
		//X：摇头  Y：点头
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

// 按键切换冲刺能力；互斥由能力标签检查，角色经唯一速度入口应用结果。
void AfpstrueCharacter::ToggleSprint()
{
	// 互斥条件在 GA 中集中声明；角色只把切换输入转成激活或取消当前能力。
	if (HealthComponent == nullptr || !CanAcceptGameplayInput() || (!HasAuthority() && !IsLocallyControlled())) return;
	// 用当前能力是否激活决定本次按键是取消还是请求冲刺，不另写冲刺状态。
	const FGameplayAbilitySpec* Sprint = HealthComponent->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass());
	if (Sprint != nullptr && Sprint->IsActive()) HealthComponent->CancelAbilityHandle(Sprint->Handle);
	else
	{
		HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Sprint));
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Sprint));
	}
}

// ==================== 瞄准 ====================

// 提交瞄准按下；能力验证装备并取消冲刺，状态成立后才通知蓝图。
void AfpstrueCharacter::StartAim()
{
	// 角色只检查输入归属；装备资格和动作互斥由 Aim GA 判断。
	if (HealthComponent == nullptr || !CanAcceptGameplayInput() || (!HasAuthority() && !IsLocallyControlled())) return;

	HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Aim));
}

// 幂等退出瞄准并重算步速；蓝图通知只在真实状态变化时发送。
void AfpstrueCharacter::StopAim()
{
	// 松开不再检查激活资格；WaitInputRelease 同步释放，GAS 结束后统一派生姿态与步速。
	if (HealthComponent != nullptr)
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
}

// ==================== 武器输入与装备关系 ====================

// 这里是输入边界：Character 不直接扣弹、射线检测或改变动作标签，只向 ASC 提交能力输入。
void AfpstrueCharacter::StartWeaponFire()
{
	// 与换弹入口一致：Character 只转发能力输入，装备资格和动作互斥由 GA 校验。
	if (HealthComponent != nullptr && CanAcceptGameplayInput() && (HasAuthority() || IsLocallyControlled()))
	{
		HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
	}
}

// 将松开或取消输入转交 ASC，只结束开火能力，不结束正在进行的换弹。
void AfpstrueCharacter::StopWeaponFire()
{
	// 松开输入、死亡、换 Controller 和 EndPlay 都可安全调用，由 GAS 幂等处理输入释放。
	if (HealthComponent != nullptr)
	{
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
	}
}

// 向 ASC 提交一次换弹请求；GA 校验动作规则，武器负责装弹与事务结束时机。
void AfpstrueCharacter::RequestWeaponReload()
{
	// 手动与自动换弹共用 Reload 能力，由能力取消瞄准/冲刺，武器只校验真实弹药与装备资源。
	if (HealthComponent != nullptr && CanAcceptGameplayInput() && (HasAuthority() || IsLocallyControlled()))
	{
		HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Reload));
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Reload));
	}
}

// 独立公开命令：完整取消冲刺和瞄准，不触碰开火、换弹与弹药状态。
void AfpstrueCharacter::ResetMovementModifiers()
{
	// 调用者不必预先取消能力：本命令取消移动能力后，从剩余标签重算步速并广播瞄准变化。
	// 能力结束的蓝图回调可能建立新动作；刷新只读取当前标签，避免覆盖监听者的新状态。
	// 只取消移动修饰能力，不能顺带结束调用本接口的 Reload 或其他战斗能力。
	if (HealthComponent != nullptr)
	{
		FGameplayTagContainer MovementAbilities;
		MovementAbilities.AddTag(FpstrueGameplayTags::Ability_Aim);
		MovementAbilities.AddTag(FpstrueGameplayTags::Ability_Sprint);
		HealthComponent->CancelAbilities(&MovementAbilities);
	}
	RefreshMovementAbilityState();
}

// 判断角色是否仍允许维持装备关系；临时失去输入与角色死亡分别处理。
bool AfpstrueCharacter::CanMaintainEquipment() const
{
	return !bEndingPlay && !IsActorBeingDestroyed() && !IsDead();
}

// 在存活与生命周期资格上增加控制权迁移保护，拒绝迁移回调中的新输入动作。
bool AfpstrueCharacter::CanAcceptGameplayInput() const
{
	return CanMaintainEquipment() && !bOwnerInputTransition;
}

// 唯一写入 MaxWalkSpeed 的入口，按瞄准优先、其次冲刺、最后步行选择速度。
void AfpstrueCharacter::ApplyMovementSpeed()
{
	GetCharacterMovement()->MaxWalkSpeed = bIsAiming ? AimWalkSpeed : (bIsSprinting ? SprintSpeed : WalkSpeed);
	if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(GetCharacterMovement()))
		Movement->SetMovementIntent(bIsAiming, bIsSprinting);
}

// 装备成功后由 WeaponComponent 回调本函数；这里仅登记关系并广播，避免角色与武器各维护一份弹药状态。
void AfpstrueCharacter::SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent)
{
	if (WeaponComponent == nullptr || EquippedWeaponComponent == WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent = WeaponComponent;
	ForceNetUpdate();
	Mesh1P->SetHiddenInGame(false, true);
	OnEquippedWeaponChanged.Broadcast(WeaponComponent);
}

// 只转交仍匹配的装备执行完整卸下；不能绕过武器取消/清理而直接清角色槽位。
void AfpstrueCharacter::ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent->DetachWeapon();
}

// 卸枪事务完成后释放匹配槽位；先提交空槽，再发事件，防止旧回调覆盖新装备。
void AfpstrueCharacter::ReleaseEquippedWeaponComponent(const UfpstrueWeaponComponent* ExpectedWeapon)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != ExpectedWeapon) return;
	// ★【重要边界】[资源所有权] 只释放调用者匹配的装备槽；先置空再退出瞄准，旧卸枪栈不能覆盖回调中装备的新武器。
	EquippedWeaponComponent = nullptr;
	ForceNetUpdate();
	Mesh1P->SetHiddenInGame(true, true);
	StopAim();
	// OnAimChanged 也能装备新武器；此时不要继续广播过期的空槽快照。
	if (EquippedWeaponComponent == nullptr) OnEquippedWeaponChanged.Broadcast(nullptr);
}

// ==================== 生命与伤害事件 ====================

// HealthComponent 是生命值唯一写入者，Character 只做 C++ Gameplay -> 蓝图表现层的事件桥接。
void AfpstrueCharacter::HandleHealthChanged(float NewHealth)
{
	if (GetNetMode() == NM_Standalone || IsLocallyControlled()) OnPlayerHealthChanged(NewHealth);
}

// Health 只报告权威端实际扣除的正伤害；这里记录计分来源并分发表现，不再次结算。
void AfpstrueCharacter::HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	if (!HasAuthority()) return;
	LastDamageInstigator = InstigatedBy;
	if (UsesNativePresentation() && !IsDead()) MulticastPresentHit();
	if (GetNetMode() == NM_Standalone) OnPlayerDamaged(DamageAmount, DamageCauser, InstigatedBy);
	// 【设计取舍】GAS 数值复制负责持久生命事实；此 RPC 只是可丢弃的镜头/音效反馈，不占可靠重传队列。
	// 仅实际脚本覆盖才发送，避免原生角色空转同步；不按素材类型封死子类合法的蓝图表现。
	else if (GetClass()->IsFunctionImplementedInScript(GET_FUNCTION_NAME_CHECKED(AfpstrueCharacter, OnPlayerDamaged)))
		ClientReceiveDamage(DamageAmount, DamageCauser, InstigatedBy);
}

// 每个视角只消费服务器已发生的受击事实；无可靠积压，不给晚加入者重放历史受击。
void AfpstrueCharacter::MulticastPresentHit_Implementation()
{
	for (USkeletalMeshComponent* PresentationMesh : {GetMesh(), Mesh1P.Get()})
		if (UfpstruePlayerAnimInstance* Animation = Cast<UfpstruePlayerAnimInstance>(PresentationMesh->GetAnimInstance())) Animation->PresentHit();
}

// 拥有者在本地播放旧蓝图镜头/音效；丢包不补播，不重放 ApplyDamage 或改变复制生命值。
void AfpstrueCharacter::ClientReceiveDamage_Implementation(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	if (IsLocallyControlled()) OnPlayerDamaged(DamageAmount, DamageCauser, InstigatedBy);
}

// 一次性结束玩家玩法：撤销移动修饰、禁用武器和移动，再通知对局与死亡表现。
void AfpstrueCharacter::HandleDeath()
{
	// HealthComponent 已保证 OnDeath 只广播一次；本地标志再保护角色侧移动、武器和蓝图表现不被重复执行。
	if (bDeathEffectsApplied)
	{
		return;
	}

	// 先锁存角色侧死亡处理，再清理输入和装备；清理回调不能重复进入同一死亡链。
	bDeathEffectsApplied = true;
	const FVector DeathVelocity = GetVelocity();
	ResetHitHistory();
#if !UE_BUILD_SHIPPING
	ReleaseNetworkSmokeMovementKey();
	if (bNetworkSmokeInjectingMove)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
			Subsystem->StopContinuousInputInjectionForAction(MoveAction);
		bNetworkSmokeInjectingMove = false;
	}
#endif
	// Health 在提交死亡事实（含客户端初始死亡快照）时已取消能力；这里仅消费结果。
	RefreshMovementAbilityState();

	if (EquippedWeaponComponent != nullptr)
	{
		if (UsesNativePresentation()) EquippedWeaponComponent->DropForDeath(DeathVelocity);
		else EquippedWeaponComponent->DisableWeapon();
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}
	if (UsesNativePresentation())
	{
		// 死亡先退出动作播放，再交给同套身体 PhysicsAsset；布娃娃只做表现，不产生新的伤害或计分。
		Mesh1P->SetHiddenInGame(true, true);
		for (USkeletalMeshComponent* PresentationMesh : {GetMesh(), Mesh1P.Get()})
			if (UAnimInstance* Animation = PresentationMesh->GetAnimInstance()) Animation->Montage_Stop(0.0f);
		if (GetMesh()->GetPhysicsAsset() != nullptr)
		{
			// 同一次死亡处理完成松手与身体物理；不等动画结束或另一个 Tick，跨端到达仍以网络为准。
			GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			// 等待重生时仍使用本人的第一人称相机；保留 OwnerNoSee，避免从布娃娃体内看见自己。
			// 该标志不隐藏其他玩家看到的尸体，也不停止其真实物理。
			GetMesh()->SetCollisionProfileName(TEXT("Ragdoll"));
			GetMesh()->SetSimulatePhysics(true);
			GetMesh()->SetAllPhysicsLinearVelocity(DeathVelocity);
		}
	}

#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpPresentationAudit")) && UsesNativePresentation())
	{
		const TWeakObjectPtr<UfpstrueWeaponComponent> Gun = EquippedWeaponComponent;
		UE_LOG(LogTemp, Display, TEXT("PVP_DEATH_DROP Begin Authority=%d BodySim=%d GunSim=%d Detached=%d Velocity=%s"),
			HasAuthority(), GetMesh()->IsSimulatingPhysics(), Gun.IsValid() && Gun->IsSimulatingPhysics(),
			Gun.IsValid() && Gun->GetAttachParent() == nullptr, *DeathVelocity.ToString());
		FTimerHandle PhysicsAuditTimer;
		GetWorldTimerManager().SetTimer(PhysicsAuditTimer, FTimerDelegate::CreateWeakLambda(this, [this, Gun]()
		{
			if (Gun.IsValid()) UE_LOG(LogTemp, Display, TEXT("PVP_DEATH_DROP Settling Authority=%d BodySim=%d GunSim=%d Detached=%d GunLocation=%s GunVelocity=%s"),
				HasAuthority(), GetMesh()->IsSimulatingPhysics(), Gun->IsSimulatingPhysics(), Gun->GetAttachParent() == nullptr,
				*Gun->GetComponentLocation().ToString(), *Gun->GetPhysicsLinearVelocity().ToString());
		}), 0.8f, false);
	}
	// 显式死亡画面验收：只暂停测试输入，不暂停世界/物理；记录真实网络死亡后枪与身体的连续状态。
	if (GetNetMode() == NM_Client && !IsLocallyControlled() && UsesNativePresentation()
		&& FParse::Param(FCommandLine::Get(), TEXT("PvpDeathVisual")))
	{
		APlayerController* Viewer = UGameplayStatics::GetPlayerController(this, 0);
		AfpstrueCharacter* ViewerPawn = Viewer != nullptr ? Cast<AfpstrueCharacter>(Viewer->GetPawn()) : nullptr;
		FString Directory;
		if (ViewerPawn != nullptr && ViewerPawn->IsLocallyControlled() && !ViewerPawn->IsDead()
			&& FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), Directory) && !Directory.IsEmpty())
		{
			ViewerPawn->StopWeaponFire();
			GetWorldTimerManager().ClearTimer(ViewerPawn->NetworkSmokeTimer);
			const FVector LookAt = GetActorLocation() - FVector(0.0, 0.0, 55.0);
			Viewer->SetControlRotation((LookAt - ViewerPawn->FirstPersonCameraComponent->GetComponentLocation()).Rotation());
			const TWeakObjectPtr<UfpstrueWeaponComponent> DroppedWeapon = EquippedWeaponComponent;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FTimerHandle CaptureTimer;
				const float Delay = Index == 0 ? 0.1f : Index == 1 ? 0.65f : 1.6f;
				GetWorldTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateWeakLambda(this,
					[this, DroppedWeapon, Directory, Index]()
					{
						UfpstrueWeaponComponent* Gun = DroppedWeapon.Get();
						if (!IsDead() || Gun == nullptr || !Gun->IsDroppedOnDeath()) return;
						UE_LOG(LogTemp, Display, TEXT("PVP_DEATH_VISUAL Frame=%d BodySim=%d GunSim=%d Detached=%d GunLocation=%s BodyLocation=%s"),
							Index, GetMesh()->IsSimulatingPhysics(), Gun->IsSimulatingPhysics(), Gun->GetAttachParent() == nullptr,
							*Gun->GetComponentLocation().ToString(), *GetMesh()->GetSocketLocation(TEXT("pelvis")).ToString());
						if (!FScreenshotRequest::IsScreenshotRequested())
							FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString::Printf(TEXT("death_%d.png"), Index)), true, false, false);
					}), Delay, false);
			}
		}
	}
#endif

	// 玩法已经停止后才通知对局和蓝图；这些监听者可能同步切图或结束比赛。
	OnPlayerDeathReported.Broadcast(this);
	if (GetNetMode() == NM_Standalone) OnPlayerDied();
	else
	{
		if (!UsesNativePresentation()) GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (HasAuthority())
		{
			if (AfpstruePvpGameMode* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>())
				Mode->HandlePlayerEliminated(this, LastDamageInstigator.Get());
		}
		if (IsLocallyControlled() && GEngine != nullptr)
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red, TEXT("Eliminated. Respawning..."));
	}
}

#if !UE_BUILD_SHIPPING
// 只释放本烟测按下的 W；沿原 Controller 配对归还，失去占有或退出时不留下下一角色的持续输入。
void AfpstrueCharacter::ReleaseNetworkSmokeMovementKey()
{
	APlayerController* Player = NetworkSmokeMovementController.Get();
	NetworkSmokeMovementController.Reset();
	if (Player != nullptr && Player->PlayerInput != nullptr)
		Player->InputKey(FInputKeyParams(EKeys::W, IE_Released, 0.0));
}

// 显式功能冒烟：客户端真实输入经 GAS 激活与输入同步到服务端，验收复制弹药、击杀成绩和新 Pawn，而非服务端直接造结果。
void AfpstrueCharacter::RunPvpSmokeStep()
{
	if (NetworkSmokeMovementController.IsValid()
		&& (NetworkSmokePhase != 9 || !IsLocallyControlled() || HasAuthority() || IsDead()
			|| Controller != NetworkSmokeMovementController.Get() || EquippedWeaponComponent == nullptr
			|| HealthComponent == nullptr || !HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting)))
		ReleaseNetworkSmokeMovementKey();
	const bool bMovingHitScenario = FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit"));
	// 混合场景必须看到服务器生成的真实敌人副本；观察端也检查，不借另一个客户端的日志充数。
	if (!HasAuthority() && IsLocallyControlled() && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokePve")))
	{
		for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
		{
			const UfpstrueHealthComponent* EnemyHealth = It->FindComponentByClass<UfpstrueHealthComponent>();
			if (It->GetIsReplicated() && EnemyHealth != nullptr && EnemyHealth->GetHealthAttributeSet() != nullptr)
			{
				if (!bNetworkSmokePveSeen)
				{
					bNetworkSmokePveSeen = true;
					UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_PVE_REPLICATED PASS Enemy=%s Health=%.1f"), *It->GetName(), EnemyHealth->GetHealth());
				}
				if (!It->IsDead() && !NetworkSmokePveTarget.IsValid()) NetworkSmokePveTarget = *It;
			}
		}
	}
	// 新进程只验证服务器恢复的身份与状态，绝不重跑旧射击、击杀和自动装弹场景。
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeRestoreOnly"))) return;
	if (NetworkSmokePhase >= 13)
	{
		if (!IsLocallyControlled() || HasAuthority() || IsDead() || Controller == nullptr || EquippedWeaponComponent == nullptr) return;
		AfpstrueEnemyCharacter* Enemy = NetworkSmokePveTarget.Get();
		if (Enemy == nullptr) return;
		if (Enemy->IsDead())
		{
			StopWeaponFire();
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpWeakPolicyCorrectness")))
			{
				// 边界场景必须等真实 ACK 和服务器动作快照收尾，不能只看到击杀便停止验证。
				if (EquippedWeaponComponent->GetPendingPredictedShotCount() != 0 || EquippedWeaponComponent->IsFiring()
					|| EquippedWeaponComponent->IsReloading() || IsAiming() || IsSprinting()) return;
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE WEAK_POLICY_SETTLED Ammo=%d Reserve=%d Outstanding=0 Firing=0 Reloading=0 Aiming=0 Sprinting=0"),
					EquippedWeaponComponent->GetCurrentAmmo(), EquippedWeaponComponent->GetReserveAmmo());
			}
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_PVE_COMPLETE PASS Enemy=%s Health=%.1f"), *Enemy->GetName(), GetCurrentHealth());
			GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
			return;
		}
		Controller->SetControlRotation((Enemy->GetActorLocation() - GetPawnViewLocation()).Rotation());
		// AI 自然选择有效玩家；服务器确认它真正伤到任一玩家后，主动端才以真实输入反击。
		const auto* Player = Cast<AfpstruePvpPlayerController>(Controller);
		if (NetworkSmokePhase == 13 && Player != nullptr && Player->IsPveSmokeCounterattackReady())
		{
			StartWeaponFire();
			NetworkSmokePhase = 14;
		}
		return;
	}
	if (!HasAuthority() && GetWorld()->GetTimeSeconds() >= NetworkSmokeNextDiagnostic)
	{
		NetworkSmokeNextDiagnostic = GetWorld()->GetTimeSeconds() + 2.0;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE WAIT Name=%s Local=%d Controller=%s Weapon=%s Ammo=%d Phase=%d Paused=%d"),
			*GetName(), IsLocallyControlled(), *GetNameSafe(Controller), *GetNameSafe(EquippedWeaponComponent),
			EquippedWeaponComponent != nullptr ? EquippedWeaponComponent->GetCurrentAmmo() : -1, NetworkSmokePhase, GetWorld()->IsPaused());
		const AGameStateBase* State = GetWorld()->GetGameState();
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CONTEXT Name=%s Role=%d PS=%s GS=%s Players=%d Owner=%s"),
			*GetName(), static_cast<int32>(GetLocalRole()), *GetNameSafe(GetPlayerState()), *GetNameSafe(State),
			State != nullptr ? State->PlayerArray.Num() : -1, *GetNameSafe(GetOwner()));
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			if (PC != nullptr) UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PC Name=%s Local=%d Pawn=%s PS=%s Role=%d"),
				*PC->GetName(), PC->IsLocalController(), *GetNameSafe(PC->GetPawn()), *GetNameSafe(PC->PlayerState), static_cast<int32>(PC->GetLocalRole()));
		}
	}
	if (!IsLocallyControlled() || HasAuthority() || IsDead() || EquippedWeaponComponent == nullptr
		|| HealthComponent == nullptr || HealthComponent->GetHealthAttributeSet() == nullptr
		|| HealthComponent->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass()) == nullptr
		|| HealthComponent->FindAbilitySpecFromClass(UfpstrueReloadAbility::StaticClass()) == nullptr
		|| HealthComponent->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass()) == nullptr
		|| HealthComponent->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass()) == nullptr
		|| (NetworkSmokeStarted < 0.0 && (EquippedWeaponComponent->GetCurrentAmmo() <= 0 || EquippedWeaponComponent->GetReserveAmmo() <= 0))) return;
	AfpstrueCharacter* Other = nullptr;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
		if (*It != this && !It->IsDead() && It->HasEquippedWeapon()) { Other = *It; break; }
	AfpstruePvpPlayerState* PS = GetPlayerState<AfpstruePvpPlayerState>();
	if (Other == nullptr || PS == nullptr || Other->GetPlayerState() == nullptr) return;
	if (NetworkSmokeStarted < 0.0)
	{
		NetworkSmokeStarted = GetWorld()->GetTimeSeconds();
		NetworkSmokeInitialReserve = EquippedWeaponComponent->GetReserveAmmo();
		NetworkSmokeTarget = Other;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_READY Player=%d"), PS->GetPlayerId());
		// 持续注入真实输入 Action，避免高帧率 NullRHI 下 Timer 的单帧脉冲被移动组件消费后立即制动。
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get(); Subsystem != nullptr && MoveAction != nullptr)
		{
			const float MoveDirection = bMovingHitScenario && PS->GetPlayerId() > Other->GetPlayerState()->GetPlayerId() ? -0.5f : 0.5f;
			Subsystem->StartContinuousInputInjectionForAction(MoveAction, FInputActionValue(FVector2D(MoveDirection, 0.0f)), {}, {});
			bNetworkSmokeInjectingMove = true;
		}
		const UCharacterMovementComponent* Movement = GetCharacterMovement();
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVEMENT_CONTEXT Class=%s Mode=%d Active=%d Tick=%d IgnoreInput=%d Location=%s"),
			*GetNameSafe(Movement != nullptr ? Movement->GetClass() : nullptr), Movement != nullptr ? int32(Movement->MovementMode) : -1,
			Movement != nullptr && Movement->IsActive(), Movement != nullptr && Movement->IsComponentTickEnabled(),
			Controller != nullptr && Controller->IsMoveInputIgnored(), *GetActorLocation().ToCompactString());
	}
	const double Elapsed = GetWorld()->GetTimeSeconds() - NetworkSmokeStarted;
	if (Elapsed >= (bMovingHitScenario ? 5.0 : 1.0) && bNetworkSmokeInjectingMove)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
			Subsystem->StopContinuousInputInjectionForAction(MoveAction);
		bNetworkSmokeInjectingMove = false;
	}
	if (PS->GetPlayerId() > Other->GetPlayerState()->GetPlayerId())
	{
		// 显式观察端验收只转动本端相机，不改变另一名玩家输入、能力或服务器命中。
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeObserve")) && Controller != nullptr)
			Controller->SetControlRotation((Other->GetActorLocation() - GetPawnViewLocation()).Rotation());
		return;
	}
	if (Controller == nullptr) return;
	const FVector AimPoint = bMovingHitScenario && Other->GetMesh() != nullptr && Other->GetMesh()->DoesSocketExist(TEXT("spine_03"))
		? Other->GetMesh()->GetSocketLocation(TEXT("spine_03")) : Other->GetActorLocation();
	const FRotator Aim = NetworkSmokePhase >= 7 ? FRotator(60.0f, 0.0f, 0.0f)
		: (AimPoint - GetPawnViewLocation()).Rotation();
	Controller->SetControlRotation(Aim);
	if (NetworkSmokePhase == 0 && Elapsed >= 2.0)
	{
		if (bMovingHitScenario)
		{
			// 显式专项必须实际横移后才射击；命令已发出或MaxWalkSpeed正确都不能冒充正在移动。
			if (Other->GetVelocity().Size2D() < 20.0f) return;
			NetworkSmokeMovingInitialHealth = Other->GetCurrentHealth();
			bNetworkSmokeMovingHitAttempted = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_ATTEMPT Target=%s TargetSpeed=%.2f Health=%.1f"),
				*Other->GetName(), Other->GetVelocity().Size2D(), NetworkSmokeMovingInitialHealth);
		}
		StartWeaponFire();
		NetworkSmokePhase = 1;
	}
	else if (NetworkSmokePhase == 1)
	{
		StopWeaponFire();
		NetworkSmokePhase = 2;
		NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
	}
	else if (NetworkSmokePhase == 2 && FParse::Param(FCommandLine::Get(), TEXT("PvpWeakPolicyCorrectness"))
		&& GetWorld()->GetTimeSeconds() - NetworkSmokePhaseStarted >= 1.0
		&& EquippedWeaponComponent->GetCurrentAmmo() == EquippedWeaponComponent->GetMagazineSize()
		&& EquippedWeaponComponent->GetPendingPredictedShotCount() == 0 && !EquippedWeaponComponent->IsFiring()
		&& !EquippedWeaponComponent->IsReloading())
	{
		// 显式边界场景允许在真实拒绝清账后重新按键，不能假定第一发总会成功，也不重发旧 ShotId。
		if (NetworkSmokeWarmupRetries >= 3)
		{
			UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL Reason=PolicyWarmupNoAcceptedShot Attempts=4"));
			GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE POLICY_WARMUP_RETRY Attempt=%d Ammo=%d Outstanding=0"),
			++NetworkSmokeWarmupRetries, EquippedWeaponComponent->GetCurrentAmmo());
		StartWeaponFire();
		NetworkSmokePhase = 1;
	}
	else if (NetworkSmokePhase == 2 && Elapsed >= 3.0 && EquippedWeaponComponent->GetCurrentAmmo() < EquippedWeaponComponent->GetMagazineSize())
	{
		if (bMovingHitScenario && bNetworkSmokeMovingHitAttempted)
		{
			// 只有同一个目标的服务器血量复制确实降低才通过；本地枪口/命中特效不算伤害证据。
			if (Other != NetworkSmokeTarget.Get() || Other->GetCurrentHealth() >= NetworkSmokeMovingInitialHealth) return;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_CLIENT PASS Target=%s HealthBefore=%.1f HealthAfter=%.1f"),
				*Other->GetName(), NetworkSmokeMovingInitialHealth, Other->GetCurrentHealth());
			bNetworkSmokeMovingHitAttempted = false;
		}
		RequestWeaponReload();
		NetworkSmokePhase = 3;
	}
	else if (NetworkSmokePhase == 3 && !EquippedWeaponComponent->IsReloading()
		&& EquippedWeaponComponent->GetCurrentAmmo() == EquippedWeaponComponent->GetMagazineSize()
		&& EquippedWeaponComponent->GetReserveAmmo() < NetworkSmokeInitialReserve)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_STATE PASS Ammo=%d Reserve=%d"), EquippedWeaponComponent->GetCurrentAmmo(), EquippedWeaponComponent->GetReserveAmmo());
		NetworkSmokePhase = 4;
	}
	else if (NetworkSmokePhase == 4 && Elapsed >= 6.0)
	{
		StartWeaponFire();
		NetworkSmokePhase = 5;
	}
	else if (NetworkSmokePhase == 5 && PS->GetKills() >= 1)
	{
		StopWeaponFire();
		NetworkSmokePhase = 6;
	}
	else if (NetworkSmokePhase == 6 && Other != NetworkSmokeTarget.Get() && PS->GetKills() >= 1 && Other->GetCurrentHealth() > 0.0f)
	{
		// 朝空中打完整个剩余弹匣，单独验证服务器空仓自动激活 Reload，不借 R 键掩盖旁路问题。
		NetworkSmokeInitialReserve = EquippedWeaponComponent->GetReserveAmmo();
		Controller->SetControlRotation(FRotator(60.0f, 0.0f, 0.0f));
		StartWeaponFire();
		NetworkSmokePhase = 7;
	}
	else if (NetworkSmokePhase == 7 && !EquippedWeaponComponent->IsReloading()
		&& EquippedWeaponComponent->GetCurrentAmmo() == EquippedWeaponComponent->GetMagazineSize()
		&& EquippedWeaponComponent->GetReserveAmmo() < NetworkSmokeInitialReserve)
	{
		StopWeaponFire();
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_AUTO_RELOAD PASS Ammo=%d Reserve=%d"),
			EquippedWeaponComponent->GetCurrentAmmo(), EquippedWeaponComponent->GetReserveAmmo());
		NetworkSmokePhase = 8;
		NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
	}
	else if (NetworkSmokePhase == 8 && !HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading))
	{
		// 前面的射击与重生验收完成后，再用真实输入验证 GAS 姿态及跨进程镜像。
		APlayerController* Player = Cast<APlayerController>(Controller);
		if (Player == nullptr || Player->PlayerInput == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL Reason=SprintMissingPlayerInput"));
			GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
			return;
		}
		ToggleSprint();
		// 标准按键保持到下一个阶段，由 EnhancedInput 每帧提交 Move，不用 Timer 单帧推力伪装持续跑动。
		NetworkSmokeMovementController = Player;
		Player->InputKey(FInputKeyParams(EKeys::W, IE_Pressed, 1.0));
		NetworkSmokePhase = 9;
		NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
	}
	else if (NetworkSmokePhase >= 9 && GetWorld()->GetTimeSeconds() - NetworkSmokePhaseStarted >= 1.0)
	{
		const FGameplayAbilitySpec* AimSpec = HealthComponent->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass());
		const FGameplayAbilitySpec* SprintSpec = HealthComponent->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass());
		const bool bAimActive = AimSpec->IsActive();
		const bool bSprintActive = SprintSpec->IsActive();
		const bool bAimTag = HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming);
		const bool bSprintTag = HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting);
		const float Speed = GetCharacterMovement()->MaxWalkSpeed;
		const float ActualSpeed = GetVelocity().Size2D();
		const bool bSprintConsistent = bSprintActive && bSprintTag && bIsSprinting
			&& !bAimActive && !bAimTag && !bIsAiming && FMath::IsNearlyEqual(Speed, SprintSpeed)
			&& GetCharacterMovement()->IsMovingOnGround() && ActualSpeed > 3.0f
			&& GetCharacterMovement()->GetCurrentAcceleration().SizeSquared2D() > UE_KINDA_SMALL_NUMBER;
		const bool bAimConsistent = bAimActive && bAimTag && bIsAiming
			&& !bSprintActive && !bSprintTag && !bIsSprinting && FMath::IsNearlyEqual(Speed, AimWalkSpeed);
		if (NetworkSmokePhase == 9 && bSprintConsistent)
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SPRINT PASS Speed=%.1f ActualSpeed=%.1f Input=StandardW"), Speed, ActualSpeed);
			ReleaseNetworkSmokeMovementKey();
			StartAim();
			NetworkSmokePhase = 10;
			NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
		}
		else if (NetworkSmokePhase == 10 && bAimConsistent)
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_AIM PASS Speed=%.1f"), Speed);
			ToggleSprint();
			NetworkSmokePhase = 11;
			NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
		}
		else if (NetworkSmokePhase == 11 && bAimConsistent)
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SPRINT_BLOCKED PASS Speed=%.1f"), Speed);
			StopAim();
			NetworkSmokePhase = 12;
			NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
		}
		else if (NetworkSmokePhase == 12 && !bAimActive && !bSprintActive && !bAimTag && !bSprintTag
			&& !bIsAiming && !bIsSprinting && FMath::IsNearlyEqual(Speed, WalkSpeed))
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_MOVEMENT PASS Speed=%.1f"), Speed);
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_COMPLETE PASS Kills=%d OtherHealth=%.1f"), PS->GetKills(), Other->GetCurrentHealth());
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokePve")))
			{
				NetworkSmokePhase = 13;
				if (auto* Player = Cast<AfpstruePvpPlayerController>(Controller)) Player->ServerBeginPveSmoke();
			}
			else GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
		}
	}
}

#endif

// 从生命组件读取死亡事实，角色不维护另一份可变血量或死亡判定。
bool AfpstrueCharacter::IsDead() const
{
	return HealthComponent != nullptr && HealthComponent->IsDead();
}

// 为 HUD 和其他模块提供当前血量的只读快照；没有组件时返回零。
float AfpstrueCharacter::GetCurrentHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
}

// 读取生命组件的最大血量，角色不复制或改写该配置。
float AfpstrueCharacter::GetMaxHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetMaxHealth() : 0.0f;
}

// 复用组件的归一化结果，HUD 不必自行计算比例或处理除零。
float AfpstrueCharacter::GetHealthNormalized() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealthNormalized() : 0.0f;
}
