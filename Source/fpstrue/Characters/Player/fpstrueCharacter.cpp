// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Player/fpstruePlayerAnimInstance.h"
#include "Characters/Player/fpstrueCharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Animation/AnimInstance.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/GameStateBase.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"
#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstruePvpPlayerState.h"
#include "Runtime/fpstrueRuntime.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#if !UE_BUILD_SHIPPING
#include "UnrealClient.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#endif

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

// 本文件只保留 Character 生命周期/输入/装备/死亡；动画、CMC、历史命中和开发烟测按原职责分文件。
// Character 只协调已有状态所有者：GAS 管动作，武器管弹药与换弹，AttributeSet 管生命；动画不反向授权玩法。

AfpstrueCharacter::AfpstrueCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UfpstrueCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	//胶囊体
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);
	// 胶囊只保留移动碰撞职责；玩家子弹的部位判定不能先撞到外层移动胶囊。
	GetCapsuleComponent()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);
	bReplicates = true;
	SetReplicateMovement(true);
	// 第三人称身体只给其他玩家看；成套网格与动画由正式角色蓝图配置，原生不补装旧素材。
	GetMesh()->SetOwnerNoSee(true);
	// 服务器也需要真实身体物理状态；存活时仍是查询体，不提前启用布娃娃。
	GetMesh()->bEnablePhysicsOnDedicatedServer = true;

	//弹簧臂创建、挂载、位置、长度
	//**构造函数中创建Actor默认组件
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	// 第一人称相机臂长为 0，不需要弹簧臂碰撞回缩。
	CameraBoom->TargetArmLength = 0.0f;
	//旋转跟随
	CameraBoom->bUsePawnControlRotation = true;
	//平滑延迟效果（位置、旋转延迟跟随）

	//相机创建、挂载、**关闭自身控制**
	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FirstPersonCameraComponent->bUsePawnControlRotation = false;

	//网格体创建、可见性、挂载、关闭动静态阴影投射、
	Mesh1P = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh1P"));
	Mesh1P->SetOnlyOwnerSee(true);
	Mesh1P->SetupAttachment(FirstPersonCameraComponent);
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->CastShadow = false;
	Mesh1P->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);

	//禁用角色自身 Tick：输入、碰撞、动画通知等事件驱动角色逻辑；Movement、相机和 Mesh 组件仍各自更新。
	PrimaryActorTick.bCanEverTick = false;
	//自定义健康组件
	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueCharacter::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	Super::CalcCamera(DeltaTime, OutResult);
	// 模板原设全局 0.0001 cm；这里只为原生动画表现使用 1 cm，保留近处手部且不把极小近裁面扩散到其他相机。
	if (UsesNativePresentation()) OutResult.PerspectiveNearClipPlane = PresentationNearClipPlane;
}

// 单机蓝图的 AutoPossessPlayer=Player0 会在第二次生成时抢走第一个玩家的 Pawn；网络只允许 GameMode 显式占有。
void AfpstrueCharacter::PreInitializeComponents()
{
	// 实际角色蓝图独占网格和 AnimClass 配置，不再运行时加载旧素材补齐。
	if (GetNetMode() == NM_DedicatedServer)
	{
		// 保留骨骼和握枪插槽供装备校验；专服只更新第三人称权威身体，不求值不可见的第一人称手臂。
		Mesh1P->SetAnimInstanceClass(nullptr);
		// 主 AnimClass 清空并不清理网格自带的后处理实例；显式停求值，不修改共享骨骼资产或声称实例已释放。
		Mesh1P->SetDisablePostProcessBlueprint(true);
		Mesh1P->PrimaryComponentTick.bStartWithTickEnabled = false;
		Mesh1P->SetComponentTickEnabled(false);
	}
	if (GetNetMode() != NM_Standalone)
	{
		if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(GetCharacterMovement())) Movement->ConfigureServerTimelineSmoothing();
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE POSSESSION_DEFAULT Name=%s AutoPlayer=%d AutoAI=%d"),
				*GetName(), static_cast<int32>(AutoPossessPlayer.GetValue()), static_cast<int32>(AutoPossessAI));
#endif
		AutoPossessPlayer = EAutoReceiveInput::Disabled;
		AutoPossessAI = EAutoPossessAI::Disabled;
	}
	Super::PreInitializeComponents();
}

