#include "Testing/Automation/fpstrueHealthMutationTestObserver.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Kismet/GameplayStatics.h"

void UfpstrueHealthMutationTestObserver::OnDamage(float Amount, AActor* Causer, AController* Instigator)
{
	if (bResetDuringDamage && Health.IsValid())
	{
		bResetDuringDamage = false;
		Health->ResetHealth();
		UGameplayStatics::ApplyDamage(Health->GetOwner(), 10.0f, nullptr, nullptr, nullptr);
	}
}

void UfpstrueHealthMutationTestObserver::OnHealth(float Value)
{
	HealthSnapshots.Add(Value);
	if (bAlwaysResetDuringHealthChange && Health.IsValid()) Health->ResetHealth();
	if (bAlternateMaximumDuringHealthChange && Health.IsValid())
	{
		Health->SetMaxHealthAndReset(Value == 100.0f ? 101.0f : 100.0f);
	}
	if (bDamageDuringHealthChange && Health.IsValid())
	{
		bDamageDuringHealthChange = false;
		UGameplayStatics::ApplyDamage(Health->GetOwner(), 100.0f, nullptr, nullptr, nullptr);
	}
}

void UfpstrueHealthMutationTestObserver::OnDeath()
{
	++Deaths;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
// 三项生命测试使用相同的无玩法 GameMode 世界，只把不同通知/命中场景留在测试正文。
bool BeginHealthTestWorld(FTestWorldWrapper& Fixture, FAutomationTestBase& Test)
{
	if (Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.GetTestWorld()->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
		if (Fixture.BeginPlayInTestWorld()) return true;
	}
	Fixture.ForwardErrorMessages(&Test);
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueHealthMutationTest, "fpstrue.Gameplay.Health.ReentrantMutationOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueHealthMutationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginHealthTestWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Health test owner"), Owner)) return false;
	UfpstrueHealthComponent* Health = NewObject<UfpstrueHealthComponent>(Owner);
	if (!TestNotNull(TEXT("Health test component"), Health)) return false;
	Owner->AddInstanceComponent(Health);
	Health->RegisterComponent();
	TStrongObjectPtr<UfpstrueHealthMutationTestObserver> Observer(NewObject<UfpstrueHealthMutationTestObserver>());
	Observer->Health = Health;
	Health->OnDamageReceived.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnDamage);
	Health->OnHealthChanged.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	Health->OnDeath.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);

	Observer->bResetDuringDamage = true;
	UGameplayStatics::ApplyDamage(Owner, 100.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("A reset in Damage cannot swallow the committed death"), Observer->Deaths, 1);
	TestTrue(TEXT("Snapshots are damage, queued reset, queued damage"), Observer->HealthSnapshots == TArray<float>({0.0f, 100.0f, 90.0f}));
	TestEqual(TEXT("Nested damage applies after queued reset"), Health->GetHealth(), 90.0f);

	Health->ResetHealth();
	Observer->HealthSnapshots.Reset();
	Observer->bDamageDuringHealthChange = true;
	UGameplayStatics::ApplyDamage(Owner, 20.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Nested lethal damage cannot reorder health snapshots"), Observer->HealthSnapshots == TArray<float>({80.0f, 0.0f}));
	TestEqual(TEXT("Exactly one death per life session"), Observer->Deaths, 2);
	UGameplayStatics::ApplyDamage(Owner, 20.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("Dead actors do not repeat death"), Observer->Deaths, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueHealthTerminationTest, "fpstrue.Gameplay.Health.NotificationTermination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueHealthTerminationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginHealthTestWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Owner"), Owner)) return false;
	UfpstrueHealthComponent* Health = NewObject<UfpstrueHealthComponent>(Owner);
	if (!TestNotNull(TEXT("Health"), Health)) return false;
	Owner->AddInstanceComponent(Health);
	Health->RegisterComponent();
	TStrongObjectPtr<UfpstrueHealthMutationTestObserver> Observer(NewObject<UfpstrueHealthMutationTestObserver>());
	Observer->Health = Health;
	Health->OnHealthChanged.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	Observer->bAlwaysResetDuringHealthChange = true;
	UGameplayStatics::ApplyDamage(Owner, 10.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("An unconditional Reset listener reaches a fixed point"), Observer->HealthSnapshots == TArray<float>({90.0f, 100.0f}));
	Health->ResetHealth();
	TestEqual(TEXT("A no-op reset publishes no change"), Observer->HealthSnapshots.Num(), 2);
	TestEqual(TEXT("Ordinary repeated Reset needs no guard rejection"), Health->GetRejectedMutationCount(), 0);
	Observer->bAlwaysResetDuringHealthChange = false;
	Observer->bAlternateMaximumDuringHealthChange = true;
	Observer->HealthSnapshots.Reset();
	AddExpectedError(TEXT("Health mutation admission limit reached"), EAutomationExpectedErrorFlags::Contains, 1);
	Health->SetMaxHealthAndReset(101.0f);
	TestEqual(TEXT("Alternating values cannot produce an unbounded dispatch"), Observer->HealthSnapshots.Num(), UfpstrueHealthComponent::MaxMutationsPerDispatch);
	TestEqual(TEXT("The next request is rejected before admission, with diagnostics"), Health->GetRejectedMutationCount(), 1);
	Observer->bAlternateMaximumDuringHealthChange = false;
	UGameplayStatics::ApplyDamage(Owner, 10.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("A later independent damage request still works"), Health->GetHealth(), 90.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyQueuedDamageContextTest, "fpstrue.Gameplay.Health.QueuedEnemyHitContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyQueuedDamageContextTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginHealthTestWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->GetMesh()->bEnableUpdateRateOptimizations = true;
	Enemy->FinishSpawning(FTransform::Identity);
	UfpstrueHealthComponent* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>();
	if (!TestNotNull(TEXT("Enemy health"), Health)) return false;
	FFPEnemyRenderSignificancePolicy Policy;
	Policy.CombatPriorityGraceSeconds = 0.05f;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, false, false, false, Policy);
	if (!Fixture.TickTestWorld(0.001f)) return false;
	const float BeforeHitTickTime = Enemy->GetMesh()->PrimaryComponentTick.GetLastTickGameTime();
	TestTrue(TEXT("The mesh ran once with an interval"), BeforeHitTickTime >= 0.0f);
	if (!Fixture.TickTestWorld(0.001f)) return false;
	TestEqual(TEXT("The mesh is actually cooling down before the hit"), Enemy->GetMesh()->PrimaryComponentTick.GetLastTickGameTime(), BeforeHitTickTime);
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("Hit protection is applied without a coordinator update"), Enemy->GetMesh()->GetComponentTickInterval(), 0.0f);
	TestFalse(TEXT("Hit protection immediately disables URO"), Enemy->GetMesh()->bEnableUpdateRateOptimizations);
	TestEqual(TEXT("Hit protection refreshes offscreen bones"), Enemy->GetMesh()->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("A hit does not grant a shadow slot"), bool(Enemy->GetMesh()->CastShadow));
	if (!Fixture.TickTestWorld(0.001f)) return false;
	TestNotEqual(TEXT("Protection releases the existing cooldown on the next schedulable tick"),
		Enemy->GetMesh()->PrimaryComponentTick.GetLastTickGameTime(), BeforeHitTickTime);
	if (!Fixture.TickTestWorld(0.1f)) return false;
	TestTrue(TEXT("Local protection expires even without a coordinator"), Enemy->GetMesh()->GetComponentTickInterval() > 0.0f);

	TArray<FName> AppliedBones;
	bool bQueueHits = true;
	const FDelegateHandle Handle = Health->OnDamageResolved.AddLambda(
		[&](float Damage, AActor*, AController*, const FFPDamageContext& Context)
		{
			AppliedBones.Add(Context.BoneName);
			if (!bQueueHits) return;
			bQueueHits = false;
			FHitResult HitB; HitB.BoneName = TEXT("hit_B"); HitB.ImpactPoint = FVector(1.0, 2.0, 3.0);
			FHitResult HitC; HitC.BoneName = TEXT("hit_C"); HitC.ImpactPoint = FVector(4.0, 5.0, 6.0);
			UGameplayStatics::ApplyPointDamage(Enemy, 98.0f, FVector::RightVector, HitB, nullptr, nullptr, nullptr);
			UGameplayStatics::ApplyPointDamage(Enemy, 1.0f, -FVector::ForwardVector, HitC, nullptr, nullptr, nullptr);
		});
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, nullptr, nullptr);
	Health->OnDamageResolved.Remove(Handle);
	TestTrue(TEXT("B is lethal; C was queued but cannot replace the lethal context"), Enemy->IsDead());
	TestTrue(TEXT("Only A and lethal B are resolved"), AppliedBones == TArray<FName>({NAME_None, TEXT("hit_B")}));
	TestEqual(TEXT("Death keeps B's bone rather than the later received C"), Enemy->DeathDamageContext.BoneName, FName(TEXT("hit_B")));
	TestEqual(TEXT("Death keeps B's direction"), Enemy->DeathDamageContext.Direction, FVector::RightVector);
	TestEqual(TEXT("Death keeps B's location"), Enemy->DeathDamageContext.Location, FVector(1.0, 2.0, 3.0));
	return true;
}
#endif
