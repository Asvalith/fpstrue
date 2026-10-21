// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
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
class UProgressBar;
class UTextBlock;

/*
 * 联机职责入口：GameInstance 管连接旅行；Controller 转发拥有者握手并估计服务器时间；
 * GameMode 管服务端对象保留、生成与重生；PlayerState/GameState 发布事实；HUD/Widget 只读显示。
 * 连接与委托均由 Game Thread 驱动；命中、弹药、生命和 AI 决策分别归各业务模块，不在这里重复实现。
 */

// 原 ingame 布局的只读表现桥；由 HUD 推送当前值，不查询单机 GameMode 或在 UMG 属性绑定里轮询角色。
UCLASS(Blueprintable)
class FPSTRUE_API UfpstruePvpIngameWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	// 构建前也可接收快照；NativeConstruct 会补读缓存，重连或晚创建不会漏掉之前的事件。
	void ApplySnapshot(float Health, float MaxHealth, bool bEquipped, int32 Ammo, int32 Reserve, const FText& WeaponName);
	bool HasAuthoredControls() const;
	bool HasPaintedSnapshot() const;
protected:
	virtual void NativeConstruct() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
private:
	friend class FFpstrueMultiplayerHudRebindTest;
	void ResolveAuthoredControls();
	void RefreshControls();
	UPROPERTY(Transient) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MagazineText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReserveText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> WeaponNameText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MatchHintText;
	UPROPERTY(Transient) TObjectPtr<UWidget> AmmoContainer;
	UPROPERTY(Transient) TObjectPtr<UWidget> CountdownRow;
	float HealthFraction = 1.0f;
	int32 CurrentAmmo = 0;
	int32 ReserveAmmo = 0;
	bool bHasEquipment = false;
	bool bPlayedFadeIn = false;
	FText CurrentWeaponName;
	mutable uint64 LastPaintFrame = 0;
	uint32 SnapshotRevision = 0;
	mutable uint32 LastPaintedSnapshotRevision = 0;
};

// 联机界面只展示拥有者当前 Pawn 的状态；换 Pawn 或重连时重新订阅并补读快照。
UCLASS()
class FPSTRUE_API AfpstruePvpHUD : public AHUD
{
	GENERATED_BODY()
public:
	AfpstruePvpHUD();
	virtual void DrawHUD() override;
	bool HasDrawnCurrentState(const AfpstrueCharacter* Pawn) const;
protected:
	virtual void PostInitializeComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 只在本地有画面时加载；专服不为 HUD 保活整套 UMG/动画资源。
	UPROPERTY(EditDefaultsOnly, Category = "Multiplayer|UI") TSoftClassPtr<UfpstruePvpIngameWidget> IngameWidgetClass;
private:
	friend class FFpstrueMultiplayerHudRebindTest;
	void BindPawn(AfpstrueCharacter* Pawn);
	bool EnsureIngameWidget();
	void RefreshIngameSnapshot();
	UFUNCTION() void HandlePossessedPawnChanged(APawn* OldPawn, APawn* NewPawn);
	UFUNCTION() void HandleHealthChanged(float Health);
	UFUNCTION() void HandleWeaponChanged(UfpstrueWeaponComponent* Weapon);
	UFUNCTION() void HandleAmmoChanged(int32 Ammo, int32 Magazine, int32 Reserve);
	TWeakObjectPtr<AfpstrueCharacter> ObservedPawn;
	TWeakObjectPtr<UfpstrueWeaponComponent> ObservedWeapon;
	UPROPERTY(Transient) TObjectPtr<UfpstruePvpIngameWidget> IngameWidget;
	float CurrentHealth = 0.0f;
	float CurrentMaxHealth = 100.0f;
	int32 CurrentAmmo = 0;
	int32 ReserveAmmo = 0;
	uint64 LastDrawFrame = 0;
	bool bAttemptedIngameWidget = false;
	FText CurrentWeaponName;
};

UENUM(BlueprintType)
enum class EFPConnectionState : uint8
{
	Idle, Connecting, WaitingReady, Connected, RetryWait, Leaving, Failed
};

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
	// 供开发验收输入驱动器读取服务器已允许的反击阶段，不代表正式战斗权限。
	bool IsPveSmokeCounterattackReady() const { return bSmokePveCounterattackReady; }
	double GetClockUncertaintySeconds() const;
	// 网络断开会先进入 PawnLeavingGame，再进入 GameMode Logout；不能在 Logout 才尝试保留已经销毁的 Pawn。
	virtual void PawnLeavingGame() override;
	UFUNCTION(Server, Reliable) void ServerSubmitConnectionToken(const FString& Token);
	UFUNCTION(Server, Reliable) void ServerLeavePvp();
	// 仅开发实机验收读取服务器导航路线；客户端仍通过正常 CMC 输入行走，不传送 Pawn。
	const TArray<FVector>& GetNormalVisualRoute() const { return NormalVisualRoute; }
	// 路线接收版本用于输入驱动器识别新规划，不参与移动复制。
	int32 GetNormalVisualRouteRevision() const { return NormalVisualRouteRevision; }
	// 只返回开发验收路线数据；不移动角色、不生成敌人，也不接纳客户端伤害或目标坐标。
	UFUNCTION(Server, Reliable) void ServerRequestNormalVisualEnemyRoute();

