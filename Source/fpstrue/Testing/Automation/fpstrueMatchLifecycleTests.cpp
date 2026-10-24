// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Automation/fpstrueTestObservers.h"

TFunction<void(AfpstrueSpawnReentryTestEnemy&)> AfpstrueSpawnReentryTestEnemy::OnTestBeginPlay;

void AfpstrueSpawnReentryTestEnemy::BeginPlay()
{
	Super::BeginPlay();
	if (OnTestBeginPlay) OnTestBeginPlay(*this);
}

bool AfpstrueSpawnTestNavigation::ProjectPoint(const FVector& Point, FNavLocation& OutLocation, const FVector& Extent,
	FSharedConstNavQueryFilter Filter, const UObject* Querier) const
{
	OutLocation = FNavLocation(Point, 1);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Game/fpstrueMultiplayer.h"
#include "GameMapsSettings.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "BrainComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NavigationSystem.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerMatchLifecycleTest,
	"fpstrue.Multiplayer.Lifecycle.SingleModeEnemyRegistrationAndSpawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 原波次测试中仍适用的生成/注销边界迁到真实多人入口，不保留单机倒计时和失败状态机。
bool FFpstrueMultiplayerMatchLifecycleTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Normal preview uses multiplayer rules"),
		UGameMapsSettings::GetGlobalDefaultGameMode(), FString(TEXT("/Script/fpstrue.fpstruePvpGameMode")));
	for (int32 Scenario = 0; Scenario < 3; ++Scenario)
	{
		FTestWorldWrapper Fixture;
		if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
		UWorld* World = Fixture.GetTestWorld();
		World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
		World->CreateAISystem();
		if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
		auto* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
		if (!TestNotNull(TEXT("Only multiplayer GameMode owns this world"), Mode)) return false;
		World->GetTimerManager().ClearTimer(Mode->PvePopulationTimer);
		Mode->EnemyClass = AfpstrueSpawnReentryTestEnemy::StaticClass();
		Mode->PveSpawnPoints.Add(World->SpawnActor<ATargetPoint>(FVector(3000.0f, 0, 100), FRotator::ZeroRotator));
		auto* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (Navigation == nullptr)
		{
			Navigation = NewObject<UNavigationSystemV1>(World);
			World->SetNavigationSystem(Navigation);
		}
		Navigation->MainNavData = World->SpawnActor<AfpstrueSpawnTestNavigation>();
		TStrongObjectPtr<AfpstrueSpawnReentryTestEnemy> Observed;
		TStrongObjectPtr<AfpstrueEnemyAIController> Controller;
		AfpstrueSpawnReentryTestEnemy::OnTestBeginPlay = [&](AfpstrueSpawnReentryTestEnemy& Enemy)
		{
			Observed.Reset(&Enemy);
			Controller.Reset(Cast<AfpstrueEnemyAIController>(Enemy.GetController()));
			if (Scenario == 1) UGameplayStatics::ApplyDamage(&Enemy, 100000.0f, nullptr, nullptr, nullptr);
			if (Scenario == 2) Mode->bEndingPvp = true;
		};
		ON_SCOPE_EXIT { AfpstrueSpawnReentryTestEnemy::OnTestBeginPlay = nullptr; };
		AfpstrueEnemyCharacter* Spawned = Mode->SpawnPveEnemy();
		if (!TestNotNull(TEXT("Spawn uses the real enemy BeginPlay"), Observed.Get())) return false;
		if (Scenario != 0)
		{
			TestNull(TEXT("Dead/ended startup never returns an active enemy"), Spawned);
			TestTrue(TEXT("Rejected startup is reclaimed"), Observed->IsActorBeingDestroyed());
			TestTrue(TEXT("Its controller cannot keep running"), !Controller.IsValid() || Controller->IsActorBeingDestroyed()
				|| !Controller->GetBrainComponent()->IsRunning());
			TestEqual(TEXT("Rejected startup never enters the registry"), Mode->PveEnemies.Num(), 0);
			continue;
		}
		if (!TestNotNull(TEXT("Healthy startup returns the enemy"), Spawned)) return false;
		Mode->RegisterPveEnemy(Spawned);
		Mode->RegisterPveEnemy(Spawned);
		TestEqual(TEXT("Repeated registration has one identity"), Mode->PveEnemies.Num(), 1);
		TestEqual(TEXT("Live population counts the registered enemy"), Mode->GetAliveEnemyCount(), 1);
		UGameplayStatics::ApplyDamage(Spawned, 100000.0f, nullptr, nullptr, nullptr);
		TestEqual(TEXT("Death immediately reduces living population"), Mode->GetAliveEnemyCount(), 0);
		TestEqual(TEXT("Corpse remains registered for EndPlay cleanup"), Mode->PveEnemies.Num(), 1);
		TestTrue(TEXT("Enemy death does not end the match"), Mode->GetGameState<AfpstruePvpGameState>()->IsMatchActive());
		Spawned->Destroy();
		Mode->HandlePveEnemyEndPlay(Spawned, EEndPlayReason::Destroyed);
		TestEqual(TEXT("Destruction and duplicate cleanup leave no entry"), Mode->PveEnemies.Num(), 0);
	}
	return true;
}

#endif
