// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class APlayerController;
class UCameraComponent;
class UWorld;


////UENUM(BlueprintType)
//Weapon组件状态枚举
UENUM(BlueprintType)
enum class EFPWeaponActionState : uint8
{
	Ready      UMETA(DisplayName = "Ready"),
	Firing     UMETA(DisplayName = "Firing"),
	Reloading  UMETA(DisplayName = "Reloading"),
	Disabled   UMETA(DisplayName = "Disabled")
};

//动态多播委托类型
//成功执行一次射击时广播
//开始换弹时告诉监听者是不是“空仓换弹”
//弹药发生变化时广播
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FWeaponFireEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWeaponReloadEvent, bool, bWasEmptyReload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FWeaponAmmoChangedEvent,
	int32, CurrentAmmo,
	int32, MagazineSize,
	int32, ReserveAmmo
);

//Hitscan命中结果
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(
	FWeaponTraceEvent,
	bool, bHit,
	FVector, TraceStart,
	FVector, TraceEnd,
	FVector, TraceTarget,
	FHitResult, HitResult
);


//BlueprintSpawnableComponent：可在蓝图Actor的Components面板
UCLASS(Blueprintable, BlueprintType, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueWeaponComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:
	// ==================== Construction ====================
	//构造
	UfpstrueWeaponComponent();

	// ==================== Equipment ====================
	//装备是否成功
	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);

	// ==================== Fire ====================
	//开火、停止射击
	void StartFire();
	void StopFire();

	// ==================== Reload ====================
	// 换弹
	// BlueprintCallable：
	// 蓝图可以主动调用该函数。
	// RequestReload 只负责“请求进入换弹状态”，
	// 不代表弹药一定已经完成转移。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool RequestReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool CommitReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void FinishReload();

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void CancelReload();

	// ==================== Owner Lifecycle ====================
	//角色死亡时停止射击和换弹，并禁用武器
	void HandleOwnerDeath();

	// ==================== State / Rule Query ====================
	UFUNCTION(BlueprintPure, Category = "Weapon|State")
	bool IsReloading() const { return ActionState == EFPWeaponActionState::Reloading; }

	//检查是否正在开火限制montage重复播放
	UFUNCTION(BlueprintPure, Category = "Weapon|State")
	bool IsFiring() const { return ActionState == EFPWeaponActionState::Firing; }

	bool CanReload() const;

	// ==================== Ammo Query ====================
	//UI可查询子弹数量相关
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentAmmo() const { return CurrentAmmo; }

	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetMagazineSize() const { return MagazineSize; }

	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetReserveAmmo() const { return ReserveAmmo; }

	// ==================== Events ====================
	//事件
	//蓝图蓝图可以Bind Event到这些动态多播委托
	//蓝图可以决定播放动画、音效、HUD、特效
	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFirePerformed;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponReloadEvent OnWeaponReloadStarted;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponTraceEvent OnWeaponTraceFinished;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponAmmoChangedEvent OnAmmoChanged;

