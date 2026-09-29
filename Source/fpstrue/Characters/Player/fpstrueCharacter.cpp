// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Engine/LocalPlayer.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

/*
 * 玩家角色是“输入与表现协调层”，不拥有武器弹药和生命值这两类业务状态：
 *
 *   Enhanced Input -> Character 做角色级前置校验 -> WeaponComponent 执行射击/换弹事务
 *   ApplyDamage -> HealthComponent 修改生命值 -> Delegate 回到 Character -> 蓝图/HUD 表现
 *
 * 这样角色只保存瞄准、冲刺和当前装备关系；武器状态归 WeaponComponent，生命状态归 HealthComponent。
 * 本类不启用逐帧 Tick，持续开火、换弹超时和后坐力恢复由武器自己的 Timer 管理。
 */

// ==================== 默认组件与生命周期 ====================

// 构造角色的胶囊体、第一人称相机、手臂网格和可复用生命组件；构造阶段不访问运行时世界。
AfpstrueCharacter::AfpstrueCharacter()
{
	//胶囊体
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);

	//弹簧臂创建、挂载、位置、长度
	//**构造函数中创建Actor默认组件
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	CameraBoom->SetRelativeLocation(FVector(-10.f, 0.f, 60.f));
	// 第一人称相机臂长为 0，不需要弹簧臂碰撞回缩。
	CameraBoom->TargetArmLength = 0.0f;
	//旋转跟随
	CameraBoom->bUsePawnControlRotation = true;
	//平滑延迟效果（位置、旋转延迟跟随）
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 3.0f;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 3.0f;

	//相机创建、挂载、**关闭自身控制**
	FirstPersonCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
	FirstPersonCameraComponent->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FirstPersonCameraComponent->bUsePawnControlRotation = false;

	//网格体创建、可见性、挂载、关闭动静态阴影投射、
	Mesh1P = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh1P"));
	Mesh1P->SetRelativeLocation(FVector(-30.f, 0.f, -150.f));
	Mesh1P->SetOnlyOwnerSee(true);
	Mesh1P->SetupAttachment(FirstPersonCameraComponent);
	Mesh1P->bCastDynamicShadow = false;
	Mesh1P->CastShadow = false;

	//禁用角色自身 Tick：输入、碰撞、动画通知等事件驱动角色逻辑；Movement、相机和 Mesh 组件仍各自更新。
	PrimaryActorTick.bCanEverTick = false;
	//自定义健康组件
	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
}

void AfpstrueCharacter::BeginPlay()
{
	//开始游戏时基函数调用
	Super::BeginPlay();

	if (HealthComponent != nullptr)
	{
		//订阅事件（受攻击、掉血、死亡）
		HealthComponent->OnHealthChanged.AddUniqueDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueCharacter::HandleDeath);
		// Component 的 BeginPlay 早于 Owner；绑定后主动同步一次初始快照，避免 HUD 错过首次广播。
		HandleHealthChanged(HealthComponent->GetHealth());
	}

	//**开局没有捡到枪的时候先隐藏mesh1P
	const bool bHasWeapon = EquippedWeaponComponent != nullptr;
	Mesh1P->SetHiddenInGame(!bHasWeapon, true);
	// 初始化基础移动速度；后续只在冲刺和瞄准状态切换时修改。
	ApplyMovementSpeed();
}

// 退出关卡、切换地图或 Actor 被销毁都会进入这里；先停止外部回调，再交给基类释放 Actor。
void AfpstrueCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// WeaponComponent 属于拾取 Actor，不能只停开火后等待玩家自动销毁它。
	bEndingPlay = true;
	ClearEquippedWeaponComponent(EquippedWeaponComponent);
	ResetMovementModifiers();
	RemoveInputMappingContexts();

	if (HealthComponent != nullptr)
	{
		// 显式解绑，避免组件销毁顺序变化时继续回调正在退出的角色。
		HealthComponent->OnHealthChanged.RemoveDynamic(this, &AfpstrueCharacter::HandleHealthChanged);
		HealthComponent->OnDamageReceived.RemoveDynamic(this, &AfpstrueCharacter::HandleDamageReceived);
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueCharacter::HandleDeath);
	}
	//最后调用基类
	Super::EndPlay(EndPlayReason);
}

// ==================== 控制器切换与输入绑定 ====================

