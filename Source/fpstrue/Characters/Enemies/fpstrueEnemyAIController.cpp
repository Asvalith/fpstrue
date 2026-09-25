// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Testing/Benchmarks/fpstrueBenchmarkConfig.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Testing/Benchmarks/fpstruePerformanceStats.h"
#include "Characters/Enemies/fpstrueSurroundManager.h"
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
 * BehaviorTree 的 Selector 拥有行为优先级，Controller 只执行叶子行为；角色执行移动、动画和伤害表现，
 * SurroundManager 则提供跨敌人的槽位与预算，三者职责互不重叠。
 *
 * 单轮决策链：
 *   自适应等待 -> 采样 Blackboard -> Selector 选择行为
 *   -> 需要移动时先做目标去重 -> 申请全局 MoveTo 预算 -> 提交给 PathFollowing。
 *
 * 状态所有权：AIState、目标、上次移动目标和失败退避属于 Controller；攻击事务属于 CombatComponent；
 * 槽位、攻击名额和帧级 MoveTo 预算属于 SurroundManager。BT 任务仍在 Game Thread 执行，降频不是多线程。
 *
 * 朝向顺序：沿路径移动 -> 到达接受范围并停稳 -> 面向玩家 -> 实际角度满足后才播放攻击。
 * Chase 同时包含行进和到位等待，不能仅凭 Chase 枚举持续锁定某一种旋转来源；
 * MoveTo 被预算拒绝、失败或只走到部分路径末端，也不能误判为已经到位。
 */

// ==================== 生命周期入口与外部上下文 ====================

AfpstrueEnemyAIController::AfpstrueEnemyAIController()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AfpstrueEnemyAIController::OnPossess(APawn* InPawn)
{
	// Possess 是 AI 生命周期入口：解析受控敌人、清空移动缓存，再启动错峰的行为树。
	Super::OnPossess(InPawn);
	bDisableDecisionThrottlingForBenchmark = FFPBenchmarkConfig::Get().bDisableAIThrottling;

	ControlledEnemy = Cast<AfpstrueEnemyCharacter>(InPawn);
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return;
	}

	AIState = EFPEnemyAIState::Idle;
	DecisionContext = FDecisionContext{};
	bHasMoveGoal = false;
	NextMoveRetryTime = 0.0f;
	bLastMoveGoalWasCombatPriority = false;
	ApplyRotationPolicy(AIState);

	if (ControlledEnemy->IsDead())
	{
		StopAI();
		return;
	}

	if (UCharacterMovementComponent* Movement = ControlledEnemy->GetCharacterMovement())
	{
		// 已保存的蓝图可能覆盖构造默认值，因此在运行时显式恢复 RVO，保证候选回切不受资产旧值影响。
		// PathFollowing 仍生成路径与期望速度，CharacterMovement 在执行速度时负责邻域避让。
		Movement->SetAvoidanceEnabled(true);
	}

	SurroundManager = ResolveSurroundManager();
	StartBehaviorLogic();
}

void AfpstrueEnemyAIController::OnUnPossess()
{
	// 先停树和导航并归还群体资源，再清空引用，防止下一次 Possess 消费旧目标。
	// StopAI 已统一清空目标、决策与移动缓存，不在解除占有时重复维护第二份清理。
	StopAI();
	ActiveBehaviorTree = nullptr;
	ControlledEnemy = nullptr;
	SurroundManager = nullptr;

	Super::OnUnPossess();
}

void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 退出世界前停止行为树并归还群体资源；Pawn 自身销毁和关卡切换都走同一清理路径。
	StopAI();
	ActiveBehaviorTree = nullptr;
	Super::EndPlay(EndPlayReason);
}

void AfpstrueEnemyAIController::InitializeCombatContext(AfpstrueCharacter* NewTargetCharacter, AfpstrueSurroundManager* NewSurroundManager)
{
	// Controller 只保存敌人决策上下文；共享包围目标由 GameMode 统一初始化。
	TargetCharacter = NewTargetCharacter;
	SurroundManager = NewSurroundManager;
}

void AfpstrueEnemyAIController::ApplyBenchmarkPathFollowingTickOverride(bool bDisablePathFollowingTick)
{
	// 仅供破坏性诊断测量 PathFollowing 成本上界；正式玩法和完整基线不会传入关闭开关。
	if (UPathFollowingComponent* PathFollowing = GetPathFollowingComponent())
	{
		PathFollowing->SetComponentTickEnabled(!bDisablePathFollowingTick);
	}
}

