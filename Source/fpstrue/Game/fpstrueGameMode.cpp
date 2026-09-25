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
 * 开局装配链：校验配置/出生点/玩家 -> 创建 SurroundManager -> 绑定玩家死亡 -> 提交 Playing 并开倒计时
 *          -> 启动 Animation Sharing 与 Significance -> 广播初始状态 -> 分帧生成第一波。
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

void AfpstrueGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	//停Timer、停止敌人、清空注册表、解绑玩家死亡事件、重置群体管理器
	// 先停止会继续产生回调的 Timer/AI，再解除 Delegate 和共享系统，最后交给 AGameModeBase。
	StopGameplay();
	ClearEnemyRegistrations();

	Super::EndPlay(EndPlayReason);
}

void AfpstrueGameMode::StartGameMode()
{
	// 所有不可恢复的配置错误都在创建 Timer/敌人前失败，避免只启动了一半的对局状态。
	// 在创建协作者或广播任何事件前占有启动流程，避免同步回调重入。
	if (MatchPhase != EFPMatchPhase::Waiting)
	{
		return;
	}
	MatchPhase = EFPMatchPhase::Starting;

	// 选中外部资产后不静默回退到旧蓝图；所有实际波次在创建 Timer/敌人前完成校验。
	const int32 ConfiguredWaveCount = GetConfiguredWaveCount();
	if ((WaveConfiguration != nullptr && WaveConfiguration->Waves.IsEmpty()) || ConfiguredWaveCount <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: wave configuration has no waves."));
		FinishGame(false);
		return;
	}
	for (int32 Wave = 1; Wave <= ConfiguredWaveCount; ++Wave)
	{
		if (!GetEnemyClassForWave(Wave))
		{
			UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: EnemyClass is not configured for wave %d."), Wave);
			FinishGame(false);
			return;
		}
	}

	// 世界配置：先收集，再校验；后续波次只复用缓存，不为每只敌人遍历世界。
	CacheSpawnPoints();
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
	//获取 PlayerCharacter，校验 HealthComponent 和存活状态
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
	// 玩家目标就绪后才能注入群体管理器，否则共享位置与槽位缓存无法初始化。
	if (!CreateSurroundManager())
	{
		UE_LOG(LogTemp, Error, TEXT("StartGameMode failed: SurroundManager could not be created."));
		FinishGame(false);
		return;
	}
	// SpawnActor 会调用管理器的蓝图 BeginPlay，若外部流程已终止本局，不能继续启动。
	if (MatchPhase != EFPMatchPhase::Starting)
	{
		return;
	}
	// 管理器的蓝图 BeginPlay 也可能改变玩家生命；绑定死亡事件前再确认当前事实。
	if (!IsPlayerAlive())
	{
		FinishGame(false);
		return;
	}
	//绑定玩家死亡状态
	BindPlayerDeathEvent();

	// 先提交完整状态，再启动协作者并广播；监听者读取到的是本局初始值。
	CurrentWave = 0;
	RemainingTime = GetConfiguredGameDuration();
	MatchPhase = EFPMatchPhase::Playing;
	UE_LOG(LogTemp, Display, TEXT("Game wave configuration: source=%s waves=%d duration=%d"),
		   WaveConfiguration != nullptr ? *GetPathNameSafe(WaveConfiguration) : TEXT("GameMode Blueprint defaults"), ConfiguredWaveCount,
		   RemainingTime);
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

	// 动态多播委托允许监听者结束本局；每次广播后只在当前局仍运行时继续。
	OnRemainingTimeChanged.Broadcast(RemainingTime);
	if (!IsRunning())
	{
		return;
	}
	OnWaveChanged.Broadcast(CurrentWave, ConfiguredWaveCount);
	if (!IsRunning())
	{
		return;
	}
	OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	if (!IsRunning())
	{
		return;
	}
	StartNextWave();
}

