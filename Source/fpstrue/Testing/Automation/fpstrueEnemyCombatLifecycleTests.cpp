// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Enemies/fpstrueEnemyCombatConfig.h"
#include "Characters/Enemies/fpstrueSurroundManager.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Animation/AnimNotifyQueue.h"
#include "AnimationSharingManager.h"
#include "BrainComponent.h"
#include "Components/SkeletalMeshComponent.h"
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
	TestTrue(TEXT("A valid window enters Active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	const FVector HistoricalSample(11.0f, 22.0f, 33.0f);
	Combat->PreviousWeaponBase = HistoricalSample;
	AttackWindow->NotifyBegin(Mesh, nullptr, 1.0f, NotifyContext);
	TestTrue(TEXT("Duplicate Begin preserves the previous frame sample"), Combat->PreviousWeaponBase == HistoricalSample);
	AttackWindow->NotifyEnd(Mesh, nullptr, NotifyContext);
	TestTrue(TEXT("Closing a valid window enters Recovery"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	Combat->BeginAttackWindow();
	TestTrue(TEXT("Another window before a hit may become Active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
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
	Combat->HandleAttackFinishedNotify();
	TestTrue(TEXT("Normal completion returns to Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);
	TestFalse(TEXT("Normal completion uses the same timer cleanup"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestFalse(TEXT("Normal completion applies its cooldown"), Combat->CanStartAttack());
	// 不等待真实时间：仅移动测试事务的冷却基准，重新建立 EndPlay 所需的活动事务。
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("A cooled-down enemy takes a new permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("A cooled-down transaction restarts"), Combat->TryAttackTarget());
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

#endif // WITH_DEV_AUTOMATION_TESTS
