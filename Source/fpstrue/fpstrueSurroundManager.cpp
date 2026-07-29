// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueSurroundManager.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "DrawDebugHelpers.h"
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
		GetWorldTimerManager().SetTimer(
			DebugDrawTimerHandle,
			this,
			&AfpstrueSurroundManager::DrawDebugSlots,
			DebugDrawInterval,
			true
		);
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
	TargetCharacter = NewTargetCharacter;
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
			Slot.SlotIndexInRing = SlotIndex;
			Slot.AngleRadians =
				2.0f * PI * static_cast<float>(SlotIndex) / static_cast<float>(SafeSlotCount);
			Slot.Radius = Radius;
		}
	};

	AddRing(0, InnerSlotCount, InnerRadius);
	AddRing(1, OuterSlotCount, OuterRadius);
}

bool AfpstrueSurroundManager::RequestSurroundSlot(AfpstrueEnemyCharacter* Enemy)
{
	if (!IsValid(Enemy) || !IsValid(TargetCharacter))
	{
		return false;
	}

	CleanupInvalidEntries();

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (EnemyToSlot.Contains(EnemyKey))
	{
		return true;
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

	ReleaseAttackToken(Enemy);

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	int32* SlotIndexPtr = EnemyToSlot.Find(EnemyKey);
	if (SlotIndexPtr == nullptr)
	{
		return;
	}

	const int32 ReleasedSlotIndex = *SlotIndexPtr;
	const bool bReleasedInnerSlot =
		SurroundSlots.IsValidIndex(ReleasedSlotIndex)
		&& SurroundSlots[ReleasedSlotIndex].RingIndex == 0;

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

bool AfpstrueSurroundManager::GetAssignedSlotLocation(
	AfpstrueEnemyCharacter* Enemy,
	FVector& OutLocation,
	bool& bOutInnerRing)
{
	bOutInnerRing = false;

	if (!IsValid(Enemy) || !IsValid(TargetCharacter))
	{
		return false;
	}

	CleanupInvalidEntries();

	const int32* SlotIndexPtr = EnemyToSlot.Find(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	if (SlotIndexPtr == nullptr || !SurroundSlots.IsValidIndex(*SlotIndexPtr))
	{
		return false;
	}

	const FfpstrueSurroundSlot& Slot = SurroundSlots[*SlotIndexPtr];
	bOutInnerRing = Slot.RingIndex == 0;
	return ProjectToNavigation(CalculateRawSlotLocation(Slot), OutLocation);
}

bool AfpstrueSurroundManager::GetAttackApproachLocation(
	AfpstrueEnemyCharacter* Enemy,
	FVector& OutLocation)
{
	if (!IsValid(Enemy) || !IsValid(TargetCharacter) || !HasAttackToken(Enemy))
	{
		return false;
	}

	const int32* SlotIndexPtr = EnemyToSlot.Find(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	if (SlotIndexPtr == nullptr || !SurroundSlots.IsValidIndex(*SlotIndexPtr))
	{
		return false;
	}

	const FVector RawLocation =
		CalculateRawSlotLocation(SurroundSlots[*SlotIndexPtr], AttackApproachRadius);
	return ProjectToNavigation(RawLocation, OutLocation);
}

bool AfpstrueSurroundManager::RequestAttackToken(AfpstrueEnemyCharacter* Enemy)
{
	if (!IsValid(Enemy) || Enemy->IsDead())
	{
		return false;
	}

	CleanupInvalidEntries();

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (ActiveAttackers.Contains(EnemyKey))
	{
		return true;
	}

	const int32* SlotIndexPtr = EnemyToSlot.Find(EnemyKey);
	if (SlotIndexPtr == nullptr
		|| !SurroundSlots.IsValidIndex(*SlotIndexPtr)
		|| SurroundSlots[*SlotIndexPtr].RingIndex != 0
		|| ActiveAttackers.Num() >= MaxConcurrentAttackers)
	{
		return false;
	}

	ActiveAttackers.Add(EnemyKey);
	return true;
}

void AfpstrueSurroundManager::ReleaseAttackToken(AfpstrueEnemyCharacter* Enemy)
{
	if (Enemy != nullptr)
	{
		ActiveAttackers.Remove(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	}
}

bool AfpstrueSurroundManager::HasAttackToken(const AfpstrueEnemyCharacter* Enemy) const
{
	return Enemy != nullptr
		&& ActiveAttackers.Contains(
			TWeakObjectPtr<AfpstrueEnemyCharacter>(
				const_cast<AfpstrueEnemyCharacter*>(Enemy)
			)
		);
}

void AfpstrueSurroundManager::ResetManager()
{
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.Occupant.Reset();
	}

	EnemyToSlot.Reset();
	ActiveAttackers.Reset();
	TargetCharacter = nullptr;
}

void AfpstrueSurroundManager::CleanupInvalidEntries()
{
	for (auto Iterator = EnemyToSlot.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Key().IsValid())
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
		if (!Iterator->IsValid())
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

			FVector ProjectedLocation;
			if (!ProjectToNavigation(CalculateRawSlotLocation(Slot), ProjectedLocation))
			{
				continue;
			}

			const float DistanceSquared =
				FVector::DistSquared2D(EnemyLocation, ProjectedLocation);
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
	if (!SurroundSlots.IsValidIndex(InnerSlotIndex)
		|| SurroundSlots[InnerSlotIndex].RingIndex != 0
		|| SurroundSlots[InnerSlotIndex].Occupant.IsValid())
	{
		return;
	}

	const FVector InnerLocation = CalculateRawSlotLocation(SurroundSlots[InnerSlotIndex]);
	int32 BestOuterSlotIndex = INDEX_NONE;
	float BestDistanceSquared = TNumericLimits<float>::Max();

	for (int32 SlotIndex = 0; SlotIndex < SurroundSlots.Num(); ++SlotIndex)
	{
		const FfpstrueSurroundSlot& Slot = SurroundSlots[SlotIndex];
		if (Slot.RingIndex == 0 || !Slot.Occupant.IsValid())
		{
			continue;
		}

		const float DistanceSquared =
			FVector::DistSquared2D(Slot.Occupant->GetActorLocation(), InnerLocation);
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

	TWeakObjectPtr<AfpstrueEnemyCharacter> PromotedEnemy =
		SurroundSlots[BestOuterSlotIndex].Occupant;
	SurroundSlots[BestOuterSlotIndex].Occupant.Reset();
	SurroundSlots[InnerSlotIndex].Occupant = PromotedEnemy;
	EnemyToSlot.FindOrAdd(PromotedEnemy) = InnerSlotIndex;
}

FVector AfpstrueSurroundManager::CalculateRawSlotLocation(
	const FfpstrueSurroundSlot& Slot,
	float RadiusOverride) const
{
	if (!IsValid(TargetCharacter))
	{
		return GetActorLocation();
	}

	const float Radius = RadiusOverride >= 0.0f ? RadiusOverride : Slot.Radius;
	const FVector Direction(
		FMath::Cos(Slot.AngleRadians),
		FMath::Sin(Slot.AngleRadians),
		0.0f
	);
	return TargetCharacter->GetActorLocation() + Direction * Radius;
}

bool AfpstrueSurroundManager::ProjectToNavigation(
	const FVector& RawLocation,
	FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	const UNavigationSystemV1* NavigationSystem =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (NavigationSystem == nullptr)
	{
		return false;
	}

	FNavLocation ProjectedLocation;
	if (!NavigationSystem->ProjectPointToNavigation(
		RawLocation,
		ProjectedLocation,
		NavigationProjectionExtent))
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

	for (const FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		FVector SlotLocation;
		if (!ProjectToNavigation(CalculateRawSlotLocation(Slot), SlotLocation))
		{
			continue;
		}

		FColor SlotColor = Slot.Occupant.IsValid() ? FColor::Cyan : FColor::Green;
		if (Slot.Occupant.IsValid() && HasAttackToken(Slot.Occupant.Get()))
		{
			SlotColor = FColor::Red;
		}

		DrawDebugSphere(
			GetWorld(),
			SlotLocation,
			24.0f,
			12,
			SlotColor,
			false,
			DebugDrawInterval * 1.25f,
			0,
			2.0f
		);
	}
}
