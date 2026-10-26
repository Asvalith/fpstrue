// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpGameState.h"
#include "BrainComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "ReferenceSkeleton.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueSurroundAttackQueueTest,
	"fpstrue.AI.Surround.AttackWaitingQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 只固定共享 EQS 的输出；目标参与、视觉、授席、实时攻击检查和事务归还均走生产入口。
bool FFpstrueSurroundAttackQueueTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Attack queue uses the native AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstruePvpGameMode* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	if (!TestNotNull(TEXT("Multiplayer-only game mode"), Mode)) return false;
	World->GetTimerManager().ClearAllTimersForObject(Mode);
	if (AfpstruePvpGameState* State = World->GetGameState<AfpstruePvpGameState>())
	{
		if (auto* Coordinator = State->FindComponentByClass<UfpstrueEnemySignificanceCoordinator>()) Coordinator->Stop();
		if (auto* Sharing = State->FindComponentByClass<UfpstrueEnemyAnimationSharingCoordinator>()) Sharing->Stop();
	}
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Live attack target"), Target)) return false;
	for (const FName Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Target->GetClass(), Name);
		if (Property != nullptr && Property->GetObjectPropertyValue_InContainer(Target) == nullptr)
			Property->SetObjectPropertyValue_InContainer(Target, NewObject<UInputAction>(Target));
	}
	APlayerController* Player = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Ordinary player owner"), Player)) return false;
	Player->Possess(Target);
	Target->SetActorLocation(FVector::ZeroVector);
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	AfpstrueSurroundManager* Manager = Mode->GetOrCreateSurroundManager(Target);
	if (!TestNotNull(TEXT("Target-owned manager"), Manager)) return false;
	TestEqual(TEXT("Production concurrency default remains eight, separately from waiting capacity"), Manager->MaxActiveAttackers, 8);
	TestEqual(TEXT("Exactly eight attack candidates may wait"), Manager->MaxWaitingAttackers, 8);
	// 测试配置 12 内圈、1 并发，才能实际制造 8 候补和第 9 个候补；不改生产默认 8/8。
	Manager->InnerSlotCount = 12;
	Manager->MaxActiveAttackers = 1;
	Manager->BuildSlots();
	World->GetTimerManager().ClearTimer(Manager->SharedTargetTimerHandle);
	const int32 OldQuery = Manager->ApproachQueryId;
	Manager->ApproachQueryId = INDEX_NONE;
	++Manager->PositionQueryRevision;
	if (OldQuery != INDEX_NONE)
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
	TArray<AfpstrueEnemyAIController*> Controllers;
	TArray<UfpstrueEnemyCombatComponent*> Combat;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		const float Angle = 2.0f * PI * static_cast<float>(Index) / 12.0f;
		const FVector Position = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * 120.0f;
		const FTransform Transform((-Position).Rotation(), Position);
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("Real attack candidate"), Enemy)) return false;
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(Transform);
		Enemy->SetActorEnableCollision(false);
		Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
		if (!TestNotNull(TEXT("Native decision controller"), Controller)) return false;
		Controller->Possess(Enemy);
		Controller->InitializeCombatContext(Target, Manager);
		UAIPerceptionComponent* Perception = Controller->GetPerceptionComponent();
		FAIStimulus Stimulus(*GetDefault<UAISense_Sight>(), 1.0f, Target->GetActorLocation(), Position, FAIStimulus::SensingSucceeded);
		Stimulus.SetWantsNotifyOnlyOnValueChange(false);
		Perception->RegisterStimulus(Target, Stimulus);
		Perception->ProcessStimuli();
		Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
		Controller->GetBrainComponent()->SetComponentTickEnabled(false);
		Controller->GetBrainComponent()->PrimaryComponentTick.bCanEverTick = false;
		UfpstrueEnemyCombatComponent* Component = Enemy->GetCombatComponent();
		if (!TestTrue(TEXT("Candidate is genuinely ready: real inner lease, target, cooldown, facing, distance and LOS"),
			Component != nullptr && Component->CanStartAttack())) return false;
		Enemies.Add(Enemy);
		Controllers.Add(Controller);
		Combat.Add(Component);
	}

	if (!TestTrue(TEXT("First ready candidate starts a complete combat transaction"), Combat[0]->TryAttackTarget())) return false;
	for (int32 Index = 1; Index <= 8; ++Index)
	{
		TestFalse(TEXT("A full active budget queues a ready attacker instead of starting it"), Combat[Index]->TryAttackTarget());
		TestFalse(TEXT("Waiting does not create a combat transaction or failsafe timer"), Combat[Index]->IsAttacking());
		TestEqual(TEXT("Each new candidate appends exactly once"), Manager->WaitingAttackers[Index - 1].Get(), Enemies[Index]);
	}
	TestFalse(TEXT("The ninth waiting request is rejected without enlarging the queue"), Combat[9]->TryAttackTarget());
	TestEqual(TEXT("Attack waiting queue has a hard bound of eight"), Manager->WaitingAttackers.Num(), 8);
	const uint64 BeforeSequence = Manager->NextRequestSequence;
	const uint64 BeforeRevision = Manager->NextAssignmentRevision;
	for (int32 Repeat = 0; Repeat < 4; ++Repeat)
		for (int32 Index = 1; Index < 10; ++Index) Combat[Index]->TryAttackTarget();
	for (int32 Index = 1; Index <= 8; ++Index)
		TestEqual(TEXT("Repeated requests preserve FIFO position without duplicates"), Manager->WaitingAttackers[Index - 1].Get(), Enemies[Index]);
	TestEqual(TEXT("Repeated rejected requests do not enlarge the queue"), Manager->WaitingAttackers.Num(), 8);
	TestEqual(TEXT("Attack contention never reassigns a spatial sequence"), Manager->NextRequestSequence, BeforeSequence);
	TestEqual(TEXT("Attack contention never changes a spatial lease"), Manager->NextAssignmentRevision, BeforeRevision);
	FFPEnemyTacticalAssignment OverflowAssignment;
	TestTrue(TEXT("Queue overflow keeps its own spatial lease and move goal, not a player-center fallback"),
		Manager->GetOrRequestTacticalAssignment(Enemies[9], OverflowAssignment) && OverflowAssignment.bHasGoal &&
		OverflowAssignment.Role == EFPEnemyTacticalRole::Inner && !OverflowAssignment.Goal.Equals(Target->GetActorLocation()));
	TestEqual(TEXT("Debug snapshot distinguishes attack waiting from spatial waiting"), Manager->GetTacticalDebugSnapshot().QueuedAttackers, 8);

	Combat[0]->FinishAttack();
	TestEqual(TEXT("Natural completion returns the permit without pre-authorizing a waiter"), Manager->ActiveAttackers.Num(), 0);
	for (int32 Index = 1; Index < 10; ++Index)
		TestFalse(TEXT("Release only wakes decisions, never starts another transaction synchronously"), Combat[Index]->IsAttacking());
	TestFalse(TEXT("An overflow caller cannot jump ahead when a permit opens"), Combat[9]->TryAttackTarget());
	TestFalse(TEXT("A later queued caller cannot jump ahead either"), Combat[2]->TryAttackTarget());
	if (!TestTrue(TEXT("The valid ready head acquires the returned permit"), Combat[1]->TryAttackTarget())) return false;
	TestEqual(TEXT("Only the head leaves the queue on acquisition"), Manager->WaitingAttackers.Num(), 7);

	// 真实 Socket 输入打开再关闭伤害窗口；收招仍持有整次攻击许可。
	USkeletalMesh* Blade = NewObject<USkeletalMesh>(Enemies[1], NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(Blade->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("weapontop"), TEXT("weapontop"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("weaponend"), TEXT("weaponend"), 0), FTransform(FVector(50.0f, 0.0f, 0.0f)));
	}
	Enemies[1]->GetMesh()->SetSkeletalMesh(Blade, false);
	Combat[1]->BeginAttackWindow();
	TestTrue(TEXT("Real weapon sockets open the damage window"), Combat[1]->AttackPhase == EFPEnemyAttackPhase::Active);
	Combat[1]->EndAttackWindow();
	TestTrue(TEXT("Window end enters recovery rather than ending the attack"), Combat[1]->AttackPhase == EFPEnemyAttackPhase::Recovery);
	TestEqual(TEXT("Window end does not return the active permit early"), Manager->ActiveAttackers.Num(), 1);
	TestFalse(TEXT("The next waiter cannot start during recovery"), Combat[2]->TryAttackTarget());
	Combat[1]->ResetCombat();
	TestEqual(TEXT("Cancellation returns the whole attack permit"), Manager->ActiveAttackers.Num(), 0);
	TestFalse(TEXT("The previous holder cannot bypass queued peers after cancellation"), Combat[1]->TryAttackTarget());
	TestEqual(TEXT("A new request by the previous holder joins at the tail"), Manager->WaitingAttackers.Last().Get(), Enemies[1]);

	// 队首状态在等待期间可变；实际空位出现后复核，不让冷却、远离或退出目标的旧请求堵住后续。
	Combat[2]->LastAttackTime = World->GetTimeSeconds();
	TestFalse(TEXT("A cold head is skipped but a later caller still cannot jump the next ready head"), Combat[9]->TryAttackTarget());
	TestEqual(TEXT("Cooldown removes only that stale attack request"), Manager->WaitingAttackers[0].Get(), Enemies[3]);
	const FVector SavedPosition = Enemies[3]->GetActorLocation();
	Enemies[3]->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	TestFalse(TEXT("A distant head is skipped without pre-authorizing its successor"), Combat[9]->TryAttackTarget());
	TestEqual(TEXT("Distance failure exposes the next ready head"), Manager->WaitingAttackers[0].Get(), Enemies[4]);
	Enemies[3]->SetActorLocation(SavedPosition);
	Controllers[4]->InitializeCombatContext(nullptr, nullptr);
	TestFalse(TEXT("Leaving the target removes its attack request"), Manager->WaitingAttackers.Contains(Enemies[4]));
	TestEqual(TEXT("Leaving a target does not reorder unrelated requests"), Manager->WaitingAttackers[0].Get(), Enemies[5]);
	const uint64 FailedRevision = Controllers[5]->GetTacticalAssignmentRevision();
	Manager->ReportTacticalMoveFailure(Enemies[5], FailedRevision);
	TestFalse(TEXT("Returning to the spatial queue cancels the old attack request"), Manager->WaitingAttackers.Contains(Enemies[5]));
	TestTrue(TEXT("Spatial retry still retains participation"), Manager->Participants.Contains(Enemies[5]));
	TestEqual(TEXT("The next eligible attack head is retained"), Manager->WaitingAttackers[0].Get(), Enemies[6]);
	Combat[6]->AttackAnimationDuration = 0.01f;
	Combat[6]->AttackFailSafeDuration = 0.01f;
	Combat[6]->AttackCompletionGracePeriod = 0.0f;
	if (!TestTrue(TEXT("A later surviving head starts its own transaction"), Combat[6]->TryAttackTarget())) return false;
	if (!Fixture.TickTestWorld(0.02f) || !Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestFalse(TEXT("The existing failsafe closes the attack without a new queue timer"), Combat[6]->IsAttacking());
	TestEqual(TEXT("Failsafe returns the active permit"), Manager->ActiveAttackers.Num(), 0);
	TestTrue(TEXT("Failsafe does not consume the next waiter's transaction"), !Combat[7]->IsAttacking() && !Combat[8]->IsAttacking());
	Manager->ResetManager();
	TestEqual(TEXT("Reset clears attack candidates"), Manager->WaitingAttackers.Num(), 0);
	TestEqual(TEXT("Reset clears active permits"), Manager->ActiveAttackers.Num(), 0);
	TestEqual(TEXT("Reset clears the separate spatial participation ledger"), Manager->Participants.Num(), 0);
	return true;
}

#endif
