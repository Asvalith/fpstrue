// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Camera/CameraShakeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"
#include "Weapons/fpstrueWeaponConfig.h"
#include "Runtime/fpstrueRuntime.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class APlayerController;
class UCameraComponent;
class UCameraShakeBase;
class UAnimInstance;
class UAnimMontage;
class UParticleSystem;
class USoundBase;
class UMaterialInterface;
class UStaticMeshComponent;
class UWorld;
class UfpstrueFireAbility;
class UfpstrueReloadAbility;
class UfpstrueAimAbility;
struct FAnimNotifyEventReference;

// 每发只携带输入身份、时间和方向；目标、起点、伤害始终由服务器产生。
USTRUCT()
struct FFPShotRequest
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<AfpstrueCharacter> Avatar = nullptr;
	UPROPERTY() uint32 EquipmentGeneration = 0;
	UPROPERTY() uint32 FireSession = 0;
	UPROPERTY() uint32 ShotId = 0;
	UPROPERTY() double ServerFireTime = 0.0;
	UPROPERTY() FVector_NetQuantizeNormal AimDirection = FVector::ForwardVector;
};

UENUM()
enum class EFPShotResult : uint8
{
	Accepted, Queued, InvalidContext, InvalidIdentity, Duplicate, InvalidTime,
	InvalidAim, RateLimited, Busy, NoAmmo, Cancelled
};

// GAS 标签派生的武器表现状态；仅用于快照和蓝图查询，不另管动作互斥。
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

// ★【网络边界】这里只复制服务器的当前结果，不把客户端动画通知当作装弹或伤害授权。
USTRUCT()
struct FFPReplicatedWeaponState
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<AfpstrueCharacter> Holder = nullptr;
	UPROPERTY() EFPWeaponActionState Action = EFPWeaponActionState::Disabled;
	UPROPERTY() int32 Ammo = 0;
	UPROPERTY() int32 Reserve = 0;
	UPROPERTY() int32 ReloadId = 0;
	// 同一 ReloadId 的表现时钟：晚加入/重连从当前阶段播放，不重新开始服务器事务。
	UPROPERTY() double ReloadStartServerTime = 0.0;
	UPROPERTY() float ReloadDuration = 0.0f;
	UPROPERTY() bool bEmptyReload = false;
	UPROPERTY() bool bCommitted = false;
	UPROPERTY() EFPReloadEndReason EndReason = EFPReloadEndReason::Completed;
	UPROPERTY() uint32 Revision = 0;
	// 和弹药同批到达的表现进度，只帮助本地换弹资格；不授权客户端命中或数值结算。
	UPROPERTY() int32 FirePresentationKey = 0;
	UPROPERTY() int32 FirePresentationShot = 0;
	UPROPERTY() uint32 EquipmentGeneration = 0;
	UPROPERTY() uint32 FireSession = 0;
	UPROPERTY() uint32 LastProcessedShotId = 0;
	UPROPERTY() EFPShotResult ShotResult = EFPShotResult::Accepted;
	// 死亡掉落是持久终态，不用一次性 RPC；初始条件只用于首帧，后续位姿交给 UE 物理复制。
	UPROPERTY() bool bDroppedOnDeath = false;
	UPROPERTY() FTransform DropTransform;
	UPROPERTY() FVector_NetQuantize10 DropVelocity = FVector::ZeroVector;
};

//动态多播委托类型
//成功执行一次射击时广播
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FWeaponFireEvent);
// 换弹执行器报告事务结束，能力据此退出；不另建装弹计时或扣弹路径。
DECLARE_MULTICAST_DELEGATE_ThreeParams(FWeaponReloadStopped, int32, EFPReloadEndReason, bool);
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
 * 玩家武器模块：独占装备、弹药事务、Hitscan、散布和后坐力，并通过事件驱动 HUD/蓝图。
 * 动作互斥与连射调度由 GA 管理；本组件只执行单发射击和带播放身份的换弹事务。
 *
 * Character 向 ASC 提交输入；Action 枚举只展示 GAS 结果，不参与业务授权。
 * 单机用 Notify 提交换弹，联网由服务器时长完成；Timer 处理换弹、后坐力和已准入网络射击，不生成新的连射输入。
 * 死亡或卸下先禁用装备，再取消相关能力，不能由旧回调重新启用武器。
 * Reloading 中提交只转移弹药；有身份的完成/中断、主动取消或超时才解除动作锁。
 */
