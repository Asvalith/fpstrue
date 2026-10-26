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

#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpGameState.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstruePvpPlayerState.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
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

		// 正常编辑器本地预览也必须走同一死亡事务。首个断言在任何手动规则调用前验证真实伤害已登记。
		TestEqual(TEXT("The normal preview fixture really runs in Standalone"), World->GetNetMode(), NM_Standalone);
		TestEqual(TEXT("Preview uses the production template Blueprint rather than a legacy solo class"),
			Mode->DefaultPawnClass->GetPathName(), FString(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C")));
		Mode->RespawnDelay = 0.1f;
		Mode->bProvideNearbySpawnPickup = false; // 隔离出生补给，不改空手重生规则。
#if !UE_BUILD_SHIPPING
		Mode->bSmokeTest = false;
#endif
		FActorSpawnParameters PlayerSpawn;
		PlayerSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* PreviewPawn = World->SpawnActor<AfpstrueCharacter>(Mode->DefaultPawnClass,
			FTransform(FVector(5000.0f, 0.0f, 1000.0f)), PlayerSpawn);
		auto* PreviewController = World->SpawnActor<AfpstruePvpPlayerController>();
		auto* PreviewStart = World->SpawnActor<APlayerStart>(FVector(8000.0f, 0.0f, 1000.0f), FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Preview death has its real player Blueprint"), PreviewPawn)
			|| !TestNotNull(TEXT("Preview death has its production Controller"), PreviewController)
			|| !TestNotNull(TEXT("Preview respawn has a real PlayerStart"), PreviewStart)) return false;
		PreviewPawn->GetCharacterMovement()->SetComponentTickEnabled(false);
		PreviewController->Possess(PreviewPawn);
		auto* PreviewState = PreviewController->GetPlayerState<AfpstruePvpPlayerState>();
		auto* MatchState = Mode->GetGameState<AfpstruePvpGameState>();
		if (!TestNotNull(TEXT("Preview owns the persistent project PlayerState"), PreviewState)
			|| !TestNotNull(TEXT("Preview has its active replicated match state"), MatchState)) return false;
		const int32 DeathsBefore = PreviewState->GetDeaths();
		const int32 EliminationsBefore = MatchState->GetEliminationCount();
		UGameplayStatics::ApplyDamage(PreviewPawn, 100000.0f, nullptr, nullptr, nullptr);
		if (!TestTrue(TEXT("Normal Standalone lethal damage reaches the shared authoritative respawn queue"),
			PreviewPawn->IsDead() && Mode->PendingRespawns.Contains(PreviewController))) return false;
		TestTrue(TEXT("The one shared respawn record owns an active production Timer"),
			Mode->PendingRespawns.Num() == 1 && World->GetTimerManager().IsTimerActive(Mode->PendingRespawns.FindChecked(PreviewController)));
		TestEqual(TEXT("Standalone lethal damage records one death without a manual GameMode call"), PreviewState->GetDeaths(), DeathsBefore + 1);
		TestEqual(TEXT("Standalone lethal damage records one match elimination"), MatchState->GetEliminationCount(), EliminationsBefore + 1);
		TestEqual(TEXT("Preview death disables the capsule just like a networked death"),
			PreviewPawn->GetCapsuleComponent()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		Mode->HandlePlayerEliminated(PreviewPawn, nullptr); // 重复入口只能复用已登记的事务。
		TestEqual(TEXT("Duplicate death cannot create a second pending respawn"), Mode->PendingRespawns.Num(), 1);
		TestEqual(TEXT("Duplicate death cannot add another death"), PreviewState->GetDeaths(), DeathsBefore + 1);
		TestEqual(TEXT("Duplicate death cannot add another elimination"), MatchState->GetEliminationCount(), EliminationsBefore + 1);
		const TWeakObjectPtr<AfpstrueCharacter> DeadPreviewPawn(PreviewPawn);
		for (int32 Frame = 0; Frame < 5; ++Frame)
			if (!Fixture.TickTestWorld(0.05f)) { Fixture.ForwardErrorMessages(this); return false; }
		auto* RespawnedPawn = Cast<AfpstrueCharacter>(PreviewController->GetPawn());
		TestTrue(TEXT("Normal Standalone respawn consumes the production Timer and creates a new living Pawn"),
			IsValid(RespawnedPawn) && RespawnedPawn != DeadPreviewPawn.Get() && !RespawnedPawn->IsDead()
			&& RespawnedPawn->GetController() == PreviewController && !Mode->PendingRespawns.Contains(PreviewController));
		TestTrue(TEXT("Standalone respawn preserves its PlayerState and exactly one death"),
			PreviewController->GetPlayerState<AfpstruePvpPlayerState>() == PreviewState && PreviewState->GetDeaths() == DeathsBefore + 1);
		TestFalse(TEXT("The replaced dead preview Pawn is reclaimed"), DeadPreviewPawn.IsValid() && !DeadPreviewPawn->IsActorBeingDestroyed());
	}
	return true;
}

#endif
