// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraShakeBase.h"
#include "fpstrueWeaponCameraShake.generated.h"

struct FFPWeaponSettings;

// 单发视觉脉冲：只修改 CameraManager 最终 POV，不写 Controller 或用于 Hitscan 的 Camera 组件。
// Shake 与 Pattern 是 UE 的两层接口，集中在同一文件；不增加 Actor、Tick 或恢复 Timer。
UCLASS()
class FPSTRUE_API UfpstrueWeaponRecoilPattern : public UCameraShakePattern
{
	GENERATED_BODY()

public:
	void Configure(const FFPWeaponSettings& Settings);

private:
	virtual void GetShakePatternInfoImpl(FCameraShakeInfo& OutInfo) const override;
	virtual void StartShakePatternImpl(const FCameraShakePatternStartParams& Params) override;
	virtual void UpdateShakePatternImpl(const FCameraShakePatternUpdateParams& Params, FCameraShakePatternUpdateResult& OutResult) override;
	virtual bool IsFinishedImpl() const override { return bStopped || Elapsed >= Duration; }
	virtual void StopShakePatternImpl(const FCameraShakePatternStopParams& Params) override;

	FRotator PeakRotation = FRotator::ZeroRotator;
	FRotator StartRotation = FRotator::ZeroRotator;
	FRotator CurrentRotation = FRotator::ZeroRotator;
	float PeakFOV = 0.0f;
	float StartFOV = 0.0f;
	float CurrentFOV = 0.0f;
	float Duration = 0.12f;
	float Elapsed = 0.0f;
	float Side = 1.0f;
	bool bStopped = true;
};

// 同一相机最多一个本类实例；连发重触发脉冲，不让震动层数随子弹数增长。
UCLASS()
class FPSTRUE_API UfpstrueWeaponCameraShake : public UCameraShakeBase
{
	GENERATED_BODY()

public:
	UfpstrueWeaponCameraShake(const FObjectInitializer& ObjectInitializer);
};