// 订阅生命事件并推送初始快照，再按当前装备显示手臂、应用基础移动速度。
void AfpstrueCharacter::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		// 组件初始化/自动激活可能重启 Tick；在完整初始化之后再次关闭纯视模工作。
		Mesh1P->SetComponentTickEnabled(false);
	}
	if (HealthComponent != nullptr)
	{
		HealthComponent->InitAbilityActorInfo(this, this);
		// 本地预测、服务器确认和取消都读取同一组标签，不在输入和 RPC 中重复修改姿态。
		// 由 GA 激活/结束后刷新，不在引擎 PreActivate 的标签广播期间调用外部蓝图。
		if (HasAuthority())
		{
			// 能力随当前 Pawn 授予一次；重生创建新 Pawn，不把旧生命的能力和预测带过去。
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueFireAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Fire), this));
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueReloadAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Reload), this));
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueAimAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Aim), this));
			HealthComponent->GiveAbility(FGameplayAbilitySpec(UfpstrueSprintAbility::StaticClass(), 1,
				static_cast<int32>(EFPCombatAbilityInput::Sprint), this));
		}
	}

	if (HealthComponent != nullptr)
	{
		// 订阅受攻击、死亡事件；HUD 直接订阅生命组件并补读快照。
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDeath);
	}

	// 由实际蓝图完成材料/展示骨骼初始化；专服不执行纯表现。
	if (UsesNativePresentation() && GetNetMode() != NM_DedicatedServer) InitializeVisualPresentation();
	const bool bHasWeapon = EquippedWeaponComponent != nullptr;
	Mesh1P->SetHiddenInGame(!bHasWeapon, true);
	// 初始化基础移动速度；后续只在冲刺和瞄准状态切换时修改。
	RefreshMovementAbilityState();
	if (GetNetMode() != NM_Standalone)
	{
		// 联网规则视点与服务器 EyesViewPoint 对齐，不让弹簧臂的平滑滞后改变准星方向或射线起点。
		// 视觉后坐力/CameraShake 仍由表现层执行；单机保留原相机延迟配置。
		CameraBoom->bEnableCameraLag = false;
		CameraBoom->bEnableCameraRotationLag = false;
		CameraBoom->SetRelativeLocation(FVector(0.0f, 0.0f, BaseEyeHeight));
		GetMesh()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		GetMesh()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		GetMesh()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
		// 【设计取舍】视模投影和隐藏只服务本地镜头；第三人称身体同时服务观察与权威命中，不能拿视模替代。
		// 专服关闭第一人称 Tick，但身体仍付出必要的骨骼刷新/历史采样成本，不能套用客户端不可见剔除。
		GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		// 不能把客户端不可见裁剪/URO 的旧姿态当作服务器当前命中形状。
		if (HasAuthority()) GetMesh()->bEnableUpdateRateOptimizations = false;
		// 移动碰撞只由胶囊负责；子弹查询 PhysicsAsset 身体形状，不能让外层胶囊提前吃掉头部命中。
		GetCapsuleComponent()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);
		Mesh1P->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);
		if (HasAuthority())
		{
			// 骨骼完成不代表父组件的移动/延后变换传播已经收尾。
			// 所有 Actor Tick 完成后、网络复制前只采一份最终姿态，不混用新骨骼和旧世界变换。
			HitHistoryPostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddWeakLambda(this,
				[this](UWorld* World, ELevelTick, float)
				{
					if (World != GetWorld()) return;
					RecordServerHitHistory();
					if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(GetCharacterMovement()))
						Movement->SetServerSnapshotTime(HitHistoryCount > 0
							? HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity].ServerTime : 0.0);
				});
			RecordServerHitHistory();
		}
		if (IsDead()) HandleDeath();
#if !UE_BUILD_SHIPPING
		// ★【重要边界】[发布隔离] 自动输入只编入开发/测试版本；Shipping 不响应测试命令行。
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		{
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PAWN_BEGIN Name=%s Authority=%d Replicates=%d HealthRep=%d"),
				*GetName(), HasAuthority(), GetIsReplicated(), HealthComponent != nullptr && HealthComponent->GetIsReplicated());
			UE_LOG(LogTemp, Display, TEXT("PVP_MEMORY Pawn=%s NetMode=%d HistoryBytes=%llu ReservedShapes=%d ShapeStride=%llu ViewmodelAnim=%d ViewmodelTick=%d ViewmodelPostProcess=%d"),
				*GetName(), static_cast<int32>(GetNetMode()), static_cast<unsigned long long>(GetHitHistoryAllocatedBytes()),
				GetHitHistoryReservedShapeCount(), static_cast<unsigned long long>(GetHitHistoryShapeStride()),
				Mesh1P->GetAnimInstance() != nullptr, Mesh1P->IsComponentTickEnabled(), Mesh1P->GetPostProcessInstance() != nullptr);
			GetWorldTimerManager().SetTimer(NetworkSmokeTimer, this, &AfpstrueCharacter::RunPvpSmokeStep, 0.05f, true);
		}
#endif
	}
}

// 角色移动仍走 CharacterMovement；这里只复制角色级姿态和装备，生命由 HealthComponent 复制。
void AfpstrueCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstrueCharacter, EquippedWeaponComponent);
	// 拥有者从 GAS 标签预测和纠正；其他客户端只需要服务器的姿态显示镜像。
	DOREPLIFETIME_CONDITION(AfpstrueCharacter, bIsAiming, COND_SimulatedOnly);
	DOREPLIFETIME_CONDITION(AfpstrueCharacter, bIsSprinting, COND_SimulatedOnly);
	DOREPLIFETIME(AfpstrueCharacter, BodyFacingState);
}

