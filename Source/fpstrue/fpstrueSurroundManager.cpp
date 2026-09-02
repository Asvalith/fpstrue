// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueSurroundManager.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "DrawDebugHelpers.h"
#include "EngineGlobals.h"
#include "Engine/World.h"
#include "NavigationSystem.h"

/*
 * 多敌人共享的群体协调器。
 * 它不替代单个 AIController 的状态机，只集中维护必须全局一致的资源：玩家位置快照、稳定槽位、
 * 并发攻击名额和每帧 MoveTo 提交预算，从而减少围攻拥堵和重复导航请求。
 *
 * 数据结构职责：
 *   TArray<Slot> 保存固定内外环布局；TMap<Enemy, Index> 保存稳定占位；TSet<Enemy> 保存攻击许可。
 * 敌人键使用 TWeakObjectPtr，Manager 不拥有敌人生命周期；死亡/销毁由主动释放和失效项清理双重兜底。
 * 玩家位置与槽位 NavMesh 投影按 Timer/位移阈值批量刷新，单个 AIController 只读取缓存结果。
 */

// ==================== 生命周期与目标注入 ====================

AfpstrueSurroundManager::AfpstrueSurroundManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AfpstrueSurroundManager::BeginPlay()
{
	Super::BeginPlay();
	BuildSlots();

	if (bDrawDebugSlots)
	{
		GetWorldTimerManager().SetTimer(DebugDrawTimerHandle, this, &AfpstrueSurroundManager::DrawDebugSlots, DebugDrawInterval, true);
	}
}

void AfpstrueSurroundManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(DebugDrawTimerHandle);
	ResetManager();
	Super::EndPlay(EndPlayReason);
}

void AfpstrueSurroundManager::SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter)
{
	// GameMode 在开局注入唯一玩家；目标变化时重建快照并重新启动共享刷新 Timer。
	if (TargetCharacter == NewTargetCharacter && bHasSharedTargetSnapshot)
	{
		return;
	}

	TargetCharacter = NewTargetCharacter;
	if (!IsValid(TargetCharacter))
	{
		bHasSharedTargetSnapshot = false;
		GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
		return;
	}

	UpdateSharedTargetSnapshot(true);
	GetWorldTimerManager().SetTimer(SharedTargetTimerHandle, this, &AfpstrueSurroundManager::RefreshSharedTargetSnapshot,
									FMath::Max(SharedTargetRefreshInterval, 0.05f), true);
}

// ==================== 共享目标与 NavMesh 投影缓存 ====================

void AfpstrueSurroundManager::RefreshSharedTargetSnapshot()
{
	UpdateSharedTargetSnapshot(false);
}

void AfpstrueSurroundManager::UpdateSharedTargetSnapshot(bool bForce)
{
	// 位移未达到阈值时复用旧目标和旧投影，避免每个敌人每次决策都查询 NavMesh。
	if (!IsValid(TargetCharacter))
	{
		return;
	}

	const FVector CurrentTargetLocation = TargetCharacter->GetActorLocation();
	const bool bMovedEnough =
		FVector::DistSquared2D(CurrentTargetLocation, CachedTargetLocation) >= FMath::Square(SharedTargetMoveThreshold);
	if (!bForce && bHasSharedTargetSnapshot && !bMovedEnough)
	{
		return;
	}

	CachedTargetLocation = CurrentTargetLocation;
	bHasSharedTargetSnapshot = true;
	RebuildProjectedSlotCache();
}

