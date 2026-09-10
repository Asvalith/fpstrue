// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueBenchmarkRunner.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyAnimationSharingCoordinator.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueGameMode.h"
#include "fpstrueHealthComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "Navigation/CrowdManager.h"

namespace
{
// 允许胶囊接触造成的厘米级微动，但拒绝窗口抢焦点或残留输入导致的实际移动/转向。
constexpr float BenchmarkPlayerLocationTolerance = 10.0f;
constexpr float BenchmarkPlayerRotationToleranceDegrees = 2.0f;
}

/*
 * 自动性能测试执行器。
 * GameMode 只负责持有本组件；这里按“启动场景 -> 等待生成 -> 预热 -> 采集 -> 保存/退出”推进一次测试，
 * 并把命令行中的消融开关统一应用到敌人，避免测试逻辑散落进正常 Gameplay 流程。
 */

// ==================== 生命周期与测试入口 ====================

UfpstrueBenchmarkRunner::UfpstrueBenchmarkRunner()
{
	// 各阶段完全由一次性 Timer 串联，Runner 不占用每帧组件 Tick。
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueBenchmarkRunner::StartIfRequested(AfpstrueGameMode* InGameMode)
{
	// 只有 -AutoBenchmark 存在时接管开局；普通游玩不会进入任何测试专用流程。
	GameMode = InGameMode;
	if (!FFPBenchmarkConfig::Get().bAutoBenchmark || !GameMode.IsValid())
	{
		return;
	}

	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UfpstrueBenchmarkRunner::BeginBenchmark);
}

void UfpstrueBenchmarkRunner::Cancel()
{
	// 取消所有阶段 Timer；若采集已经开始，还要成对停止 CSV/Trace，避免输出文件损坏。
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	FTimerManager& TimerManager = World->GetTimerManager();
	TimerManager.ClearTimer(ReadyTimerHandle);
	TimerManager.ClearTimer(StartTimerHandle);
	TimerManager.ClearTimer(StopTimerHandle);
	TimerManager.ClearTimer(ExitTimerHandle);

	if (bBenchmarkInputLocked)
	{
		if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
		{
			if (const AfpstrueGameMode* OwnerGameMode = GameMode.Get(); OwnerGameMode != nullptr &&
				IsValid(OwnerGameMode->PlayerCharacter))
			{
				OwnerGameMode->PlayerCharacter->EnableInput(PlayerController);
			}
			// SetIgnore*Input 使用计数器；这里只撤销 Benchmark 自己增加的一层。
			PlayerController->SetIgnoreMoveInput(false);
			PlayerController->SetIgnoreLookInput(false);
		}
		bBenchmarkInputLocked = false;
	}

	StopActiveProfilers();

	// 完整玩法基线不让玩家无敌；若玩家死亡导致对局提前结束，本次样本必须中止并退出，不能伪装成完整采集。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (BenchmarkConfig.bAutoBenchmark && BenchmarkConfig.bAutoQuit && OwnerGameMode != nullptr && OwnerGameMode->bGameEnded)
	{
		if (!bAbortReported)
		{
			UE_LOG(LogTemp, Error, TEXT("Automated benchmark aborted because gameplay ended before the requested capture completed."));
			bAbortReported = true;
		}
		TimerManager.SetTimer(ExitTimerHandle, this, &UfpstrueBenchmarkRunner::ExitBenchmark, 0.1f, false);
	}
}

void UfpstrueBenchmarkRunner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 世界退出时统一执行 Cancel，保证测试回调不会越过组件生命周期。
	Cancel();
	Super::EndPlay(EndPlayReason);
}

// ==================== 场景准备与预热 ====================

