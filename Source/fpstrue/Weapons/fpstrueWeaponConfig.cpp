// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponConfig.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

// 检查整组武器参数；失败返回具体原因，不改写配置或用默认值掩盖非法输入。
bool FFPWeaponSettings::Validate(FString& OutError) const
{
	// 名称与整数先校验；这些字段不能通过后续浮点范围表表达。
	OutError.Reset();
	if (GripSocketName.IsNone() || MagazineSize < 1 || StartingReserveAmmo < 0
		|| CriticalHitBones.Contains(NAME_None))
	{
		OutError = TEXT("GripSocketName/bone entries must be named; MagazineSize >= 1 and StartingReserveAmmo >= 0.");
		return false;
	}
	// 集中列出浮点参数的合法区间，供装备入口与编辑器资产验证共同使用。
	struct FRange
	{
		const TCHAR* Name;
		float Value;
		float Min;
		float Max = MAX_flt;
	};
	const FRange Ranges[] = {
		{TEXT("RoundsPerMinute"), RoundsPerMinute, 1.0f},
		{TEXT("LineTraceRange"), LineTraceRange, 1.0f},
		{TEXT("LineTraceImpulse"), LineTraceImpulse, 0.0f},
		{TEXT("LineTraceDamage"), LineTraceDamage, 0.0f},
		{TEXT("LineTraceHeadDamage"), LineTraceHeadDamage, 0.0f},
		{TEXT("HipFireSpreadAngle"), HipFireSpreadAngle, 0.0f, 45.0f},
		{TEXT("AimFireSpreadAngle"), AimFireSpreadAngle, 0.0f, 45.0f},
		{TEXT("ContinuousFireSpreadStep"), ContinuousFireSpreadStep, 0.0f, 45.0f},
		{TEXT("MaxContinuousFireSpreadAngle"), MaxContinuousFireSpreadAngle, 0.0f, 45.0f},
		{TEXT("SpreadResetDelay"), SpreadResetDelay, 0.0f},
		{TEXT("RecoilPitch"), RecoilPitch, 0.0f},
		{TEXT("RecoilYaw"), RecoilYaw, 0.0f},
		{TEXT("AimRecoilMultiplier"), AimRecoilMultiplier, 0.0f, 1.0f},
		{TEXT("RecoilRecoveryDelay"), RecoilRecoveryDelay, 0.0f},
		{TEXT("RecoilRecoverySpeed"), RecoilRecoverySpeed, 0.1f},
		{TEXT("MaxAccumulatedRecoilPitch"), MaxAccumulatedRecoilPitch, 0.0f},
		{TEXT("MaxAccumulatedRecoilYaw"), MaxAccumulatedRecoilYaw, 0.0f},
		{TEXT("VisualRecoilScale"), VisualRecoilScale, 0.0f, 2.0f},
		{TEXT("AimVisualRecoilMultiplier"), AimVisualRecoilMultiplier, 0.0f, 1.0f},
		{TEXT("VisualRecoilFOV"), VisualRecoilFOV, 0.0f, 3.0f},
		{TEXT("VisualRecoilDuration"), VisualRecoilDuration, 0.04f, 0.5f},
		{TEXT("ReloadDuration"), ReloadDuration, 0.1f},
		{TEXT("EmptyReloadDuration"), EmptyReloadDuration, 0.1f},
	};
	for (const FRange& Range : Ranges)
	{
		if (!FMath::IsFinite(Range.Value) || Range.Value < Range.Min || Range.Value > Range.Max)
		{
			OutError = FString::Printf(TEXT("%s must be finite and in [%g, %g]."), Range.Name, Range.Min, Range.Max);
			return false;
		}
	}
	// 镜头旋转轴不使用标量配置表，单独校验。
	for (const double Angle : {VisualRecoilRotation.Pitch, VisualRecoilRotation.Yaw, VisualRecoilRotation.Roll})
	{
		if (!FMath::IsFinite(Angle) || FMath::Abs(Angle) > 5.0)
		{
			OutError = TEXT("VisualRecoilRotation axes must be finite and within [-5, 5] degrees.");
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR
// 合并父类和本项目的验证结果；本组配置有效也不能覆盖父类已经报告的失败。
EDataValidationResult UfpstrueWeaponConfig::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult SuperResult = Super::IsDataValid(Context);
	FString Error;
	if (!Settings.Validate(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return SuperResult == EDataValidationResult::Invalid ? SuperResult : EDataValidationResult::Valid;
}
#endif
