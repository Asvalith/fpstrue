// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Game/fpstrueMultiplayer.h"
#include "AITypes.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavFilters/NavigationQueryFilter.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
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

// 创建视觉感知组件并关闭 Controller Actor Tick；高层决策由行为树间隔任务驱动。
AfpstrueEnemyAIController::AfpstrueEnemyAIController()
{
	PrimaryActorTick.bCanEverTick = false;
	UAIPerceptionComponent* Perception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("EnemyPerception"));
	SetPerceptionComponent(*Perception);
	UAISenseConfig_Sight* Sight = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("EnemySight"));
	Sight->SightRadius = 4500.0f;
	Sight->LoseSightRadius = 5000.0f;
	Sight->PeripheralVisionAngleDegrees = 80.0f;
	Sight->SetMaxAge(0.0f); // 搜索期限由 Controller 管理，不与刺激老化混用。
	Sight->DetectionByAffiliation.bDetectEnemies = true;
	Sight->DetectionByAffiliation.bDetectFriendlies = false;
	Sight->DetectionByAffiliation.bDetectNeutrals = false;
	Perception->ConfigureSense(*Sight);
	Perception->SetDominantSense(UAISense_Sight::StaticClass());
	Perception->OnTargetPerceptionUpdated.AddDynamic(this, &ThisClass::HandleTargetPerceptionUpdated);
}

// 提供视觉系统的阵营筛选：只发现玩家，避免大量敌人之间建立无用的感知查询对。
ETeamAttitude::Type AfpstrueEnemyAIController::GetTeamAttitudeTowards(const AActor& Other) const
{
	// 本 Demo 只发现玩家；在 Sight 建立查询对之前排除其他敌人，避免群体互相做视觉测试。
	return Other.IsA<AfpstrueCharacter>() ? ETeamAttitude::Hostile : ETeamAttitude::Neutral;
}

// 输出实际 Pawn 视点与朝向，防止视觉范围跟随陈旧的 Controller 转向目标。
void AfpstrueEnemyAIController::GetActorEyesViewPoint(FVector& OutLocation, FRotator& OutRotation) const
{
	if (const APawn* ViewedPawn = GetPawn())
	{
		OutLocation = ViewedPawn->GetPawnViewLocation();
		OutRotation = ViewedPawn->GetActorRotation(); // 视觉跟随真实朝向，不使用到位转向时缓存的 ControlRotation。
	}
	else Super::GetActorEyesViewPoint(OutLocation, OutRotation);
}

// 巡视复用 Movement 的平滑转向；感知成功、暂停/停止或死亡时，原生旋转消费者当帧就不再朝旧巡视方向转动。
FRotator AfpstrueEnemyAIController::GetDesiredRotation() const
{
	if (bIdleSearchActive && IsValid(ControlledEnemy) &&
		(!AcceptsCombatCommands() || ControlledEnemy->IsDead() || bTargetVisible || BrainComponent == nullptr ||
			!BrainComponent->IsRunning() || BrainComponent->IsPaused())) return ControlledEnemy->GetActorRotation();
	return Super::GetDesiredRotation();
}

// 接管有效敌人后重建请求归属、恢复 RVO 并启动行为树；死亡或错误 Pawn 走统一停止出口。
void AfpstrueEnemyAIController::OnPossess(APawn* InPawn)
{
	// Possess 是 AI 生命周期入口：解析受控敌人、清空移动缓存，再启动错峰的行为树。
	Super::OnPossess(InPawn);
	if (!HasAuthority() || GetNetMode() == NM_Client)
	{
		GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), false);
		return;
	}
	bDisableDecisionThrottlingForBenchmark = FFPRuntimeOptions::Get().bDisableAIThrottling;
	bUsesMultiplayerTargets = GetNetMode() != NM_Standalone && GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>() != nullptr;

	ControlledEnemy = Cast<AfpstrueEnemyCharacter>(InPawn);
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return;
	}

	// 新 Pawn 从空闲上下文开始，旧请求的完成回调不能接管新的移动缓存。
	AIState = EFPEnemyAIState::Idle;
	CommandPhase = ECommandPhase::Accepting;
	DecisionContext = FDecisionContext{};
	bHasMoveGoal = false;
	OwnedMoveRequestId = FAIRequestID::InvalidRequest;
	MoveFailures[0] = MoveFailures[1] = FMoveFailure{};
	bRetreatActive = false;
	bHasRetreatGoal = false;
	RetreatEndTime = RetreatHoldUntil = NextRetreatAllowedTime = 0.0;
	++MoveRequestRevision;
	ApplyRotationPolicy(AIState);

	if (ControlledEnemy->IsDead())
	{
		StopAI();
		return;
	}

	// 生命值仍由 Health 唯一持有；Controller 只缓存组件、上轮通知值和撤退事件资格。
	EnemyHealthComponent = ControlledEnemy->FindComponentByClass<UfpstrueHealthComponent>();
	if (EnemyHealthComponent != nullptr)
	{
		LastObservedHealth = EnemyHealthComponent->GetHealth();
		EnemyHealthComponent->OnHealthChanged.AddUniqueDynamic(this, &ThisClass::HandleEnemyHealthChanged);
	}
	bRetreatRequested = IsBelowRetreatThreshold();

	if (UCharacterMovementComponent* Movement = ControlledEnemy->GetCharacterMovement())
	{
		// 已保存的蓝图可能覆盖构造默认值，因此在运行时显式恢复 RVO，保证候选回切不受资产旧值影响。
		// PathFollowing 仍生成路径与期望速度，CharacterMovement 在执行速度时负责邻域避让。
		Movement->SetAvoidanceEnabled(true);
	}

	SurroundManager = ResolveSurroundManager();
	GetPerceptionComponent()->RequestStimuliListenerUpdate();
	StartBehaviorLogic();
}

// 解除占有前停掉决策和攻击、归还群体资源，再释放本次 Pawn 的强引用。
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

// 世界退出复用 StopAI 清理，并释放行为树模板引用。
void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 退出世界前停止行为树并归还群体资源；Pawn 自身销毁和关卡切换都走同一清理路径。
	StopAI();
	ActiveBehaviorTree = nullptr;
	Super::EndPlay(EndPlayReason);
}

