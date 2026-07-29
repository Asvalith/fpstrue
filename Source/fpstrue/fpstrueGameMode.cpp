// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueGameMode.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstrueSurroundManager.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "UObject/ConstructorHelpers.h"

AfpstrueGameMode::AfpstrueGameMode()
	: Super()
{
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnClassFinder(TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter"));
	DefaultPawnClass = PlayerPawnClassFinder.Class;
	SurroundManagerClass = AfpstrueSurroundManager::StaticClass();
}

void AfpstrueGameMode::StartGameMode()
{
	if (bGameRunning || bGameEnded)
	{
		return;
	}

	CacheSpawnPoints();

	if (!EnemyClass)
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
	OnWaveChanged.Broadcast(CurrentWave, TotalWaves);
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
	if (!bGameRunning || CurrentWave >= TotalWaves)
	{
		return;
	}

	++CurrentWave;
	OnWaveChanged.Broadcast(CurrentWave, TotalWaves);
	UE_LOG(
		LogTemp,
		Warning,
		TEXT("Wave %d/%d started. Enemies to spawn: %d"),
		CurrentWave,
		TotalWaves,
		BaseEnemiesPerWave + (CurrentWave - 1) * EnemiesAddedPerWave
	);

	SpawnCurrentWave();

	if (CurrentWave < TotalWaves)
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

void AfpstrueGameMode::SpawnCurrentWave()
{
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

	const int32 EnemiesThisWave = BaseEnemiesPerWave + (CurrentWave - 1) * EnemiesAddedPerWave;
	for (int32 EnemyIndex = 0; EnemyIndex < EnemiesThisWave; ++EnemyIndex)
	{
		const int32 SpawnPointIndex = EnemyIndex % ShuffledSpawnPoints.Num();
		const bool bUseNearbyLocation = EnemyIndex >= ShuffledSpawnPoints.Num();
		SpawnEnemyAtPoint(ShuffledSpawnPoints[SpawnPointIndex], bUseNearbyLocation);
	}
}

void AfpstrueGameMode::SpawnEnemyAtPoint(AActor* SpawnPoint, bool bUseNearbyLocation)
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
		EnemyClass,
		SpawnTransform,
		SpawnParameters
	);

	if (SpawnedEnemy)
	{
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
		UE_LOG(LogTemp, Error, TEXT("SpawnActor failed for enemy class %s"), *GetNameSafe(EnemyClass.Get()));
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
}
