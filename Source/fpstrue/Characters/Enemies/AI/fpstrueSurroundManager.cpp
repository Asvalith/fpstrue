// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "DrawDebugHelpers.h"
#include "EngineGlobals.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "Characters/Enemies/AI/fpstrueEnemyPositionQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Components/CapsuleComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

CSV_DEFINE_CATEGORY(fpstrueEQS, true);

namespace
{
	// 站位和攻击许可按目标分组，寻路提交预算按 World 共用；弱 World 键不保活已经退出的关卡。
	struct FWorldMoveRequestBudget
	{
		TSet<TWeakObjectPtr<AfpstrueSurroundManager>> Managers;
		uint64 Frame = MAX_uint64;
		int32 Consumed = 0;
		int32 Maximum = 8;
		int32 ReservedCombat = 2;
	};
	TMap<TWeakObjectPtr<UWorld>, FWorldMoveRequestBudget> WorldMoveRequestBudgets;
}

/*
 * 多敌人共享的群体协调器。
 * 它不替代单个敌人的行为树决策，只集中维护必须全局一致的资源：玩家位置快照、稳定槽位、
 * 并发攻击名额和每帧 MoveTo 提交预算，从而减少围攻拥堵和重复导航请求。
 *
 * 数据结构职责：
 *   Participants 是唯一请求身份账本，FIFO 决定内圈准入先后；固定内外环和 EnemyToSlot 保存稳定占位，
 *   ActiveAttackers 仅保存单次出手许可，完成一次攻击不重新排站位队列。
 * 敌人键使用 TWeakObjectPtr，Manager 不拥有敌人生命周期；死亡/销毁由主动释放和失效项清理双重兜底。
 * 玩家位置与槽位 NavMesh 投影按 Timer/位移阈值批量刷新，单个 AIController 只读取缓存结果。
 */

// ==================== 生命周期与目标注入 ====================

// Manager 不需要逐帧 Tick；目标快照和调试绘制分别由低频 Timer 驱动。
AfpstrueSurroundManager::AfpstrueSurroundManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

// 构建稳定内外环槽位；只有调试开关开启时才启动可视化 Timer。
void AfpstrueSurroundManager::BeginPlay()
{
	// 关卡开始时建立固定槽位拓扑；调试绘制只有显式开启后才产生 Timer 成本。
	Super::BeginPlay();
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	for (auto It = WorldMoveRequestBudgets.CreateIterator(); It; ++It)
		if (!It.Key().IsValid()) It.RemoveCurrent();
	WorldMoveRequestBudgets.FindOrAdd(GetWorld()).Managers.Add(this);
	BuildSlots();

	if (bDrawDebugSlots)
	{
		GetWorldTimerManager().SetTimer(DebugDrawTimerHandle, this, &AfpstrueSurroundManager::DrawDebugSlots, DebugDrawInterval, true);
	}
}

// 停止调试与共享快照更新，并清除当前对局的槽位、许可和缓存。
void AfpstrueSurroundManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 清理两个 Timer、弱引用容器和槽位占用，避免关卡切换后保留旧玩家或敌人。
	GetWorldTimerManager().ClearTimer(DebugDrawTimerHandle);
	if (FWorldMoveRequestBudget* Budget = WorldMoveRequestBudgets.Find(GetWorld())) Budget->Managers.Remove(this);
	ResetManager();
	Super::EndPlay(EndPlayReason);
}

// 注入唯一共享目标；换目标时撤销全部旧分配，有效目标建立快照并启动低频刷新。
void AfpstrueSurroundManager::SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter)
{
	// GameMode 在开局注入唯一玩家；目标变化时重建快照并重新启动共享刷新 Timer。
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	if (!IsValid(NewTargetCharacter) || NewTargetCharacter->IsDead() || NewTargetCharacter->IsActorBeingDestroyed()
		|| NewTargetCharacter->GetWorld() != GetWorld()) NewTargetCharacter = nullptr;
	if (TargetCharacter == NewTargetCharacter && bHasSharedTargetSnapshot)
	{
		return;
	}

	// 槽位、许可和投影属于同一个目标；换目标不能继承上一目标的分配。
	if (TargetCharacter != NewTargetCharacter)
	{
		ResetManager();
	}
	TargetCharacter = NewTargetCharacter;
	if (!IsValid(TargetCharacter))
	{
		bHasSharedTargetSnapshot = false;
		GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
		return;
	}

	UpdateSharedTargetSnapshot(true);
	// Timer 回调只负责触发带位移阈值的刷新，具体缓存更新集中在下层函数。
	GetWorldTimerManager().SetTimer(SharedTargetTimerHandle,
		FTimerDelegate::CreateUObject(this, &AfpstrueSurroundManager::UpdateSharedTargetSnapshot, false),
		FMath::Max(SharedTargetRefreshInterval, 0.05f), true);
}

// ==================== 共享目标与 NavMesh 投影缓存 ====================

