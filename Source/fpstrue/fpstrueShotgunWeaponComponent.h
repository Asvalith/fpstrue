// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "fpstrueWeaponComponent.h"
#include "fpstrueShotgunWeaponComponent.generated.h"

UCLASS(Blueprintable, BlueprintType, ClassGroup = (Weapon), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueShotgunWeaponComponent : public UfpstrueWeaponComponent
{
	GENERATED_BODY()

protected:
	virtual int32 GetTraceCount() const override;
};
