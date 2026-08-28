// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "fpstrueAnimNotifyState_AttackWindow.generated.h"

/** 敌人近战攻击窗口：由动画时间段驱动武器 Sweep 的开始、更新和结束。 */
UCLASS(meta = (DisplayName = "Enemy Attack Window"))
class FPSTRUE_API UfpstrueAnimNotifyState_AttackWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	// 动画进入攻击窗口时初始化本次近战检测。
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
							 const FAnimNotifyEventReference& EventReference) override;

	// 动画窗口持续期间更新武器轨迹检测。
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime,
							const FAnimNotifyEventReference& EventReference) override;

	// 动画离开攻击窗口时结束检测并清理临时状态。
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
						   const FAnimNotifyEventReference& EventReference) override;
};