// 注入候选玩家和共享管理器；上下文变化时先撤销旧攻击与移动，再按存活状态开放新命令。
void AfpstrueEnemyAIController::InitializeCombatContext(AfpstrueCharacter* NewTargetCharacter, AfpstrueSurroundManager* NewSurroundManager)
{
	// Controller 只保存敌人决策上下文；共享包围目标由 GameMode 统一初始化。
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	if (CommandPhase == ECommandPhase::Stopping || CommandPhase == ECommandPhase::ChangingContext)
	{
		return; // 清理期间不接受外部回调重新注入上下文。
	}
	// 无效/死亡目标统一视为撤销；恢复追击必须由同一个上下文入口重新注入。
	if (!IsTargetUsable(NewTargetCharacter)) NewTargetCharacter = nullptr;
	if (NewTargetCharacter == nullptr) NewSurroundManager = nullptr;
	const bool bContextChanged = TargetCharacter != NewTargetCharacter || SurroundManager != NewSurroundManager;
	const bool bPendingRetreatBeforeContextChange = bRetreatRequested && !bRetreatActive;
	if (bContextChanged)
	{
		// ★【重要边界】[状态提交] 先关闭旧上下文的命令入口，再停止攻击和导航，清理回调不能重启旧行为。
		CommandPhase = ECommandPhase::ChangingContext;
		if (TargetCharacter != nullptr)
		{
			TargetCharacter->OnPlayerDeathReported.RemoveDynamic(this, &ThisClass::HandleCombatTargetDied);
			TargetCharacter->OnEndPlay.RemoveDynamic(this, &ThisClass::HandleCombatTargetEndPlay);
		}
		FinishRetreat(false);
		bTargetVisible = false;
		TargetForgetTime = 0.0;
		LastKnownTargetLocation = FVector::ZeroVector;
		// 单机撤销注入仍忘记全部刺激；多人换目标必须保留其他可见玩家，不能换一次目标就清空整份感知。
		if (!bUsesMultiplayerTargets) GetPerceptionComponent()->ForgetAll();
		if (ControlledEnemy != nullptr && ControlledEnemy->GetCombatComponent() != nullptr)
		{
			ControlledEnemy->GetCombatComponent()->ResetCombat();
		}
		ReleaseSurroundSlot();
		StopMovementIfNeeded();
		DecisionContext = FDecisionContext{};
		// ★【重要边界】[回调重入] 清理可同步触发 StopAI；返回后确认仍在换上下文，才允许发布新目标。
		if (CommandPhase != ECommandPhase::ChangingContext)
		{
			return; // 同步回调调用了 StopAI，外层不能把已经停止的上下文重新激活。
		}
	}
	// 旧上下文清理完成后才发布新引用；持有玩家身份不代表已经通过视觉获得位置。
	if (!IsTargetUsable(NewTargetCharacter)) NewTargetCharacter = nullptr;
	TargetCharacter = NewTargetCharacter;
	SurroundManager = NewTargetCharacter != nullptr ? NewSurroundManager : nullptr;
	if (TargetCharacter != nullptr)
	{
		TargetCharacter->OnPlayerDeathReported.AddUniqueDynamic(this, &ThisClass::HandleCombatTargetDied);
		TargetCharacter->OnEndPlay.AddUniqueDynamic(this, &ThisClass::HandleCombatTargetEndPlay);
	}
	CommandPhase = IsValid(ControlledEnemy) && !ControlledEnemy->IsDead() ? ECommandPhase::Accepting : ECommandPhase::Stopped;
	if (bContextChanged && TargetCharacter != nullptr && GetWorld() != nullptr)
	{
		bRetreatRequested = bPendingRetreatBeforeContextChange && IsBelowRetreatThreshold() && GetWorld()->GetTimeSeconds() >= NextRetreatAllowedTime;
	}
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), AcceptsCombatCommands() && (bUsesMultiplayerTargets || TargetCharacter != nullptr));
	if (TargetCharacter == nullptr)
	{
		SetAIState(ControlledEnemy != nullptr && ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
	}
	if (bContextChanged) RequestBehaviorDecision();
}

// 当前可见目标不因另一个玩家靠近而抖动切换；失去它后才从已感知玩家中选一个新的可见目标。
void AfpstrueEnemyAIController::SelectPerceivedTarget()
{
	if (!bUsesMultiplayerTargets || !AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead()) return;
	if (bTargetVisible && IsTargetUsable(TargetCharacter)) return;
	UAIPerceptionComponent* Perception = GetPerceptionComponent();
	TArray<AActor*> VisiblePlayers;
	Perception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), VisiblePlayers);
	AfpstrueCharacter* Selected = nullptr;
	FVector SelectedLocation = FVector::ZeroVector;
	double BestDistanceSquared = TNumericLimits<double>::Max();
	const FVector EnemyLocation = ControlledEnemy->GetActorLocation();
	for (AActor* Actor : VisiblePlayers)
	{
		AfpstrueCharacter* Candidate = Cast<AfpstrueCharacter>(Actor);
		if (!IsTargetUsable(Candidate)) continue;
		// 位置取已成功的视觉数据，不从隐藏候选的真实 Transform 读取玩家信息。
		const FVector KnownLocation = Perception->GetActorLocation(*Candidate);
		if (KnownLocation.ContainsNaN()) continue;
		const double DistanceSquared = FVector::DistSquared2D(EnemyLocation, KnownLocation);
		if (DistanceSquared < BestDistanceSquared || (DistanceSquared == BestDistanceSquared && Selected != nullptr && Candidate->GetUniqueID() < Selected->GetUniqueID()))
		{
			Selected = Candidate;
			SelectedLocation = KnownLocation;
			BestDistanceSquared = DistanceSquared;
		}
	}
	if (Selected == nullptr) return; // 没有其他可见者时，旧目标只保留有限的位置记忆。
	auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>();
	AfpstrueSurroundManager* SelectedManager = Mode != nullptr ? Mode->GetOrCreateSurroundManager(Selected) : nullptr;
	if (!IsValid(SelectedManager)) return; // 不能因管理器创建失败而绕过联网群体预算。
	InitializeCombatContext(Selected, SelectedManager);
	if (!AcceptsCombatCommands() || TargetCharacter != Selected || !IsTargetUsable(Selected)) return;
	bTargetVisible = true;
	LastKnownTargetLocation = SelectedLocation;
	TargetForgetTime = 0.0;
	RequestBehaviorDecision();
}

void AfpstrueEnemyAIController::NotifyCombatTargetUnavailable(AfpstrueCharacter* UnavailableTarget)
{
	if (!HasAuthority() || GetNetMode() == NM_Client || TargetCharacter != UnavailableTarget) return;
	// EndPlay 广播期间 Actor 可能尚未置为 Destroyed，先删除这份感知缓存，不能在同一个清理栈又选回它。
	if (UnavailableTarget != nullptr) GetPerceptionComponent()->ForgetActor(UnavailableTarget);
	InitializeCombatContext(nullptr, nullptr);
	SelectPerceivedTarget();
}

void AfpstrueEnemyAIController::HandleCombatTargetDied(AfpstrueCharacter* DeadTarget)
{
	NotifyCombatTargetUnavailable(DeadTarget);
}

void AfpstrueEnemyAIController::HandleCombatTargetEndPlay(AActor* EndingTarget, EEndPlayReason::Type)
{
	NotifyCombatTargetUnavailable(Cast<AfpstrueCharacter>(EndingTarget));
}

