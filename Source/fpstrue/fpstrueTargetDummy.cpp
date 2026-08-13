// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueTargetDummy.h"
#include "fpstrueHealthComponent.h"
#include "Components/StaticMeshComponent.h"

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
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueTargetDummy::HandleDeath);
	}
}

void AfpstrueTargetDummy::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueTargetDummy::HandleDeath);
	}

	Super::EndPlay(EndPlayReason);
}

void AfpstrueTargetDummy::HandleDeath()
{
	SetActorEnableCollision(false);
	if (bDestroyOnDeath)
	{
		SetLifeSpan(DestroyDelay);
	}
}
