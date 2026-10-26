// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BrainComponent.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Game/fpstruePvpGameMode.h"
#include "AITypes.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationPath.h"
#include "NavFilters/NavigationQueryFilter.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionSystem.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "UObject/ConstructorHelpers.h"

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
	// 原生角色和蓝图共用保存的正式树；构造期引用同时让 Cook 收集该资产。
	static ConstructorHelpers::FObjectFinder<UBehaviorTree> DefaultTree(TEXT("/Game/FirstPerson/AI/BT_FPEnemy"));
	BehaviorTreeAsset = DefaultTree.Object;
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
	UAISenseConfig_Hearing* Hearing = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("EnemyHearing"));
	Hearing->HearingRange = 4500.0f;
	Hearing->SetMaxAge(4.0f); // 原生刺激缓存有界；调查硬期限另由既有 BT 决策检查。
	// Hearing 按 TeamId 而非 GetTeamAttitudeTowards 过滤；原生玩家没有 TeamId，回调仍严格只接纳可战斗玩家。
	Hearing->DetectionByAffiliation.bDetectEnemies = true;
	Hearing->DetectionByAffiliation.bDetectFriendlies = true;
	Hearing->DetectionByAffiliation.bDetectNeutrals = true;
	Perception->ConfigureSense(*Hearing);
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
		GetPerceptionComponent()->SetSenseEnabled(UAISense_Hearing::StaticClass(), false);
		return;
	}

	ControlledEnemy = Cast<AfpstrueEnemyCharacter>(InPawn);
	CommandPhase = ECommandPhase::Uninitialized;
	ActiveBehaviorTree = nullptr;
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return;
	}

	// 新 Pawn 从空闲上下文开始，旧请求的完成回调不能接管新的移动缓存。
	AIState = EFPEnemyAIState::Idle;
	DecisionContext = FDecisionContext{};
	bHasMoveGoal = false;
	OwnedMoveRequestId = FAIRequestID::InvalidRequest;
	MoveFailures[0] = MoveFailures[1] = FMoveFailure{};
	bInvestigatingNoise = false;
	LastAcceptedNoiseTime = -TNumericLimits<double>::Max();
	NextNoiseInvestigationTime = 0.0;
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

	if (UCharacterMovementComponent* Movement = ControlledEnemy->GetCharacterMovement())
	{
		// 已保存的蓝图可能覆盖构造默认值，因此在运行时显式恢复 RVO，保证候选回切不受资产旧值影响。
		// PathFollowing 仍生成路径与期望速度，CharacterMovement 在执行速度时负责邻域避让。
		Movement->SetAvoidanceEnabled(true);
	}

	SurroundManager = ResolveSurroundManager();
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Hearing::StaticClass(), true);
	GetPerceptionComponent()->RequestStimuliListenerUpdate();
	StartBehaviorLogic();
}

bool AfpstrueEnemyAIController::IsBehaviorReady() const
{
	const UBehaviorTreeComponent* Behavior = Cast<UBehaviorTreeComponent>(BrainComponent);
	return AcceptsCombatCommands() && IsValid(ControlledEnemy) && ControlledEnemy == GetPawn() &&
		!ControlledEnemy->IsDead() && ActiveBehaviorTree != nullptr && Behavior != nullptr && Behavior->IsRunning() && Behavior->GetRootTree() == ActiveBehaviorTree;
}

// 解除占有前停掉决策和攻击、归还群体资源，再释放本次 Pawn 的强引用。
void AfpstrueEnemyAIController::OnUnPossess()
{
	StopAI();
	ActiveBehaviorTree = nullptr;
	ControlledEnemy = nullptr;
	SurroundManager = nullptr;

	Super::OnUnPossess();
}

// 世界退出复用 StopAI 清理，并释放行为树模板引用。
void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopAI();
	ActiveBehaviorTree = nullptr;
	Super::EndPlay(EndPlayReason);
}