void UfpstrueBenchmarkRunner::BeginBenchmark()
{
	// 固定随机种子并启动正常 GameMode；测试器只负责观察和采集，不替换正式玩法状态。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	FMath::RandInit(BenchmarkConfig.Seed);
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark random seed: %d"), BenchmarkConfig.Seed);

	OwnerGameMode->StartGameMode();
	if (!OwnerGameMode->bGameRunning)
	{
		return;
	}

	if (BenchmarkConfig.HasPlayerHealthOverride())
	{
		if (UfpstrueHealthComponent* HealthComponent = OwnerGameMode->PlayerCharacter->GetHealthComponent())
		{
			HealthComponent->SetMaxHealthAndReset(BenchmarkConfig.PlayerHealth);
			UE_LOG(LogTemp, Display, TEXT("Automated benchmark player health override: max=%.1f current=%.1f"),
				   HealthComponent->GetMaxHealth(), HealthComponent->GetHealth());
		}
	}

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTarget(OwnerGameMode->PlayerCharacter);
		// 可见独立窗口会抢占焦点；从输入栈移除玩家 InputComponent，阻止开火、换弹和跳跃等 Action
		// 被人工鼠标/键盘误触发。Enhanced Input 子系统及正式绑定代码保持存在，只有 AutoBenchmark 进程临时隔离输入。
		OwnerGameMode->PlayerCharacter->DisableInput(PlayerController);
		// 额外屏蔽 Controller 的移动/视角入口，避免别的输入组件或控制台命令绕过角色 InputComponent。
		// 这不会关闭移动组件、碰撞或受伤逻辑，AI 仍会围攻同一个正常玩家角色。
		PlayerController->SetIgnoreMoveInput(true);
		PlayerController->SetIgnoreLookInput(true);
		PlayerController->SetControlRotation(OwnerGameMode->PlayerCharacter->GetActorRotation());
		if (UCharacterMovementComponent* Movement = OwnerGameMode->PlayerCharacter->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
		OwnerGameMode->PlayerCharacter->ConsumeMovementInputVector();
		BenchmarkPlayerLocation = OwnerGameMode->PlayerCharacter->GetActorLocation();
		BenchmarkControlRotation = PlayerController->GetControlRotation();
		bBenchmarkInputLocked = true;
		UE_LOG(LogTemp, Display,
			   TEXT("Automated benchmark input locked: location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f)"),
			   BenchmarkPlayerLocation.X, BenchmarkPlayerLocation.Y, BenchmarkPlayerLocation.Z,
			   BenchmarkControlRotation.Pitch, BenchmarkControlRotation.Yaw, BenchmarkControlRotation.Roll);
	}
	// 正式基线必须保留完整玩法：HUD、受伤/死亡、碰撞、AI、动画和渲染消费者都按正常规则运行。
	// 只有命令行显式传入 BenchmarkDisable* 时，后面的诊断阶段才允许关闭单个消费者。

	GetWorld()->GetTimerManager().SetTimer(ReadyTimerHandle, this, &UfpstrueBenchmarkRunner::WaitForBenchmarkReady, 0.25f, true, 0.0f);
}

void UfpstrueBenchmarkRunner::WaitForBenchmarkReady()
{
	// 等待分帧生成队列清空后再开始预热，使稳态数据不混入批量 Spawn 尖峰。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr || !OwnerGameMode->bGameRunning)
	{
		GetWorld()->GetTimerManager().ClearTimer(ReadyTimerHandle);
		return;
	}

	if (OwnerGameMode->PendingEnemySpawnCount > 0)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	GetWorld()->GetTimerManager().ClearTimer(ReadyTimerHandle);
	if (!ValidateBenchmarkState(TEXT("ready")))
	{
		if (BenchmarkConfig.bAutoQuit)
		{
			GetWorld()->GetTimerManager().SetTimer(ExitTimerHandle, this, &UfpstrueBenchmarkRunner::ExitBenchmark, 0.1f, false);
		}
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark ready: requested=%d alive=%d warmup=%.1fs"), BenchmarkConfig.EnemyCount,
		   OwnerGameMode->RegisteredEnemies.Num(), BenchmarkConfig.WarmupSeconds);

	// UE 的零时长 SetTimer 会清除句柄而不是执行回调；零预热必须显式安排到下一帧。
	if (BenchmarkConfig.WarmupSeconds <= KINDA_SMALL_NUMBER)
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UfpstrueBenchmarkRunner::StartCapture);
	}
	else
	{
		GetWorld()->GetTimerManager().SetTimer(StartTimerHandle, this, &UfpstrueBenchmarkRunner::StartCapture,
											   BenchmarkConfig.WarmupSeconds, false);
	}
}

// ==================== 正式采集与消融快照 ====================