// ==================== 波次、生成与共享场景资源 ====================
// 收集关卡中带 EnemySpawnTag 的 TargetPoint，后续波次只复用缓存，避免每只敌人遍历世界。
void AfpstrueGameMode::CacheSpawnPoints()
{
	//一次性收集带 EnemySpawnTag 的 TargetPoint，后续波次只复用缓存，避免每只敌人遍历世界。
	SpawnPoints.Reset();
	UGameplayStatics::GetAllActorsOfClassWithTag(this, ATargetPoint::StaticClass(), EnemySpawnTag, SpawnPoints);
}

//创建群体管理器并注入当前玩家角色；若已存在有效实例则直接注入目标并返回成功，避免重复创建。
bool AfpstrueGameMode::CreateSurroundManager()
{
	// GameMode 创建唯一群体协调器并注入玩家目标；单个 AI 只保存对它的弱引用。
	//第一阶段:如果已经存在有效的 Manager 就直接注入目标并返回成功，避免重复创建。
	if (!IsValid(SurroundManager))
	{
		//第二阶段：检查 World 和 ManagerClass 是否有效，若无效则返回失败。
		UWorld* World = GetWorld();
		if (World == nullptr || !SurroundManagerClass)
		{
			return false;
		}

		//Spawn 出协调器
		SurroundManager = World->SpawnActor<AfpstrueSurroundManager>(SurroundManagerClass, FVector::ZeroVector, FRotator::ZeroRotator);
	}

	//检查协调器是否成功创建，若创建失败则返回失败。
	if (!IsValid(SurroundManager))
	{
		return false;
	}
	//注入目标角色（PlayerCharacter）
	// GameMode 负责共享目标的生命周期，避免每个 AIController 重复写入同一状态。
	SurroundManager->SetTargetCharacter(PlayerCharacter);
	return true;
}

//波次推进
//总共几波
int32 AfpstrueGameMode::GetConfiguredWaveCount() const
{
	// 自动压测固定为一个指定规模波次；正常游戏先读外部资产，未指定资产时才读旧 WaveConfigs（压测直接一波生成）。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.HasEnemyCountOverride())
	{
		return 1;
	}

	if (WaveConfiguration != nullptr)
	{
		return WaveConfiguration->Waves.Num();
	}
	return WaveConfigs.IsEmpty() ? TotalWaves : WaveConfigs.Num();
}

// 第 N 波敌人数量查询；旧蓝图未配置显式波次时使用 Base + (N - 1) * Added。
int32 AfpstrueGameMode::GetEnemyCountForWave(int32 WaveNumber) const
{
	// 将基准覆盖、数据化波次和旧式线性增长三种来源收口成一个敌人数查询入口。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.HasEnemyCountOverride())
	{
		return BenchmarkConfig.EnemyCount;
	}

	const int32 WaveIndex = WaveNumber - 1;
	const TArray<FfpstrueWaveConfig>& Waves = WaveConfiguration != nullptr ? WaveConfiguration->Waves : WaveConfigs;
	if (Waves.IsValidIndex(WaveIndex))
	{
		return FMath::Max(Waves[WaveIndex].EnemyCount, 1);
	}

	return WaveConfiguration != nullptr ? 0 : BaseEnemiesPerWave + WaveIndex * EnemiesAddedPerWave;
}

// 波次专用敌人类只回退到当前配置来源的默认类，不跨资产与旧蓝图混用。
TSubclassOf<AfpstrueEnemyCharacter> AfpstrueGameMode::GetEnemyClassForWave(int32 WaveNumber) const
{
	// 当前波次有专用敌人类时使用专用配置，否则回退到默认 EnemyClass。
	const int32 WaveIndex = WaveNumber - 1;
	const TArray<FfpstrueWaveConfig>& Waves = WaveConfiguration != nullptr ? WaveConfiguration->Waves : WaveConfigs;
	if (Waves.IsValidIndex(WaveIndex) && Waves[WaveIndex].EnemyClass)
	{
		return Waves[WaveIndex].EnemyClass;
	}
	if (WaveConfiguration != nullptr && !Waves.IsValidIndex(WaveIndex))
	{
		return nullptr;
	}

	return WaveConfiguration != nullptr ? WaveConfiguration->DefaultEnemyClass : EnemyClass;
}