void AfpstrueSurroundManager::BuildSlots()
{
	// 槽位只保存相对角度和半径；世界位置基于最新玩家快照统一计算，保证所有敌人使用同一参考系。
	SurroundSlots.Reset();

	const auto AddRing = [this](int32 RingIndex, int32 SlotCount, float Radius)
	{
		const int32 SafeSlotCount = FMath::Max(SlotCount, 1);
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

void AfpstrueSurroundManager::RebuildProjectedSlotCache()
{
	// 同时缓存“站位点”和更靠近玩家的“攻击接近点”，AI 决策阶段不再重复 ProjectPointToNavigation。
	if (!bHasSharedTargetSnapshot)
	{
		return;
	}

	const UWorld* World = GetWorld();
	const UNavigationSystemV1* NavigationSystem =
		World != nullptr ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	const auto ProjectLocation = [this, NavigationSystem](const FVector& RawLocation, FVector& OutLocation)
	{
		if (NavigationSystem == nullptr)
		{
			return false;
		}

		FNavLocation ProjectedLocation;
		if (!NavigationSystem->ProjectPointToNavigation(RawLocation, ProjectedLocation, NavigationProjectionExtent))
		{
			return false;
		}

		OutLocation = ProjectedLocation.Location;
		return true;
	};

	// 目标移动超过阈值时集中投影一次；AI 决策阶段只读取缓存结果。
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.bHasProjectedSlotLocation = ProjectLocation(CalculateRawSlotLocation(Slot), Slot.ProjectedSlotLocation);

		const float ApproachRadius = Slot.RingIndex == 0 ? AttackApproachRadius : OuterAttackApproachRadius;
		Slot.bHasProjectedApproachLocation =
			ProjectLocation(CalculateRawSlotLocation(Slot, ApproachRadius), Slot.ProjectedApproachLocation);
	}
}

// ==================== 稳定槽位的申请与释放 ====================

bool AfpstrueSurroundManager::RequestSurroundSlot(AfpstrueEnemyCharacter* Enemy)
{
	// 已分配的敌人直接复用原槽位；只有首次申请才寻找空位，避免每轮决策改变目标导致来回晃动。
	if (!IsValid(Enemy) || !IsValid(TargetCharacter))
	{
		return false;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (EnemyToSlot.Contains(EnemyKey))
	{
		return true;
	}

	if (EnemyToSlot.Num() >= SurroundSlots.Num())
	{
		CleanupInvalidEntries();
		if (EnemyToSlot.Num() >= SurroundSlots.Num())
		{
			return false;
		}
	}

	const int32 BestSlotIndex = FindBestFreeSlot(Enemy->GetActorLocation());
	if (!SurroundSlots.IsValidIndex(BestSlotIndex))
	{
		return false;
	}

	SurroundSlots[BestSlotIndex].Occupant = Enemy;
	EnemyToSlot.Add(EnemyKey, BestSlotIndex);
	return true;
}

void AfpstrueSurroundManager::ReleaseSurroundSlot(AfpstrueEnemyCharacter* Enemy)
{
	if (!IsValid(Enemy))
	{
		return;
	}
	ReleaseAttackPermission(Enemy);

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	int32* SlotIndexPtr = EnemyToSlot.Find(EnemyKey);
	if (SlotIndexPtr == nullptr)
	{
		return;
	}

	const int32 ReleasedSlotIndex = *SlotIndexPtr;
	const bool bReleasedInnerSlot = SurroundSlots.IsValidIndex(ReleasedSlotIndex) && SurroundSlots[ReleasedSlotIndex].RingIndex == 0;

	if (SurroundSlots.IsValidIndex(ReleasedSlotIndex))
	{
		SurroundSlots[ReleasedSlotIndex].Occupant.Reset();
	}
	EnemyToSlot.Remove(EnemyKey);

	if (bReleasedInnerSlot)
	{
		PromoteOuterOccupantToInnerSlot(ReleasedSlotIndex);
	}
}

// ==================== 攻击与 MoveTo 全局预算 ====================

bool AfpstrueSurroundManager::TryAcquireAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	// 当前是容量限制而非 Top-N 排名：名额未满时先申请先获得，攻击结束后由 Controller/CombatComponent 归还。
	if (!IsValid(Enemy) || Enemy->IsDead())
	{
		return false;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
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
		if (ActiveAttackers.Num() >= AttackLimit)
		{
			return false;
		}
	}

	// 预算只保护“攻击事务”，没有拿到名额的敌人仍保留槽位并继续追踪。
	ActiveAttackers.Add(EnemyKey);
	return true;
}

void AfpstrueSurroundManager::ReleaseAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	if (Enemy != nullptr)
	{
		ActiveAttackers.Remove(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	}
}

bool AfpstrueSurroundManager::TryConsumeMoveRequestBudget(bool bCombatPriority)
{
	// GFrameCounter 形成无额外 Tick 的帧级计数器；战斗请求可使用预留额度，普通追击不能挤占全部预算。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!bEnableMoveRequestBudget || BenchmarkConfig.bDisableMoveToRequestBudget)
	{
		return true;
	}

	if (MoveRequestBudgetFrame != GFrameCounter)
	{
		MoveRequestBudgetFrame = GFrameCounter;
		MoveRequestsConsumedThisFrame = 0;
	}

	const int32 SafeMaximum = FMath::Max(MaxMoveRequestsPerFrame, 1);
	const int32 SafeReservedCombat = FMath::Clamp(ReservedCombatMoveRequestsPerFrame, 0, SafeMaximum - 1);
	const int32 RequestLimit = bCombatPriority ? SafeMaximum : SafeMaximum - SafeReservedCombat;
	if (MoveRequestsConsumedThisFrame >= RequestLimit)
	{
		return false;
	}

	// 近战槽位请求可以使用预留额度，远距离追踪不会挤占整帧的导航提交。
	++MoveRequestsConsumedThisFrame;
	return true;
}

// 槽位位置和攻击接近点都来自同一缓存，避免 AIController 在每次决策中重复导航投影。
bool AfpstrueSurroundManager::GetOrAssignAttackApproachLocation(AfpstrueEnemyCharacter* Enemy, FVector& OutLocation)
{
	if (!IsValid(Enemy) || !IsValid(TargetCharacter))
	{
		return false;
	}
	if (!RequestSurroundSlot(Enemy))
	{
		return false;
	}
	const int32* SlotIndexPtr = EnemyToSlot.Find(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	if (SlotIndexPtr == nullptr || !SurroundSlots.IsValidIndex(*SlotIndexPtr))
	{
		return false;
	}

	const FfpstrueSurroundSlot& Slot = SurroundSlots[*SlotIndexPtr];
	if (!Slot.bHasProjectedApproachLocation)
	{
		return false;
	}

	// 槽位和共享追踪使用同一份目标快照，避免同一决策周期混用实时位置与缓存位置。
	OutLocation = Slot.ProjectedApproachLocation;
	return true;
}

bool AfpstrueSurroundManager::GetSharedTargetSnapshot(FVector& OutLocation) const
{
	if (!bHasSharedTargetSnapshot)
	{
		return false;
	}

	OutLocation = CachedTargetLocation;
	return true;
}

// ==================== 清理、槽位选择与调试 ====================

void AfpstrueSurroundManager::ResetManager()
{
	EnemyToSlot.Reset();
	ActiveAttackers.Reset();
	MoveRequestBudgetFrame = MAX_uint64;
	MoveRequestsConsumedThisFrame = 0;
	TargetCharacter = nullptr;
	bHasSharedTargetSnapshot = false;
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.Occupant.Reset();
		Slot.bHasProjectedSlotLocation = false;
		Slot.bHasProjectedApproachLocation = false;
	}
	GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
}

void AfpstrueSurroundManager::CleanupInvalidEntries()
{
	// 弱引用不会阻止 Actor 销毁；本函数只清掉已失效键，并同步清空对应槽位占用。
	for (auto Iterator = EnemyToSlot.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Key().IsValid() && !Iterator.Key()->IsDead())
		{
			continue;
		}

		const int32 SlotIndex = Iterator.Value();
		if (SurroundSlots.IsValidIndex(SlotIndex))
		{
			SurroundSlots[SlotIndex].Occupant.Reset();
		}
		Iterator.RemoveCurrent();
	}

	for (auto Iterator = ActiveAttackers.CreateIterator(); Iterator; ++Iterator)
	{
		const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey = *Iterator;
		if (!EnemyKey.IsValid() || EnemyKey->IsDead())
		{
			Iterator.RemoveCurrent();
		}
	}
}

