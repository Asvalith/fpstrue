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

UENUM(BlueprintType)
enum class EFPCharacterState : uint8
{
	Idle       UMETA(DisplayName = "Idle"),
	Moving     UMETA(DisplayName = "Moving"),
	// Reload迁移至weapon组件，角色不再直接管理reload状态
	Reloading  UMETA(Hidden),
	Dead       UMETA(DisplayName = "Dead")
};

UCLASS(config=Game)
class FPSTRUE_API AfpstrueCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	/// 对外接口（查询、请求）

	AfpstrueCharacter();
	virtual void BeginPlay() override;

	//蓝图直接访问Mesh1P和FirstPersonCameraComponent组件
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }
	
	//蓝图中可以调用的换弹请求。
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void RequestReload();

	//装备/卸下武器组件
	void SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent);
	void ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent);


	//转发武器状态查询到装备的武器组件，角色本身不再直接管理武器状态
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasEquippedWeapon() const { return EquippedWeaponComponent != nullptr; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	UfpstrueWeaponComponent* GetEquippedWeaponComponent() const { return EquippedWeaponComponent; }

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsReloading() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool HasAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool CanFireWeapon() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetCurrentAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetMagazineSize() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	int32 GetReserveAmmo() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsFiring() const;

	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsAiming() const { return bIsAiming; }


	//转发HealthComponent，角色本身不再直接管理生命值状态
	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetCurrentHealth() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthNormalized() const;

	//转发HealthComponent的引用，同时方便蓝图中直接访问HealthComponent的其他功能
	UFUNCTION(BlueprintPure, Category = "Health")
	UfpstrueHealthComponent* GetHealthComponent() const { return HealthComponent; }

	UPROPERTY(BlueprintAssignable, Category = "Health|Events")
	FOnPlayerDeathReported OnPlayerDeathReported;

	//状态和移动相关
	UFUNCTION(BlueprintPure, Category = "State")
	EFPCharacterState GetCharacterState() const;

	UFUNCTION(BlueprintPure, Category = "Movement")
	bool IsSprinting() const { return bIsSprinting; }

protected:
	///// 生命周期回调、输入处理、事件处理

	//生命周期回调：Actor 销毁前、控制器变更时、引擎在 Possess 时调用（参数是增强输入组件）
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void NotifyControllerChanged() override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;


	//处理输入的函数，绑定到增强输入组件的动作上
	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void StartWeaponFire();
	void StopWeaponFire();
	void StartReload();
	void StartSprint();
	void StopSprint();
	void StartAim();
	void StopAim();
	
	//绑定到 HealthComponent 的血量变化委托。
	UFUNCTION()
	void HandleHealthChanged(float NewHealth);
	//绑定到受伤委托。参数三件套：伤害量、伤害来源 Actor、肇事控制器（用于计分判定），转发给蓝图 OnPlayerDamaged。
	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);
	//绑定到死亡委托
	UFUNCTION()
	void HandleDeath();


	//蓝图事件：瞄准状态变化、武器装备、血量变化、受伤、死亡
	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnAimChanged(bool bNewIsAiming);

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnWeaponEquipped(UfpstrueWeaponComponent* WeaponComponent);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerHealthChanged(float NewHealth);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDied();

private:
	/// 组件指针、配置参数、内部状态
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

	//装备的武器组件指针，蓝图可读，编辑器不可编辑，Transient
	UPROPERTY(Transient)
	//强引用：装备期间防止武器组件被 GC 回收。
	TObjectPtr<UfpstrueWeaponComponent> EquippedWeaponComponent;

	//增强输入子系统的弱指针，便于移除 Mapping Context
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;


	// 应用和移除输入映射上下文（UMG切换）
	void ApplyInputMappingContexts();
	void RemoveInputMappingContexts();

	//死亡处理标志，防止重复处理死亡事件
	bool bDeathHandled = false;
};