void AfpstrueEnemyAIController::SetSignificanceDecisionMultiplier(float NewMultiplier)
{
	// Significance 只放大非紧急状态的下一轮决策间隔，倍率最低为 1，不能反向提高频率。
	const float SafeMultiplier = FMath::IsFinite(NewMultiplier) ? FMath::Max(1.0f, NewMultiplier) : 1.0f;
	const bool bPromoted = SafeMultiplier < SignificanceDecisionMultiplier;
	SignificanceDecisionMultiplier = SafeMultiplier;
	if (bPromoted && BrainComponent != nullptr && BrainComponent->IsRunning())
	{
		// 提升玩法重要性时取消旧的远距等待；进入近战不能继续耗尽原 Background 长间隔。
		NextBehaviorDecisionDelay = FMath::Max(0.01f, AttackDecisionInterval);
		BrainComponent->RestartLogic();
	}
}

// ==================== 启动唯一行为树 ====================

void AfpstrueEnemyAIController::StartBehaviorLogic()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	// 原生默认模板在世界内共享；运行中的 Blackboard/节点内存由 UE 分别持有。
	ActiveBehaviorTree = BehaviorTreeAsset != nullptr ? BehaviorTreeAsset.Get() : FPGetDefaultEnemyBehaviorTree(*World);
	if (ActiveBehaviorTree == nullptr || !FPHasEnemyBlackboardSchema(ActiveBehaviorTree->BlackboardAsset))
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy BehaviorTree %s has an incompatible Blackboard; using native default."),
			   *GetNameSafe(ActiveBehaviorTree));
		ActiveBehaviorTree = FPGetDefaultEnemyBehaviorTree(*World);
	}
	if (ActiveBehaviorTree == nullptr || ActiveBehaviorTree->RootNode == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("No valid enemy BehaviorTree is available on %s"), *GetName());
		StopAI();
		return;
	}
	// 随机首帧延迟把批量生成的敌人错开，避免所有 Controller 在同一帧同时第一次决策。
	NextBehaviorDecisionDelay = FMath::FRandRange(0.01f, FMath::Max(0.01f, ChaseDecisionInterval * SignificanceDecisionMultiplier));
	if (!RunBehaviorTree(ActiveBehaviorTree))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to start enemy BehaviorTree on %s"), *GetName());
		StopAI();
	}
}

// ==================== 单轮采样：校验上下文 → 距离快照 → 更新间隔 ====================

void AfpstrueEnemyAIController::RefreshBehaviorDecision(UBlackboardComponent& DecisionBlackboard)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_SampleDecision);
	CSV_SCOPED_TIMING_STAT(fpstrueAI, DecisionTime);
	CSV_CUSTOM_STAT(fpstrueAI, DecisionCount, 1, ECsvCustomStatOp::Accumulate);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAIDecisionTime);
	INC_DWORD_STAT(STAT_fpstrueAIDecisionCount);

	// 一次采样同时供 Blackboard 条件和下一轮间隔复用；Selector 只负责选择叶子。
	const bool bHasTarget = BuildDecisionContext();
	NextBehaviorDecisionDelay =
		FMath::Max(0.01f, bHasTarget ? GetNextDecisionInterval(DecisionContext) : IdleDecisionInterval * SignificanceDecisionMultiplier);
	DecisionBlackboard.SetValueAsObject(FPEnemyBlackboard::TargetActor, bHasTarget ? TargetCharacter.Get() : nullptr);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::HasTarget, bHasTarget);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::Attacking, bHasTarget && ControlledEnemy->IsAttacking());
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::InAttackRange, DecisionContext.bInAttackRange);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::InChaseRange, DecisionContext.bInChaseRange);
}

