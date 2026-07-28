// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "fpstrueGameMode.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRemainingTimeChanged, int32, RemainingTime);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWaveChanged, int32, CurrentWave, int32, TotalWaves);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAliveEnemyCountChanged, int32, AliveEnemyCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameResult, bool, bPlayerWon);

UCLASS()
class FPSTRUE_API AfpstrueGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AfpstrueGameMode();

	UFUNCTION(BlueprintCallable, Category = "Game")
	void StartGame();

	UFUNCTION(BlueprintPure, Category = "Game")
	int32 GetRemainingTime() const { return RemainingTime; }

	UFUNCTION(BlueprintPure, Category = "Game")
	int32 GetCurrentWave() const { return CurrentWave; }

	UFUNCTION(BlueprintPure, Category = "Game")
	int32 GetAliveEnemyCount() const { return AliveEnemyCount; }

	UFUNCTION(BlueprintPure, Category = "Game")
	bool IsGameRunning() const { return bGameRunning; }

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnRemainingTimeChanged OnRemainingTimeChanged;

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnWaveChanged OnWaveChanged;

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnAliveEnemyCountChanged OnAliveEnemyCountChanged;

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnGameResult OnGameResult;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn")
	TSubclassOf<AfpstrueEnemyCharacter> EnemyClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn")
	FName EnemySpawnTag = TEXT("EnemySpawn");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "1"))
	int32 TotalWaves = 3;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "1"))
	int32 BaseEnemiesPerWave = 5;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "0"))
	int32 EnemiesAddedPerWave = 2;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "0.1"))
	float EnemySpawnInterval = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "0.0"))
	float WaveInterval = 10.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Time", meta = (ClampMin = "1"))
	int32 GameDuration = 180;

private:
	void CacheSpawnPoints();
	void StartNextWave();
	void SpawnNextEnemy();
	void UpdateCountdown();
	void FinishGame(bool bPlayerWon);
	void ClearGameplayTimers();

	UFUNCTION()
	void HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy);

	UFUNCTION()
	void HandlePlayerDied();

	UPROPERTY()
	TArray<AActor*> SpawnPoints;

	UPROPERTY()
	AfpstrueCharacter* PlayerCharacter = nullptr;

	int32 CurrentWave = 0;
	int32 EnemiesLeftToSpawn = 0;
	int32 AliveEnemyCount = 0;
	int32 RemainingTime = 0;
	bool bGameRunning = false;
	bool bGameEnded = false;

	FTimerHandle CountdownTimerHandle;
	FTimerHandle EnemySpawnTimerHandle;
	FTimerHandle WaveTimerHandle;
};



