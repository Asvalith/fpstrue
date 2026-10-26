// Copyright Epic Games, Inc. All Rights Reserved.
// 显式开发烟测的同一角色方法；Shipping 不包含脚本输入，不新增运行时组件或状态所有者。
#include "Characters/Player/fpstrueCharacter.h"
#if !UE_BUILD_SHIPPING
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Runtime/fpstrueRuntime.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstrueMultiplayerGameInstance.h"
#include "Game/fpstruePvpPlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "Weapons/fpstrueWeaponActors.h"
#include "Characters/Player/fpstruePlayerAnimInstance.h"
#include "UI/fpstruePvpHUD.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/InputSettings.h"
#include "Components/CapsuleComponent.h"
#include "Animation/AnimMontage.h"
#include "Engine/SkeletalMesh.h"
#include "EngineGlobals.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "UnrealClient.h"
#endif

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
	// 网络恢复先等本次握手/复制就绪；同进程重连的新 Pawn 不重新执行旧射击、击杀和自动装弹脚本。
	if (!HasAuthority() && IsLocallyControlled())
	{
		const auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance());
		if (Instance != nullptr && (Instance->GetConnectionState() != EFPConnectionState::Connected
			|| (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")) && Instance->IsRestoredConnection()))) return;
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
		if (bMovingHitScenario && PS->GetPlayerId() < Other->GetPlayerState()->GetPlayerId())
			if (auto* Player = Cast<AfpstruePvpPlayerController>(Controller)) Player->SetSmokeMovingHitTarget(Other);
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_READY Player=%d"), PS->GetPlayerId());
		// 持续注入真实输入 Action，避免高帧率 NullRHI 下 Timer 的单帧脉冲被移动组件消费后立即制动。
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get(); Subsystem != nullptr && MoveAction != nullptr
			&& (!bMovingHitScenario || PS->GetPlayerId() > Other->GetPlayerState()->GetPlayerId()))
		{
			// 专项隔离射手自身历史原点误差；目标走普通冲刺能力和完整横移轴，不修改 CMC 速度或位置。
			if (bMovingHitScenario)
			{
				Controller->SetControlRotation((Other->GetActorLocation() - GetActorLocation()).Rotation());
				ToggleSprint();
			}
			const float MoveDirection = bMovingHitScenario ? -1.0f : 0.5f;
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
	const bool bMovingHitShooter = bMovingHitScenario && PS->GetPlayerId() < Other->GetPlayerState()->GetPlayerId();
	const bool bMovementWindowFinished = bMovingHitShooter
		? NetworkSmokePhase >= 3 && GetWorld()->GetTimeSeconds() - NetworkSmokePhaseStarted >= 1.0
		: Elapsed >= (bMovingHitScenario ? 5.0 : 1.0);
	if (bMovementWindowFinished && bNetworkSmokeInjectingMove)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
			Subsystem->StopContinuousInputInjectionForAction(MoveAction);
		bNetworkSmokeInjectingMove = false;
		if (bMovingHitScenario && bIsSprinting) ToggleSprint();
		if (bMovingHitShooter)
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_RESUME_MOVE Stopped ElapsedSeconds=%.3f ActualSpeed=%.2f"),
				GetWorld()->GetTimeSeconds() - NetworkSmokePhaseStarted, GetVelocity().Size2D());
	}
	if (PS->GetPlayerId() > Other->GetPlayerState()->GetPlayerId())
	{
		// 显式观察端验收只转动本端相机，不改变另一名玩家输入、能力或服务器命中。
		if ((bMovingHitScenario || FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeObserve"))) && Controller != nullptr)
			Controller->SetControlRotation((Other->GetActorLocation() - GetPawnViewLocation()).Rotation());
		return;
	}
	if (Controller == nullptr) return;
	FVector AimPoint = Other->GetActorLocation();
	FName MovingAimBone;
	uint8 MovingAimShapeType = 255;
	if (bMovingHitScenario && NetworkSmokePhase <= 2)
	{
		const auto* Player = Cast<AfpstruePvpPlayerController>(Controller);
		if (Other != NetworkSmokeTarget.Get() || Player == nullptr
			|| !Player->GetSmokeMovingHitAim(AimPoint, MovingAimBone, MovingAimShapeType)) return;
	}
	const FRotator Aim = NetworkSmokePhase >= 7 ? FRotator(60.0f, 0.0f, 0.0f)
		: (AimPoint - GetPawnViewLocation()).Rotation();
	Controller->SetControlRotation(Aim);
	if (NetworkSmokePhase == 0 && Elapsed >= 2.0)
	{
		if (bMovingHitScenario)
		{
			// 显式专项必须实际横移后才射击；命令已发出或MaxWalkSpeed正确都不能冒充正在移动。
			const FVector Velocity = Other->GetVelocity();
			const float TargetSpeed = Velocity.Size2D();
			const FVector ViewLine = (Other->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
			const float LateralSpeed = FMath::Abs(FVector::CrossProduct(Velocity, ViewLine).Z);
			if (TargetSpeed < Other->SprintSpeed * 0.75f || LateralSpeed < TargetSpeed * 0.85f) return;
			FHitResult Occluder;
			FCollisionQueryParams Sight(SCENE_QUERY_STAT(FpstrueMovingHitSight), true, this);
			if (GetWorld()->LineTraceSingleByChannel(Occluder, GetPawnViewLocation(), AimPoint, ECC_Visibility, Sight)
				&& Occluder.GetActor() != Other) return;
			NetworkSmokeMovingInitialHealth = Other->GetCurrentHealth();
			bNetworkSmokeMovingHitAttempted = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_ATTEMPT Target=%s TargetSpeed=%.2f LateralSpeed=%.2f ShooterSpeed=%.2f Health=%.1f AimBone=%s ShapeType=%u"),
				*Other->GetName(), TargetSpeed, LateralSpeed, GetVelocity().Size2D(), NetworkSmokeMovingInitialHealth,
				*MovingAimBone.ToString(), MovingAimShapeType);
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
			const auto* Player = Cast<AfpstruePvpPlayerController>(Controller);
			if (Other != NetworkSmokeTarget.Get() || Player == nullptr
				|| !Player->LogSmokeMovingHitIfReplicated(Other, NetworkSmokeMovingInitialHealth)) return;
			bNetworkSmokeMovingHitAttempted = false;
			// 首发的静止射手隔离到此结束；随后仍经普通输入完成真实位移，不能删除 full smoke 的移动证明。
			UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get();
			if (Subsystem == nullptr || MoveAction == nullptr)
			{
				UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL Reason=MovingHitResumeMissingMoveInput"));
				GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
				return;
			}
			NetworkSmokePhaseStarted = GetWorld()->GetTimeSeconds();
			Subsystem->StartContinuousInputInjectionForAction(MoveAction, FInputActionValue(FVector2D(0.5f, 0.0f)), {}, {});
			bNetworkSmokeInjectingMove = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_RESUME_MOVE Started Input=MoveAction DurationSeconds=1.0"));
		}
		RequestWeaponReload();
		NetworkSmokePhase = 3;
	}
	else if (NetworkSmokePhase == 3 && !EquippedWeaponComponent->IsReloading()
		&& EquippedWeaponComponent->GetCurrentAmmo() == EquippedWeaponComponent->GetMagazineSize()
		&& EquippedWeaponComponent->GetReserveAmmo() < NetworkSmokeInitialReserve
		&& (!bMovingHitScenario || !bNetworkSmokeInjectingMove))
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

// 正常联网客户端的显式输入驱动验收：空手 -> 行走拾取 -> 双端动作 -> 可选战斗/死亡/重生；不保存资源。
namespace FpstrueNormalPairDriver
{
// 仅显式客户端验收使用；Timer 持有短期状态，所有世界/玩家/武器都是弱引用，世界退出即随 Timer 释放。
struct FWalkState
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<AfpstrueCharacter> Pawn;
	TWeakObjectPtr<AfpstrueTemplateWeapon> Target;
	FTimerHandle Timer;
	FString CaptureDirectory;
	double Deadline = 0.0;
	double EquippedAt = 0.0;
	FVector StartLocation = FVector::ZeroVector;
	bool bLockedPawn = false;
	bool bEmptyCaptureRequested = false;
	bool bWalking = false;
	bool bEquippedCaptureRequested = false;
	bool bPairCaptureRequested = false;
	int32 PairActionPhase = 0;
	double PairActionAt = 0.0;
	double PairHoldingAt = 0.0;
	int32 PairInitialReserve = 0;
	int32 NormalRoutePoint = 0;
	bool bRendezvousReached = false;
	bool bPartnerAimObserved = false;
	bool bForwardKeyHeld = false;
	float PairMovementYaw = 0.0f;
	float PairMovementPeak = 0.0f;
	bool RemoteLocomotionRequested[4] = {};
	bool RemoteLocomotionSaved[4] = {};
	double NextPickupDiagnosticAt = 0.0;
	bool bFinished = false;
	bool bFullGameplayStarted = false;
	bool bCombatShooter = false;
	int32 CombatPhase = 0;
	int32 CombatRouteRevision = 0;
	int32 CombatRoutePoint = 0;
	double CombatPhaseAt = 0.0;
	float CombatEnemyInitialHealth = 0.0f;
	bool bCombatFireHeld = false;
	bool bEnemyHitCaptured = false;
	bool bEnemyDeathCaptured = false;
	bool bEnemyDeathSettledCaptured = false;
	bool bPlayerHitCaptured = false;
	bool bPlayerDeathCaptured = false;
	bool bPlayerDeathSettledCaptured = false;
	double EnemyDeathObservedAt = 0.0;
	double PlayerDeathObservedAt = 0.0;
	bool bRespawnCaptured = false;
	bool bApproachCaptured = false;
	TWeakObjectPtr<AfpstrueEnemyCharacter> CombatEnemy;
	TWeakObjectPtr<AfpstrueCharacter> CombatPartner;
	TWeakObjectPtr<APlayerState> CombatPartnerState;
	bool bFacingReviewStarted = false;
	bool bFacingReviewComplete = false;
	bool bFacingRemoteLeft = false;
	bool bFacingRemoteRight = false;
	bool bFacingPitchUp = false;
	bool bFacingPitchDown = false;
	float FacingReviewEpoch = 0.0f;
	double FacingNextSampleAt = 0.0;
	uint64 FacingLastInputFrame = MAX_uint64;
	double FacingLastInputWorldTime = -1.0;
	int32 FacingCaptureCount = 0;
	FVector FacingStartLocation = FVector::ZeroVector;
	TArray<FString> FacingCaptures;

	// 输入仍经普通 PC RotationInput -> PlayerCameraManager::ProcessViewRotation -> CMC；不直写角色或动画朝向。
	void FacingLookInput(APlayerController* Local, const FRotator& DesiredView)
	{
		// Timer 在低帧率时会同帧补调；输入只提交一次，并按真实世界时间限速，不能把多次旧 ControlRotation 误差累加。
		if (FacingLastInputFrame == GFrameCounter || !Local->GetWorld()) return;
		FacingLastInputFrame = GFrameCounter;
		const double WorldTime = Local->GetWorld()->GetTimeSeconds();
		const double Elapsed = FacingLastInputWorldTime >= 0.0 ? WorldTime - FacingLastInputWorldTime : Local->GetWorld()->GetDeltaSeconds();
		FacingLastInputWorldTime = WorldTime;
		const float InputDeltaSeconds = static_cast<float>(FMath::Clamp(Elapsed, 0.0, 0.25));
		const float MaxYawDelta = 240.0f * InputDeltaSeconds, MaxPitchDelta = 720.0f * InputDeltaSeconds;
		float YawScale = 1.0f, PitchScale = 1.0f;
		if (GetDefault<UInputSettings>()->bEnableLegacyInputScales)
		{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			YawScale = Local->GetDeprecatedInputYawScale(); PitchScale = Local->GetDeprecatedInputPitchScale();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		if (!FMath::IsNearlyZero(YawScale))
			Local->AddYawInput(FMath::Clamp(FMath::FindDeltaAngleDegrees(Local->GetControlRotation().Yaw, DesiredView.Yaw), -MaxYawDelta, MaxYawDelta) / YawScale);
		if (!FMath::IsNearlyZero(PitchScale))
			Local->AddPitchInput(FMath::Clamp(FMath::FindDeltaAngleDegrees(Local->GetControlRotation().Pitch, DesiredView.Pitch), -MaxPitchDelta, MaxPitchDelta) / PitchScale);
	}

	// 记录角色、网络平滑和最终骨骼的真实朝向，区分复制事实与本地动画表现，不写角色姿态。
	void LogFacingPose(AfpstrueCharacter* Character, bool bRemote, const TCHAR* Phase, float ServerTime)
	{
		auto* Mesh = Character->GetMesh();
		auto* Animation = Cast<UfpstruePlayerAnimInstance>(Mesh->GetAnimInstance());
		if (!Animation) return;
		const auto& Facing = Character->GetBodyFacingState();
		const auto* Movement = Character->GetCharacterMovement();
		const auto* Prediction = Movement->GetPredictionData_Client_Character();
		const auto BoneYaw = [Mesh](const TCHAR* Bone) { return Mesh->GetSocketTransform(Bone, RTS_World).Rotator().Yaw; };
		const auto FootDirection = [Mesh](const TCHAR* Foot, const TCHAR* Ball)
		{ return (Mesh->GetSocketLocation(Ball) - Mesh->GetSocketLocation(Foot)).Rotation().Yaw; };
		UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW BODY Remote=%d Phase=%s ServerTime=%.4f Pawn=%s ActorYaw=%.4f BaseAimYaw=%.4f BaseAimPitch=%.4f MeshWorldYaw=%.4f MeshRelativeYaw=%.4f SmoothingMode=%d SmoothingRotationYaw=%.4f SmoothingTranslation=%s StateInitialized=%d Stationary=%d StateFacingYaw=%.4f StateTurnDirection=%d TurnStart=%.4f RootYawOffset=%.4f TurnTime=%.4f TurnAlpha=%.4f TurnRight=%d RootWorldYaw=%.4f PelvisWorldYaw=%.4f SpineWorldYaw=%.4f ThighLeftYaw=%.4f ThighRightYaw=%.4f FootLeftYaw=%.4f FootRightYaw=%.4f FootLeftDirectionYaw=%.4f FootRightDirectionYaw=%.4f FootLeftPosition=%s FootRightPosition=%s Speed=%.4f GroundSpeed=%.4f"),
			bRemote, Phase, ServerTime, *Character->GetName(), Character->GetActorRotation().Yaw, Character->GetBaseAimRotation().Yaw,
			FRotator::NormalizeAxis(Character->GetBaseAimRotation().Pitch), Mesh->GetComponentRotation().Yaw, Mesh->GetRelativeRotation().Yaw,
			static_cast<int32>(Movement->NetworkSmoothingMode), Prediction ? Prediction->MeshRotationOffset.Rotator().Yaw : 0.0,
			Prediction ? *Prediction->MeshTranslationOffset.ToString() : TEXT("None"), Facing.bInitialized, Facing.bStationary,
			Facing.FacingYaw, Facing.TurnDirection, Facing.TurnStartedServerTime, Animation->RootYawOffset, Animation->TurnTime,
			Animation->TurnAlpha, Animation->bTurnRight, BoneYaw(TEXT("root")), BoneYaw(TEXT("pelvis")), BoneYaw(TEXT("spine_03")),
			BoneYaw(TEXT("thigh_l")), BoneYaw(TEXT("thigh_r")), BoneYaw(TEXT("foot_l")), BoneYaw(TEXT("foot_r")),
			FootDirection(TEXT("foot_l"), TEXT("ball_l")), FootDirection(TEXT("foot_r"), TEXT("ball_r")),
			*Mesh->GetSocketLocation(TEXT("foot_l")).ToString(), *Mesh->GetSocketLocation(TEXT("foot_r")).ToString(),
			Character->GetVelocity().Size2D(), Animation->GroundSpeed);
	}

	// 仅 opt-in 的连续双端画面证据；原有拾取、换弹、走跑、停步和 ADS 门禁全部通过后才进入。
	void TickFacingReview(APlayerController* Local, AfpstrueCharacter* Character, AfpstrueCharacter* Other, double Now)
	{
		const float ServerTime = Character->GetBodyFacingServerTime();
		if (!bFacingReviewStarted)
		{
			if (!FParse::Value(FCommandLine::Get(), TEXT("PvpFacingStartServerTime="), FacingReviewEpoch))
				FacingReviewEpoch = (FMath::FloorToFloat(ServerTime / 60.0f) + 1.0f) * 60.0f;
			if (FacingReviewEpoch < ServerTime - 0.2f)
			{ Finish(false, TEXT("FacingReviewSharedEpochAlreadyPassed")); return; }
			bFacingReviewStarted = true; FacingStartLocation = Character->GetActorLocation();
			SetForwardKey(false);
			UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW START Pawn=%s PlayerId=%d Epoch=%.4f CurrentServerTime=%.4f Input=PlayerControllerRotationInput AvatarTeleport=0 ActorRotationWrites=0"),
				*Character->GetName(), Character->GetPlayerState()->GetPlayerId(), FacingReviewEpoch, ServerTime);
		}
		const bool bFirst = Character->GetPlayerState()->GetPlayerId() < Other->GetPlayerState()->GetPlayerId();
		const float OwnStart = FacingReviewEpoch + (bFirst ? 0.0f : 8.0f);
		const float OtherStart = FacingReviewEpoch + (bFirst ? 8.0f : 0.0f);
		const float OwnTime = ServerTime - OwnStart, OtherTime = ServerTime - OtherStart;
		const bool bOwnActive = OwnTime >= 0 && OwnTime < 8.0f;
		const bool bOtherActive = OtherTime >= 0 && OtherTime < 8.0f;
		// 观察伙伴时瞄向身体中心以保留完整头脚画面；仍只转普通镜头，不移动相机或改变 FOV。
		const float LookHeight = bOtherActive && !bOwnActive ? 0.0f : 35.0f;
		const FRotator TowardOther = (Other->GetActorLocation() + FVector(0, 0, LookHeight) - Character->GetPawnViewLocation()).Rotation();
		const auto PhaseAt = [](float Time) -> const TCHAR*
		{
			if (Time < 0) return TEXT("Waiting");
			if (Time < 0.8f) return TEXT("BaselineIdle");
			if (Time < 2.0f) return TEXT("SmallYaw30");
			if (Time < 4.0f) return TEXT("RightYaw90");
			if (Time < 6.3f) return TEXT("LeftYaw90");
			if (Time < 7.0f) return TEXT("PitchUp100Input");
			if (Time < 7.5f) return TEXT("PitchDown100Input");
			return TEXT("ReturnIdle");
		};
		FRotator DesiredView = TowardOther;
		if (bOwnActive)
		{
			const float YawOffset = OwnTime < 0.8f ? 0.0f : OwnTime < 1.3f ? FMath::Lerp(0.0f, 30.0f, (OwnTime - 0.8f) / 0.5f)
				: OwnTime < 2.0f ? 30.0f : OwnTime < 2.6f ? FMath::Lerp(30.0f, 90.0f, (OwnTime - 2.0f) / 0.6f)
				: OwnTime < 4.0f ? 90.0f : OwnTime < 4.8f ? FMath::Lerp(90.0f, -90.0f, (OwnTime - 4.0f) / 0.8f) : -90.0f;
			DesiredView.Yaw += YawOffset; DesiredView.Pitch = OwnTime < 6.3f ? 0.0f : OwnTime < 7.0f ? 100.0f : OwnTime < 7.5f ? -100.0f : 0.0f;
			const float Pitch = FRotator::NormalizeAxis(Local->GetControlRotation().Pitch);
			if (OwnTime >= 6.3f && OwnTime < 7.0f && Pitch >= 74.5f && Pitch <= 75.5f) bFacingPitchUp = true;
			if (OwnTime >= 7.0f && OwnTime < 7.5f && Pitch <= -74.5f && Pitch >= -75.5f) bFacingPitchDown = true;
		}
		FacingLookInput(Local, DesiredView);
		if (FVector::Dist2D(FacingStartLocation, Character->GetActorLocation()) > 3.0 || Character->GetVelocity().Size2D() > 3.0)
		{ Finish(false, TEXT("FacingReviewMustRemainStationary")); return; }
		if (Now >= FacingNextSampleAt && (bOwnActive || bOtherActive))
		{
			FacingNextSampleAt = Now + 0.1;
			LogFacingPose(Character, false, PhaseAt(OwnTime), ServerTime); LogFacingPose(Other, true, PhaseAt(OtherTime), ServerTime);
			FVector ViewLocation; FRotator ViewRotation; Local->GetPlayerViewPoint(ViewLocation, ViewRotation);
			UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW CAMERA Phase=%s ControllerYaw=%.4f ControllerPitch=%.4f CameraYaw=%.4f CameraPitch=%.4f Location=%s"),
				PhaseAt(OwnTime), Local->GetControlRotation().Yaw, FRotator::NormalizeAxis(Local->GetControlRotation().Pitch),
				ViewRotation.Yaw, FRotator::NormalizeAxis(ViewRotation.Pitch), *ViewLocation.ToString());
			if (bOtherActive)
			{
				auto* Animation = Cast<UfpstruePlayerAnimInstance>(Other->GetMesh()->GetAnimInstance());
				if (Animation && Animation->TurnAlpha > 0.1f)
				{ bFacingRemoteRight |= Animation->bTurnRight; bFacingRemoteLeft |= !Animation->bTurnRight; }
				const FString Name = FString::Printf(TEXT("FacingRemote_%04d"), FacingCaptureCount);
				if (RequestCapture(*Name)) { FacingCaptures.Add(Name); ++FacingCaptureCount; }
			}
		}
		if (ServerTime < FacingReviewEpoch + 16.5f) return;
		for (const FString& Name : FacingCaptures)
			if (IFileManager::Get().FileSize(*FPaths::Combine(CaptureDirectory, Name + TEXT(".png"))) <= 0) return;
		if (FacingCaptureCount < 40 || !bFacingRemoteLeft || !bFacingRemoteRight || !bFacingPitchUp || !bFacingPitchDown)
		{ Finish(false, TEXT("FacingReviewMissingContinuousTurnOrPitchEvidence")); return; }
		bFacingReviewComplete = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW PASS Pawn=%s Epoch=%.4f SavedFrames=%d RemoteLeft=%d RemoteRight=%d PitchUp75=%d PitchDown75=%d ActualRenderedGraph=1 VisualQualityReviewed=0"),
			*Character->GetName(), FacingReviewEpoch, FacingCaptureCount, bFacingRemoteLeft, bFacingRemoteRight, bFacingPitchUp, bFacingPitchDown);
	}

	// 新增的走跑段走标准 W 绑定；所有退出路径释放同一按键，不能只给 Timer 一次推力。
	void SetForwardKey(bool bPressed)
	{
		if (bForwardKeyHeld == bPressed) return;
		if (APlayerController* Local = Controller.Get())
			Local->InputKey(FInputKeyParams(EKeys::W, bPressed ? IE_Pressed : IE_Released, bPressed ? 1.0 : 0.0));
		bForwardKeyHeld = bPressed;
	}

	// 观察实际远端蒙皮鞋底与真实地面，区别于 root/ankle 骨点，也不修改碰撞或动画。
	bool CaptureRemoteLocomotion(AfpstrueCharacter* Other, UfpstruePlayerAnimInstance* Animation)
	{
		const TCHAR* Names[] = {TEXT("NormalRemoteWalk"), TEXT("NormalRemoteWalkStopIdle"), TEXT("NormalRemoteSprintMoving"), TEXT("NormalRemoteSprintStopIdle")};
		for (int32 Pending = 0; Pending < 4; ++Pending)
			if (RemoteLocomotionRequested[Pending] && !RemoteLocomotionSaved[Pending]
				&& IFileManager::Get().FileSize(*FPaths::Combine(CaptureDirectory, FString(Names[Pending]) + TEXT(".png"))) > 0)
			{
				RemoteLocomotionSaved[Pending] = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION SAVED Phase=%s Remote=1"), Names[Pending]);
			}
		const double Speed = Other->GetVelocity().Size2D();
		const bool bIdlePose = Speed < 2.0 && Animation->GroundSpeed < 2.0f && Animation->SprintAlpha < 0.02f;
		int32 Index = INDEX_NONE;
		if (!Other->IsSprinting() && Speed > 80.0 && Animation->GroundSpeed > 80.0f) Index = 0;
		else if (RemoteLocomotionSaved[0] && !Other->IsSprinting() && bIdlePose) Index = 1;
		else if (Other->IsSprinting() && Speed > 200.0 && Animation->GroundSpeed > 200.0f) Index = 2;
		else if (RemoteLocomotionSaved[2] && bIdlePose) Index = 3;
		if (Index == INDEX_NONE || RemoteLocomotionSaved[Index]) return true;
		if (!RemoteLocomotionRequested[Index])
		{
			USkeletalMeshComponent* Mesh = Other->GetMesh();
			USkeletalMesh* Body = Mesh->GetSkeletalMeshAsset();
			const FSkeletalMeshRenderData* Data = Body ? Body->GetResourceForRendering() : nullptr;
			FSkinWeightVertexBuffer* Weights = Mesh->GetSkinWeightBuffer(0);
			if (!Data || !Data->LODRenderData.IsValidIndex(0) || !Weights || !Weights->GetDataVertexBuffer()->GetWeightData())
			{ Finish(false, TEXT("RemoteLocomotionCPUSkinUnavailable")); return false; }
			const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
			TArray<FMatrix44f> Matrices; Mesh->GetCurrentRefToLocalMatrices(Matrices, 0);
			FVector Sole = FVector(0, 0, TNumericLimits<double>::Max());
			for (uint32 Vertex = 0; Vertex < LOD.GetNumVertices(); ++Vertex)
			{
				const FVector Position = Mesh->GetComponentTransform().TransformPosition(FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(Mesh, Vertex, LOD, *Weights, Matrices)));
				if (Position.Z < Sole.Z) Sole = Position;
			}
			FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueLocomotionSole), false, Other);
			if (const auto* Weapon = Other->GetEquippedWeaponComponent()) Query.AddIgnoredActor(Weapon->GetOwner());
			FHitResult Floor;
			const bool bFloor = World->LineTraceSingleByChannel(Floor, Sole + FVector(0, 0, 80), Sole - FVector(0, 0, 300), ECC_Visibility, Query);
			const double Clearance = bFloor ? Sole.Z - Floor.ImpactPoint.Z : TNumericLimits<double>::Max();
			const bool bIdle = Index == 1 || Index == 3;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION Phase=%s Remote=1 Pawn=%s ActualSpeed=%.4f SnapshotSpeed=%.4f SprintIntent=%d SprintAlpha=%.5f Sole=%s Floor=%s SoleClearanceCm=%.5f BoneNotSole=0 FloorHit=%d"),
				Names[Index], *Other->GetName(), Speed, Animation->GroundSpeed, Other->IsSprinting(), Animation->SprintAlpha,
				*Sole.ToString(), *GetNameSafe(Floor.GetActor()), Clearance, bFloor);
			// 静止允许胶囊常规离地间隙；移动另容纳脚掌滚动，不把抬起的另一只脚当承重脚。
			if (!bFloor || !FMath::IsFinite(Clearance) || FMath::Abs(Clearance) > (bIdle ? 5.0 : 12.0))
			{ Finish(false, TEXT("RemoteLocomotionSoleNotGrounded")); return false; }
			RemoteLocomotionRequested[Index] = RequestCapture(Names[Index]);
			return !bFinished;
		}
		return true;
	}

	// 实机完整链路只驱动原有本地输入；不写生命、装备、位置或伤害结果。
	void TickFullGameplay(double Now)
	{
		UWorld* CurrentWorld = World.Get();
		APlayerController* Local = Controller.Get();
		AfpstrueCharacter* Original = Pawn.Get();
		if (!CurrentWorld || !Local) { Finish(false, TEXT("FullGameplayControllerLost")); return; }
		auto CaptureOnce = [this](const TCHAR* Name, bool& bRequested)
		{
			if (!bRequested) bRequested = RequestCapture(Name);
			return bRequested && IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, FString(Name) + TEXT(".png")));
		};
		auto SetFire = [this, Original](bool bPressed)
		{
			if (bCombatFireHeld == bPressed) return;
			if (Original && Original->GetAbilitySystemComponent())
			{
				if (bPressed) Original->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
				else Original->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
			}
			bCombatFireHeld = bPressed;
		};
		if (CombatPhase == 0)
		{
			if (!Original || Original->IsDead()) { Finish(false, TEXT("DiedBeforeNormalCombat")); return; }
			auto* Player = Cast<AfpstruePvpPlayerController>(Local);
			if (!Player) { Finish(false, TEXT("FullGameplayControllerType")); return; }
			CombatRouteRevision = Player->GetNormalVisualRouteRevision();
			Player->ServerRequestNormalVisualEnemyRoute();
			CombatPhase = 1; CombatPhaseAt = Now;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY COMBAT_START Pawn=%s Shooter=%d AwaitNaturalRelevancy=1 NoDirectDamage=1"), *Original->GetName(), bCombatShooter);
			return;
		}
		if (CombatPhase == 1)
		{
			if (!Original || Original->IsDead()) { SetFire(false); Finish(false, TEXT("PlayerKilledBeforeEnemy")); return; }
			auto* Player = Cast<AfpstruePvpPlayerController>(Local);
			if (Player->GetNormalVisualRouteRevision() <= CombatRouteRevision) return;
			const TArray<FVector>& Route = Player->GetNormalVisualRoute();
			// 忠实经过导航拐点；提前 70cm 切角会让实际胶囊离开已经验证的路径走廊。
			while (Route.IsValidIndex(CombatRoutePoint) && FVector::DistSquared2D(Original->GetActorLocation(), Route[CombatRoutePoint]) < FMath::Square(20.0)) ++CombatRoutePoint;
			const bool bDiagnostic = Now >= NextPickupDiagnosticAt;
			if (bDiagnostic)
			{
				NextPickupDiagnosticAt = Now + 2.0;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY PROGRESS Pawn=%s Point=%d/%d Location=%s Speed=%.1f Enemy=%s DistanceCm=%.1f"),
					*Original->GetName(), CombatRoutePoint, Route.Num(), *Original->GetActorLocation().ToCompactString(), Original->GetVelocity().Size2D(),
					*GetNameSafe(CombatEnemy.Get()), CombatEnemy.IsValid() ? FVector::Dist2D(Original->GetActorLocation(), CombatEnemy->GetActorLocation()) : -1.0);
			}
			if (!CombatEnemy.IsValid())
				for (TActorIterator<AfpstrueEnemyCharacter> It(CurrentWorld); It; ++It)
					if (!It->IsDead())
					{
						CombatEnemy = *It;
						CombatEnemyInitialHealth = It->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth();
						UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_RELEVANT Enemy=%s Health=%.1f"), *It->GetName(), CombatEnemyInitialHealth);
						break;
					}
			AfpstrueEnemyCharacter* Enemy = CombatEnemy.Get();
			if (!Enemy)
			{
				// 远处对象可能尚无副本；先沿服务器路线正常走近，不扩大网络相关性或制造假 Actor。
				if (Route.IsValidIndex(CombatRoutePoint))
				{
					const FVector Direction = (Route[CombatRoutePoint] - Original->GetActorLocation()).GetSafeNormal2D();
					Local->SetControlRotation(Direction.Rotation()); Original->AddMovementInput(Direction, 1.0f, false);
				}
				return;
			}
			Local->SetControlRotation((Enemy->GetMesh()->GetSocketLocation(TEXT("spine_03")) - Original->GetPawnViewLocation()).Rotation());
			if (Enemy->IsDead())
			{
				SetFire(false);
				if (EnemyDeathObservedAt <= 0.0) EnemyDeathObservedAt = Now;
				if (!CaptureOnce(TEXT("NormalEnemyDeath"), bEnemyDeathCaptured)) return;
				// 同时保留真实死亡首帧与稳定帧，不能用延迟截图藏起错误的死态/站立表现。
				if (Now - EnemyDeathObservedAt < 0.8 || !CaptureOnce(TEXT("NormalEnemyDeathSettled"), bEnemyDeathSettledCaptured)) return;
				CombatPhase = 2; CombatPhaseAt = Now;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_KILLED Pawn=%s Enemy=%s Captured=1"), *Original->GetName(), *Enemy->GetName());
				return;
			}
			if (Enemy->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth() < CombatEnemyInitialHealth)
				CaptureOnce(TEXT("NormalEnemyHit"), bEnemyHitCaptured);
			FCollisionQueryParams Visibility(SCENE_QUERY_STAT(FpstrueNormalCombat), false, Original);
			FHitResult Hit;
			const FVector AimPoint = Enemy->GetMesh()->GetSocketLocation(TEXT("spine_03"));
			const bool bClear = !CurrentWorld->LineTraceSingleByChannel(Hit, Original->GetPawnViewLocation(), AimPoint, ECC_Visibility, Visibility) || Hit.GetActor() == Enemy;
			if (bDiagnostic)
			{
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY SIGHT Pawn=%s Clear=%d Blocker=%s Ammo=%d"), *Original->GetName(), bClear,
					*GetNameSafe(Hit.GetActor()), Original->GetEquippedWeaponComponent() ? Original->GetEquippedWeaponComponent()->GetCurrentAmmo() : -1);
				if (Now - CombatPhaseAt > 5.0) CaptureOnce(TEXT("NormalEnemyApproach"), bApproachCaptured);
			}
			if (FVector::Dist2D(Original->GetActorLocation(), Enemy->GetActorLocation()) > 1200.0 || !bClear)
			{
				SetFire(false);
				if (Route.IsValidIndex(CombatRoutePoint)) Original->AddMovementInput((Route[CombatRoutePoint] - Original->GetActorLocation()).GetSafeNormal2D(), 1.0f, false);
				else if (bClear) Original->AddMovementInput((Enemy->GetActorLocation() - Original->GetActorLocation()).GetSafeNormal2D(), 1.0f, false);
				return;
			}
			// 两个真实玩家都反击敌人；不能让先到的一人只挨打，等待另一条路线上的观察者。
			{
				if (auto* Weapon = Original->GetEquippedWeaponComponent(); Weapon && Weapon->GetCurrentAmmo() == 0 && !Weapon->IsReloading())
				{
					SetFire(false);
					Original->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Reload));
					Original->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Reload));
				}
				else SetFire(true);
			}
			return;
		}
		if (CombatPhase == 2)
		{
			if (!Original) { Finish(false, TEXT("OriginalPawnLostBeforeDeath")); return; }
			AfpstrueCharacter* Victim = bCombatShooter ? Original : CombatPartner.Get();
			if (!Victim) { Finish(false, TEXT("PvpVictimLostBeforeDeath")); return; }
			if (!bCombatShooter) Local->SetControlRotation((Victim->GetMesh()->GetSocketLocation(TEXT("spine_03")) - Original->GetPawnViewLocation()).Rotation());
			if (Victim->GetCurrentHealth() < Victim->GetMaxHealth() && !Victim->IsDead()) CaptureOnce(TEXT("NormalPlayerHit"), bPlayerHitCaptured);
			if (Victim->IsDead())
			{
				SetFire(false);
				if (PlayerDeathObservedAt <= 0.0) PlayerDeathObservedAt = Now;
				if (!CaptureOnce(TEXT("NormalPlayerDeath"), bPlayerDeathCaptured)) return;
				if (Now - PlayerDeathObservedAt < 0.8 || !CaptureOnce(TEXT("NormalPlayerDeathSettled"), bPlayerDeathSettledCaptured)) return;
				CombatPhase = 3; CombatPhaseAt = Now;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY PLAYER_DIED Victim=%s Observer=%s Captured=1"), *Victim->GetName(), *Local->GetName());
				return;
			}
			if (!bCombatShooter && Now - CombatPhaseAt > 2.0) SetFire(true);
			return;
		}
		// 真实服务器重生换 Pawn；死亡计分和空手新生命由原规则负责，旧驱动绝不复活旧对象。
		if (CombatPhase == 3)
		{
			AfpstrueCharacter* NewPawn = nullptr;
			if (bCombatShooter) NewPawn = Cast<AfpstrueCharacter>(Local->GetPawn());
			else for (TActorIterator<AfpstrueCharacter> It(CurrentWorld); It; ++It)
				if (It->GetPlayerState() == CombatPartnerState.Get() && *It != CombatPartner.Get() && !It->IsDead()) { NewPawn = *It; break; }
			if (!NewPawn || NewPawn == (bCombatShooter ? Original : CombatPartner.Get()) || NewPawn->IsDead()) return;
			if (bCombatShooter)
			{
				auto* HUD = Cast<AfpstruePvpHUD>(Local->GetHUD());
				if (!HUD || !HUD->HasDrawnCurrentState(NewPawn) || NewPawn->HasEquippedWeapon()) return;
				if (!CaptureOnce(TEXT("NormalPlayerRespawn"), bRespawnCaptured)) return;
			}
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY COMPLETE NewPawn=%s LocalVictim=%d EmptyHands=%d Health=%.1f"),
				*NewPawn->GetName(), bCombatShooter, !NewPawn->HasEquippedWeapon(), NewPawn->GetCurrentHealth());
			Finish(true, TEXT("RealEnemyCombatPlayerDeathAndNormalRespawn"));
		}
	}

	// 统计当前世界尚存的旧武器副本，验证联机替换是否完整；不主动回收它们。
	int32 CountLegacyPickups() const
	{
		int32 Count = 0;
		if (UWorld* CurrentWorld = World.Get())
			for (TActorIterator<AActor> It(CurrentWorld); It; ++It)
				if (IsValid(*It) && (It->GetClass() == AfpstrueRARWeapon::StaticClass()
					|| It->GetClass()->GetPathName() == TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon.BP_Weapon_C"))) ++Count;
		return Count;
	}

	// 单次结束验收，释放已按住的输入和 Timer，保留实际结果与未通过原因。
	void Finish(bool bPassed, const TCHAR* Reason)
	{
		if (bFinished) return;
		bFinished = true;
		SetForwardKey(false);
		if (PairActionPhase > 0)
			if (AfpstrueCharacter* Character = Pawn.Get())
				if (UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent())
				{
					ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
					ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
				}
		if (UWorld* CurrentWorld = World.Get()) CurrentWorld->GetTimerManager().ClearTimer(Timer);
		UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK %s Reason=%s LegacyRemaining=%d Scope=ClientCharacterMovementAndRealOverlap_NotEnhancedInputBinding_NotVisualQualityAcceptance"),
			bPassed ? TEXT("PASS") : TEXT("FAIL"), Reason, CountLegacyPickups());
	}

	// 仅向空闲截图系统提交不覆盖的请求；返回 true 只是已请求，不等于 PNG 已生成。
	bool RequestCapture(const TCHAR* Name)
	{
		if (FScreenshotRequest::IsScreenshotRequested()) return false;
		const FString Filename = FPaths::Combine(CaptureDirectory, FString(Name) + TEXT(".png"));
		if (IFileManager::Get().FileExists(*Filename)) { Finish(false, TEXT("CaptureAlreadyExists")); return false; }
		FScreenshotRequest::RequestScreenshot(Filename, true, false, false);
		UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK CAPTURE_REQUEST Name=%s File=%s"), Name, *Filename);
		return true;
	}

	// 在限定期限内推进真实客户端拾取/双端表现门禁；Pawn 丢失即失败，不传送或伪造装备状态。
	void Tick()
	{
		if (bFinished) return;
		UWorld* CurrentWorld = World.Get();
		const double Now = FPlatformTime::Seconds();
		if (CurrentWorld == nullptr || CurrentWorld->bIsTearingDown || CurrentWorld->GetNetMode() != NM_Client)
		{ Finish(false, TEXT("ClientWorldEnded")); return; }
		if (Now >= Deadline) { Finish(false, TEXT("AcceptanceDeadline")); return; }
		if (bFullGameplayStarted) { TickFullGameplay(Now); return; }
		if (!bLockedPawn)
		{
			APlayerController* Local = CurrentWorld->GetFirstPlayerController();
			if (Local == nullptr || !Local->IsLocalController()) return;
			auto* Character = Cast<AfpstrueCharacter>(Local->GetPawn());
			if (Character == nullptr) return; // 可由启动 ExecCmds 先等待本地连接真正产生 Pawn。
			if (Character->IsDead() || Character->HasEquippedWeapon()) { Finish(false, TEXT("RequiresLivingEmptyHands")); return; }
			Controller = Local; Pawn = Character; bLockedPawn = true;
			StartLocation = Character->GetActorLocation();
			UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK START Pawn=%s LegacyRemaining=%d Scope=AddMovementInputThroughCharacterMovement_NotEnhancedInputBinding"),
				*Character->GetName(), CountLegacyPickups());
		}
		APlayerController* Local = Controller.Get();
		AfpstrueCharacter* Character = Pawn.Get();
		if (Local == nullptr || Character == nullptr || Character->IsDead() || !Local->IsLocalController()
			|| Local->GetWorld() != CurrentWorld || Character->GetWorld() != CurrentWorld
			|| Local->GetPawn() != Character || Character->GetController() != Local || !Character->IsLocallyControlled())
		{ Finish(false, TEXT("OriginalPawnOrControllerLost")); return; }
		auto* HUD = Cast<AfpstruePvpHUD>(Local->GetHUD());
		UfpstrueWeaponComponent* Equipped = Character->GetEquippedWeaponComponent();
		if (!bWalking)
		{
			if (Equipped != nullptr) { Finish(false, TEXT("EquippedBeforeWalking")); return; }
			if (!Character->GetMesh1P()->bHiddenInGame || HUD == nullptr || !HUD->HasDrawnCurrentState(Character)) return;
			if (!bEmptyCaptureRequested) { bEmptyCaptureRequested = RequestCapture(TEXT("EmptyHands")); return; }
			if (!IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, TEXT("EmptyHands.png")))) return;
			double Nearest = TNumericLimits<double>::Max();
			for (TActorIterator<AfpstrueTemplateWeapon> It(CurrentWorld); It; ++It)
			{
				auto* Weapon = It->FindComponentByClass<UfpstrueWeaponComponent>();
				if (!IsValid(*It) || !It->GetIsReplicated() || It->HasAuthority() || It->GetOwner() != nullptr
					|| Weapon == nullptr || Weapon->GetOwningCharacter() != nullptr) continue;
				const double Distance = FVector::DistSquared2D(Character->GetActorLocation(), It->GetActorLocation());
				if (Distance < Nearest) { Nearest = Distance; Target = *It; }
			}
			if (!Target.IsValid()) return;
			bWalking = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK WALK Target=%s DistanceCm=%.1f LegacyRemaining=%d EmptyHandsHidden=1"),
				*Target->GetName(), FMath::Sqrt(Nearest), CountLegacyPickups());
		}
		AfpstrueTemplateWeapon* Rifle = Target.Get();
		if (Rifle == nullptr) { Finish(false, TEXT("TargetPickupDestroyed")); return; }
		UfpstrueWeaponComponent* ExpectedWeapon = Rifle->FindComponentByClass<UfpstrueWeaponComponent>();
		if (Equipped == nullptr)
		{
			if (Now >= NextPickupDiagnosticAt)
			{
				NextPickupDiagnosticAt = Now + 2.0;
				UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK PROGRESS Pawn=%s Location=%s Pickup=%s Location=%s DistanceCm=%.1f Owner=%s Holder=%s Speed=%.1f"),
					*Character->GetName(), *Character->GetActorLocation().ToCompactString(), *Rifle->GetName(), *Rifle->GetActorLocation().ToCompactString(),
					FVector::Dist2D(Character->GetActorLocation(), Rifle->GetActorLocation()), *GetNameSafe(Rifle->GetOwner()),
					*GetNameSafe(ExpectedWeapon ? ExpectedWeapon->GetOwningCharacter() : nullptr), Character->GetVelocity().Size2D());
			}
			if (ExpectedWeapon == nullptr || (Rifle->GetOwner() != nullptr && Rifle->GetOwner() != Character)
				|| (ExpectedWeapon->GetOwningCharacter() != nullptr && ExpectedWeapon->GetOwningCharacter() != Character))
			{ Finish(false, TEXT("TargetTakenByAnotherPlayer")); return; }
			// 自己的 Owner/Holder 先到时只等待装备引用，不能把合法复制顺序误报成被别人拾走。
			if (Rifle->GetOwner() == Character || ExpectedWeapon->GetOwningCharacter() == Character) return;
			const FVector Direction = (Rifle->GetActorLocation() - Character->GetActorLocation()).GetSafeNormal2D();
			// 不传送、不直接装备、不发送指定枪支 RPC：服务器真实 Overlap 决定拾取结果。
			Character->AddMovementInput(Direction, 1.0f, false);
			return;
		}
		if (Equipped != ExpectedWeapon) { Finish(false, TEXT("UnexpectedEquipment")); return; }
		const bool bPresented = Equipped->GetOwningCharacter() == Character && Rifle->GetOwner() == Character
			&& Equipped->GetAttachParent() == Character->GetMesh1P() && !Equipped->bHiddenInGame
			&& !Character->GetMesh1P()->bHiddenInGame && HUD != nullptr && HUD->HasDrawnCurrentState(Character);
		if (!bPresented) return; // 装备引用、Owner、弹药快照与本地 HUD 允许分批到达。
		if (EquippedAt <= 0.0)
		{
			EquippedAt = Now;
			UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK EQUIPPED Pawn=%s Weapon=%s Ammo=%d Reserve=%d MovedCm=%.1f HUDDrawn=1 ArmsHidden=0 LegacyRemaining=%d"),
				*Character->GetName(), *Rifle->GetName(), Equipped->GetCurrentAmmo(), Equipped->GetReserveAmmo(),
				FVector::Dist2D(StartLocation, Character->GetActorLocation()), CountLegacyPickups());
		}
		if (Now - EquippedAt < 0.5) return;
		if (!bEquippedCaptureRequested) { bEquippedCaptureRequested = RequestCapture(TEXT("PickupEquipped")); return; }
		if (!IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, TEXT("PickupEquipped.png")))) return;
		// 普通玩法双方都走真实拾取后，再验证观察端的常态持枪；不借烟测自动发枪或直接附着造结果。
		AfpstrueCharacter* Other = nullptr;
		for (TActorIterator<AfpstrueCharacter> It(CurrentWorld); It; ++It)
			if (*It != Character && !It->IsDead() && It->GetPlayerState() && It->HasEquippedWeapon()) { Other = *It; break; }
		if (!Other || !Character->GetPlayerState()) return;
		if (bFacingReviewStarted && !bFacingReviewComplete) { TickFacingReview(Local, Character, Other, Now); return; }
		if (Other->IsAiming()) bPartnerAimObserved = true;
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPairWalk")) && !bRendezvousReached)
		{
			const auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Local);
			if (!PvpPlayer || PvpPlayer->GetNormalVisualRoute().IsEmpty()) return;
			const TArray<FVector>& Route = PvpPlayer->GetNormalVisualRoute();
			while (Route.IsValidIndex(NormalRoutePoint) && FVector::DistSquared2D(Character->GetActorLocation(), Route[NormalRoutePoint]) < FMath::Square(60.0)) ++NormalRoutePoint;
			if (Route.IsValidIndex(NormalRoutePoint))
			{
				const FVector Direction = (Route[NormalRoutePoint] - Character->GetActorLocation()).GetSafeNormal2D();
				Local->SetControlRotation(Direction.Rotation());
				Character->AddMovementInput(Direction, 1.0f, false);
				return;
			}
			if (FVector::Dist2D(Character->GetActorLocation(), Other->GetActorLocation()) > 600.0) return;
			bRendezvousReached = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_PAIR RENDEZVOUS Pawn=%s DistanceCm=%.1f RoutePoints=%d Teleport=0"),
				*Character->GetName(), FVector::Dist2D(Character->GetActorLocation(), Other->GetActorLocation()), Route.Num());
		}
		UfpstrueWeaponComponent* OtherWeapon = Other->GetEquippedWeaponComponent();
		USkeletalMeshComponent* OtherBody = Other->GetMesh();
		auto* OtherAnimation = Cast<UfpstruePlayerAnimInstance>(OtherBody->GetAnimInstance());
		if (!OtherAnimation || !OtherAnimation->bArmed || OtherBody->bHiddenInGame || !OtherBody->IsVisible()
			|| !OtherWeapon || OtherWeapon->GetOwningCharacter() != Other || OtherWeapon->GetAttachParent() != OtherBody
			|| OtherWeapon->bHiddenInGame || !OtherWeapon->IsVisible() || OtherWeapon->bOnlyOwnerSee) return;
		if (!bPairCaptureRequested && FVector::Dist2D(Character->GetActorLocation(), Other->GetActorLocation()) < 250.0) return;
		Local->SetControlRotation((Other->GetActorLocation() + FVector(0, 0, 35) - Character->GetPawnViewLocation()).Rotation());
		if (OtherWeapon->GetReserveAmmo() < OtherWeapon->GetWeaponSettings().StartingReserveAmmo && !OtherWeapon->IsReloading()
			&& !CaptureRemoteLocomotion(Other, OtherAnimation)) return;
		if (PairHoldingAt <= 0.0) { PairHoldingAt = Now; return; }
		if (Now - PairHoldingAt < 0.6) return;
		if (!bPairCaptureRequested)
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_PAIR READY Local=%s Remote=%s PlayerId=%d Body=%s Anim=%s Gun=%s Parent=%s Socket=%s Armed=1 GunVisible=1"),
				*Character->GetName(), *Other->GetName(), Other->GetPlayerState()->GetPlayerId(), *GetPathNameSafe(OtherBody->GetSkeletalMeshAsset()),
				*OtherAnimation->GetClass()->GetPathName(), *GetNameSafe(OtherWeapon->GetOwner()), *GetNameSafe(OtherWeapon->GetAttachParent()),
				*OtherWeapon->GetAttachSocketName().ToString());
			bPairCaptureRequested = RequestCapture(TEXT("NormalRemoteHolding"));
			return;
		}
		if (!IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, TEXT("NormalRemoteHolding.png")))) return;
		if (PairActionPhase == 0)
		{
			if (Character->GetPlayerState()->GetPlayerId() > Other->GetPlayerState()->GetPlayerId()
				&& (!bPartnerAimObserved || Other->IsAiming() || Other->IsSprinting() || OtherWeapon->IsReloading())) return;
			// 等双方普通视角稳定及截图完成，不能把启动/转镜头期间错过的瞬时表现当成同步失败。
			if (PairActionAt <= 0.0) { PairActionAt = Now; return; }
			if (Now - PairActionAt < 3.0) return;
			// 先由较小 PlayerId 动作，另一端看到它真实换弹后再交换，双方都必须展示自己的第三人称动作。
			if (Character->GetPlayerState()->GetPlayerId() > Other->GetPlayerState()->GetPlayerId()
				&& OtherWeapon->GetReserveAmmo() >= OtherWeapon->GetWeaponSettings().StartingReserveAmmo) return;
			PairInitialReserve = Equipped->GetReserveAmmo();
			const float AwayYaw = (Other->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw + 70.0f;
			Local->SetControlRotation(FRotator(25.0f, AwayYaw, 0));
			Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
			PairActionPhase = 1; PairActionAt = Now;
			return;
		}
		if (PairActionPhase == 1)
		{
			// 首段保持朝空旷方向射击，不能为了演示动画而由测试直接修改生命或弹药。
			const float AwayYaw = (Other->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw + 70.0f;
			Local->SetControlRotation(FRotator(25.0f, AwayYaw, 0));
			if (Now - PairActionAt < 0.35) return;
			Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
			PairActionPhase = 2; PairActionAt = Now; return;
		}
		if (PairActionPhase == 2)
		{
			if (Now - PairActionAt < 0.35 || Equipped->GetCurrentAmmo() >= Equipped->GetMagazineSize()) return;
			Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Reload));
			Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Reload));
			PairActionPhase = 3; PairActionAt = Now; return;
		}
		if (PairActionPhase == 3)
		{
			if (Equipped->IsReloading() || Equipped->GetCurrentAmmo() != Equipped->GetMagazineSize()
				|| Equipped->GetReserveAmmo() >= PairInitialReserve) return;
			if (!Local->PlayerInput) { Finish(false, TEXT("NormalLocomotionMissingPlayerInput")); return; }
			// 先验证真实胶囊可走的短直线，再走出/跑回，不传送或绕过关卡碰撞。
			FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueNormalMove), false, Character);
			const auto* Capsule = Character->GetCapsuleComponent();
			const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
			const float BaseYaw = (Other->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw + 90.0f;
			bool bClear = false;
			for (int32 Attempt = 0; Attempt < 8; ++Attempt)
			{
				const float Yaw = BaseYaw + Attempt * 45.0f;
				const FVector Direction = FRotator(0, Yaw, 0).Vector();
				bool bBlocked[2] = {};
				for (int32 Side = 0; Side < 2; ++Side)
				{
					FHitResult Hit;
					// 走出约 250cm 后沿原路跑回；起点后方只预留制动余量，不要求整条反向走廊。
					const FVector End = Character->GetActorLocation() + Direction * (Side == 0 ? 450.0f : -100.0f);
					bBlocked[Side] = CurrentWorld->SweepSingleByProfile(Hit, Character->GetActorLocation(), End, FQuat::Identity, Capsule->GetCollisionProfileName(), Shape, Query);
					if (bBlocked[Side])
						UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION CORRIDOR_BLOCK Attempt=%d Side=%d Yaw=%.2f Pawn=%s Start=%s End=%s Radius=%.4f HalfHeight=%.4f Profile=%s HitActor=%s HitComponent=%s HitNormal=%s ImpactNormal=%s StartPenetrating=%d PenetrationDepth=%.4f Distance=%.4f Time=%.5f FloorDist=%.4f CurrentFloor=%s CurrentFloorWalkable=%d"),
							Attempt, Side, Yaw, *Character->GetName(), *Character->GetActorLocation().ToString(), *End.ToString(),
							Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight(), *Capsule->GetCollisionProfileName().ToString(),
							*GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), *Hit.Normal.ToString(), *Hit.ImpactNormal.ToString(), Hit.bStartPenetrating,
							Hit.PenetrationDepth, Hit.Distance, Hit.Time, Character->GetCharacterMovement()->CurrentFloor.FloorDist,
							*GetNameSafe(Character->GetCharacterMovement()->CurrentFloor.HitResult.GetActor()), Character->GetCharacterMovement()->CurrentFloor.IsWalkableFloor());
				}
				if (!bBlocked[0] && !bBlocked[1])
				{ PairMovementYaw = Yaw; bClear = true; break; }
			}
			if (!bClear) { Finish(false, TEXT("NormalLocomotionNoClearCapsuleCorridor")); return; }
			Local->SetControlRotation(FRotator(0, PairMovementYaw, 0));
			PairMovementPeak = 0; SetForwardKey(true);
			PairActionPhase = 7; PairActionAt = Now; return;
		}
		if (PairActionPhase == 7 || PairActionPhase == 9)
		{
			Local->SetControlRotation(FRotator(0, PairMovementYaw + (PairActionPhase == 9 ? 180.0f : 0.0f), 0));
			PairMovementPeak = FMath::Max(PairMovementPeak, static_cast<float>(Character->GetVelocity().Size2D()));
			const double MovementDuration = PairActionPhase == 7 ? 0.9 : 0.5;
			if (Now - PairActionAt < MovementDuration) return;
			SetForwardKey(false);
			if (PairMovementPeak < Character->GetCharacterMovement()->MaxWalkSpeed * 0.95f)
			{ Finish(false, TEXT("NormalLocomotionTrueWDidNotReachSpeed")); return; }
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION INPUT Phase=%s PeakActualSpeed=%.4f ConfiguredMaxSpeed=%.4f Input=StandardW Released=1"),
				PairActionPhase == 7 ? TEXT("Walk") : TEXT("Sprint"), PairMovementPeak, Character->GetCharacterMovement()->MaxWalkSpeed);
			PairActionPhase = PairActionPhase == 7 ? 8 : 10; PairActionAt = Now; return;
		}
		if (PairActionPhase == 8 || PairActionPhase == 10)
		{
			auto* Animation = Cast<UfpstruePlayerAnimInstance>(Character->GetMesh()->GetAnimInstance());
			if (Now - PairActionAt < 1.0 || Character->GetVelocity().Size2D() >= 2.0 || !Animation
				|| Animation->GroundSpeed >= 2.0f || Animation->SprintAlpha >= 0.02f) return;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION STOP Phase=%s ActualSpeed=%.4f SnapshotSpeed=%.4f SprintIntent=%d SprintAlpha=%.5f Idle=1"),
				PairActionPhase == 8 ? TEXT("Walk") : TEXT("Sprint"), Character->GetVelocity().Size2D(), Animation->GroundSpeed, Character->IsSprinting(), Animation->SprintAlpha);
			if (PairActionPhase == 8)
			{
				Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Sprint));
				Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Sprint));
				PairMovementPeak = 0; Local->SetControlRotation(FRotator(0, PairMovementYaw + 180.0f, 0)); SetForwardKey(true);
				PairActionPhase = 9;
			}
			else PairActionPhase = 4;
			PairActionAt = Now; return;
		}
		if (PairActionPhase == 4)
		{
			if (Now - PairActionAt < 0.8) return;
			Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Aim));
			PairActionPhase = 5; PairActionAt = Now; return;
		}
		if (PairActionPhase == 5)
		{
			if (Now - PairActionAt < 0.8) return;
			Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
			PairActionPhase = 6; PairActionAt = Now;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_PAIR ACTIONS_COMPLETE Pawn=%s PlayerId=%d Ammo=%d Reserve=%d Gameplay=ProductionPickupAndOwnedAbilityInputs"),
				*Character->GetName(), Character->GetPlayerState()->GetPlayerId(), Equipped->GetCurrentAmmo(), Equipped->GetReserveAmmo());
			return;
		}
		// 动作完成后仍观察伙伴数秒，让双方都能截到对方的换弹、瞄准和冲刺，不只拍拥有者视模。
		if (PairActionPhase == 6 && (Now - PairActionAt < 8.0 || !RemoteLocomotionSaved[0] || !RemoteLocomotionSaved[1]
			|| !RemoteLocomotionSaved[2] || !RemoteLocomotionSaved[3])) return;
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpFacingReview")) && !bFacingReviewComplete)
		{ TickFacingReview(Local, Character, Other, Now); return; }
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")))
		{
			bFullGameplayStarted = true;
			bCombatShooter = Character->GetPlayerState()->GetPlayerId() < Other->GetPlayerState()->GetPlayerId();
			CombatPartner = Other; CombatPartnerState = Other->GetPlayerState();
			TickFullGameplay(Now);
			return;
		}


		Finish(CountLegacyPickups() == 0, CountLegacyPickups() == 0 ? TEXT("RealPickupAndPaintedHudVerified") : TEXT("LegacyPickupReplicaRemains"));
	}
};

}

