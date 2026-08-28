// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueAnimNotifyState_AttackWindow.h"

#include "fpstrueEnemyCharacter.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
AfpstrueEnemyCharacter* GetEnemyOwner(USkeletalMeshComponent* MeshComp)
{
	return MeshComp != nullptr ? Cast<AfpstrueEnemyCharacter>(MeshComp->GetOwner()) : nullptr;
}
} // namespace

void UfpstrueAnimNotifyState_AttackWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
													   const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (AfpstrueEnemyCharacter* Enemy = GetEnemyOwner(MeshComp))
	{
		Enemy->BeginAttackWindow();
	}
}

void UfpstrueAnimNotifyState_AttackWindow::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime,
													  const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	if (AfpstrueEnemyCharacter* Enemy = GetEnemyOwner(MeshComp))
	{
		Enemy->UpdateAttackWindow();
	}
}

void UfpstrueAnimNotifyState_AttackWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
													 const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (AfpstrueEnemyCharacter* Enemy = GetEnemyOwner(MeshComp))
	{
		Enemy->EndAttackWindow();
	}
}