// 按位移、年龄或导航重试刷新目标快照；只有位置或导航缓存需要更新时才批量重新投影。
void AfpstrueSurroundManager::UpdateSharedTargetSnapshot(bool bForce)
{
	// 位移阈值、最大陈旧时间和导航重试共同限定缓存寿命。
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	if (!IsValid(TargetCharacter) || TargetCharacter->IsDead() || TargetCharacter->IsActorBeingDestroyed())
	{
		ResetManager();
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	// 与共享位置 Timer 复用同一低频维护点；即使玩家没移动也要回收失效/卡住的席位。
	CleanupInvalidEntries();
	PumpTacticalQueue();
	const FVector CurrentTargetLocation = TargetCharacter->GetActorLocation();
	const bool bMovedEnough =
		FVector::DistSquared(CurrentTargetLocation, CachedTargetLocation) >= FMath::Square(SharedTargetMoveThreshold);
	const bool bSnapshotExpired = Now - SharedTargetSnapshotTime >= FMath::Max(0.05f, SharedTargetMaxAge);
	const bool bRetryNavigation = bNeedsNavigationRetry && Now >= NextNavigationRetryTime;
	const bool bNavigationExpired = Now - NavigationCacheTime >= FMath::Max(0.05f, NavigationCacheMaxAge);
	if (!bForce && bHasSharedTargetSnapshot && !bMovedEnough && !bSnapshotExpired && !bRetryNavigation && !bNavigationExpired)
	{
		return;
	}

	// 发布新位置后再更新关联的槽位投影，成功投影与失败重试各自有有限缓存寿命。
	const bool bLocationChanged = !CurrentTargetLocation.Equals(CachedTargetLocation, 1.0f);
	CachedTargetLocation = CurrentTargetLocation;
	SharedTargetSnapshotTime = Now;
	const bool bReproject = bForce || !bHasSharedTargetSnapshot || bLocationChanged || bRetryNavigation || bNavigationExpired;
	bHasSharedTargetSnapshot = true;
	if (bReproject)
	{
		RebuildProjectedSlotCache();
	}
}

// 创建均匀极角分布的内外环槽位；固定拓扑不因每轮 AI 决策重新分配。
void AfpstrueSurroundManager::BuildSlots()
{
	// 槽位只保存相对角度和半径；世界位置基于最新玩家快照统一计算，保证所有敌人使用同一参考系。
	SurroundSlots.Reset();

	// 为一层环创建角度和半径，实际世界坐标稍后根据共享玩家快照计算。
	const auto AddRing = [this](int32 RingIndex, int32 SlotCount, float Radius)
	{
		const int32 SafeSlotCount = FMath::Clamp(SlotCount, 4, RingIndex == 0 ? 24 : 32);
		for (int32 SlotIndex = 0; SlotIndex < SafeSlotCount; ++SlotIndex)
		{
			FfpstrueSurroundSlot& Slot = SurroundSlots.AddDefaulted_GetRef();
			Slot.RingIndex = RingIndex;
			Slot.AngleRadians = 2.0f * PI * static_cast<float>(SlotIndex) / static_cast<float>(SafeSlotCount);
			Slot.Radius = Radius;
		}
	};

	AddRing(0, InnerSlotCount, InnerRadius);
	AddRing(1, OuterSlotCount, OuterRadius);
	if (bHasSharedTargetSnapshot)
	{
		RebuildProjectedSlotCache();
	}
}

// 一次性投影所有站位点和攻击接近点；失败保留重试标记，各 AI 仅消费投影结果而非共享路径。
void AfpstrueSurroundManager::RebuildProjectedSlotCache()
{
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	// 同时缓存“站位点”和更靠近玩家的“攻击接近点”，AI 决策阶段不再重复 ProjectPointToNavigation。
	// 导航系统存在且投影成功时才输出可用位置，失败不伪造可达点。
	// 每次只集中投影一次；失败标志独立于位置快照有效性。
	// 上述投影缓存职责保留，但生产和筛选已由异步 EQS 执行；不再保留手写投影循环。
	UWorld* World = GetWorld();
	UEnvQueryManager* Queries = World != nullptr ? UEnvQueryManager::GetCurrent(World) : nullptr;
	if (Queries == nullptr || !bHasSharedTargetSnapshot) return;
	const double Now = World->GetTimeSeconds();
	if (ApproachQueryId != INDEX_NONE)
	{
		if (Now - ApproachQueryStartedTime < 2.0) return;
		// ★【重要边界】[异步身份] AbortQuery 会同步调用结束委托；先作废身份，再停止旧查询。
		const int32 OldQuery = ApproachQueryId;
		ApproachQueryId = INDEX_NONE;
		++PositionQueryRevision;
		Queries->AbortQuery(OldQuery);
	}
	if (ApproachQueryTemplate == nullptr) ApproachQueryTemplate = FPCreateEnemyPositionQuery(*this, false);
	const uint32 Revision = ++PositionQueryRevision;
	const FVector QueryTarget = CachedTargetLocation;
	FEnvQueryRequest Request(ApproachQueryTemplate, this);
	Request.SetFloatParam(TEXT("OriginX"), QueryTarget.X).SetFloatParam(TEXT("OriginY"), QueryTarget.Y).SetFloatParam(TEXT("OriginZ"), QueryTarget.Z);
	Request.SetFloatParam(TEXT("ThreatX"), QueryTarget.X).SetFloatParam(TEXT("ThreatY"), QueryTarget.Y).SetFloatParam(TEXT("ThreatZ"), QueryTarget.Z);
	Request.SetFloatParam(TEXT("Cover"), 0.0f).SetFloatParam(TEXT("InnerCount"), FMath::Clamp(InnerSlotCount, 4, 24));
	Request.SetFloatParam(TEXT("OuterCount"), FMath::Clamp(OuterSlotCount, 4, 32));
	Request.SetFloatParam(TEXT("InnerRadius"), InnerRadius).SetFloatParam(TEXT("OuterRadius"), OuterRadius);
	Request.SetFloatParam(TEXT("InnerApproach"), AttackApproachRadius);
	Request.SetFloatParam(TEXT("ProjectionX"), NavigationProjectionExtent.X).SetFloatParam(TEXT("ProjectionY"), NavigationProjectionExtent.Y).SetFloatParam(TEXT("ProjectionZ"), NavigationProjectionExtent.Z);
	ApproachQueryStartedTime = NavigationCacheTime = Now;
	bNeedsNavigationRetry = false;
	ApproachQueryId = Request.Execute(EEnvQueryRunMode::AllMatching,
		FQueryFinishedSignature::CreateUObject(this, &ThisClass::HandleApproachQueryFinished, Revision, QueryTarget));
	CSV_CUSTOM_STAT(fpstrueEQS, ApproachQueries, 1, ECsvCustomStatOp::Accumulate);
	if (ApproachQueryId == INDEX_NONE)
	{
		bNeedsNavigationRetry = true;
		NextNavigationRetryTime = Now + FMath::Max(0.05f, NavigationRetryInterval);
	}
}

// 校验查询身份后一次发布新位置，候选排序不影响槽位身份；相邻点还需满足几何间距。
void AfpstrueSurroundManager::HandleApproachQueryFinished(TSharedPtr<FEnvQueryResult> Result, uint32 Revision, FVector QueryTarget)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEQS_PublishApproach);
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	if (!Result.IsValid() || Result->QueryID != ApproachQueryId || Revision != PositionQueryRevision ||
		!bHasSharedTargetSnapshot || !IsValid(TargetCharacter)) return;
	ApproachQueryId = INDEX_NONE;
	const double Now = GetWorld()->GetTimeSeconds();
	NavigationCacheTime = Now;
	bNeedsNavigationRetry = true;
	NextNavigationRetryTime = Now + FMath::Max(0.05f, NavigationRetryInterval);
	if (!Result->IsSuccessful() || Result->ItemType != UfpstrueEnvQueryItemType_Position::StaticClass() ||
		FVector::DistSquared(QueryTarget, CachedTargetLocation) > FMath::Square(FMath::Max(25.0f, SharedTargetMoveThreshold)))
	{
		for (FfpstrueSurroundSlot& Slot : SurroundSlots) Slot.bHasProjectedSlotLocation = Slot.bHasProjectedApproachLocation = false;
		return;
	}
	for (FfpstrueSurroundSlot& Slot : SurroundSlots) Slot.bHasProjectedSlotLocation = Slot.bHasProjectedApproachLocation = false;
	// 分数相同也采用固定槽位和偏移顺序，避免每轮查询排序波动导致站位来回变化。
	Result->Items.Sort([&Result](const FEnvQueryItem& A, const FEnvQueryItem& B)
	{
		if (A.Score != B.Score) return A.Score > B.Score;
		const FFPEnemyQueryPoint Left = UfpstrueEnvQueryItemType_Position::GetValue(Result->RawData.GetData() + A.DataOffset);
		const FFPEnemyQueryPoint Right = UfpstrueEnvQueryItemType_Position::GetValue(Result->RawData.GetData() + B.DataOffset);
		if (Left.SlotIndex != Right.SlotIndex) return Left.SlotIndex < Right.SlotIndex;
		if (Left.Kind != Right.Kind) return Left.Kind < Right.Kind;
		return Left.Preference < Right.Preference;
	});
	for (int32 Index = 0; Index < Result->Items.Num(); ++Index)
	{
		if (!Result->Items[Index].IsValid()) continue;
		const FFPEnemyQueryPoint& Point = Result->GetItemAsTypeChecked<UfpstrueEnvQueryItemType_Position>(Index);
		if (!SurroundSlots.IsValidIndex(Point.SlotIndex) || Point.Nav.Location.ContainsNaN()) continue;
		FfpstrueSurroundSlot& Slot = SurroundSlots[Point.SlotIndex];
		if (Point.Kind == 0 && !Slot.bHasProjectedSlotLocation)
		{
			Slot.ProjectedSlotLocation = Point.Nav.Location;
			Slot.bHasProjectedSlotLocation = true;
		}
		else if (Point.Kind == 1 && Slot.RingIndex == 0 && !Slot.bHasProjectedApproachLocation)
		{
			bool bOverlaps = false;
			for (const FfpstrueSurroundSlot& Other : SurroundSlots)
			{
				if (Other.bHasProjectedApproachLocation &&
					FVector::DistSquared2D(Other.ProjectedApproachLocation, Point.Nav.Location) < FMath::Square(MinPositionSpacing))
				{
					bOverlaps = true;
					break;
				}
			}
			if (!bOverlaps)
			{
				Slot.ProjectedApproachLocation = Point.Nav.Location;
				Slot.bHasProjectedApproachLocation = true;
			}
		}
	}
	int32 Available = 0;
	// 重试只跟真正消费的目标一致：内圈靠攻击接近点，外圈靠布局站位；不为废弃的外圈 LOS 结果重查。
	for (const FfpstrueSurroundSlot& Slot : SurroundSlots)
		Available += (Slot.RingIndex == 0 ? Slot.bHasProjectedApproachLocation : Slot.bHasProjectedSlotLocation) ? 1 : 0;
	bNeedsNavigationRetry = Available != SurroundSlots.Num();
	PublishedTargetLocation = QueryTarget;
	// 保留既有 CSV 列名，数值表示当前真正可用的战术席位，而非已废弃的外圈接近点。
	CSV_CUSTOM_STAT(fpstrueEQS, ApproachAvailableSlots, Available, ECsvCustomStatOp::Set);
	PumpTacticalQueue();
}