void UfpstrueBenchmarkRunner::StartCapture()
{
	// 预热结束后记录消费者快照，再按配置开启 Trace、内存报告、截图和 CSV。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!ValidateBenchmarkState(TEXT("capture-start")))
	{
		if (BenchmarkConfig.bAutoQuit)
		{
			GetWorld()->GetTimerManager().SetTimer(ExitTimerHandle, this, &UfpstrueBenchmarkRunner::ExitBenchmark, 0.1f, false);
		}
		return;
	}
	ApplyDiagnosticOverrides();

	if (!BenchmarkConfig.TraceFile.IsEmpty())
	{
		const FString TraceCommand = FString::Printf(TEXT("Trace.File %s cpu,frame,bookmark,task,stats"), *BenchmarkConfig.TraceFile);
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TraceCommand);
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionBegin AutomatedBenchmarkCapture"));
		bTraceActive = true;
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace started: %s"), *BenchmarkConfig.TraceFile);
	}

	if (BenchmarkConfig.bCollectTextureStats)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("DumpTextureStreamingStats"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("ListStreamingTextures"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("MemReport -full"));
	}

	if (BenchmarkConfig.bTakeScreenshot)
	{
		// Shot 用于证明场景和敌人数正确，不作为逐像素 A/B。
		// 冷缓存时它可能触发 Shader/资源收口，因此解释 Trace 时要排除采集开头的异常帧。
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Shot"));
	}

	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile start"));
	bCaptureActive = true;
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture started: requested=%d alive=%d duration=%.1fs"), BenchmarkConfig.EnemyCount,
		   OwnerGameMode->RegisteredEnemies.Num(), BenchmarkConfig.DurationSeconds);

	GetWorld()->GetTimerManager().SetTimer(StopTimerHandle, this, &UfpstrueBenchmarkRunner::StopCapture, BenchmarkConfig.DurationSeconds,
										   false);
}