// 注入候选玩家和共享管理器；上下文变化时先撤销旧攻击与移动，再按存活状态开放新命令。
void AfpstrueEnemyAIController::InitializeCombatContext(AfpstrueCharacter* NewTargetCharacter, AfpstrueSurroundManager* NewSurroundManager)
{
	// Controller 只保存敌人决策上下文；共享包围目标由 GameMode 统一初始化。
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	if (CommandPhase == ECommandPhase::Stopped || CommandPhase == ECommandPhase::Stopping || CommandPhase == ECommandPhase::ChangingContext)
	{
		return; // 清理期间不接受外部回调重新注入上下文。
	}
	if (!IsValid(ControlledEnemy) || ControlledEnemy->IsDead()) return;
	if (!IsBehaviorReady()) return;
	// 无效/死亡目标统一视为撤销；恢复追击必须由同一个上下文入口重新注入。
	if (!IsTargetUsable(NewTargetCharacter)) NewTargetCharacter = nullptr;
	if (NewTargetCharacter == nullptr) NewSurroundManager = nullptr;
	const bool bContextChanged = TargetCharacter != NewTargetCharacter || SurroundManager != NewSurroundManager;
	const bool bPendingRetreatBeforeContextChange = bRetreatRequested && !bRetreatActive;
	if (bContextChanged)
	{
		// ★【重要边界】[状态提交] 先关闭旧上下文的命令入口，再停止攻击和导航，清理回调不能重启旧行为。
		CommandPhase = ECommandPhase::ChangingContext;
		MoveFailures[0] = MoveFailures[1] = FMoveFailure{};
		if (TargetCharacter != nullptr)
		{
			TargetCharacter->OnPlayerDeathReported.RemoveDynamic(this, &ThisClass::HandleCombatTargetDied);
			TargetCharacter->OnEndPlay.RemoveDynamic(this, &ThisClass::HandleCombatTargetEndPlay);
		}
		FinishRetreat(false);
		bTargetVisible = false;
		TargetForgetTime = 0.0;
		LastKnownTargetLocation = FVector::ZeroVector;
		bInvestigatingNoise = false;
		// 换目标保留其他可见玩家的感知缓存；停止和目标离场在各自入口撤销刺激。
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
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), AcceptsCombatCommands());
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Hearing::StaticClass(), AcceptsCombatCommands());
	if (TargetCharacter == nullptr)
	{
		SetAIState(ControlledEnemy != nullptr && ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
	}
	if (bContextChanged) RequestBehaviorDecision();
}

