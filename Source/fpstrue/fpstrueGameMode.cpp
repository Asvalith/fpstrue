// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueGameMode.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueBenchmarkRunner.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyAnimationSharingCoordinator.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueEnemySignificanceCoordinator.h"
#include "fpstruePerformanceStats.h"
#include "fpstrueSurroundManager.h"
#include "Components/CapsuleComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_STAT(STAT_fpstrueWaveSpawnTime);
DEFINE_STAT(STAT_fpstrueEnemySpawnCount);

// ==================== 生命周期与开局 ====================

AfpstrueGameMode::AfpstrueGameMode()
{
	SurroundManagerClass = AfpstrueSurroundManager::StaticClass();
	BenchmarkRunner = CreateDefaultSubobject<UfpstrueBenchmarkRunner>(TEXT("BenchmarkRunner"));
	EnemySignificanceCoordinator = CreateDefaultSubobject<UfpstrueEnemySignificanceCoordinator>(TEXT("EnemySignificanceCoordinator"));
	EnemyAnimationSharingCoordinator =
		CreateDefaultSubobject<UfpstrueEnemyAnimationSharingCoordinator>(TEXT("EnemyAnimationSharingCoordinator"));
}

void AfpstrueGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (BenchmarkRunner != nullptr)
	{
		BenchmarkRunner->StartIfRequested(this);
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
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: found %d spawn points, but at least %d are required."), SpawnPoints.Num(),
			   MinimumSpawnPointCount);
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

	bGameRunning = true;
	CurrentWave = 0;
	RemainingTime = GameDuration;

	OnRemainingTimeChanged.Broadcast(RemainingTime);
	OnWaveChanged.Broadcast(CurrentWave, GetConfiguredWaveCount());
	OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());

	GetWorldTimerManager().SetTimer(CountdownTimerHandle, this, &AfpstrueGameMode::UpdateCountdown, 1.0f, true);

	// 共享管理器必须在首个敌人生成前就绪；敌人实际是否加入仍由 Render Significance 决定。
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->Start(GetEnemyClassForWave(1));
	}
	if (EnemySignificanceCoordinator != nullptr)
	{
		EnemySignificanceCoordinator->Start(this);
	}
	StartNextWave();
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

// ==================== 波次、生成与共享场景资源 ====================

void AfpstrueGameMode::CacheSpawnPoints()
{
	SpawnPoints.Reset();
	UGameplayStatics::GetAllActorsOfClassWithTag(this, ATargetPoint::StaticClass(), EnemySpawnTag, SpawnPoints);
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
		GetWorldTimerManager().SetTimer(WaveTimerHandle, this, &AfpstrueGameMode::StartNextWave, FMath::Max(SpawnInterval, 0.01f), false);
		return;
	}

	++CurrentWave;
	OnWaveChanged.Broadcast(CurrentWave, ConfiguredWaveCount);

	SpawnCurrentWave();

	if (CurrentWave < ConfiguredWaveCount)
	{
		GetWorldTimerManager().SetTimer(WaveTimerHandle, this, &AfpstrueGameMode::StartNextWave, WaveInterval, false);
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

	SurroundManager = World->SpawnActor<AfpstrueSurroundManager>(SurroundManagerClass, FVector::ZeroVector, FRotator::ZeroRotator);

	if (!IsValid(SurroundManager))
	{
		return false;
	}

	// GameMode 负责共享目标的生命周期，避免每个 AIController 重复写入同一状态。
	SurroundManager->SetTargetCharacter(PlayerCharacter);
	return true;
}

int32 AfpstrueGameMode::GetConfiguredWaveCount() const
{
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.HasEnemyCountOverride())
	{
		return 1;
	}

	return WaveConfigs.IsEmpty() ? TotalWaves : WaveConfigs.Num();
}

