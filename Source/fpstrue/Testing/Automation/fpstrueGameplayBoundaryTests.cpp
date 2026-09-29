// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Automation/fpstrueReloadReentryObserver.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"

void UfpstrueReloadReentryObserver::HandleAmmoChanged(int32 CurrentAmmo, int32 MagazineSize, int32 ReserveAmmo)
{
	++CallbackCount;
	if (ReloadAnimInstance.IsValid() && ReloadMontage.IsValid())
	{
		const FAnimMontageInstance* Playback = ReloadAnimInstance->GetActiveInstanceForMontage(ReloadMontage.Get());
		bAmmoChangedWhileMontageActive |= Playback != nullptr && Playback->IsActive();
	}
	if (bDisableWeaponOnAmmoChanged)
	{
		if (UfpstrueWeaponComponent* ObservedWeapon = Weapon.Get())
		{
			ObservedWeapon->DisableWeapon();
		}
	}
}

void UfpstrueReloadReentryObserver::HandleEquippedWeaponChanged(UfpstrueWeaponComponent* EquippedWeapon)
{
	if (bUnequipOnEquipped && EquippedWeapon != nullptr && Player.IsValid())
	{
		Player->ClearEquippedWeaponComponent(EquippedWeapon);
	}
}

void UfpstrueReloadReentryObserver::HandleFirePerformed()
{
	++FireEventCount;
}

void UfpstrueReloadReentryObserver::HandleReloadEnded(int32 ReloadId, EFPReloadEndReason Reason, bool bAmmoCommitted)
{
	++ReloadEndCount;
	LastReloadEndReason = Reason;
	bLastReloadCommitted = bAmmoCommitted;
	if (UfpstrueWeaponComponent* ObservedWeapon = Weapon.Get())
	{
		if (bFireOnReloadEnd) ObservedWeapon->StartFire();
		if (bRestartReloadOnEnd) bReloadRestartAccepted = ObservedWeapon->RequestReload();
	}
}

void UfpstrueReloadReentryObserver::HandleReloadPlaybackRequested(int32 ReloadId, bool bWasEmptyReload)
{
	if (Weapon.IsValid() && ReloadAnimInstance.IsValid() && ReloadMontage.IsValid())
	{
		Weapon->PlayReloadMontage(ReloadId, ReloadAnimInstance->GetSkelMeshComponent(), ReloadMontage.Get());
	}
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/Performance/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Shared/fpstrueCollisionChannels.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "AIController.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/Skeleton.h"
#include "Weapons/fpstrueAnimNotify_ReloadCommit.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "ReferenceSkeleton.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace FpstrueGameplayBoundaryTests
{
// FTestWorldWrapper is the engine's own temporary-world fixture. Its destructor
// ends play, removes its world context, destroys its world and restores GFrameCounter.
// No editor map, project GameMode, blueprint, asset file or global CVar is modified.
class FGameplayWorld
{
public:
	explicit FGameplayWorld(FAutomationTestBase& InTest) : Test(InTest) {}

	bool Initialize()
	{
		if (!Test.TestNotNull(TEXT("An engine is available for the temporary world"), GEngine))
		{
			return false;
		}
		if (!Wrapper.CreateTestWorld(EWorldType::Game))
		{
			Wrapper.ForwardErrorMessages(&Test);
			return false;
		}

		// The wrapper starts a GameMode to dispatch component/actor BeginPlay.
		// Select the native base so project defaults cannot spawn a real match.
		GetWorld()->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
		// FTestWorldWrapper 默认不创建 AI 系统；真实行为树/Blackboard 需要它。
		GetWorld()->CreateAISystem();
		if (!Wrapper.BeginPlayInTestWorld())
		{
			Wrapper.ForwardErrorMessages(&Test);
			return false;
		}
		return true;
	}

	UWorld* GetWorld() const { return Wrapper.GetTestWorld(); }
	bool AdvanceSingleFrame(float DeltaSeconds) { return Wrapper.TickTestWorld(DeltaSeconds); }

	bool Advance(float DurationSeconds)
	{
		// Small explicit steps advance real timers and behavior-tree tasks, including
		// the randomized initial decision wait, without a latent editor session.
		const int32 FrameCount = FMath::CeilToInt(DurationSeconds / 0.01f);
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			if (!Wrapper.TickTestWorld(0.01f))
			{
				Wrapper.ForwardErrorMessages(&Test);
				return false;
			}
		}
		return true;
	}

	template <typename TActor>
	TActor* Spawn(const FTransform& Transform = FTransform::Identity)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		TActor* Actor = GetWorld()->SpawnActor<TActor>(TActor::StaticClass(), Transform, SpawnParameters);
		Test.TestNotNull(TEXT("A native test actor was spawned"), Actor);
		return Actor;
	}

	AfpstrueCharacter* SpawnPlayer(const FVector& Location = FVector::ZeroVector)
	{
		AfpstrueCharacter* Player = Spawn<AfpstrueCharacter>(FTransform(Location));
		if (Player != nullptr)
		{
			// These tests control actor poses explicitly; no floor or physics motion is needed.
			Player->SetActorEnableCollision(false);
			Player->GetCharacterMovement()->SetComponentTickEnabled(false);
		}
		return Player;
	}

private:
	FAutomationTestBase& Test;
	FTestWorldWrapper Wrapper;
};

struct FEnemyFacingFixture
{
	AfpstrueEnemyCharacter* Enemy = nullptr;
	AfpstrueEnemyAIController* Controller = nullptr;
	UCharacterMovementComponent* Movement = nullptr;

	bool Initialize(FGameplayWorld& Fixture, FAutomationTestBase& Test, float ActorYaw, const FVector& TargetLocation)
	{
		AfpstrueCharacter* Target = Fixture.SpawnPlayer(TargetLocation);
		if (Target == nullptr)
		{
			return false;
		}

		const FTransform EnemyTransform(FRotator(0.0f, ActorYaw, 0.0f), FVector::ZeroVector);
		Enemy = Fixture.GetWorld()->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), EnemyTransform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Test.TestNotNull(TEXT("A native enemy was spawned"), Enemy))
		{
			return false;
		}
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(EnemyTransform);
		Enemy->SetActorEnableCollision(false);

		Movement = Enemy->GetCharacterMovement();
		Movement->SetComponentTickEnabled(false);
		Controller = Fixture.Spawn<AfpstrueEnemyAIController>();
		if (Controller == nullptr)
		{
			return false;
		}
		Controller->Possess(Enemy);
		Controller->InitializeCombatContext(Target, nullptr);
		// Possess restarts Character movement; set the ground-mode fixture afterwards.
		Movement->SetMovementMode(MOVE_Walking);
		return Test.TestTrue(TEXT("The native target is in attack range"), Enemy->IsTargetInAttackRange());
	}
};

