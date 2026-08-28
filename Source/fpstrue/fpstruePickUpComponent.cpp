// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstruePickUpComponent.h"
#include "fpstrueWeaponComponent.h"

//初始化UfpstruePickUpComponent组件，设置SphereRadius为32.f
UfpstruePickUpComponent::UfpstruePickUpComponent()
{
	SphereRadius = 32.f;
}

//在游戏开始时绑定 OnSphereBeginOverlap 事件
void UfpstruePickUpComponent::BeginPlay()
{ //先调用父类的 BeginPlay() 方法，确保组件的基本初始化逻辑被执行
	Super::BeginPlay();
	//OnComponentBeginOverlap添加动态广播OnSphereBeginOverlap方法，确保当发生重叠时会通过this回调OnSphereBeginOverlap函数
	OnComponentBeginOverlap.AddUniqueDynamic(this, &UfpstruePickUpComponent::OnSphereBeginOverlap);
}

//Sphere 真正发生重叠以后调用
void UfpstruePickUpComponent::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
												   UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep,
												   const FHitResult& SweepResult)
{
	if (bConsumed)
	{
		return;
	}
	//找到碰到的角色（之后判断是不是玩家）
	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(OtherActor);
	//归属玩家
	AActor* OwnerActor = GetOwner();
	//归属玩家、归属类存在检测
	UfpstrueWeaponComponent* WeaponComponent =
		OwnerActor != nullptr ? OwnerActor->FindComponentByClass<UfpstrueWeaponComponent>() : nullptr;

	if (Character == nullptr || Character->IsDead() || WeaponComponent == nullptr)
	{
		return;
	}

	bConsumed = true;
	if (!WeaponComponent->AttachWeapon(Character))
	{
		bConsumed = false;
		return;
	}
	//停止拾取组件的碰撞检测，防止重复触发
	SetGenerateOverlapEvents(false);
	//禁用碰撞，防止拾取组件与其他物体发生碰撞
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	//绑定到当前对象this的监听函数全部解除
	OnComponentBeginOverlap.RemoveAll(this);
	//广播拾取事件，通知其他系统或蓝图，角色已拾取该物品
	OnPickUp.Broadcast(Character);

	if (IsValid(this))
	{ //注销组件，从Actor数组中移除，标记为待销毁。UObject内存回收由UE的对象生命周期/GC机制处理。
		DestroyComponent();
	}
	////相关八股
	////UE 的事件解绑、Component 生命周期、UObject 指针安全、一次性事务设计
}