// 仅用于性能消融，改变 PathFollowing 的组件更新开关，不改正式玩法策略。
void AfpstrueEnemyAIController::ApplyBenchmarkPathFollowingTickOverride(bool bDisablePathFollowingTick)
{
	// 仅供破坏性诊断测量 PathFollowing 成本上界；正式玩法和完整基线不会传入关闭开关。
	if (UPathFollowingComponent* PathFollowing = GetPathFollowingComponent())
	{
		PathFollowing->SetComponentTickEnabled(!bDisablePathFollowingTick);
	}
}

// 安全应用玩法重要性倍率；频率提升时唤醒旧等待，非紧急行为才会被降频。
void AfpstrueEnemyAIController::SetSignificanceDecisionMultiplier(float NewMultiplier)
{
	// Significance 只放大非紧急状态的下一轮决策间隔，倍率最低为 1，不能反向提高频率。
	const float SafeMultiplier = FMath::IsFinite(NewMultiplier) ? FMath::Max(1.0f, NewMultiplier) : 1.0f;
	const bool bPromoted = SafeMultiplier < SignificanceDecisionMultiplier;
	SignificanceDecisionMultiplier = SafeMultiplier;
	if (bPromoted)
	{
		// 提升玩法重要性时取消旧的远距等待；进入近战不能继续耗尽原 Background 长间隔。
		RequestBehaviorDecision();
	}
}

// 向运行中的行为树发送唤醒消息，不在外部回调里同步执行整轮决策。
void AfpstrueEnemyAIController::RequestBehaviorDecision()
{
	if (AcceptsCombatCommands() && BrainComponent != nullptr && BrainComponent->IsRunning())
	{
		FAIMessage::Send(BrainComponent, FAIMessage(FPEnemyDecisionWake, this, true));
	}
}

// 更新当前玩家的可见性；丢失视觉只保留最后成功发现的位置，并及时停止攻击和包围。
void AfpstrueEnemyAIController::HandleTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead() ||
		Stimulus.Type != UAISense::GetSenseID<UAISense_Sight>()) return;
	if (Actor != TargetCharacter)
	{
		if (bUsesMultiplayerTargets && Stimulus.WasSuccessfullySensed()) SelectPerceivedTarget();
		return;
	}
	if (!IsTargetUsable(TargetCharacter)) { NotifyCombatTargetUnavailable(TargetCharacter); return; }
	const bool bWasVisible = bTargetVisible;
	if (Stimulus.WasSuccessfullySensed())
	{
		bTargetVisible = true;
		LastKnownTargetLocation = Stimulus.StimulusLocation;
		TargetForgetTime = 0.0;
	}
	else if (bWasVisible)
	{
		// ★【重要边界】[状态提交] 丢失视觉先撤销实时目标资格，只保留最后成功发现的位置；失败刺激不更新隐藏玩家坐标。
		// 先撤销实时目标资格，再停止旧攻击；停止 Montage 的回调也不能继续攻击隐藏玩家。
		bTargetVisible = false;
		TargetForgetTime = GetWorld()->GetTimeSeconds() + FMath::Max(0.0f, TargetMemorySeconds);
		DecisionContext = FDecisionContext{};
		if (UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent()) Combat->ResetCombat();
		if (!AcceptsCombatCommands() || Actor != TargetCharacter) return;
		ReleaseSurroundSlot();
		StopMovementIfNeeded();
		// 失败刺激的位置可能来自当前真实目标；不覆盖最后一次成功发现的位置，也不反复续期。
	}
	if (bWasVisible != bTargetVisible) RequestBehaviorDecision();
	if (!bTargetVisible) SelectPerceivedTarget();
}

void AfpstrueEnemyAIController::HandleEnemyHealthChanged(float NewHealth)
{
	const bool bTookDamage = FMath::IsFinite(NewHealth) && NewHealth < LastObservedHealth;
	LastObservedHealth = NewHealth;
	if (!AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead() || GetWorld() == nullptr) return;
	if (!IsBelowRetreatThreshold())
	{
		bRetreatRequested = false;
		if (bRetreatActive) RequestBehaviorDecision();
		return;
	}
	// 冷却期间的新伤害不排队；到期本身也不重启，必须再收到一次真正扣血。
	if (bTookDamage && !bRetreatActive && GetWorld()->GetTimeSeconds() >= NextRetreatAllowedTime)
	{
		bRetreatRequested = true;
		RequestBehaviorDecision();
	}
}

bool AfpstrueEnemyAIController::IsBelowRetreatThreshold() const
{
	const float Threshold = FMath::IsFinite(LowHealthRetreatThreshold) ? FMath::Clamp(LowHealthRetreatThreshold, 0.0f, 1.0f) : 0.3f;
	return IsValid(EnemyHealthComponent) && !EnemyHealthComponent->IsDead() && EnemyHealthComponent->GetHealthNormalized() <= Threshold;
}

// ==================== 启动唯一行为树 ====================

// 校验配置树及黑板契约，必要时回退原生默认树；随机首轮等待用于分散群体决策峰值。
void AfpstrueEnemyAIController::StartBehaviorLogic()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || GetNetMode() == NM_Client)
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

