// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueGameMode.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstruePerformanceStats.h"
#include "fpstrueSurroundManager.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "SignificanceManager.h"

DEFINE_STAT(STAT_fpstrueWaveSpawnTime);
DEFINE_STAT(STAT_fpstrueEnemySpawnCount);

AfpstrueGameMode::AfpstrueGameMode()
	: Super()
{
	SurroundManagerClass = AfpstrueSurroundManager::StaticClass();
}

void AfpstrueGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (FParse::Param(FCommandLine::Get(), TEXT("AutoBenchmark")))
	{
		GetWorldTimerManager().SetTimerForNextTick(this, &AfpstrueGameMode::BeginAutomatedBenchmark);
	}
}

void AfpstrueGameMode::StartGameMode()
{
	if (bGameRunning || bGameEnded)
	{
		return;
	}

	CacheSpawnPoints();

	if (!GetEnemyClassForWave(1))
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: EnemyClass is not configured."));
		FinishGame(false);
		return;
	}

	if (SpawnPoints.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: no TargetPoint has the tag '%s'."), *EnemySpawnTag.ToString());
		FinishGame(false);
		return;
	}

	if (SpawnPoints.Num() < MinimumSpawnPointCount)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("StartGameMode failed: found %d spawn points, but at least %d are required."),
			SpawnPoints.Num(),
			MinimumSpawnPointCount
		);
		FinishGame(false);
		return;
	}

	PlayerCharacter = Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
	if (!PlayerCharacter || !PlayerCharacter->GetHealthComponent())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: player or HealthComponent is invalid."));
		FinishGame(false);
		return;
	}

	if (!IsPlayerAlive())
	{
		UE_LOG(LogTemp, Warning, TEXT("StartGameMode failed: player is already dead."));
		FinishGame(false);
		return;
	}

	BindPlayerDeathEvent();

	if (!CreateSurroundManager())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: SurroundManager could not be created."));
		FinishGame(false);
		return;
	}

	ClearEnemyRegistrations();
	bGameRunning = true;
	CurrentWave = 0;
	AliveEnemyCount = 0;
	RemainingTime = GameDuration;

	OnRemainingTimeChanged.Broadcast(RemainingTime);
	OnWaveChanged.Broadcast(CurrentWave, GetConfiguredWaveCount());
	OnAliveEnemyCountChanged.Broadcast(AliveEnemyCount);

	GetWorldTimerManager().SetTimer(
		CountdownTimerHandle,
		this,
		&AfpstrueGameMode::UpdateCountdown,
		1.0f,
		true
	);

	StartNextWave();
	StartEnemySignificanceUpdates();
}

void AfpstrueGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearGameplayTimers();
	StopActiveEnemies();
	ClearEnemyRegistrations();

	UnbindPlayerDeathEvent();

	if (SurroundManager)
	{
		SurroundManager->ResetManager();
	}

	Super::EndPlay(EndPlayReason);
}

void AfpstrueGameMode::CacheSpawnPoints()
{
	SpawnPoints.Reset();
	UGameplayStatics::GetAllActorsOfClassWithTag(
		this,
		ATargetPoint::StaticClass(),
		EnemySpawnTag,
		SpawnPoints
	);
}

void AfpstrueGameMode::StartNextWave()
{
	const int32 ConfiguredWaveCount = GetConfiguredWaveCount();
	if (!bGameRunning || CurrentWave >= ConfiguredWaveCount)
	{
		return;
	}

	if (PendingEnemySpawnCount > 0)
	{
		GetWorldTimerManager().SetTimer(
			WaveTimerHandle,
			this,
			&AfpstrueGameMode::StartNextWave,
			FMath::Max(SpawnInterval, 0.01f),
			false
		);
		return;
	}

	++CurrentWave;
	OnWaveChanged.Broadcast(CurrentWave, ConfiguredWaveCount);

	SpawnCurrentWave();

	if (CurrentWave < ConfiguredWaveCount)
	{
		GetWorldTimerManager().SetTimer(
			WaveTimerHandle,
			this,
			&AfpstrueGameMode::StartNextWave,
			WaveInterval,
			false
		);
	}
}

