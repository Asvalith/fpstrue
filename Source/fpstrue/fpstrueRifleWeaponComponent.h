// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "fpstrueWeaponComponent.h"
#include "fpstrueRifleWeaponComponent.generated.h"

UCLASS(Blueprintable, BlueprintType, ClassGroup = (Weapon), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueRifleWeaponComponent : public UfpstrueWeaponComponent
{
	GENERATED_BODY()

protected:
	virtual int32 GetTraceCount() const override;
};
