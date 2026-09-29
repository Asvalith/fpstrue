// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"
#include "Testing/Benchmarks/fpstrueBenchmarkConfig.h"
#include "Async/Future.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/Performance/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Game/fpstrueGameMode.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "Navigation/CrowdManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

// CSV 的 BeginCapture 只排队，尚未开始时 EndCapture 无法撤销它。
// 业务超时不能撤销底层排队命令：租约保留到实际首帧和写盘结束，不能按秒数自行释放。
struct FFPBenchmarkCsvCapture
{
	enum class EState : uint8 { StartRequested, Capturing, CancelPendingStart, StopRequested, Released };
	FString Filename;
	FDelegateHandle FirstFrameHandle;
	EState State = EState::StartRequested;
	TSharedFuture<FString> WriteComplete;
};

// 固定含义的计数快照，不持有场景对象；验证和日志不能各自计算出不同的消费者集合。
struct UfpstrueBenchmarkRunner::FEnemySnapshot
{
	int32 Enemies = 0;
	int32 RVO = 0;
	int32 Crowd = 0;
	int32 ValidCrowd = 0;
	bool bRegistryValid = true;
	bool bDiagnosticStateValid = true;
	// 以下仅在正式采集前统计。各档按 Full / Reduced / Background，LOD 按 0 / 1 / 2+。
	int32 Movement[3] = {};
	int32 Render[3] = {};
	int32 MinLOD[3] = {};
	int32 MovementTickEnabled = 0;
	int32 SkeletalMeshTickEnabled = 0;
	int32 Attacking = 0;
	int32 ShadowCasting = 0;
	int32 RayTracingVisible = 0;
};

namespace
{
// 允许胶囊接触造成的厘米级微动，但拒绝窗口抢焦点或残留输入导致的实际移动/转向。
constexpr float BenchmarkPlayerLocationTolerance = 10.0f;
constexpr float BenchmarkPlayerRotationToleranceDegrees = 2.0f;
constexpr double ProfilerTimeoutSeconds = 30.0;
// 此句柄不依赖 Runner/World 寿命；尚有迟到启动可能时，整个进程的 Benchmark 入口保持隔离。
TSharedPtr<FFPBenchmarkCsvCapture> OutstandingCsvCapture;
bool bProcessBenchmarkConsumed = false;

const TCHAR* ResultName(EFPBenchmarkResult Result)
{
	switch (Result)
	{
	case EFPBenchmarkResult::Completed: return TEXT("Completed");
	case EFPBenchmarkResult::InvalidSample: return TEXT("InvalidSample");
	case EFPBenchmarkResult::Failed: return TEXT("Failed");
	case EFPBenchmarkResult::Cancelled: return TEXT("Cancelled");
	case EFPBenchmarkResult::Unsupported: return TEXT("Unsupported");
	default: return TEXT("None");
	}
}

// 三种 Tick 消融共用保存/关闭规则；未请求的组件既不修改，也不占用恢复状态。
void DisableTickForCapture(UActorComponent* Component, bool bDisable, bool& bWasEnabled)
{
	if (Component != nullptr && bDisable)
	{
		bWasEnabled = Component->IsComponentTickEnabled();
		Component->SetComponentTickEnabled(false);
	}
}
}

/*
 * 自动性能测试执行器。
 * GameMode 只负责持有本组件；这里按“启动场景 -> 等待生成 -> 预热 -> 采集 -> 保存/退出”推进一次测试，
 * 并把命令行中的消融开关统一应用到敌人，避免测试逻辑散落进正常 Gameplay 流程。
 */

// ==================== 生命周期与测试入口 ====================

UfpstrueBenchmarkRunner::UfpstrueBenchmarkRunner()
{
	// 准备/预热使用世界 Timer；采集和写盘由独立 Watchdog 推进，不启用组件 Tick。
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueBenchmarkRunner::StartIfRequested(AfpstrueGameMode* InGameMode)
{
	// 只有 -AutoBenchmark 存在时接管开局；普通游玩不会进入任何测试专用流程。
	if (!FFPBenchmarkConfig::Get().bAutoBenchmark || !IsValid(InGameMode) || GetWorld() == nullptr || bEndingPlay)
	{
		return;
	}
	// 同一次运行重复挂接是空操作。Finished 也不重开：场景、血量与随机流都未完整重建。
	if (!CanStartFreshRun())
	{
		UE_LOG(LogTemp, Warning, TEXT("Automated benchmark rejected: one run per process; use a fresh game process (outstandingCsv=%d)."),
			HasOutstandingCsvLease());
		return;
	}
	bProcessBenchmarkConsumed = true;
	GameMode = InGameMode;
	++RunId;
	Phase = EFPBenchmarkPhase::Queued;
	PhaseDeadline = FPlatformTime::Seconds() + ProfilerTimeoutSeconds;
	StartWatchdog();
	ScheduleStage(Phase, &UfpstrueBenchmarkRunner::BeginBenchmark, 0.0f);
}

bool UfpstrueBenchmarkRunner::CanStartFreshRun() const
{
	return Phase == EFPBenchmarkPhase::Idle && !bProcessBenchmarkConsumed && !HasOutstandingCsvLease();
}

bool UfpstrueBenchmarkRunner::HasOutstandingCsvLease()
{
	return OutstandingCsvCapture.IsValid() && OutstandingCsvCapture->State != FFPBenchmarkCsvCapture::EState::Released;
}

void UfpstrueBenchmarkRunner::StartWatchdog()
{
	if (WatchdogHandle.IsValid()) return;
	WatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this,
		[this](float) { return TickWatchdog(); }), 0.25f);
}