bool AfpstrueEnemyAIController::BuildDecisionContext()
{
	DecisionContext = FDecisionContext{};
	// 统一修复或拒绝敌人、目标和 Manager 上下文，保证后续决策分支可以直接使用这些引用。
	if (!IsValid(ControlledEnemy) || ControlledEnemy->IsDead())
	{
		// 不在采样任务的 ExecuteTask 内重入 StopTree；死亡入口会停止树，本轮走安全 Idle 叶子。
		return false;
	}

	if (!IsValid(SurroundManager))
	{
		SurroundManager = ResolveSurroundManager();
	}
	if (!IsTargetUsable(TargetCharacter))
	{
		// 平时使用 GameMode 注入的玩家；仅在缓存目标失效时安全解析，不在正常路径反复查询。
		TargetCharacter = Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
	}
	if (!IsTargetUsable(TargetCharacter))
	{
		return false;
	}

	// 一轮只计算一次二维距离平方，并派生攻击/追击两个布尔条件，避免各分支重复开方和取位置。
	// 敌人和目标校验成功后才采样；叶子保留存活检查，但不重复计算距离与范围。
	DecisionContext.DistanceSquared = FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), TargetCharacter->GetActorLocation());
	const UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
	DecisionContext.EffectiveAttackRange = Combat != nullptr ? Combat->GetEffectiveAttackRange() : 0.0f;
	DecisionContext.bInAttackRange = DecisionContext.DistanceSquared <= FMath::Square(DecisionContext.EffectiveAttackRange);
	DecisionContext.bInChaseRange = DecisionContext.DistanceSquared <= FMath::Square(ControlledEnemy->GetChaseRange());
	return true;
}