// 当前可见目标不因另一个玩家靠近而抖动切换；失去它后才从已感知玩家中选一个新的可见目标。
void AfpstrueEnemyAIController::SelectPerceivedTarget()
{
	if (!AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead()) return;
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
		const FActorPerceptionInfo* Info = Perception->GetActorInfo(*Candidate);
		const FVector KnownLocation = Info != nullptr ? Info->GetStimulusLocation(UAISense::GetSenseID<UAISense_Sight>()) : FAISystem::InvalidLocation;
		if (!FAISystem::IsValidLocation(KnownLocation) || KnownLocation.ContainsNaN()) continue;
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
	bInvestigatingNoise = false;
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

// 当前目标的死亡通知统一转为目标失效处理，不另建一条战斗停止链。
void AfpstrueEnemyAIController::HandleCombatTargetDied(AfpstrueCharacter* DeadTarget)
{
	NotifyCombatTargetUnavailable(DeadTarget);
}

// 目标未经过死亡就直接离场时，同样撤销追击与其所属的群体资源。
void AfpstrueEnemyAIController::HandleCombatTargetEndPlay(AActor* EndingTarget, EEndPlayReason::Type)
{
	NotifyCombatTargetUnavailable(Cast<AfpstrueCharacter>(EndingTarget));
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

void AfpstrueEnemyAIController::OnTacticalAssignmentChanged(AfpstrueSurroundManager* Source)
{
	if (Source != SurroundManager || !AcceptsCombatCommands()) return;
	TacticalAssignment = FFPEnemyTacticalAssignment{};
	StopMovementIfNeeded();
	RequestBehaviorDecision();
}

bool AfpstrueEnemyAIController::IsTacticalAttackPositionReady(bool bRequireFacing) const
{
	if (GetTargetCharacter() == nullptr || !IsValid(ControlledEnemy) || !IsValid(SurroundManager)) return false;
	// 内圈归属独立于导航点缓存；实际攻击距离/LOS 由 Combat 再检查，不把导航投影当攻击许可。
	if (TacticalAssignment.Role != EFPEnemyTacticalRole::Inner || TacticalAssignment.Revision == 0 ||
		!SurroundManager->HasInnerSlot(ControlledEnemy, TacticalAssignment.Revision)) return false;
	if (!bRequireFacing) return true;
	const FVector ToTarget = TargetCharacter->GetActorLocation() - ControlledEnemy->GetActorLocation();
	return ToTarget.SizeSquared2D() <= UE_SMALL_NUMBER || FMath::Abs(FMath::FindDeltaAngleDegrees(ControlledEnemy->GetActorRotation().Yaw,
		ToTarget.Rotation().Yaw)) <= FMath::Clamp(AttackFacingToleranceDegrees, 0.0f, 90.0f);
}

// 更新当前玩家的可见性；丢失视觉只保留最后成功发现的位置，并及时停止攻击和包围。
void AfpstrueEnemyAIController::HandleTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead()) return;
	if (Stimulus.Type == UAISense::GetSenseID<UAISense_Hearing>())
	{
		HandleHearingStimulus(Actor, Stimulus);
		return;
	}
	if (Stimulus.Type != UAISense::GetSenseID<UAISense_Sight>()) return;
	if (Actor != TargetCharacter)
	{
		if (Stimulus.WasSuccessfullySensed()) SelectPerceivedTarget();
		return;
	}
	if (!IsTargetUsable(TargetCharacter)) { NotifyCombatTargetUnavailable(TargetCharacter); return; }
	const bool bWasVisible = bTargetVisible;
	if (Stimulus.WasSuccessfullySensed())
	{
		bTargetVisible = true;
		bInvestigatingNoise = false; // 真实视觉接管声音调查；旧声音失效通知不得撤销新视觉。
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

// 服务端真实射击只产生一次语义声音，不发送额外 RPC，也不按每个敌人枚举查询。
bool AfpstrueEnemyAIController::ReportAuthoritativeGunshot(AfpstrueCharacter* Source, const FVector& NoiseLocation,
	float Loudness, float MaxRange)
{
	if (!IsInGameThread() || !IsValid(Source) || !Source->HasAuthority() || Source->IsActorBeingDestroyed() || Source->IsDead() ||
		Source->GetWorld() == nullptr || Source->GetNetMode() == NM_Client || !FAISystem::IsValidLocation(NoiseLocation) ||
		NoiseLocation.ContainsNaN() || !FMath::IsFinite(Loudness) || Loudness <= 0.0f || !FMath::IsFinite(MaxRange) || MaxRange < 0.0f ||
		UAIPerceptionSystem::GetCurrent(Source->GetWorld()) == nullptr) return false;
	UAISense_Hearing::ReportNoiseEvent(Source, NoiseLocation, Loudness, Source, MaxRange, TEXT("Weapon.Gunshot"));
	return true;
}

// 听见不等于看见：仅以事件位置复用 Pursue 搜索，不暴露 Actor、不攻击、不消费隐藏目标的共享位置。
void AfpstrueEnemyAIController::HandleHearingStimulus(AActor* Actor, const FAIStimulus& Stimulus)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_HearingStimulus);
	AfpstrueCharacter* Source = Cast<AfpstrueCharacter>(Actor);
	UWorld* World = GetWorld();
	if (!Stimulus.WasSuccessfullySensed() || Stimulus.Tag != FName(TEXT("Weapon.Gunshot")) || World == nullptr ||
		!IsTargetUsable(Source) ||
		!FAISystem::IsValidLocation(Stimulus.StimulusLocation) || Stimulus.StimulusLocation.ContainsNaN() ||
		!FMath::IsFinite(Stimulus.Strength) || Stimulus.Strength <= 0.0f || BrainComponent == nullptr ||
		!BrainComponent->IsRunning() || BrainComponent->IsPaused()) return;
	const double Now = World->GetTimeSeconds();
	// 已看见及有效视觉记忆优先；连续枪声不能把视觉搜索位置替换成另一份隐式实时跟踪。
	if (bTargetVisible || (!bInvestigatingNoise && Now < TargetForgetTime) || bRetreatActive || bRetreatRequested || Now < NextNoiseInvestigationTime) return;
	if (bInvestigatingNoise && Now >= TargetForgetTime) { FinishNoiseInvestigation(); return; }
	const float MergeInterval = FMath::IsFinite(NoiseMergeInterval) ? FMath::Max(0.1f, NoiseMergeInterval) : 0.5f;
	const float MergeDistance = FMath::IsFinite(NoiseMergeDistance) ? FMath::Max(0.0f, NoiseMergeDistance) : 150.0f;
	if (bInvestigatingNoise)
	{
		// 保留当前声源，不在多个隐藏玩家的枪声之间抖动换目标；无论是否合并都不续硬期限。
		if (Source != TargetCharacter || Now - LastAcceptedNoiseTime < MergeInterval ||
			FVector::DistSquared(LastKnownTargetLocation, Stimulus.StimulusLocation) <= FMath::Square(MergeDistance)) return;
	}
	else
	{
		auto* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
		AfpstrueSurroundManager* Manager = Mode != nullptr ? Mode->GetOrCreateSurroundManager(Source) : nullptr;
		if (!IsValid(Manager)) return; // 声音调查也使用同一个玩家管理器及共享 MoveTo 预算。
		InitializeCombatContext(Source, Manager);
		if (!AcceptsCombatCommands() || TargetCharacter != Source || !IsTargetUsable(Source)) return;
		bInvestigatingNoise = true;
		const float Duration = FMath::IsFinite(NoiseInvestigationSeconds) ? FMath::Max(0.1f, NoiseInvestigationSeconds) : 4.0f;
		TargetForgetTime = Now + Duration;
	}
	bTargetVisible = false;
	LastKnownTargetLocation = Stimulus.StimulusLocation;
	LastAcceptedNoiseTime = Now;
	DecisionContext = FDecisionContext{};
	RequestBehaviorDecision(); // 只排队，感知回调中不调用 MoveTo/寻路。
}

