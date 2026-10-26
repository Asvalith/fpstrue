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

#endif
