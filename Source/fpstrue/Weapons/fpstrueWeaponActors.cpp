// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponActors.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstruePickUpComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

// 原生实体只保留稳定组件拓扑、网络挂接/掉落和本端材质模式；具体素材只在武器蓝图中配置。
namespace
{
void ConfigureNativeWeaponPickup(UfpstruePickUpComponent& Pickup, UfpstrueWeaponComponent& Weapon)
{
	Pickup.SetupAttachment(&Weapon);
	Pickup.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Pickup.SetCollisionResponseToAllChannels(ECR_Ignore);
	Pickup.SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Pickup.SetGenerateOverlapEvents(true);
}
}

// RAR 只替换可见网格和纯表现动画，不继承旧 RU74 蓝图的开火/换弹事件或另一套玩法代码。
AfpstrueRARWeapon::AfpstrueRARWeapon()
{
	bReplicates = true;
	Weapon = CreateDefaultSubobject<UfpstrueWeaponComponent>(TEXT("Weapon"));
	SetRootComponent(Weapon);
	Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	const auto AddPart = [this](const TCHAR* Name, USceneComponent* Parent)
	{
		UStaticMeshComponent* Component = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetupAttachment(Parent);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return Component;
	};
	Forestock = AddPart(TEXT("Forestock"), Weapon);
	Magazine = AddPart(TEXT("Magazine"), Weapon);
	FrontSight = AddPart(TEXT("FrontSight"), Forestock);
	RearSight = AddPart(TEXT("RearSight"), Weapon);
	Pickup = CreateDefaultSubobject<UfpstruePickUpComponent>(TEXT("Pickup"));
	ConfigureNativeWeaponPickup(*Pickup, *Weapon);
}

void AfpstrueRARWeapon::OnRep_AttachmentReplication()
{
	// 旧手部附着不可先由 Super 应用，否则即使随后 Detach 也会把已落地枪瞬移回手边。
	if (Weapon->IsDroppedOnDeath()) { Weapon->RefreshNetworkEquipment(); return; }
	Super::OnRep_AttachmentReplication();
	Weapon->RefreshNetworkEquipment();
}

void AfpstrueRARWeapon::OnRep_ReplicatedMovement()
{
	if (IsReplicatingMovement() && GetReplicatedMovement().bRepPhysics) Weapon->RefreshNetworkPhysics();
	else if (Weapon->IsDroppedOnDeath()) return; // 丢弃死亡前的非物理移动快照。
	Super::OnRep_ReplicatedMovement();
}

void AfpstrueRARWeapon::GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const
{
	Super::GetReplicatedCustomConditionState(OutActiveState);
	// 根组件仍复制 NetworkState；启用移动时明确允许 Actor 物理流，运行中由原生 PreReplication 继续维护。
	DOREPCUSTOMCONDITION_ACTIVE_FAST(AActor, ReplicatedMovement, IsReplicatingMovement());
}

void AfpstrueRARWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (GetNetMode() == NM_DedicatedServer)
	{
		// 枪的动画仅表现弹匣/枪栓；专服保留网格插槽和业务组件，不创建纯表现动画实例。
		Weapon->SetAnimInstanceClass(nullptr);
		Weapon->SetDisablePostProcessBlueprint(true);
		Weapon->SetComponentTickEnabled(false);
		Weapon->PrimaryComponentTick.bStartWithTickEnabled = false;
	}
	if (GetWorld() != nullptr && GetWorld()->IsGameWorld()) SetViewmodelFOVEnabled(false);
}

// MID 由网格组件按需创建并复用；重复装备/附着快照不重复生成材质实例，也不写全局参数。
void AfpstrueRARWeapon::SetViewmodelFOVEnabled(bool bEnabled)
{
	if (GetWorld() == nullptr || !GetWorld()->IsGameWorld() || GetNetMode() == NM_DedicatedServer || ViewmodelFOVParameterName.IsNone()) return;
	const int8 Mode = bEnabled ? 1 : 0;
	if (AppliedViewmodelFOVMode == Mode) return;
	UMeshComponent* Meshes[] = {Weapon.Get(), Forestock.Get(), Magazine.Get(), FrontSight.Get(), RearSight.Get()};
	for (UMeshComponent* Mesh : Meshes) Mesh->SetScalarParameterValueOnMaterials(ViewmodelFOVParameterName, static_cast<float>(Mode));
	AppliedViewmodelFOVMode = Mode;
}

// 成套模板的枪体已包含弹匣/枪栓骨骼，无 RAR 静态附件或供应方玩法蓝图。
AfpstrueTemplateWeapon::AfpstrueTemplateWeapon()
{
	bReplicates = true;
	Weapon = CreateDefaultSubobject<UfpstrueWeaponComponent>(TEXT("Weapon"));
	SetRootComponent(Weapon);
	Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Pickup = CreateDefaultSubobject<UfpstruePickUpComponent>(TEXT("Pickup"));
	ConfigureNativeWeaponPickup(*Pickup, *Weapon);
}

void AfpstrueTemplateWeapon::OnRep_AttachmentReplication()
{
	if (Weapon->IsDroppedOnDeath()) { Weapon->RefreshNetworkEquipment(); return; }
	Super::OnRep_AttachmentReplication();
	Weapon->RefreshNetworkEquipment();
}

void AfpstrueTemplateWeapon::OnRep_ReplicatedMovement()
{
	if (IsReplicatingMovement() && GetReplicatedMovement().bRepPhysics) Weapon->RefreshNetworkPhysics();
	else if (Weapon->IsDroppedOnDeath()) return;
	Super::OnRep_ReplicatedMovement();
}

void AfpstrueTemplateWeapon::GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const
{
	Super::GetReplicatedCustomConditionState(OutActiveState);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(AActor, ReplicatedMovement, IsReplicatingMovement());
}

void AfpstrueTemplateWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (GetNetMode() == NM_DedicatedServer)
	{
		Weapon->SetAnimInstanceClass(nullptr);
		Weapon->SetDisablePostProcessBlueprint(true);
		Weapon->SetComponentTickEnabled(false);
		Weapon->PrimaryComponentTick.bStartWithTickEnabled = false;
	}
}