// 调查到位、不可达或过期都结束一次声音事务；短退避避免持续枪声立刻重开失败路径。
void AfpstrueEnemyAIController::FinishNoiseInvestigation(bool bApplyCooldown)
{
	if (!bInvestigatingNoise) return;
	bInvestigatingNoise = false;
	TargetForgetTime = 0.0;
	LastKnownTargetLocation = FVector::ZeroVector;
	DecisionContext = FDecisionContext{};
	if (bApplyCooldown && GetWorld() != nullptr)
		NextNoiseInvestigationTime = GetWorld()->GetTimeSeconds() + (FMath::IsFinite(NoiseInvestigationCooldown) ? FMath::Max(0.1f, NoiseInvestigationCooldown) : 1.5f);
	StopMovementIfNeeded();
	SetAIState(EFPEnemyAIState::Idle);
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

bool AfpstrueEnemyAIController::InitializeBlackboard(UBlackboardComponent& BlackboardComp, UBlackboardData& BlackboardAsset)
{
	return Super::InitializeBlackboard(BlackboardComp, BlackboardAsset) && CacheDecisionBlackboardKeys(BlackboardComp);
}

bool AfpstrueEnemyAIController::CacheDecisionBlackboardKeys(UBlackboardComponent& BlackboardComp)
{
	UBlackboardData* Asset = BlackboardComp.GetBlackboardAsset();
	if (Asset != nullptr && CachedDecisionBlackboardAsset.Get() == Asset) return true;
	CachedDecisionBlackboardAsset.Reset();
	DecisionKeys = FDecisionBlackboardKeys{};
	if (!FPHasEnemyBlackboardSchema(Asset)) return false;
	DecisionKeys.TargetActor = Asset->GetKeyID(FPEnemyBlackboard::TargetActor);
	DecisionKeys.HasTarget = Asset->GetKeyID(FPEnemyBlackboard::HasTarget);
	DecisionKeys.Attacking = Asset->GetKeyID(FPEnemyBlackboard::Attacking);
	DecisionKeys.InAttackRange = Asset->GetKeyID(FPEnemyBlackboard::InAttackRange);
	DecisionKeys.InChaseRange = Asset->GetKeyID(FPEnemyBlackboard::InChaseRange);
	DecisionKeys.Retreating = Asset->GetKeyID(FPEnemyBlackboard::Retreating);
	DecisionKeys.TacticalRole = Asset->GetKeyID(FPEnemyBlackboard::TacticalRole);
	DecisionKeys.TacticalGoal = Asset->GetKeyID(FPEnemyBlackboard::TacticalGoal);
	DecisionKeys.HasInnerSlot = Asset->GetKeyID(FPEnemyBlackboard::HasInnerSlot);
	CachedDecisionBlackboardAsset = Asset;
	return true;
}

// 校验配置树及黑板契约，拒绝不兼容配置；随机首轮等待用于分散群体决策峰值。
bool AfpstrueEnemyAIController::StartBehaviorLogic()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || GetNetMode() == NM_Client)
	{
		return false;
	}
	// 只运行配置资产；UE 为各敌人分别维护 Blackboard 和节点内存。
	ActiveBehaviorTree = BehaviorTreeAsset;
	if (ActiveBehaviorTree == nullptr || !FPHasEnemyBlackboardSchema(ActiveBehaviorTree->BlackboardAsset))
	{
		UE_LOG(LogTemp, Error, TEXT("Enemy BehaviorTree %s has an incompatible Blackboard; activation rejected."),
			   *GetNameSafe(ActiveBehaviorTree));
		ActiveBehaviorTree = nullptr;
	}
	if (ActiveBehaviorTree == nullptr || ActiveBehaviorTree->RootNode == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("No valid enemy BehaviorTree is available on %s"), *GetName());
		ActiveBehaviorTree = nullptr;
		StopAI();
		return false;
	}
	// 随机首帧延迟把批量生成的敌人错开，避免所有 Controller 在同一帧同时第一次决策。
	NextBehaviorDecisionDelay = FMath::FRandRange(0.01f, FMath::Max(0.01f, ChaseDecisionInterval * SignificanceDecisionMultiplier));
	if (!RunBehaviorTree(ActiveBehaviorTree))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to start enemy BehaviorTree on %s"), *GetName());
		ActiveBehaviorTree = nullptr;
		StopAI();
		return false;
	}
	// UseBlackboard 可以直接复用同资产而不再调 InitializeBlackboard；始终绑定实际运行的 schema。
	UBlackboardComponent* DecisionBlackboard = GetBlackboardComponent();
	if (DecisionBlackboard == nullptr || !CacheDecisionBlackboardKeys(*DecisionBlackboard))
	{
		ActiveBehaviorTree = nullptr;
		StopAI();
		return false;
	}
	CommandPhase = ECommandPhase::Accepting;
	// 首次启动与显式合法重启都从当前生命值恢复事件订阅，不保留旧停止周期的观测值。
	EnemyHealthComponent = ControlledEnemy->FindComponentByClass<UfpstrueHealthComponent>();
	if (EnemyHealthComponent != nullptr)
	{
		LastObservedHealth = EnemyHealthComponent->GetHealth();
		EnemyHealthComponent->OnHealthChanged.AddUniqueDynamic(this, &ThisClass::HandleEnemyHealthChanged);
	}
	bRetreatRequested = IsBelowRetreatThreshold();
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Sight::StaticClass(), true);
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Hearing::StaticClass(), true);
	if (IsBehaviorReady()) return true;
	ActiveBehaviorTree = nullptr;
	StopAI();
	return false;
}

