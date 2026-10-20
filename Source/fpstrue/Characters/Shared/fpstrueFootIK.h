// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "fpstrueFootIK.generated.h"

class ACharacter;
struct FFpstrueFootIKProxy;

// 诊断来自最后一次真实求值；不复制到网络，不参与伤害或 CMC 的移动计算。
struct FPSTRUE_API FFpstrueFootIKDebug
{
	bool bEnabled = false;
	float PelvisOffset = 0.0f;
	FVector LeftSoleWorld = FVector::ZeroVector, RightSoleWorld = FVector::ZeroVector;
	FVector RawSoleHeights = FVector::ZeroVector, SupportWeights = FVector::ZeroVector, ReachErrors = FVector::ZeroVector;
	bool bLeftPlanted = false, bRightPlanted = false;
	uint32 LeftPlantRevision = 0, RightPlantRevision = 0;
	uint32 TraceCount = 0; // 玩家后处理实际发起的累计世界查询。
	uint32 ProbeTraceCount = 0;
};

// 玩家使用 InputPose→Result 薄后处理图；原生 Proxy 在当前输入姿态上解腿，不读取上帧 IK 后的脚高作为误差。
UCLASS(Transient, Blueprintable)
class FPSTRUE_API UfpstrueFootIKAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") FVector LeftSoleOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") FVector RightSoleOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") FVector LeftSoleNormal = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") FVector RightSoleNormal = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") TArray<FVector> LeftSoleProbes;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") TArray<FVector> RightSoleProbes;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") TArray<FVector> LeftToeProbes;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") TArray<FVector> RightToeProbes;
	// 仅显式验收时使用这些真实鞋底顶点；正式动画更新不做 CPU 蒙皮。
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") TArray<int32> LeftSoleVertices;
	UPROPERTY(EditDefaultsOnly, Category="Foot IK") TArray<int32> RightSoleVertices;
	// 仅接受项目玩家角色；敌人及其他 Pawn 不安装此后处理，不共享玩家脚部资格。
	static void Install(ACharacter* Character);
	void ResetFootPlacement();
	FFpstrueFootIKDebug GetFootIKDebug() const;
	virtual void NativePostEvaluateAnimation() override;
	// 此玩家后处理在本帧输入姿态上查询World，必须在首次初始化、离地和落地前都拒绝Worker求值。
	// UE5.5可在Proxy::PreUpdate之前缓存派发条件，不能用上一轮贴地资格动态开启并行。
	// 原生主图与后处理共用Mesh求值门禁，因此仅安装本后处理的玩家身体同步；不修改敌人或全局并行开关。
	virtual bool CanRunParallelWork() const override { return false; }
protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
private:
	friend struct FFpstrueFootIKProxy;
	bool bResetRequested = true;
	FFpstrueFootIKDebug Debug;
};
