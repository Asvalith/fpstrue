// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Automation/fpstrueTestObservers.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "UObject/UnrealType.h"

// 读取实际调用生成 RPC 包装函数时传入的参数；不依赖私有成员或手工构造的 RPC 参数布局。
void UfpstrueShotPresentationTestWeapon::ProcessEvent(UFunction* Function, void* Parameters)
{
	if (Function != nullptr && Parameters != nullptr && Function->GetFName() == FName(TEXT("MulticastShot")))
	{
		const FStructProperty* Hit = FindFProperty<FStructProperty>(Function, TEXT("Hit"));
		const FIntProperty* Shot = FindFProperty<FIntProperty>(Function, TEXT("ShotIndex"));
		const FUInt32Property* Generation = FindFProperty<FUInt32Property>(Function, TEXT("EquipmentGeneration"));
		const FUInt32Property* Session = FindFProperty<FUInt32Property>(Function, TEXT("Session"));
		if (Hit != nullptr && Hit->Struct == FHitResult::StaticStruct() && Shot != nullptr && Generation != nullptr && Session != nullptr)
			ShotPresentations.Add({*Hit->ContainerPtrToValuePtr<FHitResult>(Parameters), Shot->GetPropertyValue_InContainer(Parameters),
				Generation->GetPropertyValue_InContainer(Parameters), Session->GetPropertyValue_InContainer(Parameters)});
	}
	Super::ProcessEvent(Function, Parameters);
}

// 本文件检查输入、装备、射击和换弹的业务边界；资产集成用例也不替代完整 AnimBP 姿态或 GPU 验收。
// ★【重要边界】[验收边界] 真实 Montage 的身份/通知测试不等于整套 AnimBP 姿态或画面验收，NullRHI 也不能证明 GPU 收益。
// 记录弹药通知时的真实播放状态，并可同步禁用武器，模拟通知监听者改变动作状态。
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

// 在装备成功通知中立即卸下武器，验证外层 Attach 返回前会再次核对装备关系。
void UfpstrueReloadReentryObserver::HandleEquippedWeaponChanged(UfpstrueWeaponComponent* EquippedWeapon)
{
	if (bUnequipOnEquipped && EquippedWeapon != nullptr && Player.IsValid())
	{
		Player->ClearEquippedWeaponComponent(EquippedWeapon);
	}
}

// 统计已经提交的单发事实；即使后续监听者取消动作，这个事实也不应被吞掉。
void UfpstrueReloadReentryObserver::HandleFirePerformed()
{
	++FireEventCount;
}

// 不伪造弹道事件；由武器真实广播进入后，执行测试指定的一次同步重入。
void UfpstrueReloadReentryObserver::HandleTraceFinished(bool, FVector, FVector, FVector, FHitResult)
{
	if (OnTraceReceived) OnTraceReceived();
}

// 保存换弹结束结果，并可立即请求新动作，检查旧事务清理不能覆盖回调中新建的状态。
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

