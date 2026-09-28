// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "fpstrueEnemyCombatConfig.generated.h"

/** 可复用的敌人近战参数；赋给 CombatComponent 后，整组参数覆盖其旧蓝图默认值。 */
UCLASS(BlueprintType)
class FPSTRUE_API UfpstrueEnemyCombatConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "0.0"))
	float AttackRange = 230.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "0.0"))
	float AttackDamage = 10.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = "0.0"))
	float AttackInterval = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
	float AttackAnimationDuration = 1.2f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation", meta = (ClampMin = "0.1"))
	float AttackFailSafeDuration = 5.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
	float AttackCompletionGracePeriod = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace")
	FName WeaponTraceStartSocketName = TEXT("weapontop");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace")
	FName WeaponTraceEndSocketName = TEXT("weaponend");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (ClampMin = "1.0"))
	float WeaponTraceRadius = 8.0f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (ClampMin = "2", ClampMax = "8"))
	int32 WeaponTraceSampleCount = 4;
};