protected:
	// ==================== Component Lifecycle ====================
	//UObject/ActorComponent 生命周期结束前调用
	//这里主要应该清理Timer、武器状态、Character引用等
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// ==================== Action State / Rules ====================
	// 第一层：Action State
	//武器状态修改统一入口
	void SetActionState(EFPWeaponActionState NewState);

	// 第二层：Rules
	//状态边界
	//统一检查武器已装备、未禁用且角色存活
	bool IsOperational() const;
	bool CanFire() const;
	bool HasAmmo() const { return CurrentAmmo > 0; }

	// ==================== Fire System ====================
	//射击基础逻辑
	void Fire();
	bool TryConsumeAmmo();
	//射线检测
	void FireLineTrace(UWorld* World, UCameraComponent* Camera);
	//执行trace
	void FireSingleLineTrace(UWorld* World, UCameraComponent* Camera, float SpreadAngle);

	// ==================== Reload System ====================
	// 第三层：Transaction 由 Request/Commit/Finish/Cancel Reload 维护
	// 第四层：Recovery
	//设置换弹保护Timer（换弹没有完成则自动恢复）
	void ScheduleReloadTimeout(float DurationSeconds);
	//reload timer回调
	void HandleReloadTimeout(int32 ReloadSequence);

	// ==================== Recoil System ====================
	//后坐力
	void ApplyRecoil(APlayerController* PlayerController);
	//枪口回正
	void UpdateRecoilRecovery();
	//清除清口回正
	void ClearRecoilState();

	// ==================== Runtime Helpers ====================
	//**第一次装备或运行时启动时初始化弹药等运行时数据
	void InitializeRuntimeState();
	//弹药改变后统一广播给UI
	void BroadcastAmmoChanged();

	// ==================== Configuration ====================
	// Trace
	// Hitscan 射击参数
	// 最大射线距离
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Trace", meta = (ClampMin = "1.0"))
	float LineTraceRange = 10000.0f;
	//冲量
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Trace", meta = (ClampMin = "0.0"))
	float LineTraceImpulse = 10000.0f;

	// Damage
	//设置不同伤害
	//普通部位伤害
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Damage", meta = (ClampMin = "0.0"))
	float LineTraceDamage = 40.0f;
	//头部伤害
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Damage", meta = (ClampMin = "0.0"))
	float LineTraceHeadDamage = 100.0f;

	// Ammo
	//弹药参数
	//弹匣容量
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Ammo", meta = (ClampMin = "1"))
	int32 MagazineSize = 30;
	//初始备弹
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Ammo", meta = (ClampMin = "0"))
	int32 StartingReserveAmmo = 90;

	// Spread
	//散布参数
	//腰射基础散步角
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float HipFireSpreadAngle = 1.5f;
	//ADS瞄准时基础散布角
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float AimFireSpreadAngle = 0.25f;
	//连续每开一枪额外增加多少散布
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float ContinuousFireSpreadStep = 0.2f;
	//连续射击时允许达到的最大散布
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float MaxContinuousFireSpreadAngle = 3.0f;
	//停止射击多久后重置连续射击散布
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Spread", meta = (ClampMin = "0.0"))
	float SpreadResetDelay = 0.25f;

	// Recoil
	//后坐力参数
	//Pitch视角向上抬
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilPitch = 1.0f;
	//Yaw后坐力范围
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilYaw = 0.4f;
	//ADS状态下后坐力缩放系数
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AimRecoilMultiplier = 0.5f;
	//停止射击后，延迟多久开始恢复后坐力
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float RecoilRecoveryDelay = 0.12f;
	//后坐力恢复速度
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.1"))
	float RecoilRecoverySpeed = 10.0f;
	//最大累计垂直后坐力
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float MaxAccumulatedRecoilPitch = 6.0f;
	//最大累计水平后坐力
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Recoil", meta = (ClampMin = "0.0"))
	float MaxAccumulatedRecoilYaw = 2.0f;

	// Reload Recovery
	//reload容错参数
	//如果超时仍未完成 Reload，进入 FailSafe 处理，防止动画缺失导致而永久卡在Reloading 状态
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float ReloadFailSafeDuration = 5.0f;
	//装弹后，给 FinishReload 留一点容错时间
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.0"))
	float ReloadCompletionGracePeriod = 0.1f;

	// ==================== Runtime State ====================
	// Owner
	//运行时状态
	//当前持有该武器的角色，EndPlay时置空
	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> Character;
	//防止重复初始化弹药
	bool bRuntimeStateInitialized = false;

	// 第一层 Action State
	//Ready、Firing、Reloading、Disabled四种互斥动作状态
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|State")
	EFPWeaponActionState ActionState = EFPWeaponActionState::Disabled;

	// Ammo
	//当前弹匣和备弹数量
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 CurrentAmmo = 0;
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 ReserveAmmo = 0;

	// 第三层 Transaction
	//换弹事务状态
	//防止一次换弹被多个Notify重复提交
	bool bReloadAmmoCommitted = false;
	//区分前后两次换弹，旧Timer不能结束新的换弹
	int32 ActiveReloadSequence = 0;

	// Fire / Spread
	//连续射击和后坐力状态
	int32 ConsecutiveShotCount = 0;
	double LastShotTimeSeconds = -1.0;
	double LastAcceptedShotTimeSeconds = -1.0;

	// Recoil
	float AccumulatedRecoilPitch = 0.0f;
	float AccumulatedRecoilYaw = 0.0f;

	// 第四层 Recovery / Timers
	//异步Timer句柄，EndPlay时统一清理
	FTimerHandle AutomaticFireTimerHandle;
	FTimerHandle ReloadTimerHandle;
	FTimerHandle RecoilRecoveryTimerHandle;
};
