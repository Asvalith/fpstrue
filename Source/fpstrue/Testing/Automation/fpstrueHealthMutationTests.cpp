#include "Testing/Automation/fpstrueTestObservers.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Kismet/GameplayStatics.h"

// 这些测试验证生命事务与通知顺序；不以无渲染测试世界代替受击、布娃娃或 GPU 画面验收。
// 在伤害通知内提交两项后续修改，检查组件能否在当前事务通知完后按顺序执行。
void UfpstrueHealthMutationTestObserver::OnDamage(float Amount, AActor* Causer, AController* Instigator)
{
	if (bResetDuringDamage && Health.IsValid())
	{
		bResetDuringDamage = false;
		Health->ResetHealth();
		UGameplayStatics::ApplyDamage(Health->GetOwner(), 10.0f, nullptr, nullptr, nullptr);
	}
}

// 保存每次广播的值；按测试开关构造重置、交替最大生命值或二次伤害。
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

// 只记录死亡次数，避免接收者本身改变被测死亡结算行为。
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

// 验证伤害/血量广播中的同步重入会排队，且死亡通知不会被监听者的重置吞掉。
bool FFpstrueHealthMutationTest::RunTest(const FString& Parameters)
{
	// 准备独立测试世界、生产生命组件和可重入的反射接收者。
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

	// 致死伤害的监听者请求重置和再次扣血：先完成死亡，再依次处理后续事务。
	Observer->bResetDuringDamage = true;
	UGameplayStatics::ApplyDamage(Owner, 100.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("A reset in Damage cannot swallow the committed death"), Observer->Deaths, 1);
	TestTrue(TEXT("Snapshots are damage, queued reset, queued damage"), Observer->HealthSnapshots == TArray<float>({0.0f, 100.0f, 90.0f}));
	TestEqual(TEXT("Nested damage applies after queued reset"), Health->GetHealth(), 90.0f);

	// 新生命会话中再次构造致死重入，并检查死亡后的额外伤害不会重复结算。
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

// 验证无变化的重置能收敛，恶意交替修改受事务数量上限限制，后续独立请求仍可运行。
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
	// 相同数值的反复重置不再广播，避免普通监听者形成无限通知循环。
	Observer->bAlwaysResetDuringHealthChange = true;
	UGameplayStatics::ApplyDamage(Owner, 10.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("An unconditional Reset listener reaches a fixed point"), Observer->HealthSnapshots == TArray<float>({90.0f, 100.0f}));
	Health->ResetHealth();
	TestEqual(TEXT("A no-op reset publishes no change"), Observer->HealthSnapshots.Num(), 2);
	TestEqual(TEXT("Ordinary repeated Reset needs no guard rejection"), Health->GetRejectedMutationCount(), 0);
	// 交替最大生命值永不自然收敛：检查达到上限后拒绝新请求并输出诊断。
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

// 验证受击立即保护骨骼更新，并确认排队伤害不会覆盖真正致死命中的部位、方向和位置。
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
	// 先让低频网格进入真实冷却，再施加伤害；不等待全局协调器也必须及时恢复动画更新。
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

	// 从首个伤害回调排入 B、C 两次命中；B 致死后 C 不应替换死亡表现所需的命中信息。
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
	// 移除测试委托后断言致死上下文；这里检查数据身份，不检查最终布娃娃画面。
	Health->OnDamageResolved.Remove(Handle);
	TestTrue(TEXT("B is lethal; C was queued but cannot replace the lethal context"), Enemy->IsDead());
	TestTrue(TEXT("Only A and lethal B are resolved"), AppliedBones == TArray<FName>({NAME_None, TEXT("hit_B")}));
	TestEqual(TEXT("Death keeps B's bone rather than the later received C"), Enemy->DeathDamageContext.BoneName, FName(TEXT("hit_B")));
	TestEqual(TEXT("Death keeps B's direction"), Enemy->DeathDamageContext.Direction, FVector::RightVector);
	TestEqual(TEXT("Death keeps B's location"), Enemy->DeathDamageContext.Location, FVector(1.0, 2.0, 3.0));
	return true;
}
#endif
