// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderBudgetBoundaryTest, "fpstrue.Rendering.EnemyBudget.MeshOwnershipAndDeath",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRenderBudgetBoundaryTest::RunTest(const FString& Parameters)
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
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Native enemy without a controller"), Enemy))
		return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->GetMesh()->SetCastShadow(true);
	Enemy->GetMesh()->SetVisibleInRayTracing(true);
	Enemy->FinishSpawning(FTransform::Identity);
	const auto AddOwnedMesh = [Enemy](bool bAuthoredEnabled)
	{
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Enemy);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCastShadow(bAuthoredEnabled);
		Mesh->SetVisibleInRayTracing(bAuthoredEnabled);
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Mesh->SetupAttachment(Enemy->GetRootComponent());
		Enemy->AddInstanceComponent(Mesh);
		Mesh->RegisterComponent();
		return Mesh;
	};
	UStaticMeshComponent* Accessory = AddOwnedMesh(true);
	UStaticMeshComponent* AuthoredOff = AddOwnedMesh(false);

	// ChildActor 所有权进入预算；普通 Attach 的独立 Actor 不进入。
	UChildActorComponent* ChildComponent = NewObject<UChildActorComponent>(Enemy);
	ChildComponent->SetMobility(EComponentMobility::Static);
	ChildComponent->SetChildActorClass(AStaticMeshActor::StaticClass());
	Enemy->AddInstanceComponent(ChildComponent);
	ChildComponent->RegisterComponent();
	AStaticMeshActor* Child = Cast<AStaticMeshActor>(ChildComponent->GetChildActor());
	if (!TestNotNull(TEXT("Owned ChildActor is created"), Child))
		return false;
	UStaticMeshComponent* ChildMesh = Child->GetStaticMeshComponent();
	ChildMesh->SetCastShadow(true);
	ChildMesh->SetVisibleInRayTracing(true);
	AStaticMeshActor* External = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("Independent attachment actor"), External))
		return false;
	UStaticMeshComponent* ExternalMesh = External->GetStaticMeshComponent();
	ExternalMesh->SetMobility(EComponentMobility::Movable);
	ExternalMesh->SetCastShadow(true);
	ExternalMesh->SetVisibleInRayTracing(true);
	TestTrue(TEXT("External actor is attached without transferring ownership"),
			 External->AttachToComponent(Enemy->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform));
	Enemy->RefreshRenderBudgetMeshes();

	int32 Meshes = 0, ShadowMeshes = 0, RayTracingMeshes = 0;
	const auto CheckCounts = [this, Enemy, &Meshes, &ShadowMeshes, &RayTracingMeshes](int32 ExpectedMeshes, int32 ExpectedEnabled)
	{
		Enemy->GetRenderBudgetMeshCounts(Meshes, ShadowMeshes, RayTracingMeshes);
		TestEqual(TEXT("Owned mesh count reads the actual cached set"), Meshes, ExpectedMeshes);
		TestEqual(TEXT("Shadow count reads actual component flags"), ShadowMeshes, ExpectedEnabled);
		TestEqual(TEXT("Ray tracing count reads actual component flags"), RayTracingMeshes, ExpectedEnabled);
	};
	FFPEnemyRenderSignificancePolicy Policy;
	const auto ApplyBudget = [Enemy, &Policy](bool bAllow)
	{ Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Full, bAllow, bAllow, false, Policy); };
	CheckCounts(4, 3);
	const bool bOriginalVisibility = Accessory->IsVisible();
	const ECollisionEnabled::Type OriginalCollision = Accessory->GetCollisionEnabled();
	ApplyBudget(false);
	CheckCounts(4, 0);
	TestFalse(TEXT("ChildActor mesh is budgeted"), bool(ChildMesh->CastShadow) || bool(ChildMesh->bVisibleInRayTracing));
	TestTrue(TEXT("Independent attachment retains its flags"), bool(ExternalMesh->CastShadow) && bool(ExternalMesh->bVisibleInRayTracing));
	TestEqual(TEXT("Budget does not hide an accessory"), Accessory->IsVisible(), bOriginalVisibility);
	TestTrue(TEXT("Budget does not change collision"), Accessory->GetCollisionEnabled() == OriginalCollision);
	Enemy->RefreshRenderBudgetMeshes();
	ApplyBudget(true);
	CheckCounts(4, 3);
	TestFalse(TEXT("Authored-disabled shadow is not enabled by a Full budget"), bool(AuthoredOff->CastShadow));
	TestFalse(TEXT("Authored-disabled ray tracing is not enabled by a Full budget"), bool(AuthoredOff->bVisibleInRayTracing));
	// 预算功能关闭时，调用方传入的拒绝值不应误伤原本具备资格的 Mesh。
	Policy.bEnableShadowBudget = false;
	Policy.bEnableRayTracingBudget = false;
	ApplyBudget(false);
	CheckCounts(4, 3);
	Policy.bEnableShadowBudget = true;
	Policy.bEnableRayTracingBudget = true;
	ApplyBudget(true);
	Accessory->SetCastShadow(false);
	Enemy->GetRenderBudgetMeshCounts(Meshes, ShadowMeshes, RayTracingMeshes);
	TestEqual(TEXT("Readback observes a property change outside the last budget assignment"), ShadowMeshes, 2);
	TestEqual(TEXT("Shadow readback does not alter ray tracing count"), RayTracingMeshes, 3);
	ApplyBudget(true);
	CheckCounts(4, 3);

	// 每轮分配只消费缓存；动态安装显式刷新，销毁后清理失效条目。
	ApplyBudget(false);
	UStaticMeshComponent* DynamicMesh = AddOwnedMesh(true);
	ApplyBudget(false);
	CheckCounts(4, 0);
	TestTrue(TEXT("An unrefreshed new component has not been silently discovered"),
			 bool(DynamicMesh->CastShadow) && bool(DynamicMesh->bVisibleInRayTracing));
	Enemy->RefreshRenderBudgetMeshes();
	CheckCounts(5, 0);
	TestFalse(TEXT("Refresh applies the current denial to the new component"),
			  bool(DynamicMesh->CastShadow) || bool(DynamicMesh->bVisibleInRayTracing));
	DynamicMesh->DestroyComponent();
	Enemy->RefreshRenderBudgetMeshes();
	CheckCounts(4, 0);
	ApplyBudget(true);
	CheckCounts(4, 3);

	// 走真实伤害/死亡广播，不直接调用私有死亡实现。
	UfpstrueHealthComponent* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>();
	if (!TestNotNull(TEXT("Enemy health component"), Health))
		return false;
	Health->SetMaxHealthAndReset(100.0f);
	UGameplayStatics::ApplyDamage(Enemy, 1000.0f, nullptr, nullptr, UDamageType::StaticClass());
	if (!TestTrue(TEXT("Damage triggered real enemy death"), Enemy->IsDead()))
		return false;
	CheckCounts(4, 0);
	TestFalse(TEXT("Death revokes the main skeletal mesh flags"),
			  bool(Enemy->GetMesh()->CastShadow) || bool(Enemy->GetMesh()->bVisibleInRayTracing));
	const ECollisionEnabled::Type DeathCollision = Enemy->GetMesh()->GetCollisionEnabled();
	const bool bDeathVisibility = Enemy->GetMesh()->IsVisible();
	const bool bDeathPhysics = Enemy->GetMesh()->IsSimulatingPhysics();
	TestTrue(TEXT("Death retains its ragdoll collision policy"), DeathCollision == ECollisionEnabled::QueryAndPhysics);
	ApplyBudget(true);
	Enemy->RefreshRenderBudgetMeshes();
	CheckCounts(4, 0);
	UStaticMeshComponent* DeathAccessory = AddOwnedMesh(true);
	Enemy->RefreshRenderBudgetMeshes();
	ApplyBudget(true);
	CheckCounts(5, 0);
	TestFalse(TEXT("A post-death attachment cannot regain rendering eligibility"),
			  bool(DeathAccessory->CastShadow) || bool(DeathAccessory->bVisibleInRayTracing));
	TestTrue(TEXT("Budget refresh does not change corpse collision"), Enemy->GetMesh()->GetCollisionEnabled() == DeathCollision);
	TestEqual(TEXT("Budget refresh does not hide the corpse"), Enemy->GetMesh()->IsVisible(), bDeathVisibility);
	TestEqual(TEXT("Budget refresh does not change the corpse physics state"), Enemy->GetMesh()->IsSimulatingPhysics(), bDeathPhysics);
	TestTrue(TEXT("Death still does not take over the independently attached actor"),
			 bool(ExternalMesh->CastShadow) && bool(ExternalMesh->bVisibleInRayTracing));
	// 在 World 上下文仍有效时退出 ChildActor，避免测试夹具拆世界时产生无关注销警告。
	Enemy->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderHysteresisTest, "fpstrue.Rendering.EnemyBudget.TierHysteresis",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRenderHysteresisTest::RunTest(const FString& Parameters)
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
	AfpstrueEnemyCharacter* Enemy =
		World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Tier hysteresis enemy"), Enemy))
		return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);

	using Tier = EFPEnemyRenderSignificanceTier;
	FFPEnemyRenderSignificancePolicy Policy;
	Policy.MinimumTierHoldSeconds = 0.5f;
	Policy.DemotionDelaySeconds = 0.35f;
	const auto CheckTier = [this, Enemy, &Policy](float Score, Tier Expected, const TCHAR* Reason)
	{
		FFPEnemyRenderSignificanceSample Sample;
		Sample.Score = Score;
		TestTrue(Reason, Enemy->ResolveNaturalRenderSignificanceTier(Sample, Policy) == Expected);
	};
	CheckTier(0.5f, Tier::Full, TEXT("Demotion waits instead of changing immediately"));
	World->Tick(LEVELTICK_All, 0.4f);
	CheckTier(0.5f, Tier::Full, TEXT("Demotion delay alone cannot bypass minimum hold"));
	World->Tick(LEVELTICK_All, 0.2f);
	CheckTier(0.5f, Tier::Reduced, TEXT("Full demotes to Reduced after both gates"));
	CheckTier(0.65f, Tier::Reduced, TEXT("Promotion uses its enter threshold"));
	CheckTier(0.75f, Tier::Full, TEXT("Promotion is immediate despite minimum hold"));
	CheckTier(0.1f, Tier::Full, TEXT("A new Background demotion starts its own delay"));
	World->Tick(LEVELTICK_All, 0.2f);
	CheckTier(0.8f, Tier::Full, TEXT("Recovered score cancels pending demotion"));
	CheckTier(0.1f, Tier::Full, TEXT("Returning below the threshold restarts the delay"));
	World->Tick(LEVELTICK_All, 0.2f);
	CheckTier(0.1f, Tier::Full, TEXT("Cancelled delay is not reused"));
	World->Tick(LEVELTICK_All, 0.2f);
	CheckTier(0.1f, Tier::Background, TEXT("Full can demote directly to Background"));
	CheckTier(0.4f, Tier::Reduced, TEXT("Background promotes immediately to Reduced"));
	CheckTier(0.75f, Tier::Full, TEXT("Reduced promotes immediately to Full"));
	CheckTier(0.1f, Tier::Full, TEXT("A later demotion is pending"));
	Policy.bEnableRenderTiering = false;
	CheckTier(0.1f, Tier::Full, TEXT("Disabling tiering clears pending demotion"));
	World->Tick(LEVELTICK_All, 0.6f);
	Policy.bEnableRenderTiering = true;
	CheckTier(0.1f, Tier::Full, TEXT("Re-enabling does not inherit an old pending demotion"));
	World->Tick(LEVELTICK_All, 0.4f);
	CheckTier(0.1f, Tier::Background, TEXT("Re-enabled policy demotes after a fresh delay"));
	Policy.bEnableRenderTiering = false;
	CheckTier(0.0f, Tier::Full, TEXT("Disabling restores Full from Background"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