// ==================== 单轮决策：维护上下文 → 距离筛选/申请资源 → 发布快照 ====================

bool AfpstrueEnemyAIController::PrepareBehaviorDecisionContext()
{
	AfpstrueEnemyCharacter* const PreparingEnemy = ControlledEnemy;
	if (bInvestigatingNoise && GetWorld() != nullptr && GetWorld()->GetTimeSeconds() >= TargetForgetTime) FinishNoiseInvestigation();
	if (!AcceptsCombatCommands() || ControlledEnemy != PreparingEnemy) return false;
	// 死亡入口负责停止树；这里仅拒绝继续使用敌人，不在 BT 任务内重入 StopTree。
	if (!IsValid(PreparingEnemy) || PreparingEnemy->IsDead()) return true;
	if (!IsTargetUsable(TargetCharacter))
	{
		// 只撤销失效上下文；后续选择只读已感知玩家，不从全局玩家索引偷偷恢复追击。
		InitializeCombatContext(nullptr, nullptr);
		if (!AcceptsCombatCommands() || ControlledEnemy != PreparingEnemy) return false;
	}
	SelectPerceivedTarget();
	if (!AcceptsCombatCommands() || ControlledEnemy != PreparingEnemy) return false;
	if (IsTargetUsable(TargetCharacter) && !IsValid(SurroundManager))
	{
		AfpstrueCharacter* const ResolvingTarget = TargetCharacter;
		AfpstrueSurroundManager* ResolvedManager = ResolveSurroundManager();
		if (!AcceptsCombatCommands() || ControlledEnemy != PreparingEnemy || TargetCharacter != ResolvingTarget) return false;
		SurroundManager = ResolvedManager;
	}
	return true;
}

bool AfpstrueEnemyAIController::RefreshTacticalAssignment()
{
	if (!IsValid(SurroundManager)) return true;
	AfpstrueEnemyCharacter* const RequestingEnemy = ControlledEnemy;
	AfpstrueCharacter* const RequestingTarget = TargetCharacter;
	AfpstrueSurroundManager* const RequestingManager = SurroundManager;
	FFPEnemyTacticalAssignment Assignment;
	const bool bRegistered = RequestingManager->GetOrRequestTacticalAssignment(RequestingEnemy, Assignment);
	// 授席通知可同步停路、换目标或结束 AI；旧调用栈不得回写新上下文的分配。
	if (!AcceptsCombatCommands() || ControlledEnemy != RequestingEnemy || TargetCharacter != RequestingTarget ||
		SurroundManager != RequestingManager || !bTargetVisible || !IsTargetUsable(RequestingTarget)) return false;
	if (bRegistered) TacticalAssignment = Assignment;
	return true;
}

