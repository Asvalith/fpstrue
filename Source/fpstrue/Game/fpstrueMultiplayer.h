// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "TimerManager.h"
#include "Containers/Ticker.h"
#include "fpstrueMultiplayer.generated.h"

class AfpstrueCharacter;
class APlayerController;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;
struct FFPDamageContext;
class UfpstrueMultiplayerGameInstance;
class UfpstrueEnemySignificanceCoordinator;
class UfpstrueEnemyAnimationSharingCoordinator;
class UfpstrueWeaponComponent;

// 联机界面只展示拥有者当前 Pawn 的状态；换 Pawn 或重连时重新订阅并补读快照。
UCLASS()
class FPSTRUE_API AfpstruePvpHUD : public AHUD
{
	GENERATED_BODY()
public:
	virtual void DrawHUD() override;
	bool HasDrawnCurrentState(const AfpstrueCharacter* Pawn) const;
protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
private:
	friend class FFpstrueMultiplayerHudRebindTest;
	void BindPawn(AfpstrueCharacter* Pawn);
	UFUNCTION() void HandleHealthChanged(float Health);
	UFUNCTION() void HandleWeaponChanged(UfpstrueWeaponComponent* Weapon);
	UFUNCTION() void HandleAmmoChanged(int32 Ammo, int32 Magazine, int32 Reserve);
	TWeakObjectPtr<AfpstrueCharacter> ObservedPawn;
	TWeakObjectPtr<UfpstrueWeaponComponent> ObservedWeapon;
	float CurrentHealth = 0.0f;
	int32 CurrentAmmo = 0;
	int32 ReserveAmmo = 0;
	uint64 LastDrawFrame = 0;
};

UENUM(BlueprintType)
enum class EFPConnectionState : uint8
{
	Idle, Connecting, WaitingReady, Connected, RetryWait, Leaving, Failed
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFPConnectionChanged, EFPConnectionState, State);

// 拥有者时钟只用于给射击意图标注估计时间；时间和命中仍由服务器校验，不构成客户端裁决权限。
UCLASS()
class FPSTRUE_API AfpstruePvpPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// 使用往返采样建立本机单调时钟与服务器世界时间的对应；服务端直接返回自己的世界时间。
	double GetEstimatedServerTime() const;
	bool HasServerTimeSample() const;
	// 开发版联机验收先完成原玩家战斗，再进入敌人阶段；Shipping 不执行自动场景。
	UFUNCTION(Server, Reliable) void ServerBeginPveSmoke();
	UFUNCTION(Client, Reliable) void ClientAllowPveSmokeCounterattack();
	bool IsPveSmokeCounterattackReady() const { return bSmokePveCounterattackReady; }
	double GetClockUncertaintySeconds() const;
	// 网络断开会先进入 PawnLeavingGame，再进入 GameMode Logout；不能在 Logout 才尝试保留已经销毁的 Pawn。
	virtual void PawnLeavingGame() override;
	UFUNCTION(Server, Reliable) void ServerSubmitConnectionToken(const FString& Token);
	UFUNCTION(Server, Reliable) void ServerLeavePvp();

protected:
	virtual void BeginPlayingState() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend class AfpstruePvpGameMode;
	friend class UfpstrueMultiplayerGameInstance;
	friend class FFpstrueMultiplayerResumeSessionTest;
	UFUNCTION(Client, Reliable) void ClientBeginConnectionHandshake();
	UFUNCTION(Client, Reliable) void ClientConnectionAccepted(const FString& Token, int32 ResumeId, bool bRestored);
	UFUNCTION(Client, Reliable) void ClientConnectionRejected(const FString& Reason);
	FString AssignedRejoinToken;
	bool bConnectionHandshakeComplete = false;
	bool bIntentionalLeave = false;
	bool bSmokePveCounterattackReady = false;
	void RequestClockSample();
	UFUNCTION(Server, Unreliable) void ServerRequestClockSample(uint32 Sequence);
	UFUNCTION(Client, Unreliable) void ClientReceiveClockSample(uint32 Sequence, double ServerTime);
	FTimerHandle ClockSampleTimer;
	TMap<uint32, double> PendingClockSamples;
	uint32 ClockSequence = 0;
	double ServerClockOffset = 0.0;
	double LastClockSampleLocal = -1.0;
	double BestClockRoundTrip = TNumericLimits<double>::Max();
	double ClockUncertainty = 1.0;
	double LastServerClockRequest = -1.0;
#if !UE_BUILD_SHIPPING
	void SampleSmokeMovement();
	// 有画面门禁等待真实相机、装备、HUD 和敌人副本，再申请引擎视口截图。
	void CheckVisualRecovery();
	void HandleVisualScreenshotProcessed();
	FTimerHandle VisualRecoveryTimer;
	FDelegateHandle VisualScreenshotHandle;
	FString VisualScreenshotPath;
	int32 VisualResumeId = INDEX_NONE;
	bool bVisualRestored = false;
	bool bVisualCaptureRequested = false;
	double VisualReadyAt = 0.0;
	FTimerHandle SmokeMovementTimer;
	TMap<TWeakObjectPtr<AfpstrueCharacter>, FVector> SmokeRemoteOrigins;
	bool bSmokeRemoteMovementReported = false;
