// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Characters/Shared/fpstrueActionPlayback.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class APlayerController;
class UCameraComponent;
class UAnimInstance;
class UAnimMontage;
class UWorld;
struct FAnimNotifyEventReference;

//Weapon组件状态枚举
UENUM(BlueprintType)
enum class EFPWeaponActionState : uint8
{
	Ready UMETA(DisplayName = "Ready"),
	Firing UMETA(DisplayName = "Firing"),
	Reloading UMETA(DisplayName = "Reloading"),
	Disabled UMETA(DisplayName = "Disabled")
};

UENUM(BlueprintType)
enum class EFPReloadEndReason : uint8
{
	Completed,
	Cancelled,
	TimedOut,
	Disabled,
	PlaybackFailed
};

//动态多播委托类型
//成功执行一次射击时广播
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FWeaponFireEvent);
//开始换弹时告诉监听者是不是“空仓换弹”
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWeaponReloadEvent, bool, bWasEmptyReload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FWeaponReloadPlaybackEvent, int32, ReloadId, bool, bWasEmptyReload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FWeaponReloadEndedEvent, int32, ReloadId, EFPReloadEndReason, Reason, bool, bAmmoCommitted);
//弹药发生变化时广播
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FWeaponAmmoChangedEvent, int32, CurrentAmmo, int32, MagazineSize, int32, ReserveAmmo);

//Hitscan命中结果
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FWeaponTraceEvent, bool, bHit, FVector, TraceStart, FVector, TraceEnd, FVector, TraceTarget,
											  FHitResult, HitResult);

//BlueprintSpawnableComponent：可在蓝图 Actor 的 Components 面板添加本组件
/**
 * 玩家武器模块：独占装备、动作状态、弹药事务、Hitscan、散布和后坐力，并通过事件驱动 HUD/蓝图。
 *
 * Character 只提交输入请求；本组件用 ActionState 约束开火/换弹互斥，用 Notify 提交换弹数值，
 * 并用 Timer 为连续射击、后坐力恢复和 Notify 丢失提供独立生命周期。
 * Ready -> Firing -> Ready；Ready/Firing -> Reloading -> Ready；死亡或卸下 -> Disabled。
 * Reloading 中提交只转移弹药；有身份的完成/中断、主动取消或超时才解除动作锁。
 */