float AfpstrueCharacter::GetBodyFacingServerTime() const
{
	const AGameStateBase* State = GetWorld()->GetGameState();
	return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

// 由服务器身体动画的既有更新驱动；胶囊、输入和 CMC 的移动预测仍完全沿用原通路。
void AfpstrueCharacter::UpdateBodyFacing(float TurnDuration)
{
	if (!HasAuthority() || !UsesTemplatePresentation()) return;
	const bool bStationary = !IsDead() && GetCharacterMovement()->IsMovingOnGround() && GetVelocity().SizeSquared2D() <= FMath::Square(3.0f);
	bool bChanged = false;
	if (!BodyFacingState.bInitialized || BodyFacingState.bStationary != bStationary)
	{
		BodyFacingState.bInitialized = true;
		BodyFacingState.bStationary = bStationary;
		BodyFacingState.FacingYaw = GetActorRotation().Yaw;
		BodyFacingState.TurnDirection = 0;
		bChanged = true;
	}
	const float Now = GetBodyFacingServerTime();
	if (bStationary)
	{
		if (BodyFacingState.TurnDirection != 0 && Now - BodyFacingState.TurnStartedServerTime >= TurnDuration)
		{
			BodyFacingState.FacingYaw = FRotator::NormalizeAxis(BodyFacingState.FacingYaw + BodyFacingState.TurnDirection * 90.0f);
			BodyFacingState.TurnDirection = 0;
			bChanged = true;
		}
		const float Difference = FMath::FindDeltaAngleDegrees(BodyFacingState.FacingYaw, GetActorRotation().Yaw);
		if (BodyFacingState.TurnDirection == 0 && FMath::Abs(Difference) > TurnTriggerYaw)
		{
			BodyFacingState.TurnDirection = Difference > 0.0f ? 1 : -1;
			BodyFacingState.TurnStartedServerTime = Now;
			bChanged = true;
		}
	}
	if (bChanged) ForceNetUpdate();
}

// 客户端先绑定装备事件，再读取弹药快照；武器快照较晚到达时还会补发弹药变化。
void AfpstrueCharacter::OnRep_EquippedWeapon()
{
	// 生命快照可以先于装备引用到达；迟到的装备不能把死亡角色的第一人称手臂重新打开。
	Mesh1P->SetHiddenInGame(IsDead() || EquippedWeaponComponent == nullptr || !IsLocallyControlled(), true);
	OnEquippedWeaponChanged.Broadcast(EquippedWeaponComponent);
	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->RefreshNetworkEquipment();
	}
}

// 先应用服务器互斥结果再通知本地动画，远端不操作本机 HUD。
void AfpstrueCharacter::OnRep_MovementModifiers()
{
	ApplyMovementSpeed();
#if !UE_BUILD_SHIPPING
	if (GetNetMode() == NM_Client && !IsLocallyControlled()
		&& FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
	{
		// 接收端只验证服务器派生的姿态与步速，不从远端显示镜像激活能力。
		if (bIsSprinting && !bIsAiming && FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, SprintSpeed))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_REMOTE_SPRINT PASS Pawn=%s Speed=%.1f"),
				*GetName(), GetCharacterMovement()->MaxWalkSpeed);
		if (bIsAiming && !bIsSprinting && FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, AimWalkSpeed))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_REMOTE_AIM PASS Pawn=%s Speed=%.1f"),
				*GetName(), GetCharacterMovement()->MaxWalkSpeed);
	}
#endif

}

// 沿用原生命组件作为唯一 ASC，外部 GE、能力及调试工具都从同一接口取得它。
UAbilitySystemComponent* AfpstrueCharacter::GetAbilitySystemComponent() const
{
	return HealthComponent;
}

