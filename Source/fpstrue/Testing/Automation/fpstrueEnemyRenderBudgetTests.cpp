// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpGameState.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "SignificanceManager.h"
#include "AnimationSharingManager.h"
#include "Animation/AnimInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Math/RotationMatrix.h"

#include <limits>

namespace
{
// 资格与目标选择沿生产链进入；临时原生玩家补齐蓝图原本提供的输入资产。
bool ObserveRenderCombatParticipant(FAutomationTestBase& Test, AfpstrueEnemyAIController& Controller, AfpstrueCharacter& Target)
{
	UWorld* World = Target.GetWorld();
	AfpstruePvpGameMode* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	if (!Test.TestNotNull(TEXT("Combat fixture uses the real gameplay mode"), Mode)) return false;
	for (const FName Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Target.GetClass(), Name);
		if (!Test.TestNotNull(TEXT("Combat player exposes its required input action"), Property)) return false;
		Property->SetObjectPropertyValue_InContainer(&Target, NewObject<UInputAction>(&Target));
	}
	APlayerController* Player = World->SpawnActor<APlayerController>();
	if (!Test.TestNotNull(TEXT("Combat target has an ordinary PlayerController"), Player)) return false;
	Player->Possess(&Target);
	if (!Test.TestTrue(TEXT("Combat target is a real player participant"), Mode->IsCombatParticipant(&Target))) return false;
	UAIPerceptionComponent* Perception = Controller.GetPerceptionComponent();
	if (!Test.TestNotNull(TEXT("Combat fixture uses real sight"), Perception)) return false;
	FAIStimulus Sight(*GetDefault<UAISense_Sight>(), 1.0f, Target.GetActorLocation(),
		Controller.GetPawn()->GetActorLocation(), FAIStimulus::SensingSucceeded);
	Sight.SetWantsNotifyOnlyOnValueChange(false);
	Perception->RegisterStimulus(&Target, Sight);
	Perception->ProcessStimuli();
	return Test.TestTrue(TEXT("Sight selects the participant"), Controller.GetTargetCharacter() == &Target);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemySignificanceCombatSampleTest,
	"fpstrue.Performance.Significance.ScopedCombatProtection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 同轮评分与共享资格复用近战快照，离开作用域恢复实时采样；真正攻击查询不读取快照。
bool FFpstrueEnemySignificanceCombatSampleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Significance fixture has an AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld())
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Significance sample enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Significance sample target"), Target) ||
		!TestNotNull(TEXT("Significance sample controller"), Controller)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
	Controller->Possess(Enemy);
	if (!ObserveRenderCombatParticipant(*this, *Controller, *Target)) return false;
	AfpstrueSurroundManager* Manager = Controller->GetSurroundManager();
	if (!TestNotNull(TEXT("Sample target owns a real manager"), Manager)) return false;
	Manager->ApproachQueryId = INDEX_NONE;
	++Manager->PositionQueryRevision;
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->SurroundSlots[0].ProjectedApproachLocation = Enemy->GetActorLocation();
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	if (!TestTrue(TEXT("Sample enemy obtains a real inner lease"), Manager->HasInnerSlot(Enemy,
		Controller->GetTacticalAssignmentRevision()))) return false;
	UBehaviorTreeComponent* Behavior = Cast<UBehaviorTreeComponent>(Controller->GetBrainComponent());
	if (!TestNotNull(TEXT("Behavior brain"), Behavior) ||
		!TestTrue(TEXT("Fixture starts the real behavior tree"), Behavior->IsRunning()) ||
		!TestNotNull(TEXT("Fixture owns a running tree instance"), Behavior->GetCurrentTree())) return false;
	Behavior->StopLogic(TEXT("Scoped significance sampling fixture"));
	if (!TestTrue(TEXT("The real melee check starts within reach"), Enemy->IsTargetInAttackRange())) return false;
	FFPEnemyRenderSignificancePolicy Policy;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Reduced, false, false, false, Policy);
	const float Time = World->GetTimeSeconds();

	// 用一次真实范围结果建立只读作用域，再移动目标，区分预算快照与实时攻击查询。
	const bool bProtectedSample = Enemy->IsTargetInAttackRange();
	{
		TGuardValue<const bool*> SampleScope(Enemy->SignificanceCombatProtectionSample, &bProtectedSample);
		Target->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
		TestFalse(TEXT("Live attack reach never consumes the budget sample"), Enemy->IsTargetInAttackRange());
		Enemy->ApplySignificance(0.0f);
		TestTrue(TEXT("Gameplay consumes this round's close-combat sample"),
			Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full);
		TestTrue(TEXT("Render animation protection consumes the same sample"),
			Enemy->RequiresGameplayAnimationProtection(Time, 0.0f));
		TestFalse(TEXT("Sharing cannot repeat the live query and undo the sampled protection"), Enemy->CanUseAnimationSharing());
	}
	TestNull(TEXT("No protection sample remains outside its scope"), Enemy->SignificanceCombatProtectionSample);
	Enemy->ApplySignificance(0.0f);
	TestTrue(TEXT("Outside the round gameplay samples the moved target"),
		Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Background);
	TestFalse(TEXT("Outside the round render protection also samples the moved target"),
		Enemy->RequiresGameplayAnimationProtection(Time, 0.0f));
	TestTrue(TEXT("An unprotected reduced enemy can share animation again"), Enemy->CanUseAnimationSharing());

	// 下一轮必须重新采样；空快照不能让外部攻击校验继续使用上一轮的远距结果。
	const bool bUnprotectedSample = Enemy->IsTargetInAttackRange();
	{
		TGuardValue<const bool*> SampleScope(Enemy->SignificanceCombatProtectionSample, &bUnprotectedSample);
		Target->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
		TestTrue(TEXT("Live reach sees a target that entered after budget sampling"), Enemy->IsTargetInAttackRange());
		Enemy->ApplySignificance(0.0f);
		TestTrue(TEXT("The next snapshot is independent of the previous protected one"),
			Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Background);
	}
	Enemy->ApplySignificance(0.0f);
	TestTrue(TEXT("A later round observes the newly close target"),
		Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full);
	Controller->Destroy();
	Enemy->Destroy();
	Target->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyGameplayTierReasonSnapshotTest,
	"fpstrue.Performance.Significance.GameplayTierReasonSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 走真实 ApplySignificance 和攻击事务；诊断不得把锁定目标、受击姿态保护或未知距离冒充 Full 的原因。
