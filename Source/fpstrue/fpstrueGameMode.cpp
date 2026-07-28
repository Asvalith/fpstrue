// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueGameMode.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueHealthComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"

AfpstrueGameMode::AfpstrueGameMode()
	: Super()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnClassFinder(TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter"));
	DefaultPawnClass = PlayerPawnClassFinder.Class;

}

void AfpstrueGameMode::StartGame()
{
	if (bGameRunning || bGameEnded)
	{
		return;
	}

	CacheSpawnPoints();

	if (!EnemyClass)
	{
		UE_LOG(LogTemp, Error, TEXT("StartGame failed: EnemyClass is not configured."));
		FinishGame(false);
		return;
	}

	if (SpawnPoints.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGame failed: no TargetPoint has the tag '%s'."), *EnemySpawnTag.ToString());
		FinishGame(false);
		return;
	}

	PlayerCharacter = Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
	if (!PlayerCharacter || !PlayerCharacter->GetHealthComponent())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGame failed: player or HealthComponent is invalid."));
		FinishGame(false);
		return;
	}

	PlayerCharacter->GetHealthComponent()->OnDeath.AddUniqueDynamic(this, &AfpstrueGameMode::HandlePlayerDied);

	bGameRunning = true;
	CurrentWave = 0;
	EnemiesLeftToSpawn = 0;
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
	EnemiesLeftToSpawn = BaseEnemiesPerWave + (CurrentWave - 1) * EnemiesAddedPerWave;
	OnWaveChanged.Broadcast(CurrentWave, TotalWaves);

	SpawnNextEnemy();
}

void AfpstrueGameMode::SpawnNextEnemy()
{
	if (!bGameRunning || EnemiesLeftToSpawn <= 0 || SpawnPoints.IsEmpty())
	{
		return;
	}

	const int32 SpawnPointIndex = FMath::RandRange(0, SpawnPoints.Num() - 1);
	AActor* SpawnPoint = SpawnPoints[SpawnPointIndex];

	if (IsValid(SpawnPoint))
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

		AfpstrueEnemyCharacter* SpawnedEnemy = GetWorld()->SpawnActor<AfpstrueEnemyCharacter>(
			EnemyClass,
			SpawnPoint->GetActorTransform(),
			SpawnParameters
		);

		if (SpawnedEnemy)
		{
			SpawnedEnemy->OnEnemyDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
			++AliveEnemyCount;
			OnAliveEnemyCountChanged.Broadcast(AliveEnemyCount);
		}
	}

	--EnemiesLeftToSpawn;

	if (EnemiesLeftToSpawn > 0)
	{
		GetWorldTimerManager().SetTimer(
			EnemySpawnTimerHandle,
			this,
			&AfpstrueGameMode::SpawnNextEnemy,
			EnemySpawnInterval,
			false
		);
	}
	else if (CurrentWave < TotalWaves)
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
	OnGameResult.Broadcast(bPlayerWon);
}

void AfpstrueGameMode::ClearGameplayTimers()
{
	GetWorldTimerManager().ClearTimer(CountdownTimerHandle);
	GetWorldTimerManager().ClearTimer(EnemySpawnTimerHandle);
	GetWorldTimerManager().ClearTimer(WaveTimerHandle);
}
