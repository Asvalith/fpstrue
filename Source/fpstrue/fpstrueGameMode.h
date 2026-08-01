// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "fpstrueGameMode.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;

USTRUCT(BlueprintType)
struct FfpstrueWaveConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave")
	TSubclassOf<AfpstrueEnemyCharacter> EnemyClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave", meta = (ClampMin = "1"))
	int32 EnemyCount = 5;
};

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

	UFUNCTION(BlueprintCallable, Category = "Game", meta = (DisplayName = "Start GameMode"))
	void StartGameMode();

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
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn")
	TSubclassOf<AfpstrueEnemyCharacter> EnemyClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn")
	FName EnemySpawnTag = TEXT("EnemySpawn");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|AI")
	TSubclassOf<AfpstrueSurroundManager> SurroundManagerClass;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Game|AI")
	AfpstrueSurroundManager* SurroundManager = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "1"))
	int32 MinimumSpawnPointCount = 4;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "1"))
	int32 TotalWaves = 3;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "1"))
	int32 BaseEnemiesPerWave = 5;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "0"))
	int32 EnemiesAddedPerWave = 2;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave")
	TArray<FfpstrueWaveConfig> WaveConfigs;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (ClampMin = "0.0"))
	float WaveInterval = 5.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "0.0"))
	float ReusedSpawnPointRadius = 300.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Time", meta = (ClampMin = "1"))
	int32 GameDuration = 90;

private:
	void CacheSpawnPoints();
	bool CreateSurroundManager();
	void StartNextWave();
	int32 GetConfiguredWaveCount() const;
	int32 GetEnemyCountForWave(int32 WaveNumber) const;
	TSubclassOf<AfpstrueEnemyCharacter> GetEnemyClassForWave(int32 WaveNumber) const;
	void SpawnCurrentWave();
	void SpawnEnemyAtPoint(AActor* SpawnPoint, bool bUseNearbyLocation, TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass);
	void UpdateCountdown();
	void FinishGame(bool bPlayerWon);
	void ClearGameplayTimers();
	void BeginAutomatedBenchmark();
	void StartAutomatedBenchmarkCapture();
	void StopAutomatedBenchmarkCapture();
	int32 GetBenchmarkEnemyCount() const;

	UFUNCTION()
	void HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy);

	UFUNCTION()
	void HandlePlayerDied();

	UPROPERTY()
	TArray<AActor*> SpawnPoints;

	UPROPERTY()
	AfpstrueCharacter* PlayerCharacter = nullptr;

	int32 CurrentWave = 0;
	int32 AliveEnemyCount = 0;
	int32 RemainingTime = 0;
	bool bGameRunning = false;
	bool bGameEnded = false;

	FTimerHandle CountdownTimerHandle;
	FTimerHandle WaveTimerHandle;
	FTimerHandle BenchmarkStartTimerHandle;
	FTimerHandle BenchmarkStopTimerHandle;
	float AutomatedBenchmarkDuration = 30.0f;
};