// ADS/冲刺可先在本地反馈，但服务端重新校验装备、死亡和换弹，不直接接受客户端速度。
// 上述校验由对应 GA 完成；本接口只读取 GAS 已成立的状态，不接纳客户端布尔值。
void AfpstrueCharacter::RefreshMovementAbilityState()
{
	const bool bNewAiming = HealthComponent != nullptr && HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming);
	const bool bNewSprinting = HealthComponent != nullptr && HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting);
	const bool bChanged = bIsAiming != bNewAiming || bIsSprinting != bNewSprinting;
	bIsAiming = bNewAiming;
	bIsSprinting = bNewSprinting;
	// ★【状态边界】先更新全部镜像和速度，再通知蓝图；表现不能构造另一套动作状态。
	OnRep_MovementModifiers();
	if (bChanged && HasAuthority())
	{
		ForceNetUpdate();
#if !UE_BUILD_SHIPPING
		if (HealthComponent != nullptr && GetNetMode() != NM_Standalone
			&& FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		{
			// 服务端从真实能力标签核对镜像与步速，不能仅凭客户端日志证明网络请求已生效。
			const bool bAimTag = HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Aiming);
			const bool bSprintTag = HealthComponent->HasMatchingGameplayTag(FpstrueGameplayTags::State_Sprinting);
			if (bSprintTag && !bAimTag && bIsSprinting && !bIsAiming
				&& FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, SprintSpeed))
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_MOVEMENT PASS=Sprint Pawn=%s Speed=%.1f"),
					*GetName(), GetCharacterMovement()->MaxWalkSpeed);
			if (bAimTag && !bSprintTag && bIsAiming && !bIsSprinting
				&& FMath::IsNearlyEqual(GetCharacterMovement()->MaxWalkSpeed, AimWalkSpeed))
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_MOVEMENT PASS=Aim Pawn=%s Speed=%.1f"),
					*GetName(), GetCharacterMovement()->MaxWalkSpeed);
		}
#endif
	}
}

// 退出关卡、切换地图或 Actor 被销毁都会进入这里；先停止外部回调，再交给基类释放 Actor。
void AfpstrueCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// WeaponComponent 属于拾取 Actor，不能只停开火后等待玩家自动销毁它。
	bEndingPlay = true;
	FWorldDelegates::OnWorldPostActorTick.Remove(HitHistoryPostTickHandle);
	// Actor::Destroy 后可能仍等待 GC；退出玩法时即释放历史帧的堆内存，不让尸体保留旧缓存。
	ResetHitHistory(true);
#if !UE_BUILD_SHIPPING
	ReleaseNetworkSmokeMovementKey();
	GetWorldTimerManager().ClearTimer(NetworkSmokeTimer);
#endif
	ClearEquippedWeaponComponent(EquippedWeaponComponent);
	if (HealthComponent != nullptr) HealthComponent->CancelAllAbilities();
	RefreshMovementAbilityState();
	RemoveInputMappingContexts();

	if (HealthComponent != nullptr)
	{
		// 显式解绑，避免组件销毁顺序变化时继续回调正在退出的角色。
		HealthComponent->OnDamageReceived.RemoveDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueCharacter::HandleDeath);
	}
	Super::EndPlay(EndPlayReason);
}

// ==================== 控制器切换与输入绑定 ====================

// 控制权移交：归还旧视角限制/输入 -> 取消旧持续动作 -> 刷新 ASC ActorInfo -> 安装新本地输入映射。
void AfpstrueCharacter::NotifyControllerChanged()
{
	// 放弃控制权时释放本角色的视角范围，不把最后一次站立限角遗留给旁观相机。
	if (APlayerController* Previous = Cast<APlayerController>(PreviousController); UsesNativePresentation() && Previous != Controller
		&& Previous && Previous->IsLocalController() && (Previous->GetPawn() == nullptr || Previous->GetPawn() == this) && Previous->PlayerCameraManager)
	{
		const APlayerCameraManager* Defaults = Previous->PlayerCameraManager->GetClass()->GetDefaultObject<APlayerCameraManager>();
		Previous->PlayerCameraManager->ViewPitchMin = Defaults->ViewPitchMin;
		Previous->PlayerCameraManager->ViewPitchMax = Defaults->ViewPitchMax;
		Previous->PlayerCameraManager->ViewYawMin = Defaults->ViewYawMin;
		Previous->PlayerCameraManager->ViewYawMax = Defaults->ViewYawMax;
	}
#if !UE_BUILD_SHIPPING
	ReleaseNetworkSmokeMovementKey();
#endif
	TGuardValue<bool> InputTransitionGuard(bOwnerInputTransition, true);
	if (HealthComponent != nullptr) HealthComponent->CancelAllAbilities();
	// Controller 变化时先清理旧控制器留下的持续输入状态。
	if (EquippedWeaponComponent != nullptr) EquippedWeaponComponent->InterruptOwnerInput();
	StopJumping();
	RefreshMovementAbilityState();
	RemoveInputMappingContexts();
	Super::NotifyControllerChanged();
	// ASC 的本地控制器信息随占有迁移刷新，不能一直使用组件 BeginPlay 时的空控制器。
	if (HealthComponent != nullptr) HealthComponent->InitAbilityActorInfo(this, this);
	RefreshMovementAbilityState();

	// Controller 变化后重新取得 LocalPlayer 的 Enhanced Input 子系统；弱引用只记录归属，不拥有它。
	APlayerController* PlayerController = Cast<APlayerController>(Controller);
	// 限制真实控制视角而非只夹住动画；鼠标、后坐力都经 CameraManager 同一入口，水平仍可连续转身。
	if (UsesNativePresentation() && PlayerController != nullptr && PlayerController->IsLocalController()
		&& PlayerController->PlayerCameraManager != nullptr)
	{
		PlayerController->PlayerCameraManager->ViewPitchMin = -ViewPitchLimit;
		PlayerController->PlayerCameraManager->ViewPitchMax = ViewPitchLimit;
	}
	UEnhancedInputLocalPlayerSubsystem* Subsystem = PlayerController != nullptr
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()) : nullptr;
	BoundInputSubsystem = Subsystem;
	if (Subsystem != nullptr && DefaultMappingContext != nullptr)
	{
		// 只记录自己新加的映射；移交/销毁时不能删除其他系统早已安装的相同 Context。
		AppliedMappingContext = DefaultMappingContext.Get();
		bAddedMappingContext = !Subsystem->HasMappingContext(DefaultMappingContext);
		if (bAddedMappingContext) Subsystem->AddMappingContext(DefaultMappingContext, 0);
	}
}

