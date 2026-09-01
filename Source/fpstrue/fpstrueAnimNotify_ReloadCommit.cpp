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
	return TEXT("Reload Commit");
}