// 将同一组命令行开关下发给所有存活敌人，并记录消费者数量，证明消融确实生效。
void UfpstrueBenchmarkRunner::ApplyDiagnosticOverrides()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	int32 AppliedEnemyCount = 0;
	int32 FullRateMovementCount = 0;
	int32 MidRateMovementCount = 0;
	int32 FarRateMovementCount = 0;
	int32 AttackingEnemyCount = 0;
	int32 ShadowCastingEnemyCount = 0;
	int32 RayTracingVisibleEnemyCount = 0;
	int32 MovementTickEnabledCount = 0;
	int32 SkeletalMeshTickEnabledCount = 0;
	int32 RenderFullCount = 0;
	int32 RenderReducedCount = 0;
	int32 RenderBackgroundCount = 0;
	int32 LOD0Count = 0;
	int32 LOD1Count = 0;
	int32 LOD2PlusCount = 0;
	int32 RVOEnabledCount = 0;
	int32 CrowdFollowingCount = 0;
	int32 ValidCrowdAgentCount = 0;
	const UCrowdManager* CrowdManager = UCrowdManager::GetCurrent(GetWorld());

	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr)
		{
			continue;
		}

		Enemy->ApplyBenchmarkDiagnosticOverrides(BenchmarkConfig.bDisableAttackSweep, BenchmarkConfig.bDisableEnemyPawnCollision,
												 BenchmarkConfig.bDisableCharacterMovementTick);

		if (AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
		{
			EnemyController->ApplyBenchmarkPathFollowingTickOverride(BenchmarkConfig.bDisablePathFollowingTick);
			if (const UCrowdFollowingComponent* CrowdFollowing = Cast<UCrowdFollowingComponent>(EnemyController->GetPathFollowingComponent()))
			{
				++CrowdFollowingCount;
				// 组件存在不代表 Detour 已分配代理槽位；容量不足时不能把“少算了敌人”误判为优化。
				ValidCrowdAgentCount += CrowdManager != nullptr && CrowdManager->IsAgentValid(CrowdFollowing) &&
					CrowdFollowing->IsCrowdSimulationActive() ? 1 : 0;
			}
		}

		if (const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			RVOEnabledCount += Movement->bUseRVOAvoidance ? 1 : 0;
			MovementTickEnabledCount += Movement->IsComponentTickEnabled() ? 1 : 0;
			const float TickInterval = Movement->GetComponentTickInterval();
			if (TickInterval <= KINDA_SMALL_NUMBER)
			{
				++FullRateMovementCount;
			}
			else if (Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Reduced)
			{
				++MidRateMovementCount;
			}
			else
			{
				++FarRateMovementCount;
			}
		}

		AttackingEnemyCount += Enemy->IsAttacking() ? 1 : 0;
		switch (Enemy->GetRenderSignificanceTier())
		{
		case EFPEnemyRenderSignificanceTier::Full:
			++RenderFullCount;
			break;
		case EFPEnemyRenderSignificanceTier::Reduced:
			++RenderReducedCount;
			break;
		case EFPEnemyRenderSignificanceTier::Background:
		default:
			++RenderBackgroundCount;
			break;
		}

		const int32 AppliedMinLOD = Enemy->GetAppliedMinimumLOD();
		if (AppliedMinLOD <= 0)
		{
			++LOD0Count;
		}
		else if (AppliedMinLOD == 1)
		{
			++LOD1Count;
		}
		else
		{
			++LOD2PlusCount;
		}

		if (USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh())
		{
			if (BenchmarkConfig.bDisableSkeletalMeshTick)
			{
				CharacterMesh->SetComponentTickEnabled(false);
			}
			SkeletalMeshTickEnabledCount += CharacterMesh->IsComponentTickEnabled() ? 1 : 0;
			ShadowCastingEnemyCount += CharacterMesh->CastShadow ? 1 : 0;
			RayTracingVisibleEnemyCount += CharacterMesh->bVisibleInRayTracing ? 1 : 0;
		}
		++AppliedEnemyCount;
	}

	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark diagnostics applied: enemies=%d attackSweepOff=%d pawnCollisionOff=%d pathFollowingTickOff=%d "
				"characterMovementTickOff=%d skeletalMeshTickOff=%d significanceOff=%d"),
		   AppliedEnemyCount, BenchmarkConfig.bDisableAttackSweep ? 1 : 0, BenchmarkConfig.bDisableEnemyPawnCollision ? 1 : 0,
		   BenchmarkConfig.bDisablePathFollowingTick ? 1 : 0, BenchmarkConfig.bDisableCharacterMovementTick ? 1 : 0,
		   BenchmarkConfig.bDisableSkeletalMeshTick ? 1 : 0, BenchmarkConfig.bDisableEnemySignificance ? 1 : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark enemy snapshot: movementFull=%d movementMid=%d movementFar=%d movementTickEnabled=%d skeletalMeshTickEnabled=%d "
				"attacking=%d castingShadow=%d rayTracingVisible=%d animationSharingFollowers=%d"),
		   FullRateMovementCount, MidRateMovementCount, FarRateMovementCount, MovementTickEnabledCount, SkeletalMeshTickEnabledCount,
		   AttackingEnemyCount, ShadowCastingEnemyCount, RayTracingVisibleEnemyCount,
		   OwnerGameMode->EnemyAnimationSharingCoordinator != nullptr
			   ? OwnerGameMode->EnemyAnimationSharingCoordinator->GetRegisteredEnemyCount()
			   : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark avoidance snapshot: enemies=%d rvoEnabled=%d crowdFollowing=%d crowdValid=%d"),
		   AppliedEnemyCount, RVOEnabledCount, CrowdFollowingCount, ValidCrowdAgentCount);
	const bool bConsistentRVO = AppliedEnemyCount > 0 && RVOEnabledCount == AppliedEnemyCount &&
		CrowdFollowingCount == 0 && ValidCrowdAgentCount == 0;
	const bool bConsistentDetourCrowd = AppliedEnemyCount > 0 && RVOEnabledCount == 0 &&
		CrowdFollowingCount == AppliedEnemyCount && ValidCrowdAgentCount == AppliedEnemyCount;
	if (!bConsistentRVO && !bConsistentDetourCrowd)
	{
		// 只在开采前检查一次；混用两套避让或 Crowd 代理不足的样本不能进入候选对照。
		UE_LOG(LogTemp, Warning, TEXT("Benchmark avoidance configuration mismatch: expected one complete RVO or Detour Crowd configuration."));
	}
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark render significance snapshot: renderFull=%d renderReduced=%d renderBackground=%d lod0=%d lod1=%d lod2Plus=%d "
				"fullBudget=%d shadowBudget=%d rayTracingBudget=%d"),
		   RenderFullCount, RenderReducedCount, RenderBackgroundCount, LOD0Count, LOD1Count, LOD2PlusCount,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies);
}