bool UfpstrueBenchmarkRunner::TickWatchdog()
{
	if (bEndingPlay || Phase == EFPBenchmarkPhase::Idle || Phase == EFPBenchmarkPhase::Finished)
	{
		WatchdogHandle.Reset();
		return false;
	}
	// 世界 Timer 只推进准备/预热；Profiler 启动、采集、写盘统一由本处驱动，不再双重轮询。
	// CoreTicker 不随世界暂停/时间缩放停止，采集门禁沿用 0.25 秒检查间隔。
	if (Phase == EFPBenchmarkPhase::Flushing || Phase == EFPBenchmarkPhase::Quarantined)
	{
		PollArtifacts();
	}
	else if (const UWorld* World = GetWorld(); World == nullptr || World->IsPaused() ||
		!FMath::IsNearlyEqual(World->GetWorldSettings()->GetEffectiveTimeDilation(), 1.0f))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("world paused, unavailable or time-dilated"));
	}
	else if (Phase == EFPBenchmarkPhase::StartingCapture)
	{
		PollCaptureStart();
	}
	else if (Phase == EFPBenchmarkPhase::Capturing)
	{
		PollCapture();
	}
	else if (FPlatformTime::Seconds() >= PhaseDeadline)
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("world stage failed to progress before watchdog deadline"));
	}
	return !bEndingPlay && Phase != EFPBenchmarkPhase::Finished;
}

void UfpstrueBenchmarkRunner::ScheduleStage(EFPBenchmarkPhase ExpectedPhase, FStageCallback Callback, float Delay, bool bLoop)
{
	if (UWorld* World = GetWorld(); World != nullptr && !bEndingPlay)
	{
		FTimerManager& Timers = World->GetTimerManager();
		Timers.ClearTimer(StageTimerHandle);
		const uint64 ScheduledRun = RunId;
		FTimerDelegate Delegate = FTimerDelegate::CreateWeakLambda(this, [this, ScheduledRun, ExpectedPhase, Callback]()
		{
			if (RunId == ScheduledRun && Phase == ExpectedPhase && !bEndingPlay)
			{
				(this->*Callback)();
			}
		});
		if (Delay <= KINDA_SMALL_NUMBER)
		{
			StageTimerHandle = Timers.SetTimerForNextTick(Delegate);
		}
		else
		{
			FTimerManagerTimerParameters Parameters;
			Parameters.bLoop = bLoop;
			Parameters.bMaxOncePerFrame = true;
			Timers.SetTimer(StageTimerHandle, Delegate, Delay, Parameters);
		}
	}
}

void UfpstrueBenchmarkRunner::ClearStageTimers()
{
	++RunId;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StageTimerHandle);
	}
}

void UfpstrueBenchmarkRunner::ReleaseSceneOwnership()
{
	if (AfpstrueGameMode* OwnerGameMode = GameMode.Get())
	{
		OwnerGameMode->OnAliveEnemyCountChanged.RemoveDynamic(this, &UfpstrueBenchmarkRunner::HandleEnemyCountChanged);
		if (bOwnsDurationOverride)
		{
			OwnerGameMode->BenchmarkGameDurationOverride = PreviousDurationOverride;
			OwnerGameMode->BenchmarkEnemyCountOverride = PreviousEnemyCountOverride;
		}
	}
	bOwnsDurationOverride = false;
	// 只回收本次运行取得的输入层与原始视点，不修改后来接管玩家的 Controller。
	if (bBenchmarkInputLocked)
	{
		if (APlayerController* PlayerController = LockedController.Get())
		{
			if (UInputComponent* Input = RemovedInputComponent.Get(); Input != nullptr &&
				LockedPlayer.IsValid() && PlayerController->GetPawn() == LockedPlayer.Get())
			{
				PlayerController->PushInputComponent(Input);
			}
			// SetIgnore*Input 使用计数器；这里只撤销 Benchmark 自己增加的一层。
			PlayerController->SetIgnoreMoveInput(false);
			PlayerController->SetIgnoreLookInput(false);
			if (PlayerController->GetViewTarget() == LockedPlayer.Get())
			{
				if (PreviousViewTarget.IsValid())
				{
					PlayerController->SetViewTarget(PreviousViewTarget.Get());
				}
				PlayerController->SetControlRotation(PreviousControlRotation);
			}
		}
		bBenchmarkInputLocked = false;
	}
	LockedController.Reset();
	LockedPlayer.Reset();
	RemovedInputComponent.Reset();
	PreviousViewTarget.Reset();

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	for (const FDiagnosticState& State : DiagnosticStates)
	{
		AfpstrueEnemyCharacter* Enemy = State.Enemy.Get();
		if (Enemy == nullptr || Enemy->IsDead())
		{
			continue;
		}
		if (BenchmarkConfig.bDisableAttackSweep)
		{
			if (UfpstrueEnemyCombatComponent* Combat = Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>())
			{
				Combat->SetAttackSweepDisabledForBenchmark(false);
			}
		}
		if (BenchmarkConfig.bDisableEnemyPawnCollision)
		{
			Enemy->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, static_cast<ECollisionResponse>(State.PawnResponse));
		}
		if (BenchmarkConfig.bDisableCharacterMovementTick && Enemy->GetCharacterMovement())
		{
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(State.bMovementTick);
		}
		if (BenchmarkConfig.bDisableSkeletalMeshTick && Enemy->GetMesh())
		{
			Enemy->GetMesh()->SetComponentTickEnabled(State.bMeshTick);
		}
		if (BenchmarkConfig.bDisablePathFollowingTick && State.PathFollowing.IsValid())
		{
			State.PathFollowing->SetComponentTickEnabled(State.bPathFollowingTick);
		}
	}
	DiagnosticStates.Reset();
}

