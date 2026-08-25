// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyAIController.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstruePerformanceStats.h"
#include "fpstrueSurroundManager.h"
#include "AITypes.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

DEFINE_STAT(STAT_fpstrueAIDecisionTime);
DEFINE_STAT(STAT_fpstrueAIDecisionCount);
DEFINE_STAT(STAT_fpstrueAIMoveRequestCount);
CSV_DEFINE_CATEGORY(fpstrueAI, true);

AfpstrueEnemyAIController::AfpstrueEnemyAIController()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AfpstrueEnemyAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	bDisableDecisionThrottlingForBenchmark = FParse::Param(
		FCommandLine::Get(),
		TEXT("BenchmarkDisableAIThrottling")
	);

	ControlledEnemy = Cast<AfpstrueEnemyCharacter>(InPawn);
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return;
	}

	if (ControlledEnemy->IsDead())
	{
		SetAIState(EFPEnemyAIState::Dead);
		StopAI();
		return;
	}

	SurroundManager = ResolveSurroundManager();
	if (SurroundManager != nullptr)
	{
		SurroundManager->RequestSurroundSlot(ControlledEnemy);
	}

	bHasMoveGoal = false;
	StartDecisionTimer();
	UpdateAI();
}

void AfpstrueEnemyAIController::OnUnPossess()
{
	ClearDecisionTimer();
	ReleaseSurroundSlot();
	ControlledEnemy = nullptr;
	TargetCharacter = nullptr;
	SurroundManager = nullptr;
	bHasMoveGoal = false;
	LastSharedTargetGeneration = 0;

	Super::OnUnPossess();
}

void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearDecisionTimer();
	ReleaseSurroundSlot();
	Super::EndPlay(EndPlayReason);
}

void AfpstrueEnemyAIController::StopAI()
{
	StopMovement();
	ClearDecisionTimer();
	TargetCharacter = nullptr;
	bHasMoveGoal = false;
	LastSharedTargetGeneration = 0;
	ReleaseSurroundSlot();

	if (ControlledEnemy != nullptr)
	{
		ControlledEnemy->SetTargetCharacter(nullptr);
		SetAIState(ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
	}
}

void AfpstrueEnemyAIController::InitializeCombatContext(
	AfpstrueCharacter* NewTargetCharacter,
	AfpstrueSurroundManager* NewSurroundManager
)
{
	TargetCharacter = NewTargetCharacter;
	SurroundManager = NewSurroundManager;

	if (ControlledEnemy == nullptr)
	{
		return;
	}

	ControlledEnemy->SetTargetCharacter(TargetCharacter);
	if (SurroundManager != nullptr)
	{
		SurroundManager->RequestSurroundSlot(ControlledEnemy);
	}
}

void AfpstrueEnemyAIController::ApplyBenchmarkPathFollowingTickOverride(
	bool bDisablePathFollowingTick)
{
	if (UPathFollowingComponent* PathFollowing = GetPathFollowingComponent())
	{
		PathFollowing->SetComponentTickEnabled(!bDisablePathFollowingTick);
	}
}

void AfpstrueEnemyAIController::SetSignificanceDecisionMultiplier(float NewMultiplier)
{
	SignificanceDecisionMultiplier = FMath::Max(1.0f, NewMultiplier);
}

void AfpstrueEnemyAIController::StartDecisionTimer()
{
	const float FirstDelay = FMath::FRandRange(
		0.01f,
		FMath::Max(0.01f, GetNextDecisionInterval())
	);
	ScheduleNextDecision(FirstDelay);
}

void AfpstrueEnemyAIController::ScheduleNextDecision(float Delay)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			DecisionTimerHandle,
			this,
			&AfpstrueEnemyAIController::UpdateAI,
			FMath::Max(0.01f, Delay),
			false
		);
	}
}

void AfpstrueEnemyAIController::ClearDecisionTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DecisionTimerHandle);
	}
}

