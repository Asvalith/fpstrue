// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "fpstrueCharacter.generated.h"

class UCameraComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UInputAction;
class UInputComponent;
class UInputMappingContext;
class USkeletalMeshComponent;
class USpringArmComponent;
class UfpstrueHealthComponent;
class UfpstrueWeaponComponent;
class AfpstrueCharacter;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
//动态多播委托，用于在玩家死亡时通知其他系统:DYNAMIC可以被蓝图绑定
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerDeathReported, AfpstrueCharacter*, DeadPlayer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEquippedWeaponChanged, UfpstrueWeaponComponent*, WeaponComponent);

/** 玩家角色模块：连接输入、移动、武器和通用 HealthComponent，并把状态变化转发给蓝图表现层。 */
UCLASS(config = Game)
class FPSTRUE_API AfpstrueCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	/// 对外接口（查询、请求）

	// 创建相机、第一人称 Mesh 和 HealthComponent，角色本身不启用 Tick。
	AfpstrueCharacter();
	// 绑定健康事件、同步 HUD 初始血量并初始化移动速度。
	virtual void BeginPlay() override;

	//蓝图直接访问Mesh1P和FirstPersonCameraComponent组件
	// WeaponComponent 装备时使用第一人称 Mesh 的 GripPoint。
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	// WeaponComponent 的 Hitscan 使用该相机确定射线起点和方向。
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

	//装备/卸下武器组件
	// PickUp/WeaponComponent 装备成功后登记当前武器并通知 HUD/蓝图。
	void SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent);
	// 武器销毁或卸下时清空装备关系并通知 HUD。
	void ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent);

	//Character只暴露当前装备关系，武器运行时状态由WeaponComponent持有
	// WeaponComponent 装备前检查玩家是否已经持有武器。
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasEquippedWeapon() const { return EquippedWeaponComponent != nullptr; }

	// HUD 和蓝图读取当前武器；弹药状态仍由 WeaponComponent 持有。
	UFUNCTION(BlueprintPure, Category = "Weapon")
	UfpstrueWeaponComponent* GetEquippedWeaponComponent() const { return EquippedWeaponComponent; }

	// HUD在拾取或清空武器时切换弹药事件来源，避免每帧查询。
	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FOnEquippedWeaponChanged OnEquippedWeaponChanged;

	// WeaponComponent 用它选择瞄准散布和后坐力倍率。
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsAiming() const { return bIsAiming; }

	//转发HealthComponent，角色本身不再直接管理生命值状态
	// GameMode、武器和输入规则通过这里读取玩家死亡事实。
	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const;

	// HUD 初始化时读取当前血量。
	UFUNCTION(BlueprintPure, Category = "Health")
	float GetCurrentHealth() const;

	// HUD 初始化时读取最大血量。
	UFUNCTION(BlueprintPure, Category = "Health")
	float GetMaxHealth() const;

	// UI 进度条读取归一化血量；变化仍由事件驱动。
	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthNormalized() const;

	//转发HealthComponent的引用，同时方便蓝图中直接访问HealthComponent的其他功能
	// EnemyCombatComponent 和 GameMode 通过该引用复用通用生命值判断。
	UFUNCTION(BlueprintPure, Category = "Health")
	UfpstrueHealthComponent* GetHealthComponent() const { return HealthComponent; }

	UPROPERTY(BlueprintAssignable, Category = "Health|Events")
	FOnPlayerDeathReported OnPlayerDeathReported;

protected:
	// ==================== 生命周期、输入与事件处理 ====================

	//生命周期回调：Actor 销毁前、控制器变更时、引擎在 Possess 时调用（参数是增强输入组件）
	// 停止持续开火、移除输入映射并解除健康事件。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// Controller 变化时把 Mapping Context 迁移到新的本地玩家子系统。
	virtual void NotifyControllerChanged() override;
	// 把增强输入 Action 绑定到移动、瞄准、开火和换弹接口。
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;

	//处理输入的函数，绑定到增强输入组件的动作上
	// 把二维移动输入转换为前后/左右移动。
	void Move(const FInputActionValue& Value);
	// 把二维视角输入转换为 Yaw/Pitch。
	void Look(const FInputActionValue& Value);
	// 输入开始时检查死亡、换弹和瞄准约束，再进入冲刺。
	void StartSprint();
	// 退出冲刺并恢复正常步速。
	void StopSprint();
	// 检查武器与角色状态后进入瞄准，并通知蓝图表现。
	void StartAim();
	// 退出瞄准并恢复正常步速。
	void StopAim();
	// 把开火输入转交当前 WeaponComponent。
	void StartWeaponFire();
	// 把停止开火输入转交当前 WeaponComponent。
	void StopWeaponFire();
	// 结束瞄准/冲刺后向 WeaponComponent 请求换弹。
	void StartReload();

	//绑定到 HealthComponent 的血量变化委托。
	// 把通用血量变化转发给玩家蓝图和 HUD。
	UFUNCTION()
	void HandleHealthChanged(float NewHealth);
	//绑定到受伤委托。参数三件套：伤害量、伤害来源 Actor、肇事控制器（用于计分判定），转发给蓝图 OnPlayerDamaged。
	// 把通用受伤事件转发给玩家受击表现。
	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);
	//绑定到死亡委托
	// 停止移动和武器，执行一次玩家死亡表现并通知 GameMode。
	UFUNCTION()
	void HandleDeath();

	//蓝图事件：瞄准状态变化、武器装备、血量变化、受伤、死亡
	// 蓝图用它切换瞄准动画和 UI。
	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnAimChanged(bool bNewIsAiming);

	// 蓝图用它播放装备表现。
	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnWeaponEquipped(UfpstrueWeaponComponent* WeaponComponent);

	// 蓝图/HUD 响应血量变化。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerHealthChanged(float NewHealth);

	// 蓝图播放玩家受击反馈。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	// 蓝图播放玩家死亡表现。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDied();

private:
	/// **组件指针**、配置参数、内部状态
	//Mesh1P
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> Mesh1P;

	//相机相关
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FirstPersonCameraComponent;

	//输入资产配置
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> FireAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> ReloadAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> RunAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> AimAction;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UfpstrueHealthComponent> HealthComponent;

	//移动速度配置参数，蓝图可读，编辑器可编辑
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float WalkSpeed = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float SprintSpeed = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	float AimWalkSpeed = 120.0f;

	//运行时状态变量，蓝图可读，编辑器不可编辑
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	bool bIsSprinting = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon", meta = (AllowPrivateAccess = "true"))
	bool bIsAiming = false;

	// 装备关系只在运行时存在，不参与序列化。
	UPROPERTY(Transient)
	// 强引用保证装备期间组件不会被 GC 回收。
	TObjectPtr<UfpstrueWeaponComponent> EquippedWeaponComponent;

	//增强输入子系统的弱指针，便于移除 Mapping Context
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;

	// 应用和移除输入映射上下文（UMG切换）
	// 把默认 Mapping Context 添加到当前 LocalPlayer。
	void ApplyInputMappingContexts();
	// 从旧 LocalPlayer 移除本角色添加的 Mapping Context。
	void RemoveInputMappingContexts();

	//死亡处理标志，防止重复处理死亡事件
	// HealthComponent 保存死亡事实；这里只防止死亡表现和广播重复执行。
	bool bDeathEffectsApplied = false;
};