//玩家控制变更（游戏开始、角色切换）
void AfpstrueCharacter::NotifyControllerChanged()
{
	TGuardValue<bool> InputTransitionGuard(bOwnerInputTransition, true);
	// Controller 变化时先清理旧控制器留下的持续输入状态。
	if (EquippedWeaponComponent != nullptr) EquippedWeaponComponent->InterruptOwnerInput();
	StopJumping();
	ResetMovementModifiers();
	RemoveInputMappingContexts();
	//执行基类
	Super::NotifyControllerChanged();

	// Controller 变化后重新取得 LocalPlayer 的 Enhanced Input 子系统；弱引用只记录归属，不拥有它。
	APlayerController* PlayerController = Cast<APlayerController>(Controller);
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
	const auto BindInput = [this, EnhancedInputComponent, &IsActionAssigned](const UInputAction* Action, const TCHAR* Name,
		ETriggerEvent Trigger, auto Handler)
	{
		if (IsActionAssigned(Action, Name))
		{
			EnhancedInputComponent->BindAction(Action, Trigger, this, Handler);
		}
	};
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

	// 单次命令：换弹只提交请求，完成时机由武器状态和动画 Notify 决定。
	BindInput(ReloadAction, TEXT("ReloadAction"), ETriggerEvent::Started, &AfpstrueCharacter::RequestWeaponReload);
}

void AfpstrueCharacter::RemoveInputMappingContexts()
{
	// Controller 更换或角色退出时成对移除本角色添加的映射，并清空不拥有对象生命周期的弱引用。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
	{
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

void AfpstrueCharacter::Move(const FInputActionValue& Value)
{
	// 将 Enhanced Input 的二维值映射到角色本地前向和右向，实际位移由 CharacterMovement 完成。
	//获得向量
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr && CanAcceptGameplayInput())
	{
		// 有Controller，交给移动组件移动;
		//X:左右，Y:前后
		AddMovementInput(GetActorForwardVector(), MovementVector.Y);
		AddMovementInput(GetActorRightVector(), MovementVector.X);
	}
}

void AfpstrueCharacter::Look(const FInputActionValue& Value)
{
	// 将二维视角输入写入 Controller 的 Yaw/Pitch，角色与相机最终读取控制旋转。
	//获得向量
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr && CanAcceptGameplayInput())
	{
		//有Controller，交给修改旋转;
		//X：摇头  Y：点头
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

void AfpstrueCharacter::ToggleSprint()
{
	// 死亡、换弹和瞄准期间不能切换冲刺；生命周期和换弹统一重置移动修饰状态。
	const bool bWeaponReloading = EquippedWeaponComponent != nullptr && EquippedWeaponComponent->IsReloading();
	if (!CanAcceptGameplayInput() || bWeaponReloading || bIsAiming)
	{
		return;
	}
	//标记状态实现sprint、speed的优化
	bIsSprinting = !bIsSprinting;
	ApplyMovementSpeed();
}

// ==================== 瞄准 ====================

void AfpstrueCharacter::StartAim()
{
	//检查状态避免状态冲突
	const bool bWeaponReloading = EquippedWeaponComponent != nullptr && EquippedWeaponComponent->IsReloading();
	if (!CanAcceptGameplayInput() || EquippedWeaponComponent == nullptr || bWeaponReloading || bIsAiming)
	{
		return;
	}

	//检查瞄准前置条件
	bIsAiming = true;
	bIsSprinting = false;
	ApplyMovementSpeed();
	OnAimChanged(true);
}

void AfpstrueCharacter::StopAim()
{
	// 无条件清除瞄准状态；只有确实发生状态变化时才通知蓝图并恢复移动速度。
	//保留原来状态
	const bool bWasAiming = bIsAiming;
	//无条件复位
	bIsAiming = false;
	ApplyMovementSpeed();

	//原来是在瞄准的话，修改状态
	if (bWasAiming)
	{
		OnAimChanged(false);
	}
}

// ==================== 武器输入与装备关系 ====================

// 武器交互
// 这里是输入边界：Character 不直接扣弹、射线检测或改变武器动作状态，只把请求交给当前装备组件。
void AfpstrueCharacter::StartWeaponFire()
{
	// Character 只校验装备与生存状态，弹药和武器动作互斥由 WeaponComponent 负责。
	if (EquippedWeaponComponent != nullptr && CanAcceptGameplayInput())
	{
		//转入weapon
		EquippedWeaponComponent->StartFire();
	}
}

void AfpstrueCharacter::StopWeaponFire()
{
	// 松开输入、死亡、换 Controller 和 EndPlay 都可安全调用，武器组件负责内部幂等收口。
	if (EquippedWeaponComponent != nullptr)
	{
		//转入weapon
		EquippedWeaponComponent->StopFire();
	}
}

void AfpstrueCharacter::RequestWeaponReload()
{
	// 请求规则只由武器判断；手动与自动换弹都在接纳后统一重置瞄准/冲刺。
	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->RequestReload();
	}
}

void AfpstrueCharacter::ResetMovementModifiers()
{
	// 换弹、控制权迁移和 EndPlay 共用：先清冲刺并重算步速，再广播瞄准变化。
	// StopAim 的蓝图回调返回后不再写状态，避免覆盖监听者合法建立的新状态。
	bIsSprinting = false;
	StopAim();
}

bool AfpstrueCharacter::CanMaintainEquipment() const
{
	return !bEndingPlay && !IsActorBeingDestroyed() && !IsDead();
}

bool AfpstrueCharacter::CanAcceptGameplayInput() const
{
	return CanMaintainEquipment() && !bOwnerInputTransition;
}

void AfpstrueCharacter::ApplyMovementSpeed()
{
	GetCharacterMovement()->MaxWalkSpeed = bIsAiming ? AimWalkSpeed : (bIsSprinting ? SprintSpeed : WalkSpeed);
}

//装备枪支，可见性设置
// 装备成功后由 WeaponComponent 回调本函数；这里仅登记关系并广播，避免角色与武器各维护一份弹药状态。
void AfpstrueCharacter::SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent)
{
	if (WeaponComponent == nullptr || EquippedWeaponComponent == WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent = WeaponComponent;
	Mesh1P->SetHiddenInGame(false, true);
	OnEquippedWeaponChanged.Broadcast(WeaponComponent);
	// 广播可同步卸下或结束角色生命周期；旧装备请求不能再发送“装备完成”表现。
	if (!bEndingPlay && !IsDead() && EquippedWeaponComponent == WeaponComponent)
	{
		OnWeaponEquipped(WeaponComponent);
	}
}

//清除枪支、禁止开火、可见性设置
void AfpstrueCharacter::ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != WeaponComponent)
	{
		return;
	}

	EquippedWeaponComponent->DetachWeapon();
}