int32 AfpstrueGameMode::GetEnemyCountForWave(int32 WaveNumber) const
{
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.HasEnemyCountOverride())
	{
		return BenchmarkConfig.EnemyCount;
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
		GetWorldTimerManager().SetTimer(SpawnTimerHandle, this, &AfpstrueGameMode::SpawnNextQueuedEnemy, FMath::Max(SpawnInterval, 0.01f),
										true);
	}
}

void AfpstrueGameMode::SpawnNextQueuedEnemy()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnQueuedEnemy);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);

	if (!bGameRunning || PendingEnemySpawnCount <= 0 || QueuedSpawnPoints.IsEmpty() || !QueuedEnemyClass)
	{
		ClearSpawnQueue();
		return;
	}

	const int32 SpawnPointCount = QueuedSpawnPoints.Num();
	const int32 SpawnPointIndex = NextQueuedSpawnIndex % SpawnPointCount;
	const int32 SpawnPointReuseCount = NextQueuedSpawnIndex / SpawnPointCount;
	const bool bSpawnSucceeded = SpawnEnemyAtPoint(QueuedSpawnPoints[SpawnPointIndex], SpawnPointReuseCount, QueuedEnemyClass);

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
			UE_LOG(LogTemp, Error, TEXT("Enemy spawn queue stopped after %d consecutive failures with %d enemies remaining."),
				   ConsecutiveSpawnFailureCount, PendingEnemySpawnCount);
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

bool AfpstrueGameMode::SpawnEnemyAtPoint(AActor* SpawnPoint, int32 SpawnPointReuseCount, TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass)
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
	const UCapsuleComponent* DefaultCapsule = EnemyDefaults != nullptr ? EnemyDefaults->GetCapsuleComponent() : nullptr;
	const float CapsuleHalfHeight = DefaultCapsule != nullptr ? DefaultCapsule->GetScaledCapsuleHalfHeight() : 96.0f;
	const FVector SpawnOrigin = SpawnPoint->GetActorLocation();
	const FVector ProjectionExtent(150.0f, 150.0f, 500.0f);
	const bool bAutomatedBenchmark = FFPBenchmarkConfig::Get().HasEnemyCountOverride();
	const float ReuseScale = FMath::Sqrt(static_cast<float>(FMath::Max(SpawnPointReuseCount + 1, 1)));
	const float RetryRadius =
		FMath::Clamp(FMath::Max(ReusedSpawnPointRadius, 300.0f) * ReuseScale, 300.0f, FMath::Max(MaxReusedSpawnPointRadius, 300.0f));
	constexpr int32 MaxSpawnAttempts = 8;

	AfpstrueEnemyCharacter* SpawnedEnemy = nullptr;
	for (int32 Attempt = 0; Attempt < MaxSpawnAttempts && SpawnedEnemy == nullptr; ++Attempt)
	{
		FNavLocation ProjectedLocation;
		if ((SpawnPointReuseCount > 0 || Attempt > 0) && RetryRadius > 0.0f)
		{
			// 随机可达点已经携带有效 NavMesh Poly，不再重复 ProjectPointToNavigation。
			if (!NavSystem->GetRandomReachablePointInRadius(SpawnOrigin, RetryRadius, ProjectedLocation))
			{
				continue;
			}
		}
		else if (!NavSystem->ProjectPointToNavigation(SpawnOrigin, ProjectedLocation, ProjectionExtent))
		{
			continue;
		}

		const FVector SpawnLocation = ProjectedLocation.Location + FVector::UpVector * (CapsuleHalfHeight + 2.0f);
		FRotator SpawnRotation = SpawnPoint->GetActorRotation();
		if (PlayerCharacter)
		{
			const FVector ToPlayer = PlayerCharacter->GetActorLocation() - SpawnLocation;
			SpawnRotation = FRotator(0.0f, ToPlayer.Rotation().Yaw, 0.0f);
		}

		const FTransform SpawnTransform(SpawnRotation, SpawnLocation, FVector::OneVector);
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = bAutomatedBenchmark
															 ? ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn
															 : ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

		SpawnedEnemy = World->SpawnActor<AfpstrueEnemyCharacter>(WaveEnemyClass, SpawnTransform, SpawnParameters);
	}

	if (SpawnedEnemy)
	{
		INC_DWORD_STAT(STAT_fpstrueEnemySpawnCount);

		if (SpawnedEnemy->GetController() == nullptr)
		{
			SpawnedEnemy->SpawnDefaultController();
		}

		AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(SpawnedEnemy->GetController());
		if (EnemyController != nullptr)
		{
			EnemyController->InitializeCombatContext(PlayerCharacter, SurroundManager);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Enemy %s has controller %s; expected fpstrueEnemyAIController."), *GetNameSafe(SpawnedEnemy),
				   *GetNameSafe(SpawnedEnemy->GetController()));
		}

		RegisterEnemy(SpawnedEnemy);
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("SpawnActor failed for enemy class %s at %s after %d reuse(s), sample radius %.0f."),
		   *GetNameSafe(WaveEnemyClass.Get()), *GetNameSafe(SpawnPoint), SpawnPointReuseCount, RetryRadius);
	return false;
}