bool AfpstrueGameMode::CreateSurroundManager()
{
	if (IsValid(SurroundManager))
	{
		SurroundManager->SetTargetCharacter(PlayerCharacter);
		return true;
	}

	UWorld* World = GetWorld();
	if (World == nullptr || !SurroundManagerClass)
	{
		return false;
	}

	SurroundManager = World->SpawnActor<AfpstrueSurroundManager>(
		SurroundManagerClass,
		FVector::ZeroVector,
		FRotator::ZeroRotator
	);

	if (!IsValid(SurroundManager))
	{
		return false;
	}

	SurroundManager->SetTargetCharacter(PlayerCharacter);
	return true;
}

int32 AfpstrueGameMode::GetConfiguredWaveCount() const
{
	if (GetBenchmarkEnemyCount() > 0)
	{
		return 1;
	}

	return WaveConfigs.IsEmpty() ? TotalWaves : WaveConfigs.Num();
}

int32 AfpstrueGameMode::GetEnemyCountForWave(int32 WaveNumber) const
{
	const int32 BenchmarkEnemyCount = GetBenchmarkEnemyCount();
	if (BenchmarkEnemyCount > 0)
	{
		return BenchmarkEnemyCount;
	}

	const int32 WaveIndex = WaveNumber - 1;
	if (WaveConfigs.IsValidIndex(WaveIndex))
	{
		return FMath::Max(WaveConfigs[WaveIndex].EnemyCount, 1);
	}

	return BaseEnemiesPerWave + WaveIndex * EnemiesAddedPerWave;
}

TSubclassOf<AfpstrueEnemyCharacter> AfpstrueGameMode::GetEnemyClassForWave(int32 WaveNumber) const
{
	const int32 WaveIndex = WaveNumber - 1;
	if (WaveConfigs.IsValidIndex(WaveIndex) && WaveConfigs[WaveIndex].EnemyClass)
	{
		return WaveConfigs[WaveIndex].EnemyClass;
	}

	return EnemyClass;
}

void AfpstrueGameMode::SpawnCurrentWave()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnCurrentWave);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);

	if (!bGameRunning || SpawnPoints.IsEmpty())
	{
		return;
	}

	ClearSpawnQueue();
	QueuedSpawnPoints = SpawnPoints;
	for (int32 Index = QueuedSpawnPoints.Num() - 1; Index > 0; --Index)
	{
		const int32 SwapIndex = FMath::RandRange(0, Index);
		QueuedSpawnPoints.Swap(Index, SwapIndex);
	}

	PendingEnemySpawnCount = GetEnemyCountForWave(CurrentWave);
	QueuedEnemyClass = GetEnemyClassForWave(CurrentWave);
	NextQueuedSpawnIndex = 0;
	ConsecutiveSpawnFailureCount = 0;

	SpawnNextQueuedEnemy();
	if (PendingEnemySpawnCount > 0)
	{
		GetWorldTimerManager().SetTimer(
			SpawnTimerHandle,
			this,
			&AfpstrueGameMode::SpawnNextQueuedEnemy,
			FMath::Max(SpawnInterval, 0.01f),
			true
		);
	}
}

void AfpstrueGameMode::SpawnNextQueuedEnemy()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnQueuedEnemy);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);

	if (!bGameRunning
		|| PendingEnemySpawnCount <= 0
		|| QueuedSpawnPoints.IsEmpty()
		|| !QueuedEnemyClass)
	{
		ClearSpawnQueue();
		return;
	}

	const int32 SpawnPointCount = QueuedSpawnPoints.Num();
	const int32 SpawnPointIndex = NextQueuedSpawnIndex % SpawnPointCount;
	const int32 SpawnPointReuseCount = NextQueuedSpawnIndex / SpawnPointCount;
	const bool bSpawnSucceeded = SpawnEnemyAtPoint(
		QueuedSpawnPoints[SpawnPointIndex],
		SpawnPointReuseCount,
		QueuedEnemyClass
	);

	++NextQueuedSpawnIndex;
	if (bSpawnSucceeded)
	{
		--PendingEnemySpawnCount;
		ConsecutiveSpawnFailureCount = 0;
	}
	else
	{
		++ConsecutiveSpawnFailureCount;
		const int32 FailureLimit = FMath::Max(SpawnPointCount * 4, 8);
		if (ConsecutiveSpawnFailureCount >= FailureLimit)
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("Enemy spawn queue stopped after %d consecutive failures with %d enemies remaining."),
				ConsecutiveSpawnFailureCount,
				PendingEnemySpawnCount
			);
			ClearSpawnQueue();
			return;
		}
	}

	if (PendingEnemySpawnCount <= 0)
	{
		ClearSpawnQueue();
	}
}

