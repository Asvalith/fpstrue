// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "fpstrueAnimNotify_EnemyAttackHit.generated.h"

UCLASS(meta = (DisplayName = "Enemy Attack Hit"))
class FPSTRUE_API UfpstrueAnimNotify_EnemyAttackHit : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(
		USkeletalMeshComponent* MeshComp,
		UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