// 为本轮行为树生成一次完整决策快照：有效目标、距离、范围及下一轮等待时间统一发布。
void AfpstrueEnemyAIController::RefreshBehaviorDecision(UBlackboardComponent& DecisionBlackboard)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_SampleDecision);
	CSV_SCOPED_TIMING_STAT(fpstrueAI, DecisionTime);
	CSV_CUSTOM_STAT(fpstrueAI, DecisionCount, 1, ECsvCustomStatOp::Accumulate);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAIDecisionTime);
	INC_DWORD_STAT(STAT_fpstrueAIDecisionCount);

	// 一次采样同时供 Blackboard 条件和下一轮间隔复用；Selector 只负责选择叶子。
	DecisionContext = FDecisionContext{};
	// 统一修复或拒绝敌人、目标和 Manager 上下文，保证后续决策分支可以直接使用这些引用。
	// 不在采样任务的 ExecuteTask 内重入 StopTree；死亡入口会停止树，本轮走安全 Idle 叶子。
	bool bHasTarget = AcceptsCombatCommands() && IsValid(ControlledEnemy) && !ControlledEnemy->IsDead();
	if (bHasTarget)
	{
		if (!IsTargetUsable(TargetCharacter))
		{
			// 平时使用 GameMode 注入的玩家；仅在缓存目标失效时安全解析，不在正常路径反复查询。
			// 此处只撤销失效上下文，不从全局玩家索引重新绑定，避免清空目标后又偷偷恢复追击。
			InitializeCombatContext(nullptr, nullptr);
		}
		SelectPerceivedTarget();
		if (IsTargetUsable(TargetCharacter) && !IsValid(SurroundManager)) SurroundManager = ResolveSurroundManager();
		bHasTarget = AcceptsCombatCommands() && IsTargetUsable(TargetCharacter) && (bTargetVisible || GetWorld()->GetTimeSeconds() < TargetForgetTime);
	}

	// 一轮只计算一次二维距离平方，并派生攻击/追击两个布尔条件，避免各分支重复开方和取位置。
	// 敌人和目标校验成功后才采样；叶子保留存活检查，但不重复计算距离与范围。
	// 先用廉价距离筛选，再检查高度和环境；楼上/墙后目标应走导航分支而不是原地重复尝试攻击。
	if (bHasTarget)
	{
		if (bTargetVisible) LastKnownTargetLocation = GetPerceptionComponent()->GetActorLocation(*TargetCharacter);
		const UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
		if (bTargetVisible && Combat != nullptr)
		{
			DecisionContext.bInAttackRange = Combat->SampleAttackReach(DecisionContext.DistanceSquared, DecisionContext.EffectiveAttackRange);
		}
		else
		{
			DecisionContext.DistanceSquared = FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), LastKnownTargetLocation);
		}
		DecisionContext.bInChaseRange = DecisionContext.DistanceSquared <= FMath::Square(ControlledEnemy->GetChaseRange());
	}
	if ((bRetreatRequested || bRetreatActive) && (!bHasTarget || !IsBelowRetreatThreshold())) FinishRetreat();

	// 计算下一轮等待，记忆追击不能等过遗忘期限才结束。
	NextBehaviorDecisionDelay =
		FMath::Max(0.01f, bHasTarget ? GetNextDecisionInterval(DecisionContext) : IdleDecisionInterval * SignificanceDecisionMultiplier);
	if (bHasTarget && (bRetreatRequested || bRetreatActive))
	{
		NextBehaviorDecisionDelay = FMath::IsFinite(RetreatDecisionInterval) ? FMath::Max(0.05f, RetreatDecisionInterval) : 0.25f;
		// 首次低血与等待掩体是紧急响应；已经取得目标后的维护沿用 Gameplay Significance 降频。
		if (bRetreatActive && bHasRetreatGoal) NextBehaviorDecisionDelay *= SignificanceDecisionMultiplier;
		const float SafeRetreatTimeLimit = FMath::IsFinite(RetreatTimeLimit) ? FMath::Max(0.1f, RetreatTimeLimit) : 6.0f;
		NextBehaviorDecisionDelay = FMath::Min(NextBehaviorDecisionDelay, SafeRetreatTimeLimit);
		if (bRetreatActive)
		{
			const double Deadline = RetreatHoldUntil > 0.0 ? FMath::Min(RetreatEndTime, RetreatHoldUntil) : RetreatEndTime;
			NextBehaviorDecisionDelay = FMath::Min(NextBehaviorDecisionDelay, FMath::Max(0.01f, static_cast<float>(Deadline - GetWorld()->GetTimeSeconds())));
		}
	}
	if (bHasTarget && !bTargetVisible)
	{
		NextBehaviorDecisionDelay = FMath::Min(NextBehaviorDecisionDelay, static_cast<float>(TargetForgetTime - GetWorld()->GetTimeSeconds()));
	}
	// 自定义树观察者也只能在完整的一轮快照发布后收到通知，避免读取半更新条件。
	// ★【重要边界】[状态提交] 黑板整轮快照写完才恢复通知，装饰器和观察者不能消费半更新条件。
	DecisionBlackboard.PauseObserverNotifications();
	// 搜索只提供位置记忆；不把隐藏玩家的 Actor 暴露给自定义 Blackboard 消费者。
	DecisionBlackboard.SetValueAsObject(FPEnemyBlackboard::TargetActor, bHasTarget && bTargetVisible ? TargetCharacter.Get() : nullptr);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::HasTarget, bHasTarget);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::Attacking, bHasTarget && ControlledEnemy->IsAttacking());
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::InAttackRange, DecisionContext.bInAttackRange);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::InChaseRange, DecisionContext.bInChaseRange);
	DecisionBlackboard.SetValueAsBool(FPEnemyBlackboard::Retreating, bHasTarget && (bRetreatRequested || bRetreatActive));
	DecisionBlackboard.ResumeObserverNotifications(true);
}

// 近战保持快速响应，其他状态按距离与重要性降频；返回本轮之后使用的间隔。
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

// 在共享管理器引用缺失时查找关卡中的管理器，不为每个敌人创建独立群体资源。
AfpstrueSurroundManager* AfpstrueEnemyAIController::ResolveSurroundManager() const
{
	if (bUsesMultiplayerTargets)
	{
		auto* Mode = GetWorld() != nullptr ? GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>() : nullptr;
		return Mode != nullptr && IsTargetUsable(TargetCharacter) ? Mode->GetOrCreateSurroundManager(TargetCharacter) : nullptr;
	}
	// 仅在注入失败或引用失效时兜底查找关卡中的唯一 Manager。
	return Cast<AfpstrueSurroundManager>(UGameplayStatics::GetActorOfClass(this, AfpstrueSurroundManager::StaticClass()));
}

// 仅检查候选玩家是否有效且存活；是否可见由感知状态另行约束。
bool AfpstrueEnemyAIController::IsTargetUsable(const AfpstrueCharacter* Target) const
{
	// 目标必须存在且存活；死亡后的停止、槽位释放由上层统一处理。
	if (!IsValid(Target) || Target->IsActorBeingDestroyed() || Target->IsDead() || Target->GetWorld() != GetWorld()) return false;
	if (!bUsesMultiplayerTargets) return true;
	if (!Target->HasActorBegunPlay()) return false;
	const auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>();
	// 有限恢复期保留的服务器 Pawn 仍是可受伤的参与者；不能因暂时无 Controller 就给予 AI 免伤。
	return Mode != nullptr && Mode->IsCombatParticipant(Target);
}

// ==================== 叶子执行：BT 选择行为，Controller 执行动作 ====================

