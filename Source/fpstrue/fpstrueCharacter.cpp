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

// 生命周期
AfpstrueCharacter::AfpstrueCharacter()
{ //胶囊体
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.0f);

	//弹簧臂创建、挂载、位置、长度
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

//输入绑定注册
void AfpstrueCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	//确定是新版的注册系统
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{

		//绑定基础移动
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Move);
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AfpstrueCharacter::Look);

		//条件绑定：检查开火前提，绑定开火（三个事件）
		if (FireAction)
		{
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartWeaponFire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &AfpstrueCharacter::StopWeaponFire);
			EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Canceled, this, &AfpstrueCharacter::StopWeaponFire);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("%s (%s): FireAction is NULL."), *GetName(), *GetClass()->GetPathName());
		}

		//条件绑定：检查冲刺前提，绑定冲刺（瞄准、换弹、死亡时强制关闭）
		if (RunAction)
		{
			EnhancedInputComponent->BindAction(RunAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartSprint);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("RunAction is NULL. Assign IA_Run in BP_FirstPersonCharacter."));
		}

		//条件绑定：检查瞄准前提，绑定开火（三个事件）
		if (AimAction)
		{
			EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartAim);
			EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Completed, this, &AfpstrueCharacter::StopAim);
			EnhancedInputComponent->BindAction(AimAction, ETriggerEvent::Canceled, this, &AfpstrueCharacter::StopAim);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("AimAction is NULL. Assign IA_Aim in BP_FirstPersonCharacter."));
		}

		//条件绑定：检查换弹前提，绑定开火（仅绑定started，不需要Completed、Canceled）
		if (ReloadAction)
		{
			EnhancedInputComponent->BindAction(ReloadAction, ETriggerEvent::Started, this, &AfpstrueCharacter::StartReload);
		}
		else
		{
			UE_LOG(LogTemplateCharacter, Error, TEXT("ReloadAction is NULL. Assign IA_reload in BP_FirstPersonCharacter."));
		}
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error,
			   TEXT("'%s' Failed to find an Enhanced Input Component! This template is built to use the Enhanced Input system. If you "
					"intend to use the legacy system, then you will need to update this C++ file."),
			   *GetNameSafe(this));
	}
}

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

void AfpstrueCharacter::StartSprint()
{
	// 死亡、换弹和瞄准期间禁止进入冲刺。
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

void AfpstrueCharacter::StartReload()
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
