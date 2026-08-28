// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "fpstrueEnemySignificance.h"
#include "fpstrueGameMode.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;
class UfpstrueBenchmarkRunner;
class UfpstrueEnemyAnimationSharingCoordinator;
class UfpstrueEnemySignificanceCoordinator;

// 单波敌人类型与数量配置，由 GameMode 的生成队列读取。
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

/** 游戏流程总协调器：管理波次、敌人注册表、倒计时，并持有 AI/性能共享模块。 */
UCLASS()
class FPSTRUE_API AfpstrueGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	// 创建 Benchmark、Significance 和 Animation Sharing 子组件。
	AfpstrueGameMode();

	// ==================== 游戏流程与查询 ====================

	// 由关卡或 UI 启动正式游戏流程，完成校验后进入第一波。
	UFUNCTION(BlueprintCallable, Category = "Game", meta = (DisplayName = "Start GameMode"))
	void StartGameMode();

	// HUD 初始化时读取当前剩余秒数；后续变化通过 Delegate 推送。
	UFUNCTION(BlueprintPure, Category = "Game")
	int32 GetRemainingTime() const { return RemainingTime; }

	// ==================== 对外事件 ====================

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnRemainingTimeChanged OnRemainingTimeChanged;

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnWaveChanged OnWaveChanged;

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnAliveEnemyCountChanged OnAliveEnemyCountChanged;

	UPROPERTY(BlueprintAssignable, Category = "Game|Events")
	FOnGameResult OnGameResult;

protected:
	// ==================== Actor 生命周期 ====================

	// 关卡开始时按命令行决定是否启动自动 Benchmark。
	virtual void BeginPlay() override;
	// 关卡退出时停止敌人、Timer 和所有共享协调器。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ==================== 生成、波次与共享 AI 场景配置 ====================

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

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "300.0"))
	float MaxReusedSpawnPointRadius = 2000.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "0.01"))
	float SpawnInterval = 0.05f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Time", meta = (ClampMin = "1"))
	int32 GameDuration = 90;

	// ==================== 性能策略配置 ====================

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Performance|Game Thread")
	bool bEnableEnemySignificance = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Performance|Game Thread", meta = (ClampMin = "0.1"))
	float EnemySignificanceUpdateInterval = 0.25f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Performance|Significance")
	FFPEnemyRenderSignificancePolicy EnemyRenderSignificancePolicy;

private:
	// ==================== 受控协作者 ====================

	friend class UfpstrueBenchmarkRunner;
	friend class UfpstrueEnemyAnimationSharingCoordinator;
	friend class UfpstrueEnemySignificanceCoordinator;

	// ==================== 波次与生成 ====================

	// 查找带指定 Tag 的 TargetPoint，供所有波次复用。
	void CacheSpawnPoints();
	// 创建全局 SurroundManager，并注入当前玩家目标。
	bool CreateSurroundManager();
	// 推进波次编号、广播 UI 事件并启动本波生成。
	void StartNextWave();
	// 返回正常配置或 Benchmark 覆盖后的总波数。
	int32 GetConfiguredWaveCount() const;
	// 返回指定波次应生成的敌人数。
	int32 GetEnemyCountForWave(int32 WaveNumber) const;
	// 返回指定波次使用的敌人类，未覆盖时回退到默认类。
	TSubclassOf<AfpstrueEnemyCharacter> GetEnemyClassForWave(int32 WaveNumber) const;
	// 初始化本波的分帧生成队列。
	void SpawnCurrentWave();
	// Timer 每次只生成一个敌人，降低集中 Spawn 峰值。
	void SpawnNextQueuedEnemy();
	// 停止生成 Timer 并清空待生成状态。
	void ClearSpawnQueue();
	// 在给定出生点附近寻找可导航位置，生成敌人并注入 AI 上下文。
	bool SpawnEnemyAtPoint(AActor* SpawnPoint, int32 SpawnPointReuseCount, TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass);

	// ==================== 敌人注册表 ====================

	// 把新敌人加入唯一注册表，并连接死亡事件和动画共享协调器。
	bool RegisterEnemy(AfpstrueEnemyCharacter* Enemy);
	// 从注册表和共享系统移除敌人，并按需通知 HUD 数量变化。
	bool UnregisterEnemy(AfpstrueEnemyCharacter* Enemy, bool bBroadcastCount);
	// 游戏结束时停止所有仍存活敌人的 AI。
	void StopActiveEnemies();
	// 解除全部敌人事件、共享引用并清空注册表。
	void ClearEnemyRegistrations();

	// ==================== 游戏状态与计时器 ====================

	// 通过玩家复用的 HealthComponent 判断游戏是否仍可继续。
	bool IsPlayerAlive() const;
	// 订阅玩家死亡事件，使 GameMode 能结束对局。
	void BindPlayerDeathEvent();
	// 退出或结算时解除玩家死亡事件。
	void UnbindPlayerDeathEvent();
	// 每秒减少倒计时，并向 HUD 广播最新值。
	void UpdateCountdown();
	// 只执行一次胜负结算，停止 AI 并广播结果。
	void FinishGame(bool bPlayerWon);
	// 清理倒计时、波次、生成和性能协调器 Timer。
	void ClearGameplayTimers();

	// 敌人 HealthComponent 触发死亡后更新注册表。
	UFUNCTION()
	void HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy);

	// 敌人因其他原因销毁时执行同样的注册表清理。
	UFUNCTION()
	void HandleEnemyDestroyed(AActor* DestroyedActor);

	// 玩家死亡时把本局结算为失败。
	UFUNCTION()
	void HandlePlayerDied(AfpstrueCharacter* DeadPlayer);

	// ==================== 运行时引用与状态 ====================

	UPROPERTY()
	TArray<AActor*> SpawnPoints;

	UPROPERTY()
	AfpstrueCharacter* PlayerCharacter = nullptr;

	UPROPERTY()
	TArray<AActor*> QueuedSpawnPoints;

	UPROPERTY()
	TSubclassOf<AfpstrueEnemyCharacter> QueuedEnemyClass;

	UPROPERTY(VisibleAnywhere, Category = "Performance|Benchmark")
	TObjectPtr<UfpstrueBenchmarkRunner> BenchmarkRunner;

	UPROPERTY(VisibleAnywhere, Category = "Performance|Significance")
	TObjectPtr<UfpstrueEnemySignificanceCoordinator> EnemySignificanceCoordinator;

	UPROPERTY(VisibleAnywhere, Category = "Performance|Animation Sharing")
	TObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> EnemyAnimationSharingCoordinator;

	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> RegisteredEnemies;

	int32 CurrentWave = 0;
	int32 AliveEnemyCount = 0;
	int32 RemainingTime = 0;
	int32 PendingEnemySpawnCount = 0;
	int32 NextQueuedSpawnIndex = 0;
	int32 ConsecutiveSpawnFailureCount = 0;
	bool bGameRunning = false;
	bool bGameEnded = false;

	FTimerHandle CountdownTimerHandle;
	FTimerHandle WaveTimerHandle;
	FTimerHandle SpawnTimerHandle;
};
