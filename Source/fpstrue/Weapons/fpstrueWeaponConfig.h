// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "fpstrueWeaponConfig.generated.h"

// 仅保存可调数值，不包含当前弹药、动作阶段、Timer 或播放身份。
// 每件武器首次装备时复制一次；共享资产不是所有武器共享的运行状态。
USTRUCT(BlueprintType)
struct FPSTRUE_API FFPWeaponSettings
{
	GENERATED_BODY()

	// Attachment
	// 玩家手臂骨架上的武器挂点。更换骨架时在配置资产中修改，并在装备时校验是否存在。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Attachment")
	FName GripSocketName = TEXT("GripPoint");

	// Fire
	// 每分钟射击数；用于自动射击 Timer 和单发提交节流，保证两条路径使用同一射速。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Fire", meta = (ClampMin = "1.0"))
	float RoundsPerMinute = 600.0f;

	// Trace
	// Hitscan 射击参数
	// 最大射线距离
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Trace", meta = (ClampMin = "1.0"))
	float LineTraceRange = 10000.0f;
	//冲量
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Trace", meta = (ClampMin = "0.0"))
	float LineTraceImpulse = 10000.0f;

	// Damage
	//设置不同伤害
	//普通部位伤害
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Damage", meta = (ClampMin = "0.0"))
	float LineTraceDamage = 40.0f;
	//头部伤害
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Damage", meta = (ClampMin = "0.0"))
	float LineTraceHeadDamage = 100.0f;
	// 当前目标骨架中视为关键命中的骨骼。数组很小，配置资产保存名单，命中时作线性查询。
	// 默认沿用 Mannequin 的骨骼约定；FName 在初始化时构造，避免每次命中拼接字符串或 ToLower。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Damage")
	TArray<FName> CriticalHitBones = {FName(TEXT("neck_01")), FName(TEXT("head"))};

	// Ammo
	//弹药参数
	//弹匣容量
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo", meta = (ClampMin = "1"))
	int32 MagazineSize = 30;
	//初始备弹
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo", meta = (ClampMin = "0"))
	int32 StartingReserveAmmo = 90;

	// Spread
	//散布参数
	//腰射基础散布角
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float HipFireSpreadAngle = 1.5f;
	//ADS瞄准时基础散布角
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float AimFireSpreadAngle = 0.25f;
	//连续每开一枪额外增加多少散布
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float ContinuousFireSpreadStep = 0.2f;
	//连续射击时允许达到的最大散布
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float MaxContinuousFireSpreadAngle = 3.0f;
	//停止射击多久后重置连续射击散布
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadResetDelay = 0.25f;

	// Recoil
	//后坐力参数
	//Pitch视角向上抬
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilPitch = 1.0f;
	//Yaw后坐力范围
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilYaw = 0.4f;
	//ADS状态下后坐力缩放系数
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AimRecoilMultiplier = 0.5f;
	//停止射击后，延迟多久开始恢复后坐力
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryDelay = 0.12f;
	//后坐力恢复速度
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.1"))
	float RecoilRecoverySpeed = 10.0f;
	//最大累计垂直后坐力
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float MaxAccumulatedRecoilPitch = 6.0f;
	//最大累计水平后坐力
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float MaxAccumulatedRecoilYaw = 2.0f;

	// Reload Recovery
	// 预计动画时长只用于兜底截止点：max(普通/空仓时长, FailSafeDuration) + GracePeriod。
	// 正常装填由有身份的 Notify 提交，播放结束解除锁；超时只失败收尾，不伪造动画成功。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float ReloadDuration = 0.8f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float EmptyReloadDuration = 1.2f;

	// 兜底等待时间的下限；比预计动画长时优先使用此值，避免提前结束正常动画。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float ReloadFailSafeDuration = 5.0f;
	// 额外加在兜底截止点上的宽限时间；不是 CommitReload 后另起的倒计时。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.0"))
	float ReloadCompletionGracePeriod = 0.1f;

	// 编辑器 Clamp 不能约束脚本/代码赋值；装备前与资产验证共用同一套检查。
	// 非法整组拒绝，不静默混入默认字段或修改设计数值。
	bool Validate(FString& OutError) const;
};

UCLASS(BlueprintType)
class FPSTRUE_API UfpstrueWeaponConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon", meta = (ShowOnlyInnerProperties))
	FFPWeaponSettings Settings;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
