// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SphereComponent.h"
#include "fpstrueCharacter.h"
#include "fpstruePickUpComponent.generated.h"

//声明一个Onpickup动态多播委托，用于拾取后广播
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPickUp, AfpstrueCharacter*, PickUpCharacter);
//声明组件类 UfpstruePickUpComponent
UCLASS(Blueprintable, BlueprintType, ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))

//FPSTRUE_API模块导入导出宏，解决不同 UE 模块/DLL 之间的符号可见性和链接问题
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

	//重写基类
	virtual void BeginPlay() override;
	
	//Sphere 真正发生重叠以后调用
	UFUNCTION()
	void OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

private:
	//防重复拾取的标志位，初始值为false，表示未被拾取
	bool bConsumed = false;
};