void UfpstrueBenchmarkRunner::Cancel()
{
	if (Phase == EFPBenchmarkPhase::Idle || Phase == EFPBenchmarkPhase::Finished ||
		Phase == EFPBenchmarkPhase::Flushing || Phase == EFPBenchmarkPhase::Quarantined)
	{
		return;
	}
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	const bool bGameplayEnded = OwnerGameMode != nullptr && OwnerGameMode->IsFinished();
	Finish(bGameplayEnded ? EFPBenchmarkResult::InvalidSample : EFPBenchmarkResult::Cancelled,
		bGameplayEnded ? TEXT("gameplay ended before the requested capture completed") : TEXT("cancel requested"));
}

void UfpstrueBenchmarkRunner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 不安排新的世界 Timer；排队中的 CSV 首帧租约仍负责回收尚未真正启动的采集。
	bEndingPlay = true;
	ClearStageTimers();
	FTSTicker::GetCoreTicker().RemoveTicker(WatchdogHandle);
	WatchdogHandle.Reset();
	if (Phase != EFPBenchmarkPhase::Idle && Phase != EFPBenchmarkPhase::Finished)
	{
		// 已拒收/失败的原因不能被销毁覆盖；只有未决或尚未验盘的成功才转为取消。
		if (Result == EFPBenchmarkResult::None || Result == EFPBenchmarkResult::Completed)
		{
			Result = EFPBenchmarkResult::Cancelled;
		}
		UE_LOG(LogTemp, Warning, TEXT("Automated benchmark result: %s (world ended before artifact verification)."), ResultName(Result));
	}
	StopActiveProfilers();
	// 世界 Timer 已不可依赖；迟到启动仍由进程级租约处理，不以业务超时移除清理回调。
	if (bTraceActive && !bTraceStopRequested)
	{
		const FString OwnedTrace = TraceOutputPath;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([OwnedTrace](float)
		{
			return FTraceAuxiliary::GetTraceDestinationString() == OwnedTrace && !FTraceAuxiliary::Stop();
		}), 0.1f);
	}
	ReleaseSceneOwnership();
	Phase = EFPBenchmarkPhase::Finished;
	Super::EndPlay(EndPlayReason);
}

// ==================== 场景准备与预热 ====================

void UfpstrueBenchmarkRunner::BeginBenchmark()
{
	Phase = EFPBenchmarkPhase::Preparing;
	// 固定随机种子并启动正常 GameMode；测试器只负责观察和采集，不替换正式玩法状态。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("GameMode is unavailable"));
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	FMath::RandInit(BenchmarkConfig.Seed);
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark random seed: %d"), BenchmarkConfig.Seed);

	// 正常对局时限不应截断长驻留实验。覆盖只存在于独立 AutoBenchmark 进程，
	// 额外 60 秒覆盖分帧生成、ready 轮询和退出收尾，不改变正式配置资产。
	PreviousDurationOverride = OwnerGameMode->BenchmarkGameDurationOverride;
	PreviousEnemyCountOverride = OwnerGameMode->BenchmarkEnemyCountOverride;
	bOwnsDurationOverride = true;
	OwnerGameMode->BenchmarkEnemyCountOverride = BenchmarkConfig.HasEnemyCountOverride() ? BenchmarkConfig.EnemyCount : INDEX_NONE;
	OwnerGameMode->BenchmarkGameDurationOverride =
		FMath::CeilToInt(BenchmarkConfig.WarmupSeconds + BenchmarkConfig.DurationSeconds) + 60;
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark game duration override: %d seconds"),
		   OwnerGameMode->BenchmarkGameDurationOverride);

	OwnerGameMode->StartGameMode();
	// StartGameMode 的蓝图/委托可能同步终止对局，此时 Cancel 已经改变阶段。
	if (Phase != EFPBenchmarkPhase::Preparing)
	{
		return;
	}
	if (!OwnerGameMode->IsRunning() || !IsValid(OwnerGameMode->PlayerCharacter))
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("gameplay failed to start with a valid player"));
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
	if (Phase != EFPBenchmarkPhase::Preparing)
	{
		return;
	}
	if (!OwnerGameMode->IsRunning() || !IsValid(OwnerGameMode->PlayerCharacter))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("player changed during benchmark setup"));
		return;
	}

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		LockedController = PlayerController;
		LockedPlayer = OwnerGameMode->PlayerCharacter;
		PreviousViewTarget = PlayerController->GetViewTarget();
		PreviousControlRotation = PlayerController->GetControlRotation();
		PlayerController->SetViewTarget(OwnerGameMode->PlayerCharacter);
		// 可见独立窗口会抢占焦点；从输入栈移除玩家 InputComponent，阻止开火、换弹和跳跃等 Action
		// 被人工鼠标/键盘误触发。Enhanced Input 子系统及正式绑定代码保持存在，只有 AutoBenchmark 进程临时隔离输入。
		if (UInputComponent* Input = OwnerGameMode->PlayerCharacter->InputComponent;
			Input != nullptr && PlayerController->PopInputComponent(Input))
		{
			RemovedInputComponent = Input;
		}
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
	else
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("benchmark requires a player controller"));
		return;
	}
	// 正式基线必须保留完整玩法：HUD、受伤/死亡、碰撞、AI、动画和渲染消费者都按正常规则运行。
	// 只有命令行显式传入 BenchmarkDisable* 时，后面的诊断阶段才允许关闭单个消费者。

	PhaseDeadline = FPlatformTime::Seconds() + 60.0;
	ScheduleStage(Phase, &UfpstrueBenchmarkRunner::WaitForBenchmarkReady, 0.25f, true);
}

