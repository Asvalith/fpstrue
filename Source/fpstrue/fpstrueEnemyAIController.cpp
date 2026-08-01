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
	ReleaseSurroundResources(true);
	ControlledEnemy = nullptr;
	TargetCharacter = nullptr;
	SurroundManager = nullptr;
	bHasMoveGoal = false;

	Super::OnUnPossess();
}

void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearDecisionTimer();
	ReleaseSurroundResources(true);
	Super::EndPlay(EndPlayReason);
}

void AfpstrueEnemyAIController::StopAI()
{
	StopMovement();
	ClearDecisionTimer();
	TargetCharacter = nullptr;
	bHasMoveGoal = false;
	ReleaseSurroundResources(true);

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

	if (bHasAttackToken && bObservedAttackInProgress)
	{
		ReleaseSurroundResources(false);
	}

	const float DistanceToTarget = ControlledEnemy->GetDistanceToTarget2D();
	if (DistanceToTarget > ControlledEnemy->GetChaseRange())
	{
		ReleaseSurroundResources(false);
		SetAIState(EFPEnemyAIState::Idle);
		StopMovement();
		bHasMoveGoal = false;
		return;
	}

	if (HandleAttackToken() || HandleSurroundMovement())
	{
		return;
	}

	SetAIState(EFPEnemyAIState::Chase);
	INC_DWORD_STAT(STAT_fpstrueAIMoveRequestCount);
	CSV_CUSTOM_STAT(fpstrueAI, MoveRequestCount, 1, ECsvCustomStatOp::Accumulate);
	MoveToActor(TargetCharacter, ControlledEnemy->GetAttackRange());
}

float AfpstrueEnemyAIController::GetNextDecisionInterval() const
{
	if (ControlledEnemy == nullptr
		|| ControlledEnemy->IsDead()
		|| !IsTargetUsable(TargetCharacter))
	{
		return IdleDecisionInterval;
	}

	const float DistanceToTarget = ControlledEnemy->GetDistanceToTarget2D();
	const bool bNeedsCombatResponse =
		ControlledEnemy->IsAttacking()
		|| bHasAttackToken
		|| AIState == EFPEnemyAIState::Attack
		|| DistanceToTarget <= ControlledEnemy->GetAttackRange() * 1.5f;
	if (bNeedsCombatResponse)
	{
		return AttackDecisionInterval;
	}

	if (AIState == EFPEnemyAIState::Idle
		|| DistanceToTarget > ControlledEnemy->GetChaseRange())
	{
		return IdleDecisionInterval;
	}

	if (DistanceToTarget >= FarDecisionDistance)
	{
		return FarDecisionInterval;
	}

	return ChaseDecisionInterval;
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

	ControlledEnemy->SetTargetCharacter(TargetCharacter);

	if (!IsTargetUsable(TargetCharacter))
	{
		ReleaseSurroundResources(true);
		SetAIState(EFPEnemyAIState::Idle);
		StopMovement();
		bHasMoveGoal = false;
		return false;
	}

	ControlledEnemy->UpdatePerformanceTier(ControlledEnemy->GetDistanceToTarget2D());
	UpdateFacingTarget();
	return true;
}

bool AfpstrueEnemyAIController::HandleActiveAttack()
{
	if (!ControlledEnemy->IsAttacking())
	{
		return false;
	}

	bObservedAttackInProgress = true;
	SetAIState(EFPEnemyAIState::Attack);
	StopMovement();
	bHasMoveGoal = false;
	ControlledEnemy->FaceTarget();
	return true;
}

bool AfpstrueEnemyAIController::HandleAttackToken()
{
	if (!bHasAttackToken)
	{
		return false;
	}

	const UWorld* World = GetWorld();
	const bool bTokenTimedOut =
		World != nullptr
		&& World->GetTimeSeconds() - AttackTokenAcquiredTime >= AttackTokenTimeout;
	if (bTokenTimedOut)
	{
		ReleaseSurroundResources(false);
		return false;
	}

	if (ControlledEnemy->IsTargetInAttackRange())
	{
		SetAIState(EFPEnemyAIState::Attack);
		StopMovement();
		bHasMoveGoal = false;
		ControlledEnemy->FaceTarget();
		if (ControlledEnemy->TryAttackTarget())
		{
			bObservedAttackInProgress = true;
		}
		return true;
	}

	FVector AttackGoal;
	if (SurroundManager != nullptr
		&& SurroundManager->GetAttackApproachLocation(ControlledEnemy, AttackGoal))
	{
		SetAIState(EFPEnemyAIState::Chase);
		MoveToGoal(AttackGoal);
		return true;
	}

	ReleaseSurroundResources(false);
	return false;
}

bool AfpstrueEnemyAIController::HandleSurroundMovement()
{
	if (SurroundManager != nullptr
		&& SurroundManager->RequestSurroundSlot(ControlledEnemy))
	{
		FVector SlotGoal;
		bool bInnerRing = false;
		if (SurroundManager->GetAssignedSlotLocation(ControlledEnemy, SlotGoal, bInnerRing))
		{
			const bool bAtSlot =
				FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), SlotGoal)
				<= FMath::Square(SlotArrivalTolerance);

			if (bInnerRing && bAtSlot && SurroundManager->RequestAttackToken(ControlledEnemy))
			{
				bHasAttackToken = true;
				bObservedAttackInProgress = false;
				AttackTokenAcquiredTime = GetWorld()->GetTimeSeconds();

				FVector AttackGoal;
				if (SurroundManager->GetAttackApproachLocation(ControlledEnemy, AttackGoal))
				{
					SetAIState(EFPEnemyAIState::Chase);
					MoveToGoal(AttackGoal);
					return true;
				}

				ReleaseSurroundResources(false);
			}

			SetAIState(EFPEnemyAIState::Chase);
			if (bAtSlot)
			{
				StopMovement();
				bHasMoveGoal = false;
				ControlledEnemy->FaceTarget();
			}
			else
			{
				MoveToGoal(SlotGoal);
			}
			return true;
		}
	}

	return false;
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

void AfpstrueEnemyAIController::MoveToGoal(const FVector& GoalLocation)
{
	const bool bNeedsNewPath =
		!bHasMoveGoal
		|| FVector::DistSquared2D(GoalLocation, LastMoveGoal) >= FMath::Square(PathRefreshDistance)
		|| GetMoveStatus() == EPathFollowingStatus::Idle;

	if (bNeedsNewPath)
	{
		INC_DWORD_STAT(STAT_fpstrueAIMoveRequestCount);
		CSV_CUSTOM_STAT(fpstrueAI, MoveRequestCount, 1, ECsvCustomStatOp::Accumulate);
		MoveToLocation(GoalLocation, MoveAcceptanceRadius, true, true, true, false, nullptr, true);
		LastMoveGoal = GoalLocation;
		bHasMoveGoal = true;
	}
}

void AfpstrueEnemyAIController::ReleaseSurroundResources(bool bReleaseSlot)
{
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseAttackToken(ControlledEnemy);
		if (bReleaseSlot)
		{
			SurroundManager->ReleaseSurroundSlot(ControlledEnemy);
		}
	}

	bHasAttackToken = false;
	bObservedAttackInProgress = false;
	AttackTokenAcquiredTime = 0.0f;
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