UCLASS(Blueprintable, BlueprintType, ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueWeaponComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:
	// 默认子组件参与复制；动作请求走角色 ASC，武器多播只传服务器已结算的单发表现。
	UfpstrueWeaponComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 原生根网格只复制业务快照；装备挂接由 Holder 驱动，掉落位姿由 Actor 物理复制，旧蓝图组件保留默认复制。
	virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;
	// 装备引用和组件快照可能先后到达，两个入口都可以幂等补齐本地附着及 HUD。
	void RefreshNetworkEquipment();
	// ★【死亡边界】保留死 Pawn 的装备引用供回收识别，但永久停止业务并从第三人称手部自然掉落。
	void DropForDeath(const FVector& InheritedVelocity);
	bool IsDroppedOnDeath() const { return bDroppedOnDeath; }
	// Actor 的权威物理可能早于组件快照到达；先建立无挂接物理体，再由引擎消费当前物理状态。
	void RefreshNetworkPhysics();
	// 显式报告本武器请求被拒绝，不能授权射击或伪造成功 ACK；ASC 旧会话只丢弃，不转交当前装备。
	void RejectNetworkShot(const FFPShotRequest& Request, EFPShotResult Result = EFPShotResult::InvalidContext);
	// 重新占有保留武器时作废旧连接输入身份；保留权威弹药与实际射击冷却，不重放旧请求。
	void ResetNetworkInputGeneration();
	// 蓝图始终读取原生装备关系；自动发枪与复制恢复不依赖旧拾取事件写入 owningactor。
	UFUNCTION(BlueprintPure, Category = "Weapon|Equipment")
	AfpstrueCharacter* GetOwningCharacter() const { return Character.Get(); }
	// 原生武器只在已有事实事件后播放枪械动画；这些资源不定义开火/换弹资格，也不结算弹药。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Presentation")
	FText WeaponDisplayName;
	// 作者的枪械握持偏移只改变模型装配；不参与瞄准、命中或服务器伤害判定。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Presentation")
	FVector GripLocationOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Presentation")
	FRotator GripRotationOffset = FRotator::ZeroRotator;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UAnimMontage> WeaponFireMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UAnimMontage> WeaponReloadMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UAnimMontage> WeaponEmptyReloadMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<USoundBase> FireSound;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UParticleSystem> MuzzleEffect;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UParticleSystem> ImpactEffect;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UParticleSystem> FleshImpactEffect;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<USoundBase> ImpactSound;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<UMaterialInterface> ImpactDecal;
	// 枪口来自本枪附件的实际 Socket，不借用手臂位置或另一位玩家的相机。
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> MuzzleSource;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	FName MuzzleSocketName;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	FVector MuzzleForwardAxis = FVector::ForwardVector;
	// ==================== Equipment ====================
	//装备是否成功
	bool AttachWeapon(AfpstrueCharacter* TargetCharacter);
	// 完整解除装备关系；外部 Pickup Actor 的组件不会随玩家自动结束生命周期。
	void DetachWeapon();

	// ==================== Fire ====================
	// 同步兼容入口向 ASC 提交开火按下；资格、互斥与连射任务由 Fire GA 管理。
	void StartFire();
	// 同步兼容入口向 ASC 提交开火松开；不结束换弹或重新启用装备。
	void StopFire();
	// GAS 管启动/互斥/取消；以下执行器保留唯一射速、弹药、命中及 ReloadId 事务。
	// 先让 GAS 结束旧能力并释放标签，再把同一个完成事实通知原蓝图观察者。
	FWeaponReloadStopped OnAbilityReloadEnded;

	// ==================== Reload ====================
	// 向 ASC 请求 Reload GA，由能力检查弹匣、备弹及动作标签。
	// 换弹分为请求、弹药提交、结束三个阶段；取消只结束流程，不撤销已经提交的弹药。
	// GA 先持有 Reloading 标签，执行器再建立 ReloadId 与计时后请求播放；蓝图选材后走显式播放接口。
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

	// 返回当前换弹编号；只供即时查询，延迟回调必须保存启动时取得的编号。
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	int32 GetActiveReloadId() const { return ActiveReloadId; }
	// 只读服务器时间快照；动画可据此对齐，不能用播放进度反向提交网络换弹。
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	float GetReloadProgress() const;
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	float GetReloadDuration() const { return NetworkState.ReloadDuration; }
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	double GetReloadStartServerTime() const { return NetworkState.ReloadStartServerTime; }
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	bool IsEmptyReloadPresentation() const { return NetworkState.bEmptyReload; }

	// 提交指定事务的装弹，至多转移一次；旧编号不能借用当前换弹。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	bool CommitReloadForTransaction(int32 ReloadId);

	// 正常结束指定事务；需要时补交一次弹药，再统一清理动画和动作锁。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void FinishReloadForTransaction(int32 ReloadId);

	// 中断指定事务，只结束流程，不补弹也不撤销此前已提交的弹药。
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
	// 仅供 HUD/蓝图观察；冲刺、瞄准等动作规则由 GAS 标签决定。
	UFUNCTION(BlueprintPure, Category = "Weapon|State")
	bool IsReloading() const;

	//检查是否正在开火，供蓝图限制 Montage 重复播放；查询本身不控制动画。
	UFUNCTION(BlueprintPure, Category = "Weapon|State")
	bool IsFiring() const;

	// ==================== Ammo Query ====================
	//UI可查询子弹数量相关
	// HUD 初始化时读取当前弹匣；变化由 OnAmmoChanged 推送。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentAmmo() const { return CurrentAmmo; }

	// HUD 初始化时读取弹匣容量。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetMagazineSize() const { return GetWeaponSettings().MagazineSize; }

	// 装备前读取所选配置供预览；装备后始终返回本实例快照，不因共享资产变化而改动进行中的动作。
	const FFPWeaponSettings& GetWeaponSettings() const;

	// HUD 初始化时读取剩余备弹。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetReserveAmmo() const { return ReserveAmmo; }
#if !UE_BUILD_SHIPPING
	int32 GetPendingPredictedShotCount() const { return OutstandingShots.Num(); }
#endif

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
	// 新原生武器通过同一事实委托驱动枪械；未配置表现资源的原蓝图继续保留原接线。
	virtual void BeginPlay() override;
	// 组件的附着属性可晚于 Actor/装备快照到达；收完本批通知后再恢复本端视角挂接。
	virtual void PostRepNotifies() override;
	//UObject/ActorComponent 生命周期结束前调用
	//这里主要应该清理Timer、武器状态、Character引用等
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 可反复确保物理/可见性，只有显式初始条件才设置位姿和速度，避免重复通知把落地枪弹回手边。
	void ApplyDeathDropPhysics(const FTransform* InitialTransform = nullptr, const FVector* InitialVelocity = nullptr);
	UFUNCTION() void PlayWeaponFirePresentation();
	UFUNCTION() void PlayWeaponReloadPresentation(int32 ReloadId, bool bWasEmptyReload);
	UFUNCTION() void PlayWeaponImpactPresentation(bool bHit, FVector TraceStart, FVector TraceEnd, FVector TraceTarget, FHitResult HitResult);
	friend class AfpstrueRARWeapon;
	friend class AfpstrueTemplateWeapon;
	friend class UfpstrueFireAbility;
	friend class UfpstrueReloadAbility;
	friend class UfpstrueAimAbility;
	friend class FFpstrueMultiplayerShotProtocolTest;
	// 只有 GA 能调用执行器；公开 Start/Stop/Request 只是 ASC 请求入口，不再直接启动业务。
	bool CanExecuteShot() const;
	bool HasReloadAmmo() const;
	void BeginAbilityFire(int32 PredictionKey);
	void EndAbilityFire(bool bDiscardOutstandingShots = false);
	bool BuildShotRequest(FFPShotRequest& OutRequest);
	// 固定容量的准入队列只保存合法输入；唯一执行 Timer 按绝对射速节拍消费，不在同帧补射。
	EFPShotResult AcceptShotRequest(const FFPShotRequest& Request);
	bool ExecuteNetworkShot(const FFPShotRequest& Request);
	bool HasPendingNetworkShots() const { return !PendingNetworkShots.IsEmpty(); }
	bool ExecuteNextPendingNetworkShot();
	void SchedulePendingNetworkShot();
	void ProcessPendingNetworkShot();
	// 强取消先移走所有待执行记录，再逐发确认，外部回调不能重新消费旧输入。
	void CancelPendingNetworkShots();
	void AcknowledgeShot(const FFPShotRequest& Request, EFPShotResult Result);
	bool BeginAbilityReload(int32& OutReloadId);
	// 网络状态仅由服务器发布；RepNotify 只建立本地关系、更新 HUD 和播放表现。
	void PublishNetworkState();
	UFUNCTION() void OnRep_NetworkState();
	// 单发结果是瞬时表现；弹药和动作状态另由属性复制保证晚加入可恢复。
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastShot(FVector_NetQuantize TraceStart, FVector_NetQuantize TraceEnd, const FHitResult& Hit,
		int32 PresentationKey, int32 ShotIndex, uint32 EquipmentGeneration, uint32 Session);
	// 拒绝也确认，客户端才能释放该发的弹药估计；版本阻止迟到 ACK 覆盖更新的弹药。
	UFUNCTION(Client, Reliable)
	void ClientShotResult(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId, EFPShotResult Result,
		int32 Ammo, int32 Reserve, uint32 StateRevision);
	UPROPERTY(ReplicatedUsing = OnRep_NetworkState)
	FFPReplicatedWeaponState NetworkState;
	bool bNetworkEmptyReload = false;
	EFPReloadEndReason LastNetworkReloadEndReason = EFPReloadEndReason::Completed;
	int32 LastPresentedReloadId = INDEX_NONE;
	// ==================== Equipment Eligibility / Read-only Action View ====================
	// 动作阶段由 GAS 独占；装备资格和只读网络展示不构成第二套动作状态机。
	//状态边界
	//统一检查武器已装备、未禁用且角色存活
	bool IsOperational() const;
	// 临时输入禁入与永久装备禁用分开；清理或播放变化期间不允许回调重启动作。
	bool CanAcceptOwnerInput() const;
	// 同步委托返回后检查动作是否仍是原来那次，避免旧调用继续操作新动作。
	// 同步回调的归属由 GA 激活版本和装备版本校验，不再维护第二套动作版本。
	// 射击和换弹规则读取弹匣是否还有弹药。
	bool HasAmmo() const { return CurrentAmmo > 0; }
	// 只验证当前事务编号；GAS 标签解除后仍可清理旧事务，装备有效性由执行入口检查。
	bool IsCurrentReload(int32 ReloadId) const;
	// 按结束原因收口一次换弹；先失效旧状态和委托，再停止旧播放并发结束事件。
	void EndReload(int32 ReloadId, EFPReloadEndReason Reason);
	// 只接收已绑定实例的结束；全部自然结束才成功，任一中断立即取消本次换弹。
	void HandleReloadPlaybackEnded(UAnimMontage* Montage, bool bInterrupted,
		int32 ReloadId, TWeakObjectPtr<USkeletalMeshComponent> PlaybackMesh, int32 InstanceId);

	// ==================== Fire System ====================
	// GA 的单机 Pulse 与网络已准入队列共用的单发执行入口，不生成新的输入。
	bool Fire(const FFPShotRequest* Request = nullptr, bool bAimingAtAdmission = false, int32 AdmittedPresentationKey = 0);
	// 提供上一发剩余冷却；松开再按不会重新等待完整间隔。
	// GA 调度本地输入，武器执行已准入的网络请求；二者读取同一射速预算。
	float GetNextShotDelay(bool bAdmittedNetworkSchedule = false) const;
	// 初次空仓与最后一发耗尽共用 GA 的换弹分支：能换弹就换弹，否则停止射击。
	// 空仓由 Fire GA 结束自身，再在服务器激活同一个 Reload GA。
	// 只预测拥有者的枪声、开火动画和后坐力；命中与弹药仍等待服务器，不伪称逐发数值回滚。
	bool PresentPredictedShot(const FFPShotRequest& Request);
	// 单机根据相机计算；联网使用历史服务器视点和逐发瞄准方向，再应用本发散布和命中查询。
	// 执行一条带散布的射线，处理伤害、冲量和命中事件。
	void FireLineTrace(UWorld& World, AfpstrueCharacter& OwningCharacter, UCameraComponent& Camera,
		int32 PresentationKey, int32 ShotIndex, const FFPShotRequest* Request, bool bAimingForShot);

	// ==================== Recoil System ====================
	// 射击成功后把随机水平/垂直后坐力应用到 PlayerController。
	void ApplyRecoil(APlayerController* PlayerController);
	// Timer 驱动相机逐步抵消累计后坐力。
	void UpdateRecoilRecovery();
	// 停止恢复 Timer 并清空累计后坐力。
	void ClearRecoilState();
	// 视觉后坐力独立收尾；不停止角色受伤等其他来源的 CameraShake。
	void StopVisualRecoil();

	// ==================== Runtime Helpers ====================
	//弹药改变后统一广播给UI
	void BroadcastAmmoChanged();

	// ==================== Configuration ====================
	// 唯一可编辑的数值入口；原蓝图或原生武器实体只配置各自的表现资源。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Configuration")
	TObjectPtr<UfpstrueWeaponConfig> WeaponConfiguration;

	// 首次装备复制并校验配置；之后卸下/重装只保留快照与剩余弹药。
	// 未指定资产的原生组件使用结构体默认值，组件不再保留第二组可编辑数值。
	FFPWeaponSettings Settings;

	// ==================== Runtime State ====================
	// Owner
	//运行时状态
	//当前持有该武器的角色，EndPlay时置空
	// 语法复习：这是从武器指回角色的反向观察关系；TWeakObjectPtr 避免双方互相形成强引用。
	UPROPERTY(Transient)
	TWeakObjectPtr<AfpstrueCharacter> Character;

	// Equipment / Presentation
	// Ready、Firing、Reloading、Disabled 仅表示服务器派生的展示结果。
	// 四种名称仅用于派生显示；装备资格不是动作互斥状态机。
	bool bEquipmentEnabled = false;
	// 独立单向锁存：本端已确认死亡后，旧属性中 false/Ready 不能撤销掉枪。
	bool bDroppedOnDeath = false;
	bool bReceivedAuthoritativePhysics = false;
	bool bAppliedAuthoritativeDrop = false;
	bool bDeathDropPhysicsConfigured = false;
	uint32 EquipmentRevision = 0;
	EFPWeaponActionState LastPresentedAction = EFPWeaponActionState::Disabled;

	// Ammo
	//当前弹匣和备弹数量
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 CurrentAmmo = 0;
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 ReserveAmmo = 0;

	// Reload Transaction
	// 同一次换弹中，多个已绑定 Notify / 显式完成回调最多只转移一次弹药。
	bool bReloadAmmoCommitted = false;
	// 每次接纳换弹递增；同步委托可能取消并重新请求，旧 Finish 不得操作新事务。
	// 动作版本归 GA；这里只用递增事务编号区别两次装填。
	int32 ReloadSerial = 0;
	int32 ActiveReloadId = INDEX_NONE;
	TArray<FFPActionPlayback, TInlineAllocator<3>> ReloadPlaybacks;
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
	// 保留理想节拍而非每发从实际回调时间重新计时，避免低服务器帧率将量化延迟逐发累加。
	double NextShotTimeSeconds = -1.0;
	// 激活键只关联 GA；逐发身份由装备代际、开火会话和全武器递增 ShotId 共同确定。
	int32 FirePresentationKey = 0;
	int32 FirePresentationShot = 0;
	uint32 FireSession = 0;
	uint32 LocalFireSessionSerial = 0;
	uint32 LastRemoteFireSession = 0;
	uint32 LocalShotSerial = 0;
	uint32 FirstPredictedShotForCurrentFire = 0;
	uint32 LastReceivedShotId = 0;
	uint32 LastExecutedShotId = 0;
	// 这是已授权事实的执行队列，不管理输入动作状态；普通松开可结束 GA 而不取消合法旧输入。
	static constexpr int32 MaxQueuedNetworkShots = 4;
	struct FPendingNetworkShot
	{
		FFPShotRequest Request;
		bool bAimingAtAdmission = false;
		int32 PresentationKey = 0;
	};
	TArray<FPendingNetworkShot, TInlineAllocator<MaxQueuedNetworkShots>> PendingNetworkShots;
	uint32 LastProcessedShotId = 0;
	EFPShotResult LastShotResult = EFPShotResult::Accepted;
	double LastSubmittedFireTime = -1.0;
	uint32 LastAmmoStateRevision = 0;
	TArray<uint32, TInlineAllocator<32>> OutstandingShots;
	double LastPredictedShotTime = -1.0;
	bool bPredictingFire = false;
	struct FPredictedFirePresentation
	{
		int32 Key = 0;
		uint32 Generation = 0;
		uint32 Session = 0;
		int32 PresentedShots = 0;
		int32 ConfirmedShots = 0;
	};
	TArray<FPredictedFirePresentation, TInlineAllocator<8>> PredictedPresentations;

	// Recoil
	float AccumulatedRecoilPitch = 0.0f;
	float AccumulatedRecoilYaw = 0.0f;
	double LastRecoilRecoveryTimeSeconds = 0.0;
	TWeakObjectPtr<UCameraShakeBase> VisualRecoilShake;

	// Recovery / Timers
	// Timer 延后在 Game Thread 回调，不是 Worker 并行任务；EndPlay 时统一清理句柄。
	FTimerHandle ReloadTimerHandle;
	FTimerHandle RecoilRecoveryTimerHandle;
	FTimerHandle NetworkShotTimerHandle;
};