void AfpstrueCharacter::ReleaseEquippedWeaponComponent(const UfpstrueWeaponComponent* ExpectedWeapon)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != ExpectedWeapon) return;
	EquippedWeaponComponent = nullptr;
	Mesh1P->SetHiddenInGame(true, true);
	StopAim();
	// OnAimChanged 也能装备新武器；此时不要继续广播过期的空槽快照。
	if (EquippedWeaponComponent == nullptr) OnEquippedWeaponChanged.Broadcast(nullptr);
}

// ==================== 生命与伤害事件 ====================

// 生命与伤害
//生命组件内部事件转发给Character的蓝图表现层，用于UI和动画的更新
// HealthComponent 是生命值唯一写入者，Character 只做 C++ Gameplay -> 蓝图表现层的事件桥接。
void AfpstrueCharacter::HandleHealthChanged(float NewHealth)
{
	OnPlayerHealthChanged(NewHealth);
}

void AfpstrueCharacter::HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	OnPlayerDamaged(DamageAmount, DamageCauser, InstigatedBy);
}

void AfpstrueCharacter::HandleDeath()
{
	// HealthComponent 已保证 OnDeath 只广播一次；本地标志再保护角色侧移动、武器和蓝图表现不被重复执行。
	if (bDeathEffectsApplied)
	{
		return;
	}

	bDeathEffectsApplied = true;
	bIsSprinting = false;

	const bool bWasAiming = bIsAiming;
	bIsAiming = false;
	if (bWasAiming)
	{
		OnAimChanged(false);
	}

	if (EquippedWeaponComponent != nullptr)
	{
		EquippedWeaponComponent->DisableWeapon();
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

	OnPlayerDeathReported.Broadcast(this);
	OnPlayerDied();
}

bool AfpstrueCharacter::IsDead() const
{
	// 玩家不保存第二份死亡状态，始终读取可复用 HealthComponent 的唯一事实。
	return HealthComponent != nullptr && HealthComponent->IsDead();
}

float AfpstrueCharacter::GetCurrentHealth() const
{
	// 为蓝图和其他模块提供空安全只读查询，不允许外部直接修改 HealthComponent 内部数值。
	return HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
}

float AfpstrueCharacter::GetMaxHealth() const
{
	// 最大生命值同样通过组件只读接口暴露，保持生命数据所有权集中。
	return HealthComponent != nullptr ? HealthComponent->GetMaxHealth() : 0.0f;
}

float AfpstrueCharacter::GetHealthNormalized() const
{
	// 返回组件计算后的 0~1 比例，HUD 无需自行重复处理除零和范围钳制。
	return HealthComponent != nullptr ? HealthComponent->GetHealthNormalized() : 0.0f;
}
