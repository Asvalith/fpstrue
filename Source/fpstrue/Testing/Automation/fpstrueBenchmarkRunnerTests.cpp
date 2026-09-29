// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Player/fpstrueCharacter.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueBenchmarkLifecycleTest, "fpstrue.Performance.Benchmark.CancellationAndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueBenchmarkLifecycleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game) || !World.BeginPlayInTestWorld())
	{
		World.ForwardErrorMessages(this);
		return false;
	}
	AActor* Owner = World.GetTestWorld()->SpawnActor<AActor>();
	UfpstrueBenchmarkRunner* Runner = NewObject<UfpstrueBenchmarkRunner>(Owner);
	Runner->RegisterComponent();
	FTimerManager& Timers = World.GetTestWorld()->GetTimerManager();

	// 覆盖过去遗漏的 next-tick 回调；真实 BeginBenchmark 若意外执行会报 GameMode 无效。
	Runner->Phase = EFPBenchmarkPhase::Queued;
	Runner->ScheduleStage(Runner->Phase, &UfpstrueBenchmarkRunner::BeginBenchmark, 0.0f);
	TestTrue(TEXT("Next-tick callback is tracked by a cancellable handle"), Timers.TimerExists(Runner->StageTimerHandle));
	const uint64 QueuedRun = Runner->RunId;
	Runner->Cancel();
	TestTrue(TEXT("Cancel advances run identity"), Runner->RunId > QueuedRun);
	TestTrue(TEXT("Cancel enters artifact cleanup, not capture"), Runner->Phase == EFPBenchmarkPhase::Flushing);
	Runner->PollArtifacts();
	World.TickTestWorld();
	TestTrue(TEXT("Cancelled queued start cannot restart the benchmark"), Runner->Phase == EFPBenchmarkPhase::Finished);
	TestTrue(TEXT("Cancellation is not success"), Runner->Result == EFPBenchmarkResult::Cancelled);
	Runner->Cancel();
	TestTrue(TEXT("Repeated cancellation is idempotent"), Runner->Result == EFPBenchmarkResult::Cancelled);

	Runner->Phase = EFPBenchmarkPhase::Queued;
	Runner->ScheduleStage(Runner->Phase, &UfpstrueBenchmarkRunner::BeginBenchmark, 0.0f);
	++Runner->RunId;
	World.TickTestWorld();
	TestTrue(TEXT("Stale generation callback is ignored even while object is alive"), Runner->Phase == EFPBenchmarkPhase::Queued);
	Runner->ClearStageTimers();

	// Profiler 阶段不再另设世界 Timer；独立 Watchdog 自己处理启动确认与超时。
	Runner->Phase = EFPBenchmarkPhase::StartingCapture;
	Runner->PhaseDeadline = FPlatformTime::Seconds() - 1.0;
	Runner->TickWatchdog();
	TestTrue(TEXT("Watchdog alone advances a failed profiler start into cleanup"), Runner->Phase == EFPBenchmarkPhase::Flushing);
	TestTrue(TEXT("Profiler-start timeout remains failure"), Runner->Result == EFPBenchmarkResult::Failed);
	TestFalse(TEXT("Profiler polling leaves no duplicate world timer"), Timers.TimerExists(Runner->StageTimerHandle));
	Runner->PollArtifacts();

	// 两层输入屏蔽：已有系统一层，Benchmark 再加一层，清理只撤销自己持有的一层。
	APlayerController* Controller = World.GetTestWorld()->SpawnActor<APlayerController>();
	AfpstrueCharacter* Player = World.GetTestWorld()->SpawnActor<AfpstrueCharacter>();
	AActor* OriginalView = World.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Controller"), Controller) || !TestNotNull(TEXT("Player"), Player) ||
		!TestNotNull(TEXT("Previous camera target"), OriginalView))
	{
		return false;
	}
	Controller->SetIgnoreMoveInput(true);
	Controller->SetIgnoreLookInput(true);
	Controller->SetIgnoreMoveInput(true);
	Controller->SetIgnoreLookInput(true);
	Controller->SetViewTarget(Player);
	Runner->LockedController = Controller;
	Runner->LockedPlayer = Player;
	Runner->PreviousViewTarget = OriginalView;
	Runner->PreviousControlRotation = FRotator(0.0f, 35.0f, 0.0f);
	Runner->bBenchmarkInputLocked = true;
	Runner->ReleaseSceneOwnership();
	Runner->ReleaseSceneOwnership();
	TestTrue(TEXT("Other system's move lock survives repeated cleanup"), Controller->IsMoveInputIgnored());
	TestTrue(TEXT("Other system's look lock survives repeated cleanup"), Controller->IsLookInputIgnored());
	TestTrue(TEXT("Original camera target is restored"), Controller->GetViewTarget() == OriginalView);
	TestTrue(TEXT("Original control rotation is restored"), Controller->GetControlRotation().Equals(FRotator(0.0f, 35.0f, 0.0f)));
	Controller->SetIgnoreMoveInput(false);
	Controller->SetIgnoreLookInput(false);
	TestFalse(TEXT("No benchmark move lock leaked"), Controller->IsMoveInputIgnored());
	TestFalse(TEXT("No benchmark look lock leaked"), Controller->IsLookInputIgnored());

	Runner->Phase = EFPBenchmarkPhase::Finished;
	Runner->DestroyComponent();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueBenchmarkValidationTest, "fpstrue.Performance.Benchmark.SampleValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueBenchmarkValidationTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Zero-enemy scene is a valid empty avoidance configuration"),
		UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(0, 0, 0, 0));
	TestTrue(TEXT("Complete RVO configuration"), UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(160, 160, 0, 0));
	TestTrue(TEXT("Complete Detour configuration"), UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(160, 0, 160, 160));
	TestFalse(TEXT("Partial Crowd capacity invalidates sample"), UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(160, 0, 160, 50));
	TestFalse(TEXT("Mixed avoidance invalidates sample"), UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(160, 80, 80, 80));
	TestFalse(TEXT("No avoidance in a non-empty baseline is invalid"), UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(160, 0, 0, 0));

	UfpstrueBenchmarkRunner* Runner = NewObject<UfpstrueBenchmarkRunner>();
	Runner->ExpectedEnemyCount = 2;
	Runner->HandleEnemyCountChanged(2);
	TestFalse(TEXT("Unchanged count does not invalidate sample"), Runner->bPopulationChanged);
	Runner->HandleEnemyCountChanged(1);
	Runner->HandleEnemyCountChanged(2);
	TestTrue(TEXT("Mid-window population change stays invalid even after count recovers"), Runner->bPopulationChanged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueBenchmarkLateCsvTest, "fpstrue.Performance.Benchmark.LateCsvStartAfterCleanupTimeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueBenchmarkLateCsvTest::RunTest(const FString& Parameters)
{
#if CSV_PROFILER
	FCsvProfiler* Csv = FCsvProfiler::Get();
	if (Csv->IsCapturing() || Csv->IsWritingFile() || Csv->IsEndCapturePending() || UfpstrueBenchmarkRunner::HasOutstandingCsvLease())
	{
		AddError(TEXT("Late-start integration test requires an idle CSV profiler; it must not stop another capture."));
		return false;
	}
	const TSharedRef<FTestWorldWrapper> World = MakeShared<FTestWorldWrapper>();
	if (!World->CreateTestWorld(EWorldType::Game) || !World->BeginPlayInTestWorld())
	{
		World->ForwardErrorMessages(this);
		return false;
	}
	AActor* Owner = World->GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Benchmark owner"), Owner)) return false;
	UfpstrueBenchmarkRunner* Runner = NewObject<UfpstrueBenchmarkRunner>(Owner);
	Runner->RegisterComponent();
	Runner->Phase = EFPBenchmarkPhase::StartingCapture;
	FString DeferredFilename;
	// 只延迟提交时机；后续通过真实 FCsvProfiler 首帧委托和写盘 Future 验证，不伪造 started/finished。
	Runner->RequestCsvCapture([&DeferredFilename](const FString& Filename) { DeferredFilename = Filename; });
	Runner->Finish(EFPBenchmarkResult::Failed, TEXT("injected delayed CSV submission"));
	Runner->PhaseDeadline = FPlatformTime::Seconds() - 1.0; // 推进业务截止点，不等待 30 秒墙钟。
	AddExpectedError(TEXT("Automated benchmark artifact flush timed out:"), EAutomationExpectedErrorFlags::Contains, 1);
	Runner->PollArtifacts();
	TestTrue(TEXT("Business failure does not declare process resources released"), Runner->Phase == EFPBenchmarkPhase::Quarantined);
	TestTrue(TEXT("Result is Failed, never Completed"), Runner->Result == EFPBenchmarkResult::Failed);
	TestTrue(TEXT("CSV lease survives cleanup timeout"), UfpstrueBenchmarkRunner::HasOutstandingCsvLease());
	Runner->DestroyComponent();
	TestTrue(TEXT("World/component teardown preserves the established failure reason"), Runner->Result == EFPBenchmarkResult::Failed);
	UfpstrueBenchmarkRunner* AnotherRunner = NewObject<UfpstrueBenchmarkRunner>(Owner);
	AnotherRunner->RegisterComponent();
	TestFalse(TEXT("A different Runner cannot start while a late CSV request is outstanding"), AnotherRunner->CanStartFreshRun());
	AnotherRunner->DestroyComponent();

	Csv->BeginCapture(-1, FString(), DeferredFilename);
	const FString ExpectedPath = FPaths::ProfilingDir() / TEXT("CSV") / DeferredFilename;
	const double Deadline = FPlatformTime::Seconds() + 15.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, World, ExpectedPath, Deadline]()
	{
		if (UfpstrueBenchmarkRunner::HasOutstandingCsvLease())
		{
			if (FPlatformTime::Seconds() < Deadline) return false;
			AddError(TEXT("Actual late CSV start/stop did not finish within integration-test deadline."));
			return true;
		}
		TestFalse(TEXT("Actual late capture was stopped after the Runner was destroyed"), FCsvProfiler::Get()->IsCapturing());
		TestTrue(TEXT("Actual CSV write completed and produced an artifact"), IFileManager::Get().FileSize(*ExpectedPath) > 0);
		AddInfo(FString::Printf(TEXT("Late-start fault-injection artifact: %s"), *ExpectedPath));
		return true;
	}));
	return true;