// Copyright Epic Games, Inc. All Rights Reserved.

struct FFPWeaponSettings;

// 单发视觉脉冲：只修改 CameraManager 最终 POV，不写 Controller 或用于 Hitscan 的 Camera 组件。
// Shake 与 Pattern 是 UE 的两层接口，集中在同一文件；不增加 Actor、Tick 或恢复 Timer。
UCLASS()
class FPSTRUE_API UfpstrueWeaponRecoilPattern : public UCameraShakePattern
{
	GENERATED_BODY()

public:
	// 从当前武器配置复制视觉脉冲参数，不改变控制旋转、弹药或命中方向。
	void Configure(const FFPWeaponSettings& Settings);

private:
	// 向相机系统声明自定义时长，实际计时、回正和完成由本 Pattern 自己计算。
	virtual void GetShakePatternInfoImpl(FCameraShakeInfo& OutInfo) const override;
	// 新射击或重触发时从当前画面偏移继续脉冲，避免连发累积多层 Shake。
	virtual void StartShakePatternImpl(const FCameraShakePatternStartParams& Params) override;
	// 按经过时间输出旋转和 FOV 偏移；只作用于 CameraManager 最终画面。
	virtual void UpdateShakePatternImpl(const FCameraShakePatternUpdateParams& Params, FCameraShakePatternUpdateResult& OutResult) override;
	// 主动停止或单次脉冲耗尽后，允许相机系统回收该播放。
	virtual bool IsFinishedImpl() const override { return bStopped || Elapsed >= Duration; }
	// 强制停止并归零脉冲；正常松开扳机不会走此清理入口。
	virtual void StopShakePatternImpl(const FCameraShakePatternStopParams& Params) override;

