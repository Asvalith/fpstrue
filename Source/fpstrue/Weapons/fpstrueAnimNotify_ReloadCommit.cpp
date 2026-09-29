// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueAnimNotify_ReloadCommit.h"

#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimNotifyQueue.h"

// 换弹装填帧携带原始 Montage 播放身份；旧动画通知不能通过“当前装备”提交新一轮换弹。
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
	UfpstrueWeaponComponent* WeaponComponent = Cast<UfpstrueWeaponComponent>(MeshComp);
	if (const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(MeshComp->GetOwner()))
	{
		WeaponComponent = Character->GetEquippedWeaponComponent();
	}

	if (WeaponComponent != nullptr)
	{
		WeaponComponent->CommitReloadFromNotify(MeshComp, EventReference);
	}
}

void UfpstrueAnimNotify_ReloadCommit::BranchingPointNotify(FBranchingPointNotifyPayload& Payload)
{
	// UE 的默认桥接会丢弃 MontageInstanceID；显式携带，Queued/BranchingPoint 使用同一身份校验。
	FAnimNotifyEventReference EventReference(Payload.NotifyEvent, Payload.SequenceAsset);
	EventReference.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(Payload.MontageInstanceID);
	Notify(Payload.SkelMeshComponent, Payload.SequenceAsset, EventReference);
}

FString UfpstrueAnimNotify_ReloadCommit::GetNotifyName_Implementation() const
{
	// 为动画编辑器提供可读名称，不参与运行时换弹状态判断。
	return TEXT("Reload Commit");
}
