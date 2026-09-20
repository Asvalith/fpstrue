// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "fpstrueAnimNotify_ReloadCommit.generated.h"

/** 换弹提交点：在动画指定帧把备弹真正转移到弹匣。 */
UCLASS(meta = (DisplayName = "Reload Commit"))
class FPSTRUE_API UfpstrueAnimNotify_ReloadCommit : public UAnimNotify
{
	GENERATED_BODY()

public:
	// 找到当前武器并提交一次换弹事务。
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
						const FAnimNotifyEventReference& EventReference) override;

	// 返回动画编辑器中显示的 Notify 名称。
	virtual FString GetNotifyName_Implementation() const override;
};
