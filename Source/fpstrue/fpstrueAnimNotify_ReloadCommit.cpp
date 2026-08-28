// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueAnimNotify_ReloadCommit.h"

#include "fpstrueCharacter.h"
#include "fpstrueWeaponComponent.h"
#include "Components/SkeletalMeshComponent.h"

void UfpstrueAnimNotify_ReloadCommit::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
											 const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (MeshComp == nullptr)
	{
		return;
	}

	UfpstrueWeaponComponent* WeaponComponent = Cast<UfpstrueWeaponComponent>(MeshComp);
	if (WeaponComponent == nullptr)
	{
		if (const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(MeshComp->GetOwner()))
		{
			WeaponComponent = Character->GetEquippedWeaponComponent();
		}
	}

	if (WeaponComponent != nullptr)
	{
		WeaponComponent->CommitReload();
	}
}

FString UfpstrueAnimNotify_ReloadCommit::GetNotifyName_Implementation() const
{
	return TEXT("Reload Commit");
}
