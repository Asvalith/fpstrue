// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Automation/fpstrueReloadReentryObserver.h"
#include "Weapons/fpstrueWeaponComponent.h"

void UfpstrueReloadReentryObserver::HandleAmmoChanged(int32 CurrentAmmo, int32 MagazineSize, int32 ReserveAmmo)
{
	++CallbackCount;
	if (bDisableWeaponOnAmmoChanged)
	{
		if (UfpstrueWeaponComponent* ObservedWeapon = Weapon.Get())
		{
			ObservedWeapon->HandleOwnerDeath();
		}
	}
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Shared/fpstrueCollisionChannels.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "AIController.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
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
		if (!Wrapper.BeginPlayInTestWorld())
		{
			Wrapper.ForwardErrorMessages(&Test);
			return false;
		}
		return true;
	}

	UWorld* GetWorld() const { return Wrapper.GetTestWorld(); }

	bool Advance(float DurationSeconds)
	{
		// Small explicit steps advance real TimerManager callbacks, including the
		// controller's randomized first decision, without a latent editor session.
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

	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(Player, NAME_None, RF_Transient);
	Player->AddInstanceComponent(Weapon);
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

	// Exercise the product's inclusive tolerance through the same timer-driven decision.
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
	Facing.Enemy->EndAttackWindow();
	Facing.Enemy->UpdateAttackWindow();
	Facing.Enemy->EndAttackWindow();
	TestTrue(TEXT("Closing the damage window leaves the attack transaction active"), Facing.Enemy->IsAttacking());
	Facing.Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>()->ResetCombat();
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
	const UCameraComponent* Camera = CastChecked<AfpstrueCharacter>(Weapon->GetOwner())->GetFirstPersonCameraComponent();
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
	TestTrue(TEXT("The first commit is accepted"), Weapon->CommitReload());
	TestTrue(TEXT("Commit leaves the transaction Reloading until Finish"), Weapon->IsReloading());
	TestFalse(TEXT("A duplicate commit is rejected"), Weapon->CommitReload());
	Weapon->FinishReload();
	Weapon->FinishReload();
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
	Weapon->FinishReload();
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

#endif // WITH_DEV_AUTOMATION_TESTS
