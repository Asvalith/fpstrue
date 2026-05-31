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
class UInputAction;
class UInputMappingContext;
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
	
public:

	// ----------------------	
	// 构造函数与生命周期
	// ----------------------
	AfpstrueCharacter();
	virtual void Tick(float DeltaTime) override;

protected:
	//输入函数声明，供输入系统调用
	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

	/** Called for reload input */
	void StartReload();

	void FinishReload();

	bool CanReload() const;

	FString GetReloadBlockReason() const;

	void UpdateCharacterState();

	FString GetCharacterStateString() const;

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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "State")
	EFPCharacterState CharacterState = EFPCharacterState::Idle;

	FTimerHandle ReloadTimerHandle;

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

	/** 当前是否还有弹匣内子弹 */
	bool HasAmmo() const;

	/** 尝试消耗一发子弹。成功返回 true，失败返回 false */
	bool TryConsumeAmmo();

	/** 外部请求换弹，比如没子弹时自动换弹 */
	void RequestReload();

	/** 调试/UI 用：获取当前弹药信息 */
	int32 GetCurrentAmmo() const { return CurrentAmmo; }
	int32 GetMagazineSize() const { return MagazineSize; }
	int32 GetReserveAmmo() const { return ReserveAmmo; }
	
	
	// ----------------------
	// Getter 函数（方便 Blueprint 或 C++ 调用）
	// ----------------------
	/** Returns Mesh1P subobject **/
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	/** Returns FirstPersonCameraComponent subobject **/
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

};