// 输入绑定注册：连续轴使用 Triggered，普通按住动作成对处理 Started/Completed，切换和命令只响应 Started。
void AfpstrueCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (EnhancedInputComponent == nullptr)
	{
		UE_LOG(LogTemplateCharacter, Error,
			   TEXT("%s (%s): Enhanced Input Component is required by this character."), *GetName(), *GetClass()->GetPathName());
		return;
	}

	// 所有可配置 InputAction 使用同一校验和日志格式；单个资产缺失不会阻止其他动作完成绑定。
	const auto IsActionAssigned = [this](const UInputAction* Action, const TCHAR* PropertyName)
	{
		if (Action != nullptr)
		{
			return true;
		}

		UE_LOG(LogTemplateCharacter, Error, TEXT("%s (%s): %s is not assigned."), *GetName(), *GetClass()->GetPathName(), PropertyName);
		return false;
	};
	// 为单个触发阶段绑定处理函数；校验只影响该 Action，不中断其他输入注册。
	const auto BindInput = [this, EnhancedInputComponent, &IsActionAssigned](const UInputAction* Action, const TCHAR* Name,
		ETriggerEvent Trigger, auto Handler)
	{
		if (IsActionAssigned(Action, Name))
		{
			EnhancedInputComponent->BindAction(Action, Trigger, this, Handler);
		}
	};
	// 按住动作同时绑定正常松开和取消，防止输入条件撤销后留下持续动作。
	const auto BindHeldInput = [this, EnhancedInputComponent, &IsActionAssigned](const UInputAction* Action, const TCHAR* Name,
		auto Start, auto Stop)
	{
		if (!IsActionAssigned(Action, Name)) return;
		EnhancedInputComponent->BindAction(Action, ETriggerEvent::Started, this, Start);
		for (ETriggerEvent Release : {ETriggerEvent::Completed, ETriggerEvent::Canceled})
		{
			EnhancedInputComponent->BindAction(Action, Release, this, Stop);
		}
	};

	// 连续轴输入：Triggered 在触发条件满足期间逐帧调用；方向键一直按住时，即使值不变也持续移动。
	BindInput(MoveAction, TEXT("MoveAction"), ETriggerEvent::Triggered, &AfpstrueCharacter::Move);
	BindInput(LookAction, TEXT("LookAction"), ETriggerEvent::Triggered, &AfpstrueCharacter::Look);

	// 按住型输入：Completed 与 Canceled 都结束动作，覆盖 Trigger 条件变化和输入被撤销。
	BindHeldInput(JumpAction, TEXT("JumpAction"), &ACharacter::Jump, &ACharacter::StopJumping);
	BindHeldInput(FireAction, TEXT("FireAction"), &AfpstrueCharacter::StartWeaponFire, &AfpstrueCharacter::StopWeaponFire);
	BindHeldInput(AimAction, TEXT("AimAction"), &AfpstrueCharacter::StartAim, &AfpstrueCharacter::StopAim);

	// 切换型输入：SprintAction 每次按下切换一次，不在松开时自动停止。
	BindInput(SprintAction, TEXT("SprintAction"), ETriggerEvent::Started, &AfpstrueCharacter::ToggleSprint);

	// 单次命令：换弹只提交能力请求；单机由身份化动画/显式完成，联网由服务器计时完成。
	BindInput(ReloadAction, TEXT("ReloadAction"), ETriggerEvent::Started, &AfpstrueCharacter::RequestWeaponReload);
}

// 只移除本角色实际添加的映射，保留其他系统拥有的同名 Context。
void AfpstrueCharacter::RemoveInputMappingContexts()
{
	// Controller 更换或角色退出时成对移除本角色添加的映射，并清空不拥有对象生命周期的弱引用。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
	{
#if !UE_BUILD_SHIPPING
		if (bNetworkSmokeInjectingMove)
		{
			Subsystem->StopContinuousInputInjectionForAction(MoveAction);
			bNetworkSmokeInjectingMove = false;
		}
#endif
		if (bAddedMappingContext && AppliedMappingContext.IsValid())
		{
			Subsystem->RemoveMappingContext(AppliedMappingContext.Get());
		}
	}
	// 清空弱引用，防止下一次 Controller 切换误用旧子系统。
	BoundInputSubsystem.Reset();
	AppliedMappingContext.Reset();
	bAddedMappingContext = false;
}

