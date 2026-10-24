// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Net/UnrealNetwork.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponReplicationAudienceTest,
	"fpstrue.Multiplayer.Weapon.ReplicationAudienceAndArrivalOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查真实复制条件和 RepNotify 消费顺序；固定输入模拟网络到达，不冒充真实连接/丢包传输验收。
bool FFpstrueWeaponReplicationAudienceTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AActor* WeaponActor = World->SpawnActor<AActor>();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* Holder = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("A native Actor owns the replicated component"), WeaponActor)
		|| !TestNotNull(TEXT("The public holder is a real Character"), Holder)) return false;
	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(WeaponActor);
	WeaponActor->AddInstanceComponent(Weapon);
	WeaponActor->SetRootComponent(Weapon);
	Weapon->RegisterComponent();
	Holder->SetActorEnableCollision(false);
	Holder->SetActorTickEnabled(false);

	UClass* Class = Weapon->GetClass();
	Class->SetUpRuntimeReplicationData();
	const FProperty* PublicProperty = FindFProperty<FProperty>(Class, TEXT("NetworkState"));
	const FProperty* OwnerProperty = FindFProperty<FProperty>(Class, TEXT("OwnerState"));
	if (!TestNotNull(TEXT("Public state is reflected"), PublicProperty)
		|| !TestNotNull(TEXT("Owner state is reflected"), OwnerProperty)) return false;
	TArray<FLifetimeProperty> Properties;
	Weapon->GetLifetimeReplicatedProps(Properties);
	const FLifetimeProperty* PublicLifetime = Properties.FindByPredicate(
		[PublicProperty](const FLifetimeProperty& Property) { return Property.RepIndex == PublicProperty->RepIndex; });
	const FLifetimeProperty* OwnerLifetime = Properties.FindByPredicate(
		[OwnerProperty](const FLifetimeProperty& Property) { return Property.RepIndex == OwnerProperty->RepIndex; });
	if (!TestNotNull(TEXT("Public state has a lifetime replication entry"), PublicLifetime)
		|| !TestNotNull(TEXT("Owner state has a lifetime replication entry"), OwnerLifetime)) return false;
	TestTrue(TEXT("Public equipment and presentation reach relevant observers"), PublicLifetime->Condition == COND_None);
	TestTrue(TEXT("Private ammunition and decisions only reach the owning connection"), OwnerLifetime->Condition == COND_OwnerOnly);
	for (const TCHAR* PrivateName : {TEXT("Ammo"), TEXT("Reserve"), TEXT("LastProcessedShotId"), TEXT("ShotResult")})
	{
		TestNull(FString(TEXT("Public state has no obsolete private field: ")) + PrivateName,
			FindFProperty<FProperty>(FFPReplicatedWeaponState::StaticStruct(), PrivateName));
	}
	for (const TCHAR* AmmoName : {TEXT("Ammo"), TEXT("Reserve")})
		TestNotNull(FString(TEXT("Owner state carries private ammunition: ")) + AmmoName,
			FindFProperty<FProperty>(FFPReplicatedWeaponOwnerState::StaticStruct(), AmmoName));
	for (const TCHAR* AckName : {TEXT("LastProcessedShotId"), TEXT("ShotResult")})
		TestNull(FString(TEXT("Reliable per-request ACK needs no duplicate persistent owner field: ")) + AckName,
			FindFProperty<FProperty>(FFPReplicatedWeaponOwnerState::StaticStruct(), AckName));

	// 唯一发布从现有业务字段派生，两份版本/代际一致；不另维护可写弹药规则。
	Weapon->Character = Holder;
	Weapon->bEquipmentEnabled = true;
	Weapon->EquipmentRevision = 7;
	Weapon->CurrentAmmo = 19;
	Weapon->ReserveAmmo = 41;
	Weapon->LastProcessedShotId = 5;
	Weapon->LastShotResult = EFPShotResult::Accepted;
	{
		// 仅令同步发布进入联网分支，不创建网络驱动或实际旅行。
		TGuardValue<FString> ListenMode(World->NextURL, TEXT("?listen"));
		if (!TestTrue(TEXT("Publish fixture enters the server network branch"), Weapon->GetNetMode() != NM_Standalone)) return false;
		Weapon->PublishNetworkState();
	}
	TestEqual(TEXT("One publish stamps both audiences once"), Weapon->OwnerState.Revision, Weapon->NetworkState.Revision);
	TestEqual(TEXT("One publish uses the same equipment identity"), Weapon->OwnerState.EquipmentGeneration, Weapon->NetworkState.EquipmentGeneration);
	TestEqual(TEXT("Owner snapshot derives the actual magazine"), Weapon->OwnerState.Ammo, 19);
	TestEqual(TEXT("Owner snapshot derives the actual reserve"), Weapon->OwnerState.Reserve, 41);
	TestEqual(TEXT("Publishing audience snapshots preserves the server's real finalized request"), Weapon->LastProcessedShotId, uint32(5));
	TestTrue(TEXT("Publishing audience snapshots preserves the server's real finalized result"), Weapon->LastShotResult == EFPShotResult::Accepted);
	TestTrue(TEXT("Public snapshot retains the actual holder"), Weapon->NetworkState.Holder == Holder);

	// 以下固定收到的快照并走真实消费函数，不调用 Attach/Fire/Reload 等权威执行器。
	WeaponActor->SetRole(ROLE_SimulatedProxy);
	Holder->SetRole(ROLE_SimulatedProxy);
	Weapon->Character.Reset();
	Weapon->EquipmentRevision = 0;
	Weapon->CurrentAmmo = Weapon->ReserveAmmo = 0;
	FFPReplicatedWeaponState& Public = Weapon->NetworkState;
	FFPReplicatedWeaponOwnerState& Private = Weapon->OwnerState;
	Public = FFPReplicatedWeaponState();
	Public.Action = EFPWeaponActionState::Ready;
	Public.EquipmentGeneration = 7;
	Public.Revision = 10;
	Private.EquipmentGeneration = 7;
	Private.Revision = 10;
	Private.Ammo = 9;
	Private.Reserve = 31;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("Private-first delivery waits for the common equipment identity"), Weapon->GetCurrentAmmo(), 0);
	Weapon->OnRep_NetworkState();
	TestEqual(TEXT("An unresolved public holder does not consume the private snapshot"), Weapon->GetCurrentAmmo(), 0);
	Public.Holder = Holder;
	Weapon->OnRep_NetworkState(); // 同版本 NetGUID 后续解析，不必新建 Timer。
	TestEqual(TEXT("Holder resolution consumes the already received private snapshot"), Weapon->GetCurrentAmmo(), 9);
	TestEqual(TEXT("Holder resolution restores the matching reserve"), Weapon->GetReserveAmmo(), 31);
	TestNull(TEXT("Private consumption does not require the Controller reference to arrive first"), Holder->GetController());
	TestNull(TEXT("A missing equipped component does not block the owner HUD mirror"), Holder->GetEquippedWeaponComponent());

	Public.EquipmentGeneration = 8;
	Public.Revision = 20;
	Weapon->OnRep_NetworkState();
	TestEqual(TEXT("Public-first new equipment cannot inherit the old owner's magazine"), Weapon->GetCurrentAmmo(), 0);
	Private.EquipmentGeneration = 8;
	Private.Revision = 20;
	Private.Ammo = 8;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("Matching private data later fills the new equipment mirror"), Weapon->GetCurrentAmmo(), 8);

	Weapon->OutstandingShots.Add(51);
	Weapon->ClientShotResult_Implementation(8, 1, 51, EFPShotResult::Accepted, 6, 30, 24);
	TestTrue(TEXT("A reliable terminal ACK releases its pending estimate"), Weapon->OutstandingShots.IsEmpty());
	Public.Action = EFPWeaponActionState::Reloading;
	Public.ReloadId = 73;
	Public.Revision = 21;
	Weapon->OnRep_NetworkState();
	TestEqual(TEXT("A newer ammo ACK never hides an arriving public reload phase"), Weapon->GetActiveReloadId(), 73);
	Private.Revision = 22;
	Private.Ammo = 7;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("An older owner snapshot cannot overwrite the newer ACK magazine"), Weapon->GetCurrentAmmo(), 6);
	TestEqual(TEXT("An older owner snapshot cannot overwrite the newer ACK reserve"), Weapon->GetReserveAmmo(), 30);
	Weapon->OutstandingShots.Add(52);
	Weapon->ClientShotResult_Implementation(8, 1, 52, EFPShotResult::Cancelled, 8, 31, 23);
	TestTrue(TEXT("An older terminal ACK still releases its own estimate"), Weapon->OutstandingShots.IsEmpty());
	TestEqual(TEXT("An older terminal ACK does not regress the magazine"), Weapon->GetCurrentAmmo(), 6);
	Public.Action = EFPWeaponActionState::Ready;
	Public.EndReason = EFPReloadEndReason::Completed;
	Public.Revision = 25;
	Weapon->OnRep_NetworkState();
	Private.Revision = 25;
	Private.Ammo = 30;
	Private.Reserve = 6;
	Weapon->OnRep_OwnerState();
	TestFalse(TEXT("Public completion ends the received reload phase"), Weapon->IsReloading());
	TestEqual(TEXT("A newer owner snapshot replaces the older ACK"), Weapon->GetCurrentAmmo(), 30);
	Public.Action = EFPWeaponActionState::Disabled;
	Public.Revision = 24;
	Weapon->OnRep_NetworkState();
	TestTrue(TEXT("A stale common snapshot cannot undo newer presentation"), Weapon->LastPresentedAction == EFPWeaponActionState::Ready);
	Private.EquipmentGeneration = 7;
	Private.Revision = 100;
	Private.Ammo = 99;
	Weapon->OnRep_OwnerState();
	Weapon->ClientShotResult_Implementation(7, 1, 53, EFPShotResult::Accepted, 98, 99, 101);
	TestEqual(TEXT("Old equipment snapshots and ACKs cannot borrow a new holder's identity"), Weapon->GetCurrentAmmo(), 30);

	Private.EquipmentGeneration = 9;
	Private.Revision = 30;
	Private.Ammo = 15;
	Private.Reserve = 20;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("A future private equipment identity waits instead of changing the current magazine"), Weapon->GetCurrentAmmo(), 30);
	Public.Action = EFPWeaponActionState::Ready;
	Public.EquipmentGeneration = 9;
	Public.Revision = 30;
	Public.Holder = nullptr;
	Weapon->OnRep_NetworkState();
	TestEqual(TEXT("Unmapped holder references do not expose another identity's ammo"), Weapon->GetCurrentAmmo(), 0);
	Public.Holder = Holder;
	Weapon->OnRep_NetworkState();
	TestEqual(TEXT("The normal public readiness retry consumes the pending new generation"), Weapon->GetCurrentAmmo(), 15);
	TestEqual(TEXT("The readiness retry restores only matching reserve data"), Weapon->GetReserveAmmo(), 20);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