// ==================== 敌人注册表与协调器连接 ====================

void AfpstrueGameMode::RegisterEnemy(AfpstrueEnemyCharacter* Enemy)
{
	if (!IsValid(Enemy))
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredEnemies.Contains(EnemyKey))
	{
		return;
	}

	RegisteredEnemies.Add(EnemyKey);
	Enemy->SetAnimationSharingCoordinator(EnemyAnimationSharingCoordinator);
	Enemy->OnEnemyDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnDestroyed.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);

	if (bGameRunning)
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::UnregisterEnemy(AfpstrueEnemyCharacter* Enemy, bool bBroadcastCount)
{
	if (Enemy == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredEnemies.Remove(EnemyKey) == 0)
	{
		return;
	}

	Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->SuspendEnemy(Enemy);
	}
	Enemy->SetAnimationSharingCoordinator(nullptr);

	if (bBroadcastCount && bGameRunning)
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::StopActiveEnemies()
{
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		if (AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get())
		{
			if (AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
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
			if (EnemyAnimationSharingCoordinator != nullptr)
			{
				EnemyAnimationSharingCoordinator->SuspendEnemy(Enemy);
			}
			Enemy->SetAnimationSharingCoordinator(nullptr);
			Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
			Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
		}
	}

	RegisteredEnemies.Reset();
}

// ==================== 游戏状态、事件与计时器 ====================

bool AfpstrueGameMode::IsPlayerAlive() const
{
	if (!IsValid(PlayerCharacter))
	{
		return false;
	}

	return !PlayerCharacter->IsDead();
}

void AfpstrueGameMode::BindPlayerDeathEvent()
{
	if (IsValid(PlayerCharacter))
	{
		PlayerCharacter->OnPlayerDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandlePlayerDied);
	}
}

void AfpstrueGameMode::UnbindPlayerDeathEvent()
{
	if (IsValid(PlayerCharacter))
	{
		PlayerCharacter->OnPlayerDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandlePlayerDied);
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
	UE_LOG(LogTemp, Log, TEXT("Game finished: %s. Remaining time: %d. Player health: %.1f"), bPlayerWon ? TEXT("Victory") : TEXT("Defeat"),
		   RemainingTime, IsValid(PlayerCharacter) ? PlayerCharacter->GetCurrentHealth() : 0.0f);
	OnGameResult.Broadcast(bPlayerWon);
}

void AfpstrueGameMode::ClearGameplayTimers()
{
	GetWorldTimerManager().ClearTimer(CountdownTimerHandle);
	GetWorldTimerManager().ClearTimer(WaveTimerHandle);
	ClearSpawnQueue();
	if (EnemySignificanceCoordinator != nullptr)
	{
		EnemySignificanceCoordinator->Stop();
	}
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->Stop();
	}
	if (BenchmarkRunner != nullptr)
	{
		BenchmarkRunner->Cancel();
	}
}
