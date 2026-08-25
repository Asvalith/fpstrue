// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueSurroundManager.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;

USTRUCT()
struct FfpstrueSurroundSlot
{
	GENERATED_BODY()

	int32 RingIndex = 0;
	int32 SlotIndexInRing = 0;
	float AngleRadians = 0.0f;
	float Radius = 250.0f;
	TWeakObjectPtr<AfpstrueEnemyCharacter> Occupant;
};

UCLASS()
class FPSTRUE_API AfpstrueSurroundManager : public AActor
{
	GENERATED_BODY()

public:
	AfpstrueSurroundManager();

	void SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter);
	bool RequestSurroundSlot(AfpstrueEnemyCharacter* Enemy);
	void ReleaseSurroundSlot(AfpstrueEnemyCharacter* Enemy);
	bool GetAssignedSlotLocation(
		AfpstrueEnemyCharacter* Enemy,
		FVector& OutLocation,
		bool& bOutInnerRing
	);
	bool GetAttackApproachLocation(AfpstrueEnemyCharacter* Enemy, FVector& OutLocation);
	bool GetSharedTargetSnapshot(
		FVector& OutLocation,
		uint32& OutTargetGeneration
	) const;

	void ResetManager();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "4", ClampMax = "24"))
	int32 InnerSlotCount = 8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "4", ClampMax = "32"))
	int32 OuterSlotCount = 12;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "100.0"))
	float InnerRadius = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Slots", meta = (ClampMin = "150.0"))
	float OuterRadius = 430.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Attack", meta = (ClampMin = "50.0"))
	float AttackApproachRadius = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Attack", meta = (ClampMin = "100.0"))
	float OuterAttackApproachRadius = 220.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Navigation")
	FVector NavigationProjectionExtent = FVector(120.0f, 120.0f, 250.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Shared Target", meta = (ClampMin = "0.05"))
	float SharedTargetRefreshInterval = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Shared Target", meta = (ClampMin = "25.0"))
	float SharedTargetMoveThreshold = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Debug")
	bool bDrawDebugSlots = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Surround|Debug", meta = (ClampMin = "0.1"))
	float DebugDrawInterval = 0.25f;

private:
	void BuildSlots();
	void RefreshSharedTargetSnapshot();
	void UpdateSharedTargetSnapshot(bool bForce);
	void CleanupInvalidEntries();
	int32 FindBestFreeSlot(const FVector& EnemyLocation);
	void PromoteOuterOccupantToInnerSlot(int32 InnerSlotIndex);
	FVector CalculateRawSlotLocation(const FfpstrueSurroundSlot& Slot, float RadiusOverride = -1.0f) const;
	bool ProjectToNavigation(const FVector& RawLocation, FVector& OutLocation) const;
	void DrawDebugSlots();

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter = nullptr;

	FVector CachedTargetLocation = FVector::ZeroVector;
	uint32 SharedTargetGeneration = 0;
	bool bHasSharedTargetSnapshot = false;

	TArray<FfpstrueSurroundSlot> SurroundSlots;
	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, int32> EnemyToSlot;
	FTimerHandle DebugDrawTimerHandle;
	FTimerHandle SharedTargetTimerHandle;
};
