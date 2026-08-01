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

	OnComponentBeginOverlap.AddDynamic(this, &UfpstruePickUpComponent::OnSphereBeginOverlap);
}

void UfpstruePickUpComponent::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(OtherActor);
	AActor* OwnerActor = GetOwner();
	UfpstrueWeaponComponent* WeaponComponent =
		OwnerActor != nullptr ? OwnerActor->FindComponentByClass<UfpstrueWeaponComponent>() : nullptr;

	if (Character != nullptr
		&& !Character->IsDead()
		&& WeaponComponent != nullptr
		&& WeaponComponent->AttachWeapon(Character))
	{
		SetGenerateOverlapEvents(false);
		SetCollisionEnabled(ECollisionEnabled::NoCollision);
		OnComponentBeginOverlap.RemoveAll(this);
		OnPickUp.Broadcast(Character);

		if (IsValid(this))
		{
			DestroyComponent();
		}
	}
}
