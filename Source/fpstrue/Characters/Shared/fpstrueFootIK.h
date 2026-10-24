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

// 玩家模块安装 InputPose→Result 薄后处理图：PreUpdate 采样资格 -> 当前主图姿态查询支撑 -> 两骨 IK -> 诊断回读。
// 只有玩家身体使用；Proxy 独占贴地/锁脚缓存，不决定移动、命中规则或敌人 LOD，当前世界查询始终在 Game Thread。
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
	// 请求下次 PreUpdate 清除锁脚和骨盆缓存，并立即标记诊断为未启用。
	void ResetFootPlacement();
	// 返回最近一次求值的诊断副本；LeaderPose 跟随者不宣称独立完成贴地。
	FFpstrueFootIKDebug GetFootIKDebug() const;
	// 求值结束后在 Game Thread 从 Proxy 取回只读诊断，不再执行地面查询。
	virtual void NativePostEvaluateAnimation() override;
	// 【设计取舍】依赖本帧输入姿态再查询 World，固定拒绝 Worker 求值，不用上一帧资格动态放行。
	// UE5.5 可能在 PreUpdate 前缓存派发条件；代价是安装本后处理的玩家身体主图也同步。
	// 敌人与全局并行开关不受影响，不能把这一玩家后处理描述成已完成并行化。
	virtual bool CanRunParallelWork() const override { return false; }
protected:
	// 为本动画实例创建独占的贴地求值与缓存 Proxy。
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	// 由引擎在动画实例退出时成对释放本类创建的 Proxy。
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
private:
	friend struct FFpstrueFootIKProxy;
	bool bResetRequested = true;
	FFpstrueFootIKDebug Debug;
};
