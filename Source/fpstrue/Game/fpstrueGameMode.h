// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Characters/Enemies/fpstrueEnemySignificance.h"
#include "Game/fpstrueWaveConfiguration.h"
#include "fpstrueGameMode.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;
class UfpstrueBenchmarkRunner;
class UfpstrueEnemyAnimationSharingCoordinator;
class UfpstrueEnemySignificanceCoordinator;

// 对局阶段互斥；Starting 防止装配期间的同步回调重复启动，Finished 为终态。
enum class EFPMatchPhase : uint8
{
	Waiting,
	Starting,
	Playing,
	Finished
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRemainingTimeChanged, int32, RemainingTime);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWaveChanged, int32, CurrentWave, int32, TotalWaves);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAliveEnemyCountChanged, int32, AliveEnemyCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameResult, bool, bPlayerWon);

/**
 * 游戏流程总协调器：管理波次、敌人注册表、倒计时，并装配群体AI与性能共享模块。
 *
 * GameMode 只拥有对局级状态；具体寻路、战斗、生命、显著性评分和动画共享均委托给独立对象。
 * 对局开始时负责连接这些模块；结束时先停止 Timer 与 AI，再解除 Delegate，并在 EndPlay 清空注册表。
 */
UCLASS()
class FPSTRUE_API AfpstrueGameMode : public AGameModeBase
{
	// UHT 在此接入反射代码；构造函数由本类显式声明。
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

	UFUNCTION(BlueprintPure, Category = "Game")
	bool IsRunning() const { return MatchPhase == EFPMatchPhase::Playing; }

	UFUNCTION(BlueprintPure, Category = "Game")
	bool IsFinished() const { return MatchPhase == EFPMatchPhase::Finished; }

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

