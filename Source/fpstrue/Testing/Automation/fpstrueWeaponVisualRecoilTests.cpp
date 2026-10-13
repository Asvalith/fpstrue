// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Weapons/fpstrueWeaponComponent.h"
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

// 本组检查 CameraShake 的数学结果与视图隔离；实际手感、清晰度和 GPU 开销仍需运行时观察。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueVisualRecoilPulseTest,
	"fpstrue.Gameplay.Weapon.VisualRecoil.PulseAndScaling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证单发产生旋转/FOV 脉冲，瞄准缩小脉冲，结束或立即停止后不残留偏移。
bool FFpstrueVisualRecoilPulseTest::RunTest(const FString& Parameters)
{
	// 使用相同配置准备腰射和瞄准两种原生镜头震动，不依赖武器蓝图动画。
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

	// 在相同时间采样最终视图，比较旋转、FOV 与位置不变的边界。
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

	// 推进到自然结束，再测试强制停止；二者都应恢复未修改的基础视图。
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

// 验证震动按经过时间计算，连续重新触发保持连续且有界，最后一发结束后可归零。
bool FFpstrueVisualRecoilRetriggerTest::RunTest(const FString& Parameters)
{
	const FFPWeaponSettings Settings;
	TStrongObjectPtr<UfpstrueWeaponCameraShake> Shake(NewObject<UfpstrueWeaponCameraShake>());
	TStrongObjectPtr<UfpstrueWeaponCameraShake> FineStepShake(NewObject<UfpstrueWeaponCameraShake>());
	UfpstrueWeaponRecoilPattern* Pattern = Cast<UfpstrueWeaponRecoilPattern>(Shake->GetRootShakePattern());
	UfpstrueWeaponRecoilPattern* FinePattern = Cast<UfpstrueWeaponRecoilPattern>(FineStepShake->GetRootShakePattern());
	if (!TestNotNull(TEXT("Coarse-step pattern"), Pattern) || !TestNotNull(TEXT("Fine-step pattern"), FinePattern)) return false;
	Pattern->Configure(Settings);
	FinePattern->Configure(Settings);
	Shake->StartShake(nullptr, 1.0f, ECameraShakePlaySpace::CameraLocal);
	FineStepShake->StartShake(nullptr, 1.0f, ECameraShakePlaySpace::CameraLocal);

	FMinimalViewInfo BaseView;
	BaseView.Rotation = FRotator::ZeroRotator;
	BaseView.FOV = 90.0f;
	FMinimalViewInfo CoarseView = BaseView;
	FMinimalViewInfo FineView = BaseView;
	// 同一总时长分别一次更新与六次更新，排除震动结果依赖帧率的错误。
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

	// 模拟连发 24 次，检查重触发不会无限叠加，同时最后一发仍有可见输出。
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

// 验证视觉后坐力只修改最终渲染视图，不反写控制旋转或用于射线的相机组件方向。
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

	// 通过真实 CameraManager 应用修改器，记录视图输出和游戏计算数据的分离。
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
	// 验证连发复用同一震动对象，停止后也不留下视图旋转或 FOV 偏移。
	CameraManager->StopCameraShake(Shake, true);
	ShakenView = BaseView;
	CameraManager->ApplyCameraModifiers(0.01f, ShakenView);
	TestTrue(TEXT("Stopping the managed shake restores the final view"), ShakenView.Rotation.Equals(BaseView.Rotation));
	TestEqual(TEXT("Stopping the managed shake restores the final FOV"), ShakenView.FOV, BaseView.FOV);
	return true;
}

#endif