	FRotator PeakRotation = FRotator::ZeroRotator;
	FRotator StartRotation = FRotator::ZeroRotator;
	FRotator CurrentRotation = FRotator::ZeroRotator;
	float PeakFOV = 0.0f;
	float StartFOV = 0.0f;
	float CurrentFOV = 0.0f;
	float Duration = 0.12f;
	float Elapsed = 0.0f;
	float Side = 1.0f;
	bool bStopped = true;
};

// 同一相机最多一个本类实例；连发重触发脉冲，不让震动层数随子弹数增长。
UCLASS()
class FPSTRUE_API UfpstrueWeaponCameraShake : public UCameraShakeBase
{
	GENERATED_BODY()

public:
	// 创建单实例视觉脉冲 Pattern，连发复用同一类 Shake 而非无限叠加实例。
	UfpstrueWeaponCameraShake(const FObjectInitializer& ObjectInitializer);
};

/** 换弹提交点：在动画指定帧把备弹真正转移到弹匣。 */
UCLASS(meta = (DisplayName = "Reload Commit"))
class FPSTRUE_API UfpstrueAnimNotify_ReloadCommit : public UAnimNotify
{
	GENERATED_BODY()

public:
	// 找到武器后校验通知的 Montage 实例身份，再提交对应换弹事务。
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,const FAnimNotifyEventReference& EventReference) override;
	// 将 Branching Point 的原始播放实例编号转为同一身份校验入口，不猜测当前播放。
	virtual void BranchingPointNotify(FBranchingPointNotifyPayload& BranchingPointPayload) override;

	// 返回动画编辑器中显示的 Notify 名称。
	virtual FString GetNotifyName_Implementation() const override;
};

