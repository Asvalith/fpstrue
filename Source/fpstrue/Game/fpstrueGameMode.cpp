// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstrueGameMode.h"
#include "Testing/Benchmarks/fpstrueBenchmarkConfig.h"
#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"
#include "Characters/Enemies/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemySignificanceCoordinator.h"
#include "Testing/Benchmarks/fpstruePerformanceStats.h"
#include "Characters/Enemies/fpstrueSurroundManager.h"
#include "Components/CapsuleComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_STAT(STAT_fpstrueWaveSpawnTime);
DEFINE_STAT(STAT_fpstrueEnemySpawnCount);

/*
 * 单机对局的流程总协调器。
 * 本类只拥有玩家引用、波次/倒计时和敌人注册表；单敌人决策交给 AIController，群体资源交给
 * SurroundManager，性能分级与动画共享分别交给两个 Coordinator，避免 GameMode 变成万能类。
 *
 * 开局装配链：校验玩家/出生点 -> 创建 SurroundManager -> 启动 Significance 与 Animation Sharing
 *          -> 绑定玩家死亡 -> 开倒计时 -> 分帧生成第一波。
 * 敌人链：Spawn -> 注入目标/群体 Manager -> RegisterEnemy 绑定死亡与销毁 -> Death/Destroyed 统一注销。
 * GameMode 只协调模块和对局状态，不直接执行射击、近战、寻路、动画评分或组件级性能设置。
 */

// ==================== 生命周期与开局 ====================

// 构造对局级协调组件；它们随 GameMode 生命周期存在，但只有正式开局后才开始调度。
AfpstrueGameMode::AfpstrueGameMode()
{
	SurroundManagerClass = AfpstrueSurroundManager::StaticClass();
	BenchmarkRunner = CreateDefaultSubobject<UfpstrueBenchmarkRunner>(TEXT("BenchmarkRunner"));
	EnemySignificanceCoordinator = CreateDefaultSubobject<UfpstrueEnemySignificanceCoordinator>(TEXT("EnemySignificanceCoordinator"));
	EnemyAnimationSharingCoordinator =
		CreateDefaultSubobject<UfpstrueEnemyAnimationSharingCoordinator>(TEXT("EnemyAnimationSharingCoordinator"));
}

void AfpstrueGameMode::BeginPlay()
{
	// 普通游玩等待关卡/UI显式开始；只有命令行要求自动基准时才由 Runner 代为启动。
	Super::BeginPlay();

	if (BenchmarkRunner != nullptr)
	{
		BenchmarkRunner->StartIfRequested(this);
	}
}

void AfpstrueGameMode::StartGameMode()
{
	// 所有不可恢复的配置错误都在创建 Timer/敌人前失败，避免只启动了一半的对局状态。
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
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: found %d spawn points, but at least %d are required."), SpawnPoints.Num(),
			   MinimumSpawnPointCount);
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

	if (!IsPlayerAlive())
	{
		UE_LOG(LogTemp, Warning, TEXT("StartGameMode failed: player is already dead."));
		FinishGame(false);
		return;
	}

	BindPlayerDeathEvent();

	if (!CreateSurroundManager())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: SurroundManager could not be created."));
		FinishGame(false);
		return;
	}

	bGameRunning = true;
	CurrentWave = 0;
	RemainingTime = GameDuration;

	OnRemainingTimeChanged.Broadcast(RemainingTime);
	OnWaveChanged.Broadcast(CurrentWave, GetConfiguredWaveCount());
	OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());

	GetWorldTimerManager().SetTimer(CountdownTimerHandle, this, &AfpstrueGameMode::UpdateCountdown, 1.0f, true);

	// 共享管理器必须在首个敌人生成前就绪；敌人实际是否加入仍由 Render Significance 决定。
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->Start(GetEnemyClassForWave(1));
	}
	if (EnemySignificanceCoordinator != nullptr)
	{
		EnemySignificanceCoordinator->Start(this);
	}
	StartNextWave();
}

void AfpstrueGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 先停止会继续产生回调的 Timer/AI，再解除 Delegate 和共享系统，最后交给 AGameModeBase。
	ClearGameplayTimers();
	StopActiveEnemies();
	ClearEnemyRegistrations();

	UnbindPlayerDeathEvent();

	if (SurroundManager)
	{
		SurroundManager->ResetManager();
	}

	Super::EndPlay(EndPlayReason);
}