void AfpstrueGameMode::ClearSpawnQueue()
{
	GetWorldTimerManager().ClearTimer(SpawnTimerHandle);
	QueuedSpawnPoints.Reset();
	QueuedEnemyClass = nullptr;
	PendingEnemySpawnCount = 0;
	NextQueuedSpawnIndex = 0;
	ConsecutiveSpawnFailureCount = 0;
}

bool AfpstrueGameMode::SpawnEnemyAtPoint(
	AActor* SpawnPoint,
	int32 SpawnPointReuseCount,
	TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass
)
{
	if (!IsValid(SpawnPoint))
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy spawn point is invalid."));
		return false;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (NavSystem == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy spawn requires a valid navigation system."));
		return false;
	}

	const AfpstrueEnemyCharacter* EnemyDefaults = WaveEnemyClass.GetDefaultObject();
	const UCapsuleComponent* DefaultCapsule = EnemyDefaults != nullptr
		? EnemyDefaults->GetCapsuleComponent()
		: nullptr;
	const float CapsuleHalfHeight = DefaultCapsule != nullptr
		? DefaultCapsule->GetScaledCapsuleHalfHeight()
		: 96.0f;
	const FVector SpawnOrigin = SpawnPoint->GetActorLocation();
	const FVector ProjectionExtent(150.0f, 150.0f, 500.0f);
	const bool bAutomatedBenchmark = GetBenchmarkEnemyCount() > 0;
	const float ReuseScale = FMath::Sqrt(static_cast<float>(FMath::Max(SpawnPointReuseCount + 1, 1)));
	const float RetryRadius = FMath::Clamp(
		FMath::Max(ReusedSpawnPointRadius, 300.0f) * ReuseScale,
		300.0f,
		FMath::Max(MaxReusedSpawnPointRadius, 300.0f)
	);
	constexpr int32 MaxSpawnAttempts = 8;

	AfpstrueEnemyCharacter* SpawnedEnemy = nullptr;
	for (int32 Attempt = 0; Attempt < MaxSpawnAttempts && SpawnedEnemy == nullptr; ++Attempt)
	{
		FVector CandidateLocation = SpawnOrigin;
		if ((SpawnPointReuseCount > 0 || Attempt > 0) && RetryRadius > 0.0f)
		{
			FNavLocation NearbyLocation;
			if (!NavSystem->GetRandomReachablePointInRadius(
				SpawnOrigin,
				RetryRadius,
				NearbyLocation))
			{
				continue;
			}
			CandidateLocation = NearbyLocation.Location;
		}

		FNavLocation ProjectedLocation;
		if (!NavSystem->ProjectPointToNavigation(
			CandidateLocation,
			ProjectedLocation,
			ProjectionExtent))
		{
			continue;
		}

		const FVector SpawnLocation = ProjectedLocation.Location
			+ FVector::UpVector * (CapsuleHalfHeight + 2.0f);
		FRotator SpawnRotation = SpawnPoint->GetActorRotation();
		if (PlayerCharacter)
		{
			const FVector ToPlayer = PlayerCharacter->GetActorLocation() - SpawnLocation;
			SpawnRotation = FRotator(0.0f, ToPlayer.Rotation().Yaw, 0.0f);
		}

		const FTransform SpawnTransform(SpawnRotation, SpawnLocation, FVector::OneVector);
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			bAutomatedBenchmark
			? ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn
			: ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

		SpawnedEnemy = World->SpawnActor<AfpstrueEnemyCharacter>(
			WaveEnemyClass,
			SpawnTransform,
			SpawnParameters);
	}

	if (SpawnedEnemy)
	{
		INC_DWORD_STAT(STAT_fpstrueEnemySpawnCount);

		if (SpawnedEnemy->GetController() == nullptr)
		{
			SpawnedEnemy->SpawnDefaultController();
		}

		AfpstrueEnemyAIController* EnemyController =
			Cast<AfpstrueEnemyAIController>(SpawnedEnemy->GetController());
		if (EnemyController != nullptr)
		{
			EnemyController->InitializeCombatContext(
				PlayerCharacter,
				SurroundManager
			);
		}
		else
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("Enemy %s has controller %s; expected fpstrueEnemyAIController."),
				*GetNameSafe(SpawnedEnemy),
				*GetNameSafe(SpawnedEnemy->GetController())
			);
		}

		RegisterEnemy(SpawnedEnemy);
		return true;
	}

	UE_LOG(
		LogTemp,
		Error,
		TEXT("SpawnActor failed for enemy class %s at %s after %d reuse(s), sample radius %.0f."),
		*GetNameSafe(WaveEnemyClass.Get()),
		*GetNameSafe(SpawnPoint),
		SpawnPointReuseCount,
		RetryRadius
	);
	return false;
}

