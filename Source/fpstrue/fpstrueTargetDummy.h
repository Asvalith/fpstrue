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
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleHealthChanged(float NewHealth);

	UFUNCTION()
	void HandleDeath();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Target")
	float DestroyDelay = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Target")
	bool bDestroyOnDeath = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Target")
	UStaticMeshComponent* MeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Target")
	UfpstrueHealthComponent* HealthComponent;
};