// ==================== 波次、生成与共享场景资源 ====================

void AfpstrueGameMode::CacheSpawnPoints()
{
	// 一次性收集带 EnemySpawnTag 的 TargetPoint，后续波次只复用缓存，避免每只敌人遍历世界。
	SpawnPoints.Reset();
	UGameplayStatics::GetAllActorsOfClassWithTag(this, ATargetPoint::StaticClass(), EnemySpawnTag, SpawnPoints);
}

void AfpstrueGameMode::StartNextWave()
{
	// 波次状态先提交并广播，再建立生成队列；HUD 看到的波次编号始终与即将生成的配置一致。
	const int32 ConfiguredWaveCount = GetConfiguredWaveCount();
	if (!bGameRunning || CurrentWave >= ConfiguredWaveCount)
	{
		return;
	}

	if (PendingEnemySpawnCount > 0)
	{
		GetWorldTimerManager().SetTimer(WaveTimerHandle, this, &AfpstrueGameMode::StartNextWave, FMath::Max(SpawnInterval, 0.01f), false);
		return;
	}

	++CurrentWave;
	OnWaveChanged.Broadcast(CurrentWave, ConfiguredWaveCount);

	SpawnCurrentWave();

	if (CurrentWave < ConfiguredWaveCount)
	{
		GetWorldTimerManager().SetTimer(WaveTimerHandle, this, &AfpstrueGameMode::StartNextWave, WaveInterval, false);
	}
}

bool AfpstrueGameMode::CreateSurroundManager()
{
	// GameMode 创建唯一群体协调器并注入玩家目标；单个 AI 只保存对它的弱引用。
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

	SurroundManager = World->SpawnActor<AfpstrueSurroundManager>(SurroundManagerClass, FVector::ZeroVector, FRotator::ZeroRotator);

	if (!IsValid(SurroundManager))
	{
		return false;
	}

	// GameMode 负责共享目标的生命周期，避免每个 AIController 重复写入同一状态。
	SurroundManager->SetTargetCharacter(PlayerCharacter);
	return true;
}

int32 AfpstrueGameMode::GetConfiguredWaveCount() const
{
	// 自动压测固定为一个指定规模波次；正常游戏优先读取显式 WaveConfigs。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.HasEnemyCountOverride())
	{
		return 1;
	}

	return WaveConfigs.IsEmpty() ? TotalWaves : WaveConfigs.Num();
}

int32 AfpstrueGameMode::GetEnemyCountForWave(int32 WaveNumber) const
{
	// 将基准覆盖、数据化波次和旧式线性增长三种来源收口成一个敌人数查询入口。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.HasEnemyCountOverride())
	{
		return BenchmarkConfig.EnemyCount;
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
	// 当前波次有专用敌人类时使用专用配置，否则回退到默认 EnemyClass。
	const int32 WaveIndex = WaveNumber - 1;
	if (WaveConfigs.IsValidIndex(WaveIndex) && WaveConfigs[WaveIndex].EnemyClass)
	{
		return WaveConfigs[WaveIndex].EnemyClass;
	}

	return EnemyClass;
}

void AfpstrueGameMode::SpawnCurrentWave()
{
	// 这里只准备队列，不集中 Spawn；SpawnNextQueuedEnemy 由 Timer 每次消费一个，平滑创建尖峰。
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnCurrentWave);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);

	if (!bGameRunning || SpawnPoints.IsEmpty())
	{
		return;
	}

	ClearSpawnQueue();
	QueuedSpawnPoints = SpawnPoints;
	for (int32 Index = QueuedSpawnPoints.Num() - 1; Index > 0; --Index)
	{
		const int32 SwapIndex = FMath::RandRange(0, Index);
		QueuedSpawnPoints.Swap(Index, SwapIndex);
	}

	PendingEnemySpawnCount = GetEnemyCountForWave(CurrentWave);
	QueuedEnemyClass = GetEnemyClassForWave(CurrentWave);

	SpawnNextQueuedEnemy();
	if (PendingEnemySpawnCount > 0)
	{
		GetWorldTimerManager().SetTimer(SpawnTimerHandle, this, &AfpstrueGameMode::SpawnNextQueuedEnemy, FMath::Max(SpawnInterval, 0.01f),
										true);
	}
}

