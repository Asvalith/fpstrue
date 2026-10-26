// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstruePickUpComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"

//初始化UfpstruePickUpComponent组件，设置SphereRadius为32.f
UfpstruePickUpComponent::UfpstruePickUpComponent()
{
	SphereRadius = 32.f;
}

// 装备恢复、第一/第三人称切换和死亡掉落都会递归显示枪械；碰撞球不能随之成为可见模型。
void UfpstruePickUpComponent::OnHiddenInGameChanged()
{
	// 仅约束渲染隐藏，不修改可见附件、碰撞开关或重叠事件；直接恢复标记避免递归调用自身。
	bHiddenInGame = true;
	Super::OnHiddenInGameChanged();
}

// 组件启动时只绑定一次权威拾取入口，装备资格仍由重叠回调和武器事务检查。
void UfpstruePickUpComponent::BeginPlay()
{
	Super::BeginPlay();
	OnComponentBeginOverlap.AddUniqueDynamic(this, &UfpstruePickUpComponent::OnSphereBeginOverlap);
}

// 服务器发现玩家候选后申请完整装备，成功才消费并广播；先占用以阻止装备通知重入拾取。
void UfpstruePickUpComponent::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (bConsumed || GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}
	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(OtherActor);
	//拾取组件的 Owner 是武器 Actor，不是刚进入范围的玩家。
	AActor* OwnerActor = GetOwner();
	UfpstrueWeaponComponent* WeaponComponent = OwnerActor != nullptr ? OwnerActor->FindComponentByClass<UfpstrueWeaponComponent>() : nullptr;

	if (Character == nullptr || WeaponComponent == nullptr)
	{
		return;
	}

	// ★【重要边界】[结算去重] 外部装备调用前先占用拾取物；失败才释放，成功后关闭重叠再通知表现。
	// AttachWeapon 会同步广播装备事件，先占用再调用，防止监听者重入拾取。
	bConsumed = true;
	if (!WeaponComponent->AttachWeapon(Character))
	{
		bConsumed = false;
		return;
	}
	// 成功后先关闭入口再广播；监听者即使同步销毁拾取物，也不会再次装备。
	SetGenerateOverlapEvents(false);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OnComponentBeginOverlap.RemoveAll(this);
	OnPickUp.Broadcast(Character);

	if (IsValid(this))
	{
		DestroyComponent();
	}
}