// 准备阶段允许资源和上下文副作用，随后才生成并一次性发布本轮快照；BT 仍独占高层分支选择。
void AfpstrueEnemyAIController::RefreshBehaviorDecision(UBlackboardComponent& DecisionBlackboard)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyAI_SampleDecision);
	CSV_SCOPED_TIMING_STAT(fpstrueAI, DecisionTime);
	CSV_CUSTOM_STAT(fpstrueAI, DecisionCount, 1, ECsvCustomStatOp::Accumulate);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAIDecisionTime);
	INC_DWORD_STAT(STAT_fpstrueAIDecisionCount);
	if (!AcceptsCombatCommands() || !CacheDecisionBlackboardKeys(DecisionBlackboard)) return;
	DecisionContext = FDecisionContext{};
	TacticalAssignment = FFPEnemyTacticalAssignment{};
	if (!PrepareBehaviorDecisionContext()) return;
	const bool bHasTarget = IsValid(ControlledEnemy) && !ControlledEnemy->IsDead() && IsTargetUsable(TargetCharacter)
		&& (bTargetVisible || GetWorld()->GetTimeSeconds() < TargetForgetTime);

	// 一轮只计算一次二维距离平方，并派生攻击/追击两个布尔条件，避免各分支重复开方和取位置。
	// 敌人和目标校验成功后才采样；叶子保留存活检查，但不重复计算距离与范围。
	// 先用廉价距离筛选，再检查高度和环境；楼上/墙后目标应走导航分支而不是原地重复尝试攻击。
	if (bHasTarget)
	{
		if (bTargetVisible)
		{
			// 多感官接入后明确读取 Sight，不能把较新的 Hearing 位置冒充视觉采样。
			if (const FActorPerceptionInfo* Info = GetPerceptionComponent()->GetActorInfo(*TargetCharacter))
			{
				const FVector SightLocation = Info->GetStimulusLocation(UAISense::GetSenseID<UAISense_Sight>());
				if (FAISystem::IsValidLocation(SightLocation) && !SightLocation.ContainsNaN()) LastKnownTargetLocation = SightLocation;
			}
		}
		const UfpstrueEnemyCombatComponent* Combat = ControlledEnemy->GetCombatComponent();
		DecisionContext.DistanceSquared = FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), LastKnownTargetLocation);
		DecisionContext.EffectiveAttackRange = Combat != nullptr ? Combat->GetConfiguredAttackRange() : 0.0f;
		DecisionContext.bInChaseRange = DecisionContext.DistanceSquared <= FMath::Square(ControlledEnemy->GetChaseRange());
		if (bTargetVisible && DecisionContext.bInChaseRange && !bRetreatRequested && !bRetreatActive)
		{
			if (!RefreshTacticalAssignment()) return;
			// 非内环只维护候场/外环目标，不执行不能转化为攻击的近战遮挡查询。
			if (Combat != nullptr && TacticalAssignment.Role == EFPEnemyTacticalRole::Inner)
				DecisionContext.bInAttackRange = Combat->SampleAttackReach(DecisionContext.DistanceSquared, DecisionContext.EffectiveAttackRange);
		}
	}
	if ((bRetreatRequested || bRetreatActive) && (!bHasTarget || !IsBelowRetreatThreshold()))
	{
		AfpstrueEnemyCharacter* const FinishingEnemy = ControlledEnemy;
		AfpstrueCharacter* const FinishingTarget = TargetCharacter;
		AfpstrueSurroundManager* const FinishingManager = SurroundManager;
		FinishRetreat(); // 归还掩体/停路也可同步触发外部回调，之后才能继续发布这一轮。
		if (!AcceptsCombatCommands() || ControlledEnemy != FinishingEnemy || TargetCharacter != FinishingTarget || SurroundManager != FinishingManager) return;
	}

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
	DecisionBlackboard.SetValue<UBlackboardKeyType_Object>(DecisionKeys.TargetActor, bHasTarget && bTargetVisible ? TargetCharacter.Get() : nullptr);
	DecisionBlackboard.SetValue<UBlackboardKeyType_Bool>(DecisionKeys.HasTarget, bHasTarget);
	DecisionBlackboard.SetValue<UBlackboardKeyType_Bool>(DecisionKeys.Attacking, bHasTarget && ControlledEnemy->IsAttacking());
	DecisionBlackboard.SetValue<UBlackboardKeyType_Bool>(DecisionKeys.InAttackRange, DecisionContext.bInAttackRange);
	DecisionBlackboard.SetValue<UBlackboardKeyType_Bool>(DecisionKeys.InChaseRange, DecisionContext.bInChaseRange);
	DecisionBlackboard.SetValue<UBlackboardKeyType_Bool>(DecisionKeys.Retreating, bHasTarget && (bRetreatRequested || bRetreatActive));
	// 战术字段只是 Manager 分配结果的镜像；完整契约已在启动时校验。
	DecisionBlackboard.SetValue<UBlackboardKeyType_Int>(DecisionKeys.TacticalRole, static_cast<int32>(TacticalAssignment.Role));
	if (TacticalAssignment.bHasGoal) DecisionBlackboard.SetValue<UBlackboardKeyType_Vector>(DecisionKeys.TacticalGoal, TacticalAssignment.Goal);
	else DecisionBlackboard.ClearValue(DecisionKeys.TacticalGoal);
	DecisionBlackboard.SetValue<UBlackboardKeyType_Bool>(DecisionKeys.HasInnerSlot,
		TacticalAssignment.Role == EFPEnemyTacticalRole::Inner && TacticalAssignment.Revision != 0);
	DecisionBlackboard.ResumeObserverNotifications(true);
}

