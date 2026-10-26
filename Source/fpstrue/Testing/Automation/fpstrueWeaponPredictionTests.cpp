// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Testing/Automation/fpstrueTestObservers.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "AIController.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponAmmoPredictionTest,
	"fpstrue.Multiplayer.Prediction.AmmoSnapshotAckReordering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 固定消息到达顺序，调用真实 RepNotify/ACK 消费入口；不把单进程端点回归冒充网络传输。
bool FFpstrueWeaponAmmoPredictionTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AActor* Actor = World->SpawnActor<AActor>();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* Holder = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("Prediction weapon actor"), Actor) || !TestNotNull(TEXT("Prediction holder"), Holder)) return false;
	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(Actor);
	Actor->AddInstanceComponent(Weapon);
	Actor->SetRootComponent(Weapon);
	Weapon->RegisterComponent();
	Actor->SetRole(ROLE_SimulatedProxy);
	Holder->SetRole(ROLE_SimulatedProxy);
	Weapon->NetworkState.Holder = Holder;
	Weapon->NetworkState.Action = EFPWeaponActionState::Ready;
	Weapon->NetworkState.EquipmentGeneration = 7;
	Weapon->NetworkState.Revision = 1;
	Weapon->OnRep_NetworkState();
	Weapon->OwnerState.EquipmentGeneration = 7;
	Weapon->OwnerState.Revision = 1;
	Weapon->OwnerState.Ammo = 10;
	Weapon->OwnerState.Reserve = 20;
	Weapon->OnRep_OwnerState();
	const auto MakeReceipt = [](uint32 Generation, uint32 Session, uint32 ShotId, EFPShotResult Result,
		int32 Ammo, int32 Reserve, uint32 Revision, uint32 IncludedShotId)
	{
		FFPShotReceipt Receipt;
		Receipt.EquipmentGeneration = Generation; Receipt.Session = Session; Receipt.ShotId = ShotId;
		Receipt.Result = Result; Receipt.Ammo = Ammo; Receipt.Reserve = Reserve;
		Receipt.StateRevision = Revision; Receipt.IncludedShotId = IncludedShotId;
		return Receipt;
	};
	const auto ReceiveFast = [&](uint32 Generation, uint32 Session, uint32 ShotId, EFPShotResult Result,
		int32 Ammo, int32 Reserve, uint32 Revision, uint32 IncludedShotId)
	{
		Weapon->ClientFastShotResult_Implementation({MakeReceipt(Generation, Session, ShotId, Result, Ammo, Reserve, Revision, IncludedShotId)});
	};
	TStrongObjectPtr<UfpstrueReloadReentryObserver> AmmoObserver(NewObject<UfpstrueReloadReentryObserver>());
	Weapon->OnAmmoChanged.AddDynamic(AmmoObserver.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);
	TestEqual(TEXT("Owner starts from the received authoritative baseline"), Weapon->GetCurrentAmmo(), 10);
	Weapon->OutstandingShots = {1, 2, 3};
	TestEqual(TEXT("Three pending inputs immediately deduct three displayed rounds"), Weapon->GetCurrentAmmo(), 7);
	TestEqual(TEXT("Prediction never overwrites the authoritative baseline"), Weapon->CurrentAmmo, 10);
	Weapon->ClientShotResult_Implementation(7, 1, 2, EFPShotResult::InvalidTime, 10, 20, 2, 0);
	TestEqual(TEXT("Rejecting the middle input refunds only that input"), Weapon->GetCurrentAmmo(), 8);
	TestTrue(TEXT("Earlier and later pending inputs both survive a middle rejection"),
		Weapon->OutstandingShots.Num() == 2 && Weapon->OutstandingShots.Contains(1) && Weapon->OutstandingShots.Contains(3));
	const int32 AfterFirstRejection = AmmoObserver->CallbackCount;
	ReceiveFast(7, 1, 2, EFPShotResult::InvalidTime, 10, 20, 2, 0);
	Weapon->ClientShotResult_Implementation(7, 1, 2, EFPShotResult::InvalidTime, 10, 20, 2, 0);
	TestEqual(TEXT("A repeated terminal rejection cannot refund twice"), Weapon->GetCurrentAmmo(), 8);
	TestEqual(TEXT("Reliable-first then fast duplicate does not notify the HUD again"), AmmoObserver->CallbackCount, AfterFirstRejection);
	Weapon->OwnerState.Revision = 3;
	Weapon->OwnerState.Ammo = 9;
	Weapon->OwnerState.IncludedShotId = 1;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("Snapshot before ACK does not deduct its included first shot twice"), Weapon->GetCurrentAmmo(), 8);
	TestEqual(TEXT("Snapshot updates do not falsely terminate the reliable input ledger"), Weapon->OutstandingShots.Num(), 2);
	ReceiveFast(7, 1, 1, EFPShotResult::Accepted, 9, 20, 2, 1);
	TestEqual(TEXT("Older ACK releases its input without regressing newer baseline or later debit"), Weapon->GetCurrentAmmo(), 8);
	TestEqual(TEXT("A fast terminal receipt is recorded until its reliable counterpart"), Weapon->FastShotReceipts.Num(), 1);
	const int32 AfterFastAcceptance = AmmoObserver->CallbackCount;
	ReceiveFast(7, 1, 1, EFPShotResult::Accepted, 9, 20, 2, 1);
	Weapon->ClientShotResult_Implementation(7, 1, 1, EFPShotResult::Accepted, 9, 20, 2, 1);
	TestEqual(TEXT("Fast-first then either duplicate produces exactly one HUD confirmation"), AmmoObserver->CallbackCount, AfterFastAcceptance);
	TestTrue(TEXT("Reliable convergence reclaims fast receipt history"), Weapon->FastShotReceipts.IsEmpty());
	Weapon->OwnerState.Revision = 5;
	Weapon->OwnerState.Ammo = 8;
	Weapon->OwnerState.IncludedShotId = 3;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("Snapshot containing the later shot converges before its final ACK"), Weapon->GetCurrentAmmo(), 8);
	Weapon->ClientShotResult_Implementation(7, 1, 3, EFPShotResult::Accepted, 8, 20, 4, 3);
	TestTrue(TEXT("All terminal ACKs finish the actual ledger"), Weapon->OutstandingShots.IsEmpty());
	TestEqual(TEXT("Finishing the ledger leaves exactly the authoritative magazine"), Weapon->GetCurrentAmmo(), 8);
	Weapon->OutstandingShots = {4, 5};
	Weapon->ClientShotResult_Implementation(6, 1, 4, EFPShotResult::Accepted, 99, 99, 100, 4);
	ReceiveFast(6, 1, 4, EFPShotResult::Accepted, 99, 99, 100, 4);
	TestEqual(TEXT("Old equipment ACK cannot overwrite or refund this generation"), Weapon->GetCurrentAmmo(), 6);
	TestEqual(TEXT("Old equipment ACK cannot consume a new input with the same number"), Weapon->OutstandingShots.Num(), 2);
	Weapon->FirstPredictedShotForCurrentFire = 5;
	Weapon->LocalShotSerial = 5;
	Weapon->EndAbilityFire(true, false);
	TestEqual(TEXT("Cancelling continuous fire retains submitted facts until their own verdicts"), Weapon->OutstandingShots.Num(), 2);
	Weapon->EndAbilityFire(true, true);
	TestTrue(TEXT("Actual activation rejection only discards its own prediction interval"),
		Weapon->OutstandingShots.Num() == 1 && Weapon->OutstandingShots.Contains(4));
	Weapon->OwnerState.EquipmentGeneration = 8;
	Weapon->OwnerState.Revision = 8;
	Weapon->OwnerState.Ammo = 6;
	Weapon->OwnerState.IncludedShotId = 0;
	Weapon->OnRep_OwnerState();
	TestEqual(TEXT("Future-generation private snapshot waits for equipment identity"), Weapon->GetCurrentAmmo(), 7);
	Weapon->LastPredictedShotTime = FPlatformTime::Seconds();
	Weapon->LastSubmittedFireTime = 50.0;
	Weapon->NetworkState.EquipmentGeneration = 8;
	Weapon->NetworkState.Revision = 8;
	Weapon->OnRep_NetworkState();
	TestTrue(TEXT("New equipment clears the old input ledger"), Weapon->OutstandingShots.IsEmpty());
	TestEqual(TEXT("New equipment resets included-shot watermark and reads its own baseline"), Weapon->GetCurrentAmmo(), 6);
	TestTrue(TEXT("Equipment ownership generation also resets both client cadence clock endpoints"),
		Weapon->LastPredictedShotTime < 0.0 && Weapon->LastSubmittedFireTime < 0.0);
	// 丢失快速副本不影响可靠收口；多个fast反向到达时仍按各自身份结束，而非误用最新快照水位。
	Weapon->OutstandingShots = {1, 2, 3};
	ReceiveFast(8, 3, 3, EFPShotResult::Accepted, 4, 20, 11, 3);
	ReceiveFast(8, 3, 2, EFPShotResult::InvalidTime, 5, 20, 10, 1);
	TestTrue(TEXT("Reordered fast receipts leave only the input with no terminal receipt"),
		Weapon->OutstandingShots.Num() == 1 && Weapon->OutstandingShots.Contains(1));
	Weapon->ClientShotResult_Implementation(8, 3, 1, EFPShotResult::Accepted, 5, 20, 9, 1);
	TestTrue(TEXT("Reliable backup alone finishes an input whose fast receipt was lost"), Weapon->OutstandingShots.IsEmpty());
	TestEqual(TEXT("An older reliable backup cannot regress newer fast authoritative ammunition"), Weapon->CurrentAmmo, 4);
	const int32 BeforeDuplicateBaseline = AmmoObserver->CallbackCount;
	ReceiveFast(8, 3, 3, EFPShotResult::Accepted, 3, 19, 12, 3);
	TestEqual(TEXT("A duplicate may still carry a newer persistent baseline"), Weapon->CurrentAmmo, 3);
	TestEqual(TEXT("A newer duplicate baseline refreshes the HUD only for its actual value change"), AmmoObserver->CallbackCount, BeforeDuplicateBaseline + 1);
	Weapon->ClientShotResult_Implementation(8, 3, 2, EFPShotResult::InvalidTime, 5, 20, 10, 1);
	Weapon->ClientShotResult_Implementation(8, 3, 3, EFPShotResult::Accepted, 4, 20, 11, 3);
	TestTrue(TEXT("Both delayed reliable receipts retire fast history without replaying confirmations"), Weapon->FastShotReceipts.IsEmpty());
	TestEqual(TEXT("Delayed reliable copies cannot overwrite the newer duplicate baseline"), Weapon->CurrentAmmo, 3);
	const int32 BeforeBoundedFastReceipts = AmmoObserver->CallbackCount;
	for (uint32 Id = 4; Id < 68; ++Id)
	{
		Weapon->OutstandingShots.Add(Id);
		ReceiveFast(8, 3, Id, EFPShotResult::InvalidTime, 3, 19, Id + 10, 3);
	}
	TestEqual(TEXT("Long reliable delay cannot grow fast receipt history beyond its fixed bound"), Weapon->FastShotReceipts.Num(), 64);
	TestEqual(TEXT("Each first fast terminal receipt notifies exactly once"), AmmoObserver->CallbackCount, BeforeBoundedFastReceipts + 64);
	Weapon->OutstandingShots.Add(68);
	ReceiveFast(8, 3, 68, EFPShotResult::InvalidTime, 3, 19, 78, 3);
	TestTrue(TEXT("A full fast history defers new terminal ownership to the reliable backup"), Weapon->OutstandingShots.Contains(68));
	Weapon->ClientShotResult_Implementation(8, 3, 68, EFPShotResult::InvalidTime, 3, 19, 78, 3);
	TestTrue(TEXT("Reliable backup still closes input when fast history was full"), Weapon->OutstandingShots.IsEmpty());
	TestTrue(TEXT("Reliable progress reclaims the bounded fast history"), Weapon->FastShotReceipts.IsEmpty());
	// 整个首个fast包丢失：不递送其单条裁决，后续3条窗口必须真正补回首发和中间拒绝。
	Weapon->OwnerState.EquipmentGeneration = 9; Weapon->OwnerState.Revision = 100;
	Weapon->OwnerState.Ammo = 10; Weapon->OwnerState.Reserve = 20; Weapon->OwnerState.IncludedShotId = 0;
	Weapon->OnRep_OwnerState();
	Weapon->NetworkState.EquipmentGeneration = 9; Weapon->NetworkState.Revision = 100;
	Weapon->OnRep_NetworkState();
	Weapon->OutstandingShots = {1, 2, 3, 4};
	const FFPShotReceipt RecoveredFirst = MakeReceipt(9, 4, 1, EFPShotResult::Accepted, 9, 20, 101, 1);
	const FFPShotReceipt RecoveredRejected = MakeReceipt(9, 4, 2, EFPShotResult::InvalidTime, 9, 20, 102, 1);
	const FFPShotReceipt RecoveredThird = MakeReceipt(9, 5, 3, EFPShotResult::Accepted, 8, 20, 103, 3);
	const FFPShotReceipt FourthReceipt = MakeReceipt(9, 5, 4, EFPShotResult::Accepted, 7, 20, 104, 4);
	const int32 BeforeRecoveredWindow = AmmoObserver->CallbackCount;
	Weapon->ClientFastShotResult_Implementation({RecoveredFirst, RecoveredRejected, RecoveredThird});
	TestTrue(TEXT("A later fast window finishes missing first and rejected inputs across normal fire sessions"),
		Weapon->OutstandingShots.Num() == 1 && Weapon->OutstandingShots.Contains(4));
	TestEqual(TEXT("Window recovery preserves the still-pending next shot's predicted debit"), Weapon->GetCurrentAmmo(), 7);
	TestEqual(TEXT("Each recovered terminal identity is consumed once"), AmmoObserver->CallbackCount, BeforeRecoveredWindow + 3);
	Weapon->OwnerState.Revision = 105; Weapon->OwnerState.Ammo = 7; Weapon->OwnerState.IncludedShotId = 4;
	Weapon->OnRep_OwnerState();
	TestTrue(TEXT("Snapshot convergence still cannot impersonate the missing final receipt"), Weapon->OutstandingShots.Contains(4));
	const TArray<FFPShotReceipt> ReversedWindow{FourthReceipt, RecoveredThird, RecoveredRejected, RecoveredFirst};
	const int32 BeforeReversedWindow = AmmoObserver->CallbackCount;
	Weapon->ClientFastShotResult_Implementation(ReversedWindow);
	Weapon->ClientFastShotResult_Implementation(ReversedWindow);
	TestTrue(TEXT("Reordered duplicate windows converge all identities"), Weapon->OutstandingShots.IsEmpty());
	TestEqual(TEXT("Window duplicates do not repeat terminal HUD notifications"), AmmoObserver->CallbackCount, BeforeReversedWindow + 1);
	Weapon->ClientShotResult_Implementation(9, 4, 1, EFPShotResult::Accepted, 9, 20, 101, 1);
	Weapon->ClientShotResult_Implementation(9, 4, 2, EFPShotResult::InvalidTime, 9, 20, 102, 1);
	Weapon->ClientShotResult_Implementation(9, 5, 3, EFPShotResult::Accepted, 8, 20, 103, 3);
	Weapon->ClientShotResult_Implementation(9, 5, 4, EFPShotResult::Accepted, 7, 20, 104, 4);
	TestTrue(TEXT("Reliable copies of window-recovered results reclaim history without another settlement"), Weapon->FastShotReceipts.IsEmpty());
	TestEqual(TEXT("Reliable copies after window recovery never regress the newer snapshot"), Weapon->CurrentAmmo, 7);
	Weapon->OwnerState.EquipmentGeneration = 10; Weapon->OwnerState.Revision = 110;
	Weapon->OwnerState.Ammo = 10; Weapon->OwnerState.IncludedShotId = 0;
	Weapon->OnRep_OwnerState();
	Weapon->NetworkState.EquipmentGeneration = 10; Weapon->NetworkState.Revision = 110;
	Weapon->OnRep_NetworkState();
	Weapon->OutstandingShots = {1};
	Weapon->ClientFastShotResult_Implementation(ReversedWindow);
	TestTrue(TEXT("An entire stale window cannot settle matching shot numbers in a new equipment generation"), Weapon->OutstandingShots.Contains(1));
	TestEqual(TEXT("An old window cannot lend an old owner's ammo to the new generation"), Weapon->CurrentAmmo, 10);
	ReceiveFast(10, 1, 1, EFPShotResult::Accepted, 9, 20, 111, 1);
	TestTrue(TEXT("The new generation accepts its own same-numbered terminal receipt"), Weapon->OutstandingShots.IsEmpty());
	// 实际显示时间以每个目标身份独立查询；未知目标才fallback，绝不把已有的旧显示时刻改新。
	AfpstrueCharacter* ViewTarget = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform(FVector(1000, 0, 0)), Spawn);
	AfpstrueCharacter* UnseenTarget = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform(FVector(1500, 0, 0)), Spawn);
	if (!TestNotNull(TEXT("Rendered-view target"), ViewTarget) || !TestNotNull(TEXT("Unknown new target"), UnseenTarget)) return false;
	FFPShotRequest ViewRequest;
	ViewRequest.Avatar = Holder; ViewRequest.Weapon = Actor; ViewRequest.ServerFireTime = 9.9;
	ViewRequest.RenderedViews.Add({ViewTarget, 82});
	bool bNativeView = false;
	TestTrue(TEXT("The target's real display timeline replaces the RTT estimate"),
		FMath::IsNearlyEqual(ViewRequest.GetTargetViewTime(ViewTarget, 9.7, &bNativeView), 9.818, 1.e-6) && bNativeView);
	bNativeView = true;
	TestEqual(TEXT("A new target with no native snapshot preserves its explicit fallback"), ViewRequest.GetTargetViewTime(UnseenTarget, 9.7, &bNativeView), 9.7);
	TestFalse(TEXT("Fallback can never be mislabeled as a native rendered time"), bNativeView);
	TestTrue(TEXT("A valid native rendered age enters the authority policy"), Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::Accepted);
	ViewRequest.RenderedViews[0].AgeMilliseconds = 350;
	TestTrue(TEXT("Old but valid native age is retained even beyond current history coverage"),
		FMath::IsNearlyEqual(ViewRequest.GetTargetViewTime(ViewTarget, 9.8, &bNativeView), 9.55, 1.e-6) && bNativeView
		&& Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::Accepted);
	ViewRequest.ServerFireTime = 9.96;
	ViewRequest.RenderedViews[0].AgeMilliseconds = -50;
	TestTrue(TEXT("The signed lower age bound can represent a valid native timeline ahead of clock estimate"),
		Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::Accepted);
	ViewRequest.ServerFireTime = 10.0;
	TestTrue(TEXT("Signed view age never widens the existing twenty-five millisecond future limit"),
		Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::InvalidTime);
	for (const int16 InvalidAge : {int16(-51), int16(351)})
	{
		ViewRequest.RenderedViews[0].AgeMilliseconds = InvalidAge;
		TestTrue(TEXT("Out-of-range rendered age is rejected before hitscan"), Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::InvalidTime);
	}
	ViewRequest.ServerFireTime = 9.9;
	ViewRequest.RenderedViews[0].AgeMilliseconds = 82;
	ViewRequest.RenderedViews.Add({ViewTarget, 90});
	TestTrue(TEXT("Two time choices for the same target are rejected"), Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::InvalidIdentity);
	ViewRequest.RenderedViews.SetNum(1);
	ViewRequest.RenderedViews[0].Target = Holder;
	TestTrue(TEXT("Target metadata cannot reinterpret the shooter's origin timeline"), Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::InvalidIdentity);
	ViewRequest.RenderedViews[0].Target = nullptr;
	TestTrue(TEXT("A target that vanished in flight does not invalidate the whole input"), Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::Accepted);
	ViewRequest.RenderedViews[0].Target = AfpstrueCharacter::StaticClass()->GetDefaultObject<AfpstrueCharacter>();
	TestTrue(TEXT("A character object outside the shot world cannot be used as target metadata"), Weapon->ValidateRenderedViews(ViewRequest, 10.0) == EFPShotResult::InvalidIdentity);
	ViewRequest.RenderedViews[0].Target = ViewTarget;
	FFPShotRequest NativeCopied;
	FFPShotRequest::StaticStruct()->CopyScriptStruct(&NativeCopied, &ViewRequest);
	bNativeView = false;
	TestTrue(TEXT("UScriptStruct native copy preserves the unreflected inline rendered-view payload"),
		NativeCopied.RenderedViews.Num() == 1 && NativeCopied.RenderedViews[0].Target == ViewTarget
		&& FMath::IsNearlyEqual(NativeCopied.GetTargetViewTime(ViewTarget, 0.0, &bNativeView), 9.818, 1.e-6) && bNativeView);
	// count帧界测试只绕过引擎GUID映射，不冒充真实网络Actor往返；目标身份准入使用上面的真实世界对象。
	FFPShotRequest CountRequest;
	CountRequest.AimDirection = FVector::ForwardVector;
	CountRequest.RenderedViews.SetNum(FFPShotRequest::MaxRenderedViews);
	for (int32 Index = 0; Index < CountRequest.RenderedViews.Num(); ++Index)
		CountRequest.RenderedViews[Index].AgeMilliseconds = Index % 2 == 0 ? int16(-50) : int16(350);
	FBitWriter Writer(1024, true);
	bool bSerialized = false;
	CountRequest.NetSerialize(Writer, nullptr, bSerialized);
	TestTrue(TEXT("Exactly sixteen signed rendered ages serialize within the bounded frame"), bSerialized && !Writer.IsError());
	FBitReader Reader(Writer.GetData(), Writer.GetNumBits());
	FFPShotRequest Decoded;
	Decoded.NetSerialize(Reader, nullptr, bSerialized);
	TestTrue(TEXT("The bounded frame restores sixteen entries without heap growth"), bSerialized && Decoded.RenderedViews.Num() == 16);
	TestTrue(TEXT("Signed sixteen-bit view ages survive the actual serializer"), Decoded.RenderedViews[0].AgeMilliseconds == -50 && Decoded.RenderedViews.Last().AgeMilliseconds == 350);
	CountRequest.RenderedViews.AddDefaulted();
	TestTrue(TEXT("Authority validation independently rejects a direct oversized rendered-view list"), Weapon->ValidateRenderedViews(CountRequest, 0.0) == EFPShotResult::InvalidIdentity);
	FBitWriter OversizedWriter(1024, true);
	CountRequest.NetSerialize(OversizedWriter, nullptr, bSerialized);
	TestTrue(TEXT("Seventeen rendered targets fail serialization before writing a partial request"), !bSerialized && OversizedWriter.IsError() && OversizedWriter.GetNumBits() == 0);
	CountRequest.RenderedViews.Reset();
	FBitWriter MalformedWriter(1024, true);
	CountRequest.NetSerialize(MalformedWriter, nullptr, bSerialized);
	// 空列表最后5位是count；注入31，验证加载在SetNum前拒绝而非按网络数量分配。
	for (int64 Bit = MalformedWriter.GetNumBits() - 5; Bit < MalformedWriter.GetNumBits(); ++Bit)
		MalformedWriter.GetData()[Bit >> 3] |= static_cast<uint8>(1u << (Bit & 7));
	FBitReader MalformedReader(MalformedWriter.GetData(), MalformedWriter.GetNumBits());
	Decoded.RenderedViews.Reset();
	Decoded.NetSerialize(MalformedReader, nullptr, bSerialized);
	TestTrue(TEXT("Forged wire counts above sixteen fail before creating target entries"), !bSerialized && MalformedReader.IsError() && Decoded.RenderedViews.IsEmpty());
	Weapon->OnAmmoChanged.RemoveDynamic(AmmoObserver.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponReloadPredictionTest,
	"fpstrue.Multiplayer.Prediction.ReloadPlaybackAdoptionAndAuthorityClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实作者蓝图与 MontageInstance 验证接管/拒绝；RPC 与弱网时延另由独立进程验收。
bool FFpstrueWeaponReloadPredictionTest::RunTest(const FString&)
{
	for (const TCHAR* Family : {TEXT("Template"), TEXT("RAR")})
	{
		FTestWorldWrapper Fixture;
		if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
		UWorld* World = Fixture.GetTestWorld();
		World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
		if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
		const FString CharacterPath = FString::Printf(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_%sCharacter.BP_%sCharacter_C"), Family, Family);
		const FString WeaponPath = FString::Printf(TEXT("/Game/FirstPerson/Blueprints/Weapons/BP_%sWeapon.BP_%sWeapon_C"), Family, Family);
		UClass* CharacterClass = LoadClass<AfpstrueCharacter>(nullptr, *CharacterPath);
		UClass* WeaponClass = LoadClass<AActor>(nullptr, *WeaponPath);
		if (!TestNotNull(TEXT("Authored character blueprint"), CharacterClass) || !TestNotNull(TEXT("Authored weapon blueprint"), WeaponClass)) return false;
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AfpstrueCharacter* Holder = World->SpawnActor<AfpstrueCharacter>(CharacterClass, FTransform::Identity, Spawn);
		APlayerController* Controller = World->SpawnActor<APlayerController>();
		AActor* Actor = World->SpawnActor<AActor>(WeaponClass, FTransform::Identity, Spawn);
		if (!TestNotNull(TEXT("Authored prediction holder"), Holder) || !TestNotNull(TEXT("Local controller"), Controller)
			|| !TestNotNull(TEXT("Authored prediction weapon"), Actor)) return false;
		Holder->SetActorEnableCollision(false);
		Holder->GetCharacterMovement()->SetComponentTickEnabled(false);
		Controller->Possess(Holder);
		UfpstrueWeaponComponent* Weapon = Actor->FindComponentByClass<UfpstrueWeaponComponent>();
		if (!TestNotNull(TEXT("Real weapon component"), Weapon) || !TestTrue(TEXT("Real authority equipment transaction"), Weapon->AttachWeapon(Holder))) return false;
		if (!Fixture.TickTestWorld(.01f)) { Fixture.ForwardErrorMessages(this); return false; }
		Weapon->CurrentAmmo = Weapon->GetMagazineSize() - 2;
		const int32 OriginalAmmo = Weapon->CurrentAmmo, OriginalReserve = Weapon->ReserveAmmo;
		UAbilitySystemComponent* ASC = Holder->GetAbilitySystemComponent();
		ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
		Actor->SetRole(ROLE_SimulatedProxy);
		if (!TestTrue(TEXT("Local prediction accepts a real magazine gap"), Weapon->BeginPredictedReload(123))) return false;
		TestTrue(TEXT("Prediction immediately exposes a local playback identity"), Weapon->IsReloading() && Weapon->ActiveReloadId < INDEX_NONE);
		TestFalse(TEXT("Local prediction does not create an authoritative completion timer"), World->GetTimerManager().TimerExists(Weapon->ReloadTimerHandle));
		if (!TestTrue(TEXT("Prediction starts actual authored montage instances"), !Weapon->ReloadPlaybacks.IsEmpty())) return false;
		TArray<FFPActionPlayback> PredictedInstances;
		for (const FFPActionPlayback& Playback : Weapon->ReloadPlaybacks) PredictedInstances.Add(Playback);
		Weapon->NetworkState.Holder = Holder;
		Weapon->NetworkState.Action = EFPWeaponActionState::Ready;
		Weapon->NetworkState.EquipmentGeneration = Weapon->EquipmentRevision;
		Weapon->NetworkState.Revision = 10;
		Weapon->OnRep_NetworkState();
		TestEqual(TEXT("Unrelated older Ready snapshot preserves pending prediction"), Weapon->ActiveReloadId, -124);
		Weapon->NetworkState.Action = EFPWeaponActionState::Reloading;
		Weapon->NetworkState.ReloadId = 44;
		Weapon->NetworkState.ReloadPredictionKey = 123;
		Weapon->NetworkState.ReloadStartServerTime = World->GetTimeSeconds();
		Weapon->NetworkState.ReloadDuration = Weapon->GetWeaponSettings().ReloadDuration;
		Weapon->NetworkState.Revision = 11;
		Weapon->OnRep_NetworkState();
		TestEqual(TEXT("Confirmation adopts the server transaction identity"), Weapon->ActiveReloadId, 44);
		TestEqual(TEXT("Confirmation neither adds nor restarts montage instances"), Weapon->ReloadPlaybacks.Num(), PredictedInstances.Num());
		for (int32 Index = 0; Index < PredictedInstances.Num(); ++Index)
		{
			TestEqual(TEXT("Adoption preserves the exact montage instance ID"), Weapon->ReloadPlaybacks[Index].MontageInstanceId, PredictedInstances[Index].MontageInstanceId);
			TestTrue(TEXT("Adoption preserves the exact playing mesh"), Weapon->ReloadPlaybacks[Index].Mesh == PredictedInstances[Index].Mesh);
			TestEqual(TEXT("Adoption rebinds the existing instance to the authority ID"), Weapon->ReloadPlaybacks[Index].ActionId, uint32(44));
		}
		TestFalse(TEXT("A predicted owner cannot commit the adopted authority reload"), Weapon->CommitReloadForTransaction(44));
		Weapon->EndPredictedReload(123);
		ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
		Weapon->OnRep_NetworkState();
		TestFalse(TEXT("A late Reloading snapshot for the explicitly ended key cannot resurrect the action"), Weapon->IsReloading());
		TestTrue(TEXT("Ended-key filtering clears the transaction instead of replaying its montage"),
			Weapon->ActiveReloadId == INDEX_NONE && Weapon->ReloadPlaybacks.IsEmpty());
		ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
		if (!TestTrue(TEXT("A subsequent prediction can reuse the same authored asset"), Weapon->BeginPredictedReload(456))) return false;
		const int32 NewInstance = Weapon->ReloadPlaybacks.IsEmpty() ? INDEX_NONE : Weapon->ReloadPlaybacks[0].MontageInstanceId;
		Weapon->EndPredictedReload(123);
		TestEqual(TEXT("Old rejection cannot stop a new prediction using the same montage asset"), Weapon->LocalReloadPredictionKey, 456);
		TestEqual(TEXT("Old rejection cannot replace the new playback instance"),
			Weapon->ReloadPlaybacks.IsEmpty() ? INDEX_NONE : Weapon->ReloadPlaybacks[0].MontageInstanceId, NewInstance);
		const float Duration = Weapon->GetReloadDuration();
		for (int32 Tick = 0; Tick < FMath::CeilToInt(Duration * 60.0f) + 3; ++Tick)
			if (!Fixture.TickTestWorld(1.0f / 60.0f)) { Fixture.ForwardErrorMessages(this); return false; }
		TestEqual(TEXT("Playing a complete local reload never manufactures ammunition"), Weapon->GetCurrentAmmo(), OriginalAmmo);
		TestEqual(TEXT("Playing a complete local reload never consumes reserve"), Weapon->GetReserveAmmo(), OriginalReserve);
		Weapon->EndPredictedReload(456);
		// 真实复制结束通知可同步开启下一次预测；旧快照必须在通知前释放自己的 key。
		if (!TestTrue(TEXT("A fresh prediction prepares the replicated-completion reentry fixture"), Weapon->BeginPredictedReload(789))) return false;
		Weapon->NetworkState.Action = EFPWeaponActionState::Reloading;
		Weapon->NetworkState.ReloadId = 45;
		Weapon->NetworkState.ReloadPredictionKey = 789;
		Weapon->NetworkState.ReloadStartServerTime = World->GetTimeSeconds();
		Weapon->NetworkState.Revision = 12;
		Weapon->OnRep_NetworkState();
		if (!TestEqual(TEXT("The reentry fixture adopts a real authority transaction"), Weapon->ActiveReloadId, 45)) return false;
		TStrongObjectPtr<UfpstrueReloadReentryObserver> CompletionObserver(NewObject<UfpstrueReloadReentryObserver>());
		bool bStartedDuringCompletion = false;
		int32 InstanceCreatedDuringCompletion = INDEX_NONE;
		CompletionObserver->OnReloadEnded = [&]()
		{
			bStartedDuringCompletion = Weapon->BeginPredictedReload(790);
			InstanceCreatedDuringCompletion = Weapon->ReloadPlaybacks.IsEmpty() ? INDEX_NONE : Weapon->ReloadPlaybacks[0].MontageInstanceId;
		};
		Weapon->OnWeaponReloadEnded.AddDynamic(CompletionObserver.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
		Weapon->NetworkState.Action = EFPWeaponActionState::Ready;
		Weapon->NetworkState.EndReason = EFPReloadEndReason::Cancelled;
		Weapon->NetworkState.Revision = 13;
		Weapon->OnRep_NetworkState();
		Weapon->OnWeaponReloadEnded.RemoveDynamic(CompletionObserver.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
		CompletionObserver->OnReloadEnded = nullptr;
		TestEqual(TEXT("The replicated completion publishes exactly one old-transaction end"), CompletionObserver->ReloadEndCount, 1);
		TestTrue(TEXT("The real completion callback starts the next predicted reload"), bStartedDuringCompletion);
		TestEqual(TEXT("Returning through the old snapshot cannot erase the new prediction key"), Weapon->LocalReloadPredictionKey, 790);
		TestEqual(TEXT("The new transaction keeps the negative identity owned by its key"), Weapon->ActiveReloadId, -791);
		TestTrue(TEXT("The reentrant prediction plays the authored montage instead of only changing flags"), InstanceCreatedDuringCompletion != INDEX_NONE);
		TestEqual(TEXT("Old-snapshot cleanup cannot stop or replace the newly created montage instance"),
			Weapon->ReloadPlaybacks.IsEmpty() ? INDEX_NONE : Weapon->ReloadPlaybacks[0].MontageInstanceId, InstanceCreatedDuringCompletion);
		TestEqual(TEXT("The new prediction still cannot manufacture ammunition"), Weapon->GetCurrentAmmo(), OriginalAmmo);
		Weapon->EndPredictedReload(790);
		ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
		Actor->SetRole(ROLE_Authority);
		Weapon->CurrentAmmo = 0;
		Weapon->ReserveAmmo = 10;
		// 在监听世界创建全新远端控制器，避免 Standalone 的本地控制器缓存把测试走成本地激活。
		TGuardValue<FString> ListenMode(World->NextURL, TEXT("?listen"));
		APlayerController* Remote = World->SpawnActor<APlayerController>();
		if (!TestNotNull(TEXT("Server-side remote controller"), Remote)) return false;
		Remote->Possess(Holder);
		if (!TestFalse(TEXT("Authority automatic reload fixture is genuinely not locally controlled"), Holder->IsLocallyControlled())) return false;
		if (!TestTrue(TEXT("Authority empty-magazine request starts immediately without owner round trip"), Weapon->RequestReload())) return false;
		FGameplayAbilitySpec* ReloadSpec = ASC->FindAbilitySpecFromClass(UfpstrueReloadAbility::StaticClass());
		TestTrue(TEXT("Automatic reload holds the same real GAS ability and positive authority transaction"), ReloadSpec != nullptr && ReloadSpec->IsActive() && Weapon->ActiveReloadId > 0);
		TestTrue(TEXT("Automatic reload uses a server-origin GAS key"), Weapon->NetworkState.ReloadPredictionKey < 0);
		const int32 AuthorityId = Weapon->ActiveReloadId;
		const float AuthorityDuration = Weapon->GetReloadDuration();
		for (int32 Tick = 0; Tick < FMath::CeilToInt(AuthorityDuration * 60.0f) + 3; ++Tick)
			if (!Fixture.TickTestWorld(1.0f / 60.0f)) { Fixture.ForwardErrorMessages(this); return false; }
		TestEqual(TEXT("Authority clock transfers available reserve exactly once"), Weapon->GetCurrentAmmo(), 10);
		TestEqual(TEXT("Authority reload conserves total ammunition"), Weapon->GetReserveAmmo(), 0);
		Weapon->FinishReloadForTransaction(AuthorityId);
		TestEqual(TEXT("Old completion cannot transfer ammunition twice"), Weapon->GetCurrentAmmo(), 10);
		TestFalse(TEXT("Authority completion ends the actual GAS ability"), ReloadSpec->IsActive());
		TestFalse(TEXT("Authority completion clears its only completion timer"), World->GetTimerManager().TimerExists(Weapon->ReloadTimerHandle));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponShotTransportTest,
	"fpstrue.Multiplayer.Prediction.BoundedShotTransportReorderAndTail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 固定到达顺序检查真实入口与计时；网络丢包概率和吞吐量由独立进程的正常/弱网采集验证。
bool FFpstrueWeaponShotTransportTest::RunTest(const FString&)
{
	const auto StreamFor = [](const FFPShotRequest& Request, uint32 FirstShotId)
	{
		FFPShotStreamIdentity Stream;
		Stream.Weapon = Request.Weapon;
		Stream.EquipmentGeneration = Request.EquipmentGeneration;
		Stream.FireSession = Request.FireSession;
		Stream.FirstShotId = FirstShotId;
		return Stream;
	};
	// 查询对象的显示年龄包含下行估计；该纯时间策略不改射手origin和开火输入的硬截止窗。
	const double HistoryFloor = 10.0 - static_cast<double>(AfpstrueCharacter::MaxHitHistorySeconds);
	TestTrue(TEXT("Symmetric one-way estimate and configured interpolation both contribute to target time"),
		FMath::IsNearlyEqual(UfpstrueWeaponComponent::ComputeTargetTraceTime(9.9, 10.0, 200.0, .05, true), 9.75, 1.e-6));
	TestTrue(TEXT("Disabling latency compensation preserves the same interpolation policy"),
		FMath::IsNearlyEqual(UfpstrueWeaponComponent::ComputeTargetTraceTime(9.9, 10.0, 200.0, .05, false), 9.85, 1.e-6));
	TestTrue(TEXT("Execution queue age reduces remaining history instead of extending the history window"),
		FMath::IsNearlyEqual(UfpstrueWeaponComponent::ComputeTargetTraceTime(9.71, 10.0, 200.0, .05, true), HistoryFloor, 1.e-6));
	TestTrue(TEXT("Very high ping is bounded by the existing stored history"),
		FMath::IsNearlyEqual(UfpstrueWeaponComponent::ComputeTargetTraceTime(9.9, 10.0, 100000.0, .05, true), HistoryFloor, 1.e-6));
	TestTrue(TEXT("Negative ping cannot shift the target into a later pose"),
		FMath::IsNearlyEqual(UfpstrueWeaponComponent::ComputeTargetTraceTime(9.9, 10.0, -200.0, .05, true), 9.85, 1.e-6));
	TestTrue(TEXT("Nonfinite ping never poisons the target query timestamp"),
		FMath::IsNearlyEqual(UfpstrueWeaponComponent::ComputeTargetTraceTime(9.9, 10.0, std::numeric_limits<double>::quiet_NaN(), .05, true), 9.85, 1.e-6));
	const double NoBudgetTime = UfpstrueWeaponComponent::ComputeTargetTraceTime(9.9, 10.0, 0.0, 0.0, true);
	TestTrue(TEXT("Target query is never later than the shot's original fire time"), NoBudgetTime <= 9.9 && NoBudgetTime >= HistoryFloor);
	TestTrue(TEXT("A permitted small future input never asks for an unrecorded future target pose"),
		UfpstrueWeaponComponent::ComputeTargetTraceTime(10.02, 10.0, 0.0, 0.0, true) <= 10.0);
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	World->CreateAISystem();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AfpstrueCharacter* Holder = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, Spawn);
	AAIController* Controller = World->SpawnActor<AAIController>();
	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Transport holder"), Holder) || !TestNotNull(TEXT("Transport controller"), Controller) || !TestNotNull(TEXT("Transport weapon actor"), Actor)) return false;
	Holder->SetActorEnableCollision(false);
	Holder->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->SetActorTickEnabled(false);
	Controller->Possess(Holder);
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Holder);
	{
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("GripPoint")), TEXT("GripPoint"), INDEX_NONE), FTransform::Identity);
	}
	Mesh->CalculateInvRefMatrices();
	Holder->GetMesh1P()->bEnableAnimation = false;
	Holder->GetMesh1P()->SetComponentTickEnabled(false);
	Holder->GetMesh1P()->SetSkeletalMesh(Mesh);
	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(Actor);
	Actor->AddInstanceComponent(Weapon);
	Actor->SetRootComponent(Weapon);
	Weapon->RegisterComponent();
	if (!TestTrue(TEXT("Transport uses the production equipment transaction"), Weapon->AttachWeapon(Holder))) return false;
	Actor->SetOwner(Holder); // Standalone 装备不需要网络 Owner；本专项显式建立真实控制回执的拥有关系。
	Weapon->Settings.RoundsPerMinute = 1200.0f;
	UfpstrueHealthComponent* ASC = Holder->GetHealthComponent();
	ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	const int32 InitialAmmo = Weapon->GetCurrentAmmo();
	const auto TickQueuedShot = [&](uint32 ExpectedShotId)
	{
		// 调度只检查原绝对截止点；第一次到期的Timer阶段应执行，不能再额外等一次Pending激活。
		constexpr float Step = .01f;
		const float Remaining = World->GetTimerManager().GetTimerRemaining(Weapon->NetworkShotTimerHandle);
		const double StartedAt = World->GetTimeSeconds();
		const double PlannedDeadline = Weapon->NextShotTimeSeconds;
		const double PlannedDelay = FMath::Max(0.0, PlannedDeadline - StartedAt);
		if (!TestTrue(TEXT("A real queued input owns the original execution timer"), Remaining >= 0.0f && !Weapon->PendingNetworkShots.IsEmpty())) return false;
		const int32 MaxSteps = FMath::CeilToInt(PlannedDelay / Step) + 1;
		for (int32 Tick = 0; Tick < MaxSteps && Weapon->LastExecutedShotId != ExpectedShotId; ++Tick)
		{
			if (!Fixture.TickTestWorld(Step)) { Fixture.ForwardErrorMessages(this); return false; }
			if (World->GetTimeSeconds() + UE_DOUBLE_SMALL_NUMBER < PlannedDeadline)
				TestNotEqual(TEXT("Next-timer-stage checks never fire ahead of the original authority deadline"), Weapon->LastExecutedShotId, ExpectedShotId);
		}
		const double Elapsed = World->GetTimeSeconds() - StartedAt;
		AddInfo(FString::Printf(TEXT("Queued shot %u: remaining %.6f s, fixture elapsed %.6f s, planned authority deadline %.6f, actual world %.6f, executed %u"),
			ExpectedShotId, Remaining, Elapsed, PlannedDeadline, World->GetTimeSeconds(), Weapon->LastExecutedShotId));
		TestTrue(TEXT("Execution occurs within one frame of the original deadline without pending-timer extra delay"), Elapsed <= PlannedDelay + Step + .000001);
		return TestEqual(TEXT("The original timer actually executes the expected queued identity"), Weapon->LastExecutedShotId, ExpectedShotId);
	};
	const auto MakeRequest = [&](uint32 Id, double Time)
	{
		FFPShotRequest Request;
		Request.Avatar = Holder; Request.Weapon = Actor;
		Request.EquipmentGeneration = Weapon->EquipmentRevision;
		Request.FireSession = 1; Request.ShotId = Id;
		Request.ServerFireTime = Time; Request.AimDirection = FVector::ForwardVector;
		return Request;
	};
	if (!Fixture.TickTestWorld(.01f)) { Fixture.ForwardErrorMessages(this); return false; }
	const FFPShotRequest First = MakeRequest(1, World->GetTimeSeconds());
	if (!Fixture.TickTestWorld(.05f)) { Fixture.ForwardErrorMessages(this); return false; }
	const FFPShotRequest Second = MakeRequest(2, World->GetTimeSeconds());
	Weapon->ReceiveShotInputBatch(1, {Second});
	TestEqual(TEXT("Later input waits for its missing predecessor"), Weapon->GetCurrentAmmo(), InitialAmmo);
	TestEqual(TEXT("Out-of-order input does not advance the committed ammo watermark"), Weapon->LastExecutedShotId, uint32(0));
	Weapon->ReceiveShotInputBatch(1, {First, Second});
	TestEqual(TEXT("Recovered predecessor executes first"), Weapon->GetCurrentAmmo(), InitialAmmo - 1);
	TestEqual(TEXT("Recovered later input enters the original rate-limited execution queue"), Weapon->PendingNetworkShots.Num(), 1);
	if (!TickQueuedShot(2)) return false;
	TestEqual(TEXT("The existing execution clock consumes the later input once"), Weapon->GetCurrentAmmo(), InitialAmmo - 2);
	const int32 FinalizedBeforeDuplicate = Weapon->FinalizedShotInputs.Num();
	const int32 RecentBeforeDuplicate = Weapon->RecentShotReceipts.Num();
	Weapon->ReceiveShotInputBatch(1, {First, Second});
	TestEqual(TEXT("Redundant batch cannot deduct ammunition twice"), Weapon->GetCurrentAmmo(), InitialAmmo - 2);
	TestEqual(TEXT("Redundant batch cannot create another reliable verdict"), Weapon->FinalizedShotInputs.Num(), FinalizedBeforeDuplicate);
	TestEqual(TEXT("Redundant input cannot append another recent fast-window result"), Weapon->RecentShotReceipts.Num(), RecentBeforeDuplicate);
	{
		TGuardValue<FString> Listen(World->NextURL, TEXT("?listen"));
		const uint32 Revision = Weapon->NetworkState.Revision;
		Weapon->AcknowledgeShot(Second, EFPShotResult::Accepted);
		TestEqual(TEXT("Already reliable verdict is not republished for duplicate input"), Weapon->NetworkState.Revision, Revision);
	}
	if (!Fixture.TickTestWorld(.05f)) { Fixture.ForwardErrorMessages(this); return false; }
	const FFPShotRequest Fourth = MakeRequest(4, World->GetTimeSeconds());
	Weapon->ReceiveShotInputBatch(1, {Fourth});
	for (int32 Tick = 0; Tick < 9; ++Tick)
		if (!Fixture.TickTestWorld(.01f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestTrue(TEXT("A hole gets one explicit terminal cancellation after the bounded reorder wait"), Weapon->FinalizedShotInputs.Contains(3));
	TestEqual(TEXT("Later input executes only after the hole is explicitly terminated"), Weapon->GetCurrentAmmo(), InitialAmmo - 3);
	TestEqual(TEXT("Committed snapshot watermark follows ordered execution"), Weapon->LastExecutedShotId, uint32(4));
	Weapon->ReceiveShotInputBatch(1, {MakeRequest(3, Fourth.ServerFireTime - .05)});
	TestEqual(TEXT("A recovered packet after terminal hole cancellation cannot execute"), Weapon->GetCurrentAmmo(), InitialAmmo - 3);
	Weapon->ReceiveShotInputBatch(1, {MakeRequest(5, World->GetTimeSeconds())}, false, true);
	Weapon->ReceiveShotInputBatch(1, {MakeRequest(6, World->GetTimeSeconds())}, false, false, true);
	TestEqual(TEXT("Explicit cancel and timeout controls cannot cause damage or consume rounds"), Weapon->GetCurrentAmmo(), InitialAmmo - 3);
	TestTrue(TEXT("Both controls finish their exact pending identities"), Weapon->FinalizedShotInputs.Contains(5) && Weapon->FinalizedShotInputs.Contains(6));
	TestEqual(TEXT("Authority recent-terminal buffer retains exactly four immutable results"), Weapon->RecentShotReceipts.Num(), 4);
	TestTrue(TEXT("Authority recent window evicts oldest verdicts without changing later results"),
		Weapon->RecentShotReceipts[0].ShotId == 3 && Weapon->RecentShotReceipts.Last().ShotId == 6);
	const FFPShotReceipt* FourthVerdict = Weapon->RecentShotReceipts.FindByPredicate([](const FFPShotReceipt& Receipt) { return Receipt.ShotId == 4; });
	TestTrue(TEXT("Recent accepted result retains its own original ammo baseline and terminal verdict"),
		FourthVerdict != nullptr && FourthVerdict->Result == EFPShotResult::Accepted && FourthVerdict->Ammo == InitialAmmo - 3);
	TArray<FFPShotRequest> Oversized;
	for (uint32 Id = 7; Id < 16; ++Id) Oversized.Add(MakeRequest(Id, World->GetTimeSeconds()));
	TestFalse(TEXT("Oversized input batches cannot enter the bounded window"), Weapon->ReceiveShotInputBatch(1, Oversized));
	TestTrue(TEXT("Rejected oversized batch leaves no buffered work"), Weapon->OrderedShotInputs.IsEmpty());
	if (!Fixture.TickTestWorld(.06f)) { Fixture.ForwardErrorMessages(this); return false; }
	ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
	if (!TestNotNull(TEXT("Lifecycle tail binds the actual granted fire spec"), Spec)) return false;
	// 固定客户端上下文收据；不在 authority 上调用会刻意返回空键的客户端发号器。
	FPredictionKey Key;
	Key.Current = 100;
	Weapon->FirePresentationKey = Key.Current;
	Weapon->InputClosedServerSeconds = World->GetTimeSeconds();
	ASC->RememberClosedShotStream(Spec->Handle, Key, Weapon, false);
	const FFPShotRequest Tail = MakeRequest(7, World->GetTimeSeconds());
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, Key, StreamFor(Tail, 1), {Tail}, false, false);
	TestEqual(TEXT("Reliable lifecycle tail recovers the final dropped input after normal release"), Weapon->GetCurrentAmmo(), InitialAmmo - 4);
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, Key, StreamFor(Tail, 1), {Tail}, false, false);
	TestEqual(TEXT("Repeated tail closure cannot replay the final shot"), Weapon->GetCurrentAmmo(), InitialAmmo - 4);
	ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
	ASC->ServerSubmitShotBatch_Implementation(Spec->Handle, Key, 1, {MakeRequest(8, World->GetTimeSeconds())});
	TestEqual(TEXT("Closed authorization never bypasses a newly started reload"), Weapon->GetCurrentAmmo(), InitialAmmo - 4);
	TestTrue(TEXT("Late input during replacement receives a cancellation instead of execution"), Weapon->FinalizedShotInputs.Contains(8));
	ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
	// 快速松开重按：已准入旧会话仍由同一个执行队列履约，不能被新会话的身份清空。
	ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	// 前面的迟到输入会保留理想节拍；先明确跨过锚点重建阈值，才能要求后续10号确实排队。
	const double Interval = 60.0 / Weapon->Settings.RoundsPerMinute;
	const double PreviousDeadline = Weapon->NextShotTimeSeconds;
	const float IdleUntilReset = static_cast<float>(FMath::Max(.01, PreviousDeadline + Interval + .01 - World->GetTimeSeconds()));
	if (!Fixture.TickTestWorld(IdleUntilReset)) { Fixture.ForwardErrorMessages(this); return false; }
	if (!TestTrue(TEXT("Old cadence anchor has genuinely expired before establishing the queued-shot fixture"),
		World->GetTimeSeconds() > PreviousDeadline + Interval)) return false;
	Weapon->BeginAbilityFire(101);
	FFPShotRequest Ninth = MakeRequest(9, World->GetTimeSeconds());
	Ninth.FireSession = 2;
	Weapon->ReceiveShotInputBatch(9, {Ninth});
	if (!Fixture.TickTestWorld(.03f)) { Fixture.ForwardErrorMessages(this); return false; }
	FFPShotRequest Tenth = MakeRequest(10, Ninth.ServerFireTime + .05);
	Tenth.FireSession = 2;
	if (!TestTrue(TEXT("The second input arrives while the real authority clock still has cooldown"), Weapon->GetNextShotDelay(true) > 0.0f)) return false;
	Weapon->ReceiveShotInputBatch(9, {Tenth});
	if (!TestEqual(TEXT("Old session has one already admitted shot waiting for the original execution clock"), Weapon->PendingNetworkShots.Num(), 1)) return false;
	FPredictionKey OldKey;
	OldKey.Current = 101;
	ASC->RememberClosedShotStream(Spec->Handle, OldKey, Weapon, false);
	Weapon->EndAbilityFire(false);
	Weapon->BeginAbilityFire(102);
	TestEqual(TEXT("New fire activation preserves the old session's admitted queue"), Weapon->PendingNetworkShots.Num(), 1);
	ASC->ServerSubmitShotBatch_Implementation(Spec->Handle, OldKey, 9, {Tenth});
	TestFalse(TEXT("Late duplicate from the old session cannot prematurely cancel its admitted shot"), Weapon->FinalizedShotInputs.Contains(10));
	if (!TickQueuedShot(10)) return false;
	TestEqual(TEXT("Already admitted old shot completes after the new activation"), Weapon->LastExecutedShotId, uint32(10));
	if (!Fixture.TickTestWorld(.05f)) { Fixture.ForwardErrorMessages(this); return false; }
	FFPShotRequest Eleventh = MakeRequest(11, World->GetTimeSeconds());
	Eleventh.FireSession = 3;
	Weapon->ReceiveShotInputBatch(11, {Eleventh});
	TestEqual(TEXT("The new session then advances the same global ammo watermark"), Weapon->LastExecutedShotId, uint32(11));
	TestEqual(TEXT("Both sessions consume exactly their admitted rounds"), Weapon->GetCurrentAmmo(), InitialAmmo - 7);
	ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	for (int32 Count = 0; Count < 12; ++Count)
	{
		FPredictionKey ReceiptKey;
		ReceiptKey.Current = static_cast<FPredictionKey::KeyType>(1000 + Count);
		ASC->RememberClosedShotStream(Spec->Handle, ReceiptKey, Weapon, true);
	}
	TestEqual(TEXT("Lifecycle authorizations stay strictly bounded"), ASC->ClosedShotStreams.Num(), 8);
	// 超过8条会淘汰旧回执；淘汰只撤销执行权，过期的同拥有者输入仍须得到终结结果。
	Actor->SetOwner(Holder);
	FFPShotRequest EvictedInput = MakeRequest(12, World->GetTimeSeconds());
	EvictedInput.FireSession = 3;
	FPredictionKey EvictedKey;
	EvictedKey.Current = 1000;
	if (!Fixture.TickTestWorld(.31f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestFalse(TEXT("The oldest receipt was really evicted by the eight-entry bound"), ASC->ClosedShotStreams.ContainsByPredicate(
		[EvictedKey](const UfpstrueHealthComponent::FClosedShotStream& Stream) { return Stream.Key == EvictedKey; }));
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, EvictedKey, StreamFor(EvictedInput, 12), {EvictedInput}, false, true);
	TestTrue(TEXT("Evicted authorization still emits one terminal result for its expired owned input"), Weapon->FinalizedShotInputs.Contains(12));
	TestEqual(TEXT("Finalizing an evicted input cannot consume ammunition"), Weapon->GetCurrentAmmo(), InitialAmmo - 7);
	Weapon->OutstandingShots = {12};
	Weapon->ClientShotInputs.Add({EvictedInput, Spec->Handle, EvictedKey, 12, 6, FPlatformTime::Seconds() - .4, true});
	Actor->SetRole(ROLE_SimulatedProxy);
	Weapon->ClientShotResult_Implementation(Weapon->EquipmentRevision, 3, 12, EFPShotResult::InvalidTime,
		Weapon->CurrentAmmo, Weapon->ReserveAmmo, Weapon->NetworkState.Revision, Weapon->LastExecutedShotId);
	TestTrue(TEXT("That terminal result releases both the client's prediction ledger and bounded input slot"),
		Weapon->OutstandingShots.IsEmpty() && Weapon->ClientShotInputs.IsEmpty());
	Actor->SetRole(ROLE_Authority);
	FPredictionKey ExpiredKey;
	ExpiredKey.Current = 1600;
	ASC->RememberClosedShotStream(Spec->Handle, ExpiredKey, Weapon, false);
	FFPShotRequest ExpiredInput = MakeRequest(13, World->GetTimeSeconds());
	ExpiredInput.FireSession = 3;
	for (int32 Tick = 0; Tick < 21; ++Tick)
		if (!Fixture.TickTestWorld(.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, ExpiredKey, StreamFor(ExpiredInput, 13), {ExpiredInput}, false, true);
	TestFalse(TEXT("Two-second authorization really expires instead of being extended for cleanup"), ASC->ClosedShotStreams.ContainsByPredicate(
		[ExpiredKey](const UfpstrueHealthComponent::FClosedShotStream& Stream) { return Stream.Key == ExpiredKey; }));
	TestTrue(TEXT("Expired authorization can finish its input without permitting execution"), Weapon->FinalizedShotInputs.Contains(13));
	TestEqual(TEXT("Expiration-only cleanup preserves authoritative ammunition"), Weapon->GetCurrentAmmo(), InitialAmmo - 7);
	{
		TGuardValue<FString> Listen(World->NextURL, TEXT("?listen"));
		const uint32 Revision = Weapon->NetworkState.Revision;
		ASC->ServerFinishShotInputs_Implementation(Spec->Handle, ExpiredKey, StreamFor(ExpiredInput, 13), {ExpiredInput}, false, true);
		ASC->ServerFinishShotInputs_Implementation(Spec->Handle, Key, StreamFor(Tail, 1), {Tail}, false, true);
		TestEqual(TEXT("Repeated expiration cannot republish rejection or change a previously accepted result"), Weapon->NetworkState.Revision, Revision);
	}
	FPredictionKey OwnershipKey;
	OwnershipKey.Current = 1800;
	ASC->RememberClosedShotStream(Spec->Handle, OwnershipKey, Weapon, true);
	FFPShotRequest WrongOwnerInput = MakeRequest(15, World->GetTimeSeconds() - .31);
	WrongOwnerInput.FireSession = 3;
	AActor* OtherOwner = World->SpawnActor<AActor>();
	Actor->SetOwner(OtherOwner);
	const int32 FinalizedBeforeOwnerChange = Weapon->FinalizedShotInputs.Num();
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, OwnershipKey, StreamFor(WrongOwnerInput, 15), {WrongOwnerInput}, false, true);
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, ExpiredKey, StreamFor(WrongOwnerInput, 15), {WrongOwnerInput}, false, true);
	TestEqual(TEXT("Neither live nor expired authorization sends old-owner ACKs after actual Actor ownership changes"),
		Weapon->FinalizedShotInputs.Num(), FinalizedBeforeOwnerChange);
	Actor->SetOwner(Holder);
	// 用真实出手委托重启同一个 InstancedPerActor GA，旧 Finish 调用栈不能结束新激活。
	if (!Fixture.TickTestWorld(.06f)) { Fixture.ForwardErrorMessages(this); return false; }
	FPredictionKey SourceKey, ReplacementKey;
	SourceKey.Current = 2000;
	ReplacementKey.Current = 2001;
	{
		TGuardValue<FString> Listen(World->NextURL, TEXT("?listen"));
		APlayerController* Remote = World->SpawnActor<APlayerController>();
		if (!TestNotNull(TEXT("Reentry uses a server-side remote controller"), Remote)) return false;
		Remote->Possess(Holder);
		if (!TestFalse(TEXT("Remote activation does not independently pulse an authority-local shot"), Holder->IsLocallyControlled())) return false;
		ASC->AbilitySpecInputPressed(*Spec);
		if (!TestTrue(TEXT("The real original GAS activation starts"), ASC->InternalTryActivateAbility(Spec->Handle, SourceKey))) return false;
	}
	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	bool bReentered = false;
	Observer->OnTraceReceived = [&]
	{
		if (bReentered) return;
		bReentered = true;
		ASC->CancelAbilityHandle(Spec->Handle);
		ASC->AbilitySpecInputPressed(*Spec);
		TestTrue(TEXT("Synchronous trace callback starts the replacement on the same granted ability"),
			ASC->InternalTryActivateAbility(Spec->Handle, ReplacementKey));
	};
	Weapon->OnWeaponTraceFinished.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleTraceFinished);
	FFPShotRequest Fourteenth = MakeRequest(14, World->GetTimeSeconds());
	Fourteenth.FireSession = 4;
	ASC->ServerFinishShotInputs_Implementation(Spec->Handle, SourceKey, StreamFor(Fourteenth, 14), {Fourteenth}, false, false);
	Weapon->OnWeaponTraceFinished.RemoveDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleTraceFinished);
	Observer->OnTraceReceived = nullptr;
	TestTrue(TEXT("The original shot reached its real synchronous callback"), bReentered);
	TestTrue(TEXT("Old finish control preserves the replacement ability instance"), Spec->IsActive());
	TestTrue(TEXT("Old finish control preserves the replacement activation key"),
		Spec->GetPrimaryInstance()->GetCurrentActivationInfo().GetActivationPredictionKey() == ReplacementKey);
	TestEqual(TEXT("Synchronous replacement does not execute the original shot twice"), Weapon->GetCurrentAmmo(), InitialAmmo - 8);
	ASC->CancelAbilityHandle(Spec->Handle);
	// 经真实准入入口拒绝显示时间元数据，而非只测试helper返回值；拒绝不能结算子弹或伤害。
	ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	Weapon->BeginAbilityFire(3000);
	const int32 AmmoBeforeViewPolicy = Weapon->GetCurrentAmmo();
	const uint32 ExecutedBeforeViewPolicy = Weapon->LastExecutedShotId;
	FFPShotRequest InvalidViewRequest = MakeRequest(16, World->GetTimeSeconds());
	InvalidViewRequest.FireSession = Weapon->LastRemoteFireSession + 1;
	AfpstrueCharacter* PolicyTarget = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform(FVector(2000, 0, 0)), Spawn);
	if (!TestNotNull(TEXT("Real metadata policy target"), PolicyTarget)) return false;
	InvalidViewRequest.RenderedViews.Add({PolicyTarget, 351});
	TestTrue(TEXT("Illegal rendered age is rejected by the actual authority admission path"), Weapon->AcceptShotRequest(InvalidViewRequest) == EFPShotResult::InvalidTime);
	InvalidViewRequest.ShotId = 17;
	InvalidViewRequest.RenderedViews[0].AgeMilliseconds = 0;
	InvalidViewRequest.RenderedViews.Add({PolicyTarget, 0});
	TestTrue(TEXT("Duplicate target metadata is rejected by the actual authority admission path"), Weapon->AcceptShotRequest(InvalidViewRequest) == EFPShotResult::InvalidIdentity);
	TestEqual(TEXT("Rejected rendered-view metadata never spends authoritative ammo"), Weapon->GetCurrentAmmo(), AmmoBeforeViewPolicy);
	TestEqual(TEXT("Rejected rendered-view metadata never advances execution identity"), Weapon->LastExecutedShotId, ExecutedBeforeViewPolicy);
	Weapon->EndAbilityFire(true);
	ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Firing);
	const int32 AmmoBeforeGenerationReset = Weapon->GetCurrentAmmo();
	const uint32 PreviousClientGeneration = Weapon->EquipmentRevision;
	{
		// 同一夹具稍后切换接收端；由真实发布入口生成这次新代际的公共/私有快照。
		TGuardValue<FString> Listen(World->NextURL, TEXT("?listen"));
		Weapon->ResetNetworkInputGeneration();
	}
	TestTrue(TEXT("Input generation reset cannot resend another owner's recent terminal window"), Weapon->RecentShotReceipts.IsEmpty());
	TestEqual(TEXT("Clearing recent receipts never changes authoritative ammunition"), Weapon->GetCurrentAmmo(), AmmoBeforeGenerationReset);
	// 复用真实已授予的Fire实例：世界时间跳过多个节拍，实际100ms未到仍不能生成第二个输入。
	AfpstruePvpPlayerController* CadenceController = World->SpawnActor<AfpstruePvpPlayerController>();
	if (!TestNotNull(TEXT("Cadence fixture uses the real clock-owning controller"), CadenceController)) return false;
	// 本地占有会执行正式增强输入校验；使用项目真实输入资源，不能以Expected Error掩盖缺配置。
	const TPair<const TCHAR*, const TCHAR*> InputAssets[] = {
		{TEXT("DefaultMappingContext"), TEXT("/Game/FirstPerson/Input/IMC_Default.IMC_Default")},
		{TEXT("MoveAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_Move.IA_Move")},
		{TEXT("LookAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_Look.IA_Look")},
		{TEXT("JumpAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_Jump.IA_Jump")},
		{TEXT("FireAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_Shoot.IA_Shoot")},
		{TEXT("AimAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_Aim.IA_Aim")},
		{TEXT("SprintAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_Run.IA_Run")},
		{TEXT("ReloadAction"), TEXT("/Game/FirstPerson/Input/Actions/IA_reload.IA_reload")}
	};
	for (const auto& Input : InputAssets)
	{
		FObjectProperty* Property = FindFProperty<FObjectProperty>(Holder->GetClass(), Input.Key);
		UObject* Asset = StaticLoadObject(FCString::Strcmp(Input.Key, TEXT("DefaultMappingContext")) == 0
			? UInputMappingContext::StaticClass() : UInputAction::StaticClass(), nullptr, Input.Value);
		if (!TestNotNull(TEXT("Authored input property"), Property) || !TestNotNull(TEXT("Authored input asset"), Asset)) return false;
		Property->SetObjectPropertyValue_InContainer(Holder, Asset);
	}
	CadenceController->Possess(Holder);
	ASC->InitAbilityActorInfo(Holder, Holder);
	Weapon->Settings.RoundsPerMinute = 600.0f; // 本段明确验证正式100ms输入节拍，不改变上面的1200RPM队列夹具。
	Weapon->LastShotTimeSeconds = World->GetTimeSeconds(); // 原生权威激活只安排等待，不能在切端前先射一发。
	FPredictionKey CadenceKey; CadenceKey.Current = 3100;
	ASC->AbilitySpecInputPressed(*Spec);
	if (!TestTrue(TEXT("Real granted Fire ability activates for cadence regression"), ASC->InternalTryActivateAbility(Spec->Handle, CadenceKey))) return false;
	auto* CadenceFire = Cast<UfpstrueFireAbility>(Spec->GetPrimaryInstance());
	if (!TestNotNull(TEXT("Cadence checks execute the real Fire Pulse"), CadenceFire)) return false;
	const double AuthorityNow = World->GetTimeSeconds();
	Weapon->NextShotTimeSeconds = AuthorityNow + .05;
	Weapon->LastPredictedShotTime = FPlatformTime::Seconds() + 100.0;
	Weapon->LastSubmittedFireTime = AuthorityNow + 100.0;
	TestTrue(TEXT("Authority cadence ignores the client-only monotonic and reported-time endpoints"),
		FMath::IsNearlyEqual(Weapon->GetNextShotDelay(true), .05f, 1.e-5f));
	Actor->SetRole(ROLE_SimulatedProxy); Holder->SetRole(ROLE_AutonomousProxy);
	CadenceController->SetRole(ROLE_AutonomousProxy);
	ASC->InitAbilityActorInfo(Holder, Holder);
	// 前半段接收过含11号出手的回执；接收端仍看到旧代际，必须走正式RepNotify完成换代，不能直接清账。
	Weapon->EquipmentRevision = PreviousClientGeneration;
	Weapon->OnRep_NetworkState();
	TestEqual(TEXT("The real generation notification resets the previous endpoint's ammo watermark"), Weapon->LastAmmoIncludedShotId, uint32(0));
	TestEqual(TEXT("The cadence endpoint adopts the actually published equipment generation"), Weapon->EquipmentRevision, Weapon->NetworkState.EquipmentGeneration);
	CadenceFire->GetCurrentActivationInfoRef().ActivationMode = EGameplayAbilityActivationMode::Predicting;
	CadenceController->LastClockSampleLocal = FPlatformTime::Seconds();
	CadenceController->ServerClockOffset = World->GetTimeSeconds() - FPlatformTime::Seconds();
	CadenceController->ClockUncertainty = .001;
	Weapon->BeginAbilityFire(CadenceKey.Current);
	FFPShotRequest FirstCadenceInput;
	if (!TestTrue(TEXT("The real first cadence input builds through production"), Weapon->BuildShotRequest(FirstCadenceInput))) return false;
	const double BeforePresentationMono = FPlatformTime::Seconds();
	if (!TestTrue(TEXT("The real first cadence input presents through production"), Weapon->PresentPredictedShot(FirstCadenceInput))) return false;
	const double AfterPresentationMono = FPlatformTime::Seconds();
	TestTrue(TEXT("Actual presentation records its real monotonic endpoint instead of WorldTime"),
		Weapon->LastPredictedShotTime >= BeforePresentationMono && Weapon->LastPredictedShotTime <= AfterPresentationMono);
	const double BeforeInputCreated = FPlatformTime::Seconds();
	Weapon->SubmitShotInput(FirstCadenceInput, Spec->Handle, CadenceKey);
	if (!TestEqual(TEXT("The real submitted input owns one bounded client transport slot"), Weapon->ClientShotInputs.Num(), 1)) return false;
	TestTrue(TEXT("Transport creation and first send share the actual monotonic domain"),
		Weapon->ClientShotInputs[0].CreatedSeconds >= BeforeInputCreated && Weapon->ClientShotInputs[0].LastSentSeconds >= BeforeInputCreated
		&& Weapon->ClientShotInputs[0].LastSentSeconds <= FPlatformTime::Seconds());
	const uint32 SerialBeforeEarlyPulse = Weapon->LocalShotSerial;
	const int32 OutstandingBeforeEarlyPulse = Weapon->OutstandingShots.Num();
	const int32 DisplayBeforeEarlyPulse = Weapon->GetCurrentAmmo();
	// 固定一秒尚未到的单调截止点，排除压测机器抢占造成fixture本身偶然跨过100ms。
	const double OriginalPresentationTime = Weapon->LastPredictedShotTime;
	Weapon->LastPredictedShotTime += 1.0;
	if (!Fixture.TickTestWorld(.5f)) { Fixture.ForwardErrorMessages(this); return false; }
	CadenceFire->Pulse();
	TestTrue(TEXT("Advancing WorldTime cannot consume a future monotonic shot budget"), Weapon->GetNextShotDelay() > .10f);
	TestEqual(TEXT("An early actual Pulse cannot generate another ShotId"), Weapon->LocalShotSerial, SerialBeforeEarlyPulse);
	TestEqual(TEXT("An early actual Pulse cannot add predicted input debt"), Weapon->OutstandingShots.Num(), OutstandingBeforeEarlyPulse);
	TestEqual(TEXT("An early actual Pulse cannot deduct displayed ammo"), Weapon->GetCurrentAmmo(), DisplayBeforeEarlyPulse);
	// 恢复真实100ms门禁并立即唤醒；不靠世界Timer是否恰好在这一帧分派来间接推断。
	Weapon->LastPredictedShotTime = FPlatformTime::Seconds();
	CadenceFire->Pulse();
	TestTrue(TEXT("A fresh presentation keeps a positive real one-hundred-millisecond budget"), Weapon->GetNextShotDelay() > .05f);
	TestEqual(TEXT("WorldTime already ahead still cannot bypass the fresh real cadence"), Weapon->LocalShotSerial, SerialBeforeEarlyPulse);
	Weapon->LastPredictedShotTime = OriginalPresentationTime - 1.0;
	const double ReportedBeforeClockBack = CadenceController->GetEstimatedServerTime();
	Weapon->LastSubmittedFireTime = ReportedBeforeClockBack;
	CadenceController->ServerClockOffset -= .25;
	const double ActualClockBackTime = CadenceController->GetEstimatedServerTime();
	CadenceFire->Pulse();
	TestTrue(TEXT("A backwards server estimate delays the next real Pulse even when monotonic cadence is open"), Weapon->GetNextShotDelay() > .20f);
	TestEqual(TEXT("Backwards correction does not consume another ShotId"), Weapon->LocalShotSerial, SerialBeforeEarlyPulse);
	TestEqual(TEXT("Backwards correction cannot rewrite the last genuinely reported fire time"), Weapon->LastSubmittedFireTime, ReportedBeforeClockBack);
	// 直接Build仍可构造真实的过旧/回退边界输入；它不把timestamp夹到上发+interval来伪装合法。
	FFPShotRequest ClockBackRequest;
	if (!TestTrue(TEXT("Explicit boundary construction still uses the normal Build entry"), Weapon->BuildShotRequest(ClockBackRequest))) return false;
	TestTrue(TEXT("Build preserves the actually estimated older time instead of manufacturing a future one"),
		ClockBackRequest.ServerFireTime < ReportedBeforeClockBack && ClockBackRequest.ServerFireTime >= ActualClockBackTime);
	TestEqual(TEXT("Successful Build records its exact reported timestamp for the next gate"), Weapon->LastSubmittedFireTime, ClockBackRequest.ServerFireTime);
	// 复现实机982→280→268→284→292ms启动序列，走真实接收/调度/发送入口，不手工清零variation。
	CadenceController->ResetTimeSamples();
	uint32 StartupSequence = 1;
	for (const double Sample : {.982, .280, .268, .284, .292})
	{
		const double SampleNow = FPlatformTime::Seconds();
		CadenceController->PendingClockSamples.Add(StartupSequence, SampleNow - Sample);
		CadenceController->ClientReceiveClockSample_Implementation(StartupSequence++, SampleNow + 100.0 - Sample * .5);
	}
	const auto RecoveredStartupTiming = Weapon->GetShotRetryTiming();
	TestTrue(TEXT("The actual send policy consumes the improved startup estimator without a guessed one-way cutoff"),
		FMath::Abs(RecoveredStartupTiming.RoundTripSeconds - .2807890625) < .003
		&& FMath::Abs(RecoveredStartupTiming.VariationSeconds - .064984375) < .003
		&& FMath::IsNearlyEqual(RecoveredStartupTiming.IntervalSeconds, .06, 1.e-6));
	auto& StartupInput = Weapon->ClientShotInputs[0];
	StartupInput.CreatedSeconds = FPlatformTime::Seconds();
	StartupInput.LastSentSeconds = FPlatformTime::Seconds() - .065;
	const int32 BeforeStartupRetry = StartupInput.Attempts;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("The recovered estimate actually permits a bounded resend through production"), StartupInput.Attempts, BeforeStartupRetry + 1);
	TestFalse(TEXT("Startup recovery does not manufacture a terminal receipt"), StartupInput.bExpirationSent);
	// 同一真实发送入口消费PC的动态观测，证明它改变实际重发门禁和单次Timer，而不只改变日志。
	CadenceController->LastRoundTripSampleLocal = FPlatformTime::Seconds();
	CadenceController->SmoothedRoundTripTime = .04;
	CadenceController->RoundTripVariation = .005;
	const auto LowLatencyTiming = Weapon->GetShotRetryTiming();
	TestTrue(TEXT("Fresh low-latency observations wait one round trip plus twice its variation"),
		FMath::IsNearlyEqual(LowLatencyTiming.IntervalSeconds, .05, 1.e-6));
	CadenceController->SmoothedRoundTripTime = .2;
	CadenceController->RoundTripVariation = .02;
	const auto HighLatencyTiming = Weapon->GetShotRetryTiming();
	TestTrue(TEXT("A slower volatile route requests the bounded sixty-millisecond interval"),
		FMath::IsNearlyEqual(HighLatencyTiming.IntervalSeconds, .06, 1.e-6));
	TestTrue(TEXT("A slower route changes the client interval without changing the server age limit"),
		HighLatencyTiming.IntervalSeconds > LowLatencyTiming.IntervalSeconds
		&& UfpstrueWeaponComponent::MaxNetworkShotAgeSeconds == .30);
	const FString PreviousCommandLine(FCommandLine::Get());
	FCommandLine::Append(TEXT(" -PvpFixedShotRetry"));
	const auto FixedTiming = Weapon->GetShotRetryTiming();
	FCommandLine::Set(*PreviousCommandLine);
	TestTrue(TEXT("The reference changes only the requested interval while using the same observation and lifecycle expiry"),
		FixedTiming.bFixedReference && FMath::IsNearlyEqual(FixedTiming.IntervalSeconds, .04, 1.e-6)
		&& FixedTiming.RoundTripSeconds == HighLatencyTiming.RoundTripSeconds
		&& UfpstrueWeaponComponent::ShotInputExpireSeconds == .40f);
	auto& AdaptiveInput = Weapon->ClientShotInputs[0];
	AdaptiveInput.CreatedSeconds = FPlatformTime::Seconds();
	AdaptiveInput.LastSentSeconds = FPlatformTime::Seconds() - .045;
	const int32 BeforeAdaptiveWake = AdaptiveInput.Attempts;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("Forty-five milliseconds cannot bypass a freshly selected sixty-millisecond gate"), AdaptiveInput.Attempts, BeforeAdaptiveWake);
	TestTrue(TEXT("The actual retry timer targets the pending adaptive due time rather than a fixed polling interval"),
		World->GetTimerManager().TimerExists(Weapon->ShotRetryTimerHandle)
		&& World->GetTimerManager().GetTimerRemaining(Weapon->ShotRetryTimerHandle) > 0.0f
		&& World->GetTimerManager().GetTimerRate(Weapon->ShotRetryTimerHandle) < .04f);
	AdaptiveInput.LastSentSeconds = FPlatformTime::Seconds() - .065;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("The same real send entry retries when its adaptive interval is actually due"), AdaptiveInput.Attempts, BeforeAdaptiveWake + 1);
	CadenceController->SmoothedRoundTripTime = .04;
	CadenceController->RoundTripVariation = .005;
	AdaptiveInput.CreatedSeconds = FPlatformTime::Seconds();
	AdaptiveInput.LastSentSeconds = FPlatformTime::Seconds() - .055;
	const int32 BeforeRecoveredRoute = AdaptiveInput.Attempts;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("A recovered route really sends after its fifty-millisecond interval while the old sixty-millisecond gate would still wait"),
		AdaptiveInput.Attempts, BeforeRecoveredRoute + 1);
	CadenceController->SmoothedRoundTripTime = .2;
	CadenceController->RoundTripVariation = .02;
	AdaptiveInput.CreatedSeconds = FPlatformTime::Seconds();
	AdaptiveInput.LastSentSeconds = FPlatformTime::Seconds() - .03;
	const int32 BeforePiggyback = AdaptiveInput.Attempts;
	Weapon->SubmitShotInput(ClockBackRequest, Spec->Handle, CadenceKey);
	TestEqual(TEXT("A genuinely new input can carry an old unconfirmed input before its standalone due time"),
		Weapon->ClientShotInputs[0].Attempts, BeforePiggyback + 1);
	TestEqual(TEXT("Piggyback preserves one first send for the newly built input"), Weapon->ClientShotInputs[1].Attempts, 1);
	const int32 AfterPiggyback = Weapon->ClientShotInputs[0].Attempts;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("An immediate repeated wake cannot turn piggyback into unlimited resends"), Weapon->ClientShotInputs[0].Attempts, AfterPiggyback);
	// 客户端不以RTT/2猜测服务器准入期限；在原400ms账本寿命内仍可用剩余次数补发，由服务器裁决。
	auto& LateInput = Weapon->ClientShotInputs[1];
	LateInput.CreatedSeconds = FPlatformTime::Seconds() - .32;
	LateInput.LastSentSeconds = FPlatformTime::Seconds() - .065;
	const int32 BeforeLateRetry = LateInput.Attempts;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("A live input is not suppressed by a client-invented three-hundred-millisecond authority deadline"),
		LateInput.Attempts, BeforeLateRetry + 1);
	TestFalse(TEXT("A local retry does not manufacture a final verdict before the real lifecycle expiry"), LateInput.bExpirationSent);
	CadenceController->LastRoundTripSampleLocal = FPlatformTime::Seconds() - 6.0;
	const auto StaleTiming = Weapon->GetShotRetryTiming();
	TestTrue(TEXT("An expired RTT observation falls back to forty milliseconds"),
		StaleTiming.RoundTripSeconds < 0.0 && FMath::IsNearlyEqual(StaleTiming.IntervalSeconds, .04, 1.e-6));

	// 不改变真实WorldTime，通过已有时间端点构造早醒/重发/过期的确定边界。
	UfpstrueWeaponComponent::FClientShotInput& RetryInput = Weapon->ClientShotInputs[0];
	RetryInput.CreatedSeconds = FPlatformTime::Seconds();
	RetryInput.LastSentSeconds = FPlatformTime::Seconds() + .10;
	const int32 AttemptsBeforeEarlyRetry = RetryInput.Attempts;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("An early retry wake cannot accelerate the actual forty-millisecond resend budget"), RetryInput.Attempts, AttemptsBeforeEarlyRetry);
	RetryInput.LastSentSeconds = FPlatformTime::Seconds() - .05;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("Exactly one normal resend occurs after the same monotonic forty-millisecond boundary"), RetryInput.Attempts, AttemptsBeforeEarlyRetry + 1);
	// 固定已过重发间隔、尚未过期的端点，检查真实发送入口仍以六次为硬上限。
	for (int32 Retry = RetryInput.Attempts; Retry < 6; ++Retry)
	{
		RetryInput.CreatedSeconds = FPlatformTime::Seconds();
		RetryInput.LastSentSeconds = FPlatformTime::Seconds() - .05;
		Weapon->SendPendingShotInputs();
	}
	TestEqual(TEXT("The real transport stops after six total normal attempts"), RetryInput.Attempts, 6);
	RetryInput.CreatedSeconds = FPlatformTime::Seconds();
	RetryInput.LastSentSeconds = FPlatformTime::Seconds() - .05;
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("A further eligible retry wake cannot exceed six attempts"), RetryInput.Attempts, 6);
	TestFalse(TEXT("Attempt exhaustion alone cannot send an early expiry control"), RetryInput.bExpirationSent);
	RetryInput.CreatedSeconds = FPlatformTime::Seconds() - .41;
	const int32 AttemptsBeforeExpiry = RetryInput.Attempts;
	Weapon->SendPendingShotInputs();
	TestTrue(TEXT("The original expiry control is sent after four hundred actual milliseconds"), RetryInput.bExpirationSent);
	TestEqual(TEXT("Expiration cannot manufacture another normal retransmission"), RetryInput.Attempts, AttemptsBeforeExpiry);
	Weapon->SendPendingShotInputs();
	TestEqual(TEXT("Repeated expiry wakes cannot exceed the same one-shot lifecycle closure"), RetryInput.Attempts, AttemptsBeforeExpiry);
	Actor->SetRole(ROLE_Authority); Holder->SetRole(ROLE_Authority); CadenceController->SetRole(ROLE_Authority);
	CadenceFire->GetCurrentActivationInfoRef().ActivationMode = EGameplayAbilityActivationMode::Authority;
	ASC->InitAbilityActorInfo(Holder, Holder);
	ASC->CancelAbilityHandle(Spec->Handle);
	return true;
}

#endif