// ==================== 移动与视角输入 ====================

// 接收连续移动轴并提交前后、左右方向输入，实际位移与碰撞由 CharacterMovement 处理。
void AfpstrueCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr && CanAcceptGameplayInput())
	{
		//X:左右，Y:前后
		AddMovementInput(GetActorForwardVector(), MovementVector.Y);
		AddMovementInput(GetActorRightVector(), MovementVector.X);
	}
}

// 将连续视角输入交给控制器；无控制权、死亡或退出期间不改变控制旋转。
void AfpstrueCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr && CanAcceptGameplayInput())
	{
		//X：摇头  Y：点头
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

// 按键切换冲刺能力；互斥由能力标签检查，角色经唯一速度入口应用结果。
void AfpstrueCharacter::ToggleSprint()
{
	// 互斥条件在 GA 中集中声明；角色只把切换输入转成激活或取消当前能力。
	if (HealthComponent == nullptr || !CanAcceptGameplayInput() || (!HasAuthority() && !IsLocallyControlled())) return;
	// 用当前能力是否激活决定本次按键是取消还是请求冲刺，不另写冲刺状态。
	const FGameplayAbilitySpec* Sprint = HealthComponent->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass());
	if (Sprint != nullptr && Sprint->IsActive()) HealthComponent->CancelAbilityHandle(Sprint->Handle);
	else
	{
		HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Sprint));
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Sprint));
	}
}

// ==================== 瞄准 ====================

// 提交瞄准按下；能力验证装备并取消冲刺，状态成立后才通知蓝图。
void AfpstrueCharacter::StartAim()
{
	// 角色只检查输入归属；装备资格和动作互斥由 Aim GA 判断。
	if (HealthComponent == nullptr || !CanAcceptGameplayInput() || (!HasAuthority() && !IsLocallyControlled())) return;

	HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Aim));
}

// 幂等退出瞄准并重算步速；蓝图通知只在真实状态变化时发送。
void AfpstrueCharacter::StopAim()
{
	// 松开不再检查激活资格；WaitInputRelease 同步释放，GAS 结束后统一派生姿态与步速。
	if (HealthComponent != nullptr)
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
}

// ==================== 武器输入与装备关系 ====================

// 这里是输入边界：Character 不直接扣弹、射线检测或改变动作标签，只向 ASC 提交能力输入。
void AfpstrueCharacter::StartWeaponFire()
{
	// 与换弹入口一致：Character 只转发能力输入，装备资格和动作互斥由 GA 校验。
	if (HealthComponent != nullptr && CanAcceptGameplayInput() && (HasAuthority() || IsLocallyControlled()))
	{
		HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
	}
}

// 将松开或取消输入转交 ASC，只结束开火能力，不结束正在进行的换弹。
void AfpstrueCharacter::StopWeaponFire()
{
	// 松开输入、死亡、换 Controller 和 EndPlay 都可安全调用，由 GAS 幂等处理输入释放。
	if (HealthComponent != nullptr)
	{
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
	}
}

// 向 ASC 提交一次换弹请求；GA 校验动作规则，武器负责装弹与事务结束时机。
void AfpstrueCharacter::RequestWeaponReload()
{
	// 手动与自动换弹共用 Reload 能力，由能力取消瞄准/冲刺，武器只校验真实弹药与装备资源。
	if (HealthComponent != nullptr && CanAcceptGameplayInput() && (HasAuthority() || IsLocallyControlled()))
	{
		HealthComponent->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Reload));
		HealthComponent->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Reload));
	}
}

// 判断角色是否仍允许维持装备关系；临时失去输入与角色死亡分别处理。
bool AfpstrueCharacter::CanMaintainEquipment() const
{
	return !bEndingPlay && !IsActorBeingDestroyed() && !IsDead();
}

// 在存活与生命周期资格上增加控制权迁移保护，拒绝迁移回调中的新输入动作。
bool AfpstrueCharacter::CanAcceptGameplayInput() const
{
	return CanMaintainEquipment() && !bOwnerInputTransition;
}

// 唯一写入 MaxWalkSpeed 的入口，按瞄准优先、其次冲刺、最后步行选择速度。
void AfpstrueCharacter::ApplyMovementSpeed()
{
	GetCharacterMovement()->MaxWalkSpeed = bIsAiming ? AimWalkSpeed : (bIsSprinting ? SprintSpeed : WalkSpeed);
	if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(GetCharacterMovement()))
		Movement->SetMovementIntent(bIsAiming, bIsSprinting);
}

