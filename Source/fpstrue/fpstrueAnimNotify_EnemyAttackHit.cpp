// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueAnimNotify_EnemyAttackHit.h"

#include "fpstrueEnemyCharacter.h"
#include "Components/SkeletalMeshComponent.h"

void UfpstrueAnimNotify_EnemyAttackHit::Notify(
	USkeletalMeshComponent* MeshComp,
	UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (MeshComp == nullptr)
	{
		return;
	}

	if (AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(MeshComp->GetOwner()))
	{
		Enemy->HandleAttackHitNotify();
	}
}

FString UfpstrueAnimNotify_EnemyAttackHit::GetNotifyName_Implementation() const
{
	return TEXT("Enemy Attack Hit");
}
