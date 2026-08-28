// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueSurroundManager.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "DrawDebugHelpers.h"
#include "EngineGlobals.h"
#include "Engine/World.h"
#include "NavigationSystem.h"

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
	GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
	ResetManager();
	Super::EndPlay(EndPlayReason);
}

void AfpstrueSurroundManager::SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter)
{
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

void AfpstrueSurroundManager::RefreshSharedTargetSnapshot()
{
	UpdateSharedTargetSnapshot(false);
}

void AfpstrueSurroundManager::UpdateSharedTargetSnapshot(bool bForce)
{
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
	++SharedTargetGeneration;
	RebuildProjectedSlotCache();
}

void AfpstrueSurroundManager::BuildSlots()
{
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
	EnsureProjectedSlotCache();
}

void AfpstrueSurroundManager::EnsureProjectedSlotCache()
{
	if (!bHasSharedTargetSnapshot)
	{
		return;
	}

	const bool bNeedsRebuild = SurroundSlots.ContainsByPredicate([this](const FfpstrueSurroundSlot& Slot)
																 { return Slot.ProjectionGeneration != SharedTargetGeneration; });
	if (bNeedsRebuild)
	{
		RebuildProjectedSlotCache();
	}
}

void AfpstrueSurroundManager::RebuildProjectedSlotCache()
{
	if (!bHasSharedTargetSnapshot)
	{
		return;
	}

	// 每个 TargetGeneration 只投影一次全部槽位，AI 决策阶段只读取缓存结果。
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.bHasProjectedSlotLocation = ProjectToNavigation(CalculateRawSlotLocation(Slot), Slot.ProjectedSlotLocation);

		const float ApproachRadius = Slot.RingIndex == 0 ? AttackApproachRadius : OuterAttackApproachRadius;
		Slot.bHasProjectedApproachLocation =
			ProjectToNavigation(CalculateRawSlotLocation(Slot, ApproachRadius), Slot.ProjectedApproachLocation);
		Slot.ProjectionGeneration = SharedTargetGeneration;
	}
}

bool AfpstrueSurroundManager::RequestSurroundSlot(AfpstrueEnemyCharacter* Enemy)
{
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

bool AfpstrueSurroundManager::TryAcquireAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	if (!IsValid(Enemy) || Enemy->IsDead())
	{
		return false;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!bEnableActiveAttackerBudget || BenchmarkConfig.bDisableActiveAttackerBudget)
	{
		return true;
	}

	CleanupInvalidEntries();
	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (ActiveAttackers.Contains(EnemyKey))
	{
		return true;
	}

	if (ActiveAttackers.Num() >= FMath::Max(MaxActiveAttackers, 1))
	{
		return false;
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
	EnsureProjectedSlotCache();

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

bool AfpstrueSurroundManager::GetSharedTargetSnapshot(FVector& OutLocation, uint32& OutTargetGeneration) const
{
	if (!bHasSharedTargetSnapshot)
	{
		return false;
	}

	OutTargetGeneration = SharedTargetGeneration;
	OutLocation = CachedTargetLocation;
	return true;
}

void AfpstrueSurroundManager::ResetManager()
{
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.Occupant.Reset();
	}

	EnemyToSlot.Reset();
	ActiveAttackers.Reset();
	MoveRequestBudgetFrame = MAX_uint64;
	MoveRequestsConsumedThisFrame = 0;
	TargetCharacter = nullptr;
	bHasSharedTargetSnapshot = false;
	SharedTargetGeneration = 0;
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.ProjectionGeneration = 0;
		Slot.bHasProjectedSlotLocation = false;
		Slot.bHasProjectedApproachLocation = false;
	}
	GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
}

void AfpstrueSurroundManager::CleanupInvalidEntries()
{
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
	EnsureProjectedSlotCache();
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

	EnsureProjectedSlotCache();
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

bool AfpstrueSurroundManager::ProjectToNavigation(const FVector& RawLocation, FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	const UNavigationSystemV1* NavigationSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
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
}

void AfpstrueSurroundManager::DrawDebugSlots()
{
	if (!bDrawDebugSlots || !IsValid(TargetCharacter))
	{
		return;
	}

	CleanupInvalidEntries();
	EnsureProjectedSlotCache();

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