	// 不指定时保留现有 GameMode 蓝图配置；指定时以下旧波次字段不再参与取值。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave")
	TObjectPtr<UfpstrueWaveConfiguration> WaveConfiguration;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (EditCondition = "WaveConfiguration == nullptr"))
	TSubclassOf<AfpstrueEnemyCharacter> EnemyClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn")
	FName EnemySpawnTag = TEXT("EnemySpawn");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|AI")
	TSubclassOf<AfpstrueSurroundManager> SurroundManagerClass;

	// 本局敌人共用的围攻管理器实例；可编辑的是上面的类配置，不是此运行时实例。
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Game|AI")
	TObjectPtr<AfpstrueSurroundManager> SurroundManager;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "1"))
	int32 MinimumSpawnPointCount = 4;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave",
			  meta = (ClampMin = "1", EditCondition = "WaveConfiguration == nullptr"))
	int32 TotalWaves = 3;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave",
			  meta = (ClampMin = "1", EditCondition = "WaveConfiguration == nullptr"))
	int32 BaseEnemiesPerWave = 5;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave",
			  meta = (ClampMin = "0", EditCondition = "WaveConfiguration == nullptr"))
	int32 EnemiesAddedPerWave = 2;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave", meta = (EditCondition = "WaveConfiguration == nullptr"))
	TArray<FfpstrueWaveConfig> WaveConfigs;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Wave",
			  meta = (ClampMin = "0.0", EditCondition = "WaveConfiguration == nullptr"))
	float WaveInterval = 5.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "0.0"))
	float ReusedSpawnPointRadius = 300.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "300.0"))
	float MaxReusedSpawnPointRadius = 2000.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Spawn", meta = (ClampMin = "0.01"))
	float SpawnInterval = 0.05f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game|Time",
			  meta = (ClampMin = "1", EditCondition = "WaveConfiguration == nullptr"))
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

	// 性能协调器与基准采集器通过受控访问读取对局内部状态。
	friend class UfpstrueBenchmarkRunner;
	friend class UfpstrueEnemySignificanceCoordinator;
	friend class FFpstrueGameModeStartupTest;
	friend class FFpstrueWaveConfigurationTest;

	// ==================== 波次与生成 ====================

	// 查找带指定 Tag 的 TargetPoint，供所有波次复用。
	void CacheSpawnPoints();
	// 创建全局 SurroundManager，并注入当前玩家目标。
	bool CreateSurroundManager();
	// 返回正常配置或 Benchmark 覆盖后的总波数。
	int32 GetConfiguredWaveCount() const;
	// 返回指定波次应生成的敌人数。
	int32 GetEnemyCountForWave(int32 WaveNumber) const;
	// 返回指定波次使用的敌人类，未覆盖时回退到默认类。
	TSubclassOf<AfpstrueEnemyCharacter> GetEnemyClassForWave(int32 WaveNumber) const;
	// 配置只选一种来源；外部资产绝不逐字段回退到旧蓝图默认值。
	float GetConfiguredWaveInterval() const;
	int32 GetConfiguredGameDuration() const;
	// 推进波次编号、广播 UI 事件并启动本波生成。
	void StartNextWave();
	// 初始化本波的分帧生成队列。
	void SpawnCurrentWave();
	// Timer 每次只生成一个敌人，降低集中 Spawn 峰值。
	void SpawnNextQueuedEnemy();
	// 在给定出生点附近寻找可导航位置，生成敌人并注入 AI 上下文。
	bool SpawnEnemyAtPoint(AActor* SpawnPoint, int32 SpawnPointReuseCount, TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass);
	// 停止生成 Timer 并清空待生成状态。
	void ClearSpawnQueue();

	// ==================== 敌人注册表 ====================

	// 把新敌人加入唯一注册表，并连接死亡事件和动画共享协调器。
	void RegisterEnemy(AfpstrueEnemyCharacter* Enemy);
	// 从注册表和共享系统移除敌人，并按需通知 HUD 数量变化。
	void UnregisterEnemy(AfpstrueEnemyCharacter* Enemy);
	// 单个注销与退出批量清理共用，解除委托和共享引用，不修改集合或广播。
	void DisconnectEnemy(AfpstrueEnemyCharacter* Enemy);
	// 防御性移除已失效弱键；正常离场由 OnEndPlay 主动注销，本函数处理遗漏的边界路径。
	void PruneInvalidEnemyRegistrations();
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
	// 游戏结束时停止所有仍存活敌人的 AI。
	void StopActiveEnemies();
	// 结算与 EndPlay 共用：清理倒计时、波次、生成和性能协调器 Timer，再停止 AI、解绑玩家。
	void StopGameplay();

	// 敌人 HealthComponent 触发死亡后更新注册表。
	UFUNCTION()
	void HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy);

	// 敌人因其他原因销毁时执行同样的注册表清理。
	UFUNCTION()
	void HandleEnemyDestroyed(AActor* DestroyedActor);

	// 覆盖关卡移除、流送和世界切换等不一定经过死亡/Destroyed 委托的离场路径。
	UFUNCTION()
	void HandleEnemyEndPlay(AActor* EndingActor, EEndPlayReason::Type EndPlayReason);

	// 玩家死亡时把本局结算为失败。
	UFUNCTION()
	void HandlePlayerDied(AfpstrueCharacter* DeadPlayer);

	// ==================== 运行时引用与状态 ====================

	// 出生点数组与 GameplayStatics 的 TArray<AActor*>& 接口保持一致，仅缓存本局运行时引用。
	UPROPERTY(Transient)
	TArray<AActor*> SpawnPoints;

	// GC 可达的运行时引用；Actor 显式销毁后仍须校验有效性。
	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> PlayerCharacter;

	UPROPERTY(Transient)
	TArray<AActor*> QueuedSpawnPoints;

	// 本波已解析的硬类引用；Transient 不保存生成队列，不代表异步加载。
	UPROPERTY(Transient)
	TSubclassOf<AfpstrueEnemyCharacter> QueuedEnemyClass;

	UPROPERTY(VisibleAnywhere, Category = "Performance|Benchmark")
	TObjectPtr<UfpstrueBenchmarkRunner> BenchmarkRunner;

	UPROPERTY(VisibleAnywhere, Category = "Performance|Significance")
	TObjectPtr<UfpstrueEnemySignificanceCoordinator> EnemySignificanceCoordinator;

	UPROPERTY(VisibleAnywhere, Category = "Performance|Animation Sharing")
	TObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> EnemyAnimationSharingCoordinator;

	//注册表保证敌人唯一；弱引用不阻止 Actor 销毁，失效项由注销流程清理。
	// 幂等注销由 Remove 的返回值保证，弱引用仅表示注册表不拥有敌人。
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> RegisteredEnemies;

	int32 CurrentWave = 0;
	int32 RemainingTime = 0;
	int32 PendingEnemySpawnCount = 0;
	int32 NextQueuedSpawnIndex = 0;
	int32 ConsecutiveSpawnFailureCount = 0;
	EFPMatchPhase MatchPhase = EFPMatchPhase::Waiting;

	FTimerHandle CountdownTimerHandle;
	FTimerHandle WaveTimerHandle;
	FTimerHandle SpawnTimerHandle;
};
