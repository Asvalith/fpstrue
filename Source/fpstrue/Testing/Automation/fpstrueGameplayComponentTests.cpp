#include "Testing/Automation/fpstrueTestObservers.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Class.h"

// 观察真实包装入口，不手工调用伤害处理器，也不跳过服务器生命结算。
void AfpstrueDamagePresentationTestCharacter::ProcessEvent(UFunction* Function, void* Parameters)
{
	if (Function != nullptr)
	{
		if (Function->GetFName() == TEXT("ClientReceiveDamage")) ++OwnerDamageFeedbackRequests;
		else if (Function->GetFName() == TEXT("MulticastPresentHit")) ++NativeHitRequests;
	}
	Super::ProcessEvent(Function, Parameters);
}

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

// 可在死亡通知中请求回满，检查它是否仍排在当前事务之后。
void UfpstrueHealthMutationTestObserver::OnDeath()
{
	++Deaths;
	if (bResetDuringDeath && Health.IsValid())
	{
		bResetDuringDeath = false;
		Health->ResetHealth();
	}
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
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueWeaponConfig.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"

// 生命事务与通知重入：先验证写入顺序，再验证死亡上下文与终止边界。
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
	const UfpstrueHealthAttributeSet* Attributes = Health->GetHealthAttributeSet();
	if (!TestNotNull(TEXT("Registered health creates its native attribute set"), Attributes)) return false;
	TestEqual(TEXT("Native ASC lookup supports owners without IAbilitySystemInterface"), Attributes->GetOwningAbilitySystemComponent(), static_cast<UAbilitySystemComponent*>(Health));
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

	Health->ResetHealth();
	Observer->HealthSnapshots.Reset();
	Observer->bResetDuringDeath = true;
	UGameplayStatics::ApplyDamage(Owner, 100.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("Reset from death does not duplicate the death edge"), Observer->Deaths, 3);
	TestTrue(TEXT("Death callback reset runs after the lethal snapshot"), Observer->HealthSnapshots == TArray<float>({0.0f, 100.0f}));
	TestEqual(TEXT("Death callback reset restores health data"), Health->GetHealth(), 100.0f);
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

	// EndPlay 要丢弃已排入的重置，并终止本次尚未发出的伤害、血量和死亡通知。
	const int32 SnapshotsBeforeEndPlay = Observer->HealthSnapshots.Num();
	bool bDestroyedDuringDamage = false;
	const FDelegateHandle EndPlayHandle = Health->OnDamageResolved.AddLambda(
		[&](float, AActor*, AController*, const FFPDamageContext&)
		{
			Health->ResetHealth();
			bDestroyedDuringDamage = Owner->Destroy();
		});
	UGameplayStatics::ApplyDamage(Owner, 100.0f, nullptr, nullptr, nullptr);
	Health->OnDamageResolved.Remove(EndPlayHandle);
	TestTrue(TEXT("The damage callback can end the owner lifecycle"), bDestroyedDuringDamage);
	TestEqual(TEXT("EndPlay stops remaining health notifications"), Observer->HealthSnapshots.Num(), SnapshotsBeforeEndPlay);
	TestEqual(TEXT("EndPlay stops the pending death notification"), Observer->Deaths, 0);
	TestEqual(TEXT("Queued reset does not run after EndPlay"), Health->GetHealth(), 0.0f);
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
	const UfpstrueHealthAttributeSet* Attributes = Health->GetHealthAttributeSet();
	if (!TestNotNull(TEXT("Enemy owns its native attribute set"), Attributes)) return false;
	TestEqual(TEXT("Native ASC lookup uses the enemy's IAbilitySystemInterface"), Attributes->GetOwningAbilitySystemComponent(), Enemy->GetAbilitySystemComponent());
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

// 视觉后坐力：验证脉冲、重复触发和视图隔离，不扩大业务边界测试文件。
// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

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