UfpstrueWeaponComponent* EquipWeaponWithOneSpentRound(FGameplayWorld& Fixture, FAutomationTestBase& Test)
{
	AfpstrueCharacter* Player = Fixture.SpawnPlayer();
	AAIController* Controller = Fixture.Spawn<AAIController>();
	if (Player == nullptr || Controller == nullptr)
	{
		return nullptr;
	}
	// Fire requires a controller, but no local player/input assets or recoil are needed.
	Controller->SetActorTickEnabled(false);
	Controller->Possess(Player);

	// AttachWeapon accepts a socket or bone. A transient one-bone reference mesh
	// provides the real GripPoint prerequisite without loading animation/render assets.
	USkeletalMesh* AttachmentMesh = NewObject<USkeletalMesh>(Player, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(AttachmentMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("GripPoint")), TEXT("GripPoint"), INDEX_NONE), FTransform::Identity);
	}
	AttachmentMesh->CalculateInvRefMatrices();
	USkeletalMeshComponent* Arms = Player->GetMesh1P();
	Arms->bEnableAnimation = false;
	Arms->SetComponentTickEnabled(false);
	Arms->SetVisibility(false);
	Arms->SetSkeletalMesh(AttachmentMesh);
	if (!Test.TestTrue(TEXT("The transient mesh supplies the real attachment bone"), Arms->DoesSocketExist(TEXT("GripPoint"))))
	{
		return nullptr;
	}

	// Match production ownership: equipment belongs to a separate pickup Actor, not the player.
	AActor* PickupActor = Fixture.Spawn<AActor>();
	if (PickupActor == nullptr) return nullptr;
	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(PickupActor, NAME_None, RF_Transient);
	PickupActor->AddInstanceComponent(Weapon);
	PickupActor->SetRootComponent(Weapon);
	Weapon->RegisterComponent();
	if (!Test.TestTrue(TEXT("The weapon equips through its public API"), Weapon->AttachWeapon(Player)))
	{
		return nullptr;
	}

	Weapon->StartFire();
	Weapon->StopFire();
	if (!Test.TestEqual(TEXT("One real shot creates a reloadable magazine"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize() - 1))
	{
		return nullptr;
	}
	// Leave the firing interval behind so the final enabled/disabled probe is meaningful.
	return Fixture.Advance(0.2f) ? Weapon : nullptr;
}
} // namespace FpstrueGameplayBoundaryTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueFacingToleranceTest, "fpstrue.Gameplay.Boundaries.AI.FacingTolerance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueFacingToleranceTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	FEnemyFacingFixture Facing;
	if (!World.Initialize() || !Facing.Initialize(World, *this, 16.0f, FVector(200.0f, 0.0f, 0.0f)))
	{
		return false;
	}

	// Simulate residual ground motion while PathFollowing is already idle.
	TestEqual(TEXT("The default turn rate remains 540 degrees per second"), Facing.Movement->RotationRate.Yaw, 540.0);
	Facing.Movement->Velocity = FVector(100.0f, 0.0f, 0.0f);
	Facing.Movement->RequestDirectMove(FVector(100.0f, 0.0f, 0.0f), false);
	Facing.Enemy->AddMovementInput(FVector::ForwardVector, 1.0f, true);
	if (!World.Advance(0.35f))
	{
		return false;
	}

	TestFalse(TEXT("Controller Actor Tick remains disabled"), Facing.Controller->IsActorTickEnabled());
	TestFalse(TEXT("A 16-degree error cannot start the attack transaction"), Facing.Enemy->IsAttacking());
	TestTrue(TEXT("The decision clears residual ground velocity"), Facing.Movement->Velocity.IsNearlyZero());
	TestTrue(TEXT("The decision consumes queued movement input"), Facing.Movement->GetPendingInputVector().IsNearlyZero());
	TestTrue(TEXT("Standing turn uses controller desired rotation"), Facing.Movement->bUseControllerDesiredRotation);
	TestFalse(TEXT("Standing turn no longer uses movement orientation"), Facing.Movement->bOrientRotationToMovement);
	TestTrue(TEXT("The desired yaw faces the target"), FMath::IsNearlyZero(Facing.Controller->GetControlRotation().Yaw, 0.01));
	TestTrue(TEXT("Decisions do not snap the actor to ControlRotation"),
		FMath::IsNearlyEqual(Facing.Enemy->GetActorRotation().Yaw, 16.0, 0.01));

	// Exercise the product's inclusive tolerance through the same behavior-tree decision.
	// No private helper or duplicate angular-error implementation is used by this test.
	Facing.Enemy->SetActorRotation(FRotator(0.0f, 15.0f, 0.0f));
	if (!World.Advance(0.15f))
	{
		return false;
	}
	TestTrue(TEXT("A 15-degree error permits the attack transaction"), Facing.Enemy->IsAttacking());

	FEnemyFacingFixture WrappedFacing;
	// Input geometry straddles +/-180; the product must treat this as a two-degree turn.
	const FVector TargetLocation = FRotator(0.0f, -179.0f, 0.0f).Vector() * 200.0f;
	if (!WrappedFacing.Initialize(World, *this, 179.0f, TargetLocation) || !World.Advance(0.35f))
	{
		return false;
	}

	TestTrue(TEXT("179 to -179 degrees is already within attack tolerance"), WrappedFacing.Enemy->IsAttacking());
	TestTrue(TEXT("The actual actor pose remains 179 degrees"),
		FMath::IsNearlyEqual(WrappedFacing.Enemy->GetActorRotation().Yaw, 179.0, 0.01));

	// 关闭伤害窗口可以重复调用，但不能提前结束完整攻击；完整重置才释放事务。
	UfpstrueEnemyCombatComponent* Combat = Facing.Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("Character exposes its owned combat component"), Combat))
		return false;
	Combat->EndAttackWindow();
	Combat->UpdateAttackWindow();
	Combat->EndAttackWindow();
	TestTrue(TEXT("Closing the damage window leaves the attack transaction active"), Facing.Enemy->IsAttacking());
	Combat->ResetCombat();
	Facing.Enemy->HandleAttackFinishedNotify();
	TestFalse(TEXT("A late finish notification cannot restart a reset attack"), Facing.Enemy->IsAttacking());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRotationConfigTest, "fpstrue.Gameplay.Boundaries.AI.RotationConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRotationConfigTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	const FFloatProperty* TurnRateProperty =
		FindFProperty<FFloatProperty>(AfpstrueEnemyCharacter::StaticClass(), TEXT("MovementYawRotationRate"));
	if (!World.Initialize() || !TestNotNull(TEXT("The turn rate is an editable reflected property"), TurnRateProperty))
	{
		return false;
	}

	// 模拟蓝图默认值在 BeginPlay 前生效，不增加产品代码的测试专用接口。
	for (const float ConfiguredRate : {180.0f, -1.0f})
	{
		AfpstrueEnemyCharacter* Enemy = World.GetWorld()->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("The configured enemy was created"), Enemy))
		{
			return false;
		}
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		TurnRateProperty->SetPropertyValue_InContainer(Enemy, ConfiguredRate);
		Enemy->FinishSpawning(FTransform::Identity);
		TestEqual(TEXT("BeginPlay applies the configured rate with its safety limit"),
			Enemy->GetCharacterMovement()->RotationRate.Yaw, static_cast<double>(FMath::Max(ConfiguredRate, 1.0f)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueAnimationDefaultsConfigTest, "fpstrue.Gameplay.Boundaries.AI.AnimationDefaultsConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueAnimationDefaultsConfigTest::RunTest(const FString& Parameters)
{
	const UfpstrueEnemyAnimationSharingCoordinator* Defaults = GetDefault<UfpstrueEnemyAnimationSharingCoordinator>();
	// 验证真正的原生 CDO 已从 INI 初始化软引用；不加载动画、不修改蓝图或配置文件。
	for (const TCHAR* PropertyName : {TEXT("IdleAnimation"), TEXT("MovingAnimation")})
	{
		FString ConfiguredPath;
		const FSoftObjectProperty* AssetProperty = FindFProperty<FSoftObjectProperty>(Defaults->GetClass(), PropertyName);
		if (!TestNotNull(TEXT("Animation reference remains an editable property"), AssetProperty) ||
			!TestTrue(TEXT("Project animation defaults are configured"), GConfig != nullptr &&
				GConfig->GetString(TEXT("fpstrue.EnemyAnimationSharing"), PropertyName, ConfiguredPath, GGameIni)))
		{
			return false;
		}
		TestFalse(TEXT("Default animation path is not empty"), ConfiguredPath.IsEmpty());
		TestEqual(TEXT("Native animation reference matches project configuration"),
			AssetProperty->GetPropertyValue_InContainer(Defaults).ToSoftObjectPath().ToString(), ConfiguredPath);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueHitscanDamageTest, "fpstrue.Gameplay.Boundaries.Weapon.HitscanDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueHitscanDamageTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize())
	{
		return false;
	}
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	AActor* Target = World.Spawn<AActor>();
	if (Weapon == nullptr || Target == nullptr)
	{
		return false;
	}

	// 临时靶子只阻挡射击专用通道；测试完整 Fire -> Trace -> PointDamage -> Health 链。
	UBoxComponent* TargetBox = NewObject<UBoxComponent>(Target);
	Target->AddInstanceComponent(TargetBox);
	Target->SetRootComponent(TargetBox);
	TargetBox->SetBoxExtent(FVector(40.0f));
	TargetBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TargetBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	TargetBox->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	TargetBox->RegisterComponent();
	const UCameraComponent* Camera = CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner())->GetFirstPersonCameraComponent();
	Target->SetActorLocation(Camera->GetComponentLocation() + Camera->GetForwardVector() * 500.0f);
	UfpstrueHealthComponent* Health = NewObject<UfpstrueHealthComponent>(Target);
	Target->AddInstanceComponent(Health);
	Health->RegisterComponent();
	Health->SetMaxHealthAndReset(100.0f);
	const int32 AmmoBeforeShot = Weapon->GetCurrentAmmo();
	Weapon->StartFire();
	Weapon->StopFire();
	TestEqual(TEXT("A single shot consumes exactly one round"), Weapon->GetCurrentAmmo(), AmmoBeforeShot - 1);
	TestEqual(TEXT("A blocking hit applies native body damage once"), Health->GetHealth(), 60.0f);

	TargetBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (!World.Advance(0.2f))
	{
		return false;
	}
	Weapon->StartFire();
	Weapon->StopFire();
	TestEqual(TEXT("A miss still consumes one round"), Weapon->GetCurrentAmmo(), AmmoBeforeShot - 2);
	TestEqual(TEXT("A nonblocking target takes no additional damage"), Health->GetHealth(), 60.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadTransitionsTest, "fpstrue.Gameplay.Boundaries.Weapon.ReloadTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueReloadTransitionsTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;

	Weapon->StartFire();
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	const int32 ReserveBefore = Weapon->GetReserveAmmo();
	TestTrue(TEXT("Reload interrupts automatic fire"), Weapon->RequestReload());
	const int32 CancelledReloadId = Weapon->GetActiveReloadId();
	Weapon->StartFire();
	Weapon->StopFire();
	TestTrue(TEXT("Trigger input cannot unlock Reloading"), Weapon->IsReloading());
	TestFalse(TEXT("Repeated reload is rejected"), Weapon->RequestReload());
	if (!World.Advance(0.3f)) return false;
	TestEqual(TEXT("The old firing timer cannot consume ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);
	Weapon->CancelReload();
	TestFalse(TEXT("Cancelled reload rejects late commit"), Weapon->CommitReloadForTransaction(CancelledReloadId));
	Weapon->FinishReloadForTransaction(CancelledReloadId);
	if (!World.Advance(5.2f)) return false;
	TestEqual(TEXT("Cancellation before commit does not load ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);
	TestEqual(TEXT("Cancelled fallback does not spend reserve"), Weapon->GetReserveAmmo(), ReserveBefore);

	TestTrue(TEXT("Reload can restart after cancellation"), Weapon->RequestReload());
	TestTrue(TEXT("The new reload commits once"), Weapon->CommitReloadForTransaction(Weapon->GetActiveReloadId()));
	Weapon->CancelReload();
	TestEqual(TEXT("Cancellation after commit keeps loaded ammunition"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	TestEqual(TEXT("Reload conserves total ammunition"), Weapon->GetCurrentAmmo() + Weapon->GetReserveAmmo(), AmmoBefore + ReserveBefore);

	Weapon->StartFire();
	Weapon->StopFire();
	TestTrue(TEXT("Missing animation notification still has a fallback"), Weapon->RequestReload());
	const int32 AmmoBeforeTimeout = Weapon->GetCurrentAmmo();
	if (!World.Advance(5.2f)) return false;
	TestFalse(TEXT("Fallback closes the reload"), Weapon->IsReloading());
	TestEqual(TEXT("Fallback cannot disguise a missing animation commit as successful reload"), Weapon->GetCurrentAmmo(), AmmoBeforeTimeout);

	Weapon->StartFire();
	Weapon->StopFire();
	TestTrue(TEXT("Begin reload before unequipping"), Weapon->RequestReload());
	const int32 UnequippedReloadId = Weapon->GetActiveReloadId();
	const int32 AmmoBeforeUnequip = Weapon->GetCurrentAmmo();
	CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner())->ClearEquippedWeaponComponent(Weapon);
	Weapon->FinishReloadForTransaction(UnequippedReloadId);
	Weapon->CancelReload();
	Weapon->StartFire();
	if (!World.Advance(5.2f)) return false;
	TestFalse(TEXT("Unequipped weapon remains disabled"), Weapon->IsFiring());
	TestFalse(TEXT("Unequipping cancels reload"), Weapon->IsReloading());
	TestFalse(TEXT("Unequipped weapon cannot reload"), Weapon->CanReload());
	TestEqual(TEXT("Late callbacks and timeout cannot load an unequipped weapon"), Weapon->GetCurrentAmmo(), AmmoBeforeUnequip);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueAutoReloadConflictTest, "fpstrue.Gameplay.Boundaries.Weapon.AutoReloadConflicts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueAutoReloadConflictTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	AfpstrueCharacter* Player = CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner());
	const FBoolProperty* Aiming = FindFProperty<FBoolProperty>(Player->GetClass(), TEXT("bIsAiming"));
	const FBoolProperty* Sprinting = FindFProperty<FBoolProperty>(Player->GetClass(), TEXT("bIsSprinting"));
	if (!TestNotNull(TEXT("Aim state is reflected"), Aiming) || !TestNotNull(TEXT("Sprint state is reflected"), Sprinting)) return false;
	// 只设置输入前态；耗尽弹匣、换弹和状态收尾均运行真实代码。
	for (const FBoolProperty* InputState : {Aiming, Sprinting})
	{
		InputState->SetPropertyValue_InContainer(Player, true);
		Weapon->StartFire();
		if (!World.Advance(3.2f)) return false;
		TestTrue(TEXT("Empty magazine starts automatic reload"), Weapon->IsReloading());
		TestFalse(TEXT("Automatic reload clears aiming"), Player->IsAiming());
		TestFalse(TEXT("Automatic reload clears sprinting"), Sprinting->GetPropertyValue_InContainer(Player));
		TestEqual(TEXT("Reload restores the unique walk-speed policy"), Player->GetCharacterMovement()->MaxWalkSpeed, 300.0f);
		Weapon->FinishReloadForTransaction(Weapon->GetActiveReloadId());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadFinishReentryTest,
	"fpstrue.Gameplay.Boundaries.Weapon.ReloadFinishPreservesDisabledOnReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueReloadFinishReentryTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize())
	{
		return false;
	}
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr)
	{
		return false;
	}

	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Observer->Weapon = Weapon;
	Weapon->OnAmmoChanged.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);
	const int32 ReserveBeforeFirstReload = Weapon->GetReserveAmmo();
	if (!TestTrue(TEXT("The initial reload request is accepted"), Weapon->RequestReload()))
	{
		return false;
	}
	const int32 FirstReloadId = Weapon->GetActiveReloadId();
	TestTrue(TEXT("The first commit is accepted"), Weapon->CommitReloadForTransaction(FirstReloadId));
	TestTrue(TEXT("Commit leaves the transaction Reloading until Finish"), Weapon->IsReloading());
	TestFalse(TEXT("A duplicate commit is rejected"), Weapon->CommitReloadForTransaction(FirstReloadId));
	Weapon->FinishReloadForTransaction(FirstReloadId);
	Weapon->FinishReloadForTransaction(FirstReloadId);
	TestEqual(TEXT("Duplicate Commit and Finish cannot emit another ammo change"), Observer->CallbackCount, 1);
	TestEqual(TEXT("Reserve ammo was transferred exactly once"), Weapon->GetReserveAmmo(), ReserveBeforeFirstReload - 1);
	TestFalse(TEXT("Finish closes the initial reload transaction"), Weapon->IsReloading());
	Weapon->OnAmmoChanged.RemoveDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);

	Weapon->StartFire();
	Weapon->StopFire();
	if (!TestEqual(TEXT("A real shot prepares the reentrant reload"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize() - 1)
		|| !World.Advance(0.2f))
	{
		return false;
	}
	Observer->CallbackCount = 0;
	Observer->bDisableWeaponOnAmmoChanged = true;
	Weapon->OnAmmoChanged.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);
	const int32 ReserveBefore = Weapon->GetReserveAmmo();
	if (!TestTrue(TEXT("Reload begins through the public request"), Weapon->RequestReload()))
	{
		return false;
	}

	// Exact regression stack: Finish -> Commit -> synchronous ammo event -> owner-death
	// handler -> old Finish resumes. The pawn stays alive so IsDead cannot mask Ready.
	Weapon->FinishReloadForTransaction(Weapon->GetActiveReloadId());
	TestEqual(TEXT("Finish emitted exactly one commit notification"), Observer->CallbackCount, 1);
	TestEqual(TEXT("The accepted commit filled the magazine"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	TestEqual(TEXT("The accepted commit spent one reserve round"), Weapon->GetReserveAmmo(), ReserveBefore - 1);
	TestFalse(TEXT("The death handler ended Reloading"), Weapon->IsReloading());
	Weapon->OnAmmoChanged.RemoveDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);

	// Probe the state using gameplay behavior, with no private-state accessor or reflection.
	// Before the fix the resumed Finish wrote Ready, so this shot fired successfully.
	const int32 AmmoBeforeProbe = Weapon->GetCurrentAmmo();
	Weapon->StartFire();
	TestFalse(TEXT("The resumed Finish cannot re-enable firing"), Weapon->IsFiring());
	TestEqual(TEXT("The disabled weapon cannot consume another round"), Weapon->GetCurrentAmmo(), AmmoBeforeProbe);
	Weapon->StopFire();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadIdentityTest, "fpstrue.Gameplay.Boundaries.Weapon.ReloadIdentityAndEndReasons",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueReloadIdentityTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Weapon->OnWeaponReloadEnded.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
	TestTrue(TEXT("Reload A starts"), Weapon->RequestReload());
	const int32 ReloadA = Weapon->GetActiveReloadId();
	Weapon->CancelReloadForTransaction(ReloadA);
	TestEqual(TEXT("Cancellation has an explicit end reason"), Observer->LastReloadEndReason, EFPReloadEndReason::Cancelled);
	TestFalse(TEXT("Cancellation does not report a commit"), Observer->bLastReloadCommitted);
	TestTrue(TEXT("Reload B starts"), Weapon->RequestReload());
	const int32 ReloadB = Weapon->GetActiveReloadId();
	TestNotEqual(TEXT("A new reload has a new identity"), ReloadB, ReloadA);
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	TestFalse(TEXT("A delayed commit for A cannot commit B"), Weapon->CommitReloadForTransaction(ReloadA));
	Weapon->FinishReloadForTransaction(ReloadA);
	Weapon->CancelReloadForTransaction(ReloadA);
	TestEqual(TEXT("A delayed finish/cancel cannot terminate B"), Weapon->GetActiveReloadId(), ReloadB);

	// A queue callback without a bound playback must fail closed, not borrow the current reload ID.
	FAnimNotifyEventReference UnboundNotify;
	UnboundNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(12345);
	TestFalse(TEXT("An unbound Montage notify cannot commit the current transaction"),
		Weapon->CommitReloadFromNotify(Weapon, UnboundNotify));
	TestEqual(TEXT("Rejected old callbacks preserve the magazine"), Weapon->GetCurrentAmmo(), AmmoBefore);
	Weapon->FinishReloadForTransaction(ReloadB);
	TestEqual(TEXT("A current finish reports Completed"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
	TestTrue(TEXT("Completed reports the actual ammo commit"), Observer->bLastReloadCommitted);
	TestEqual(TEXT("Only A cancellation and B completion were reported"), Observer->ReloadEndCount, 2);

	Weapon->StartFire();
	Weapon->StopFire();
	TestTrue(TEXT("A missing-notify reload starts"), Weapon->RequestReload());
	const int32 AmmoBeforeTimeout = Weapon->GetCurrentAmmo();
	if (!World.Advance(5.2f)) return false;
	TestEqual(TEXT("The fallback is reported as TimedOut, not Completed"), Observer->LastReloadEndReason, EFPReloadEndReason::TimedOut);
	TestFalse(TEXT("The timeout reports missing commit instead of claiming animation success"), Observer->bLastReloadCommitted);
	TestEqual(TEXT("Timeout cannot invent the missing ammo commit"), Weapon->GetCurrentAmmo(), AmmoBeforeTimeout);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueExternalEquipmentLifetimeTest, "fpstrue.Gameplay.Boundaries.Weapon.ExternalOwnerAndEquipReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueExternalEquipmentLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	AfpstrueCharacter* Player = CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner());
	TestNotEqual(TEXT("The production-style weapon is not owned by the player"), Weapon->GetOwner(), static_cast<AActor*>(Player));
	const FBoolProperty* Aiming = FindFProperty<FBoolProperty>(Player->GetClass(), TEXT("bIsAiming"));
	if (!TestNotNull(TEXT("Aim state exists"), Aiming)) return false;
	Aiming->SetPropertyValue_InContainer(Player, true);
	Player->GetCharacterMovement()->MaxWalkSpeed = 120.0f;
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	Player->ClearEquippedWeaponComponent(Weapon);
	TestFalse(TEXT("Unequip exits ADS"), Player->IsAiming());
	TestEqual(TEXT("Unequip restores walk speed"), Player->GetCharacterMovement()->MaxWalkSpeed, 300.0f);
	TestNull(TEXT("Unequip removes physical attachment"), Weapon->GetAttachParent());
	TestFalse(TEXT("Unequip clears the character slot"), Player->HasEquippedWeapon());

	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Observer->Player = Player;
	Observer->bUnequipOnEquipped = true;
	Player->OnEquippedWeaponChanged.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleEquippedWeaponChanged);
	TestFalse(TEXT("Attach does not report success after an observer synchronously unequips"), Weapon->AttachWeapon(Player));
	TestFalse(TEXT("No half-equipped slot survives the callback"), Player->HasEquippedWeapon());
	TestNull(TEXT("No half-equipped attachment survives the callback"), Weapon->GetAttachParent());
	Player->OnEquippedWeaponChanged.RemoveDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleEquippedWeaponChanged);
	TestTrue(TEXT("A later complete equipment session succeeds"), Weapon->AttachWeapon(Player));
	TestEqual(TEXT("Re-equipping does not refill ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);

	Weapon->OnWeaponReloadEnded.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
	TestTrue(TEXT("Reload starts before player destruction"), Weapon->RequestReload());
	const int32 DestroyedOwnerReloadId = Weapon->GetActiveReloadId();
	AActor* PickupActor = Weapon->GetOwner();
	Player->Destroy();
	TestTrue(TEXT("The independently owned pickup Actor remains alive"), IsValid(PickupActor));
	TestFalse(TEXT("Player EndPlay closes the external weapon's reload"), Weapon->IsReloading());
	TestEqual(TEXT("Owner teardown reports Disabled"), Observer->LastReloadEndReason, EFPReloadEndReason::Disabled);
	TestNull(TEXT("Player EndPlay releases external attachment"), Weapon->GetAttachParent());
	Weapon->FinishReloadForTransaction(DestroyedOwnerReloadId);
	Weapon->StartFire();
	if (!World.Advance(5.2f)) return false;
	TestEqual(TEXT("Late completion and fallback cannot load the orphaned weapon"), Weapon->GetCurrentAmmo(), AmmoBefore);
	TestFalse(TEXT("An orphaned external weapon cannot keep firing"), Weapon->IsFiring());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueShotCooldownReentryTest, "fpstrue.Gameplay.Boundaries.Weapon.RemainingCooldownAndShotFact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueShotCooldownReentryTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	Weapon->StartFire();
	Weapon->StopFire();
	const int32 AmmoAfterShot = Weapon->GetCurrentAmmo();
	if (!World.Advance(0.07f)) return false;
	Weapon->StartFire();
	TestEqual(TEXT("A quick re-press respects the remaining cooldown"), Weapon->GetCurrentAmmo(), AmmoAfterShot);
	if (!World.Advance(0.05f)) return false;
	Weapon->StopFire();
	TestEqual(TEXT("The next shot uses the remainder, not a fresh full interval"), Weapon->GetCurrentAmmo(), AmmoAfterShot - 1);
	if (!World.Advance(0.2f)) return false;

	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Observer->Weapon = Weapon;
	Observer->bDisableWeaponOnAmmoChanged = true;
	Weapon->OnAmmoChanged.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);
	Weapon->OnWeaponFirePerformed.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleFirePerformed);
	Weapon->StartFire();
	TestEqual(TEXT("A committed shot still emits its fact event after a reentrant disable"), Observer->FireEventCount, 1);
	TestFalse(TEXT("The fact event does not restart the disabled action"), Weapon->IsFiring());
	const int32 AmmoAfterDisable = Weapon->GetCurrentAmmo();
	if (!World.Advance(0.3f)) return false;
	TestEqual(TEXT("No automatic-fire callback survives the disable"), Weapon->GetCurrentAmmo(), AmmoAfterDisable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadMontageIdentityTest, "fpstrue.Gameplay.Boundaries.Weapon.ReloadMontageInstanceIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueReloadMontageIdentityTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	USkeletalMeshComponent* Arms = CastChecked<USkeletalMeshComponent>(Weapon->GetAttachParent());
	// This fixture exercises engine Montage instance allocation and native notify routing, not pose/visual playback.
	// No project asset or internal MontageInstances/ActiveMontagesMap array is edited.
	UAnimInstance* Anim = NewObject<UAnimInstance>(Arms);
	Arms->AnimScriptInstance = Anim;
	Anim->InitializeAnimation();
	Anim->CurrentSkeleton = NewObject<USkeleton>(Anim);
	UAnimMontage* Montage = NewObject<UAnimMontage>(Anim);
	Montage->SetSkeleton(Anim->CurrentSkeleton);
	Montage->SetCompositeLength(1.0f);
	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Observer->ReloadAnimInstance = Anim;
	Observer->ReloadMontage = Montage;
	Observer->Weapon = Weapon;
	Weapon->OnWeaponReloadPlaybackRequested.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleReloadPlaybackRequested);
	TestTrue(TEXT("Reload A synchronously starts its Montage"), Weapon->RequestReload());
	FAnimMontageInstance* InstanceA = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("Engine allocated playback A"), InstanceA)) return false;
	const int32 PlaybackA = InstanceA->GetInstanceID();
	Weapon->CancelReload();
	TestTrue(TEXT("Reload B restarts the same Montage asset"), Weapon->RequestReload());
	FAnimMontageInstance* InstanceB = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("Engine allocated playback B"), InstanceB)) return false;
	const int32 PlaybackB = InstanceB->GetInstanceID();
	TestNotEqual(TEXT("The same asset gets a new playback identity"), PlaybackA, PlaybackB);
	// An unrelated Montage on the same mesh must not replace the explicit reload binding.
	UAnimMontage* Unrelated = NewObject<UAnimMontage>(Anim);
	Unrelated->SetSkeleton(Anim->CurrentSkeleton);
	Unrelated->SetCompositeLength(1.0f);
	Anim->Montage_Play(Unrelated, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false);
	FAnimMontageInstance* UnrelatedInstance = Anim->GetActiveInstanceForMontage(Unrelated);
	if (!TestNotNull(TEXT("The unrelated same-mesh playback exists"), UnrelatedInstance)) return false;
	TestFalse(TEXT("An unrelated playback cannot overwrite the reload's explicit binding"),
		Weapon->BindReloadMontage(Weapon->GetActiveReloadId(), Arms, Unrelated));
	FAnimNotifyEventReference UnrelatedNotify(nullptr, Unrelated);
	UnrelatedNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(UnrelatedInstance->GetInstanceID());
	TestFalse(TEXT("The unrelated playback cannot commit ammunition"), Weapon->CommitReloadFromNotify(Arms, UnrelatedNotify));
	TStrongObjectPtr<UfpstrueAnimNotify_ReloadCommit> Notify(NewObject<UfpstrueAnimNotify_ReloadCommit>());
	FAnimNotifyEventReference OldNotify(nullptr, Montage);
	OldNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(PlaybackA);
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	Notify->Notify(Arms, Montage, OldNotify);
	TestEqual(TEXT("A queued notify from A cannot load B"), Weapon->GetCurrentAmmo(), AmmoBefore);
	FBranchingPointNotifyPayload OldBranch(Arms, Montage, nullptr, PlaybackA);
	Notify->BranchingPointNotify(OldBranch);
	TestEqual(TEXT("A branching-point notify from A cannot load B either"), Weapon->GetCurrentAmmo(), AmmoBefore);
	FAnimNotifyEventReference CurrentNotify(nullptr, Montage);
	CurrentNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(PlaybackB);
	Notify->Notify(Arms, Montage, CurrentNotify);
	TestEqual(TEXT("The explicitly bound current playback commits ammunition"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	const int32 ReserveAfter = Weapon->GetReserveAmmo();
	FBranchingPointNotifyPayload CurrentBranch(Arms, Montage, nullptr, PlaybackB);
	Notify->BranchingPointNotify(CurrentBranch);
	TestEqual(TEXT("Duplicate queued/branching-point routes still commit once"), Weapon->GetReserveAmmo(), ReserveAfter);
	Weapon->FinishReloadForTransaction(Weapon->GetActiveReloadId());
	TestTrue(TEXT("Ending reload preserves unrelated playback on the same mesh"), UnrelatedInstance->IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadLegacyCallbackTest, "fpstrue.Gameplay.Boundaries.Weapon.LegacyCallbacksFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueReloadLegacyCallbackTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (!Weapon) return false;
	TestTrue(TEXT("Old reload starts"), Weapon->RequestReload());
	Weapon->CancelReload();
	TestTrue(TEXT("New reload starts before stale callbacks arrive"), Weapon->RequestReload());
	const int32 NewId = Weapon->GetActiveReloadId();
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	AddExpectedMessage(TEXT("Legacy identityless CommitReload rejected"), ELogVerbosity::Warning);
	AddExpectedMessage(TEXT("Legacy identityless FinishReload rejected"), ELogVerbosity::Warning);
	TestFalse(TEXT("Identityless legacy commit emits a diagnostic and is rejected"), Weapon->CommitReload());
	Weapon->FinishReload();
	TestEqual(TEXT("Identityless completion cannot end the new reload"), Weapon->GetActiveReloadId(), NewId);
	TestEqual(TEXT("Identityless commit cannot fill the new reload"), Weapon->GetCurrentAmmo(), AmmoBefore);
	Weapon->FinishReloadForTransaction(NewId);
	TestFalse(TEXT("The explicit current identity can complete normally"), Weapon->IsReloading());
	TestEqual(TEXT("Explicit completion preserves the legitimate ammo path"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadPlaybackFailureTest, "fpstrue.Gameplay.Boundaries.Weapon.PlaybackFailureAndEndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueReloadPlaybackFailureTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (!Weapon) return false;
	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Weapon->OnWeaponReloadEnded.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	TestTrue(TEXT("Reload accepts a presentation request"), Weapon->RequestReload());
	TestFalse(TEXT("Missing presentation asset fails immediately"), Weapon->PlayReloadMontage(Weapon->GetActiveReloadId(), Weapon, nullptr));
	TestFalse(TEXT("Playback failure releases the action lock"), Weapon->IsReloading());
	TestEqual(TEXT("Playback failure has an explicit reason"), Observer->LastReloadEndReason, EFPReloadEndReason::PlaybackFailed);
	TestFalse(TEXT("Playback failure never pretends to commit ammo"), Observer->bLastReloadCommitted);
	TestEqual(TEXT("Playback failure leaves ammunition unchanged"), Weapon->GetCurrentAmmo(), AmmoBefore);
	if (!World.Advance(5.2f)) return false;
	TestEqual(TEXT("No fallback succeeds after playback failure"), Observer->ReloadEndCount, 1);

	USkeletalMeshComponent* Arms = CastChecked<USkeletalMeshComponent>(Weapon->GetAttachParent());
	UAnimInstance* Anim = NewObject<UAnimInstance>(Arms);
	Arms->AnimScriptInstance = Anim;
	Anim->InitializeAnimation();
	Anim->CurrentSkeleton = NewObject<USkeleton>(Anim);
	UAnimMontage* Montage = NewObject<UAnimMontage>(Anim);
	Montage->SetSkeleton(Anim->CurrentSkeleton);
	Montage->SetCompositeLength(1.0f);
	TestTrue(TEXT("Reload can retry after playback failure"), Weapon->RequestReload());
	TestTrue(TEXT("Explicit playback starts"), Weapon->PlayReloadMontage(Weapon->GetActiveReloadId(), Arms, Montage));
	FAnimMontageInstance* Instance = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("Explicit playback has an instance"), Instance)) return false;
	Weapon->CancelReload();
	TestFalse(TEXT("Cancellation stops the exact owned playback"), Instance->IsActive());
	TestEqual(TEXT("Cancellation before notify does not load ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);
	TestTrue(TEXT("A subsequent reload gets a clean playback"), Weapon->RequestReload());
	TestTrue(TEXT("The same resource can play in the new transaction"), Weapon->PlayReloadMontage(Weapon->GetActiveReloadId(), Arms, Montage));
	FAnimMontageInstance* CompletedInstance = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("The new explicit playback has an instance"), CompletedInstance)) return false;
	// Drive the public engine instance timing; Advance executes its real Terminate/end-delegate path.
	for (int32 Index = 0; Index < 20 && Weapon->IsReloading(); ++Index)
	{
		CompletedInstance->UpdateWeight(0.1f);
		CompletedInstance->Advance(0.1f, nullptr, false);
	}
	TestFalse(TEXT("Native playback completion releases the action without Blueprint Finish"), Weapon->IsReloading());
	TestEqual(TEXT("A successful current playback reports Completed"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
	TestEqual(TEXT("Successful explicit playback fills ammunition"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponAssetReloadTest, "fpstrue.Gameplay.Boundaries.Weapon.AssetReloadNormalAndEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueWeaponAssetReloadTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UClass* WeaponClass = LoadClass<AActor>(nullptr, TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon.BP_Weapon_C"));
	USkeletalMesh* ArmsAsset = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/FPS_Assault_Pack/FPS_Arms/Meshes/Arms/SK_Custom_Arms.SK_Custom_Arms"));
	if (!TestNotNull(TEXT("Real weapon Blueprint class loads"), WeaponClass) || !TestNotNull(TEXT("Real arms skeleton loads"), ArmsAsset)) return false;
	AfpstrueCharacter* Player = World.SpawnPlayer();
	if (!Player) return false;
	USkeletalMeshComponent* Arms = Player->GetMesh1P();
	Arms->SetSkeletalMeshAsset(ArmsAsset);
	// This is a gameplay/asset-graph integration fixture, not a pose or visual acceptance test.
	// Native AnimInstance runs real Montage timing/notifies while unrelated locomotion AnimBP is outside scope.
	Arms->SetAnimInstanceClass(UAnimInstance::StaticClass());
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Pickup = World.GetWorld()->SpawnActor<AActor>(WeaponClass, FTransform(FVector(10000.0, 0.0, 0.0)), SpawnParameters);
	if (!TestNotNull(TEXT("Real weapon Blueprint actor spawns"), Pickup)) return false;
	Pickup->SetActorEnableCollision(false);
	UfpstrueWeaponComponent* Weapon = Pickup->FindComponentByClass<UfpstrueWeaponComponent>();
	if (!TestNotNull(TEXT("Real Blueprint contains its weapon component"), Weapon)) return false;
	const FObjectProperty* OwnerProperty = FindFProperty<FObjectProperty>(WeaponClass, TEXT("owningactor"));
	const FIntProperty* AmmoProperty = FindFProperty<FIntProperty>(Weapon->GetClass(), TEXT("CurrentAmmo"));
	if (!TestNotNull(TEXT("Blueprint presentation owner field exists"), OwnerProperty)
		|| !TestNotNull(TEXT("Ammo fixture precondition is reflected"), AmmoProperty)) return false;
	// Establish the same presentation context as pickup, without involving unrelated overlap/input/audio.
	OwnerProperty->SetObjectPropertyValue_InContainer(Pickup, Player);
	Weapon->SetAnimInstanceClass(UAnimInstance::StaticClass());
	if (!TestTrue(TEXT("Real weapon component equips through the production boundary"), Weapon->AttachWeapon(Player))) return false;
	for (USkeletalMeshComponent* Mesh : {Arms, static_cast<USkeletalMeshComponent*>(Weapon)})
	{
		Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Mesh->bEnableUpdateRateOptimizations = false;
		Mesh->SetComponentTickEnabled(true);
	}
	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Weapon->OnAmmoChanged.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleAmmoChanged);
	Weapon->OnWeaponReloadEnded.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
	for (const bool bEmpty : {false, true})
	{
		const FString Suffix = bEmpty ? TEXT("Reload_Empty_Montage") : TEXT("Reload_Montage");
		const FString ArmsPath = TEXT("/Game/FPS_Assault_Pack/Animations/RU74/A_Arms_RU74_") + Suffix;
		const FString WeaponPath = TEXT("/Game/FPS_Assault_Pack/Animations/RU74/A_RU74_") + Suffix;
		UAnimMontage* ArmsMontage = LoadObject<UAnimMontage>(nullptr, *ArmsPath);
		UAnimMontage* WeaponMontage = LoadObject<UAnimMontage>(nullptr, *WeaponPath);
		if (!TestNotNull(TEXT("Expected real arms reload Montage loads"), ArmsMontage)
			|| !TestNotNull(TEXT("Expected real weapon reload Montage loads"), WeaponMontage)) return false;
		AmmoProperty->SetPropertyValue_InContainer(Weapon, bEmpty ? 0 : Weapon->GetMagazineSize() - 1);
		const int32 TotalBefore = Weapon->GetCurrentAmmo() + Weapon->GetReserveAmmo();
		Observer->ReloadAnimInstance = Weapon->GetAnimInstance();
		Observer->ReloadMontage = WeaponMontage;
		Observer->bAmmoChangedWhileMontageActive = false;
		const int32 EndCountBefore = Observer->ReloadEndCount;
		if (!TestTrue(bEmpty ? TEXT("Real Blueprint empty-reload request accepted") : TEXT("Real Blueprint normal-reload request accepted"), Weapon->RequestReload())) return false;
		FAnimMontageInstance* ArmsPlayback = Arms->GetAnimInstance()->GetActiveInstanceForMontage(ArmsMontage);
		FAnimMontageInstance* WeaponPlayback = Weapon->GetAnimInstance()->GetActiveInstanceForMontage(WeaponMontage);
		if (!TestNotNull(TEXT("Blueprint selected and played the expected arms resource"), ArmsPlayback)
			|| !TestNotNull(TEXT("Blueprint selected and played the expected weapon resource"), WeaponPlayback)) return false;
		TestTrue(TEXT("The real arms playback has its native completion binding"), ArmsPlayback->OnMontageEnded.IsBound());
		TestTrue(TEXT("The real weapon playback has its native completion binding"), WeaponPlayback->OnMontageEnded.IsBound());
		const float Limit = FMath::Max(ArmsMontage->GetPlayLength(), WeaponMontage->GetPlayLength()) + 1.0f;
		for (int32 Step = 0; Step < FMath::CeilToInt(Limit / 0.01f) && Weapon->IsReloading(); ++Step)
			if (!World.AdvanceSingleFrame(0.01f)) return false;
		TestFalse(TEXT("Real Montage completion clears Reloading without legacy Blueprint callbacks"), Weapon->IsReloading());
		TestTrue(TEXT("The real asset Notify commits while its bound weapon Montage is active"), Observer->bAmmoChangedWhileMontageActive);
		TestEqual(TEXT("Real playback reports Completed, not timeout recovery"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
		TestEqual(TEXT("The paired playbacks emit exactly one transaction end"), Observer->ReloadEndCount, EndCountBefore + 1);
		TestEqual(TEXT("The real asset reload fills the magazine"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
		TestEqual(TEXT("Real asset reload conserves total ammunition"), Weapon->GetCurrentAmmo() + Weapon->GetReserveAmmo(), TotalBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueRecoilElapsedTimeTest, "fpstrue.Gameplay.Boundaries.Weapon.RecoilUsesElapsedTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueRecoilElapsedTimeTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	AfpstrueCharacter* Player = CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner());
	// A real PlayerController may set up Enhanced Input during possession; provide its required assets,
	// rather than suppressing errors from an intentionally incomplete native pawn fixture.
	for (const TCHAR* Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		const FObjectProperty* Property = FindFProperty<FObjectProperty>(Player->GetClass(), Name);
		if (!TestNotNull(TEXT("Required input configuration property exists"), Property)) return false;
		UInputAction* Action = NewObject<UInputAction>(Player);
		Action->ValueType = FCString::Strcmp(Name, TEXT("MoveAction")) == 0 || FCString::Strcmp(Name, TEXT("LookAction")) == 0
			? EInputActionValueType::Axis2D : EInputActionValueType::Boolean;
		Property->SetObjectPropertyValue_InContainer(Player, Action);
	}
	APlayerController* Controller = World.Spawn<APlayerController>();
	if (Controller == nullptr) return false;
	Controller->Possess(Player);
	Controller->SetActorTickEnabled(false);
	Player->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->RotationInput = FRotator::ZeroRotator;
	Weapon->StartFire();
	Weapon->StopFire();
	const double Kick = Controller->RotationInput.Pitch;
	if (!TestFalse(TEXT("The real PlayerController received recoil input"), FMath::IsNearlyZero(Kick))) return false;
	// Direct test calls occur outside World::Tick: arm TimerManager's pending timer at zero elapsed time
	// before simulating the hitch, otherwise that hitch is spent registering (not running) the timer.
	if (!World.AdvanceSingleFrame(0.0f)) return false;
	TestEqual(TEXT("Arming the pending timer does not consume recoil or advance game time"), Controller->RotationInput.Pitch, Kick);
	const double RecoveryStartTime = World.GetWorld()->GetTimeSeconds();
	// A 140ms hitch includes 120ms without recovery and 20ms of actual recovery.
	// At 10 degrees/s a 1-degree kick must retain 80%, regardless of timer catch-up callback count.
	if (!World.AdvanceSingleFrame(0.14f)) return false;
	TestEqual(TEXT("The hitch advances exactly 140ms of game time"), World.GetWorld()->GetTimeSeconds() - RecoveryStartTime, 0.14, 0.0001);
	TestEqual(TEXT("Recovery integrates elapsed time, excluding the configured delay"), Controller->RotationInput.Pitch / Kick, 0.8, 0.025);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueOwnerInputTransitionTest, "fpstrue.Gameplay.Boundaries.Weapon.OwnerInputTransitionRejectsReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueOwnerInputTransitionTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	AfpstrueCharacter* Player = CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner());
	TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
	Observer->Weapon = Weapon;
	Observer->bRestartReloadOnEnd = true;
	Observer->bFireOnReloadEnd = true;
	Weapon->OnWeaponReloadEnded.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleReloadEnded);
	TestTrue(TEXT("A reload is active before owner input is interrupted"), Weapon->RequestReload());
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	Weapon->InterruptOwnerInput();
	TestFalse(TEXT("Cleanup callback cannot start a new reload"), Observer->bReloadRestartAccepted);
	TestFalse(TEXT("Cleanup leaves no reentrant reload"), Weapon->IsReloading());
	TestFalse(TEXT("Cleanup callback cannot start firing"), Weapon->IsFiring());
	TestEqual(TEXT("Cleanup callbacks cannot spend ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);
	if (!World.Advance(5.2f)) return false;
	TestEqual(TEXT("No reentrant fallback timer survives cleanup"), Weapon->GetCurrentAmmo(), AmmoBefore);

	// Ordinary cancellation remains a normal gameplay transition: an ended listener may start the next action.
	Observer->bFireOnReloadEnd = false;
	TestTrue(TEXT("Temporary input guard does not disable equipment"), Weapon->RequestReload());
	const int32 PreviousReload = Weapon->GetActiveReloadId();
	Weapon->CancelReload();
	TestTrue(TEXT("Normal reload-end continuation remains permitted"), Observer->bReloadRestartAccepted);
	TestNotEqual(TEXT("The normal continuation has its own identity"), Weapon->GetActiveReloadId(), PreviousReload);

	Observer->bFireOnReloadEnd = true;
	Player->GetController()->UnPossess();
	TestFalse(TEXT("Actual UnPossess rejects reload reentry during controller migration"), Observer->bReloadRestartAccepted);
	TestFalse(TEXT("UnPossess leaves neither reload nor firing active"), Weapon->IsReloading() || Weapon->IsFiring());
	TestEqual(TEXT("UnPossess callback did not fire a shot"), Weapon->GetCurrentAmmo(), AmmoBefore);
	Observer->bRestartReloadOnEnd = false;
	Observer->bFireOnReloadEnd = false;
	AAIController* NewController = World.Spawn<AAIController>();
	if (NewController == nullptr) return false;
	NewController->Possess(Player);
	TestTrue(TEXT("A new controller can start a legitimate reload after migration"), Weapon->RequestReload());
	Weapon->CancelReload();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