bool FFpstrueEnemyGameplayTierReasonSnapshotTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Reason fixture has native AI services"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Reason fixture enemy"), Enemy)) return false;
	TestFalse(TEXT("A not-yet-sampled enemy starts explicitly unknown"), Enemy->GetGameplayTierReasonSnapshot().bHasPolicySample);
	TestFalse(TEXT("An unknown enemy has no invented zero-distance measurement"), Enemy->GetGameplayTierReasonSnapshot().bDistanceSampleValid);
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	auto* Target = World->SpawnActor<AfpstrueCharacter>(FVector(100.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	auto* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Reason fixture target"), Target) || !TestNotNull(TEXT("Reason fixture controller"), Controller)) return false;
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->Possess(Enemy);
	if (!ObserveRenderCombatParticipant(*this, *Controller, *Target)) return false;
	AfpstrueSurroundManager* Manager = Controller->GetSurroundManager();
	if (!TestNotNull(TEXT("Reason target owns a real manager"), Manager)) return false;
	Manager->ApproachQueryId = INDEX_NONE;
	++Manager->PositionQueryRevision;
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->SurroundSlots[0].ProjectedApproachLocation = Enemy->GetActorLocation();
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	if (!TestTrue(TEXT("Reason enemy obtains a real inner lease"), Manager->HasInnerSlot(Enemy,
		Controller->GetTacticalAssignmentRevision()))) return false;
	Controller->GetBrainComponent()->StopLogic(TEXT("Gameplay reason snapshot fixture"));
	const bool bNotInRange = false;
	TGuardValue<const bool*> NoRangeSample(Enemy->SignificanceCombatProtectionSample, &bNotInRange);
	Enemy->ApplySignificance(1.0f / 501.0f);
	TestTrue(TEXT("Near max-player distance score alone selects Full"), Enemy->GetGameplayTierReasonSnapshot().bFullByDistance);
	TestTrue(TEXT("Distance is recovered from the actual policy score"),
		Enemy->GetGameplayTierReasonSnapshot().bDistanceSampleValid && FMath::IsNearlyEqual(Enemy->GetGameplayTierReasonSnapshot().NearestPlayerDistance2D, 500.0, 0.01));
	TestFalse(TEXT("A sampled-out melee reason is not invented from the live target"), Enemy->GetGameplayTierReasonSnapshot().bFullByMeleeRange);
	Enemy->ApplySignificance(1.0f / 9001.0f);
	TestTrue(TEXT("A locked target alone does not make far gameplay Full"), Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Background);
	const bool bInRange = true;
	{
		TGuardValue<const bool*> RangeSample(Enemy->SignificanceCombatProtectionSample, &bInRange);
		Enemy->ApplySignificance(1.0f / 9001.0f);
		TestTrue(TEXT("Non-attacking close-combat sample explains Full"), Enemy->GetGameplayTierReasonSnapshot().bFullByMeleeRange);
		TestFalse(TEXT("Far score remains a non-distance Full reason"), Enemy->GetGameplayTierReasonSnapshot().bFullByDistance);
		TestTrue(TEXT("Reason records actual scoped sample use"), Enemy->GetGameplayTierReasonSnapshot().bUsedScopedCombatSample);
	}
	if (!TestTrue(TEXT("Reason fixture begins a real attack transaction"), Enemy->GetCombatComponent()->TryAttackTarget())) return false;
	Enemy->ApplySignificance(1.0f / 9001.0f);
	TestTrue(TEXT("An actual attack explains Full even with a sampled-out melee range"), Enemy->GetGameplayTierReasonSnapshot().bFullByAttack);
	TestFalse(TEXT("Attack short-circuit does not also count a melee-range reason"), Enemy->GetGameplayTierReasonSnapshot().bFullByMeleeRange);
	TestFalse(TEXT("Attack short-circuit does not claim it consumed the close-combat sample"), Enemy->GetGameplayTierReasonSnapshot().bUsedScopedCombatSample);
	Enemy->GetCombatComponent()->ResetCombat();
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, Target, UDamageType::StaticClass());
	Enemy->ApplySignificance(0.0f);
	const auto& Final = Enemy->GetGameplayTierReasonSnapshot();
	TestTrue(TEXT("Zero-score application is recorded, not confused with never sampled"), Final.bHasPolicySample);
	TestFalse(TEXT("Zero score has no measured nearest-player distance"), Final.bDistanceSampleValid);
	TestTrue(TEXT("Hit animation protection does not change the real gameplay tier"), Final.AppliedTier == EFPEnemySignificanceTier::Background);
	TestFalse(TEXT("Hit animation protection is not falsely reported as a Full reason"), Final.bFullByDistance || Final.bFullByAttack || Final.bFullByMeleeRange);
	TestEqual(TEXT("The reason timestamp is the actual last policy application time"), Final.SampleWorldTimeSeconds, static_cast<double>(World->GetTimeSeconds()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyPostProcessIsolationTest,
	"fpstrue.Rendering.EnemyBudget.NoPostProcessAcrossLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 敌人移除后处理实例及重初始化入口，但不改共享资源或其他角色的后处理，不清主动画。
bool FFpstrueEnemyPostProcessIsolationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Postprocess isolation enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	ACharacter* Other = World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("Unrelated character sharing a resource"), Other)) return false;
	USkeletalMesh* SharedResource = NewObject<USkeletalMesh>();
	SharedResource->SetPostProcessAnimBlueprint(UAnimInstance::StaticClass());
	Enemy->GetMesh()->SetSkeletalMeshAsset(SharedResource);
	Other->GetMesh()->SetSkeletalMeshAsset(SharedResource);
	Other->GetMesh()->SetDisablePostProcessBlueprint(false);
	UAnimInstance* OtherPost = NewObject<UAnimInstance>(Other->GetMesh());
	Other->GetMesh()->PostProcessAnimInstance = OtherPost;
	UAnimInstance* Main = NewObject<UAnimInstance>(Enemy->GetMesh());
	Enemy->GetMesh()->AnimScriptInstance = Main;
	// 固定输入只模拟组件上残留的旧后处理，不加载将删除的旧敌人IK资产。
	const auto ReintroduceOldPostProcess = [Enemy]()
	{
		Enemy->GetMesh()->SetOverridePostProcessAnimBP(UAnimInstance::StaticClass(), false);
		Enemy->GetMesh()->SetDisablePostProcessBlueprint(false);
		Enemy->GetMesh()->PostProcessAnimInstance = NewObject<UAnimInstance>(Enemy->GetMesh());
	};
	const auto CheckRemoved = [this, Enemy, Other, SharedResource, OtherPost, Main]()
	{
		TestTrue(TEXT("Enemy postprocess stays disabled"), Enemy->GetMesh()->GetDisablePostProcessBlueprint());
		TestNull(TEXT("Enemy has no resident postprocess instance that can force synchronous evaluation"), Enemy->GetMesh()->GetPostProcessInstance());
		TestNull(TEXT("Enemy has no component-level postprocess override"), Enemy->GetMesh()->OverridePostProcessAnimBP.Get());
		TestTrue(TEXT("Main animation instance is not removed"), Enemy->GetMesh()->GetAnimInstance() == Main);
		TestTrue(TEXT("Shared mesh asset is not globally edited"), SharedResource->GetPostProcessAnimBlueprint() == UAnimInstance::StaticClass());
		TestFalse(TEXT("Other consumers keep their own enabled postprocess"), Other->GetMesh()->GetDisablePostProcessBlueprint());
		TestTrue(TEXT("Other consumers keep their own postprocess instance"), Other->GetMesh()->GetPostProcessInstance() == OtherPost);
	};
	FFPEnemyRenderSignificancePolicy Policy;
	for (const EFPEnemyRenderSignificanceTier Tier : {EFPEnemyRenderSignificanceTier::Full, EFPEnemyRenderSignificanceTier::Reduced,
		EFPEnemyRenderSignificanceTier::Background})
	{
		ReintroduceOldPostProcess();
		Enemy->ApplyRenderSignificanceTier(Tier, false, false, false, Policy);
		CheckRemoved();
	}
	ReintroduceOldPostProcess();
	Enemy->GetMesh()->OnAnimInitialized.Broadcast(); // 调用实际绑定的生命周期封口，而非直接访问私有清理。
	CheckRemoved();
	return true;
}

// 本组验证预算选择、组件标志和降级时序；NullRHI 下的通过不证明阴影、光追或运动画面正确。
// ★【重要边界】[验收边界] NullRHI 只能确认行为和组件旗标；真实阴影/光追画面与 GPU 耗时须另行运行验收。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderBudgetBoundaryTest, "fpstrue.Rendering.EnemyBudget.MeshOwnershipAndDeath",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证预算只管理敌人拥有的网格、不扩大作者资格，并在死亡后永久撤销阴影/光追资格。
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
	// 构造作者允许/禁止阴影与光追的附件，让恢复预算也不能误启原本关闭的标志。
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
	// 从真实组件标志回读计数，不只检查协调器记录的上次分配值。
	const auto CheckCounts = [this, Enemy, &Meshes, &ShadowMeshes, &RayTracingMeshes](int32 ExpectedMeshes, int32 ExpectedEnabled)
	{
		Enemy->GetRenderBudgetMeshCounts(Meshes, ShadowMeshes, RayTracingMeshes);
		TestEqual(TEXT("Owned mesh count reads the actual cached set"), Meshes, ExpectedMeshes);
		TestEqual(TEXT("Shadow count reads actual component flags"), ShadowMeshes, ExpectedEnabled);
		TestEqual(TEXT("Ray tracing count reads actual component flags"), RayTracingMeshes, ExpectedEnabled);
	};
	// 受击/战斗保护只恢复动画及时性，不额外占用阴影和光追预算。
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
	// 统一切换本用例的 Full 档分配，保持各轮唯一变化为阴影与光追许可。
	const auto ApplyBudget = [Enemy, &Policy](bool bAllow)
	{ Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Full, bAllow, bAllow, false, Policy); };
	// 检查收紧/恢复预算不会改变可见性、碰撞，也不会接管普通附着的外部 Actor。
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
	TestTrue(TEXT("Death does not re-enable enemy postprocess"), Enemy->GetMesh()->GetDisablePostProcessBlueprint());
	TestNull(TEXT("Death keeps the enemy postprocess instance removed"), Enemy->GetMesh()->GetPostProcessInstance());
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

// 验证升档立即响应、降档同时满足驻留和延迟，恢复评分或关闭功能会丢弃旧降档计时。
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
	// 仅变化评分和世界时间，调用生产自然档位判定并检查期望档位。
	const auto CheckTier = [this, Enemy, &Policy](float Score, Tier Expected, const TCHAR* Reason)
	{
		FFPEnemyRenderSignificanceSample Sample;
		Sample.Score = Score;
		TestTrue(Reason, Enemy->ResolveNaturalRenderSignificanceTier(Sample, Policy) == Expected);
	};
	// 在阈值附近反复跨越，覆盖驻留、延迟取消、重新计时和直接降至 Background。
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
	// 开关停用后再启用，不能继承停用前积累的降档时间。
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

#if WITH_EDITOR

// 只固定临时世界的 PIE 网络角色以覆盖权限分支；不建立 Socket，也不替代多进程弱网和真实画面验收。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerSignificanceCoordinatorTest,
	"fpstrue.Multiplayer.Performance.ServerAllParticipantsAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 用真实协调器验证服务器考虑全部参与者，并覆盖开始、停止和参与者离场的注册关系。