#else
	AddError(TEXT("CSV profiler is required for the real late-start integration test."));
	return false;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueBenchmarkWatchdogTest, "fpstrue.Performance.Benchmark.PauseWatchdogAndExitContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueBenchmarkWatchdogTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Completed has success exit code"), UfpstrueBenchmarkRunner::GetResultExitCode(EFPBenchmarkResult::Completed), uint8(0));
	for (EFPBenchmarkResult Result : {EFPBenchmarkResult::InvalidSample, EFPBenchmarkResult::Failed,
		EFPBenchmarkResult::Cancelled, EFPBenchmarkResult::Unsupported})
	{
		TestTrue(TEXT("Every non-success result has a failure exit code"), UfpstrueBenchmarkRunner::GetResultExitCode(Result) != 0);
	}
	const TSharedRef<FTestWorldWrapper> World = MakeShared<FTestWorldWrapper>();
	if (!World->CreateTestWorld(EWorldType::Game) || !World->BeginPlayInTestWorld())
	{
		World->ForwardErrorMessages(this);
		return false;
	}
	AActor* Owner = World->GetTestWorld()->SpawnActor<AActor>();
	APlayerState* Pauser = World->GetTestWorld()->SpawnActor<APlayerState>();
	if (!TestNotNull(TEXT("Owner"), Owner) || !TestNotNull(TEXT("Pauser"), Pauser)) return false;
	UfpstrueBenchmarkRunner* Runner = NewObject<UfpstrueBenchmarkRunner>(Owner);
	Runner->RegisterComponent();
	Runner->Phase = EFPBenchmarkPhase::WarmingUp;
	Runner->PhaseDeadline = FPlatformTime::Seconds() + 60.0;
	World->GetTestWorld()->GetWorldSettings()->SetPauserPlayerState(Pauser);
	TestTrue(TEXT("The sample world is actually paused"), World->GetTestWorld()->IsPaused());
	Runner->StartWatchdog();
	const TWeakObjectPtr<UfpstrueBenchmarkRunner> WeakRunner(Runner);
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	// 不 TickTestWorld：让真实 CoreTicker 独立拒收暂停样本并完成没有 Profiler 的清理。
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, World, WeakRunner, Deadline]()
	{
		UfpstrueBenchmarkRunner* Active = WeakRunner.Get();
		if (Active == nullptr)
		{
			AddError(TEXT("Watchdog test Runner was unexpectedly destroyed."));
			return true;
		}
		if (Active->Phase != EFPBenchmarkPhase::Finished && FPlatformTime::Seconds() < Deadline) return false;
		TestTrue(TEXT("Paused-world cleanup progresses without World Timer ticks"), Active->Phase == EFPBenchmarkPhase::Finished);
		TestTrue(TEXT("Pause is rejected as InvalidSample"), Active->Result == EFPBenchmarkResult::InvalidSample);
		TestFalse(TEXT("A completed Runner cannot reuse its existing scene as a fresh experiment"), Active->CanStartFreshRun());
		World->GetTestWorld()->GetWorldSettings()->SetPauserPlayerState(nullptr);
		Active->DestroyComponent();
		return true;
	}));
	return true;
}

#endif
