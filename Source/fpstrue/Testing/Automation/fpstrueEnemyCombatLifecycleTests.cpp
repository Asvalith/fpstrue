// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/Performance/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Enemies/fpstrueEnemyCombatConfig.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "AnimationSharingManager.h"
#include "BrainComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyCombatLifecycleTest,
	"fpstrue.Gameplay.Combat.AttackProtectionAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyCombatLifecycleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("The native BT has an AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld())
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}

	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
			Enemy->SetActorEnableCollision(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* Enemy = SpawnEnemy();
	AfpstrueEnemyCharacter* Probe = SpawnEnemy();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueSurroundManager* Surround = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("Enemy"), Enemy) || !TestNotNull(TEXT("Budget probe"), Probe)
		|| !TestNotNull(TEXT("Player"), Target) || !TestNotNull(TEXT("Controller"), Controller)
		|| !TestNotNull(TEXT("Surround manager"), Surround)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(200.0f, 0.0f, 0.0f));
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, Surround);
	// 直接驱动攻击入口，避免 BT 下一轮重新申请名额掩盖清理测试。
	if (!TestNotNull(TEXT("Behavior brain"), Controller->GetBrainComponent())) return false;
	Controller->GetBrainComponent()->StopLogic(TEXT("Combat lifecycle fixture"));
	FIntProperty* BudgetProperty = FindFProperty<FIntProperty>(Surround->GetClass(), TEXT("MaxActiveAttackers"));
	if (!TestNotNull(TEXT("Editable attack budget"), BudgetProperty)) return false;
	BudgetProperty->SetPropertyValue_InContainer(Surround, 1);
	UfpstrueEnemyCombatComponent* Combat = Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>();
	if (!TestNotNull(TEXT("Native combat component"), Combat)) return false;
	TestEqual(TEXT("Notify reads the existing component without another lookup"), Enemy->GetCombatComponent(), Combat);
	// 合并源文件不能改变已保存动画资产使用的反射类路径；同一 Notify 对象不保存某个敌人的事务。
	TestEqual(TEXT("Saved attack-window class path remains valid"),
		LoadClass<UAnimNotifyState>(nullptr, TEXT("/Script/fpstrue.fpstrueAnimNotifyState_AttackWindow")),
		UfpstrueAnimNotifyState_AttackWindow::StaticClass());
	UfpstrueAnimNotifyState_AttackWindow* AttackWindow = NewObject<UfpstrueAnimNotifyState_AttackWindow>(World);
	const FAnimNotifyEventReference NotifyContext;
	AttackWindow->NotifyBegin(nullptr, nullptr, 1.0f, NotifyContext);
	AttackWindow->NotifyBegin(Target->GetMesh(), nullptr, 1.0f, NotifyContext);
	TestTrue(TEXT("A new combat transaction is Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);

	// 与共享注销回归一致：只构造最小插件登记，不加载项目动画资产；注销走真实插件实现。
	if (!TestTrue(TEXT("Animation Sharing is enabled"), UAnimationSharingManager::AnimationSharingEnabled())) return false;
	UfpstrueEnemyAnimationSharingCoordinator* Sharing = NewObject<UfpstrueEnemyAnimationSharingCoordinator>(Surround);
	Surround->AddInstanceComponent(Sharing);
	Sharing->RegisterComponent();
	UAnimationSharingManager* Manager = NewObject<UAnimationSharingManager>(World);
	Sharing->SharingManager = Manager;
	const FArrayProperty* SkeletonDataProperty = FindFProperty<FArrayProperty>(Manager->GetClass(), TEXT("PerSkeletonData"));
	const FObjectPropertyBase* EntryProperty = SkeletonDataProperty != nullptr
		? CastField<FObjectPropertyBase>(SkeletonDataProperty->Inner) : nullptr;
	if (!TestNotNull(TEXT("Plugin skeleton data schema"), EntryProperty)) return false;
	UAnimSharingInstance* Data = static_cast<UAnimSharingInstance*>(NewObject<UObject>(Manager, EntryProperty->PropertyClass));
	SkeletonDataProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UAnimSharingInstance>>>(Manager)->Add(Data);
	USkeletalMeshComponent* Leader = NewObject<USkeletalMeshComponent>(Surround);
	Surround->AddInstanceComponent(Leader);
	Leader->RegisterComponent();
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	Mesh->SetLeaderPoseComponent(Leader, true);
	Mesh->SetComponentTickEnabled(false);
	Mesh->PrimaryComponentTick.bCanEverTick = false;
	Mesh->bIgnoreLeaderPoseComponentLOD = true;
	Mesh->SetComponentTickInterval(0.5f);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	Enemy->GetCharacterMovement()->SetComponentTickInterval(0.5f);
	Data->RegisteredActors.Add(Enemy);
	Data->PerActorData.AddZeroed_GetRef().ComponentIndices.Add(0);
	FPerComponentData& ComponentData = Data->PerComponentData.AddZeroed_GetRef();
	ComponentData.ActorIndex = 0;
	ComponentData.Component = Mesh;
	Sharing->RegisteredActorHandles.Add(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy), 0);
	Enemy->SetAnimationSharingCoordinator(Sharing);

	TestTrue(TEXT("Attacker takes the single permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestFalse(TEXT("Another enemy cannot take the occupied permit"), Surround->TryAcquireAttackPermission(Probe));
	if (!TestTrue(TEXT("A real attack starts"), Combat->TryAttackTarget())) return false;
	TestTrue(TEXT("An active attack owns its failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("Starting an attack enters Windup"), Combat->AttackPhase == EFPEnemyAttackPhase::Windup);
	TestEqual(TEXT("Attack restores full-rate movement before animation"), Enemy->GetCharacterMovement()->GetComponentTickInterval(), 0.0f);
	TestEqual(TEXT("Attack restores full-rate animation"), Mesh->GetComponentTickInterval(), 0.0f);
	TestTrue(TEXT("Attack always refreshes pose and bones"),
		Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("Attack detaches from the animation leader"), Mesh->LeaderPoseComponent.IsValid());
	TestFalse(TEXT("Attack restores independent LOD control"), Mesh->bIgnoreLeaderPoseComponentLOD);
	TestTrue(TEXT("Attack restores animation tick eligibility"), Mesh->PrimaryComponentTick.bCanEverTick);
	TestEqual(TEXT("Attack removes the real plugin registration"), Data->RegisteredActors.Num(), 0);
	TestEqual(TEXT("Attack removes the coordinator handle"), Sharing->GetRegisteredEnemyCount(), 0);

	// 两根临时骨骼提供真实 Socket 查询，无需加载 Montage 或项目模型。
	USkeletalMesh* BladeMesh = NewObject<USkeletalMesh>(Enemy, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(BladeMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("weapontop"), TEXT("weapontop"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("weaponend"), TEXT("weaponend"), 0), FTransform(FVector(50.0f, 0.0f, 0.0f)));
	}
	BladeMesh->CalculateInvRefMatrices();
	Mesh->bEnableAnimation = false;
	Mesh->SetSkeletalMesh(BladeMesh);
	AttackWindow->NotifyBegin(Mesh, nullptr, 1.0f, NotifyContext);
	TestTrue(TEXT("Anonymous Notify cannot open an unbound attack"), Combat->AttackPhase == EFPEnemyAttackPhase::Windup);
	// 本测试关注窗口/伤害规则；真实播放身份与停止重入由 ExplicitPlaybackAndStopReentry 覆盖。
	Combat->BeginAttackWindow();
	TestTrue(TEXT("A valid window enters Active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	const FVector HistoricalSample(11.0f, 22.0f, 33.0f);
	Combat->PreviousWeaponBase = HistoricalSample;
	Combat->BeginAttackWindow();
	TestTrue(TEXT("Duplicate Begin preserves the previous frame sample"), Combat->PreviousWeaponBase == HistoricalSample);
	Combat->EndAttackWindow();
	TestTrue(TEXT("Closing a valid window enters Recovery"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	Combat->BeginAttackWindow();
	TestTrue(TEXT("Another window before a hit may become Active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	// 环境规则不依赖 NavMesh：近战不可穿越阻挡 Visibility 的墙，也不能攻击明显不同楼层。
	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Melee blocker owner"), Wall)) return false;
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(WallBox);
	Wall->SetRootComponent(WallBox);
	WallBox->SetBoxExtent(FVector(10.0f, 100.0f, 150.0f));
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WallBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	WallBox->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	WallBox->RegisterComponent();
	Wall->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
	TestFalse(TEXT("A wall added after attack start blocks damage"), Combat->TryApplyAttackDamage(Target));
	float SampledDistanceSquared, SampledAttackRange;
	TestFalse(TEXT("BT reach snapshot includes the same wall rule"), Combat->SampleAttackReach(SampledDistanceSquared, SampledAttackRange));
	TestEqual(TEXT("Blocked snapshot still provides distance for pursuit"), SampledDistanceSquared, 40000.0f);
	TestTrue(TEXT("Blocked snapshot still provides range for decision frequency"), SampledAttackRange >= Combat->GetConfiguredAttackRange());
	Wall->Destroy();
	Target->SetActorLocation(FVector(200.0f, 0.0f, 1000.0f));
	TestFalse(TEXT("Moving to another floor after attack start blocks damage"), Combat->TryApplyAttackDamage(Target));
	TestFalse(TEXT("BT reach snapshot also rejects another floor"), Combat->SampleAttackReach(SampledDistanceSquared, SampledAttackRange));
	Target->SetActorLocation(FVector(200.0f, 0.0f, 0.0f));
	TestTrue(TEXT("A new reach sample observes the removed blocker"), Combat->SampleAttackReach(SampledDistanceSquared, SampledAttackRange));
	TestTrue(TEXT("The first valid player hit commits damage"), Combat->TryApplyAttackDamage(Target));
	TestTrue(TEXT("A committed hit closes the window"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	Combat->BeginAttackWindow();
	TestTrue(TEXT("A hit transaction cannot reopen its window"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	TestFalse(TEXT("The same transaction cannot commit damage again"), Combat->TryApplyAttackDamage(Target));

	Combat->EndAttackWindow();
	AttackWindow->NotifyTick(Mesh, nullptr, 0.016f, NotifyContext);
	Combat->EndAttackWindow();
	TestTrue(TEXT("Repeated window close does not finish an attack"), Combat->IsAttacking());
	TestFalse(TEXT("Window close does not release the attack permit"), Surround->TryAcquireAttackPermission(Probe));
	Combat->ResetCombat();
	TestFalse(TEXT("Reset ends the transaction"), Combat->IsAttacking());
	TestFalse(TEXT("Reset clears the failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("Reset returns to Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);
	TestTrue(TEXT("Reset releases the permit while the enemy is alive"), Surround->TryAcquireAttackPermission(Probe));
	Combat->ResetCombat();
	TestFalse(TEXT("Repeated reset cannot release another enemy's permit"), Surround->TryAcquireAttackPermission(Enemy));
	Surround->ReleaseAttackPermission(Probe);

	TestTrue(TEXT("The original enemy can take the permit again"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("Reset does not impose normal-finish cooldown"), Combat->TryAttackTarget());
	Combat->FinishAttack();
	TestTrue(TEXT("Normal completion returns to Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);
	TestFalse(TEXT("Normal completion uses the same timer cleanup"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestFalse(TEXT("Normal completion applies its cooldown"), Combat->CanStartAttack());
	// 不等待真实时间：仅移动测试事务的冷却基准，重新建立 EndPlay 所需的活动事务。
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("A cooled-down enemy takes a new permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("A cooled-down transaction restarts"), Combat->TryAttackTarget());
	Controller->StopAI();
	TestFalse(TEXT("StopAI cancels the attack, not only the decision tree"), Combat->IsAttacking());
	TestFalse(TEXT("StopAI cancels the old attack timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("StopAI releases the captured permit issuer"), Surround->TryAcquireAttackPermission(Probe));
	Surround->ReleaseAttackPermission(Probe);
	Controller->InitializeCombatContext(Target, Surround);
	TestTrue(TEXT("The stopped enemy can acquire a fresh permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("A new context begins a fresh attack"), Combat->TryAttackTarget());
	AfpstrueCharacter* ReplacementTarget = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Replacement player"), ReplacementTarget)) return false;
	ReplacementTarget->SetActorEnableCollision(false);
	ReplacementTarget->SetActorLocation(FVector(200.0f, 0.0f, 0.0f));
	Controller->InitializeCombatContext(ReplacementTarget, Surround);
	TestFalse(TEXT("Replacing combat target cancels the old attack instead of retargeting its damage"), Combat->IsAttacking());
	TestFalse(TEXT("Old target context is cleared"), Combat->AttackTarget.IsValid());
	TestTrue(TEXT("Replacement target permits a new attack"), Combat->TryAttackTarget());
	TestTrue(TEXT("New attack captures the replacement target"), Combat->AttackTarget.Get() == ReplacementTarget);
	// 只销毁组件，保留有效 Enemy，避免 Manager 的失效弱引用兜底让遗漏清理的测试也通过。
	Combat->DestroyComponent();
	TestFalse(TEXT("Component EndPlay ends its attack"), Combat->IsAttacking());
	TestTrue(TEXT("Component EndPlay releases its living owner's permit"), Surround->TryAcquireAttackPermission(Probe));
	Combat->HandleAttackFinishedNotify();
	TestFalse(TEXT("A finish callback after EndPlay cannot revive the transaction"), Combat->IsAttacking());
	Surround->ReleaseAttackPermission(Probe);
	Enemy->SetAnimationSharingCoordinator(nullptr);
	Sharing->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueSurroundCacheLifecycleTest,
	"fpstrue.AI.Surround.CacheAndSlotOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueSurroundCacheLifecycleTest::RunTest(const FString& Parameters)
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
	AfpstrueSurroundManager* Manager = World->SpawnActor<AfpstrueSurroundManager>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Manager"), Manager) || !TestNotNull(TEXT("Target"), Target)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector::ZeroVector);
	Manager->SetTargetCharacter(Target);
	TestTrue(TEXT("Missing navigation schedules retry independently of location validity"), Manager->bNeedsNavigationRetry);
	Target->SetActorLocation(FVector(190.0f, 0.0f, 0.0f));
	Manager->NextNavigationRetryTime = World->GetTimeSeconds() + 10.0;
	Manager->UpdateSharedTargetSnapshot(false);
	TestEqual(TEXT("Before TTL, sub-threshold motion reuses snapshot"), Manager->CachedTargetLocation.X, 0.0);
	Manager->SharedTargetSnapshotTime -= Manager->SharedTargetMaxAge + 0.1;
	Manager->UpdateSharedTargetSnapshot(false);
	TestEqual(TEXT("Expired snapshot catches sub-threshold motion even after it stops"), Manager->CachedTargetLocation.X, 190.0);
	Manager->NextNavigationRetryTime = World->GetTimeSeconds() - 1.0;
	Manager->UpdateSharedTargetSnapshot(false);
	TestTrue(TEXT("Stationary failed projection is retried and rescheduled"), Manager->NextNavigationRetryTime > World->GetTimeSeconds());
	// 模拟过去成功的缓存进入新导航世代；目标未动且没有失败重试，也必须重新向当前导航系统取值。
	Manager->bNeedsNavigationRetry = false;
	Manager->NavigationCacheTime = World->GetTimeSeconds() - Manager->NavigationCacheMaxAge - 0.1;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->UpdateSharedTargetSnapshot(false);
	TestFalse(TEXT("Expired success cache is reprojected instead of trusted forever"), Manager->SurroundSlots[0].bHasProjectedApproachLocation);
	TestEqual(TEXT("Reprojection refreshes its own age independently of target motion"), Manager->NavigationCacheTime, World->GetTimeSeconds());

	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* OldEnemy = SpawnEnemy();
	AfpstrueEnemyCharacter* NewEnemy = SpawnEnemy();
	if (!TestNotNull(TEXT("Old occupant"), OldEnemy) || !TestNotNull(TEXT("New occupant"), NewEnemy)) return false;
	Manager->EnemyToSlot.Add(OldEnemy, 0);
	Manager->SurroundSlots[0].Occupant = NewEnemy;
	Manager->ReleaseSurroundSlot(OldEnemy);
	TestTrue(TEXT("Stale slot mapping does not release the new occupant"), Manager->SurroundSlots[0].Occupant.Get() == NewEnemy);
	Manager->EnemyToSlot.Add(TWeakObjectPtr<AfpstrueEnemyCharacter>(OldEnemy), 0);
	OldEnemy->Destroy();
	Manager->CleanupInvalidEntries();
	TestTrue(TEXT("Expired weak-key cleanup also preserves a reused slot"), Manager->SurroundSlots[0].Occupant.Get() == NewEnemy);
	Manager->SurroundSlots[0].Occupant.Reset();
	Manager->SurroundSlots[0].bHasProjectedSlotLocation = true;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = false;
	TestEqual(TEXT("An unreachable approach is never reserved based only on a valid standing point"),
		Manager->FindBestFreeSlot(FVector::ZeroVector), INDEX_NONE);
	Manager->SetTargetCharacter(nullptr);
	FVector Snapshot;
	TestFalse(TEXT("Clearing target invalidates the shared snapshot"), Manager->GetSharedTargetSnapshot(Snapshot));
	TestEqual(TEXT("Clearing target releases old allocations"), Manager->EnemyToSlot.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyCombatStateConfigTest,
	"fpstrue.Gameplay.Combat.ConfigurationSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyCombatStateConfigTest::RunTest(const FString& Parameters)
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
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Temporary configuration owner"), Owner)) return false;
	UfpstrueEnemyCombatComponent* Legacy = NewObject<UfpstrueEnemyCombatComponent>(Owner);
	Legacy->AttackRange = 321.0f;
	Legacy->AttackDamage = 13.0f;
	Owner->AddInstanceComponent(Legacy);
	Legacy->RegisterComponent();
	TestEqual(TEXT("No asset preserves the existing Blueprint range"), Legacy->AttackRange, 321.0f);
	TestEqual(TEXT("No asset preserves the existing Blueprint damage"), Legacy->AttackDamage, 13.0f);

	UfpstrueEnemyCombatConfig* Configuration = NewObject<UfpstrueEnemyCombatConfig>(Owner);
	Configuration->AttackRange = 654.0f;
	Configuration->AttackDamage = 17.0f;
	Configuration->AttackInterval = 2.0f;
	Configuration->AttackAnimationDuration = 1.7f;
	Configuration->AttackFailSafeDuration = 6.0f;
	Configuration->AttackCompletionGracePeriod = 0.3f;
	Configuration->WeaponTraceStartSocketName = TEXT("BladeRoot");
	Configuration->WeaponTraceEndSocketName = TEXT("BladeTip");
	Configuration->WeaponTraceRadius = 12.0f;
	Configuration->WeaponTraceSampleCount = 6;
	UfpstrueEnemyCombatComponent* Configured = NewObject<UfpstrueEnemyCombatComponent>(Owner);
	Configured->CombatConfiguration = Configuration;
	Configured->AttackRange = 999.0f;
	Configured->bDrawAttackTrace = true;
	Owner->AddInstanceComponent(Configured);
	Configured->RegisterComponent();
	TestEqual(TEXT("The asset wins over legacy Blueprint range"), Configured->AttackRange, Configuration->AttackRange);
	TestEqual(TEXT("Damage comes from the same source"), Configured->AttackDamage, Configuration->AttackDamage);
	TestEqual(TEXT("Cooldown comes from the same source"), Configured->AttackInterval, Configuration->AttackInterval);
	TestEqual(TEXT("Animation duration comes from the same source"), Configured->AttackAnimationDuration, Configuration->AttackAnimationDuration);
	TestEqual(TEXT("Fail-safe comes from the same source"), Configured->AttackFailSafeDuration, Configuration->AttackFailSafeDuration);
	TestEqual(TEXT("Grace period comes from the same source"), Configured->AttackCompletionGracePeriod, Configuration->AttackCompletionGracePeriod);
	TestEqual(TEXT("Start Socket comes from the same source"), Configured->WeaponTraceStartSocketName, Configuration->WeaponTraceStartSocketName);
	TestEqual(TEXT("End Socket comes from the same source"), Configured->WeaponTraceEndSocketName, Configuration->WeaponTraceEndSocketName);
	TestEqual(TEXT("Trace radius comes from the same source"), Configured->WeaponTraceRadius, Configuration->WeaponTraceRadius);
	TestEqual(TEXT("Sample count comes from the same source"), Configured->WeaponTraceSampleCount, Configuration->WeaponTraceSampleCount);
	TestTrue(TEXT("The local debug switch is not gameplay configuration"), Configured->bDrawAttackTrace);
	TestEqual(TEXT("BeginPlay initializes cooldown using the selected interval"),
		Configured->LastAttackTime, static_cast<float>(World->GetTimeSeconds() - Configuration->AttackInterval));
	Configuration->AttackDamage = 99.0f;
	TestEqual(TEXT("Asset edits do not silently hot-reload an active instance"), Configured->AttackDamage, 17.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyAttackPlaybackTest,
	"fpstrue.Gameplay.Combat.ExplicitPlaybackAndStopReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyAttackPlaybackTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->CreateAISystem();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Target"), Target) || !TestNotNull(TEXT("Controller"), Controller)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(200.0, 0.0, 0.0));
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, nullptr);
	Controller->GetBrainComponent()->StopLogic(TEXT("Explicit playback fixture"));
	UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	USkeletalMesh* BladeMesh = NewObject<USkeletalMesh>(Enemy);
	{
		FReferenceSkeletonModifier Modifier(BladeMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("weapontop"), TEXT("weapontop"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("weaponend"), TEXT("weaponend"), 0), FTransform(FVector(50.0, 0.0, 0.0)));
	}
	BladeMesh->CalculateInvRefMatrices();
	USkeleton* PlaybackSkeleton = NewObject<USkeleton>(Enemy);
	BladeMesh->SetSkeleton(PlaybackSkeleton);
	PlaybackSkeleton->MergeAllBonesToBoneTree(BladeMesh);
	Mesh->SetSkeletalMesh(BladeMesh);
	// 引擎真实分配 Montage 实例；不向 MontageInstances/事务字段注入伪造播放 ID。
	UAnimInstance* Anim = NewObject<UAnimInstance>(Mesh);
	Mesh->AnimScriptInstance = Anim;
	Anim->InitializeAnimation();
	const auto MakeMontage = [Anim](FName Slot, FName Group)
	{
		Anim->CurrentSkeleton->SetSlotGroupName(Slot, Group);
		UAnimMontage* Montage = NewObject<UAnimMontage>(Anim);
		Montage->SetSkeleton(Anim->CurrentSkeleton);
		Montage->SetCompositeLength(2.0f);
		Montage->SlotAnimTracks[0].SlotName = Slot;
		// 运行时数据构造，不依赖 WITH_EDITOR 的 AddAnimCompositeSection 编辑器辅助接口。
		FCompositeSection& Section = Montage->CompositeSections.AddDefaulted_GetRef();
		Section.SectionName = TEXT("Default");
		Section.Link(Montage, 0.0f);
		return Montage;
	};
	UAnimMontage* Unrelated = MakeMontage(TEXT("HitSlot"), TEXT("HitGroup"));
	UAnimMontage* Attack = MakeMontage(TEXT("AttackSlot"), TEXT("AttackGroup"));
	TestTrue(TEXT("Unrelated slot plays first"), Anim->Montage_Play(Unrelated, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false) > 0.0f);
	if (!TestTrue(TEXT("Attack A starts through business entrypoint"), Combat->TryAttackTarget())) return false;
	const int64 ActionA = Combat->AttackSequence;
	if (!TestTrue(TEXT("The explicit attack command plays and binds A"), Combat->PlayAttackMontageForAttack(ActionA, Attack))) return false;
	const int32 PlaybackA = Combat->AttackPlayback.MontageInstanceId;
	TestTrue(TEXT("An earlier unrelated slot does not steal attack ownership"), Combat->AttackPlayback.Montage.Get() == Attack);
	TestNotNull(TEXT("Unrelated slot remains active"), Anim->GetActiveInstanceForMontage(Unrelated));
	Combat->ResetCombat();
	TestNotNull(TEXT("Reset only stops captured attack, not unrelated slot"), Anim->GetActiveInstanceForMontage(Unrelated));
	if (!TestTrue(TEXT("Attack B starts"), Combat->TryAttackTarget())) return false;
	const int64 ActionB = Combat->AttackSequence;
	TestFalse(TEXT("Late command A cannot play into B"), Combat->PlayAttackMontageForAttack(ActionA, Attack));
	if (!TestTrue(TEXT("Same Montage asset is explicitly replayed for B"), Combat->PlayAttackMontageForAttack(ActionB, Attack))) return false;
	TestNotEqual(TEXT("Real replay receives new instance identity"), Combat->AttackPlayback.MontageInstanceId, PlaybackA);
	TestFalse(TEXT("Another slot cannot overwrite bound playback"), Combat->BindAttackMontageForAttack(ActionB, Mesh, Unrelated));
	FAnimNotifyEventReference OldNotify(nullptr, Attack);
	OldNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(PlaybackA);
	FAnimNotifyEventReference CurrentNotify(nullptr, Attack);
	CurrentNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(Combat->AttackPlayback.MontageInstanceId);
	UfpstrueAnimNotifyState_AttackWindow* Window = NewObject<UfpstrueAnimNotifyState_AttackWindow>(World);
	UfpstrueAnimNotify_AttackFinished* Finished = NewObject<UfpstrueAnimNotify_AttackFinished>(World);
	Window->NotifyBegin(Mesh, Attack, 0.5f, CurrentNotify);
	TestTrue(TEXT("Current real playback opens damage window"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	Window->NotifyEnd(Mesh, Attack, OldNotify);
	Finished->Notify(Mesh, Attack, OldNotify);
	FBranchingPointNotifyPayload OldBranch(Mesh, Attack, nullptr, PlaybackA);
	Window->BranchingPointNotifyEnd(OldBranch);
	TestTrue(TEXT("Old queued and branching notifications cannot close B"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	TestFalse(TEXT("A different mesh cannot supply the current attack identity"), Combat->IsAttackNotifyCurrent(Target->GetMesh(), CurrentNotify));
	Combat->HandleAttackFinishedNotify();
	TestTrue(TEXT("Deprecated identity-free completion fails closed"), Combat->IsAttacking());
	Finished->Notify(Mesh, Attack, CurrentNotify);
	TestFalse(TEXT("Current native completion finishes B"), Combat->IsAttacking());
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("Attack C starts after cooldown"), Combat->TryAttackTarget());
	TestTrue(TEXT("Attack C binds playback"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, Attack));
	FAnimMontageInstance* Instance = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Stop reentry uses engine playback"), Instance)) return false;
	int32 StopCallbacks = 0;
	bool bRestarted = false;
	Instance->OnMontageBlendingOutStarted.BindLambda([&](UAnimMontage*, bool)
	{
		++StopCallbacks;
		Controller->InitializeCombatContext(Target, nullptr);
		bRestarted |= Combat->TryAttackTarget();
	});
	Controller->StopAI();
	TestEqual(TEXT("Real Montage stop dispatches the reentry callback"), StopCallbacks, 1);
	TestFalse(TEXT("Stop callback cannot start a replacement attack"), bRestarted);
	TestFalse(TEXT("Whole AI stop leaves no attack"), Combat->IsAttacking());
	TestFalse(TEXT("Stopped controller keeps command gate closed"), Controller->AcceptsCombatCommands());
	TestNull(TEXT("Stop callback cannot re-inject the target"), Controller->GetTargetCharacter());
	Instance->OnMontageBlendingOutStarted.Unbind();

	// 没有 AttackFinished Notify 的动画也通过真实实例自然结束收尾，而不是等待保护 Timer。
	Controller->InitializeCombatContext(Target, nullptr);
	TestTrue(TEXT("A new session accepts the next attack"), Combat->TryAttackTarget());
	TestTrue(TEXT("Natural-completion attack plays"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, Attack));
	FAnimMontageInstance* NaturalPlayback = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Natural playback instance"), NaturalPlayback)) return false;
	for (int32 Step = 0; Step < 30 && Combat->IsAttacking(); ++Step)
	{
		// 引擎推进实际 Montage 权重/时间线并调用 Terminate；未手动执行业务完成委托或伪造播放身份。
		NaturalPlayback->UpdateWeight(0.1f);
		NaturalPlayback->Advance(0.1f, nullptr, false);
	}
	TestFalse(TEXT("Natural Montage completion closes the attack without a finish Notify"), Combat->IsAttacking());
	TestFalse(TEXT("Natural completion clears failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestFalse(TEXT("Natural completion applies normal attack cooldown"), Combat->CanStartAttack());
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("Explicit external-play binding starts a transaction"), Combat->TryAttackTarget());
	const int64 BoundAction = Combat->AttackSequence;
	TestTrue(TEXT("External explicit Montage playback succeeds"), Anim->Montage_Play(Attack, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false) > 0.0f);
	FAnimMontageInstance* InterruptedPlayback = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Interrupted playback instance"), InterruptedPlayback)) return false;
	bool bObserverSawCleanState = false;
	InterruptedPlayback->OnMontageEnded.BindLambda([&](UAnimMontage*, bool bInterrupted)
	{
		bObserverSawCleanState = bInterrupted && !Combat->IsAttacking();
	});
	TestTrue(TEXT("Explicit binding installs the same owned completion handler"), Combat->BindAttackMontageForAttack(BoundAction, Mesh, Attack));
	TestTrue(TEXT("Duplicate explicit bind remains idempotent"), Combat->BindAttackMontageForAttack(BoundAction, Mesh, Attack));
	const float CooldownBeforeInterrupt = Combat->LastAttackTime;
	Anim->Montage_Stop(0.0f, Attack);
	InterruptedPlayback->UpdateWeight(0.1f);
	InterruptedPlayback->Advance(0.1f, nullptr, false);
	TestFalse(TEXT("Real Montage interruption cancels the attack"), Combat->IsAttacking());
	TestEqual(TEXT("Interruption does not consume normal-finish cooldown"), Combat->LastAttackTime, CooldownBeforeInterrupt);
	TestTrue(TEXT("An existing external observer runs only after owned cleanup"), bObserverSawCleanState);
	// 空资产是明确播放失败；不能让攻击事务和保护 Timer 占位到超时。
	TestTrue(TEXT("Failed-playback transaction starts"), Combat->TryAttackTarget());
	TestFalse(TEXT("Missing Montage reports playback failure"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, nullptr));
	TestFalse(TEXT("Playback failure releases the transaction immediately"), Combat->IsAttacking());
	TestFalse(TEXT("Playback failure clears the failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("Playback failure remains retryable"), Combat->CanStartAttack());
	TestTrue(TEXT("Slow attack starts"), Combat->TryAttackTarget());
	TestTrue(TEXT("A legal 0.1x attack plays"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, Attack, 0.1f));
	FAnimMontageInstance* SlowPlayback = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Slow engine playback instance"), SlowPlayback)) return false;
	TestTrue(TEXT("Failsafe covers actual 20-second playback rather than fixed five seconds"),
		World->GetTimerManager().GetTimerRate(Combat->AttackFinishTimerHandle) >= 20.0f);
	// 暂停夹具物理/自动动画，只推进真实世界 Timer；不能靠修改 Timer 状态假装跨过旧超时点。
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Mesh->SetComponentTickEnabled(false);
	if (!Fixture.TickTestWorld(0.0f) || !Fixture.TickTestWorld(5.5f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestTrue(TEXT("The slow attack survives beyond the previous 5.1-second timeout"), Combat->IsAttacking());
	for (int32 Step = 0; Step < 230 && Combat->IsAttacking(); ++Step)
	{
		SlowPlayback->UpdateWeight(0.1f);
		SlowPlayback->Advance(0.1f, nullptr, false);
	}
	TestFalse(TEXT("Slow playback still finishes through its engine completion"), Combat->IsAttacking());
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("External slow binding transaction starts"), Combat->TryAttackTarget());
	TestTrue(TEXT("External 0.1x play succeeds"), Anim->Montage_Play(Attack, 0.1f, EMontagePlayReturnType::MontageLength, 0.0f, true) > 0.0f);
	Anim->Montage_SetPosition(Attack, 0.5f);
	TestTrue(TEXT("External slow play binds"), Combat->BindAttackMontageForAttack(Combat->AttackSequence, Mesh, Attack));
	const float RemainingBudget = World->GetTimerManager().GetTimerRate(Combat->AttackFinishTimerHandle);
	TestTrue(TEXT("Explicit binding budgets remaining 15 seconds, not the whole 20 or fixed five"), RemainingBudget >= 15.0f && RemainingBudget < 16.0f);
	Combat->ResetCombat();
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyAttackAssetIntegrationTest,
	"fpstrue.Gameplay.Combat.ProjectBlueprintPlaybackIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyAttackAssetIntegrationTest::RunTest(const FString& Parameters)
{
	// 必需资产缺失必须失败；本测试不以原生空 Montage 或默认类替代真实项目蓝图。
	UClass* EnemyClass = LoadClass<AfpstrueEnemyCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP_C"));
	if (!TestNotNull(TEXT("Required project enemy Blueprint class loads"), EnemyClass)) return false;
	TArray<UAnimMontage*> ProjectAttacks;
	for (const TCHAR* Name : {TEXT("enenmyattack1"), TEXT("enemyattack2"), TEXT("enemyattack3"), TEXT("enemyattack4")})
	{
		const FString AssetPath = FString::Printf(TEXT("/Game/EnemyWarriorAnimPack/Animations/InPlace/Attacks/%s.%s"), Name, Name);
		UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *AssetPath);
		if (!TestNotNull(FString::Printf(TEXT("Required project attack loads: %s"), Name), Montage)) return false;
		ProjectAttacks.Add(Montage);
	}
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->CreateAISystem();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Test target"), Target)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(180.0, 0.0, 0.0));
	// 分别用新实例测试自然结束和中断，不改事务状态/播放 ID，也无需跳过冷却。
	for (const bool bInterrupt : {false, true})
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(EnemyClass,
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("Real project Blueprint spawns"), Enemy)) return false;
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(FTransform::Identity);
		Enemy->SetActorEnableCollision(false);
		AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
		if (!TestNotNull(TEXT("Controller"), Controller)) return false;
		Controller->Possess(Enemy);
		Controller->InitializeCombatContext(Target, nullptr);
		Controller->GetBrainComponent()->StopLogic(TEXT("Project attack Blueprint integration"));
		UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
		USkeletalMeshComponent* Mesh = Enemy->GetMesh();
		UAnimInstance* ProjectAnim = Mesh->GetAnimInstance();
		if (!TestNotNull(TEXT("Project SkeletalMesh retained"), Mesh->GetSkeletalMeshAsset()) ||
			!TestNotNull(TEXT("Project AnimBlueprint initialized"), ProjectAnim)) return false;
		TestTrue(TEXT("Animation is the configured Blueprint instance, not a synthetic UAnimInstance"),
			ProjectAnim->GetClass()->ClassGeneratedBy != nullptr && ProjectAnim->GetClass() == Mesh->GetAnimClass());
		const float InitialCooldown = Combat->LastAttackTime;
		// 唯一业务输入：由 enemy_BP 的新事件自己选择资产并调用 PlayAttackMontageForAttack。
		if (!TestTrue(TEXT("TryAttackTarget executes the real project Blueprint playback bridge"), Combat->TryAttackTarget())) return false;
		if (!TestTrue(TEXT("Blueprint bound an owned playback synchronously"), Combat->AttackPlayback.IsSet())) return false;
		UAnimMontage* SelectedMontage = Combat->AttackPlayback.Montage.Get();
		TestTrue(TEXT("Blueprint selected one of the four real attack Montages"), ProjectAttacks.Contains(SelectedMontage));
		TestTrue(TEXT("Owned playback uses the project mesh and its original AnimInstance"),
			Combat->AttackPlayback.Mesh.Get() == Mesh && Combat->AttackPlayback.AnimInstance.Get() == ProjectAnim);
		FAnimMontageInstance* Playback = ProjectAnim->GetMontageInstanceForID(Combat->AttackPlayback.MontageInstanceId);
		if (!TestNotNull(TEXT("The engine allocated the bound project Montage instance"), Playback)) return false;
		TestTrue(TEXT("Bound instance is actually playing the selected project asset"), Playback->IsPlaying() && Playback->Montage == SelectedMontage);
		if (bInterrupt) ProjectAnim->Montage_Stop(0.0f, SelectedMontage);
		const int32 MaxSteps = FMath::CeilToInt((SelectedMontage->GetPlayLength() + 2.0f) / 0.05f);
		for (int32 Step = 0; Step < MaxSteps && Combat->IsAttacking(); ++Step)
		{
			Playback->UpdateWeight(0.05f);
			Playback->Advance(0.05f, nullptr, false);
		}
		TestFalse(bInterrupt ? TEXT("Project Montage interruption clears attack") : TEXT("Project Montage naturally completes attack"), Combat->IsAttacking());
		TestFalse(TEXT("Project completion releases the failsafe timer without ticking that timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
		TestFalse(TEXT("Project completion releases playback ownership"), Combat->AttackPlayback.IsSet());
		if (bInterrupt) TestEqual(TEXT("Interruption preserves prior cooldown"), Combat->LastAttackTime, InitialCooldown);
		else TestFalse(TEXT("Natural completion applies configured attack cooldown"), Combat->CanStartAttack());
		Controller->StopAI();
		Enemy->Destroy();
		Controller->Destroy();
	}
	return true;
}
#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
