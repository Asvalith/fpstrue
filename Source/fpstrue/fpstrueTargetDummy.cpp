// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueTargetDummy.h"
#include "fpstrueHealthComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"

AfpstrueTargetDummy::AfpstrueTargetDummy()
{
	PrimaryActorTick.bCanEverTick = false;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
	RootComponent = MeshComponent;
	MeshComponent->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	MeshComponent->SetSimulatePhysics(false);

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueTargetDummy::BeginPlay()
{
	Super::BeginPlay();

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnHealthChanged.AddDynamic(this, &AfpstrueTargetDummy::HandleHealthChanged);
		HealthComponent->OnDeath.AddDynamic(this, &AfpstrueTargetDummy::HandleDeath);
	}
}

void AfpstrueTargetDummy::HandleHealthChanged(float NewHealth)
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			8,
			2.0f,
			FColor::Cyan,
			FString::Printf(TEXT("%s Health: %.0f"), *GetName(), NewHealth)
		);
	}
}

void AfpstrueTargetDummy::HandleDeath()
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			9,
			DestroyDelay,
			FColor::Red,
			FString::Printf(TEXT("%s Dead"), *GetName())
		);
	}

	SetActorEnableCollision(false);
	SetLifeSpan(DestroyDelay);
}