// 装备成功后由 WeaponComponent 回调本函数；这里仅登记关系并广播，避免角色与武器各维护一份弹药状态。
void AfpstrueCharacter::SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent)
{
	if (WeaponComponent == nullptr || EquippedWeaponComponent == WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent = WeaponComponent;
	ForceNetUpdate();
	Mesh1P->SetHiddenInGame(false, true);
	OnEquippedWeaponChanged.Broadcast(WeaponComponent);
}

// 只转交仍匹配的装备执行完整卸下；不能绕过武器取消/清理而直接清角色槽位。
void AfpstrueCharacter::ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent->DetachWeapon();
}

// 卸枪事务完成后释放匹配槽位；先提交空槽，再发事件，防止旧回调覆盖新装备。
void AfpstrueCharacter::ReleaseEquippedWeaponComponent(const UfpstrueWeaponComponent* ExpectedWeapon)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != ExpectedWeapon) return;
	// ★【重要边界】[资源所有权] 只释放调用者匹配的装备槽；先置空再退出瞄准，旧卸枪栈不能覆盖回调中装备的新武器。
	EquippedWeaponComponent = nullptr;
	ForceNetUpdate();
	Mesh1P->SetHiddenInGame(true, true);
	StopAim();
	// StopAim 可同步触发 GAS 取消回调并换装；此时不要继续广播过期的空槽快照。
	if (EquippedWeaponComponent == nullptr) OnEquippedWeaponChanged.Broadcast(nullptr);
}

// ==================== 生命与伤害事件 ====================

// Health 只报告权威端实际扣除的正伤害；这里记录计分来源并分发表现，不再次结算。
void AfpstrueCharacter::HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	if (!HasAuthority()) return;
	LastDamageInstigator = InstigatedBy;
	if (UsesNativePresentation() && !IsDead()) MulticastPresentHit();

}

// 每个视角只消费服务器已发生的受击事实；无可靠积压，不给晚加入者重放历史受击。
void AfpstrueCharacter::MulticastPresentHit_Implementation()
{
	for (USkeletalMeshComponent* PresentationMesh : {GetMesh(), Mesh1P.Get()})
		if (UfpstruePlayerAnimInstance* Animation = Cast<UfpstruePlayerAnimInstance>(PresentationMesh->GetAnimInstance())) Animation->PresentHit();
}

// 一次性结束玩家玩法：撤销移动修饰、禁用武器和移动，再通知对局与死亡表现。
void AfpstrueCharacter::HandleDeath()
{
	// HealthComponent 已保证 OnDeath 只广播一次；本地标志再保护角色侧移动、武器和蓝图表现不被重复执行。
	if (bDeathEffectsApplied)
	{
		return;
	}

	// 先锁存角色侧死亡处理，再清理输入和装备；清理回调不能重复进入同一死亡链。
	bDeathEffectsApplied = true;
	const FVector DeathVelocity = GetVelocity();
	ResetHitHistory();
#if !UE_BUILD_SHIPPING
	ReleaseNetworkSmokeMovementKey();
	if (bNetworkSmokeInjectingMove)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
			Subsystem->StopContinuousInputInjectionForAction(MoveAction);
		bNetworkSmokeInjectingMove = false;
	}
#endif
	// Health 在提交死亡事实（含客户端初始死亡快照）时已取消能力；这里仅消费结果。
	RefreshMovementAbilityState();

	if (EquippedWeaponComponent != nullptr)
	{
		if (UsesNativePresentation()) EquippedWeaponComponent->DropForDeath(DeathVelocity);
		else EquippedWeaponComponent->DisableWeapon();
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}
	if (UsesNativePresentation())
	{
		// 死亡先退出动作播放，再交给同套身体 PhysicsAsset；布娃娃只做表现，不产生新的伤害或计分。
		Mesh1P->SetHiddenInGame(true, true);
		for (USkeletalMeshComponent* PresentationMesh : {GetMesh(), Mesh1P.Get()})
			if (UAnimInstance* Animation = PresentationMesh->GetAnimInstance()) Animation->Montage_Stop(0.0f);
		if (GetMesh()->GetPhysicsAsset() != nullptr)
		{
			// 同一次死亡处理完成松手与身体物理；不等动画结束或另一个 Tick，跨端到达仍以网络为准。
			GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			// 等待重生时仍使用本人的第一人称相机；保留 OwnerNoSee，避免从布娃娃体内看见自己。
			// 该标志不隐藏其他玩家看到的尸体，也不停止其真实物理。
			GetMesh()->SetCollisionProfileName(TEXT("Ragdoll"));
			GetMesh()->SetSimulatePhysics(true);
			GetMesh()->SetAllPhysicsLinearVelocity(DeathVelocity);
		}
	}