bool FFpstrueMultiplayerSignificanceCoordinatorTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	auto* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	auto* State = World->GetGameState<AfpstruePvpGameState>();
	if (!TestNotNull(TEXT("Real multiplayer GameMode"), Mode) || !TestNotNull(TEXT("Real multiplayer GameState"), State)) return false;
	Mode->EnemyPopulation = 0;
	auto* Coordinator = State->FindComponentByClass<UfpstrueEnemySignificanceCoordinator>();
	auto* Sharing = State->FindComponentByClass<UfpstrueEnemyAnimationSharingCoordinator>();
	if (!TestNotNull(TEXT("GameState owns the significance coordinator"), Coordinator) ||
		!TestNotNull(TEXT("GameState owns the sharing coordinator"), Sharing) ||
		!TestTrue(TEXT("GameState BeginPlay starts the existing coordinator"), Coordinator->bRunning) ||
		!TestNotNull(TEXT("The real significance plugin is available"), USignificanceManager::Get(World))) return false;
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldTypeScope(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	TestTrue(TEXT("Fixture enters the dedicated-server branch without creating a network transport"), World->GetNetMode() == NM_DedicatedServer);
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* Players[2] = {};
	APlayerController* Controllers[2] = {};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Players[Index] = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, Spawn);
		Controllers[Index] = World->SpawnActor<APlayerController>();
		if (!TestNotNull(TEXT("Server participant Pawn"), Players[Index]) || !TestNotNull(TEXT("Server participant Controller"), Controllers[Index])) return false;
		Players[Index]->SetActorEnableCollision(false);
		Players[Index]->GetCharacterMovement()->SetComponentTickEnabled(false);
		Players[Index]->SetActorLocation(FVector(Index == 0 ? 50000.0f : 1000.0f, 0.0f, 0.0f));
		Controllers[Index]->Possess(Players[Index]);
		if (!TestTrue(TEXT("Possessed Pawn satisfies production participation rules"), Mode->IsCombatParticipant(Players[Index]))) return false;
	}
	auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity,
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Server significance enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Enemy->bReferenceEnemyReplication = false; // 用例固定被测策略，不改变进程A/B命令行。
	Enemy->ApplyNetworkReplicationPolicy();
	Coordinator->Update();
	TestTrue(TEXT("Near second participant wins over far first participant, rather than averaging"),
		Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full);
	TestEqual(TEXT("A near second participant protects Actor replication for every connection"),
		Enemy->GetNetUpdateFrequency(), FMath::Min(Enemy->AuthoredNetUpdateFrequency, 30.0f));
	TestFalse(TEXT("DS never publishes a render application"), Coordinator->bReportedRenderUpdate);
	TestNull(TEXT("DS never creates the World animation-sharing manager"), UAnimationSharingManager::GetManagerForWorld(World));
	TestEqual(TEXT("DS has no sharing followers"), Sharing->GetRegisteredEnemyCount(), 0);

	// 保留槽位是本用例的服务器前置；参与资格、失效和多视点评分仍调用生产代码。
	Controllers[1]->UnPossess();
	AfpstruePvpGameMode::FRejoinSlot Retained;
	Retained.Pawn = Players[1];
	Retained.bDisconnected = true;
	Retained.ExpiresAt = World->GetTimeSeconds() + 30.0;
	Mode->RejoinSlots.Add(TEXT("significance-retained-fixture"), Retained);
	Coordinator->Update();
	TestTrue(TEXT("Retained nearby player keeps enemy response"), Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full);
	Mode->RejoinSlots.FindChecked(TEXT("significance-retained-fixture")).ExpiresAt = World->GetTimeSeconds() - 1.0;
	Coordinator->Update();
	TestTrue(TEXT("Only distant valid participant permits background gameplay"), Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Background);
	TestEqual(TEXT("Only distant valid participants permit the background replication cap"),
		Enemy->GetNetUpdateFrequency(), FMath::Min(Enemy->AuthoredNetUpdateFrequency, 5.0f));
	Players[0]->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	Coordinator->Update();
	TestTrue(TEXT("Moving the first participant near independently restores full gameplay"), Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full);
	TestEqual(TEXT("Either participant independently restores Full replication"),
		Enemy->GetNetUpdateFrequency(), FMath::Min(Enemy->AuthoredNetUpdateFrequency, 30.0f));
	State->Destroy();
	TestFalse(TEXT("GameState EndPlay stops its coordinator"), Coordinator->bRunning);
	TestFalse(TEXT("GameState EndPlay clears the owned update timer"), World->GetTimerManager().IsTimerActive(Coordinator->UpdateTimerHandle));
	// 停止后改变真实评分输入；直接观察玩法档位，避免为测试维护另一份更新序号。
	Players[0]->SetActorLocation(FVector(50000.0f, 0.0f, 0.0f));
	Coordinator->Update();
	TestTrue(TEXT("Stopped coordinator cannot apply the changed viewpoint"), Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full);
	TestEqual(TEXT("Cleanup leaves no sharing followers"), Sharing->GetRegisteredEnemyCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyReplicationTieringTest,
	"fpstrue.Multiplayer.Performance.EnemyReplicationTieringAndReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 临时PIE角色只验证调度策略与权限，不替代真实160AI带宽/复制CPU/状态陈旧度对照。
bool FFpstrueEnemyReplicationTieringTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Replication fixture has native AI services"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	const auto SpawnConfiguredEnemy = [World](float Frequency, float MinFrequency)
	{
		auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity,
			nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy == nullptr) return Enemy;
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->SetNetUpdateFrequency(Frequency);
		Enemy->SetMinNetUpdateFrequency(MinFrequency);
		Enemy->FinishSpawning(FTransform::Identity);
		Enemy->SetActorEnableCollision(false);
		Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		return Enemy;
	};
	auto* Standalone = SpawnConfiguredEnemy(80.0f, 10.0f);
	if (!TestNotNull(TEXT("Standalone enemy"), Standalone)) return false;
	Standalone->SignificanceTier = EFPEnemySignificanceTier::Background;
	Standalone->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Standalone frequency is untouched"), Standalone->GetNetUpdateFrequency(), 80.0f);
	TestFalse(TEXT("Standalone does not capture or own replication policy"), Standalone->bCapturedAuthoredReplicationFrequency);
	TestTrue(TEXT("Reference command line is case insensitive"), AfpstrueEnemyCharacter::IsReferenceReplicationRequested(TEXT("-EnemyReplication=reference")));
	TestFalse(TEXT("Tiered mode does not select Reference"), AfpstrueEnemyCharacter::IsReferenceReplicationRequested(TEXT("-EnemyReplication=Tiered")));
	TestFalse(TEXT("No option keeps the shipping-compatible Tiered default"), AfpstrueEnemyCharacter::IsReferenceReplicationRequested(TEXT("")));
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldTypeScope(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	TestTrue(TEXT("The fixture enters the real server mode guard"), World->GetNetMode() == NM_DedicatedServer);
	auto* Enemy = SpawnConfiguredEnemy(100.0f, 20.0f);
	auto* LowAuthored = SpawnConfiguredEnemy(12.0f, 8.0f);
	if (!TestNotNull(TEXT("Server enemy"), Enemy) || !TestNotNull(TEXT("Low-frequency authored enemy"), LowAuthored)) return false;
	Enemy->bReferenceEnemyReplication = LowAuthored->bReferenceEnemyReplication = false;
	const auto CheckTier = [&](EFPEnemySignificanceTier Tier, float ExpectedFrequency, float ExpectedMin)
	{
		Enemy->SignificanceTier = Tier;
		Enemy->ApplyNetworkReplicationPolicy();
		TestEqual(TEXT("Server applies the expected Actor-level frequency cap"), Enemy->GetNetUpdateFrequency(), ExpectedFrequency);
		TestEqual(TEXT("Adaptive minimum cannot exceed the cap or original minimum"), Enemy->GetMinNetUpdateFrequency(), ExpectedMin);
	};
	CheckTier(EFPEnemySignificanceTier::Full, 30.0f, 20.0f);
	CheckTier(EFPEnemySignificanceTier::Reduced, 15.0f, 15.0f);
	CheckTier(EFPEnemySignificanceTier::Background, 5.0f, 5.0f);
	const float MovementInterval = Enemy->GetCharacterMovement()->GetComponentTickInterval();
	const float MeshInterval = Enemy->GetMesh()->GetComponentTickInterval();
	const auto RenderTier = Enemy->GetRenderSignificanceTier();
	const bool bPostProcessDisabled = Enemy->GetMesh()->GetDisablePostProcessBlueprint();
	Enemy->bReferenceEnemyReplication = true;
	Enemy->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Reference restores the actual authored frequency, not Full30"), Enemy->GetNetUpdateFrequency(), 100.0f);
	TestEqual(TEXT("Reference restores the actual authored adaptive minimum"), Enemy->GetMinNetUpdateFrequency(), 20.0f);
	Enemy->bReferenceEnemyReplication = false;
	Enemy->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Leaving Reference reapplies Background"), Enemy->GetNetUpdateFrequency(), 5.0f);
	Enemy->bEnableNetworkReplicationTiering = false;
	Enemy->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Disabling the feature restores authored frequency"), Enemy->GetNetUpdateFrequency(), 100.0f);
	Enemy->bEnableNetworkReplicationTiering = true;
	Enemy->ApplyNetworkReplicationPolicy();
	LowAuthored->SignificanceTier = EFPEnemySignificanceTier::Full;
	LowAuthored->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Full never raises an asset configured below30Hz"), LowAuthored->GetNetUpdateFrequency(), 12.0f);
	LowAuthored->SignificanceTier = EFPEnemySignificanceTier::Reduced;
	LowAuthored->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Reduced never raises an asset configured below15Hz"), LowAuthored->GetNetUpdateFrequency(), 12.0f);
	LowAuthored->SignificanceTier = EFPEnemySignificanceTier::Background;
	LowAuthored->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Background still tightens an authored low-rate actor"), LowAuthored->GetNetUpdateFrequency(), 5.0f);
	TestEqual(TEXT("Replication policy never changes movement cadence"), Enemy->GetCharacterMovement()->GetComponentTickInterval(), MovementInterval);
	TestEqual(TEXT("Replication policy never changes animation cadence"), Enemy->GetMesh()->GetComponentTickInterval(), MeshInterval);
	TestTrue(TEXT("Replication policy never changes RenderTier"), Enemy->GetRenderSignificanceTier() == RenderTier);
	TestEqual(TEXT("Replication policy never re-enables enemy postprocess"), Enemy->GetMesh()->GetDisablePostProcessBlueprint(), bPostProcessDisabled);
	TestNull(TEXT("Replication policy leaves no enemy postprocess instance"), Enemy->GetMesh()->GetPostProcessInstance());
	TestTrue(TEXT("Every tier keeps Actor and movement replication enabled"), Enemy->GetIsReplicated() && Enemy->IsReplicatingMovement());
	TestTrue(TEXT("Every tier keeps the original dormancy mode"), Enemy->NetDormancy == DORM_Awake);

	// 真实攻击提交后角色桥接必须立即提升，Reset后退回当前自然档；不直接伪造Combat私有阶段。
	auto* Target = World->SpawnActor<AfpstrueCharacter>(FVector(100.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	auto* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Combat protection target"), Target) || !TestNotNull(TEXT("Combat protection controller"), Controller)) return false;
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->Possess(Enemy);
	Controller->GetBrainComponent()->SetComponentTickEnabled(false);
	if (!ObserveRenderCombatParticipant(*this, *Controller, *Target)) return false;
	AfpstrueSurroundManager* Manager = Controller->GetSurroundManager();
	if (!TestNotNull(TEXT("Replication target owns a real manager"), Manager)) return false;
	// 实际感知给出的目标只影响该观察者的排序；不更改基础相关性、频率或另一连接的优先级。
	auto* OtherViewTarget = World->SpawnActor<AfpstrueCharacter>(FVector(0.0f, 1500.0f, 0.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Replication priority has a distinct second observer"), OtherViewTarget)) return false;
	OtherViewTarget->SetActorEnableCollision(false);
	OtherViewTarget->GetCharacterMovement()->SetComponentTickEnabled(false);
	const FVector ViewPosition(1000.0f, 0.0f, 0.0f), ViewDirection(-1.0f, 0.0f, 0.0f);
	const auto Priority = [&](AActor* ViewTarget, float Time)
	{
		return Enemy->GetNetPriority(ViewPosition, ViewDirection, Target->GetController(), ViewTarget, nullptr, Time, false);
	};
	const auto NativePriority = [&](AActor* ViewTarget, float Time)
	{
		return Enemy->ACharacter::GetNetPriority(ViewPosition, ViewDirection, Target->GetController(), ViewTarget, nullptr, Time, false);
	};
	TestTrue(TEXT("A perceived direct threat has twice the native priority for its target observer"),
		FMath::IsNearlyEqual(Priority(Target, 0.25f), NativePriority(Target, 0.25f) * 2.0f));
	TestTrue(TEXT("Another observer keeps exactly the native priority"),
		FMath::IsNearlyEqual(Priority(OtherViewTarget, 0.25f), NativePriority(OtherViewTarget, 0.25f)));
	TestTrue(TEXT("A missing view target cannot grant a threat boost"),
		FMath::IsNearlyEqual(Priority(nullptr, 0.25f), NativePriority(nullptr, 0.25f)));
	TestTrue(TEXT("An unrelated connection remains nonzero and gains priority while waiting"),
		Priority(OtherViewTarget, 0.25f) > 0.0f && Priority(OtherViewTarget, 1.0f) > Priority(OtherViewTarget, 0.25f));
	{
		TGuardValue<bool> ReferencePriority(Enemy->bReferenceEnemyReplication, true);
		TestTrue(TEXT("Reference bypasses the direct-threat extension even with a valid combat target"),
			FMath::IsNearlyEqual(Priority(Target, 0.25f), NativePriority(Target, 0.25f)));
	}
	Manager->ApproachQueryId = INDEX_NONE;
	++Manager->PositionQueryRevision;
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->SurroundSlots[0].ProjectedApproachLocation = Enemy->GetActorLocation();
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	if (!TestTrue(TEXT("Replication enemy obtains a real inner lease"), Manager->HasInnerSlot(Enemy,
		Controller->GetTacticalAssignmentRevision()))) return false;
	if (!TestTrue(TEXT("Protection starts a real attack transaction"), Enemy->GetCombatComponent()->TryAttackTarget())) return false;
	TestEqual(TEXT("Attack immediately restores the Full replication cap"), Enemy->GetNetUpdateFrequency(), 30.0f);
	TestTrue(TEXT("An actually attacking enemy gets four times the native priority only for its target"),
		FMath::IsNearlyEqual(Priority(Target, 0.25f), NativePriority(Target, 0.25f) * 4.0f)
		&& FMath::IsNearlyEqual(Priority(OtherViewTarget, 0.25f), NativePriority(OtherViewTarget, 0.25f)));
	TestTrue(TEXT("Combat weighting preserves the native waiting-time compensation"), Priority(Target, 1.0f) > Priority(Target, 0.25f));
	Enemy->GetCombatComponent()->ResetCombat();
	TestEqual(TEXT("Attack cleanup returns to the unchanged natural Background tier"), Enemy->GetNetUpdateFrequency(), 5.0f);
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, Target, UDamageType::StaticClass());
	TestEqual(TEXT("Actual damage immediately protects replication"), Enemy->GetNetUpdateFrequency(), 30.0f);
	Enemy->RefreshHitAnimationProtection();
	TestEqual(TEXT("Hit protection cleanup returns to Background"), Enemy->GetNetUpdateFrequency(), 5.0f);

	World->SetPlayInEditorInitialNetMode(NM_Client);
	Enemy->SetNetUpdateFrequency(17.0f);
	Enemy->SetMinNetUpdateFrequency(3.0f);
	Enemy->SignificanceTier = EFPEnemySignificanceTier::Full;
	Enemy->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Client network mode never writes authority replication settings"), Enemy->GetNetUpdateFrequency(), 17.0f);
	World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	Enemy->SetRole(ROLE_SimulatedProxy);
	Enemy->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("A simulated replica is rejected even in a listen-server world"), Enemy->GetNetUpdateFrequency(), 17.0f);
	Enemy->SetRole(ROLE_Authority);
	Enemy->ApplyNetworkReplicationPolicy();
	TestEqual(TEXT("Authority in a listen-server world can resume policy"), Enemy->GetNetUpdateFrequency(), 30.0f);
	UGameplayStatics::ApplyDamage(Enemy, 10000.0f, nullptr, Target, UDamageType::StaticClass());
	TestTrue(TEXT("Actual death leaves the live-enemy replication budget"), Enemy->IsDead());
	TestTrue(TEXT("A dead enemy returns to native priority for its final state"),
		FMath::IsNearlyEqual(Priority(Target, 0.25f), NativePriority(Target, 0.25f)));
	TestEqual(TEXT("Death restores authored replication scheduling for its final state"), Enemy->GetNetUpdateFrequency(), 100.0f);
	TestEqual(TEXT("Death also restores the authored adaptive minimum"), Enemy->GetMinNetUpdateFrequency(), 20.0f);
	TestTrue(TEXT("Death does not hide the final replicated Actor state"), Enemy->GetIsReplicated());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerClientPerformanceTest,
	"fpstrue.Multiplayer.Performance.ClientLocalRenderAndVelocityState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证客户端仅应用本地动画/渲染预算，远端移动状态读取复制速度而不启动权威 AI。
bool FFpstrueMultiplayerClientPerformanceTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldTypeScope(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_Client);
	auto* State = World->SpawnActorDeferred<AfpstruePvpGameState>(AfpstruePvpGameState::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Client GameState fixture"), State)) return false;
	State->SetRole(ROLE_SimulatedProxy);
	State->FinishSpawning(FTransform::Identity);
	auto* Coordinator = State->FindComponentByClass<UfpstrueEnemySignificanceCoordinator>();
	if (!TestNotNull(TEXT("Client GameState owns its coordinator"), Coordinator) ||
		!TestTrue(TEXT("Client GameState BeginPlay also starts local coordination"), Coordinator->bRunning)) return false;
	auto* Player = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Client local view Controller"), Player) || !TestTrue(TEXT("Fixture Controller owns the client view"), Player->IsLocalController())) return false;
	const FTransform EnemyTransform(FVector(-20000.0f, 0.0f, 0.0f));
	auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), EnemyTransform);
	if (!TestNotNull(TEXT("Client enemy replica fixture"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->SetRole(ROLE_SimulatedProxy);
	Enemy->GetMesh()->bEnableUpdateRateOptimizations = true;
	Enemy->FinishSpawning(EnemyTransform);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	TestNull(TEXT("Client has no enemy AIController"), Enemy->GetController());
	TestFalse(TEXT("Client enemy never registers a gameplay significance callback"), Enemy->bRegisteredWithSignificanceManager);
	// 该用例隔离预算与权限；真实素材注册和共享姿态另由多进程渲染验收，避免原生空骨骼加载失败污染断言。
	Coordinator->bSharingStartAttempted = true;
	Coordinator->ActiveRenderPolicy.MinimumTierHoldSeconds = 0.0f;
	Coordinator->ActiveRenderPolicy.DemotionDelaySeconds = 0.0f;
	const auto GameplayTierBefore = Enemy->GetGameplaySignificanceTier();
	Enemy->GetCharacterMovement()->SetComponentTickInterval(0.0125f);
	Enemy->ApplySignificance(0.0f);
	Enemy->ApplyGameplaySignificanceIntervals();
	Coordinator->Update();
	TestTrue(TEXT("Client still publishes its own render policy"), Coordinator->bReportedRenderUpdate);
	TestTrue(TEXT("Client cannot overwrite the gameplay tier"), Enemy->GetGameplaySignificanceTier() == GameplayTierBefore);
	TestEqual(TEXT("Client cannot change CharacterMovement update cadence"), Enemy->GetCharacterMovement()->GetComponentTickInterval(), 0.0125f);
	TestTrue(TEXT("Client background animation may be throttled"), Enemy->GetMesh()->GetComponentTickInterval() > 0.0f);
	TestTrue(TEXT("Client may retain authored animation URO"), Enemy->GetMesh()->bEnableUpdateRateOptimizations);

	auto* Processor = NewObject<UfpstrueEnemyAnimationSharingStateProcessor>(State);
	int32 AnimationState = INDEX_NONE;
	bool bShouldProcess = false;
	Enemy->GetCharacterMovement()->Velocity = FVector(200.0f, 0.0f, 0.0f);
	Processor->ProcessActorState_Implementation(AnimationState, Enemy, 0, 0, bShouldProcess);
	TestTrue(TEXT("Sharing state accepts a replica without an AIController"), bShouldProcess);
	TestEqual(TEXT("Replicated velocity selects moving animation"), AnimationState, static_cast<int32>(EFPEnemyAIState::Chase));
	Enemy->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	Processor->ProcessActorState_Implementation(AnimationState, Enemy, 0, 0, bShouldProcess);
	TestEqual(TEXT("Stopped replica selects idle instead of running in place"), AnimationState, static_cast<int32>(EFPEnemyAIState::Idle));
	// 模拟引擎移除客户端复制副本，不能用客户端普通 Destroy 请求绕过网络销毁权限。
	State->Destroy(true);
	TestFalse(TEXT("Client GameState cleanup stops its local coordinator"), Coordinator->bRunning);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderBudgetStabilityTest,
	"fpstrue.Rendering.EnemyBudget.IndependentBudgetRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 直接调用生产预算分配阶段，不把自然档迟滞、Actor 注册顺序或真实渲染耗时混入名额稳定性断言。
bool FFpstrueEnemyRenderBudgetStabilityTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueEnemyCharacter* Enemies[3] = {};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FTransform Transform(FVector(Index * 500.0f, 0.0f, 0.0f));
		Enemies[Index] = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), Transform,
			nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("Budget candidate enemy"), Enemies[Index])) return false;
		Enemies[Index]->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemies[Index]->FinishSpawning(Transform);
		Enemies[Index]->GetCharacterMovement()->SetComponentTickEnabled(false);
	}
	auto* Coordinator = NewObject<UfpstrueEnemySignificanceCoordinator>();
	auto& Policy = Coordinator->ActiveRenderPolicy;
	Policy.MaxFullRenderEnemies = Policy.MaxShadowCastingEnemies = Policy.MaxRayTracingEnemies = 1;
	Policy.BudgetRetentionMargin = 0.05f;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		auto& Candidate = Coordinator->CandidateBuffer.AddDefaulted_GetRef();
		Candidate.Enemy = Enemies[Index];
		Candidate.PriorityKey = {0.8f - Index * 0.01f, static_cast<uint32>(Index + 1), true, true};
		Candidate.Sample.Score = Candidate.PriorityKey.Score;
		Candidate.Sample.Distance = 100.0f;
		Candidate.Sample.bInExpandedFrustum = true;
		Candidate.NaturalTier = EFPEnemyRenderSignificanceTier::Full;
		Candidate.bShadowEligible = Candidate.bRayTracingEligible = true;
	}
	int32 FullRejected = 0, ShadowRejected = 0, RayTracingRejected = 0;
	const auto Allocate = [&]() { Coordinator->AllocatePresentationBudgets(FullRejected, ShadowRejected, RayTracingRejected); };
	const auto Holds = [](const TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>>& Holders, AfpstrueEnemyCharacter* Enemy)
	{
		return Holders.Contains(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	};
	Allocate();
	TestTrue(TEXT("Initial Full selection uses raw priority"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	Coordinator->CandidateBuffer[1].PriorityKey.Score = 0.81f;
	Allocate();
	TestTrue(TEXT("A small score crossing does not replace an existing Full holder"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	TestEqual(TEXT("Budget scoring leaves the raw key untouched"), Coordinator->CandidateBuffer[1].PriorityKey.Score, 0.81f);
	TestEqual(TEXT("Budget scoring leaves the sampled score untouched"), Coordinator->CandidateBuffer[0].Sample.Score, 0.8f);
	TestTrue(TEXT("A budget-reduced candidate retains its natural Full tier"),
		Coordinator->CandidateBuffer[1].NaturalTier == EFPEnemyRenderSignificanceTier::Full);
	Coordinator->CandidateBuffer[1].PriorityKey.Score = 0.86f;
	Allocate();
	TestTrue(TEXT("A clearly better candidate replaces the incumbent"), Holds(Coordinator->FullBudgetHolders, Enemies[1]));
	TestEqual(TEXT("Full rejection accounting still reflects the strict budget"), FullRejected, 2);

	Coordinator->Stop();
	Coordinator->CandidateBuffer[0].PriorityKey.Score = 0.99f;
	Coordinator->CandidateBuffer[1].PriorityKey.Score = 0.98f;
	Allocate();
	Coordinator->CandidateBuffer[1].PriorityKey.Score = 1.0f;
	Allocate();
	TestTrue(TEXT("Near-one scores retain the margin instead of saturating away the bonus"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	Policy.BudgetRetentionMargin = 0.0f;
	Allocate();
	TestTrue(TEXT("Zero retention restores raw-score selection"), Holds(Coordinator->FullBudgetHolders, Enemies[1]));
	Policy.BudgetRetentionMargin = 0.05f;
	Coordinator->CandidateBuffer[1].PriorityKey.bInPrimaryFrustum = false;
	Allocate();
	TestTrue(TEXT("A higher frustum class wins immediately despite the previous holder"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	Coordinator->CandidateBuffer[0].NaturalTier = EFPEnemyRenderSignificanceTier::Reduced;
	Allocate();
	TestFalse(TEXT("A holder that loses Full eligibility is released"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	TestEqual(TEXT("Losing eligibility never leaves an unused Full slot"), Coordinator->FullBudgetHolders.Num(), 1);

	// 三个池先各自形成历史；Full 资格不能把阴影/RT 名额一并赠送给同一敌人。
	Coordinator->Stop();
	Policy.MaxFullRenderEnemies = 3;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		auto& Candidate = Coordinator->CandidateBuffer[Index];
		Candidate.PriorityKey = {0.8f, static_cast<uint32>(Index + 1), true, true};
		Candidate.NaturalTier = EFPEnemyRenderSignificanceTier::Full;
		Candidate.bShadowEligible = Index == 1;
		Candidate.bRayTracingEligible = Index == 2;
	}
	Allocate();
	TestTrue(TEXT("Shadow history belongs only to its selected pool"), Holds(Coordinator->ShadowBudgetHolders, Enemies[1]));
	TestTrue(TEXT("Ray tracing history belongs only to its selected pool"), Holds(Coordinator->RayTracingBudgetHolders, Enemies[2]));
	Coordinator->CandidateBuffer[0].PriorityKey.Score = 0.81f;
	for (auto& Candidate : Coordinator->CandidateBuffer) Candidate.bShadowEligible = Candidate.bRayTracingEligible = true;
	Allocate();
	TestTrue(TEXT("Shadow keeps its own holder, not the raw-score/Full winner"), Holds(Coordinator->ShadowBudgetHolders, Enemies[1]));
	TestTrue(TEXT("Ray tracing keeps its different holder independently"), Holds(Coordinator->RayTracingBudgetHolders, Enemies[2]));
	TestEqual(TEXT("Full allocations do not increase the shadow cap"), Coordinator->ShadowBudgetHolders.Num(), 1);
	TestEqual(TEXT("Full allocations do not increase the ray tracing cap"), Coordinator->RayTracingBudgetHolders.Num(), 1);
	Coordinator->CandidateBuffer[1].Sample.Distance = Policy.ShadowMaxDistance + 1.0f;
	Coordinator->CandidateBuffer[2].bRayTracingEligible = false;
	Allocate();
	TestFalse(TEXT("Shadow holder loses its slot outside the distance limit"), Holds(Coordinator->ShadowBudgetHolders, Enemies[1]));
	TestFalse(TEXT("Ray tracing holder loses its slot when asset eligibility is removed"), Holds(Coordinator->RayTracingBudgetHolders, Enemies[2]));
	Coordinator->CandidateBuffer[1].Sample.Distance = 100.0f;
	Coordinator->CandidateBuffer[2].bRayTracingEligible = true;
	Policy.MaxFullRenderEnemies = 1;
	Allocate();
	TestEqual(TEXT("Shrinking Full capacity immediately respects the new budget"), Coordinator->FullBudgetHolders.Num(), 1);
	TestTrue(TEXT("Ray tracing still requires assigned Full, not only old ray tracing membership"),
		Holds(Coordinator->RayTracingBudgetHolders, Enemies[0]));

	Policy.bEnableRenderTiering = Policy.bEnableShadowBudget = Policy.bEnableRayTracingBudget = false;
	Allocate();
	TestEqual(TEXT("Disabled Full budgeting clears its membership history"), Coordinator->FullBudgetHolders.Num(), 0);
	TestEqual(TEXT("Disabled shadow budgeting clears its membership history"), Coordinator->ShadowBudgetHolders.Num(), 0);
	TestEqual(TEXT("Disabled ray tracing budgeting clears its membership history"), Coordinator->RayTracingBudgetHolders.Num(), 0);
	Policy.bEnableRenderTiering = Policy.bEnableShadowBudget = Policy.bEnableRayTracingBudget = true;
	Policy.MaxFullRenderEnemies = 3;
	Allocate();
	TestTrue(TEXT("Re-enabled shadow budgeting starts with raw priority"), Holds(Coordinator->ShadowBudgetHolders, Enemies[0]));
	TestTrue(TEXT("Re-enabled ray tracing budgeting starts without stale holders"), Holds(Coordinator->RayTracingBudgetHolders, Enemies[0]));
	Policy.MaxFullRenderEnemies = Policy.MaxShadowCastingEnemies = Policy.MaxRayTracingEnemies = 0;
	Allocate();
	TestEqual(TEXT("Zero Full budget has no retained members"), Coordinator->FullBudgetHolders.Num(), 0);
	TestEqual(TEXT("Zero shadow budget has no retained members"), Coordinator->ShadowBudgetHolders.Num(), 0);
	TestEqual(TEXT("Zero ray tracing budget has no retained members"), Coordinator->RayTracingBudgetHolders.Num(), 0);

	Policy.MaxFullRenderEnemies = Policy.MaxShadowCastingEnemies = Policy.MaxRayTracingEnemies = 1;
	Allocate();
	TestTrue(TEXT("Destroyed-candidate scenario begins with A holding Full"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	Enemies[0]->Destroy();
	Coordinator->CandidateBuffer.RemoveAt(0);
	Allocate();
	TestFalse(TEXT("An enemy removed from the live snapshot cannot keep a Full slot"), Holds(Coordinator->FullBudgetHolders, Enemies[0]));
	TestFalse(TEXT("A removed enemy cannot keep its shadow slot"), Holds(Coordinator->ShadowBudgetHolders, Enemies[0]));
	TestFalse(TEXT("A removed enemy cannot keep its ray tracing slot"), Holds(Coordinator->RayTracingBudgetHolders, Enemies[0]));
	TestEqual(TEXT("The remaining live candidate receives the released Full slot"), Coordinator->FullBudgetHolders.Num(), 1);
	Coordinator->Stop();
	TestEqual(TEXT("Stop clears Full history"), Coordinator->FullBudgetHolders.Num(), 0);
	TestEqual(TEXT("Stop clears shadow history"), Coordinator->ShadowBudgetHolders.Num(), 0);
	TestEqual(TEXT("Stop clears ray tracing history"), Coordinator->RayTracingBudgetHolders.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerAuthorityPoseTest,
	"fpstrue.Multiplayer.Performance.AuthorityPoseNeverUsesClientBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证权威端必要姿态更新不被客户端渲染档位、共享姿态或不可见策略关闭。
bool FFpstrueMultiplayerAuthorityPoseTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Authority pose fixture has native AI services"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldTypeScope(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Network-authority enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->GetMesh()->bEnableUpdateRateOptimizations = true;
	Enemy->FinishSpawning(FTransform::Identity);
	// 显式走真实配置入口固定fixture模式，既验证组件配置，也不依赖整进程A/B命令行。
	Enemy->ConfigureHitProxies(true);
	TestTrue(TEXT("Simple mode uses animation-independent hit proxies"), Enemy->UsesSimpleHitProxies());
	TestFalse(TEXT("An idle Simple authority has no full-pose correctness dependency"), Enemy->RequiresFullAuthorityPose());
	Enemy->GetMesh()->SetComponentTickInterval(0.1f);
	FFPEnemyRenderSignificancePolicy Policy;
	Policy.FullMinLOD = 2;
	Enemy->SignificanceTier = EFPEnemySignificanceTier::Full;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, false, false, false, Policy);
	TestEqual(TEXT("Full gameplay removes mesh cooldown even with Background rendering"), Enemy->GetMesh()->GetComponentTickInterval(), 0.0f);
	TestTrue(TEXT("Idle Simple authority may retain authored visibility ticking"),
		Enemy->GetMesh()->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered);
	TestFalse(TEXT("Authority can never become an animation-sharing follower"), Enemy->CanUseAnimationSharing());
	TestTrue(TEXT("Authority has no enemy postprocess at Full gameplay"), Enemy->GetMesh()->GetDisablePostProcessBlueprint());
	TestNull(TEXT("Authority retains no enemy postprocess instance"), Enemy->GetMesh()->GetPostProcessInstance());
	Enemy->SignificanceTier = EFPEnemySignificanceTier::Reduced;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Full, false, false, false, Policy);
	TestEqual(TEXT("Reduced server pose follows gameplay, not a Full local render tier"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->MidRateAnimationTickInterval);
	Enemy->SignificanceTier = EFPEnemySignificanceTier::Background;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Full, false, false, false, Policy);
	TestEqual(TEXT("Background server pose follows gameplay despite Full local rendering"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->FarRateAnimationTickInterval);
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, true, true, false, Policy);
	TestEqual(TEXT("Changing render tier and shadow/RT flags cannot change server pose cadence"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->FarRateAnimationTickInterval);
	TestTrue(TEXT("Full rendering cannot re-enable enemy postprocess at Background gameplay"), Enemy->GetMesh()->GetDisablePostProcessBlueprint());
	// 渲染消融只控制本地表现，不能成为服务器姿态调度开关。
	Policy.bEnableRenderTiering = false;
	Policy.bEnableAnimationTickTiering = false;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Full, false, false, false, Policy);
	TestEqual(TEXT("Disabling client render tiering does not disable server gameplay cadence"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->FarRateAnimationTickInterval);
	Policy.bEnableRenderTiering = Policy.bEnableAnimationTickTiering = true;
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, false, false, false, Policy);

	// 真实感知和攻击事务即时恢复姿态；不是手工改 Combat 的攻击阶段来绕过生产路径。
	auto* Target = World->SpawnActor<AfpstrueCharacter>(FVector(100.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	auto* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Authority pose combat target"), Target) || !TestNotNull(TEXT("Authority pose controller"), Controller)) return false;
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->Possess(Enemy);
	Controller->GetBrainComponent()->SetComponentTickEnabled(false);
	if (!ObserveRenderCombatParticipant(*this, *Controller, *Target)) return false;
	AfpstrueSurroundManager* Manager = Controller->GetSurroundManager();
	if (!TestNotNull(TEXT("Authority pose target owns a real manager"), Manager)) return false;
	Manager->ApproachQueryId = INDEX_NONE;
	++Manager->PositionQueryRevision;
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->SurroundSlots[0].ProjectedApproachLocation = Enemy->GetActorLocation();
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	if (!TestTrue(TEXT("Authority pose enemy obtains a real inner lease"), Manager->HasInnerSlot(Enemy,
		Controller->GetTacticalAssignmentRevision()))) return false;
	if (!TestTrue(TEXT("Simple authority starts a real attack transaction"), Enemy->GetCombatComponent()->TryAttackTarget())) return false;
	TestTrue(TEXT("Attack protects authority Socket and Notify pose"), Enemy->RequiresFullAuthorityPose());
	TestEqual(TEXT("Attack immediately removes the previous Background cooldown"), Enemy->GetMesh()->GetComponentTickInterval(), 0.0f);
	TestTrue(TEXT("Attack refreshes pose even offscreen"), Enemy->GetMesh()->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("Attack disables URO for necessary authority pose"), Enemy->GetMesh()->bEnableUpdateRateOptimizations);
	Enemy->GetCombatComponent()->ResetCombat();
	TestFalse(TEXT("Attack cleanup releases full authority pose"), Enemy->RequiresFullAuthorityPose());
	TestEqual(TEXT("Attack cleanup returns to the unchanged Gameplay Background cadence"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->FarRateAnimationTickInterval);
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, Target, UDamageType::StaticClass());
	TestTrue(TEXT("Actual damage immediately protects authority pose"), Enemy->RequiresFullAuthorityPose());
	TestEqual(TEXT("Hit protection removes animation cooldown"), Enemy->GetMesh()->GetComponentTickInterval(), 0.0f);
	Enemy->RefreshHitAnimationProtection();
	TestFalse(TEXT("Expired hit protection releases full authority pose"), Enemy->RequiresFullAuthorityPose());
	TestEqual(TEXT("Hit protection cleanup returns to Gameplay Background cadence"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->FarRateAnimationTickInterval);

	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Background, false, false, false, Policy);
	TestEqual(TEXT("Dedicated Simple authority also obeys Gameplay Background cadence"),
		Enemy->GetMesh()->GetComponentTickInterval(), Enemy->FarRateAnimationTickInterval);
	TestTrue(TEXT("Idle dedicated Simple authority need not evaluate offscreen poses"),
		Enemy->GetMesh()->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered);
	TestNull(TEXT("Dedicated authority never recreates enemy postprocess"), Enemy->GetMesh()->GetPostProcessInstance());

	// Reference复现旧命中链，不能跟着Simple方案降频或跳过骨骼；用于性能和正确性对照。
	Enemy->ConfigureHitProxies(false);
	TestFalse(TEXT("Reference mode leaves simple hit proxies"), Enemy->UsesSimpleHitProxies());
	TestTrue(TEXT("Reference authority always needs bone-driven hit shapes"), Enemy->RequiresFullAuthorityPose());
	Enemy->ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier::Full, false, false, false, Policy);
	TestEqual(TEXT("Reference restores full update cadence even at Gameplay Background"), Enemy->GetMesh()->GetComponentTickInterval(), 0.0f);
	TestTrue(TEXT("Reference retains full main-pose hit-shape updates"), Enemy->GetMesh()->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("Reference disables URO independently of render visibility"), Enemy->GetMesh()->bEnableUpdateRateOptimizations);
	TestEqual(TEXT("Reference removes local reduced MinLOD restrictions"), Enemy->GetAppliedMinimumLOD(), 0);
	TestFalse(TEXT("Reference authority cannot become a shared-pose follower"), Enemy->CanUseAnimationSharing());
	World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	auto* Sharing = NewObject<UfpstrueEnemyAnimationSharingCoordinator>(Enemy);
	Enemy->AddInstanceComponent(Sharing);
	Sharing->RegisterComponent();
	Sharing->Start(Enemy->GetClass());
	TestNull(TEXT("Listen-server rendering cannot replace authority pose with a shared manager"), UAnimationSharingManager::GetManagerForWorld(World));
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS

// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderPriorityOrderTest,
	"fpstrue.Performance.Significance.RenderPriorityOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证评分比较器具有严格弱序，近似分数、同分 ID 和非有限值不会组成排序环。
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

	// 穷举自反与传递性，再检查主视锥、扩展视锥、分数及 ID 的最终排序。
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

// 将有界 Top-K 与完整排序对照，检查零预算、超额预算和正反输入均选出相同优先对象。
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
	// 使用生产堆插入函数选出 ID，排序仅用于比较集合，不假设堆内部已按优先级排序。
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

	// 对每个预算同时验证正向和反向遍历，排除注册顺序影响最终选择。
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderBudgetRetentionKeyTest,
	"fpstrue.Performance.Significance.RenderBudgetRetentionOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 持有加成只在入堆前产生新键，不能把非传递的“差不多相等”带回比较器。
bool FFpstrueEnemyRenderBudgetRetentionKeyTest::RunTest(const FString& Parameters)
{
	const TArray<FFPEnemyRenderPriorityKey> RawKeys{
		{0.050000000f, 1, false, false},
		{0.050000007f, 2, false, false},
		{0.050000014f, 3, false, false},
		{0.99f, 4, true, true},
		{1.0f, 5, true, true},
		{0.0f, 6, false, true},
		{std::numeric_limits<float>::quiet_NaN(), 7, true, true},
		{std::numeric_limits<float>::infinity(), 8, true, true},
	};
	const FFPEnemyRenderPriorityLess Before;
	for (const float Margin : {0.0f, 0.05f, 1.0f})
	{
		TArray<FFPEnemyRenderPriorityKey> BudgetKeys;
		for (int32 Index = 0; Index < RawKeys.Num(); ++Index)
		{
			BudgetKeys.Add(FPEnemyRenderBudgetPriorityKey(RawKeys[Index], Index % 2 == 0, Margin));
			TestFalse(TEXT("A retention key never sorts before itself"), Before(BudgetKeys.Last(), BudgetKeys.Last()));
		}
		for (int32 A = 0; A < BudgetKeys.Num(); ++A)
		{
			for (int32 B = 0; B < BudgetKeys.Num(); ++B)
			{
				if (Margin == 0.0f)
					TestEqual(TEXT("Zero margin exactly preserves every raw ordering decision"),
						Before(BudgetKeys[A], BudgetKeys[B]), Before(RawKeys[A], RawKeys[B]));
				for (int32 C = 0; C < BudgetKeys.Num(); ++C)
					if (Before(BudgetKeys[A], BudgetKeys[B]) && Before(BudgetKeys[B], BudgetKeys[C]))
						TestTrue(TEXT("Budget retention ordering remains transitive"), Before(BudgetKeys[A], BudgetKeys[C]));
			}
		}
		TArray<FFPEnemyRenderPriorityKey> Sorted = BudgetKeys;
		Sorted.Sort(Before);
		for (const bool bReverse : {false, true})
		{
			TArray<FFPEnemyRenderTopKEntry, TInlineAllocator<8>> Heap;
			for (int32 Step = 0; Step < BudgetKeys.Num(); ++Step)
			{
				const int32 Index = bReverse ? BudgetKeys.Num() - 1 - Step : Step;
				FPEnemyRenderTopKInsert(Heap, 3, Index, BudgetKeys[Index]);
			}
			for (int32 Index = 0; Index < 3; ++Index)
				TestTrue(TEXT("Retention Top-K equals full sorting for either input order"),
					Heap.ContainsByPredicate([&Sorted, Index](const FFPEnemyRenderTopKEntry& Entry)
						{ return Entry.PriorityKey.TieBreakId == Sorted[Index].TieBreakId; }));
		}
	}
	const FFPEnemyRenderPriorityKey NaNHolder = FPEnemyRenderBudgetPriorityKey(RawKeys[6], true, 1.0f);
	TestFalse(TEXT("A retained NaN score does not become a valid score"), FMath::IsFinite(NaNHolder.Score));
	const FFPEnemyRenderPriorityKey ValidZero{0.0f, 9, true, true};
	TestTrue(TEXT("A finite score still outranks a retained nonfinite score within the same frustum"), Before(ValidZero, NaNHolder));
	TestEqual(TEXT("Nonfinite retention configuration disables the score adjustment"),
		FPEnemyRenderBudgetPriorityKey(RawKeys[0], true, std::numeric_limits<float>::quiet_NaN()).Score, RawKeys[0].Score);
	const FFPEnemyRenderPriorityKey Outside{1.0f, 10, false, false};
	const FFPEnemyRenderPriorityKey Expanded{0.0f, 11, false, true};
	const FFPEnemyRenderPriorityKey Primary{0.0f, 12, true, true};
	TestTrue(TEXT("New expanded-frustum priority is never blocked by an outside holder"),
		Before(FPEnemyRenderBudgetPriorityKey(Expanded, false, 1.0f), FPEnemyRenderBudgetPriorityKey(Outside, true, 1.0f)));
	TestTrue(TEXT("New primary-frustum priority is never blocked by an expanded holder"),
		Before(FPEnemyRenderBudgetPriorityKey(Primary, false, 1.0f), FPEnemyRenderBudgetPriorityKey(Expanded, true, 1.0f)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderScoringSnapshotTest,
	"fpstrue.Performance.Significance.RenderScoringSnapshotEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 保留优化前的纯数学公式作为测试预言机，确保公共量外提不会修改视锥、近期可见性、分数或 Top-K 排序输入。
bool FFpstrueEnemyRenderScoringSnapshotTest::RunTest(const FString& Parameters)
{
	const auto ReferenceSample = [](const FFPEnemyRenderViewContext& View, const FFPEnemyRenderSignificancePolicy& Policy,
		const FVector& Origin, float Radius, float& LastPrimaryTime)
	{
		FFPEnemyRenderSignificanceSample Sample;
		const FVector Offset = Origin - View.ViewLocation;
		Sample.Distance = Offset.Size();
		const FRotationMatrix Rotation(View.ViewRotation);
		const float Forward = FVector::DotProduct(Offset, Rotation.GetUnitAxis(EAxis::X));
		const float Horizontal = FVector::DotProduct(Offset, Rotation.GetUnitAxis(EAxis::Y));
		const float Vertical = FVector::DotProduct(Offset, Rotation.GetUnitAxis(EAxis::Z));
		const float HalfFOV = FMath::DegreesToRadians(FMath::Clamp(View.HorizontalFOVDegrees, 5.0f, 170.0f) * 0.5f);
		const float TanHorizontal = FMath::Max(FMath::Tan(HalfFOV), 0.01f);
		const float TanVertical = TanHorizontal / FMath::Max(View.AspectRatio, 0.1f);
		const float SafeForward = FMath::Max(Forward, 1.0f);
		const float HorizontalRadius = Radius / (SafeForward * TanHorizontal);
		const float VerticalRadius = Radius / (SafeForward * TanVertical);
		const float HorizontalPosition = Horizontal / (SafeForward * TanHorizontal);
		const float VerticalPosition = Vertical / (SafeForward * TanVertical);
		const auto Intersects = [&](float Margin)
		{
			return Forward + Radius > 0.0f && FMath::Abs(HorizontalPosition) <= 1.0f + Margin + HorizontalRadius &&
				FMath::Abs(VerticalPosition) <= 1.0f + Margin + VerticalRadius;
		};
		Sample.bInPrimaryFrustum = Intersects(0.0f);
		Sample.bInExpandedFrustum = Intersects(FMath::Max(Policy.ExpandedFrustumMargin, 0.0f));
		Sample.FrustumFactor = Sample.bInPrimaryFrustum ? 1.0f : (Sample.bInExpandedFrustum ? 0.5f : 0.0f);
		const float RecentGrace = FMath::Max(Policy.RecentFrustumGraceSeconds, 0.0f);
		if (Sample.bInPrimaryFrustum)
		{
			LastPrimaryTime = View.TimeSeconds;
			Sample.RecentFrustumFactor = 1.0f;
		}
		else if (RecentGrace > 0.0f)
		{
			Sample.RecentFrustumFactor = 1.0f - FMath::Clamp((View.TimeSeconds - LastPrimaryTime) / RecentGrace, 0.0f, 1.0f);
		}
		const float ScreenRadius = Forward > 0.0f ? FMath::Max(HorizontalRadius, VerticalRadius) : 0.0f;
		Sample.ScreenCoverageFactor = Sample.bInExpandedFrustum
			? FMath::Clamp(ScreenRadius / FMath::Max(Policy.ScreenRadiusForFullScore, 0.001f), 0.0f, 1.0f) : 0.0f;
		const float Near = FMath::Max(Policy.NearDistance, 0.0f);
		const float Far = FMath::Max(Policy.FarDistance, Near + 1.0f);
		Sample.DistanceFactor = 1.0f - FMath::Clamp((Sample.Distance - Near) / (Far - Near), 0.0f, 1.0f);
		const float FrustumWeight = FMath::Max(Policy.FrustumWeight, 0.0f);
		const float ScreenWeight = FMath::Max(Policy.ScreenCoverageWeight, 0.0f);
		const float RecentWeight = FMath::Max(Policy.RecentFrustumWeight, 0.0f);
		const float DistanceWeight = FMath::Max(Policy.DistanceWeight, 0.0f);
		const float Sum = FMath::Max(FrustumWeight + ScreenWeight + RecentWeight + DistanceWeight, KINDA_SMALL_NUMBER);
		Sample.Score = FMath::Clamp((Sample.FrustumFactor * FrustumWeight + Sample.ScreenCoverageFactor * ScreenWeight +
			Sample.RecentFrustumFactor * RecentWeight + Sample.DistanceFactor * DistanceWeight) / Sum, 0.0f, 1.0f);
		return Sample;
	};

	// 正面、边界、背面、近零深度及远距均覆盖；转镜、变焦、视口比例变化使用新一轮快照。
	const TArray<FVector> Origins{
		FVector(0.0f), FVector(1000.0f, 0.0f, 0.0f), FVector(1000.0f, 1100.0f, 0.0f),
		FVector(-1000.0f, 0.0f, 0.0f), FVector(-1.0f, 100.0f, 500.0f), FVector(12000.0f, 2000.0f, -1000.0f)};
	for (int32 ViewIndex = 0; ViewIndex < 4; ++ViewIndex)
	{
		FFPEnemyRenderViewContext View;
		View.ViewLocation = FVector(35.0f, -40.0f, 75.0f) * ViewIndex;
		View.ViewRotation = FRotator(12.0f * ViewIndex, 80.0f * ViewIndex, -3.0f * ViewIndex);
		View.HorizontalFOVDegrees = ViewIndex == 0 ? 90.0f : (ViewIndex == 1 ? 20.0f : (ViewIndex == 2 ? 180.0f : 0.0f));
		View.AspectRatio = ViewIndex == 3 ? 0.0f : 16.0f / 9.0f;
		View.TimeSeconds = 10.0f + ViewIndex * 0.2f;
		for (int32 PolicyIndex = 0; PolicyIndex < 3; ++PolicyIndex)
		{
			FFPEnemyRenderSignificancePolicy Policy;
			if (PolicyIndex == 1)
			{
				Policy.FrustumWeight = 2.0f;
				Policy.ScreenCoverageWeight = 3.0f;
				Policy.RecentFrustumWeight = 0.0f;
				Policy.DistanceWeight = -1.0f;
				Policy.ExpandedFrustumMargin = 0.0f;
				Policy.RecentFrustumGraceSeconds = 0.0f;
			}
			else if (PolicyIndex == 2)
			{
				Policy.FrustumWeight = Policy.ScreenCoverageWeight = Policy.RecentFrustumWeight = Policy.DistanceWeight = 0.0f;
				Policy.NearDistance = -100.0f;
				Policy.FarDistance = -200.0f;
				Policy.ScreenRadiusForFullScore = 0.0f;
			}
			const FFPEnemyRenderScoringContext Context(View, Policy);
			for (int32 BoundsIndex = 0; BoundsIndex < Origins.Num(); ++BoundsIndex)
			{
				const float Radius = BoundsIndex == 0 ? 1.0f : 100.0f;
				float ReferenceTime = 9.9f;
				float SnapshotTime = ReferenceTime;
				const auto Expected = ReferenceSample(View, Policy, Origins[BoundsIndex], Radius, ReferenceTime);
				const auto Actual = Context.EvaluateBounds(Origins[BoundsIndex], Radius, SnapshotTime);
				const FString Label = FString::Printf(TEXT("View%d Policy%d Bounds%d"), ViewIndex, PolicyIndex, BoundsIndex);
				// 排序直接比较 float 分数，使用零容差，不能让普通近似断言掩盖预算边界变化。
				TestEqual(Label + TEXT(" Score"), Actual.Score, Expected.Score, 0.0f);
				TestEqual(Label + TEXT(" Distance"), Actual.Distance, Expected.Distance, 0.0f);
				TestEqual(Label + TEXT(" FrustumFactor"), Actual.FrustumFactor, Expected.FrustumFactor, 0.0f);
				TestEqual(Label + TEXT(" ScreenCoverageFactor"), Actual.ScreenCoverageFactor, Expected.ScreenCoverageFactor, 0.0f);
				TestEqual(Label + TEXT(" RecentFrustumFactor"), Actual.RecentFrustumFactor, Expected.RecentFrustumFactor, 0.0f);
				TestEqual(Label + TEXT(" DistanceFactor"), Actual.DistanceFactor, Expected.DistanceFactor, 0.0f);
				TestEqual(Label + TEXT(" Primary"), Actual.bInPrimaryFrustum, Expected.bInPrimaryFrustum);
				TestEqual(Label + TEXT(" Expanded"), Actual.bInExpandedFrustum, Expected.bInExpandedFrustum);
				TestEqual(Label + TEXT(" History"), SnapshotTime, ReferenceTime, 0.0f);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