void AfpstrueEnemyAIController::UpdateAI()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_UpdateAI);
	CSV_SCOPED_TIMING_STAT(fpstrueAI, DecisionTime);
	CSV_CUSTOM_STAT(fpstrueAI, DecisionCount, 1, ECsvCustomStatOp::Accumulate);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAIDecisionTime);
	INC_DWORD_STAT(STAT_fpstrueAIDecisionCount);

	// Rearm before evaluating state so StopAI can still cancel this one-shot timer.
	ScheduleNextDecision(GetNextDecisionInterval());

	if (!PrepareDecisionContext())
	{
		return;
	}

	if (HandleActiveAttack())
	{
		return;
	}

	const float DistanceToTarget = ControlledEnemy->GetDistanceToTarget2D();
	if (DistanceToTarget > ControlledEnemy->GetChaseRange())
	{
		SetAIState(EFPEnemyAIState::Idle);
		StopMovement();
		bHasMoveGoal = false;
		return;
	}

	if (ControlledEnemy->IsTargetInAttackRange())
	{
		SetAIState(EFPEnemyAIState::Attack);
		StopMovement();
		bHasMoveGoal = false;
		ControlledEnemy->FaceTarget();
		ControlledEnemy->TryAttackTarget();
		return;
	}

	if (HandleAttackApproach() || HandleSurroundMovement())
	{
		return;
	}

	SetAIState(EFPEnemyAIState::Chase);
	HandleSharedPursuit();
}

float AfpstrueEnemyAIController::GetNextDecisionInterval() const
{
	if (bDisableDecisionThrottlingForBenchmark)
	{
		return AttackDecisionInterval;
	}

	if (ControlledEnemy == nullptr
		|| ControlledEnemy->IsDead()
		|| !IsTargetUsable(TargetCharacter))
	{
		return IdleDecisionInterval * SignificanceDecisionMultiplier;
	}

	const float DistanceToTarget = ControlledEnemy->GetDistanceToTarget2D();
	const bool bNeedsCombatResponse =
		ControlledEnemy->IsAttacking()
		|| AIState == EFPEnemyAIState::Attack
		|| DistanceToTarget <= ControlledEnemy->GetAttackRange() * 1.5f;
	if (bNeedsCombatResponse)
	{
		return AttackDecisionInterval;
	}

	if (AIState == EFPEnemyAIState::Idle
		|| DistanceToTarget > ControlledEnemy->GetChaseRange())
	{
		return IdleDecisionInterval * SignificanceDecisionMultiplier;
	}

	if (DistanceToTarget >= FarDecisionDistance)
	{
		return FarDecisionInterval * SignificanceDecisionMultiplier;
	}

	return ChaseDecisionInterval * SignificanceDecisionMultiplier;
}

bool AfpstrueEnemyAIController::PrepareDecisionContext()
{
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return false;
	}

	if (ControlledEnemy->IsDead())
	{
		SetAIState(EFPEnemyAIState::Dead);
		StopAI();
		return false;
	}

	if (SurroundManager == nullptr)
	{
		SurroundManager = ResolveSurroundManager();
		if (SurroundManager != nullptr)
		{
			SurroundManager->RequestSurroundSlot(ControlledEnemy);
		}
	}

	if (!IsTargetUsable(TargetCharacter))
	{
		TargetCharacter = ResolveTarget();
	}

	if (!IsTargetUsable(TargetCharacter))
	{
		ReleaseSurroundSlot();
		SetAIState(EFPEnemyAIState::Idle);
		StopMovement();
		bHasMoveGoal = false;
		return false;
	}

	ControlledEnemy->SetTargetCharacter(TargetCharacter);
	if (SurroundManager != nullptr)
	{
		SurroundManager->SetTargetCharacter(TargetCharacter);
	}

	UpdateFacingTarget();
	return true;
}

bool AfpstrueEnemyAIController::HandleActiveAttack()
{
	if (!ControlledEnemy->IsAttacking())
	{
		return false;
	}

	SetAIState(EFPEnemyAIState::Attack);
	StopMovement();
	bHasMoveGoal = false;
	ControlledEnemy->FaceTarget();
	return true;
}

bool AfpstrueEnemyAIController::HandleAttackApproach()
{
	if (SurroundManager == nullptr)
	{
		return false;
	}

	FVector AttackGoal;
	if (SurroundManager->GetAttackApproachLocation(ControlledEnemy, AttackGoal))
	{
		SetAIState(EFPEnemyAIState::Chase);
		MoveToGoal(AttackGoal, CombatMoveAcceptanceRadius);
		return true;
	}

	return false;
}

