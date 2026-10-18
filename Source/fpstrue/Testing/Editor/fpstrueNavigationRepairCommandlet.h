// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "fpstrueNavigationRepairCommandlet.generated.h"

// 只重烘已批准的现役 Baked 地图导航；原美术源图和物理碰撞设置不变。
UCLASS()
class UfpstrueNavigationRepairCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UfpstrueNavigationRepairCommandlet();
	virtual int32 Main(const FString& Params) override;
};