// GameState的统一诊断入口按NormalPair模式启动；沿用原5秒准备期，同一个Timer持有状态，世界销毁即停止。
void StartPvpNormalPairDriver(UWorld* World)
{
	static TWeakPtr<FpstrueNormalPairDriver::FWalkState> Active;
	if (World == nullptr || !World->IsGameWorld() || World->GetNetMode() != NM_Client
		|| !FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPair"))
		|| FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")) || !FApp::CanEverRender())
	{ UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_WALK FAIL Reason=RequiresNormalRenderedClient")); return; }
	if (Active.IsValid()) { UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_WALK REJECT Reason=AlreadyRunning")); return; }
	auto State = MakeShared<FpstrueNormalPairDriver::FWalkState>();
	State->World = World; State->Deadline = FPlatformTime::Seconds() + 245.0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), State->CaptureDirectory)
		|| State->CaptureDirectory.IsEmpty() || FPaths::IsRelative(State->CaptureDirectory)
		|| !IFileManager::Get().MakeDirectory(*State->CaptureDirectory, true))
	{ UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_WALK FAIL Reason=RequiresWritableAbsolutePvpSmokeCaptureDir")); return; }
	Active = State;
	World->GetTimerManager().SetTimer(State->Timer, FTimerDelegate::CreateLambda([State]()
	{
		const auto KeepAlive = State;
		KeepAlive->Tick();
	}), 0.03f, true, 5.0f);
}


#endif
