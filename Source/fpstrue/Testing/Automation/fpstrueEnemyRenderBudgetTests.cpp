// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/Performance/fpstrueEnemySignificance.h"
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

#include <limits>

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
	Enemy->GetMesh()->bEnableUpdateRateOptimizations = true;
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
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, false, false, true, Policy);
	TestEqual(TEXT("Gameplay protection refreshes offscreen bones"), Enemy->GetMesh()->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("Gameplay protection disables URO independently of render budget"), Enemy->GetMesh()->bEnableUpdateRateOptimizations);
	TestEqual(TEXT("Gameplay protection removes tick throttling"), Enemy->GetMesh()->GetComponentTickInterval(), 0.0f);
	TestFalse(TEXT("Animation protection grants no extra shadow budget"), bool(Enemy->GetMesh()->CastShadow));
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, true, true, false, Policy);
	TestTrue(TEXT("Releasing protection restores authored URO"), Enemy->GetMesh()->bEnableUpdateRateOptimizations);
	TestEqual(TEXT("Releasing protection restores authored visibility ticking"), Enemy->GetMesh()->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered);
	TestTrue(TEXT("Background animation interval is restored"), Enemy->GetMesh()->GetComponentTickInterval() > 0.0f);
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

// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderPriorityOrderTest,
	"fpstrue.Performance.Significance.RenderPriorityOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRenderPriorityOrderTest::RunTest(const FString& Parameters)
{
	// A/B 和 B/C 在旧 IsNearlyEqual 比较器中会走 ID，A/C 则会走评分，能够组成比较环。
	const FFPEnemyRenderPriorityKey A{0.050000000f, 1, false, false};
	const FFPEnemyRenderPriorityKey B{0.050000007f, 2, false, false};
	const FFPEnemyRenderPriorityKey C{0.050000014f, 3, false, false};
	const FFPEnemyRenderPriorityKey SameScoreLowerId{A.Score, 0, false, false};
	const FFPEnemyRenderPriorityKey Expanded{0.0f, 4, false, true};
	const FFPEnemyRenderPriorityKey Primary{0.0f, 5, true, true};
	const FFPEnemyRenderPriorityKey InvalidNaN{std::numeric_limits<float>::quiet_NaN(), 6, false, false};
	const FFPEnemyRenderPriorityKey InvalidInfinity{std::numeric_limits<float>::infinity(), 7, false, false};
	const TArray<FFPEnemyRenderPriorityKey> Keys{
		A, B, C, SameScoreLowerId, Expanded, Primary, InvalidNaN, InvalidInfinity};
	const FFPEnemyRenderPriorityLess Before;

	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("A key never sorts before itself (%d)"), Index), Before(Keys[Index], Keys[Index]));
	}
	for (int32 LeftIndex = 0; LeftIndex < Keys.Num(); ++LeftIndex)
	{
		for (int32 MiddleIndex = 0; MiddleIndex < Keys.Num(); ++MiddleIndex)
		{
			for (int32 RightIndex = 0; RightIndex < Keys.Num(); ++RightIndex)
			{
				if (Before(Keys[LeftIndex], Keys[MiddleIndex]) && Before(Keys[MiddleIndex], Keys[RightIndex]))
				{
					TestTrue(
						FString::Printf(TEXT("The order is transitive (%d, %d, %d)"), LeftIndex, MiddleIndex, RightIndex),
						Before(Keys[LeftIndex], Keys[RightIndex]));
				}
			}
		}
	}

	TArray<FFPEnemyRenderPriorityKey> Sorted = Keys;
	Sorted.Sort(Before);
	const TArray<uint32> ExpectedIds{5, 4, 3, 2, 0, 1, 6, 7};
	TestEqual(TEXT("All priority keys remain in the sorted output"), Sorted.Num(), ExpectedIds.Num());
	for (int32 Index = 0; Index < Sorted.Num() && Index < ExpectedIds.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("Priority position %d"), Index), Sorted[Index].TieBreakId, ExpectedIds[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderTopKTest,
	"fpstrue.Performance.Significance.RenderTopKSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRenderTopKTest::RunTest(const FString& Parameters)
{
	// 输入故意打乱；Top-K 集合必须只由优先级键决定，不能依赖注册或遍历顺序。
	const TArray<FFPEnemyRenderPriorityKey> Keys{
		{0.4f, 40, false, false},
		{0.1f, 10, true, true},
		{0.9f, 90, false, false},
		{0.2f, 20, false, true},
		{0.8f, 80, false, false},
		{0.3f, 30, true, true},
	};

	TArray<FFPEnemyRenderPriorityKey> FullySorted = Keys;
	FullySorted.Sort(FFPEnemyRenderPriorityLess{});
	const auto SelectIds = [&Keys](int32 Budget, bool bReverseInput)
	{
		TArray<FFPEnemyRenderTopKEntry, TInlineAllocator<8>> Heap;
		for (int32 Step = 0; Step < Keys.Num(); ++Step)
		{
			const int32 Index = bReverseInput ? Keys.Num() - 1 - Step : Step;
			FPEnemyRenderTopKInsert(Heap, Budget, Index, Keys[Index]);
		}

		TArray<uint32> SelectedIds;
		for (const FFPEnemyRenderTopKEntry& Entry : Heap)
		{
			SelectedIds.Add(Entry.PriorityKey.TieBreakId);
		}
		SelectedIds.Sort();
		return SelectedIds;
	};

	for (int32 Budget = 0; Budget <= Keys.Num() + 1; ++Budget)
	{
		TArray<uint32> ExpectedIds;
		for (int32 Index = 0; Index < FMath::Min(Budget, FullySorted.Num()); ++Index)
		{
			ExpectedIds.Add(FullySorted[Index].TieBreakId);
		}
		ExpectedIds.Sort();

		for (const bool bReverseInput : {false, true})
		{
			const TArray<uint32> SelectedIds = SelectIds(Budget, bReverseInput);
			TestEqual(FString::Printf(TEXT("Budget %d selects the expected count"), Budget), SelectedIds.Num(), ExpectedIds.Num());
			for (int32 Index = 0; Index < SelectedIds.Num() && Index < ExpectedIds.Num(); ++Index)
			{
				TestEqual(FString::Printf(TEXT("Budget %d direction %d selected ID %d"), Budget, bReverseInput ? 1 : 0, Index),
					SelectedIds[Index], ExpectedIds[Index]);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
