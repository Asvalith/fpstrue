// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Engine/World.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueSurroundAdmissionTest,
	"fpstrue.AI.Surround.ParticipationAndGoalAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 固定共享导航输出，仍通过真实登记、FIFO、释放与失败回队验证容量和身份语义。
bool FFpstrueSurroundAdmissionTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Admission fixture has the native AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueSurroundManager* Manager = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("Live tactical target"), Target) || !TestNotNull(TEXT("Native surround manager"), Manager)) return false;
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Manager->SetTargetCharacter(Target);
	World->GetTimerManager().ClearTimer(Manager->SharedTargetTimerHandle);
	const int32 OldQuery = Manager->ApproachQueryId;
	Manager->ApproachQueryId = INDEX_NONE;
	++Manager->PositionQueryRevision;
	if (UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(World)) Queries->AbortQuery(OldQuery);
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	for (FfpstrueSurroundSlot& Slot : Manager->SurroundSlots)
	{
		const FVector Direction(FMath::Cos(Slot.AngleRadians), FMath::Sin(Slot.AngleRadians), 0.0f);
		Slot.ProjectedSlotLocation = Manager->CachedTargetLocation + Direction * Slot.Radius;
		Slot.ProjectedApproachLocation = Manager->CachedTargetLocation + Direction * Manager->AttackApproachRadius;
		Slot.bHasProjectedSlotLocation = true;
		Slot.bHasProjectedApproachLocation = Slot.RingIndex == 0;
	}
	TArray<AfpstrueEnemyCharacter*> Enemies;
	TArray<FFPEnemyTacticalAssignment> Accepted;
	for (int32 Index = 0; Index < 160; ++Index)
	{
		const FTransform Transform(FVector(1000.0f + Index * 10.0f, 0.0f, 0.0f));
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("Native admission candidate"), Enemy)) return false;
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(Transform);
		Enemy->SetActorEnableCollision(false);
		Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		Enemies.Add(Enemy);
		FFPEnemyTacticalAssignment Assignment;
		const bool bAccepted = Manager->GetOrRequestTacticalAssignment(Enemy, Assignment);
		TestTrue(TEXT("All one hundred sixty participants retain their own move goal"),
			bAccepted && Assignment.Revision != 0 && Assignment.bHasGoal);
		Accepted.Add(Assignment);
	}
	TestEqual(TEXT("Attack waiting capacity does not cap space participants"), Manager->Participants.Num(), 160);
	TestEqual(TEXT("Eight inner holders leave one hundred fifty-two spatial candidates including outer holders"), Manager->SlotWaitingQueue.Num(), 152);
	TestEqual(TEXT("Each participant allocates one request sequence"), Manager->NextRequestSequence, uint64(160));
	const FFPEnemyTacticalDebugSnapshot Full = Manager->GetTacticalDebugSnapshot();
	TestEqual(TEXT("All eight inner seats are occupied"), Full.OccupiedInnerSlots, 8);
	TestEqual(TEXT("All twelve outer seats remain useful while awaiting promotion"), Full.OccupiedOuterSlots, 12);
	const uint64 BeforeRevision = Manager->NextAssignmentRevision;
	const uint64 BeforeGrants = Manager->TacticalGrantCount;
	for (int32 Index = 0; Index < Enemies.Num(); ++Index)
	{
		FFPEnemyTacticalAssignment Repeated;
		Repeated.Role = EFPEnemyTacticalRole::Inner;
		Repeated.Revision = MAX_uint64;
		Repeated.bHasGoal = true;
		const bool bAccepted = Manager->GetOrRequestTacticalAssignment(Enemies[Index], Repeated);
		TestTrue(TEXT("Repeated requests keep their lease and goal, including callers beyond sixty-four"),
			bAccepted && Repeated.Role == Accepted[Index].Role && Repeated.Revision == Accepted[Index].Revision &&
			Repeated.bHasGoal && Repeated.Goal.Equals(Accepted[Index].Goal));
	}
	TestEqual(TEXT("Repeated calls do not allocate new request numbers"), Manager->NextRequestSequence, uint64(160));
	TestEqual(TEXT("Polling does not republish assignments"), Manager->NextAssignmentRevision, BeforeRevision);
	TestEqual(TEXT("Polling does not grant another seat"), Manager->TacticalGrantCount, BeforeGrants);
	TestEqual(TEXT("Polling leaves one FIFO entry per spatial candidate"), Manager->SlotWaitingQueue.Num(), 152);

	// 释放内圈只晋升空间队头；注销后重新加入的请求排到尾，不越过原候补。
	const uint64 RetainedSequence = Manager->Participants.FindChecked(Enemies[30]).RequestSequence;
	const FVector RetainedOffset = Manager->Participants.FindChecked(Enemies[30]).WaitingOffset;
	Manager->ReleaseSurroundSlot(Enemies[0]);
	TestTrue(TEXT("The oldest outer holder is promoted when an inner holder leaves"), Manager->HasInnerSlot(Enemies[8]));
	TestEqual(TEXT("Promotion preserves the next candidate's FIFO rank"), Manager->SlotWaitingQueue[0].Enemy.Get(), Enemies[9]);
	FFPEnemyTacticalAssignment NewAssignment;
	TestTrue(TEXT("A released participant can rejoin without a total participation cap"), Manager->GetOrRequestTacticalAssignment(Enemies[0], NewAssignment));
	TestEqual(TEXT("Rejoining appends behind all existing candidates"), Manager->SlotWaitingQueue.Last().Enemy.Get(), Enemies[0]);
	Manager->ReleaseSurroundSlot(Enemies[10]);
	TestTrue(TEXT("A cancelled queued request can rejoin"), Manager->GetOrRequestTacticalAssignment(Enemies[10], NewAssignment));
	TestEqual(TEXT("Unrelated cancellations do not renumber an existing candidate"), Manager->Participants.FindChecked(Enemies[30]).RequestSequence, RetainedSequence);
	TestTrue(TEXT("Unrelated cancellations do not move a candidate's stable waiting offset"),
		Manager->Participants.FindChecked(Enemies[30]).WaitingOffset.Equals(RetainedOffset));

	// 导航目标暂不可用不等于丢掉内圈；资格和目标有效性必须独立发布。
	FFPEnemyTacticalAssignment BeforeGoalFailure;
	if (!Manager->GetOrRequestTacticalAssignment(Enemies[1], BeforeGoalFailure)) return false;
	FfpstrueSurroundSlot& Inner = Manager->SurroundSlots[Manager->EnemyToSlot.FindChecked(Enemies[1])];
	Inner.bHasProjectedApproachLocation = false;
	FFPEnemyTacticalAssignment WithoutGoal;
	TestTrue(TEXT("An admitted identity survives missing shared navigation output"), Manager->GetOrRequestTacticalAssignment(Enemies[1], WithoutGoal));
	TestTrue(TEXT("Missing navigation preserves the real role and revision"), WithoutGoal.Role == EFPEnemyTacticalRole::Inner &&
		WithoutGoal.Revision == BeforeGoalFailure.Revision && !WithoutGoal.bHasGoal && Manager->HasInnerSlot(Enemies[1], WithoutGoal.Revision));
	FVector Goal;
	TestFalse(TEXT("A retained lease cannot move toward an unavailable goal"), Manager->ResolveTacticalMoveGoal(Enemies[1], WithoutGoal, Goal));
	Inner.bHasProjectedApproachLocation = true;
	TestTrue(TEXT("Navigation recovery restores a goal without regranting the lease"), Manager->GetOrRequestTacticalAssignment(Enemies[1], NewAssignment) &&
		NewAssignment.bHasGoal && NewAssignment.Revision == BeforeGoalFailure.Revision);
	Manager->PublishedTargetLocation += FVector(Manager->SharedTargetMoveThreshold + 1.0f, 0.0f, 0.0f);
	TestTrue(TEXT("An outdated layout also preserves role while withholding its goal"), Manager->GetOrRequestTacticalAssignment(Enemies[1], WithoutGoal) &&
		WithoutGoal.Role == EFPEnemyTacticalRole::Inner && WithoutGoal.Revision == BeforeGoalFailure.Revision && !WithoutGoal.bHasGoal);
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;

	// 内圈失路回队保留身份，不把空间候场与固定攻击候补混为同一种容量。
	Manager->ReportTacticalMoveFailure(Enemies[1], BeforeGoalFailure.Revision);
	TestTrue(TEXT("Path failure retains the participant identity for its retry"), Manager->Participants.Contains(Enemies[1]));
	TestFalse(TEXT("Path failure revokes only the previous inner lease"), Manager->HasInnerSlot(Enemies[1], BeforeGoalFailure.Revision));
	TestTrue(TEXT("The next FIFO candidate receives the released inner seat"), Manager->HasInnerSlot(Enemies[9]));
	TestEqual(TEXT("A failed inner holder goes to the spatial queue tail"), Manager->SlotWaitingQueue.Last().Enemy.Get(), Enemies[1]);
	TestEqual(TEXT("Requeue keeps all one hundred sixty participants active"), Manager->Participants.Num(), 160);
	TestEqual(TEXT("Requeue preserves one spatial entry per candidate"), Manager->SlotWaitingQueue.Num(), 152);
	TestEqual(TEXT("Requeue leaves unrelated waiting identities in their original order"), Manager->Participants.FindChecked(Enemies[30]).RequestSequence, RetainedSequence);
	return true;
}

#endif