protected:
	virtual void BeginPlayingState() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend class AfpstruePvpGameMode;
	friend class UfpstrueMultiplayerGameInstance;
	friend class FFpstrueMultiplayerResumeSessionTest;
	friend class FFpstrueMultiplayerSpawnReadinessTest;
	UFUNCTION(Client, Reliable) void ClientBeginConnectionHandshake();
	UFUNCTION(Client, Reliable) void ClientConnectionAccepted(const FString& Token, int32 ResumeId, bool bRestored);
	UFUNCTION(Client, Reliable) void ClientConnectionRejected(const FString& Reason);
	UFUNCTION(Client, Reliable) void ClientNormalVisualRoute(const TArray<FVector>& Points);
	TArray<FVector> NormalVisualRoute;
	int32 NormalVisualRouteRevision = 0;
	bool bNormalFullRouteRequested = false;
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
	// 查询服务器复制的离线保留事实；本地不据此延长恢复期限。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") bool IsDisconnected() const { return bDisconnected; }

private:
	UPROPERTY(Replicated)
	int32 Kills = 0;
	UPROPERTY(Replicated)
	int32 Deaths = 0;
	UPROPERTY(Replicated) bool bDisconnected = false;
};

// 【设计取舍】GameMode 在服务器接纳、计分和重生，GameState/PlayerState 发布公共事实，客户端 UI 不读 GameMode。
// 协调器即使挂在复制 Actor 上，也各端独立计算预算；不因此复制相机评分或本地渲染档位。
UCLASS()
class FPSTRUE_API AfpstruePvpGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AfpstruePvpGameState();
	// 返回本端性能分级协调器；实例存在于两端，评分和预算不经网络复制。
	UfpstrueEnemySignificanceCoordinator* GetEnemySignificanceCoordinator() const { return EnemySignificanceCoordinator; }
	// 返回本端动画共享协调器，不拥有 AI 战斗或命中权限。
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
UCLASS(Config = Game)
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
	friend class AfpstruePvpPlayerController;
	friend class FFpstrueMultiplayerPveTargetLifecycleTest;
	friend class FFpstrueMultiplayerSignificanceCoordinatorTest;
	friend class FFpstrueMultiplayerSpawnReadinessTest;
	friend class FFpstrueMultiplayerEnemyActivationTest;
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
	// 只改变显式基线的入场顺序：两个真实玩家先出生，随后仍按原规则生成并验证160个敌人。
	bool bBaselinePopulationStarted = false;
#if !UE_BUILD_SHIPPING
	void SendNormalVisualRoutes();
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

// 跨世界的本地连接状态机；使用引擎地址直连，不代替在线账号认证或服务器玩法权威。
UCLASS(Config = Game)
class FPSTRUE_API UfpstrueMultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;
	// 仅显式回环基线接管 Shipping 启动 URL，普通启动与旅行仍由引擎负责。
	virtual void StartGameInstance() override;
	// 使用配置的PvP地图或当前GameDefaultMap开启监听服务器。
	UFUNCTION(Exec)
	void HostPvp();
	// 当前本地玩家连接IP或主机名，可附带端口。
	UFUNCTION(Exec)
	void JoinPvp(const FString& Address);
	UFUNCTION(Exec) void CancelPvpConnection();
	UFUNCTION(Exec) void LeavePvp();
	UFUNCTION(Exec) void RetryPvpConnection();
	// UI 读取本地连接阶段；服务端接纳后仍可能等待必要复制数据到达。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") EFPConnectionState GetConnectionState() const { return ConnectionState; }
	// UI 读取最近一次连接/旅行错误，不含恢复票据。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") FString GetConnectionError() const { return LastConnectionError; }
	void BeginConnectionHandshake(AfpstruePvpPlayerController* Player);
	void AcceptConnectionToken(AfpstruePvpPlayerController* Player, const FString& Token, int32 ResumeId, bool bRestored);
	void RejectConnection(const FString& Reason);

protected:
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer")
	FString PvpMap;

private:
	friend class AfpstruePvpPlayerController;
	friend class FFpstrueMultiplayerConnectionRevisionTest;
	friend class FFpstrueMultiplayerResumeSessionTest;
	friend class FFpstrueMultiplayerSpawnReadinessTest;
	// 空手是合法就绪状态；有装备时才要求其持有者关系已经解析，能力与生命仍必须就绪。
	static bool IsPawnStateReady(const AfpstrueCharacter* Pawn, const AfpstruePvpPlayerState* State);
	// 地图配置读取沿用引擎设置；业务入口仍只接纳合法包路径，不传额外 URL 选项。
	FString ResolvePvpMapPackage() const;
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