// 低血撤退只在首次请求时发查询；后续轮询同一缓存，未获得提交预算时等待而非执行同步查询。
EFPCoverQueryResult AfpstrueSurroundManager::FindOrRequestCover(AfpstrueEnemyCharacter* Enemy, const FVector& KnownThreatLocation, FVector& OutLocation)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || GetNetMode() == NM_Client || World == nullptr || !IsValid(Enemy)
		|| Enemy->IsDead() || KnownThreatLocation.ContainsNaN()) return EFPCoverQueryResult::Failed;
	const TWeakObjectPtr<AfpstrueEnemyCharacter> Key(Enemy);
	if (FCoverRequest* Existing = CoverRequests.Find(Key))
	{
		if (Existing->State == EFPCoverQueryResult::Pending && World->GetTimeSeconds() - Existing->StartedTime >= 2.0)
		{
			ReleaseCover(Enemy);
			return EFPCoverQueryResult::Failed;
		}
		if (Existing->State == EFPCoverQueryResult::Found) OutLocation = Existing->Location;
		return Existing->State;
	}
	if (CoverQueryBudgetFrame != GFrameCounter)
	{
		CoverQueryBudgetFrame = GFrameCounter;
		CoverQueriesStartedThisFrame = 0;
	}
	if (PendingCoverQueries >= FMath::Clamp(MaxPendingCoverQueries, 1, 8) ||
		CoverQueriesStartedThisFrame >= FMath::Clamp(MaxCoverQueriesPerFrame, 1, 4)) return EFPCoverQueryResult::Pending;
	UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(World);
	if (Queries == nullptr) return EFPCoverQueryResult::Failed;
	if (CoverQueryTemplate == nullptr) CoverQueryTemplate = FPCreateEnemyPositionQuery(*this, true);
	FCoverRequest& Cover = CoverRequests.Add(Key);
	Cover.ThreatLocation = KnownThreatLocation;
	Cover.StartedTime = World->GetTimeSeconds();
	Cover.Revision = ++NextCoverRevision;
	const FVector Origin = Enemy->GetActorLocation();
	FEnvQueryRequest Request(CoverQueryTemplate, Enemy);
	Request.SetFloatParam(TEXT("OriginX"), Origin.X).SetFloatParam(TEXT("OriginY"), Origin.Y).SetFloatParam(TEXT("OriginZ"), Origin.Z);
	Request.SetFloatParam(TEXT("ThreatX"), KnownThreatLocation.X).SetFloatParam(TEXT("ThreatY"), KnownThreatLocation.Y).SetFloatParam(TEXT("ThreatZ"), KnownThreatLocation.Z);
	Request.SetFloatParam(TEXT("Cover"), 1.0f);
	Request.SetFloatParam(TEXT("AgentRadius"), Enemy->GetCapsuleComponent()->GetScaledCapsuleRadius());
	Request.SetFloatParam(TEXT("AgentHeight"), Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	Request.SetFloatParam(TEXT("ProjectionX"), NavigationProjectionExtent.X).SetFloatParam(TEXT("ProjectionY"), NavigationProjectionExtent.Y).SetFloatParam(TEXT("ProjectionZ"), NavigationProjectionExtent.Z);
	Cover.QueryId = Request.Execute(EEnvQueryRunMode::AllMatching,
		FQueryFinishedSignature::CreateUObject(this, &ThisClass::HandleCoverQueryFinished, Key, Cover.Revision));
	if (Cover.QueryId == INDEX_NONE) Cover.State = EFPCoverQueryResult::Failed;
	else { ++PendingCoverQueries; ++CoverQueriesStartedThisFrame; }
	CSV_CUSTOM_STAT(fpstrueEQS, CoverQueries, 1, ECsvCustomStatOp::Accumulate);
	return Cover.State;
}