// ==================== 武器拾取入口 ====================
// Copyright Epic Games, Inc. All Rights Reserved.



class AfpstrueCharacter;

//声明一个Onpickup动态多播委托，用于拾取后广播
// 语法复习：声明指针类型只需要前置声明；只有访问成员或需要完整布局时才 include 对应头文件。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPickUp, AfpstrueCharacter*, PickUpCharacter);
//声明组件类 UfpstruePickUpComponent
/** 一次性拾取触发器：检测玩家进入球形范围并向蓝图广播拾取事件。 */
UCLASS(Blueprintable, BlueprintType, ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))

//FPSTRUE_API模块导入导出宏，解决不同 UE 模块/DLL 之间的符号可见性和链接问题
class FPSTRUE_API UfpstruePickUpComponent : public USphereComponent
{
	GENERATED_BODY()

public:
	//初始化UfpstruePickUpComponent组件
	UfpstruePickUpComponent();
	//创建拾取事件暴露给蓝图，作为事件分发器
	UPROPERTY(BlueprintAssignable, Category = "Interaction")
	FOnPickUp OnPickUp;

protected:
	// 绑定球形碰撞的重叠回调。
	virtual void BeginPlay() override;
	// 父级递归显示手臂/枪械时，拾取范围仍只作碰撞查询，不渲染红色球体。
	virtual void OnHiddenInGameChanged() override;

