// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyAIController.h"
#include "fpstrueBenchmarkConfig.h"
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
DEFINE_STAT(STAT_fpstrueAIMoveBudgetRejectedCount);
DEFINE_STAT(STAT_fpstrueAIAttackBudgetRejectedCount);
CSV_DEFINE_CATEGORY(fpstrueAI, true);

AfpstrueEnemyAIController::AfpstrueEnemyAIController()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AfpstrueEnemyAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	bDisableDecisionThrottlingForBenchmark = FFPBenchmarkConfig::Get().bDisableAIThrottling;

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
	bHasMoveGoal = false;
	StartDecisionTimer();
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
	StopMovementIfNeeded();
	ClearDecisionTimer();
	TargetCharacter = nullptr;
	LastSharedTargetGeneration = 0;
	ReleaseSurroundSlot();

	if (ControlledEnemy != nullptr)
	{
		SetAIState(ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
	}
}

void AfpstrueEnemyAIController::InitializeCombatContext(AfpstrueCharacter* NewTargetCharacter, AfpstrueSurroundManager* NewSurroundManager)
{
	// Controller 只保存敌人决策上下文；共享包围目标由 GameMode 统一初始化。
	TargetCharacter = NewTargetCharacter;
	SurroundManager = NewSurroundManager;
}

void AfpstrueEnemyAIController::ApplyBenchmarkPathFollowingTickOverride(bool bDisablePathFollowingTick)
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
	const float FirstDelay = FMath::FRandRange(0.01f, FMath::Max(0.01f, ChaseDecisionInterval * SignificanceDecisionMultiplier));
	ScheduleNextDecision(FirstDelay);
}

void AfpstrueEnemyAIController::ScheduleNextDecision(float Delay)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(DecisionTimerHandle, this, &AfpstrueEnemyAIController::UpdateAI, FMath::Max(0.01f, Delay), false);
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

	if (!PrepareDecisionContext())
	{
		if (ControlledEnemy != nullptr && !ControlledEnemy->IsDead())
		{
			ScheduleNextDecision(IdleDecisionInterval * SignificanceDecisionMultiplier);
		}
		return;
	}

	const FDecisionContext Context = BuildDecisionContext();
	ScheduleNextDecision(GetNextDecisionInterval(Context));

	if (HandleActiveAttack())
	{
		return;
	}

	if (!Context.bInChaseRange)
	{
		SetAIState(EFPEnemyAIState::Idle);
		StopMovementIfNeeded();
		return;
	}

	if (Context.bInAttackRange)
	{
		if (!TryAcquireAttackPermission())
		{
			INC_DWORD_STAT(STAT_fpstrueAIAttackBudgetRejectedCount);
			CSV_CUSTOM_STAT(fpstrueAI, AttackBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
			HandleSurroundMovement();
			return;
		}

		SetAIState(EFPEnemyAIState::Attack);
		StopMovementIfNeeded();
		ControlledEnemy->FaceTarget();
		if (!ControlledEnemy->TryAttackTarget())
		{
			ReleaseAttackPermission();
		}
		return;
	}

	if (HandleSurroundMovement())
	{
		return;
	}

	SetAIState(EFPEnemyAIState::Chase);
	HandleSharedPursuit();
}

AfpstrueEnemyAIController::FDecisionContext AfpstrueEnemyAIController::BuildDecisionContext() const
{
	FDecisionContext Context;
	if (ControlledEnemy == nullptr || !IsTargetUsable(TargetCharacter))
	{
		return Context;
	}

	Context.DistanceSquared = FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), TargetCharacter->GetActorLocation());
	Context.bInAttackRange = Context.DistanceSquared <= FMath::Square(ControlledEnemy->GetEffectiveAttackRange());
	Context.bInChaseRange = Context.DistanceSquared <= FMath::Square(ControlledEnemy->GetChaseRange());
	return Context;
}

float AfpstrueEnemyAIController::GetNextDecisionInterval(const FDecisionContext& Context) const
{
	if (bDisableDecisionThrottlingForBenchmark)
	{
		return AttackDecisionInterval;
	}

	if (ControlledEnemy == nullptr || ControlledEnemy->IsDead() || !IsTargetUsable(TargetCharacter))
	{
		return IdleDecisionInterval * SignificanceDecisionMultiplier;
	}

	const bool bNeedsCombatResponse = ControlledEnemy->IsAttacking() || AIState == EFPEnemyAIState::Attack ||
									  Context.DistanceSquared <= FMath::Square(ControlledEnemy->GetEffectiveAttackRange() * 1.5f);
	if (bNeedsCombatResponse)
	{
		return AttackDecisionInterval;
	}

	if (AIState == EFPEnemyAIState::Idle || !Context.bInChaseRange)
	{
		return IdleDecisionInterval * SignificanceDecisionMultiplier;
	}

	if (Context.DistanceSquared >= FMath::Square(FarDecisionDistance))
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
	}

	if (!IsTargetUsable(TargetCharacter))
	{
		AfpstrueCharacter* ResolvedTarget = ResolveTarget();
		if (TargetCharacter != ResolvedTarget)
		{
			TargetCharacter = ResolvedTarget;
		}
	}

	if (!IsTargetUsable(TargetCharacter))
	{
		ReleaseSurroundSlot();
		SetAIState(EFPEnemyAIState::Idle);
		StopMovementIfNeeded();
		return false;
	}
	return true;
}