// 只认本次敌人的查询；候选已通过环境与路径检查，发布前再核对其他撤退敌人的占用。
void AfpstrueSurroundManager::HandleCoverQueryFinished(TSharedPtr<FEnvQueryResult> Result, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, uint32 Revision)
{
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	FCoverRequest* Cover = CoverRequests.Find(Enemy);
	if (Cover == nullptr || !Result.IsValid() || Result->QueryID != Cover->QueryId || Revision != Cover->Revision) return;
	Cover->QueryId = INDEX_NONE;
	PendingCoverQueries = FMath::Max(0, PendingCoverQueries - 1);
	Cover->State = EFPCoverQueryResult::Failed;
	if (!Enemy.IsValid() || Enemy->IsDead() || !Result->IsSuccessful()) return;
	for (int32 Index = 0; Index < Result->Items.Num(); ++Index)
	{
		if (!Result->Items[Index].IsValid()) continue;
		const FVector Location = Result->GetItemAsLocation(Index);
		bool bOccupied = Location.ContainsNaN();
		for (const auto& Pair : CoverRequests)
		{
			if (Pair.Key != Enemy && Pair.Key.IsValid() && !Pair.Key->IsDead() && Pair.Value.State == EFPCoverQueryResult::Found &&
				FVector::DistSquared2D(Pair.Value.Location, Location) < FMath::Square(MinPositionSpacing))
			{
				bOccupied = true;
				break;
			}
		}
		if (!bOccupied) { Cover->Location = Location; Cover->State = EFPCoverQueryResult::Found; break; }
	}
	CSV_CUSTOM_STAT(fpstrueEQS, CoverFound, Cover->State == EFPCoverQueryResult::Found ? 1 : 0, ECsvCustomStatOp::Accumulate);
}

// 删除登记先于 Abort，避免同步结束委托发布已被释放的掩体；重复释放无副作用。
void AfpstrueSurroundManager::ReleaseCover(AfpstrueEnemyCharacter* Enemy)
{
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	FCoverRequest Cover;
	if (Enemy == nullptr || !CoverRequests.RemoveAndCopyValue(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy), Cover)) return;
	if (Cover.QueryId != INDEX_NONE)
	{
		PendingCoverQueries = FMath::Max(0, PendingCoverQueries - 1);
		if (UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(GetWorld())) Queries->AbortQuery(Cover.QueryId);
	}
}

// 返回有效共享玩家位置；调用者仍须通过自己的感知状态决定是否允许消费该位置。
bool AfpstrueSurroundManager::GetSharedTargetSnapshot(FVector& OutLocation) const
{
	// AIController 读取同一份玩家位置快照，避免一批敌人在相邻时刻使用不同目标点。
	if (!HasAuthority() || GetNetMode() == NM_Client || !bHasSharedTargetSnapshot || !IsValid(TargetCharacter)
		|| TargetCharacter->IsDead() || TargetCharacter->IsActorBeingDestroyed())
	{
		return false;
	}

	OutLocation = CachedTargetLocation;
	return true;
}

// ==================== 稳定槽位的申请与释放 ====================

// 首次申请建立唯一请求，之后复用稳定席位；FIFO 决定准入先后，距离只决定同环选哪个空位。
bool AfpstrueSurroundManager::GetOrRequestTacticalAssignment(AfpstrueEnemyCharacter* Enemy, FFPEnemyTacticalAssignment& OutAssignment)
{
	if (!HasAuthority() || GetNetMode() == NM_Client || !IsValid(Enemy) || Enemy->IsDead()
		|| Enemy->IsActorBeingDestroyed() || Enemy->GetWorld() != GetWorld()
		|| !bHasSharedTargetSnapshot || !IsValid(TargetCharacter) || TargetCharacter->IsDead()) return false;
	const TWeakObjectPtr<AfpstrueEnemyCharacter> Key(Enemy);
	if (!Participants.Contains(Key))
	{
		FTacticalParticipant& Participant = Participants.Add(Key);
		Participant.RequestSequence = ++NextRequestSequence;
		Participant.AssignmentRevision = ++NextAssignmentRevision;
		Participant.LastProgressLocation = Enemy->GetActorLocation();
		Participant.LastProgressTime = GetWorld()->GetTimeSeconds();
		// 黄金角把同向涌入的候场者摊开；每个请求固定一个位置，队列前进不会让全员洗牌。
		const float Angle = static_cast<float>(Participant.RequestSequence % 256) * 2.39996323f;
		const float Radius = FMath::Max(OuterRadius, InnerRadius + 150.0f) + 180.0f
			+ static_cast<float>((Participant.RequestSequence / 32) % 4) * 140.0f;
		Participant.WaitingOffset = FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0f);
		WaitingQueue.Add({Key, Participant.RequestSequence});
		PumpTacticalQueue();
	}
	return BuildAssignment(Key, OutAssignment);
}

