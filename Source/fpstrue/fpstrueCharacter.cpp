// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstrueWeaponComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
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

// 生命周期
AfpstrueCharacter::AfpstrueCharacter()
{ //胶囊体
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

	//禁用tick（逻辑完全由事件驱动（输入、碰撞、动画通知等））
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
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

// 退出关卡、切换地图或 Actor 被销毁都会进入这里；先停止外部回调，再交给基类释放 Actor。
void AfpstrueCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 统一终止持续输入，并移除本角色添加的 Mapping Context。
	StopWeaponFire();
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

//玩家控制变更（游戏开始、角色切换）
void AfpstrueCharacter::NotifyControllerChanged()
{
	// Controller 变化时先清理旧控制器留下的持续输入状态。
	StopWeaponFire();
	RemoveInputMappingContexts();
	//执行基类
	Super::NotifyControllerChanged();

	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		//通过当前玩家的 PlayerController 获取对应的LocalPlayer，再从LocalPlayer中找到 Enhanced Input子系统，用来管理该玩家的输入映射
		//目的是：Controller变化后重新获取当前玩家对应的Enhanced Input管理器
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
				ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			//保存输入管理器引用，把输入配置加载进去
			//注意BoundInputSubsystem是弱指针
			BoundInputSubsystem = Subsystem;
			ApplyInputMappingContexts();
		}
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

	// 连续轴输入：只有输入值持续变化时才需要反复调用。
	if (IsActionAssigned(MoveAction, TEXT("MoveAction")))
	{
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Move);
	}
	if (IsActionAssigned(LookAction, TEXT("LookAction")))
	{
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Look);
	}

	// 按住型输入：当前均为普通 Boolean Action，Started 开启、Completed 松开，不配置未使用的 Canceled 分支。
	if (IsActionAssigned(JumpAction, TEXT("JumpAction")))
	{
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	}
	if (IsActionAssigned(FireAction, TEXT("FireAction")))
	{
		EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartWeaponFire);
		EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &AfpstrueCharacter::StopWeaponFire);
	}
	if (IsActionAssigned(AimAction, TEXT("AimAction")))
	{
		EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartAim);
		EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Completed, this, &AfpstrueCharacter::StopAim);
	}

	// 切换型输入：SprintAction 每次按下切换一次，不在松开时自动停止。
	if (IsActionAssigned(SprintAction, TEXT("SprintAction")))
	{
		EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Started, this, &AfpstrueCharacter::ToggleSprint);
	}

	// 单次命令：换弹只提交请求，完成时机由武器状态和动画 Notify 决定。
	if (IsActionAssigned(ReloadAction, TEXT("ReloadAction")))
	{
		EnhancedInputComponent->BindAction(ReloadAction, ETriggerEvent::Started, this, &AfpstrueCharacter::RequestWeaponReload);
	}
}

// Mapping Context 归 LocalPlayer 子系统所有；角色只记录自己添加过的上下文，便于换 Controller 时成对移除。
void AfpstrueCharacter::ApplyInputMappingContexts()
{
	// TWeakObjectPtr 不拥有子系统，使用 Get() 读取当前仍有效的对象。
	UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get();
	if (Subsystem == nullptr)
	{
		return;
	}
	//从保存的 Enhanced Input 子系统中取出输入管理器，
	// 如果有效，就把默认输入映射 DefaultMappingContext 加载进去，让玩家的按键重新生效。
	if (DefaultMappingContext != nullptr)
	{
		Subsystem->AddMappingContext(DefaultMappingContext, 0);
	}
}

void AfpstrueCharacter::RemoveInputMappingContexts()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
	{
		if (DefaultMappingContext != nullptr)
		{
			Subsystem->RemoveMappingContext(DefaultMappingContext);
		}
	}
	// 清空弱引用，防止下一次 Controller 切换误用旧子系统。
	BoundInputSubsystem.Reset();
}

// ==================== 移动与视角输入 ====================