void AfpstrueGameMode::SpawnNextQueuedEnemy()
{
	// 每次 Timer 只消费一个生成请求；失败时换点重试，连续失败达到上限后终止队列。
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnQueuedEnemy);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);

	if (!bGameRunning || PendingEnemySpawnCount <= 0 || QueuedSpawnPoints.IsEmpty() || !QueuedEnemyClass)
	{
		ClearSpawnQueue();
		return;
	}

	const int32 SpawnPointCount = QueuedSpawnPoints.Num();
	const int32 SpawnPointIndex = NextQueuedSpawnIndex % SpawnPointCount;
	const int32 SpawnPointReuseCount = NextQueuedSpawnIndex / SpawnPointCount;
	const bool bSpawnSucceeded = SpawnEnemyAtPoint(QueuedSpawnPoints[SpawnPointIndex], SpawnPointReuseCount, QueuedEnemyClass);

	++NextQueuedSpawnIndex;
	if (bSpawnSucceeded)
	{
		--PendingEnemySpawnCount;
		ConsecutiveSpawnFailureCount = 0;
	}
	else
	{
		++ConsecutiveSpawnFailureCount;
		const int32 FailureLimit = FMath::Max(SpawnPointCount * 4, 8);
		if (ConsecutiveSpawnFailureCount >= FailureLimit)
		{
			UE_LOG(LogTemp, Error, TEXT("Enemy spawn queue stopped after %d consecutive failures with %d enemies remaining."),
				   ConsecutiveSpawnFailureCount, PendingEnemySpawnCount);
			ClearSpawnQueue();
			return;
		}
	}

	if (PendingEnemySpawnCount <= 0)
	{
		ClearSpawnQueue();
	}
}

void AfpstrueGameMode::ClearSpawnQueue()
{
	// 停止分帧生成并清空队列游标，供结算、退出和重新开始统一收口。
	GetWorldTimerManager().ClearTimer(SpawnTimerHandle);
	QueuedSpawnPoints.Reset();
	QueuedEnemyClass = nullptr;
	PendingEnemySpawnCount = 0;
	NextQueuedSpawnIndex = 0;
	ConsecutiveSpawnFailureCount = 0;
}

bool AfpstrueGameMode::SpawnEnemyAtPoint(AActor* SpawnPoint, int32 SpawnPointReuseCount, TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass)
{
	// 单次 Spawn 成功后才注入目标、Benchmark 开关并注册；任一步失败都不增加 Alive 计数。
	if (!IsValid(SpawnPoint))
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy spawn point is invalid."));
		return false;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (NavSystem == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy spawn requires a valid navigation system."));
		return false;
	}

	const AfpstrueEnemyCharacter* EnemyDefaults = WaveEnemyClass.GetDefaultObject();
	const UCapsuleComponent* DefaultCapsule = EnemyDefaults != nullptr ? EnemyDefaults->GetCapsuleComponent() : nullptr;
	const float CapsuleHalfHeight = DefaultCapsule != nullptr ? DefaultCapsule->GetScaledCapsuleHalfHeight() : 96.0f;
	const FVector SpawnOrigin = SpawnPoint->GetActorLocation();
	const FVector ProjectionExtent(150.0f, 150.0f, 500.0f);
	const float ReuseScale = FMath::Sqrt(static_cast<float>(FMath::Max(SpawnPointReuseCount + 1, 1)));
	const float RetryRadius =
		FMath::Clamp(FMath::Max(ReusedSpawnPointRadius, 300.0f) * ReuseScale, 300.0f, FMath::Max(MaxReusedSpawnPointRadius, 300.0f));
	constexpr int32 MaxSpawnAttempts = 8;

	AfpstrueEnemyCharacter* SpawnedEnemy = nullptr;
	for (int32 Attempt = 0; Attempt < MaxSpawnAttempts && SpawnedEnemy == nullptr; ++Attempt)
	{
		FNavLocation ProjectedLocation;
		if ((SpawnPointReuseCount > 0 || Attempt > 0) && RetryRadius > 0.0f)
		{
			// 随机可达点已经携带有效 NavMesh Poly，不再重复 ProjectPointToNavigation。
			if (!NavSystem->GetRandomReachablePointInRadius(SpawnOrigin, RetryRadius, ProjectedLocation))
			{
				continue;
			}
		}
		else if (!NavSystem->ProjectPointToNavigation(SpawnOrigin, ProjectedLocation, ProjectionExtent))
		{
			continue;
		}

		const FVector SpawnLocation = ProjectedLocation.Location + FVector::UpVector * (CapsuleHalfHeight + 2.0f);
		FRotator SpawnRotation = SpawnPoint->GetActorRotation();
		if (PlayerCharacter)
		{
			const FVector ToPlayer = PlayerCharacter->GetActorLocation() - SpawnLocation;
			SpawnRotation = FRotator(0.0f, ToPlayer.Rotation().Yaw, 0.0f);
		}

		const FTransform SpawnTransform(SpawnRotation, SpawnLocation, FVector::OneVector);
		FActorSpawnParameters SpawnParameters;
		// 基准测试与正常玩法使用同一套生成碰撞规则，不能为了凑满数量强制把敌人生成到重叠位置。
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

		SpawnedEnemy = World->SpawnActor<AfpstrueEnemyCharacter>(WaveEnemyClass, SpawnTransform, SpawnParameters);
	}

	if (SpawnedEnemy)
	{
		INC_DWORD_STAT(STAT_fpstrueEnemySpawnCount);

		if (SpawnedEnemy->GetController() == nullptr)
		{
			SpawnedEnemy->SpawnDefaultController();
		}

		AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(SpawnedEnemy->GetController());
		if (EnemyController != nullptr)
		{
			EnemyController->InitializeCombatContext(PlayerCharacter, SurroundManager);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Enemy %s has controller %s; expected fpstrueEnemyAIController."), *GetNameSafe(SpawnedEnemy),
				   *GetNameSafe(SpawnedEnemy->GetController()));
		}

		RegisterEnemy(SpawnedEnemy);
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("SpawnActor failed for enemy class %s at %s after %d reuse(s), sample radius %.0f."),
		   *GetNameSafe(WaveEnemyClass.Get()), *GetNameSafe(SpawnPoint), SpawnPointReuseCount, RetryRadius);
	return false;
}