bool AfpstrueSurroundManager::BuildAssignment(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, FFPEnemyTacticalAssignment& OutAssignment) const
{
	const FTacticalParticipant* Participant = Participants.Find(Enemy);
	if (Participant == nullptr) return false;
	OutAssignment = {};
	OutAssignment.Revision = Participant->AssignmentRevision;
	OutAssignment.Goal = CachedTargetLocation + Participant->WaitingOffset;
	if (const int32* SlotIndex = EnemyToSlot.Find(Enemy))
	{
		if (FVector::DistSquared(CachedTargetLocation, PublishedTargetLocation)
			> FMath::Square(FMath::Max(25.0f, SharedTargetMoveThreshold))) return false;
		if (!SurroundSlots.IsValidIndex(*SlotIndex)) return false;
		const FfpstrueSurroundSlot& Slot = SurroundSlots[*SlotIndex];
		if (Slot.Occupant != Enemy || (Slot.RingIndex == 0 ? !Slot.bHasProjectedApproachLocation : !Slot.bHasProjectedSlotLocation)) return false;
		OutAssignment.Role = Slot.RingIndex == 0 ? EFPEnemyTacticalRole::Inner : EFPEnemyTacticalRole::Outer;
		OutAssignment.Goal = Slot.RingIndex == 0 ? Slot.ProjectedApproachLocation : Slot.ProjectedSlotLocation;
	}
	return !OutAssignment.Goal.ContainsNaN();
}

bool AfpstrueSurroundManager::HasInnerSlot(AfpstrueEnemyCharacter* Enemy, uint64 Revision) const
{
	if (!IsValid(Enemy) || Enemy->IsDead() || !bHasSharedTargetSnapshot || !IsValid(TargetCharacter) || TargetCharacter->IsDead()) return false;
	const FTacticalParticipant* Participant = Participants.Find(Enemy);
	const int32* SlotIndex = EnemyToSlot.Find(Enemy);
	return Participant != nullptr && (Revision == 0 || Revision == Participant->AssignmentRevision)
		&& SlotIndex != nullptr && SurroundSlots.IsValidIndex(*SlotIndex)
		&& SurroundSlots[*SlotIndex].RingIndex == 0 && SurroundSlots[*SlotIndex].Occupant == Enemy;
}

int32 AfpstrueSurroundManager::FindBestFreeSlot(const FVector& EnemyLocation, int32 RingIndex) const
{
	int32 BestSlotIndex = INDEX_NONE;
	float BestDistanceSquared = TNumericLimits<float>::Max();
	for (int32 SlotIndex = 0; SlotIndex < SurroundSlots.Num(); ++SlotIndex)
	{
		const FfpstrueSurroundSlot& Slot = SurroundSlots[SlotIndex];
		if (Slot.RingIndex != RingIndex || Slot.Occupant.IsValid()
			|| (RingIndex == 0 ? !Slot.bHasProjectedApproachLocation : !Slot.bHasProjectedSlotLocation)) continue;
		const FVector& Goal = RingIndex == 0 ? Slot.ProjectedApproachLocation : Slot.ProjectedSlotLocation;
		const float DistanceSquared = FVector::DistSquared2D(EnemyLocation, Goal);
		if (DistanceSquared < BestDistanceSquared) { BestDistanceSquared = DistanceSquared; BestSlotIndex = SlotIndex; }
	}
	return BestSlotIndex;
}

void AfpstrueSurroundManager::AssignSlot(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy, int32 SlotIndex)
{
	if (const int32* Previous = EnemyToSlot.Find(Enemy))
		if (SurroundSlots.IsValidIndex(*Previous) && SurroundSlots[*Previous].Occupant == Enemy) SurroundSlots[*Previous].Occupant.Reset();
	SurroundSlots[SlotIndex].Occupant = Enemy;
	EnemyToSlot.Add(Enemy, SlotIndex);
	FTacticalParticipant& Participant = Participants.FindChecked(Enemy);
	Participant.AssignmentRevision = ++NextAssignmentRevision;
	Participant.LastProgressLocation = Enemy->GetActorLocation();
	Participant.LastProgressTime = GetWorld()->GetTimeSeconds();
	if (SurroundSlots[SlotIndex].RingIndex == 0) Participant.RequestSequence = 0;
	++TacticalGrantCount;
	PendingTacticalNotifications.Add(Enemy);
}

// 同一服务器 Game Thread 内先提交全部账本，再发“重新决策”通知；通知不直接启动攻击。
void AfpstrueSurroundManager::PumpTacticalQueue()
{
	if (bPumpingTacticalQueue || !bHasSharedTargetSnapshot || !IsValid(TargetCharacter) || TargetCharacter->IsDead()) return;
	bPumpingTacticalQueue = true;
	const double Now = GetWorld()->GetTimeSeconds();
	const bool bPositionsCurrent = FVector::DistSquared(CachedTargetLocation, PublishedTargetLocation)
		<= FMath::Square(FMath::Max(25.0f, SharedTargetMoveThreshold));
	bool bInnerHasRoom = bPositionsCurrent;
	for (int32 Index = 0; Index < WaitingQueue.Num();)
	{
		const FTacticalRequest Request = WaitingQueue[Index];
		FTacticalParticipant* Participant = Participants.Find(Request.Enemy);
		if (Participant == nullptr || Participant->RequestSequence != Request.Sequence)
		{
			WaitingQueue.RemoveAt(Index);
			continue;
		}
		if (!Request.Enemy.IsValid() || Request.Enemy->IsDead() || Request.Enemy->IsActorBeingDestroyed())
		{
			// 使用原 weak 键清理，不把 Get()==nullptr 变成另一把空键。
			RemoveParticipant(Request.Enemy);
			continue;
		}
		const int32 InnerSlot = bInnerHasRoom && Now >= Participant->RetryAfter
			? FindBestFreeSlot(Request.Enemy->GetActorLocation(), 0) : INDEX_NONE;
		if (InnerSlot != INDEX_NONE)
		{
			AssignSlot(Request.Enemy, InnerSlot);
			WaitingQueue.RemoveAt(Index);
			continue;
		}
		if (Now >= Participant->RetryAfter) bInnerHasRoom = false;
		++Index;
	}
	// 晋升先释放外环，随后按同一 FIFO 为无槽位者补外环；已有外环身份不洗牌。
	if (bPositionsCurrent)
		for (const FTacticalRequest& Request : WaitingQueue)
			if (!EnemyToSlot.Contains(Request.Enemy) && Now >= Participants.FindChecked(Request.Enemy).RetryAfter)
			{
				const int32 OuterSlot = FindBestFreeSlot(Request.Enemy->GetActorLocation(), 1);
				if (OuterSlot == INDEX_NONE) break;
				AssignSlot(Request.Enemy, OuterSlot);
			}
	bPumpingTacticalQueue = false;
	NotifyTacticalChanges();
}