bool UfpstrueBenchmarkRunner::ValidateBenchmarkState(const TCHAR* Phase) const
{
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	const UfpstrueHealthComponent* HealthComponent =
		OwnerGameMode != nullptr && IsValid(OwnerGameMode->PlayerCharacter)
			? OwnerGameMode->PlayerCharacter->GetHealthComponent()
			: nullptr;
	const int32 AliveEnemyCount = OwnerGameMode != nullptr ? OwnerGameMode->RegisteredEnemies.Num() : 0;
	const int32 RequestedEnemyCount = BenchmarkConfig.HasEnemyCountOverride() ? BenchmarkConfig.EnemyCount : AliveEnemyCount;
	const float PlayerHealth = HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
	const bool bEnemyCountValid = !BenchmarkConfig.HasEnemyCountOverride() || AliveEnemyCount == RequestedEnemyCount;
	const APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	const FVector CurrentPlayerLocation = OwnerGameMode != nullptr && IsValid(OwnerGameMode->PlayerCharacter)
		? OwnerGameMode->PlayerCharacter->GetActorLocation()
		: FVector::ZeroVector;
	const FRotator CurrentControlRotation = PlayerController != nullptr ? PlayerController->GetControlRotation() : FRotator::ZeroRotator;
	const float LocationDrift = bBenchmarkInputLocked
		? FVector::Distance(CurrentPlayerLocation, BenchmarkPlayerLocation)
		: 0.0f;
	const float PitchDrift = bBenchmarkInputLocked
		? FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentControlRotation.Pitch, BenchmarkControlRotation.Pitch))
		: 0.0f;
	const float YawDrift = bBenchmarkInputLocked
		? FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentControlRotation.Yaw, BenchmarkControlRotation.Yaw))
		: 0.0f;
	const bool bViewTransformValid = !bBenchmarkInputLocked ||
		(LocationDrift <= BenchmarkPlayerLocationTolerance && PitchDrift <= BenchmarkPlayerRotationToleranceDegrees &&
		 YawDrift <= BenchmarkPlayerRotationToleranceDegrees);
	const bool bStateValid = OwnerGameMode != nullptr && OwnerGameMode->bGameRunning && !OwnerGameMode->bGameEnded &&
							 HealthComponent != nullptr && !HealthComponent->IsDead() && bEnemyCountValid && bViewTransformValid;

	if (bStateValid)
	{
		UE_LOG(LogTemp, Display,
			   TEXT("Automated benchmark validation: phase=%s requested=%d alive=%d playerHealth=%.1f locationDrift=%.2f pitchDrift=%.2f yawDrift=%.2f"),
			   Phase, RequestedEnemyCount, AliveEnemyCount, PlayerHealth, LocationDrift, PitchDrift, YawDrift);
	}
	else
	{
		UE_LOG(LogTemp, Error,
			   TEXT("Automated benchmark invalid: phase=%s requested=%d alive=%d playerHealth=%.1f running=%d ended=%d locationDrift=%.2f pitchDrift=%.2f yawDrift=%.2f"),
			   Phase, RequestedEnemyCount, AliveEnemyCount, PlayerHealth,
			   OwnerGameMode != nullptr && OwnerGameMode->bGameRunning ? 1 : 0,
			   OwnerGameMode != nullptr && OwnerGameMode->bGameEnded ? 1 : 0,
			   LocationDrift, PitchDrift, YawDrift);
	}

	return bStateValid;
}

// ==================== 采集结束与进程退出 ====================

void UfpstrueBenchmarkRunner::StopCapture()
{
	// 采集到期后成对停止 CSV 和 Trace；AutoQuit 延迟一秒退出，给文件写盘留出时间。
	const bool bStateValid = ValidateBenchmarkState(TEXT("capture-end"));
	const bool bWasTraceActive = bTraceActive;
	StopActiveProfilers();
	if (bWasTraceActive)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace stopped."));
	}
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture stopped."));
	if (bStateValid)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark completed successfully."));
	}

	if (FFPBenchmarkConfig::Get().bAutoQuit)
	{
		GetWorld()->GetTimerManager().SetTimer(ExitTimerHandle, this, &UfpstrueBenchmarkRunner::ExitBenchmark, 1.0f, false);
	}
}

void UfpstrueBenchmarkRunner::StopActiveProfilers()
{
	// 两个入口保持相同的命令顺序；状态复位后再次取消不会重复停止或关闭 Trace 区间。
	if (bCaptureActive)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile stop"));
		bCaptureActive = false;
	}
	if (bTraceActive)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionEnd AutomatedBenchmarkCapture"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.Stop"));
		bTraceActive = false;
	}
}

void UfpstrueBenchmarkRunner::ExitBenchmark()
{
	// 通过 UE 正常退出入口关闭进程，确保日志和分析文件完成收尾。
	UKismetSystemLibrary::QuitGame(this, UGameplayStatics::GetPlayerController(this, 0), EQuitPreference::Quit, false);
}
