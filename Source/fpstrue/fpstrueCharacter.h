// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "fpstrueCharacter.generated.h"

//声明类和结构体，告诉编译器它们的存在，减少头文件依赖，提高编译速度
class UInputComponent;
class USkeletalMeshComponent;
class UCameraComponent;
class USpringArmComponent;
class UInputAction;
class UInputMappingContext;
class UfpstrueHealthComponent;
class UfpstrueWeaponComponent;
struct FInputActionValue;

// 日志分类声明
DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
// LogTemplateCharacter 用于后续 UE_LOG 输出信息
// Log: 默认级别
// All: 所有类型消息都允许


// ----------------------
// Character states
// ----------------------
UENUM(BlueprintType)   //蓝图可用
enum class EFPCharacterState : uint8
{
	Idle        UMETA(DisplayName = "Idle"),
	Moving      UMETA(DisplayName = "Moving"),
	Reloading   UMETA(DisplayName = "Reloading"),
	Dead        UMETA(DisplayName = "Dead")
};

// ----------------------
// AfpstrueCharacter 类声明
// ----------------------
UCLASS(config=Game)// UE 反射宏，声明这是一个可被编辑器识别的类
class AfpstrueCharacter : public ACharacter // 继承 ACharacter，获得基础移动和碰撞功能
{
	GENERATED_BODY()

	/** Pawn mesh: 1st person view (arms; seen only by self) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category=Mesh, meta = (AllowPrivateAccess = "true"))
	USkeletalMeshComponent* Mesh1P;

	/** Camera boom used for first person camera lag */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** First person camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Camera, meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FirstPersonCameraComponent;

	/** MappingContext */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputMappingContext* DefaultMappingContext;

	/** Jump Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	UInputAction* JumpAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Input, meta=(AllowPrivateAccess = "true"))
	UInputAction* MoveAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	class UInputAction* LookAction;

	/** Reload Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* ReloadAction;

	/** Run Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* RunAction;

	/** Aim Input Action */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Input, meta = (AllowPrivateAccess = "true"))
	UInputAction* AimAction;

	/** Player health */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Health, meta = (AllowPrivateAccess = "true"))
	UfpstrueHealthComponent* HealthComponent;
	
public:

	// ----------------------	
	// 构造函数与生命周期
	// ----------------------
	AfpstrueCharacter();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

protected:
	//输入函数声明，供输入系统调用
	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

	/** Called for reload input */
	void StartReload();

	void StartSprint();
	void StopSprint();
	void StartAim();
	void StopAim();

	void FinishReload();

	bool CanReload() const;

	FString GetReloadBlockReason() const;

	void UpdateCharacterState();

	FString GetCharacterStateString() const;

	UFUNCTION()
	void HandleHealthChanged(float NewHealth);

	UFUNCTION()
	void HandleDeath();

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnAimChanged(bool bNewIsAiming);

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnFireStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "Weapon")
	void OnFireStopped();

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerHealthChanged(float NewHealth);

	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnPlayerDied();

protected:

	//弹药弹夹和状态变量
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 MagazineSize = 30;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 CurrentAmmo = 10;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 ReserveAmmo = 90;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	float ReloadDuration = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	float EmptyReloadDuration = 1.2f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	EFPCharacterState CharacterState = EFPCharacterState::Idle;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float WalkSpeed = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float SprintSpeed = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float AimWalkSpeed = 120.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
	bool bIsSprinting = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
	bool bIsAiming = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
	bool bIsFiring = false;

	FTimerHandle ReloadTimerHandle;

	UPROPERTY(Transient)
	UfpstrueWeaponComponent* EquippedWeaponComponent = nullptr;

protected:
	// ----------------------
	// APawn 接口覆盖（UE 自带）
	// ----------------------
	// APawn interface
	virtual void NotifyControllerChanged() override;
	virtual void SetupPlayerInputComponent(UInputComponent* InputComponent) override;
	// End of APawn interface

public:

	// ----------------------
	// WeaponComponent 调用的弹药接口
	// ----------------------
	/** 当前是否正在换弹 */
	bool IsReloading() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const;

	/** 当前是否还有弹匣内子弹 */
	bool HasAmmo() const;

	/** 只检查当前是否允许开火，不消耗子弹 */
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool CanFireWeapon() const;

	/** 尝试消耗一发子弹。成功返回 true，失败返回 false */
	bool TryConsumeAmmo();

	/** 左键开始按下，进入射击中状态 */
	void NotifyFireStarted();

	/** 左键松开或取消，退出射击中状态 */
	void NotifyFireStopped();


	/** 外部请求换弹，比如没子弹时自动换弹 */
	void RequestReload();

	void SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent);

	/** 调试/UI 用：获取当前弹药信息 */
	int32 GetCurrentAmmo() const { return CurrentAmmo; }
	int32 GetMagazineSize() const { return MagazineSize; }
	int32 GetReserveAmmo() const { return ReserveAmmo; }
	UfpstrueHealthComponent* GetHealthComponent() const { return HealthComponent; }
	UFUNCTION(BlueprintPure, Category = "Movement")
	bool IsSprinting() const { return bIsSprinting; }
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsAiming() const { return bIsAiming; }
	UFUNCTION(BlueprintPure, Category = "Weapon")
	bool IsFiring() const { return bIsFiring; }
	
	
	// ----------------------
	// Getter 函数（方便 Blueprint 或 C++ 调用）
	// ----------------------
	/** Returns Mesh1P subobject **/
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	/** Returns FirstPersonCameraComponent subobject **/
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

};

