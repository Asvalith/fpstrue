// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/NetConnection.h"
#include "Engine/ChildConnection.h"
#include "Engine/PackageMapClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "ReferenceSkeleton.h"
#include "Serialization/BitWriter.h"
#include "Serialization/BitReader.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueShotActivationTest,
    "fpstrue.Multiplayer.Prediction.BatchFireAuthorizationAndNativeIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实ASC/native validator、武器执行器和历史采样；合成物理资产提供实际查询几何，不构造虚假的网络送达。
bool FFpstrueShotActivationTest::RunTest(const FString&)
{
    FTestWorldWrapper Fixture;
    if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
    UWorld* World = Fixture.GetTestWorld();
    World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
    if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
    TGuardValue<FString> Listen(World->NextURL, TEXT("?listen"));
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Holder = World->SpawnActor<AfpstrueCharacter>(AfpstrueCharacter::StaticClass(), FTransform::Identity, Spawn);
    auto* Controller = World->SpawnActor<APlayerController>();
    auto* Actor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("Real player avatar"), Holder) || !TestNotNull(TEXT("Remote authority controller"), Controller)
        || !TestNotNull(TEXT("Weapon actor"), Actor)) return false;
    Holder->SetActorEnableCollision(false);
    Holder->GetCharacterMovement()->SetComponentTickEnabled(false);
    Controller->SetActorTickEnabled(false);
    Controller->Possess(Holder);
    if (!TestFalse(TEXT("Remote server activation cannot independently pulse a local shot"), Holder->IsLocallyControlled())) return false;
    USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Holder);
    {
        FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("GripPoint")), TEXT("GripPoint"), INDEX_NONE), FTransform::Identity);
    }
    Mesh->CalculateInvRefMatrices();
    auto* Physics = NewObject<UPhysicsAsset>(Mesh);
    auto* Body = NewObject<USkeletalBodySetup>(Physics);
    Body->BoneName = TEXT("GripPoint");
    Body->AggGeom.SphereElems.AddDefaulted_GetRef().Radius = 10.0f;
    Physics->SkeletalBodySetups.Add(Body);
    Physics->UpdateBodySetupIndexMap();
    Mesh->SetPhysicsAsset(Physics);
    for (USkeletalMeshComponent* Part : {Holder->GetMesh1P(), Holder->GetMesh()})
    {
        Part->bEnableAnimation = false;
        Part->SetComponentTickEnabled(false);
        Part->SetSkeletalMesh(Mesh);
    }
    auto* Weapon = NewObject<UfpstrueWeaponComponent>(Actor);
    Actor->AddInstanceComponent(Weapon);
    Actor->SetRootComponent(Weapon);
    Weapon->RegisterComponent();
    if (!TestTrue(TEXT("Use the real equipment transaction"), Weapon->AttachWeapon(Holder))) return false;
    UfpstrueHealthComponent* ASC = Holder->GetHealthComponent();
    ASC->InitAbilityActorInfo(Holder, Holder);
    FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
    if (!TestNotNull(TEXT("Real granted Fire spec"), Spec)) return false;
    const FGameplayAbilitySpecHandle Handle = Spec->Handle;
    const auto Fire = [&]() { return Cast<UfpstrueFireAbility>(ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance()); };
    const auto Key = [](int16 Current) { FPredictionKey Result; Result.Current = Current; return Result; };
    const auto Request = [&](uint32 Shot, uint32 Session)
    {
        Holder->RecordServerHitHistory();
        FFPShotRequest R;
        R.Avatar = Holder; R.Weapon = Actor; R.EquipmentGeneration = Weapon->EquipmentRevision;
        R.FireSession = Session; R.ShotId = Shot; R.ServerFireTime = World->GetTimeSeconds();
        R.AimDirection = FVector::ForwardVector;
        return R;
    };
    const auto Identity = [](const FFPShotRequest& R, uint32 First)
    { return FFPShotStreamIdentity{R.Weapon, R.EquipmentGeneration, R.FireSession, First}; };
    const auto Tick = [&](float Seconds)
    {
        if (!Fixture.TickTestWorld(Seconds)) { Fixture.ForwardErrorMessages(this); return false; }
        return true;
    };
    const int32 StartingAmmo = Weapon->GetCurrentAmmo();
    const FPredictionKey FirstKey = Key(32766);
    const FFPShotRequest First = Request(1, 1);
    FVector FirstView;
    if (!TestTrue(TEXT("The real server history sampler provides the network execution prerequisite"), Holder->GetHistoricalViewOrigin(First.ServerFireTime, FirstView))) return false;
    ASC->InternalServerTryActivateAbility(Handle, true, FirstKey, nullptr);
    TestFalse(TEXT("Reliable native activation alone never authorizes remote Fire"), ASC->FindAbilitySpecFromHandle(Handle)->IsActive());
    TestEqual(TEXT("Ignoring native activation consumes no ammo"), Weapon->GetCurrentAmmo(), StartingAmmo);
    ASC->ServerSubmitShotBatch_Implementation(Handle, FirstKey, 1, {First});
    if (!TestNotNull(TEXT("Batch creates the real reusable native instance"), Fire())) return false;
    TestTrue(TEXT("First batch activates through the native validator"), Fire()->IsActive());
    AddInfo(FString::Printf(TEXT("First authorized input: verdict=%d received=%u executed=%u pending=%d ammo=%d"),
        static_cast<int32>(Weapon->LastShotResult), Weapon->LastReceivedShotId, Weapon->LastExecutedShotId,
        Weapon->PendingNetworkShots.Num(), Weapon->GetCurrentAmmo()));
    TestEqual(TEXT("First batch executes the unique weapon path once"), Weapon->GetCurrentAmmo(), StartingAmmo - 1);
    const uint32 FirstRevision = Fire()->ActivationRevision;
    ASC->InternalServerTryActivateAbility(Handle, true, FirstKey, nullptr);
    ASC->ServerSubmitShotBatch_Implementation(Handle, FirstKey, 1, {First});
    TestEqual(TEXT("Late native activation and duplicate batch cannot reactivate"), Fire()->ActivationRevision, FirstRevision);
    TestEqual(TEXT("Duplicate paths cannot spend another round"), Weapon->GetCurrentAmmo(), StartingAmmo - 1);

    FGameplayAbilityActivationInfo OldInfo = Fire()->GetCurrentActivationInfo();
    ASC->ServerEndAbility_Implementation(Handle, OldInfo, FirstKey);
    ASC->ServerCancelAbility_Implementation(Handle, OldInfo);
    ASC->ServerSetReplicatedEvent_Implementation(EAbilityGenericReplicatedEvent::InputReleased, Handle, FirstKey, FirstKey);
    ASC->ServerSetReplicatedEventWithPayload_Implementation(EAbilityGenericReplicatedEvent::InputReleased, Handle, FirstKey, FirstKey, FVector::ZeroVector);
    ASC->ServerSetReplicatedTargetDataCancelled_Implementation(Handle, FirstKey, FirstKey);
    ASC->ServerSetReplicatedTargetData_Implementation(Handle, FirstKey, FGameplayAbilityTargetDataHandle(), FGameplayTag(), FirstKey);
    ASC->ServerSetInputReleased_Implementation(Handle);
    ASC->ServerSetInputPressed_Implementation(Handle);
    TestTrue(TEXT("Native key-only controls cannot end or mark the live Fire instance remotely ended"), Fire()->IsActive());
    TestTrue(TEXT("Native release cannot clear current input state"), ASC->FindAbilitySpecFromHandle(Handle)->InputPressed);
    ASC->ServerFinishShotInputs_Implementation(Handle, FirstKey, Identity(First, 1), {}, false, false);
    TestFalse(TEXT("Empty acknowledged tail still ends the real server ability"), Fire()->IsActive());
    TestFalse(TEXT("Empty tail releases Firing tag"), ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
    ASC->InternalServerTryActivateAbility(Handle, true, FirstKey, nullptr);
    TestFalse(TEXT("Reliable activation arriving after explicit end cannot resurrect Fire"), Fire()->IsActive());

    const FFPShotRequest CancelledBeforeArrival = Request(2, 2);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(32767), Identity(CancelledBeforeArrival, 2), {}, true, false);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(32767), 2, {CancelledBeforeArrival});
    TestFalse(TEXT("A cancel before the first input permanently closes that identity"), Fire()->IsActive());
    TestEqual(TEXT("Cancelled before arrival never spends ammo"), Weapon->GetCurrentAmmo(), StartingAmmo - 1);
    if (!Tick(.2f)) return false;
    const FFPShotRequest Tap = Request(3, 3);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(32767), Identity(Tap, 3), {Tap}, false, false);
    TestFalse(TEXT("Quick tap can authorize its genuine tail then end"), Fire()->IsActive());
    TestEqual(TEXT("Quick tap executes exactly its genuine input"), Weapon->GetCurrentAmmo(), StartingAmmo - 2);
    if (!Tick(.2f)) return false;
    const FFPShotRequest Wrapped = Request(4, 4);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(1), 4, {Wrapped});
    TestTrue(TEXT("Sixteen-bit prediction key wrap is allowed by newer extended stream identity"), Fire()->IsActive());
    const uint32 WrappedRevision = Fire()->ActivationRevision;
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(32767), Identity(Tap, 3), {}, false, false);
    ASC->ServerEndAbility_Implementation(Handle, OldInfo, FirstKey);
    TestTrue(TEXT("Old extended and native ends cannot affect the post-wrap stream"), Fire()->IsActive());
    TestEqual(TEXT("Old controls do not restart the new stream"), Fire()->ActivationRevision, WrappedRevision);
    if (!Tick(2.1f)) return false;
    FFPShotRequest OldReplay = CancelledBeforeArrival;
    OldReplay.ServerFireTime = World->GetTimeSeconds();
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(32767), 2, {OldReplay});
    TestEqual(TEXT("Closed-receipt expiry cannot revive an old stream with a forged fresh timestamp"), Fire()->ActivationRevision, WrappedRevision);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(1), Identity(Wrapped, 4), {}, false, false);

    const int32 BeforeRejected = Weapon->GetCurrentAmmo();
    ASC->AddLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
    FFPShotRequest Rejected = Request(5, 5);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(2), 5, {Rejected});
    TestFalse(TEXT("Native CanActivate still rejects reload conflict"), Fire()->IsActive());
    TestTrue(TEXT("A genuinely denied stream consumes its freshness fence"), ASC->FireStreamFence.Phase == UfpstrueHealthComponent::EFireStreamPhase::Closed);
    ASC->RemoveLooseGameplayTag(FpstrueGameplayTags::State_Reloading);
    Rejected.ServerFireTime = World->GetTimeSeconds();
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(2), 5, {Rejected});
    TestFalse(TEXT("Removing a blocker cannot revive the same denied stream"), Fire()->IsActive());
    TestEqual(TEXT("Native denial and replay consume no ammo"), Weapon->GetCurrentAmmo(), BeforeRejected);
    const FFPShotRequest Sixth = Request(6, 6);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(3), 6, {Sixth});
    TestTrue(TEXT("A genuinely new stream can activate after the blocker clears"), Fire()->IsActive());
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(3), Identity(Sixth, 6), {}, false, false);
    if (!Tick(.2f)) return false;

    FFPShotRequest StaleFirst = Request(7, 7);
    StaleFirst.ServerFireTime -= 1.0;
    const int32 BeforePolicy = Weapon->GetCurrentAmmo();
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(4), 7, {StaleFirst});
    TestFalse(TEXT("Stale input cannot activate"), Fire()->IsActive());
    TestTrue(TEXT("Stale first input gets its own terminal refund"), Weapon->FinalizedShotInputs.Contains(7));
    TestEqual(TEXT("Pre-authorization rejection does not advance the global admitted watermark"), Weapon->LastReceivedShotId, uint32(6));
    const FFPShotRequest FreshSecond = Request(8, 7);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(4), 7, {FreshSecond});
    TestTrue(TEXT("Later fresh input in the same held stream can authorize normally"), Fire()->IsActive());
    TestEqual(TEXT("Already finalized stale first is skipped without a false reorder hole"), Weapon->LastExecutedShotId, uint32(8));
    TestTrue(TEXT("No fake missing-input wait remains after skipping the refunded first"), Weapon->OrderedShotInputs.IsEmpty());
    TestEqual(TEXT("Only the later valid input spends ammunition"), Weapon->GetCurrentAmmo(), BeforePolicy - 1);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(4), Identity(FreshSecond, 7), {}, false, false);
    if (!Tick(.2f)) return false;

    const FFPShotRequest Outer = Request(9, 8), Replacement = Request(10, 9);
    const FPredictionKey OuterKey = Key(5), ReplacementKey = Key(6);
    const int32 BeforeCommit = Weapon->GetCurrentAmmo();
    bool bReentered = false;
    const FDelegateHandle Commit = ASC->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility* Ability)
    {
        if (bReentered || !Ability->IsA<UfpstrueFireAbility>()) return;
        bReentered = true;
        ASC->ServerFinishShotInputs_Implementation(Handle, OuterKey, Identity(Outer, 9), {}, true, false);
        ASC->ServerSubmitShotBatch_Implementation(Handle, ReplacementKey, 10, {Replacement});
    });
    ASC->ServerSubmitShotBatch_Implementation(Handle, OuterKey, 9, {Outer});
    ASC->AbilityCommittedCallbacks.Remove(Commit);
    TestTrue(TEXT("The real native Commit synchronously performed cancellation and replacement"), bReentered);
    TestTrue(TEXT("Only the replacement native instance remains active"), Fire()->IsActive() && Fire()->GetCurrentActivationInfo().GetActivationPredictionKey() == ReplacementKey);
    TestTrue(TEXT("Cancelled outer first input is finalized immediately without waiting for400ms expiry"), Weapon->FinalizedShotInputs.Contains(9));
    TestEqual(TEXT("Only the nested replacement executes"), Weapon->LastExecutedShotId, uint32(10));
    TestEqual(TEXT("Old activation stack cannot spend a second round"), Weapon->GetCurrentAmmo(), BeforeCommit - 1);
    ASC->ServerFinishShotInputs_Implementation(Handle, ReplacementKey, Identity(Replacement, 10), {}, false, false);

    // 真实FPredictionKey序列化建立PackageMap归属；不伪造私有connection字段。
    // 只测PackageMap归属，不建立socket/driver；ChildConnection的正式清理允许没有独立Driver。
    auto* Connection = NewObject<UChildConnection>(Controller);
    auto* Map = NewObject<UPackageMapClient>(Connection);
    auto* ForeignMap = NewObject<UPackageMapClient>(Connection);
    Connection->PackageMap = Map;
    Controller->Player = Connection;
    Controller->NetConnection = Connection;
    const auto ReceivedKey = [](UPackageMap* ForMap)
    {
        FPredictionKey Source; Source.Current = 7;
        FBitWriter Writer(128, true); bool bSuccess = true;
        Source.NetSerialize(Writer, ForMap, bSuccess);
        FBitReader Reader(Writer.GetData(), Writer.GetNumBits());
        FPredictionKey Result; Result.NetSerialize(Reader, ForMap, bSuccess);
        return Result;
    };
    const FPredictionKey Owned = ReceivedKey(Map), Foreign = ReceivedKey(ForeignMap);
    TestTrue(TEXT("Current PackageMap is accepted"), ASC->HasCurrentPredictionConnection(Owned));
    TestFalse(TEXT("Same numeric key from another PackageMap is rejected"), ASC->HasCurrentPredictionConnection(Foreign));
    const FFPShotRequest WrongConnection = Request(11, 10);
    const uint32 BeforeForeign = Fire()->ActivationRevision;
    ASC->ServerSubmitShotBatch_Implementation(Handle, Foreign, 11, {WrongConnection});
    TestEqual(TEXT("Foreign connection cannot enter the native validator"), Fire()->ActivationRevision, BeforeForeign);
    Connection->PackageMap = ForeignMap;
    TestFalse(TEXT("Old connection mapping is invalid immediately after replacement"), ASC->HasCurrentPredictionConnection(Owned));
    TestTrue(TEXT("New connection mapping is recognized"), ASC->HasCurrentPredictionConnection(Foreign));
    Controller->Player = nullptr;
    Controller->NetConnection = nullptr;
    Connection->CleanUp();
    TestNull(TEXT("The ownership-only connection explicitly releases its PackageMap before later GC"), Connection->PackageMap.Get());
    const FFPShotRequest BeforeGeneration = Request(12, 11);
    Weapon->ResetNetworkInputGeneration();
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(8), 12, {BeforeGeneration});
    TestFalse(TEXT("Pre-reconnect equipment generation cannot authorize the retained pawn"), Fire()->IsActive());
    if (!Tick(.2f)) return false;
    const FFPShotRequest AfterGeneration = Request(1, 1);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(1), 1, {AfterGeneration});
    TestTrue(TEXT("New generation rebuilds the scope and permits a fresh sequence"), Fire()->IsActive());
    TestEqual(TEXT("New generation has its own execution identity"), Weapon->LastExecutedShotId, uint32(1));
    const int32 BeforeEmptyNewer = Weapon->GetCurrentAmmo();
    const FFPShotRequest EmptyNewer = Request(2, 2);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(2), Identity(EmptyNewer, 2), {}, false, false);
    TestFalse(TEXT("A newer empty finished stream ends an older server Fire whose reliable finish was delayed"), Fire()->IsActive());
    TestFalse(TEXT("Newer empty finish releases the older firing tag"), ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
    TestFalse(TEXT("Explicit finish updates the real server input state before callbacks"), ASC->FindAbilitySpecFromHandle(Handle)->InputPressed);
    TestEqual(TEXT("Ending an empty newer stream does not consume ammo"), Weapon->GetCurrentAmmo(), BeforeEmptyNewer);
    if (!Tick(.2f)) return false;
    const FFPShotRequest Newest = Request(3, 3);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(3), 3, {Newest});
    TestTrue(TEXT("A subsequent real stream can start after the empty finish"), Fire()->IsActive());
    const uint32 NewestRevision = Fire()->ActivationRevision;
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(2), Identity(EmptyNewer, 2), {}, false, false);
    TestTrue(TEXT("The delayed empty finish cannot end a newer stream"), Fire()->IsActive());
    TestEqual(TEXT("Delayed empty finish preserves the new activation revision"), Fire()->ActivationRevision, NewestRevision);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(3), Identity(Newest, 3), {}, false, false);
    TestFalse(TEXT("Final explicit end clears the server input flag"), ASC->FindAbilitySpecFromHandle(Handle)->InputPressed);

    Weapon->ResetNetworkInputGeneration();
    const int32 BeforeEmptyTaps = Weapon->GetCurrentAmmo();
    const FFPShotRequest EmptyTapOne = Request(1, 1), EmptyTapTwo = Request(1, 2);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(4), Identity(EmptyTapOne, 1), {}, false, false);
    TestEqual(TEXT("First no-shot quick tap closes its own session"), ASC->FireStreamFence.Session, uint32(1));
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(5), Identity(EmptyTapTwo, 1), {}, false, false);
    TestEqual(TEXT("Second no-shot quick tap may retain the same unconsumed first shot"), ASC->FireStreamFence.Session, uint32(2));
    TestFalse(TEXT("Consecutive empty taps never create an active server ability"), Fire()->IsActive());
    TestEqual(TEXT("Consecutive empty taps consume no ammo"), Weapon->GetCurrentAmmo(), BeforeEmptyTaps);
    if (!Tick(.2f)) return false;
    const FFPShotRequest AfterEmptyTaps = Request(1, 3);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(6), 1, {AfterEmptyTaps});
    TestTrue(TEXT("A newer session can fire the still-unconsumed first shot after consecutive empty taps"), Fire()->IsActive());
    TestEqual(TEXT("The first real shot after empty taps executes exactly once"), Weapon->LastExecutedShotId, uint32(1));
    TestEqual(TEXT("Only the real shot after empty taps consumes ammo"), Weapon->GetCurrentAmmo(), BeforeEmptyTaps - 1);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(6), Identity(AfterEmptyTaps, 1), {}, false, false);

    Weapon->ResetNetworkInputGeneration();
    if (!Tick(.2f)) return false;
    TArray<FFPShotRequest> StaleWindow;
    for (uint32 Shot = 1; Shot <= 8; ++Shot)
    {
        FFPShotRequest R = Request(Shot, 1);
        R.ServerFireTime -= 1.0;
        StaleWindow.Add(R);
    }
    const int32 BeforeWindowRecovery = Weapon->GetCurrentAmmo();
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(7), 1, StaleWindow);
    TestFalse(TEXT("A wholly stale first window does not create authorization"), Fire()->IsActive());
    TestEqual(TEXT("All eight stale inputs are individually finalized"), Weapon->FinalizedShotInputs.Num(), 8);
    TestEqual(TEXT("Refunding a whole first window does not forge received input"), Weapon->LastReceivedShotId, uint32(0));
    const FFPShotRequest NinthFresh = Request(9, 1);
    ASC->ServerSubmitShotBatch_Implementation(Handle, Key(7), 1, {NinthFresh});
    TestTrue(TEXT("The ninth fresh input can authorize after the first eight were finalized"), Fire()->IsActive());
    TestEqual(TEXT("The ninth input executes through the unchanged eight-input window"), Weapon->LastExecutedShotId, uint32(9));
    TestEqual(TEXT("Receive advances only after the real ninth input"), Weapon->LastReceivedShotId, uint32(9));
    TestEqual(TEXT("Finalized prefix leaves no false missing-input wait"), Weapon->NextOrderedShotId, uint32(10));
    TestTrue(TEXT("Finalized prefix leaves no queued reorder work"), Weapon->OrderedShotInputs.IsEmpty());
    TestFalse(TEXT("Finalized prefix schedules no fake lost-input timer"), World->GetTimerManager().IsTimerActive(Weapon->ShotReorderTimerHandle));
    TestEqual(TEXT("Only the fresh ninth input spends ammunition"), Weapon->GetCurrentAmmo(), BeforeWindowRecovery - 1);
    ASC->ServerFinishShotInputs_Implementation(Handle, Key(7), Identity(NinthFresh, 1), {}, false, false);

    // 真正让WaitInputRelease在ReadyForActivation内同步消费已松开的Spec；此处只测两个端点，不声称RPC已走过网络。
    Weapon->ResetNetworkInputGeneration();
    Controller->SetAsLocalPlayerController();
    Controller->SetRole(ROLE_AutonomousProxy);
    Holder->SetRole(ROLE_AutonomousProxy);
    Actor->SetRole(ROLE_SimulatedProxy);
    ASC->InitAbilityActorInfo(Holder, Holder);
    ASC->FindAbilitySpecFromHandle(Handle)->InputPressed = false;
    const uint32 BeforeImmediateSession = Weapon->LocalFireSessionSerial, BeforeImmediateShot = Weapon->LocalShotSerial;
    const int32 BeforeImmediateAmmo = Weapon->GetCurrentAmmo();
    TestTrue(TEXT("Already released local input still enters the real native activation lifecycle"), ASC->InternalTryActivateAbility(Handle));
    const FPredictionKey ImmediateKey = Fire()->GetCurrentActivationInfo().GetActivationPredictionKey();
    TestFalse(TEXT("WaitInputRelease immediately ends the real predicted activation"), Fire()->IsActive());
    TestFalse(TEXT("Immediate release removes its local Firing tag"), ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
    TestEqual(TEXT("Immediate release still prepares exactly one stream before the callback"), Weapon->LocalFireSessionSerial, BeforeImmediateSession + 1);
    TestEqual(TEXT("Immediate release produces no shot input"), Weapon->LocalShotSerial, BeforeImmediateShot);
    TestEqual(TEXT("Immediate release consumes no authority ammo"), Weapon->GetCurrentAmmo(), BeforeImmediateAmmo);
    const auto* ImmediatePresentation = Weapon->PredictedPresentations.FindByPredicate([&](const UfpstrueWeaponComponent::FPredictedFirePresentation& P)
        { return P.Key == ImmediateKey.Current && P.Generation == Weapon->EquipmentRevision; });
    if (!TestNotNull(TEXT("Immediate End can resolve its own nonempty stream identity for empty-tail Finish"), ImmediatePresentation)) return false;
    FFPShotStreamIdentity ImmediateStream{Actor, ImmediatePresentation->Generation, ImmediatePresentation->Session, ImmediatePresentation->FirstShotId};
    Actor->SetRole(ROLE_Authority);
    Holder->SetRole(ROLE_Authority);
    Controller->SetRole(ROLE_Authority);
    ASC->InitAbilityActorInfo(Holder, Holder);
    ASC->ServerFinishShotInputs_Implementation(Handle, ImmediateKey, ImmediateStream, {}, false, false);
    TestTrue(TEXT("The exact immediate-release identity closes server prediction without a fake Fire activation"),
        ASC->FireStreamFence.Generation == ImmediateStream.EquipmentGeneration && ASC->FireStreamFence.Session == ImmediateStream.FireSession
        && ASC->FireStreamFence.FirstShotId == ImmediateStream.FirstShotId && ASC->FireStreamFence.Phase == UfpstrueHealthComponent::EFireStreamPhase::Closed);
    TestFalse(TEXT("Closing immediate-release prediction leaves no server Firing tag"), ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
    TestEqual(TEXT("Closing a no-shot prediction never spends ammo"), Weapon->GetCurrentAmmo(), BeforeImmediateAmmo);
    return true;
}
#endif