float AfpstrueGameMode::GetConfiguredWaveInterval() const
{
	return FMath::Max(WaveConfiguration != nullptr ? WaveConfiguration->WaveInterval : WaveInterval, 0.0f);
}

int32 AfpstrueGameMode::GetConfiguredGameDuration() const
{
	return FMath::Max(WaveConfiguration != nullptr ? WaveConfiguration->GameDuration : GameDuration, 1);
}

// 推进波次编号、广播 UI 事件并启动本波生成。
void AfpstrueGameMode::StartNextWave()
{
	//波次状态先提交并广播，再建立生成队列；HUD 看到的波次编号始终与即将生成的配置一致。
	//配置波次数量优先读取所选资产，再读旧 WaveConfigs；旧配置为空时回退到 TotalWaves，Benchmark 固定为 1 波。
	const int32 ConfiguredWaveCount = GetConfiguredWaveCount();
	//运行时状态检查：游戏未开始或已到达最后一波时不再生成新波次。
	if (!IsRunning() || CurrentWave >= ConfiguredWaveCount)
	{
		return;
	}

	// 等待上一波生成队列完成；存活敌人可以跨波存在，不按清场条件推进。
	if (PendingEnemySpawnCount > 0)
	{
		GetWorldTimerManager().SetTimer(WaveTimerHandle, this, &AfpstrueGameMode::StartNextWave, FMath::Max(SpawnInterval, 0.01f), false);
		return;
	}

	//正式进入下一波，广播波次变化事件，开始分帧生成。
	++CurrentWave;
	OnWaveChanged.Broadcast(CurrentWave, ConfiguredWaveCount);
	if (!IsRunning())
	{
		return;
	}

	//生成当前波次的敌人，分帧生成由 SpawnNextQueuedEnemy 处理。
	SpawnCurrentWave();

	// 未到最后一波时安排下一波；当前模式以存活到倒计时结束为胜利条件。
	if (IsRunning() && CurrentWave < ConfiguredWaveCount)
	{
		// 零间隔表示下一帧，不传零给 SetTimer（零值会取消 Timer）。
		const float Interval = GetConfiguredWaveInterval();
		if (Interval > 0.0f)
		{
			GetWorldTimerManager().SetTimer(WaveTimerHandle, this, &AfpstrueGameMode::StartNextWave, Interval, false);
		}
		else
		{
			WaveTimerHandle = GetWorldTimerManager().SetTimerForNextTick(this, &AfpstrueGameMode::StartNextWave);
		}
	}
}

//分帧生成敌人
void AfpstrueGameMode::SpawnCurrentWave()
{
	// 这里准备队列并立即生成首个敌人，其余由 Timer 每次消费一个，平滑创建尖峰。
	//性能分析：记录本次队列准备和首个生成调用的耗时，不是整波跨帧历时；后续生成另有相同 Stat 和独立 Trace 作用域。
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnCurrentWave);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);
	// 运行时状态检查：游戏未开始或没有可用出生点时不再生成新波次。
	if (!IsRunning() || SpawnPoints.IsEmpty())
	{
		return;
	}

	// 清空上波的生成队列，避免残留点位和类引用影响本波生成。
	ClearSpawnQueue();
	// 随机化出生点顺序，避免每波敌人都从同一位置开始生成。
	QueuedSpawnPoints = SpawnPoints;
	for (int32 Index = QueuedSpawnPoints.Num() - 1; Index > 0; --Index)
	{
		const int32 SwapIndex = FMath::RandRange(0, Index);
		QueuedSpawnPoints.Swap(Index, SwapIndex);
	}

	// 计算本波敌人数量和类引用，供 SpawnNextQueuedEnemy 使用。
	PendingEnemySpawnCount = GetEnemyCountForWave(CurrentWave);
	QueuedEnemyClass = GetEnemyClassForWave(CurrentWave);

	//立即生成首个敌人，后续由 Timer 分帧生成。
	SpawnNextQueuedEnemy();
	//生成第一个后启用定时器
	if (IsRunning() && PendingEnemySpawnCount > 0)
	{
		GetWorldTimerManager().SetTimer(SpawnTimerHandle, this, &AfpstrueGameMode::SpawnNextQueuedEnemy, FMath::Max(SpawnInterval, 0.01f),
										true);
	}
}