bool AfpstrueGameMode::RegisterEnemy(AfpstrueEnemyCharacter* Enemy)
{
	if (!IsValid(Enemy))
	{
		return false;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredEnemies.Contains(EnemyKey))
	{
		return false;
	}

	RegisteredEnemies.Add(EnemyKey);
	Enemy->OnEnemyDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnDestroyed.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
	AliveEnemyCount = RegisteredEnemies.Num();

	if (bGameRunning)
	{
		OnAliveEnemyCountChanged.Broadcast(AliveEnemyCount);
	}
	return true;
}

bool AfpstrueGameMode::UnregisterEnemy(AfpstrueEnemyCharacter* Enemy, bool bBroadcastCount)
{
	if (Enemy == nullptr)
	{
		return false;
	}

	Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredEnemies.Remove(EnemyKey) == 0)
	{
		return false;
	}

	AliveEnemyCount = RegisteredEnemies.Num();
	if (bBroadcastCount && bGameRunning)
	{
		OnAliveEnemyCountChanged.Broadcast(AliveEnemyCount);
	}
	return true;
}

void AfpstrueGameMode::StopActiveEnemies()
{
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		if (AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get())
		{
			if (AfpstrueEnemyAIController* EnemyController =
				Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
			{
				EnemyController->StopAI();
			}
		}
	}
}

void AfpstrueGameMode::ClearEnemyRegistrations()
{
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		if (AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get())
		{
			Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
			Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
		}
	}

	RegisteredEnemies.Reset();
	AliveEnemyCount = 0;
}

bool AfpstrueGameMode::IsPlayerAlive() const
{
	if (!IsValid(PlayerCharacter))
	{
		return false;
	}

	const UfpstrueHealthComponent* HealthComponent = PlayerCharacter->GetHealthComponent();
	return IsValid(HealthComponent)
		&& HealthComponent->GetHealth() > 0.0f
		&& !HealthComponent->IsDead()
		&& !PlayerCharacter->IsDead();
}

void AfpstrueGameMode::BindPlayerDeathEvent()
{
	if (IsValid(PlayerCharacter))
	{
		PlayerCharacter->OnPlayerDeathReported.AddUniqueDynamic(
			this,
			&AfpstrueGameMode::HandlePlayerDied
		);
	}
}

void AfpstrueGameMode::UnbindPlayerDeathEvent()
{
	if (IsValid(PlayerCharacter))
	{
		PlayerCharacter->OnPlayerDeathReported.RemoveDynamic(
			this,
			&AfpstrueGameMode::HandlePlayerDied
		);
	}
}