// 执行行为树已经选定的叶子动作；返回失败让 Selector 尝试后备分支，不另建第二套高层决策。
bool AfpstrueEnemyAIController::ExecuteBehaviorAction(EFPEnemyBehaviorAction Action)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_ExecuteBehaviorAction);
	CSV_SCOPED_TIMING_STAT(fpstrueAI, DecisionTime);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAIDecisionTime);
	// 叶子仍校验对象存活，避免自定义树跳过采样，或外部回调使目标失效。
	if (!AcceptsCombatCommands()) return false;
	if (Action == EFPEnemyBehaviorAction::Idle)
	{
		FinishRetreat();
		// 没有目标或超出追击范围才停路，并立即归还槽位，避免 Idle 敌人长期占用共享资源。
		// 远距离但仍在追击范围内的敌人仍可沿共享目标追踪，不能仅凭远距档位直接休眠。
		if (ControlledEnemy != nullptr && ControlledEnemy->GetCombatComponent() != nullptr)
		{
			ControlledEnemy->GetCombatComponent()->ResetCombat();
		}
		ReleaseSurroundSlot();
		SetAIState(ControlledEnemy != nullptr && ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
		StopMovementIfNeeded();
		// 固定出生朝向加有限视锥不能负责全周警戒：Idle 只发布下段巡视方向，Movement 按转速和原生 DeltaTime 平滑转动。
		// 不读取隐藏玩家的位置、不增加 Tick/Timer；发现仍只由真实 Sight 确认，暂停和停止由旋转查询同步拒绝。
		const float SafeYawSpeed = FMath::IsFinite(IdleSearchYawSpeed) ? FMath::Clamp(IdleSearchYawSpeed, 0.0f, 90.0f) : 30.0f;
		const bool bSearch = bUsesMultiplayerTargets && !bTargetVisible && SafeYawSpeed > 0.0f && IsValid(ControlledEnemy) && !ControlledEnemy->IsDead() &&
			BrainComponent != nullptr && BrainComponent->IsRunning() && !BrainComponent->IsPaused() &&
			ControlledEnemy->GetCharacterMovement() != nullptr && ControlledEnemy->GetCharacterMovement()->IsMovingOnGround();
		ApplyRotationPolicy(AIState, false, bSearch);
		if (bSearch)
		{
			const float TurnDegrees = FMath::Clamp(SafeYawSpeed * FMath::Max(1.0f, NextBehaviorDecisionDelay), 15.0f, 120.0f);
			SetControlRotation(FRotator(0.0f, ControlledEnemy->GetActorRotation().Yaw + TurnDegrees, 0.0f));
		}
		return true;
	}
	if (!IsValid(ControlledEnemy) || ControlledEnemy->IsDead() || !IsTargetUsable(TargetCharacter))
	{
		return false;
	}
	if (bUsesMultiplayerTargets && !IsValid(SurroundManager)) return false;
	// 撤退只使用进入时的已知位置；允许丢失视觉后继续沿这个固定威胁快照寻找掩体。
	if (Action == EFPEnemyBehaviorAction::Retreat) return HandleRetreat();
	if (bRetreatActive || bRetreatRequested) return false;
	// 失去视觉后只允许 Pursue 使用位置记忆，旧快照不能继续攻击或消费实时包围槽。
	if (!bTargetVisible && (Action != EFPEnemyBehaviorAction::Pursue || GetWorld()->GetTimeSeconds() >= TargetForgetTime)) return false;

	switch (Action)
	{
	case EFPEnemyBehaviorAction::SustainAttack:
		// 已有攻击只维持朝向和停路，不允许本轮决策重新提交移动。
		if (!ControlledEnemy->IsAttacking())
		{
			return false;
		}
		SetAIState(EFPEnemyAIState::Attack);
		StopMovementIfNeeded(true);
		UpdateFacingTarget();
		return true;
	case EFPEnemyBehaviorAction::TryAttack:
	{
		// 冷却/预算失败交由 BT 的下一个叶子维持站位，而不是在 Controller 再跑一套分支链。
		// 冷却中的敌人继续维持包围位置，不申请并立即释放攻击名额。
		UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
		if (Combat == nullptr || !DecisionContext.bInAttackRange || !Combat->IsAttackReady())
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
		if (Combat->TryAttackTarget() || Combat->IsAttacking())
		{
			return true; // 请求成功或动画回调启动了替代事务，都不能归还仍在使用的名额。
		}
		ReleaseAttackPermission();
		SetAIState(EFPEnemyAIState::Chase);
		return false;
	}
	case EFPEnemyBehaviorAction::MaintainCombatPosition:
		// 冷却和预算拒绝共用等待行为：有槽位继续走，无槽位站定；不残留 Attack 旋转策略。
		SetAIState(EFPEnemyAIState::Chase);
		if (!HandleSurroundMovement())
		{
			FaceTargetAtRest();
		}
		return true;
	case EFPEnemyBehaviorAction::Surround:
		return HandleSurroundMovement();
	case EFPEnemyBehaviorAction::Pursue:
	{
		SetAIState(EFPEnemyAIState::Chase);
		// 没有专属槽位时，使用共享目标快照进行低成本追击。
		// 没有可用环形槽位时追逐 Manager 的共享玩家位置；Manager 失效才直接读取玩家 Transform。
		// 感知约束：隐藏时或共享快照不可用时改用成功视觉记录，不直接读取玩家当前 Transform。
		FVector Goal = LastKnownTargetLocation;
		float AcceptanceRadius = MoveAcceptanceRadius;
		if (bTargetVisible && SurroundManager != nullptr && SurroundManager->GetSharedTargetSnapshot(Goal))
		{
			const float SafePursuitAcceptanceRangeMultiplier =
				FMath::IsFinite(PursuitAcceptanceRangeMultiplier) ? FMath::Clamp(PursuitAcceptanceRangeMultiplier, 0.0f, 1.0f) : 0.8f;
			const UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
			const float ConfiguredAttackRange = Combat != nullptr ? Combat->GetConfiguredAttackRange() : 0.0f;
			AcceptanceRadius = FMath::Max(MoveAcceptanceRadius, ConfiguredAttackRange * SafePursuitAcceptanceRangeMultiplier);
		}
		MoveToGoal(Goal, AcceptanceRadius, false);
		return true;
	}
	default:
		return false;
	}
}

// 消费共享接近点并请求移动；不可达时释放槽位并允许后备追击，预算延后则保留当前路径。
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
	const EMoveGoalResult Result = MoveToGoal(AttackGoal, CombatMoveAcceptanceRadius, true);
	if (Result == EMoveGoalResult::Unreachable || Result == EMoveGoalResult::InvalidContext)
	{
		ReleaseSurroundSlot();
		return false; // 给 BT 的共享追击后备分支机会，而不是吞掉导航失败。
	}
	return true; // 帧预算延后也保持当前行为，不中断有效路径。
}