#endif
};

// 计分跟随玩家控制器而不是Pawn，死亡换角色后继续保留本局击杀和死亡数。
UCLASS()
class FPSTRUE_API AfpstruePvpPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	// 注册服务器向所有客户端复制的个人计分字段。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 界面读取本局击杀数，不允许客户端直接提交成绩。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	int32 GetKills() const { return Kills; }
	// 界面读取本局死亡数。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	int32 GetDeaths() const { return Deaths; }
	// 仅服务器结算一次有效击杀，同时更新引擎通用Score。
	void AddKill();
	// 仅服务器结算一次死亡。
	void AddDeath();
	// 暂时离线保留同一个服务器 PlayerState，不复制客户端成绩来恢复身份。
	virtual void OnDeactivated() override;
	void SetRetainedForReconnect(bool bRetained);
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") bool IsDisconnected() const { return bDisconnected; }

private:
	UPROPERTY(Replicated)
	int32 Kills = 0;
	UPROPERTY(Replicated)
	int32 Deaths = 0;
	UPROPERTY(Replicated) bool bDisconnected = false;
	bool bRetainedForReconnect = false;
};

// 客户端通过GameState和引擎PlayerArray读取比赛状态及所有参与玩家的计分。
UCLASS()
class FPSTRUE_API AfpstruePvpGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AfpstruePvpGameState();
	UfpstrueEnemySignificanceCoordinator* GetEnemySignificanceCoordinator() const { return EnemySignificanceCoordinator; }
	UfpstrueEnemyAnimationSharingCoordinator* GetEnemyAnimationSharingCoordinator() const { return EnemyAnimationSharingCoordinator; }
	// 注册比赛活动状态和已结算死亡总数的复制。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 返回按击杀、死亡和玩家编号稳定排序的计分表。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	TArray<AfpstruePvpPlayerState*> GetScoreboard() const;
	// 客户端读取对局是否接受战斗和重生。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Match")
	bool IsMatchActive() const { return bMatchActive; }
	// 读取服务器已经接纳的死亡事务数量。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	int32 GetEliminationCount() const { return EliminationCount; }
	// 由服务器GameMode开放或关闭当前对局。
	void SetMatchActive(bool bActive);
	// 由服务器登记一次已去重的死亡事务。
	void AddElimination();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 每端各自运行：服务器计算 Gameplay 距离，客户端分配自己的表现预算；不复制相机评分。
	UPROPERTY(VisibleAnywhere, Category = "Performance") TObjectPtr<UfpstrueEnemySignificanceCoordinator> EnemySignificanceCoordinator;
	UPROPERTY(VisibleAnywhere, Category = "Performance") TObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> EnemyAnimationSharingCoordinator;
	UPROPERTY(Replicated)
	bool bMatchActive = false;
	UPROPERTY(Replicated)
	int32 EliminationCount = 0;
};

// 沿用原 Pvp 类名的 PvPvE 自由对战；玩家互伤与敌人同场，不启动单机的死亡即失败流程。
UCLASS()
class FPSTRUE_API AfpstruePvpGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	// 选择现有玩家和武器蓝图，并使用原生复制计分对象。
	AfpstruePvpGameMode();
	// 角色服务器死亡入口；同一玩家重生前的重复通知不会重复计分或排队。
	void HandlePlayerEliminated(AfpstrueCharacter* Victim, AController* Killer);
	// 每个有效玩家拥有独立站位缓存；寻路提交预算仍由所有管理器按 World 共用。
	AfpstrueSurroundManager* GetOrCreateSurroundManager(AfpstrueCharacter* Target);
	bool IsCombatParticipant(const AfpstrueCharacter* Target) const;
	// 返回服务器登记的存活敌人数，不把客户端表现副本当作 AI 实例。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|PvE") int32 GetAliveEnemyCount() const;
	void StartPveSmoke(AfpstruePvpPlayerController* Requester);
	// 玩家离开时撤销待重生Timer并清理服务器生成的武器。
	virtual void Logout(AController* Exiting) override;
	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	bool PreserveDisconnectedPlayer(AfpstruePvpPlayerController* Player);
	void CompleteConnectionHandshake(AfpstruePvpPlayerController* Player, const FString& Token);
	bool IsRetainedDisconnectedPawn(const AfpstrueCharacter* Pawn) const;
	bool HandleRetainedPawnEliminated(AfpstrueCharacter* Pawn, AController* Killer);
	void ClearRejoinSlots();