#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpPresentationAudit")) && UsesNativePresentation())
	{
		const TWeakObjectPtr<UfpstrueWeaponComponent> Gun = EquippedWeaponComponent;
		UE_LOG(LogTemp, Display, TEXT("PVP_DEATH_DROP Begin Authority=%d BodySim=%d GunSim=%d Detached=%d Velocity=%s"),
			HasAuthority(), GetMesh()->IsSimulatingPhysics(), Gun.IsValid() && Gun->IsSimulatingPhysics(),
			Gun.IsValid() && Gun->GetAttachParent() == nullptr, *DeathVelocity.ToString());
		FTimerHandle PhysicsAuditTimer;
		GetWorldTimerManager().SetTimer(PhysicsAuditTimer, FTimerDelegate::CreateWeakLambda(this, [this, Gun]()
		{
			if (Gun.IsValid()) UE_LOG(LogTemp, Display, TEXT("PVP_DEATH_DROP Settling Authority=%d BodySim=%d GunSim=%d Detached=%d GunLocation=%s GunVelocity=%s"),
				HasAuthority(), GetMesh()->IsSimulatingPhysics(), Gun->IsSimulatingPhysics(), Gun->GetAttachParent() == nullptr,
				*Gun->GetComponentLocation().ToString(), *Gun->GetPhysicsLinearVelocity().ToString());
		}), 0.8f, false);
	}
	// 显式死亡画面验收：只暂停测试输入，不暂停世界/物理；记录真实网络死亡后枪与身体的连续状态。
	if (GetNetMode() == NM_Client && !IsLocallyControlled() && UsesNativePresentation()
		&& FParse::Param(FCommandLine::Get(), TEXT("PvpDeathVisual")))
	{
		APlayerController* Viewer = UGameplayStatics::GetPlayerController(this, 0);
		AfpstrueCharacter* ViewerPawn = Viewer != nullptr ? Cast<AfpstrueCharacter>(Viewer->GetPawn()) : nullptr;
		FString Directory;
		if (ViewerPawn != nullptr && ViewerPawn->IsLocallyControlled() && !ViewerPawn->IsDead()
			&& FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), Directory) && !Directory.IsEmpty())
		{
			ViewerPawn->StopWeaponFire();
			GetWorldTimerManager().ClearTimer(ViewerPawn->NetworkSmokeTimer);
			const FVector LookAt = GetActorLocation() - FVector(0.0, 0.0, 55.0);
			Viewer->SetControlRotation((LookAt - ViewerPawn->FirstPersonCameraComponent->GetComponentLocation()).Rotation());
			const TWeakObjectPtr<UfpstrueWeaponComponent> DroppedWeapon = EquippedWeaponComponent;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FTimerHandle CaptureTimer;
				const float Delay = Index == 0 ? 0.1f : Index == 1 ? 0.65f : 1.6f;
				GetWorldTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateWeakLambda(this,
					[this, DroppedWeapon, Directory, Index]()
					{
						UfpstrueWeaponComponent* Gun = DroppedWeapon.Get();
						if (!IsDead() || Gun == nullptr || !Gun->IsDroppedOnDeath()) return;
						UE_LOG(LogTemp, Display, TEXT("PVP_DEATH_VISUAL Frame=%d BodySim=%d GunSim=%d Detached=%d GunLocation=%s BodyLocation=%s"),
							Index, GetMesh()->IsSimulatingPhysics(), Gun->IsSimulatingPhysics(), Gun->GetAttachParent() == nullptr,
							*Gun->GetComponentLocation().ToString(), *GetMesh()->GetSocketLocation(TEXT("pelvis")).ToString());
						if (!FScreenshotRequest::IsScreenshotRequested())
							FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString::Printf(TEXT("death_%d.png"), Index)), true, false, false);
					}), Delay, false);
			}
		}
	}
#endif

	// 玩法已经停止后才通知对局和蓝图；这些监听者可能同步切图或结束比赛。
	OnPlayerDeathReported.Broadcast(this);
	if (!UsesNativePresentation()) GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// 本地预览也是权威世界，和监听/专服共用同一计分与重生入口；网络模式不再选择另一套死亡玩法。
	if (HasAuthority())
	{
		if (AfpstruePvpGameMode* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>())
			Mode->HandlePlayerEliminated(this, LastDamageInstigator.Get());
	}
}


// 从生命组件读取死亡事实，角色不维护另一份可变血量或死亡判定。
bool AfpstrueCharacter::IsDead() const
{
	return HealthComponent != nullptr && HealthComponent->IsDead();
}

// 为 HUD 和其他模块提供当前血量的只读快照；没有组件时返回零。
float AfpstrueCharacter::GetCurrentHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
}

// 读取生命组件的最大血量，角色不复制或改写该配置。
float AfpstrueCharacter::GetMaxHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetMaxHealth() : 0.0f;
}

// 复用组件的归一化结果，HUD 不必自行计算比例或处理除零。
float AfpstrueCharacter::GetHealthNormalized() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealthNormalized() : 0.0f;
}
