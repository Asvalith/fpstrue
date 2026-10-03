// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponConfig.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

bool FFPWeaponSettings::Validate(FString& OutError) const
{
	OutError.Reset();
	if (GripSocketName.IsNone() || MagazineSize < 1 || StartingReserveAmmo < 0
		|| CriticalHitBones.Contains(NAME_None))
	{
		OutError = TEXT("GripSocketName/bone entries must be named; MagazineSize >= 1 and StartingReserveAmmo >= 0.");
		return false;
	}
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
		{TEXT("ReloadDuration"), ReloadDuration, 0.1f},
		{TEXT("EmptyReloadDuration"), EmptyReloadDuration, 0.1f},
		{TEXT("ReloadFailSafeDuration"), ReloadFailSafeDuration, 0.1f},
		{TEXT("ReloadCompletionGracePeriod"), ReloadCompletionGracePeriod, 0.0f},
	};
	for (const FRange& Range : Ranges)
	{
		if (!FMath::IsFinite(Range.Value) || Range.Value < Range.Min || Range.Value > Range.Max)
		{
			OutError = FString::Printf(TEXT("%s must be finite and in [%g, %g]."), Range.Name, Range.Min, Range.Max);
			return false;
		}
	}
	if (!FMath::IsFinite(FMath::Max3(ReloadDuration, EmptyReloadDuration, ReloadFailSafeDuration) + ReloadCompletionGracePeriod))
	{
		OutError = TEXT("Reload timeout plus grace period must be finite.");
		return false;
	}
	return true;
}

#if WITH_EDITOR
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
