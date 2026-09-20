// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "AnimationSharingManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueAnimationSharingStopTest,
	"fpstrue.Performance.AnimationSharing.StopAndSwapLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueAnimationSharingStopTest::RunTest(const FString& Parameters)
{
	// 使用临时世界与插件真实的 UnregisterActor；只省略依赖动画资产的注册 Setup。
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game))
	{
		World.ForwardErrorMessages(this);
		return false;
	}
	World.GetTestWorld()->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!World.BeginPlayInTestWorld())
	{
		World.ForwardErrorMessages(this);
		return false;
	}
	if (!TestTrue(TEXT("The plugin is enabled for the unregister regression"), UAnimationSharingManager::AnimationSharingEnabled()))
	{
		return false;
	}

	AActor* Owner = World.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("A temporary coordinator owner exists"), Owner))
	{
		return false;
	}
	UfpstrueEnemyAnimationSharingCoordinator* Coordinator = NewObject<UfpstrueEnemyAnimationSharingCoordinator>(Owner);
	Owner->AddInstanceComponent(Coordinator);
	Coordinator->RegisterComponent();
	UAnimationSharingManager* Manager = NewObject<UAnimationSharingManager>(World.GetTestWorld());
	Coordinator->SharingManager = Manager;

	// PerSkeletonData 是插件现有反射属性。通过它建立最小状态，避免测试专用插件子类、
	// 私有访问宏或未导出的 UAnimSharingInstance::StaticClass 链接依赖。
	const FArrayProperty* SkeletonDataProperty = FindFProperty<FArrayProperty>(Manager->GetClass(), TEXT("PerSkeletonData"));
	const FObjectPropertyBase* SkeletonEntryProperty = SkeletonDataProperty != nullptr
		? CastField<FObjectPropertyBase>(SkeletonDataProperty->Inner) : nullptr;
	if (!TestNotNull(TEXT("The plugin exposes its reflected skeleton data array"), SkeletonEntryProperty))
	{
		return false;
	}
	UAnimSharingInstance* Data = static_cast<UAnimSharingInstance*>(NewObject<UObject>(Manager, SkeletonEntryProperty->PropertyClass));
	SkeletonDataProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UAnimSharingInstance>>>(Manager)->Add(Data);

	USkeletalMeshComponent* Leader = NewObject<USkeletalMeshComponent>(Owner);
	Owner->AddInstanceComponent(Leader);
	Leader->RegisterComponent();
	TArray<AfpstrueEnemyCharacter*> Enemies;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AfpstrueEnemyCharacter* Enemy = World.GetTestWorld()->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("A native follower exists"), Enemy))
		{
			return false;
		}
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(FTransform::Identity);
		Enemies.Add(Enemy);
	}

	const auto SeedFollowers = [&]()
	{
		Coordinator->bRunning = true;
		for (int32 Index = 0; Index < Enemies.Num(); ++Index)
		{
			AfpstrueEnemyCharacter* Enemy = Enemies[Index];
			const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
			USkeletalMeshComponent* Mesh = Enemy->GetMesh();
			Mesh->SetLeaderPoseComponent(Leader, true);
			Mesh->SetComponentTickEnabled(false);
			Mesh->PrimaryComponentTick.bCanEverTick = false;
			Mesh->bIgnoreLeaderPoseComponentLOD = true;
			Data->RegisteredActors.Add(Enemy);
			FPerActorData& ActorData = Data->PerActorData.AddZeroed_GetRef();
			ActorData.ComponentIndices.Add(Index);
			ActorData.UpdateActorHandleDelegate = FUpdateActorHandle::CreateUObject(
				Coordinator, &UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated, EnemyKey);
			FPerComponentData& ComponentData = Data->PerComponentData.AddZeroed_GetRef();
			ComponentData.ActorIndex = Index;
			ComponentData.Component = Mesh;
			// Skeleton 0 的 ActorHandle 等于 ActorIndex，真实注销会在交换时重新计算它。
			Coordinator->RegisteredActorHandles.Add(EnemyKey, static_cast<uint32>(Index));
		}
	};

	SeedFollowers();
	Coordinator->Stop();
	TestFalse(TEXT("Stop disables new registrations"), Coordinator->bRunning);
	TestEqual(TEXT("Stop removes every plugin actor"), Data->RegisteredActors.Num(), 0);
	TestEqual(TEXT("Stop removes every plugin component"), Data->PerComponentData.Num(), 0);
	TestEqual(TEXT("Stop removes every plugin actor record"), Data->PerActorData.Num(), 0);
	TestEqual(TEXT("Stop removes every coordinator handle"), Coordinator->RegisteredActorHandles.Num(), 0);
	TestEqual(TEXT("Stop removes every pending request"), Coordinator->PendingActorRegistrations.Num(), 0);
	for (AfpstrueEnemyCharacter* Enemy : Enemies)
	{
		// 旧实现会在 Stop 的 swap 回调内递归注销尾对象，跳过该对象的 LOD 标志恢复。
		TestFalse(TEXT("Every follower regains its own LOD control"), Enemy->GetMesh()->bIgnoreLeaderPoseComponentLOD);
		TestFalse(TEXT("Every follower is detached from its leader"), Enemy->GetMesh()->LeaderPoseComponent.IsValid());
		TestTrue(TEXT("Every follower can tick independently"), Enemy->GetMesh()->PrimaryComponentTick.bCanEverTick);
	}
	Coordinator->HandleActorHandleUpdated(42, TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemies[0]));
	TestEqual(TEXT("An unknown callback cannot revive a stopped registration"), Coordinator->RegisteredActorHandles.Num(), 0);

	// 正常运行时移除首对象，插件尾对象换到首槽；后续 Significance 必须命中那个对象。
	SeedFollowers();
	Coordinator->SuspendEnemy(Enemies[0]);
	const uint32* SwappedHandle = Coordinator->RegisteredActorHandles.Find(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemies[2]));
	if (!TestNotNull(TEXT("The swapped follower keeps its registration"), SwappedHandle))
	{
		Coordinator->Stop();
		return false;
	}
	TestEqual(TEXT("The tail follower receives the first actor handle"), *SwappedHandle, uint32(0));
	TestEqual(TEXT("The actual plugin swaps the tail actor into the first slot"), Data->RegisteredActors[0].Get(), static_cast<AActor*>(Enemies[2]));
	Manager->UpdateSignificanceForActorHandle(*SwappedHandle, 0.75f);
	TestEqual(TEXT("The updated handle addresses the swapped actor"), Data->PerActorData[0].SignificanceValue, 0.75f);
	TestEqual(TEXT("The other follower significance is unchanged"), Data->PerActorData[1].SignificanceValue, 0.0f);
	Coordinator->SuspendEnemy(Enemies[0]);
	TestEqual(TEXT("Repeated Suspend does not remove another follower"), Data->RegisteredActors.Num(), 2);

	// 失败或中止的待注册请求也必须能退出，并归还可能已经接管的 LOD 标志。
	const TWeakObjectPtr<AfpstrueEnemyCharacter> PendingEnemy(Enemies[0]);
	Coordinator->PendingActorRegistrations.Add(PendingEnemy);
	Enemies[0]->GetMesh()->bIgnoreLeaderPoseComponentLOD = true;
	Coordinator->SuspendEnemy(Enemies[0]);
	TestFalse(TEXT("Suspend clears a pending request"), Coordinator->PendingActorRegistrations.Contains(PendingEnemy));
	TestFalse(TEXT("Suspend restores pending follower LOD control"), Enemies[0]->GetMesh()->bIgnoreLeaderPoseComponentLOD);
	Coordinator->Stop();
	Coordinator->Stop();
	TestEqual(TEXT("Repeated Stop leaves the plugin empty"), Data->RegisteredActors.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
