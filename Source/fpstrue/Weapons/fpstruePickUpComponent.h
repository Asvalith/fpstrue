// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SphereComponent.h"
#include "fpstruePickUpComponent.generated.h"

// ==================== 武器拾取入口 ====================
class AfpstrueCharacter;

// 只广播成功装备的事实，真正的资格检查与状态提交归武器组件。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPickUp, AfpstrueCharacter*, PickUpCharacter);
/** 一次性拾取触发器：检测玩家进入球形范围并向蓝图广播拾取事件。 */
UCLASS(Blueprintable, BlueprintType, ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))

class FPSTRUE_API UfpstruePickUpComponent : public USphereComponent
{
	GENERATED_BODY()

public:
	//初始化UfpstruePickUpComponent组件
	UfpstruePickUpComponent();
	//创建拾取事件暴露给蓝图，作为事件分发器
	UPROPERTY(BlueprintAssignable, Category = "Interaction")
	FOnPickUp OnPickUp;

protected:
	// 绑定球形碰撞的重叠回调。
	virtual void BeginPlay() override;
	// 父级递归显示手臂/枪械时，拾取范围仍只作碰撞查询，不渲染红色球体。
	virtual void OnHiddenInGameChanged() override;

	// 权威重叠回调先占用拾取物，再申请完整装备；失败归还，成功通知后销毁触发器。
	UFUNCTION()
	void OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
							  int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

private:
	//防重复拾取的标志位，初始值为false，表示未被拾取
	bool bConsumed = false;
};