// 近战保持快速响应，其他状态按距离与重要性降频；返回本轮之后使用的间隔。
float AfpstrueEnemyAIController::GetNextDecisionInterval(const FDecisionContext& Context) const
{
	// 战斗响应优先于 Significance 降频；Reduced/Background 倍率只放大追击、远距和 Idle 间隔。
	if (FFPRuntimeOptions::Get().bDisableAIThrottling)
	{
		return AttackDecisionInterval;
	}

	// 编辑器 Clamp 不约束所有运行时来源；非有限值回退到旧默认值，再限制有效倍率。
	const float SafeCombatResponseRangeMultiplier =
		FMath::IsFinite(CombatResponseRangeMultiplier) ? FMath::Clamp(CombatResponseRangeMultiplier, 1.0f, 10.0f) : 1.5f;
	const bool bNeedsCombatResponse =
		ControlledEnemy->IsAttacking() || AIState == EFPEnemyAIState::Attack ||
		(TacticalAssignment.Role == EFPEnemyTacticalRole::Inner &&
		Context.DistanceSquared <= FMath::Square(Context.EffectiveAttackRange * SafeCombatResponseRangeMultiplier));
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

// 始终使用该目标玩家在唯一 GameMode 中登记的管理器。
AfpstrueSurroundManager* AfpstrueEnemyAIController::ResolveSurroundManager() const
{
	auto* Mode = GetWorld() != nullptr ? GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>() : nullptr;
	return Mode != nullptr && IsTargetUsable(TargetCharacter) ? Mode->GetOrCreateSurroundManager(TargetCharacter) : nullptr;
}

// 仅检查候选玩家是否有效且存活；是否可见由感知状态另行约束。
bool AfpstrueEnemyAIController::IsTargetUsable(const AfpstrueCharacter* Target) const
{
	// 目标必须存在且存活；死亡后的停止、槽位释放由上层统一处理。
	if (!IsValid(Target) || Target->IsActorBeingDestroyed() || Target->IsDead() || Target->GetWorld() != GetWorld()) return false;
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
		FinishNoiseInvestigation();
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
		const bool bSearch = !bTargetVisible && SafeYawSpeed > 0.0f && IsValid(ControlledEnemy) && !ControlledEnemy->IsDead() &&
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
	if (!IsValid(SurroundManager)) return false;
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
		if (SurroundManager != nullptr && !SurroundManager->HasInnerSlot(ControlledEnemy))
		{
			ControlledEnemy->GetCombatComponent()->ResetCombat();
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
		if (Combat == nullptr || !DecisionContext.bInAttackRange || !Combat->IsAttackReady() || !IsTacticalAttackPositionReady(false))
		{
			return false;
		}
		SetAIState(EFPEnemyAIState::Chase);
		// 设置 ControlRotation 不代表角色已经转正：Movement 需要后续更新才能按 RotationRate 转过去。
		// 准备阶段不启动 Montage/伤害事务，也不长期占用攻击名额；下一轮重新校验距离、冷却与预算。
		if (!FaceTargetAtRest())
		{
			// 转向未完成时结束本轮，不让 fallback 重新 MoveTo；下一轮再次检查冷却和许可。
			return true;
		}
		SetAIState(EFPEnemyAIState::Attack);
		if (Combat->TryAttackTarget() || Combat->IsAttacking())
		{
			return true; // 请求成功或动画回调启动了替代事务，都不能归还仍在使用的名额。
		}
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
		// Pursue 仅用于已失去视觉的有限记忆/声音调查，无槽位不能退化为追玩家中心。
		if (bTargetVisible) return false;
		SetAIState(EFPEnemyAIState::Chase);
		const EMoveGoalResult MoveResult = MoveToGoal(LastKnownTargetLocation, MoveAcceptanceRadius, false);
		if (bInvestigatingNoise && (MoveResult == EMoveGoalResult::Arrived || MoveResult == EMoveGoalResult::Unreachable || MoveResult == EMoveGoalResult::InvalidContext))
			FinishNoiseInvestigation();
		return true;
	}
	default:
		return false;
	}
}