void AfpstrueGameMode::UpdateCountdown()
{
	if (!bGameRunning)
	{
		return;
	}

	RemainingTime = FMath::Max(RemainingTime - 1, 0);
	OnRemainingTimeChanged.Broadcast(RemainingTime);

	if (RemainingTime <= 0 && IsPlayerAlive())
	{
		FinishGame(true);
	}
}

void AfpstrueGameMode::HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy)
{
	UnregisterEnemy(DeadEnemy, true);
}

void AfpstrueGameMode::HandleEnemyDestroyed(AActor* DestroyedActor)
{
	UnregisterEnemy(Cast<AfpstrueEnemyCharacter>(DestroyedActor), true);
}

void AfpstrueGameMode::HandlePlayerDied(AfpstrueCharacter* DeadPlayer)
{
	if (bGameRunning && DeadPlayer == PlayerCharacter)
	{
		FinishGame(false);
	}
}

void AfpstrueGameMode::FinishGame(bool bPlayerWon)
{
	if (bGameEnded)
	{
		return;
	}

	bGameEnded = true;
	bGameRunning = false;
	ClearGameplayTimers();
	UnbindPlayerDeathEvent();
	StopActiveEnemies();
	if (SurroundManager)
	{
		SurroundManager->ResetManager();
	}
	UE_LOG(
		LogTemp,
		Log,
		TEXT("Game finished: %s. Remaining time: %d. Player health: %.1f"),
		bPlayerWon ? TEXT("Victory") : TEXT("Defeat"),
		RemainingTime,
		IsValid(PlayerCharacter) ? PlayerCharacter->GetCurrentHealth() : 0.0f
	);
	OnGameResult.Broadcast(bPlayerWon);
}

void AfpstrueGameMode::ClearGameplayTimers()
{
	GetWorldTimerManager().ClearTimer(CountdownTimerHandle);
	GetWorldTimerManager().ClearTimer(WaveTimerHandle);
	GetWorldTimerManager().ClearTimer(EnemySignificanceTimerHandle);
	ClearSpawnQueue();
	GetWorldTimerManager().ClearTimer(BenchmarkReadyTimerHandle);
	GetWorldTimerManager().ClearTimer(BenchmarkStartTimerHandle);
	GetWorldTimerManager().ClearTimer(BenchmarkStopTimerHandle);
	GetWorldTimerManager().ClearTimer(BenchmarkExitTimerHandle);
}

void AfpstrueGameMode::StartEnemySignificanceUpdates()
{
	const bool bDisabledForBenchmark =
		FParse::Param(FCommandLine::Get(), TEXT("BenchmarkDisableEnemySignificance"))
		|| FParse::Param(FCommandLine::Get(), TEXT("BenchmarkDisableEnemyUpdateBudget"));
	if (!bEnableEnemySignificance || bDisabledForBenchmark)
	{
		return;
	}

	GetWorldTimerManager().SetTimer(
		EnemySignificanceTimerHandle,
		this,
		&AfpstrueGameMode::UpdateEnemySignificance,
		FMath::Max(EnemySignificanceUpdateInterval, 0.1f),
		true,
		0.1f
	);
}

void AfpstrueGameMode::UpdateEnemySignificance()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_UpdateEnemySignificance);
	if (!IsValid(PlayerCharacter))
	{
		return;
	}

	USignificanceManager* Manager = USignificanceManager::Get(GetWorld());
	if (Manager == nullptr)
	{
		return;
	}

	TArray<FTransform> Viewpoints;
	Viewpoints.Reserve(1);
	Viewpoints.Add(PlayerCharacter->GetActorTransform());
	Manager->Update(Viewpoints);
}

