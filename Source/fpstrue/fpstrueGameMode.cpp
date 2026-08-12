// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueGameMode.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstruePerformanceStats.h"
#include "fpstrueSurroundManager.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_STAT(STAT_fpstrueWaveSpawnTime);
DEFINE_STAT(STAT_fpstrueEnemySpawnCount);

AfpstrueGameMode::AfpstrueGameMode()
	: Super()
{
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnClassFinder(TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter"));
	DefaultPawnClass = PlayerPawnClassFinder.Class;
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

	PlayerCharacter->GetHealthComponent()->OnDeath.AddUniqueDynamic(this, &AfpstrueGameMode::HandlePlayerDied);

	if (!CreateSurroundManager())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: SurroundManager could not be created."));
		FinishGame(false);
		return;
	}

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
}

void AfpstrueGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearGameplayTimers();

	if (PlayerCharacter && PlayerCharacter->GetHealthComponent())
	{
		PlayerCharacter->GetHealthComponent()->OnDeath.RemoveDynamic(this, &AfpstrueGameMode::HandlePlayerDied);
	}

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

	++CurrentWave;
	OnWaveChanged.Broadcast(CurrentWave, ConfiguredWaveCount);
	UE_LOG(
		LogTemp,
		Warning,
		TEXT("Wave %d/%d started. Enemies to spawn: %d"),
		CurrentWave,
		ConfiguredWaveCount,
		GetEnemyCountForWave(CurrentWave)
	);

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

	TArray<AActor*> ShuffledSpawnPoints = SpawnPoints;
	for (int32 Index = ShuffledSpawnPoints.Num() - 1; Index > 0; --Index)
	{
		const int32 SwapIndex = FMath::RandRange(0, Index);
		ShuffledSpawnPoints.Swap(Index, SwapIndex);
	}

	const int32 EnemiesThisWave = GetEnemyCountForWave(CurrentWave);
	const TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass = GetEnemyClassForWave(CurrentWave);
	for (int32 EnemyIndex = 0; EnemyIndex < EnemiesThisWave; ++EnemyIndex)
	{
		const int32 SpawnPointIndex = EnemyIndex % ShuffledSpawnPoints.Num();
		const bool bUseNearbyLocation = EnemyIndex >= ShuffledSpawnPoints.Num();
		SpawnEnemyAtPoint(ShuffledSpawnPoints[SpawnPointIndex], bUseNearbyLocation, WaveEnemyClass);
	}
}

void AfpstrueGameMode::SpawnEnemyAtPoint(
	AActor* SpawnPoint,
	bool bUseNearbyLocation,
	TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass
)
{
	if (!IsValid(SpawnPoint))
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy spawn point is invalid."));
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	FVector SpawnLocation = SpawnPoint->GetActorLocation();
	if (bUseNearbyLocation && ReusedSpawnPointRadius > 0.0f)
	{
		if (UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
		{
			FNavLocation NearbyLocation;
			if (NavSystem->GetRandomReachablePointInRadius(SpawnLocation, ReusedSpawnPointRadius, NearbyLocation))
			{
				SpawnLocation = NearbyLocation.Location;
			}
		}
	}

	FRotator SpawnRotation = SpawnPoint->GetActorRotation();
	if (PlayerCharacter)
	{
		const FVector ToPlayer = PlayerCharacter->GetActorLocation() - SpawnLocation;
		SpawnRotation = FRotator(0.0f, ToPlayer.Rotation().Yaw, 0.0f);
	}

	const FTransform SpawnTransform(SpawnRotation, SpawnLocation, FVector::OneVector);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AfpstrueEnemyCharacter* SpawnedEnemy = World->SpawnActor<AfpstrueEnemyCharacter>(
		WaveEnemyClass,
		SpawnTransform,
		SpawnParameters
	);

	if (SpawnedEnemy)
	{
		INC_DWORD_STAT(STAT_fpstrueEnemySpawnCount);

		if (AfpstrueEnemyAIController* EnemyController =
			Cast<AfpstrueEnemyAIController>(SpawnedEnemy->GetController()))
		{
			EnemyController->InitializeCombatContext(
				PlayerCharacter,
				SurroundManager
			);
		}

		SpawnedEnemy->OnEnemyDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
		++AliveEnemyCount;
		OnAliveEnemyCountChanged.Broadcast(AliveEnemyCount);
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("Enemy spawned: %s at %s. Alive enemies: %d"),
			*GetNameSafe(SpawnedEnemy),
			*SpawnLocation.ToString(),
			AliveEnemyCount
		);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("SpawnActor failed for enemy class %s"), *GetNameSafe(WaveEnemyClass.Get()));
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

	if (RemainingTime <= 0)
	{
		const bool bPlayerIsAlive = PlayerCharacter && !PlayerCharacter->IsDead();
		FinishGame(bPlayerIsAlive);
	}
}

void AfpstrueGameMode::HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy)
{
	if (DeadEnemy)
	{
		DeadEnemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	}

	if (!bGameRunning)
	{
		return;
	}

	AliveEnemyCount = FMath::Max(AliveEnemyCount - 1, 0);
	OnAliveEnemyCountChanged.Broadcast(AliveEnemyCount);
}

void AfpstrueGameMode::HandlePlayerDied()
{
	FinishGame(false);
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
	if (SurroundManager)
	{
		SurroundManager->ResetManager();
	}
	OnGameResult.Broadcast(bPlayerWon);
}

void AfpstrueGameMode::ClearGameplayTimers()
{
	GetWorldTimerManager().ClearTimer(CountdownTimerHandle);
	GetWorldTimerManager().ClearTimer(WaveTimerHandle);
	GetWorldTimerManager().ClearTimer(BenchmarkStartTimerHandle);
	GetWorldTimerManager().ClearTimer(BenchmarkStopTimerHandle);
}

// TODO: Move benchmark orchestration to a development-only runner.
void AfpstrueGameMode::BeginAutomatedBenchmark()
{
	StartGameMode();
	if (!bGameRunning)
	{
		return;
	}

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTarget(PlayerCharacter);
	}
	UWidgetLayoutLibrary::RemoveAllWidgets(this);

	float WarmupSeconds = 8.0f;
	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkWarmup="), WarmupSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkDuration="), AutomatedBenchmarkDuration);
	WarmupSeconds = FMath::Max(WarmupSeconds, 0.0f);
	AutomatedBenchmarkDuration = FMath::Max(AutomatedBenchmarkDuration, 1.0f);

	GetWorldTimerManager().SetTimer(
		BenchmarkStartTimerHandle,
		this,
		&AfpstrueGameMode::StartAutomatedBenchmarkCapture,
		WarmupSeconds,
		false
	);
}

void AfpstrueGameMode::StartAutomatedBenchmarkCapture()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("BenchmarkTextureStats")))
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("DumpTextureStreamingStats"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("ListStreamingTextures"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("MemReport -full"));
	}

	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile start"));
	UE_LOG(
		LogTemp,
		Display,
		TEXT("Automated benchmark capture started: enemies=%d duration=%.1fs"),
		GetBenchmarkEnemyCount(),
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

void AfpstrueGameMode::StopAutomatedBenchmarkCapture()
{
	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile stop"));
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture stopped."));
}

int32 AfpstrueGameMode::GetBenchmarkEnemyCount() const
{
	int32 BenchmarkEnemyCount = 0;
	FParse::Value(FCommandLine::Get(), TEXT("BenchmarkEnemies="), BenchmarkEnemyCount);
	return FMath::Max(BenchmarkEnemyCount, 0);
}