// Timer 每次只生成一个敌人，降低集中 Spawn 峰值；失败时换点重试，连续失败达到上限后终止队列。
void AfpstrueGameMode::SpawnNextQueuedEnemy()
{
	// 每次 Timer 只消费一个生成请求；失败时换点重试，连续失败达到上限后终止队列。
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_SpawnQueuedEnemy);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueWaveSpawnTime);

	// 运行时状态检查：游戏未开始、没有待生成敌人、没有可用出生点或没有有效敌人类时停止分帧生成。
	if (!IsRunning() || PendingEnemySpawnCount <= 0 || QueuedSpawnPoints.IsEmpty() || !QueuedEnemyClass)
	{
		ClearSpawnQueue();
		return;
	}

	// 计算当前生成点索引和重用次数，供 SpawnEnemyAtPoint 使用
	//敌人堆积优化
	const int32 SpawnPointCount = QueuedSpawnPoints.Num();
	const int32 SpawnPointIndex = NextQueuedSpawnIndex % SpawnPointCount;
	const int32 SpawnPointReuseCount = NextQueuedSpawnIndex / SpawnPointCount;
	const bool bSpawnSucceeded = SpawnEnemyAtPoint(QueuedSpawnPoints[SpawnPointIndex], SpawnPointReuseCount, QueuedEnemyClass);
	// Spawn/注册会同步广播存活数量；若监听者结算本局，队列已经被清空，不再写回旧队列状态。
	if (!IsRunning())
	{
		return;
	}

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

//具体位置生成敌人：在给定出生点附近寻找可导航位置，生成敌人并注入 AI 上下文；单次 Spawn 成功后才注入目标并注册。
bool AfpstrueGameMode::SpawnEnemyAtPoint(AActor* SpawnPoint, int32 SpawnPointReuseCount, TSubclassOf<AfpstrueEnemyCharacter> WaveEnemyClass)
{
	// 安全检查（出生点，world，导航系统）
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
		////Navmesh投影
		FNavLocation ProjectedLocation;
		if (SpawnPointReuseCount > 0 || Attempt > 0)
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

		//计算最终生成位置（敌人卡住）
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
		//最终生成
		SpawnedEnemy = World->SpawnActor<AfpstrueEnemyCharacter>(WaveEnemyClass, SpawnTransform, SpawnParameters);
		// Construction/BeginPlay 可以同步结算；即使回调同时销毁敌人，也不能继续下一次重试。
		if (!IsRunning())
		{
			break;
		}
	}

	//生成成功后注入目标并注册；失败时记录错误日志。
	// 单次 Spawn 成功后才注入 AI 上下文并注册；任一步失败都不增加 Alive 计数。Benchmark 开关由敌人初始化及 Runner 采集入口处理。
	if (SpawnedEnemy == nullptr)
	{
		if (IsRunning())
		{
			UE_LOG(LogTemp, Error, TEXT("SpawnActor failed for enemy class %s at %s after %d reuse(s), sample radius %.0f."),
				   *GetNameSafe(WaveEnemyClass.Get()), *GetNameSafe(SpawnPoint), SpawnPointReuseCount, RetryRadius);
		}
		return false;
	}

	const auto CanActivateSpawnedEnemy = [this, SpawnedEnemy]()
	{
		if (IsRunning() && IsValid(SpawnedEnemy) && !SpawnedEnemy->IsDead())
		{
			return true;
		}
		// 未登记的敌人漏过了结算时的注册表遍历；补停其 Possess 自动启动的 BT。
		if (IsValid(SpawnedEnemy))
		{
			if (AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(SpawnedEnemy->GetController()))
			{
				Controller->StopAI();
			}
		}
		return false;
	};
	if (!CanActivateSpawnedEnemy())
	{
		return false;
	}

	if (SpawnedEnemy->GetController() == nullptr)
	{
		SpawnedEnemy->SpawnDefaultController();
	}
	// Controller 的 BeginPlay/OnPossess 也是外部回调边界，不能只检查 Pawn 的 Spawn。
	if (!CanActivateSpawnedEnemy())
	{
		return false;
	}

	INC_DWORD_STAT(STAT_fpstrueEnemySpawnCount);
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

