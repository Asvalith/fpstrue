// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "fpstrueFootIK.generated.h"

class ACharacter;
class UWorld;
struct FFpstrueFootIKProxy;

// 本进程、本 World、本引擎帧的敌人后处理计量；求值墙钟包含查询及解腿，不等于线程 on-core 时间。
struct FPSTRUE_API FFpstrueFootIKFrameStats
{
	double EnemySolveMs = 0.0;
	uint32 Evaluations = 0, WorldQueries = 0, ComponentQueries = 0;
	uint32 ReusedFootprints = 0, FallbackFootprints = 0;
};

// 诊断来自最后一次真实求值；不复制到网络，不参与伤害或 CMC 的移动计算。
struct FPSTRUE_API FFpstrueFootIKDebug
{
	bool bEnabled = false;
	float PelvisOffset = 0.0f;
	FVector LeftSoleWorld = FVector::ZeroVector, RightSoleWorld = FVector::ZeroVector;
	FVector RawSoleHeights = FVector::ZeroVector, SupportWeights = FVector::ZeroVector, ReachErrors = FVector::ZeroVector;
	bool bLeftPlanted = false, bRightPlanted = false;
	uint32 LeftPlantRevision = 0, RightPlantRevision = 0;
	uint32 TraceCount = 0; // 累计世界查询；组件窄相仍单独计数，不能把换入口算成消除了碰撞工作。
	uint32 ComponentTraceCount = 0, ReusedFootprints = 0, FallbackFootprints = 0;
	uint32 ProbeTraceCount = 0, CandidateMismatches = 0;
};

// 两种骨架各用一个 InputPose→Result 薄后处理图；同一原生 Proxy 在当前输入姿态上解腿，不读取上帧 IK 后的脚高作为误差。
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
	// 仅敌人可复用同次求值的候选；不缓存上帧平面，不减少鞋底/脚趾采样，玩家始终使用原世界查询。
	bool bReuseEnemyQueryCandidates = true;
	// 仅功能验收显式打开：每条快路射线再跑原世界查询作为 oracle，正式压测不能打开。
	bool bVerifyEnemyCandidateQueries = false;
	static void Install(ACharacter* Character, bool bEnemy);
	static FFpstrueFootIKFrameStats GetEnemyFrameStats(const UWorld* World);
	void ResetFootPlacement();
	FFpstrueFootIKDebug GetFootIKDebug() const;
	virtual void NativePostEvaluateAnimation() override;
	virtual bool CanRunParallelWork() const override { return !bRequireSynchronousEvaluation; }
protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
private:
	friend struct FFpstrueFootIKProxy;
	bool bResetRequested = true;
	bool bRequireSynchronousEvaluation = false;
	FFpstrueFootIKDebug Debug;
};
