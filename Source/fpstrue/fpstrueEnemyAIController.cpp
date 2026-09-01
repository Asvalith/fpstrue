// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyAIController.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstruePerformanceStats.h"
#include "fpstrueSurroundManager.h"
#include "AITypes.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Navigation/PathFollowingComponent.h"
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

	AIState = EFPEnemyAIState::Idle;
	bHasMoveGoal = false;
	NextMoveRetryTime = 0.0f;
	bLastMoveGoalWasCombatPriority = false;
	ApplyRotationPolicy(AIState);

	if (ControlledEnemy->IsDead())
	{
		StopAI();
		return;
	}

	SurroundManager = ResolveSurroundManager();
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
	NextMoveRetryTime = 0.0f;
	bLastMoveGoalWasCombatPriority = false;

	Super::OnUnPossess();
}

void AfpstrueEnemyAIController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	// 主动 StopMovement 和新 MoveTo 都会产生 Aborted；只有真实寻路失败才进入退避。
	if (Result.IsFailure() && Result.Code != EPathFollowingResult::Aborted)
	{
		bHasMoveGoal = false;
		if (const UWorld* World = GetWorld())
		{
			NextMoveRetryTime = World->GetTimeSeconds() + FMath::Max(FailedMoveRetryDelay, 0.1f);
		}
	}

	Super::OnMoveCompleted(RequestID, Result);
}

void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearDecisionTimer();
	bHasMoveGoal = false;
	ReleaseSurroundSlot();
	Super::EndPlay(EndPlayReason);
}

void AfpstrueEnemyAIController::StopAI()
{
	StopMovementIfNeeded();
	ClearDecisionTimer();
	TargetCharacter = nullptr;
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
		if (AIState != EFPEnemyAIState::Idle)
		{
			// 退出追击范围后立即归还槽位，避免远处 Idle 敌人长期占用共享资源。
			ReleaseSurroundSlot();
		}
		SetAIState(EFPEnemyAIState::Idle);
		StopMovementIfNeeded();
		return;
	}

	if (Context.bInAttackRange)
	{
		// 冷却中的敌人继续维持包围位置，不申请并立即释放攻击名额。
		if (!ControlledEnemy->CanStartAttack())
		{
			SetAIState(EFPEnemyAIState::Chase);
			if (!HandleSurroundMovement())
			{
				StopMovementIfNeeded();
			}
			return;
		}

		if (!TryAcquireAttackPermission())
		{
			INC_DWORD_STAT(STAT_fpstrueAIAttackBudgetRejectedCount);
			CSV_CUSTOM_STAT(fpstrueAI, AttackBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
			HandleSurroundMovement();
			return;
		}

		SetAIState(EFPEnemyAIState::Attack);
		StopMovementIfNeeded(true);
		UpdateFacingTarget();
		if (!ControlledEnemy->TryAttackTarget())
		{
			ReleaseAttackPermission();
			SetAIState(EFPEnemyAIState::Chase);
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
	// UpdateAI 只会在 PrepareDecisionContext 成功后调用，避免同一轮重复校验敌人和目标。
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

	const bool bNeedsCombatResponse = ControlledEnemy->IsAttacking() || AIState == EFPEnemyAIState::Attack ||
									  Context.DistanceSquared <= FMath::Square(ControlledEnemy->GetEffectiveAttackRange() * 1.5f);
	if (bNeedsCombatResponse)
	{
		return AttackDecisionInterval;
	}

	if (!Context.bInChaseRange)
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
	StopMovementIfNeeded(true);
	UpdateFacingTarget();
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
	MoveToGoal(AttackGoal, CombatMoveAcceptanceRadius, true);
	return true;
}

void AfpstrueEnemyAIController::HandleSharedPursuit()
{
	FVector SharedGoal;
	if (SurroundManager != nullptr && SurroundManager->GetSharedTargetSnapshot(SharedGoal))
	{
		const float PursuitAcceptanceRadius = FMath::Max(MoveAcceptanceRadius, ControlledEnemy->GetAttackRange() * 0.8f);
		MoveToGoal(SharedGoal, PursuitAcceptanceRadius, false);
		return;
	}

	MoveToGoal(TargetCharacter->GetActorLocation(), MoveAcceptanceRadius, false);
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
	const bool bSameGoal = bCombatPriority == bLastMoveGoalWasCombatPriority &&
		FVector::DistSquared2D(GoalLocation, LastMoveGoal) < FMath::Square(PathRefreshDistance);
	if (bSameGoal && bHasMoveGoal)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (bSameGoal && World != nullptr && World->GetTimeSeconds() < NextMoveRetryTime)
	{
		return;
	}

	if (SurroundManager != nullptr && !SurroundManager->TryConsumeMoveRequestBudget(bCombatPriority))
	{
		// 预算拒绝时继续沿旧路径移动；LastMoveGoal 不更新，下一轮仍会识别到待刷新的目标。
		INC_DWORD_STAT(STAT_fpstrueAIMoveBudgetRejectedCount);
		CSV_CUSTOM_STAT(fpstrueAI, MoveBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
		return;
	}

	INC_DWORD_STAT(STAT_fpstrueAIMoveRequestCount);
	CSV_CUSTOM_STAT(fpstrueAI, MoveRequestCount, 1, ECsvCustomStatOp::Accumulate);
	const EPathFollowingRequestResult::Type MoveResult =
		MoveToLocation(GoalLocation, AcceptanceRadius, true, true, !bCombatPriority, false, nullptr, true);
	LastMoveGoal = GoalLocation;
	bLastMoveGoalWasCombatPriority = bCombatPriority;
	bHasMoveGoal = MoveResult != EPathFollowingRequestResult::Failed;
	if (bHasMoveGoal)
	{
		NextMoveRetryTime = 0.0f;
	}
	else if (World != nullptr)
	{
		NextMoveRetryTime = World->GetTimeSeconds() + FMath::Max(FailedMoveRetryDelay, 0.1f);
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

void AfpstrueEnemyAIController::StopMovementIfNeeded(bool bPreserveMoveGoal)
{
	const bool bShouldStop = GetMoveStatus() != EPathFollowingStatus::Idle;
	if (!bPreserveMoveGoal || bShouldStop)
	{
		bHasMoveGoal = false;
		NextMoveRetryTime = 0.0f;
		bLastMoveGoalWasCombatPriority = false;
	}
	if (bShouldStop)
	{
		StopMovement();
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
	ApplyRotationPolicy(NewState);
}

void AfpstrueEnemyAIController::ApplyRotationPolicy(EFPEnemyAIState NewState)
{
	if (ControlledEnemy == nullptr)
	{
		return;
	}

	if (UCharacterMovementComponent* Movement = ControlledEnemy->GetCharacterMovement())
	{
		// Chase 只读取路径速度；Attack 只读取 Controller 朝向；Idle/Dead 不再继续改写朝向。
		Movement->bOrientRotationToMovement = NewState == EFPEnemyAIState::Chase;
		Movement->bUseControllerDesiredRotation = NewState == EFPEnemyAIState::Attack;
	}
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