UCLASS(Blueprintable, BlueprintType, ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueWeaponComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:
	// 设置默认关键骨骼名单；其余素材相关参数使用 UPROPERTY 默认值，可由武器蓝图覆盖。
	UfpstrueWeaponComponent();

	// ==================== Equipment ====================
	//装备是否成功
	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);
	// 完整解除装备关系；外部 Pickup Actor 的组件不会随玩家自动结束生命周期。
	void DetachWeapon();

	// ==================== Fire ====================
	// Character 的输入入口调用；校验状态后开始单发或自动射击。
	void StartFire();
	// Character 松开输入、换弹、死亡和 EndPlay 时调用，停止持续射击。
	void StopFire();

	// ==================== Reload ====================
	// 换弹分为请求、弹药提交、结束三个阶段；取消只结束流程，不撤销已经提交的弹药。
	// 先建立 Reloading 和兜底 Timer，再发带 ReloadId 的播放请求，蓝图选材后走显式播放接口。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool RequestReload();

	// 仅保留反射入口用于诊断旧资产；无来源身份的动画回调不能借用当前事务 ID。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload", meta = (DeprecatedFunction, DeprecationMessage = "Use CommitReloadForTransaction with the saved ReloadId, or the native ReloadCommit notify."))
	bool CommitReload();

	// 旧无参完成入口只诊断、不写状态；显式播放由原生实例结束委托收口。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload", meta = (DeprecatedFunction, DeprecationMessage = "Use PlayReloadMontage or FinishReloadForTransaction with the saved ReloadId."))
	void FinishReload();

	// 无参数仅供同步取消命令；延迟中断回调须保存 ReloadId，调用 CancelReloadForTransaction。
	// 提交前取消不装弹，提交后取消不退弹。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void CancelReload();

	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	int32 GetActiveReloadId() const { return IsReloading() ? ActiveReloadId : INDEX_NONE; }

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool CommitReloadForTransaction(int32 ReloadId);

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void FinishReloadForTransaction(int32 ReloadId);

	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void CancelReloadForTransaction(int32 ReloadId);

	// 首选播放入口：调用者明确给出事务、Mesh、资源，原生代码拥有播放实例与完成/中断回调。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool PlayReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage, float PlayRate = 1.0f);

	// 外部已播放的 Montage 必须紧接播放绑定；不扫描全局 Montage，也不覆盖同 Mesh 的已绑定实例。
	// 此接口接管实例的结束委托；不要再为同一实例设置 PlayMontageProxy 完成回调。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool BindReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage);

	// 原生 Notify 只接受已绑定的播放实例；没有来源身份时拒绝，不猜“当前换弹”。
	bool CommitReloadFromNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference);

	// ==================== Owner Lifecycle ====================
	//角色死亡、卸下或 EndPlay 时停止射击、换弹和后坐力，并禁用武器。
	void DisableWeapon();
	// 控制权切换保留装备与弹药，但不把旧控制器的持续动作/后坐力交给新控制器。
	void InterruptOwnerInput();

	// ==================== State / Rule Query ====================
	// Character 的冲刺/瞄准规则读取当前是否换弹。
	UFUNCTION(BlueprintPure, Category = "Weapon|State")
	bool IsReloading() const { return ActionState == EFPWeaponActionState::Reloading; }

	//检查是否正在开火，供蓝图限制 Montage 重复播放；查询本身不控制动画。
	UFUNCTION(BlueprintPure, Category = "Weapon|State")
	bool IsFiring() const { return ActionState == EFPWeaponActionState::Firing; }

	// Character 在提交换弹请求前检查弹匣、备弹和动作状态。
	bool CanReload() const;

	// ==================== Ammo Query ====================
	//UI可查询子弹数量相关
	// HUD 初始化时读取当前弹匣；变化由 OnAmmoChanged 推送。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentAmmo() const { return CurrentAmmo; }

	// HUD 初始化时读取弹匣容量。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetMagazineSize() const { return MagazineSize; }

	// HUD 初始化时读取剩余备弹。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetReserveAmmo() const { return ReserveAmmo; }

	// ==================== Events ====================
	//事件
	//蓝图可以 Bind Event 到这些动态多播委托
	//蓝图可以决定播放动画、音效、HUD、特效
	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFirePerformed;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponReloadEvent OnWeaponReloadStarted;

	// 播放命令携带事务身份，蓝图只选素材并调用 PlayReloadMontage；Started 保留为观察事件。
	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponReloadPlaybackEvent OnWeaponReloadPlaybackRequested;

	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponReloadEndedEvent OnWeaponReloadEnded;

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
	// Action State 只从这里切换；离开 Firing/Reloading 时同步清理对应 Timer。
	void SetActionState(EFPWeaponActionState NewState);
	//状态边界
	//统一检查武器已装备、未禁用且角色存活
	bool IsOperational() const;
	// 暂时输入禁入与永久 Disabled 分开；结束旧换弹仍可恢复 Ready，但回调不能重启动作。
	bool CanAcceptOwnerInput() const;
	// 同步委托返回后检查动作是否仍是原来那次，避免旧调用继续操作新动作。
	bool IsCurrentAction(EFPWeaponActionState ExpectedState, uint32 ExpectedRevision) const;
	// 射击和换弹规则读取弹匣是否还有弹药。
	bool HasAmmo() const { return CurrentAmmo > 0; }
	bool IsCurrentReload(int32 ReloadId) const;
	void EndReload(int32 ReloadId, EFPReloadEndReason Reason);
	void HandleReloadPlaybackEnded(UAnimMontage* Montage, bool bInterrupted,
		int32 ReloadId, TWeakObjectPtr<USkeletalMeshComponent> PlaybackMesh, int32 InstanceId);

	// ==================== Fire System ====================
	// 自动射击 Timer 和首次按下输入共用的单次射击入口。
	void Fire();
	// 按上一发剩余冷却安排一次性回调；松开再按不会重新等待完整间隔。
	void ScheduleNextShot();
	// 空仓入口与最后一发共用：能换弹就换弹，否则停止射击。
	void HandleEmptyMagazine();
	// 根据相机、瞄准状态和连续射击次数计算本发 Hitscan。
	// 执行一条带散布的射线，处理伤害、冲量和命中事件。
	void FireLineTrace(UWorld& World, AfpstrueCharacter& OwningCharacter, UCameraComponent& Camera);

	// ==================== Recoil System ====================
	// 射击成功后把随机水平/垂直后坐力应用到 PlayerController。
	void ApplyRecoil(APlayerController* PlayerController);
	// Timer 驱动相机逐步抵消累计后坐力。
	void UpdateRecoilRecovery();
	// 停止恢复 Timer 并清空累计后坐力。
	void ClearRecoilState();

	// ==================== Runtime Helpers ====================
	//弹药改变后统一广播给UI
	void BroadcastAmmoChanged();

	// ==================== Configuration ====================
	// Attachment
	// 玩家手臂骨架上的武器挂点。更换骨架时可在武器蓝图中覆盖，并在装备时校验是否存在。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Attachment")
	FName GripSocketName = TEXT("GripPoint");

	// Fire
	// 每分钟射击数；用于自动射击 Timer 和单发提交节流，保证两条路径使用同一射速。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Fire", meta = (ClampMin = "1.0"))
	float RoundsPerMinute = 600.0f;

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
	// 当前目标骨架中视为关键命中的骨骼。数组很小，线性查询便于在蓝图中直接配置。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Damage")
	TArray<FName> CriticalHitBones;

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
	//腰射基础散布角
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
	// 预计动画时长只用于兜底截止点：max(普通/空仓时长, FailSafeDuration) + GracePeriod。
	// 正常装填由有身份的 Notify 提交，播放结束解除锁；超时只失败收尾，不伪造动画成功。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float ReloadDuration = 0.8f;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float EmptyReloadDuration = 1.2f;

	// 兜底等待时间的下限；比预计动画长时优先使用此值，避免提前结束正常动画。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.1"))
	float ReloadFailSafeDuration = 5.0f;
	// 额外加在兜底截止点上的宽限时间；不是 CommitReload 后另起的倒计时。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Reload", meta = (ClampMin = "0.0"))
	float ReloadCompletionGracePeriod = 0.1f;

	// ==================== Runtime State ====================
	// Owner
	//运行时状态
	//当前持有该武器的角色，EndPlay时置空
	// 这是从武器指回角色的反向观察关系；TWeakObjectPtr 避免双方互相形成强引用。
	UPROPERTY(Transient)
	TWeakObjectPtr<AfpstrueCharacter> Character;

	// Action State
	//Ready、Firing、Reloading、Disabled四种互斥动作状态
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|State")
	EFPWeaponActionState ActionState = EFPWeaponActionState::Disabled;

	// Ammo
	//当前弹匣和备弹数量
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 CurrentAmmo = 0;
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 ReserveAmmo = 0;

	// Reload Transaction
	// 同一次换弹中，多个已绑定 Notify / 显式完成回调最多只转移一次弹药。
	bool bReloadAmmoCommitted = false;
	// 每次动作切换递增；同步委托可能取消并重新请求，旧 Fire/Finish 不得操作新动作。
	uint32 ActionRevision = 0;
	int32 ActiveReloadId = 0;
	TArray<FFPActionPlayback, TInlineAllocator<2>> ReloadPlaybacks;
	// 播放/停止可同步调用动画委托：此范围允许取消，禁止重入启动另一动作或播放。
	bool bChangingReloadPlayback = false;
	bool bInterruptingOwnerInput = false;
	bool bAmmoInitialized = false;
	bool bEndingPlay = false;
	bool bDetaching = false;

	// Fire / Spread
	//连续射击和后坐力状态
	int32 ConsecutiveShotCount = 0;
	double LastShotTimeSeconds = -1.0;

	// Recoil
	float AccumulatedRecoilPitch = 0.0f;
	float AccumulatedRecoilYaw = 0.0f;
	double LastRecoilRecoveryTimeSeconds = 0.0;

	// Recovery / Timers
	// Timer 延后在 Game Thread 回调，不是 Worker 并行任务；EndPlay 时统一清理句柄。
	FTimerHandle AutomaticFireTimerHandle;
	FTimerHandle ReloadTimerHandle;
	FTimerHandle RecoilRecoveryTimerHandle;
};