void AfpstrueCharacter::Move(const FInputActionValue& Value)
{
	//获得向量
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// 有Controller，交给移动组件移动;
		//X:左右，Y:前后
		AddMovementInput(GetActorForwardVector(), MovementVector.Y);
		AddMovementInput(GetActorRightVector(), MovementVector.X);
	}
}

void AfpstrueCharacter::Look(const FInputActionValue& Value)
{
	//获得向量
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		//有Controller，交给修改旋转;
		//X：摇头  Y：点头
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}

void AfpstrueCharacter::ToggleSprint()
{
	// 死亡、换弹和瞄准期间不能切换冲刺；其他强制中断统一调用 StopSprint。
	const bool bWeaponReloading = EquippedWeaponComponent != nullptr && EquippedWeaponComponent->IsReloading();
	if (IsDead() || bWeaponReloading || bIsAiming)
	{
		return;
	}
	//标记状态实现sprint、speed的优化
	bIsSprinting = !bIsSprinting;
	GetCharacterMovement()->MaxWalkSpeed = bIsSprinting ? SprintSpeed : WalkSpeed;
}

void AfpstrueCharacter::StopSprint()
{
	//停止冲刺
	bIsSprinting = false;
	GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
}

// ==================== 瞄准 ====================

void AfpstrueCharacter::StartAim()
{
	//检查状态避免状态冲突
	const bool bWeaponReloading = EquippedWeaponComponent != nullptr && EquippedWeaponComponent->IsReloading();
	if (EquippedWeaponComponent == nullptr || IsDead() || bWeaponReloading || bIsAiming)
	{
		return;
	}

	//检查瞄准前置条件
	bIsAiming = true;
	bIsSprinting = false;
	GetCharacterMovement()->MaxWalkSpeed = AimWalkSpeed;
	OnAimChanged(true);
}

void AfpstrueCharacter::StopAim()
{
	//保留原来状态
	const bool bWasAiming = bIsAiming;
	//无条件复位
	bIsAiming = false;

	//原来是在瞄准的话，修改状态
	if (bWasAiming)
	{
		OnAimChanged(false);
		GetCharacterMovement()->MaxWalkSpeed = WalkSpeed;
	}
}

// 武器交互
// 这里是输入边界：Character 不直接扣弹、射线检测或改变武器动作状态，只把请求交给当前装备组件。
void AfpstrueCharacter::StartWeaponFire()
{
	// Character 只校验装备与生存状态，弹药和武器动作互斥由 WeaponComponent 负责。
	if (EquippedWeaponComponent != nullptr && !IsDead())
	{
		//转入weapon
		EquippedWeaponComponent->StartFire();
	}
}

void AfpstrueCharacter::StopWeaponFire()
{
	if (EquippedWeaponComponent != nullptr)
	{
		//转入weapon
		EquippedWeaponComponent->StopFire();
	}
}

void AfpstrueCharacter::RequestWeaponReload()
{
	if (EquippedWeaponComponent == nullptr || IsDead() || !EquippedWeaponComponent->CanReload())
	{
		return;
	}

	StopAim();
	StopSprint();
	EquippedWeaponComponent->RequestReload();
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
	OnWeaponEquipped(WeaponComponent);
}

//清除枪支、禁止开火、可见性设置
void AfpstrueCharacter::ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent)
{
	if (EquippedWeaponComponent == nullptr || EquippedWeaponComponent != WeaponComponent)
	{
		return;
	}

	StopWeaponFire();
	EquippedWeaponComponent = nullptr;
	Mesh1P->SetHiddenInGame(true, true);
	OnEquippedWeaponChanged.Broadcast(nullptr);
}

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
		EquippedWeaponComponent->HandleOwnerDeath();
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
	return HealthComponent != nullptr && HealthComponent->IsDead();
}

float AfpstrueCharacter::GetCurrentHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
}

float AfpstrueCharacter::GetMaxHealth() const
{
	return HealthComponent != nullptr ? HealthComponent->GetMaxHealth() : 0.0f;
}

float AfpstrueCharacter::GetHealthNormalized() const
{
	return HealthComponent != nullptr ? HealthComponent->GetHealthNormalized() : 0.0f;
}