bool AfpstrueEnemyAIController::HandleRetreat()
{
	UWorld* World = GetWorld();
	if (World == nullptr || (!bRetreatRequested && !bRetreatActive)) return false;
	const double Now = World->GetTimeSeconds();
	if (!bRetreatActive)
	{
		if (!bTargetVisible && Now >= TargetForgetTime)
		{
			FinishRetreat();
			return true;
		}
		bRetreatRequested = false;
		bRetreatActive = true;
		bHasRetreatGoal = false;
		RetreatThreatLocation = LastKnownTargetLocation;
		RetreatEndTime = Now + (FMath::IsFinite(RetreatTimeLimit) ? FMath::Max(0.1f, RetreatTimeLimit) : 6.0f);
		RetreatHoldUntil = 0.0;
		// 停 Montage 可能同步回调；先关攻击命令入口，再撤销攻击事务与共享资源。
		CommandPhase = ECommandPhase::ChangingContext;
		if (UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent()) Combat->ResetCombat();
		ReleaseAttackPermission();
		ReleaseSurroundSlot();
		StopMovementIfNeeded();
		if (CommandPhase != ECommandPhase::ChangingContext) return true;
		CommandPhase = ECommandPhase::Accepting;
		SetAIState(EFPEnemyAIState::Idle);
	}
	if (Now >= RetreatEndTime || !IsBelowRetreatThreshold() || !IsValid(SurroundManager))
	{
		FinishRetreat();
		return true;
	}
	if (RetreatHoldUntil > 0.0)
	{
		if (Now >= RetreatHoldUntil) FinishRetreat();
		return true;
	}
	if (!bHasRetreatGoal)
	{
		FVector CoverLocation;
		const EFPCoverQueryResult CoverResult = SurroundManager->FindOrRequestCover(ControlledEnemy, RetreatThreatLocation, CoverLocation);
		if (CoverResult == EFPCoverQueryResult::Pending) return true;
		if (CoverResult != EFPCoverQueryResult::Found || CoverLocation.ContainsNaN())
		{
			// 失败结束本轮并进入冷却，不在低血持续期间反复创建 EQS 查询。
			FinishRetreat();
			return true;
		}
		RetreatGoalLocation = CoverLocation;
		bHasRetreatGoal = true;
	}
	SetAIState(EFPEnemyAIState::Chase);
	const EMoveGoalResult MoveResult = MoveToGoal(RetreatGoalLocation, CombatMoveAcceptanceRadius, true);
	if (MoveResult == EMoveGoalResult::Arrived)
	{
		StopMovementIfNeeded(true);
		SetAIState(EFPEnemyAIState::Idle);
		const float HoldSeconds = FMath::IsFinite(RetreatHoldSeconds) ? FMath::Max(0.0f, RetreatHoldSeconds) : 1.5f;
		RetreatHoldUntil = Now + HoldSeconds;
		// 到达发生在本轮间隔采样之后，立即缩短下一次等待，避免低重要性档位延长短停留。
		NextBehaviorDecisionDelay = FMath::Min(NextBehaviorDecisionDelay, FMath::Max(0.01f, HoldSeconds));
	}
	else if (MoveResult == EMoveGoalResult::Unreachable || MoveResult == EMoveGoalResult::InvalidContext)
	{
		FinishRetreat();
	}
	return true;
}

void AfpstrueEnemyAIController::FinishRetreat(bool bApplyCooldown)
{
	const bool bHadRetreat = bRetreatActive || bRetreatRequested || bHasRetreatGoal;
	bRetreatRequested = false;
	bRetreatActive = false;
	bHasRetreatGoal = false;
	RetreatEndTime = RetreatHoldUntil = 0.0;
	RetreatThreatLocation = RetreatGoalLocation = FVector::ZeroVector;
	if (IsValid(SurroundManager) && ControlledEnemy != nullptr) SurroundManager->ReleaseCover(ControlledEnemy);
	if (!bHadRetreat) return;
	if (bApplyCooldown && GetWorld() != nullptr)
	{
		NextRetreatAllowedTime = GetWorld()->GetTimeSeconds() + (FMath::IsFinite(RetreatCooldown) ? FMath::Max(0.1f, RetreatCooldown) : 8.0f);
	}
	StopMovementIfNeeded();
	SetAIState(EFPEnemyAIState::Idle);
}

// ==================== 移动与朝向：请求去重 → 预算 → 到达后转向 ====================

// 比较会影响寻路结果的请求语义及三维位置容差，用于避免重复提交同一目标。
bool AfpstrueEnemyAIController::FMoveGoalKey::Matches(const FMoveGoalKey& Other, float DistanceTolerance) const
{
	return bCombatPriority == Other.bCombatPriority && NavigationFilter == Other.NavigationFilter &&
		FMath::IsNearlyEqual(AcceptanceRadius, Other.AcceptanceRadius) &&
		FVector::DistSquared(Location, Other.Location) < FMath::Square(FMath::Max(1.0f, DistanceTolerance));
}

// 包围和追击各自记录失败退避，在目标没有明显变化时暂缓重复寻路。
void AfpstrueEnemyAIController::RecordMoveFailure(const FMoveGoalKey& Goal)
{
	FMoveFailure& Failure = MoveFailures[Goal.bCombatPriority ? 1 : 0];
	Failure.Goal = Goal;
	Failure.RetryAfter = GetWorld()->GetTimeSeconds() + FMath::Max(FailedMoveRetryDelay, 0.1f);
}