// ==================== 敌人注册表与协调器连接 ====================

void AfpstrueGameMode::RegisterEnemy(AfpstrueEnemyCharacter* Enemy)
{
	// 注册表只持有弱引用，同时绑定死亡/EndPlay/销毁出口并接入显著性与动画共享协调器。
	// 注册表是 GameMode 对“当前存活参与者”的唯一视图；Add 返回 false 时不重复绑定事件或累计数量。
	if (!IsValid(Enemy))
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredEnemies.Contains(EnemyKey))
	{
		return;
	}

	RegisteredEnemies.Add(EnemyKey);
	Enemy->SetAnimationSharingCoordinator(EnemyAnimationSharingCoordinator);
	Enemy->OnEnemyDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnEndPlay.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyEndPlay);
	Enemy->OnDestroyed.AddUniqueDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);

	if (bGameRunning)
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::UnregisterEnemy(AfpstrueEnemyCharacter* Enemy)
{
	// 无论敌人通过死亡还是直接销毁离场，都在这里解除委托、协调器关系并更新存活计数。
	// Death 和 Destroyed 可能先后到达；TSet::Remove 的返回值让注销与数量广播保持幂等。
	if (Enemy == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredEnemies.Remove(EnemyKey) == 0)
	{
		return;
	}

	Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnEndPlay.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyEndPlay);
	Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->SuspendEnemy(Enemy);
	}
	Enemy->SetAnimationSharingCoordinator(nullptr);

	if (bGameRunning)
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::PruneInvalidEnemyRegistrations()
{
	// OnEndPlay 是主清理路径；这里仅兜底清除已经无法解引用的弱键，避免 Num() 和预算容量长期虚高。
	const int32 PreviousCount = RegisteredEnemies.Num();
	for (auto Iterator = RegisteredEnemies.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator->IsValid())
		{
			Iterator.RemoveCurrent();
		}
	}

	if (bGameRunning && RegisteredEnemies.Num() != PreviousCount)
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::StopActiveEnemies()
{
	// 对局结束时停止所有仍存活 AI；只冻结行为，不在遍历过程中直接销毁 Actor。
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		if (AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get())
		{
			if (AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
			{
				EnemyController->StopAI();
			}
		}
	}
}

void AfpstrueGameMode::ClearEnemyRegistrations()
{
	// EndPlay 阶段批量解除敌人委托和协调器引用，最后清空弱引用集合。
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		if (AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get())
		{
			if (EnemyAnimationSharingCoordinator != nullptr)
			{
				EnemyAnimationSharingCoordinator->SuspendEnemy(Enemy);
			}
			Enemy->SetAnimationSharingCoordinator(nullptr);
			Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
			Enemy->OnEndPlay.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyEndPlay);
			Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
		}
	}

	RegisteredEnemies.Reset();
}

