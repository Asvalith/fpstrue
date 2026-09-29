// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Automation/fpstrueGameModeTestObserver.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Game/fpstrueGameMode.h"
#include "Kismet/GameplayStatics.h"

TFunction<void(AfpstrueSpawnReentryTestEnemy&)> AfpstrueSpawnReentryTestEnemy::OnTestBeginPlay;

void AfpstrueSpawnReentryTestEnemy::BeginPlay()
{
	Super::BeginPlay();
	if (OnTestBeginPlay)
		OnTestBeginPlay(*this);
}

bool AfpstrueSpawnTestNavigation::ProjectPoint(const FVector& Point, FNavLocation& OutLocation, const FVector& Extent,
											   FSharedConstNavQueryFilter Filter, const UObject* Querier) const
{
	OutLocation = FNavLocation(Point, 1);
	return true;
}

void UfpstrueGameModeTestObserver::HandleTime(int32 RemainingTime)
{
	if (TimeEvents++ == 0)
	{
		InitialTime = RemainingTime;
		bSawRunningState = GameMode.IsValid() && GameMode->IsRunning();
	}
	if (bReenterStart && GameMode.IsValid())
	{
		bReenterStart = false;
		GameMode->StartGameMode();
	}
	if (bKillOnInitialTime && Player.IsValid())
	{
		bKillOnInitialTime = false;
		UGameplayStatics::ApplyDamage(Player.Get(), 100000.0f, nullptr, nullptr, nullptr);
	}
}

void UfpstrueGameModeTestObserver::HandleWave(int32 CurrentWave, int32 TotalWaves)
{
	++WaveEvents;
	if (bKillOnFirstWave && CurrentWave == 1 && Player.IsValid())
	{
		bKillOnFirstWave = false;
		UGameplayStatics::ApplyDamage(Player.Get(), 100000.0f, nullptr, nullptr, nullptr);
	}
}

