// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TimerManager.h"
#include "fpstruePvpGameMode.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;
class AfpstruePvpPlayerController;
class AfpstruePvpPlayerState;
class UNetConnection;
struct FFPDamageContext;

// 唯一服务器规则入口：生成与重生、PvE 登记、恢复资格和对象保留；不接管客户端 UI 或连接重试。

// 沿用原 Pvp 类名的 PvPvE 自由对战；玩家互伤与敌人同场，不启动单机的死亡即失败流程。
UCLASS(Config = Game)
class FPSTRUE_API AfpstruePvpGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	// 选择现有玩家和武器蓝图，并使用原生复制计分对象。
	AfpstruePvpGameMode();
	// 统一服务器命中公平政策；显示缓冲预算不是客户端可任意扩大的回溯时间。
	float GetTargetInterpolationSeconds() const { return FMath::Clamp(TargetInterpolationSeconds, 0.0f, 0.1f); }
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|HitValidation", meta = (ClampMin = "0.0", ClampMax = "0.1"))
	float TargetInterpolationSeconds = 0.05f;
	// 用服务器观测 RTT 的一半估计目标下行年龄；历史仍受固定硬限，不接纳客户端自报 Ping。
	bool IsTargetLatencyCompensationEnabled() const { return bEnableTargetLatencyCompensation; }
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|HitValidation")
	bool bEnableTargetLatencyCompensation = true;
	// 角色服务器死亡入口；同一玩家重生前的重复通知不会重复计分或排队。
	void HandlePlayerEliminated(AfpstrueCharacter* Victim, AController* Killer);
	// 每个有效玩家拥有独立站位缓存；寻路提交预算仍由所有管理器按 World 共用。
	AfpstrueSurroundManager* GetOrCreateSurroundManager(AfpstrueCharacter* Target);
	bool IsCombatParticipant(const AfpstrueCharacter* Target) const;
	// 返回服务器登记的存活敌人数，不把客户端表现副本当作 AI 实例。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|PvE") int32 GetAliveEnemyCount() const;
#if !UE_BUILD_SHIPPING
	void StartPveSmoke(AfpstruePvpPlayerController* Requester);
#endif
	// 玩家离开时撤销待重生Timer并清理服务器生成的武器。
	virtual void Logout(AController* Exiting) override;
	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;

protected:
	// 世界开始后将复制的对局状态置为活动。
	virtual void BeginPlay() override;
	// 初始生成和重生都从空手开始；只有显式开发烟测在完成占有后自动装备默认武器。
	virtual void FinishRestartPlayer(AController* NewPlayer, const FRotator& StartRotation) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual void FailedToRestartPlayer(AController* NewPlayer) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
	// 世界退出先拒绝新事务，再停止全部重生Timer和清理武器。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer|Respawn", meta = (ClampMin = "0.1"))
	float RespawnDelay = 3.0f;
	// 包含客户端冷启动及地图加载的恢复预算；离线角色仍可受伤，过期即回收，不能由客户端延长。
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|Connection", meta = (ClampMin = "5", ClampMax = "120"))
	float ReconnectGraceSeconds = 90.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer|Equipment")
	TSubclassOf<AActor> DefaultWeaponClass;
	// 分散出生仍保留；只在落点附近提供一把需走近拾取的枪，不把满弹装备直接送进手中。
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|Equipment")
	bool bProvideNearbySpawnPickup = true;
	// PvPvE 分帧补充敌人；不复用“单个玩家死亡就全局失败”的单机结算规则。
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|PvE")
	TSubclassOf<AfpstrueEnemyCharacter> EnemyClass;
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|PvE", meta = (ClampMin = "0", ClampMax = "160"))
	int32 EnemyPopulation = 12;
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|PvE", meta = (ClampMin = "0.05"))
	float EnemySpawnInterval = 0.2f;
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|PvE", meta = (ClampMin = "0.1"))
	float EnemyRespawnDelay = 8.0f;
	UPROPERTY(Config, EditDefaultsOnly, Category = "Multiplayer|PvE")
	FName EnemySpawnTag = TEXT("EnemySpawn");

private:
	// 握手/离线结算是本规则内部步骤，Controller只转交请求。
	bool PreserveDisconnectedPlayer(AfpstruePvpPlayerController* Player);
	void CompleteConnectionHandshake(AfpstruePvpPlayerController* Player, const FString& Token);
	bool IsRetainedDisconnectedPawn(const AfpstrueCharacter* Pawn) const;
	bool HandleRetainedPawnEliminated(AfpstrueCharacter* Pawn, AController* Killer);
	void ClearRejoinSlots();
	friend class AfpstruePvpPlayerController;
	friend class FFpstrueMultiplayerPveTargetLifecycleTest;
	friend class FFpstrueMultiplayerSignificanceCoordinatorTest;
	friend class FFpstrueMultiplayerSpawnReadinessTest;
	friend class FFpstrueMultiplayerEnemyActivationTest;
	friend class FFpstrueMultiplayerMatchLifecycleTest;
	void UpdatePvePopulation();
	AfpstrueEnemyCharacter* SpawnPveEnemy(AfpstrueCharacter* NearPlayer = nullptr);
	void RegisterPveEnemy(AfpstrueEnemyCharacter* Enemy);
	UFUNCTION() void HandlePveEnemyDied(AfpstrueEnemyCharacter* Enemy);
	UFUNCTION() void HandlePveEnemyEndPlay(AActor* Enemy, EEndPlayReason::Type Reason);
	void StopPve();
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> PveEnemies;
	TMap<TWeakObjectPtr<AfpstrueCharacter>, TWeakObjectPtr<AfpstrueSurroundManager>> TargetSurroundManagers;
	UPROPERTY(Transient) TArray<TObjectPtr<AActor>> PveSpawnPoints;
	FTimerHandle PvePopulationTimer;
	double NextEnemySpawnTime = 0.0;
	int32 NextPveSpawnIndex = 0;
	// 正式玩法保持160上限；仅非Shipping显式基线可以扩大，仍复用同一生成/行为树流程。
	int32 PopulationLimit = 160;
	// 只改变显式基线的入场顺序：两个真实玩家先出生，随后按原规则生成并验证目标人数。
	bool bBaselinePopulationStarted = false;