bool AfpstrueEnemyAIController::HandleActiveAttack()
{
	if (!ControlledEnemy->IsAttacking())
	{
		return false;
	}

	SetAIState(EFPEnemyAIState::Attack);
	StopMovementIfNeeded();
	ControlledEnemy->FaceTarget();
	return true;
}

bool AfpstrueEnemyAIController::HandleSurroundMovement()
{
	if (SurroundManager == nullptr)
	{
		return false;
	}

	FVector AttackGoal;
	if (!SurroundManager->GetOrAssignAttackApproachLocation(ControlledEnemy, AttackGoal))
	{
		return false;
	}

	SetAIState(EFPEnemyAIState::Chase);
	UpdateFacingTarget();
	MoveToGoal(AttackGoal, CombatMoveAcceptanceRadius, true);
	return true;
}

bool AfpstrueEnemyAIController::HandleSharedPursuit()
{
	UpdateFacingTarget();

	FVector SharedGoal;
	uint32 TargetGeneration = 0;
	if (SurroundManager != nullptr && SurroundManager->GetSharedTargetSnapshot(SharedGoal, TargetGeneration))
	{
		const bool bTargetChanged = TargetGeneration != LastSharedTargetGeneration;
		const bool bPathIdle = GetMoveStatus() == EPathFollowingStatus::Idle;
		if (bTargetChanged || !bHasMoveGoal || bPathIdle)
		{
			const float PursuitAcceptanceRadius = FMath::Max(MoveAcceptanceRadius, ControlledEnemy->GetAttackRange() * 0.8f);
			MoveToGoal(SharedGoal, PursuitAcceptanceRadius, false);
			LastSharedTargetGeneration = TargetGeneration;
		}
		return true;
	}

	MoveToGoal(TargetCharacter->GetActorLocation(), MoveAcceptanceRadius, false);
	return true;
}

void AfpstrueEnemyAIController::UpdateFacingTarget()
{
	if (ControlledEnemy == nullptr || !IsTargetUsable(TargetCharacter))
	{
		return;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - ControlledEnemy->GetActorLocation();
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

void AfpstrueEnemyAIController::MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority)
{
	const bool bAtGoal = ControlledEnemy != nullptr && FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), GoalLocation) <=
														   FMath::Square(FMath::Max(AcceptanceRadius, 1.0f));
	if (bAtGoal)
	{
		StopMovementIfNeeded();
		LastMoveGoal = GoalLocation;
		return;
	}

	const bool bNeedsNewPath = bDisableDecisionThrottlingForBenchmark || !bHasMoveGoal ||
							   FVector::DistSquared2D(GoalLocation, LastMoveGoal) >= FMath::Square(PathRefreshDistance) ||
							   (GetMoveStatus() == EPathFollowingStatus::Idle && !bAtGoal);

	if (bNeedsNewPath)
	{
		if (SurroundManager != nullptr && !SurroundManager->TryConsumeMoveRequestBudget(bCombatPriority))
		{
			// 保留旧路径但标记待刷新，让下一次分散后的 AI 决策继续重试。
			bHasMoveGoal = false;
			INC_DWORD_STAT(STAT_fpstrueAIMoveBudgetRejectedCount);
			CSV_CUSTOM_STAT(fpstrueAI, MoveBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
			return;
		}

		INC_DWORD_STAT(STAT_fpstrueAIMoveRequestCount);
		CSV_CUSTOM_STAT(fpstrueAI, MoveRequestCount, 1, ECsvCustomStatOp::Accumulate);
		const EPathFollowingRequestResult::Type MoveResult =
			MoveToLocation(GoalLocation, AcceptanceRadius, true, true, true, false, nullptr, true);
		LastMoveGoal = GoalLocation;
		bHasMoveGoal = MoveResult == EPathFollowingRequestResult::RequestSuccessful;
	}
}

bool AfpstrueEnemyAIController::TryAcquireAttackPermission()
{
	return SurroundManager == nullptr || SurroundManager->TryAcquireAttackPermission(ControlledEnemy);
}

void AfpstrueEnemyAIController::ReleaseAttackPermission()
{
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseAttackPermission(ControlledEnemy);
	}
}

void AfpstrueEnemyAIController::StopMovementIfNeeded()
{
	if (bHasMoveGoal || GetMoveStatus() != EPathFollowingStatus::Idle)
	{
		StopMovement();
	}
	bHasMoveGoal = false;
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
	return Cast<AfpstrueSurroundManager>(UGameplayStatics::GetActorOfClass(this, AfpstrueSurroundManager::StaticClass()));
}

bool AfpstrueEnemyAIController::IsTargetUsable(const AfpstrueCharacter* Target) const
{
	return Target != nullptr && !Target->IsDead();
}
