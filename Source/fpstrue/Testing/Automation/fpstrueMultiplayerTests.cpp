// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Game/fpstrueMultiplayer.h"
#include "Game/fpstrueGameMode.h"
#include "GameMapsSettings.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "AbilitySystemInterface.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "AIController.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "BrainComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/ProgressBar.h"
#include "Components/SphereComponent.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Testing/Automation/fpstrueTestObservers.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Engine/ReplicatedState.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "InputAction.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/CoreNet.h"
#if WITH_EDITOR
#include "UObject/GarbageCollection.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AnimGraphNode_ApplyMeshSpaceAdditive.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_RotateRootBone.h"
#include "AnimGraphNode_RotationOffsetBlendSpace.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_VariableGet.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#endif

// 进程内只固定 Actor 的角色与收到的反射快照，验证权限/提交边界；不替代多进程连接、丢包或 RPC 传输验收。
namespace
{
bool BeginMultiplayerUnitWorld(FTestWorldWrapper& Fixture, FAutomationTestBase& Test, bool bCreateAI = false, UClass* ModeClass = nullptr)
{
	if (Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.GetTestWorld()->GetWorldSettings()->DefaultGameMode = ModeClass != nullptr ? ModeClass : AGameModeBase::StaticClass();
		if (bCreateAI && !Test.TestNotNull(TEXT("Weapon fixture has its real AI controller system"), Fixture.GetTestWorld()->CreateAISystem())) return false;
		if (Fixture.BeginPlayInTestWorld()) return true;
	}
	Fixture.ForwardErrorMessages(&Test);
	return false;
}

FString DescribeEnemyAttackGate(AfpstrueEnemyCharacter& Enemy, AfpstrueEnemyAIController& Controller,
	const FFPEnemyTacticalAssignment& Assignment, float AcceptanceRadius)
{
	float AgentRadius = 0.0f, AgentHalfHeight = 0.0f;
	Enemy.GetSimpleCollisionCylinder(AgentRadius, AgentHalfHeight);
	const UPathFollowingComponent* Following = Controller.GetPathFollowingComponent();
	const UfpstrueEnemyCombatComponent* Combat = Enemy.GetCombatComponent();
	return FString::Printf(TEXT("AttackReady=%d PositionReady=%d FacingReady=%d InRange=%d Role=%d Revision=%llu HasInner=%d HasReached=%d AgentRadius=%.1f AgentHalfHeight=%.1f Acceptance=%.1f GoalDistance2D=%.1f Feet=%s Goal=%s"),
		Combat->IsAttackReady(), Controller.IsTacticalAttackPositionReady(false), Controller.IsTacticalAttackPositionReady(),
		Combat->IsTargetInAttackRange(), static_cast<int32>(Assignment.Role), Assignment.Revision,
		Controller.GetSurroundManager()->HasInnerSlot(&Enemy, Assignment.Revision),
		Following != nullptr && Following->HasReached(Assignment.Goal, EPathFollowingReachMode::OverlapAgent, AcceptanceRadius),
		AgentRadius, AgentHalfHeight, AcceptanceRadius, FVector::Dist2D(Enemy.GetNavAgentLocation(), Assignment.Goal),
		*Enemy.GetNavAgentLocation().ToCompactString(), *Assignment.Goal.ToCompactString());
}

UfpstrueHealthComponent* AddMultiplayerTestHealth(AActor& Owner)
{
	UfpstrueHealthComponent* Health = NewObject<UfpstrueHealthComponent>(&Owner);
	Owner.AddInstanceComponent(Health);
	Health->RegisterComponent();
	return Health;
}

// 新增网络规则用例共用原生装备前置条件；真实输入、能力、扣弹和伤害仍使用生产实现。
// 不加载项目动画和整张地图，也不把同进程调用说成已完成 RPC 传输或公网验收。
bool SpawnMultiplayerArmedCharacter(UWorld& World, FAutomationTestBase& Test,
	AfpstrueCharacter*& OutPlayer, AAIController*& OutController, UfpstrueWeaponComponent*& OutWeapon)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	OutPlayer = World.SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, SpawnParameters);
	OutController = World.SpawnActor<AAIController>();
	AActor* WeaponActor = World.SpawnActor<AActor>();
	if (!Test.TestNotNull(TEXT("Network rules fixture player"), OutPlayer) ||
		!Test.TestNotNull(TEXT("Network rules fixture controller"), OutController) ||
		!Test.TestNotNull(TEXT("Network rules fixture weapon actor"), WeaponActor)) return false;
	OutPlayer->SetActorEnableCollision(false);
	OutPlayer->GetCharacterMovement()->SetComponentTickEnabled(false);
	OutController->SetActorTickEnabled(false);
	OutController->Possess(OutPlayer);
	USkeletalMesh* AttachmentMesh = NewObject<USkeletalMesh>(OutPlayer, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(AttachmentMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("GripPoint")), TEXT("GripPoint"), INDEX_NONE), FTransform::Identity);
	}
	AttachmentMesh->CalculateInvRefMatrices();
	OutPlayer->GetMesh1P()->bEnableAnimation = false;
	OutPlayer->GetMesh1P()->SetComponentTickEnabled(false);
	OutPlayer->GetMesh1P()->SetVisibility(false);
	OutPlayer->GetMesh1P()->SetSkeletalMesh(AttachmentMesh);
	OutWeapon = NewObject<UfpstrueWeaponComponent>(WeaponActor, NAME_None, RF_Transient);
	WeaponActor->AddInstanceComponent(OutWeapon);
	WeaponActor->SetRootComponent(OutWeapon);
	OutWeapon->RegisterComponent();
	return Test.TestTrue(TEXT("Network rules fixture uses the complete public equipment transaction"), OutWeapon->AttachWeapon(OutPlayer));
}

// 会话规则使用真实项目 Controller/PlayerState；仅以临时输入资产满足本地 Possess 的配置前置。
bool SpawnMultiplayerSessionPlayer(UWorld& World, FAutomationTestBase& Test,
	AfpstruePvpPlayerController*& OutController, AfpstrueCharacter*& OutPawn, UfpstrueWeaponComponent*& OutWeapon)
{
	AAIController* EquipmentController = nullptr;
	if (!SpawnMultiplayerArmedCharacter(World, Test, OutPawn, EquipmentController, OutWeapon)) return false;
	for (const TCHAR* Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(OutPawn->GetClass(), Name);
		if (!Test.TestNotNull(TEXT("Session fixture player action property"), Property)) return false;
		Property->SetObjectPropertyValue_InContainer(OutPawn, NewObject<UInputAction>(OutPawn));
	}
	EquipmentController->UnPossess();
	EquipmentController->Destroy();
	OutController = World.SpawnActor<AfpstruePvpPlayerController>();
	if (!Test.TestNotNull(TEXT("Session fixture project controller"), OutController)) return false;
	OutController->Possess(OutPawn);
	return Test.TestNotNull(TEXT("Session fixture project PlayerState"), OutController->GetPlayerState<AfpstruePvpPlayerState>());
}

// 最小参考骨骼提供头部球、旋转躯干盒、手臂胶囊，隔离美术资产与动画姿态，不替换生产历史采样和射线算法。
AfpstrueCharacter* SpawnMultiplayerHistoryTarget(UWorld& World, FAutomationTestBase& Test, const FVector& Location)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* Target = World.SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(),
		FTransform(FRotator::ZeroRotator, Location), SpawnParameters);
	if (!Test.TestNotNull(TEXT("Historical query target"), Target)) return nullptr;
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	USkeletalMeshComponent* Body = Target->GetMesh();
	Body->bEnableAnimation = true;
	Body->bUseRefPoseOnInitAnim = true;
	Body->SetComponentTickEnabled(false);
	Body->SetVisibility(false);
	Body->SetRelativeTransform(FTransform::Identity);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Target, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("spine_03"), TEXT("spine_03"), 0), FTransform(FVector(0.0f, 0.0f, 30.0f)));
		Modifier.Add(FMeshBoneInfo(TEXT("head"), TEXT("head"), 1), FTransform(FVector(0.0f, 0.0f, 60.0f)));
		Modifier.Add(FMeshBoneInfo(TEXT("upperarm_l"), TEXT("upperarm_l"), 0), FTransform(FVector(0.0f, 80.0f, 40.0f)));
	}
	Mesh->CalculateInvRefMatrices();
	// SkeletalMesh 的参考骨骼并不会自动成为组件姿态：InitAnim 需要 Skeleton 和有效 LOD 的 RequiredBones。
	// 这里用最小空渲染 LOD 初始化真实参考姿态，不手写 ComponentSpaceTransforms，也不改生产采样算法。
	USkeleton* Skeleton = NewObject<USkeleton>(Mesh, NAME_None, RF_Transient);
	Mesh->SetSkeleton(Skeleton);
	if (!Test.TestTrue(TEXT("Historical fixture merges its complete reference skeleton"), Skeleton->MergeAllBonesToBoneTree(Mesh, false))) return nullptr;
	Mesh->AllocateResourceForRendering();
	TRefCountPtr<FSkeletalMeshLODRenderData> LOD = MakeRefCount<FSkeletalMeshLODRenderData>();
	LOD->RequiredBones = {0, 1, 2, 3};
	LOD->ActiveBoneIndices = LOD->RequiredBones;
	Mesh->GetResourceForRendering()->LODRenderData.Add(LOD);
	Mesh->AddLODInfo();
	UPhysicsAsset* Physics = NewObject<UPhysicsAsset>(Mesh, NAME_None, RF_Transient);
	USkeletalBodySetup* Torso = NewObject<USkeletalBodySetup>(Physics);
	Torso->BoneName = TEXT("spine_03");
	FKBoxElem& TorsoBox = Torso->AggGeom.BoxElems.AddDefaulted_GetRef();
	TorsoBox.X = 50.0f;
	TorsoBox.Y = 40.0f;
	TorsoBox.Z = 50.0f;
	TorsoBox.Rotation = FRotator(0.0f, 30.0f, 0.0f);
	Physics->SkeletalBodySetups.Add(Torso);
	USkeletalBodySetup* Head = NewObject<USkeletalBodySetup>(Physics);
	Head->BoneName = TEXT("head");
	Head->AggGeom.SphereElems.AddDefaulted_GetRef().Radius = 15.0f;
	Physics->SkeletalBodySetups.Add(Head);
	USkeletalBodySetup* Arm = NewObject<USkeletalBodySetup>(Physics);
	Arm->BoneName = TEXT("upperarm_l");
	FKSphylElem& ArmCapsule = Arm->AggGeom.SphylElems.AddDefaulted_GetRef();
	ArmCapsule.Radius = 10.0f;
	ArmCapsule.Length = 40.0f;
	Physics->SkeletalBodySetups.Add(Arm);
	Physics->UpdateBodySetupIndexMap();
	Mesh->SetPhysicsAsset(Physics);
	Body->SetSkeletalMesh(Mesh);
	Body->InitAnim(true);
	Body->bEnableAnimation = false;
	if (!Test.TestTrue(TEXT("Historical fixture publishes the real head component-space reference pose"),
		Body->GetBoneTransform(Body->GetBoneIndex(TEXT("head"))).GetLocation().Equals(Location + FVector(0.0f, 0.0f, 90.0f), 0.001f)) ||
		!Test.TestTrue(TEXT("Historical fixture publishes the real torso component-space reference pose"),
			Body->GetBoneTransform(Body->GetBoneIndex(TEXT("spine_03"))).GetLocation().Equals(Location + FVector(0.0f, 0.0f, 30.0f), 0.001f)) ||
		!Test.TestTrue(TEXT("Historical fixture publishes the real limb component-space reference pose"),
			Body->GetBoneTransform(Body->GetBoneIndex(TEXT("upperarm_l"))).GetLocation().Equals(Location + FVector(0.0f, 80.0f, 40.0f), 0.001f))) return nullptr;
	return Target;
}

FFPReplicatedHealthSnapshot* ReadMultiplayerHealthSnapshot(UfpstrueHealthComponent& Health, FAutomationTestBase& Test)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(Health.GetClass(), TEXT("HealthSnapshot"));
	if (!Test.TestNotNull(TEXT("Health retains its reflected compatibility snapshot"), Property)) return nullptr;
	if (!Test.TestTrue(TEXT("Health snapshot has the declared struct type"), Property->Struct == FFPReplicatedHealthSnapshot::StaticStruct())) return nullptr;
	Test.TestFalse(TEXT("Compatibility health snapshot is not a second replicated health store"), Property->HasAnyPropertyFlags(CPF_Net));
	return Property->ContainerPtrToValuePtr<FFPReplicatedHealthSnapshot>(&Health);
}

// 回放属性复制实际调用的两个 RepNotify 与批次结束入口；不写旧镜像、不伪装网络传输已被测过。
bool ReceiveMultiplayerHealthAttributes(UfpstrueHealthComponent& Health, FAutomationTestBase& Test, float CurrentHealth, float MaxHealth)
{
	UfpstrueHealthAttributeSet* Attributes = const_cast<UfpstrueHealthAttributeSet*>(Health.GetHealthAttributeSet());
	// 客户端不自行生成权威属性集；这里显式模拟 ASC 已经接收到服务器的属性子对象。
	if (Attributes == nullptr)
	{
		Attributes = NewObject<UfpstrueHealthAttributeSet>(Health.GetOwner());
		Health.AddAttributeSetSubobject(Attributes);
	}
	if (!Test.TestNotNull(TEXT("Health owns the production GAS attribute set"), Attributes)) return false;
	const FStructProperty* HealthProperty = FindFProperty<FStructProperty>(Attributes->GetClass(), TEXT("Health"));
	const FStructProperty* MaximumProperty = FindFProperty<FStructProperty>(Attributes->GetClass(), TEXT("MaxHealth"));
	UFunction* HealthRepNotify = Attributes->FindFunction(TEXT("OnRep_Health"));
	UFunction* MaximumRepNotify = Attributes->FindFunction(TEXT("OnRep_MaxHealth"));
	if (!Test.TestNotNull(TEXT("GAS health property"), HealthProperty) || !Test.TestNotNull(TEXT("GAS maximum property"), MaximumProperty) ||
		!Test.TestNotNull(TEXT("GAS health RepNotify"), HealthRepNotify) || !Test.TestNotNull(TEXT("GAS maximum RepNotify"), MaximumRepNotify)) return false;
	if (!Test.TestTrue(TEXT("Health is replicated as FGameplayAttributeData"), HealthProperty->HasAnyPropertyFlags(CPF_Net) &&
		HealthProperty->Struct == FGameplayAttributeData::StaticStruct()) ||
		!Test.TestTrue(TEXT("Maximum health is replicated as FGameplayAttributeData"), MaximumProperty->HasAnyPropertyFlags(CPF_Net) &&
		MaximumProperty->Struct == FGameplayAttributeData::StaticStruct())) return false;
	FGameplayAttributeData* HealthData = HealthProperty->ContainerPtrToValuePtr<FGameplayAttributeData>(Attributes);
	FGameplayAttributeData* MaximumData = MaximumProperty->ContainerPtrToValuePtr<FGameplayAttributeData>(Attributes);
	struct FAttributeNotifyParameters { FGameplayAttributeData Previous; };
	FAttributeNotifyParameters PreviousHealth{*HealthData};
	FAttributeNotifyParameters PreviousMaximum{*MaximumData};
	HealthData->SetBaseValue(CurrentHealth);
	HealthData->SetCurrentValue(CurrentHealth);
	MaximumData->SetBaseValue(MaxHealth);
	MaximumData->SetCurrentValue(MaxHealth);
	Attributes->ProcessEvent(MaximumRepNotify, &PreviousMaximum);
	Attributes->ProcessEvent(HealthRepNotify, &PreviousHealth);
	Attributes->PostRepNotifies();
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerHealthAuthorityTest,
	"fpstrue.Multiplayer.Health.AuthorityAndGASAttributeCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 同一真实伤害链先验证 Standalone 权威写入，再切换角色验证客户端不能借旧伤害绑定或公开重置入口写血量。
bool FFpstrueMultiplayerHealthAuthorityTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	AActor* Owner = Fixture.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Health authority owner"), Owner)) return false;
	UfpstrueHealthComponent* Health = AddMultiplayerTestHealth(*Owner);
	FFPReplicatedHealthSnapshot* Snapshot = ReadMultiplayerHealthSnapshot(*Health, *this);
	if (Snapshot == nullptr) return false;
	TestTrue(TEXT("Standalone owner retains authority"), Owner->HasAuthority());
	TestTrue(TEXT("Health component replicates by default"), Health->GetIsReplicated());
	const UfpstrueHealthAttributeSet* Attributes = Health->GetHealthAttributeSet();
	if (!TestNotNull(TEXT("Authoritative health is backed by GAS attributes"), Attributes)) return false;
	TestTrue(TEXT("Default initial health still produces a versioned snapshot"), Snapshot->Revision > 0);
	Health->SetMaxHealthAndReset(200.0f);
	TestEqual(TEXT("Authoritative maximum reset remains operational"), Health->GetHealth(), 200.0f);
	TestEqual(TEXT("Maximum reset is committed to the actual GAS attribute"), Attributes->GetMaxHealth(), 200.0f);
	TestEqual(TEXT("Health reset is committed to the actual GAS attribute"), Attributes->GetHealth(), 200.0f);
	TestEqual(TEXT("Maximum is mirrored for existing local consumers"), Snapshot->MaxHealth, 200.0f);
	int32 ResolvedDamageEvents = 0;
	const FDelegateHandle Observer = Health->OnDamageResolved.AddLambda(
		[&](float AppliedDamage, AActor*, AController*, const FFPDamageContext&)
		{
			++ResolvedDamageEvents;
			TestEqual(TEXT("Damage observer sees the already committed health snapshot"), Snapshot->CurrentHealth, Health->GetHealth());
			TestEqual(TEXT("Damage observer sees the already committed maximum snapshot"), Snapshot->MaxHealth, Health->GetMaxHealth());
			TestEqual(TEXT("Real standalone damage resolves once"), AppliedDamage, 25.0f);
		});
	UGameplayStatics::ApplyDamage(Owner, 25.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("Standalone damage still follows the production damage chain"), Health->GetHealth(), 175.0f);
	TestEqual(TEXT("Production damage modifies the actual GAS health attribute"), Attributes->GetHealth(), 175.0f);
	TestEqual(TEXT("Authoritative damage broadcasts its committed fact once"), ResolvedDamageEvents, 1);
	const uint32 AuthorityRevision = Snapshot->Revision;
	Owner->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("The owner now has a genuine non-authority role"), Owner->HasAuthority());
	UGameplayStatics::ApplyDamage(Owner, 10.0f, nullptr, nullptr, nullptr);
	Health->ResetHealth();
	Health->SetMaxHealthAndReset(50.0f);
	TestEqual(TEXT("A non-authority owner cannot apply damage or reset health"), Health->GetHealth(), 175.0f);
	TestEqual(TEXT("A non-authority owner cannot change maximum health"), Health->GetMaxHealth(), 200.0f);
	TestEqual(TEXT("Rejected client writes leave the actual GAS health unchanged"), Attributes->GetHealth(), 175.0f);
	TestEqual(TEXT("Rejected client writes leave the actual GAS maximum unchanged"), Attributes->GetMaxHealth(), 200.0f);
	TestEqual(TEXT("Rejected mutations cannot advance replicated health state"), Snapshot->Revision, AuthorityRevision);
	TestEqual(TEXT("Rejected client damage does not publish a damage fact"), ResolvedDamageEvents, 1);
	Health->OnDamageResolved.Remove(Observer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerHealthSnapshotTest,
	"fpstrue.Multiplayer.Health.ReplicatedAttributeNotifications",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 只回放真实 RepNotify 入口；客户端收到初始/死亡/回满快照时通知本地消费者，不伪造伤害来源。
// GAS 版本的“快照”由属性复制批次提供；旧结构体不再独立复制或充当客户端写入入口。
bool FFpstrueMultiplayerHealthSnapshotTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	AActor* Owner = Fixture.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Snapshot receiver owner"), Owner)) return false;
	Owner->SetRole(ROLE_SimulatedProxy);
	UfpstrueHealthComponent* Health = AddMultiplayerTestHealth(*Owner);
	if (ReadMultiplayerHealthSnapshot(*Health, *this) == nullptr) return false;
	TStrongObjectPtr<UfpstrueHealthMutationTestObserver> Observer(NewObject<UfpstrueHealthMutationTestObserver>());
	Observer->Health = Health;
	Observer->bAlwaysResetDuringHealthChange = true;
	Health->OnHealthChanged.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	Health->OnDeath.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);
	int32 DamageEvents = 0;
	const FDelegateHandle DamageObserver = Health->OnDamageResolved.AddLambda(
		[&](float, AActor*, AController*, const FFPDamageContext&) { ++DamageEvents; });
	TestFalse(TEXT("Default client data does not start dead"), Health->IsDead());
	if (!ReceiveMultiplayerHealthAttributes(*Health, *this, 40.0f, 200.0f)) return false;
	TestEqual(TEXT("Initial snapshot updates current health"), Health->GetHealth(), 40.0f);
	TestEqual(TEXT("Initial snapshot updates its maximum in the same callback"), Health->GetMaxHealth(), 200.0f);
	TestTrue(TEXT("Initial health notification carries the committed received value"), Observer->HealthSnapshots == TArray<float>({40.0f}));
	TestEqual(TEXT("A living initial snapshot does not broadcast death"), Observer->Deaths, 0);
	// 借现有反射观察者在 HealthChanged 内触发独立权威探针，直接检查此刻数值已提交而 Death 尚未通知。
	AActor* NotificationProbeOwner = Fixture.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Health notification order probe"), NotificationProbeOwner)) return false;
	UfpstrueHealthComponent* NotificationProbe = AddMultiplayerTestHealth(*NotificationProbeOwner);
	int32 HealthOrderProbes = 0;
	const FDelegateHandle OrderObserver = NotificationProbe->OnDamageResolved.AddLambda(
		[&](float, AActor*, AController*, const FFPDamageContext&)
		{
			++HealthOrderProbes;
			TestEqual(TEXT("HealthChanged runs after the received health is committed"), Health->GetHealth(), 0.0f);
			TestEqual(TEXT("HealthChanged runs after the received maximum is committed"), Health->GetMaxHealth(), 250.0f);
			TestEqual(TEXT("HealthChanged is published before the first local death event"), Observer->Deaths, 0);
		});
	Observer->Health = NotificationProbe;
	Observer->bDamageDuringHealthChange = true;
	if (!ReceiveMultiplayerHealthAttributes(*Health, *this, 0.0f, 250.0f)) return false;
	NotificationProbe->OnDamageResolved.Remove(OrderObserver);
	Observer->Health = Health;
	TestEqual(TEXT("The real HealthChanged callback executed the ordering probe once"), HealthOrderProbes, 1);
	TestTrue(TEXT("Received zero health is the local death fact"), Health->IsDead());
	TestEqual(TEXT("Death snapshot updates maximum before consumers query it"), Health->GetMaxHealth(), 250.0f);
	TestTrue(TEXT("Death also publishes the zero-health snapshot"), Observer->HealthSnapshots == TArray<float>({40.0f, 0.0f}));
	TestEqual(TEXT("First received death broadcasts once"), Observer->Deaths, 1);
	if (!ReceiveMultiplayerHealthAttributes(*Health, *this, 0.0f, 250.0f)) return false;
	TestEqual(TEXT("Repeated dead snapshots cannot repeat the death edge"), Observer->Deaths, 1);
	if (!ReceiveMultiplayerHealthAttributes(*Health, *this, 250.0f, 250.0f)) return false;
	TestFalse(TEXT("An authoritative reset snapshot restores life data"), Health->IsDead());
	TestEqual(TEXT("A reset does not invent another death edge"), Observer->Deaths, 1);
	if (!ReceiveMultiplayerHealthAttributes(*Health, *this, 0.0f, 250.0f)) return false;
	TestEqual(TEXT("A later life session can notify one new death"), Observer->Deaths, 2);
	TestEqual(TEXT("RepNotify never manufactures server damage-context events"), DamageEvents, 0);
	Health->OnDamageResolved.Remove(DamageObserver);
	Health->OnHealthChanged.RemoveDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	Health->OnDeath.RemoveDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);
	// 后加入的客户端可能收到的第一份状态已经死亡，不能要求先收到一次伤害或存活快照。
	AActor* LateOwner = Fixture.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Late client snapshot owner"), LateOwner)) return false;
	LateOwner->SetRole(ROLE_SimulatedProxy);
	UfpstrueHealthComponent* LateHealth = AddMultiplayerTestHealth(*LateOwner);
	if (ReadMultiplayerHealthSnapshot(*LateHealth, *this) == nullptr) return false;
	TStrongObjectPtr<UfpstrueHealthMutationTestObserver> LateObserver(NewObject<UfpstrueHealthMutationTestObserver>());
	LateObserver->Health = LateHealth;
	LateHealth->OnHealthChanged.AddDynamic(LateObserver.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	LateHealth->OnDeath.AddDynamic(LateObserver.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);
	if (!ReceiveMultiplayerHealthAttributes(*LateHealth, *this, 0.0f, 175.0f)) return false;
	TestTrue(TEXT("An initially dead replicated snapshot publishes its zero-health value"), LateObserver->HealthSnapshots == TArray<float>({0.0f}));
	TestEqual(TEXT("An initially dead snapshot publishes one local death edge"), LateObserver->Deaths, 1);
	if (!ReceiveMultiplayerHealthAttributes(*LateHealth, *this, 0.0f, 175.0f)) return false;
	TestEqual(TEXT("Repeating an initial dead snapshot cannot repeat death"), LateObserver->Deaths, 1);
	LateHealth->OnHealthChanged.RemoveDynamic(LateObserver.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	LateHealth->OnDeath.RemoveDynamic(LateObserver.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerWeaponAuthorityTest,
	"fpstrue.Multiplayer.Weapon.AuthorityAndReplicatedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 权威对照经真实射线扣血；同一装备切换为客户端角色并收到快照后，公开动作不能再次扣弹、装弹或结算命中。
bool FFpstrueMultiplayerWeaponAuthorityTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* Player = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, SpawnParameters);
	AAIController* Controller = World->SpawnActor<AAIController>();
	AActor* Pickup = World->SpawnActor<AActor>();
	AActor* Target = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Weapon holder"), Player) || !TestNotNull(TEXT("Weapon controller"), Controller) ||
		!TestNotNull(TEXT("Weapon actor"), Pickup) || !TestNotNull(TEXT("Damage target"), Target)) return false;
	Player->SetActorEnableCollision(false);
	Player->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->SetActorTickEnabled(false);
	Controller->Possess(Player);
	// 沿用已有业务夹具的单骨骼挂接前提，不加载项目动画，也不替代装备/射击结果。
	USkeletalMesh* AttachmentMesh = NewObject<USkeletalMesh>(Player, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(AttachmentMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("GripPoint")), TEXT("GripPoint"), INDEX_NONE), FTransform::Identity);
	}
	AttachmentMesh->CalculateInvRefMatrices();
	Player->GetMesh1P()->bEnableAnimation = false;
	Player->GetMesh1P()->SetComponentTickEnabled(false);
	Player->GetMesh1P()->SetVisibility(false);
	Player->GetMesh1P()->SetSkeletalMesh(AttachmentMesh);
	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(Pickup, NAME_None, RF_Transient);
	Pickup->AddInstanceComponent(Weapon);
	Pickup->SetRootComponent(Weapon);
	Weapon->RegisterComponent();
	if (!TestTrue(TEXT("The authority equips the weapon through its public API"), Weapon->AttachWeapon(Player))) return false;
	TestTrue(TEXT("Native equipment getter resolves the actual authority holder"), Weapon->GetOwningCharacter() == Player);
	const UFunction* HolderGetter = Weapon->FindFunction(TEXT("GetOwningCharacter"));
	if (!TestNotNull(TEXT("Weapon holder is exposed to Blueprint without a pickup-event cache"), HolderGetter)) return false;
	TestTrue(TEXT("Blueprint reads the native holder through a pure getter"), HolderGetter->HasAllFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure));
	TestTrue(TEXT("Weapon component replicates by default"), Weapon->GetIsReplicated());
	IAbilitySystemInterface* AbilityOwner = Cast<IAbilitySystemInterface>(Player);
	if (!TestNotNull(TEXT("Player exposes its ASC through the engine ability-system interface"), AbilityOwner)) return false;
	UAbilitySystemComponent* AbilitySystem = AbilityOwner->GetAbilitySystemComponent();
	if (!TestTrue(TEXT("The player reuses its health component as its single ASC"), AbilitySystem == Player->GetHealthComponent())) return false;
	FGameplayAbilitySpec* FireSpec = AbilitySystem->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
	FGameplayAbilitySpec* ReloadSpec = AbilitySystem->FindAbilitySpecFromClass(UfpstrueReloadAbility::StaticClass());
	FGameplayAbilitySpec* AimSpec = AbilitySystem->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass());
	FGameplayAbilitySpec* SprintSpec = AbilitySystem->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass());
	if (!TestNotNull(TEXT("Authority grants the real fire ability"), FireSpec) ||
		!TestNotNull(TEXT("Authority grants the real reload ability"), ReloadSpec) ||
		!TestNotNull(TEXT("Authority grants the real aim ability"), AimSpec) ||
		!TestNotNull(TEXT("Authority grants the real sprint ability"), SprintSpec)) return false;
	const int32 FireInput = static_cast<int32>(EFPCombatAbilityInput::Fire);
	const int32 ReloadInput = static_cast<int32>(EFPCombatAbilityInput::Reload);
	TestEqual(TEXT("Fire spec is bound to the configured GAS input"), FireSpec->InputID, FireInput);
	TestEqual(TEXT("Reload spec is bound to the configured GAS input"), ReloadSpec->InputID, ReloadInput);
	TestEqual(TEXT("Aim spec is bound to the configured GAS input"), AimSpec->InputID, static_cast<int32>(EFPCombatAbilityInput::Aim));
	TestEqual(TEXT("Sprint spec is bound to the configured GAS input"), SprintSpec->InputID, static_cast<int32>(EFPCombatAbilityInput::Sprint));
	TestEqual(TEXT("Player grants exactly the four combat and movement abilities"), AbilitySystem->GetActivatableAbilities().Num(), 4);
	TestTrue(TEXT("Reload starts on the server without an owner round trip"),
		ReloadSpec->Ability->GetNetExecutionPolicy() == EGameplayAbilityNetExecutionPolicy::ServerInitiated);
	TestNull(TEXT("Fire does not retain a competing custom server RPC"), Player->FindFunction(TEXT("ServerSetFirePressed")));
	TestNull(TEXT("Reload does not retain a competing custom server RPC"), Player->FindFunction(TEXT("ServerRequestReload")));
	TestNull(TEXT("Aim and sprint do not retain competing movement-state RPCs"), Player->FindFunction(TEXT("ServerSetMovementModifiers")));
	TestNull(TEXT("Weapon has no reflected independently writable action state"), FindFProperty<FProperty>(Weapon->GetClass(), TEXT("ActionState")));
	UBoxComponent* TargetBox = NewObject<UBoxComponent>(Target);
	Target->AddInstanceComponent(TargetBox);
	Target->SetRootComponent(TargetBox);
	TargetBox->SetBoxExtent(FVector(40.0f));
	TargetBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TargetBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	TargetBox->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	TargetBox->RegisterComponent();
	const UCameraComponent* Camera = Player->GetFirstPersonCameraComponent();
	Target->SetActorLocation(Camera->GetComponentLocation() + Camera->GetForwardVector() * 500.0f);
	UfpstrueHealthComponent* TargetHealth = AddMultiplayerTestHealth(*Target);
	const int32 AmmoBeforeShot = Weapon->GetCurrentAmmo();
	Controller->SetControlRotation(Camera->GetComponentRotation());
	AbilitySystem->AbilityLocalInputPressed(FireInput);
	TestTrue(TEXT("GAS fire activation stays active while the automatic trigger is held"), FireSpec->IsActive());
	TestTrue(TEXT("Active fire owns the GAS firing tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
	AbilitySystem->AbilityLocalInputReleased(FireInput);
	TestFalse(TEXT("GAS input release ends the real fire ability"), FireSpec->IsActive());
	TestFalse(TEXT("Fire release removes the firing tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
	TestEqual(TEXT("Standalone authority still consumes one round"), Weapon->GetCurrentAmmo(), AmmoBeforeShot - 1);
	TestEqual(TEXT("Standalone authority still resolves the actual hitscan damage"), TargetHealth->GetHealth(), 60.0f);
	if (!Fixture.TickTestWorld(0.2f)) { Fixture.ForwardErrorMessages(this); return false; }
	// 经真实能力启动换弹，再尝试开火和主动取消；能力标签与原武器事务必须一起收尾。
	AbilitySystem->AbilityLocalInputPressed(ReloadInput);
	AbilitySystem->AbilityLocalInputReleased(ReloadInput);
	TestTrue(TEXT("GAS reload activation holds the actual reload transaction"), ReloadSpec->IsActive() && Weapon->IsReloading());
	TestTrue(TEXT("Active reload owns the GAS reloading tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading));
	AbilitySystem->AbilityLocalInputPressed(FireInput);
	AbilitySystem->AbilityLocalInputReleased(FireInput);
	TestFalse(TEXT("Reload blocks a conflicting fire ability"), FireSpec->IsActive());
	TestEqual(TEXT("Blocked fire cannot spend a round during reload"), Weapon->GetCurrentAmmo(), AmmoBeforeShot - 1);
	AbilitySystem->CancelAbilityHandle(ReloadSpec->Handle);
	TestFalse(TEXT("GAS reload cancellation clears the real reload transaction"), ReloadSpec->IsActive() || Weapon->IsReloading());
	TestFalse(TEXT("Reload cancellation removes the reload tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading));
	TestEqual(TEXT("Cancel before reload commit does not manufacture ammo"), Weapon->GetCurrentAmmo(), AmmoBeforeShot - 1);
	// 动作互斥只经真实能力标签验证，不反射写入角色 bool 或武器动作枚举来制造前态。
	const int32 AimInput = static_cast<int32>(EFPCombatAbilityInput::Aim);
	const int32 SprintInput = static_cast<int32>(EFPCombatAbilityInput::Sprint);
	AbilitySystem->AbilityLocalInputPressed(SprintInput);
	AbilitySystem->AbilityLocalInputReleased(SprintInput);
	TestTrue(TEXT("Sprint remains a toggle ability after the input pulse"), SprintSpec->IsActive());
	TestTrue(TEXT("Sprint owns the GAS movement tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	AbilitySystem->AbilityLocalInputPressed(AimInput);
	TestTrue(TEXT("Aim activation replaces sprint through the GAS cancellation rule"), AimSpec->IsActive() && !SprintSpec->IsActive());
	TestTrue(TEXT("Aim presentation follows its active GAS tag"), Player->IsAiming() && AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming));
	TestFalse(TEXT("Aim cancellation removes the old sprint tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	AbilitySystem->AbilityLocalInputPressed(SprintInput);
	AbilitySystem->AbilityLocalInputReleased(SprintInput);
	TestFalse(TEXT("Aim blocks a conflicting sprint activation"), SprintSpec->IsActive());
	AbilitySystem->AbilityLocalInputPressed(ReloadInput);
	AbilitySystem->AbilityLocalInputReleased(ReloadInput);
	TestTrue(TEXT("Reload activates while cancelling aim"), ReloadSpec->IsActive() && !AimSpec->IsActive());
	TestFalse(TEXT("Reload cancellation clears aim presentation"), Player->IsAiming());
	TestFalse(TEXT("Reload blocks aim activation"), AbilitySystem->TryActivateAbilityByClass(UfpstrueAimAbility::StaticClass()));
	TestFalse(TEXT("Reload blocks sprint activation"), AbilitySystem->TryActivateAbilityByClass(UfpstrueSprintAbility::StaticClass()));
	AbilitySystem->AbilityLocalInputReleased(AimInput);
	AbilitySystem->CancelAbilityHandle(ReloadSpec->Handle);
	TestFalse(TEXT("Cancelling reload leaves no movement or reload tags"),
		AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading) ||
		AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming) ||
		AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	Pickup->SetRole(ROLE_SimulatedProxy);
	Player->SetRole(ROLE_SimulatedProxy);
	const FStructProperty* StateProperty = FindFProperty<FStructProperty>(Weapon->GetClass(), TEXT("NetworkState"));
	UFunction* RepNotify = Weapon->FindFunction(TEXT("OnRep_NetworkState"));
	if (!TestNotNull(TEXT("Weapon has a reflected network state"), StateProperty) ||
		!TestNotNull(TEXT("Weapon network state has a real RepNotify"), RepNotify)) return false;
	if (!TestTrue(TEXT("Weapon snapshot has the declared complete state type"), StateProperty->Struct == FFPReplicatedWeaponState::StaticStruct())) return false;
	TestTrue(TEXT("Weapon state participates in property replication"), StateProperty->HasAnyPropertyFlags(CPF_Net));
	FFPReplicatedWeaponState* State = StateProperty->ContainerPtrToValuePtr<FFPReplicatedWeaponState>(Weapon);
	State->Holder = Player;
	State->Action = EFPWeaponActionState::Ready;
	State->Ammo = 7;
	State->Reserve = 32;
	State->Revision = 10;
	Weapon->ProcessEvent(RepNotify, nullptr);
	TestTrue(TEXT("Received equipment snapshot resolves the same native holder getter"), Weapon->GetOwningCharacter() == Player);
	TestEqual(TEXT("Client snapshot sets the received magazine"), Weapon->GetCurrentAmmo(), 7);
	TestEqual(TEXT("Client snapshot sets the received reserve"), Weapon->GetReserveAmmo(), 32);
	TestFalse(TEXT("A client cannot authorize another equipment attachment"), Weapon->AttachWeapon(Player));
	Weapon->StartFire();
	Weapon->StopFire();
	TestEqual(TEXT("Client fire cannot spend an authoritative round"), Weapon->GetCurrentAmmo(), 7);
	TestEqual(TEXT("Client fire cannot execute hitscan damage"), TargetHealth->GetHealth(), 60.0f);
	TestFalse(TEXT("Client cannot start an authoritative reload from an otherwise ready state"), Weapon->RequestReload());
	// 客户端只预测表现，不写弹药或伤害；即使服务器动作快照还在 Ready，能力结束/禁用也须立即清理预测计时。
	// GAS 重写后预测调度归 Fire AbilityTask；本夹具不伪造远端授予或预测键，真实预测与任务退出由双客户端 smoke 验证。
	TStrongObjectPtr<UfpstrueReloadReentryObserver> PresentationObserver(NewObject<UfpstrueReloadReentryObserver>());
	Weapon->OnWeaponFirePerformed.AddDynamic(PresentationObserver.Get(), &UfpstrueReloadReentryObserver::HandleFirePerformed);
	Weapon->StartFire();
	Weapon->StopFire();
	if (!Fixture.TickTestWorld(0.25f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestEqual(TEXT("Client weapon compatibility APIs cannot bypass GAS to predict firing"), PresentationObserver->FireEventCount, 0);
	TestEqual(TEXT("Rejected direct client commands do not subtract authoritative ammo"), Weapon->GetCurrentAmmo(), 7);
	TestEqual(TEXT("Rejected direct client commands do not execute another hitscan"), TargetHealth->GetHealth(), 60.0f);
	// 迟到快照只剩一发时，预测达到本地上限只能停表现，不能替服务器结束连射或打断空仓换弹。
	State->Ammo = 1;
	Weapon->ProcessEvent(RepNotify, nullptr);
	Weapon->StartFire();
	Weapon->StopFire();
	const float PredictionWait = FMath::Max(0.25f, 120.0f / Weapon->GetWeaponSettings().RoundsPerMinute);
	if (!Fixture.TickTestWorld(PredictionWait)) { Fixture.ForwardErrorMessages(this); return false; }
	TestEqual(TEXT("A late one-round estimate cannot authorize direct client firing"), PresentationObserver->FireEventCount, 0);
	TestEqual(TEXT("A client estimate remains read-only authoritative ammo"), Weapon->GetCurrentAmmo(), 1);
	TestFalse(TEXT("Direct client commands cannot create or end the fire ability"), FireSpec->IsActive());
	Weapon->OnWeaponFirePerformed.RemoveDynamic(PresentationObserver.Get(), &UfpstrueReloadReentryObserver::HandleFirePerformed);
	State->Ammo = 7;
	Weapon->ProcessEvent(RepNotify, nullptr);
	State->Action = EFPWeaponActionState::Reloading;
	State->ReloadId = 77;
	State->bCommitted = false;
	State->Revision = 11;
	Weapon->ProcessEvent(RepNotify, nullptr);
	TestEqual(TEXT("Client receives the server's current reload identity"), Weapon->GetActiveReloadId(), 77);
	TestFalse(TEXT("Even a matching client animation identity cannot commit server ammo"), Weapon->CommitReloadForTransaction(77));
	TestEqual(TEXT("Rejected client reload leaves magazine unchanged"), Weapon->GetCurrentAmmo(), 7);
	TestEqual(TEXT("Rejected client reload leaves reserve unchanged"), Weapon->GetReserveAmmo(), 32);
	TestEqual(TEXT("Rejected client commands cannot publish a new network revision"), State->Revision, uint32(11));
	const UFunction* DamagePresentation = Player->FindFunction(TEXT("ClientReceiveDamage"));
	if (!TestNotNull(TEXT("Owner damage feedback has a reflected RPC entry"), DamagePresentation)) return false;
	TestTrue(TEXT("Owner damage feedback is a reliable client RPC"), DamagePresentation->HasAllFunctionFlags(FUNC_Net | FUNC_NetClient | FUNC_NetReliable));
	TestFalse(TEXT("Damage feedback is not an additional server damage request"), DamagePresentation->HasAnyFunctionFlags(FUNC_NetServer));
	UfpstrueHealthComponent* PlayerHealth = Player->FindComponentByClass<UfpstrueHealthComponent>();
	if (!TestNotNull(TEXT("Damage presentation retains the real player health component"), PlayerHealth)) return false;
	FFPReplicatedHealthSnapshot* PlayerSnapshot = ReadMultiplayerHealthSnapshot(*PlayerHealth, *this);
	if (PlayerSnapshot == nullptr) return false;
	const float HealthBeforeFeedback = PlayerHealth->GetHealth();
	const uint32 HealthRevisionBeforeFeedback = PlayerSnapshot->Revision;
	int32 FeedbackDamageFacts = 0;
	const FDelegateHandle FeedbackObserver = PlayerHealth->OnDamageResolved.AddLambda(
		[&](float, AActor*, AController*, const FFPDamageContext&) { ++FeedbackDamageFacts; });
	// 直接回放接收实现，不伪装 RPC 已完成传输；当前 AI 控制器使本地表现分支实际可执行。
	TestTrue(TEXT("Damage-feedback fixture reaches the locally controlled presentation branch"), Player->IsLocallyControlled());
	Player->ClientReceiveDamage_Implementation(25.0f, Target, Controller);
	TestEqual(TEXT("Received damage feedback cannot deduct health a second time"), PlayerHealth->GetHealth(), HealthBeforeFeedback);
	TestEqual(TEXT("Received damage feedback cannot publish a new health revision"), PlayerSnapshot->Revision, HealthRevisionBeforeFeedback);
	TestEqual(TEXT("Received damage feedback cannot manufacture an authoritative damage fact"), FeedbackDamageFacts, 0);
	PlayerHealth->OnDamageResolved.Remove(FeedbackObserver);
	// 同一夹具恢复服务端权限，验证死亡取消当前能力，而不是只把 UI 或武器镜像标成不可用。
	Pickup->SetRole(ROLE_Authority);
	Player->SetRole(ROLE_Authority);
	Weapon->CancelReloadForTransaction(77);
	AbilitySystem->AbilityLocalInputPressed(ReloadInput);
	AbilitySystem->AbilityLocalInputReleased(ReloadInput);
	TestTrue(TEXT("A real reload ability is active before authoritative death"), ReloadSpec->IsActive() && Weapon->IsReloading());
	UGameplayStatics::ApplyDamage(Player, 1000.0f, Controller, Target, nullptr);
	TestTrue(TEXT("Authoritative lethal damage reaches the GAS health attribute"), PlayerHealth->IsDead());
	TestTrue(TEXT("Death owns the authoritative dead tag"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Dead));
	TestFalse(TEXT("Death cancels all four combat and movement ability instances"),
		FireSpec->IsActive() || ReloadSpec->IsActive() || AimSpec->IsActive() || SprintSpec->IsActive());
	TestFalse(TEXT("Death clears the reload transaction"), Weapon->IsReloading());
	TestFalse(TEXT("Death leaves no action-owned tags behind"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing) ||
		AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading) ||
		AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming) ||
		AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	TestFalse(TEXT("Dead players cannot reactivate the fire ability"), AbilitySystem->TryActivateAbilityByClass(UfpstrueFireAbility::StaticClass()));
	TestFalse(TEXT("Dead players cannot reactivate the reload ability"), AbilitySystem->TryActivateAbilityByClass(UfpstrueReloadAbility::StaticClass()));
	TestFalse(TEXT("Dead players cannot reactivate the aim ability"), AbilitySystem->TryActivateAbilityByClass(UfpstrueAimAbility::StaticClass()));
	TestFalse(TEXT("Dead players cannot reactivate the sprint ability"), AbilitySystem->TryActivateAbilityByClass(UfpstrueSprintAbility::StaticClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerExternalHealthEffectTest,
	"fpstrue.Multiplayer.GAS.ExternalHealthEffectsAndReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 不经旧 ApplyDamage 包装，真实外部 Instant GE 也须统一提交、广播和排队，不能悄悄只改 GAS 属性。
bool FFpstrueMultiplayerExternalHealthEffectTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	AActor* Owner = Fixture.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("External health-effect owner"), Owner)) return false;
	UfpstrueHealthComponent* Health = AddMultiplayerTestHealth(*Owner);
	const UfpstrueHealthAttributeSet* Attributes = Health->GetHealthAttributeSet();
	if (!TestNotNull(TEXT("External effect targets a production health attribute set"), Attributes)) return false;
	TStrongObjectPtr<UfpstrueHealthMutationTestObserver> Observer(NewObject<UfpstrueHealthMutationTestObserver>());
	Observer->Health = Health;
	Health->OnHealthChanged.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	Health->OnDeath.AddDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);
	const auto ApplyDamageEffect = [&](float Damage)
	{
		FGameplayEffectSpecHandle Spec = Health->MakeOutgoingSpec(UfpstrueHealthDamageEffect::StaticClass(), 1.0f, Health->MakeEffectContext());
		if (!TestTrue(TEXT("External damage spec is valid"), Spec.IsValid())) return;
		Spec.Data->SetSetByCallerMagnitude(FpstrueGameplayTags::Data_Damage, -Damage);
		Health->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	};
	ApplyDamageEffect(25.0f);
	TestEqual(TEXT("External Instant GE changes the real health attribute"), Attributes->GetHealth(), 75.0f);
	TestEqual(TEXT("External Instant GE refreshes the compatibility getter"), Health->GetHealth(), 75.0f);
	TestTrue(TEXT("External Instant GE publishes exactly one health notification"), Observer->HealthSnapshots == TArray<float>({75.0f}));
	Owner->SetRole(ROLE_SimulatedProxy);
	ApplyDamageEffect(25.0f);
	FGameplayEffectSpecHandle ResetSpec = Health->MakeOutgoingSpec(UfpstrueHealthResetEffect::StaticClass(), 1.0f, Health->MakeEffectContext());
	if (!TestTrue(TEXT("External reset spec is valid"), ResetSpec.IsValid())) return false;
	ResetSpec.Data->SetSetByCallerMagnitude(FpstrueGameplayTags::Data_MaxHealth, 200.0f);
	ResetSpec.Data->SetSetByCallerMagnitude(FpstrueGameplayTags::Data_Health, 200.0f);
	Health->ApplyGameplayEffectSpecToSelf(*ResetSpec.Data.Get());
	TestEqual(TEXT("Client cannot apply an external damage or reset GE to health"), Attributes->GetHealth(), 75.0f);
	TestEqual(TEXT("Client cannot apply an external maximum-health GE"), Attributes->GetMaxHealth(), 100.0f);
	TestTrue(TEXT("Rejected client effects do not notify local health consumers"), Observer->HealthSnapshots == TArray<float>({75.0f}));
	Owner->SetRole(ROLE_Authority);
	Health->ResetHealth();
	Observer->HealthSnapshots.Reset();
	// 当前生命事务只接纳瞬时数值提交；持续/周期 Health GE 不能绕过同一清理与通知协议。
	TStrongObjectPtr<UGameplayEffect> UnsupportedEffect(NewObject<UGameplayEffect>());
	UnsupportedEffect->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	UnsupportedEffect->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(2.0f));
	FGameplayModifierInfo& UnsupportedModifier = UnsupportedEffect->Modifiers.AddDefaulted_GetRef();
	UnsupportedModifier.Attribute = UfpstrueHealthAttributeSet::GetHealthAttribute();
	UnsupportedModifier.ModifierOp = EGameplayModOp::Additive;
	UnsupportedModifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-10.0f));
	AddExpectedError(TEXT("Health GAS only accepts instant non-periodic Health/MaxHealth effects"), EAutomationExpectedErrorFlags::Contains, 2);
	FGameplayEffectSpec DurationSpec(UnsupportedEffect.Get(), Health->MakeEffectContext(), 1.0f);
	TestFalse(TEXT("Duration health effects are explicitly rejected"), Health->ApplyGameplayEffectSpecToSelf(DurationSpec).IsValid());
	UnsupportedEffect->DurationPolicy = EGameplayEffectDurationType::Instant;
	UnsupportedEffect->Period = FScalableFloat(0.1f);
	FGameplayEffectSpec PeriodicSpec(UnsupportedEffect.Get(), Health->MakeEffectContext(), 1.0f);
	TestFalse(TEXT("Periodic health effects are explicitly rejected"), Health->ApplyGameplayEffectSpecToSelf(PeriodicSpec).IsValid());
	TestEqual(TEXT("Unsupported effects cannot change authoritative health"), Attributes->GetHealth(), 100.0f);
	TestTrue(TEXT("Unsupported effects do not publish health notifications"), Observer->HealthSnapshots.IsEmpty());
	TArray<float> AppliedDamage;
	const FDelegateHandle DamageObserver = Health->OnDamageResolved.AddLambda(
		[&](float Damage, AActor*, AController*, const FFPDamageContext&)
		{
			AppliedDamage.Add(Damage);
			TestEqual(TEXT("External GE observer sees already committed GAS and compatibility health"), Attributes->GetHealth(), Health->GetHealth());
			if (AppliedDamage.Num() == 1)
			{
				ApplyDamageEffect(60.0f);
				TestEqual(TEXT("A reentrant external GE waits until the current notification finishes"), Health->GetHealth(), 40.0f);
				TestEqual(TEXT("Queued external GE does not prematurely mutate its actual attribute"), Attributes->GetHealth(), 40.0f);
			}
		});
	ApplyDamageEffect(60.0f);
	TestTrue(TEXT("External reentrant effects resolve actual damage once per transaction"), AppliedDamage == TArray<float>({60.0f, 40.0f}));
	TestTrue(TEXT("External reentrant health notifications preserve commit order"), Observer->HealthSnapshots == TArray<float>({40.0f, 0.0f}));
	TestEqual(TEXT("External lethal GE commits zero to the actual attribute"), Attributes->GetHealth(), 0.0f);
	TestEqual(TEXT("External lethal GE notifies death exactly once"), Observer->Deaths, 1);
	TestTrue(TEXT("External lethal GE owns the same dead tag as UE damage"), Health->HasMatchingGameplayTag(FpstrueGameplayTags::State_Dead));
	ApplyDamageEffect(10.0f);
	TestEqual(TEXT("Repeated GE against dead health does not repeat damage facts"), AppliedDamage.Num(), 2);
	TestEqual(TEXT("Repeated lethal GE cannot repeat the death edge"), Observer->Deaths, 1);
	Health->OnDamageResolved.Remove(DamageObserver);
	Health->OnHealthChanged.RemoveDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnHealth);
	Health->OnDeath.RemoveDynamic(Observer.Get(), &UfpstrueHealthMutationTestObserver::OnDeath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerSavedMoveTest,
	"fpstrue.Multiplayer.Movement.SavedModesReplayAndAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 直接测试 UE 使用的 SavedMove 与压缩标志入口；恢复旧移动不重激活 GAS，不广播新的瞄准动作。
bool FFpstrueMultiplayerSavedMoveTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	AfpstrueCharacter* Player = nullptr;
	AAIController* Controller = nullptr;
	UfpstrueWeaponComponent* Weapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*Fixture.GetTestWorld(), *this, Player, Controller, Weapon)) return false;
	UfpstrueCharacterMovementComponent* Movement = Cast<UfpstrueCharacterMovementComponent>(Player->GetCharacterMovement());
	if (!TestNotNull(TEXT("Character constructs the production predicted movement component"), Movement)) return false;
	Movement->SetMovementMode(MOVE_Falling);
	auto* ClientData = static_cast<FNetworkPredictionData_Client_Character*>(Movement->GetPredictionData_Client());
	if (!TestNotNull(TEXT("Movement allocates real engine client prediction data"), ClientData)) return false;
	FSavedMovePtr SprintMove = ClientData->AllocateNewMove();
	FSavedMovePtr AimMove = ClientData->AllocateNewMove();
	if (!TestTrue(TEXT("Prediction data allocates both saved moves"), SprintMove.IsValid() && AimMove.IsValid())) return false;
	Movement->SetMovementIntent(false, true);
	SprintMove->SetMoveFor(Player, 0.016f, FVector::ZeroVector, *ClientData);
	TestTrue(TEXT("A saved sprint move carries its custom compressed bit"),
		(SprintMove->GetCompressedFlags() & FSavedMove_Character::FLAG_Custom_1) != 0);
	TestFalse(TEXT("A saved sprint move does not also carry aim"),
		(SprintMove->GetCompressedFlags() & FSavedMove_Character::FLAG_Custom_0) != 0);
	Movement->SetMovementIntent(true, false);
	AimMove->SetMoveFor(Player, 0.016f, FVector::ZeroVector, *ClientData);
	TestTrue(TEXT("A saved aim move carries its own compressed bit"),
		(AimMove->GetCompressedFlags() & FSavedMove_Character::FLAG_Custom_0) != 0);
	TestFalse(TEXT("Sprint and aim moves cannot be merged across a movement rule boundary"),
		SprintMove->CanCombineWith(AimMove, Player, 0.125f));
	TestFalse(TEXT("The reverse aim-to-sprint transition cannot be merged either"),
		AimMove->CanCombineWith(SprintMove, Player, 0.125f));
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	const int32 AbilityCountBeforeReplay = ASC->GetActivatableAbilities().Num();
	SprintMove->PrepMoveFor(Player);
	TestTrue(TEXT("Replaying sprint restores the historical movement mode, not today's aim mode"),
		Movement->IsSprintMovementRequested() && !Movement->IsAimMovementRequested());
	TestEqual(TEXT("Historical sprint uses the sprint speed"), Movement->GetMaxSpeed(), 600.0f);
	AimMove->PrepMoveFor(Player);
	TestTrue(TEXT("Replaying aim restores its own historical movement mode"),
		Movement->IsAimMovementRequested() && !Movement->IsSprintMovementRequested());
	TestEqual(TEXT("Historical aim uses the aim walking speed"), Movement->GetMaxSpeed(), 120.0f);
	TestFalse(TEXT("SavedMove replay cannot activate an aim ability or presentation mirror"),
		ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming) || Player->IsAiming());
	TestFalse(TEXT("SavedMove replay cannot activate a sprint ability"), ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	TestEqual(TEXT("SavedMove replay grants no additional gameplay abilities"), ASC->GetActivatableAbilities().Num(), AbilityCountBeforeReplay);
	SprintMove->Clear();
	TestEqual(TEXT("A recycled saved move clears both custom mode bits"),
		uint8(SprintMove->GetCompressedFlags() & (FSavedMove_Character::FLAG_Custom_0 | FSavedMove_Character::FLAG_Custom_1)), uint8(0));
	// 当前没有授权的 Sprint/Aim；客户端标志只是请求，不能凭空取得服务器速度或能力标签。
	Movement->SetMovementIntent(false, false);
	Movement->UpdateFromCompressedFlags(FSavedMove_Character::FLAG_Custom_1);
	TestFalse(TEXT("An ungranted client sprint bit is rejected by the server movement entry"), Movement->IsSprintMovementRequested());
	Movement->UpdateFromCompressedFlags(FSavedMove_Character::FLAG_Custom_0 | FSavedMove_Character::FLAG_Custom_1);
	TestFalse(TEXT("Mutually conflicting client movement bits cannot authorize either mode"),
		Movement->IsAimMovementRequested() || Movement->IsSprintMovementRequested());
	TestFalse(TEXT("Rejected movement flags cannot modify the authoritative GAS tags"),
		ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming) || ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerHitHistoryTest,
	"fpstrue.Multiplayer.Hitscan.HistoryGeometryTimeAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 查询过去的形状，不倒放整个世界；测试同时确认头身、移动插值、时间窗口、遮挡和历史失效。
// 本用例直接调用服务器查询实现，不代表客户端时间同步、RPC 传输或公网对战已经验收。
bool FFpstrueMultiplayerHitHistoryTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueCharacter* Target = SpawnMultiplayerHistoryTarget(*World, *this, FVector(500.0f, 0.0f, 0.0f));
	if (Target == nullptr) return false;
	// 用本机实际类型大小比较旧的32*64上限预留，不把缓存预算改善冒充整个进程内存收益。
	struct FLegacyHistoricalShapeBudget { FTransform Transform; FVector Extent; FName Bone; uint8 Type; };
	const SIZE_T LegacyMaximumShapeBytes = SIZE_T(32) * 64 * sizeof(FLegacyHistoricalShapeBudget);
	Target->ResetHitHistory(true);
	TestEqual(TEXT("Explicit historical-cache release returns allocated shape storage to zero"), Target->GetHitHistoryAllocatedBytes(), SIZE_T(0));
	Target->RecordServerHitHistory();
	const SIZE_T WarmHistoryBytes = Target->GetHitHistoryAllocatedBytes();
	const int32 WarmShapeCapacity = Target->GetHitHistoryReservedShapeCount();
	TestTrue(TEXT("History preallocates the fixture's three useful shapes for all ring frames, not the global maximum"),
		WarmShapeCapacity >= 32 * 3 && WarmShapeCapacity <= 32 * 4);
	TestTrue(TEXT("Removing invariant transform scale reduces the actual per-shape stride"),
		AfpstrueCharacter::GetHitHistoryShapeStride() < sizeof(FLegacyHistoricalShapeBudget));
	TestTrue(TEXT("PhysicsAsset-sized shape storage is smaller than the previous maximum reservation"),
		WarmHistoryBytes > 0 && WarmHistoryBytes < LegacyMaximumShapeBytes);
	for (int32 Sample = 0; Sample < 96; ++Sample)
	{
		if (!Fixture.TickTestWorld(1.0f / 60.0f)) { Fixture.ForwardErrorMessages(this); return false; }
		Target->RecordServerHitHistory();
	}
	TestEqual(TEXT("Three complete ring wraps cannot grow historical shape allocation"), Target->GetHitHistoryAllocatedBytes(), WarmHistoryBytes);
	TestEqual(TEXT("Hot historical sampling preserves the same reserved shape count"), Target->GetHitHistoryReservedShapeCount(), WarmShapeCapacity);
	Target->ResetHitHistory();
	TestEqual(TEXT("Normal life/teleport reset reuses its bounded allocation"), Target->GetHitHistoryAllocatedBytes(), WarmHistoryBytes);
	Target->ResetHitHistory(true);
	TestEqual(TEXT("Explicit release also frees a fully warmed ring"), Target->GetHitHistoryAllocatedBytes(), SIZE_T(0));
	Target->RecordServerHitHistory();
	TestEqual(TEXT("Sampling after release rebuilds only the same PhysicsAsset-sized storage"), Target->GetHitHistoryAllocatedBytes(), WarmHistoryBytes);
	AddInfo(FString::Printf(TEXT("HitHistory shape-only bytes: old upper reservation=%llu, actual=%llu; stride old=%llu new=%llu; reserved shapes=%d"),
		static_cast<uint64>(LegacyMaximumShapeBytes), static_cast<uint64>(WarmHistoryBytes),
		static_cast<uint64>(sizeof(FLegacyHistoricalShapeBudget)), static_cast<uint64>(AfpstrueCharacter::GetHitHistoryShapeStride()), WarmShapeCapacity));
	Target->ResetHitHistory();
	const double FirstTime = World->GetTimeSeconds();
	Target->RecordServerHitHistory();
	FHitResult Hit;
	TestTrue(TEXT("Historical head query uses a skeletal body rather than the movement capsule"),
		Target->TraceHistoricalHit(FirstTime, FVector(0.0f, 0.0f, 90.0f), FVector(1000.0f, 0.0f, 90.0f), Hit));
	TestEqual(TEXT("Historical head query preserves the actual bone identity"), Hit.BoneName, FName(TEXT("head")));
	TestTrue(TEXT("Historical body query finds the torso shape"),
		Target->TraceHistoricalHit(FirstTime, FVector(0.0f, 0.0f, 30.0f), FVector(1000.0f, 0.0f, 30.0f), Hit));
	TestEqual(TEXT("Historical body query does not manufacture a head hit"), Hit.BoneName, FName(TEXT("spine_03")));
	TestTrue(TEXT("A rotated box preserves its transformed surface normal"), FMath::Abs(Hit.ImpactNormal.Y) > 0.1f);
	TestTrue(TEXT("Capsule side geometry participates in the same historical query"),
		Target->TraceHistoricalHit(FirstTime, FVector(0.0f, 80.0f, 40.0f), FVector(1000.0f, 80.0f, 40.0f), Hit));
	TestEqual(TEXT("Capsule side retains its limb bone identity"), Hit.BoneName, FName(TEXT("upperarm_l")));
	TestTrue(TEXT("Capsule rounded end is not approximated by an oversized box"),
		Target->TraceHistoricalHit(FirstTime, FVector(0.0f, 80.0f, 68.0f), FVector(1000.0f, 80.0f, 68.0f), Hit));
	TestFalse(TEXT("Capsule end rejects points beyond its spherical cap"),
		Target->TraceHistoricalHit(FirstTime, FVector(0.0f, 80.0f, 72.0f), FVector(1000.0f, 80.0f, 72.0f), Hit));
	TestFalse(TEXT("A ray inside the broad movement capsule but outside all body shapes misses"),
		Target->TraceHistoricalHit(FirstTime, FVector(0.0f, 45.0f, 90.0f), FVector(1000.0f, 45.0f, 90.0f), Hit));
	if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	Target->SetActorLocation(FVector(500.0f, 100.0f, 0.0f));
	Target->RecordServerHitHistory();
	const double SecondTime = World->GetTimeSeconds();
	const double MiddleTime = (FirstTime + SecondTime) * 0.5;
	const FVector HistoricalHeadStart(0.0f, 50.0f, 90.0f);
	const FVector HistoricalHeadEnd(1000.0f, 50.0f, 90.0f);
	TestTrue(TEXT("A moving target can be hit at its interpolated historical position"),
		Target->TraceHistoricalHit(MiddleTime, HistoricalHeadStart, HistoricalHeadEnd, Hit));
	TestEqual(TEXT("Interpolation preserves the matched shape's head identity"), Hit.BoneName, FName(TEXT("head")));
	TestFalse(TEXT("The same ray misses the latest pose after the target has moved away"),
		Target->TraceHistoricalHit(SecondTime, HistoricalHeadStart, HistoricalHeadEnd, Hit));
	TestFalse(TEXT("An expired request cannot borrow the oldest retained frame"),
		Target->TraceHistoricalHit(FirstTime - AfpstrueCharacter::MaxHitHistorySeconds - 0.1,
			HistoricalHeadStart, HistoricalHeadEnd, Hit));
	TestFalse(TEXT("A future request outside the accepted time tolerance is rejected"),
		Target->TraceHistoricalHit(SecondTime + 1.0, HistoricalHeadStart, HistoricalHeadEnd, Hit));
	// 来源视点允许服务器本帧骨骼采样尚未结束的一帧间隔；目标形状不能因此额外向未来外推。
	if (!Fixture.TickTestWorld(0.033f)) { Fixture.ForwardErrorMessages(this); return false; }
	FVector LatestViewOrigin;
	const double BeforeNextBoneSample = World->GetTimeSeconds();
	TestTrue(TEXT("Shooter origin tolerates one bounded server frame before the next bone sample"),
		Target->GetHistoricalViewOrigin(BeforeNextBoneSample, LatestViewOrigin));
	TestFalse(TEXT("Target shape history does not inherit the shooter-origin end-of-frame tolerance"),
		Target->TraceHistoricalHit(BeforeNextBoneSample, FVector(0.0f, 100.0f, 90.0f), FVector(1000.0f, 100.0f, 90.0f), Hit));
	TestFalse(TEXT("Shooter-origin tolerance still rejects an implausibly future time"),
		Target->GetHistoricalViewOrigin(BeforeNextBoneSample + 1.0, LatestViewOrigin));
	// 静态遮挡仍参与查询，历史目标形状不能让子弹穿过服务器现在明确存在的墙。
	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Historical hitscan static blocker"), Wall)) return false;
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(WallBox);
	Wall->SetRootComponent(WallBox);
	WallBox->SetMobility(EComponentMobility::Static);
	WallBox->SetBoxExtent(FVector(10.0f, 300.0f, 200.0f));
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WallBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	WallBox->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	WallBox->SetWorldLocation(FVector(250.0f, 0.0f, 0.0f));
	WallBox->RegisterComponent();
	const bool bBlockedTraceHit = AfpstrueCharacter::TraceHistoricalHit(World, nullptr, MiddleTime,
		HistoricalHeadStart, HistoricalHeadEnd, Hit);
	TestTrue(TEXT("Global historical trace cannot select the player behind a blocking wall"), !bBlockedTraceHit || Hit.GetActor() != Target);
	Wall->Destroy();
	TestTrue(TEXT("Removing the wall restores the same historical player query"),
		AfpstrueCharacter::TraceHistoricalHit(World, nullptr, MiddleTime, HistoricalHeadStart, HistoricalHeadEnd, Hit));
	TestTrue(TEXT("The unblocked global query resolves the actual target"), Hit.GetActor() == Target);
	const uint32 PreviousEpoch = Target->GetHitHistoryEpoch();
	Target->ResetHitHistory();
	Target->RecordServerHitHistory();
	TestTrue(TEXT("Reset advances the historical life/teleport identity"), Target->GetHitHistoryEpoch() != PreviousEpoch);
	TestFalse(TEXT("A new history epoch cannot interpolate through the previous life"),
		Target->TraceHistoricalHit(MiddleTime, HistoricalHeadStart, HistoricalHeadEnd, Hit));
	UGameplayStatics::ApplyDamage(Target, 1000.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Lethal damage reaches the real health lifecycle"), Target->IsDead());
	TestFalse(TEXT("A dead target cannot be hit by retained history"),
		Target->TraceHistoricalHit(SecondTime, FVector(0.0f, 100.0f, 90.0f), FVector(1000.0f, 100.0f, 90.0f), Hit));
	Target->Destroy();
	TestEqual(TEXT("EndPlay releases the retained historical shape allocation"), Target->GetHitHistoryAllocatedBytes(), SIZE_T(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerShotProtocolTest,
	"fpstrue.Multiplayer.Weapon.ShotIdentityCadenceAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 在明确的服务器开火上下文中调用同一逐发入口，覆盖重复、射速、时间、装备和死亡边界。
// 这里只建立 ASC 的 Firing 前态，不激活会自行发射的 Standalone 连射任务；真实 GA 生命周期由上面的完整用例覆盖。
bool FFpstrueMultiplayerShotProtocolTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueCharacter* Player = nullptr;
	AAIController* Controller = nullptr;
	UfpstrueWeaponComponent* Weapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*World, *this, Player, Controller, Weapon)) return false;
	AfpstrueCharacter* Target = SpawnMultiplayerHistoryTarget(*World, *this, FVector(500.0f, 0.0f, 0.0f));
	if (Target == nullptr) return false;
	Target->GetHealthComponent()->SetMaxHealthAndReset(300.0f);
	Target->ResetHitHistory();
	Target->RecordServerHitHistory();
	// 只取消随机散布以便确定射向头或身体；射速、伤害、扣弹和请求校验仍由生产代码执行。
	Weapon->Settings.HipFireSpreadAngle = 0.0f;
	Weapon->Settings.AimFireSpreadAngle = 0.0f;
	Weapon->Settings.ContinuousFireSpreadStep = 0.0f;
	Weapon->Settings.MaxContinuousFireSpreadAngle = 0.0f;
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	UfpstrueHealthComponent* HealthASC = Player->GetHealthComponent();
	FGameplayAbilitySpec* FireSpec = ASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
	FGameplayAbilitySpec* AimSpec = ASC->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass());
	if (!TestNotNull(TEXT("Late target-data fixture has its granted Fire ability"), FireSpec) ||
		!TestNotNull(TEXT("Late target-data fixture has its granted Aim ability"), AimSpec)) return false;
	FPredictionKey OldKey;
	OldKey.Current = 123;
	FGameplayAbilityTargetDataHandle LateData(new FGameplayAbilityTargetData_FPShot());
	FGameplayAbilitySpecHandle InvalidHandle;
	const int32 AmmoBeforeLateData = Weapon->GetCurrentAmmo();
	// 不先 AddDelegate 制造缓存；未知、非活动和旧激活键的数据都必须在入口被消费/拒绝。
	HealthASC->ServerSetReplicatedTargetData_Implementation(InvalidHandle, OldKey, LateData, FGameplayTag(), FPredictionKey());
	TestFalse(TEXT("Unknown ability data cannot create a replicated target-data cache"),
		ASC->CallReplicatedTargetDataDelegatesIfSet(InvalidHandle, OldKey));
	HealthASC->ServerSetReplicatedTargetData_Implementation(FireSpec->Handle, OldKey, LateData, FGameplayTag(), FPredictionKey());
	TestFalse(TEXT("Inactive Fire data cannot recreate a cancelled activation cache"),
		ASC->CallReplicatedTargetDataDelegatesIfSet(FireSpec->Handle, OldKey));
	ASC->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Aim));
	if (!TestTrue(TEXT("Old-key check uses a genuinely active production ability"), AimSpec->IsActive())) return false;
	HealthASC->ServerSetReplicatedTargetData_Implementation(AimSpec->Handle, OldKey, LateData, FGameplayTag(), FPredictionKey());
	TestFalse(TEXT("An old prediction key cannot create data for a different active activation"),
		ASC->CallReplicatedTargetDataDelegatesIfSet(AimSpec->Handle, OldKey));
	TestTrue(TEXT("Rejecting old target data does not cancel the current activation"), AimSpec->IsActive());
	ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
	TestEqual(TEXT("Rejected unknown, inactive and old-key data cannot spend ammunition"), Weapon->GetCurrentAmmo(), AmmoBeforeLateData);
	ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	Weapon->BeginAbilityFire(31);
	const auto MakeRequest = [&](uint32 ShotId, const FVector& AimPoint)
	{
		FFPShotRequest Request;
		Request.Avatar = Player;
		Request.EquipmentGeneration = Weapon->EquipmentRevision;
		Request.FireSession = 1;
		Request.ShotId = ShotId;
		Request.ServerFireTime = World->GetTimeSeconds();
		Request.AimDirection = (AimPoint - Player->GetFirstPersonCameraComponent()->GetComponentLocation()).GetSafeNormal();
		return Request;
	};
	const FVector BodyPoint = Target->GetActorLocation() + FVector(0.0f, 0.0f, 30.0f);
	const FVector HeadPoint = Target->GetActorLocation() + FVector(0.0f, 0.0f, 90.0f);
	const int32 InitialAmmo = Weapon->GetCurrentAmmo();
	FFPShotRequest First = MakeRequest(1, BodyPoint);
	TestTrue(TEXT("First valid shot is accepted by the authoritative request entry"), Weapon->AcceptShotRequest(First) == EFPShotResult::Accepted);
	TestEqual(TEXT("An accepted shot spends exactly one round"), Weapon->GetCurrentAmmo(), InitialAmmo - 1);
	TestEqual(TEXT("Body identity produces ordinary damage through the real health chain"), Target->GetCurrentHealth(), 260.0f);
	TestTrue(TEXT("Replaying the same ShotId is explicitly rejected"), Weapon->AcceptShotRequest(First) == EFPShotResult::Duplicate);
	TestEqual(TEXT("A duplicate shot cannot spend another round"), Weapon->GetCurrentAmmo(), InitialAmmo - 1);
	TestEqual(TEXT("A duplicate shot cannot apply damage again"), Target->GetCurrentHealth(), 260.0f);
	FFPShotRequest TooFast = MakeRequest(2, BodyPoint);
	TestTrue(TEXT("A different ShotId cannot bypass the configured firing cadence"), Weapon->AcceptShotRequest(TooFast) == EFPShotResult::RateLimited);
	TestEqual(TEXT("A rate-limited request does not spend ammo"), Weapon->GetCurrentAmmo(), InitialAmmo - 1);
	const float ShotInterval = 60.0f / Weapon->GetWeaponSettings().RoundsPerMinute;
	if (!Fixture.TickTestWorld(ShotInterval + 0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
	Target->RecordServerHitHistory();
	FFPShotRequest Head = MakeRequest(3, HeadPoint);
	TestTrue(TEXT("The next correctly timed head shot is accepted"), Weapon->AcceptShotRequest(Head) == EFPShotResult::Accepted);
	TestEqual(TEXT("A second valid shot spends one more round"), Weapon->GetCurrentAmmo(), InitialAmmo - 2);
	TestEqual(TEXT("Head identity selects critical damage rather than the movement capsule"), Target->GetCurrentHealth(), 160.0f);
	const int32 AmmoAfterValidShots = Weapon->GetCurrentAmmo();
	FFPShotRequest Expired = MakeRequest(4, BodyPoint);
	Expired.ServerFireTime -= AfpstrueCharacter::MaxHitHistorySeconds + 0.1;
	TestTrue(TEXT("Expired fire time is rejected rather than clamped into the history"), Weapon->AcceptShotRequest(Expired) == EFPShotResult::InvalidTime);
	FFPShotRequest Future = MakeRequest(5, BodyPoint);
	Future.ServerFireTime += 1.0;
	TestTrue(TEXT("An implausibly future fire time is rejected"), Weapon->AcceptShotRequest(Future) == EFPShotResult::InvalidTime);
	FFPShotRequest InvalidAim = MakeRequest(6, BodyPoint);
	InvalidAim.AimDirection = FVector::ZeroVector;
	TestTrue(TEXT("A zero direction is rejected before authoritative hitscan"), Weapon->AcceptShotRequest(InvalidAim) == EFPShotResult::InvalidAim);
	FFPShotRequest WrongAvatar = MakeRequest(7, BodyPoint);
	WrongAvatar.Avatar = Target;
	TestTrue(TEXT("A shot cannot name a different shooter avatar"), Weapon->AcceptShotRequest(WrongAvatar) == EFPShotResult::InvalidIdentity);
	TestEqual(TEXT("Invalid time, direction and identity cannot subtract ammo"), Weapon->GetCurrentAmmo(), AmmoAfterValidShots);
	TestEqual(TEXT("Rejected malformed requests cannot damage the target"), Target->GetCurrentHealth(), 160.0f);
	// 即使武器 Actor 复用，重新装备仍是新的代际；旧请求不能借用刚绑定的新关系。
	FFPShotRequest BeforeReequip = MakeRequest(8, BodyPoint);
	const uint32 OldGeneration = BeforeReequip.EquipmentGeneration;
	Weapon->EndAbilityFire();
	Weapon->DetachWeapon();
	if (!TestTrue(TEXT("The same weapon can be equipped again through the complete public transaction"), Weapon->AttachWeapon(Player))) return false;
	ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	Weapon->BeginAbilityFire(32);
	TestTrue(TEXT("Re-equipping advances the authoritative equipment generation"), Weapon->EquipmentRevision != OldGeneration);
	TestTrue(TEXT("A delayed pre-equip request is rejected by generation"), Weapon->AcceptShotRequest(BeforeReequip) == EFPShotResult::InvalidIdentity);
	TestEqual(TEXT("A stale equipment request cannot spend the surviving magazine"), Weapon->GetCurrentAmmo(), AmmoAfterValidShots);
	FFPShotRequest BeforeDeath = MakeRequest(9, BodyPoint);
	UGameplayStatics::ApplyDamage(Player, 1000.0f, Controller, Target, nullptr);
	TestTrue(TEXT("The shooter enters its real authoritative death lifecycle"), Player->IsDead());
	TestTrue(TEXT("A delayed request after death cannot execute"), Weapon->AcceptShotRequest(BeforeDeath) == EFPShotResult::InvalidContext);
	TestEqual(TEXT("Death cannot turn a delayed shot into another ammo commit"), Weapon->GetCurrentAmmo(), AmmoAfterValidShots);
	TestEqual(TEXT("Delayed old-life fire cannot damage another player"), Target->GetCurrentHealth(), 160.0f);
	ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing, ASC->GetTagCount(FpstrueGameplayTags::State_Firing));
	// 冷却中的单个请求只能有一个最终裁决；同一活动轮次重复送达不能先用 Duplicate ACK 覆盖待执行结果。
	AfpstrueCharacter* QueuePlayer = nullptr;
	AAIController* QueueController = nullptr;
	UfpstrueWeaponComponent* QueueWeapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*World, *this, QueuePlayer, QueueController, QueueWeapon)) return false;
	UfpstrueHealthComponent* QueueASC = QueuePlayer->GetHealthComponent();
	QueueASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	QueueWeapon->BeginAbilityFire(41);
	FFPShotRequest QueueFirst;
	QueueFirst.Avatar = QueuePlayer;
	QueueFirst.EquipmentGeneration = QueueWeapon->EquipmentRevision;
	QueueFirst.FireSession = 1;
	QueueFirst.ShotId = 1;
	// 两个客户端时间间隔合法，但到达服务器时集中在同一帧，第二发必须等待真实射速冷却。
	QueueFirst.ServerFireTime = World->GetTimeSeconds() - 0.09;
	QueueFirst.AimDirection = FVector::UpVector;
	TestTrue(TEXT("Queued-shot fixture first commits one real authoritative round"),
		QueueWeapon->AcceptShotRequest(QueueFirst) == EFPShotResult::Accepted);
	const int32 AmmoBeforeQueue = QueueWeapon->GetCurrentAmmo();
	FFPShotRequest Pending = QueueFirst;
	Pending.ShotId = 2;
	Pending.ServerFireTime += 60.0 / QueueWeapon->GetWeaponSettings().RoundsPerMinute;
	TestTrue(TEXT("A correctly spaced client shot waits for the authoritative cooldown"),
		QueueWeapon->AcceptShotRequest(Pending) == EFPShotResult::Queued);
	TestEqual(TEXT("Queueing a shot cannot prematurely advance the finalized acknowledgement"), QueueWeapon->LastProcessedShotId, uint32(1));
	TestEqual(TEXT("Queueing cannot spend the pending round early"), QueueWeapon->GetCurrentAmmo(), AmmoBeforeQueue);
	TestTrue(TEXT("A repeated pending identity remains queued rather than receiving a terminal duplicate ACK"),
		QueueWeapon->AcceptShotRequest(Pending) == EFPShotResult::Queued);
	TestEqual(TEXT("Repeating a pending request leaves the previous finalized identity intact"), QueueWeapon->LastProcessedShotId, uint32(1));
	TestTrue(TEXT("A pending duplicate cannot overwrite the previous accepted result"), QueueWeapon->LastShotResult == EFPShotResult::Accepted);
	TestEqual(TEXT("A pending duplicate cannot spend another round"), QueueWeapon->GetCurrentAmmo(), AmmoBeforeQueue);
	// 夹具在两次 World Tick 之间建 Timer；先以 1ms 短帧激活 Pending，再验证冷却到期后的自动消费。
	if (!Fixture.TickTestWorld(0.001f)) { Fixture.ForwardErrorMessages(this); return false; }
	if (!Fixture.TickTestWorld(60.0f / QueueWeapon->GetWeaponSettings().RoundsPerMinute + 0.02f))
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	TestFalse(TEXT("The unique admitted-shot timer automatically drains the original queued request"), QueueWeapon->HasPendingNetworkShots());
	TestFalse(TEXT("An already timer-executed request cannot be executed again manually"), QueueWeapon->ExecuteNetworkShot(Pending));
	TestEqual(TEXT("The original queued request finalizes its own identity"), QueueWeapon->LastProcessedShotId, Pending.ShotId);
	TestTrue(TEXT("The pending request finally publishes Accepted rather than a stale Duplicate result"),
		QueueWeapon->LastShotResult == EFPShotResult::Accepted && QueueWeapon->NetworkState.ShotResult == EFPShotResult::Accepted);
	TestEqual(TEXT("Queue plus repeated delivery spends exactly one pending round"), QueueWeapon->GetCurrentAmmo(), AmmoBeforeQueue - 1);
	TestTrue(TEXT("A completed queued request cannot execute a second time"), QueueWeapon->AcceptShotRequest(Pending) == EFPShotResult::Duplicate);
	TestTrue(TEXT("Repeating a completed request cannot replace its persistent accepted acknowledgement"),
		QueueWeapon->LastShotResult == EFPShotResult::Accepted && QueueWeapon->NetworkState.ShotResult == EFPShotResult::Accepted);
	TestEqual(TEXT("A finalized duplicate leaves the magazine unchanged"), QueueWeapon->GetCurrentAmmo(), AmmoBeforeQueue - 1);

	// 三个时间间隔合法的输入在同一服务器帧到达：保留有界FIFO，而非第一发以后只能容纳一项。
	// 单测把射速提高到1800RPM，使三发时间均位于默认100ms准入窗；不修改生产时间容差。
	AfpstrueCharacter* BatchPlayer = nullptr;
	AAIController* BatchController = nullptr;
	UfpstrueWeaponComponent* BatchWeapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*World, *this, BatchPlayer, BatchController, BatchWeapon)) return false;
	UfpstrueHealthComponent* BatchASC = BatchPlayer->GetHealthComponent();
	BatchASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	BatchWeapon->Settings.RoundsPerMinute = 1800.0f;
	BatchWeapon->BeginAbilityFire(51);
	const int32 BatchInitialAmmo = BatchWeapon->GetCurrentAmmo();
	const double BatchReceivedTime = World->GetTimeSeconds();
	const double BatchInterval = 60.0 / BatchWeapon->Settings.RoundsPerMinute;
	FFPShotRequest BatchRequests[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FFPShotRequest& Request = BatchRequests[Index];
		Request.Avatar = BatchPlayer;
		Request.EquipmentGeneration = BatchWeapon->EquipmentRevision;
		Request.FireSession = 1;
		Request.ShotId = Index + 1;
		Request.ServerFireTime = BatchReceivedTime - 0.09 + BatchInterval * Index;
		Request.AimDirection = FVector::UpVector;
		TestTrue(TEXT("Each correctly timed round in a three-round arrival burst is admitted"),
			BatchWeapon->AcceptShotRequest(Request) == (Index == 0 ? EFPShotResult::Accepted : EFPShotResult::Queued));
	}
	TestEqual(TEXT("Two admitted arrival-burst rounds remain in FIFO without spending early"), BatchWeapon->PendingNetworkShots.Num(), 2);
	TestEqual(TEXT("Only the first burst round spends ammunition immediately"), BatchWeapon->GetCurrentAmmo(), BatchInitialAmmo - 1);
	TestTrue(TEXT("Repeating the last pending round observes its existing queued entry"),
		BatchWeapon->AcceptShotRequest(BatchRequests[2]) == EFPShotResult::Queued);
	TestEqual(TEXT("Pending duplicates cannot grow the FIFO"), BatchWeapon->PendingNetworkShots.Num(), 2);
	BatchWeapon->EndAbilityFire();
	BatchASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	TestEqual(TEXT("Normal trigger release keeps both already-admitted rounds"), BatchWeapon->PendingNetworkShots.Num(), 2);
	BatchASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	BatchWeapon->BeginAbilityFire(52);
	FFPShotRequest NextSessionRound = BatchRequests[2];
	NextSessionRound.FireSession = 2;
	NextSessionRound.ShotId = 4;
	NextSessionRound.ServerFireTime = BatchReceivedTime + 0.01;
	TestTrue(TEXT("A fresh trigger session can admit work while the previous session's FIFO still drains"),
		BatchWeapon->AcceptShotRequest(NextSessionRound) == EFPShotResult::Queued);
	TestEqual(TEXT("Beginning another input session cannot discard the old admitted rounds"), BatchWeapon->PendingNetworkShots.Num(), 3);
	BatchWeapon->EndAbilityFire();
	BatchASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	if (!Fixture.TickTestWorld(0.001f)) { Fixture.ForwardErrorMessages(this); return false; }
	for (int32 Index = 1; Index < 4; ++Index)
	{
		if (!Fixture.TickTestWorld(float(BatchInterval) + 0.002f)) { Fixture.ForwardErrorMessages(this); return false; }
		TestTrue(TEXT("Each admitted round executes independently after input release"), BatchWeapon->LastShotResult == EFPShotResult::Accepted);
		TestEqual(TEXT("FIFO execution finalizes the correct round identity"), BatchWeapon->LastProcessedShotId, uint32(Index + 1));
		TestEqual(TEXT("Each drained burst entry spends exactly one authoritative round"), BatchWeapon->GetCurrentAmmo(), BatchInitialAmmo - Index - 1);
	}
	TestFalse(TEXT("A completely drained burst has no hidden extra round"), BatchWeapon->HasPendingNetworkShots());
	TestFalse(TEXT("Draining an empty FIFO cannot execute another shot"), BatchWeapon->ExecuteNextPendingNetworkShot());
	BatchASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	BatchWeapon->BeginAbilityFire(53);
	FFPShotRequest CancelledBurst = BatchRequests[2];
	CancelledBurst.FireSession = 3;
	CancelledBurst.ShotId = 5;
	CancelledBurst.ServerFireTime = World->GetTimeSeconds();
	TestTrue(TEXT("A later valid round is queued behind the just-executed cooldown"),
		BatchWeapon->AcceptShotRequest(CancelledBurst) == EFPShotResult::Queued);
	const int32 BeforeStrongCancel = BatchWeapon->GetCurrentAmmo();
	BatchWeapon->EndAbilityFire(true);
	TestFalse(TEXT("Strong cancellation removes every admitted pending round"), BatchWeapon->HasPendingNetworkShots());
	TestTrue(TEXT("Strong cancellation records an explicit terminal Cancelled result"), BatchWeapon->LastShotResult == EFPShotResult::Cancelled);
	TestEqual(TEXT("Cancelling admitted work cannot spend its still-pending ammunition"), BatchWeapon->GetCurrentAmmo(), BeforeStrongCancel);
	TestFalse(TEXT("A cancelled queued round cannot execute after a delayed cooldown callback"), BatchWeapon->ExecuteNetworkShot(CancelledBurst));
	BatchASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);

	// 真正的 Timer 回归：复用本夹具已有的1800RPM/100ms准入窗，不改变生产射速、年龄或队列上限。
	// 尾部准入不能重建未变队头的句柄/截止时间；提前唤醒仍必须经生产回调重新安排。
	if (!Fixture.TickTestWorld(0.2f)) { Fixture.ForwardErrorMessages(this); return false; }
	BatchASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	BatchWeapon->BeginAbilityFire(54);
	const int32 TimerCaseInitialAmmo = BatchWeapon->GetCurrentAmmo();
	const double TimerCaseReceivedTime = World->GetTimeSeconds();
	FFPShotRequest TimerCaseRequests[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FFPShotRequest& Request = TimerCaseRequests[Index];
		Request.Avatar = BatchPlayer;
		Request.EquipmentGeneration = BatchWeapon->EquipmentRevision;
		Request.FireSession = 4;
		Request.ShotId = 6 + Index;
		Request.ServerFireTime = TimerCaseReceivedTime - 0.09 + BatchInterval * Index;
		Request.AimDirection = FVector::UpVector;
	}
	if (!TestTrue(TEXT("Timer ownership fixture commits its first legal round"),
		BatchWeapon->AcceptShotRequest(TimerCaseRequests[0]) == EFPShotResult::Accepted)
		|| !TestTrue(TEXT("Timer ownership fixture queues its original cooldown head"),
		BatchWeapon->AcceptShotRequest(TimerCaseRequests[1]) == EFPShotResult::Queued)) return false;
	// 第一个真实 Tick 激活 Pending，第二个 Tick 消耗部分等待，但不走到33.3ms截止时间。
	if (!Fixture.TickTestWorld(0.001f) || !Fixture.TickTestWorld(0.01f)) { Fixture.ForwardErrorMessages(this); return false; }
	FTimerManager& ShotTimers = World->GetTimerManager();
	const FTimerHandle OriginalHeadTimer = BatchWeapon->NetworkShotTimerHandle;
	const float HeadRemainingBeforeTail = ShotTimers.GetTimerRemaining(OriginalHeadTimer);
	const float HeadRateBeforeTail = ShotTimers.GetTimerRate(OriginalHeadTimer);
	const double HeadDeadlineBeforeTail = World->GetTimeSeconds() + HeadRemainingBeforeTail;
	const double CadenceDeadlineBeforeTail = BatchWeapon->NextShotTimeSeconds;
	if (!TestTrue(TEXT("The real original head timer is active with positive remaining time"),
		ShotTimers.TimerExists(OriginalHeadTimer) && !ShotTimers.IsTimerPending(OriginalHeadTimer) && HeadRemainingBeforeTail > 0.0f)) return false;
	TestTrue(TEXT("A legal tail request joins the existing cooldown head"),
		BatchWeapon->AcceptShotRequest(TimerCaseRequests[2]) == EFPShotResult::Queued);
	TestEqual(TEXT("Tail admission keeps exactly the original head plus one tail"), BatchWeapon->PendingNetworkShots.Num(), 2);
	TestTrue(TEXT("Tail admission preserves the original head timer handle"), BatchWeapon->NetworkShotTimerHandle == OriginalHeadTimer);
	TestTrue(TEXT("Tail admission preserves original head remaining time without a World tick"),
		FMath::IsNearlyEqual(ShotTimers.GetTimerRemaining(BatchWeapon->NetworkShotTimerHandle), HeadRemainingBeforeTail, 0.000001f));
	TestTrue(TEXT("Tail admission preserves original head timer rate"),
		FMath::IsNearlyEqual(ShotTimers.GetTimerRate(BatchWeapon->NetworkShotTimerHandle), HeadRateBeforeTail, 0.000001f));
	TestTrue(TEXT("Tail admission preserves original head absolute deadline"),
		FMath::IsNearlyEqual(World->GetTimeSeconds() + ShotTimers.GetTimerRemaining(BatchWeapon->NetworkShotTimerHandle), HeadDeadlineBeforeTail, 0.000001));
	TestEqual(TEXT("Tail admission cannot shift the server firing cadence deadline"), BatchWeapon->NextShotTimeSeconds, CadenceDeadlineBeforeTail);
	TestEqual(TEXT("Tail admission cannot commit another round early"), BatchWeapon->GetCurrentAmmo(), TimerCaseInitialAmmo - 1);

	// 定向提前唤醒注入：只换测试调度，不改射速截止时间；实际生产 Process 在 Executing 状态下必须能重新排 Timer。
	ShotTimers.SetTimer(BatchWeapon->NetworkShotTimerHandle, BatchWeapon, &UfpstrueWeaponComponent::ProcessPendingNetworkShot, 0.001f, false);
	const FTimerHandle EarlyWakeTimer = BatchWeapon->NetworkShotTimerHandle;
	if (!Fixture.TickTestWorld(0.001f) || !Fixture.TickTestWorld(0.002f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestEqual(TEXT("An early real timer callback cannot consume an unexpired cooldown head"), BatchWeapon->GetCurrentAmmo(), TimerCaseInitialAmmo - 1);
	TestEqual(TEXT("Early callback preserves both pending facts"), BatchWeapon->PendingNetworkShots.Num(), 2);
	TestTrue(TEXT("Executing production callback can replace its early wake with a valid future timer"),
		!ShotTimers.TimerExists(EarlyWakeTimer) && ShotTimers.TimerExists(BatchWeapon->NetworkShotTimerHandle)
		&& BatchWeapon->NetworkShotTimerHandle != EarlyWakeTimer);
	for (int32 Tick = 0; Tick < 8 && BatchWeapon->HasPendingNetworkShots(); ++Tick)
	{
		const int32 AmmoBeforeTimerTick = BatchWeapon->GetCurrentAmmo();
		if (!Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
		TestTrue(TEXT("Actual rescheduled timer callbacks commit at most one round per World tick"),
			AmmoBeforeTimerTick - BatchWeapon->GetCurrentAmmo() <= 1);
		if (BatchWeapon->HasPendingNetworkShots()) TestTrue(TEXT("A remaining head retains one valid continuation timer"),
			ShotTimers.TimerExists(BatchWeapon->NetworkShotTimerHandle));
	}
	TestFalse(TEXT("The early-wake production callback still drains its original head and tail"), BatchWeapon->HasPendingNetworkShots());
	TestFalse(TEXT("A drained timer queue has no hidden continuation timer"), ShotTimers.TimerExists(BatchWeapon->NetworkShotTimerHandle));
	TestEqual(TEXT("The timer ownership fixture spends exactly its three actual rounds"), BatchWeapon->GetCurrentAmmo(), TimerCaseInitialAmmo - 3);
	TestEqual(TEXT("The real continuation finalizes its own final identity"), BatchWeapon->LastProcessedShotId, uint32(8));
	TestTrue(TEXT("The final timer-driven verdict is Accepted"), BatchWeapon->LastShotResult == EFPShotResult::Accepted);
	FFPShotRequest TimerCancelledRequest = TimerCaseRequests[2];
	TimerCancelledRequest.ShotId = 9;
	TimerCancelledRequest.ServerFireTime = World->GetTimeSeconds();
	TestTrue(TEXT("Strong-cancel timer fixture first queues one real cooldown request"),
		BatchWeapon->AcceptShotRequest(TimerCancelledRequest) == EFPShotResult::Queued);
	const FTimerHandle StrongCancelledTimer = BatchWeapon->NetworkShotTimerHandle;
	const int32 AmmoBeforeTimerCancel = BatchWeapon->GetCurrentAmmo();
	BatchWeapon->EndAbilityFire(true);
	TestFalse(TEXT("Strong cancellation destroys the actual pending head timer"), ShotTimers.TimerExists(StrongCancelledTimer));
	if (!Fixture.TickTestWorld(0.2f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestFalse(TEXT("Ticking past a cancelled deadline cannot revive the queue"), BatchWeapon->HasPendingNetworkShots());
	TestFalse(TEXT("Cancelled continuation remains absent after a real later World tick"), ShotTimers.TimerExists(BatchWeapon->NetworkShotTimerHandle));
	TestEqual(TEXT("Cancelled timer cannot consume a late round"), BatchWeapon->GetCurrentAmmo(), AmmoBeforeTimerCancel);
	TestEqual(TEXT("A later tick cannot replace the cancelled terminal identity"), BatchWeapon->LastProcessedShotId, uint32(9));
	TestTrue(TEXT("A later tick leaves the terminal verdict Cancelled"), BatchWeapon->LastShotResult == EFPShotResult::Cancelled);
	BatchASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);

	// 直接执行客户端裁决接收入口，验证账本收敛与版本；这不是RPC传输或客户端命中预测测试。
	BatchWeapon->GetOwner()->SetRole(ROLE_SimulatedProxy);
	BatchPlayer->SetRole(ROLE_SimulatedProxy);
	BatchWeapon->CurrentAmmo = BatchWeapon->Settings.MagazineSize;
	BatchWeapon->ReserveAmmo = 32;
	BatchWeapon->LastAmmoStateRevision = 100;
	BatchWeapon->OutstandingShots = {101};
	BatchWeapon->bPredictingFire = true;
	BatchWeapon->EndAbilityFire();
	TestEqual(TEXT("Normal trigger release preserves a single unacknowledged shot"), BatchWeapon->OutstandingShots.Num(), 1);
	TestTrue(TEXT("A full but stale magazine snapshot cannot block reload before the single-shot ACK"), BatchWeapon->HasReloadAmmo());
	BatchWeapon->ClientShotResult_Implementation(BatchWeapon->EquipmentRevision, 1, 101, EFPShotResult::InvalidTime,
		BatchWeapon->Settings.MagazineSize, 32, 100);
	TestEqual(TEXT("An explicit rejected-shot ACK releases exactly that outstanding estimate"), BatchWeapon->OutstandingShots.Num(), 0);
	TestFalse(TEXT("After rejection a genuinely full magazine has no artificial reload gap"), BatchWeapon->HasReloadAmmo());
	TestEqual(TEXT("Rejected prediction cannot manufacture an authoritative ammo decrement"), BatchWeapon->GetCurrentAmmo(), BatchWeapon->Settings.MagazineSize);
	BatchWeapon->OutstandingShots = {102, 103};
	BatchWeapon->ClientShotResult_Implementation(BatchWeapon->EquipmentRevision, 2, 102, EFPShotResult::Accepted,
		BatchWeapon->Settings.MagazineSize - 1, 31, 102);
	TestEqual(TEXT("A valid ACK removes only its own estimate, not another in-flight round"), BatchWeapon->OutstandingShots.Num(), 1);
	BatchWeapon->ClientShotResult_Implementation(BatchWeapon->EquipmentRevision, 1, 103, EFPShotResult::Cancelled,
		BatchWeapon->Settings.MagazineSize, 32, 101);
	TestEqual(TEXT("An older terminal ACK still releases its matching estimate"), BatchWeapon->OutstandingShots.Num(), 0);
	TestEqual(TEXT("An older ACK cannot overwrite newer authoritative magazine state"), BatchWeapon->GetCurrentAmmo(), BatchWeapon->Settings.MagazineSize - 1);
	TestEqual(TEXT("An older ACK cannot overwrite newer authoritative reserve state"), BatchWeapon->GetReserveAmmo(), 31);
	BatchWeapon->OutstandingShots = {103, 104};
	BatchWeapon->FirstPredictedShotForCurrentFire = 104;
	BatchWeapon->LocalShotSerial = 104;
	BatchWeapon->EndAbilityFire(true);
	TestTrue(TEXT("Rejecting the current prediction discards only its own shot range, not an older session's estimate"),
		BatchWeapon->OutstandingShots.Num() == 1 && BatchWeapon->OutstandingShots.Contains(103) && !BatchWeapon->OutstandingShots.Contains(104));
	BatchWeapon->DisableWeapon();
	TestEqual(TEXT("Permanent equipment disable discards every remaining obsolete estimate"), BatchWeapon->OutstandingShots.Num(), 0);

	// 用真实GAS按下/松开验证输入生命周期：已准入事实由武器排空，不继续占着旧能力挡下一次按下。
	AfpstrueCharacter* ReleasePlayer = nullptr;
	AAIController* ReleaseController = nullptr;
	UfpstrueWeaponComponent* ReleaseWeapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*World, *this, ReleasePlayer, ReleaseController, ReleaseWeapon)) return false;
	UfpstrueHealthComponent* ReleaseASC = ReleasePlayer->GetHealthComponent();
	ReleaseWeapon->Settings.RoundsPerMinute = 1800.0f;
	FGameplayAbilitySpec* ReleaseSpec = ReleaseASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
	if (!TestNotNull(TEXT("Release fixture owns the genuinely granted Fire GA"), ReleaseSpec)) return false;
	const int32 ReleaseInitialAmmo = ReleaseWeapon->GetCurrentAmmo();
	const int32 ReleaseInputId = static_cast<int32>(EFPCombatAbilityInput::Fire);
	ReleaseASC->AbilityLocalInputPressed(ReleaseInputId);
	TestTrue(TEXT("A real trigger press activates the fixture's Fire GA"), ReleaseSpec->IsActive());
	TestEqual(TEXT("The standalone fixture executes its one immediate local-authority round"), ReleaseWeapon->GetCurrentAmmo(), ReleaseInitialAmmo - 1);
	const double ReleaseReceivedAt = World->GetTimeSeconds();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FFPShotRequest Request;
		Request.Avatar = ReleasePlayer;
		Request.EquipmentGeneration = ReleaseWeapon->EquipmentRevision;
		Request.FireSession = 1;
		Request.ShotId = Index + 1;
		Request.ServerFireTime = ReleaseReceivedAt - 0.09 + Index * (60.0 / 1800.0);
		Request.AimDirection = FVector::UpVector;
		TestTrue(TEXT("A release fixture admits both correctly spaced cooldown requests"), ReleaseWeapon->AcceptShotRequest(Request) == EFPShotResult::Queued);
	}
	ReleaseASC->AbilityLocalInputReleased(ReleaseInputId);
	TestFalse(TEXT("Normal trigger release immediately ends the old Fire GA"), ReleaseSpec->IsActive());
	TestEqual(TEXT("Ending the input GA keeps both already-admitted facts"), ReleaseWeapon->PendingNetworkShots.Num(), 2);
	ReleaseASC->AbilityLocalInputPressed(ReleaseInputId);
	TestTrue(TEXT("A rapid new trigger press activates without waiting for the previous FIFO to empty"), ReleaseSpec->IsActive());
	TestEqual(TEXT("Beginning the new GA cannot erase previous admitted requests"), ReleaseWeapon->PendingNetworkShots.Num(), 2);
	ReleaseASC->AbilityLocalInputReleased(ReleaseInputId);
	TestFalse(TEXT("The second normal release also immediately ends its own input GA"), ReleaseSpec->IsActive());
	for (int32 Tick = 0; Tick < 8 && ReleaseWeapon->HasPendingNetworkShots(); ++Tick)
	{
		if (!Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
	}
	TestFalse(TEXT("The unique weapon timer drains old facts without an active Fire GA"), ReleaseWeapon->HasPendingNetworkShots());
	TestEqual(TEXT("Release/repress creates no extra round beyond one local and two admitted requests"), ReleaseWeapon->GetCurrentAmmo(), ReleaseInitialAmmo - 3);
	TestTrue(TEXT("The final old-session request remains Accepted after the new GA starts and stops"), ReleaseWeapon->LastShotResult == EFPShotResult::Accepted);

	// 用固定22Hz世界Tick接收10Hz输入，验证100ms节拍不会逐发变成三帧136ms并最终挤出历史窗。
	// 这是调度与事务回归，不伪称测量了UDP、客户端插值或公网命中精度。
	AfpstrueCharacter* CadencePlayer = nullptr;
	AAIController* CadenceController = nullptr;
	UfpstrueWeaponComponent* CadenceWeapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*World, *this, CadencePlayer, CadenceController, CadenceWeapon)) return false;
	UfpstrueHealthComponent* CadenceASC = CadencePlayer->GetHealthComponent();
	CadenceASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	CadenceWeapon->Settings.RoundsPerMinute = 600.0f;
	CadenceWeapon->BeginAbilityFire(61);
	const int32 CadenceInitialAmmo = CadenceWeapon->GetCurrentAmmo();
	const double CadenceStartedAt = World->GetTimeSeconds();
	constexpr float ServerTickSeconds = 1.0f / 22.0f;
	int32 SubmittedRounds = 0;
	for (int32 Tick = 0; Tick < 100; ++Tick)
	{
		const int32 BeforeTickAmmo = CadenceWeapon->GetCurrentAmmo();
		const double Now = World->GetTimeSeconds();
		if (SubmittedRounds < 20 && Now + 0.0001 >= CadenceStartedAt + SubmittedRounds * 0.1)
		{
			FFPShotRequest Request;
			Request.Avatar = CadencePlayer;
			Request.EquipmentGeneration = CadenceWeapon->EquipmentRevision;
			Request.FireSession = 1;
			Request.ShotId = SubmittedRounds + 1;
			Request.ServerFireTime = CadenceStartedAt + SubmittedRounds * 0.1;
			Request.AimDirection = FVector::UpVector;
			const EFPShotResult Result = CadenceWeapon->AcceptShotRequest(Request);
			TestTrue(TEXT("Every legal 10Hz request remains admitted under a 22Hz server tick"),
				Result == EFPShotResult::Accepted || Result == EFPShotResult::Queued);
			++SubmittedRounds;
		}
		if (!Fixture.TickTestWorld(ServerTickSeconds)) { Fixture.ForwardErrorMessages(this); return false; }
		TestTrue(TEXT("A low-rate server update never catches up by committing multiple queued rounds"),
			BeforeTickAmmo - CadenceWeapon->GetCurrentAmmo() <= 1);
		if (SubmittedRounds == 20 && !CadenceWeapon->HasPendingNetworkShots()) break;
	}
	TestEqual(TEXT("The low-tick fixture actually submitted all twenty rounds"), SubmittedRounds, 20);
	TestEqual(TEXT("Twenty valid rounds execute without accumulated queue expiry"), CadenceWeapon->GetCurrentAmmo(), CadenceInitialAmmo - 20);
	TestEqual(TEXT("The twentieth round reaches the terminal authoritative identity"), CadenceWeapon->LastProcessedShotId, uint32(20));
	TestTrue(TEXT("The terminal cadence verdict is Accepted rather than a hidden age/capacity rejection"), CadenceWeapon->LastShotResult == EFPShotResult::Accepted);
	TestFalse(TEXT("The stable-rate stream leaves no growing admitted backlog"), CadenceWeapon->HasPendingNetworkShots());
	if (!Fixture.TickTestWorld(1.0f)) { Fixture.ForwardErrorMessages(this); return false; }
	CadenceWeapon->Settings.RoundsPerMinute = 1800.0f;
	const double IdleBurstTime = World->GetTimeSeconds();
	const int32 BeforeIdleBurstAmmo = CadenceWeapon->GetCurrentAmmo();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FFPShotRequest Request;
		Request.Avatar = CadencePlayer;
		Request.EquipmentGeneration = CadenceWeapon->EquipmentRevision;
		Request.FireSession = 1;
		Request.ShotId = 21 + Index;
		Request.ServerFireTime = IdleBurstTime - 0.09 + Index * (60.0 / 1800.0);
		Request.AimDirection = FVector::UpVector;
		TestTrue(TEXT("A post-idle arrival burst is still admitted without inventing extra time credit"),
			CadenceWeapon->AcceptShotRequest(Request) == (Index == 0 ? EFPShotResult::Accepted : EFPShotResult::Queued));
	}
	TestEqual(TEXT("One second idle cannot accumulate permission to fire three rounds in the arrival frame"),
		CadenceWeapon->GetCurrentAmmo(), BeforeIdleBurstAmmo - 1);
	for (int32 Tick = 0; Tick < 4 && CadenceWeapon->HasPendingNetworkShots(); ++Tick)
	{
		const int32 BeforeTickAmmo = CadenceWeapon->GetCurrentAmmo();
		if (!Fixture.TickTestWorld(ServerTickSeconds)) { Fixture.ForwardErrorMessages(this); return false; }
		TestTrue(TEXT("Post-idle FIFO remains limited to one authoritative commit per server update"), BeforeTickAmmo - CadenceWeapon->GetCurrentAmmo() <= 1);
	}
	TestEqual(TEXT("The post-idle burst eventually executes all and only its three rounds"), CadenceWeapon->GetCurrentAmmo(), BeforeIdleBurstAmmo - 3);
	TestFalse(TEXT("The post-idle burst fully drains"), CadenceWeapon->HasPendingNetworkShots());
	CadenceWeapon->EndAbilityFire();
	CadenceASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);

	// A/B 是两个独立武器，初次装备代际和开火 nonce 可相同。旧能力数据必须在 ASC 层丢弃，不能 ACK 当前 B。
	const FFPShotRequest OldWeaponRequest = Pending;
	QueueWeapon->EndAbilityFire();
	QueueWeapon->DetachWeapon();
	AActor* ReplacementActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Replacement weapon actor for stale ability data"), ReplacementActor)) return false;
	UfpstrueWeaponComponent* ReplacementWeapon = NewObject<UfpstrueWeaponComponent>(ReplacementActor, NAME_None, RF_Transient);
	ReplacementActor->AddInstanceComponent(ReplacementWeapon);
	ReplacementActor->SetRootComponent(ReplacementWeapon);
	ReplacementWeapon->RegisterComponent();
	if (!TestTrue(TEXT("A different weapon completes its public equipment transaction"), ReplacementWeapon->AttachWeapon(QueuePlayer))) return false;
	ReplacementWeapon->BeginAbilityFire(42);
	FFPShotRequest NewWeaponRequest = OldWeaponRequest;
	NewWeaponRequest.EquipmentGeneration = ReplacementWeapon->EquipmentRevision;
	NewWeaponRequest.ServerFireTime = World->GetTimeSeconds();
	TestTrue(TEXT("Replacement weapon commits its own authoritative request"), ReplacementWeapon->AcceptShotRequest(NewWeaponRequest) == EFPShotResult::Accepted);
	TestTrue(TEXT("The stale-data fixture intentionally reuses generation, session and shot nonce across distinct weapons"),
		OldWeaponRequest.EquipmentGeneration == ReplacementWeapon->EquipmentRevision &&
		OldWeaponRequest.FireSession == ReplacementWeapon->FireSession &&
		OldWeaponRequest.ShotId == ReplacementWeapon->LastProcessedShotId);
	const FFPReplicatedWeaponState BeforeStaleData = ReplacementWeapon->NetworkState;
	const int32 AmmoBeforeStaleData = ReplacementWeapon->GetCurrentAmmo();
	FGameplayAbilityTargetDataHandle OldWeaponData(new FGameplayAbilityTargetData_FPShot());
	static_cast<FGameplayAbilityTargetData_FPShot*>(OldWeaponData.Get(0))->Request = OldWeaponRequest;
	FGameplayAbilitySpec* QueueFireSpec = QueueASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
	if (!TestNotNull(TEXT("Stale weapon data retains its original granted Fire spec"), QueueFireSpec)) return false;
	QueueASC->ServerSetReplicatedTargetData_Implementation(QueueFireSpec->Handle, OldKey, OldWeaponData, FGameplayTag(), FPredictionKey());
	TestFalse(TEXT("Old weapon ability data cannot recreate a cancelled target-data cache"),
		QueueASC->CallReplicatedTargetDataDelegatesIfSet(QueueFireSpec->Handle, OldKey));
	TestEqual(TEXT("Discarding old-key data does not publish another weapon's snapshot"), ReplacementWeapon->NetworkState.Revision, BeforeStaleData.Revision);
	TestEqual(TEXT("Discarding old-key data cannot advance the new weapon's finalized shot"), ReplacementWeapon->NetworkState.LastProcessedShotId, BeforeStaleData.LastProcessedShotId);
	TestTrue(TEXT("Discarding old-key data cannot overwrite the new weapon's accepted result"),
		ReplacementWeapon->NetworkState.ShotResult == BeforeStaleData.ShotResult && ReplacementWeapon->LastShotResult == EFPShotResult::Accepted);
	TestEqual(TEXT("Discarding old-key data cannot spend or restore the new weapon's ammunition"), ReplacementWeapon->GetCurrentAmmo(), AmmoBeforeStaleData);
	ReplacementWeapon->EndAbilityFire();
	QueueASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing, QueueASC->GetTagCount(FpstrueGameplayTags::State_Firing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerSpawnReadinessTest,
	"fpstrue.Multiplayer.Lifecycle.BoundedSpawnRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实碰撞拒绝、定时重试及最终失败；不把握手成功或观战状态当成已拥有游戏Pawn。
bool FFpstrueMultiplayerSpawnReadinessTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, false, AfpstruePvpGameMode::StaticClass())) return false;
	UWorld* World = Fixture.GetTestWorld();
	auto* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	if (!TestNotNull(TEXT("Spawn recovery uses the real PvP mode"), Mode)) return false;
	Mode->EnemyPopulation = 0;
	Mode->bProvideNearbySpawnPickup = false;
	APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(5000, 0, 1000), FRotator::ZeroRotator);
	AActor* Blocker = World->SpawnActor<AActor>();
	auto* Player = World->SpawnActor<AfpstruePvpPlayerController>();
	if (!TestNotNull(TEXT("Spawn recovery player start"), Start) || !TestNotNull(TEXT("Spawn recovery blocker"), Blocker) ||
		!TestNotNull(TEXT("Spawn recovery controller"), Player)) return false;
	// 单个实心大盒可以被FindTeleportSpot合法推出；两堵墙只留下40cm通道，确实容不下玩家胶囊。
	for (const float Side : {-1.0f, 1.0f})
	{
		UBoxComponent* Block = NewObject<UBoxComponent>(Blocker);
		if (Blocker->GetRootComponent() == nullptr) Blocker->SetRootComponent(Block);
		else Block->SetupAttachment(Blocker->GetRootComponent());
		Blocker->AddInstanceComponent(Block);
		Block->SetMobility(EComponentMobility::Static);
		Block->SetBoxExtent(FVector(1000, 10000, 10000));
		Block->SetCollisionObjectType(ECC_WorldStatic);
		Block->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Block->SetCollisionResponseToAllChannels(ECR_Block);
		Block->SetWorldLocation(Start->GetActorLocation() + FVector(Side * 1020, 0, 0));
		Block->RegisterComponent();
	}
	if (!Fixture.TickTestWorld(0.05f)) { Fixture.ForwardErrorMessages(this); return false; }
	const APawn* PawnTemplate = Mode->DefaultPawnClass.GetDefaultObject();
	FVector BlockedLocation = Start->GetActorLocation();
	if (!TestTrue(TEXT("The fixture has real blocking geometry at the selected spawn"),
		World->EncroachingBlockingGeometry(PawnTemplate, BlockedLocation, FRotator::ZeroRotator)) ||
		!TestFalse(TEXT("Native collision adjustment cannot fit a player into the narrow passage"),
		World->FindTeleportSpot(PawnTemplate, BlockedLocation, FRotator::ZeroRotator))) return false;
	Player->StartSpot = Start;
	Player->bConnectionHandshakeComplete = true;
	Mode->RestartPlayer(Player);
	TestNull(TEXT("Blocked spawn does not force an overlapping Pawn"), Player->GetPawn());
	TestFalse(TEXT("Connection admission without a Pawn is not gameplay ready"),
		UfpstrueMultiplayerGameInstance::IsPawnStateReady(nullptr, Player->GetPlayerState<AfpstruePvpPlayerState>()));
	if (!TestTrue(TEXT("One blocked attempt schedules a bounded recovery"), Mode->SpawnRecoveries.Contains(Player))) return false;
	TestEqual(TEXT("Initial spawn consumes exactly one attempt"), Mode->SpawnRecoveries.FindChecked(Player).Attempts, 1);
	const FTimerHandle Retry = Mode->SpawnRecoveries.FindChecked(Player).Timer;
	Mode->FailedToRestartPlayer(Player);
	TestTrue(TEXT("Repeated failure notification keeps the same recovery timer"), Mode->SpawnRecoveries.FindChecked(Player).Timer == Retry);
	Blocker->SetActorEnableCollision(false);
	for (int32 Frame = 0; Frame < 7; ++Frame)
		if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	auto* Pawn = Cast<AfpstrueCharacter>(Player->GetPawn());
	TestTrue(TEXT("Clearing the obstacle recovers through normal RestartPlayer and possession"),
		IsValid(Pawn) && Pawn->GetController() == Player && !Pawn->IsDead());
	TestFalse(TEXT("Successful possession removes the recovery record"), Mode->SpawnRecoveries.Contains(Player));
	TestFalse(TEXT("Successful possession cancels the old retry timer"), World->GetTimerManager().TimerExists(Retry));
	TestTrue(TEXT("The recovered Pawn meets the same ability and state readiness gate"),
		UfpstrueMultiplayerGameInstance::IsPawnStateReady(Pawn, Player->GetPlayerState<AfpstruePvpPlayerState>()));

	// 缺少Pawn类模拟持续无法生成；不改真实资产/CDO，也不需要吞掉重复Spawn碰撞日志。
	Mode->DefaultPawnClass = nullptr;
	auto* Failed = World->SpawnActor<AfpstruePvpPlayerController>();
	if (!TestNotNull(TEXT("Permanently failing spawn controller"), Failed)) return false;
	Mode->RestartPlayer(Failed);
	for (int32 Frame = 0; Frame < 24; ++Frame)
		if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestNull(TEXT("Exhausted recovery cannot report a spawned Pawn"), Failed->GetPawn());
	TestEqual(TEXT("Recovery stops at the initial attempt plus three retries"), Mode->SpawnRecoveries.FindChecked(Failed).Attempts, 4);
	TestFalse(TEXT("Exhausted recovery has no pending timer"), World->GetTimerManager().TimerExists(Mode->SpawnRecoveries.FindChecked(Failed).Timer));
	Mode->RestartPlayer(Failed);
	TestEqual(TEXT("An extra restart request cannot reset an exhausted transaction"), Mode->SpawnRecoveries.FindChecked(Failed).Attempts, 4);
	Mode->PendingConnectionHandshakes.Add(Failed, World->GetTimeSeconds() + 5.0);
	Mode->CompleteConnectionHandshake(Failed, FString());
	TestTrue(TEXT("A later handshake terminates an already exhausted birth instead of accepting Pawn=None"), Failed->bIntentionalLeave);
	TestNull(TEXT("The late handshake does not restart or fabricate the exhausted Pawn"), Failed->GetPawn());
	TestEqual(TEXT("Late admission cannot replenish the exhausted spawn budget"), Mode->SpawnRecoveries.FindChecked(Failed).Attempts, 4);
	Mode->Logout(Failed);
	TestFalse(TEXT("Leaving removes the failed transaction"), Mode->SpawnRecoveries.Contains(Failed));
	auto* Spectator = World->SpawnActor<AfpstruePvpPlayerController>();
	if (!TestNotNull(TEXT("Explicit spectator controller"), Spectator)) return false;
	Spectator->GetPlayerState<AfpstruePvpPlayerState>()->SetIsOnlyASpectator(true);
	Mode->RestartPlayer(Spectator);
	TestFalse(TEXT("Legal spectators do not enter Pawn recovery"), Mode->SpawnRecoveries.Contains(Spectator));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerEnemyActivationTest,
	"fpstrue.Multiplayer.Lifecycle.EnemyBehaviorStartupRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 两种模式的登记入口必须拒绝本次BT启动失败；后续注入上下文不能伪造成功。
bool FFpstrueMultiplayerEnemyActivationTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("has an incompatible Blackboard; activation rejected."), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("No valid enemy BehaviorTree"), EAutomationExpectedErrorFlags::Contains, 2);
	for (UClass* ModeClass : {AfpstrueGameMode::StaticClass(), AfpstruePvpGameMode::StaticClass()})
	{
		FTestWorldWrapper Fixture;
		if (!BeginMultiplayerUnitWorld(Fixture, *this, true, ModeClass)) return false;
		UWorld* World = Fixture.GetTestWorld();
		auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		auto* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
		if (!TestNotNull(TEXT("Activation enemy"), Enemy) || !TestNotNull(TEXT("Activation controller"), Controller)) return false;
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(FTransform::Identity);
		Enemy->SetActorEnableCollision(false);
		auto* TreeProperty = FindFProperty<FObjectPropertyBase>(Controller->GetClass(), TEXT("BehaviorTreeAsset"));
		if (!TestNotNull(TEXT("Activation fixture configures the real behavior-tree property"), TreeProperty)) return false;
		TreeProperty->SetObjectPropertyValue_InContainer(Controller, NewObject<UBehaviorTree>(Controller));
		Controller->Possess(Enemy);
		TestFalse(TEXT("Failed startup is not ready before any behavior tick"), Controller->IsBehaviorReady());
		Controller->InitializeCombatContext(nullptr, nullptr);
		TestFalse(TEXT("Context injection cannot resurrect failed startup"), Controller->IsBehaviorReady());
		if (auto* Pvp = World->GetAuthGameMode<AfpstruePvpGameMode>())
		{
			Pvp->RegisterPveEnemy(Enemy);
			TestEqual(TEXT("PvPvE does not register failed behavior startup"), Pvp->GetAliveEnemyCount(), 0);
		}
		else
		{
			auto* Single = World->GetAuthGameMode<AfpstrueGameMode>();
			Single->RegisterEnemy(Enemy);
			TestEqual(TEXT("Wave mode does not register failed behavior startup"), Single->RegisteredEnemies.Num(), 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerPveAuthorityTest,
	"fpstrue.Multiplayer.PvPvE.ServerCombatAndSingleCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 用真实感知、攻击事务和 GAS 伤害验证端权限；不是把单世界角色切换等同于真实网络复制。
bool FFpstrueMultiplayerPveAuthorityTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("PvPvE authority fixture enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* FirstPlayer = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueCharacter* SecondPlayer = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueSurroundManager* Manager = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("PvPvE enemy controller"), Controller) || !TestNotNull(TEXT("PvPvE first player"), FirstPlayer) ||
		!TestNotNull(TEXT("PvPvE second player"), SecondPlayer) || !TestNotNull(TEXT("PvPvE permit manager"), Manager)) return false;
	for (AfpstrueCharacter* Player : {FirstPlayer, SecondPlayer})
	{
		Player->SetActorEnableCollision(false);
		Player->GetCharacterMovement()->SetComponentTickEnabled(false);
	}
	FirstPlayer->SetActorLocation(FVector(150.0f, 0.0f, 0.0f));
	SecondPlayer->SetActorLocation(FVector(0.0f, 150.0f, 0.0f));
	Controller->Possess(Enemy);
	Manager->SetTargetCharacter(FirstPlayer);
	Controller->InitializeCombatContext(FirstPlayer, Manager);
	UAIPerceptionComponent* Perception = Controller->GetPerceptionComponent();
	if (!TestNotNull(TEXT("PvPvE authority uses the production sight listener"), Perception)) return false;
	FAIStimulus Seen(*GetDefault<UAISense_Sight>(), 1.0f, FirstPlayer->GetActorLocation(), Enemy->GetActorLocation(), FAIStimulus::SensingSucceeded);
	Perception->RegisterStimulus(FirstPlayer, Seen);
	Perception->ProcessStimuli();
	if (!TestTrue(TEXT("Actual sight discovers the injected player"), Controller->GetTargetCharacter() == FirstPlayer)) return false;
	// 暂停IsRunning会变false；这里只禁用夹具Tick，保留真实启动与上下文切换资格。
	if (Controller->GetBrainComponent() != nullptr) Controller->GetBrainComponent()->SetComponentTickEnabled(false);
	UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("PvPvE production melee component"), Combat)) return false;
	// 临时世界不烘焙NavMesh；只提供确定的投影结果，资格和版本仍由生产队列授予。
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	FfpstrueSurroundSlot& Inner = Manager->SurroundSlots[0];
	Inner.bHasProjectedSlotLocation = Inner.bHasProjectedApproachLocation = true;
	Inner.ProjectedSlotLocation = Manager->CachedTargetLocation - FVector(250, 0, 0);
	Inner.ProjectedApproachLocation = Manager->CachedTargetLocation - FVector(120, 0, 0);
	Inner.ProjectedSlotLocation.Z = Inner.ProjectedApproachLocation.Z = Enemy->GetCharacterMovement()->GetActorFeetLocation().Z;
	FFPEnemyTacticalAssignment Assignment;
	if (!TestTrue(TEXT("The server grants a real inner-ring assignment before an attack permit"),
		Manager->GetOrRequestTacticalAssignment(Enemy, Assignment) && Assignment.Role == EFPEnemyTacticalRole::Inner)) return false;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestTrue(TEXT("Server-side enemy acquires an attack permit"), Manager->TryAcquireAttackPermission(Enemy));
	AddInfo(TEXT("Before arrival: ") + DescribeEnemyAttackGate(*Enemy, *Controller, Controller->TacticalAssignment, Controller->CombatMoveAcceptanceRadius));
	TestTrue(TEXT("The controller retains the granted inner lease revision"), Manager->HasInnerSlot(Enemy, Controller->TacticalAssignment.Revision));
	TestFalse(TEXT("Holding an inner lease alone cannot attack before reaching its position"), Combat->TryAttackTarget());
	// 禁碰撞夹具的原生AgentCylinder为0，不享有胶囊半径到达容差；明确移到席位，而非放宽生产接受半径。
	Enemy->SetActorLocation(Assignment.Goal + FVector(0, 0, Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	AddInfo(TEXT("After arrival: ") + DescribeEnemyAttackGate(*Enemy, *Controller, Controller->TacticalAssignment, Controller->CombatMoveAcceptanceRadius));
	TestTrue(TEXT("Arrival satisfies the real tactical position and facing gate"), Controller->IsTacticalAttackPositionReady());
	if (!TestTrue(TEXT("Server-side enemy starts the real attack transaction"), Combat->TryAttackTarget())) return false;
	USkeletalMesh* Blade = NewObject<USkeletalMesh>(Enemy, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(Blade->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("weapontop"), TEXT("weapontop"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("weaponend"), TEXT("weaponend"), 0), FTransform(FVector(50.0f, 0.0f, 0.0f)));
	}
	Blade->CalculateInvRefMatrices();
	Enemy->GetMesh()->bEnableAnimation = false;
	Enemy->GetMesh()->SetSkeletalMesh(Blade);
	Combat->BeginAttackWindow();
	if (!TestTrue(TEXT("Production attack window is active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active)) return false;
	const float FirstHealth = FirstPlayer->GetCurrentHealth();
	int32 DamageFacts = 0;
	const FDelegateHandle ReentryObserver = FirstPlayer->GetHealthComponent()->OnDamageResolved.AddLambda(
		[&](float, AActor*, AController*, const FFPDamageContext&)
		{
			++DamageFacts;
			TestFalse(TEXT("Synchronous damage notification cannot commit the same melee attack twice"), Combat->TryApplyAttackDamage(FirstPlayer));
		});
	TestFalse(TEXT("A melee transaction cannot hit a different player than its captured target"), Combat->TryApplyAttackDamage(SecondPlayer));
	TestTrue(TEXT("Authoritative melee commits through the real damage chain"), Combat->TryApplyAttackDamage(FirstPlayer));
	TestEqual(TEXT("Enemy melee deducts its configured damage once"), FirstPlayer->GetCurrentHealth(), FirstHealth - Combat->AttackDamage);
	Combat->BeginAttackWindow();
	TestFalse(TEXT("A repeated window or sweep cannot damage the player again"), Combat->TryApplyAttackDamage(FirstPlayer));
	TestEqual(TEXT("One melee transaction publishes one damage fact"), DamageFacts, 1);
	FirstPlayer->GetHealthComponent()->OnDamageResolved.Remove(ReentryObserver);
	Combat->ResetCombat();
	// 玩家与 AI 同场不能意外变成合作免伤；双方仍走同一个服务器伤害入口。
	const float SecondHealth = SecondPlayer->GetCurrentHealth();
	UGameplayStatics::ApplyDamage(SecondPlayer, 7.0f, nullptr, FirstPlayer, nullptr);
	UGameplayStatics::ApplyDamage(FirstPlayer, 9.0f, nullptr, SecondPlayer, nullptr);
	TestEqual(TEXT("Player one can still damage player two"), SecondPlayer->GetCurrentHealth(), SecondHealth - 7.0f);
	TestEqual(TEXT("Player two can still damage player one"), FirstPlayer->GetCurrentHealth(), FirstHealth - Combat->AttackDamage - 9.0f);
	// 即使资产或旧通知带有攻击阶段，客户端敌人也不能执行服务器战斗规则。
	Combat->AttackTarget = FirstPlayer;
	Combat->AttackPhase = EFPEnemyAttackPhase::Active;
	Combat->bHitTargetThisAttack = false;
	Enemy->SetRole(ROLE_SimulatedProxy);
	Controller->SetRole(ROLE_SimulatedProxy);
	const float BeforeClientAttempt = FirstPlayer->GetCurrentHealth();
	Controller->InitializeCombatContext(SecondPlayer, Manager);
	TestFalse(TEXT("A client-role controller never accepts AI commands"), Controller->AcceptsCombatCommands());
	TestFalse(TEXT("A client-role enemy cannot start an attack"), Combat->TryAttackTarget());
	TestFalse(TEXT("A stale client animation window cannot apply damage"), Combat->TryApplyAttackDamage(FirstPlayer));
	TestEqual(TEXT("Rejected client melee leaves authoritative health unchanged"), FirstPlayer->GetCurrentHealth(), BeforeClientAttempt);
	Enemy->SetRole(ROLE_Authority);
	Controller->SetRole(ROLE_Authority);
	Combat->ResetCombat();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueNetworkEnemyPresentationTest,
	"fpstrue.Multiplayer.PvPvE.ClientSnapshotIsPresentationOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 回放真实 OnRep，检查攻击/死亡快照可恢复表现但不授予客户端攻击事务或属性写权限。
bool FFpstrueNetworkEnemyPresentationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	AfpstrueEnemyCharacter* Enemy = Fixture.GetTestWorld()->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Client enemy presentation receiver"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->SetRole(ROLE_SimulatedProxy);
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	const FStructProperty* SnapshotProperty = FindFProperty<FStructProperty>(Enemy->GetClass(), TEXT("NetworkPresentation"));
	if (!TestNotNull(TEXT("Enemy current presentation is a reflected snapshot"), SnapshotProperty)) return false;
	TestTrue(TEXT("Enemy current presentation is replicated with its real OnRep"),
		SnapshotProperty->HasAllPropertyFlags(CPF_Net | CPF_RepNotify) &&
		SnapshotProperty->Struct == FFPEnemyNetworkPresentation::StaticStruct());
	const float HealthBefore = Enemy->HealthComponent->GetHealth();
	Enemy->NetworkPresentation.AttackId = 17;
	Enemy->NetworkPresentation.bAttacking = true;
	Enemy->NetworkPresentation.ServerStartTime = Fixture.GetTestWorld()->GetTimeSeconds();
	Enemy->OnRep_NetworkPresentation();
	Enemy->OnRep_NetworkPresentation();
	TestTrue(TEXT("A replicated attack is visible to presentation readers"), Enemy->IsAttacking());
	TestFalse(TEXT("A replicated attack does not start a local authoritative melee transaction"), Enemy->CombatComponent->IsAttacking());
	TestFalse(TEXT("Client presentation cannot start a server attack"), Enemy->CombatComponent->TryAttackTarget());
	Enemy->PublishAttackPresentation(99, nullptr, 1.0f, false);
	TestEqual(TEXT("Client cannot publish a replacement authoritative attack identity"), Enemy->NetworkPresentation.AttackId, uint32(17));
	TestTrue(TEXT("Client publish attempt cannot stop the server attack snapshot"), Enemy->NetworkPresentation.bAttacking);
	Enemy->NetworkPresentation.bAttacking = false;
	Enemy->OnRep_NetworkPresentation();
	TestFalse(TEXT("A later stop snapshot clears the presentation state"), Enemy->IsAttacking());
	UGameplayStatics::ApplyDamage(Enemy, 10.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("Client-side local damage cannot mutate replicated GAS health"), Enemy->HealthComponent->GetHealth(), HealthBefore);
	Enemy->NetworkPresentation.bDead = true;
	Enemy->NetworkPresentation.DeathDirection = FVector::ForwardVector;
	Enemy->NetworkPresentation.DeathLocation = FVector(10.0f, 20.0f, 30.0f);
	Enemy->NetworkPresentation.DeathBone = TEXT("spine_03");
	Enemy->OnRep_NetworkPresentation();
	TestTrue(TEXT("Death presentation works before health attributes arrive"), Enemy->IsDead() && Enemy->bDeathEffectsApplied);
	UWidgetComponent* HealthBar = Enemy->FindComponentByClass<UWidgetComponent>();
	if (!TestNotNull(TEXT("The client corpse retains its native health-bar anchor component"), HealthBar)) return false;
	TestTrue(TEXT("A death snapshot hides the health bar before health attributes arrive"), HealthBar->bHiddenInGame);
	TestNull(TEXT("Death removes the screen widget rather than leaving a stale screen-layer entry"), HealthBar->GetUserWidgetObject());
	Enemy->HealthComponent->OnHealthChanged.Broadcast(HealthBefore);
	TestTrue(TEXT("A late pre-death health notification cannot reveal the corpse's health bar"), HealthBar->bHiddenInGame);
	TestNull(TEXT("A late health notification cannot recreate the removed screen widget"), HealthBar->GetUserWidgetObject());
	TestTrue(TEXT("Ragdoll uses the death context from the same snapshot"),
		Enemy->DeathDamageContext.Location.Equals(FVector(10.0f, 20.0f, 30.0f)) && Enemy->DeathDamageContext.BoneName == TEXT("spine_03"));
	Enemy->OnRep_NetworkPresentation();
	TestFalse(TEXT("Repeated death snapshots never create a melee transaction"), Enemy->CombatComponent->IsAttacking());
	TestEqual(TEXT("Death presentation cannot manufacture authoritative health mutation"), Enemy->HealthComponent->GetHealth(), HealthBefore);
	TestEqual(TEXT("Client corpse is not independently timed for authoritative destruction"), Enemy->GetLifeSpan(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerPveBudgetTest,
	"fpstrue.Multiplayer.PvPvE.WorldSharedNavigationBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 同一帧的两个玩家管理器共享总额；换目标、重置或新增管理器不能给整场寻路预算充值。
bool FFpstrueMultiplayerPveBudgetTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueSurroundManager* First = World->SpawnActor<AfpstrueSurroundManager>();
	AfpstrueSurroundManager* Second = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("First target navigation manager"), First) || !TestNotNull(TEXT("Second target navigation manager"), Second)) return false;
	First->MaxMoveRequestsPerFrame = Second->MaxMoveRequestsPerFrame = 8;
	First->ReservedCombatMoveRequestsPerFrame = Second->ReservedCombatMoveRequestsPerFrame = 2;
	for (int32 Index = 0; Index < 6; ++Index)
		TestTrue(TEXT("Two targets consume the same six ordinary navigation slots"), (Index % 2 == 0 ? First : Second)->TryConsumeMoveRequestBudget(false));
	TestFalse(TEXT("First target cannot consume the combat reserve as ordinary work"), First->TryConsumeMoveRequestBudget(false));
	TestFalse(TEXT("Second target cannot create a second ordinary allowance"), Second->TryConsumeMoveRequestBudget(false));
	TestTrue(TEXT("First target can consume the first combat reserve"), First->TryConsumeMoveRequestBudget(true));
	TestTrue(TEXT("Second target can consume the remaining combat reserve"), Second->TryConsumeMoveRequestBudget(true));
	TestFalse(TEXT("Whole-world combat allowance stops at eight rather than eight per target"), First->TryConsumeMoveRequestBudget(true));
	Second->ResetManager();
	TestFalse(TEXT("Target reset does not replenish the world allowance"), Second->TryConsumeMoveRequestBudget(true));
	AfpstrueSurroundManager* Third = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("New target manager in the exhausted frame"), Third)) return false;
	TestFalse(TEXT("A newly created target manager cannot bypass the frame cap"), Third->TryConsumeMoveRequestBudget(true));
	Third->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("Client-role managers cannot submit navigation requests"), Third->TryConsumeMoveRequestBudget(true));
	FTestWorldWrapper OtherFixture;
	if (!BeginMultiplayerUnitWorld(OtherFixture, *this)) return false;
	AfpstrueSurroundManager* OtherWorldManager = OtherFixture.GetTestWorld()->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("Other world navigation manager"), OtherWorldManager)) return false;
	TestTrue(TEXT("An independent world has its own allowance in the same frame"), OtherWorldManager->TryConsumeMoveRequestBudget(true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerPveTargetLifecycleTest,
	"fpstrue.Multiplayer.PvPvE.TargetDeathDestroyAndResourceRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 两个已登记的保留 Pawn 同样是可受伤目标；视觉缓存选敌，目标退出后释放该目标全部群体资源。
bool FFpstrueMultiplayerPveTargetLifecycleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true, AfpstruePvpGameMode::StaticClass())) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstruePvpGameMode* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	if (!TestNotNull(TEXT("Target lifecycle uses the real PvPvE GameMode"), Mode)) return false;
	Mode->EnemyPopulation = 0;
	AfpstruePvpPlayerController* FirstPlayer = nullptr;
	AfpstruePvpPlayerController* SecondPlayer = nullptr;
	AfpstrueCharacter* FirstPawn = nullptr;
	AfpstrueCharacter* SecondPawn = nullptr;
	UfpstrueWeaponComponent* FirstWeapon = nullptr;
	UfpstrueWeaponComponent* SecondWeapon = nullptr;
	if (!SpawnMultiplayerSessionPlayer(*World, *this, FirstPlayer, FirstPawn, FirstWeapon) ||
		!SpawnMultiplayerSessionPlayer(*World, *this, SecondPlayer, SecondPawn, SecondWeapon)) return false;
	FirstPawn->SetActorLocation(FVector(150.0f, 0.0f, 0.0f));
	SecondPawn->SetActorLocation(FVector(300.0f, 0.0f, 0.0f));
	AfpstruePvpPlayerState* FirstState = FirstPlayer->GetPlayerState<AfpstruePvpPlayerState>();
	AfpstruePvpPlayerState* SecondState = SecondPlayer->GetPlayerState<AfpstruePvpPlayerState>();
	FirstPlayer->UnPossess();
	SecondPlayer->UnPossess();
	FirstPawn->SetPlayerState(FirstState);
	SecondPawn->SetPlayerState(SecondState);
	const auto RegisterRetainedTarget = [&](AfpstrueCharacter* Pawn, AfpstruePvpPlayerState* State, UfpstrueWeaponComponent* Weapon, const TCHAR* Key)
	{
		AfpstruePvpGameMode::FRejoinSlot Slot;
		Slot.Pawn = Pawn;
		Slot.State = State;
		Slot.Weapon = Weapon->GetOwner();
		Slot.bDisconnected = true;
		Slot.ExpiresAt = World->GetTimeSeconds() + 30.0;
		Mode->RejoinSlots.Add(Key, Slot);
	};
	RegisterRetainedTarget(FirstPawn, FirstState, FirstWeapon, TEXT("lifecycle-first-server-slot"));
	RegisterRetainedTarget(SecondPawn, SecondState, SecondWeapon, TEXT("lifecycle-second-server-slot"));
	TestTrue(TEXT("A retained disconnected Pawn remains a combat participant"), Mode->IsCombatParticipant(FirstPawn));
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Target lifecycle enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Target lifecycle controller"), Controller)) return false;
	Controller->Possess(Enemy);
	// 临时世界没有 NetDriver；只固定多人选敌模式，参与资格仍调用真实 Mode 注册表检查。
	Controller->bUsesMultiplayerTargets = true;
	if (Controller->GetBrainComponent() != nullptr) Controller->GetBrainComponent()->SetComponentTickEnabled(false);
	const auto ReportSeen = [&](AfpstrueCharacter* Pawn)
	{
		FAIStimulus Seen(*GetDefault<UAISense_Sight>(), 1.0f, Pawn->GetActorLocation(), Enemy->GetActorLocation(), FAIStimulus::SensingSucceeded);
		Controller->GetPerceptionComponent()->RegisterStimulus(Pawn, Seen);
		Controller->GetPerceptionComponent()->ProcessStimuli();
	};
	ReportSeen(FirstPawn);
	ReportSeen(SecondPawn);
	AfpstrueCharacter* UnregisteredPawn = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("An unregistered closer Pawn"), UnregisteredPawn)) return false;
	UnregisteredPawn->SetActorEnableCollision(false);
	UnregisteredPawn->GetCharacterMovement()->SetComponentTickEnabled(false);
	UnregisteredPawn->SetActorLocation(FVector(50.0f, 0.0f, 0.0f));
	ReportSeen(UnregisteredPawn);
	TestFalse(TEXT("An arbitrary Pawn does not become an admitted combat participant"), Mode->IsCombatParticipant(UnregisteredPawn));
	if (!TestTrue(TEXT("A second visible player does not steal a valid stable target"), Controller->GetTargetCharacter() == FirstPawn)) return false;
	AfpstrueSurroundManager* FirstManager = Controller->GetSurroundManager();
	if (!TestNotNull(TEXT("The first player has its own surround manager"), FirstManager)) return false;
	TestTrue(TEXT("Repeated requests reuse that player's manager"), Mode->GetOrCreateSurroundManager(FirstPawn) == FirstManager);
	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	// 导航投影另由地图烟测覆盖；通过真实队列取得席位和版本，观察换目标是否对称撤销。
	const auto GrantInner = [&](AfpstrueSurroundManager* Manager)
	{
		Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
		FfpstrueSurroundSlot& Inner = Manager->SurroundSlots[0];
		Inner.bHasProjectedSlotLocation = Inner.bHasProjectedApproachLocation = true;
		Inner.ProjectedSlotLocation = Manager->CachedTargetLocation - FVector(250, 0, 0);
		Inner.ProjectedApproachLocation = Manager->CachedTargetLocation - FVector(120, 0, 0);
		Inner.ProjectedSlotLocation.Z = Inner.ProjectedApproachLocation.Z = Enemy->GetCharacterMovement()->GetActorFeetLocation().Z;
		FFPEnemyTacticalAssignment Assignment;
		return Manager->GetOrRequestTacticalAssignment(Enemy, Assignment) && Assignment.Role == EFPEnemyTacticalRole::Inner;
	};
	if (!TestTrue(TEXT("Target lifecycle starts with a queue-granted inner seat"), GrantInner(FirstManager))) return false;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	AfpstrueSurroundManager::FCoverRequest Cover;
	Cover.State = EFPCoverQueryResult::Found;
	FirstManager->CoverRequests.Add(EnemyKey, Cover);
	TestTrue(TEXT("Target lifecycle starts with a real granted attack permit"), FirstManager->TryAcquireAttackPermission(Enemy));
	AddInfo(TEXT("Lifecycle before arrival: ") + DescribeEnemyAttackGate(*Enemy, *Controller, Controller->TacticalAssignment, Controller->CombatMoveAcceptanceRadius));
	TestFalse(TEXT("Target lifecycle cannot bypass arrival merely by holding the permit"), Enemy->GetCombatComponent()->TryAttackTarget());
	Enemy->SetActorLocation(Controller->TacticalAssignment.Goal + FVector(0, 0, Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	AddInfo(TEXT("Lifecycle after arrival: ") + DescribeEnemyAttackGate(*Enemy, *Controller, Controller->TacticalAssignment, Controller->CombatMoveAcceptanceRadius));
	TestTrue(TEXT("The lifecycle fixture reaches its genuine tactical assignment"), Controller->IsTacticalAttackPositionReady());
	TestTrue(TEXT("Target lifecycle starts a real combat transaction"), Enemy->GetCombatComponent()->TryAttackTarget());
	UGameplayStatics::ApplyDamage(FirstPawn, 1000.0f, nullptr, Enemy, nullptr);
	TestTrue(TEXT("The first retained player dies through the actual damage chain"), FirstPawn->IsDead());
	TestTrue(TEXT("Death selects the admitted perceived player rather than a closer unregistered Pawn"), Controller->GetTargetCharacter() == SecondPawn);
	TestFalse(TEXT("Changing targets cancels the captured old attack"), Enemy->GetCombatComponent()->IsAttacking());
	TestFalse(TEXT("Changing targets releases the old attack permit"), FirstManager->ActiveAttackers.Contains(EnemyKey));
	TestFalse(TEXT("Changing targets releases the old surround slot"), FirstManager->EnemyToSlot.Contains(EnemyKey));
	TestFalse(TEXT("Changing targets releases the old cover reservation"), FirstManager->CoverRequests.Contains(EnemyKey));
	AfpstrueSurroundManager* SecondManager = Controller->GetSurroundManager();
	if (!TestNotNull(TEXT("The replacement target has a manager"), SecondManager)) return false;
	TestTrue(TEXT("Two players do not share the same occupied slot registry"), SecondManager != FirstManager);
	if (!TestTrue(TEXT("The replacement target grants a new inner seat"), GrantInner(SecondManager))) return false;
	TestTrue(TEXT("The replacement target grants its own permit"), SecondManager->TryAcquireAttackPermission(Enemy));
	SecondPawn->Destroy();
	TestNull(TEXT("EndPlay does not reselect the same Actor during its destruction callback"), Controller->GetTargetCharacter());
	TestNull(TEXT("No visible living target leaves no active target manager"), Controller->GetSurroundManager());
	TestFalse(TEXT("Destroying the replacement target returns its attack permit"), SecondManager->ActiveAttackers.Contains(EnemyKey));
	TestFalse(TEXT("No remaining target cannot start another attack"), Enemy->GetCombatComponent()->TryAttackTarget());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerResumeSessionTest,
	"fpstrue.Multiplayer.Reconnect.ServerRetentionAndSingleUseClaim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 单世界规则测试从“服务器已签发会话”前置开始；真实持有者 RPC、硬断与磁盘票据由多进程烟测覆盖。
bool FFpstrueMultiplayerResumeSessionTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true, AfpstruePvpGameMode::StaticClass())) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstruePvpGameMode* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	if (!TestNotNull(TEXT("Resume rules use the real PvPvE GameMode"), Mode)) return false;
	Mode->EnemyPopulation = 0;
	AfpstruePvpPlayerController* Original = nullptr;
	AfpstrueCharacter* Pawn = nullptr;
	UfpstrueWeaponComponent* Weapon = nullptr;
	if (!SpawnMultiplayerSessionPlayer(*World, *this, Original, Pawn, Weapon)) return false;
	const UFunction* SubmitToken = Original->FindFunction(TEXT("ServerSubmitConnectionToken"));
	const UFunction* AcceptToken = Original->FindFunction(TEXT("ClientConnectionAccepted"));
	if (!TestNotNull(TEXT("Resume token uses the owning Controller RPC"), SubmitToken) ||
		!TestNotNull(TEXT("Resume acceptance uses an owner-only client RPC"), AcceptToken)) return false;
	TestTrue(TEXT("Resume submission is a reliable server RPC, not a multicast"),
		SubmitToken->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable) && !SubmitToken->HasAnyFunctionFlags(FUNC_NetMulticast));
	TestTrue(TEXT("Resume credentials return through a reliable owner client RPC"),
		AcceptToken->HasAllFunctionFlags(FUNC_Net | FUNC_NetClient | FUNC_NetReliable) && !AcceptToken->HasAnyFunctionFlags(FUNC_NetMulticast));
	AfpstruePvpPlayerState* State = Original->GetPlayerState<AfpstruePvpPlayerState>();
	Pawn->GetHealthComponent()->SetMaxHealthAndReset(200.0f);
	State->AddKill();
	State->AddKill();
	State->AddDeath();
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	const FString InitialToken = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	AfpstruePvpGameMode::FRejoinSlot Initial;
	Initial.Pawn = Pawn;
	Initial.State = State;
	Initial.Weapon = Weapon->GetOwner();
	Initial.Controller = Original;
	Mode->RejoinSlots.Add(InitialToken, Initial);
	Mode->PlayerWeapons.Add(Original, Weapon->GetOwner());
	Original->AssignedRejoinToken = InitialToken;
	Original->bConnectionHandshakeComplete = true;
	const auto NewClaim = [&]()
	{
		AfpstruePvpPlayerController* Player = World->SpawnActor<AfpstruePvpPlayerController>();
		if (Player != nullptr) Mode->PendingConnectionHandshakes.Add(Player, World->GetTimeSeconds() + 5.0);
		return Player;
	};
	AfpstruePvpPlayerController* ActiveClaim = NewClaim();
	if (!TestNotNull(TEXT("A competing active-session claimant"), ActiveClaim)) return false;
	Mode->CompleteConnectionHandshake(ActiveClaim, InitialToken);
	TestFalse(TEXT("Possession of an active token cannot steal the online participant"), ActiveClaim->bConnectionHandshakeComplete);
	TestTrue(TEXT("Rejected active claim preserves the original controller and Pawn"), Original->GetPawn() == Pawn &&
		Mode->RejoinSlots.FindChecked(InitialToken).Controller.Get() == Original);
	TestTrue(TEXT("Production disconnect preservation accepts the established session"), Mode->PreserveDisconnectedPlayer(Original));
	TestTrue(TEXT("Disconnect stores the server-configured cold-load budget, bounded to at most two minutes"),
		FMath::IsNearlyEqual(Mode->RejoinSlots.FindChecked(InitialToken).ExpiresAt - World->GetTimeSeconds(),
			double(FMath::Clamp(Mode->ReconnectGraceSeconds, 5.0f, 120.0f)), 0.01)
		&& Mode->RejoinSlots.FindChecked(InitialToken).ExpiresAt <= World->GetTimeSeconds() + 120.01);
	TestNull(TEXT("Disconnected Pawn is no longer controlled by its old connection"), Pawn->GetController());
	TestTrue(TEXT("The original server Pawn remains a combat participant during the grace period"), Mode->IsCombatParticipant(Pawn));
	TestTrue(TEXT("The single disconnected field also records server retention"), State->IsDisconnected());
	State->OnDeactivated();
	TestTrue(TEXT("Deactivation preserves the retained PlayerState rather than duplicating or destroying it"),
		IsValid(State) && !State->IsActorBeingDestroyed());
	TestNull(TEXT("Retained deactivation drops the obsolete connection owner"), State->GetOwner());
	TestTrue(TEXT("The retained PlayerState remains in the native GameState PlayerArray"),
		World->GetGameState()->PlayerArray.Contains(State));
	Original->Destroy();
	UGameplayStatics::ApplyDamage(Pawn, 25.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("A retained disconnected Pawn is not invulnerable"), Pawn->GetCurrentHealth(), 175.0f);
	AfpstruePvpPlayerController* Restored = NewClaim();
	if (!TestNotNull(TEXT("The returning controller"), Restored)) return false;
	Mode->CompleteConnectionHandshake(Restored, InitialToken);
	if (!TestTrue(TEXT("A valid disconnected token completes its production claim"), Restored->bConnectionHandshakeComplete)) return false;
	// 出生补给有自己的有界登记，不能与已经装备/死亡掉落的武器共用强制销毁逻辑。
	AActor* UnclaimedSupply = World->SpawnActor<AfpstrueTemplateWeapon>(FVector(50000, 50000, 1000), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Spawn-supply cleanup uses a real unowned pickup"), UnclaimedSupply)) return false;
	Mode->SpawnPickups.Add(Restored, UnclaimedSupply);
	Mode->ReleaseUnclaimedSpawnPickup(Restored);
	TestTrue(TEXT("Leaving or respawning removes an unclaimed supply"), UnclaimedSupply->IsActorBeingDestroyed());
	TestFalse(TEXT("Supply cleanup consumes its controller record once"), Mode->SpawnPickups.Contains(Restored));
	Mode->ReleaseUnclaimedSpawnPickup(Restored);
	Mode->SpawnPickups.Add(Restored, Weapon->GetOwner());
	Mode->ReleaseUnclaimedSpawnPickup(Restored);
	TestTrue(TEXT("A claimed supply is not destroyed by its original spawn record"), IsValid(Weapon->GetOwner()) && !Weapon->GetOwner()->IsActorBeingDestroyed());
	TestTrue(TEXT("Alive reconnect reuses the exact server Pawn"), Restored->GetPawn() == Pawn);
	TestTrue(TEXT("Reconnect reuses the exact server PlayerState"), Restored->GetPlayerState<AfpstruePvpPlayerState>() == State);
	TestFalse(TEXT("Successful reclaim clears the same disconnected/retention field"), State->IsDisconnected());
	TestEqual(TEXT("Reconnect does not roll back offline damage"), Pawn->GetCurrentHealth(), 175.0f);
	TestEqual(TEXT("Reconnect preserves authoritative ammunition"), Pawn->GetEquippedWeaponComponent()->GetCurrentAmmo(), AmmoBefore);
	TestEqual(TEXT("Reconnect preserves authoritative kills"), State->GetKills(), 2);
	TestEqual(TEXT("Reconnect preserves authoritative deaths"), State->GetDeaths(), 1);
	TestFalse(TEXT("Successful claim consumes the old token"), Mode->RejoinSlots.Contains(InitialToken));
	const FString RotatedToken = Restored->AssignedRejoinToken;
	TestTrue(TEXT("Successful claim rotates a nonempty server-generated token"), RotatedToken.Len() == 64 && RotatedToken != InitialToken);
	AfpstruePvpPlayerController* Replay = NewClaim();
	if (!TestNotNull(TEXT("A replay of the consumed token"), Replay)) return false;
	Mode->CompleteConnectionHandshake(Replay, InitialToken);
	TestFalse(TEXT("Consumed credentials cannot be claimed again"), Replay->bConnectionHandshakeComplete);
	TestTrue(TEXT("Rejected replay cannot replace the restored owner"), Pawn->GetController() == Restored);
	Mode->ReconnectGraceSeconds = 1000.0f;
	TestTrue(TEXT("The same live participant can be retained on a later disconnect"), Mode->PreserveDisconnectedPlayer(Restored));
	TestTrue(TEXT("An excessive configured retention budget is capped at two minutes by production preservation"),
		FMath::IsNearlyEqual(Mode->RejoinSlots.FindChecked(RotatedToken).ExpiresAt - World->GetTimeSeconds(), 120.0, 0.01));
	Restored->Destroy();
	UGameplayStatics::ApplyDamage(Pawn, 1000.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Offline authoritative damage can kill the retained Pawn"), Pawn->IsDead());
	Mode->HandleRetainedPawnEliminated(Pawn, nullptr);
	const int32 DeathsAfterOfflineKill = State->GetDeaths();
	TestEqual(TEXT("Offline death is recorded exactly once"), DeathsAfterOfflineKill, 2);
	Mode->HandleRetainedPawnEliminated(Pawn, nullptr);
	TestEqual(TEXT("Repeated retained-death callbacks do not duplicate the death"), State->GetDeaths(), DeathsAfterOfflineKill);
	TestEqual(TEXT("The server retention record never rewrites dead health"), Pawn->GetCurrentHealth(), 0.0f);
	TestTrue(TEXT("The retention record preserves the death fact for the next claim"), Mode->RejoinSlots.FindChecked(RotatedToken).bDeathRecorded);
	Mode->RejoinSlots.FindChecked(RotatedToken).ExpiresAt = World->GetTimeSeconds() - 1.0;
	AfpstruePvpPlayerController* ExpiredClaim = NewClaim();
	if (!TestNotNull(TEXT("An expired-token claimant"), ExpiredClaim)) return false;
	Mode->CompleteConnectionHandshake(ExpiredClaim, RotatedToken);
	TestFalse(TEXT("Expired credentials cannot revive a disconnected player"), ExpiredClaim->bConnectionHandshakeComplete);
	TestNull(TEXT("An expired claim is not given a replacement Pawn"), ExpiredClaim->GetPawn());
	Mode->SweepRejoinSlots();
	TestFalse(TEXT("Expiry clears the retained token record"), Mode->RejoinSlots.Contains(RotatedToken));
	TestFalse(TEXT("Expiry releases the retained server Pawn"), IsValid(Pawn) && !Pawn->IsActorBeingDestroyed());
	TestFalse(TEXT("Expiry releases the retained PlayerState"), IsValid(State) && !State->IsActorBeingDestroyed());
	TestFalse(TEXT("Expiry releases the retained server weapon"), IsValid(Weapon->GetOwner()) && !Weapon->GetOwner()->IsActorBeingDestroyed());
	AfpstruePvpPlayerController* TimedOut = NewClaim();
	if (!TestNotNull(TEXT("An incomplete owner handshake"), TimedOut)) return false;
	Mode->PendingConnectionHandshakes[TimedOut] = World->GetTimeSeconds() - 1.0;
	Mode->SweepRejoinSlots();
	TestFalse(TEXT("A timed-out owner handshake is no longer pending"), Mode->PendingConnectionHandshakes.Contains(TimedOut));
	TestFalse(TEXT("A timed-out owner handshake cannot remain an active player"), IsValid(TimedOut) && !TimedOut->IsActorBeingDestroyed());

	// 正常出生空手；仍使用原持枪夹具准备输入/挂点，随后通过真实销毁路径卸枪，不改旧规则夹具。
	AfpstruePvpPlayerController* UnarmedOriginal = nullptr;
	AfpstrueCharacter* UnarmedPawn = nullptr;
	UfpstrueWeaponComponent* InitialWeapon = nullptr;
	if (!SpawnMultiplayerSessionPlayer(*World, *this, UnarmedOriginal, UnarmedPawn, InitialWeapon)) return false;
	InitialWeapon->GetOwner()->Destroy();
#if !UE_BUILD_SHIPPING
	Mode->bSmokeTest = false;
#endif
	Mode->FinishRestartPlayer(UnarmedOriginal, UnarmedPawn->GetActorRotation());
	TestFalse(TEXT("Normal initial spawn and respawn finish do not automatically grant a weapon"), UnarmedPawn->HasEquippedWeapon());
	TestTrue(TEXT("An unarmed local viewmodel remains hidden until pickup"), UnarmedPawn->GetMesh1P()->bHiddenInGame);
	auto* UnarmedState = UnarmedOriginal->GetPlayerState<AfpstruePvpPlayerState>();
	if (!TestNotNull(TEXT("The unarmed participant has its authoritative PlayerState"), UnarmedState)) return false;
	TestTrue(TEXT("Connection state accepts a live unarmed Pawn with health and all granted abilities"),
		UfpstrueMultiplayerGameInstance::IsPawnStateReady(UnarmedPawn, UnarmedState));
	TestFalse(TEXT("Connection state still rejects a missing PlayerState"),
		UfpstrueMultiplayerGameInstance::IsPawnStateReady(UnarmedPawn, nullptr));
	const FString UnarmedToken = TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	AfpstruePvpGameMode::FRejoinSlot UnarmedSlot;
	UnarmedSlot.Pawn = UnarmedPawn;
	UnarmedSlot.State = UnarmedState;
	UnarmedSlot.Controller = UnarmedOriginal;
	Mode->RejoinSlots.Add(UnarmedToken, UnarmedSlot);
	UnarmedOriginal->AssignedRejoinToken = UnarmedToken;
	UnarmedOriginal->bConnectionHandshakeComplete = true;
	Mode->ReconnectGraceSeconds = 0.0f;
	TestTrue(TEXT("An unarmed participant can enter the reconnect grace period"), Mode->PreserveDisconnectedPlayer(UnarmedOriginal));
	TestTrue(TEXT("A nonpositive configured retention budget still has the five-second bounded minimum"),
		FMath::IsNearlyEqual(Mode->RejoinSlots.FindChecked(UnarmedToken).ExpiresAt - World->GetTimeSeconds(), 5.0, 0.01));
	TestFalse(TEXT("Unarmed retention does not manufacture a weapon record"), Mode->RejoinSlots.FindChecked(UnarmedToken).Weapon.IsValid());
	UnarmedOriginal->Destroy();
	UGameplayStatics::ApplyDamage(UnarmedPawn, 13.0f, nullptr, nullptr, nullptr);
	const float UnarmedHealth = UnarmedPawn->GetCurrentHealth();
	AfpstruePvpPlayerController* UnarmedRestored = NewClaim();
	if (!TestNotNull(TEXT("The unarmed returning controller"), UnarmedRestored)) return false;
	Mode->CompleteConnectionHandshake(UnarmedRestored, UnarmedToken);
	TestTrue(TEXT("An unarmed reconnect preserves the exact living Pawn and PlayerState"),
		UnarmedRestored->GetPawn() == UnarmedPawn && UnarmedRestored->GetPlayerState<AfpstruePvpPlayerState>() == UnarmedState);
	TestEqual(TEXT("Unarmed reconnect preserves damage taken while disconnected"), UnarmedPawn->GetCurrentHealth(), UnarmedHealth);
	TestFalse(TEXT("Unarmed reconnect stays unarmed rather than silently granting a rifle"), UnarmedPawn->HasEquippedWeapon());
	TestTrue(TEXT("Restored unarmed state remains eligible for connection readiness"),
		UfpstrueMultiplayerGameInstance::IsPawnStateReady(UnarmedPawn, UnarmedState));

	// 手动广播真实组件重叠入口只测试规则，不冒充物理碰撞或跨进程复制；拾取不写默认发枪的记录表。
	AActor* PickedUpActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("The world pickup weapon actor"), PickedUpActor)) return false;
	UfpstrueWeaponComponent* PickedUpWeapon = NewObject<UfpstrueWeaponComponent>(PickedUpActor);
	PickedUpActor->AddInstanceComponent(PickedUpWeapon);
	PickedUpActor->SetRootComponent(PickedUpWeapon);
	PickedUpWeapon->RegisterComponent();
	PickedUpActor->SetActorLocation(FVector(20000.0, 0.0, 1000.0));
	UfpstruePickUpComponent* Pickup = NewObject<UfpstruePickUpComponent>(PickedUpActor);
	PickedUpActor->AddInstanceComponent(Pickup);
	Pickup->SetupAttachment(PickedUpWeapon);
	Pickup->RegisterComponent();
	Pickup->OnComponentBeginOverlap.Broadcast(Pickup, UnarmedPawn, UnarmedPawn->GetCapsuleComponent(), 0, false, FHitResult());
	if (!TestTrue(TEXT("The authority overlap uses the complete public equipment transaction"), UnarmedPawn->GetEquippedWeaponComponent() == PickedUpWeapon)) return false;
	TestFalse(TEXT("A scene pickup does not depend on the default-weapon registry"), Mode->PlayerWeapons.Contains(UnarmedRestored));
	const int32 PickedUpAmmo = PickedUpWeapon->GetCurrentAmmo();
	const FString PickupToken = UnarmedRestored->AssignedRejoinToken;
	TestTrue(TEXT("The picked-up weapon holder can disconnect"), Mode->PreserveDisconnectedPlayer(UnarmedRestored));
	TestTrue(TEXT("Retention captures the Pawn's actual scene-picked weapon without a default record"),
		Mode->RejoinSlots.FindChecked(PickupToken).Weapon.Get() == PickedUpActor);
	UnarmedRestored->Destroy();
	AfpstruePvpPlayerController* PickupRestored = NewClaim();
	if (!TestNotNull(TEXT("The returning scene-pickup holder"), PickupRestored)) return false;
	Mode->CompleteConnectionHandshake(PickupRestored, PickupToken);
	TestTrue(TEXT("Scene pickup reconnect retains the exact Pawn and equipped weapon"),
		PickupRestored->GetPawn() == UnarmedPawn && UnarmedPawn->GetEquippedWeaponComponent() == PickedUpWeapon);
	TestEqual(TEXT("Scene pickup reconnect retains its ammunition"), PickedUpWeapon->GetCurrentAmmo(), PickedUpAmmo);
	Mode->PlayerWeapons.Remove(PickupRestored);
	PickupRestored->bIntentionalLeave = true;
	PickupRestored->PawnLeavingGame();
	TestFalse(TEXT("Intentional leave cleans a scene-picked actor before Pawn destruction, without a default-weapon record"),
		IsValid(PickedUpActor) && !PickedUpActor->IsActorBeingDestroyed());
	TestFalse(TEXT("Intentional leave removes the old Pawn instead of retaining an abandoned armed actor"),
		IsValid(UnarmedPawn) && !UnarmedPawn->IsActorBeingDestroyed());

	// 真实模板死亡掉枪与会话清理共用：重连重生不能提前回收仍在独立八秒展示期的枪。
	FActorSpawnParameters TemplateSpawn;
	TemplateSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* DropPawn = World->SpawnActor<AfpstrueCharacter>(Mode->DefaultPawnClass,
		FTransform(FVector(30000.0f, 0.0f, 1000.0f)), TemplateSpawn);
	AfpstruePvpPlayerController* DropController = World->SpawnActor<AfpstruePvpPlayerController>();
	AfpstrueTemplateWeapon* DropRifle = World->SpawnActor<AfpstrueTemplateWeapon>(AfpstrueTemplateWeapon::StaticClass(),
		FTransform(FVector(40000.0f, 0.0f, 1000.0f)), TemplateSpawn);
	APlayerStart* RejoinStart = World->SpawnActor<APlayerStart>(FVector(33000.0f, 0.0f, 1000.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Death reconnect has the currently selected real template Pawn"), DropPawn) ||
		!TestNotNull(TEXT("Death reconnect has its project Controller"), DropController) ||
		!TestNotNull(TEXT("Death reconnect has the actual template rifle"), DropRifle) ||
		!TestNotNull(TEXT("Death reconnect has a real player start for production RestartPlayer"), RejoinStart)) return false;
	DropPawn->GetCharacterMovement()->SetComponentTickEnabled(false);
	DropController->Possess(DropPawn);
	UfpstrueWeaponComponent* DropWeapon = DropRifle->FindComponentByClass<UfpstrueWeaponComponent>();
	AfpstruePvpPlayerState* DropState = DropController->GetPlayerState<AfpstruePvpPlayerState>();
	if (!TestNotNull(TEXT("Death reconnect owns its authoritative PlayerState"), DropState) ||
		!TestNotNull(TEXT("Death reconnect retains the production weapon component"), DropWeapon) ||
		!TestTrue(TEXT("Death reconnect uses native presentation rather than the legacy Disable-only fixture"), DropPawn->UsesNativePresentation()) ||
		!TestTrue(TEXT("Death reconnect equips through the public transaction"), DropWeapon->AttachWeapon(DropPawn))) return false;
	const FString DropToken = TEXT("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
	AfpstruePvpGameMode::FRejoinSlot DropSlot;
	DropSlot.Pawn = DropPawn;
	DropSlot.State = DropState;
	DropSlot.Weapon = DropRifle;
	DropSlot.Controller = DropController;
	Mode->RejoinSlots.Add(DropToken, DropSlot);
	DropController->AssignedRejoinToken = DropToken;
	DropController->bConnectionHandshakeComplete = true;
	if (!TestTrue(TEXT("The real template Pawn enters the normal disconnect retention path"), Mode->PreserveDisconnectedPlayer(DropController))) return false;
	DropController->Destroy();
	UGameplayStatics::ApplyDamage(DropPawn, 1000.0f, nullptr, nullptr, nullptr);
	if (!TestTrue(TEXT("Offline template death starts its real terminal physics drop"), DropPawn->IsDead() && DropWeapon->IsDroppedOnDeath() && DropWeapon->IsSimulatingPhysics())) return false;
	// 临时世界是 Standalone；与上方保留 Pawn 夹具相同，显式调用服务器规则入口，不冒充网络回调验收。
	Mode->HandleRetainedPawnEliminated(DropPawn, nullptr);
	const float DropLifetime = DropRifle->GetLifeSpan();
	TestTrue(TEXT("The dead template retains its weapon until the session cleanup boundary"), DropPawn->GetEquippedWeaponComponent() == DropWeapon);
	AfpstruePvpPlayerController* DropClaim = NewClaim();
	if (!TestNotNull(TEXT("The returning dead template player's new controller"), DropClaim)) return false;
	Mode->CompleteConnectionHandshake(DropClaim, DropToken);
	AfpstrueCharacter* RebornPawn = Cast<AfpstrueCharacter>(DropClaim->GetPawn());
	if (!TestTrue(TEXT("Returning after offline death creates a new living Pawn through RestartPlayer"),
		DropClaim->bConnectionHandshakeComplete && IsValid(RebornPawn) && RebornPawn != DropPawn && !RebornPawn->IsDead())) return false;
	RebornPawn->GetCharacterMovement()->SetComponentTickEnabled(false);
	TestTrue(TEXT("Death reconnect preserves the current PlayerState and recorded death"), DropClaim->GetPlayerState<AfpstruePvpPlayerState>() == DropState && DropState->GetDeaths() == 1);
	TestFalse(TEXT("Death reconnect destroys its old Pawn rather than reviving it"), IsValid(DropPawn) && !DropPawn->IsActorBeingDestroyed());
	TestNull(TEXT("Death cleanup releases the old equipped slot"), DropPawn->GetEquippedWeaponComponent());
	TestNull(TEXT("Terminal detach releases the obsolete reverse holder reference"), DropWeapon->GetOwningCharacter());
	TestTrue(TEXT("Death reconnect leaves the independent drop visible and simulating"),
		IsValid(DropRifle) && !DropRifle->IsActorBeingDestroyed() && DropWeapon->IsSimulatingPhysics() && !DropWeapon->bHiddenInGame);
	TestTrue(TEXT("Death reconnect neither cancels nor restarts the original drop lifetime"), FMath::IsNearlyEqual(DropRifle->GetLifeSpan(), DropLifetime, 0.01f));
	TestFalse(TEXT("The returning player's normal respawn is empty-handed"), RebornPawn->HasEquippedWeapon());
	for (int32 Frame = 0; Frame < 32; ++Frame)
		if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestTrue(TEXT("The old gun survives beyond the three-second Pawn respawn cleanup delay"), IsValid(DropRifle) && !DropRifle->IsActorBeingDestroyed());
	for (int32 Frame = 0; Frame < 51; ++Frame)
		if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestFalse(TEXT("The independent drop is still reclaimed at its eight-second deadline"), IsValid(DropRifle) && !DropRifle->IsActorBeingDestroyed());

	// 旧保留记录的枪已被其他活玩家持有时，只删旧记录，不能销毁别人正在使用的新装备。
	AfpstrueTemplateWeapon* LiveRifle = World->SpawnActor<AfpstrueTemplateWeapon>(AfpstrueTemplateWeapon::StaticClass(),
		FTransform(FVector(40000.0f, 0.0f, 1000.0f)), TemplateSpawn);
	if (!TestNotNull(TEXT("Stale retention cleanup has a distinct living player's rifle"), LiveRifle)) return false;
	UfpstrueWeaponComponent* LiveWeapon = LiveRifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("The live rifle has its production component"), LiveWeapon) ||
		!TestTrue(TEXT("The fresh Pawn owns the live rifle before a stale slot expires"), LiveWeapon->AttachWeapon(RebornPawn))) return false;
	AfpstruePvpGameMode::FRejoinSlot StaleSlot;
	StaleSlot.Weapon = LiveRifle;
	StaleSlot.bDisconnected = true;
	StaleSlot.ExpiresAt = World->GetTimeSeconds() - 1.0;
	Mode->RejoinSlots.Add(TEXT("stale-weapon-ownership-cleanup"), StaleSlot);
	Mode->SweepRejoinSlots();
	TestTrue(TEXT("Expiring an obsolete slot cannot destroy another live Pawn's actual rifle"),
		IsValid(LiveRifle) && !LiveRifle->IsActorBeingDestroyed() && RebornPawn->GetEquippedWeaponComponent() == LiveWeapon);
	AfpstruePvpPlayerState* ReleasedState = World->SpawnActor<AfpstruePvpPlayerState>();
	if (!TestNotNull(TEXT("Retention-reset lifecycle fixture PlayerState"), ReleasedState)) return false;
	ReleasedState->SetRetainedForReconnect(true);
	ReleasedState->SetRetainedForReconnect(false);
	ReleasedState->OnDeactivated();
	TestTrue(TEXT("Clearing retention restores native deactivation destruction"), ReleasedState->IsActorBeingDestroyed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerConnectionRevisionTest,
	"fpstrue.Multiplayer.Reconnect.CancelInvalidatesOldTravelAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 不 Init、不建立 World、不读取真实票据；取消链与缓存写入只使用本用例自己的合成数据。
bool FFpstrueMultiplayerConnectionRevisionTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UfpstrueMultiplayerGameInstance> Instance(NewObject<UfpstrueMultiplayerGameInstance>());
	if (!TestNotNull(TEXT("Connection lifecycle fixture GameInstance"), Instance.Get())) return false;
	// 只暂改原生设置 CDO 的反射值，作用域退出即恢复；不写配置、不触发地图旅行。
	UGameMapsSettings* Maps = GetMutableDefault<UGameMapsSettings>();
	const FStructProperty* ClientMapProperty = FindFProperty<FStructProperty>(Maps->GetClass(), TEXT("GameDefaultMap"));
	const FStructProperty* ServerMapProperty = FindFProperty<FStructProperty>(Maps->GetClass(), TEXT("ServerDefaultMap"));
	if (!TestNotNull(TEXT("The native client map setting"), ClientMapProperty) ||
		!TestNotNull(TEXT("The distinct native server map setting"), ServerMapProperty)) return false;
	TGuardValue<FSoftObjectPath> ClientMapGuard(*ClientMapProperty->ContainerPtrToValuePtr<FSoftObjectPath>(Maps),
		FSoftObjectPath(TEXT("/Game/Tests/ClientFallback.ClientFallback")));
	TGuardValue<FSoftObjectPath> ServerMapGuard(*ServerMapProperty->ContainerPtrToValuePtr<FSoftObjectPath>(Maps),
		FSoftObjectPath(TEXT("/Game/Tests/ServerFallback.ServerFallback")));
	Instance->PvpMap.Reset();
	TestEqual(TEXT("An empty PvP override uses the native client map, not the dedicated-server default"),
		Instance->ResolvePvpMapPackage(), FString(TEXT("/Game/Tests/ClientFallback")));
	Instance->PvpMap = TEXT("   ");
	TestEqual(TEXT("Whitespace-only overrides use the same native client map"),
		Instance->ResolvePvpMapPackage(), UGameMapsSettings::GetGameDefaultMap(EDefaultMapRequestType::Client));
	Instance->PvpMap = TEXT(" /Game/Tests/PvpOverride.PvpOverride ");
	TestEqual(TEXT("An explicit PvP object path remains the normalized priority override"),
		Instance->ResolvePvpMapPackage(), FString(TEXT("/Game/Tests/PvpOverride")));
	Instance->PvpMap = TEXT("/Game/Tests/PvpOverride?listen");
	TestFalse(TEXT("The shared resolver does not make URL options into an accepted map package"),
		FPackageName::IsValidLongPackageName(Instance->ResolvePvpMapPackage()));
	Instance->PvpMap.Reset();
	Instance->OperationRevision = 41;
	Instance->ScheduledRetryRevision = 40;
	Instance->ConnectionState = EFPConnectionState::RetryWait;
	Instance->bAutoReconnect = true;
	Instance->RetryCount = 1;
	Instance->RetryAt = FPlatformTime::Seconds() - 1.0;
	Instance->ServerAddress = TEXT("127.0.0.1:7797");
	Instance->TickConnection(0.1f);
	TestEqual(TEXT("An old retry cannot advance the current operation"), Instance->OperationRevision, uint32(41));
	TestEqual(TEXT("An old retry cannot consume another retry attempt"), Instance->RetryCount, 1);
	const uint32 CancelledRevision = Instance->OperationRevision;
	Instance->CancelPvpConnection();
	TestFalse(TEXT("Explicit cancellation disables automatic reconnect"), Instance->bAutoReconnect);
	TestTrue(TEXT("Cancellation invalidates callbacks carrying the old operation revision"), Instance->OperationRevision != CancelledRevision);
	TestTrue(TEXT("Cancellation leaves no active connect, ready wait or scheduled retry"),
		Instance->ConnectionState != EFPConnectionState::Connecting && Instance->ConnectionState != EFPConnectionState::WaitingReady &&
		Instance->ConnectionState != EFPConnectionState::RetryWait);
	const EFPConnectionState AfterCancel = Instance->GetConnectionState();
	const uint32 RevisionAfterCancel = Instance->OperationRevision;
	const int32 AttemptsAfterCancel = Instance->RetryCount;
	Instance->StartClientTravel(CancelledRevision);
	Instance->TickConnection(0.1f);
	TestTrue(TEXT("A late old Travel callback cannot leave the canceled state"), Instance->GetConnectionState() == AfterCancel);
	TestEqual(TEXT("A late callback cannot create a new operation"), Instance->OperationRevision, RevisionAfterCancel);
	TestEqual(TEXT("Ticking after cancellation cannot consume another retry"), Instance->RetryCount, AttemptsAfterCancel);
	TestTrue(TEXT("An uninitialized fixture never chooses a credential file"), Instance->ResumeCacheFile.IsEmpty());

	// 独立 UUID 沙盒不使用真实 PvpResume.dat；只清理本用例创建的两个文件和空目录。
	const FString Sandbox = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(),
		TEXT("ResumeCacheMove_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString CacheFixture = FPaths::Combine(Sandbox, TEXT("CacheFixture.dat"));
	const FString TemporaryFixture = CacheFixture + TEXT(".tmp");
	AddInfo(TEXT("Synthetic resume-cache regression sandbox: ") + Sandbox);
	TUniquePtr<IFileHandle> LockedCache;
	ON_SCOPE_EXIT
	{
		LockedCache.Reset();
		Instance->ResumeCacheFile.Reset();
		IFileManager::Get().Delete(*TemporaryFixture, false, true, true);
		IFileManager::Get().Delete(*CacheFixture, false, true, true);
		IFileManager::Get().DeleteDirectory(*Sandbox, false, false);
	};
	if (!TestTrue(TEXT("Cache regression creates only its unique test directory"), IFileManager::Get().MakeDirectory(*Sandbox, true)) ||
		!TestTrue(TEXT("Cache regression seeds a synthetic previous file"), FFileHelper::SaveStringToFile(TEXT("synthetic-previous-value"), *CacheFixture))) return false;
	Instance->ResumeCacheFile = CacheFixture;
	Instance->ResumeServerAddress = TEXT("127.0.0.1:7797");
	Instance->ResumeToken = FString::ChrN(64, TCHAR('a'));
	FAutomationTestExecutionInfo BeforeSuccess;
	GetExecutionInfo(BeforeSuccess);
	Instance->SaveResumeCache();
	FAutomationTestExecutionInfo AfterSuccess;
	GetExecutionInfo(AfterSuccess);
	TestEqual(TEXT("A successful real cache replacement does not report a warning"), AfterSuccess.GetWarningTotal(), BeforeSuccess.GetWarningTotal());
	FString SavedFixture;
	if (!TestTrue(TEXT("Successful cache replacement leaves a readable synthetic file"), FFileHelper::LoadFileToString(SavedFixture, *CacheFixture))) return false;
	TArray<FString> SavedLines;
	SavedFixture.ParseIntoArrayLines(SavedLines, true);
	TestTrue(TEXT("The synthetic cache has the complete four-line format and a future expiry"), SavedLines.Num() == 4
		&& SavedLines[0] == TEXT("FPSTRUE_RESUME_1") && SavedLines[1] == Instance->ResumeServerAddress
		&& SavedLines[2] == Instance->ResumeToken && FCString::Atoi64(*SavedLines[3]) > FDateTime::UtcNow().GetTicks());
	TestFalse(TEXT("Successful replacement consumes its temporary file"), IFileManager::Get().FileExists(*TemporaryFixture));

	// Windows 的只读打开不共享删除权限：临时文件仍可写，但旧目标不能被删除或改名。
	// 只验证这个真实锁定替换失败分支，不声称 Generic Move 在所有故障下原子保旧。
	LockedCache.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*CacheFixture, false));
	if (!TestNotNull(TEXT("Cache failure fixture holds a real destination read handle"), LockedCache.Get())) return false;
	Instance->ResumeToken = FString::ChrN(64, TCHAR('b'));
	AddExpectedErrorPlain(TEXT("Resume cache could not be saved; reconnect is available only while this process remains alive."), EAutomationExpectedErrorFlags::Contains, 1);
	Instance->SaveResumeCache();
	TestTrue(TEXT("The real locked move failure emits exactly its one expected warning"), HasMetExpectedErrors());
	LockedCache.Reset();
	FString PreservedFixture;
	FString PendingFixture;
	TestTrue(TEXT("A locked destination remains readable after the failed move"), FFileHelper::LoadFileToString(PreservedFixture, *CacheFixture));
	TestTrue(TEXT("The locked replacement failure preserves the old synthetic bytes"), PreservedFixture == SavedFixture);
	TestTrue(TEXT("The failed move leaves a fully written sibling temporary file"), FFileHelper::LoadFileToString(PendingFixture, *TemporaryFixture));
	TestTrue(TEXT("The temporary file contains the new synthetic value, proving the failure followed its write"), PendingFixture.Contains(Instance->ResumeToken));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerHudRebindTest, "fpstrue.Multiplayer.Reconnect.HudRebindAndCurrentSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 覆盖订阅、补读与解绑；真实视口是否绘制由有画面双端门禁单独验收。
bool FFpstrueMultiplayerHudRebindTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	auto* World = Fixture.GetTestWorld();
	AfpstrueCharacter* First = nullptr;
	AfpstrueCharacter* Second = nullptr;
	AAIController* FirstController = nullptr;
	AAIController* SecondController = nullptr;
	UfpstrueWeaponComponent* FirstWeapon = nullptr;
	UfpstrueWeaponComponent* SecondWeapon = nullptr;
	if (!SpawnMultiplayerArmedCharacter(*World, *this, First, FirstController, FirstWeapon)
		|| !SpawnMultiplayerArmedCharacter(*World, *this, Second, SecondController, SecondWeapon)) return false;
	auto* HUD = World->SpawnActor<AfpstruePvpHUD>();
	if (!TestNotNull(TEXT("Multiplayer HUD fixture"), HUD)) return false;
	// 最小控件树使用已审查的原 ingame 名称；只验证事件接线，不把规则测试冒充真实视口绘制。
	TStrongObjectPtr<UfpstruePvpIngameWidget> Bridge(NewObject<UfpstruePvpIngameWidget>(World));
	if (!TestNotNull(TEXT("The native original-layout HUD bridge"), Bridge.Get())) return false;
	const FText InitialWeaponName = FText::FromString(TEXT("测试步枪"));
	Bridge->ApplySnapshot(50.0f, 200.0f, true, 7, 80, InitialWeaponName);
	TestFalse(TEXT("A missing authored layout is rejected rather than reported as restored"), Bridge->HasAuthoredControls());
	TestFalse(TEXT("A cached snapshot before construction is not a rendered HUD"), Bridge->HasPaintedSnapshot());
	Bridge->WidgetTree = NewObject<UWidgetTree>(Bridge.Get());
	auto* Root = Bridge->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("TestIngameRoot"));
	Bridge->WidgetTree->RootWidget = Root;
	auto* HealthBar = Bridge->WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("血量组件"));
	auto* MagazineText = Bridge->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("当前子弹"));
	auto* ReserveText = Bridge->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("剩余子弹"));
	auto* WeaponNameText = Bridge->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TextBlock_2"));
	auto* MatchHintText = Bridge->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TextBlock_65"));
	auto* AmmoContainer = Bridge->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("子弹数"));
	auto* CountdownRow = Bridge->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("HorizontalBox_327"));
	UWidget* Controls[] = {HealthBar, MagazineText, ReserveText, WeaponNameText, MatchHintText, AmmoContainer, CountdownRow};
	for (UWidget* Widget : Controls)
		Root->AddChildToVerticalBox(Widget);
	Bridge->ResolveAuthoredControls();
	Bridge->RefreshControls();
	TestTrue(TEXT("The exact original layout names resolve all required controls"), Bridge->HasAuthoredControls());
	TestEqual(TEXT("Constructing late restores the cached normalized health"), HealthBar->GetPercent(), 0.25f);
	TestEqual(TEXT("Constructing late restores the cached magazine"), MagazineText->GetText().ToString(), FText::AsNumber(7).ToString());
	TestEqual(TEXT("Constructing late restores the cached reserve"), ReserveText->GetText().ToString(), FText::AsNumber(80).ToString());
	TestTrue(TEXT("The weapon label comes from the equipment snapshot rather than the original RU74 text"), WeaponNameText->GetText().EqualTo(InitialWeaponName));
	TestEqual(TEXT("Multiplayer hides the single-player countdown row"), CountdownRow->GetVisibility(), ESlateVisibility::Collapsed);
	TestTrue(TEXT("The original task label explains multiplayer enemy survival and player damage"),
		MatchHintText->GetText().ToString().Contains(TEXT("敌人")) && MatchHintText->GetText().ToString().Contains(TEXT("互相伤害")));
	const uint32 UnchangedRevision = Bridge->SnapshotRevision;
	Bridge->ApplySnapshot(50.0f, 200.0f, true, 7, 80, InitialWeaponName);
	TestEqual(TEXT("An identical event snapshot does not invalidate the display again"), Bridge->SnapshotRevision, UnchangedRevision);
	Bridge->ApplySnapshot(50.0f, 0.0f, false, 7, 80, InitialWeaponName);
	TestEqual(TEXT("Invalid maximum health safely displays an empty health bar"), HealthBar->GetPercent(), 0.0f);
	TestEqual(TEXT("An unarmed snapshot collapses the entire original ammo panel"), AmmoContainer->GetVisibility(), ESlateVisibility::Collapsed);
	TestEqual(TEXT("An unarmed snapshot clears residual magazine text"), MagazineText->GetText().ToString(), FText::AsNumber(0).ToString());
	TestTrue(TEXT("An unarmed snapshot clears the old weapon name"), WeaponNameText->GetText().IsEmpty());
	FirstWeapon->WeaponDisplayName = FText::FromString(TEXT("第一把步枪"));
	SecondWeapon->WeaponDisplayName = FText::FromString(TEXT("第二把步枪"));
	HUD->IngameWidget = Bridge.Get();
	HUD->BindPawn(First);
	TestEqual(TEXT("Late HUD reads the current health snapshot"), HUD->CurrentHealth, First->GetCurrentHealth());
	TestEqual(TEXT("Late HUD reads the current ammo snapshot"), HUD->CurrentAmmo, FirstWeapon->GetCurrentAmmo());
	TestEqual(TEXT("The HUD feeds authoritative health into the original progress bar"), HealthBar->GetPercent(), First->GetHealthNormalized());
	TestTrue(TEXT("Equipping reveals the original ammo panel"), AmmoContainer->GetVisibility() != ESlateVisibility::Collapsed);
	TestTrue(TEXT("Equipping updates the original weapon label from its configured display name"), WeaponNameText->GetText().EqualTo(FirstWeapon->WeaponDisplayName));
	UGameplayStatics::ApplyDamage(First, 10.0f, SecondController, Second, nullptr);
	TestEqual(TEXT("Real health commit updates the subscribed HUD"), HUD->CurrentHealth, First->GetCurrentHealth());
	TestEqual(TEXT("Real health events update the original progress bar without a property binding"), HealthBar->GetPercent(), First->GetHealthNormalized());
	TestFalse(TEXT("Creating and binding HUD is not a real rendered frame"), HUD->HasDrawnCurrentState(First));
	HUD->BindPawn(Second);
	TestTrue(TEXT("Rebinding a Pawn switches the original weapon label"), WeaponNameText->GetText().EqualTo(SecondWeapon->WeaponDisplayName));
	const float SecondHealth = HUD->CurrentHealth;
	const int32 SecondAmmo = HUD->CurrentAmmo;
	UGameplayStatics::ApplyDamage(First, 10.0f, SecondController, Second, nullptr);
	FirstWeapon->OnAmmoChanged.Broadcast(1, 30, 1);
	TestEqual(TEXT("Old Pawn health cannot overwrite rebound HUD"), HUD->CurrentHealth, SecondHealth);
	TestEqual(TEXT("Old weapon event cannot overwrite rebound HUD"), HUD->CurrentAmmo, SecondAmmo);
	SecondWeapon->OnAmmoChanged.Broadcast(7, 30, 80);
	TestEqual(TEXT("Current weapon remains subscribed"), HUD->CurrentAmmo, 7);
	TestEqual(TEXT("Current weapon ammo events update the original magazine text"), MagazineText->GetText().ToString(), FText::AsNumber(7).ToString());
	TestEqual(TEXT("Current weapon ammo events update the original reserve text"), ReserveText->GetText().ToString(), FText::AsNumber(80).ToString());
	Second->ClearEquippedWeaponComponent(SecondWeapon);
	TestNull(TEXT("Unarmed HUD no longer observes the detached weapon"), HUD->ObservedWeapon.Get());
	TestEqual(TEXT("Unarmed HUD clears the magazine display"), HUD->CurrentAmmo, 0);
	TestEqual(TEXT("Unarmed HUD clears the reserve display"), HUD->ReserveAmmo, 0);
	TestEqual(TEXT("Unarmed HUD keeps displaying the current health"), HUD->CurrentHealth, Second->GetCurrentHealth());
	TestEqual(TEXT("Detaching equipment hides the original ammo panel"), AmmoContainer->GetVisibility(), ESlateVisibility::Collapsed);
	SecondWeapon->OnAmmoChanged.Broadcast(29, 30, 90);
	TestEqual(TEXT("Detached weapon events cannot repopulate an unarmed HUD"), HUD->CurrentAmmo, 0);
	HUD->BindPawn(nullptr);
	SecondWeapon->OnAmmoChanged.Broadcast(3, 30, 20);
	TestEqual(TEXT("Unbound HUD has no remaining weapon subscription"), HUD->CurrentAmmo, 0);
	TestFalse(TEXT("Unbound HUD never reports visual restoration"), HUD->HasDrawnCurrentState(Second));

	// 原生占有通知专项不手调 BindPawn：先占有再创建 HUD，之后只用真实 Possess/UnPossess 推进。
	for (AfpstrueCharacter* Pawn : {First, Second})
	{
		for (const TCHAR* Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
			TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
		{
			FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Pawn->GetClass(), Name);
			if (!TestNotNull(TEXT("Native HUD possession fixture action property"), Property)) return false;
			Property->SetObjectPropertyValue_InContainer(Pawn, NewObject<UInputAction>(Pawn));
		}
	}
	APlayerController* NativeController = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Native HUD lifecycle player controller"), NativeController)) return false;
	NativeController->Possess(First);
	if (!TestTrue(TEXT("The native owner actually possesses the first Pawn before HUD creation"), NativeController->GetPawn() == First)) return false;
	FActorSpawnParameters HudSpawn;
	HudSpawn.Owner = NativeController;
	AfpstruePvpHUD* NativeHUD = World->SpawnActor<AfpstruePvpHUD>(AfpstruePvpHUD::StaticClass(), FTransform::Identity, HudSpawn);
	if (!TestNotNull(TEXT("Late native-owned HUD"), NativeHUD)) return false;
	TestTrue(TEXT("Native HUD initialization discovers its already-possessed Pawn without a DrawHUD poll"), NativeHUD->ObservedPawn.Get() == First);
	TestEqual(TEXT("Late HUD initialization immediately reads the current health"), NativeHUD->CurrentHealth, First->GetCurrentHealth());
	TestEqual(TEXT("Late HUD initialization immediately reads the current weapon ammo"), NativeHUD->CurrentAmmo, FirstWeapon->GetCurrentAmmo());
	TestTrue(TEXT("HUD subscribes to the native authority/client Pawn-change delegate"),
		NativeController->OnPossessedPawnChanged.IsAlreadyBound(NativeHUD, &AfpstruePvpHUD::HandlePossessedPawnChanged));
	NativeController->UnPossess();
	TestNull(TEXT("Native UnPossess immediately unbinds the HUD without a rendered frame"), NativeHUD->ObservedPawn.Get());
	TestEqual(TEXT("Native UnPossess clears the previous magazine"), NativeHUD->CurrentAmmo, 0);
	NativeController->Possess(Second);
	TestTrue(TEXT("Native Possess switches the HUD to its new Pawn without manual rebinding"), NativeHUD->ObservedPawn.Get() == Second);
	TestEqual(TEXT("Native Possess reads the new Pawn's health snapshot"), NativeHUD->CurrentHealth, Second->GetCurrentHealth());
	TestEqual(TEXT("The unarmed replacement Pawn does not inherit the old magazine"), NativeHUD->CurrentAmmo, 0);
	const float NativeCurrentHealth = NativeHUD->CurrentHealth;
	UGameplayStatics::ApplyDamage(First, 1.0f, nullptr, Second, nullptr);
	FirstWeapon->OnAmmoChanged.Broadcast(1, 30, 1);
	TestEqual(TEXT("Old Pawn events cannot overwrite the natively rebound HUD"), NativeHUD->CurrentHealth, NativeCurrentHealth);
	TestEqual(TEXT("Old weapon events cannot populate the natively rebound unarmed HUD"), NativeHUD->CurrentAmmo, 0);
	NativeHUD->Destroy();
	TestFalse(TEXT("HUD EndPlay explicitly removes its native Pawn-change binding"),
		NativeController->OnPossessedPawnChanged.IsAlreadyBound(NativeHUD, &AfpstruePvpHUD::HandlePossessedPawnChanged));
	TestNull(TEXT("HUD EndPlay clears the last observed Pawn"), NativeHUD->ObservedPawn.Get());
	NativeController->UnPossess();
	NativeController->Possess(First);
	TestNull(TEXT("Later owner transitions cannot rebind the ended HUD"), NativeHUD->ObservedPawn.Get());
	return true;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemySimpleHitProxyTest,
	"fpstrue.Multiplayer.PvPvE.SimpleHitProxyLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真正的世界WeaponTrace验证活体几何、遮挡和部位身份；角色切换只检查权限，不冒充网络传输或画面验收。
bool FFpstrueEnemySimpleHitProxyTest::RunTest(const FString&)
{
	// 前序FTestWorldWrapper析构只销毁World、不强制GC；先清理它们，防止蓝图重实例化尝试在已teardown的World生成替身。
	// 必须放在当前fixture创建之前，避免让本测试的原始对象指针跨越全局回收边界。
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	// GetNetMode仅在PIE类型世界消费测试InitialNetMode；只设置枚举会仍落入Standalone分支。
	// 先确认真实生产查询读到Dedicated，再检验Reference的完整姿态要求，不能让资格断言空转。
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldTypeScope(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_DedicatedServer);
	if (!TestEqual(TEXT("Hit-proxy fixture actually runs the dedicated-server policy branch"),
		static_cast<int32>(World->GetNetMode()), static_cast<int32>(NM_DedicatedServer))) return false;
	constexpr ECollisionChannel WeaponTrace = ECC_GameTraceChannel3;
	const auto SpawnEnemy = [&](UClass* Class, const FVector& Location, bool bClientRole = false,
		ECollisionResponse VisibilityOverride = ECR_MAX, ECollisionResponse CameraOverride = ECR_MAX)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(Class, Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			// 写在真实BeginPlay之前，模拟旧Blueprint的Custom响应，不依赖运行后刷新缓存的测试旁路。
			if (VisibilityOverride != ECR_MAX) Enemy->GetMesh()->SetCollisionResponseToChannel(ECC_Visibility, VisibilityOverride);
			if (CameraOverride != ECR_MAX) Enemy->GetMesh()->SetCollisionResponseToChannel(ECC_Camera, CameraOverride);
			if (bClientRole) Enemy->SetRole(ROLE_SimulatedProxy);
			Enemy->FinishSpawning(Transform);
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* Enemy = SpawnEnemy(AfpstrueEnemyCharacter::StaticClass(), FVector(0.0f, 0.0f, 500.0f));
	if (!TestNotNull(TEXT("Simple hit-proxy authority fixture"), Enemy) ||
		!TestNotNull(TEXT("Native head query component"), Enemy->EnemyHeadHitProxy.Get()) ||
		!TestNotNull(TEXT("Native body query component"), Enemy->EnemyBodyHitProxy.Get())) return false;
	TestTrue(TEXT("The authored default selects simplified live enemy hit geometry"), Enemy->UsesSimpleHitProxies());
	Enemy->ConfigureHitProxies(true);
	TestTrue(TEXT("Simple mode is a real production configuration"), Enemy->UsesSimpleHitProxies());
	TestFalse(TEXT("An idle dedicated authority with root-bound proxies does not require full hit-bone animation"), Enemy->RequiresFullAuthorityPose());
	const auto GetProxies = [](AfpstrueEnemyCharacter& Target)
	{
		return TArray<UBoxComponent*, TInlineAllocator<6>>{Target.EnemyHeadHitProxy.Get(), Target.EnemyBodyHitProxy.Get(),
			Target.EnemyLeftArmHitProxy.Get(), Target.EnemyRightArmHitProxy.Get(), Target.EnemyLeftLegHitProxy.Get(), Target.EnemyRightLegHitProxy.Get()};
	};
	const FName ExpectedBones[] = {TEXT("head"), TEXT("spine_03"), TEXT("upperarm_l"), TEXT("upperarm_r"), TEXT("thigh_l"), TEXT("thigh_r")};
	const TArray<UBoxComponent*, TInlineAllocator<6>> Proxies = GetProxies(*Enemy);
	TestEqual(TEXT("Six independently identified root-bound query components exist"), Proxies.Num(), 6);
	const auto CheckBoxGeometry = [&](AfpstrueEnemyCharacter& Target)
	{
		int32 BoxCount = 0;
		for (UActorComponent* Component : Target.GetComponents())
		{
			if (!Component->ComponentHasTag(TEXT("EnemyHitProxy"))) continue;
			++BoxCount;
			if (!TestTrue(TEXT("Every tagged live hit proxy is a native box, not a mesh adapter"), Component->GetClass() == UBoxComponent::StaticClass())) return false;
		}
		if (!TestEqual(TEXT("The real actor contains exactly six tagged native boxes"), BoxCount, 6)) return false;
		for (UBoxComponent* Proxy : GetProxies(Target))
		{
			if (!TestNotNull(TEXT("Each part uses a real native box component"), Proxy)) return false;
			const UBodySetup* Body = Proxy->GetBodySetup();
			if (!TestNotNull(TEXT("Each native box owns its actual query geometry"), Body) ||
				!TestEqual(TEXT("Each hit part has exactly one native BoxElem"), Body->AggGeom.BoxElems.Num(), 1)) return false;
			TestEqual(TEXT("Native hit boxes have no convex geometry"), Body->AggGeom.ConvexElems.Num(), 0);
			TestEqual(TEXT("No spheres, capsules or other geometry remain alongside the box"), Body->AggGeom.GetElementCount(), 1);
			const FVector Extent = Proxy->GetUnscaledBoxExtent();
			TestTrue(TEXT("Native boxes use real positive extents rather than mesh scale conversion"),
				!Extent.ContainsNaN() && Extent.GetMin() >= 1.0 && Proxy->GetRelativeScale3D().Equals(FVector::OneVector));
			const FKBoxElem& Box = Body->AggGeom.BoxElems[0];
			TestTrue(TEXT("The native BoxElem dimensions match the configured component half extents"),
				FVector(Box.X, Box.Y, Box.Z).Equals(Extent * 2.0, 0.001) && Box.GetTransform().Equals(FTransform::Identity));
			TestTrue(TEXT("Complex weapon and sight rays use the same native box geometry"), Body->CollisionTraceFlag == CTF_UseSimpleAsComplex);
		}
		return true;
	};
	if (!CheckBoxGeometry(*Enemy)) return false;
	for (UBoxComponent* Proxy : Proxies)
	{
		if (!TestNotNull(TEXT("Each of the six proxy components exists"), Proxy)) return false;
		TestTrue(TEXT("Every part attaches to the movement root without a bone socket"),
			Proxy->GetAttachParent() == Enemy->GetRootComponent() && Proxy->GetAttachSocketName().IsNone());
		TestTrue(TEXT("Each authoritative proxy is query-only"), Proxy->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
		TestTrue(TEXT("Each proxy blocks the real weapon trace channel"), Proxy->GetCollisionResponseToChannel(WeaponTrace) == ECR_Block);
		TestTrue(TEXT("Proxy object type preserves the original mesh query filtering"),
			Proxy->GetCollisionObjectType() == Enemy->GetMesh()->GetCollisionObjectType());
		TestFalse(TEXT("Hit proxies never generate gameplay overlap events"), Proxy->GetGenerateOverlapEvents());
		TestFalse(TEXT("Hit proxies do not change navigation geometry"), Proxy->CanEverAffectNavigation());
		TestFalse(TEXT("Query-only boxes never become visible render geometry"), Proxy->IsVisible());
		TestFalse(TEXT("Query-only boxes cannot consume a shadow slot"), Proxy->CastShadow);
		TestFalse(TEXT("Query-only boxes cannot consume a ray-tracing slot"), Proxy->bVisibleInRayTracing);
		for (int32 Channel = 0; Channel < ECC_OverlapAll_Deprecated; ++Channel)
			TestTrue(TEXT("Every proxy query response preserves the original mesh contract rather than clearing non-weapon channels"),
				Proxy->GetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel)) ==
				Enemy->GetMesh()->GetCollisionResponseToChannel(static_cast<ECollisionChannel>(Channel)));
	}
	TestTrue(TEXT("Movement capsule cannot steal a weapon trace"), Enemy->GetCapsuleComponent()->GetCollisionResponseToChannel(WeaponTrace) == ECR_Ignore);
	TestTrue(TEXT("The old live mesh no longer participates in collision"), Enemy->GetMesh()->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestFalse(TEXT("Simplified live mesh cannot force physics-state creation"), Enemy->GetMesh()->bAlwaysCreatePhysicsState);
	TestEqual(TEXT("Simplified native enemy has no live skeletal body instances"), Enemy->GetMesh()->Bodies.Num(), 0);

	const auto TraceProxy = [&](AfpstrueEnemyCharacter& Target, UPrimitiveComponent& Expected, float Distance, FName ExpectedBone)
	{
		const FVector Center = Expected.GetComponentLocation();
		const FVector Direction = Target.GetActorForwardVector();
		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByChannel(Hit, Center - Direction * Distance, Center + Direction * 100.0f,
			WeaponTrace, FCollisionQueryParams(SCENE_QUERY_STAT(EnemySimpleHitProxyTest), true));
		if (!TestTrue(TEXT("A world weapon ray reaches the actual enemy query proxy"), bHit) ||
			!TestTrue(TEXT("World hit preserves the authoritative target and exact proxy component"),
				Hit.GetActor() == &Target && Hit.GetComponent() == &Expected)) return false;
		Target.ResolveHitProxyBone(Hit);
		TestEqual(TEXT("Proxy normalization preserves the exact part damage identity"), Hit.BoneName, ExpectedBone);
		TestTrue(TEXT("Identity normalization cannot move or replace the actual physics hit"),
			Hit.GetActor() == &Target && Hit.GetComponent() == &Expected && Hit.bBlockingHit && !Hit.ImpactPoint.ContainsNaN());
		return true;
	};
	TArray<FVector, TInlineAllocator<6>> LocalCenters;
	for (int32 Index = 0; Index < Proxies.Num(); ++Index)
	{
		const float NearDistance = static_cast<float>(Proxies[Index]->Bounds.BoxExtent.Size()) + 5.0f;
		if (!TraceProxy(*Enemy, *Proxies[Index], NearDistance, ExpectedBones[Index]) ||
			!TraceProxy(*Enemy, *Proxies[Index], 300.0f, ExpectedBones[Index]) ||
			!TraceProxy(*Enemy, *Proxies[Index], 6000.0f, ExpectedBones[Index])) return false;
		LocalCenters.Add(Proxies[Index]->GetRelativeLocation());
		// 单组件真实物理射线隔离相邻部位，验证新盒体的完整角区与边界外不命中，不拿Bounds冒充碰撞。
		const FVector Extent = Proxies[Index]->GetUnscaledBoxExtent();
		const FTransform Transform = Proxies[Index]->GetComponentTransform();
		const FVector CornerOffset(0.0, Extent.Y * 0.95, Extent.Z * 0.95);
		FHitResult CornerHit;
		TestTrue(TEXT("A real native box ray includes its full corner rather than the old chamfered hull"),
			Proxies[Index]->LineTraceComponent(CornerHit,
				Transform.TransformPosition(CornerOffset - FVector(Extent.X + 5.0, 0.0, 0.0)),
				Transform.TransformPosition(CornerOffset + FVector(Extent.X + 5.0, 0.0, 0.0)),
				FCollisionQueryParams(SCENE_QUERY_STAT(EnemyNativeBoxCorner), true)));
		FHitResult OutsideHit;
		const FVector OutsideOffset(0.0, Extent.Y + 1.0, 0.0);
		TestFalse(TEXT("A real native box ray just outside the configured face cannot hit the part"),
			Proxies[Index]->LineTraceComponent(OutsideHit,
				Transform.TransformPosition(OutsideOffset - FVector(Extent.X + 5.0, 0.0, 0.0)),
				Transform.TransformPosition(OutsideOffset + FVector(Extent.X + 5.0, 0.0, 0.0)),
				FCollisionQueryParams(SCENE_QUERY_STAT(EnemyNativeBoxOutside), true)));
	}
	Enemy->GetMesh()->SetComponentTickEnabled(false);
	Enemy->SetActorLocationAndRotation(FVector(1700.0f, 400.0f, 650.0f), FRotator(0.0f, 90.0f, 0.0f), false, nullptr, ETeleportType::TeleportPhysics);
	for (int32 Index = 0; Index < Proxies.Num(); ++Index)
	{
		TestTrue(TEXT("Every native box follows root movement and rotation without an animation tick"),
			Proxies[Index]->GetComponentLocation().Equals(Enemy->GetActorTransform().TransformPosition(LocalCenters[Index]), 0.001f));
		if (!TraceProxy(*Enemy, *Proxies[Index], 300.0f, ExpectedBones[Index])) return false;
	}

	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Simple hit-proxy real occluder"), Wall)) return false;
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(WallBox);
	Wall->SetRootComponent(WallBox);
	WallBox->SetBoxExtent(FVector(60.0f, 20.0f, 150.0f));
	WallBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	WallBox->SetCollisionResponseToChannel(WeaponTrace, ECR_Block);
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WallBox->RegisterComponent();
	const FVector BodyCenter = Enemy->EnemyBodyHitProxy->GetComponentLocation();
	const FVector Forward = Enemy->GetActorForwardVector();
	Wall->SetActorLocation(BodyCenter - Forward * 150.0f);
	FHitResult BlockedHit;
	TestTrue(TEXT("Weapon trace still sees a real wall before the simplified enemy"),
		World->LineTraceSingleByChannel(BlockedHit, BodyCenter - Forward * 300.0f, BodyCenter + Forward * 100.0f,
			WeaponTrace, FCollisionQueryParams(SCENE_QUERY_STAT(EnemySimpleHitProxyOcclusion), true)) && BlockedHit.GetActor() == Wall);
	Enemy->ResolveHitProxyBone(BlockedHit);
	TestTrue(TEXT("Enemy normalization cannot manufacture a hit identity on an occluder"), BlockedHit.GetActor() == Wall && BlockedHit.BoneName.IsNone());
	Wall->Destroy();

	Enemy->ConfigureHitProxies(false);
	TestFalse(TEXT("Reference selects the original live skeletal hit path"), Enemy->UsesSimpleHitProxies());
	TestTrue(TEXT("Reference retains full dedicated authority pose for the skeletal hit path"), Enemy->RequiresFullAuthorityPose());
	for (UPrimitiveComponent* Proxy : Proxies)
		TestTrue(TEXT("Reference disables every additional native box"), Proxy->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("Reference restores original live query collision"), Enemy->GetMesh()->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
	Enemy->ConfigureHitProxies(true);
	TestFalse(TEXT("Returning to simplified queries releases only the unneeded full-pose requirement"), Enemy->RequiresFullAuthorityPose());
	if (!TraceProxy(*Enemy, *Enemy->EnemyHeadHitProxy, 300.0f, TEXT("head"))) return false;

	AfpstrueEnemyCharacter* Client = SpawnEnemy(AfpstrueEnemyCharacter::StaticClass(), FVector(10000.0f, 10000.0f, 500.0f), true);
	if (!TestNotNull(TEXT("Client-role proxy instance"), Client)) return false;
	Client->ConfigureHitProxies(true);
	for (UPrimitiveComponent* Proxy : GetProxies(*Client))
		TestTrue(TEXT("Client keeps every native box without authoritative queries"), Proxy->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	const FVector ClientCenter = Client->EnemyBodyHitProxy->GetComponentLocation();
	FHitResult ClientHit;
	TestFalse(TEXT("A client-role enemy cannot expose a local authoritative body query"),
		World->LineTraceSingleByChannel(ClientHit, ClientCenter - FVector(300.0f, 0.0f, 0.0f), ClientCenter + FVector(100.0f, 0.0f, 0.0f),
			WeaponTrace, FCollisionQueryParams(SCENE_QUERY_STAT(EnemySimpleHitProxyClient), true)));

	// 原生Sight使用complex Visibility线且只忽略监听者；删旧Mesh不能顺带让其他AI失去视觉遮挡。
	// 这里只验证同通道真实物理射线，完整感知调度仍由既有Sight测试负责。
	const auto CheckPerceptionResponse = [&](const FVector& Location, ECollisionResponse VisibilityResponse,
		ECollisionResponse CameraResponse)
	{
		AfpstrueEnemyCharacter* Occluder = SpawnEnemy(AfpstrueEnemyCharacter::StaticClass(), Location, false,
			VisibilityResponse, CameraResponse);
		if (!TestNotNull(TEXT("Sight-channel fixture creates another real AI"), Occluder)) return false;
		for (UPrimitiveComponent* Proxy : GetProxies(*Occluder))
		{
			TestTrue(TEXT("Simple proxies preserve an authored Custom Visibility response"),
				Proxy->GetCollisionResponseToChannel(ECC_Visibility) == VisibilityResponse);
			TestTrue(TEXT("Simple proxies preserve an authored Custom Camera response"),
				Proxy->GetCollisionResponseToChannel(ECC_Camera) == CameraResponse);
		}
		const FVector Center = Occluder->EnemyBodyHitProxy->GetComponentLocation();
		Enemy->SetActorLocation(Center - FVector(300.0f, 0.0f, 0.0f));
		FHitResult SightHit;
		FCollisionQueryParams SightQuery(SCENE_QUERY_STAT(EnemySimpleHitProxySight), true, Enemy);
		const bool bSightBlocked = World->LineTraceSingleByChannel(SightHit, Enemy->GetActorLocation(),
			Center + FVector(300.0f, 0.0f, 0.0f), ECC_Visibility, SightQuery);
		if (VisibilityResponse == ECR_Block)
			TestTrue(TEXT("Another AI still occludes a real complex Sight-channel ray"),
				bSightBlocked && SightHit.GetActor() == Occluder && SightHit.GetComponent() == Occluder->EnemyBodyHitProxy);
		else
			TestFalse(TEXT("Authored Visibility Ignore does not become an accidental AI occluder"), bSightBlocked);
		return true;
	};
	if (!CheckPerceptionResponse(FVector(1000.0f, 12000.0f, 500.0f), ECR_Block, ECR_Overlap) ||
		!CheckPerceptionResponse(FVector(1000.0f, 16000.0f, 500.0f), ECR_Ignore, ECR_Ignore)) return false;

	// 扫描磁盘包引用而非只看已加载对象，硬/软包引用都不能保留专用命中网格依赖；不修改或保存资产。
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanPathsSynchronous({TEXT("/Game")}, true);
	const FName RetiredHullPackage(TEXT("/Game/PerformanceCandidates/EnemyCollision/SM_EnemyHitConvex"));
	TArray<FName> HullReferencers;
	Registry.GetReferencers(RetiredHullPackage, HullReferencers, UE::AssetRegistry::EDependencyCategory::Package);
	for (const FName Referencer : HullReferencers)
		AddError(FString::Printf(TEXT("Project package retains a hard or soft reference to the removed hit hull: %s"), *Referencer.ToString()));
	TestEqual(TEXT("No on-disk package retains a hard or soft reference to the retired hull"), HullReferencers.Num(), 0);
	TestFalse(TEXT("The retired special-purpose convex asset is no longer an active project package"),
		FPackageName::DoesPackageExist(RetiredHullPackage.ToString()));
	const auto CheckBlueprintDefaults = [&](UClass* Class, const TCHAR* Phase)
	{
		AddInfo(FString::Printf(TEXT("Checking actual enemy Blueprint native-box defaults: %s"), Phase));
		AfpstrueEnemyCharacter* Defaults = Class->GetDefaultObject<AfpstrueEnemyCharacter>();
		for (const FName Name : {FName(TEXT("EnemyHeadHitProxy")), FName(TEXT("EnemyBodyHitProxy")),
			FName(TEXT("EnemyLeftArmHitProxy")), FName(TEXT("EnemyRightArmHitProxy")),
			FName(TEXT("EnemyLeftLegHitProxy")), FName(TEXT("EnemyRightLegHitProxy"))})
		{
			const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Class, Name);
			if (!TestNotNull(TEXT("The actual Blueprint exposes each inherited hit-proxy property"), Property) ||
				!TestTrue(TEXT("Reflected hit-proxy properties use UBoxComponent, not the retired static-mesh type"),
					Property->PropertyClass == UBoxComponent::StaticClass())) return false;
			UObject* DefaultPart = Property->GetObjectPropertyValue_InContainer(Defaults);
			if (!TestNotNull(TEXT("Each Blueprint CDO hit-proxy property has a real default subobject"), DefaultPart) ||
				!TestTrue(TEXT("The Blueprint CDO retains exactly the named native box default subobject"),
					DefaultPart->GetClass() == UBoxComponent::StaticClass() &&
					DefaultPart == Defaults->GetDefaultSubobjectByName(Name))) return false;
		}
		return true;
	};
	// 先验证磁盘蓝图直接加载和正常BeginPlay，不能靠编译重建CDO或手动Configure掩盖失效模板。
	UClass* BlueprintClass = LoadClass<AfpstrueEnemyCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP_C"));
	if (!TestNotNull(TEXT("Hit-proxy lifecycle loads the actual enemy Blueprint"), BlueprintClass)) return false;
	if (!CheckBlueprintDefaults(BlueprintClass, TEXT("direct load before compile"))) return false;
	AfpstrueEnemyCharacter* DirectLoadEnemy = SpawnEnemy(BlueprintClass, FVector(18000.0f, 0.0f, 500.0f));
	if (!TestNotNull(TEXT("Directly loaded enemy Blueprint spawns before explicit compilation"), DirectLoadEnemy) ||
		!TestTrue(TEXT("Normal Blueprint BeginPlay selects native boxes without manual reconfiguration"), DirectLoadEnemy->UsesSimpleHitProxies()) ||
		!CheckBoxGeometry(*DirectLoadEnemy) ||
		!TraceProxy(*DirectLoadEnemy, *DirectLoadEnemy->EnemyHeadHitProxy, 300.0f, TEXT("head"))) return false;
	TestEqual(TEXT("Normal Blueprint BeginPlay retires live skeletal bodies before any test reconfiguration"), DirectLoadEnemy->GetMesh()->Bodies.Num(), 0);
	DirectLoadEnemy->Destroy();
	// 再重新编译验证旧StaticMesh组件类型/节点未藏在保存图表里；不Save。
	UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(BlueprintClass);
	if (!TestNotNull(TEXT("The actual enemy generated class retains its editable source Blueprint"), Blueprint)) return false;
	FCompilerResultsLog Compilation;
	FKismetEditorUtilities::CompileBlueprint(Blueprint,
		EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::SkipSave, &Compilation);
	for (const TSharedRef<FTokenizedMessage>& Message : Compilation.Messages)
		AddInfo(FString::Printf(TEXT("Enemy Blueprint compile: %s"), *Message->ToText().ToString()));
	if (!TestEqual(TEXT("The real enemy Blueprint compiles without stale mesh-typed proxy errors"), Compilation.NumErrors, 0) ||
		!TestTrue(TEXT("The recompiled enemy Blueprint is valid"), Blueprint->Status != BS_Error)) return false;
	BlueprintClass = Blueprint->GeneratedClass;
	if (!TestNotNull(TEXT("Successful native-box compilation retains a generated enemy class"), BlueprintClass)) return false;
	if (!CheckBlueprintDefaults(BlueprintClass, TEXT("after compile"))) return false;
	TArray<FName> BlueprintDependencies;
	Registry.GetDependencies(BlueprintClass->GetOutermost()->GetFName(), BlueprintDependencies, UE::AssetRegistry::EDependencyCategory::Package);
	TestFalse(TEXT("The actual enemy Blueprint has no direct on-disk retired-hull dependency"), BlueprintDependencies.Contains(RetiredHullPackage));
	AfpstrueEnemyCharacter* AssetEnemy = SpawnEnemy(BlueprintClass, FVector(20000.0f, 0.0f, 500.0f));
	if (!TestNotNull(TEXT("Actual skeleton-backed enemy"), AssetEnemy)) return false;
	if (!CheckBoxGeometry(*AssetEnemy)) return false;
	USkeletalMeshComponent* AssetMesh = AssetEnemy->GetMesh();
	UPhysicsAsset* DeathPhysics = AssetMesh->GetPhysicsAsset();
	if (!TestNotNull(TEXT("Full source physics asset remains available for death"), DeathPhysics)) return false;
	TestTrue(TEXT("The actual legacy enemy mesh retains its saved Custom Visibility Block response"),
		AssetMesh->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block);
	AssetEnemy->ConfigureHitProxies(false);
	TestTrue(TEXT("Reference actually creates the original skeleton-backed query bodies"), AssetMesh->Bodies.Num() > 0);
	AssetEnemy->ConfigureHitProxies(true);
	TestEqual(TEXT("Switching the actual enemy to Simple retires all old live skeletal bodies"), AssetMesh->Bodies.Num(), 0);
	TestFalse(TEXT("Actual live Simple mesh has no residual physics state"), AssetMesh->IsPhysicsStateCreated());
	TestTrue(TEXT("Retiring live bodies does not mutate the source death asset"), AssetMesh->GetPhysicsAsset() == DeathPhysics);
	for (UPrimitiveComponent* Proxy : GetProxies(*AssetEnemy))
		TestTrue(TEXT("All six parts preserve the actual legacy enemy Sight-channel occlusion"),
			Proxy->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block);
	if (!TraceProxy(*AssetEnemy, *AssetEnemy->EnemyHeadHitProxy, 300.0f, TEXT("head"))) return false;
	FHitResult DeathHit;
	const FVector HeadCenter = AssetEnemy->EnemyHeadHitProxy->GetComponentLocation();
	if (!TestTrue(TEXT("Death fixture starts with a real proxy head hit"),
		World->LineTraceSingleByChannel(DeathHit, HeadCenter - FVector(300.0f, 0.0f, 0.0f), HeadCenter + FVector(100.0f, 0.0f, 0.0f),
			WeaponTrace, FCollisionQueryParams(SCENE_QUERY_STAT(EnemySimpleHitProxyDeath), true)))) return false;
	AssetEnemy->ResolveHitProxyBone(DeathHit);
	UGameplayStatics::ApplyPointDamage(AssetEnemy, 10000.0f, FVector::ForwardVector, DeathHit, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Actual point-damage path reaches the enemy death transaction"), AssetEnemy->IsDead());
	for (UPrimitiveComponent* Proxy : GetProxies(*AssetEnemy))
		TestTrue(TEXT("Death retires every live native box"), Proxy->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("Death restores the original PhysicsAsset and simulation"), AssetMesh->GetPhysicsAsset() == DeathPhysics && AssetMesh->IsSimulatingPhysics());
	TestTrue(TEXT("Death recreates actual skeletal rigid bodies"), AssetMesh->Bodies.Num() > 0);
	TestEqual(TEXT("Death impulse retains the normalized head bone identity"), AssetEnemy->DeathDamageContext.BoneName, FName(TEXT("head")));
	TestTrue(TEXT("Death impulse retains the original geometric impact point"), AssetEnemy->DeathDamageContext.Location.Equals(DeathHit.ImpactPoint, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerEnemyHealthBarTest,
	"fpstrue.Multiplayer.PvPvE.EnemyHealthBarAnchorAndHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 使用真实敌人蓝图与原生敌人验证世界锚点和事件驱动数值；不冒充屏幕投影观感或网络传输验收。
bool FFpstrueMultiplayerEnemyHealthBarTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	UClass* BlueprintClass = LoadClass<AfpstrueEnemyCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP_C"));
	if (!TestNotNull(TEXT("Health-bar fixture loads the actual project enemy Blueprint"), BlueprintClass)) return false;
	const auto SpawnEnemy = [&](UClass* Class, const FVector& Location, bool bLegacyAnchor)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(Class, Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			if (bLegacyAnchor)
			{
				// 模拟旧 BP 保存的父节点和绝对变换，必须由真实 BeginPlay 修复，不能只测新 native CDO。
				if (UWidgetComponent* Bar = Enemy->FindComponentByClass<UWidgetComponent>())
				{
					Bar->AttachToComponent(Enemy->GetCapsuleComponent(), FAttachmentTransformRules::KeepRelativeTransform);
					Bar->SetAbsolute(true, true, true);
					Bar->SetWorldLocationAndRotation(FVector(10000.0f, -5000.0f, 900.0f), FRotator(30.0f, 75.0f, 15.0f));
					Bar->SetWorldScale3D(FVector(2.0f));
				}
			}
			Enemy->FinishSpawning(Transform);
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
			Enemy->SetActorEnableCollision(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* BlueprintEnemy = SpawnEnemy(BlueprintClass, FVector(100.0f, 200.0f, 300.0f), true);
	AfpstrueEnemyCharacter* NativeEnemy = SpawnEnemy(AfpstrueEnemyCharacter::StaticClass(), FVector(-1000.0f, 500.0f, 300.0f), false);
	if (!TestNotNull(TEXT("Health-bar fixture spawns the real enemy Blueprint"), BlueprintEnemy)
		|| !TestNotNull(TEXT("Health-bar fixture also covers native component defaults"), NativeEnemy)) return false;
	TestTrue(TEXT("The project enemy keeps its actual Blueprint class"), BlueprintEnemy->GetClass() == BlueprintClass);
	UWidgetComponent* BlueprintBar = BlueprintEnemy->FindComponentByClass<UWidgetComponent>();
	UWidgetComponent* NativeBar = NativeEnemy->FindComponentByClass<UWidgetComponent>();
	UfpstrueHealthComponent* BlueprintHealth = BlueprintEnemy->FindComponentByClass<UfpstrueHealthComponent>();
	UfpstrueHealthComponent* NativeHealth = NativeEnemy->FindComponentByClass<UfpstrueHealthComponent>();
	if (!TestNotNull(TEXT("Project enemy contains its native health-bar component"), BlueprintBar)
		|| !TestNotNull(TEXT("Native enemy contains its health-bar component"), NativeBar)
		|| !TestNotNull(TEXT("Project enemy has the real health owner"), BlueprintHealth)
		|| !TestNotNull(TEXT("Native enemy has its own health owner"), NativeHealth)) return false;
	for (AfpstrueEnemyCharacter* Enemy : {BlueprintEnemy, NativeEnemy})
	{
		UWidgetComponent* Bar = Enemy->FindComponentByClass<UWidgetComponent>();
		const FVector ExpectedOffset(0.0f, 0.0f, Enemy->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 28.0f);
		const FVector ExpectedWorld = Enemy->GetCapsuleComponent()->GetComponentTransform().TransformPosition(ExpectedOffset);
		TestTrue(TEXT("The health bar belongs to this enemy and follows its network-smoothed body component without a bone socket"),
			Bar->GetOwner() == Enemy && Bar->GetAttachParent() == Enemy->GetMesh() && Bar->GetAttachSocketName().IsNone());
		TestTrue(TEXT("Each enemy initially retains its actual capsule height plus a fixed head clearance, including legacy Blueprint instances"),
			Bar->GetComponentLocation().Equals(ExpectedWorld, 0.01f));
		TestTrue(TEXT("BeginPlay removes old absolute transforms so the anchor follows the smoothed mesh"),
			!Bar->IsUsingAbsoluteLocation() && !Bar->IsUsingAbsoluteRotation() && !Bar->IsUsingAbsoluteScale()
			&& Bar->GetRelativeScale3D().Equals(FVector::OneVector, 0.001f));
		TestTrue(TEXT("World-space event drawing starts at a stable local orientation before the local camera billboard tick"),
			Bar->GetWidgetSpace() == EWidgetSpace::World && Bar->GetManuallyRedraw() && Bar->GetRelativeRotation().IsNearlyZero());
		TestFalse(TEXT("Only health replicates; the per-client UI component is not replicated"), Bar->GetIsReplicated());
		TestTrue(TEXT("The health bar adds no gameplay collision"), Bar->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
		TestTrue(TEXT("The world bar retains its fixed content draw size and centred bottom pivot"),
			Bar->GetDrawSize().Equals(FVector2D(80.0f, 8.0f)) && Bar->GetPivot().Equals(FVector2D(0.5f, 1.0f)));
	}
	UfpstrueEnemyHealthBarWidget* BlueprintWidget = Cast<UfpstrueEnemyHealthBarWidget>(BlueprintBar->GetUserWidgetObject());
	UfpstrueEnemyHealthBarWidget* NativeWidget = Cast<UfpstrueEnemyHealthBarWidget>(NativeBar->GetUserWidgetObject());
	// 不为测试手动注入 Widget：若本次 NullRHI/Slate 环境未创建真实对象，就明确报告无法验证数值绑定。
	if (!TestNotNull(TEXT("The production component initializes the real project-enemy widget in this test environment"), BlueprintWidget)
		|| !TestNotNull(TEXT("The production component initializes the native-enemy widget without test injection"), NativeWidget)) return false;
	TestTrue(TEXT("Initial binding immediately reads each enemy's current health"),
		FMath::IsNearlyEqual(BlueprintWidget->GetHealthFraction(), BlueprintHealth->GetHealthNormalized())
		&& FMath::IsNearlyEqual(NativeWidget->GetHealthFraction(), NativeHealth->GetHealthNormalized()));
	TestTrue(TEXT("Two enemies retain separate health-bar widget instances"), BlueprintWidget != NativeWidget);
	const FVector BeforeViewChange = BlueprintBar->GetComponentLocation();
	ACameraActor* Camera = World->SpawnActor<ACameraActor>();
	APlayerController* Viewer = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Anchor fixture has an independent gameplay camera"), Camera)
		|| !TestNotNull(TEXT("Anchor fixture has an independent viewer Controller"), Viewer)) return false;
	Viewer->SetViewTarget(Camera);
	for (const FRotator Rotation : {FRotator(65.0f, 120.0f, 0.0f), FRotator(-50.0f, -160.0f, 0.0f)})
	{
		Viewer->SetControlRotation(Rotation);
		Camera->SetActorRotation(Rotation);
		Camera->GetCameraComponent()->SetFieldOfView(70.0f);
		TestTrue(TEXT("Changing another player's view cannot move the enemy's world-space health-bar anchor"),
			BlueprintBar->GetComponentLocation().Equals(BeforeViewChange, 0.01f));
	}
	BlueprintEnemy->SetActorLocationAndRotation(FVector(750.0f, -250.0f, 400.0f), FRotator(0.0f, 135.0f, 0.0f), false, nullptr,
		ETeleportType::TeleportPhysics);
	TestTrue(TEXT("Moving and rotating the enemy moves the anchor with its body component"), BlueprintBar->GetComponentLocation().Equals(
		BlueprintEnemy->GetMesh()->GetComponentTransform().TransformPosition(BlueprintBar->GetRelativeLocation()), 0.01f));
	TestFalse(TEXT("The anchor does not stay behind at its old screen-associated world position"),
		BlueprintBar->GetComponentLocation().Equals(BeforeViewChange, 0.01f));
	const FVector CapsuleBeforeSmoothing = BlueprintEnemy->GetCapsuleComponent()->GetComponentLocation();
	const FVector BarBeforeSmoothing = BlueprintBar->GetComponentLocation();
	const FVector OtherBarBeforeSmoothing = NativeBar->GetComponentLocation();
	const FVector MeshOffsetBeforeSmoothing = BlueprintEnemy->GetMesh()->GetRelativeLocation();
	const FVector SimulatedSmoothingOffset(45.0f, -25.0f, 12.0f);
	// CharacterMovement 的网络平滑只偏移 Mesh，不偏移已经校正的 Capsule；这里直接复现该变换边界。
	BlueprintEnemy->GetMesh()->SetRelativeLocation(MeshOffsetBeforeSmoothing + SimulatedSmoothingOffset);
	const FVector ExpectedSmoothingDelta = BlueprintEnemy->GetMesh()->GetAttachParent()->GetComponentTransform()
		.TransformVector(SimulatedSmoothingOffset);
	TestTrue(TEXT("A simulated network smoothing update leaves the authoritative capsule position unchanged"),
		BlueprintEnemy->GetCapsuleComponent()->GetComponentLocation().Equals(CapsuleBeforeSmoothing, 0.01f));
	TestTrue(TEXT("The health-bar anchor follows the same visual mesh offset instead of staying at the corrected capsule"),
		BlueprintBar->GetComponentLocation().Equals(BarBeforeSmoothing + ExpectedSmoothingDelta, 0.01f));
	TestTrue(TEXT("Smoothing one enemy cannot move another enemy's health bar"),
		NativeBar->GetComponentLocation().Equals(OtherBarBeforeSmoothing, 0.01f));
	BlueprintEnemy->GetMesh()->SetRelativeLocation(MeshOffsetBeforeSmoothing);
	TestTrue(TEXT("Finishing visual smoothing returns the health bar to the body anchor without a separate UI update"),
		BlueprintBar->GetComponentLocation().Equals(BarBeforeSmoothing, 0.01f));
	BlueprintHealth->SetMaxHealthAndReset(200.0f);
	UGameplayStatics::ApplyDamage(BlueprintEnemy, 50.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Authoritative damage updates the bound project-enemy health fraction through its real health event"),
		FMath::IsNearlyEqual(BlueprintWidget->GetHealthFraction(), 0.75f, 0.001f));
	TestTrue(TEXT("Damage to one enemy does not change the other's displayed health"),
		FMath::IsNearlyEqual(NativeWidget->GetHealthFraction(), 1.0f));
	UGameplayStatics::ApplyDamage(NativeEnemy, 20.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("The second enemy also consumes only its own health component's value"),
		FMath::IsNearlyEqual(NativeWidget->GetHealthFraction(), NativeHealth->GetHealthNormalized(), 0.001f)
		&& FMath::IsNearlyEqual(BlueprintWidget->GetHealthFraction(), 0.75f, 0.001f));
	BlueprintWidget->TakeWidget();
	UProgressBar* Progress = Cast<UProgressBar>(BlueprintWidget->GetWidgetFromName(TEXT("HealthProgress")));
	if (!TestNotNull(TEXT("The native widget builds a real progress bar rather than a test-only cached value"), Progress)) return false;
	TestTrue(TEXT("Building the Slate widget applies the latest health to the actual progress bar"),
		FMath::IsNearlyEqual(Progress->GetPercent(), BlueprintWidget->GetHealthFraction(), 0.001f));
	TestTrue(TEXT("Health changes reuse the existing per-enemy widget instead of creating new UI objects"),
		BlueprintBar->GetUserWidgetObject() == BlueprintWidget && NativeBar->GetUserWidgetObject() == NativeWidget);
	UGameplayStatics::ApplyDamage(BlueprintEnemy, 1000.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Authoritative death hides the smoothed body's health bar"), BlueprintEnemy->IsDead() && BlueprintBar->bHiddenInGame);
	TestNull(TEXT("Death removes the screen widget instead of leaving a stale projected health bar"), BlueprintBar->GetUserWidgetObject());
	BlueprintHealth->OnHealthChanged.Broadcast(BlueprintHealth->GetHealth());
	TestTrue(TEXT("A late health event cannot reveal a dead enemy's health bar"), BlueprintBar->bHiddenInGame);
	TestNull(TEXT("A late health event cannot recreate the dead enemy's screen widget"), BlueprintBar->GetUserWidgetObject());
	TestTrue(TEXT("The surviving enemy keeps its own health bar after another enemy dies"),
		!NativeBar->bHiddenInGame && NativeBar->GetUserWidgetObject() == NativeWidget
		&& FMath::IsNearlyEqual(NativeWidget->GetHealthFraction(), NativeHealth->GetHealthNormalized(), 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerBodyAnimationDefaultsTest,
	"fpstrue.Multiplayer.Animation.BodyDefaultsAndViewSeparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 身体与动画必须来自同一套骨架；只接一个可见网格，不能算已经接好第三人称动画。
bool FFpstrueMultiplayerBodyAnimationDefaultsTest::RunTest(const FString&)
{
	const AfpstrueCharacter* Defaults = GetDefault<AfpstrueCharacter>();
	TestNull(TEXT("Native CDO does not permanently root an unused legacy animation Blueprint"), Defaults->GetMesh()->GetAnimClass());
	TestNull(TEXT("Native CDO does not permanently retain an unused legacy body mesh"), Defaults->GetMesh()->GetSkeletalMeshAsset());
	const FObjectPropertyBase* ViewmodelCollection = FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("ViewmodelFOVCollection"));
	if (!TestNotNull(TEXT("Native presentation retains its reflected optional viewmodel collection"), ViewmodelCollection)) return false;
	TestNull(TEXT("The base animation CDO does not retain a RAR-only material collection"),
		ViewmodelCollection->GetObjectPropertyValue_InContainer(GetDefault<UfpstruePlayerAnimInstance>()));
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	AfpstrueCharacter* NativeCharacter = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Native compatibility fallback is verified after component initialization"), NativeCharacter)) return false;
	USkeletalMeshComponent* Body = NativeCharacter->GetMesh();
	const USkeletalMeshComponent* Arms = NativeCharacter->GetMesh1P();
	if (!TestNotNull(TEXT("Default character has a third-person body component"), Body)
		|| !TestNotNull(TEXT("Default character retains its first-person arms component"), Arms)) return false;
	const USkeletalMesh* Mesh = Body->GetSkeletalMeshAsset();
	UClass* AnimClass = Body->GetAnimClass();
	if (!TestNotNull(TEXT("Third-person body has its real skeletal mesh"), Mesh)
		|| !TestNotNull(TEXT("Third-person body has a configured AnimBlueprint class"), AnimClass)) return false;
	const IAnimClassInterface* Animation = IAnimClassInterface::GetFromClass(AnimClass);
	if (!TestNotNull(TEXT("Body animation is a compiled AnimBlueprint, not a reference pose fallback"), Animation)) return false;
	TestEqual(TEXT("Native compatibility fallback retains its previous lightweight Manny mesh"), Mesh->GetPathName(),
		FString(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
	TestEqual(TEXT("Body uses the migrated locomotion AnimBlueprint"), AnimClass->GetPathName(),
		FString(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Animations/ABP_Manny.ABP_Manny_C")));
	TestNotNull(TEXT("Body mesh retains a real skeleton"), Mesh->GetSkeleton());
	TestTrue(TEXT("Mesh and AnimBlueprint use the same skeleton"),
		Mesh->GetSkeleton() != nullptr && Mesh->GetSkeleton() == Animation->GetTargetSkeleton());
	TestTrue(TEXT("The owner sees its own arms, not another player's arms"), Arms->bOnlyOwnerSee && !Arms->bOwnerNoSee);
	TestTrue(TEXT("Other players see the body while its owner does not"), Body->bOwnerNoSee && !Body->bOnlyOwnerSee);
	// 联网实际生成的是 GameMode 配置的蓝图子类；它保存的组件覆盖也必须通过，不能只检查 native CDO。
	UClass* PawnClass = GetDefault<AfpstruePvpGameMode>()->DefaultPawnClass.Get();
	if (!TestNotNull(TEXT("Multiplayer GameMode retains its actual default Pawn class"), PawnClass)
		|| !TestTrue(TEXT("The configured Pawn derives from the project character"), PawnClass->IsChildOf<AfpstrueCharacter>())) return false;
	TestEqual(TEXT("Multiplayer spawns the project UE5.5 template presentation Blueprint"), PawnClass->GetPathName(),
		FString(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C")));
	const AfpstrueCharacter* BlueprintDefaults = PawnClass->GetDefaultObject<AfpstrueCharacter>();
	USkeletalMeshComponent* BlueprintBody = BlueprintDefaults->GetMesh();
	USkeletalMeshComponent* BlueprintArms = BlueprintDefaults->GetMesh1P();
	if (!TestNotNull(TEXT("Configured Blueprint retains its third-person body"), BlueprintBody)
		|| !TestNotNull(TEXT("Configured Blueprint retains its first-person arms"), BlueprintArms)) return false;
	TestEqual(TEXT("Both network players use the selected UE5.5 template body mesh"),
		GetPathNameSafe(BlueprintBody->GetSkeletalMeshAsset()),
		FString(TEXT("/Game/FPSMPT55/Demo/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
	TestEqual(TEXT("Configured Blueprint supplies the project full-body animation graph"), GetPathNameSafe(BlueprintBody->GetAnimClass()),
		FString(TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateBody.ABP_TemplateBody_C")));
	TestTrue(TEXT("Configured Blueprint preserves owner-only arms"), BlueprintArms->bOnlyOwnerSee && !BlueprintArms->bOwnerNoSee);
	TestTrue(TEXT("Configured Blueprint preserves owner-hidden third-person body"), BlueprintBody->bOwnerNoSee && !BlueprintBody->bOnlyOwnerSee);
	if (!TestNotNull(TEXT("Configured Blueprint has the matching template first-person mesh"), BlueprintArms->GetSkeletalMeshAsset())) return false;
	TestEqual(TEXT("Template presentation uses its matching first-person mesh without replacing the original RU74 asset"), BlueprintArms->GetSkeletalMeshAsset()->GetPathName(),
		FString(TEXT("/Game/FPSMPT55/Demo/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
	TestEqual(TEXT("Configured Blueprint supplies the project first-person animation graph"), GetPathNameSafe(BlueprintArms->GetAnimClass()),
		FString(TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateArms.ABP_TemplateArms_C")));
	TestTrue(TEXT("The actual Blueprint enables the native presentation path"), BlueprintDefaults->UsesNativePresentation());
	TestFalse(TEXT("Template presentation does not inherit RAR-specific FOV or bone compensation"), BlueprintDefaults->UsesRARPresentation());
	TestTrue(TEXT("Body and first-person mesh share the exact template skeleton"),
		BlueprintArms->GetSkeletalMeshAsset()->GetSkeleton() == BlueprintBody->GetSkeletalMeshAsset()->GetSkeleton());
	UClass* OriginalClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter.BP_FirstPersonCharacter_C"));
	if (!TestNotNull(TEXT("Original first-person Blueprint remains available"), OriginalClass)) return false;
	const AfpstrueCharacter* OriginalDefaults = OriginalClass->GetDefaultObject<AfpstrueCharacter>();
	for (const TCHAR* Name : {TEXT("DefaultMappingContext"), TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"),
		TEXT("FireAction"), TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(PawnClass, Name);
		if (!TestNotNull(TEXT("Template character retains the real input property"), Property)) return false;
		const UObject* ConfiguredInput = Property->GetObjectPropertyValue_InContainer(BlueprintDefaults);
		TestNotNull(FString(Name) + TEXT(" remains configured"), ConfiguredInput);
		TestTrue(FString(Name) + TEXT(" reuses the existing input asset without vendor input rules"),
			ConfiguredInput == Property->GetObjectPropertyValue_InContainer(OriginalDefaults));
	}
	// 占有真实模板后检查引擎最终控制旋转；仅夹动画参数不能代替真正的视角限制。
	FActorSpawnParameters ViewSpawn;
	ViewSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* ViewCharacter = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>(PawnClass,
		FTransform(FVector(10000.0f, 0.0f, 0.0f)), ViewSpawn);
	auto* ViewController = Fixture.GetTestWorld()->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("View-limit fixture uses the actual default template Blueprint"), ViewCharacter)
		|| !TestNotNull(TEXT("View-limit fixture uses an ordinary player controller"), ViewController)) return false;
	ViewCharacter->GetCharacterMovement()->SetComponentTickEnabled(false);
	ViewController->Possess(ViewCharacter);
	auto* ViewManager = ViewController->PlayerCameraManager.Get();
	auto* ViewAnimation = Cast<UfpstruePlayerAnimInstance>(ViewCharacter->GetMesh()->GetAnimInstance());
	if (!TestTrue(TEXT("The template view fixture is locally possessed"), ViewCharacter->IsLocallyControlled())
		|| !TestNotNull(TEXT("The local controller retains its real camera manager"), ViewManager)
		|| !TestNotNull(TEXT("The possessed template runs its real body animation"), ViewAnimation)
		|| !TestNotNull(TEXT("The template body owns the configured left turn"), ViewAnimation->TurnLeftAnimation.Get())
		|| !TestNotNull(TEXT("The template body owns the configured right turn"), ViewAnimation->TurnRightAnimation.Get())) return false;
	ViewCharacter->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	ViewCharacter->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	ViewCharacter->GetMesh()->TickAnimation(1.0f / 60.0f, false);
	ViewCharacter->GetMesh()->RefreshBoneTransforms();
	TestEqual(TEXT("Possession applies the template's lower view limit to the real camera manager"),
		ViewManager->ViewPitchMin, -AfpstrueCharacter::ViewPitchLimit);
	TestEqual(TEXT("Possession applies the template's upper view limit to the real camera manager"),
		ViewManager->ViewPitchMax, AfpstrueCharacter::ViewPitchLimit);
	for (const float Sign : {1.0f, -1.0f})
	{
		ViewController->SetControlRotation(FRotator::ZeroRotator);
		ViewController->RotationInput = FRotator(Sign * 120.0f, 0.0f, 0.0f);
		ViewController->UpdateRotation(1.0f / 60.0f);
		TestTrue(TEXT("The actual UpdateRotation path clamps an oversized pitch input"),
			FMath::IsNearlyEqual(FRotator::NormalizeAxis(ViewController->GetControlRotation().Pitch),
				Sign * AfpstrueCharacter::ViewPitchLimit, 0.01f));
		ViewController->RotationInput = FRotator::ZeroRotator;
		if (!Fixture.TickTestWorld(1.0f / 60.0f)) { Fixture.ForwardErrorMessages(this); return false; }
		ViewCharacter->GetMesh()->TickAnimation(0.0f, false);
		ViewCharacter->GetMesh()->RefreshBoneTransforms();
		const float ActualAimPitch = FRotator::NormalizeAxis(ViewCharacter->GetBaseAimRotation().Pitch);
		TestTrue(TEXT("The real gameplay view stays inside the same pitch limits while standalone camera lag settles"),
			FMath::Abs(ActualAimPitch) <= AfpstrueCharacter::ViewPitchLimit + 0.1f);
		TestTrue(TEXT("The upper-body aim snapshot consumes that same actual gameplay view"),
			FMath::IsNearlyEqual(ViewAnimation->AimPitch, ActualAimPitch, 0.1f));
	}
	for (const float Sign : {1.0f, -1.0f})
	{
		ViewController->SetControlRotation(FRotator::ZeroRotator);
		ViewController->RotationInput = FRotator(0.0f, Sign * 120.0f, 0.0f);
		ViewController->UpdateRotation(1.0f / 60.0f);
		TestTrue(TEXT("Standing horizontal look is limited relative to the retained foot direction"),
			FMath::IsNearlyEqual(FRotator::NormalizeAxis(ViewController->GetControlRotation().Yaw),
				Sign * AfpstrueCharacter::ViewYawLimit, 0.01f));
	}
	ViewCharacter->GetCharacterMovement()->Velocity = FVector(300.0f, 0.0f, 0.0f);
	ViewCharacter->GetMesh()->TickAnimation(1.0f / 60.0f, false);
	ViewCharacter->GetMesh()->RefreshBoneTransforms();
	TestTrue(TEXT("Actual ground movement releases the stationary horizontal view cone"),
		!ViewCharacter->GetBodyFacingState().bStationary && FMath::IsNearlyZero(ViewManager->ViewYawMin)
		&& FMath::IsNearlyEqual(ViewManager->ViewYawMax, 359.999f, 0.001f));
	ViewController->SetControlRotation(FRotator(0.0f, 170.0f, 0.0f));
	ViewController->RotationInput = FRotator(0.0f, 40.0f, 0.0f);
	ViewController->UpdateRotation(1.0f / 60.0f);
	TestTrue(TEXT("Horizontal look crosses the signed 180-degree boundary without a yaw cone"),
		FMath::IsNearlyEqual(FRotator::NormalizeAxis(ViewController->GetControlRotation().Yaw), -150.0f, 0.01f));
	ViewController->SetControlRotation(FRotator::ZeroRotator);
	ViewController->RotationInput = FRotator(0.0f, 765.0f, 0.0f);
	ViewController->UpdateRotation(1.0f / 60.0f);
	TestTrue(TEXT("Horizontal look preserves complete revolutions rather than accumulating a turn limit"),
		FMath::IsNearlyEqual(FRotator::NormalizeAxis(ViewController->GetControlRotation().Yaw), 45.0f, 0.01f));
	ViewController->RotationInput = FRotator::ZeroRotator;
	ViewCharacter->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	ViewCharacter->GetMesh()->TickAnimation(1.0f / 60.0f, false);
	ViewCharacter->GetMesh()->RefreshBoneTransforms();
	// 新控制器接管同一角色也走真实占有入口；不额外调用 ClientRestart 或重新初始化整个输入生命周期。
	ViewController->UnPossess();
	const auto* ViewDefaults = ViewManager->GetClass()->GetDefaultObject<APlayerCameraManager>();
	TestTrue(TEXT("The relinquished controller restores its own camera-class view limits rather than retaining the old pawn cone"),
		ViewManager->ViewPitchMin == ViewDefaults->ViewPitchMin && ViewManager->ViewPitchMax == ViewDefaults->ViewPitchMax
		&& ViewManager->ViewYawMin == ViewDefaults->ViewYawMin && ViewManager->ViewYawMax == ViewDefaults->ViewYawMax);
	auto* ReplacementController = Fixture.GetTestWorld()->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("View-limit fixture can create a replacement local controller"), ReplacementController)) return false;
	ReplacementController->Possess(ViewCharacter);
	if (!TestNotNull(TEXT("Replacement controller retains its own camera manager"), ReplacementController->PlayerCameraManager.Get())) return false;
	TestTrue(TEXT("A replacement local controller receives the same view limits on normal possession"),
		ReplacementController->PlayerCameraManager->ViewPitchMin == -AfpstrueCharacter::ViewPitchLimit
		&& ReplacementController->PlayerCameraManager->ViewPitchMax == AfpstrueCharacter::ViewPitchLimit);
	ViewCharacter->GetMesh()->TickAnimation(1.0f / 60.0f, false);
	ViewCharacter->GetMesh()->RefreshBoneTransforms();
	TestTrue(TEXT("The actual body animation restores the stationary view cone on the replacement controller"),
		FMath::IsNearlyEqual(ReplacementController->PlayerCameraManager->ViewYawMin,
			ViewCharacter->GetBodyFacingState().FacingYaw - AfpstrueCharacter::ViewYawLimit, 0.01f)
		&& FMath::IsNearlyEqual(ReplacementController->PlayerCameraManager->ViewYawMax,
			ViewCharacter->GetBodyFacingState().FacingYaw + AfpstrueCharacter::ViewYawLimit, 0.01f));
	ReplacementController->UnPossess();
	ViewCharacter->SetRole(ROLE_SimulatedProxy);
	ViewCharacter->SetActorRotation(FRotator::ZeroRotator);
	for (const float Pitch : {-89.0f, -75.0f, 75.0f, 89.0f})
	{
		ViewCharacter->SetRemoteViewPitch(Pitch);
		const FRotator ReplicatedAim = ViewCharacter->GetBaseAimRotation();
		ViewCharacter->GetMesh()->TickAnimation(0.0f, false);
		ViewCharacter->GetMesh()->RefreshBoneTransforms();
		const float ExpectedPitch = FMath::Clamp(FRotator::NormalizeAxis(ReplicatedAim.Pitch),
			-AfpstrueCharacter::ViewPitchLimit, AfpstrueCharacter::ViewPitchLimit);
		TestTrue(TEXT("Remote upper-body aim clamps the actual quantized view rather than inventing another view source"),
			FMath::IsNearlyEqual(ViewAnimation->AimPitch, ExpectedPitch, 0.01f));
		TestTrue(TEXT("Within-limit remote view remains consistent to one compressed pitch step"),
			FMath::Abs(ViewAnimation->AimPitch - FMath::Clamp(Pitch,
				-AfpstrueCharacter::ViewPitchLimit, AfpstrueCharacter::ViewPitchLimit)) <= 360.0f / 256.0f);
		TestTrue(TEXT("Remote animation sampling never writes back the received gameplay aim"),
			ViewCharacter->GetBaseAimRotation().Equals(ReplicatedAim, 0.001f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerRemoteLocomotionAnimationTest,
	"fpstrue.Multiplayer.Animation.RemoteVelocityWithoutAcceleration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 固定远端已接收的速度快照，驱动真实 AnimBP；不手写动画变量，也不冒充网络传输或画面观感验收。
bool FFpstrueMultiplayerRemoteLocomotionAnimationTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	// 此用例保留 RAR 零偏移/独立枪链回归；当前默认 Template 的非零作者偏移由独立门禁验证。
	UClass* PawnClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter.BP_RARCharacter_C"));
	if (!TestNotNull(TEXT("Remote fixture retains the real RAR presentation Pawn class"), PawnClass)
		|| !TestTrue(TEXT("Remote fixture Pawn class derives from the project character"), PawnClass->IsChildOf<AfpstrueCharacter>())) return false;
	AfpstrueCharacter* Character = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>(PawnClass);
	if (!TestNotNull(TEXT("Remote animation fixture is the real project character"), Character)) return false;
	TestTrue(TEXT("Runtime fixture executes the actual configured Blueprint class"), Character->GetClass() == PawnClass);
	// 不先推进动画：装备完成后的首发必须立即进入身体动画，不能靠夹具提前 Tick 掩盖事件漏绑。
	AfpstrueRARWeapon* RemoteRifle = Fixture.GetTestWorld()->SpawnActor<AfpstrueRARWeapon>(FVector(5000.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Remote fixture has its real paired RAR rifle"), RemoteRifle)) return false;
	UfpstrueWeaponComponent* RemoteWeapon = RemoteRifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("Remote fixture has its production weapon component"), RemoteWeapon)
		|| !TestTrue(TEXT("Remote fixture equips through the authority transaction before receiving its proxy role"), RemoteWeapon->AttachWeapon(Character))) return false;
	Character->SetRole(ROLE_SimulatedProxy);
	RemoteRifle->SetRole(ROLE_SimulatedProxy);
	RemoteWeapon->RefreshNetworkEquipment();
	USkeletalMeshComponent* Body = Character->GetMesh();
	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	UfpstruePlayerAnimInstance* Animation = Cast<UfpstruePlayerAnimInstance>(Body->GetAnimInstance());
	if (!TestNotNull(TEXT("Real body AnimBlueprint initializes in the test world"), Animation)) return false;
	TestNull(TEXT("Remote presentation fixture has no local Controller"), Character->GetController());
	TestTrue(TEXT("A non-local player's world gun attaches to its complete body, not the first-person arms or root"),
		RemoteWeapon->GetAttachParent() == Body && RemoteWeapon->GetAttachSocketName() == TEXT("SOCKET_Weapon"));
	TestTrue(TEXT("The equipped RAR rifle uses socket-relative presentation rather than absolute transforms or simulated physics"),
		!RemoteWeapon->IsUsingAbsoluteLocation() && !RemoteWeapon->IsUsingAbsoluteRotation() && !RemoteWeapon->IsSimulatingPhysics());
	// Actor/组件复制可先恢复父级，再送来旧相对变换；同父级同插槽的 AttachTo 会提前返回，不能只验父子关系。
	const FVector AuthoredScale(0.9f, 0.95f, 1.1f);
	RemoteWeapon->SetRelativeScale3D(AuthoredScale);
	RemoteWeapon->SetRelativeLocationAndRotation(FVector(-3000.0f, 3500.0f, 550.0f), FRotator(15.0f, 90.0f, 0.0f));
	RemoteWeapon->RefreshNetworkEquipment();
	TestTrue(TEXT("A repeated same-socket refresh clears a stale replicated location and rotation"),
		RemoteWeapon->GetRelativeLocation().IsNearlyZero(0.01f) && RemoteWeapon->GetRelativeRotation().IsNearlyZero(0.01f));
	TestTrue(TEXT("The remote gun is physically at its body socket, not merely attached to the correct parent"),
		FVector::Dist(RemoteWeapon->GetComponentLocation(), Body->GetSocketLocation(TEXT("SOCKET_Weapon"))) < 0.1f);
	TestTrue(TEXT("Correcting the socket transform preserves the authored relative scale"), RemoteWeapon->GetRelativeScale3D().Equals(AuthoredScale, 0.001f));
	RemoteWeapon->SetRelativeLocationAndRotation(FVector(4500.0f, -500.0f, 700.0f), FRotator(0.0f, 180.0f, 0.0f));
	RemoteRifle->OnRep_AttachmentReplication();
	TestTrue(TEXT("A late Actor attachment notification also restores the exact body socket transform"),
		RemoteWeapon->GetRelativeLocation().IsNearlyZero(0.01f) && RemoteWeapon->GetRelativeRotation().IsNearlyZero(0.01f)
		&& FVector::Dist(RemoteWeapon->GetComponentLocation(), Body->GetSocketLocation(TEXT("SOCKET_Weapon"))) < 0.1f);
	RemoteWeapon->SetRelativeScale3D(FVector::OneVector);
	TestTrue(TEXT("A remote player never displays another pair of first-person arms"), Character->GetMesh1P()->bHiddenInGame);
	RemoteWeapon->MulticastShot_Implementation(FVector::ZeroVector, FVector(1000.0f, 0.0f, 0.0f), FHitResult(), 1, 1, 1, 1);
	TestTrue(TEXT("The first received shot starts the remote body action without a preceding animation update"),
		Animation->Montage_IsPlaying(Animation->FireMontage));
	const UAnimInstance* RemoteArms = Character->GetMesh1P()->GetAnimInstance();
	TestTrue(TEXT("The remote shot cannot start the hidden first-person action"),
		RemoteArms == nullptr || !RemoteArms->Montage_IsPlaying(Animation->FireMontage));
	Animation->Montage_Stop(0.0f);
	const AfpstrueCharacter* PresentationDefaults = PawnClass->GetDefaultObject<AfpstrueCharacter>();
	TestTrue(TEXT("Spawned Blueprint retains its configured migrated body mesh"), Body->GetSkeletalMeshAsset() == PresentationDefaults->GetMesh()->GetSkeletalMeshAsset());
	TestTrue(TEXT("Spawned Blueprint retains its project RAR body AnimClass"), Body->GetAnimClass() == PresentationDefaults->GetMesh()->GetAnimClass());
	const IAnimClassInterface* BodyAnimation = IAnimClassInterface::GetFromClass(Body->GetAnimClass());
	if (!TestNotNull(TEXT("Spawned Blueprint uses a compiled body AnimBlueprint"), BodyAnimation)
		|| !TestNotNull(TEXT("Spawned Blueprint retains its body mesh"), Body->GetSkeletalMeshAsset())) return false;
	TestTrue(TEXT("Spawned Blueprint mesh and animation use the same real skeleton"), Body->GetSkeletalMeshAsset()->GetSkeleton() != nullptr
		&& Body->GetSkeletalMeshAsset()->GetSkeleton() == BodyAnimation->GetTargetSkeleton());
	TestTrue(TEXT("Spawned Blueprint preserves the first-person and third-person visibility split"),
		Character->GetMesh1P()->bOnlyOwnerSee && !Character->GetMesh1P()->bOwnerNoSee && Body->bOwnerNoSee && !Body->bOnlyOwnerSee);
	TestTrue(TEXT("The fixture uses the configured Blueprint, not a synthetic AnimInstance"),
		Animation->GetClass() == Body->GetAnimClass() && Animation->GetClass()->ClassGeneratedBy != nullptr);
	const auto UpdateBody = [&]()
	{
		// 不推进世界，避免空测试关卡的重力/碰撞覆盖输入快照；无 TickFunction 的骨骼刷新同步完成求值。
		Body->TickAnimation(1.0f / 30.0f, false);
		Body->RefreshBoneTransforms();
	};
	Movement->SetMovementMode(MOVE_Walking);
	Movement->Velocity = FVector(300.0f, 0.0f, 0.0f);
	UpdateBody();
	TestTrue(TEXT("A remote proxy does not require a locally controlled Pawn"), !Character->IsLocallyControlled());
	TestTrue(TEXT("The proxy has no local input acceleration"), Movement->GetCurrentAcceleration().IsNearlyZero());
	TestTrue(TEXT("Real AnimBlueprint reads received horizontal velocity"), FMath::IsNearlyEqual(Animation->GroundSpeed, 300.0f, 0.01f));
	TestTrue(TEXT("Received velocity alone enables locomotion without local acceleration"), Animation->MovementAlpha > 0.0f);
	TestFalse(TEXT("Grounded movement does not select falling"), Animation->bFalling);
	Movement->SetMovementMode(MOVE_Falling);
	Movement->Velocity = FVector(300.0f, 0.0f, 200.0f);
	UpdateBody();
	TestTrue(TEXT("Jump and fall selection follows the real movement mode"), Animation->bFalling && Animation->bJumpRising);
	TestTrue(TEXT("Vertical jump speed is not counted as horizontal locomotion speed"), FMath::IsNearlyEqual(Animation->GroundSpeed, 300.0f, 0.01f));
	TestTrue(TEXT("Airborne movement does not blend a grounded walk cycle"), FMath::IsNearlyZero(Animation->MovementAlpha));
	TestEqual(TEXT("Jump and fall do not inherit a nearly frozen locomotion play rate"), Animation->LocomotionPlayRate, 1.0f);
	Movement->SetMovementMode(MOVE_Walking);
	Movement->Velocity = FVector::ZeroVector;
	UpdateBody();
	TestTrue(TEXT("Stopping clears the animation's horizontal speed"), FMath::IsNearlyZero(Animation->GroundSpeed, 0.01f));
	TestTrue(TEXT("Stopping returns the real AnimBlueprint to idle"), FMath::IsNearlyZero(Animation->MovementAlpha));
	TestFalse(TEXT("Landing clears the animation's falling state"), Animation->bFalling);
	TestTrue(TEXT("Landing exposes a bounded visual transition instead of another movement state"), Animation->bLanding);
	TestEqual(TEXT("Idle uses the authored idle play rate rather than a five-percent walk rate"), Animation->LocomotionPlayRate, 1.0f);
	// 和远端属性复制一样固定姿态镜像；这里只验动画消费，不把模拟快照冒充能力授权或网络传输。
	const FBoolProperty* SprintMirror = FindFProperty<FBoolProperty>(Character->GetClass(), TEXT("bIsSprinting"));
	if (!TestNotNull(TEXT("Sprint retains its replicated presentation mirror"), SprintMirror)) return false;
	SprintMirror->SetPropertyValue_InContainer(Character, true);
	UpdateBody();
	TestEqual(TEXT("A stationary sprint toggle cannot raise the remote sprint pose"), Animation->SprintAlpha, 0.0f);
	Movement->Velocity = FVector(600.0f, 0.0f, 0.0f);
	for (int32 Frame = 0; Frame < 12; ++Frame) UpdateBody();
	TestTrue(TEXT("A genuinely moving remote sprint needs no unreplicated local acceleration"), Animation->SprintAlpha > 0.9f);
	Movement->Velocity = FVector(2.0f, -2.0f, 0.0f);
	const FVector CorrectionVelocity = Movement->Velocity;
	for (int32 Frame = 0; Frame < 3; ++Frame) UpdateBody();
	TestEqual(TEXT("Stopping blends sprint fully to zero within one tenth of a second"), Animation->SprintAlpha, 0.0f);
	TestTrue(TEXT("Sub-three-centimeter correction noise stays in the exact idle animation sample"),
		Animation->GroundSpeed == 0.0f && Animation->ForwardSpeed == 0.0f && Animation->RightSpeed == 0.0f
		&& Animation->MovementAlpha == 0.0f && Animation->LocomotionPlayRate == 1.0f);
	TestTrue(TEXT("Animation cleanup does not write the movement component's real velocity"), Movement->Velocity.Equals(CorrectionVelocity));
	Movement->Velocity = FVector(3.0f, 0.0f, 0.0f);
	UpdateBody();
	TestEqual(TEXT("The stationary threshold includes exactly three centimeters per second"), Animation->GroundSpeed, 0.0f);
	Movement->Velocity = FVector(4.0f, 0.0f, 0.0f);
	UpdateBody();
	TestEqual(TEXT("Movement above the dead zone is preserved rather than clamped out"), Animation->GroundSpeed, 4.0f);
	Movement->Velocity = FVector(600.0f, 0.0f, 0.0f);
	for (int32 Frame = 0; Frame < 12; ++Frame) UpdateBody();
	Movement->SetMovementMode(MOVE_Falling);
	Movement->Velocity = FVector(600.0f, 0.0f, 200.0f);
	for (int32 Frame = 0; Frame < 3; ++Frame) UpdateBody();
	TestTrue(TEXT("A sprint jump preserves the real airborne state while removing only the grounded sprint pose"),
		Animation->bFalling && Animation->bJumpRising && Animation->GroundSpeed == 600.0f
		&& Animation->SprintAlpha == 0.0f && Animation->MovementAlpha == 0.0f);
	SprintMirror->SetPropertyValue_InContainer(Character, false);
	Movement->SetMovementMode(MOVE_Walking);
	Movement->Velocity = FVector::ZeroVector;
	UpdateBody();
	// 走与复制相同的快照入口，确认远端完整身体与枪接收换弹，而不是向隐藏手臂重复播放。
	const FStructProperty* StateProperty = FindFProperty<FStructProperty>(RemoteWeapon->GetClass(), TEXT("NetworkState"));
	UFunction* ReloadNotify = RemoteWeapon->FindFunction(TEXT("OnRep_NetworkState"));
	if (!TestNotNull(TEXT("Remote reload retains its replicated snapshot"), StateProperty)
		|| !TestNotNull(TEXT("Remote reload retains its production RepNotify"), ReloadNotify)) return false;
	FFPReplicatedWeaponState* State = StateProperty->ContainerPtrToValuePtr<FFPReplicatedWeaponState>(RemoteWeapon);
	State->Holder = Character;
	State->Action = EFPWeaponActionState::Reloading;
	State->ReloadId = 91;
	State->Ammo = 7;
	State->Reserve = 32;
	State->ReloadDuration = RemoteWeapon->GetWeaponSettings().ReloadDuration;
	State->ReloadStartServerTime = Fixture.GetTestWorld()->GetTimeSeconds();
	State->Revision += 10;
	RemoteWeapon->ProcessEvent(ReloadNotify, nullptr);
	TestTrue(TEXT("The received reload starts on the complete remote body"), Animation->Montage_IsPlaying(Animation->ReloadMontage));
	const UAnimInstance* RemoteGun = RemoteWeapon->GetAnimInstance();
	TestTrue(TEXT("The same received reload starts the paired RAR gun mechanics"),
		RemoteGun != nullptr && RemoteGun->Montage_IsPlaying(RemoteWeapon->WeaponReloadMontage));
	TestTrue(TEXT("Received remote reload does not play the hidden first-person arms"),
		RemoteArms == nullptr || !RemoteArms->Montage_IsPlaying(Animation->ReloadMontage));
	TestEqual(TEXT("Remote animation does not independently commit ammunition"), RemoteWeapon->GetCurrentAmmo(), 7);
	for (int32 Frame = 0; Frame < 3; ++Frame) UpdateBody();
	TestTrue(TEXT("Returning locomotion to idle cannot stop the received reload montage"), Animation->Montage_IsPlaying(Animation->ReloadMontage));
	TestTrue(TEXT("Returning locomotion to idle cannot cancel the weapon's reload transaction"), RemoteWeapon->IsReloading());
	TestEqual(TEXT("Idle sampling does not manufacture ammunition during reload"), RemoteWeapon->GetCurrentAmmo(), 7);
	// 本地只固定 CMC 采样后的 Acceleration/Velocity，覆盖松键惯性；不伪造真实按键或移动距离验收。
	AfpstrueCharacter* LocalCharacter = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>(PawnClass, FVector(10000.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	APlayerController* LocalController = Fixture.GetTestWorld()->SpawnActor<APlayerController>();
	AfpstrueRARWeapon* LocalRifle = Fixture.GetTestWorld()->SpawnActor<AfpstrueRARWeapon>(FVector(15000.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Local animation fixture retains the same real character Blueprint"), LocalCharacter)
		|| !TestNotNull(TEXT("Local animation fixture has a real local Controller"), LocalController)
		|| !TestNotNull(TEXT("Local animation fixture has its paired RAR rifle"), LocalRifle)) return false;
	LocalController->Possess(LocalCharacter);
	UfpstrueWeaponComponent* LocalWeapon = LocalRifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestTrue(TEXT("The local fixture is locally controlled"), LocalCharacter->IsLocallyControlled())
		|| !TestNotNull(TEXT("The local rifle retains its real weapon component"), LocalWeapon)
		|| !TestTrue(TEXT("The local fixture uses the normal equip transaction"), LocalWeapon->AttachWeapon(LocalCharacter))) return false;
	UCharacterMovementComponent* LocalMovement = LocalCharacter->GetCharacterMovement();
	UfpstruePlayerAnimInstance* LocalAnimation = Cast<UfpstruePlayerAnimInstance>(LocalCharacter->GetMesh()->GetAnimInstance());
	const FStructProperty* AccelerationSnapshot = FindFProperty<FStructProperty>(LocalMovement->GetClass(), TEXT("Acceleration"));
	if (!TestNotNull(TEXT("The local fixture runs the real paired AnimBlueprint"), LocalAnimation)
		|| !TestNotNull(TEXT("CMC retains its real input acceleration snapshot"), AccelerationSnapshot)
		|| !TestTrue(TEXT("Local sprint is activated by the actual GAS ability"),
			LocalCharacter->GetHealthComponent()->TryActivateAbilityByClass(UfpstrueSprintAbility::StaticClass()))) return false;
	const auto UpdateLocalBody = [&]()
	{
		LocalCharacter->GetMesh()->TickAnimation(1.0f / 30.0f, false);
		LocalCharacter->GetMesh()->RefreshBoneTransforms();
	};
	LocalMovement->SetMovementMode(MOVE_Walking);
	LocalMovement->Velocity = FVector(600.0f, 0.0f, 0.0f);
	FVector* LocalAcceleration = AccelerationSnapshot->ContainerPtrToValuePtr<FVector>(LocalMovement);
	*LocalAcceleration = FVector::ZeroVector;
	UpdateLocalBody();
	TestEqual(TEXT("Residual local velocity without movement input does not raise a sprint pose"), LocalAnimation->SprintAlpha, 0.0f);
	*LocalAcceleration = FVector(LocalMovement->GetMaxAcceleration(), 0.0f, 0.0f);
	for (int32 Frame = 0; Frame < 12; ++Frame) UpdateLocalBody();
	TestTrue(TEXT("Local input plus real ground movement raises the authorized sprint pose"), LocalAnimation->SprintAlpha > 0.9f);
	*LocalAcceleration = FVector::ZeroVector;
	for (int32 Frame = 0; Frame < 3; ++Frame) UpdateLocalBody();
	TestEqual(TEXT("Releasing movement removes the local sprint pose even while deceleration retains speed"), LocalAnimation->SprintAlpha, 0.0f);
	TestEqual(TEXT("Animation cleanup leaves local physical speed under CMC control"), LocalMovement->Velocity.X, 600.0);
	TestTrue(TEXT("Stopping the visual sprint does not cancel the player's toggle ability"),
		LocalCharacter->IsSprinting() && LocalCharacter->GetHealthComponent()->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting));
	return true;
}

namespace
{
// RAR 表现用例使用真正配置的角色、枪械和动画图；不复用无渲染资源的单骨业务夹具。
bool SpawnRARPresentationFixture(UWorld& World, FAutomationTestBase& Test,
	AfpstrueCharacter*& Character, UfpstrueWeaponComponent*& Weapon)
{
	UClass* PawnClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter.BP_RARCharacter_C"));
	if (!Test.TestNotNull(TEXT("RAR fixture retains its explicit paired Pawn class independently of the current default"), PawnClass)) return false;
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Character = World.SpawnActor<AfpstrueCharacter>(PawnClass, FTransform::Identity, SpawnParameters);
	AAIController* Controller = World.SpawnActor<AAIController>();
	AfpstrueRARWeapon* Rifle = World.SpawnActor<AfpstrueRARWeapon>(AfpstrueRARWeapon::StaticClass(),
		FTransform(FVector(5000.0f, 0.0f, 0.0f)), SpawnParameters);
	if (!Test.TestNotNull(TEXT("RAR fixture character"), Character)
		|| !Test.TestNotNull(TEXT("RAR fixture controller"), Controller)
		|| !Test.TestNotNull(TEXT("RAR fixture rifle"), Rifle)) return false;
	Character->SetActorEnableCollision(false);
	Character->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->SetActorTickEnabled(false);
	Controller->Possess(Character);
	Weapon = Rifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!Test.TestNotNull(TEXT("RAR fixture uses the production weapon component"), Weapon)
		|| !Test.TestTrue(TEXT("RAR fixture completes the real equipment transaction"), Weapon->AttachWeapon(Character))) return false;
	for (USkeletalMeshComponent* Mesh : {Character->GetMesh(), Character->GetMesh1P(), static_cast<USkeletalMeshComponent*>(Weapon)})
	{
		Mesh->SetComponentTickEnabled(false);
		Mesh->TickAnimation(0.016f, false);
		Mesh->RefreshBoneTransforms();
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerRARAssetsTest,
	"fpstrue.Multiplayer.Animation.RARAssetsAndMontageBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 资源存在、同骨架、Slot 和附件完整才算接线完成；NullRHI 不能证明手握枪和画面观感已通过。
bool FFpstrueMultiplayerRARAssetsTest::RunTest(const FString&)
{
	UClass* PawnClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter.BP_RARCharacter_C"));
	if (!TestNotNull(TEXT("RAR asset gate retains its original paired Pawn Blueprint"), PawnClass)) return false;
	const AfpstrueCharacter* Character = PawnClass->GetDefaultObject<AfpstrueCharacter>();
	const USkeletalMeshComponent* ConfiguredArms = Character->GetMesh1P();
	TestTrue(TEXT("RAR arms remain directly attached to the gameplay camera"),
		ConfiguredArms->GetAttachParent() == Character->GetFirstPersonCameraComponent());
	// 源相机挂在手臂 head 的 SOCKET_Camera；这里校验逆向得到的 Camera→Arms，而非复制源 SpringArm→Arms。
	TestTrue(TEXT("RAR camera-relative arms location matches the authored head/socket layout"),
		ConfiguredArms->GetRelativeLocation().Equals(FVector(-0.66443537, 0.0, -162.57509561), 0.001));
	TestTrue(TEXT("RAR camera-relative arms rotation and scale retain the authored presentation"),
		ConfiguredArms->GetRelativeRotation().Equals(FRotator(0.0, -90.0, 0.0), 0.01)
		&& ConfiguredArms->GetRelativeScale3D().Equals(FVector::OneVector, 0.001));
	const AfpstrueRARWeapon* Rifle = GetDefault<AfpstrueRARWeapon>();
	UfpstrueWeaponComponent* Weapon = Rifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("RAR native rifle has a weapon mesh component"), Weapon)) return false;
	const USkeletalMesh* BodyMesh = Character->GetMesh()->GetSkeletalMeshAsset();
	if (!TestNotNull(TEXT("RAR character has a full body mesh"), BodyMesh)) return false;
	TestEqual(TEXT("RAR remote body uses the complete source-template Manny mesh"), BodyMesh->GetPathName(),
		FString(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny.SKM_Manny")));
	TestNotNull(TEXT("Complete Manny retains its authored post-process animation"), BodyMesh->GetPostProcessAnimBlueprint().Get());
	TestNotNull(TEXT("Complete Manny retains a body PhysicsAsset for authoritative hits"), BodyMesh->GetPhysicsAsset());
	const USkeleton* CharacterSkeleton = BodyMesh->GetSkeleton();
	if (!TestNotNull(TEXT("RAR body has a real skeleton"), CharacterSkeleton)) return false;
	for (USkeletalMeshComponent* Mesh : {Character->GetMesh(), Character->GetMesh1P(), static_cast<USkeletalMeshComponent*>(Weapon)})
	{
		if (!TestNotNull(TEXT("Every RAR visual component has its mesh"), Mesh->GetSkeletalMeshAsset())
			|| !TestNotNull(TEXT("Every RAR visual component has an animation class"), Mesh->GetAnimClass())) return false;
		const IAnimClassInterface* Animation = IAnimClassInterface::GetFromClass(Mesh->GetAnimClass());
		if (!TestNotNull(TEXT("Every RAR animation class is a compiled graph"), Animation)) return false;
		TestTrue(TEXT("Each graph targets its own mesh skeleton"), Animation->GetTargetSkeleton() == Mesh->GetSkeletalMeshAsset()->GetSkeleton());
		TestFalse(TEXT("RAR visual meshes do not silently fall back to RU74 assets"), Mesh->GetSkeletalMeshAsset()->GetPathName().Contains(TEXT("FPS_Assault_Pack")));
	}
	TestEqual(TEXT("RAR rifle has its matching weapon mesh"), Weapon->GetSkeletalMeshAsset()->GetPathName(),
		FString(TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SK_RAR_AssaultRifle.SK_RAR_AssaultRifle")));
	const FFPWeaponSettings& Settings = Weapon->GetWeaponSettings();
	TestTrue(TEXT("The configured grip exists on both arms and body"), Character->GetMesh1P()->DoesSocketExist(Settings.GripSocketName)
		&& Character->GetMesh()->DoesSocketExist(Settings.GripSocketName));
	TestEqual(TEXT("RAR grip follows the authored independent weapon IK chain"), Settings.GripSocketName, FName(TEXT("SOCKET_Weapon")));
	TestTrue(TEXT("RAR normal and empty reload use the paired authored timings"), FMath::IsNearlyEqual(Settings.ReloadDuration, 2.3666667f, 0.001f)
		&& FMath::IsNearlyEqual(Settings.EmptyReloadDuration, 2.8f, 0.001f));
	TArray<UAnimMontage*> Montages;
	for (USkeletalMeshComponent* Mesh : {Character->GetMesh(), Character->GetMesh1P()})
	{
		if (!TestTrue(TEXT("Body and arms share the native read-only presentation entry"), Mesh->GetAnimClass()->IsChildOf<UfpstruePlayerAnimInstance>())) return false;
		const auto* Animation = Mesh->GetAnimClass()->GetDefaultObject<UfpstruePlayerAnimInstance>();
		for (UAnimMontage* Montage : {Animation->FireMontage.Get(), Animation->AimedFireMontage.Get(), Animation->ReloadMontage.Get(),
			Animation->EmptyReloadMontage.Get(), Animation->EquipMontage.Get()})
		{
			if (!TestNotNull(TEXT("RAR body/arms has every required action montage"), Montage)) return false;
			TestTrue(TEXT("Body/arms action uses the exact character skeleton"), Montage->GetSkeleton() == CharacterSkeleton);
			Montages.AddUnique(Montage);
		}
	}
	for (UAnimMontage* Montage : {Weapon->WeaponFireMontage.Get(), Weapon->WeaponReloadMontage.Get(), Weapon->WeaponEmptyReloadMontage.Get()})
	{
		if (!TestNotNull(TEXT("RAR gun has each paired mechanical action montage"), Montage)) return false;
		TestTrue(TEXT("Mechanical action uses the gun skeleton, never the human skeleton"), Montage->GetSkeleton() == Weapon->GetSkeletalMeshAsset()->GetSkeleton()
			&& Montage->GetSkeleton() != CharacterSkeleton);
		Montages.AddUnique(Montage);
	}
	for (const UAnimMontage* Montage : Montages)
	{
		TestTrue(TEXT("A project montage has a positive finite playback length"), FMath::IsFinite(Montage->GetPlayLength()) && Montage->GetPlayLength() > 0.0f);
		TestTrue(TEXT("A project montage actually contains an animation slot"), !Montage->SlotAnimTracks.IsEmpty());
		for (const FAnimNotifyEvent& Event : Montage->Notifies)
		{
			TestTrue(TEXT("Montage notifications do not import vendor rules or duplicate audiovisual effects"),
				Event.Notify != nullptr && Event.Notify->IsA<UfpstrueAnimNotify_ReloadCommit>() && Event.NotifyStateClass == nullptr);
		}
		for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
		{
			TestEqual(TEXT("Project actions use the same slot consumed by all three animation graphs"), Slot.SlotName, FName(TEXT("RARAction")));
			TestTrue(TEXT("Action slot is declared on its target skeleton"), Montage->GetSkeleton()->ContainsSlotName(Slot.SlotName));
			TestTrue(TEXT("Action slot contains its authored clip"), !Slot.AnimTrack.AnimSegments.IsEmpty());
			for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
			{
				const UAnimSequenceBase* Clip = Segment.GetAnimReference();
				if (!TestNotNull(TEXT("Montage segment resolves its source clip"), Clip)) return false;
				TestTrue(TEXT("Source clip and montage target the same skeleton"), Clip->GetSkeleton() == Montage->GetSkeleton());
				TestEqual(TEXT("Source clips have no hidden vendor Sound, FX or gameplay notifications"), Clip->Notifies.Num(), 0);
			}
		}
	}
	TArray<UStaticMeshComponent*> Parts;
	Rifle->GetComponents(Parts);
	TestEqual(TEXT("RAR rifle retains all four essential visible attachments"), Parts.Num(), 4);
	for (const UStaticMeshComponent* Part : Parts)
	{
		if (!TestNotNull(TEXT("RAR attachment has its actual static mesh"), Part->GetStaticMesh().Get())
			|| !TestNotNull(TEXT("RAR attachment has its component parent"), Part->GetAttachParent())) return false;
		TestTrue(TEXT("RAR attachment uses an authored parent socket/bone"), Part->GetAttachParent()->DoesSocketExist(Part->GetAttachSocketName()));
		TestTrue(TEXT("Attachments never add gameplay-blocking collision"), Part->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	}
	TestNotNull(TEXT("RAR fire keeps its audible feedback"), Weapon->FireSound.Get());
	TestNotNull(TEXT("RAR fire keeps its muzzle feedback"), Weapon->MuzzleEffect.Get());
	TestTrue(TEXT("Gun muzzle comes from a real attachment socket"), Weapon->MuzzleSource != nullptr
		&& Weapon->MuzzleSource->DoesSocketExist(Weapon->MuzzleSocketName));
	// 材质只能在真实游戏实例上创建；本地视模与世界模型不能共用一个会投影变形的材质状态。
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	AfpstrueCharacter* RuntimeCharacter = nullptr;
	UfpstrueWeaponComponent* RuntimeWeapon = nullptr;
	if (!SpawnRARPresentationFixture(*Fixture.GetTestWorld(), *this, RuntimeCharacter, RuntimeWeapon)) return false;
	const FObjectPropertyBase* ViewmodelCollection = FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("ViewmodelFOVCollection"));
	auto* RuntimeArmsAnimation = Cast<UfpstruePlayerAnimInstance>(RuntimeCharacter->GetMesh1P()->GetAnimInstance());
	auto* RuntimeBodyAnimation = Cast<UfpstruePlayerAnimInstance>(RuntimeCharacter->GetMesh()->GetAnimInstance());
	if (!TestNotNull(TEXT("RAR material collection check retains the native animation property"), ViewmodelCollection)
		|| !TestNotNull(TEXT("RAR material collection check uses a real first-person animation instance"), RuntimeArmsAnimation)
		|| !TestNotNull(TEXT("RAR material collection check uses a real body animation instance"), RuntimeBodyAnimation)) return false;
	TestTrue(TEXT("The real RAR material collection fixture is not a dedicated server"), RuntimeCharacter->GetNetMode() != NM_DedicatedServer);
	const UObject* RuntimeCollection = ViewmodelCollection->GetObjectPropertyValue_InContainer(RuntimeArmsAnimation);
	if (!TestNotNull(TEXT("Only the real RAR first-person instance acquires its authored material collection"), RuntimeCollection)) return false;
	TestEqual(TEXT("The runtime RAR viewmodel collection resolves the actual Infima resource"), RuntimeCollection->GetPathName(),
		FString(TEXT("/Game/InfimaGames/ArtCore/MaterialLibrary/MPC_IG_FOV.MPC_IG_FOV")));
	TestNull(TEXT("The real RAR third-person body does not retain the first-person-only material collection"),
		ViewmodelCollection->GetObjectPropertyValue_InContainer(RuntimeBodyAnimation));
	FMinimalViewInfo View;
	RuntimeCharacter->CalcCamera(0.0f, View);
	TestTrue(TEXT("RAR camera uses its bounded one-centimetre near clip"), FMath::IsNearlyEqual(View.PerspectiveNearClipPlane, 1.0f, 0.001f));
	AfpstrueCharacter* LegacyCharacter = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Non-RAR camera fixture remains available"), LegacyCharacter)) return false;
	FMinimalViewInfo LegacyView;
	LegacyCharacter->CalcCamera(0.0f, LegacyView);
	TestTrue(TEXT("RAR near clip does not change the original character camera"), LegacyView.PerspectiveNearClipPlane < 0.0f);
	const auto CheckMaterialMode = [&](UMeshComponent* Mesh, float Expected, bool bRequireParameter)
	{
		if (!TestNotNull(TEXT("Runtime FOV gate retains its mesh component"), Mesh)) return;
		TestTrue(TEXT("Runtime FOV gate covers actual material slots"), Mesh->GetNumMaterials() > 0);
		for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
		{
			UMaterialInterface* Material = Mesh->GetMaterial(Slot);
			if (!TestNotNull(TEXT("Runtime visual slot resolves its material"), Material)) continue;
			float Value = -1.0f;
			const bool bHasParameter = Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Field Of View Alpha")), Value);
			TestTrue(TEXT("First-person material has its authored FOV-alpha parameter"), !bRequireParameter || bHasParameter);
			if (bHasParameter)
			{
				TestTrue(TEXT("Per-mesh FOV mode belongs to a dynamic instance, not a shared source asset"), Material->IsA<UMaterialInstanceDynamic>());
				TestTrue(TEXT("Runtime material uses the expected local/world FOV mode"), FMath::IsNearlyEqual(Value, Expected, 0.001f));
			}
		}
	};
	CheckMaterialMode(RuntimeCharacter->GetMesh(), 0.0f, false);
	CheckMaterialMode(RuntimeCharacter->GetMesh1P(), 1.0f, true);
	TArray<UMeshComponent*> RuntimeGunMeshes;
	RuntimeWeapon->GetOwner()->GetComponents(RuntimeGunMeshes);
	TestEqual(TEXT("Runtime local FOV gate includes gun and all four attachments"), RuntimeGunMeshes.Num(), 5);
	for (UMeshComponent* Mesh : RuntimeGunMeshes) CheckMaterialMode(Mesh, 1.0f, true);
	UMaterialInterface* LocalGunMaterial = RuntimeWeapon->GetMaterial(0);
	RuntimeWeapon->RefreshNetworkEquipment();
	TestEqual(TEXT("Repeated equipment refresh reuses the local gun material instance"), RuntimeWeapon->GetMaterial(0), LocalGunMaterial);
	// 本用例只检查未持有世界枪的材质；远离无手臂的原生相机夹具，避免拾取 Overlap 干扰这一断言。
	AfpstrueRARWeapon* WorldRifle = Fixture.GetTestWorld()->SpawnActor<AfpstrueRARWeapon>(FVector(10000.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Unheld world rifle uses a separate real actor instance"), WorldRifle)) return false;
	TArray<UMeshComponent*> WorldGunMeshes;
	WorldRifle->GetComponents(WorldGunMeshes);
	TestEqual(TEXT("World FOV gate includes gun and all four attachments"), WorldGunMeshes.Num(), 5);
	for (UMeshComponent* Mesh : WorldGunMeshes) CheckMaterialMode(Mesh, 0.0f, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerTemplateAssetsTest,
	"fpstrue.Multiplayer.Animation.TemplateAssetsGripAndOriginalHud",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 当前默认素材独立验收；保留 RAR 的零偏移/材质回归，不让切换默认类掩盖旧资产或新握持偏移错误。
bool FFpstrueMultiplayerTemplateAssetsTest::RunTest(const FString&)
{
	const auto* Mode = GetDefault<AfpstruePvpGameMode>();
	UClass* PawnClass = Mode->DefaultPawnClass.Get();
	if (!TestNotNull(TEXT("The current default template Pawn exists"), PawnClass)
		|| !TestTrue(TEXT("The template Pawn derives directly from the project's native character, not supplier gameplay"),
			PawnClass->GetSuperClass() == AfpstrueCharacter::StaticClass())) return false;
	TestEqual(TEXT("GameMode selects the new project-owned template Blueprint"), PawnClass->GetPathName(),
		FString(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C")));
	const auto* WeaponClass = FindFProperty<FClassProperty>(Mode->GetClass(), TEXT("DefaultWeaponClass"));
	if (!TestNotNull(TEXT("The current GameMode exposes its weapon class"), WeaponClass)) return false;
	TestTrue(TEXT("GameMode selects the native template weapon rather than a supplier weapon Blueprint"),
		WeaponClass->GetObjectPropertyValue_InContainer(Mode) == AfpstrueTemplateWeapon::StaticClass());
	const auto* Character = PawnClass->GetDefaultObject<AfpstrueCharacter>();
	TestTrue(TEXT("The template enables native presentation without RAR-specific viewmodel processing"),
		Character->UsesNativePresentation() && !Character->UsesRARPresentation());
	const FString BodyPath(TEXT("/Game/FPSMPT55/Demo/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	const FString GraphPaths[] = {
		TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateBody.ABP_TemplateBody_C"),
		TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateArms.ABP_TemplateArms_C")};
	USkeletalMeshComponent* HumanMeshes[] = {Character->GetMesh(), Character->GetMesh1P()};
	TArray<UAnimMontage*> Montages;
	USkeleton* CharacterSkeleton = nullptr;
	const FObjectPropertyBase* ViewmodelCollection = FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("ViewmodelFOVCollection"));
	if (!TestNotNull(TEXT("Template material isolation uses the reflected native animation property"), ViewmodelCollection)) return false;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(HumanMeshes); ++Index)
	{
		USkeletalMeshComponent* Mesh = HumanMeshes[Index];
		if (!TestNotNull(TEXT("Each template view has its skeletal mesh component"), Mesh)
			|| !TestNotNull(TEXT("Each template view has the selected skeletal mesh"), Mesh->GetSkeletalMeshAsset())
			|| !TestNotNull(TEXT("Each template view has its project animation graph"), Mesh->GetAnimClass())) return false;
		TestEqual(TEXT("Both template views retain the matching source full-skeleton mesh"), Mesh->GetSkeletalMeshAsset()->GetPathName(), BodyPath);
		TestEqual(TEXT("Each template view uses its separate project-owned animation graph"), Mesh->GetAnimClass()->GetPathName(), GraphPaths[Index]);
		TestTrue(TEXT("Both views expose the authored hand_r attachment"), Mesh->DoesSocketExist(TEXT("hand_r")));
		const auto* Graph = IAnimClassInterface::GetFromClass(Mesh->GetAnimClass());
		if (!TestNotNull(TEXT("Each template graph is a compiled AnimBlueprint"), Graph)
			|| !TestTrue(TEXT("Each template graph inherits the shared native read-only animation interface"),
				Mesh->GetAnimClass()->IsChildOf<UfpstruePlayerAnimInstance>())) return false;
		CharacterSkeleton = Mesh->GetSkeletalMeshAsset()->GetSkeleton();
		if (!TestNotNull(TEXT("The selected template mesh retains its real skeleton"), CharacterSkeleton)) return false;
		TestTrue(TEXT("Each template graph targets its corresponding mesh skeleton"), Graph->GetTargetSkeleton() == CharacterSkeleton);
		const auto* Animation = Mesh->GetAnimClass()->GetDefaultObject<UfpstruePlayerAnimInstance>();
		TestNull(TEXT("Template animation CDOs do not retain an unused RAR viewmodel material collection"),
			ViewmodelCollection->GetObjectPropertyValue_InContainer(Animation));
		if (Index == 0)
		{
			UAnimSequence* Turns[] = {Animation->TurnLeftAnimation.Get(), Animation->TurnRightAnimation.Get()};
			const TCHAR* TurnPaths[] = {
				TEXT("/Game/FirstPerson/Animations/FPSMPT/A_TemplateTurnLeft.A_TemplateTurnLeft"),
				TEXT("/Game/FirstPerson/Animations/FPSMPT/A_TemplateTurnRight.A_TemplateTurnRight")};
			for (int32 Side = 0; Side < UE_ARRAY_COUNT(Turns); ++Side)
			{
				UAnimSequence* Turn = Turns[Side];
				if (!TestNotNull(TEXT("Template body configures each independent project turn sequence"), Turn)) return false;
				TestEqual(TEXT("Turns use project copies rather than modifying supplier animation assets"), Turn->GetPathName(), FString(TurnPaths[Side]));
				TestTrue(TEXT("Each turn matches the template skeleton and remains in-place without gameplay notifies"),
					Turn->GetSkeleton() == CharacterSkeleton && Turn->bForceRootLock && !Turn->bEnableRootMotion
					&& FMath::IsNearlyEqual(Turn->RateScale, 1.5f) && Turn->Notifies.IsEmpty());
				const FTransform TurnDelta = Turn->ExtractRootMotion(0.0f, Turn->GetPlayLength(), false);
				TestTrue(TEXT("Each turn retains its authored stationary ninety-degree root curve for timed presentation"),
					FMath::Abs(FMath::FindDeltaAngleDegrees(TurnDelta.Rotator().Yaw, Side == 0 ? -90.0f : 90.0f)) < 1.0f
					&& TurnDelta.GetLocation().Size() < 0.1f);
			}
		}
		for (UAnimMontage* Montage : {Animation->FireMontage.Get(), Animation->AimedFireMontage.Get(),
			Animation->ReloadMontage.Get(), Animation->EmptyReloadMontage.Get()})
		{
			if (!TestNotNull(TEXT("Each template view has its fire and reload action montages"), Montage)) return false;
			TestTrue(TEXT("Template human montages target the exact selected character skeleton"), Montage->GetSkeleton() == CharacterSkeleton);
			Montages.AddUnique(Montage);
		}
		if (Index == 1)
		{
			if (!TestNotNull(TEXT("First-person template presentation includes its authored equip montage"), Animation->EquipMontage.Get())) return false;
			Montages.AddUnique(Animation->EquipMontage.Get());
		}
	}
#if WITH_EDITOR
	// 检查实际保存的编辑器图，而不只断言 HitAlpha 数值：受击必须在持枪权重分层之后才能覆盖空手身体。
	const auto* BodyBlueprint = Cast<UAnimBlueprint>(Character->GetMesh()->GetAnimClass()->ClassGeneratedBy);
	if (!TestNotNull(TEXT("Template body retains its authored AnimBlueprint for graph inspection"), BodyBlueprint)) return false;
	UAnimGraphNode_Root* RootNode = nullptr;
	for (UEdGraph* FunctionGraph : BodyBlueprint->FunctionGraphs)
	{
		if (FunctionGraph == nullptr || FunctionGraph->GetFName() != TEXT("AnimGraph")) continue;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
			if (auto* Root = Cast<UAnimGraphNode_Root>(Node)) RootNode = Root;
	}
	if (!TestNotNull(TEXT("Template body graph contains its final pose root"), RootNode)) return false;
	const UEdGraphPin* RootInput = RootNode->FindPin(TEXT("Result"));
	if (!TestTrue(TEXT("Template body root has one final pose source"), RootInput != nullptr && RootInput->LinkedTo.Num() == 1)) return false;
	const auto* FinalHit = Cast<UAnimGraphNode_ApplyMeshSpaceAdditive>(RootInput->LinkedTo[0]->GetOwningNode());
	if (!TestNotNull(TEXT("Template body applies hit reaction after the final armed/unarmed layer"), FinalHit)) return false;
	const UEdGraphPin* FinalHitBase = FinalHit->FindPin(TEXT("Base"));
	if (!TestTrue(TEXT("Final template hit reaction has exactly one base pose"), FinalHitBase != nullptr && FinalHitBase->LinkedTo.Num() == 1)) return false;
	const auto* BodyLayer = Cast<UAnimGraphNode_LayeredBoneBlend>(FinalHitBase->LinkedTo[0]->GetOwningNode());
	if (!TestNotNull(TEXT("The final hit reaction consumes the complete body layered pose, not the armed-only upper pose"), BodyLayer)) return false;
	const auto PoseSource = [](const UEdGraphNode* Node, const TCHAR* PinName) -> const UEdGraphNode*
	{
		const UEdGraphPin* Pin = Node ? Node->FindPin(PinName) : nullptr;
		return Pin && Pin->LinkedTo.Num() == 1 ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
	};
	const auto ReadsSnapshot = [](const UEdGraphNode* Node, const TCHAR* PinName, const FName Variable)
	{
		const UEdGraphPin* Pin = Node ? Node->FindPin(PinName) : nullptr;
		const auto* Read = Pin && Pin->LinkedTo.Num() == 1 ? Cast<UK2Node_VariableGet>(Pin->LinkedTo[0]->GetOwningNode()) : nullptr;
		return Read && Read->VariableReference.GetMemberName() == Variable;
	};
	TestTrue(TEXT("The complete upper body remains isolated at spine_01 with full mesh-space rotation blending"),
		BodyLayer->Node.bMeshSpaceRotationBlend && BodyLayer->Node.LayerSetup.Num() == 1
		&& BodyLayer->Node.LayerSetup[0].BranchFilters.Num() == 1
		&& BodyLayer->Node.LayerSetup[0].BranchFilters[0].BoneName == TEXT("spine_01")
		&& BodyLayer->Node.LayerSetup[0].BranchFilters[0].BlendDepth == 1);
	const auto* UpperAim = Cast<UAnimGraphNode_RotationOffsetBlendSpace>(PoseSource(BodyLayer, TEXT("BlendPoses_0")));
	const auto* UpperAction = Cast<UAnimGraphNode_Slot>(PoseSource(UpperAim, TEXT("BasePose")));
	TestTrue(TEXT("Upper-body pitch and action slot remain connected after the lower-body turning repair"),
		UpperAim && ReadsSnapshot(UpperAim, TEXT("Y"), TEXT("AimPitch"))
		&& UpperAction && UpperAction->Node.SlotName == TEXT("TemplateAction"));
	const auto* LowerRotation = Cast<UAnimGraphNode_RotateRootBone>(PoseSource(BodyLayer, TEXT("BasePose")));
	if (!TestNotNull(TEXT("Only the lower-body input passes through root-yaw compensation"), LowerRotation)) return false;
	TestTrue(TEXT("Lower-body root yaw consumes the native snapshot without rotating movement root-motion attributes"),
		ReadsSnapshot(LowerRotation, TEXT("Yaw"), TEXT("RootYawOffset")) && !LowerRotation->Node.bRotateRootMotionAttribute);
	const auto* TurnBlend = Cast<UAnimGraphNode_TwoWayBlend>(PoseSource(LowerRotation, TEXT("BasePose")));
	const auto* OriginalLower = Cast<UAnimGraphNode_BlendListByBool>(PoseSource(TurnBlend, TEXT("A")));
	const auto* TurnDirection = Cast<UAnimGraphNode_BlendListByBool>(PoseSource(TurnBlend, TEXT("B")));
	if (!TestTrue(TEXT("Turning retains the original falling/ground locomotion input and only blends a timed lower-body turn"),
		TurnBlend && ReadsSnapshot(TurnBlend, TEXT("Alpha"), TEXT("TurnAlpha"))
		&& OriginalLower && ReadsSnapshot(OriginalLower, TEXT("bActiveValue"), TEXT("bFalling"))
		&& TurnDirection && ReadsSnapshot(TurnDirection, TEXT("bActiveValue"), TEXT("bTurnRight")))) return false;
	const auto* BodyDefaults = Character->GetMesh()->GetAnimClass()->GetDefaultObject<UfpstruePlayerAnimInstance>();
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const auto* Evaluator = Cast<UAnimGraphNode_SequenceEvaluator>(PoseSource(TurnDirection, Side == 0 ? TEXT("BlendPose_0") : TEXT("BlendPose_1")));
		TestTrue(TEXT("Each turn samples the matching sequence at native server-clock time without advancing notifies or root extraction"),
			Evaluator && Evaluator->Node.GetSequence() == (Side == 0 ? BodyDefaults->TurnRightAnimation.Get() : BodyDefaults->TurnLeftAnimation.Get())
			&& Evaluator->Node.GetTeleportToExplicitTime() && !Evaluator->Node.IsLooping() && !Evaluator->Node.ShouldUseExplicitFrame()
			&& Evaluator->Node.GetGroupName() == NAME_None && Evaluator->Node.GetGroupMethod() == EAnimSyncMethod::DoNotSync
			&& ReadsSnapshot(Evaluator, TEXT("ExplicitTime"), TEXT("TurnTime")));
	}
	const auto* ArmsBlueprint = Cast<UAnimBlueprint>(Character->GetMesh1P()->GetAnimClass()->ClassGeneratedBy);
	if (!TestNotNull(TEXT("Template first-person arms retain their authored graph for sway inspection"), ArmsBlueprint)) return false;
	int32 PreservedWalkBlends = 0;
	for (UEdGraph* FunctionGraph : ArmsBlueprint->FunctionGraphs)
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			const auto* WalkBlend = Cast<UAnimGraphNode_TwoWayBlend>(Node);
			const auto* Walk = Cast<UAnimGraphNode_SequencePlayer>(PoseSource(WalkBlend, TEXT("B")));
			if (!Walk || !Walk->Node.GetSequence() || Walk->Node.GetSequence()->GetPathName()
				!= TEXT("/Game/FPSMPT55/Animations/FPP/AR/A_FPP_AR_Walk.A_FPP_AR_Walk")) continue;
			++PreservedWalkBlends;
			const UEdGraphPin* Alpha = WalkBlend->FindPin(TEXT("Alpha"));
			TestTrue(TEXT("The original first-person walk-sway nodes remain present but cannot move the aiming upper body"),
				Alpha && Alpha->LinkedTo.IsEmpty() && FMath::IsNearlyZero(WalkBlend->BlendNode.Alpha)
				&& FMath::IsNearlyZero(FCString::Atof(*Alpha->DefaultValue)));
		}
	TestEqual(TEXT("Exactly one authored first-person walk-sway blend is retained and disabled"), PreservedWalkBlends, 1);
#endif
	TestNotNull(TEXT("The selected body keeps a PhysicsAsset for authoritative hits and death"), Character->GetMesh()->GetPhysicsAsset());
	TestTrue(TEXT("The template keeps the existing first/third-person visibility split"),
		Character->GetMesh1P()->bOnlyOwnerSee && !Character->GetMesh1P()->bOwnerNoSee
		&& Character->GetMesh()->bOwnerNoSee && !Character->GetMesh()->bOnlyOwnerSee);
	TestTrue(TEXT("First-person template geometry remains camera-relative"),
		Character->GetMesh1P()->GetAttachParent() == Character->GetFirstPersonCameraComponent());
	const auto* Rifle = GetDefault<AfpstrueTemplateWeapon>();
	UfpstrueWeaponComponent* Weapon = Rifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("The native template rifle contains the existing gameplay weapon component"), Weapon)
		|| !TestNotNull(TEXT("The template gun has its mesh"), Weapon->GetSkeletalMeshAsset())
		|| !TestNotNull(TEXT("The template gun has its animation class"), Weapon->GetAnimClass())) return false;
	TestEqual(TEXT("The template gun is the corresponding assault rifle asset"), Weapon->GetSkeletalMeshAsset()->GetPathName(),
		FString(TEXT("/Game/FPSMPT55/Weapons/AssaultRifle/SKM_AssaultRifle.SKM_AssaultRifle")));
	TestEqual(TEXT("The template gun uses the project-owned mechanical animation graph"), Weapon->GetAnimClass()->GetPathName(),
		FString(TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateWeapon.ABP_TemplateWeapon_C")));
	const auto* GunGraph = IAnimClassInterface::GetFromClass(Weapon->GetAnimClass());
	if (!TestNotNull(TEXT("The gun animation class is a compiled graph"), GunGraph)) return false;
	TestTrue(TEXT("The gun graph targets the weapon skeleton rather than the player skeleton"),
		GunGraph->GetTargetSkeleton() == Weapon->GetSkeletalMeshAsset()->GetSkeleton()
		&& GunGraph->GetTargetSkeleton() != CharacterSkeleton);
	const FVector GripOffset(-6.236122, 3.008201, 1.059424);
	const FRotator GripRotation(3.4989, 105.08198, 4.0575);
	TestTrue(TEXT("Template grip retains the authored hand-relative translation"), Weapon->GripLocationOffset.Equals(GripOffset, 0.001));
	TestTrue(TEXT("Template grip retains the authored hand-relative rotation"), Weapon->GripRotationOffset.Equals(GripRotation, 0.01));
	TestEqual(TEXT("Template equipment attaches to hand_r rather than the RAR independent gun chain"), Weapon->GetWeaponSettings().GripSocketName, FName(TEXT("hand_r")));
	TestEqual(TEXT("The configured muzzle uses the template's muzzleFlash socket"), Weapon->MuzzleSocketName, FName(TEXT("muzzleFlash")));
	TestTrue(TEXT("Muzzle feedback uses the actual gun socket and its authored forward axis"),
		Weapon->MuzzleSource.Get() == Weapon && Weapon->DoesSocketExist(Weapon->MuzzleSocketName)
		&& Weapon->MuzzleForwardAxis.Equals(FVector::ForwardVector));
	TestFalse(TEXT("The template weapon provides a user-facing equipment name"), Weapon->WeaponDisplayName.IsEmpty());
	const TCHAR* GunMontagePaths[] = {
		TEXT("/Game/FirstPerson/Animations/FPSMPT/AM_TemplateWeapon_Fire.AM_TemplateWeapon_Fire"),
		TEXT("/Game/FirstPerson/Animations/FPSMPT/AM_TemplateWeapon_Reload.AM_TemplateWeapon_Reload"),
		TEXT("/Game/FirstPerson/Animations/FPSMPT/AM_TemplateWeapon_ReloadEmpty.AM_TemplateWeapon_ReloadEmpty")};
	UAnimMontage* GunMontages[] = {Weapon->WeaponFireMontage.Get(), Weapon->WeaponReloadMontage.Get(), Weapon->WeaponEmptyReloadMontage.Get()};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(GunMontages); ++Index)
	{
		UAnimMontage* Montage = GunMontages[Index];
		if (!TestNotNull(TEXT("The template gun has each of its three mechanical action montages"), Montage)) return false;
		TestEqual(TEXT("Each mechanical action uses its corresponding project montage"), Montage->GetPathName(), FString(GunMontagePaths[Index]));
		TestTrue(TEXT("Each mechanical montage targets the gun's exact skeleton"), Montage->GetSkeleton() == GunGraph->GetTargetSkeleton());
		Montages.AddUnique(Montage);
	}
	for (const UAnimMontage* Montage : Montages)
	{
		TestTrue(TEXT("Template montages have a positive finite playback length"), FMath::IsFinite(Montage->GetPlayLength()) && Montage->GetPlayLength() > 0.0f);
		TestTrue(TEXT("Template montages contain an actual action slot"), !Montage->SlotAnimTracks.IsEmpty());
		for (const FAnimNotifyEvent& Notify : Montage->Notifies)
			TestTrue(TEXT("Template montages do not import supplier gameplay or duplicate audiovisual notifications"),
				Notify.Notify != nullptr && Notify.Notify->IsA<UfpstrueAnimNotify_ReloadCommit>() && Notify.NotifyStateClass == nullptr);
		for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
		{
			TestEqual(TEXT("Template graphs and montages share the project action slot"), Slot.SlotName, FName(TEXT("TemplateAction")));
			TestTrue(TEXT("The action slot is registered on its actual skeleton"), Montage->GetSkeleton() != nullptr && Montage->GetSkeleton()->ContainsSlotName(Slot.SlotName));
			TestTrue(TEXT("Template action tracks contain their authored clip"), !Slot.AnimTrack.AnimSegments.IsEmpty());
			for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
			{
				const UAnimSequenceBase* Clip = Segment.GetAnimReference();
				if (!TestNotNull(TEXT("Template montage segments resolve a real animation clip"), Clip)) return false;
				TestTrue(TEXT("Each source clip shares the montage skeleton"), Clip->GetSkeleton() == Montage->GetSkeleton());
				TestEqual(TEXT("Source presentation clips do not execute supplier gameplay notifications"), Clip->Notifies.Num(), 0);
			}
		}
	}
	// 检查真实 Blueprint 编译后的树，不触发 Construct，也不把加载 UMG 类当作画面恢复。
	UClass* OriginalHud = LoadClass<UUserWidget>(nullptr, TEXT("/Game/FirstPerson/UI/ingame.ingame_C"));
	UClass* NetworkHud = LoadClass<UUserWidget>(nullptr, TEXT("/Game/FirstPerson/UI/ingame_network.ingame_network_C"));
	if (!TestNotNull(TEXT("The original single-player HUD asset remains available"), OriginalHud)
		|| !TestNotNull(TEXT("The network copy of the original HUD exists"), NetworkHud)) return false;
	TestTrue(TEXT("The original single-player HUD retains its original native parent"), OriginalHud->GetSuperClass() == UUserWidget::StaticClass());
	TestTrue(TEXT("Only the network HUD copy inherits the native event-driven bridge"), NetworkHud->GetSuperClass() == UfpstruePvpIngameWidget::StaticClass());
	const auto* OriginalHudClass = Cast<UWidgetBlueprintGeneratedClass>(OriginalHud);
	const auto* NetworkHudClass = Cast<UWidgetBlueprintGeneratedClass>(NetworkHud);
	if (!TestNotNull(TEXT("The original HUD remains a compiled WidgetBlueprint"), OriginalHudClass)
		|| !TestNotNull(TEXT("The network HUD remains a compiled WidgetBlueprint"), NetworkHudClass)
		|| !TestNotNull(TEXT("The original HUD retains its authored widget tree"), OriginalHudClass->GetWidgetTreeArchetype())
		|| !TestNotNull(TEXT("The network HUD retains the copied authored widget tree"), NetworkHudClass->GetWidgetTreeArchetype())) return false;
	TestTrue(TEXT("Network HUD uses a separate tree rather than mutating the original"),
		OriginalHudClass->GetWidgetTreeArchetype() != NetworkHudClass->GetWidgetTreeArchetype());
	for (const TCHAR* Name : {TEXT("血量组件"), TEXT("当前子弹"), TEXT("剩余子弹"), TEXT("TextBlock_2"), TEXT("TextBlock_65"), TEXT("子弹数"), TEXT("HorizontalBox_327")})
	{
		TestNotNull(FString(TEXT("Original authored HUD retains ")) + Name, OriginalHudClass->GetWidgetTreeArchetype()->FindWidget(FName(Name)));
		TestNotNull(FString(TEXT("Network HUD copy retains ")) + Name, NetworkHudClass->GetWidgetTreeArchetype()->FindWidget(FName(Name)));
	}
	TestEqual(TEXT("The network HUD cannot run old polling bindings over the event-driven values"), NetworkHudClass->Bindings.Num(), 0);
	const auto* HudClassProperty = FindFProperty<FSoftClassProperty>(AfpstruePvpHUD::StaticClass(), TEXT("IngameWidgetClass"));
	if (!TestNotNull(TEXT("The native HUD exposes its soft widget-class configuration"), HudClassProperty)) return false;
	const auto* ConfiguredHud = HudClassProperty->ContainerPtrToValuePtr<TSoftClassPtr<UfpstruePvpIngameWidget>>(GetDefault<AfpstruePvpHUD>());
	TestEqual(TEXT("PvP HUD selects the network copy rather than replacing the single-player source"), ConfiguredHud->ToSoftObjectPath().ToString(),
		FString(TEXT("/Game/FirstPerson/UI/ingame_network.ingame_network_C")));
	// 真实装备后注入同父级迟到变换；非零作者偏移必须恢复，不能机械清零为 RAR 的规则。
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this)) return false;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* RuntimeCharacter = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>(PawnClass, FTransform::Identity, Spawn);
	auto* RuntimeRifle = Fixture.GetTestWorld()->SpawnActor<AfpstrueTemplateWeapon>(AfpstrueTemplateWeapon::StaticClass(), FTransform(FVector(5000, 0, 0)), Spawn);
	if (!TestNotNull(TEXT("The template runtime fixture uses the actual selected character Blueprint"), RuntimeCharacter)
		|| !TestNotNull(TEXT("The template runtime fixture uses its paired native gun"), RuntimeRifle)) return false;
	USkeletalMeshComponent* RuntimeBodyMesh = RuntimeCharacter->GetMesh();
	auto* RuntimeBodyAnimation = Cast<UfpstruePlayerAnimInstance>(RuntimeBodyMesh->GetAnimInstance());
	auto* RuntimeArmsAnimation = Cast<UfpstruePlayerAnimInstance>(RuntimeCharacter->GetMesh1P()->GetAnimInstance());
	if (!TestNotNull(TEXT("The actual empty-handed template has its native body animation instance"), RuntimeBodyAnimation)
		|| !TestNotNull(TEXT("The actual template has its native first-person animation instance"), RuntimeArmsAnimation)) return false;
	TestNull(TEXT("A real template body instance never acquires RAR-only viewmodel material data"),
		ViewmodelCollection->GetObjectPropertyValue_InContainer(RuntimeBodyAnimation));
	TestNull(TEXT("A real template first-person instance never acquires RAR-only viewmodel material data"),
		ViewmodelCollection->GetObjectPropertyValue_InContainer(RuntimeArmsAnimation));
	TestFalse(TEXT("The template fixture begins empty-handed before its production pickup transaction"), RuntimeCharacter->HasEquippedWeapon());
	RuntimeBodyMesh->SetComponentTickEnabled(false);
	RuntimeBodyMesh->TickAnimation(0.016f, false);
	RuntimeBodyMesh->RefreshBoneTransforms();
	TestFalse(TEXT("The actual native animation snapshot identifies the empty-handed state"), RuntimeBodyAnimation->bArmed);
	RuntimeBodyAnimation->PresentHit();
	TestEqual(TEXT("An empty-handed hit starts the native visual pulse"), RuntimeBodyAnimation->HitAlpha, 1.0f);
	TestEqual(TEXT("An empty-handed hit restarts its own sampling time"), RuntimeBodyAnimation->HitTime, 0.0f);
	RuntimeBodyMesh->TickAnimation(0.016f, false);
	RuntimeBodyMesh->RefreshBoneTransforms();
	TestTrue(TEXT("The empty-handed body keeps a positive hit pulse while the real final graph evaluates"),
		!RuntimeBodyAnimation->bArmed && RuntimeBodyAnimation->HitAlpha > 0.0f && RuntimeBodyAnimation->HitTime > 0.0f);
	auto* RuntimeWeapon = RuntimeRifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("The runtime gun retains the production weapon component"), RuntimeWeapon)
		|| !TestTrue(TEXT("The template runtime fixture completes the real authority equipment transaction"), RuntimeWeapon->AttachWeapon(RuntimeCharacter))) return false;
	RuntimeCharacter->SetRole(ROLE_SimulatedProxy);
	RuntimeRifle->SetRole(ROLE_SimulatedProxy);
	RuntimeWeapon->RefreshNetworkEquipment();
	TestTrue(TEXT("A remote template gun attaches to the body hand, never the hidden viewmodel"),
		RuntimeWeapon->GetAttachParent() == RuntimeCharacter->GetMesh() && RuntimeWeapon->GetAttachSocketName() == TEXT("hand_r"));
	const FVector RelativeScale(0.9, 0.95, 1.1);
	RuntimeWeapon->SetRelativeScale3D(RelativeScale);
	const auto CheckGrip = [&]()
	{
		TestTrue(TEXT("Late replication restores the authored template grip translation and rotation"),
			RuntimeWeapon->GetRelativeLocation().Equals(GripOffset, 0.001) && RuntimeWeapon->GetRelativeRotation().Equals(GripRotation, 0.01));
		TestTrue(TEXT("Restoring template grip preserves authored relative scale"), RuntimeWeapon->GetRelativeScale3D().Equals(RelativeScale, 0.001));
		const FTransform Expected = FTransform(GripRotation, GripOffset, RelativeScale)
			* RuntimeCharacter->GetMesh()->GetSocketTransform(TEXT("hand_r"));
		TestTrue(TEXT("The template gun's actual world transform agrees with its body hand and author offset"),
			FVector::Dist(RuntimeWeapon->GetComponentLocation(), Expected.GetLocation()) < 0.1
			&& RuntimeWeapon->GetComponentQuat().AngularDistance(Expected.GetRotation()) < 0.001);
	};
	RuntimeWeapon->SetRelativeLocationAndRotation(FVector(-3000, 3500, 550), FRotator(15, 90, 0));
	RuntimeWeapon->RefreshNetworkEquipment();
	CheckGrip();
	RuntimeWeapon->SetRelativeLocationAndRotation(FVector(4500, -500, 700), FRotator(0, 180, 0));
	RuntimeRifle->OnRep_AttachmentReplication();
	CheckGrip();
	TestTrue(TEXT("The remote template viewmodel remains hidden after repeated equipment repair"), RuntimeCharacter->GetMesh1P()->bHiddenInGame);
	// 状态边界用真实模板与服务器时钟推进；固定 CMC 输入快照不冒充多进程复制或画面验收。
	auto* FacingCharacter = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>(PawnClass,
		FTransform(FRotator(0.0f, 170.0f, 0.0f), FVector(15000.0f, 0.0f, 0.0f)), Spawn);
	if (!TestNotNull(TEXT("Facing fixture uses the actual template character"), FacingCharacter)) return false;
	USkeletalMeshComponent* FacingMesh = FacingCharacter->GetMesh();
	UCharacterMovementComponent* FacingMovement = FacingCharacter->GetCharacterMovement();
	FacingMesh->SetComponentTickEnabled(false); FacingMesh->bEnableUpdateRateOptimizations = false;
	FacingMovement->SetComponentTickEnabled(false); FacingMovement->SetMovementMode(MOVE_Walking);
	FacingMovement->Velocity = FVector::ZeroVector;
	auto* FacingAnimation = Cast<UfpstruePlayerAnimInstance>(FacingMesh->GetAnimInstance());
	if (!TestNotNull(TEXT("Facing fixture runs its configured template body animation"), FacingAnimation)
		|| !TestNotNull(TEXT("Facing fixture retains its actual right turn curve"), FacingAnimation->TurnRightAnimation.Get())) return false;
	const float TurnDuration = FacingAnimation->TurnRightAnimation->GetPlayLength() / FacingAnimation->TurnRightAnimation->RateScale;
	RuntimeCharacter->SetRole(ROLE_Authority); RuntimeRifle->SetRole(ROLE_Authority);
	RuntimeWeapon->DetachWeapon();
	if (!TestTrue(TEXT("The facing fixture equips through the normal authority transaction"), RuntimeWeapon->AttachWeapon(FacingCharacter))) return false;
	const auto EvaluateFacing = [&](float Delta)
	{
		FacingMesh->TickAnimation(Delta, false); FacingMesh->RefreshBoneTransforms();
	};
	EvaluateFacing(1.0f / 60.0f); FacingAnimation->Montage_Stop(0.0f); FacingAnimation->HitAlpha = 0.0f;
	for (int32 Frame = 0; Frame < 12; ++Frame) EvaluateFacing(1.0f / 30.0f);
	const FQuat UpperBeforeWalk = FacingMesh->GetSocketTransform(TEXT("spine_03"), RTS_Component).GetRotation();
	const FVector FootBeforeWalk = FacingMesh->GetSocketTransform(TEXT("foot_l"), RTS_Component).GetLocation();
	double MaximumUpperWalkDegrees = 0.0, MaximumFootWalkDistance = 0.0;
	FacingMovement->Velocity = FacingCharacter->GetActorForwardVector() * 300.0f;
	for (int32 Frame = 0; Frame < 12; ++Frame)
	{
		EvaluateFacing(1.0f / 30.0f);
		MaximumUpperWalkDegrees = FMath::Max(MaximumUpperWalkDegrees, FMath::RadiansToDegrees(
			UpperBeforeWalk.AngularDistance(FacingMesh->GetSocketTransform(TEXT("spine_03"), RTS_Component).GetRotation())));
		MaximumFootWalkDistance = FMath::Max(MaximumFootWalkDistance,
			FVector::Distance(FootBeforeWalk, FacingMesh->GetSocketTransform(TEXT("foot_l"), RTS_Component).GetLocation()));
	}
	AddInfo(FString::Printf(TEXT("Template armed walk: upper rotation change %.4f degrees, lower foot travel %.4f cm (component-space pose check, not visual/network acceptance)."),
		MaximumUpperWalkDegrees, MaximumFootWalkDistance));
	TestTrue(TEXT("Armed walk keeps the aimed upper-body rotation while evaluating real leg movement"),
		MaximumUpperWalkDegrees < 1.0 && MaximumFootWalkDistance > 3.0);
	FacingMovement->Velocity = FVector::ZeroVector; EvaluateFacing(1.0f / 30.0f);
	const FFpstrueBodyFacingState InitialFacing = FacingCharacter->GetBodyFacingState();
	TestTrue(TEXT("Stopping initializes a stationary foot anchor from the actual actor yaw"),
		InitialFacing.bInitialized && InitialFacing.bStationary && InitialFacing.TurnDirection == 0
		&& FMath::IsNearlyEqual(InitialFacing.FacingYaw, 170.0f, 0.01f));
	FacingCharacter->SetActorRotation(FRotator(0.0f, -140.0f, 0.0f));
	FacingCharacter->UpdateBodyFacing(TurnDuration); EvaluateFacing(1.0f / 30.0f);
	TestTrue(TEXT("A stationary look inside the trigger threshold retains the feet rather than sliding them"),
		FacingCharacter->GetBodyFacingState().TurnDirection == 0 && FMath::IsNearlyEqual(FacingAnimation->RootYawOffset, -50.0f, 0.1f));
	FacingCharacter->SetActorRotation(FRotator(0.0f, -130.0f, 0.0f));
	FacingCharacter->UpdateBodyFacing(TurnDuration);
	const FFpstrueBodyFacingState Turning = FacingCharacter->GetBodyFacingState();
	FacingCharacter->UpdateBodyFacing(TurnDuration);
	TestTrue(TEXT("Crossing signed 180 degrees chooses the short right turn and repeated same-frame updates do not restart it"),
		Turning.TurnDirection == 1 && FacingCharacter->GetBodyFacingState().TurnStartedServerTime == Turning.TurnStartedServerTime
		&& FMath::IsNearlyEqual(Turning.FacingYaw, 170.0f, 0.01f));
	const auto AdvanceFacing = [&](float Delta)
	{
		// WorldSettings 会把单帧 .5 秒截为 .4 秒；按真实世界时钟分步，不把请求步长误当作已经过去的时间。
		const float BeforeTime = FacingCharacter->GetBodyFacingServerTime();
		const float TargetTime = BeforeTime + Delta;
		const FFpstrueBodyFacingState BeforeState = FacingCharacter->GetBodyFacingState();
		int32 Steps = 0;
		while (FacingCharacter->GetBodyFacingServerTime() + UE_KINDA_SMALL_NUMBER < TargetTime)
		{
			const float BeforeStep = FacingCharacter->GetBodyFacingServerTime();
			const float Step = FMath::Min(1.0f / 30.0f, TargetTime - BeforeStep);
			if (!Fixture.TickTestWorld(Step)) { Fixture.ForwardErrorMessages(this); return false; }
			if (FacingCharacter->GetBodyFacingServerTime() <= BeforeStep)
			{ AddError(TEXT("Facing fixture world clock did not advance.")); return false; }
			FacingCharacter->UpdateBodyFacing(TurnDuration); EvaluateFacing(Step); ++Steps;
		}
		const FFpstrueBodyFacingState& AfterState = FacingCharacter->GetBodyFacingState();
		const float ActualFootYaw = (FacingMesh->GetSocketQuaternion(TEXT("root"))
			* FacingCharacter->GetBaseRotationOffset().Inverse()).Rotator().Yaw;
		AddInfo(FString::Printf(TEXT("Facing advance: requested %.6f s, server clock %.6f->%.6f, steps %d, max world frame %.4f s; turn %d->%d, anchor %.4f->%.4f, turn start %.6f, sample %.6f, root offset %.4f, actual skeletal foot yaw %.4f."),
			Delta, BeforeTime, FacingCharacter->GetBodyFacingServerTime(), Steps,
			Fixture.GetTestWorld()->GetWorldSettings()->MaxUndilatedFrameTime,
			BeforeState.TurnDirection, AfterState.TurnDirection, BeforeState.FacingYaw, AfterState.FacingYaw,
			AfterState.TurnStartedServerTime, FacingAnimation->TurnTime, FacingAnimation->RootYawOffset, ActualFootYaw));
		return true;
	};
	if (!AdvanceFacing(TurnDuration * 0.5f)) return false;
	const float MidTurnTime = FacingAnimation->TurnTime;
	const FFpstrueBodyFacingState ReplicatedFacing = FacingCharacter->GetBodyFacingState();
	FacingCharacter->SetRole(ROLE_SimulatedProxy);
	FacingCharacter->SetActorRotation(FRotator(0.0f, 0.0f, 0.0f));
	FacingCharacter->UpdateBodyFacing(TurnDuration);
	FacingMesh->InitAnim(true);
	FacingAnimation = Cast<UfpstruePlayerAnimInstance>(FacingMesh->GetAnimInstance());
	if (!TestNotNull(TEXT("The simulated proxy rebuilds its real body animation instance"), FacingAnimation)) return false;
	EvaluateFacing(1.0f / 60.0f);
	TestTrue(TEXT("A rebuilt proxy animation recovers the current turn phase from received state and server time without reauthoring it"),
		FacingAnimation->bTurnRight && FMath::IsNearlyEqual(FacingAnimation->TurnTime, MidTurnTime, 0.001f)
		&& FacingCharacter->GetBodyFacingState().FacingYaw == ReplicatedFacing.FacingYaw
		&& FacingCharacter->GetBodyFacingState().TurnDirection == ReplicatedFacing.TurnDirection
		&& FacingCharacter->GetBodyFacingState().TurnStartedServerTime == ReplicatedFacing.TurnStartedServerTime);
	FacingCharacter->SetRole(ROLE_Authority); FacingCharacter->SetActorRotation(FRotator(0.0f, -130.0f, 0.0f));
	if (!AdvanceFacing(TurnDuration * 0.49f)) return false;
	const float FootYawBeforeEnd = (FacingMesh->GetSocketQuaternion(TEXT("root")) * FacingCharacter->GetBaseRotationOffset().Inverse()).Rotator().Yaw;
	if (!AdvanceFacing(TurnDuration * 0.02f)) return false;
	const float FootYawAfterEnd = (FacingMesh->GetSocketQuaternion(TEXT("root")) * FacingCharacter->GetBaseRotationOffset().Inverse()).Rotator().Yaw;
	AddInfo(FString::Printf(TEXT("Facing endpoint: duration %.6f, elapsed %.6f, turn %d, anchor %.4f, actor yaw %.4f, root offset %.4f, actual skeletal foot yaw %.4f->%.4f (delta %.4f degrees)."),
		TurnDuration, FacingCharacter->GetBodyFacingServerTime() - Turning.TurnStartedServerTime,
		FacingCharacter->GetBodyFacingState().TurnDirection, FacingCharacter->GetBodyFacingState().FacingYaw,
		FacingCharacter->GetActorRotation().Yaw, FacingAnimation->RootYawOffset, FootYawBeforeEnd, FootYawAfterEnd,
		FMath::Abs(FMath::FindDeltaAngleDegrees(FootYawBeforeEnd, FootYawAfterEnd))));
	TestTrue(TEXT("Turn completion commits ninety degrees without snapping the skeletal root back to the actor yaw"),
		FacingCharacter->GetBodyFacingState().TurnDirection == 0
		&& FMath::IsNearlyEqual(FacingCharacter->GetBodyFacingState().FacingYaw, -100.0f, 0.1f)
		&& FMath::IsNearlyEqual(FacingAnimation->RootYawOffset, 30.0f, 0.1f)
		&& FMath::Abs(FMath::FindDeltaAngleDegrees(FootYawBeforeEnd, FootYawAfterEnd)) < 4.0f);
	FacingCharacter->SetActorRotation(FRotator(0.0f, -160.0f, 0.0f)); FacingCharacter->UpdateBodyFacing(TurnDuration);
	TestEqual(TEXT("Looking sufficiently left begins the corresponding independent left turn"), FacingCharacter->GetBodyFacingState().TurnDirection, int8(-1));
	const FVector MovingVelocity = FacingCharacter->GetActorForwardVector() * 300.0f;
	FacingMovement->Velocity = MovingVelocity; FacingCharacter->UpdateBodyFacing(TurnDuration); EvaluateFacing(0.1f);
	TestTrue(TEXT("Ground movement cancels the old turn and returns root compensation to locomotion without modifying physical velocity"),
		!FacingCharacter->GetBodyFacingState().bStationary && FacingCharacter->GetBodyFacingState().TurnDirection == 0
		&& FMath::IsNearlyZero(FacingAnimation->RootYawOffset, 0.01f) && FacingMovement->Velocity.Equals(MovingVelocity));
	FacingMovement->Velocity = FVector::ZeroVector; FacingCharacter->SetActorRotation(FRotator(0.0f, -170.0f, 0.0f));
	FacingCharacter->UpdateBodyFacing(TurnDuration); FacingCharacter->SetActorRotation(FRotator(0.0f, 130.0f, 0.0f));
	FacingCharacter->UpdateBodyFacing(TurnDuration);
	TestEqual(TEXT("The opposite signed-180 crossing chooses the short left turn"), FacingCharacter->GetBodyFacingState().TurnDirection, int8(-1));
	FacingMovement->SetMovementMode(MOVE_Falling); FacingCharacter->UpdateBodyFacing(TurnDuration);
	TestTrue(TEXT("Leaving the ground cancels stationary turning rather than carrying a planted-foot state through a jump"),
		!FacingCharacter->GetBodyFacingState().bStationary && FacingCharacter->GetBodyFacingState().TurnDirection == 0);
	FacingMovement->SetMovementMode(MOVE_Walking); FacingCharacter->SetActorRotation(FRotator::ZeroRotator);
	FacingCharacter->UpdateBodyFacing(TurnDuration); FacingCharacter->SetActorRotation(FRotator(0.0f, 60.0f, 0.0f));
	FacingCharacter->UpdateBodyFacing(TurnDuration);
	UGameplayStatics::ApplyDamage(FacingCharacter, 10000.0f, nullptr, nullptr, nullptr);
	FacingCharacter->UpdateBodyFacing(TurnDuration);
	TestTrue(TEXT("Authoritative death clears an in-flight standing turn"), FacingCharacter->IsDead()
		&& !FacingCharacter->GetBodyFacingState().bStationary && FacingCharacter->GetBodyFacingState().TurnDirection == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerRARActionPresentationTest,
	"fpstrue.Multiplayer.Animation.RARAbilityPresentationAndCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实能力和装备事务驱动三套播放，取消不补弹、旧编号不停止新动作；不宣称已经验证公网或动画观感。
bool FFpstrueMultiplayerRARActionPresentationTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	AfpstrueCharacter* Character = nullptr;
	UfpstrueWeaponComponent* Weapon = nullptr;
	if (!SpawnRARPresentationFixture(*Fixture.GetTestWorld(), *this, Character, Weapon)) return false;
	UfpstruePlayerAnimInstance* Body = Cast<UfpstruePlayerAnimInstance>(Character->GetMesh()->GetAnimInstance());
	UfpstruePlayerAnimInstance* Arms = Cast<UfpstruePlayerAnimInstance>(Character->GetMesh1P()->GetAnimInstance());
	UAnimInstance* Gun = Weapon->GetAnimInstance();
	if (!TestNotNull(TEXT("RAR real body animation initialized"), Body) || !TestNotNull(TEXT("RAR real arms animation initialized"), Arms)
		|| !TestNotNull(TEXT("RAR real gun animation initialized"), Gun)) return false;
	TestTrue(TEXT("Both human graphs read the real equipped relation"), Body->bArmed && Arms->bArmed);
	TestTrue(TEXT("Visual locomotion cannot become a competing network movement source"), Body->RootMotionMode == ERootMotionMode::IgnoreRootMotion);
	// 枪链不属于脊柱子树：通过真实 Controller 抬头后比较两骨相对姿态，防止身体转了而枪留在原处。
	// 先移除装备动画并预热落地过渡，确保各角度比较同一稳定 idle；不把落地或源空仓换弹滑握当作俯仰误差。
	AAIController* AimController = Cast<AAIController>(Character->GetController());
	USkeletalMeshComponent* BodyMesh = Character->GetMesh();
	if (!TestNotNull(TEXT("Grip aim fixture retains its real controller"), AimController)) return false;
	const int32 HandIndex = BodyMesh->GetBoneIndex(TEXT("hand_r"));
	const int32 GunIndex = BodyMesh->GetBoneIndex(TEXT("ik_hand_gun"));
	if (!TestTrue(TEXT("Grip aim fixture has both authored hand and weapon IK bones"), HandIndex != INDEX_NONE && GunIndex != INDEX_NONE)) return false;
	Body->Montage_Stop(0.0f);
	BodyMesh->bEnableUpdateRateOptimizations = false;
	Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Character->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	AimController->SetControlRotation(FRotator::ZeroRotator);
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		BodyMesh->TickAnimation(1.0f / 60.0f, false);
		BodyMesh->RefreshBoneTransforms();
	}
	const auto SampleGrip = [&](float Pitch)
	{
		AimController->SetControlRotation(FRotator(Pitch, 0.0f, 0.0f));
		TestTrue(TEXT("Animation aim is supplied by the actual Pawn/controller view path"),
			FMath::IsNearlyEqual(FRotator::NormalizeAxis(Character->GetBaseAimRotation().Pitch), Pitch, 0.1f));
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			BodyMesh->TickAnimation(1.0f / 60.0f, false);
			BodyMesh->RefreshBoneTransforms();
		}
		return BodyMesh->GetBoneTransform(GunIndex).GetRelativeTransform(BodyMesh->GetBoneTransform(HandIndex));
	};
	const FTransform NeutralGrip = SampleGrip(0.0f);
	AddInfo(FString::Printf(TEXT("Complete-body idle absolute hand-to-gun offset: %s (%.3f cm)."),
		*NeutralGrip.GetTranslation().ToCompactString(), NeutralGrip.GetTranslation().Size()));
	TestTrue(TEXT("The absolute idle weapon anchor stays within the authored hand grip, not at the body's origin"),
		NeutralGrip.GetTranslation().Size() > 1.0 && NeutralGrip.GetTranslation().Size() < 25.0);
	for (const float Pitch : {60.0f, -60.0f})
	{
		const FTransform PitchedGrip = SampleGrip(Pitch);
		const float GripTranslationError = FVector::Distance(NeutralGrip.GetTranslation(), PitchedGrip.GetTranslation());
		const float GripRotationError = FMath::RadiansToDegrees(NeutralGrip.GetRotation().AngularDistance(PitchedGrip.GetRotation()));
		AddInfo(FString::Printf(TEXT("RAR stable idle aim 0->%.0f degrees: hand-to-gun drift %.4f cm, %.4f degrees (pose evaluation, not visual/network acceptance)."),
			Pitch, GripTranslationError, GripRotationError));
		TestTrue(FString::Printf(TEXT("Idle pitch %.0f preserves the authored grip within one centimeter"), Pitch),
			FMath::IsFinite(GripTranslationError) && GripTranslationError < 1.0f);
		TestTrue(FString::Printf(TEXT("Idle pitch %.0f preserves hand-to-gun orientation within two degrees"), Pitch),
			FMath::IsFinite(GripRotationError) && GripRotationError < 2.0f);
	}
	SampleGrip(0.0f);
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	Weapon->StartFire();
	Weapon->StopFire();
	TestEqual(TEXT("RAR input uses GAS and spends one authoritative round"), Weapon->GetCurrentAmmo(), AmmoBefore - 1);
	TestTrue(TEXT("One committed shot starts the body action"), Body->Montage_IsPlaying(Body->FireMontage));
	TestTrue(TEXT("One committed shot starts the first-person action"), Arms->Montage_IsPlaying(Arms->FireMontage));
	TestTrue(TEXT("One committed shot starts the gun mechanical action"), Gun->Montage_IsPlaying(Weapon->WeaponFireMontage));
	if (!TestTrue(TEXT("RAR reload is accepted through the actual ability"), Weapon->RequestReload())) return false;
	const int32 Cancelled = Weapon->GetActiveReloadId();
	TestTrue(TEXT("Reload starts matching body, arms and gun montages"), Body->Montage_IsPlaying(Body->ReloadMontage)
		&& Arms->Montage_IsPlaying(Arms->ReloadMontage) && Gun->Montage_IsPlaying(Weapon->WeaponReloadMontage));
	Weapon->StartFire();
	Weapon->StopFire();
	TestEqual(TEXT("Reload mutual exclusion prevents another shot"), Weapon->GetCurrentAmmo(), AmmoBefore - 1);
	Weapon->CancelReloadForTransaction(Cancelled);
	TestFalse(TEXT("Cancel closes the authoritative reload transaction"), Weapon->IsReloading());
	TestFalse(TEXT("Cancel stops all three reload presentations"), Body->Montage_IsPlaying(Body->ReloadMontage)
		|| Arms->Montage_IsPlaying(Arms->ReloadMontage) || Gun->Montage_IsPlaying(Weapon->WeaponReloadMontage));
	TestEqual(TEXT("Cancel before commit never creates ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore - 1);
	if (!TestTrue(TEXT("The same weapon accepts a new reload after cancel"), Weapon->RequestReload())) return false;
	const int32 Current = Weapon->GetActiveReloadId();
	TestTrue(TEXT("Restarted reload has a new identity"), Current != Cancelled);
	Weapon->CancelReloadForTransaction(Cancelled);
	TestTrue(TEXT("A delayed old cancellation cannot stop the new reload"), Weapon->IsReloading() && Weapon->GetActiveReloadId() == Current);
	Weapon->CancelReloadForTransaction(Current);
	Body->PresentHit();
	TestEqual(TEXT("Damage feedback only raises the visual hit parameter"), Body->HitAlpha, 1.0f);
	Character->GetMesh()->TickAnimation(0.016f, false);
	TestTrue(TEXT("The visual hit parameter decays without changing health"), Body->HitAlpha >= 0.0f && Body->HitAlpha < 1.0f);
	Weapon->DetachWeapon();
	Character->GetMesh()->TickAnimation(0.016f, false);
	Character->GetMesh1P()->TickAnimation(0.016f, false);
	TestFalse(TEXT("Unarmed state is read from equipment, not a second animation state machine"), Body->bArmed || Arms->bArmed);
	if (!TestTrue(TEXT("Death presentation fixture re-equips through the complete public transaction"), Weapon->AttachWeapon(Character))) return false;
	UGameplayStatics::ApplyDamage(Character, 1000.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("The real damage pipeline marks the RAR player dead"), Character->IsDead());
	TestTrue(TEXT("Death hides the RAR first-person arms but leaves the dropped world gun visible"),
		Character->GetMesh1P()->bHiddenInGame && !Weapon->bHiddenInGame && Weapon->IsDroppedOnDeath());
	TestNull(TEXT("The RAR death drop detaches its gun from the dead player's hand"), Weapon->GetAttachParent());
	// 生命、装备引用和 Actor 附着并不保证同批到达；重走真实迟到入口，不能显示死亡手臂或把掉落的世界枪挂回去。
	UFunction* EquipmentNotify = Character->FindFunction(TEXT("OnRep_EquippedWeapon"));
	if (!TestNotNull(TEXT("The production equipment RepNotify remains callable by replication"), EquipmentNotify)) return false;
	Character->ProcessEvent(EquipmentNotify, nullptr);
	TestTrue(TEXT("A late equipment reference preserves hidden RAR arms and the detached visible gun"),
		Character->GetMesh1P()->bHiddenInGame && !Weapon->bHiddenInGame && Weapon->GetAttachParent() == nullptr);
	Weapon->RefreshNetworkEquipment();
	TestTrue(TEXT("A repeated component refresh cannot reattach the RAR death drop or reveal dead arms"),
		Character->GetMesh1P()->bHiddenInGame && !Weapon->bHiddenInGame && Weapon->GetAttachParent() == nullptr);
	AfpstrueRARWeapon* Rifle = Cast<AfpstrueRARWeapon>(Weapon->GetOwner());
	if (!TestNotNull(TEXT("Late attachment uses the actual RAR weapon Actor"), Rifle)) return false;
	Rifle->OnRep_AttachmentReplication();
	TestTrue(TEXT("A late RAR Actor attachment snapshot leaves the world gun visible and detached"),
		Character->GetMesh1P()->bHiddenInGame && !Weapon->bHiddenInGame && Weapon->GetAttachParent() == nullptr);
	TArray<UStaticMeshComponent*> DeadParts;
	Rifle->GetComponents(DeadParts);
	for (const UStaticMeshComponent* Part : DeadParts)
		TestFalse(TEXT("The dropped RAR gun keeps each of its attached visible parts after late replication"), Part->bHiddenInGame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerTemplateDeathDropTest,
	"fpstrue.Multiplayer.Animation.TemplateDeathWeaponPhysicsAndLateReplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实角色、枪械和物理资产走伤害死亡链；仅验证物理启用与复制清理边界，不冒充落地观感或网络传输验收。
bool FFpstrueMultiplayerTemplateDeathDropTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	UClass* PawnClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C"));
	if (!TestNotNull(TEXT("Death drop uses the actual selected template character Blueprint"), PawnClass)) return false;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* Character = World->SpawnActor<AfpstrueCharacter>(PawnClass, FTransform::Identity, Spawn);
	auto* Controller = World->SpawnActor<AAIController>();
	auto* Rifle = World->SpawnActor<AfpstrueTemplateWeapon>(AfpstrueTemplateWeapon::StaticClass(),
		FTransform(FVector(5000.0f, 0.0f, 0.0f)), Spawn);
	if (!TestNotNull(TEXT("Death drop has its real template Pawn"), Character)
		|| !TestNotNull(TEXT("Death drop has a real ability-owning Controller"), Controller)
		|| !TestNotNull(TEXT("Death drop has its actual native template rifle"), Rifle)) return false;
	Character->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->SetActorTickEnabled(false);
	Controller->Possess(Character);
	auto* Weapon = Rifle->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("Death drop retains the production weapon component"), Weapon)) return false;
	auto* PickupShape = Rifle->FindComponentByClass<UfpstruePickUpComponent>();
	if (!TestNotNull(TEXT("The uncollected template rifle has its real pickup collision component"), PickupShape)) return false;
	const ECollisionEnabled::Type EquippedPickupCollision = PickupShape->GetCollisionEnabled();
	Weapon->SetHiddenInGame(true, true);
	Weapon->SetHiddenInGame(false, true);
	Weapon->SetVisibility(true, true);
	TestTrue(TEXT("Recursive equipment visibility never renders the pickup collision sphere"), PickupShape->bHiddenInGame);
	TestEqual(TEXT("Hiding pickup geometry does not change its collision policy"), PickupShape->GetCollisionEnabled(), EquippedPickupCollision);
	if (!TestTrue(TEXT("Death drop starts with a real authority equipment transaction"), Weapon->AttachWeapon(Character))) return false;
	Character->GetMesh1P()->SetHiddenInGame(false, true);
	// 权威端真实 Overlap 可以在挂接时消费并移除拾取球；观察端仍保留的球也不能被父级显示。
	TestTrue(TEXT("Attached equipment never leaves a visible pickup sphere, whether consumed or still present"),
		!IsValid(PickupShape) || PickupShape->bHiddenInGame);
	const UPhysicsAsset* GunPhysics = Weapon->GetPhysicsAsset();
	if (!TestNotNull(TEXT("The template gun retains its real source PhysicsAsset"), GunPhysics)) return false;
	TestEqual(TEXT("Death drop uses the migrated weapon PhysicsAsset rather than a generated placeholder"), GunPhysics->GetPathName(),
		FString(TEXT("/Game/FPSMPT55/Weapons/AssaultRifle/PA_AssaultRifle.PA_AssaultRifle")));
	TestTrue(TEXT("The weapon PhysicsAsset contains a nonempty collision body on a real gun bone"),
		GunPhysics->SkeletalBodySetups.ContainsByPredicate([Weapon](const USkeletalBodySetup* Setup)
		{
			return Setup != nullptr && Weapon->GetBoneIndex(Setup->BoneName) != INDEX_NONE && Setup->AggGeom.GetElementCount() > 0;
		}));
	// Standalone equipment does not need an owning connection; set the production-style owner to verify death clears it.
	Rifle->SetOwner(Character);
	TestTrue(TEXT("Death fixture begins with the live player as weapon Actor owner"), Rifle->GetOwner() == Character);
	Weapon->StartFire();
	Weapon->StopFire();
	if (!TestTrue(TEXT("A real reload transaction is active when lethal damage arrives"), Weapon->RequestReload())) return false;
	const int32 OldReloadId = Weapon->GetActiveReloadId();
	const int32 AmmoBeforeDeath = Weapon->GetCurrentAmmo();
	const FVector InheritedVelocity(120.0f, -40.0f, 30.0f);
	Character->GetCharacterMovement()->Velocity = InheritedVelocity;
	UGameplayStatics::ApplyDamage(Character, 1000.0f, nullptr, nullptr, nullptr);
	if (!TestTrue(TEXT("The real lethal damage chain marks the template player dead"), Character->IsDead())) return false;
	TestTrue(TEXT("The same death call starts body ragdoll without waiting for the corpse cleanup timer"), Character->GetMesh()->IsSimulatingPhysics());
	TestTrue(TEXT("The dead owner's first-person camera cannot render the inside of its own ragdoll"), Character->GetMesh()->bOwnerNoSee);
	TestTrue(TEXT("Owner hiding leaves the real ragdoll visible to other players"),
		!Character->GetMesh()->bOnlyOwnerSee && !Character->GetMesh()->bHiddenInGame && Character->GetMesh()->IsVisible());
	TestTrue(TEXT("The same death call starts a simulated weapon drop"), Weapon->IsDroppedOnDeath() && Weapon->IsSimulatingPhysics());
	TestNull(TEXT("The dropped gun is no longer parented to either hand mesh"), Weapon->GetAttachParent());
	TestNull(TEXT("A dropped weapon no longer belongs to the dead player's owning connection"), Rifle->GetOwner());
	TestTrue(TEXT("The dead Pawn retains its equipped reference for an out-of-order death snapshot"), Character->GetEquippedWeaponComponent() == Weapon);
	TestTrue(TEXT("Death hides first-person arms but keeps the independent world gun visible"),
		Character->GetMesh1P()->bHiddenInGame && !Weapon->bHiddenInGame && !Rifle->IsHidden());
	TestTrue(TEXT("Death drop visibility never reveals any remaining pickup collision sphere"),
		!IsValid(PickupShape) || PickupShape->bHiddenInGame);
	TestTrue(TEXT("The gun inherits the player's pre-death velocity rather than the stopped Movement velocity"),
		Weapon->GetPhysicsLinearVelocity().Equals(InheritedVelocity, 0.1f));
	TestEqual(TEXT("The dropped gun collides with the world floor"), Weapon->GetCollisionResponseToChannel(ECC_WorldStatic), ECR_Block);
	TestEqual(TEXT("A dropped gun cannot push a living player capsule"), Weapon->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);
	TestTrue(TEXT("The independent death drop has the bounded eight-second lifetime"), FMath::IsNearlyEqual(Rifle->GetLifeSpan(), 8.0f, 0.01f));
	TestFalse(TEXT("Death closes the in-flight reload transaction"), Weapon->IsReloading());
	TestFalse(TEXT("A stale pre-death reload commit cannot manufacture ammunition"), Weapon->CommitReloadForTransaction(OldReloadId));
	TestEqual(TEXT("Death and the stale commit leave the pre-death ammunition unchanged"), Weapon->GetCurrentAmmo(), AmmoBeforeDeath);
	const FTransform InitialDropTransform = Weapon->GetComponentTransform();
	const FVector InitialDropVelocity = Weapon->GetPhysicsLinearVelocity();
	const float InitialLifetime = Rifle->GetLifeSpan();
	Weapon->DropForDeath(FVector(-900.0f, 700.0f, 300.0f));
	TestTrue(TEXT("Repeating the death-drop command never resets its original world transform"), Weapon->GetComponentTransform().Equals(InitialDropTransform, 0.001f));
	TestTrue(TEXT("Repeating the death-drop command does not add inherited velocity a second time"), Weapon->GetPhysicsLinearVelocity().Equals(InitialDropVelocity, 0.1f));
	TestTrue(TEXT("Repeating the death-drop command cannot extend its cleanup lifetime"), FMath::IsNearlyEqual(Rifle->GetLifeSpan(), InitialLifetime, 0.001f));
	auto* LivePawn = World->SpawnActor<AfpstrueCharacter>(PawnClass, FTransform(FVector(10000.0f, 0.0f, 0.0f)), Spawn);
	if (!TestNotNull(TEXT("Pickup rejection has a real living template Pawn"), LivePawn)) return false;
	TestTrue(TEXT("Pickup rejection is checked while the dropped rifle still has authority"), Rifle->HasAuthority() && !LivePawn->IsDead());
	TestFalse(TEXT("The terminal death drop cannot be picked up by another live Pawn"), Weapon->AttachWeapon(LivePawn));
	TestFalse(TEXT("A rejected death-drop pickup leaves the live Pawn empty-handed"), LivePawn->HasEquippedWeapon());
	if (auto* Pickup = Rifle->FindComponentByClass<UfpstruePickUpComponent>())
		TestEqual(TEXT("The drop also disables its overlap-based pickup trigger"), Pickup->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	UFunction* EquipmentNotify = Character->FindFunction(TEXT("OnRep_EquippedWeapon"));
	if (!TestNotNull(TEXT("Death-drop fixture retains the real equipment RepNotify"), EquipmentNotify)) return false;
	Character->ProcessEvent(EquipmentNotify, nullptr);
	Weapon->RefreshNetworkEquipment();
	TestTrue(TEXT("Late equipment and component notifications cannot reattach or hide a dropped gun"),
		Weapon->IsDroppedOnDeath() && Weapon->GetAttachParent() == nullptr && Weapon->IsSimulatingPhysics() && !Weapon->bHiddenInGame);
	TestTrue(TEXT("Late equipment notifications cannot reveal the dead first-person viewmodel"), Character->GetMesh1P()->bHiddenInGame);
	TestTrue(TEXT("Late equipment notifications also preserve the dead owner's hidden third-person body"), Character->GetMesh()->bOwnerNoSee);
	// Feed the actual stale attachment payload, not just a no-op notification with an empty parent field.
	const FStructProperty* AttachmentProperty = FindFProperty<FStructProperty>(AActor::StaticClass(), TEXT("AttachmentReplication"));
	if (!TestNotNull(TEXT("Death-drop fixture retains the engine's actual replicated attachment"), AttachmentProperty)) return false;
	FRepAttachment* Attachment = AttachmentProperty->ContainerPtrToValuePtr<FRepAttachment>(Rifle);
	Attachment->AttachParent = Character;
	Attachment->AttachComponent = Character->GetMesh();
	Attachment->AttachSocket = TEXT("hand_r");
	Attachment->LocationOffset = Weapon->GripLocationOffset;
	Attachment->RotationOffset = Weapon->GripRotationOffset;
	Attachment->RelativeScale3D = FVector::OneVector;
	Rifle->SetRole(ROLE_SimulatedProxy);
	Rifle->OnRep_AttachmentReplication();
	TestTrue(TEXT("A late hand-attachment snapshot cannot weld a terminal physics drop back to the corpse"),
		Weapon->IsDroppedOnDeath() && Weapon->GetAttachParent() == nullptr && Weapon->IsSimulatingPhysics());
	const FStructProperty* StateProperty = FindFProperty<FStructProperty>(Weapon->GetClass(), TEXT("NetworkState"));
	UFunction* StateNotify = Weapon->FindFunction(TEXT("OnRep_NetworkState"));
	if (!TestNotNull(TEXT("Death drop retains its real replicated component snapshot"), StateProperty)
		|| !TestNotNull(TEXT("Death drop retains its real component RepNotify"), StateNotify)) return false;
	FFPReplicatedWeaponState* State = StateProperty->ContainerPtrToValuePtr<FFPReplicatedWeaponState>(Weapon);
	State->bDroppedOnDeath = false;
	State->Holder = Character;
	State->Action = EFPWeaponActionState::Ready;
	State->Revision += 10;
	Weapon->ProcessEvent(StateNotify, nullptr);
	Weapon->RefreshNetworkEquipment();
	TestTrue(TEXT("A later-delivered false component snapshot cannot undo the latched death drop"),
		Weapon->IsDroppedOnDeath() && Weapon->GetAttachParent() == nullptr && Weapon->IsSimulatingPhysics() && !Weapon->bHiddenInGame);
	TestTrue(TEXT("Late snapshots do not replay the drop's initial velocity"), Weapon->GetPhysicsLinearVelocity().Equals(InitialDropVelocity, 0.1f));
	TestTrue(TEXT("Late snapshots do not teleport the drop back to its old hand transform"), Weapon->GetComponentTransform().Equals(InitialDropTransform, 0.001f));
	TestTrue(TEXT("Late snapshots retain the bounded original drop lifetime"), FMath::IsNearlyEqual(Rifle->GetLifeSpan(), InitialLifetime, 0.001f));
	// 新的晚加入枪没有执行本地死亡链；分别验证 Actor 权威物理与组件掉落事实的两种到达顺序。
	const auto CheckLateJoinOrder = [&](bool bPhysicsFirst)
	{
		const FVector CurrentLocation = bPhysicsFirst ? FVector(20000.0f, 500.0f, 200.0f) : FVector(25000.0f, -500.0f, 200.0f);
		auto* LateRifle = World->SpawnActor<AfpstrueTemplateWeapon>(AfpstrueTemplateWeapon::StaticClass(), FTransform(CurrentLocation), Spawn);
		if (!TestNotNull(TEXT("Late-join order uses a fresh real template rifle"), LateRifle)) return false;
		auto* LateWeapon = LateRifle->FindComponentByClass<UfpstrueWeaponComponent>();
		if (!TestNotNull(TEXT("Late-join rifle retains the production component"), LateWeapon)) return false;
		TestFalse(TEXT("A fresh late-join rifle has no locally invented death-drop fact"), LateWeapon->IsDroppedOnDeath());
		LateRifle->SetReplicateMovement(true);
		LateRifle->SetRole(ROLE_SimulatedProxy);
		FFPReplicatedWeaponState* LateState = StateProperty->ContainerPtrToValuePtr<FFPReplicatedWeaponState>(LateWeapon);
		LateState->bDroppedOnDeath = true;
		LateState->Action = EFPWeaponActionState::Disabled;
		LateState->Holder = Character;
		LateState->DropTransform = FTransform(FRotator(0.0f, 20.0f, 0.0f), FVector(50.0f, 20.0f, 100.0f));
		LateState->DropVelocity = FVector(120.0f, -40.0f, 30.0f);
		++LateState->Revision;
		if (bPhysicsFirst)
		{
			const FFPReplicatedWeaponState IncomingDropState = *LateState;
			*LateState = FFPReplicatedWeaponState{};
			FRepMovement Physics;
			Physics.bRepPhysics = true;
			Physics.Location = CurrentLocation;
			Physics.Rotation = FRotator::ZeroRotator;
			Physics.LinearVelocity = FVector(25.0f, 40.0f, -15.0f);
			LateRifle->SetReplicatedMovement(Physics);
			LateRifle->OnRep_ReplicatedMovement();
			TestTrue(TEXT("Actor physics arriving first enables and latches the real drop before the component RepNotify"),
				!LateState->bDroppedOnDeath && LateWeapon->IsDroppedOnDeath() && LateWeapon->IsSimulatingPhysics());
			*LateState = IncomingDropState;
		}
		else
		{
			LateWeapon->ProcessEvent(StateNotify, nullptr);
			TestTrue(TEXT("A first drop-state snapshot initializes a fresh late-join gun at its saved drop transform"),
				LateWeapon->IsDroppedOnDeath() && LateWeapon->GetComponentTransform().Equals(LateState->DropTransform, 0.001f));
		}
		// 固定引擎已经接收/推进的物理快照；不伪造 Drop 标记，也不把此单元用例说成 Chaos/网络传输测试。
		const FTransform CurrentPhysics(FRotator(10.0f, 65.0f, -15.0f), CurrentLocation);
		const FVector CurrentVelocity(25.0f, 40.0f, -15.0f);
		LateWeapon->SetWorldTransform(CurrentPhysics, false, nullptr, ETeleportType::TeleportPhysics);
		LateWeapon->SetPhysicsLinearVelocity(CurrentVelocity);
		if (bPhysicsFirst) LateWeapon->RefreshNetworkPhysics();
		LateWeapon->ProcessEvent(StateNotify, nullptr);
		TestTrue(TEXT("A late or duplicate drop-state snapshot cannot restore the old hand transform over current physics"),
			LateWeapon->GetComponentTransform().Equals(CurrentPhysics, 0.001f));
		TestTrue(TEXT("A late or duplicate drop-state snapshot cannot replay its initial inherited velocity"),
			LateWeapon->GetPhysicsLinearVelocity().Equals(CurrentVelocity, 0.1f));
		FRepMovement Physics;
		Physics.bRepPhysics = true;
		Physics.Location = CurrentLocation;
		Physics.Rotation = CurrentPhysics.Rotator();
		Physics.LinearVelocity = CurrentVelocity;
		LateRifle->SetReplicatedMovement(Physics);
		LateRifle->OnRep_ReplicatedMovement();
		LateWeapon->ProcessEvent(StateNotify, nullptr);
		LateWeapon->RefreshNetworkPhysics();
		TestTrue(TEXT("Repeating both authoritative physics and drop-state notifications never jumps the gun back to its hand"),
			LateWeapon->IsDroppedOnDeath() && LateWeapon->IsSimulatingPhysics() && LateWeapon->GetAttachParent() == nullptr
			&& LateWeapon->GetComponentTransform().Equals(CurrentPhysics, 0.001f)
			&& LateWeapon->GetPhysicsLinearVelocity().Equals(CurrentVelocity, 0.1f));
		return true;
	};
	if (!CheckLateJoinOrder(true) || !CheckLateJoinOrder(false)) return false;
	// 同一组件类按所属实例决定复制通道；不得为了原生枪改写共享 Class 复制布局而误伤旧蓝图武器。
	auto* LegacyActor = World->SpawnActor<AActor>(FVector(30000.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Custom-condition isolation has an unrelated legacy weapon Actor"), LegacyActor)) return false;
	auto* LegacyWeapon = NewObject<UfpstrueWeaponComponent>(LegacyActor);
	LegacyActor->AddInstanceComponent(LegacyWeapon);
	LegacyActor->SetRootComponent(LegacyWeapon);
	LegacyWeapon->RegisterComponent();
	const auto CheckSceneConditions = [&](const UfpstrueWeaponComponent* Component, bool bExpectedActive)
	{
		UClass* Class = Component->GetClass();
		Class->SetUpRuntimeReplicationData();
		FCustomPropertyConditionState Conditions(Class->ClassReps.Num());
		Component->GetReplicatedCustomConditionState(Conditions);
		for (const TCHAR* Name : {TEXT("RelativeLocation"), TEXT("RelativeRotation"), TEXT("RelativeScale3D"),
			TEXT("AttachParent"), TEXT("AttachSocketName"), TEXT("AttachChildren"), TEXT("bShouldBeAttached"),
			TEXT("bShouldSnapLocationWhenAttached"), TEXT("bShouldSnapRotationWhenAttached"), TEXT("bShouldSnapScaleWhenAttached")})
		{
			const FProperty* Property = FindFProperty<FProperty>(Class, Name);
			if (!TestNotNull(FString(TEXT("The inspected Scene replication property exists: ")) + Name, Property)) return false;
			if (!TestTrue(TEXT("The Scene property has a valid runtime replication index"),
				Property->HasAnyPropertyFlags(CPF_Net) && Property->RepIndex < Conditions.GetNumProperties())) return false;
			TestEqual(FString(TEXT("Per-instance Scene channel activity: ")) + Name, Conditions.GetActiveState(Property->RepIndex), bExpectedActive);
		}
		const FProperty* GameplayState = FindFProperty<FProperty>(Class, TEXT("NetworkState"));
		if (!TestNotNull(TEXT("Custom Scene filtering retains the gameplay snapshot property"), GameplayState)) return false;
		TestTrue(TEXT("Disabling the duplicate Scene channels never disables the weapon gameplay snapshot"), Conditions.GetActiveState(GameplayState->RepIndex));
		return true;
	};
	if (!CheckSceneConditions(Weapon, false) || !CheckSceneConditions(LegacyWeapon, true) || !CheckSceneConditions(Weapon, false)) return false;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerRARReloadProgressTest,
	"fpstrue.Multiplayer.Animation.RARReceivedReloadProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 接收中途换弹的真实 RepNotify，并让手臂动画延迟就绪；这里只验证快照和时钟，不冒充断线网络传输验收。
bool FFpstrueMultiplayerRARReloadProgressTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!BeginMultiplayerUnitWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueCharacter* Character = nullptr;
	UfpstrueWeaponComponent* Weapon = nullptr;
	if (!SpawnRARPresentationFixture(*World, *this, Character, Weapon)) return false;
	USkeletalMeshComponent* ArmsMesh = Character->GetMesh1P();
	UClass* ArmsClass = ArmsMesh->GetAnimClass();
	ArmsMesh->SetAnimInstanceClass(nullptr);
	TGuardValue<TEnumAsByte<EWorldType::Type>> WorldTypeScope(World->WorldType, EWorldType::PIE);
	World->SetPlayInEditorInitialNetMode(NM_Client);
	Character->SetRole(ROLE_SimulatedProxy);
	Weapon->GetOwner()->SetRole(ROLE_SimulatedProxy);
	if (!TestTrue(TEXT("Snapshot fixture actually enters the client presentation branch without a transport"), World->GetNetMode() == NM_Client)) return false;
	const FStructProperty* StateProperty = FindFProperty<FStructProperty>(Weapon->GetClass(), TEXT("NetworkState"));
	UFunction* OnRep = Weapon->FindFunction(TEXT("OnRep_NetworkState"));
	if (!TestNotNull(TEXT("Reload uses the actual replicated weapon snapshot"), StateProperty)
		|| !TestNotNull(TEXT("Reload uses the actual RepNotify"), OnRep)) return false;
	FFPReplicatedWeaponState* State = StateProperty->ContainerPtrToValuePtr<FFPReplicatedWeaponState>(Weapon);
	State->Holder = Character;
	State->Action = EFPWeaponActionState::Reloading;
	State->ReloadId = 71;
	State->Ammo = 7;
	State->Reserve = 32;
	State->ReloadDuration = Weapon->GetWeaponSettings().ReloadDuration;
	State->ReloadStartServerTime = World->GetTimeSeconds() - State->ReloadDuration * 0.45;
	State->Revision += 10;
	Weapon->ProcessEvent(OnRep, nullptr);
	TestTrue(TEXT("Received server clock identifies the current reload phase"), FMath::IsNearlyEqual(Weapon->GetReloadProgress(), 0.45f, 0.01f));
	ArmsMesh->SetAnimInstanceClass(ArmsClass);
	ArmsMesh->TickAnimation(0.001f, false);
	ArmsMesh->RefreshBoneTransforms();
	const UfpstruePlayerAnimInstance* Body = Cast<UfpstruePlayerAnimInstance>(Character->GetMesh()->GetAnimInstance());
	const UfpstruePlayerAnimInstance* Arms = Cast<UfpstruePlayerAnimInstance>(ArmsMesh->GetAnimInstance());
	const UAnimInstance* Gun = Weapon->GetAnimInstance();
	if (!TestNotNull(TEXT("Existing body animation remains available"), Body) || !TestNotNull(TEXT("Late arms animation initializes"), Arms)
		|| !TestNotNull(TEXT("Existing gun animation remains available"), Gun)) return false;
	TestTrue(TEXT("The late animation reads current equipment and the active reload"), Arms->bArmed && Weapon->GetActiveReloadId() == 71);
	for (const TPair<const UAnimInstance*, const UAnimMontage*>& Playback : {
		TPair<const UAnimInstance*, const UAnimMontage*>(Body, Body->ReloadMontage),
		TPair<const UAnimInstance*, const UAnimMontage*>(Arms, Arms->ReloadMontage),
		TPair<const UAnimInstance*, const UAnimMontage*>(Gun, Weapon->WeaponReloadMontage)})
	{
		TestTrue(TEXT("Each view joins the current reload rather than restarting at frame zero"), Playback.Key->Montage_IsPlaying(Playback.Value)
			&& FMath::IsNearlyEqual(Playback.Key->Montage_GetPosition(Playback.Value) / Playback.Value->GetPlayLength(), 0.45f, 0.02f));
	}
	TestEqual(TEXT("Client presentation does not execute an ammunition commit"), Weapon->GetCurrentAmmo(), 7);
	TestFalse(TEXT("Even a matching client reload identity cannot commit ammo"), Weapon->CommitReloadForTransaction(71));
	State->Action = EFPWeaponActionState::Ready;
	State->EndReason = EFPReloadEndReason::Cancelled;
	++State->Revision;
	Weapon->ProcessEvent(OnRep, nullptr);
	TestFalse(TEXT("Authoritative cancellation clears the received transaction"), Weapon->IsReloading());
	TestFalse(TEXT("Authoritative cancellation stops each active presentation"), Body->Montage_IsPlaying(Body->ReloadMontage)
		|| Arms->Montage_IsPlaying(Arms->ReloadMontage) || Gun->Montage_IsPlaying(Weapon->WeaponReloadMontage));
	TestEqual(TEXT("Cancellation cannot invent or roll back received ammunition"), Weapon->GetCurrentAmmo(), 7);
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
