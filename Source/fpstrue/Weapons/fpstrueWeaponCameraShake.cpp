// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponCameraShake.h"
#include "Weapons/fpstrueWeaponConfig.h"

UfpstrueWeaponCameraShake::UfpstrueWeaponCameraShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSingleInstance = true;
	SetRootShakePattern(CreateDefaultSubobject<UfpstrueWeaponRecoilPattern>(TEXT("WeaponRecoilPattern")));
}

void UfpstrueWeaponRecoilPattern::Configure(const FFPWeaponSettings& Settings)
{
	// 武器首次装备已校验配置。这里仅复制视觉参数，不能读取/修改散布或控制后坐力累计值。
	PeakRotation = Settings.VisualRecoilRotation;
	PeakFOV = Settings.VisualRecoilFOV;
	Duration = Settings.VisualRecoilDuration;
}

void UfpstrueWeaponRecoilPattern::GetShakePatternInfoImpl(FCameraShakeInfo& OutInfo) const
{
	// 时长由每件武器的配置决定，脉冲自行计时和收尾，避免再叠一层引擎淡入淡出。
	OutInfo.Duration = FCameraShakeDuration::Custom();
}

void UfpstrueWeaponRecoilPattern::StartShakePatternImpl(const FCameraShakePatternStartParams& Params)
{
	// 重触发从当前偏移冲向本发峰值，不先跳回零；新实例/回收实例从零开始。
	StartRotation = Params.bIsRestarting && !bStopped ? CurrentRotation : FRotator::ZeroRotator;
	StartFOV = Params.bIsRestarting && !bStopped ? CurrentFOV : 0.0f;
	CurrentRotation = StartRotation;
	CurrentFOV = StartFOV;
	Elapsed = 0.0f;
	bStopped = false;
	// 不消耗全局随机数：增加纯视觉效果不能扰动后续子弹散布的随机序列。
	Side = -Side;
}

void UfpstrueWeaponRecoilPattern::UpdateShakePatternImpl(const FCameraShakePatternUpdateParams& Params, FCameraShakePatternUpdateResult& OutResult)
{
	if (bStopped) return;
	Elapsed = FMath::Min(Elapsed + FMath::Max(Params.DeltaTime, 0.0f), Duration);
	const float Progress = Elapsed / Duration;
	const FRotator TargetRotation(PeakRotation.Pitch, PeakRotation.Yaw * Side, PeakRotation.Roll * Side);
	constexpr float AttackFraction = 0.15f;
	if (Progress < AttackFraction)
	{
		const float Attack = FMath::SmoothStep(0.0f, AttackFraction, Progress);
		CurrentRotation = FMath::Lerp(StartRotation, TargetRotation, Attack);
		CurrentFOV = FMath::Lerp(StartFOV, PeakFOV, Attack);
	}
	else
	{
		const float Return = 1.0f - FMath::SmoothStep(AttackFraction, 1.0f, Progress);
		CurrentRotation = TargetRotation * Return;
		CurrentFOV = PeakFOV * Return;
	}
	// 局部旋转带来抽动/侧倾，微小 FOV 脉冲带来透视冲击；无位置推移，避免镜头穿墙。
	OutResult.Rotation = CurrentRotation;
	OutResult.FOV = CurrentFOV;
}

void UfpstrueWeaponRecoilPattern::StopShakePatternImpl(const FCameraShakePatternStopParams& Params)
{
	// 仅供死亡、卸下、失去控制等强制中断；正常松开扳机让最后一发自行回正。
	bStopped = true;
	CurrentRotation = FRotator::ZeroRotator;
	CurrentFOV = 0.0f;
}
