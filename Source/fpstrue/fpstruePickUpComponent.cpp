// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstruePickUpComponent.h"
#include "fpstrueWeaponComponent.h"

UfpstruePickUpComponent::UfpstruePickUpComponent()
{
	SphereRadius = 32.f;
}

void UfpstruePickUpComponent::BeginPlay()
{
	Super::BeginPlay();

	OnComponentBeginOverlap.AddUniqueDynamic(this, &UfpstruePickUpComponent::OnSphereBeginOverlap);
}

void UfpstruePickUpComponent::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (bConsumed)
	{
		return;
	}

	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(OtherActor);
	AActor* OwnerActor = GetOwner();
	UfpstrueWeaponComponent* WeaponComponent =
		OwnerActor != nullptr ? OwnerActor->FindComponentByClass<UfpstrueWeaponComponent>() : nullptr;

	if (Character == nullptr || Character->IsDead() || WeaponComponent == nullptr)
	{
		return;
	}

	bConsumed = true;
	if (!WeaponComponent->AttachWeapon(Character))
	{
		bConsumed = false;
		return;
	}

	SetGenerateOverlapEvents(false);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OnComponentBeginOverlap.RemoveAll(this);
	OnPickUp.Broadcast(Character);

	if (IsValid(this))
	{
		DestroyComponent();
	}
}