void AfpstrueGameMode::BeginAutomatedBenchmark()
{
	int32 BenchmarkSeed = 1337;
	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkSeed="), BenchmarkSeed);
	FMath::RandInit(BenchmarkSeed);
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark random seed: %d"), BenchmarkSeed);

	StartGameMode();
	if (!bGameRunning)
	{
		return;
	}

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTarget(PlayerCharacter);
	}
	PlayerCharacter->SetCanBeDamaged(false);
	UWidgetLayoutLibrary::RemoveAllWidgets(this);

	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkWarmup="), AutomatedBenchmarkWarmup);
	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkDuration="), AutomatedBenchmarkDuration);
	AutomatedBenchmarkWarmup = FMath::Max(AutomatedBenchmarkWarmup, 0.0f);
	AutomatedBenchmarkDuration = FMath::Max(AutomatedBenchmarkDuration, 1.0f);

	GetWorldTimerManager().SetTimer(
		BenchmarkReadyTimerHandle,
		this,
		&AfpstrueGameMode::WaitForAutomatedBenchmarkReady,
		0.25f,
		true,
		0.0f
	);
}

void AfpstrueGameMode::WaitForAutomatedBenchmarkReady()
{
	if (!bGameRunning)
	{
		GetWorldTimerManager().ClearTimer(BenchmarkReadyTimerHandle);
		return;
	}

	if (PendingEnemySpawnCount > 0)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(BenchmarkReadyTimerHandle);
	UE_LOG(
		LogTemp,
		Display,
		TEXT("Automated benchmark ready: requested=%d alive=%d warmup=%.1fs"),
		GetBenchmarkEnemyCount(),
		AliveEnemyCount,
		AutomatedBenchmarkWarmup
	);

	GetWorldTimerManager().SetTimer(
		BenchmarkStartTimerHandle,
		this,
		&AfpstrueGameMode::StartAutomatedBenchmarkCapture,
		AutomatedBenchmarkWarmup,
		false
	);
}

void AfpstrueGameMode::StartAutomatedBenchmarkCapture()
{
	ApplyAutomatedBenchmarkDiagnosticOverrides();

	FString BenchmarkTraceFile;
	if (FParse::Value(FCommandLine::Get(), TEXT("BenchmarkTraceFile="), BenchmarkTraceFile) &&
		!BenchmarkTraceFile.IsEmpty())
	{
		BenchmarkTraceFile.TrimQuotesInline();
		const FString TraceCommand = FString::Printf(
			TEXT("Trace.File %s cpu,frame,bookmark,task,stats"),
			*BenchmarkTraceFile
		);
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TraceCommand);
		UKismetSystemLibrary::ExecuteConsoleCommand(
			this,
			TEXT("Trace.RegionBegin AutomatedBenchmarkCapture")
		);
		bAutomatedBenchmarkTraceActive = true;
		UE_LOG(
			LogTemp,
			Display,
			TEXT("Automated benchmark Insights trace started: %s"),
			*BenchmarkTraceFile
		);
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("BenchmarkTextureStats")))
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("DumpTextureStreamingStats"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("ListStreamingTextures"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("MemReport -full"));
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("BenchmarkScreenshot")))
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Shot"));
	}

	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile start"));
	UE_LOG(
		LogTemp,
		Display,
		TEXT("Automated benchmark capture started: requested=%d alive=%d duration=%.1fs"),
		GetBenchmarkEnemyCount(),
		AliveEnemyCount,
		AutomatedBenchmarkDuration
	);

	GetWorldTimerManager().SetTimer(
		BenchmarkStopTimerHandle,
		this,
		&AfpstrueGameMode::StopAutomatedBenchmarkCapture,
		AutomatedBenchmarkDuration,
		false
	);
}