int32 AfpstrueSurroundManager::FindBestFreeSlot(const FVector& EnemyLocation)
{
	for (int32 RingIndex = 0; RingIndex <= 1; ++RingIndex)
	{
		int32 BestSlotIndex = INDEX_NONE;
		float BestDistanceSquared = TNumericLimits<float>::Max();

		for (int32 SlotIndex = 0; SlotIndex < SurroundSlots.Num(); ++SlotIndex)
		{
			const FfpstrueSurroundSlot& Slot = SurroundSlots[SlotIndex];
			if (Slot.RingIndex != RingIndex || Slot.Occupant.IsValid())
			{
				continue;
			}

			if (!Slot.bHasProjectedSlotLocation)
			{
				continue;
			}

			const float DistanceSquared = FVector::DistSquared2D(EnemyLocation, Slot.ProjectedSlotLocation);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestSlotIndex = SlotIndex;
			}
		}

		if (BestSlotIndex != INDEX_NONE)
		{
			return BestSlotIndex;
		}
	}

	return INDEX_NONE;
}

void AfpstrueSurroundManager::PromoteOuterOccupantToInnerSlot(int32 InnerSlotIndex)
{
	if (!SurroundSlots.IsValidIndex(InnerSlotIndex) || SurroundSlots[InnerSlotIndex].RingIndex != 0 ||
		SurroundSlots[InnerSlotIndex].Occupant.IsValid())
	{
		return;
	}

	const FfpstrueSurroundSlot& InnerSlot = SurroundSlots[InnerSlotIndex];
	const FVector InnerLocation =
		InnerSlot.bHasProjectedSlotLocation ? InnerSlot.ProjectedSlotLocation : CalculateRawSlotLocation(InnerSlot);
	int32 BestOuterSlotIndex = INDEX_NONE;
	float BestDistanceSquared = TNumericLimits<float>::Max();

	for (int32 SlotIndex = 0; SlotIndex < SurroundSlots.Num(); ++SlotIndex)
	{
		const FfpstrueSurroundSlot& Slot = SurroundSlots[SlotIndex];
		if (Slot.RingIndex == 0 || !Slot.Occupant.IsValid())
		{
			continue;
		}

		const float DistanceSquared = FVector::DistSquared2D(Slot.Occupant->GetActorLocation(), InnerLocation);
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			BestOuterSlotIndex = SlotIndex;
		}
	}

	if (!SurroundSlots.IsValidIndex(BestOuterSlotIndex))
	{
		return;
	}

	TWeakObjectPtr<AfpstrueEnemyCharacter> PromotedEnemy = SurroundSlots[BestOuterSlotIndex].Occupant;
	SurroundSlots[BestOuterSlotIndex].Occupant.Reset();
	SurroundSlots[InnerSlotIndex].Occupant = PromotedEnemy;
	EnemyToSlot.FindOrAdd(PromotedEnemy) = InnerSlotIndex;
}

FVector AfpstrueSurroundManager::CalculateRawSlotLocation(const FfpstrueSurroundSlot& Slot, float RadiusOverride) const
{
	if (!bHasSharedTargetSnapshot)
	{
		return GetActorLocation();
	}

	const float Radius = RadiusOverride >= 0.0f ? RadiusOverride : Slot.Radius;
	const FVector Direction(FMath::Cos(Slot.AngleRadians), FMath::Sin(Slot.AngleRadians), 0.0f);
	return CachedTargetLocation + Direction * Radius;
}

void AfpstrueSurroundManager::DrawDebugSlots()
{
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