void UfpstrueGameModeTestObserver::HandleAlive(int32 AliveEnemies)
{
	++AliveEvents;
}
void UfpstrueGameModeTestObserver::HandleResult(bool bPlayerWon)
{
	++ResultEvents;
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/Performance/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "BrainComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "Misc/AutomationTest.h"
#include "NavigationSystem.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
// 使用真正的项目 GameMode 与 PlayerController，运行真实 StartGameMode。
// 不加载地图/动画资产；导航生成由独立真实地图烟测覆盖。
bool PrepareGameModeWorld(FTestWorldWrapper& Wrapper, FAutomationTestBase& Test, AfpstrueGameMode*& OutGameMode,
						  AfpstrueCharacter*& OutPlayer)
{
	if (!Wrapper.CreateTestWorld(EWorldType::Game))
	{
		Wrapper.ForwardErrorMessages(&Test);
		return false;
	}
	UWorld* World = Wrapper.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AfpstrueGameMode::StaticClass();
	World->CreateAISystem();
	if (!Wrapper.BeginPlayInTestWorld())
	{
		Wrapper.ForwardErrorMessages(&Test);
		return false;
	}
	OutGameMode = Cast<AfpstrueGameMode>(World->GetAuthGameMode());
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	OutPlayer = World->SpawnActor<AfpstrueCharacter>();
	ATargetPoint* SpawnPoint = World->SpawnActor<ATargetPoint>();
	if (!Test.TestNotNull(TEXT("Project GameMode exists"), OutGameMode) ||
		!Test.TestNotNull(TEXT("Player controller exists"), Controller) || !Test.TestNotNull(TEXT("Player exists"), OutPlayer) ||
		!Test.TestNotNull(TEXT("Spawn point exists"), SpawnPoint))
	{
		return false;
	}
	// 原生角色没有蓝图输入资产；为真实 Possess/SetupPlayerInputComponent 提供临时配置，
	// 不屏蔽生产代码的缺配置错误，也不载入项目资源。
	for (const TCHAR* ActionName : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"), TEXT("AimAction"),
									TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* ActionProperty = FindFProperty<FObjectPropertyBase>(OutPlayer->GetClass(), ActionName);
		if (!Test.TestNotNull(TEXT("Player input action property exists"), ActionProperty))
		{
			return false;
		}
		ActionProperty->SetObjectPropertyValue_InContainer(OutPlayer, NewObject<UInputAction>(OutPlayer));
	}
	Controller->Possess(OutPlayer);
	OutPlayer->GetCharacterMovement()->SetComponentTickEnabled(false);
	SpawnPoint->Tags.Add(TEXT("EnemySpawn"));
	SpawnPoint->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	UfpstrueEnemyAnimationSharingCoordinator* Sharing = OutGameMode->FindComponentByClass<UfpstrueEnemyAnimationSharingCoordinator>();
	FBoolProperty* SharingEnabled =
		Sharing != nullptr ? FindFProperty<FBoolProperty>(Sharing->GetClass(), TEXT("bEnableAnimationSharing")) : nullptr;
	if (!Test.TestNotNull(TEXT("Sharing can be disabled without loading animation assets"), SharingEnabled))
	{
		return false;
	}
	SharingEnabled->SetPropertyValue_InContainer(Sharing, false);
	return true;
}

void BindObserver(UfpstrueGameModeTestObserver* Observer, AfpstrueGameMode* GameMode, AfpstrueCharacter* Player)
{
	Observer->GameMode = GameMode;
	Observer->Player = Player;
	GameMode->OnRemainingTimeChanged.AddDynamic(Observer, &UfpstrueGameModeTestObserver::HandleTime);
	GameMode->OnWaveChanged.AddDynamic(Observer, &UfpstrueGameModeTestObserver::HandleWave);
	GameMode->OnAliveEnemyCountChanged.AddDynamic(Observer, &UfpstrueGameModeTestObserver::HandleAlive);
	GameMode->OnGameResult.AddDynamic(Observer, &UfpstrueGameModeTestObserver::HandleResult);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueGameModeStartupTest, "fpstrue.Gameplay.GameMode.StartupReentryAndCleanup",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueGameModeStartupTest::RunTest(const FString& Parameters)
{
	// 正常启动、初始时间广播中死亡、首波广播中死亡分别覆盖三种调用边界。
	for (int32 Scenario = 0; Scenario < 3; ++Scenario)
	{
		FTestWorldWrapper World;
		AfpstrueGameMode* GameMode = nullptr;
		AfpstrueCharacter* Player = nullptr;
		if (!PrepareGameModeWorld(World, *this, GameMode, Player))
		{
			return false;
		}
		GameMode->MinimumSpawnPointCount = 1;
		GameMode->EnemyClass = AfpstrueEnemyCharacter::StaticClass();
		GameMode->TotalWaves = 2;
		GameMode->BaseEnemiesPerWave = 1;
		GameMode->GameDuration = 37;
		GameMode->SpawnInterval = 1000.0f;
		GameMode->WaveInterval = 1000.0f;
		TStrongObjectPtr<UfpstrueGameModeTestObserver> Observer(NewObject<UfpstrueGameModeTestObserver>());
		BindObserver(Observer.Get(), GameMode, Player);
		Observer->bReenterStart = true;
		Observer->bKillOnInitialTime = Scenario == 1;
		Observer->bKillOnFirstWave = Scenario == 2;

		TestFalse(TEXT("New match is not running"), GameMode->IsRunning());
		GameMode->MatchPhase = EFPMatchPhase::Starting;
		GameMode->StartGameMode();
		TestEqual(TEXT("Starting guard does not broadcast or start another transaction"), Observer->TimeEvents, 0);
		TestTrue(TEXT("Starting guard preserves phase"), GameMode->MatchPhase == EFPMatchPhase::Starting);
		GameMode->MatchPhase = EFPMatchPhase::Waiting;

		if (Scenario == 0)
		{
			// 本临时世界没有导航数据；只预期首次生成失败，不能把该错误吞成实际生成验收。
			AddExpectedError(FNavigationSystem::GetCurrent<UNavigationSystemV1>(World.GetTestWorld()) == nullptr
								 ? TEXT("Enemy spawn requires a valid navigation system.")
								 : TEXT("SpawnActor failed for enemy class"),
							 EAutomationExpectedErrorFlags::Contains, 1);
		}
		GameMode->StartGameMode();
		TestEqual(TEXT("Spawn points were cached before validation"), GameMode->SpawnPoints.Num(), 1);
		TestEqual(TEXT("Initial HUD event sees configured duration"), Observer->InitialTime, 37);
		TestTrue(TEXT("Initial HUD event sees committed running state"), Observer->bSawRunningState);
		TestEqual(TEXT("Reentrant Start does not repeat initial HUD event"), Observer->TimeEvents, 1);
		TestNotNull(TEXT("Surround manager was created"), GameMode->SurroundManager.Get());

		if (Scenario == 0)
		{
			TestTrue(TEXT("Real StartGameMode entered Playing"), GameMode->IsRunning());
			FVector SharedTarget;
			TestTrue(TEXT("Surround manager received the valid player before startup"),
					 GameMode->SurroundManager->GetSharedTargetSnapshot(SharedTarget));
			TestTrue(TEXT("Shared target is the player location"), SharedTarget.Equals(Player->GetActorLocation()));
			TestEqual(TEXT("Initial wave and first wave each broadcast once"), Observer->WaveEvents, 2);
			TestTrue(TEXT("Countdown timer is active"), GameMode->GetWorldTimerManager().TimerExists(GameMode->CountdownTimerHandle));
			TestTrue(TEXT("Spawn retry queue remains scheduled"), GameMode->GetWorldTimerManager().TimerExists(GameMode->SpawnTimerHandle));
			GameMode->StartGameMode();
			TestEqual(TEXT("Repeated Start while Playing does nothing"), Observer->TimeEvents, 1);
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AfpstrueEnemyCharacter* Enemy = World.GetTestWorld()->SpawnActor<AfpstrueEnemyCharacter>(
				AfpstrueEnemyCharacter::StaticClass(), FVector(2000.0f, 0.0f, 100.0f), FRotator::ZeroRotator, SpawnParameters);
			if (!TestNotNull(TEXT("A native enemy exists for registry lifecycle"), Enemy))
			{
				return false;
			}
			GameMode->RegisterEnemy(Enemy);
			GameMode->RegisterEnemy(Enemy);
			TestEqual(TEXT("Enemy registration is unique"), GameMode->RegisteredEnemies.Num(), 1);
			GameMode->FinishGame(false);
		}
		else
		{
			TestEqual(TEXT("No enemy is generated after a terminating broadcast"), GameMode->RegisteredEnemies.Num(), 0);
			TestEqual(TEXT("Terminated initial event does not continue to wave broadcasts"), Observer->WaveEvents, Scenario == 1 ? 0 : 2);
		}

		TestTrue(TEXT("Match is finished"), GameMode->IsFinished());
		TestFalse(TEXT("Finished and running cannot both be true"), GameMode->IsRunning());
		TestEqual(TEXT("Finish clears pending spawn count"), GameMode->PendingEnemySpawnCount, 0);
		TestFalse(TEXT("Countdown cleared"), GameMode->GetWorldTimerManager().TimerExists(GameMode->CountdownTimerHandle));
		TestFalse(TEXT("Wave timer not re-created by old call stack"),
				  GameMode->GetWorldTimerManager().TimerExists(GameMode->WaveTimerHandle));
		TestFalse(TEXT("Spawn timer cleared"), GameMode->GetWorldTimerManager().TimerExists(GameMode->SpawnTimerHandle));
		TestFalse(TEXT("Player death delegate unbound"),
				  Player->OnPlayerDeathReported.Contains(GameMode, GET_FUNCTION_NAME_CHECKED(AfpstrueGameMode, HandlePlayerDied)));
		GameMode->FinishGame(true);
		GameMode->StartGameMode();
		TestEqual(TEXT("Repeated finish/start cannot revive match or rebroadcast result"), Observer->ResultEvents, 1);
		World.EndPlayInTestWorld();
		TestEqual(TEXT("EndPlay clears registry"), GameMode->RegisteredEnemies.Num(), 0);
		TestEqual(TEXT("EndPlay does not emit another match result"), Observer->ResultEvents, 1);
	}

	// 补测真正的敌人 SpawnActor/BeginPlay 回调；不是在已登记的敌人上直接调用 FinishGame。
	// 导航仅提供确定性投影点，不把这项单测当作 NavMesh/可达性验收。
	for (int32 Scenario = 0; Scenario < 2; ++Scenario)
	{
		FTestWorldWrapper World;
		AfpstrueGameMode* GameMode = nullptr;
		AfpstrueCharacter* Player = nullptr;
		if (!PrepareGameModeWorld(World, *this, GameMode, Player))
			return false;
		UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World.GetTestWorld());
		if (NavSystem == nullptr)
		{
			NavSystem = NewObject<UNavigationSystemV1>(World.GetTestWorld());
			World.GetTestWorld()->SetNavigationSystem(NavSystem);
		}
		NavSystem->MainNavData = World.GetTestWorld()->SpawnActor<AfpstrueSpawnTestNavigation>();
		if (!TestNotNull(TEXT("Deterministic spawn projection fixture"), NavSystem->MainNavData.Get()))
			return false;
		GameMode->MinimumSpawnPointCount = 1;
		GameMode->EnemyClass = AfpstrueSpawnReentryTestEnemy::StaticClass();
		GameMode->TotalWaves = 1;
		GameMode->BaseEnemiesPerWave = 1;
		GameMode->SpawnInterval = 1000.0f;
		TWeakObjectPtr<AfpstrueSpawnReentryTestEnemy> SpawnedEnemy;
		TWeakObjectPtr<AfpstrueEnemyAIController> SpawnedController;
		bool bBrainWasRunningBeforeCallback = false;
		AfpstrueSpawnReentryTestEnemy::OnTestBeginPlay = [&](AfpstrueSpawnReentryTestEnemy& Enemy)
		{
			SpawnedEnemy = &Enemy;
			SpawnedController = Cast<AfpstrueEnemyAIController>(Enemy.GetController());
			bBrainWasRunningBeforeCallback = SpawnedController.IsValid() && SpawnedController->GetBrainComponent() != nullptr &&
											 SpawnedController->GetBrainComponent()->IsRunning();
			// 第一种杀死玩家同步结算；第二种敌人自身死亡，仍不应该加入存活注册表。
			UGameplayStatics::ApplyDamage(Scenario == 0 ? static_cast<AActor*>(Player) : &Enemy, 100000.0f, nullptr, nullptr, nullptr);
		};
		GameMode->StartGameMode();
		AfpstrueSpawnReentryTestEnemy::OnTestBeginPlay = nullptr;
		if (!TestTrue(TEXT("Production spawn reached the enemy BeginPlay callback"), SpawnedEnemy.IsValid()) ||
			!TestTrue(TEXT("Possess had already started a real behavior brain"), bBrainWasRunningBeforeCallback) ||
			!TestTrue(TEXT("Spawned controller is available for cleanup assertions"), SpawnedController.IsValid()))
			return false;
		TestFalse(TEXT("The unregistered spawned enemy cannot keep its behavior tree running"),
				  SpawnedController->GetBrainComponent()->IsRunning());
		TestNull(TEXT("No target is injected after a terminating spawn callback"), SpawnedController->GetTargetCharacter());
		TestEqual(TEXT("Ended/dead spawn is not registered as a living participant"), GameMode->RegisteredEnemies.Num(), 0);
		if (Scenario == 0)
		{
			TestTrue(TEXT("Enemy BeginPlay synchronously finished the match"), GameMode->IsFinished());
			TestEqual(TEXT("The terminated spawn queue stays cleared"), GameMode->PendingEnemySpawnCount, 0);
			TestFalse(TEXT("The terminated spawn does not schedule another timer"),
					  GameMode->GetWorldTimerManager().TimerExists(GameMode->SpawnTimerHandle));
		}
		else
		{
			TestTrue(TEXT("Enemy self-damage reached real death"), SpawnedEnemy->IsDead());
			TestEqual(TEXT("A dead spawn does not consume a valid pending enemy"), GameMode->PendingEnemySpawnCount, 1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWaveConfigurationTest, "fpstrue.Gameplay.GameMode.WaveConfigurationAuthority",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueWaveConfigurationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper World;
	AfpstrueGameMode* GameMode = nullptr;
	AfpstrueCharacter* Player = nullptr;
	if (!PrepareGameModeWorld(World, *this, GameMode, Player))
	{
		return false;
	}
	GameMode->MinimumSpawnPointCount = 1;
	GameMode->EnemyClass = AfpstrueEnemyCharacter::StaticClass();
	TestEqual(TEXT("Legacy wave count retained without asset"), GameMode->GetConfiguredWaveCount(), 3);
	TestEqual(TEXT("Legacy growth retained without asset"), GameMode->GetWaveConfig(2).EnemyCount, 7);
	GameMode->WaveConfigs.SetNum(1);
	GameMode->WaveConfigs[0].EnemyCount = 23;
	TestEqual(TEXT("Legacy explicit wave count replaces linear count"), GameMode->GetConfiguredWaveCount(), 1);
	TestEqual(TEXT("Legacy explicit enemy count is read"), GameMode->GetWaveConfig(1).EnemyCount, 23);
	UfpstrueWaveConfiguration* Config = NewObject<UfpstrueWaveConfiguration>(GameMode);
	Config->Waves.SetNum(2);
	Config->Waves[0].EnemyCount = 11;
	Config->Waves[1].EnemyCount = 17;
	Config->GameDuration = 61;
	Config->WaveInterval = 0.0f;
	GameMode->WaveConfiguration = Config;
	TestEqual(TEXT("Asset determines complete wave count"), GameMode->GetConfiguredWaveCount(), 2);
	TestEqual(TEXT("Asset determines wave enemy count"), GameMode->GetWaveConfig(2).EnemyCount, 17);
	TestEqual(TEXT("Invalid asset wave does not use legacy linear growth"), GameMode->GetWaveConfig(3).EnemyCount, 0);
	TestFalse(TEXT("Missing asset class must not silently use legacy class"), bool(GameMode->GetWaveConfig(1).EnemyClass));
	Config->DefaultEnemyClass = AfpstrueEnemyCharacter::StaticClass();
	TestTrue(TEXT("Asset default enemy class is used"), GameMode->GetWaveConfig(1).EnemyClass == Config->DefaultEnemyClass);
	TestFalse(TEXT("Invalid asset wave has no class even with an asset default"), bool(GameMode->GetWaveConfig(3).EnemyClass));
	TestEqual(TEXT("Asset interval is authoritative including zero"), GameMode->GetConfiguredWaveInterval(), 0.0f);
	TStrongObjectPtr<UfpstrueGameModeTestObserver> Observer(NewObject<UfpstrueGameModeTestObserver>());
	BindObserver(Observer.Get(), GameMode, Player);
	Observer->bKillOnInitialTime = true;
	GameMode->StartGameMode();
	TestEqual(TEXT("Real Start publishes asset duration, not legacy value"), Observer->InitialTime, 61);
	TestTrue(TEXT("Real asset startup reached Playing before listener ended it"), Observer->bSawRunningState);
	TestTrue(TEXT("Listener can finish asset-configured match"), GameMode->IsFinished());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueGameModeParticipantBoundaryTest, "fpstrue.Gameplay.GameMode.ParticipantDeparture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueGameModeParticipantBoundaryTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	AfpstrueGameMode* GameMode = nullptr;
	AfpstrueCharacter* Player = nullptr;
	if (!PrepareGameModeWorld(Fixture, *this, GameMode, Player))
		return false;
	// 不开生成/导航：验证已登记敌人的销毁/离场注销，以及玩家 EndPlay 结束对局。
	GameMode->PlayerCharacter = Player;
	GameMode->MatchPhase = EFPMatchPhase::Playing;
	GameMode->RemainingTime = 30;
	GameMode->BindPlayerDeathEvent();
	TStrongObjectPtr<UfpstrueGameModeTestObserver> Observer(NewObject<UfpstrueGameModeTestObserver>());
	BindObserver(Observer.Get(), GameMode, Player);
	// 注册发生在 BeginPlay 之后，EndPlay 同时覆盖直接销毁和关卡移除；不依赖冗余 OnDestroyed 绑定。
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	for (const bool bDestroy : {true, false})
	{
		AfpstrueEnemyCharacter* Enemy = Fixture.GetTestWorld()->SpawnActor<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform(FVector(2000.0f, 0.0f, 0.0f)), SpawnParameters);
		if (!TestNotNull(TEXT("Enemy participant"), Enemy)) return false;
		GameMode->RegisterEnemy(Enemy);
		const int32 EventsBeforeDeparture = Observer->AliveEvents;
		if (bDestroy) Enemy->Destroy();
		else Enemy->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
		TestEqual(TEXT("Both departure paths clear the registry"), GameMode->RegisteredEnemies.Num(), 0);
		TestEqual(TEXT("Departure publishes once"), Observer->AliveEvents, EventsBeforeDeparture + 1);
		GameMode->UnregisterEnemy(Enemy);
		TestEqual(TEXT("Late duplicate unregister is inert"), Observer->AliveEvents, EventsBeforeDeparture + 1);
	}
	Player->Destroy();
	TestTrue(TEXT("Direct player destruction ends Playing"), GameMode->IsFinished());
	TestEqual(TEXT("Departure broadcasts one result"), Observer->ResultEvents, 1);
	GameMode->UpdateCountdown();
	TestEqual(TEXT("Countdown cannot finish the match again"), Observer->ResultEvents, 1);
	GameMode->BenchmarkEnemyCountOverride = 0;
	TestEqual(TEXT("Explicit zero enemy override remains meaningful"), GameMode->GetWaveConfig(1).EnemyCount, 0);
	TestEqual(TEXT("Override produces one benchmark wave"), GameMode->GetConfiguredWaveCount(), 1);
	GameMode->BenchmarkEnemyCountOverride = INDEX_NONE;
	TestEqual(TEXT("Removing override restores authored wave count"), GameMode->GetConfiguredWaveCount(), 3);
	return true;
}

#endif
