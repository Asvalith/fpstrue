// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Weapons/fpstrueWeaponCameraShake.h"
#include "Weapons/fpstrueWeaponConfig.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueVisualRecoilPulseTest,
	"fpstrue.Gameplay.Weapon.VisualRecoil.PulseAndScaling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueVisualRecoilPulseTest::RunTest(const FString& Parameters)
{
	const FFPWeaponSettings Settings;
	TStrongObjectPtr<UfpstrueWeaponCameraShake> HipShake(NewObject<UfpstrueWeaponCameraShake>());
	TStrongObjectPtr<UfpstrueWeaponCameraShake> AimShake(NewObject<UfpstrueWeaponCameraShake>());
	UfpstrueWeaponRecoilPattern* HipPattern = Cast<UfpstrueWeaponRecoilPattern>(HipShake->GetRootShakePattern());
	UfpstrueWeaponRecoilPattern* AimPattern = Cast<UfpstrueWeaponRecoilPattern>(AimShake->GetRootShakePattern());
	if (!TestNotNull(TEXT("Hip shake has its native pattern"), HipPattern)
		|| !TestNotNull(TEXT("Aim shake has its native pattern"), AimPattern)) return false;
	HipPattern->Configure(Settings);
	AimPattern->Configure(Settings);
	HipShake->StartShake(nullptr, Settings.VisualRecoilScale, ECameraShakePlaySpace::CameraLocal);
	AimShake->StartShake(nullptr, Settings.VisualRecoilScale * Settings.AimVisualRecoilMultiplier, ECameraShakePlaySpace::CameraLocal);

	FMinimalViewInfo BaseView;
	BaseView.Location = FVector(110.0, -40.0, 70.0);
	BaseView.Rotation = FRotator::ZeroRotator;
	BaseView.FOV = 90.0f;
	FMinimalViewInfo HipView = BaseView;
	FMinimalViewInfo AimView = BaseView;
	const float SampleTime = Settings.VisualRecoilDuration * 0.25f;
	HipShake->UpdateAndApplyCameraShake(SampleTime, 1.0f, HipView);
	AimShake->UpdateAndApplyCameraShake(SampleTime, 1.0f, AimView);
	TestTrue(TEXT("A shot produces visible pitch"), FMath::Abs(HipView.Rotation.Pitch) > 0.001);
	TestTrue(TEXT("A shot produces visible roll"), FMath::Abs(HipView.Rotation.Roll) > 0.001);
	TestTrue(TEXT("A shot produces a FOV pulse"), FMath::Abs(HipView.FOV - BaseView.FOV) > 0.001f);
	TestTrue(TEXT("Visual recoil preserves the camera position"), HipView.Location.Equals(BaseView.Location));
	TestTrue(TEXT("Aiming reduces visible pitch"), FMath::Abs(AimView.Rotation.Pitch) < FMath::Abs(HipView.Rotation.Pitch));
	TestTrue(TEXT("Aiming reduces visible roll"), FMath::Abs(AimView.Rotation.Roll) < FMath::Abs(HipView.Rotation.Roll));
	TestTrue(TEXT("Aiming reduces the FOV pulse"),
		FMath::Abs(AimView.FOV - BaseView.FOV) < FMath::Abs(HipView.FOV - BaseView.FOV));

	HipView = BaseView;
	HipShake->UpdateAndApplyCameraShake(Settings.VisualRecoilDuration * 2.0f, 1.0f, HipView);
	TestTrue(TEXT("A single pulse finishes without a gameplay timer"), HipShake->IsFinished());
	TestTrue(TEXT("Finished recoil leaves no rotation offset"), HipView.Rotation.Equals(BaseView.Rotation));
	TestEqual(TEXT("Finished recoil leaves no FOV offset"), HipView.FOV, BaseView.FOV);

	AimShake->StopShake(true);
	AimView = BaseView;
	AimShake->UpdateAndApplyCameraShake(0.01f, 1.0f, AimView);
	TestTrue(TEXT("An immediate stop finishes the pulse"), AimShake->IsFinished());
	TestTrue(TEXT("An immediate stop removes visual rotation"), AimView.Rotation.Equals(BaseView.Rotation));
	TestEqual(TEXT("An immediate stop removes the FOV pulse"), AimView.FOV, BaseView.FOV);
	HipShake->TeardownShake();
	AimShake->TeardownShake();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueVisualRecoilRetriggerTest,
	"fpstrue.Gameplay.Weapon.VisualRecoil.RetriggerAndStepSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueVisualRecoilRetriggerTest::RunTest(const FString& Parameters)
{
	const FFPWeaponSettings Settings;
	TStrongObjectPtr<UfpstrueWeaponCameraShake> Shake(NewObject<UfpstrueWeaponCameraShake>());
	TStrongObjectPtr<UfpstrueWeaponCameraShake> FineStepShake(NewObject<UfpstrueWeaponCameraShake>());
	UfpstrueWeaponRecoilPattern* Pattern = Cast<UfpstrueWeaponRecoilPattern>(Shake->GetRootShakePattern());
	UfpstrueWeaponRecoilPattern* FinePattern = Cast<UfpstrueWeaponRecoilPattern>(FineStepShake->GetRootShakePattern());
	if (!TestNotNull(TEXT("Coarse-step pattern"), Pattern) || !TestNotNull(TEXT("Fine-step pattern"), FinePattern)) return false;
	TestTrue(TEXT("Successive shots use one shake instance"), Shake->bSingleInstance);
	Pattern->Configure(Settings);
	FinePattern->Configure(Settings);
	Shake->StartShake(nullptr, 1.0f, ECameraShakePlaySpace::CameraLocal);
	FineStepShake->StartShake(nullptr, 1.0f, ECameraShakePlaySpace::CameraLocal);

	FMinimalViewInfo BaseView;
	BaseView.Rotation = FRotator::ZeroRotator;
	BaseView.FOV = 90.0f;
	FMinimalViewInfo CoarseView = BaseView;
	FMinimalViewInfo FineView = BaseView;
	const float Elapsed = Settings.VisualRecoilDuration * 0.6f;
	Shake->UpdateAndApplyCameraShake(Elapsed, 1.0f, CoarseView);
	for (int32 Step = 0; Step < 6; ++Step)
	{
		FineView = BaseView;
		FineStepShake->UpdateAndApplyCameraShake(Elapsed / 6.0f, 1.0f, FineView);
	}
	TestTrue(TEXT("The same elapsed time gives the same rotation at different update rates"),
		CoarseView.Rotation.Equals(FineView.Rotation, 0.002));
	TestTrue(TEXT("The same elapsed time gives the same FOV at different update rates"),
		FMath::IsNearlyEqual(CoarseView.FOV, FineView.FOV, 0.002f));

	Shake->StartShake(nullptr, 1.0f, ECameraShakePlaySpace::CameraLocal);
	Pattern->Configure(Settings);
	FMinimalViewInfo RestartView = BaseView;
	Shake->UpdateAndApplyCameraShake(0.0f, 1.0f, RestartView);
	TestTrue(TEXT("Retriggering preserves the current visible rotation"),
		RestartView.Rotation.Equals(CoarseView.Rotation, 0.002));
	TestTrue(TEXT("Retriggering preserves the current visible FOV"),
		FMath::IsNearlyEqual(RestartView.FOV, CoarseView.FOV, 0.002f));

	bool bBurstRemainsBounded = true;
	FMinimalViewInfo BurstView = BaseView;
	for (int32 Shot = 0; Shot < 24; ++Shot)
	{
		Shake->StartShake(nullptr, 1.0f, ECameraShakePlaySpace::CameraLocal);
		Pattern->Configure(Settings);
		BurstView = BaseView;
		Shake->UpdateAndApplyCameraShake(Settings.VisualRecoilDuration * 0.4f, 1.0f, BurstView);
		bBurstRemainsBounded &= FMath::Abs(BurstView.Rotation.Pitch) <= FMath::Abs(Settings.VisualRecoilRotation.Pitch) * 2.0 + 0.001
			&& FMath::Abs(BurstView.Rotation.Roll) <= FMath::Abs(Settings.VisualRecoilRotation.Roll) * 2.0 + 0.001
			&& FMath::Abs(BurstView.FOV - BaseView.FOV) <= FMath::Abs(Settings.VisualRecoilFOV) * 2.0f + 0.001f;
	}
	TestTrue(TEXT("Sustained retriggering does not accumulate an unbounded visual offset"), bBurstRemainsBounded);
	TestFalse(TEXT("Repeated shots keep a live pulse after the first duration has elapsed"), Shake->IsFinished());
	TestTrue(TEXT("Late shots still produce visible recoil"), FMath::Abs(BurstView.Rotation.Pitch) > 0.001);
	BurstView = BaseView;
	Shake->UpdateAndApplyCameraShake(Settings.VisualRecoilDuration * 2.0f, 1.0f, BurstView);
	TestTrue(TEXT("The burst finishes after the last shot"), Shake->IsFinished());
	TestTrue(TEXT("The burst returns to the unmodified view"), BurstView.Rotation.Equals(BaseView.Rotation));
	TestEqual(TEXT("The burst restores the unmodified FOV"), BurstView.FOV, BaseView.FOV);
	Shake->TeardownShake();
	FineStepShake->StopShake(true);
	FineStepShake->TeardownShake();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueVisualRecoilViewIsolationTest,
	"fpstrue.Gameplay.Weapon.VisualRecoil.ViewIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueVisualRecoilViewIsolationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld())
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	ACameraActor* CameraActor = World->SpawnActor<ACameraActor>();
	if (!TestNotNull(TEXT("Controller for the camera modifier"), Controller)
		|| !TestNotNull(TEXT("Unmodified gameplay camera"), CameraActor)) return false;
	APlayerCameraManager* CameraManager = Controller->PlayerCameraManager;
	if (!TestNotNull(TEXT("The controller owns a camera manager"), CameraManager)) return false;
	Controller->SetControlRotation(FRotator(4.0, 18.0, 0.0));
	Controller->SetViewTarget(CameraActor);
	UCameraComponent* Camera = CameraActor->GetCameraComponent();
	const FTransform CameraTransform = Camera->GetComponentTransform();
	const FVector ShotForward = Camera->GetForwardVector();
	const FRotator ControlRotation = Controller->GetControlRotation();
	FMinimalViewInfo BaseView;
	Camera->GetCameraView(0.0f, BaseView);

	UCameraShakeBase* Shake = CameraManager->StartCameraShake(UfpstrueWeaponCameraShake::StaticClass(), 1.0f);
	if (!TestNotNull(TEXT("The camera manager accepts visual recoil"), Shake)) return false;
	UfpstrueWeaponRecoilPattern* Pattern = Cast<UfpstrueWeaponRecoilPattern>(Shake->GetRootShakePattern());
	if (!TestNotNull(TEXT("The managed shake has the native recoil pattern"), Pattern)) return false;
	const FFPWeaponSettings Settings;
	Pattern->Configure(Settings);
	FMinimalViewInfo ShakenView = BaseView;
	CameraManager->ApplyCameraModifiers(Settings.VisualRecoilDuration * 0.25f, ShakenView);
	TestFalse(TEXT("The final view receives visual recoil"), ShakenView.Rotation.Equals(BaseView.Rotation, 0.001));
	TestTrue(TEXT("Visual recoil does not write back control rotation"), Controller->GetControlRotation().Equals(ControlRotation));
	TestTrue(TEXT("Visual recoil does not move the camera component"), Camera->GetComponentTransform().Equals(CameraTransform));
	TestTrue(TEXT("The hitscan forward vector is unchanged"), Camera->GetForwardVector().Equals(ShotForward));
	TestTrue(TEXT("The camera manager reuses the same shake for another shot"),
		CameraManager->StartCameraShake(UfpstrueWeaponCameraShake::StaticClass(), 1.0f) == Shake);
	CameraManager->StopCameraShake(Shake, true);
	ShakenView = BaseView;
	CameraManager->ApplyCameraModifiers(0.01f, ShakenView);
	TestTrue(TEXT("Stopping the managed shake restores the final view"), ShakenView.Rotation.Equals(BaseView.Rotation));
	TestEqual(TEXT("Stopping the managed shake restores the final FOV"), ShakenView.FOV, BaseView.FOV);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueVisualRecoilConfigurationTest,
	"fpstrue.Gameplay.Weapon.VisualRecoil.ConfigurationValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueVisualRecoilConfigurationTest::RunTest(const FString& Parameters)
{
	FFPWeaponSettings Settings;
	FString Error;
	TestTrue(TEXT("Default visual recoil settings are valid"), Settings.Validate(Error));
	Settings.VisualRecoilDuration = 0.0f;
	TestFalse(TEXT("A zero visual recoil duration is rejected before playback"), Settings.Validate(Error));
	TestTrue(TEXT("Invalid duration identifies its setting"), Error.Contains(TEXT("VisualRecoilDuration")));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilScale = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("A non-finite visual scale is rejected"), Settings.Validate(Error));
	TestTrue(TEXT("Invalid scale identifies its setting"), Error.Contains(TEXT("VisualRecoilScale")));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilRotation.Pitch = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("A non-finite visual rotation is rejected"), Settings.Validate(Error));
	TestTrue(TEXT("Invalid rotation identifies its setting"), Error.Contains(TEXT("VisualRecoilRotation")));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilScale = 0.0f;
	TestTrue(TEXT("Zero visual scale remains a valid way to disable the effect"), Settings.Validate(Error));
	return true;
}

#endif