// 先复用路径或到位缓存，再查失败退避与全局预算；只对真正需要刷新的目标提交独立 MoveTo。
AfpstrueEnemyAIController::EMoveGoalResult AfpstrueEnemyAIController::MoveToGoal(
	const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority)
{
	if (!AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead() || GetWorld() == nullptr ||
		GoalLocation.ContainsNaN() || !FMath::IsFinite(AcceptanceRadius) || AcceptanceRadius < 0.0f ||
		(bUsesMultiplayerTargets && !IsValid(SurroundManager)))
	{
		return EMoveGoalResult::InvalidContext;
	}
	const FMoveGoalKey RequestedGoal{GoalLocation, AcceptanceRadius, DefaultNavigationFilterClass.Get(), bCombatPriority};
	// 去重和预算都发生在提交 PathFollowing 之前；被限流时保留旧路径，下轮仍可重试，不会让角色原地急停。
	const bool bSameGoal = RequestedGoal.Matches(LastMoveGoal, PathRefreshDistance);
	if (bSameGoal && bHasMoveGoal)
	{
		if (OwnedMoveRequestId.IsValid() && GetCurrentMoveRequestID() == OwnedMoveRequestId && GetMoveStatus() != EPathFollowingStatus::Idle)
		{
			// 目标没变且路径仍在执行：保持路径朝向，不因靠近玩家或上次原地等待而横移。
			ApplyRotationPolicy(EFPEnemyAIState::Chase);
			return EMoveGoalResult::Moving;
		}

		const UPathFollowingComponent* PathFollowing = GetPathFollowingComponent();
		if (PathFollowing != nullptr &&
			PathFollowing->HasReached(LastResolvedMoveGoal, EPathFollowingReachMode::OverlapAgent, AcceptanceRadius))
		{
			// 复用引擎含胶囊半径/高度的到达规则，并检查上次请求经过导航投影后的目标。
			// 成功到位后缓存继续有效，只刷新朝向，不反复提交 AlreadyAtGoal 请求。
			FaceTargetAtRest();
			return EMoveGoalResult::Arrived;
		}

		// Idle 也可能是路径中断、部分路径走完或角色被挤离槽位，不能永久用旧缓存阻止重寻路。
		bHasMoveGoal = false;
		OwnedMoveRequestId = FAIRequestID::InvalidRequest;
		RecordMoveFailure(RequestedGoal);
		return EMoveGoalResult::Unreachable;
	}

	// 复用未命中时，先检查目标级失败退避；帧预算限制的是新请求数量，不共享敌人的路径。
	const UWorld* World = GetWorld();
	const FMoveFailure& Failure = MoveFailures[bCombatPriority ? 1 : 0];
	if (World->GetTimeSeconds() < Failure.RetryAfter && RequestedGoal.Matches(Failure.Goal, PathRefreshDistance))
	{
		return EMoveGoalResult::Unreachable;
	}

	if (SurroundManager != nullptr && !SurroundManager->TryConsumeMoveRequestBudget(bCombatPriority))
	{
		// 预算拒绝时继续沿旧路径移动；LastMoveGoal 不更新，下一轮仍会识别到待刷新的目标。
		INC_DWORD_STAT(STAT_fpstrueAIMoveBudgetRejectedCount);
		CSV_CUSTOM_STAT(fpstrueAI, MoveBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
		return EMoveGoalResult::Deferred;
	}

	INC_DWORD_STAT(STAT_fpstrueAIMoveRequestCount);
	CSV_CUSTOM_STAT(fpstrueAI, MoveRequestCount, 1, ECsvCustomStatOp::Accumulate);
	const uint32 SubmittingRevision = ++MoveRequestRevision;
	// Abort/MoveTo 可能同步广播完成事件。先撤回旧归属，只有本次返回且仍为当前请求才能发布新缓存。
	OwnedMoveRequestId = FAIRequestID::InvalidRequest;
	bHasMoveGoal = false;
	// 使用显式请求替代一串位置布尔参数，保留原 MoveToLocation 的导航、接受半径和部分路径语义。
	// 与引擎包装函数一致，提交新请求前取消旧路径，但保留速度；不得在预算拒绝之前执行这一步。
	if (UPathFollowingComponent* PathFollowing = GetPathFollowingComponent();
		PathFollowing != nullptr && PathFollowing->GetStatus() != EPathFollowingStatus::Idle)
	{
		PathFollowing->AbortMove(*this, FPathFollowingResultFlags::ForcedScript | FPathFollowingResultFlags::NewRequest,
								 FAIRequestID::CurrentRequest, EPathFollowingVelocityMode::Keep);
	}
	if (MoveRequestRevision != SubmittingRevision || !AcceptsCombatCommands()) return EMoveGoalResult::Deferred;

	// 为本敌人构造完整请求；包围点已集中投影，普通追击点仍让引擎投影。
	FAIMoveRequest MoveRequest(GoalLocation);
	MoveRequest.SetUsePathfinding(true);
	MoveRequest.SetAllowPartialPath(true);
	MoveRequest.SetProjectGoalLocation(!bCombatPriority); // 环形槽位已由 Manager 集中投影。
	MoveRequest.SetNavigationFilter(DefaultNavigationFilterClass);
	MoveRequest.SetAcceptanceRadius(AcceptanceRadius);
	MoveRequest.SetReachTestIncludesAgentRadius(true);
	MoveRequest.SetCanStrafe(false);
	FPathFollowingRequestResult Result;
	{
		// 只包真正提交的请求，拒绝/复用分支不伪造成独立寻路成本；包含引擎在此同步完成的工作。
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_SubmitMoveTo);
		Result = MoveTo(MoveRequest);
	}
	// ★【重要边界】[回调重入] MoveTo 可能同步结束旧请求；只有提交版本和命令入口仍有效，才缓存本次结果。
	if (MoveRequestRevision != SubmittingRevision || !AcceptsCombatCommands()) return EMoveGoalResult::Deferred;
	const EPathFollowingRequestResult::Type MoveResult = Result.Code;
	LastMoveGoal = RequestedGoal;
	// UE MoveTo 会把投影结果写回请求。不能用部分路径末端替代它，否则到达死路也会被当成到位。
	LastResolvedMoveGoal = MoveRequest.GetGoalLocation();
	bHasMoveGoal = MoveResult != EPathFollowingRequestResult::Failed;
	OwnedMoveRequestId = MoveResult == EPathFollowingRequestResult::RequestSuccessful ? Result.MoveId : FAIRequestID::InvalidRequest;
	// 缓存发布后按引擎结果收口；失败、到达、移动各自只维护一次副作用和返回值。
	if (MoveResult == EPathFollowingRequestResult::Failed)
	{
		RecordMoveFailure(RequestedGoal);
		return EMoveGoalResult::Unreachable;
	}
	MoveFailures[bCombatPriority ? 1 : 0] = FMoveFailure{};
	if (MoveResult == EPathFollowingRequestResult::AlreadyAtGoal)
	{
		FaceTargetAtRest();
		return EMoveGoalResult::Arrived;
	}
	// 只有接受了新移动请求才撤销站定朝向；预算拒绝不打断旧路径，也不伪造到达。
	ApplyRotationPolicy(EFPEnemyAIState::Chase);
	return EMoveGoalResult::Moving;
}

// 只接收当前拥有的异步导航结果，旧请求不能撤销新路径；失败时安排目标级退避。
void AfpstrueEnemyAIController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	// 只处理拥有的异步请求。MoveTo 内的同步失败/AlreadyAtGoal 由返回值提交，不在回调抢写尚未发布的缓存。
	// ★【重要边界】[异步身份] 完成回调必须匹配当前拥有的 RequestID，旧导航结果不能清掉新请求归属。
	if (OwnedMoveRequestId.IsValid() && RequestID == OwnedMoveRequestId)
	{
		OwnedMoveRequestId = FAIRequestID::InvalidRequest;
		if (Result.IsFailure())
		{
			bHasMoveGoal = false;
			if (Result.Code != EPathFollowingResult::Aborted && GetWorld() != nullptr) RecordMoveFailure(LastMoveGoal);
		}
	}

	Super::OnMoveCompleted(RequestID, Result);
}