void AfpstrueSurroundManager::NotifyTacticalChanges()
{
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> Notifications = MoveTemp(PendingTacticalNotifications);
	PendingTacticalNotifications.Reset();
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& Enemy : Notifications)
		if (Enemy.IsValid())
			if (AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
				Controller->OnTacticalAssignmentChanged(this);
}

void AfpstrueSurroundManager::RemoveParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy)
{
	ActiveAttackers.Remove(Enemy);
	const bool bRemoved = Participants.Remove(Enemy) > 0;
	WaitingQueue.RemoveAll([Enemy](const FTacticalRequest& Request) { return Request.Enemy == Enemy; });
	int32 SlotIndex = INDEX_NONE;
	if (EnemyToSlot.RemoveAndCopyValue(Enemy, SlotIndex)
		&& SurroundSlots.IsValidIndex(SlotIndex) && SurroundSlots[SlotIndex].Occupant == Enemy)
	{
		// ★【重要边界】[资源归还] 旧映射不能清掉已经由另一敌人占据的同一下标。
		SurroundSlots[SlotIndex].Occupant.Reset();
	}
	if (bRemoved) { ++TacticalReleaseCount; PendingTacticalNotifications.Add(Enemy); }
}

void AfpstrueSurroundManager::ReleaseSurroundSlot(AfpstrueEnemyCharacter* Enemy)
{
	if (!HasAuthority() || GetNetMode() == NM_Client || Enemy == nullptr) return;
	RemoveParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	PumpTacticalQueue();
	NotifyTacticalChanges();
}

void AfpstrueSurroundManager::RequeueParticipant(TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy)
{
	int32 SlotIndex = INDEX_NONE;
	if (EnemyToSlot.RemoveAndCopyValue(Enemy, SlotIndex)
		&& SurroundSlots.IsValidIndex(SlotIndex) && SurroundSlots[SlotIndex].Occupant == Enemy)
		SurroundSlots[SlotIndex].Occupant.Reset();
	ActiveAttackers.Remove(Enemy);
	WaitingQueue.RemoveAll([Enemy](const FTacticalRequest& Request) { return Request.Enemy == Enemy; });
	FTacticalParticipant& Participant = Participants.FindChecked(Enemy);
	Participant.RequestSequence = ++NextRequestSequence;
	Participant.AssignmentRevision = ++NextAssignmentRevision;
	Participant.LastProgressTime = GetWorld()->GetTimeSeconds();
	Participant.RetryAfter = Participant.LastProgressTime + 1.0;
	WaitingQueue.Add({Enemy, Participant.RequestSequence});
	PendingTacticalNotifications.Add(Enemy);
	++TacticalReleaseCount;
}

void AfpstrueSurroundManager::ReportTacticalMoveFailure(AfpstrueEnemyCharacter* Enemy, uint64 Revision)
{
	if (!HasAuthority() || GetNetMode() == NM_Client) return;
	const FTacticalParticipant* Participant = Participants.Find(Enemy);
	if (Participant == nullptr || Participant->AssignmentRevision != Revision || !EnemyToSlot.Contains(Enemy)) return;
	RequeueParticipant(Enemy);
	PumpTacticalQueue();
}

float AfpstrueSurroundManager::GetInnerBoundary(AfpstrueEnemyCharacter* Enemy) const
{
	return InnerRadius + Enemy->GetCapsuleComponent()->GetScaledCapsuleRadius() + 10.0f;
}

bool AfpstrueSurroundManager::ResolveTacticalMoveGoal(AfpstrueEnemyCharacter* Enemy,
	const FFPEnemyTacticalAssignment& Assignment, FVector& OutGoal) const
{
	FFPEnemyTacticalAssignment Current;
	if (!IsValid(Enemy) || !BuildAssignment(Enemy, Current) || Current.Revision != Assignment.Revision) return false;
	OutGoal = Current.Goal;
	if (Current.Role == EFPEnemyTacticalRole::Inner) return true;
	const float Boundary = GetInnerBoundary(Enemy);
	FVector From = Enemy->GetActorLocation() - CachedTargetLocation;
	FVector To = OutGoal - CachedTargetLocation;
	From.Z = To.Z = 0.0f;
	if (To.SizeSquared() < FMath::Square(Boundary)) return false;
	if (From.SizeSquared() < FMath::Square(Boundary))
	{
		// 被动进入内圈后先沿当前半径向外恢复；不瞬移，也不让路径横穿玩家。
		const FVector Direction = From.IsNearlyZero() ? To.GetSafeNormal() : From.GetSafeNormal();
		OutGoal = CachedTargetLocation + Direction * (Boundary + 100.0f);
		OutGoal.Z = Current.Goal.Z;
		return true;
	}
	const FVector Segment = To - From;
	const float Alpha = Segment.SizeSquared() > SMALL_NUMBER ? FMath::Clamp(-FVector::DotProduct(From, Segment) / Segment.SizeSquared(), 0.0f, 1.0f) : 0.0f;
	if ((From + Alpha * Segment).SizeSquared() >= FMath::Square(Boundary)) return true;
	// 直接弦会穿圈：下一段沿外侧圆弧走最多 30 度，后续决策继续消费原席位。
	const float FromAngle = FMath::Atan2(From.Y, From.X);
	const float Delta = FMath::FindDeltaAngleRadians(FromAngle, FMath::Atan2(To.Y, To.X));
	const float Step = FMath::Clamp(Delta, -PI / 6.0f, PI / 6.0f);
	const float Radius = FMath::Max(FMath::Max(From.Size(), To.Size()), (Boundary + 20.0f) / FMath::Cos(PI / 12.0f));
	const float NextAngle = From.Size() < Radius - 5.0f ? FromAngle : FromAngle + Step;
	OutGoal = CachedTargetLocation + FVector(FMath::Cos(NextAngle) * Radius, FMath::Sin(NextAngle) * Radius, 0.0f);
	OutGoal.Z = Current.Goal.Z;
	return true;
}