protected:
	// 世界开始后将复制的对局状态置为活动。
	virtual void BeginPlay() override;
	// 完成服务器占有后自动装备本局默认武器，初始生成和重生共用此入口。
	virtual void FinishRestartPlayer(AController* NewPlayer, const FRotator& StartRotation) override;
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
	// 世界退出先拒绝新事务，再停止全部重生Timer和清理武器。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer|Respawn", meta = (ClampMin = "0.1"))
	float RespawnDelay = 3.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer|Equipment")
	TSubclassOf<AActor> DefaultWeaponClass;
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
	friend class FFpstrueMultiplayerPveTargetLifecycleTest;
	friend class FFpstrueMultiplayerSignificanceCoordinatorTest;
	void UpdatePvePopulation();
	AfpstrueEnemyCharacter* SpawnPveEnemy(AfpstrueCharacter* NearPlayer = nullptr);
	void RegisterPveEnemy(AfpstrueEnemyCharacter* Enemy);
	UFUNCTION() void HandlePveEnemyDied(AfpstrueEnemyCharacter* Enemy);
	UFUNCTION() void HandlePveEnemyEndPlay(AActor* Enemy, EEndPlayReason::Type Reason);
	void HandlePvePlayerDamaged(TWeakObjectPtr<AfpstrueCharacter> Player, float Damage,
		AActor* DamageCauser, AController* Instigator, const FFPDamageContext& Context);
	void StopPve();
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> PveEnemies;
	TMap<TWeakObjectPtr<AfpstrueCharacter>, TWeakObjectPtr<AfpstrueSurroundManager>> TargetSurroundManagers;
	TSet<TWeakObjectPtr<AfpstrueCharacter>> ObservedPvePlayers;
	UPROPERTY(Transient) TArray<TObjectPtr<AActor>> PveSpawnPoints;
	FTimerHandle PvePopulationTimer;
	double NextEnemySpawnTime = 0.0;
	int32 NextPveSpawnIndex = 0;
#if !UE_BUILD_SHIPPING
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
	// 先移出记录再销毁武器，组件退出回调不能重复清理同一条记录。
	void ReleasePlayerWeapon(APlayerController* Player);

	TMap<TWeakObjectPtr<APlayerController>, FTimerHandle> PendingRespawns;
	TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AActor>> PlayerWeapons;
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

// 本地控制台提供局域网入口；连接使用引擎地址直连，不依赖在线会话服务。
UCLASS(Config = Game)
class FPSTRUE_API UfpstrueMultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;
	// 使用配置的PvP地图或当前GameDefaultMap开启监听服务器。
	UFUNCTION(Exec)
	void HostPvp();
	// 当前本地玩家连接IP或主机名，可附带端口。
	UFUNCTION(Exec)
	void JoinPvp(const FString& Address);
	UFUNCTION(Exec) void CancelPvpConnection();
	UFUNCTION(Exec) void LeavePvp();
	UFUNCTION(Exec) void RetryPvpConnection();
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") EFPConnectionState GetConnectionState() const { return ConnectionState; }
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") FString GetConnectionError() const { return LastConnectionError; }
	UPROPERTY(BlueprintAssignable, Category = "Multiplayer|Connection") FFPConnectionChanged OnConnectionStateChanged;
	void BeginConnectionHandshake(AfpstruePvpPlayerController* Player);
	void AcceptConnectionToken(AfpstruePvpPlayerController* Player, const FString& Token, int32 ResumeId, bool bRestored);
	void RejectConnection(const FString& Reason);

protected:
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer")
	FString PvpMap;

private:
	friend class AfpstruePvpPlayerController;
	friend class FFpstrueMultiplayerConnectionRevisionTest;
	bool TickConnection(float DeltaSeconds);
	void StartClientTravel(uint32 Revision);
	void SetConnectionState(EFPConnectionState State);
	void HandleConnectionFailure();
	void ReturnToLocalWorld();
	bool OwnsNetworkFailure(UWorld* World, class UNetDriver* Driver) const;
	void HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Failure, const FString& Error);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type Failure, const FString& Error);
	void HandlePostLoadMap(UWorld* World);
	void SaveResumeCache();
	void LoadResumeCache();
	void ClearResumeCache();
	EFPConnectionState ConnectionState = EFPConnectionState::Idle;
	FString LastConnectionError;
	FString ServerAddress;
	FString ResumeServerAddress;
	FString ResumeToken;
	FString ResumeCacheFile;
	TWeakObjectPtr<AfpstruePvpPlayerController> ConnectingController;
	TWeakObjectPtr<UNetDriver> AttemptNetDriver;
	uint32 OperationRevision = 0;
	uint32 ScheduledRetryRevision = 0;
	int32 RetryCount = 0;
	int32 ResumeId = INDEX_NONE;
	bool bRestoredConnection = false;
	bool bAutoReconnect = false;
	bool bReturningToLocalWorld = false;
	double AttemptDeadline = 0.0;
	double RetryAt = 0.0;
	FTSTicker::FDelegateHandle ConnectionTicker;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoadHandle;
};
