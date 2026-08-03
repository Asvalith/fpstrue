// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueShotgunWeaponComponent.h"
#include "fpstrueWeaponDataAsset.h"

int32 UfpstrueShotgunWeaponComponent::GetTraceCount() const
{
	const UfpstrueWeaponDataAsset* Data = GetWeaponData();
	return Data != nullptr ? FMath::Max(2, Data->PelletsPerShot) : 8;
}