void UfpstrueBenchmarkRunner::WaitForBenchmarkReady()
{
	// 等待分帧生成队列清空后再开始预热，使稳态数据不混入批量 Spawn 尖峰。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr || !OwnerGameMode->IsRunning())
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("gameplay stopped before ready"));
		return;
	}

	if (OwnerGameMode->PendingEnemySpawnCount > 0)
	{
		// 超时统一交给独立 Watchdog，世界暂停也不会阻塞拒收。
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	ExpectedEnemyCount = BenchmarkConfig.HasEnemyCountOverride()
		? BenchmarkConfig.EnemyCount : OwnerGameMode->RegisteredEnemies.Num();
	bPopulationChanged = false;
	OwnerGameMode->OnAliveEnemyCountChanged.AddUniqueDynamic(this, &UfpstrueBenchmarkRunner::HandleEnemyCountChanged);
	// 配置先应用，再预热。内存报告/截图等扰动也留在预热之前，不混入正式采集窗口。
	ApplyDiagnosticOverrides();
	if (!ValidateBenchmarkState(TEXT("ready")))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("ready validation failed"));
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark ready: requested=%d alive=%d warmup=%.1fs"), BenchmarkConfig.EnemyCount,
		   OwnerGameMode->RegisteredEnemies.Num(), BenchmarkConfig.WarmupSeconds);

	if (BenchmarkConfig.bCollectTextureStats)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("DumpTextureStreamingStats"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("ListStreamingTextures"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("MemReport -full"));
	}
	if (BenchmarkConfig.bTakeScreenshot)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Shot"));
	}
	Phase = EFPBenchmarkPhase::WarmingUp;
	PhaseDeadline = FPlatformTime::Seconds() + BenchmarkConfig.WarmupSeconds + ProfilerTimeoutSeconds;
	// 零预热也保存 next-tick 句柄；正常样本仍应配置足够预热让资源与诊断设置稳定。
	ScheduleStage(Phase, &UfpstrueBenchmarkRunner::StartCapture, BenchmarkConfig.WarmupSeconds);
}

// ==================== 正式采集与消融快照 ====================

void UfpstrueBenchmarkRunner::StartCapture()
{
	// 只读消费者快照，不能在预热完成后重新施加另一套配置。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!ValidateBenchmarkState(TEXT("capture-start"), /*bLogSuccess=*/true, /*bLogConsumers=*/true))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("capture-start validation failed"));
		return;
	}
#if CSV_PROFILER
	FCsvProfiler* Csv = FCsvProfiler::Get();
	if (HasOutstandingCsvLease() || Csv->IsCapturing() || Csv->IsWritingFile() || Csv->IsEndCapturePending() || FTraceAuxiliary::IsConnected())
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("profiler is already owned by another capture"));
		return;
	}
	if (!BenchmarkConfig.TraceFile.IsEmpty())
	{
		if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *BenchmarkConfig.TraceFile,
			TEXT("cpu,frame,bookmark,task,stats")))
		{
			Finish(EFPBenchmarkResult::Failed, TEXT("Trace start was rejected"));
			return;
		}
		bTraceActive = true;
		TraceOutputPath = FTraceAuxiliary::GetTraceDestinationString();
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace started: %s"), *TraceOutputPath);
	}
	RequestCsvCapture([](const FString& Filename) { FCsvProfiler::Get()->BeginCapture(-1, FString(), Filename); });
	ClearStageTimers();
	Phase = EFPBenchmarkPhase::StartingCapture;
	PhaseDeadline = FPlatformTime::Seconds() + ProfilerTimeoutSeconds;
#else
	Finish(EFPBenchmarkResult::Unsupported, TEXT("CSV profiler is not compiled in this target"));
#endif
}

void UfpstrueBenchmarkRunner::RequestCsvCapture(TFunctionRef<void(const FString&)> SubmitStart)
{
#if CSV_PROFILER
	check(!HasOutstandingCsvLease());
	CsvCapture = MakeShared<FFPBenchmarkCsvCapture>();
	CsvCapture->Filename = FString::Printf(TEXT("AutomatedBenchmark_%s.csv"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	OutstandingCsvCapture = CsvCapture;
	const TSharedRef<FFPBenchmarkCsvCapture> Capture = CsvCapture.ToSharedRef();
	Capture->FirstFrameHandle = FCsvProfiler::Get()->OnCSVProfileFirstFrame().AddLambda([Capture]()
	{
		FCsvProfiler* Profiler = FCsvProfiler::Get();
		// 手动采集若抢先启动，不会被本 Runner 停止，也不会被标记为本次输出。
		if (FPaths::GetCleanFilename(Profiler->GetOutputFilename()) == Capture->Filename)
		{
			const bool bCancelLateStart = Capture->State == FFPBenchmarkCsvCapture::EState::CancelPendingStart;
			Capture->State = FFPBenchmarkCsvCapture::EState::Capturing;
			Profiler->OnCSVProfileFirstFrame().Remove(Capture->FirstFrameHandle);
			Capture->FirstFrameHandle.Reset();
			if (bCancelLateStart)
			{
				Capture->WriteComplete = Profiler->EndCapture();
				Capture->State = FFPBenchmarkCsvCapture::EState::StopRequested;
			}
		}
	});
	// 不持有 UObject，也不依赖 World Tick。失败的业务已可结束，但租约要等真实产物写完才释放。
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Capture](float)
	{
		if (Capture->State == FFPBenchmarkCsvCapture::EState::StopRequested &&
			Capture->WriteComplete.IsValid() && Capture->WriteComplete.IsReady())
		{
			Capture->State = FFPBenchmarkCsvCapture::EState::Released;
		}
		if (Capture->State != FFPBenchmarkCsvCapture::EState::Released) return true;
		if (OutstandingCsvCapture == Capture) OutstandingCsvCapture.Reset();
		return false;
	}), 0.1f);
	SubmitStart(Capture->Filename);