// ==================== 游戏状态、事件与计时器 ====================

bool AfpstrueGameMode::IsPlayerAlive() const
{
	// 对局胜负只读取玩家角色的统一死亡查询，不自行缓存第二份生命状态。
	if (!IsValid(PlayerCharacter))
	{
		return false;
	}

	return !PlayerCharacter->IsDead();
}

void AfpstrueGameMode::BindPlayerDeathEvent()
{
	// 使用 AddUniqueDynamic 防止重复开始流程导致同一死亡回调被绑定多次。
	if (IsValid(PlayerCharacter))
	{
		PlayerCharacter->OnPlayerDeathReported.AddUniqueDynamic(this, &AfpstrueGameMode::HandlePlayerDied);
	}
}

void AfpstrueGameMode::UnbindPlayerDeathEvent()
{
	// 结算和 EndPlay 都显式解绑，避免生命周期末尾继续收到玩家事件。
	if (IsValid(PlayerCharacter))
	{
		PlayerCharacter->OnPlayerDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandlePlayerDied);
	}
}

void AfpstrueGameMode::UpdateCountdown()
{
	// 一秒 Timer 驱动倒计时并广播变化；HUD只在事件到达时刷新，不进行逐帧函数绑定。
	if (!bGameRunning)
	{
		return;
	}

	RemainingTime = FMath::Max(RemainingTime - 1, 0);
	OnRemainingTimeChanged.Broadcast(RemainingTime);

	if (RemainingTime <= 0 && IsPlayerAlive())
	{
		FinishGame(true);
	}
}

void AfpstrueGameMode::HandleEnemyDied(AfpstrueEnemyCharacter* DeadEnemy)
{
	// 敌人死亡和 Actor 销毁最终都进入同一个幂等注销入口。
	UnregisterEnemy(DeadEnemy);
}

void AfpstrueGameMode::HandleEnemyDestroyed(AActor* DestroyedActor)
{
	// Destroyed 委托给出 AActor，安全 Cast 成敌人后复用注销流程。
	UnregisterEnemy(Cast<AfpstrueEnemyCharacter>(DestroyedActor));
}

void AfpstrueGameMode::HandleEnemyEndPlay(AActor* EndingActor, EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 比 Destroyed 覆盖面更广；UnregisterEnemy 幂等，因此 Destroy 路径不会重复广播或重复释放。
	UnregisterEnemy(Cast<AfpstrueEnemyCharacter>(EndingActor));
}

void AfpstrueGameMode::HandlePlayerDied(AfpstrueCharacter* DeadPlayer)
{
	// 只响应当前登记玩家的死亡事件，避免无关角色结束本局游戏。
	if (bGameRunning && DeadPlayer == PlayerCharacter)
	{
		FinishGame(false);
	}
}

void AfpstrueGameMode::FinishGame(bool bPlayerWon)
{
	// 结算只允许一次：先冻结生成、计时与 AI，再广播最终结果，避免 UI 收到结果后世界状态仍继续变化。
	if (bGameEnded)
	{
		return;
	}

	bGameEnded = true;
	bGameRunning = false;
	ClearGameplayTimers();
	UnbindPlayerDeathEvent();
	StopActiveEnemies();
	if (SurroundManager)
	{
		SurroundManager->ResetManager();
	}
	UE_LOG(LogTemp, Log, TEXT("Game finished: %s. Remaining time: %d. Player health: %.1f"), bPlayerWon ? TEXT("Victory") : TEXT("Defeat"),
		   RemainingTime, IsValid(PlayerCharacter) ? PlayerCharacter->GetCurrentHealth() : 0.0f);
	OnGameResult.Broadcast(bPlayerWon);
}

void AfpstrueGameMode::ClearGameplayTimers()
{
	// 先阻止倒计时、生成和性能协调器继续产生回调，再取消可能仍在进行的基准采集。
	GetWorldTimerManager().ClearTimer(CountdownTimerHandle);
	GetWorldTimerManager().ClearTimer(WaveTimerHandle);
	ClearSpawnQueue();
	if (EnemySignificanceCoordinator != nullptr)
	{
		EnemySignificanceCoordinator->Stop();
	}
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->Stop();
	}
	if (BenchmarkRunner != nullptr)
	{
		BenchmarkRunner->Cancel();
	}
}