bool AfpstrueSurroundManager::IsTacticalPathAllowed(AfpstrueEnemyCharacter* Enemy, const FNavigationPath& Path, uint32 FirstPoint) const
{
	if (!IsValid(Enemy) || !Participants.Contains(Enemy)) return false;
	if (HasInnerSlot(Enemy)) return true;
	const float Boundary = GetInnerBoundary(Enemy);
	FVector From = Enemy->GetActorLocation() - CachedTargetLocation;
	From.Z = 0.0f;
	const TArray<FNavPathPoint>& Points = Path.GetPathPoints();
	for (uint32 Index = FirstPoint; Index < static_cast<uint32>(Points.Num()); ++Index)
	{
		FVector To = Points[Index].Location - CachedTargetLocation;
		To.Z = 0.0f;
		const FVector Segment = To - From;
		if (From.SizeSquared() < FMath::Square(Boundary - 1.0f))
		{
			if (FVector::DotProduct(From, Segment) < -1.0f || To.SizeSquared() + 1.0f < From.SizeSquared()) return false;
		}
		else
		{
			const float Alpha = Segment.SizeSquared() > SMALL_NUMBER ? FMath::Clamp(-FVector::DotProduct(From, Segment) / Segment.SizeSquared(), 0.0f, 1.0f) : 0.0f;
			if ((From + Alpha * Segment).SizeSquared() < FMath::Square(Boundary - 1.0f)) return false;
		}
		From = To;
	}
	return true;
}

FFPEnemyTacticalDebugSnapshot AfpstrueSurroundManager::GetTacticalDebugSnapshot() const
{
	FFPEnemyTacticalDebugSnapshot Snapshot;
	for (const FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		const bool bInner = Slot.RingIndex == 0;
		(bInner ? Snapshot.InnerCapacity : Snapshot.OuterCapacity)++;
		if (bInner ? Slot.bHasProjectedApproachLocation : Slot.bHasProjectedSlotLocation)
			(bInner ? Snapshot.UsableInnerSlots : Snapshot.UsableOuterSlots)++;
		if (Slot.Occupant.IsValid() && !Slot.Occupant->IsDead())
			(bInner ? Snapshot.OccupiedInnerSlots : Snapshot.OccupiedOuterSlots)++;
	}
	Snapshot.QueuedEnemies = WaitingQueue.Num();
	Snapshot.ActiveAttackers = ActiveAttackers.Num();
	Snapshot.GrantCount = TacticalGrantCount;
	Snapshot.ReleaseCount = TacticalReleaseCount;
	Snapshot.TimeoutCount = TacticalTimeoutCount;
	return Snapshot;
}

// ==================== 攻击与 MoveTo 全局预算 ====================

// 申请有界并发攻击名额；已有持有者可重查，容量不足返回失败，不关闭敌人的追击行为。
bool AfpstrueSurroundManager::TryAcquireAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	// 当前是容量限制而非 Top-N 排名：名额未满时先申请先获得，攻击结束后由 Controller/CombatComponent 归还。
	if (!HasAuthority() || GetNetMode() == NM_Client || !HasInnerSlot(Enemy))
	{
		return false;
	}

	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (!bEnableActiveAttackerBudget || BenchmarkConfig.bDisableActiveAttackerBudget)
	{
		return true;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (ActiveAttackers.Contains(EnemyKey))
	{
		return true;
	}

	const int32 AttackLimit = FMath::Max(MaxActiveAttackers, 1);
	if (ActiveAttackers.Num() >= AttackLimit)
	{
		// 只有预算确实满时才扫描弱引用，避免每次攻击申请都遍历两张表。
		CleanupInvalidEntries();
		if (!HasInnerSlot(Enemy) || ActiveAttackers.Num() >= AttackLimit)
		{
			return false;
		}
	}

	// 预算只保护“攻击事务”，没有拿到名额的敌人仍保留槽位并继续追踪。
	// ★【重要边界】[状态提交] 攻击许可用唯一敌人集合登记，已有持有者重查不重复占用，容量不足者不能开始攻击。
	ActiveAttackers.Add(EnemyKey);
	return true;
}

// 移除唯一攻击者登记，结束、死亡和中断重复归还均安全。
void AfpstrueSurroundManager::ReleaseAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	// TSet::Remove 使重复归还天然幂等，不需要另外维护“已经释放”标志。
	if (HasAuthority() && GetNetMode() != NM_Client && Enemy != nullptr)
	{
		ActiveAttackers.Remove(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	}
}

// 消费本帧的新 MoveTo 提交预算；普通追击让出战斗预留额度，拒绝不表示路径不可达。
bool AfpstrueSurroundManager::TryConsumeMoveRequestBudget(bool bCombatPriority)
{
	// GFrameCounter 形成无额外 Tick 的帧级计数器；战斗请求可使用预留额度，普通追击不能挤占全部预算。
	if (!HasAuthority() || GetNetMode() == NM_Client || GetWorld() == nullptr) return false;
	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (!bEnableMoveRequestBudget || BenchmarkConfig.bDisableMoveToRequestBudget)
	{
		return true;
	}

	FWorldMoveRequestBudget& Budget = WorldMoveRequestBudgets.FindOrAdd(GetWorld());
	Budget.Managers.Add(this);
	if (Budget.Frame != GFrameCounter)
	{
		Budget.Frame = GFrameCounter;
		Budget.Consumed = 0;
		Budget.Maximum = MAX_int32;
		Budget.ReservedCombat = 0;
		for (auto It = Budget.Managers.CreateIterator(); It; ++It)
		{
			const AfpstrueSurroundManager* Manager = It->Get();
			if (!IsValid(Manager) || Manager->IsActorBeingDestroyed()) { It.RemoveCurrent(); continue; }
			if (!Manager->bEnableMoveRequestBudget) continue;
			// 不同目标使用不同配置时也不相加；取最紧总额与最大预留，保持整 World 的确定上限。
			Budget.Maximum = FMath::Min(Budget.Maximum, FMath::Max(Manager->MaxMoveRequestsPerFrame, 1));
			Budget.ReservedCombat = FMath::Max(Budget.ReservedCombat, Manager->ReservedCombatMoveRequestsPerFrame);
		}
		if (Budget.Maximum == MAX_int32) Budget.Maximum = FMath::Max(MaxMoveRequestsPerFrame, 1);
		Budget.ReservedCombat = FMath::Clamp(Budget.ReservedCombat, 0, Budget.Maximum - 1);
	}
	const int32 RequestLimit = bCombatPriority ? Budget.Maximum : Budget.Maximum - Budget.ReservedCombat;
	if (Budget.Consumed >= RequestLimit)
	{
		return false;
	}

	// 近战槽位请求可以使用预留额度，远距离追踪不会挤占整帧的导航提交。
	++Budget.Consumed;
	return true;
}