void AfpstrueGameMode::ApplyAutomatedBenchmarkDiagnosticOverrides()
{
	const bool bDisableAttackSweep = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisableAttackSweep")
	);
	const bool bDisablePawnCollision = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisableEnemyPawnCollision")
	);
	const bool bDisablePathFollowingTick = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisablePathFollowingTick")
	);
	const bool bDisableCharacterMovementTick = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisableCharacterMovementTick")
	);
	const bool bDisableSkeletalMeshTick = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisableSkeletalMeshTick")
	);
	const bool bDisableEnemySignificance =
		FParse::Param(FCommandLine::Get(), TEXT("BenchmarkDisableEnemySignificance"))
		|| FParse::Param(FCommandLine::Get(), TEXT("BenchmarkDisableEnemyUpdateBudget"));

	int32 AppliedEnemyCount = 0;
	int32 FullRateMovementCount = 0;
	int32 MidRateMovementCount = 0;
	int32 FarRateMovementCount = 0;
	int32 AttackingEnemyCount = 0;
	int32 ShadowCastingEnemyCount = 0;
	int32 MovementTickEnabledCount = 0;
	int32 SkeletalMeshTickEnabledCount = 0;

	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr)
		{
			continue;
		}

		Enemy->ApplyBenchmarkDiagnosticOverrides(
			bDisableAttackSweep,
			bDisablePawnCollision,
			bDisableCharacterMovementTick
		);

		if (AfpstrueEnemyAIController* EnemyController =
			Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
		{
			EnemyController->ApplyBenchmarkPathFollowingTickOverride(
				bDisablePathFollowingTick
			);
		}

		if (const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			MovementTickEnabledCount += Movement->IsComponentTickEnabled() ? 1 : 0;
			const float TickInterval = Movement->GetComponentTickInterval();
			if (TickInterval <= KINDA_SMALL_NUMBER)
			{
				++FullRateMovementCount;
			}
			else if (TickInterval <= 0.075f)
			{
				++MidRateMovementCount;
			}
			else
			{
				++FarRateMovementCount;
			}
		}

		AttackingEnemyCount += Enemy->IsAttacking() ? 1 : 0;
		if (USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh())
		{
			if (bDisableSkeletalMeshTick)
			{
				CharacterMesh->SetComponentTickEnabled(false);
			}
			SkeletalMeshTickEnabledCount +=
				CharacterMesh->IsComponentTickEnabled() ? 1 : 0;
			ShadowCastingEnemyCount += CharacterMesh->CastShadow ? 1 : 0;
		}
		++AppliedEnemyCount;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("Benchmark diagnostics applied: enemies=%d attackSweepOff=%d pawnCollisionOff=%d pathFollowingTickOff=%d characterMovementTickOff=%d skeletalMeshTickOff=%d significanceOff=%d"),
		AppliedEnemyCount,
		bDisableAttackSweep ? 1 : 0,
		bDisablePawnCollision ? 1 : 0,
		bDisablePathFollowingTick ? 1 : 0,
		bDisableCharacterMovementTick ? 1 : 0,
		bDisableSkeletalMeshTick ? 1 : 0,
		bDisableEnemySignificance ? 1 : 0
	);
	UE_LOG(
		LogTemp,
		Display,
		TEXT("Benchmark enemy snapshot: movementFull=%d movementMid=%d movementFar=%d movementTickEnabled=%d skeletalMeshTickEnabled=%d attacking=%d castingShadow=%d"),
		FullRateMovementCount,
		MidRateMovementCount,
		FarRateMovementCount,
		MovementTickEnabledCount,
		SkeletalMeshTickEnabledCount,
		AttackingEnemyCount,
		ShadowCastingEnemyCount
	);
}

void AfpstrueGameMode::StopAutomatedBenchmarkCapture()
{
	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile stop"));
	if (bAutomatedBenchmarkTraceActive)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(
			this,
			TEXT("Trace.RegionEnd AutomatedBenchmarkCapture")
		);
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.Stop"));
		bAutomatedBenchmarkTraceActive = false;
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace stopped."));
	}
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture stopped."));

	if (FParse::Param(FCommandLine::Get(), TEXT("BenchmarkAutoQuit")))
	{
		GetWorldTimerManager().SetTimer(
			BenchmarkExitTimerHandle,
			this,
			&AfpstrueGameMode::ExitAutomatedBenchmark,
			1.0f,
			false
		);
	}
}

void AfpstrueGameMode::ExitAutomatedBenchmark()
{
	UKismetSystemLibrary::QuitGame(
		this,
		UGameplayStatics::GetPlayerController(this, 0),
		EQuitPreference::Quit,
		false
	);
}

int32 AfpstrueGameMode::GetBenchmarkEnemyCount() const
{
	int32 BenchmarkEnemyCount = 0;
	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkEnemies="), BenchmarkEnemyCount);
	return FMath::Max(BenchmarkEnemyCount, 0);
}