bool AfpstrueEnemyAIController::HandleSurroundMovement()
{
	if (SurroundManager != nullptr
		&& SurroundManager->RequestSurroundSlot(ControlledEnemy))
	{
		if (HandleAttackApproach())
		{
			return true;
		}

		FVector SlotGoal;
		bool bInnerRing = false;
		if (SurroundManager->GetAssignedSlotLocation(ControlledEnemy, SlotGoal, bInnerRing))
		{
			const bool bAtSlot =
				FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), SlotGoal)
				<= FMath::Square(SlotArrivalTolerance);

			SetAIState(EFPEnemyAIState::Chase);
			if (bAtSlot)
			{
				StopMovement();
				bHasMoveGoal = false;
				ControlledEnemy->FaceTarget();
			}
			else
			{
				const float SlotMoveAcceptanceRadius =
					bInnerRing ? CombatMoveAcceptanceRadius : MoveAcceptanceRadius;
				MoveToGoal(SlotGoal, SlotMoveAcceptanceRadius);
			}
			return true;
		}
	}

	return false;
}

bool AfpstrueEnemyAIController::HandleSharedPursuit()
{
	FVector SharedGoal;
	uint32 TargetGeneration = 0;
	if (SurroundManager != nullptr
		&& SurroundManager->GetSharedTargetSnapshot(
			SharedGoal,
			TargetGeneration
		))
	{
		const bool bTargetChanged = TargetGeneration != LastSharedTargetGeneration;
		const bool bPathIdle = GetMoveStatus() == EPathFollowingStatus::Idle;
		if (bTargetChanged || !bHasMoveGoal || bPathIdle)
		{
			const float PursuitAcceptanceRadius = FMath::Max(
				MoveAcceptanceRadius,
				ControlledEnemy->GetAttackRange() * 0.8f
			);
			MoveToGoal(SharedGoal, PursuitAcceptanceRadius);
			LastSharedTargetGeneration = TargetGeneration;
		}
		return true;
	}

	MoveToGoal(TargetCharacter->GetActorLocation(), MoveAcceptanceRadius);
	return true;
}

void AfpstrueEnemyAIController::UpdateFacingTarget()
{
	if (ControlledEnemy == nullptr || !IsTargetUsable(TargetCharacter))
	{
		return;
	}

	const FVector ToTarget =
		TargetCharacter->GetActorLocation() - ControlledEnemy->GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	if (HorizontalToTarget.IsNearlyZero())
	{
		return;
	}

	FRotator TargetRotation = HorizontalToTarget.Rotation();
	TargetRotation.Pitch = 0.0f;
	TargetRotation.Roll = 0.0f;
	SetControlRotation(TargetRotation);
}

void AfpstrueEnemyAIController::MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius)
{
	const bool bAtGoal = ControlledEnemy != nullptr
		&& FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), GoalLocation)
		<= FMath::Square(FMath::Max(AcceptanceRadius, 1.0f));
	const bool bNeedsNewPath =
		bDisableDecisionThrottlingForBenchmark
		|| !bHasMoveGoal
		|| FVector::DistSquared2D(GoalLocation, LastMoveGoal) >= FMath::Square(PathRefreshDistance)
		|| (GetMoveStatus() == EPathFollowingStatus::Idle && !bAtGoal);

	if (bNeedsNewPath)
	{
		INC_DWORD_STAT(STAT_fpstrueAIMoveRequestCount);
		CSV_CUSTOM_STAT(fpstrueAI, MoveRequestCount, 1, ECsvCustomStatOp::Accumulate);
		MoveToLocation(GoalLocation, AcceptanceRadius, true, true, true, false, nullptr, true);
		LastMoveGoal = GoalLocation;
		bHasMoveGoal = true;
	}
}

void AfpstrueEnemyAIController::ReleaseSurroundSlot()
{
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseSurroundSlot(ControlledEnemy);
	}
}

void AfpstrueEnemyAIController::SetAIState(EFPEnemyAIState NewState)
{
	if (AIState == NewState)
	{
		return;
	}

	AIState = NewState;
}

AfpstrueCharacter* AfpstrueEnemyAIController::ResolveTarget() const
{
	return Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
}

AfpstrueSurroundManager* AfpstrueEnemyAIController::ResolveSurroundManager() const
{
	return Cast<AfpstrueSurroundManager>(
		UGameplayStatics::GetActorOfClass(this, AfpstrueSurroundManager::StaticClass())
	);
}

bool AfpstrueEnemyAIController::IsTargetUsable(const AfpstrueCharacter* Target) const
{
	return Target != nullptr && !Target->IsDead();
}
