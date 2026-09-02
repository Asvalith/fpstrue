// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueAnimNotifyState_AttackWindow.h"

#include "fpstrueEnemyCharacter.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
// Notify 收到的是播放动画的 Mesh；攻击事务属于它的 Enemy Owner，而不是 Notify 对象本身。
AfpstrueEnemyCharacter* GetEnemyOwner(USkeletalMeshComponent* MeshComp)
{
	return MeshComp != nullptr ? Cast<AfpstrueEnemyCharacter>(MeshComp->GetOwner()) : nullptr;
}
} // namespace

// 动画进入有效帧区间时建立第一组刀刃采样点，并清空本次攻击的命中去重状态。
void UfpstrueAnimNotifyState_AttackWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
													   const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (AfpstrueEnemyCharacter* Enemy = GetEnemyOwner(MeshComp))
	{
		Enemy->BeginAttackWindow();
	}
}

// 有效区间内每个动画更新步推进一次连续 Sweep，覆盖相邻姿态之间刀刃扫过的空间。
void UfpstrueAnimNotifyState_AttackWindow::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime,
													  const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	if (AfpstrueEnemyCharacter* Enemy = GetEnemyOwner(MeshComp))
	{
		Enemy->UpdateAttackWindow();
	}
}

// 离开有效帧区间后立即关闭检测，攻击事务本身仍由结束 Notify 或保护 Timer 收尾。
void UfpstrueAnimNotifyState_AttackWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
													 const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (AfpstrueEnemyCharacter* Enemy = GetEnemyOwner(MeshComp))
	{
		Enemy->EndAttackWindow();
	}
}