float AfpstrueEnemyAIController::GetNextDecisionInterval(const FDecisionContext& Context) const
{
	// 战斗响应优先于 Significance 降频；Reduced/Background 倍率只放大追击、远距和 Idle 间隔。
	if (bDisableDecisionThrottlingForBenchmark)
	{
		return AttackDecisionInterval;
	}

	// 编辑器 Clamp 不约束所有运行时来源；非有限值回退到旧默认值，再限制有效倍率。
	const float SafeCombatResponseRangeMultiplier =
		FMath::IsFinite(CombatResponseRangeMultiplier) ? FMath::Clamp(CombatResponseRangeMultiplier, 1.0f, 10.0f) : 1.5f;
	const bool bNeedsCombatResponse =
		ControlledEnemy->IsAttacking() || AIState == EFPEnemyAIState::Attack ||
		Context.DistanceSquared <= FMath::Square(Context.EffectiveAttackRange * SafeCombatResponseRangeMultiplier);
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

AfpstrueSurroundManager* AfpstrueEnemyAIController::ResolveSurroundManager() const
{
	// 仅在注入失败或引用失效时兜底查找关卡中的唯一 Manager。
	return Cast<AfpstrueSurroundManager>(UGameplayStatics::GetActorOfClass(this, AfpstrueSurroundManager::StaticClass()));
}

bool AfpstrueEnemyAIController::IsTargetUsable(const AfpstrueCharacter* Target) const
{
	// 目标必须存在且存活；死亡后的停止、槽位释放由上层统一处理。
	return IsValid(Target) && !Target->IsDead();
}

// ==================== 叶子执行：BT 选择行为，Controller 执行动作 ====================

bool AfpstrueEnemyAIController::ExecuteBehaviorAction(EFPEnemyBehaviorAction Action)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_ExecuteBehaviorAction);
	CSV_SCOPED_TIMING_STAT(fpstrueAI, DecisionTime);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAIDecisionTime);
	// 叶子仍校验对象存活，避免自定义树跳过采样，或外部回调使目标失效。
	if (Action == EFPEnemyBehaviorAction::Idle)
	{
		// 没有目标或超出追击范围才停路，并立即归还槽位，避免 Idle 敌人长期占用共享资源。
		// 远距离但仍在追击范围内的敌人仍可沿共享目标追踪，不能仅凭远距档位直接休眠。
		ReleaseSurroundSlot();
		ReleaseAttackPermission();
		SetAIState(ControlledEnemy != nullptr && ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
		StopMovementIfNeeded();
		return true;
	}
	if (!IsValid(ControlledEnemy) || ControlledEnemy->IsDead() || !IsTargetUsable(TargetCharacter))
	{
		return false;
	}

	switch (Action)
	{
	case EFPEnemyBehaviorAction::SustainAttack:
		return HandleActiveAttack();
	case EFPEnemyBehaviorAction::TryAttack:
	{
		// 冷却/预算失败交由 BT 的下一个叶子维持站位，而不是在 Controller 再跑一套分支链。
		// 冷却中的敌人继续维持包围位置，不申请并立即释放攻击名额。
		UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
		if (Combat == nullptr || !Combat->CanStartAttackAtDistanceSquared(DecisionContext.DistanceSquared))
		{
			return false;
		}
		// 向 SurroundManager 申请并发攻击名额；无 Manager 的退化场景仍允许单个敌人正常攻击。
		if (SurroundManager != nullptr && !SurroundManager->TryAcquireAttackPermission(ControlledEnemy))
		{
			// 攻击预算只限制攻击事务，拒绝者仍由后备叶子维持 Chase 和包围槽位，不是关闭 AI。
			INC_DWORD_STAT(STAT_fpstrueAIAttackBudgetRejectedCount);
			CSV_CUSTOM_STAT(fpstrueAI, AttackBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
			return false;
		}
		SetAIState(EFPEnemyAIState::Chase);
		// 设置 ControlRotation 不代表角色已经转正：Movement 需要后续更新才能按 RotationRate 转过去。
		// 准备阶段不启动 Montage/伤害事务，也不长期占用攻击名额；下一轮重新校验距离、冷却与预算。
		if (!FaceTargetAtRest())
		{
			// 转向未完成时结束本轮，不让 fallback 重新 MoveTo；下一轮再次检查冷却和许可。
			ReleaseAttackPermission();
			return true;
		}
		SetAIState(EFPEnemyAIState::Attack);
		if (!Combat->TryAttackTarget())
		{
			ReleaseAttackPermission();
			SetAIState(EFPEnemyAIState::Chase);
			return false;
		}
		return true;
	}
	case EFPEnemyBehaviorAction::MaintainCombatPosition:
		MaintainCombatPosition();
		return true;
	case EFPEnemyBehaviorAction::Surround:
		return HandleSurroundMovement();
	case EFPEnemyBehaviorAction::Pursue:
		SetAIState(EFPEnemyAIState::Chase);
		HandleSharedPursuit();
		return true;
	default:
		return false;
	}
}

bool AfpstrueEnemyAIController::HandleActiveAttack()
{
	// 攻击事务已经开始时保持 Attack 和面向目标，不允许本轮决策重新提交移动。
	if (!ControlledEnemy->IsAttacking())
	{
		return false;
	}

	SetAIState(EFPEnemyAIState::Attack);
	StopMovementIfNeeded(true);
	UpdateFacingTarget();
	return true;
}

void AfpstrueEnemyAIController::MaintainCombatPosition()
{
	// 冷却和预算拒绝共用等待行为，避免一个分支清了移动、另一个分支却残留 Attack 旋转策略。
	SetAIState(EFPEnemyAIState::Chase);
	if (!HandleSurroundMovement())
	{
		FaceTargetAtRest();
	}
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
	// 没有可用环形槽位时追逐 Manager 的共享玩家位置；Manager 失效才直接读取玩家 Transform。
	FVector SharedGoal;
	if (SurroundManager != nullptr && SurroundManager->GetSharedTargetSnapshot(SharedGoal))
	{
		const float SafePursuitAcceptanceRangeMultiplier =
			FMath::IsFinite(PursuitAcceptanceRangeMultiplier) ? FMath::Clamp(PursuitAcceptanceRangeMultiplier, 0.0f, 1.0f) : 0.8f;
		const UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
		const float ConfiguredAttackRange = Combat != nullptr ? Combat->GetConfiguredAttackRange() : 0.0f;
		const float PursuitAcceptanceRadius =
			FMath::Max(MoveAcceptanceRadius, ConfiguredAttackRange * SafePursuitAcceptanceRangeMultiplier);
		MoveToGoal(SharedGoal, PursuitAcceptanceRadius, false);
		return;
	}

	MoveToGoal(TargetCharacter->GetActorLocation(), MoveAcceptanceRadius, false);
}

// ==================== 移动与朝向：请求去重 → 预算 → 到达后转向 ====================

void AfpstrueEnemyAIController::MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority)
{
	// 去重和预算都发生在提交 PathFollowing 之前；被限流时保留旧路径，下轮仍可重试，不会让角色原地急停。
	const bool bSameGoal = bCombatPriority == bLastMoveGoalWasCombatPriority &&
						   FVector::DistSquared2D(GoalLocation, LastMoveGoal) < FMath::Square(PathRefreshDistance);
	if (bSameGoal && bHasMoveGoal)
	{
		if (GetMoveStatus() != EPathFollowingStatus::Idle)
		{
			// 目标没变且路径仍在执行：保持路径朝向，不因靠近玩家或上次原地等待而横移。
			ApplyRotationPolicy(EFPEnemyAIState::Chase);
			return;
		}

		const UPathFollowingComponent* PathFollowing = GetPathFollowingComponent();
		if (PathFollowing != nullptr &&
			PathFollowing->HasReached(LastResolvedMoveGoal, EPathFollowingReachMode::OverlapAgent, AcceptanceRadius))
		{
			// 复用引擎含胶囊半径/高度的到达规则，并检查上次请求经过导航投影后的目标。
			// 成功到位后缓存继续有效，只刷新朝向，不反复提交 AlreadyAtGoal 请求。
			FaceTargetAtRest();
			return;
		}

		// Idle 也可能是路径中断、部分路径走完或角色被挤离槽位，不能永久用旧缓存阻止重寻路。
		bHasMoveGoal = false;
		if (const UWorld* RetryWorld = GetWorld())
		{
			NextMoveRetryTime = RetryWorld->GetTimeSeconds() + FMath::Max(FailedMoveRetryDelay, 0.1f);
		}
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
	// 使用显式请求替代一串位置布尔参数，保留原 MoveToLocation 的导航、接受半径和部分路径语义。
	// 与引擎包装函数一致，提交新请求前取消旧路径，但保留速度；不得在预算拒绝之前执行这一步。
	if (UPathFollowingComponent* PathFollowing = GetPathFollowingComponent();
		PathFollowing != nullptr && PathFollowing->GetStatus() != EPathFollowingStatus::Idle)
	{
		PathFollowing->AbortMove(*this, FPathFollowingResultFlags::ForcedScript | FPathFollowingResultFlags::NewRequest,
								 FAIRequestID::CurrentRequest, EPathFollowingVelocityMode::Keep);
	}

	FAIMoveRequest MoveRequest(GoalLocation);
	MoveRequest.SetUsePathfinding(true);
	MoveRequest.SetAllowPartialPath(true);
	MoveRequest.SetProjectGoalLocation(!bCombatPriority); // 环形槽位已由 Manager 集中投影。
	MoveRequest.SetNavigationFilter(DefaultNavigationFilterClass);
	MoveRequest.SetAcceptanceRadius(AcceptanceRadius);
	MoveRequest.SetReachTestIncludesAgentRadius(true);
	MoveRequest.SetCanStrafe(false);
	const EPathFollowingRequestResult::Type MoveResult = MoveTo(MoveRequest).Code;
	LastMoveGoal = GoalLocation;
	// UE MoveTo 会把投影结果写回请求。不能用部分路径末端替代它，否则到达死路也会被当成到位。
	LastResolvedMoveGoal = MoveRequest.GetGoalLocation();
	bLastMoveGoalWasCombatPriority = bCombatPriority;
	bHasMoveGoal = MoveResult != EPathFollowingRequestResult::Failed;
	if (bHasMoveGoal)
	{
		NextMoveRetryTime = 0.0f;
		if (MoveResult == EPathFollowingRequestResult::AlreadyAtGoal)
		{
			FaceTargetAtRest();
		}
		else
		{
			// 只有接受了新移动请求才撤销站定朝向；预算拒绝不打断旧路径，也不伪造到达。
			ApplyRotationPolicy(EFPEnemyAIState::Chase);
		}
	}
	else if (World != nullptr)
	{
		NextMoveRetryTime = World->GetTimeSeconds() + FMath::Max(FailedMoveRetryDelay, 0.1f);
	}
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

bool AfpstrueEnemyAIController::FaceTargetAtRest()
{
	// 停路和停速度不是同一个动作：加速度寻路模式可能在 StopMovement 后继续制动。
	// 只在地面进入站定朝向；不清空腾空角色的重力速度，也不把跳跃当成到位。
	UCharacterMovementComponent* Movement = ControlledEnemy != nullptr ? ControlledEnemy->GetCharacterMovement() : nullptr;
	if (Movement == nullptr || !Movement->IsMovingOnGround())
	{
		// 未能站定时保留 Chase 的路径朝向，不能提前切到 Attack 后边赶路边面向玩家。
		ApplyRotationPolicy(AIState);
		return false;
	}

	StopMovementIfNeeded(true);
	ControlledEnemy->ConsumeMovementInputVector();
	Movement->StopMovementImmediately();
	ApplyRotationPolicy(AIState, true);
	return UpdateFacingTarget();
}

bool AfpstrueEnemyAIController::UpdateFacingTarget()
{
	// 仅站定/攻击时离散刷新期望朝向，移动阶段的连续转向交给 CharacterMovement。
	if (ControlledEnemy == nullptr || !IsTargetUsable(TargetCharacter))
	{
		return false;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - ControlledEnemy->GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	if (HorizontalToTarget.IsNearlyZero())
	{
		// 同一水平位置没有唯一朝向，不应永久卡在等待转正；距离/伤害边界仍由战斗组件校验。
		return true;
	}

	FRotator TargetRotation = HorizontalToTarget.Rotation();
	TargetRotation.Pitch = 0.0f;
	TargetRotation.Roll = 0.0f;
	SetControlRotation(TargetRotation);
	const float YawError = FMath::Abs(FMath::FindDeltaAngleDegrees(ControlledEnemy->GetActorRotation().Yaw, TargetRotation.Yaw));
	return YawError <= FMath::Clamp(AttackFacingToleranceDegrees, 0.0f, 90.0f);
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

void AfpstrueEnemyAIController::ApplyRotationPolicy(EFPEnemyAIState NewState, bool bAtRestFacingTarget)
{
	// 把状态到旋转来源的映射集中设置，防止 MoveTo 与面向玩家同时争夺角色朝向。
	if (ControlledEnemy == nullptr)
	{
		return;
	}

	if (UCharacterMovementComponent* Movement = ControlledEnemy->GetCharacterMovement())
	{
		// 移动朝向使用 Acceleration/RequestedVelocity，而不是直接朝玩家；RVO 的瞬时侧向避让仍可能存在。
		// Attack 和站定的 Chase（含转向准备）使用 Controller 朝向；Idle/Dead 关闭两种来源。
		const bool bFaceTarget = NewState == EFPEnemyAIState::Attack || (NewState == EFPEnemyAIState::Chase && bAtRestFacingTarget);
		Movement->bOrientRotationToMovement = NewState == EFPEnemyAIState::Chase && !bFaceTarget;
		Movement->bUseControllerDesiredRotation = bFaceTarget;
	}
}

// ==================== 停止与公共清理 ====================

void AfpstrueEnemyAIController::StopAI()
{
	// 先停行为树，再停止导航和清理资源；否则下一次 BT 唤醒可能重新发起 MoveTo。
	// 敌人死亡、解除占有和对局结束都走同一收口，确保路径、行为树、槽位和攻击名额不会只清理一部分。
	if (BrainComponent != nullptr)
	{
		BrainComponent->StopLogic(TEXT("Enemy stopped"));
	}
	StopMovementIfNeeded();
	ReleaseAttackPermission();
	TargetCharacter = nullptr;
	ReleaseSurroundSlot();
	DecisionContext = FDecisionContext{};
	if (UBlackboardComponent* DecisionBlackboard = GetBlackboardComponent())
	{
		// RunBehaviorTree 可以复用兼容 Blackboard；停止后不能保留上一轮目标和条件。
		DecisionBlackboard->ClearValue(FPEnemyBlackboard::TargetActor);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::HasTarget, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::Attacking, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::InAttackRange, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::InChaseRange, false);
	}

	if (ControlledEnemy != nullptr)
	{
		SetAIState(ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
	}
}

void AfpstrueEnemyAIController::StopMovementIfNeeded(bool bPreserveMoveGoal)
{
	// 只有 PathFollowing 正在运行时才提交 StopMovement；按调用场景决定是否保留目标缓存。
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

void AfpstrueEnemyAIController::ReleaseAttackPermission()
{
	// 攻击结束、失败、死亡和解除占有均可调用，Manager 的 TSet 保证重复释放无副作用。
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseAttackPermission(ControlledEnemy);
	}
}

void AfpstrueEnemyAIController::ReleaseSurroundSlot()
{
	// 将敌人与稳定槽位的映射交还给群体 Manager，并触发可能的外环补位。
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseSurroundSlot(ControlledEnemy);
	}
}