#endif
}

void UfpstrueBenchmarkRunner::PollCaptureStart()
{
	if (CsvCapture.IsValid() && CsvCapture->State == FFPBenchmarkCsvCapture::EState::Capturing)
	{
		if (!ValidateBenchmarkState(TEXT("capture-start-confirmed")))
		{
			Finish(EFPBenchmarkResult::InvalidSample, TEXT("state changed while starting profilers"));
			return;
		}
		Phase = EFPBenchmarkPhase::Capturing;
		if (bTraceActive)
		{
			UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionBegin AutomatedBenchmarkCapture"));
			bTraceRegionActive = true;
		}
		PhaseDeadline = FPlatformTime::Seconds() + FFPBenchmarkConfig::Get().DurationSeconds;
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture started: requested=%d alive=%d duration=%.1fs"),
			FFPBenchmarkConfig::Get().EnemyCount, ExpectedEnemyCount, FFPBenchmarkConfig::Get().DurationSeconds);
	}
	else if (FPlatformTime::Seconds() >= PhaseDeadline)
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("CSV capture did not start"));
	}
}

void UfpstrueBenchmarkRunner::PollCapture()
{
	const bool bCaptureEnded = FPlatformTime::Seconds() >= PhaseDeadline;
	if (!ValidateBenchmarkState(bCaptureEnded ? TEXT("capture-end") : TEXT("capture-window"), bCaptureEnded))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("capture validation failed"));
		return;
	}
#if CSV_PROFILER
	if (!FCsvProfiler::Get()->IsCapturing() ||
		FPaths::GetCleanFilename(FCsvProfiler::Get()->GetOutputFilename()) != CsvCapture->Filename ||
		(bTraceActive && FTraceAuxiliary::GetTraceDestinationString() != TraceOutputPath))
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("owned profiler was interrupted"));
		return;
	}
#endif
	if (bCaptureEnded)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture stopped."));
		Finish(EFPBenchmarkResult::Completed, TEXT("capture window ended; verifying artifacts"));
	}
}

void UfpstrueBenchmarkRunner::ApplyDiagnosticOverrides()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr || !DiagnosticStates.IsEmpty())
	{
		return;
	}
	const FFPBenchmarkConfig& Config = FFPBenchmarkConfig::Get();
	if (!Config.bDisableAttackSweep && !Config.bDisableEnemyPawnCollision && !Config.bDisableCharacterMovementTick &&
		!Config.bDisableSkeletalMeshTick && !Config.bDisablePathFollowingTick)
	{
		return;
	}
	DiagnosticStates.Reserve(OwnerGameMode->RegisteredEnemies.Num());
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr || Enemy->IsDead())
		{
			continue;
		}
		FDiagnosticState& State = DiagnosticStates.AddDefaulted_GetRef();
		State.Enemy = Enemy;
		State.PawnResponse = Enemy->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn);
		if (Config.bDisableAttackSweep)
		{
			if (UfpstrueEnemyCombatComponent* Combat = Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>())
			{
				Combat->SetAttackSweepDisabledForBenchmark(true);
			}
		}
		if (Config.bDisableEnemyPawnCollision)
		{
			Enemy->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		}
		DisableTickForCapture(Enemy->GetCharacterMovement(), Config.bDisableCharacterMovementTick, State.bMovementTick);
		DisableTickForCapture(Enemy->GetMesh(), Config.bDisableSkeletalMeshTick, State.bMeshTick);
		if (AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
		{
			State.PathFollowing = Controller->GetPathFollowingComponent();
			DisableTickForCapture(State.PathFollowing.Get(), Config.bDisablePathFollowingTick, State.bPathFollowingTick);
		}
	}
}