// ==================== 敌人注册表与协调器连接 ====================

void AfpstrueGameMode::RegisterEnemy(AfpstrueEnemyCharacter* Enemy)
{
	// 注册表只持有弱引用，同时绑定死亡/EndPlay/销毁出口并接入显著性与动画共享协调器。
	// 注册表是 GameMode 对“当前存活参与者”的唯一视图；Contains 命中时不重复绑定事件或累计数量。
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

	if (IsRunning())
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
	DisconnectEnemy(Enemy);

	if (IsRunning())
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::DisconnectEnemy(AfpstrueEnemyCharacter* Enemy)
{
	Enemy->OnEnemyDeathReported.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDied);
	Enemy->OnEndPlay.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyEndPlay);
	Enemy->OnDestroyed.RemoveDynamic(this, &AfpstrueGameMode::HandleEnemyDestroyed);
	if (EnemyAnimationSharingCoordinator != nullptr)
	{
		EnemyAnimationSharingCoordinator->SuspendEnemy(Enemy);
	}
	Enemy->SetAnimationSharingCoordinator(nullptr);
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

	if (IsRunning() && RegisteredEnemies.Num() != PreviousCount)
	{
		OnAliveEnemyCountChanged.Broadcast(RegisteredEnemies.Num());
	}
}

void AfpstrueGameMode::ClearEnemyRegistrations()
{
	// EndPlay 阶段批量解除敌人委托和协调器引用，最后清空弱引用集合。
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		if (AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get())
		{
			DisconnectEnemy(Enemy);
		}
	}
	RegisteredEnemies.Reset();
}

// ==================== 游戏状态、事件与计时器 ====================

bool AfpstrueGameMode::IsPlayerAlive() const
{
	// 对局胜负只读取玩家角色的统一死亡查询，不自行缓存第二份生命状态。
	return IsValid(PlayerCharacter) && !PlayerCharacter->IsDead();
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
	if (!IsRunning())
	{
		return;
	}

	RemainingTime = FMath::Max(RemainingTime - 1, 0);
	OnRemainingTimeChanged.Broadcast(RemainingTime);

	if (IsRunning() && RemainingTime <= 0 && IsPlayerAlive())
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
	if (IsRunning() && DeadPlayer == PlayerCharacter)
	{
		FinishGame(false);
	}
}

void AfpstrueGameMode::FinishGame(bool bPlayerWon)
{
	// 结算只允许一次：先冻结生成、计时与 AI，再广播最终结果，避免 UI 收到结果后世界状态仍继续变化。
	if (IsFinished())
	{
		return;
	}
	StopGameplay();
	UE_LOG(LogTemp, Log, TEXT("Game finished: %s. Remaining time: %d. Player health: %.1f"), bPlayerWon ? TEXT("Victory") : TEXT("Defeat"),
		   RemainingTime, IsValid(PlayerCharacter) ? PlayerCharacter->GetCurrentHealth() : 0.0f);
	OnGameResult.Broadcast(bPlayerWon);
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

void AfpstrueGameMode::StopGameplay()
{
	// 先提交终态；下方清理触发的同步回调不得重新生成敌人或再次结算。
	MatchPhase = EFPMatchPhase::Finished;
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
	UnbindPlayerDeathEvent();
	StopActiveEnemies();
	if (SurroundManager)
	{
		SurroundManager->ResetManager();
	}
}
