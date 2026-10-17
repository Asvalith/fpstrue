// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Game/fpstrueMultiplayer.h"
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
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Testing/Automation/fpstrueTestObservers.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "InputAction.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

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
	if (!Fixture.TickTestWorld(60.0f / QueueWeapon->GetWeaponSettings().RoundsPerMinute + 0.02f))
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	TestTrue(TEXT("The original queued request still executes after the real cooldown"), QueueWeapon->ExecuteNetworkShot(Pending));
	TestEqual(TEXT("The original queued request finalizes its own identity"), QueueWeapon->LastProcessedShotId, Pending.ShotId);
	TestTrue(TEXT("The pending request finally publishes Accepted rather than a stale Duplicate result"),
		QueueWeapon->LastShotResult == EFPShotResult::Accepted && QueueWeapon->NetworkState.ShotResult == EFPShotResult::Accepted);
	TestEqual(TEXT("Queue plus repeated delivery spends exactly one pending round"), QueueWeapon->GetCurrentAmmo(), AmmoBeforeQueue - 1);
	TestTrue(TEXT("A completed queued request cannot execute a second time"), QueueWeapon->AcceptShotRequest(Pending) == EFPShotResult::Duplicate);
	TestTrue(TEXT("Repeating a completed request cannot replace its persistent accepted acknowledgement"),
		QueueWeapon->LastShotResult == EFPShotResult::Accepted && QueueWeapon->NetworkState.ShotResult == EFPShotResult::Accepted);
	TestEqual(TEXT("A finalized duplicate leaves the magazine unchanged"), QueueWeapon->GetCurrentAmmo(), AmmoBeforeQueue - 1);

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
	Controller->InitializeCombatContext(FirstPlayer, Manager);
	UAIPerceptionComponent* Perception = Controller->GetPerceptionComponent();
	if (!TestNotNull(TEXT("PvPvE authority uses the production sight listener"), Perception)) return false;
	FAIStimulus Seen(*GetDefault<UAISense_Sight>(), 1.0f, FirstPlayer->GetActorLocation(), Enemy->GetActorLocation(), FAIStimulus::SensingSucceeded);
	Perception->RegisterStimulus(FirstPlayer, Seen);
	Perception->ProcessStimuli();
	if (!TestTrue(TEXT("Actual sight discovers the injected player"), Controller->GetTargetCharacter() == FirstPlayer)) return false;
	if (Controller->GetBrainComponent() != nullptr) Controller->GetBrainComponent()->StopLogic(TEXT("PvPvE deterministic combat fixture"));
	UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("PvPvE production melee component"), Combat)) return false;
	TestTrue(TEXT("Server-side enemy acquires an attack permit"), Manager->TryAcquireAttackPermission(Enemy));
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
	if (Controller->GetBrainComponent() != nullptr) Controller->GetBrainComponent()->StopLogic(TEXT("PvPvE target lifecycle fixture"));
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
	// 导航生成另由地图烟测覆盖；这里只放入已有占用记录，观察生产清理是否对称撤销。
	FirstManager->EnemyToSlot.Add(EnemyKey, 0);
	FirstManager->SurroundSlots[0].Occupant = Enemy;
	AfpstrueSurroundManager::FCoverRequest Cover;
	Cover.State = EFPCoverQueryResult::Found;
	FirstManager->CoverRequests.Add(EnemyKey, Cover);
	TestTrue(TEXT("Target lifecycle starts with a real granted attack permit"), FirstManager->TryAcquireAttackPermission(Enemy));
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
	TestTrue(TEXT("Disconnect stores a bounded grace period"), Mode->RejoinSlots.FindChecked(InitialToken).ExpiresAt > World->GetTimeSeconds() &&
		Mode->RejoinSlots.FindChecked(InitialToken).ExpiresAt <= World->GetTimeSeconds() + 30.01);
	TestNull(TEXT("Disconnected Pawn is no longer controlled by its old connection"), Pawn->GetController());
	TestTrue(TEXT("The original server Pawn remains a combat participant during the grace period"), Mode->IsCombatParticipant(Pawn));
	Original->Destroy();
	UGameplayStatics::ApplyDamage(Pawn, 25.0f, nullptr, nullptr, nullptr);
	TestEqual(TEXT("A retained disconnected Pawn is not invulnerable"), Pawn->GetCurrentHealth(), 175.0f);
	AfpstruePvpPlayerController* Restored = NewClaim();
	if (!TestNotNull(TEXT("The returning controller"), Restored)) return false;
	Mode->CompleteConnectionHandshake(Restored, InitialToken);
	if (!TestTrue(TEXT("A valid disconnected token completes its production claim"), Restored->bConnectionHandshakeComplete)) return false;
	TestTrue(TEXT("Alive reconnect reuses the exact server Pawn"), Restored->GetPawn() == Pawn);
	TestTrue(TEXT("Reconnect reuses the exact server PlayerState"), Restored->GetPlayerState<AfpstruePvpPlayerState>() == State);
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
	TestTrue(TEXT("The same live participant can be retained on a later disconnect"), Mode->PreserveDisconnectedPlayer(Restored));
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
	TestFalse(TEXT("Expiry releases the retained server weapon"), IsValid(Weapon->GetOwner()) && !Weapon->GetOwner()->IsActorBeingDestroyed());
	AfpstruePvpPlayerController* TimedOut = NewClaim();
	if (!TestNotNull(TEXT("An incomplete owner handshake"), TimedOut)) return false;
	Mode->PendingConnectionHandshakes[TimedOut] = World->GetTimeSeconds() - 1.0;
	Mode->SweepRejoinSlots();
	TestFalse(TEXT("A timed-out owner handshake is no longer pending"), Mode->PendingConnectionHandshakes.Contains(TimedOut));
	TestFalse(TEXT("A timed-out owner handshake cannot remain an active player"), IsValid(TimedOut) && !TimedOut->IsActorBeingDestroyed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerConnectionRevisionTest,
	"fpstrue.Multiplayer.Reconnect.CancelInvalidatesOldTravelAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 不 Init、不建立 World、不读取票据：只驱动生产取消与旧版本回调，避免规则测试真的启动 Travel。
bool FFpstrueMultiplayerConnectionRevisionTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UfpstrueMultiplayerGameInstance> Instance(NewObject<UfpstrueMultiplayerGameInstance>());
	if (!TestNotNull(TEXT("Connection lifecycle fixture GameInstance"), Instance.Get())) return false;
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
	HUD->BindPawn(First);
	TestEqual(TEXT("Late HUD reads the current health snapshot"), HUD->CurrentHealth, First->GetCurrentHealth());
	TestEqual(TEXT("Late HUD reads the current ammo snapshot"), HUD->CurrentAmmo, FirstWeapon->GetCurrentAmmo());
	UGameplayStatics::ApplyDamage(First, 10.0f, SecondController, Second, nullptr);
	TestEqual(TEXT("Real health commit updates the subscribed HUD"), HUD->CurrentHealth, First->GetCurrentHealth());
	TestFalse(TEXT("Creating and binding HUD is not a real rendered frame"), HUD->HasDrawnCurrentState(First));
	HUD->BindPawn(Second);
	const float SecondHealth = HUD->CurrentHealth;
	const int32 SecondAmmo = HUD->CurrentAmmo;
	UGameplayStatics::ApplyDamage(First, 10.0f, SecondController, Second, nullptr);
	FirstWeapon->OnAmmoChanged.Broadcast(1, 30, 1);
	TestEqual(TEXT("Old Pawn health cannot overwrite rebound HUD"), HUD->CurrentHealth, SecondHealth);
	TestEqual(TEXT("Old weapon event cannot overwrite rebound HUD"), HUD->CurrentAmmo, SecondAmmo);
	SecondWeapon->OnAmmoChanged.Broadcast(7, 30, 80);
	TestEqual(TEXT("Current weapon remains subscribed"), HUD->CurrentAmmo, 7);
	HUD->BindPawn(nullptr);
	SecondWeapon->OnAmmoChanged.Broadcast(3, 30, 20);
	TestEqual(TEXT("Unbound HUD has no remaining weapon subscription"), HUD->CurrentAmmo, 0);
	TestFalse(TEXT("Unbound HUD never reports visual restoration"), HUD->HasDrawnCurrentState(Second));
	return true;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueMultiplayerBodyAnimationDefaultsTest,
	"fpstrue.Multiplayer.Animation.BodyDefaultsAndViewSeparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 身体与动画必须来自同一套骨架；只接一个可见网格，不能算已经接好第三人称动画。
bool FFpstrueMultiplayerBodyAnimationDefaultsTest::RunTest(const FString&)
{
	const AfpstrueCharacter* Defaults = GetDefault<AfpstrueCharacter>();
	USkeletalMeshComponent* Body = Defaults->GetMesh();
	const USkeletalMeshComponent* Arms = Defaults->GetMesh1P();
	if (!TestNotNull(TEXT("Default character has a third-person body component"), Body)
		|| !TestNotNull(TEXT("Default character retains its first-person arms component"), Arms)) return false;
	const USkeletalMesh* Mesh = Body->GetSkeletalMeshAsset();
	UClass* AnimClass = Body->GetAnimClass();
	if (!TestNotNull(TEXT("Third-person body has its real skeletal mesh"), Mesh)
		|| !TestNotNull(TEXT("Third-person body has a configured AnimBlueprint class"), AnimClass)) return false;
	const IAnimClassInterface* Animation = IAnimClassInterface::GetFromClass(AnimClass);
	if (!TestNotNull(TEXT("Body animation is a compiled AnimBlueprint, not a reference pose fallback"), Animation)) return false;
	TestEqual(TEXT("Body uses the migrated complete Manny mesh"), Mesh->GetPathName(),
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
	TestEqual(TEXT("Multiplayer spawns the project RAR presentation Blueprint"), PawnClass->GetPathName(),
		FString(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter.BP_RARCharacter_C")));
	const AfpstrueCharacter* BlueprintDefaults = PawnClass->GetDefaultObject<AfpstrueCharacter>();
	USkeletalMeshComponent* BlueprintBody = BlueprintDefaults->GetMesh();
	USkeletalMeshComponent* BlueprintArms = BlueprintDefaults->GetMesh1P();
	if (!TestNotNull(TEXT("Configured Blueprint retains its third-person body"), BlueprintBody)
		|| !TestNotNull(TEXT("Configured Blueprint retains its first-person arms"), BlueprintArms)) return false;
	TestTrue(TEXT("Blueprint component overrides do not replace the migrated body mesh"), BlueprintBody->GetSkeletalMeshAsset() == Mesh);
	TestEqual(TEXT("Configured Blueprint supplies the project full-body animation graph"), GetPathNameSafe(BlueprintBody->GetAnimClass()),
		FString(TEXT("/Game/FirstPerson/Animations/RAR/ABP_RARBody.ABP_RARBody_C")));
	TestTrue(TEXT("Configured Blueprint preserves owner-only arms"), BlueprintArms->bOnlyOwnerSee && !BlueprintArms->bOwnerNoSee);
	TestTrue(TEXT("Configured Blueprint preserves owner-hidden third-person body"), BlueprintBody->bOwnerNoSee && !BlueprintBody->bOnlyOwnerSee);
	if (!TestNotNull(TEXT("Configured Blueprint has the matching RAR first-person arms"), BlueprintArms->GetSkeletalMeshAsset())) return false;
	TestEqual(TEXT("RAR arms replace only the new presentation Blueprint, not the original RU74 asset"), BlueprintArms->GetSkeletalMeshAsset()->GetPathName(),
		FString(TEXT("/Game/InfimaGames/ArtCore/Characters/Meshes/SK_IG_FP_Arms.SK_IG_FP_Arms")));
	TestEqual(TEXT("Configured Blueprint supplies the project first-person animation graph"), GetPathNameSafe(BlueprintArms->GetAnimClass()),
		FString(TEXT("/Game/FirstPerson/Animations/RAR/ABP_RARArms.ABP_RARArms_C")));
	TestTrue(TEXT("The actual Blueprint enables its native RAR presentation path"), BlueprintDefaults->UsesRARPresentation());
	TestTrue(TEXT("Body and first-person arms share the exact RAR skeleton"),
		BlueprintArms->GetSkeletalMeshAsset()->GetSkeleton() == BlueprintBody->GetSkeletalMeshAsset()->GetSkeleton());
	UClass* OriginalClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter.BP_FirstPersonCharacter_C"));
	if (!TestNotNull(TEXT("Original first-person Blueprint remains available"), OriginalClass)) return false;
	const AfpstrueCharacter* OriginalDefaults = OriginalClass->GetDefaultObject<AfpstrueCharacter>();
	for (const TCHAR* Name : {TEXT("DefaultMappingContext"), TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"),
		TEXT("FireAction"), TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(PawnClass, Name);
		if (!TestNotNull(TEXT("RAR character retains the real input property"), Property)) return false;
		const UObject* ConfiguredInput = Property->GetObjectPropertyValue_InContainer(BlueprintDefaults);
		TestNotNull(FString(Name) + TEXT(" remains configured"), ConfiguredInput);
		TestTrue(FString(Name) + TEXT(" reuses the existing input asset without vendor input rules"),
			ConfiguredInput == Property->GetObjectPropertyValue_InContainer(OriginalDefaults));
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
	UClass* PawnClass = GetDefault<AfpstruePvpGameMode>()->DefaultPawnClass.Get();
	if (!TestNotNull(TEXT("Remote fixture uses the actual GameMode Pawn class"), PawnClass)
		|| !TestTrue(TEXT("Remote fixture Pawn class derives from the project character"), PawnClass->IsChildOf<AfpstrueCharacter>())) return false;
	AfpstrueCharacter* Character = Fixture.GetTestWorld()->SpawnActor<AfpstrueCharacter>(PawnClass);
	if (!TestNotNull(TEXT("Remote animation fixture is the real project character"), Character)) return false;
	TestTrue(TEXT("Runtime fixture executes the actual configured Blueprint class"), Character->GetClass() == PawnClass);
	Character->SetRole(ROLE_SimulatedProxy);
	USkeletalMeshComponent* Body = Character->GetMesh();
	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	UfpstruePlayerAnimInstance* Animation = Cast<UfpstruePlayerAnimInstance>(Body->GetAnimInstance());
	if (!TestNotNull(TEXT("Real body AnimBlueprint initializes in the test world"), Animation)) return false;
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
	Movement->SetMovementMode(MOVE_Walking);
	Movement->Velocity = FVector::ZeroVector;
	UpdateBody();
	TestTrue(TEXT("Stopping clears the animation's horizontal speed"), FMath::IsNearlyZero(Animation->GroundSpeed, 0.01f));
	TestTrue(TEXT("Stopping returns the real AnimBlueprint to idle"), FMath::IsNearlyZero(Animation->MovementAlpha));
	TestFalse(TEXT("Landing clears the animation's falling state"), Animation->bFalling);
	TestTrue(TEXT("Landing exposes a bounded visual transition instead of another movement state"), Animation->bLanding);
	return true;
}

namespace
{
// RAR 表现用例使用真正配置的角色、枪械和动画图；不复用无渲染资源的单骨业务夹具。
bool SpawnRARPresentationFixture(UWorld& World, FAutomationTestBase& Test,
	AfpstrueCharacter*& Character, UfpstrueWeaponComponent*& Weapon)
{
	UClass* PawnClass = GetDefault<AfpstruePvpGameMode>()->DefaultPawnClass.Get();
	if (!Test.TestNotNull(TEXT("RAR fixture has its actual configured Pawn class"), PawnClass)) return false;
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
	const AfpstruePvpGameMode* Mode = GetDefault<AfpstruePvpGameMode>();
	const FClassProperty* WeaponClassProperty = FindFProperty<FClassProperty>(Mode->GetClass(), TEXT("DefaultWeaponClass"));
	if (!TestNotNull(TEXT("GameMode exposes its configured weapon class"), WeaponClassProperty)) return false;
	TestTrue(TEXT("GameMode uses the native RAR pickup instead of the legacy weapon Blueprint"),
		WeaponClassProperty->GetObjectPropertyValue_InContainer(Mode) == AfpstrueRARWeapon::StaticClass());
	UClass* PawnClass = Mode->DefaultPawnClass.Get();
	if (!TestNotNull(TEXT("RAR asset gate loads the actual Pawn Blueprint"), PawnClass)) return false;
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
	TestTrue(TEXT("Death initially hides first-person arms and the equipped gun"), Character->GetMesh1P()->bHiddenInGame && Weapon->bHiddenInGame);
	// 生命、装备引用和 Actor 附着并不保证同批到达；重走真实迟到入口，不能把已死亡的手臂和枪重新显示。
	UFunction* EquipmentNotify = Character->FindFunction(TEXT("OnRep_EquippedWeapon"));
	if (!TestNotNull(TEXT("The production equipment RepNotify remains callable by replication"), EquipmentNotify)) return false;
	Character->ProcessEvent(EquipmentNotify, nullptr);
	TestTrue(TEXT("A late equipment reference cannot reveal dead first-person arms or gun"), Character->GetMesh1P()->bHiddenInGame && Weapon->bHiddenInGame);
	Weapon->RefreshNetworkEquipment();
	TestTrue(TEXT("A repeated component refresh cannot reveal dead first-person arms or gun"), Character->GetMesh1P()->bHiddenInGame && Weapon->bHiddenInGame);
	AfpstrueRARWeapon* Rifle = Cast<AfpstrueRARWeapon>(Weapon->GetOwner());
	if (!TestNotNull(TEXT("Late attachment uses the actual RAR weapon Actor"), Rifle)) return false;
	Rifle->OnRep_AttachmentReplication();
	TestTrue(TEXT("A late Actor attachment snapshot cannot reveal dead first-person arms or gun"), Character->GetMesh1P()->bHiddenInGame && Weapon->bHiddenInGame);
	TArray<UStaticMeshComponent*> DeadParts;
	Rifle->GetComponents(DeadParts);
	for (const UStaticMeshComponent* Part : DeadParts)
		TestTrue(TEXT("Death hiding also applies to each attached gun part after late replication"), Part->bHiddenInGame);
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