// 消费已授权的目标；不可达时释放本次席位，不能退回玩家中心。
bool AfpstrueEnemyAIController::HandleSurroundMovement()
{
	// Controller 不计算全局站位，只消费 SurroundManager 已缓存并投影到 NavMesh 的接近点。
	if (!IsValid(SurroundManager))
	{
		return false;
	}

	FVector Goal;
	if (!TacticalAssignment.bHasGoal || !SurroundManager->ResolveTacticalMoveGoal(ControlledEnemy, TacticalAssignment, Goal))
	{
		StopMovementIfNeeded();
		return true;
	}

	SetAIState(EFPEnemyAIState::Chase);
	const bool bInner = TacticalAssignment.Role == EFPEnemyTacticalRole::Inner;
	const EMoveGoalResult Result = MoveToGoal(Goal, bInner ? CombatMoveAcceptanceRadius : MoveAcceptanceRadius, bInner);
	if (Result == EMoveGoalResult::Unreachable || Result == EMoveGoalResult::InvalidContext)
	{
		const uint64 FailedRevision = TacticalAssignment.Revision;
		StopMovementIfNeeded();
		SurroundManager->ReportTacticalMoveFailure(ControlledEnemy, FailedRevision);
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
// 【设计取舍】共享的是目标规划与提交预算，不是让多名敌人共用一条路径；各自仍走原生 PathFollowing。
// 拒绝新请求时保留旧路径，挤离槽位/路径失效后再重寻路，避免把拥堵优化做成冻结战斗单位。
AfpstrueEnemyAIController::EMoveGoalResult AfpstrueEnemyAIController::MoveToGoal(
	const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority)
{
	if (!AcceptsCombatCommands() || !IsValid(ControlledEnemy) || ControlledEnemy->IsDead() || GetWorld() == nullptr ||
		GoalLocation.ContainsNaN() || !FMath::IsFinite(AcceptanceRadius) || AcceptanceRadius < 0.0f ||
		!IsValid(SurroundManager))
	{
		return EMoveGoalResult::InvalidContext;
	}
	// 自动重规划和玩家移动都可能使原本合法的旧路径穿圈；在既有决策轮复核，早于去重/预算保留旧路径。
	// 这是软准入边界，不新增逐帧空气墙，也不让隐藏目标记忆和撤退读取实时玩家圈。
	if (bTargetVisible && !bRetreatActive && SurroundManager != nullptr && OwnedMoveRequestId.IsValid() &&
		GetCurrentMoveRequestID() == OwnedMoveRequestId && GetMoveStatus() != EPathFollowingStatus::Idle)
	{
		const UPathFollowingComponent* Following = GetPathFollowingComponent();
		const FNavPathSharedPtr Path = Following->GetPath();
		// 只检查当前目标点之后的剩余路径，不把已走过的历史前缀倒接回当前角色。
		if (Path.IsValid() && !SurroundManager->IsTacticalPathAllowed(ControlledEnemy, *Path, Following->GetNextPathIndex()))
		{
			StopMovementIfNeeded();
			return EMoveGoalResult::Unreachable; // 战术叶子统一报告 Revision 并按既有策略回收、退避。
		}
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

FAIRequestID AfpstrueEnemyAIController::RequestMove(const FAIMoveRequest& MoveRequest, FNavPathSharedPtr Path)
{
	// 在原生 MoveTo 已完成投影和寻路、尚未发布路径时检查整条折线，而非只检查合法终点。
	// 隐藏目标的记忆搜索和撤退不读取实时战斗圈；主动围攻才受准入边界约束。
	if (!AcceptsCombatCommands() || (bTargetVisible && !bRetreatActive && SurroundManager != nullptr &&
		Path.IsValid() && !SurroundManager->IsTacticalPathAllowed(ControlledEnemy, *Path))) return FAIRequestID::InvalidRequest;
	return Super::RequestMove(MoveRequest, Path);
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
	if (CommandPhase == ECommandPhase::Stopping || CommandPhase == ECommandPhase::Stopped) return;
	// StopLogic/StopMovement/ResetCombat 都可能同步回调；先关门，旧调用栈就不能注入新上下文。
	CommandPhase = ECommandPhase::Stopping;
	MoveFailures[0] = MoveFailures[1] = FMoveFailure{};
	bInvestigatingNoise = false;
	LastAcceptedNoiseTime = -TNumericLimits<double>::Max();
	NextNoiseInvestigationTime = 0.0;
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
	GetPerceptionComponent()->SetSenseEnabled(UAISense_Hearing::StaticClass(), false);
	GetPerceptionComponent()->ForgetAll();
	// 先停生产命令的行为树，再取消正在执行的移动和攻击。
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
	if (UBlackboardComponent* DecisionBlackboard = GetBlackboardComponent(); DecisionBlackboard != nullptr && CacheDecisionBlackboardKeys(*DecisionBlackboard))
	{
		// RunBehaviorTree 可以复用兼容 Blackboard；停止后不能保留上一轮目标和条件。
		DecisionBlackboard->PauseObserverNotifications();
		DecisionBlackboard->ClearValue(DecisionKeys.TargetActor);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Bool>(DecisionKeys.HasTarget, false);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Bool>(DecisionKeys.Attacking, false);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Bool>(DecisionKeys.InAttackRange, false);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Bool>(DecisionKeys.InChaseRange, false);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Bool>(DecisionKeys.Retreating, false);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Int>(DecisionKeys.TacticalRole, static_cast<int32>(EFPEnemyTacticalRole::Waiting));
		DecisionBlackboard->ClearValue(DecisionKeys.TacticalGoal);
		DecisionBlackboard->SetValue<UBlackboardKeyType_Bool>(DecisionKeys.HasInnerSlot, false);
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
		// 停路不等于换上下文；失败出口刚写入的目标级退避必须保留，尤其是没有槽位可回收的候场者。
	}
	if (bShouldStop)
	{
		StopMovement();
	}
}

// 归还本敌人的稳定槽位，让管理器按需安排外环补位。
void AfpstrueEnemyAIController::ReleaseSurroundSlot()
{
	TacticalAssignment = FFPEnemyTacticalAssignment{};
	// 将敌人与稳定槽位的映射交还给群体 Manager，并触发可能的外环补位。
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseSurroundSlot(ControlledEnemy);
	}
}