#if !UE_BUILD_SHIPPING
	// 显式验收的准备/观察实现位于 Testing/Networking；不替代正式玩法规则。
	bool PreparePveSmokePopulation(AfpstrueCharacter*& NearPlayer);
	void HandlePvePlayerDamaged(TWeakObjectPtr<AfpstrueCharacter> Player, float Damage,
		AActor* DamageCauser, AController* Instigator, const FFPDamageContext& Context);
	TSet<TWeakObjectPtr<AfpstrueCharacter>> ObservedPvePlayers;
	void SendNormalVisualRoutes();
	void TickLiveReconnectOutage();
	TWeakObjectPtr<UNetConnection> LiveOutageConnection;
	double LiveOutageRestoreAt = 0.0;
	int32 LiveOutagePreviousLoss = 0;
	int32 LiveOutageResumeId = INDEX_NONE;
	bool bLiveOutageStarted = false;
	bool bLiveOutageRestored = false;
	bool bNormalVisualRoutesSent = false;
	TWeakObjectPtr<AfpstrueCharacter> SmokePvePlayer;
	TWeakObjectPtr<AfpstrueEnemyCharacter> SmokePveEnemy;
	TWeakObjectPtr<AfpstrueCharacter> SmokeOfflineTarget;
	bool bSmokePveStarted = false;
	bool bSmokePveEnemyHit = false;
	bool bSmokePveCompleted = false;
	bool bSmokeOfflineDamage = false;
	bool bSmokeOfflineScenePrepared = false;
	bool bSmokeOfflineSceneFailed = false;
#endif
	friend class FFpstrueMultiplayerResumeSessionTest;
	// 服务器独占的恢复登记：弱引用真实对象，不保存可由客户端提交的生命/弹药快照。
	struct FRejoinSlot
	{
		TWeakObjectPtr<AfpstrueCharacter> Pawn;
		TWeakObjectPtr<AfpstruePvpPlayerState> State;
		TWeakObjectPtr<AActor> Weapon;
		TWeakObjectPtr<AfpstruePvpPlayerController> Controller;
		FRotator ViewRotation = FRotator::ZeroRotator;
		double ExpiresAt = 0.0;
		bool bDeathRecorded = false;
		bool bDisconnected = false;
		bool bPreviousPhysicsWithoutController = false;
	};
	void SweepRejoinSlots();
	TMap<FString, FRejoinSlot> RejoinSlots;
	TMap<TWeakObjectPtr<AfpstruePvpPlayerController>, double> PendingConnectionHandshakes;
	FTimerHandle RejoinCleanupTimer;
	// Timer只处理仍属于本玩家的旧Pawn，迟到回调不能销毁已经换上的新角色。
	void RespawnPlayer(TWeakObjectPtr<APlayerController> Player, TWeakObjectPtr<AfpstrueCharacter> EliminatedPawn);
	// 在服务器生成持枪Actor，完整装备成功后才保存清理关系。
	void EquipDefaultWeapon(APlayerController* Player);
	void SpawnNearbyWeaponPickup(APlayerController* Player);
	void ReleaseUnclaimedSpawnPickup(APlayerController* Player);
	// 先移出记录再销毁武器，组件退出回调不能重复清理同一条记录。
	void ReleasePlayerWeapon(APlayerController* Player);

	TMap<TWeakObjectPtr<APlayerController>, FTimerHandle> PendingRespawns;
	struct FSpawnRecovery
	{
		FTimerHandle Timer;
		int32 Attempts = 0;
	};
	// 同一次出生最多初次加三次重试；成功、离场和世界退出统一撤销，不开每帧轮询。
	TMap<TWeakObjectPtr<APlayerController>, FSpawnRecovery> SpawnRecoveries;
	TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AActor>> PlayerWeapons;
	TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AActor>> SpawnPickups;
	bool bEndingPvp = false;
#if !UE_BUILD_SHIPPING
	void SampleSmokeMovement();
	FTimerHandle SmokeMovementTimer;
	TMap<TWeakObjectPtr<AfpstrueCharacter>, FVector> SmokeSpawnOrigins;
	TSet<TWeakObjectPtr<AfpstrueCharacter>> SmokeMovedPlayers;
	bool bSmokeTest = false;
	bool bSmokeSawPlayerKill = false;
	bool bSmokeFinalReported = false;
#endif
};
