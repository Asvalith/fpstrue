// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "fpstrueWeaponActors.generated.h"

class UfpstrueWeaponComponent;
class UfpstruePickUpComponent;
class UStaticMeshComponent;

// RAR 使用独立原生拾取实体，避免继承旧 RU74 蓝图的表现事件；玩法仍由同一个武器组件和 GAS 执行。
UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueRARWeapon : public AActor
{
	GENERATED_BODY()

public:
	// 创建通用网格/附件拓扑与拾取范围；具体素材和装配参数由派生武器蓝图保存。
	AfpstrueRARWeapon();
	// 只修改本端实例材质：世界枪/他人枪为 0，自己的第一人称枪及附件为 1，不改共享材质或全局 MPC。
	void SetViewmodelFOVEnabled(bool bEnabled);
	// 根组件的服务器挂接晚到时，重新应用本端第一/第三人称挂点，不改变服务器装备归属。
	virtual void OnRep_AttachmentReplication() override;
	// 权威物理包到达前先确保掉落体就绪，死亡后的旧手持移动包不再应用。
	virtual void OnRep_ReplicatedMovement() override;
	// 保留 Actor 原生物理位姿复制，不再复制根网格的第二套手部位姿。
	virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;
	// 专服关闭纯枪械动画；游戏实例默认使用世界枪材质模式，装备后按本地视角切换。
	virtual void PostInitializeComponents() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstrueWeaponComponent> Weapon;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstruePickUpComponent> Pickup;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> Forestock;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> Magazine;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> FrontSight;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> RearSight;
	// 素材参数名是武器蓝图的只读配置，不在原生类写死供应方材质约定。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	FName ViewmodelFOVParameterName;
	// -1 表示当前实例尚未初始化；只缓存材质模式，不参与任何玩法状态或网络复制。
	int8 AppliedViewmodelFOVMode = -1;
};

// UE5.5 模板只提供匹配的枪械/动画素材；拾取、弹药、命中和联网仍复用项目现有组件与 GAS。
UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueTemplateWeapon : public AActor
{
	GENERATED_BODY()

public:
	// 创建整枪与拾取拓扑；具体网格、动画及反馈由派生武器蓝图提供。
	AfpstrueTemplateWeapon();
	// Actor 附着与组件快照可以分批到达，始终以本端实际持有者恢复正确的第一/第三人称装配。
	virtual void OnRep_AttachmentReplication() override;
	// 接收权威掉落物理；忽略死亡前遗留的非物理移动状态。
	virtual void OnRep_ReplicatedMovement() override;
	// 仅在 Actor 开启移动复制时传送原生物理位姿，业务状态仍归组件快照。
	virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;
	// 专服不运行弹匣/枪栓等纯表现动画，保留武器业务组件及挂点。
	virtual void PostInitializeComponents() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstrueWeaponComponent> Weapon;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstruePickUpComponent> Pickup;
};