// 验证与起始日志共用一次采样；普通采集检查不统计渲染档位，也不遍历世界。
UfpstrueBenchmarkRunner::FEnemySnapshot UfpstrueBenchmarkRunner::ReadEnemySnapshot(bool bDetailed) const
{
	FEnemySnapshot Snapshot;
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr) return Snapshot;
	const FFPBenchmarkConfig& Config = FFPBenchmarkConfig::Get();
	const UCrowdManager* CrowdManager = UCrowdManager::GetCurrent(GetWorld());
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		const AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr || Enemy->IsDead())
		{
			Snapshot.bRegistryValid = false;
			continue;
		}
		++Snapshot.Enemies;
		const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
		const USkeletalMeshComponent* Mesh = Enemy->GetMesh();
		const AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController());
		const UPathFollowingComponent* Path = Controller != nullptr ? Controller->GetPathFollowingComponent() : nullptr;
		Snapshot.bRegistryValid &= Controller != nullptr;
		if (Movement != nullptr)
		{
			Snapshot.RVO += Movement->bUseRVOAvoidance ? 1 : 0;
			Snapshot.bDiagnosticStateValid &= !Config.bDisableCharacterMovementTick || !Movement->IsComponentTickEnabled();
		}
		if (const UCrowdFollowingComponent* Crowd = Cast<UCrowdFollowingComponent>(Path))
		{
			++Snapshot.Crowd;
			// 组件存在不等于已分配 Detour 槽位，必须同时检查真实代理与模拟状态。
			Snapshot.ValidCrowd += CrowdManager && CrowdManager->IsAgentValid(Crowd) && Crowd->IsCrowdSimulationActive() ? 1 : 0;
		}
		Snapshot.bDiagnosticStateValid &= !Config.bDisablePathFollowingTick || (Path && !Path->IsComponentTickEnabled());
		Snapshot.bDiagnosticStateValid &= !Config.bDisableSkeletalMeshTick || (Mesh && !Mesh->IsComponentTickEnabled());
		Snapshot.bDiagnosticStateValid &= !Config.bDisableEnemyPawnCollision ||
			Enemy->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Ignore;
		if (!bDetailed) continue;
		if (Movement != nullptr)
		{
			Snapshot.MovementTickEnabled += Movement->IsComponentTickEnabled() ? 1 : 0;
			const int32 Tier = Movement->GetComponentTickInterval() <= KINDA_SMALL_NUMBER ? 0 :
				(Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Reduced ? 1 : 2);
			++Snapshot.Movement[Tier];
		}
		Snapshot.Attacking += Enemy->IsAttacking() ? 1 : 0;
		++Snapshot.Render[FMath::Clamp(static_cast<int32>(Enemy->GetRenderSignificanceTier()), 0, 2)];
		++Snapshot.MinLOD[FMath::Clamp(Enemy->GetAppliedMinimumLOD(), 0, 2)];
		if (Mesh != nullptr)
		{
			Snapshot.SkeletalMeshTickEnabled += Mesh->IsComponentTickEnabled() ? 1 : 0;
			Snapshot.ShadowCasting += Mesh->CastShadow ? 1 : 0;
			Snapshot.RayTracingVisible += Mesh->bVisibleInRayTracing ? 1 : 0;
		}
	}
	return Snapshot;
}

void UfpstrueBenchmarkRunner::LogConsumerSnapshot(const FEnemySnapshot& Snapshot) const
{
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr) return;
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark diagnostics applied: enemies=%d attackSweepOff=%d pawnCollisionOff=%d pathFollowingTickOff=%d "
				"characterMovementTickOff=%d skeletalMeshTickOff=%d significanceOff=%d"),
		   Snapshot.Enemies, BenchmarkConfig.bDisableAttackSweep ? 1 : 0, BenchmarkConfig.bDisableEnemyPawnCollision ? 1 : 0,
		   BenchmarkConfig.bDisablePathFollowingTick ? 1 : 0, BenchmarkConfig.bDisableCharacterMovementTick ? 1 : 0,
		   BenchmarkConfig.bDisableSkeletalMeshTick ? 1 : 0, BenchmarkConfig.bDisableEnemySignificance ? 1 : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark enemy snapshot: movementFull=%d movementMid=%d movementFar=%d movementTickEnabled=%d skeletalMeshTickEnabled=%d "
				"attacking=%d castingShadow=%d rayTracingVisible=%d animationSharingFollowers=%d"),
		   Snapshot.Movement[0], Snapshot.Movement[1], Snapshot.Movement[2], Snapshot.MovementTickEnabled, Snapshot.SkeletalMeshTickEnabled,
		   Snapshot.Attacking, Snapshot.ShadowCasting, Snapshot.RayTracingVisible,
		   OwnerGameMode->EnemyAnimationSharingCoordinator != nullptr
			   ? OwnerGameMode->EnemyAnimationSharingCoordinator->GetRegisteredEnemyCount()
			   : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark avoidance snapshot: enemies=%d rvoEnabled=%d crowdFollowing=%d crowdValid=%d"),
		   Snapshot.Enemies, Snapshot.RVO, Snapshot.Crowd, Snapshot.ValidCrowd);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark render significance snapshot: renderFull=%d renderReduced=%d renderBackground=%d lod0=%d lod1=%d lod2Plus=%d "
				"fullBudget=%d shadowBudget=%d rayTracingBudget=%d"),
		   Snapshot.Render[0], Snapshot.Render[1], Snapshot.Render[2], Snapshot.MinLOD[0], Snapshot.MinLOD[1], Snapshot.MinLOD[2],
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies);
	UE_LOG(LogTemp, Display, TEXT("Benchmark LOD snapshot semantics: lod0/lod1/lod2Plus count applied minimum-LOD constraints, not actually selected LODs."));

	// 一次性读回实际管理范围，尸体已退出存活注册表，必须单独观察；不在每帧遍历世界。
	int32 AliveMeshes = 0, AliveShadowMeshes = 0, AliveRayTracingMeshes = 0;
	int32 CorpseMeshes = 0, CorpseShadowMeshes = 0, CorpseRayTracingMeshes = 0;
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
	{
		int32 Meshes, ShadowMeshes, RayTracingMeshes;
		It->GetRenderBudgetMeshCounts(Meshes, ShadowMeshes, RayTracingMeshes);
		if (It->IsDead())
		{
			CorpseMeshes += Meshes;
			CorpseShadowMeshes += ShadowMeshes;
			CorpseRayTracingMeshes += RayTracingMeshes;
		}
		else
		{
			AliveMeshes += Meshes;
			AliveShadowMeshes += ShadowMeshes;
			AliveRayTracingMeshes += RayTracingMeshes;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("Benchmark managed Mesh flags: alive=%d shadow=%d rayTracing=%d corpse=%d corpseShadow=%d corpseRayTracing=%d (components, not GPU primitives)"),
		AliveMeshes, AliveShadowMeshes, AliveRayTracingMeshes, CorpseMeshes, CorpseShadowMeshes, CorpseRayTracingMeshes);
}

