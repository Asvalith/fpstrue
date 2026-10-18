// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Math/RotationMatrix.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

namespace
{
AfpstrueEnemyCharacter* SpawnHealthBarEnemy(FTestWorldWrapper& Fixture, FAutomationTestBase& Test)
{
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(&Test); return nullptr; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(&Test); return nullptr; }
	const FTransform Transform(FVector(500.0f, 0.0f, 0.0f));
	auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), Transform);
	if (Enemy != nullptr)
	{
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(Transform);
		Enemy->SetActorEnableCollision(false);
	}
	return Enemy;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWorldHealthBarModesTest,
	"fpstrue.Multiplayer.EnemyUI.WorldModesAndEventRedraw", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueWorldHealthBarModesTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	auto* Enemy = SpawnHealthBarEnemy(Fixture, *this);
	if (!TestNotNull(TEXT("Native enemy"), Enemy)) return false;
	auto* Bar = Enemy->FindComponentByClass<UfpstrueEnemyHealthBarComponent>();
	if (!TestNotNull(TEXT("Production metric-aware widget component"), Bar)) return false;
	for (const auto Mode : {EFPEnemyHealthBarMode::Off, EFPEnemyHealthBarMode::Screen,
		EFPEnemyHealthBarMode::WorldAlways, EFPEnemyHealthBarMode::WorldEvent})
	{
		Bar->Configure(Mode);
		TestTrue(TEXT("Mode is read back from the actual component"), Bar->GetPresentationMode() == Mode);
		if (Mode == EFPEnemyHealthBarMode::Off)
		{
			TestNull(TEXT("Off allocates no Widget"), Bar->GetUserWidgetObject());
			TestFalse(TEXT("Off does not tick"), Bar->IsComponentTickEnabled());
			continue;
		}
		if (!TestNotNull(TEXT("Each active mode creates the real Widget"), Bar->GetUserWidgetObject())) return false;
		TestTrue(TEXT("Only the historical baseline is screen-space"),
			Bar->GetWidgetSpace() == (Mode == EFPEnemyHealthBarMode::Screen ? EWidgetSpace::Screen : EWidgetSpace::World));
		TestTrue(TEXT("Only event-world uses manual RT redraw"), Bar->GetManuallyRedraw() == (Mode == EFPEnemyHealthBarMode::WorldEvent));
		TestTrue(TEXT("World geometry depth-tests rather than floating above occluders"), Bar->GetBlendMode() == EWidgetBlendMode::Masked);
	}
	Bar->SetHealthFraction(0.75f);
	FFPEnemyHealthBarMetrics Before;
	Bar->AppendMetrics(Before);
	Bar->SetHealthFraction(0.75f);
	FFPEnemyHealthBarMetrics After;
	Bar->AppendMetrics(After);
	TestEqual(TEXT("An unchanged health event does not rewrite the Widget"), After.ValueUpdates, Before.ValueUpdates);
	TestEqual(TEXT("An unchanged health event does not request a new texture"), After.RedrawRequests, Before.RedrawRequests);
	Bar->SetHealthFraction(0.5f);
	FFPEnemyHealthBarMetrics Changed;
	Bar->AppendMetrics(Changed);
	TestEqual(TEXT("A real health change is one event update"), Changed.ValueUpdates, After.ValueUpdates + 1);
	TestEqual(TEXT("A real health change requests one redraw"), Changed.RedrawRequests, After.RedrawRequests + 1);
	auto* Widget = Cast<UfpstrueEnemyHealthBarWidget>(Bar->GetUserWidgetObject());
	TestTrue(TEXT("The real display contains the latest health"), Widget != nullptr && FMath::IsNearlyEqual(Widget->GetHealthFraction(), 0.5f));
	Bar->ReleasePresentation();
	Bar->SetHealthFraction(1.0f);
	TestNull(TEXT("Late events cannot recreate released UI"), Bar->GetUserWidgetObject());
	TestFalse(TEXT("Death/EndPlay stops local view ticks"), Bar->IsComponentTickEnabled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWorldHealthBarViewTest,
	"fpstrue.Multiplayer.EnemyUI.WorldViewReturnAndSmoothedAnchor", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueWorldHealthBarViewTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	auto* Enemy = SpawnHealthBarEnemy(Fixture, *this);
	if (!TestNotNull(TEXT("View fixture enemy"), Enemy)) return false;
	auto* Bar = Enemy->FindComponentByClass<UfpstrueEnemyHealthBarComponent>();
	if (!TestNotNull(TEXT("View fixture bar"), Bar)) return false;
	Bar->Configure(EFPEnemyHealthBarMode::WorldEvent);
	const FVector Anchor = Bar->GetComponentLocation();
	const FVector Camera(0.0f, 0.0f, Anchor.Z);
	Bar->UpdateForView(Camera, FRotator::ZeroRotator, 90.0f, 16.0f / 9.0f);
	TestTrue(TEXT("A bar in front of the local camera is a frustum candidate"), Bar->IsInLocalView() && Bar->IsVisible());
	TestTrue(TEXT("The world quad faces the viewer, not the enemy's movement direction"),
		FVector::DotProduct(Bar->GetForwardVector(), (Camera - Anchor).GetSafeNormal()) > 0.999f);
	TestTrue(TEXT("Billboarding never changes the enemy's world anchor"), Bar->GetComponentLocation().Equals(Anchor, 0.001f));
	FFPEnemyHealthBarMetrics Visible;
	Bar->AppendMetrics(Visible);
	Bar->UpdateForView(Camera, FRotator(0.0f, 180.0f, 0.0f), 90.0f, 16.0f / 9.0f);
	TestFalse(TEXT("Looking away hides the world quad without destroying its Widget"), Bar->IsInLocalView() || Bar->IsVisible());
	TestNotNull(TEXT("An offscreen bar keeps its last event-driven value"), Bar->GetUserWidgetObject());
	Bar->SetHealthFraction(0.25f);
	Bar->UpdateForView(Camera, FRotator::ZeroRotator, 90.0f, 16.0f / 9.0f);
	FFPEnemyHealthBarMetrics Returned;
	Bar->AppendMetrics(Returned);
	TestTrue(TEXT("Returning to view restores visibility"), Bar->IsInLocalView() && Bar->IsVisible());
	TestEqual(TEXT("A health change and view return request two redraws, not every view update"), Returned.RedrawRequests, Visible.RedrawRequests + 2);
	Bar->UpdateForView(Camera, FRotator::ZeroRotator, 90.0f, 16.0f / 9.0f);
	FFPEnemyHealthBarMetrics Stable;
	Bar->AppendMetrics(Stable);
	TestEqual(TEXT("A stationary visible view does not redraw event-world textures"), Stable.RedrawRequests, Returned.RedrawRequests);
	const FRotator EnemyRotation = Enemy->GetActorRotation();
	for (const FRotator ViewRotation : {FRotator(-58.57f, 90.0f, 0.0f), FRotator(-90.0f, 90.0f, 0.0f), FRotator(-45.0f, -20.0f, 17.0f)})
	{
		const FRotationMatrix ViewBasis(ViewRotation);
		const FVector Forward = ViewBasis.GetUnitAxis(EAxis::X);
		const FVector Right = ViewBasis.GetUnitAxis(EAxis::Y);
		const FVector Up = ViewBasis.GetUnitAxis(EAxis::Z);
		// 让血条明显偏离光轴，不能只用画面中心的特例掩盖俯视径向倾斜。
		const FVector ViewLocation = Anchor - Forward * 1800.0f + Right * 600.0f + Up * 250.0f;
		Bar->UpdateForView(ViewLocation, ViewRotation, 90.0f, 16.0f / 9.0f);
		TestTrue(TEXT("An off-axis bar stays in the tested view"), Bar->IsInLocalView());
		TestTrue(TEXT("World-plane normal is opposite the real camera forward"), FVector::DotProduct(Bar->GetForwardVector(), -Forward) > 0.9999f);
		TestTrue(TEXT("Oblique, vertical and rolled cameras retain the same screen-up axis"), FVector::DotProduct(Bar->GetUpVector(), Up) > 0.9999f);
		const FVector LeftEdge = Anchor + Bar->GetRightVector() * 40.0f - ViewLocation;
		const FVector RightEdge = Anchor - Bar->GetRightVector() * 40.0f - ViewLocation;
		const double LeftScreenY = FVector::DotProduct(LeftEdge, Up) / FVector::DotProduct(LeftEdge, Forward);
		const double RightScreenY = FVector::DotProduct(RightEdge, Up) / FVector::DotProduct(RightEdge, Forward);
		TestTrue(TEXT("A projected health bar has no unintended screen roll"), FMath::IsNearlyEqual(LeftScreenY, RightScreenY, 0.000001));
		TestTrue(TEXT("Per-view billboarding does not rotate the enemy"), Enemy->GetActorRotation().Equals(EnemyRotation, 0.001f));
	}
	const FVector Offset(15.0f, 10.0f, 5.0f);
	Enemy->GetMesh()->AddLocalOffset(Offset);
	TestTrue(TEXT("Only the visual mesh moves the attached health-bar anchor"), Bar->GetAttachParent() == Enemy->GetMesh()
		&& !Bar->GetComponentLocation().Equals(Anchor, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWorldHealthBarServerTest,
	"fpstrue.Multiplayer.EnemyUI.DedicatedServerHasNoPresentation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueWorldHealthBarServerTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	auto* Enemy = SpawnHealthBarEnemy(Fixture, *this);
	if (!TestNotNull(TEXT("Dedicated fixture enemy"), Enemy)) return false;
	UWorld* World = Fixture.GetTestWorld();
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldType(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	auto* Bar = Enemy->FindComponentByClass<UfpstrueEnemyHealthBarComponent>();
	if (!TestNotNull(TEXT("Dedicated fixture component"), Bar)) return false;
	Bar->Configure(EFPEnemyHealthBarMode::WorldEvent);
	Bar->SetHealthFraction(0.5f);
	TestNull(TEXT("Dedicated mode never creates a Widget"), Bar->GetUserWidgetObject());
	TestFalse(TEXT("Dedicated mode never ticks UI"), Bar->IsComponentTickEnabled());
	TestTrue(TEXT("Dedicated mode reads back Off regardless of requested client presentation"), Bar->GetPresentationMode() == EFPEnemyHealthBarMode::Off);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWorldHealthBarViewCacheTest,
	"fpstrue.Multiplayer.EnemyUI.WorldViewSnapshotCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 复用生产缓存/预计算/可见性入口；没有真实 viewport 的夹具只替换相机采样，不伪装渲染性能结果。
bool FFpstrueWorldHealthBarViewCacheTest::RunTest(const FString&)
{
	using FView = UfpstrueEnemyHealthBarComponent::FViewSnapshot;
	FTestWorldWrapper Fixture;
	auto SecondFixture = MakeUnique<FTestWorldWrapper>();
	auto* Enemy = SpawnHealthBarEnemy(Fixture, *this);
	auto* OtherEnemy = SpawnHealthBarEnemy(*SecondFixture, *this);
	if (!TestNotNull(TEXT("View-cache first enemy"), Enemy) || !TestNotNull(TEXT("View-cache other-world enemy"), OtherEnemy)) return false;
	UWorld* World = Fixture.GetTestWorld();
	UWorld* OtherWorld = SecondFixture->GetTestWorld();
	TMap<UWorld*, APlayerController*> Viewers;
	TMap<UWorld*, FVector> Locations;
	for (UWorld* ViewWorld : {World, OtherWorld})
	{
		auto* Viewer = ViewWorld->SpawnActor<APlayerController>();
		auto* Camera = ViewWorld->SpawnActor<APlayerCameraManager>();
		if (!TestNotNull(TEXT("View-cache viewer identity"), Viewer) || !TestNotNull(TEXT("View-cache camera identity"), Camera)) return false;
		Viewer->PlayerCameraManager = Camera;
		Viewers.Add(ViewWorld, Viewer);
		Locations.Add(ViewWorld, FVector(ViewWorld == World ? 0.0f : 800.0f, 0.0f, Enemy->FindComponentByClass<UfpstrueEnemyHealthBarComponent>()->GetComponentLocation().Z));
	}
	int32 Samples = 0;
	const auto Sampler = [&](UWorld* ViewWorld, FView& OutView)
	{
		++Samples;
		APlayerController* Viewer = Viewers.FindRef(ViewWorld);
		if (!IsValid(Viewer) || !IsValid(Viewer->PlayerCameraManager)) return false;
		OutView = UfpstrueEnemyHealthBarComponent::BuildViewSnapshot(Locations.FindChecked(ViewWorld), FRotator::ZeroRotator, 90.0f, 16.0f / 9.0f);
		OutView.Viewer = Viewer;
		OutView.CameraManager = Viewer->PlayerCameraManager.Get();
		return true;
	};
	FView View;
	bool bHit = false;
	int32 Hits = 0;
	for (int32 Index = 0; Index < 160; ++Index)
	{
		if (!TestTrue(TEXT("Same-world view request succeeds"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(World, 73, true, View, bHit, Sampler))) return false;
		Hits += bHit ? 1 : 0;
	}
	TestEqual(TEXT("160 requests in one world/frame sample the camera exactly once"), Samples, 1);
	TestEqual(TEXT("The remaining 159 requests reuse the same-frame snapshot"), Hits, 159);
	auto* Bar = Enemy->FindComponentByClass<UfpstrueEnemyHealthBarComponent>();
	Bar->Configure(EFPEnemyHealthBarMode::WorldEvent);
	Bar->UpdateForView(Locations.FindChecked(World), FRotator::ZeroRotator, 90.0f, 16.0f / 9.0f);
	const FVector Anchor = Bar->GetComponentLocation();
	const FRotator Rotation = Bar->GetComponentRotation();
	FFPEnemyHealthBarMetrics Before;
	Bar->AppendMetrics(Before);
	Bar->UpdateForView(View);
	FFPEnemyHealthBarMetrics After;
	Bar->AppendMetrics(After);
	TestTrue(TEXT("Cached and direct views preserve visibility, view-plane orientation and the smoothed world anchor"),
		Bar->IsInLocalView() && Bar->IsVisible() && Bar->GetComponentLocation().Equals(Anchor, 0.001f)
		&& Bar->GetComponentRotation().Equals(Rotation, 0.001f));
	TestEqual(TEXT("A cache hit does not add an event redraw"), After.RedrawRequests, Before.RedrawRequests);
	if (!TestTrue(TEXT("Other-world view can be sampled in the same engine frame"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(OtherWorld, 73, true, View, bHit, Sampler))) return false;
	TestFalse(TEXT("Worlds never share a cached camera"), bHit);
	TestTrue(TEXT("The other world retains its own camera location"), View.Location.Equals(Locations.FindChecked(OtherWorld), 0.001f));
	TestEqual(TEXT("Two worlds require two actual samples"), Samples, 2);
	Locations[World].X = 100.0f;
	if (!TestTrue(TEXT("The next frame samples again"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(World, 74, true, View, bHit, Sampler))) return false;
	TestFalse(TEXT("Camera changes are not delayed into another frame"), bHit);
	TestTrue(TEXT("The current frame sees the new camera location"), View.Location.Equals(Locations.FindChecked(World), 0.001f));
	TestEqual(TEXT("Advancing one frame performs one additional sample"), Samples, 3);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TestTrue(TEXT("Per-component A/B fallback still obtains a valid view"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(World, 74, false, View, bHit, Sampler));
		TestFalse(TEXT("Per-component fallback never counts a shared hit"), bHit);
	}
	TestEqual(TEXT("The A/B fallback samples separately for every request"), Samples, 5);
	Viewers.FindChecked(World)->PlayerCameraManager->Destroy();
	TestFalse(TEXT("A destroyed camera cannot keep serving the cached same-frame view"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(World, 74, true, View, bHit, Sampler));
	TestFalse(TEXT("A failed sample is not retained as a valid cache entry"), UfpstrueEnemyHealthBarComponent::GetViewCache().Worlds.Contains(TWeakObjectPtr<UWorld>(World)));
	Viewers.FindChecked(World)->PlayerCameraManager = World->SpawnActor<APlayerCameraManager>();
	if (!TestNotNull(TEXT("A replacement local camera exists"), Viewers.FindChecked(World)->PlayerCameraManager.Get())) return false;
	TestTrue(TEXT("A replacement camera recovers within the same frame"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(World, 74, true, View, bHit, Sampler));
	TestFalse(TEXT("Recovery resamples rather than using the invalid camera"), bHit);
	TestEqual(TEXT("Invalid view and same-frame recovery each invoke the sampler once"), Samples, 7);
	TestTrue(TEXT("Later components can reuse the recovered camera"), UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(World, 74, true, View, bHit, Sampler));
	TestTrue(TEXT("Recovered same-frame view is a cache hit"), bHit);
	TestEqual(TEXT("The recovered cache needs no third sample"), Samples, 7);
	const TWeakObjectPtr<UWorld> OldWorld(OtherWorld);
	SecondFixture.Reset();
	TestFalse(TEXT("Real world cleanup removes its entry without a global per-frame scan"), UfpstrueEnemyHealthBarComponent::GetViewCache().Worlds.Contains(OldWorld));
	TestTrue(TEXT("Cleaning another world does not remove the active world's view"), UfpstrueEnemyHealthBarComponent::GetViewCache().Worlds.Contains(TWeakObjectPtr<UWorld>(World)));
	return true;
}

#endif