// 响应生产播放命令，用该命令携带的换弹编号播放和绑定测试动画，不临时读取另一次编号。
void UfpstrueReloadReentryObserver::HandleReloadPlaybackRequested(int32 ReloadId, bool bWasEmptyReload)
{
	if (Weapon.IsValid() && ReloadAnimInstance.IsValid() && ReloadMontage.IsValid())
	{
		Weapon->PlayReloadMontage(ReloadId, ReloadAnimInstance->GetSkelMeshComponent(), ReloadMontage.Get());
	}
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Game/fpstrueMultiplayer.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "AIController.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/Skeleton.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraModifier_CameraShake.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTask.h"
#include "InputAction.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "ReferenceSkeleton.h"
#include "Tests/AutomationCommon.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include <limits>

namespace FpstrueGameplayBoundaryTests
{
// 通过真实感知组件建立可见目标，避免朝向测试绕过当前 AI 的感知门禁。
bool ReportVisibleEnemyTarget(AfpstrueEnemyAIController* Controller, AfpstrueCharacter* Target, FAutomationTestBase& Test)
{
	UAIPerceptionComponent* Perception = Controller->GetPerceptionComponent();
	if (!Test.TestNotNull(TEXT("Facing fixture has the real sight listener"), Perception)) return false;
	FAIStimulus Stimulus(*GetDefault<UAISense_Sight>(), 1.0f, Target->GetActorLocation(),
		Controller->GetPawn()->GetActorLocation(), FAIStimulus::SensingSucceeded);
	Stimulus.SetWantsNotifyOnlyOnValueChange(false);
	Perception->RegisterStimulus(Target, Stimulus);
	Perception->ProcessStimuli();
	return true;
}

// FTestWorldWrapper is the engine's own temporary-world fixture. Its destructor
// ends play, removes its world context, destroys its world and restores GFrameCounter.
// No editor map, project GameMode, blueprint, asset file or global CVar is modified.
class FGameplayWorld
{
public:
	// 保留自动化断言接收者，由夹具负责测试世界的创建、推进和退出。
	explicit FGameplayWorld(FAutomationTestBase& InTest) : Test(InTest) {}

	// 只有 AI 场景使用真实多人资格；其余武器边界仍隔离项目玩法。
	bool Initialize(UClass* GameModeClass = AGameModeBase::StaticClass())
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
		GetWorld()->GetWorldSettings()->DefaultGameMode = GameModeClass;
		// FTestWorldWrapper 默认不创建 AI 系统；真实行为树/Blackboard 需要它。
		GetWorld()->CreateAISystem();
		if (!Wrapper.BeginPlayInTestWorld())
		{
			Wrapper.ForwardErrorMessages(&Test);
			return false;
		}
		if (AfpstruePvpGameMode* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>())
		{
			GetWorld()->GetTimerManager().ClearAllTimersForObject(Mode);
			if (AfpstruePvpGameState* State = GetWorld()->GetGameState<AfpstruePvpGameState>())
			{
				if (auto* Coordinator = State->FindComponentByClass<UfpstrueEnemySignificanceCoordinator>()) Coordinator->Stop();
				if (auto* Sharing = State->FindComponentByClass<UfpstrueEnemyAnimationSharingCoordinator>()) Sharing->Stop();
			}
		}
		return true;
	}

	// 只返回当前夹具拥有的世界，避免借用编辑器已有地图状态。
	UWorld* GetWorld() const { return Wrapper.GetTestWorld(); }
	// 按给定经过时间推进一帧，用于计时器积压和卡顿场景。
	bool AdvanceSingleFrame(float DeltaSeconds) { return Wrapper.TickTestWorld(DeltaSeconds); }

	// 按小步推进真实世界计时器和行为树，保证事件顺序而不伪造回调结果。
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

	// 生成原生 Actor 并记录创建失败；AlwaysSpawn 只隔离本夹具无关的出生碰撞。
	template <typename TActor>
	TActor* Spawn(const FTransform& Transform = FTransform::Identity)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		TActor* Actor = GetWorld()->SpawnActor<TActor>(TActor::StaticClass(), Transform, SpawnParameters);
		Test.TestNotNull(TEXT("A native test actor was spawned"), Actor);
		return Actor;
	}

	// 创建玩家并关闭自动移动/碰撞，后续测试显式控制位置或输入前态。
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

// 为站定转身测试保存真实 Enemy/Controller/Movement，复用装配而不复制角度判定实现。
struct FEnemyFacingFixture
{
	AfpstrueEnemyCharacter* Enemy = nullptr;
	AfpstrueEnemyAIController* Controller = nullptr;
	UCharacterMovementComponent* Movement = nullptr;

	// 生成敌人与可见玩家、建立 Controller 上下文，然后固定地面模式与初始朝向。
	bool Initialize(FGameplayWorld& Fixture, FAutomationTestBase& Test, float ActorYaw, const FVector& TargetLocation)
	{
		AfpstrueCharacter* Target = Fixture.SpawnPlayer(TargetLocation);
		if (Target == nullptr)
		{
			return false;
		}
		for (const FName Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
			TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
		{
			FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Target->GetClass(), Name);
			if (!Test.TestNotNull(TEXT("Facing target has the required input property"), Property)) return false;
			Property->SetObjectPropertyValue_InContainer(Target, NewObject<UInputAction>(Target));
		}
		APlayerController* PlayerController = Fixture.Spawn<APlayerController>();
		if (PlayerController == nullptr) return false;
		PlayerController->Possess(Target);
		AfpstruePvpGameMode* Mode = Fixture.GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>();
		if (!Test.TestNotNull(TEXT("Facing fixture uses the gameplay mode"), Mode) ||
			!Test.TestTrue(TEXT("Facing target is a real combat participant"), Mode->IsCombatParticipant(Target))) return false;

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
		if (!ReportVisibleEnemyTarget(Controller, Target, Test)) return false;
		// Possess restarts Character movement; set the ground-mode fixture afterwards.
		Movement->SetMovementMode(MOVE_Walking);
		return Test.TestTrue(TEXT("The native target is in attack range"), Enemy->IsTargetInAttackRange());
	}
};

// 经真实装备和开火入口准备少一发子弹的武器；临时骨骼只提供挂接前提，不模拟换弹结果。
UfpstrueWeaponComponent* EquipWeaponWithOneSpentRound(FGameplayWorld& Fixture, FAutomationTestBase& Test,
	UfpstrueWeaponConfig* Configuration = nullptr, UClass* WeaponClass = UfpstrueWeaponComponent::StaticClass())
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
	// 同一最小骨架同时属于 Mesh 与测试 Montage，不能让 AnimInstance 单独持有一套不相干的 Skeleton。
	// 此夹具仍不启用物理或姿态求值；真实项目动画/PhysicsAsset 的完整性另由资产用例检查。
	USkeleton* AttachmentSkeleton = NewObject<USkeleton>(AttachmentMesh, NAME_None, RF_Transient);
	AttachmentMesh->SetSkeleton(AttachmentSkeleton);
	if (!Test.TestTrue(TEXT("The attachment mesh has a matching real skeleton"),
		AttachmentSkeleton->MergeAllBonesToBoneTree(AttachmentMesh, false))) return nullptr;
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
	UfpstrueWeaponComponent* Weapon = NewObject<UfpstrueWeaponComponent>(PickupActor, WeaponClass, NAME_None, RF_Transient);
	if (Configuration != nullptr)
	{
		const FObjectProperty* Property = FindFProperty<FObjectProperty>(Weapon->GetClass(), TEXT("WeaponConfiguration"));
		if (!Test.TestNotNull(TEXT("The weapon has one reflected configuration binding"), Property)) return nullptr;
		Property->SetObjectPropertyValue_InContainer(Weapon, Configuration);
	}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponConfigValidationTest, "fpstrue.Gameplay.Boundaries.Weapon.ConfigurationValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查整份武器配置校验能拒绝无效数量、空插槽和非有限值，同时允许明确禁用视觉效果。
bool FFpstrueWeaponConfigValidationTest::RunTest(const FString& Parameters)
{
	FString Error;
	FFPWeaponSettings Settings;
	TestTrue(TEXT("Native defaults are a complete valid configuration"), Settings.Validate(Error));
	// 无效配置必须整体拒绝并说明原因，不能靠静默裁剪或混用默认字段修补。
	const auto Reject = [this, &Error](FFPWeaponSettings Invalid)
	{
		TestFalse(TEXT("Invalid tuning is rejected without clamping or mixed fallback"), Invalid.Validate(Error));
		TestFalse(TEXT("Invalid tuning explains why"), Error.IsEmpty());
	};
	Settings.MagazineSize = 0; Reject(Settings);
	Settings = {}; Settings.StartingReserveAmmo = -1; Reject(Settings);
	Settings = {}; Settings.GripSocketName = NAME_None; Reject(Settings);
	Settings = {}; Settings.RoundsPerMinute = 0.0f; Reject(Settings);
	Settings = {}; Settings.LineTraceDamage = std::numeric_limits<float>::quiet_NaN(); Reject(Settings);
	Settings = {}; Settings.RecoilRecoverySpeed = std::numeric_limits<float>::infinity(); Reject(Settings);
	Settings = {}; Settings.HipFireSpreadAngle = 46.0f; Reject(Settings);
	Settings = {}; Settings.AimRecoilMultiplier = 1.1f; Reject(Settings);
	Settings = {}; Settings.ReloadDuration = -1.0f; Reject(Settings);
	Settings = {}; Settings.CriticalHitBones.Add(NAME_None); Reject(Settings);
	Settings = {}; Settings.EmptyReloadDuration = std::numeric_limits<float>::infinity(); Reject(Settings);
	Settings = {}; Settings.CriticalHitBones.Reset();
	TestTrue(TEXT("Weapons without critical-hit bones are allowed"), Settings.Validate(Error));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilDuration = 0.0f;
	TestFalse(TEXT("A zero visual recoil duration is rejected before playback"), Settings.Validate(Error));
	TestTrue(TEXT("Invalid duration identifies its setting"), Error.Contains(TEXT("VisualRecoilDuration")));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilScale = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("A non-finite visual scale is rejected"), Settings.Validate(Error));
	TestTrue(TEXT("Invalid scale identifies its setting"), Error.Contains(TEXT("VisualRecoilScale")));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilRotation.Pitch = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("A non-finite visual rotation is rejected"), Settings.Validate(Error));
	TestTrue(TEXT("Invalid rotation identifies its setting"), Error.Contains(TEXT("VisualRecoilRotation")));
	Settings = FFPWeaponSettings();
	Settings.VisualRecoilScale = 0.0f;
	TestTrue(TEXT("Zero visual scale remains a valid way to disable the effect"), Settings.Validate(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponConfigSnapshotTest, "fpstrue.Gameplay.Boundaries.Weapon.ConfigurationSnapshotAndReequip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证首次装备复制配置快照，重装备不补弹/热改参数，非法配置在改动装备关系前整体失败。
bool FFpstrueWeaponConfigSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	TStrongObjectPtr<UfpstrueWeaponConfig> Config(NewObject<UfpstrueWeaponConfig>());
	Config->Settings.MagazineSize = 5;
	Config->Settings.StartingReserveAmmo = 10;
	Config->Settings.LineTraceDamage = 17.0f;
	Config->Settings.ReloadDuration = 0.2f;
	Config->Settings.EmptyReloadDuration = 0.3f;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this, Config.Get());
	if (Weapon == nullptr) return false;
	TestEqual(TEXT("Asset controls initial magazine"), Weapon->GetCurrentAmmo(), 4);
	TestEqual(TEXT("Asset controls initial reserve"), Weapon->GetReserveAmmo(), 10);
	// 改资产后旧实例保持快照，新实例才读新值；实例弹药彼此独立。
	Config->Settings.MagazineSize = 12;
	Config->Settings.LineTraceDamage = 77.0f;
	Config->Settings.CriticalHitBones.Add(TEXT("custom_bone"));
	TestEqual(TEXT("Running weapon holds its original capacity"), Weapon->GetMagazineSize(), 5);
	TestEqual(TEXT("Running weapon holds its original damage"), Weapon->GetWeaponSettings().LineTraceDamage, 17.0f);
	TestEqual(TEXT("Bone array is independently copied"), Weapon->GetWeaponSettings().CriticalHitBones.Num(), 2);
	UfpstrueWeaponComponent* Other = EquipWeaponWithOneSpentRound(World, *this, Config.Get());
	if (Other == nullptr) return false;
	TestEqual(TEXT("A newly equipped weapon sees updated tuning"), Other->GetMagazineSize(), 12);
	TestEqual(TEXT("A second instance does not consume first instance ammo"), Weapon->GetCurrentAmmo(), 4);
	// 权威时钟采用首次装备快照；动画不参与装填，重装备也不会重新初始化弹药。
	TestTrue(TEXT("Configured reload starts"), Weapon->RequestReload());
	if (!World.Advance(0.1f)) return false;
	TestTrue(TEXT("The configured authority clock does not finish early"), Weapon->IsReloading());
	if (!World.Advance(0.15f)) return false;
	TestFalse(TEXT("The configured authority clock releases the action"), Weapon->IsReloading());
	TestEqual(TEXT("Authority timing fills the snapshotted magazine"), Weapon->GetCurrentAmmo(), 5);
	TestEqual(TEXT("Authority timing consumes the missing round once"), Weapon->GetReserveAmmo(), 9);
	AfpstrueCharacter* Player = Cast<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner());
	Weapon->DetachWeapon();
	if (!TestTrue(TEXT("The same weapon re-equips"), Weapon->AttachWeapon(Player))) return false;
	TestEqual(TEXT("Re-equipping preserves the already completed magazine"), Weapon->GetCurrentAmmo(), 5);
	TestEqual(TEXT("Re-equipping cannot replenish the consumed reserve"), Weapon->GetReserveAmmo(), 9);
	TestEqual(TEXT("Re-equipping does not replace its configuration snapshot"), Weapon->GetMagazineSize(), 5);
	Weapon->DetachWeapon();
	// A fresh instance must fail before changing the player/equipment relationship.
	AActor* Pickup = World.Spawn<AActor>();
	if (Pickup == nullptr) return false;
	UfpstrueWeaponComponent* InvalidWeapon = NewObject<UfpstrueWeaponComponent>(Pickup);
	Pickup->AddInstanceComponent(InvalidWeapon);
	InvalidWeapon->RegisterComponent();
	const FObjectProperty* Binding = FindFProperty<FObjectProperty>(InvalidWeapon->GetClass(), TEXT("WeaponConfiguration"));
	if (!TestNotNull(TEXT("Configuration binding exists"), Binding)) return false;
	Binding->SetObjectPropertyValue_InContainer(InvalidWeapon, Config.Get());
	Config->Settings.MagazineSize = 0;
	AddExpectedError(TEXT("Weapon configuration rejected"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Invalid config refuses equip"), InvalidWeapon->AttachWeapon(Player));
	TestFalse(TEXT("Failed config cannot leave a half-equipped player"), Player->HasEquippedWeapon());
	TestEqual(TEXT("Failed config cannot initialize ammunition"), InvalidWeapon->GetCurrentAmmo(), 0);
	Config->Settings.MagazineSize = 7;
	TestTrue(TEXT("A corrected config can retry equip before initialization"), InvalidWeapon->AttachWeapon(Player));
	TestEqual(TEXT("Retry uses the corrected config as a whole"), InvalidWeapon->GetCurrentAmmo(), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueFacingToleranceTest, "fpstrue.Gameplay.Boundaries.AI.FacingTolerance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 通过真实行为树验证站定清除残余移动、15 度含边界的攻击容差和跨 +/-180 度的角度差。
bool FFpstrueFacingToleranceTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	FEnemyFacingFixture Facing;
	if (!World.Initialize(AfpstruePvpGameMode::StaticClass()) || !Facing.Initialize(World, *this, 16.0f, FVector(200.0f, 0.0f, 0.0f)))
	{
		return false;
	}
	// 只固定无 NavMesh 临时世界的导航输入；席位身份仍经真实 Manager 授予。
	const auto PrepareLease = [this](FEnemyFacingFixture& Fixture)
	{
		AfpstrueSurroundManager* Manager = Fixture.Controller->GetSurroundManager();
		if (!TestNotNull(TEXT("Perceived player owns a real manager"), Manager)) return false;
		Manager->GetWorldTimerManager().ClearAllTimersForObject(Manager);
		Manager->ApproachQueryId = INDEX_NONE;
		++Manager->PositionQueryRevision;
		Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
		Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
		Manager->SurroundSlots[0].ProjectedApproachLocation = Fixture.Enemy->GetActorLocation();
		Fixture.Controller->RefreshBehaviorDecision(*Fixture.Controller->GetBlackboardComponent());
		return TestTrue(TEXT("Facing enemy obtains a real inner lease"), Manager->HasInnerSlot(Fixture.Enemy,
			Fixture.Controller->GetTacticalAssignmentRevision()));
	};
	if (!PrepareLease(Facing)) return false;

	// Simulate residual ground motion while PathFollowing is already idle.
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
	if (!WrappedFacing.Initialize(World, *this, 179.0f, TargetLocation) || !PrepareLease(WrappedFacing) || !World.Advance(0.35f))
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
	Combat->HandleAttackFinishedNotify(Facing.Enemy->GetMesh(), FAnimNotifyEventReference());
	TestFalse(TEXT("A late finish notification cannot restart a reset attack"), Facing.Enemy->IsAttacking());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRotationConfigTest, "fpstrue.Gameplay.Boundaries.AI.RotationConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证蓝图在 BeginPlay 前配置的转身速度被采用，非法负值只使用生产安全下限。
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

// 只验证 INI 到原生默认对象的动画软引用配置，不加载动画或宣称共享动画表现已通过。
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

// 经单发射线与真实生命组件检查命中扣血一次、未命中仍扣弹且不额外伤害。
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

	// 关闭靶子阻挡后再次发射：只改变命中条件，不改变武器与弹药逻辑。
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

// 覆盖换弹打断连射、提交前/后取消的不同弹药结果，以及卸下武器后的迟到通知和保护计时器。
bool FFpstrueReloadTransitionsTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;

	// 连射进入换弹后旧开火 Timer 不得继续扣弹，重复请求也不能解除换弹锁。
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

	// 提交前取消不补弹；提交后取消保留已经转移的弹药，总量必须守恒。
	TestTrue(TEXT("Reload can restart after cancellation"), Weapon->RequestReload());
	TestTrue(TEXT("The new reload commits once"), Weapon->CommitReloadForTransaction(Weapon->GetActiveReloadId()));
	Weapon->CancelReload();
	TestEqual(TEXT("Cancellation after commit keeps loaded ammunition"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	TestEqual(TEXT("Reload conserves total ammunition"), Weapon->GetCurrentAmmo() + Weapon->GetReserveAmmo(), AmmoBefore + ReserveBefore);

	Weapon->StartFire();
	Weapon->StopFire();
	TestTrue(TEXT("Begin reload before unequipping"), Weapon->RequestReload());
	// 保存被卸下时的旧编号，之后结束/取消/超时都不能继续装弹或开火。
	const int32 UnequippedReloadId = Weapon->GetActiveReloadId();
	const int32 AmmoBeforeUnequip = Weapon->GetCurrentAmmo();
	CastChecked<AfpstrueCharacter>(Weapon->GetAttachParent()->GetOwner())->ClearEquippedWeaponComponent(Weapon);
	Weapon->FinishReloadForTransaction(UnequippedReloadId);
	Weapon->CancelReload();
	Weapon->StartFire();
	if (!World.Advance(5.2f)) return false;
	TestFalse(TEXT("Unequipped weapon remains disabled"), Weapon->IsFiring());
	TestFalse(TEXT("Unequipping cancels reload"), Weapon->IsReloading());
	TestFalse(TEXT("Unequipped weapon cannot reload"), Weapon->RequestReload());
	TestEqual(TEXT("Late callbacks and timeout cannot load an unequipped weapon"), Weapon->GetCurrentAmmo(), AmmoBeforeUnequip);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueAutoReloadConflictTest, "fpstrue.Gameplay.Boundaries.Weapon.AutoReloadConflicts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证真实连射耗尽弹匣后自动换弹，并通过同一角色速度策略退出瞄准与冲刺。
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
	UAbilitySystemComponent* AbilitySystem = Player->GetAbilitySystemComponent();
	if (!TestNotNull(TEXT("Movement conflicts use the player's real ASC"), AbilitySystem)) return false;
	// 只设置输入前态；耗尽弹匣、换弹和状态收尾均运行真实代码。
	// 前态现在也由真实 GA 建立，bIsAiming/bIsSprinting 只作为标签派生的表现镜像读取。
	for (const int32 MovementInput : {static_cast<int32>(EFPCombatAbilityInput::Aim), static_cast<int32>(EFPCombatAbilityInput::Sprint)})
	{
		AbilitySystem->AbilityLocalInputPressed(MovementInput);
		if (!TestTrue(TEXT("The actual movement ability establishes the initial state"), Player->IsAiming() || Sprinting->GetPropertyValue_InContainer(Player))) return false;
		Weapon->StartFire();
		TestTrue(TEXT("Fire preserves the existing aim or sprint ability until reload"), Player->IsAiming() || Sprinting->GetPropertyValue_InContainer(Player));
		const int32 MaxSteps = FMath::CeilToInt((Weapon->GetCurrentAmmo() * 60.0f / Weapon->GetWeaponSettings().RoundsPerMinute + 1.0f) / 0.01f);
		// 在第一次观测到空仓的同一帧验收能力切换，不多推进一帧来掩盖延迟或额外请求往返。
		for (int32 Step = 0; Step < MaxSteps && Weapon->GetCurrentAmmo() > 0; ++Step)
		{
			if (!World.Advance(0.01f)) return false;
		}
		TestEqual(TEXT("The real held fire empties the magazine"), Weapon->GetCurrentAmmo(), 0);
		TestTrue(TEXT("Empty magazine starts automatic reload"), Weapon->IsReloading());
		TestTrue(TEXT("Automatic reload activates the authoritative reload ability"),
			AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading));
		TestFalse(TEXT("Automatic reload releases the firing ability tag"),
			AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing));
		TestFalse(TEXT("Automatic reload clears aiming"), Player->IsAiming());
		TestFalse(TEXT("Automatic reload clears sprinting"), Sprinting->GetPropertyValue_InContainer(Player));
		TestEqual(TEXT("Reload restores the unique walk-speed policy"), Player->GetCharacterMovement()->MaxWalkSpeed, 300.0f);
		Weapon->FinishReloadForTransaction(Weapon->GetActiveReloadId());
		AbilitySystem->AbilityLocalInputReleased(MovementInput);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadFinishReentryTest,
	"fpstrue.Gameplay.Boundaries.Weapon.ReloadFinishPreservesDisabledOnReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证装弹只提交一次，且弹药广播中禁用武器后，旧 Finish 返回不能重新写回 Ready。
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

	// 先完成普通换弹，再用真实射击准备重入场景，避免未提交弹药掩盖错误。
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

// 检查换弹 A/B 编号隔离与结束原因，拒绝无绑定动画通知，区分完成与超时解锁。
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

	TestNull(TEXT("Blueprint cannot submit ammunition through a second authority entry"),
		Weapon->GetClass()->FindFunctionByName(TEXT("CommitReloadForTransaction")));
	TestNull(TEXT("Blueprint cannot finish gameplay from a visual completion"),
		Weapon->GetClass()->FindFunctionByName(TEXT("FinishReloadForTransaction")));
	TestEqual(TEXT("Rejected old callbacks preserve the magazine"), Weapon->GetCurrentAmmo(), AmmoBefore);
	Weapon->FinishReloadForTransaction(ReloadB);
	TestEqual(TEXT("A current finish reports Completed"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
	TestTrue(TEXT("Completed reports the actual ammo commit"), Observer->bLastReloadCommitted);
	TestEqual(TEXT("Only A cancellation and B completion were reported"), Observer->ReloadEndCount, 2);

	Weapon->StartFire();
	Weapon->StopFire();
	// 没有动画也使用唯一权威时钟，预览不能保留另一套 Notify 装弹规则。
	TestTrue(TEXT("A presentation-free reload starts"), Weapon->RequestReload());
	const int32 ReserveBeforeTimer = Weapon->GetReserveAmmo();
	if (!World.Advance(Weapon->GetReloadDuration() + 0.1f)) return false;
	TestFalse(TEXT("The authority timer closes the reload"), Weapon->IsReloading());
	TestEqual(TEXT("The authority timer reports Completed"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
	TestTrue(TEXT("A timer completion reports its single commit"), Observer->bLastReloadCommitted);
	TestEqual(TEXT("The timer fills the magazine"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	TestEqual(TEXT("The timer consumes exactly the missing round"), Weapon->GetReserveAmmo(), ReserveBeforeTimer - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueExternalEquipmentLifetimeTest, "fpstrue.Gameplay.Boundaries.Weapon.ExternalOwnerAndEquipReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查武器仍归拾取 Actor 所有时的装备重入与玩家退出清理，防止外部武器成为活动孤儿。
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
	UAbilitySystemComponent* AbilitySystem = Player->GetAbilitySystemComponent();
	if (!TestNotNull(TEXT("Equipment lifetime uses the actual ASC"), AbilitySystem)) return false;
	AbilitySystem->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Aim));
	if (!TestTrue(TEXT("The actual aim ability is active before unequipping"),
		Player->IsAiming() && AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming))) return false;
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	Player->ClearEquippedWeaponComponent(Weapon);
	TestFalse(TEXT("Unequip exits ADS"), Player->IsAiming());
	TestFalse(TEXT("Unequip cancels the ability rather than only clearing its mirror"), AbilitySystem->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming));
	TestEqual(TEXT("Unequip restores walk speed"), Player->GetCharacterMovement()->MaxWalkSpeed, 300.0f);
	TestNull(TEXT("Unequip removes physical attachment"), Weapon->GetAttachParent());
	TestFalse(TEXT("Unequip clears the character slot"), Player->HasEquippedWeapon());

	// 装备广播中同步卸下，Attach 必须报告失败并且不保留半完成关系。
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
	// 玩家销毁不会自动销毁独立拾取 Actor；角色 EndPlay 必须主动关闭并脱离外部武器。
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

// 检查快速松开再按使用剩余射速间隔，且已扣弹的一枪即使被回调禁用也会发布事实。
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
	// 在弹药通知内禁用武器，随后既要有一次已发射事件，又不能残留后续连射回调。
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueShotPresentationReentryTest,
	"fpstrue.Gameplay.Boundaries.Weapon.ShotPresentationIdentityOnReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 命中伤害与弹道通知分别重启同一个真实 GA；旧单发 RPC 必须保存旧身份，新轮从第一发重新编号。
// 这里验证服务器结算和生成 RPC 的分派实参，不替代多进程传输、丢包或客户端画面验收。
bool FFpstrueShotPresentationReentryTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	const UFunction* ShotRPC = UfpstrueWeaponComponent::StaticClass()->FindFunctionByName(TEXT("MulticastShot"));
	if (!TestNotNull(TEXT("Shot presentation retains its native RPC"), ShotRPC)) return false;
	TestEqual(TEXT("The RPC carries one hit result and three identity fields"), static_cast<int32>(ShotRPC->NumParms), 4);
	TestNull(TEXT("TraceStart is not serialized a second time"), FindFProperty<FProperty>(ShotRPC, TEXT("TraceStart")));
	TestNull(TEXT("TraceEnd is not serialized a second time"), FindFProperty<FProperty>(ShotRPC, TEXT("TraceEnd")));
	TestNull(TEXT("The unused activation-key RPC field is removed"), FindFProperty<FProperty>(ShotRPC, TEXT("PresentationKey")));
	for (const bool bRestartFromDamage : {true, false})
	{
		const FString Label = bRestartFromDamage ? TEXT("Damage callback: ") : TEXT("Trace callback: ");
		FGameplayWorld Fixture(*this);
		if (!Fixture.Initialize()) return false;
		UfpstrueShotPresentationTestWeapon* Weapon = Cast<UfpstrueShotPresentationTestWeapon>(
			EquipWeaponWithOneSpentRound(Fixture, *this, nullptr, UfpstrueShotPresentationTestWeapon::StaticClass()));
		AActor* Target = Fixture.Spawn<AActor>();
		if (!TestNotNull(Label + TEXT("Production weapon with RPC observer"), Weapon) || Target == nullptr) return false;
		AfpstrueCharacter* Player = Weapon->GetOwningCharacter();
		UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
		FGameplayAbilitySpec* Fire = ASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass());
		const FStructProperty* StateProperty = FindFProperty<FStructProperty>(Weapon->GetClass(), TEXT("NetworkState"));
		if (!TestNotNull(Label + TEXT("Real fire spec"), Fire) ||
			!TestNotNull(Label + TEXT("Real network presentation snapshot"), StateProperty)) return false;
		FFPReplicatedWeaponState* State = StateProperty->ContainerPtrToValuePtr<FFPReplicatedWeaponState>(Weapon);
		UBoxComponent* TargetBox = NewObject<UBoxComponent>(Target);
		Target->AddInstanceComponent(TargetBox);
		Target->SetRootComponent(TargetBox);
		TargetBox->SetBoxExtent(FVector(100.0f));
		TargetBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		TargetBox->SetCollisionResponseToAllChannels(ECR_Ignore);
		TargetBox->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
		TargetBox->RegisterComponent();
		FVector EyeLocation;
		FRotator EyeRotation;
		Player->GetActorEyesViewPoint(EyeLocation, EyeRotation);
		Target->SetActorLocation(EyeLocation + EyeRotation.Vector() * 500.0f);
		UfpstrueHealthComponent* Health = NewObject<UfpstrueHealthComponent>(Target);
		Target->AddInstanceComponent(Health);
		Health->RegisterComponent();
		Health->SetMaxHealthAndReset(1000.0f);
		int32 Reentries = 0;
		int32 OldKey = 0;
		int32 ReplacementKey = 0;
		uint32 OriginalGeneration = 0;
		uint32 OriginalSession = 0;
		const auto RestartFire = [&]()
		{
			if (Reentries++ != 0) return;
			OldKey = Fire->GetPrimaryInstance()->GetCurrentActivationInfoRef().GetActivationPredictionKey().Current;
			OriginalGeneration = State->EquipmentGeneration;
			OriginalSession = State->FireSession;
			Weapon->StopFire();
			Weapon->StartFire();
			ReplacementKey = Fire->GetPrimaryInstance()->GetCurrentActivationInfoRef().GetActivationPredictionKey().Current;
			TestTrue(Label + TEXT("The replacement ability really activates"), Fire->IsActive());
			TestNotEqual(Label + TEXT("The replacement owns a different activation key"), ReplacementKey, OldKey);
			TestEqual(Label + TEXT("A new activation starts before its first shot"), State->FirePresentationShot, 0);
			// 模拟伤害/表现回调改变当前世界；旧射线仍须保留查询时的端点。
			Player->SetActorLocation(Player->GetActorLocation() + FVector(0.0f, 500.0f, 0.0f));
		};
		TStrongObjectPtr<UfpstrueReloadReentryObserver> Observer(NewObject<UfpstrueReloadReentryObserver>());
		FDelegateHandle DamageObserver;
		if (bRestartFromDamage)
			DamageObserver = Health->OnDamageResolved.AddLambda([&](float, AActor*, AController*, const FFPDamageContext&) { RestartFire(); });
		else
		{
			Observer->OnTraceReceived = RestartFire;
			Weapon->OnWeaponTraceFinished.AddDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleTraceFinished);
		}
		const int32 AmmoBefore = Weapon->GetCurrentAmmo();
		{
			// UE 可由待旅行 URL 推导 ListenServer 模式；作用域内只执行同步开火，绝不 Tick 或实际旅行。
			TGuardValue<FString> ListenMode(Fixture.GetWorld()->NextURL, TEXT("?listen"));
			if (!TestTrue(Label + TEXT("The real network-only shot branch is enabled"), Weapon->GetNetMode() != NM_Standalone)) return false;
			Weapon->StartFire();
		}
		TestTrue(Label + TEXT("The temporary URL is restored before any world tick"), Fixture.GetWorld()->NextURL.IsEmpty());
		TestEqual(Label + TEXT("The expected real callback reentered once"), Reentries, 1);
		TestTrue(Label + TEXT("The original activation has a real nonzero key"), OldKey > 0);
		TestEqual(Label + TEXT("Only the original shot spends ammunition synchronously"), Weapon->GetCurrentAmmo(), AmmoBefore - 1);
		if (!TestEqual(Label + TEXT("Exactly one real MulticastShot was dispatched"), Weapon->ShotPresentations.Num(), 1)) return false;
		const FObservedShotPresentation& Shot = Weapon->ShotPresentations[0];
		TestEqual(Label + TEXT("The shot retains its committed equipment generation"), Shot.EquipmentGeneration, OriginalGeneration);
		TestEqual(Label + TEXT("The shot retains its committed fire session"), Shot.Session, OriginalSession);
		TestEqual(Label + TEXT("The old shot RPC retains ordinal one instead of the new round's zero"), Shot.ShotIndex, 1);
		TestTrue(Label + TEXT("The ordinary environment hit remains blocking"), Shot.Hit.bBlockingHit);
		TestTrue(Label + TEXT("The compact RPC retains the queried environment actor"), Shot.Hit.GetActor() == Target);
		TestTrue(Label + TEXT("The hit stores its original start before a callback moves the shooter"),
			Shot.Hit.TraceStart.Equals(EyeLocation, 0.001f));
		TestTrue(Label + TEXT("The hit stores the complete original ray endpoint, not its impact point"),
			FMath::IsNearlyEqual((Shot.Hit.TraceEnd - Shot.Hit.TraceStart).Size(),
				static_cast<double>(Weapon->GetWeaponSettings().LineTraceRange), 0.01));
		TestEqual(Label + TEXT("The new round keeps its own published key"), State->FirePresentationKey, ReplacementKey);
		TestEqual(Label + TEXT("The old call stack cannot increment the replacement round"), State->FirePresentationShot, 0);
		Health->OnDamageResolved.Remove(DamageObserver);
		Observer->OnTraceReceived = nullptr;
		Weapon->OnWeaponTraceFinished.RemoveDynamic(Observer.Get(), &UfpstrueReloadReentryObserver::HandleTraceFinished);
		// 在已恢复 Standalone 的世界推进真正 WaitDelay；验证新轮只有一发、序号从一开始，而非伪造任务回调。
		if (!Fixture.Advance(60.0f / Weapon->GetWeaponSettings().RoundsPerMinute + 0.03f)) return false;
		TestEqual(Label + TEXT("Only the replacement task fires the next single shot"), Weapon->GetCurrentAmmo(), AmmoBefore - 2);
		{
			// Standalone 不发布网络快照；同步结束能力时临时恢复网络分支，读取真实新轮计数，仍不 Tick 或旅行。
			TGuardValue<FString> ListenMode(Fixture.GetWorld()->NextURL, TEXT("?listen"));
			Weapon->StopFire();
		}
		TestEqual(Label + TEXT("The replacement shot preserves its own activation key"), State->FirePresentationKey, ReplacementKey);
		TestEqual(Label + TEXT("The replacement round starts with ordinal one"), State->FirePresentationShot, 1);
		TestFalse(Label + TEXT("Cleanup ends the replacement fire ability"), Fire->IsActive());
	}
	// 无阻挡时没有 ImpactPoint 可代替射线终点；仍经真实 Fire/查询/RPC 分派检查 FHitResult 端点。
	{
		FGameplayWorld Fixture(*this);
		if (!Fixture.Initialize()) return false;
		UfpstrueShotPresentationTestWeapon* Weapon = Cast<UfpstrueShotPresentationTestWeapon>(
			EquipWeaponWithOneSpentRound(Fixture, *this, nullptr, UfpstrueShotPresentationTestWeapon::StaticClass()));
		if (!TestNotNull(TEXT("Unobstructed shot uses the production weapon"), Weapon)) return false;
		FVector EyeLocation;
		FRotator EyeRotation;
		Weapon->GetOwningCharacter()->GetActorEyesViewPoint(EyeLocation, EyeRotation);
		{
			TGuardValue<FString> ListenMode(Fixture.GetWorld()->NextURL, TEXT("?listen"));
			Weapon->StartFire();
			Weapon->StopFire();
		}
		if (!TestEqual(TEXT("An unobstructed shot still dispatches one presentation RPC"), Weapon->ShotPresentations.Num(), 1)) return false;
		const FHitResult& Miss = Weapon->ShotPresentations[0].Hit;
		TestFalse(TEXT("A miss remains nonblocking after compacting the RPC"), Miss.bBlockingHit);
		TestTrue(TEXT("A miss retains its original trace start"), Miss.TraceStart.Equals(EyeLocation, 0.001f));
		TestTrue(TEXT("A miss retains the full trace endpoint"), FMath::IsNearlyEqual((Miss.TraceEnd - Miss.TraceStart).Size(),
			static_cast<double>(Weapon->GetWeaponSettings().LineTraceRange), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadMontageIdentityTest, "fpstrue.Gameplay.Boundaries.Weapon.ReloadMontageInstanceIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证同一 Montage 重播的实例 ID 隔离，旧 Notify/分支点无法提交新换弹，无关播放不被停止。
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
	Anim->CurrentSkeleton = Arms->GetSkeletalMeshAsset()->GetSkeleton();
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
	FFPActionPlayback SavedPlaybackA;
	TestNull(TEXT("Unbound playback has no instance"), SavedPlaybackA.GetBoundInstance());
	if (!TestTrue(TEXT("Capture the explicit playback A"), SavedPlaybackA.TryBind(Weapon->GetActiveReloadId(), Arms, Montage) == InstanceA)) return false;
	TestTrue(TEXT("Rebinding the same identity returns the same instance"),
		SavedPlaybackA.TryBind(Weapon->GetActiveReloadId(), Arms, Montage) == InstanceA);
	TestNull(TEXT("Anonymous binding returns no instance"), SavedPlaybackA.TryBind(0, Arms, Montage));
	TestTrue(TEXT("Instance resolution uses the captured playback"), SavedPlaybackA.GetBoundInstance() == InstanceA);
	Weapon->CancelReload();
	TestTrue(TEXT("Reload B restarts the same Montage asset"), Weapon->RequestReload());
	FAnimMontageInstance* InstanceB = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("Engine allocated playback B"), InstanceB)) return false;
	const int32 PlaybackB = InstanceB->GetInstanceID();
	TestNotEqual(TEXT("The same asset gets a new playback identity"), PlaybackA, PlaybackB);
	TestNull(TEXT("Old binding cannot adopt a new playback"), SavedPlaybackA.TryBind(Weapon->GetActiveReloadId(), Arms, Montage));
	TestTrue(TEXT("Old binding never resolves to the new playback of the same asset"), SavedPlaybackA.GetBoundInstance() != InstanceB);
	// An unrelated Montage on the same mesh must not replace the explicit reload binding.
	UAnimMontage* Unrelated = NewObject<UAnimMontage>(Anim);
	Unrelated->SetSkeleton(Anim->CurrentSkeleton);
	Unrelated->SetCompositeLength(1.0f);
	FFPActionPlayback MismatchedPlayback;
	if (!TestTrue(TEXT("Capture current playback before testing an asset mismatch"),
		MismatchedPlayback.TryBind(Weapon->GetActiveReloadId(), Arms, Montage) == InstanceB)) return false;
	MismatchedPlayback.Montage = Unrelated;
	TestNull(TEXT("An instance ID cannot resolve with the wrong Montage asset"), MismatchedPlayback.GetBoundInstance());
	SavedPlaybackA.Reset();
	TestNull(TEXT("Reset removes the saved playback identity"), SavedPlaybackA.GetBoundInstance());
	Anim->Montage_Play(Unrelated, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false);
	FAnimMontageInstance* UnrelatedInstance = Anim->GetActiveInstanceForMontage(Unrelated);
	if (!TestNotNull(TEXT("The unrelated same-mesh playback exists"), UnrelatedInstance)) return false;
	TestFalse(TEXT("An unrelated playback cannot overwrite the reload's explicit binding"),
		Weapon->BindReloadMontage(Weapon->GetActiveReloadId(), Arms, Unrelated));
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	const int32 ReserveBefore = Weapon->GetReserveAmmo();
	TestNull(TEXT("The old visual commit class has been removed"),
		FindObject<UClass>(nullptr, TEXT("/Script/fpstrue.fpstrueAnimNotify_ReloadCommit")));
	TestEqual(TEXT("Binding and replacing visual playbacks never loads ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);
	Weapon->FinishReloadForTransaction(Weapon->GetActiveReloadId());
	TestEqual(TEXT("Authority completion loads the missing round exactly once"), Weapon->GetReserveAmmo(), ReserveBefore - 1);
	TestTrue(TEXT("Ending reload preserves unrelated playback on the same mesh"), UnrelatedInstance->IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadStaleTransactionTest, "fpstrue.Gameplay.Boundaries.Weapon.StaleTransactionCallbacksIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 旧换弹结束后启动新事务，迟到的提交、完成和取消都不能借用新编号。
bool FFpstrueReloadStaleTransactionTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (!Weapon) return false;
	TestTrue(TEXT("Old reload starts"), Weapon->RequestReload());
	const int32 OldId = Weapon->GetActiveReloadId();
	Weapon->CancelReload();
	TestTrue(TEXT("New reload starts before stale callbacks arrive"), Weapon->RequestReload());
	const int32 NewId = Weapon->GetActiveReloadId();
	TestNotEqual(TEXT("The new reload has its own identity"), NewId, OldId);
	const int32 AmmoBefore = Weapon->GetCurrentAmmo();
	const int32 ReserveBefore = Weapon->GetReserveAmmo();
	TestFalse(TEXT("A stale transaction cannot commit ammunition"), Weapon->CommitReloadForTransaction(OldId));
	Weapon->FinishReloadForTransaction(OldId);
	TestEqual(TEXT("Stale completion cannot end the new reload"), Weapon->GetActiveReloadId(), NewId);
	Weapon->CancelReloadForTransaction(OldId);
	TestEqual(TEXT("Stale cancellation cannot end the new reload"), Weapon->GetActiveReloadId(), NewId);
	TestEqual(TEXT("Stale callbacks cannot fill the new reload"), Weapon->GetCurrentAmmo(), AmmoBefore);
	TestEqual(TEXT("Stale callbacks cannot spend reserve ammunition"), Weapon->GetReserveAmmo(), ReserveBefore);
	Weapon->FinishReloadForTransaction(NewId);
	TestFalse(TEXT("The explicit current identity can complete normally"), Weapon->IsReloading());
	TestEqual(TEXT("Explicit completion preserves the legitimate ammo path"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueReloadPlaybackFailureTest, "fpstrue.Gameplay.Boundaries.Weapon.PlaybackFailureAndEndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查播放失败立即解锁且不补弹，取消只停止所绑定实例，真实自然结束可完成当前换弹。
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
	TestTrue(TEXT("Playback failure leaves the authority transaction running"), Weapon->IsReloading());
	TestEqual(TEXT("Playback failure cannot report a gameplay end"), Observer->ReloadEndCount, 0);
	TestEqual(TEXT("Playback failure leaves ammunition unchanged"), Weapon->GetCurrentAmmo(), AmmoBefore);
	if (!World.Advance(5.2f)) return false;
	TestEqual(TEXT("The authority timer completes despite missing presentation"), Observer->ReloadEndCount, 1);
	TestEqual(TEXT("The timer reports Completed, not animation success"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
	TestEqual(TEXT("The authority timer filled the magazine once"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	Weapon->StartFire();
	Weapon->StopFire();

	// 用真实原生动画实例重试播放，分别检查取消与自然结束的弹药和动作结果。
	USkeletalMeshComponent* Arms = CastChecked<USkeletalMeshComponent>(Weapon->GetAttachParent());
	UAnimInstance* Anim = NewObject<UAnimInstance>(Arms);
	Arms->AnimScriptInstance = Anim;
	Anim->InitializeAnimation();
	Anim->CurrentSkeleton = Arms->GetSkeletalMeshAsset()->GetSkeleton();
	UAnimMontage* Montage = NewObject<UAnimMontage>(Anim);
	Montage->SetSkeleton(Anim->CurrentSkeleton);
	Montage->SetCompositeLength(1.0f);
	TestTrue(TEXT("Reload can retry after playback failure"), Weapon->RequestReload());
	TestTrue(TEXT("Explicit playback starts"), Weapon->PlayReloadMontage(Weapon->GetActiveReloadId(), Arms, Montage));
	FAnimMontageInstance* Instance = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("Explicit playback has an instance"), Instance)) return false;
	Weapon->CancelReload();
	TestFalse(TEXT("Cancellation stops the exact owned playback"), Instance->IsActive());
	TestEqual(TEXT("Cancellation before authority completion does not load ammunition"), Weapon->GetCurrentAmmo(), AmmoBefore);
	TestTrue(TEXT("A subsequent reload gets a clean playback"), Weapon->RequestReload());
	TestTrue(TEXT("The same resource can play in the new transaction"), Weapon->PlayReloadMontage(Weapon->GetActiveReloadId(), Arms, Montage));
	FAnimMontageInstance* CompletedInstance = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("The new explicit playback has an instance"), CompletedInstance)) return false;
	// Drive the public engine instance timing; Advance executes its real Terminate/end-delegate path.
	for (int32 Index = 0; Index < 20 && Weapon->IsReloading(); ++Index)
	{
		FAnimMontageInstance* Playing = Anim->GetActiveInstanceForMontage(Montage);
		if (!Playing) break;
		Playing->UpdateWeight(0.1f);
		Playing->Advance(0.1f, nullptr, false);
	}
	TestTrue(TEXT("Native playback completion cannot finish the authority transaction"), Weapon->IsReloading());
	if (!World.Advance(Weapon->GetReloadDuration() + 0.1f)) return false;
	TestFalse(TEXT("The authority clock completes after presentation has ended"), Weapon->IsReloading());
	TestEqual(TEXT("A successful current playback reports Completed"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
	TestEqual(TEXT("Successful explicit playback fills ammunition"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueWeaponAssetReloadTest, "fpstrue.Gameplay.Boundaries.Weapon.AssetReloadNormalAndEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 加载真实武器蓝图和配置，覆盖普通/空仓的双 Montage 桥、播放中提交及弹药守恒。
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
	UfpstrueWeaponConfig* Config = LoadObject<UfpstrueWeaponConfig>(nullptr, TEXT("/Game/Config/DA_FPWeapon_RU74.DA_FPWeapon_RU74"));
	const FObjectProperty* ConfigProperty = FindFProperty<FObjectProperty>(Weapon->GetClass(), TEXT("WeaponConfiguration"));
	if (!TestNotNull(TEXT("The migrated weapon configuration is shipped"), Config)
		|| !TestNotNull(TEXT("The real component exposes its configuration binding"), ConfigProperty)) return false;
	TestTrue(TEXT("The real Blueprint references the migrated asset, not native fallback"), ConfigProperty->GetObjectPropertyValue_InContainer(Weapon) == Config);
	FString ConfigurationError;
	TestTrue(TEXT("The shipped weapon tuning passes validation"), Config->Settings.Validate(ConfigurationError));
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
	// 两种弹匣前态必须各自选对手臂/武器资产，且一组双播放仅广播一次事务结束。
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
		TestFalse(TEXT("Authority timing clears Reloading without legacy Blueprint callbacks"), Weapon->IsReloading());
		TestTrue(TEXT("The authority clock commits while the paired visual is active"), Observer->bAmmoChangedWhileMontageActive);
		TestEqual(TEXT("Real playback reports Completed, not timeout recovery"), Observer->LastReloadEndReason, EFPReloadEndReason::Completed);
		TestEqual(TEXT("The paired playbacks emit exactly one transaction end"), Observer->ReloadEndCount, EndCountBefore + 1);
		TestEqual(TEXT("The real asset reload fills the magazine"), Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize());
		TestEqual(TEXT("Real asset reload conserves total ammunition"), Weapon->GetCurrentAmmo() + Weapon->GetReserveAmmo(), TotalBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueRecoilElapsedTimeTest, "fpstrue.Gameplay.Boundaries.Weapon.RecoilUsesElapsedTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 经真实武器和 PlayerController 检查后坐力按经过时间恢复、视觉震动隔离，以及控制权退出清理。
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
	Controller->SetViewTarget(Player);
	Controller->SetActorTickEnabled(false);
	Player->GetCharacterMovement()->SetComponentTickEnabled(false);
	Controller->RotationInput = FRotator::ZeroRotator;
	Weapon->StartFire();
	Weapon->StopFire();
	const double Kick = Controller->RotationInput.Pitch;
	if (!TestFalse(TEXT("The real PlayerController received recoil input"), FMath::IsNearlyZero(Kick))) return false;
	APlayerCameraManager* CameraManager = Controller->PlayerCameraManager;
	if (!TestNotNull(TEXT("The local player owns a camera manager"), CameraManager)) return false;
	UCameraModifier_CameraShake* ShakeModifier = Cast<UCameraModifier_CameraShake>(CameraManager->FindCameraModifierByClass(UCameraModifier_CameraShake::StaticClass()));
	if (!TestNotNull(TEXT("Camera shake modifier exists"), ShakeModifier)) return false;
	TArray<FActiveCameraShakeInfo> ActiveShakes;
	ShakeModifier->GetActiveCameraShakes(ActiveShakes);
	if (!TestEqual(TEXT("A real shot starts one visual recoil; releasing fire preserves its short return"), ActiveShakes.Num(), 1)) return false;
	// 实际一枪应只创建一个视觉震动；最终 POV 可变化，但射线相机与控制输入不受视觉震动反写。
	UCameraShakeBase* VisualShake = ActiveShakes[0].ShakeInstance;
	if (!TestTrue(TEXT("Weapon uses its isolated visual recoil class"), VisualShake->IsA<UfpstrueWeaponCameraShake>())) return false;
	const FTransform AimTransform = Player->GetFirstPersonCameraComponent()->GetComponentTransform();
	const FRotator ControlRotation = Controller->GetControlRotation();
	const FRotator RotationInput = Controller->RotationInput;
	FMinimalViewInfo VisualPOV;
	VisualPOV.Location = AimTransform.GetLocation();
	VisualPOV.Rotation = AimTransform.Rotator();
	VisualShake->UpdateAndApplyCameraShake(0.02f, 1.0f, VisualPOV);
	TestFalse(TEXT("The shot visibly changes final POV"), VisualPOV.Rotation.Equals(AimTransform.Rotator()));
	TestTrue(TEXT("Visual recoil does not rotate or move the hitscan camera"), AimTransform.Equals(Player->GetFirstPersonCameraComponent()->GetComponentTransform()));
	TestTrue(TEXT("Visual recoil does not write control rotation"), ControlRotation.Equals(Controller->GetControlRotation()));
	TestTrue(TEXT("Visual recoil does not add control input"), RotationInput.Equals(Controller->RotationInput));
	// Direct test calls occur outside World::Tick: arm TimerManager's pending timer at zero elapsed time
	// before simulating the hitch, otherwise that hitch is spent registering (not running) the timer.
	if (!World.AdvanceSingleFrame(0.0f)) return false;
	TestEqual(TEXT("Arming the pending timer does not consume recoil or advance game time"), Controller->RotationInput.Pitch, Kick);
	const double RecoveryStartTime = World.GetWorld()->GetTimeSeconds();
	// A 140ms hitch includes 120ms without recovery and 20ms of actual recovery.
	// At 10 degrees/s a 1-degree kick must retain 80%; the native timer skips same-frame catch-up calls.
	if (!World.AdvanceSingleFrame(0.14f)) return false;
	TestEqual(TEXT("The hitch advances exactly 140ms of game time"), World.GetWorld()->GetTimeSeconds() - RecoveryStartTime, 0.14, 0.0001);
	TestEqual(TEXT("Recovery integrates elapsed time, excluding the configured delay"), Controller->RotationInput.Pitch / Kick, 0.8, 0.025);
	// 连续低帧继续按经过时间积分，而不是按一次回调固定恢复 1/60 秒。
	if (!World.AdvanceSingleFrame(0.04f)) return false;
	TestEqual(TEXT("A subsequent 40ms frame integrates its full recovery interval"), Controller->RotationInput.Pitch / Kick, 0.4, 0.025);
	if (!World.AdvanceSingleFrame(0.04f)) return false;
	TestTrue(TEXT("Recovery reaches zero without overshooting during low frame rate"), FMath::IsNearlyZero(Controller->RotationInput.Pitch, FMath::Abs(Kick) * 0.025));
	const FRotator RecoveredInput = Controller->RotationInput;
	if (!World.AdvanceSingleFrame(0.1f)) return false;
	TestTrue(TEXT("Completed recovery leaves no later input correction"), Controller->RotationInput.Equals(RecoveredInput, 0.001));
	Weapon->StartFire();
	Weapon->StopFire();
	ActiveShakes.Reset(); // UE 的查询接口向输出数组追加，不能把上次快照算成仍在播放的实例。
	ShakeModifier->GetActiveCameraShakes(ActiveShakes);
	TestEqual(TEXT("Repeated fire never stacks visual recoil instances"), ActiveShakes.Num(), 1);
	if (ActiveShakes.Num() == 1) TestTrue(TEXT("Repeated fire reuses the running instance"), ActiveShakes[0].ShakeInstance == VisualShake);
	// 控制权中断与死亡/禁用都必须移除自有镜头效果，不能留下持续晃动实例。
	Weapon->InterruptOwnerInput();
	ActiveShakes.Reset();
	ShakeModifier->GetActiveCameraShakes(ActiveShakes);
	TestEqual(TEXT("Losing owner input removes weapon visual recoil"), ActiveShakes.Num(), 0);
	if (!World.Advance(0.2f)) return false;
	Weapon->StartFire();
	Weapon->StopFire();
	Weapon->DisableWeapon();
	ActiveShakes.Reset();
	ShakeModifier->GetActiveCameraShakes(ActiveShakes);
	TestEqual(TEXT("Disable/death leaves no weapon visual recoil"), ActiveShakes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueOwnerInputTransitionTest, "fpstrue.Gameplay.Boundaries.Weapon.OwnerInputTransitionRejectsReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查临时输入迁移期间拒绝结束通知重入，普通取消允许接续，新 Controller 接管后可重新动作。
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

	// 实际 UnPossess 同样关闭迁移窗口内的新动作；迁移结束后不应永久禁用装备。
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueAbilityCommitReentryTest,
	"fpstrue.Gameplay.Boundaries.GAS.CommitCancellationAndSameSpecRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实 GAS Commit 广播中取消并重启同一能力，旧激活栈不能再扣弹、建立换弹或注册重复任务。
bool FFpstrueAbilityCommitReentryTest::RunTest(const FString& Parameters)
{
	using namespace FpstrueGameplayBoundaryTests;
	FGameplayWorld World(*this);
	if (!World.Initialize()) return false;
	UfpstrueWeaponComponent* Weapon = EquipWeaponWithOneSpentRound(World, *this);
	if (Weapon == nullptr) return false;
	AfpstrueCharacter* Player = Weapon->GetOwningCharacter();
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	if (!TestNotNull(TEXT("Commit reentry exercises the production ASC"), ASC)) return false;
	const auto CountTasks = [](UGameplayAbility* Ability)
	{
		int32 Count = 0;
		for (TObjectIterator<UGameplayTask> Task; Task; ++Task)
		{
			if (IsValid(*Task) && Task->GetTaskOwner() == Ability && !Task->IsFinished()) ++Count;
		}
		return Count;
	};
	struct FCase { UClass* Class; EFPCombatAbilityInput Input; FGameplayTag State; };
	const FCase Cases[] = {
		{UfpstrueFireAbility::StaticClass(), EFPCombatAbilityInput::Fire, FpstrueGameplayTags::State_Firing},
		{UfpstrueReloadAbility::StaticClass(), EFPCombatAbilityInput::Reload, FpstrueGameplayTags::State_Reloading},
		{UfpstrueAimAbility::StaticClass(), EFPCombatAbilityInput::Aim, FpstrueGameplayTags::State_Aiming},
		{UfpstrueSprintAbility::StaticClass(), EFPCombatAbilityInput::Sprint, FpstrueGameplayTags::State_Sprinting}
	};
	for (const FCase& Case : Cases)
	{
		FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(Case.Class);
		if (!TestNotNull(TEXT("The real combat ability is granted"), Spec)) return false;
		UGameplayAbility* Instance = Spec->GetPrimaryInstance();
		if (!TestNotNull(TEXT("The granted ability has its real reusable instance"), Instance)) return false;
		for (const bool bRestart : {false, true})
		{
			const FString Label = FString::Printf(TEXT("%s %s: "), *Case.Class->GetName(), bRestart ? TEXT("restart") : TEXT("cancel"));
			const int32 AmmoBefore = Weapon->GetCurrentAmmo();
			const float IdleSpeed = Player->GetCharacterMovement()->MaxWalkSpeed;
			int32 Commits = 0;
			bool bRestartAccepted = false;
			const FDelegateHandle Observer = ASC->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility* Ability)
			{
				if (Ability != Instance || ++Commits != 1) return;
				ASC->CancelAbilityHandle(Spec->Handle);
				if (bRestart) bRestartAccepted = ASC->TryActivateAbility(Spec->Handle);
			});
			ASC->AbilityLocalInputPressed(static_cast<int32>(Case.Input));
			ASC->AbilityCommittedCallbacks.Remove(Observer);
			TestEqual(Label + TEXT("only expected activations commit"), Commits, bRestart ? 2 : 1);
			TestEqual(Label + TEXT("same-spec restart is accepted only when requested"), bRestartAccepted, bRestart);
			TestEqual(Label + TEXT("only the requested replacement stays active"), Spec->IsActive(), bRestart);
			TestEqual(Label + TEXT("the action tag matches the actual replacement"), ASC->HasMatchingGameplayTag(Case.State), bRestart);
			const bool bFire = Case.Input == EFPCombatAbilityInput::Fire;
			const bool bReload = Case.Input == EFPCombatAbilityInput::Reload;
			TestEqual(Label + TEXT("the cancelled outer activation cannot spend a round"),
				Weapon->GetCurrentAmmo(), AmmoBefore - (bRestart && bFire ? 1 : 0));
			TestEqual(Label + TEXT("only a replacement owns tasks, never its cancelled outer stack"), CountTasks(Instance),
				bRestart ? (bFire ? 2 : Case.Input == EFPCombatAbilityInput::Aim ? 1 : 0) : 0);
			TestEqual(Label + TEXT("only the replacement reload opens a transaction"), Weapon->GetActiveReloadId() != INDEX_NONE, bRestart && bReload);
			TestEqual(Label + TEXT("aim presentation follows the real replacement"), Player->IsAiming(), bRestart && Case.Input == EFPCombatAbilityInput::Aim);
			if (bRestart && bFire)
			{
				// 真的推进下一次 WaitDelay，重复注册的旧任务会多开火或错误结束新能力。
				if (!World.Advance(60.0f / Weapon->GetWeaponSettings().RoundsPerMinute + 0.03f)) return false;
				TestEqual(Label + TEXT("exactly one new pulse fires at the next interval"), Weapon->GetCurrentAmmo(), AmmoBefore - 2);
				TestTrue(Label + TEXT("the replacement still owns held fire"), Spec->IsActive());
			}
			ASC->AbilityLocalInputReleased(static_cast<int32>(Case.Input));
			ASC->CancelAbilityHandle(Spec->Handle);
			TestFalse(Label + TEXT("cleanup leaves no active ability"), Spec->IsActive());
			TestFalse(Label + TEXT("cleanup removes the action tag"), ASC->HasMatchingGameplayTag(Case.State));
			TestEqual(Label + TEXT("cleanup ends every ability-owned task"), CountTasks(Instance), 0);
			TestEqual(Label + TEXT("cleanup closes the reload transaction"), Weapon->GetActiveReloadId(), INDEX_NONE);
			TestFalse(Label + TEXT("cleanup clears aim presentation"), Player->IsAiming());
			TestEqual(Label + TEXT("cleanup restores movement speed"), Player->GetCharacterMovement()->MaxWalkSpeed, IdleSpeed);
			const int32 SettledAmmo = Weapon->GetCurrentAmmo();
			const float SettleTime = bReload ? Weapon->GetWeaponSettings().EmptyReloadDuration + 0.1f : 0.3f;
			if (!World.Advance(SettleTime)) return false;
			TestEqual(Label + TEXT("advancing real timers cannot execute a ghost shot or reload"), Weapon->GetCurrentAmmo(), SettledAmmo);
			TestFalse(Label + TEXT("advancing real timers cannot reactivate the old action"), Spec->IsActive());
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
