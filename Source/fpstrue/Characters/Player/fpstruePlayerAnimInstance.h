// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "fpstruePlayerAnimInstance.generated.h"
class AfpstrueCharacter;
class UfpstrueWeaponComponent;
class UAnimSequence;
class UAnimMontage;
class UMaterialParameterCollection;

// 每个实际 AnimBP 的共享只读配置；仅控制动画/材质表现，不改变速度、碰撞或命中规则。
USTRUCT(BlueprintType)
struct FFpstruePlayerAnimationSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float MovementDeadZone = 3.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float LandingDuration = 0.15f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float FullMovementBlendSpeed = 160.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float ReferenceLocomotionSpeed = 400.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float MinLocomotionPlayRate = 0.05f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float MaxLocomotionPlayRate = 2.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float AimBlendSpeed = 12.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float SprintInBlendSpeed = 8.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float SprintOutBlendSpeed = 10.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float TurnBlendSpeed = 10.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float YawRecoverySpeed = 15.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float HitRecoverySpeed = 12.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float HitDuration = 2.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float BodyAimPitchLimit = 75.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float BodyAimPitchScale = 0.6f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName SpinePivotBone;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName GunRootBone;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName ViewmodelFOVCurve;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName ViewmodelFOVAddCurve;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float DefaultViewmodelFOV = 100.0f;
};

// 玩家动画只消费移动快照、GAS 派生姿态和武器事实事件；不创建另一套动作状态，也不从通知结算伤害或弹药。
// 身体和手臂共用快照入口；具体资源、曲线与混合参数由各自 AnimBP 默认值提供。
UCLASS(Blueprintable, BlueprintType)
class FPSTRUE_API UfpstruePlayerAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	// 重建实例时复位验收身份、禁用 RootMotion 位移并绑定当前装备事实。
	virtual void NativeInitializeAnimation() override;
	// 解绑角色/武器事实、终止旧播放并作废旧实例的延迟验收请求。
	virtual void NativeUninitializeAnimation() override;
	// Game Thread 采样移动、GAS 派生姿态和朝向；动画图只消费快照，不结算玩法。
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	// 本地 RAR 手臂求值后将素材 FOV 曲线投影到本世界材质参数，不改变命中视点。
	virtual void NativePostEvaluateAnimation() override;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float GroundSpeed = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float ForwardSpeed = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float RightSpeed = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float LocomotionPlayRate = 1.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") bool bFalling = false;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") bool bJumpRising = false;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") bool bLanding = false;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") bool bArmed = false;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float MovementAlpha = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float AimAlpha = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float SprintAlpha = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float AimPitch = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float RootYawOffset = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float TurnTime = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float TurnAlpha = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") bool bTurnRight = false;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimSequence> TurnLeftAnimation;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimSequence> TurnRightAnimation;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") FRotator AimRotation = FRotator::ZeroRotator;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") FVector GunAimTranslation = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> FireMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> AimedFireMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> ReloadMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> EmptyReloadMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> EquipMontage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation") FFpstruePlayerAnimationSettings AnimationSettings;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation") TObjectPtr<UMaterialParameterCollection> ViewmodelFOVCollection;
	// 实际 RAR AnimBP 消费投影快照并更新本世界材质实例；工作线程不调用此事件。
	UFUNCTION(BlueprintImplementableEvent, Category = "Presentation") void PresentViewmodelProjection(float FieldOfView);
	// 已结算的受击表现可由拥有者与观察者调用；幅度仅进入动画图，不改控制旋转或命中历史。
	void PresentHit();
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float HitAlpha = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float HitTime = 0.0f;
private:
	// 成对切换事实订阅，换弹已开始时按当前进度补播，不生成另一套动作时钟。
	void BindCurrentWeapon();
	// 装备变化即刻重绑，避免首发早于下一次动画更新时漏播。
	UFUNCTION() void HandleEquipmentChanged(UfpstrueWeaponComponent* Weapon);
	// 按当前瞄准表现选择开火 Montage，不重复扣弹或结算命中。
	UFUNCTION() void PresentFire();
	// 选择普通/空仓素材，再由武器按 ReloadId 统一播放、绑定与取消。
	UFUNCTION() void PresentReload(int32 ReloadId, bool bWasEmpty);
	TWeakObjectPtr<UfpstrueWeaponComponent> AnimationWeapon;
	TWeakObjectPtr<AfpstrueCharacter> EquipmentSource;
	float LandingRemaining = 0.0f;
	bool bWasFalling = false;
#if !UE_BUILD_SHIPPING
	// 仅显式视觉验收延迟截取有效动作帧；旧实例、旧装备或旧事务不生成截图。
	void CapturePresentation(const TCHAR* Phase);
	TSet<FName> PendingPresentationCaptures;
	uint32 PresentationCaptureRevision = 0;
#endif
};