	//Sphere 真正发生重叠以后调用
	UFUNCTION()
	void OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
							  int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

private:
	//防重复拾取的标志位，初始值为false，表示未被拾取
	bool bConsumed = false;
};

// RAR 使用独立原生拾取实体，避免继承旧 RU74 蓝图的表现事件；玩法仍由同一个武器组件和 GAS 执行。
UCLASS()
class FPSTRUE_API AfpstrueRARWeapon : public AActor
{
	GENERATED_BODY()

public:
	AfpstrueRARWeapon();
	// 只修改本端实例材质：世界枪/他人枪为 0，自己的第一人称枪及附件为 1，不改共享材质或全局 MPC。
	void SetViewmodelFOVEnabled(bool bEnabled);
	// 根组件的服务器挂接晚到时，重新应用本端第一/第三人称挂点，不改变服务器装备归属。
	virtual void OnRep_AttachmentReplication() override;
	virtual void OnRep_ReplicatedMovement() override;
	virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;
	virtual void PostInitializeComponents() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstrueWeaponComponent> Weapon;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstruePickUpComponent> Pickup;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> Forestock;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> Magazine;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> FrontSight;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UStaticMeshComponent> RearSight;
	// -1 表示当前实例尚未初始化；只缓存材质模式，不参与任何玩法状态或网络复制。
	int8 AppliedViewmodelFOVMode = -1;
};

// UE5.5 模板只提供匹配的枪械/动画素材；拾取、弹药、命中和联网仍复用项目现有组件与 GAS。
UCLASS()
class FPSTRUE_API AfpstrueTemplateWeapon : public AActor
{
	GENERATED_BODY()

public:
	AfpstrueTemplateWeapon();
	// Actor 附着与组件快照可以分批到达，始终以本端实际持有者恢复正确的第一/第三人称装配。
	virtual void OnRep_AttachmentReplication() override;
	virtual void OnRep_ReplicatedMovement() override;
	virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;
	// 专服不运行弹匣/枪栓等纯表现动画，保留武器业务组件及挂点。
	virtual void PostInitializeComponents() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstrueWeaponComponent> Weapon;
	UPROPERTY(VisibleAnywhere, Category = "Weapon")
	TObjectPtr<UfpstruePickUpComponent> Pickup;
};