bool UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(int32 Enemies, int32 RVO, int32 Crowd, int32 ValidCrowd)
{
	// 空场景本来就没有避让消费者，不能被当成错误配置；非空场景要求完整的一套实现。
	return (Enemies == 0 && RVO == 0 && Crowd == 0 && ValidCrowd == 0) ||
		(Enemies > 0 && RVO == Enemies && Crowd == 0 && ValidCrowd == 0) ||
		(Enemies > 0 && RVO == 0 && Crowd == Enemies && ValidCrowd == Enemies);
}

void UfpstrueBenchmarkRunner::HandleEnemyCountChanged(int32 AliveEnemies)
{
	// 锁存中途变化，即使采集结束前人数又恢复，也不能把替换过的样本判为稳定。
	if (ExpectedEnemyCount != INDEX_NONE && AliveEnemies != ExpectedEnemyCount)
	{
		bPopulationChanged = true;
	}
}

bool UfpstrueBenchmarkRunner::ValidateBenchmarkState(const TCHAR* ValidationPhase, bool bLogSuccess, bool bLogConsumers) const
{
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	const AfpstrueCharacter* Player =
		OwnerGameMode != nullptr && IsValid(OwnerGameMode->PlayerCharacter)
			? OwnerGameMode->PlayerCharacter.Get()
			: nullptr;
	const UfpstrueHealthComponent* HealthComponent = Player != nullptr ? Player->GetHealthComponent() : nullptr;
	const FEnemySnapshot Snapshot = ReadEnemySnapshot(bLogConsumers);
	const float PlayerHealth = HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
	const bool bEnemyCountValid = ExpectedEnemyCount != INDEX_NONE && Snapshot.Enemies == ExpectedEnemyCount &&
		!bPopulationChanged && Snapshot.bRegistryValid;
	const bool bAvoidanceValid = IsAvoidanceConfigurationValid(Snapshot.Enemies, Snapshot.RVO, Snapshot.Crowd, Snapshot.ValidCrowd);
	const APlayerController* PlayerController = LockedController.Get();
	const FVector CurrentPlayerLocation = Player != nullptr ? Player->GetActorLocation() : FVector::ZeroVector;
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
	const bool bViewTransformValid = bBenchmarkInputLocked && PlayerController != nullptr && Player == LockedPlayer.Get() &&
		PlayerController->GetPawn() == Player && PlayerController->GetViewTarget() == Player &&
		(LocationDrift <= BenchmarkPlayerLocationTolerance && PitchDrift <= BenchmarkPlayerRotationToleranceDegrees &&
		 YawDrift <= BenchmarkPlayerRotationToleranceDegrees);
	const bool bStateValid = OwnerGameMode != nullptr && OwnerGameMode->IsRunning() &&
		HealthComponent != nullptr && !HealthComponent->IsDead() && bEnemyCountValid && bViewTransformValid &&
		bAvoidanceValid && Snapshot.bDiagnosticStateValid;

	if (bStateValid && bLogSuccess)
	{
		UE_LOG(LogTemp, Display,
			   TEXT("Automated benchmark validation: phase=%s requested=%d alive=%d playerHealth=%.1f locationDrift=%.2f pitchDrift=%.2f yawDrift=%.2f"),
			   ValidationPhase, ExpectedEnemyCount, Snapshot.Enemies, PlayerHealth, LocationDrift, PitchDrift, YawDrift);
	}
	else if (!bStateValid)
	{
		UE_LOG(LogTemp, Error,
			   TEXT("Automated benchmark invalid: phase=%s requested=%d alive=%d playerHealth=%.1f running=%d ended=%d locationDrift=%.2f pitchDrift=%.2f yawDrift=%.2f"),
			   ValidationPhase, ExpectedEnemyCount, Snapshot.Enemies, PlayerHealth,
			   OwnerGameMode != nullptr && OwnerGameMode->IsRunning() ? 1 : 0,
			   OwnerGameMode != nullptr && OwnerGameMode->IsFinished() ? 1 : 0,
			   LocationDrift, PitchDrift, YawDrift);
		UE_LOG(LogTemp, Error, TEXT("Benchmark validation gates: populationChanged=%d registryValid=%d avoidanceValid=%d diagnosticStateValid=%d rvo=%d crowd=%d crowdValid=%d"),
			bPopulationChanged, Snapshot.bRegistryValid, bAvoidanceValid, Snapshot.bDiagnosticStateValid, Snapshot.RVO, Snapshot.Crowd, Snapshot.ValidCrowd);
	}

	if (bStateValid && bLogConsumers) LogConsumerSnapshot(Snapshot);
	return bStateValid;
}

// ==================== 采集结束与进程退出 ====================

void UfpstrueBenchmarkRunner::Finish(EFPBenchmarkResult InResult, const TCHAR* Reason)
{
	ClearStageTimers();
	Result = InResult;
	Phase = EFPBenchmarkPhase::Flushing;
	StopActiveProfilers();
	ReleaseSceneOwnership();
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark finishing: result=%s reason=%s"), ResultName(Result), Reason);
	PhaseDeadline = FPlatformTime::Seconds() + ProfilerTimeoutSeconds;
	StartWatchdog();
}

