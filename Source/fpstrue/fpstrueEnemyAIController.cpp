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

/*
 * 单个敌人的决策与寻路所有者。
 * Controller 用一次性 Timer 驱动低频状态机，不启用 Actor Tick；角色只执行移动、动画和伤害表现，
 * SurroundManager 则提供跨敌人的槽位与预算，三者职责互不重叠。
 *
 * 单轮决策链：
 *   校验 Pawn/目标 -> 生成距离上下文 -> 先安排下一次 Timer -> 按 Idle/Chase/Attack 优先级处理
 *   -> 需要移动时先做目标去重 -> 申请全局 MoveTo 预算 -> 提交给 PathFollowing。
 *
 * 状态所有权：AIState、目标、上次移动目标和失败退避属于 Controller；攻击事务属于 CombatComponent；
 * 槽位、攻击名额和帧级 MoveTo 预算属于 SurroundManager。Timer 仍在 Game Thread 执行，降频不是多线程。
 */

// ==================== Possess 生命周期与外部上下文 ====================

AfpstrueEnemyAIController::AfpstrueEnemyAIController()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AfpstrueEnemyAIController::OnPossess(APawn* InPawn)
{
	// Possess 是 AI 生命周期入口：解析受控敌人、清空上一个 Pawn 的移动缓存，再启动错峰的一次性决策 Timer。
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
	// 先归还 Timer、槽位和攻击权限，再清空裸指针；避免 Manager 中残留一个已经失去 Pawn 的参与者。
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
	// 死亡、玩家失效和对局结束都走同一收口，确保路径、Timer、槽位和攻击名额不会只清理一部分。
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

// ==================== Timer 驱动的决策调度 ====================

void AfpstrueEnemyAIController::StartDecisionTimer()
{
	// 随机首帧延迟把批量生成的敌人错开，避免所有 Controller 在同一帧同时第一次决策。
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
	// 这里是唯一决策入口；先安排下一轮可保证任一行为分支提前 return 时仍能继续运行状态机。
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
		// 超出追击范围才真正进入 Idle 并停路；远距离但仍在追击范围内的敌人仍可沿共享目标追踪。
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
			// 攻击预算只限制攻击事务，拒绝者仍保持 Chase 并维持包围槽位，不能误解为关闭该敌人的 AI。
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

// ==================== 单轮决策上下文与优先级分支 ====================

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
	// 战斗响应优先于 Significance 降频；Reduced/Background 倍率只放大追击、远距和 Idle 间隔。
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
	// Controller 不计算全局站位，只消费 SurroundManager 已缓存并投影到 NavMesh 的接近点。
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

// ==================== MoveTo 去重、限流与失败退避 ====================

void AfpstrueEnemyAIController::MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority)
{
	// 去重和预算都发生在提交 PathFollowing 之前；被限流时保留旧路径，下轮仍可重试，不会让角色原地急停。
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

// ==================== 共享资源回收与状态切换 ====================

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
	// 状态变化只在 Controller 内提交，并在同一位置更新旋转策略，避免决策分支各自修改朝向产生抖动。
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