// ==================== 清理、槽位选择与调试 ====================
// 槽位选择与补位算法紧邻申请/释放入口；此处集中清理与调试。

// 对局结束或更换目标时统一清空跨敌人资源并停掉共享刷新，允许之后重新注入目标。
void AfpstrueSurroundManager::ResetManager()
{
	// 对局结束时一次性归零槽位、攻击名额、帧预算和目标缓存，Manager 可安全退出或重新初始化。
	// ★【重要边界】[生命周期/异步身份] 清空身份与登记后再 Abort；回调不能恢复已经结束的世界状态。
	const int32 OldApproach = ApproachQueryId;
	ApproachQueryId = INDEX_NONE;
	++PositionQueryRevision;
	TArray<int32, TInlineAllocator<8>> OldCoverQueries;
	for (const auto& Pair : CoverRequests) if (Pair.Value.QueryId != INDEX_NONE) OldCoverQueries.Add(Pair.Value.QueryId);
	CoverRequests.Reset();
	PendingCoverQueries = 0;
	CoverQueryBudgetFrame = MAX_uint64;
	CoverQueriesStartedThisFrame = 0;
	if (UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(GetWorld()))
	{
		if (OldApproach != INDEX_NONE) Queries->AbortQuery(OldApproach);
		for (const int32 QueryId : OldCoverQueries) Queries->AbortQuery(QueryId);
	}
	PublishedTargetLocation = FVector::ZeroVector;
	for (const auto& Pair : Participants) PendingTacticalNotifications.Add(Pair.Key);
	Participants.Reset();
	WaitingQueue.Reset();
	EnemyToSlot.Reset();
	ActiveAttackers.Reset();
	TargetCharacter = nullptr;
	bHasSharedTargetSnapshot = false;
	bNeedsNavigationRetry = false;
	SharedTargetSnapshotTime = 0.0;
	NextNavigationRetryTime = 0.0;
	NavigationCacheTime = 0.0;
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.Occupant.Reset();
		Slot.bHasProjectedSlotLocation = false;
		Slot.bHasProjectedApproachLocation = false;
	}
	GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
	NotifyTacticalChanges();
}

// 删除死亡或失效弱引用，并核对槽位身份后清占用，避免旧登记误清新占用者。
void AfpstrueSurroundManager::CleanupInvalidEntries()
{
	// 低频维护只看已登记参与者；没有逐 AI Tick，也不会把候场等同于暂停感知/伤害。
	const double Now = GetWorld()->GetTimeSeconds();
	TArray<TWeakObjectPtr<AfpstrueEnemyCharacter>, TInlineAllocator<16>> Removed;
	for (auto& Pair : Participants)
	{
		AfpstrueEnemyCharacter* Enemy = Pair.Key.Get();
		if (!IsValid(Enemy) || Enemy->IsDead() || Enemy->IsActorBeingDestroyed()) { Removed.Add(Pair.Key); continue; }
		FTacticalParticipant& Participant = Pair.Value;
		if (!EnemyToSlot.Contains(Pair.Key)) continue;
		FFPEnemyTacticalAssignment Assignment;
		const bool bValidGoal = BuildAssignment(Pair.Key, Assignment);
		const FVector Location = Enemy->GetActorLocation();
		const bool bAtGoal = bValidGoal && FVector::DistSquared2D(Location, Assignment.Goal) <= FMath::Square(125.0f);
		const bool bHasProgress = bValidGoal && FVector::DistSquared2D(Location, Assignment.Goal) + FMath::Square(25.0f)
			< FVector::DistSquared2D(Participant.LastProgressLocation, Assignment.Goal);
		if (bAtGoal || bHasProgress || Enemy->IsAttacking())
		{
			Participant.LastProgressLocation = Location;
			Participant.LastProgressTime = Now;
		}
		else if (Now - Participant.LastProgressTime >= FMath::Max(1.0f, SlotApproachTimeout))
		{
			RequeueParticipant(Pair.Key);
			++TacticalTimeoutCount;
		}
	}
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& Enemy : Removed) RemoveParticipant(Enemy);
}

// 把相对极角和半径换算为世界位置；可选半径用于同角度的攻击接近点。
	// 使用缓存玩家位置、槽位极角和半径计算二维世界坐标；随后由批量 NavMesh 投影修正可达性。
// 以上原始坐标计算说明保留；对应实现集中到 EQS Generator，调试只绘制已发布的结果。

// 调试时绘制有效投影槽位，以青色和绿色区分占用与空闲；不参与正式决策。
void AfpstrueSurroundManager::DrawDebugSlots()
{
	// 仅开发调试使用：绿色为空位、青色为占用位，不参与正式站位决策。
	if (!bDrawDebugSlots || !IsValid(TargetCharacter))
	{
		return;
	}

	CleanupInvalidEntries();

	for (const FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		if (!Slot.bHasProjectedSlotLocation)
		{
			continue;
		}

		const FColor SlotColor = Slot.Occupant.IsValid() ? FColor::Cyan : FColor::Green;

		DrawDebugSphere(GetWorld(), Slot.ProjectedSlotLocation, 24.0f, 12, SlotColor, false, DebugDrawInterval * 1.25f, 0, 2.0f);
	}
}