// 仅在地面停稳后切换面向目标，并返回真实角色朝向是否达标；隐藏目标不能继续被实时跟踪。
bool AfpstrueEnemyAIController::FaceTargetAtRest()
{
	if (bRetreatActive)
	{
		StopMovementIfNeeded(true);
		ApplyRotationPolicy(EFPEnemyAIState::Idle);
		return false;
	}
	if (!bTargetVisible)
	{
		StopMovementIfNeeded(true);
		ApplyRotationPolicy(EFPEnemyAIState::Idle);
		return false; // 到最后已知位置后等待重新发现，不再转向隐藏玩家。
	}
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

// 更新玩家方向对应的期望 Yaw，并以实际 Actor Yaw 判断攻击转向是否完成。
bool AfpstrueEnemyAIController::UpdateFacingTarget()
{
	// 仅站定/攻击时离散刷新期望朝向，移动阶段的连续转向交给 CharacterMovement。
	if (ControlledEnemy == nullptr || GetTargetCharacter() == nullptr)
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

// 仅在表现状态改变时提交状态及旋转策略，保持动画消费者读取同一份状态。
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

// 按行进、站定或攻击阶段选定唯一旋转来源，避免移动方向与 Controller 朝向互相争抢。
void AfpstrueEnemyAIController::ApplyRotationPolicy(EFPEnemyAIState NewState, bool bAtRestFacingTarget, bool bIdleSearch)
{
	// 把状态到旋转来源的映射集中设置，防止 MoveTo 与面向玩家同时争夺角色朝向。
	if (ControlledEnemy == nullptr)
	{
		return;
	}

	if (UCharacterMovementComponent* Movement = ControlledEnemy->GetCharacterMovement())
	{
		// 巡视临时降低 Yaw 速度；离开 Idle、停止或发现目标时恢复原值，不覆盖敌人蓝图的战斗转速配置。
		if (bIdleSearch && !bIdleSearchActive) IdleSearchSavedYawRate = Movement->RotationRate.Yaw;
		if (!bIdleSearch && bIdleSearchActive) Movement->RotationRate.Yaw = IdleSearchSavedYawRate;
		bIdleSearchActive = bIdleSearch;
		if (bIdleSearch) Movement->RotationRate.Yaw = FMath::IsFinite(IdleSearchYawSpeed) ? FMath::Clamp(IdleSearchYawSpeed, 0.0f, 90.0f) : 30.0f;
		// 移动朝向使用 Acceleration/RequestedVelocity，而不是直接朝玩家；RVO 的瞬时侧向避让仍可能存在。
		// Attack 和站定的 Chase（含转向准备）使用 Controller 朝向；Idle 仅在巡视时使用它，Dead 关闭两种来源。
		const bool bFaceTarget = NewState == EFPEnemyAIState::Attack || (NewState == EFPEnemyAIState::Chase && bAtRestFacingTarget);
		Movement->bOrientRotationToMovement = NewState == EFPEnemyAIState::Chase && !bFaceTarget;
		Movement->bUseControllerDesiredRotation = bFaceTarget || bIdleSearch;
	}
}

// ==================== 停止与公共清理 ====================

// 先关闭命令入口，再停止感知、行为树、导航和攻击，最后清空黑板与群体资源；允许重复调用。
void AfpstrueEnemyAIController::StopAI()
{
	if (CommandPhase == ECommandPhase::Stopping) return;
	// ★【重要边界】[生命周期] 停止时先禁止新命令，再停感知、行为树、移动和攻击，最后清目标、黑板与槽位。
	CommandPhase = ECommandPhase::Stopping;
	if (TargetCharacter != nullptr)
	{
		TargetCharacter->OnPlayerDeathReported.RemoveDynamic(this, &ThisClass::HandleCombatTargetDied);
		TargetCharacter->OnEndPlay.RemoveDynamic(this, &ThisClass::HandleCombatTargetEndPlay);
	}
	if (EnemyHealthComponent != nullptr)
	{
		EnemyHealthComponent->OnHealthChanged.RemoveDynamic(this, &ThisClass::HandleEnemyHealthChanged);
		EnemyHealthComponent = nullptr;
	}
	FinishRetreat(false);
	bTargetVisible = false;
	TargetForgetTime = 0.0;
	LastKnownTargetLocation = FVector::ZeroVector;
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), false);
	GetPerceptionComponent()->ForgetAll();
	// 先停行为树，再停止导航和清理资源；否则下一次 BT 唤醒可能重新发起 MoveTo。
	// 敌人死亡、解除占有和对局结束都走同一收口，确保路径、行为树、槽位和攻击名额不会只清理一部分。
	if (BrainComponent != nullptr)
	{
		BrainComponent->StopLogic(TEXT("Enemy stopped"));
	}
	StopMovementIfNeeded();
	if (ControlledEnemy != nullptr && ControlledEnemy->GetCombatComponent() != nullptr)
	{
		// StopAI 是完整业务停止：先取消旧攻击，归还其捕获的许可，再清 Controller 上下文。
		ControlledEnemy->GetCombatComponent()->ResetCombat();
	}
	// 生产者已经停止，清掉目标和完整快照，避免下次启动复用旧条件。
	TargetCharacter = nullptr;
	ReleaseSurroundSlot();
	DecisionContext = FDecisionContext{};
	if (UBlackboardComponent* DecisionBlackboard = GetBlackboardComponent())
	{
		// RunBehaviorTree 可以复用兼容 Blackboard；停止后不能保留上一轮目标和条件。
		DecisionBlackboard->PauseObserverNotifications();
		DecisionBlackboard->ClearValue(FPEnemyBlackboard::TargetActor);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::HasTarget, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::Attacking, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::InAttackRange, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::InChaseRange, false);
		DecisionBlackboard->SetValueAsBool(FPEnemyBlackboard::Retreating, false);
		DecisionBlackboard->ResumeObserverNotifications(false);
	}

	if (ControlledEnemy != nullptr)
	{
		SetAIState(ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
		ApplyRotationPolicy(AIState); // 原本已经是 Idle 也要撤销巡视并恢复转速，不能被同状态短路跳过清理。
	}
	CommandPhase = ECommandPhase::Stopped;
}

// 按需撤销移动归属与缓存；只有导航实际运行时才提交停止请求。
void AfpstrueEnemyAIController::StopMovementIfNeeded(bool bPreserveMoveGoal)
{
	// 只有 PathFollowing 正在运行时才提交 StopMovement；按调用场景决定是否保留目标缓存。
	const bool bShouldStop = GetMoveStatus() != EPathFollowingStatus::Idle;
	if (!bPreserveMoveGoal || bShouldStop)
	{
		++MoveRequestRevision;
		OwnedMoveRequestId = FAIRequestID::InvalidRequest;
		bHasMoveGoal = false;
		MoveFailures[0] = MoveFailures[1] = FMoveFailure{};
	}
	if (bShouldStop)
	{
		StopMovement();
	}
}

// 把当前敌人的攻击名额交回管理器；重复释放不新增副作用。
void AfpstrueEnemyAIController::ReleaseAttackPermission()
{
	// 攻击结束、失败、死亡和解除占有均可调用，Manager 的 TSet 保证重复释放无副作用。
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseAttackPermission(ControlledEnemy);
	}
}

// 归还本敌人的稳定槽位，让管理器按需安排外环补位。
void AfpstrueEnemyAIController::ReleaseSurroundSlot()
{
	// 将敌人与稳定槽位的映射交还给群体 Manager，并触发可能的外环补位。
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseSurroundSlot(ControlledEnemy);
	}
}