void UfpstrueBenchmarkRunner::StopActiveProfilers()
{
#if CSV_PROFILER
	if (CsvCapture.IsValid())
	{
		FCsvProfiler* Csv = FCsvProfiler::Get();
		if (CsvCapture->State == FFPBenchmarkCsvCapture::EState::StartRequested)
		{
			CsvCapture->State = FFPBenchmarkCsvCapture::EState::CancelPendingStart;
		}
		else if (CsvCapture->State == FFPBenchmarkCsvCapture::EState::Capturing && Csv->IsCapturing() &&
			FPaths::GetCleanFilename(Csv->GetOutputFilename()) == CsvCapture->Filename)
		{
			CsvCapture->WriteComplete = Csv->EndCapture();
			CsvCapture->State = FFPBenchmarkCsvCapture::EState::StopRequested;
		}
	}
#endif
	if (bTraceActive && !bTraceStopRequested)
	{
		if (FTraceAuxiliary::GetTraceDestinationString() != TraceOutputPath)
		{
			// 外部手动关闭/替换 Trace：不能替它停止新会话，也不能认证旧样本完整。
			bTraceActive = false;
			Result = EFPBenchmarkResult::Failed;
			return;
		}
		if (bTraceRegionActive)
		{
			UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionEnd AutomatedBenchmarkCapture"));
			bTraceRegionActive = false;
		}
		// UE 5.5 Stop 仅排入停止请求，文件真正关闭由后续 IsConnected 读回确认。
		bTraceStopRequested = FTraceAuxiliary::Stop();
	}
}

void UfpstrueBenchmarkRunner::PollArtifacts()
{
	StopActiveProfilers();
	const bool bCsvFinished = !CsvCapture.IsValid() || (CsvCapture->WriteComplete.IsValid() && CsvCapture->WriteComplete.IsReady());
	const bool bTraceFinished = !bTraceActive || (bTraceStopRequested && !FTraceAuxiliary::IsConnected());
	if (!bCsvFinished || !bTraceFinished)
	{
		if (Phase != EFPBenchmarkPhase::Quarantined && FPlatformTime::Seconds() >= PhaseDeadline)
		{
			ClearStageTimers();
			Phase = EFPBenchmarkPhase::Quarantined;
			Result = EFPBenchmarkResult::Failed;
			UE_LOG(LogTemp, Error, TEXT("Automated benchmark artifact flush timed out: csv=%d trace=%d; result=Failed resources=Quarantined (late-start cleanup retained)."),
				bCsvFinished, bTraceFinished);
			ExitBenchmark();
		}
		// 结果已失败，但尚未回收：既不移除 CSV 首帧回调，也不把进程所有权标为空闲。
		return;
	}
	if (bTraceActive && bTraceStopRequested && bTraceFinished)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace stopped."));
	}
	// bCsvFinished 已确认 Future 可读；租约仅由独立 Ticker 释放，Runner 不维护第二套释放路径。
	const FString CsvPath = CsvCapture.IsValid() ? CsvCapture->WriteComplete.Get() : FString();
	const bool bCsvArtifactValid = !CsvPath.IsEmpty() && IFileManager::Get().FileSize(*CsvPath) > 0;
	const bool bTraceArtifactValid = TraceOutputPath.IsEmpty() || IFileManager::Get().FileSize(*TraceOutputPath) > 0;
	if (Result == EFPBenchmarkResult::Completed && (!bCsvArtifactValid || !bTraceArtifactValid))
	{
		Result = EFPBenchmarkResult::Failed;
		UE_LOG(LogTemp, Error, TEXT("Automated benchmark artifact missing or empty: csv=%s trace=%s"), *CsvPath, *TraceOutputPath);
	}
	ClearStageTimers();
	bTraceActive = false;
	Phase = EFPBenchmarkPhase::Finished;
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark result: %s csv=%s trace=%s"), ResultName(Result), *CsvPath, *TraceOutputPath);
	if (Result == EFPBenchmarkResult::Completed)
	{
		// 保持脚本兼容，但只有写盘完成且文件存在才发出成功标记。
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark completed successfully."));
	}
	ExitBenchmark();
}

uint8 UfpstrueBenchmarkRunner::GetResultExitCode(EFPBenchmarkResult InResult)
{
	// 与外部批处理约定：0 仅表示 Runner 完成；最终样本仍须通过脚本离线校验。
	switch (InResult)
	{
	case EFPBenchmarkResult::Completed: return 0;
	case EFPBenchmarkResult::InvalidSample: return 2;
	case EFPBenchmarkResult::Failed: return 3;
	case EFPBenchmarkResult::Cancelled: return 4;
	case EFPBenchmarkResult::Unsupported: return 5;
	default: return 3;
	}
}

void UfpstrueBenchmarkRunner::ExitBenchmark()
{
	if (!FFPBenchmarkConfig::Get().bAutoQuit) return;
	const uint8 ExitCode = GetResultExitCode(Result);
	// 编辑器编译的 -game 独立进程也支持；PIE/编辑器宿主绝不请求进程退出。
	if (!GIsEditor && GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Game)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark process exit: code=%d result=%s"), ExitCode, ResultName(Result));
		FPlatformMisc::RequestExitWithStatus(false, ExitCode, TEXT("fpstrueBenchmarkRunner"));
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark auto-quit suppressed for editor/PIE: code=%d result=%s"), ExitCode, ResultName(Result));
	}
}
