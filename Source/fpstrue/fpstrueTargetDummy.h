// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueTargetDummy.generated.h"

class UStaticMeshComponent;
class UfpstrueHealthComponent;

UCLASS()
class FPSTRUE_API AfpstrueTargetDummy : public AActor
{
	GENERATED_BODY()

public:
	AfpstrueTargetDummy();

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void HandleHealthChanged(float NewHealth);

	UFUNCTION()
	void HandleDeath();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Target")
	float DestroyDelay = 2.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Target")
	UStaticMeshComponent* MeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Target")
	UfpstrueHealthComponent* HealthComponent;
};
