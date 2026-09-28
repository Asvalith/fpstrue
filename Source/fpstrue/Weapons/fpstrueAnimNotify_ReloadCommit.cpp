// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueAnimNotify_ReloadCommit.h"

#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Components/SkeletalMeshComponent.h"

// 换弹动画到达装填时刻时尝试提交弹药变更；WeaponComponent 用当前状态和单次提交标志过滤重复 Notify。
void UfpstrueAnimNotify_ReloadCommit::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
											 const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (MeshComp == nullptr)
	{
		return;
	}

	// 语法复习：UE Cast 只做 UObject 继承关系检查；两个无继承关系的组件之间 Cast 永远失败。
	// MeshComp 是播放动画的骨骼组件，应从它的 Owner 取得装备关系。
	UfpstrueWeaponComponent* WeaponComponent = nullptr;
	if (const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(MeshComp->GetOwner()))
	{
		WeaponComponent = Character->GetEquippedWeaponComponent();
	}

	if (WeaponComponent != nullptr)
	{
		WeaponComponent->CommitReload();
	}
}

FString UfpstrueAnimNotify_ReloadCommit::GetNotifyName_Implementation() const
{
	// 为动画编辑器提供可读名称，不参与运行时换弹状态判断。
	return TEXT("Reload Commit");
}
