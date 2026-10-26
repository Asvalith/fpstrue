// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Camera/CameraShakeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"
#include "Weapons/fpstrueWeaponConfig.h"
#include "Runtime/fpstrueRuntime.h"
#include "GameplayAbilitySpecHandle.h"
#include "GameplayPrediction.h"
#include "fpstrueWeaponComponent.generated.h"

class AfpstrueCharacter;
class AfpstrueRARWeapon;
class AfpstrueTemplateWeapon;
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
class UfpstrueHealthComponent;
struct FAnimNotifyEventReference;

// 每个远端玩家复用原生CMC线性平滑的显示时刻；只传身份和相对时间，不传位置或命中结论。
struct FFPShotRenderedView
{
	TWeakObjectPtr<AfpstrueCharacter> Target;
	int16 AgeMilliseconds = 0;
};

// 每发只携带输入身份、时间和方向；目标、起点、伤害始终由服务器产生。
// 【设计取舍】装备代际隔离卸下/重装，会话隔离不同按键轮次，ShotId 隔离逐发请求。
// 三者不能只留一个“当前编号”：旧包可能在换枪或新一轮开火后才到达。
USTRUCT()
struct FFPShotRequest
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<AfpstrueCharacter> Avatar = nullptr;
	// 两把枪可有相同代际与 ShotId；真实 Actor 身份防止旧数据被当前装备冒领。
	UPROPERTY() TObjectPtr<AActor> Weapon = nullptr;
	UPROPERTY() uint32 EquipmentGeneration = 0;
	UPROPERTY() uint32 FireSession = 0;
	UPROPERTY() uint32 ShotId = 0;
	UPROPERTY() double ServerFireTime = 0.0;
	UPROPERTY() FVector_NetQuantizeNormal AimDirection = FVector::ForwardVector;
	static constexpr int32 MaxRenderedViews = 16;
	static constexpr int32 MinRenderedViewAgeMilliseconds = -50;
	static constexpr int32 MaxRenderedViewAgeMilliseconds = 350;
	// 自定义NetSerialize有界传输；Inline避免每次输入重发为少量玩家时间戳单独分配。
	TArray<FFPShotRenderedView, TInlineAllocator<MaxRenderedViews>> RenderedViews;
	// 找不到该目标的真实显示时间才使用Fallback；找到后不clamp、不换时间以凑历史命中。
	double GetTargetViewTime(const AfpstrueCharacter* Target, double Fallback, bool* bOutNativeView = nullptr) const;
	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

template<> struct TStructOpsTypeTraits<FFPShotRequest> : TStructOpsTypeTraitsBase2<FFPShotRequest>
{
	enum { WithNetSerializer = true, WithCopy = true };
};

UENUM()
enum class EFPShotResult : uint8
{
	Accepted, Queued, InvalidContext, InvalidIdentity, Duplicate, InvalidTime,
	InvalidAim, RateLimited, Busy, NoAmmo, Cancelled
};

// 已经终结的同一发裁决；近期快速窗口只重复携带事实，不重新扣弹或发布状态。
USTRUCT()
struct FFPShotReceipt
{
	GENERATED_BODY()
	UPROPERTY() uint32 EquipmentGeneration = 0;
	UPROPERTY() uint32 Session = 0;
	UPROPERTY() uint32 ShotId = 0;
	UPROPERTY() EFPShotResult Result = EFPShotResult::InvalidContext;
	UPROPERTY() int32 Ammo = 0;
	UPROPERTY() int32 Reserve = 0;
	UPROPERTY() uint32 StateRevision = 0;
	UPROPERTY() uint32 IncludedShotId = 0;
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
	UPROPERTY() int32 ReloadId = 0;
	// 同一次 GAS 激活的键：正数客户端预测，负数服务器主动；符号隔离原生两类键。
	UPROPERTY() int32 ReloadPredictionKey = 0;
	// 同一 ReloadId 的表现时钟：晚加入/重连从当前阶段播放，不重新开始服务器事务。
	UPROPERTY() double ReloadStartServerTime = 0.0;
	UPROPERTY() float ReloadDuration = 0.0f;
	UPROPERTY() bool bEmptyReload = false;
	UPROPERTY() bool bCommitted = false;
	UPROPERTY() EFPReloadEndReason EndReason = EFPReloadEndReason::Completed;
	UPROPERTY() uint32 Revision = 0;
	UPROPERTY() uint32 EquipmentGeneration = 0;
	// 死亡掉落是持久终态，不用一次性 RPC；初始条件只用于首帧，后续位姿交给 UE 物理复制。
	UPROPERTY() bool bDroppedOnDeath = false;
	UPROPERTY() FTransform DropTransform;
	UPROPERTY() FVector_NetQuantize10 DropVelocity = FVector::ZeroVector;
};

// 拥有者私有的派生结果；与公共快照共用发布版本和装备代际，不构成第二套弹药业务状态。
USTRUCT()
struct FFPReplicatedWeaponOwnerState
{
	GENERATED_BODY()
	UPROPERTY() uint32 EquipmentGeneration = 0;
	UPROPERTY() uint32 Revision = 0;
	UPROPERTY() int32 Ammo = 0;
	UPROPERTY() int32 Reserve = 0;
	// 本快照弹药已包含的最高实际射击编号；未收到 ACK 的旧射击不能再次扣减显示值。
	UPROPERTY() uint32 IncludedShotId = 0;
};

//动态多播委托类型
//成功执行一次射击时广播
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FWeaponFireEvent);
// 换弹执行器报告事务结束，能力据此退出；不另建装弹计时或扣弹路径。
DECLARE_MULTICAST_DELEGATE_ThreeParams(FWeaponReloadStopped, int32, EFPReloadEndReason, bool);
// 播放请求携带换弹编号及空仓标记；接收者只能为这次事务选择动画。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FWeaponReloadPlaybackEvent, int32, ReloadId, bool, bWasEmptyReload);
// 已提交结束状态后报告原因及装弹结果，不授权监听者绕过事务修改弹药。
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
 * 原生唯一 Timer 按服务器时长完成换弹；Timer 另处理后坐力和已准入网络射击，不生成新的连射输入。
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
	// 装备/动作复制给观察者，弹药仅复制给拥有者；逐发裁决走拥有者可靠 ACK。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 原生根网格只复制业务快照；装备挂接由 Holder 驱动，掉落位姿由 Actor 物理复制，旧蓝图组件保留默认复制。
	virtual void GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const override;
	// 装备引用和组件快照可能先后到达，两个入口都可以幂等补齐本地附着及 HUD。
	void RefreshNetworkEquipment();
	// ★【死亡边界】保留死 Pawn 的装备引用供回收识别，但永久停止业务并从第三人称手部自然掉落。
	void DropForDeath(const FVector& InheritedVelocity);
	// 查询本端已锁存的死亡掉落终态；旧装备/附着包不能把它恢复为手持。
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
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	FVector ImpactDecalSize = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation", meta = (ClampMin = "0.0"))
	float ImpactDecalLifeSeconds = 0.0f;
	// 由武器蓝图绑定本枪组件；UE 实例化将默认子对象引用映射到对应实例，不跨网复制。
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	TObjectPtr<USceneComponent> MuzzleSource;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	FName MuzzleSocketName;
	UPROPERTY(EditDefaultsOnly, Category = "Weapon|Presentation")
	FVector MuzzleForwardAxis = FVector::ForwardVector;
	// ==================== Equipment ====================
	// 权威端完整装备事务；成功时双方关系、挂点和初始弹药均已提交，失败不留下半装备。
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

	// 无参数仅供同步取消命令；延迟中断回调须保存 ReloadId，调用 CancelReloadForTransaction。
	// 提交前取消不装弹，提交后取消不退弹。
	UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
	void CancelReload();

	// 返回当前换弹编号；只供即时查询，延迟回调必须保存启动时取得的编号。
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	int32 GetActiveReloadId() const { return ActiveReloadId; }
	// 用服务器时间计算本轮归一化进度，仅供动画对齐，不能反向提交网络换弹。
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	float GetReloadProgress() const;
	// 查询本轮服务器选定的换弹时长，仅供本地动画对齐。
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	float GetReloadDuration() const;
	// 查询本轮是否空仓起手，以选择对应素材，不据此决定补弹量。
	UFUNCTION(BlueprintPure, Category = "Weapon|Reload")
	bool IsEmptyReloadPresentation() const;

	// 原生完成入口内部提交一次，蓝图无装弹写入权限；旧编号不能借用当前换弹。
	bool CommitReloadForTransaction(int32 ReloadId);

	// 原生计时捕获编号后完成事务；不向蓝图暴露第二个动画完成入口。
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
	int32 GetCurrentAmmo() const;

	// HUD 初始化时读取弹匣容量。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetMagazineSize() const { return GetWeaponSettings().MagazineSize; }

	// 装备前读取所选配置供预览；装备后始终返回本实例快照，不因共享资产变化而改动进行中的动作。
	const FFPWeaponSettings& GetWeaponSettings() const;

	// HUD 初始化时读取剩余备弹。
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetReserveAmmo() const { return ReserveAmmo; }
#if !UE_BUILD_SHIPPING
	// 开发验收读取尚无终结 ACK 的输入数；不把它当作权威弹药。
	int32 GetPendingPredictedShotCount() const { return OutstandingShots.Num(); }
	// 只读监测权威执行队列，容量验收不能拿“已停止输入”冒充服务器已排空。
	int32 GetPendingAuthoritativeShotCount() const { return PendingNetworkShots.Num(); }
#endif

	// ==================== Events ====================
	//事件
	//蓝图可以 Bind Event 到这些动态多播委托
	//蓝图可以决定播放动画、音效、HUD、特效
	UPROPERTY(BlueprintAssignable, Category = "Weapon|Events")
	FWeaponFireEvent OnWeaponFirePerformed;

	// 播放命令携带事务身份，蓝图只选素材并调用 PlayReloadMontage。
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
	// 先拒绝新动作并解绑表现，再经完整卸枪路径清理 Timer 和双向装备关系。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 可反复确保物理/可见性，只有显式初始条件才设置位姿和速度，避免重复通知把落地枪弹回手边。
	void ApplyDeathDropPhysics(const FTransform* InitialTransform = nullptr, const FVector* InitialVelocity = nullptr);
	// 消费去重后的单发事实，播放本枪 Montage、枪口和声音，不结算伤害。
	UFUNCTION() void PlayWeaponFirePresentation();
	// 按事务和服务器时长选择本枪换弹 Montage，播放身份仍由事务统一绑定。
	UFUNCTION() void PlayWeaponReloadPresentation(int32 ReloadId, bool bWasEmptyReload);
	// 消费权威射线结果，只播放命中特效、声音和贴花，不预测命中。
	UFUNCTION() void PlayWeaponImpactPresentation(bool bHit, FVector TraceStart, FVector TraceEnd, FVector TraceTarget, FHitResult HitResult);
	friend class AfpstrueRARWeapon;
	friend class AfpstrueTemplateWeapon;
	friend class UfpstrueFireAbility;
	friend class UfpstrueReloadAbility;
	friend class UfpstrueAimAbility;
	friend class FFpstrueMultiplayerShotProtocolTest;
	friend class FFpstrueWeaponReplicationAudienceTest;
	friend class FFpstrueMultiplayerRemoteLocomotionAnimationTest;
	friend class FFpstrueWeaponAmmoPredictionTest;
	friend class FFpstrueWeaponReloadPredictionTest;
	friend class FFpstrueWeaponShotTransportTest;
	friend class FFpstrueShotPresentationReentryTest;
	friend class UfpstrueHealthComponent;
	// 只有 GA 能调用执行器；公开 Start/Stop/Request 只是 ASC 请求入口，不再直接启动业务。
	bool CanExecuteShot() const;
	// 纯校验，不消费 ShotId、不修改会话；准入、执行和 ACK 共用装备归属规则。
	bool MatchesShotEquipment(const FFPShotRequest& Request) const;
	// 准入与执行复核元数据身份和未来界；过旧显示时间由原历史窗口自然miss，不另改较新时间。
	EFPShotResult ValidateRenderedViews(const FFPShotRequest& Request, double ServerNow) const;
	// 目标呈现时间按服务器观测 RTT/2 估计，受真实历史长度约束；不改变射手输入时间或准入窗口。
	static double ComputeTargetTraceTime(double FireTime, double ServerNow, double ObservedRTTMilliseconds,
		double InterpolationSeconds, bool bEnableLatencyCompensation);
	// 检查装备、真实备弹与弹匣缺口；客户端尚未确认的射击只允许提出换弹请求。
	bool HasReloadAmmo() const;
	// 记录本轮能力的表现身份；只有本地拥有者生成逐发会话和预测去重记录。
	void BeginAbilityFire(int32 PredictionKey);
	// 服务器强取消排队；GAS 激活被拒时额外撤销本轮预测，普通动作替换保留在途输入等待 ACK。
	void EndAbilityFire(bool bCancelQueuedShots = false, bool bDiscardPredictedShots = true);
	// 本地拥有者生成一发带时钟和装备身份的输入并登记未确认账本，不执行命中。
	bool BuildShotRequest(FFPShotRequest& OutRequest);
	// 固定容量的准入队列只保存合法输入；唯一执行 Timer 按绝对射速节拍消费，不在同帧补射。
	EFPShotResult AcceptShotRequest(const FFPShotRequest& Request, bool bReleasedAuthorization = false);
	// 高频输入唯一使用不可靠滑窗；可靠消息仅收口生命周期/超时及最终裁决。
	void SubmitShotInput(const FFPShotRequest& Request, FGameplayAbilitySpecHandle Handle, FPredictionKey Key);
	void SendPendingShotInputs();
	void FinishShotInputStream(FGameplayAbilitySpecHandle Handle, FPredictionKey Key, bool bCancelled);
	bool ReceiveShotInputBatch(uint32 FirstShotId, const TArray<FFPShotRequest>& Requests,
		bool bReleasedAuthorization = false, bool bCancelled = false, bool bExpired = false);
	void DrainOrderedShotInputs();
	void CancelOrderedShotInputs();
	// 队头出队后复核装备、时间、生命、弹药与冷却，唯一进入 Fire 并发送终结 ACK。
	bool ExecuteNetworkShot(const FFPShotRequest& Request);
	// 查询服务器是否还有已准入但尚未终结的逐发输入，不代表 Fire GA 仍激活。
	bool HasPendingNetworkShots() const { return !PendingNetworkShots.IsEmpty(); }
	// 按值取得当前队头再交统一执行入口，避免回调使数组引用失效。
	bool ExecuteNextPendingNetworkShot();
	// 依据绝对下一发截止点建立唯一 Timer；只消费已有输入，不生成新射击。
	void SchedulePendingNetworkShot();
	// Timer 每轮最多执行一发，回调后身份仍有效且空仓时请求服务器 Reload GA。
	void ProcessPendingNetworkShot();
	// 强取消先移走所有待执行记录，再逐发确认，外部回调不能重新消费旧输入。
	void CancelPendingNetworkShots();
	// 权威端发布该发裁决和最新弹药，身份匹配时才推进持久快照的已处理编号。
	void AcknowledgeShot(const FFPShotRequest& Request, EFPShotResult Result);
	// Reload GA 的唯一事务入口；广播前先写 OutReloadId、状态和计时，允许监听者同步结束。
	bool BeginAbilityReload(int32& OutReloadId, int32 PredictionKey = 0);
	// 客户端只预测动作，不创建装弹 Timer；确认时把同一播放实例转交权威 ReloadId。
	bool BeginPredictedReload(int32 PredictionKey);
	void EndPredictedReload(int32 PredictionKey, EFPReloadEndReason Reason = EFPReloadEndReason::Cancelled);
	void PromotePredictedReload(int32 ReloadId);
	// 网络状态仅由服务器发布；RepNotify 只建立本地关系、更新 HUD 和播放表现。
	void PublishNetworkState();
	// 客户端应用最新业务快照，先收尾旧播放再恢复新阶段，不反向执行伤害或补弹。
	UFUNCTION() void OnRep_NetworkState();
	// 私有快照可以先到；就绪入口只在公共装备身份匹配后消费，不猜测 Holder/Controller。
	UFUNCTION() void OnRep_OwnerState();
	// 应用比可靠 ACK 更新的弹药结果；返回是否确实更新了本地镜像，不修改权威业务。
	bool ApplyOwnerNetworkState();
	// 单发结果是瞬时表现；射线端点已由 FHitResult 序列化，不重复传输。
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastShot(const FHitResult& Hit, int32 ShotIndex, uint32 EquipmentGeneration, uint32 Session);
	// 拒绝也确认，客户端才能释放该发的弹药估计；版本阻止迟到 ACK 覆盖更新的弹药。
	UFUNCTION(Client, Reliable)
	void ClientShotResult(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId, EFPShotResult Result,
		int32 Ammo, int32 Reserve, uint32 StateRevision, uint32 IncludedShotId = 0);
	virtual void ClientShotResult_Implementation(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId, EFPShotResult Result,
		int32 Ammo, int32 Reserve, uint32 StateRevision, uint32 IncludedShotId = 0);
	// 最多4条近期终结裁决，后续快速包可补回此前丢失的快速收据；尾发仍由可靠副本收口。
	UFUNCTION(Client, Unreliable)
	void ClientFastShotResult(const TArray<FFPShotReceipt>& Receipts);
	void ConsumeShotResult(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId, EFPShotResult Result,
		int32 Ammo, int32 Reserve, uint32 StateRevision, uint32 IncludedShotId, bool bReliableReceipt);
	UPROPERTY(ReplicatedUsing = OnRep_NetworkState)
	FFPReplicatedWeaponState NetworkState;
	UPROPERTY(ReplicatedUsing = OnRep_OwnerState)
	FFPReplicatedWeaponOwnerState OwnerState;
	bool bNetworkEmptyReload = false;
	EFPReloadEndReason LastNetworkReloadEndReason = EFPReloadEndReason::Completed;
	int32 LastPresentedReloadId = INDEX_NONE;
	// ==================== Equipment Eligibility / Read-only Action View ====================
	// 动作阶段由 GAS 独占；装备资格和只读网络展示不构成第二套动作状态机。
	// 武器已装备、未禁用且角色存活。
	bool IsOperational() const;
	// 临时输入禁入与永久装备禁用分开；清理或播放变化期间不允许回调重启动作。
	bool CanAcceptOwnerInput() const;
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
	bool Fire(const FFPShotRequest* Request = nullptr, bool bAimingAtAdmission = false);
	// 返回下一发等待量，松开再按不能重置预算。
	// 服务器按世界时间执行；拥有者按单调时间生成输入，并等待校时后的报告时间满足相同射速。
	float GetNextShotDelay(bool bAdmittedNetworkSchedule = false) const;
	// 拥有者立即显示预测扣弹并播放枪声/动画/后坐力；命中只由服务器提交。
	bool PresentPredictedShot(const FFPShotRequest& Request);
	// 单机根据相机计算；联网使用历史服务器视点和逐发瞄准方向，再应用本发散布和命中查询。
	// 执行一条带散布的射线，处理伤害、冲量和命中事件。
	void FireLineTrace(UWorld& World, AfpstrueCharacter& OwningCharacter, UCameraComponent& Camera,
		int32 ShotIndex, const FFPShotRequest* Request, bool bAimingForShot);

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
	// 反向弱引用只观察持有者；装备关系由双方完整装备/卸下事务成对维护。
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
	// 公共表现与私有弹药分别拒绝旧版本：新 ACK 不能阻止尚未到达的公共换弹阶段。
	uint32 LastPresentedStateRevision = 0;

	// Ammo
	//当前弹匣和备弹数量
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 CurrentAmmo = 0;
	UPROPERTY(VisibleInstanceOnly, Category = "Weapon|Ammo")
	int32 ReserveAmmo = 0;

	// Reload Transaction
	// 同一次换弹最多提交一次弹药；动画回调只结束表现，不提交或撤销弹药。
	bool bReloadAmmoCommitted = false;
	// 每次接纳换弹递增；同步委托可能取消并重新请求，旧 Finish 不得操作新事务。
	// 动作版本归 GA；这里只用递增事务编号区别两次装填。
	int32 ReloadSerial = 0;
	int32 ActiveReloadId = INDEX_NONE;
	// 负编号仅标识本地播放；服务器的正 ReloadId 与 Timer 才能装弹。
	int32 LocalReloadPredictionKey = 0;
	int32 LastEndedReloadPredictionKey = 0;
	double PredictedReloadStartSeconds = 0.0;
	float PredictedReloadDuration = 0.0f;
	bool bPredictedEmptyReload = false;
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
		// 同进程单调时钟只测准入后的等待，不能与客户端时钟相减来估计网络单向延迟。
		double AdmissionMonotonicSeconds = 0.0;
	};
	TArray<FPendingNetworkShot, TInlineAllocator<MaxQueuedNetworkShots>> PendingNetworkShots;
	uint32 LastProcessedShotId = 0;
	EFPShotResult LastShotResult = EFPShotResult::Accepted;
	// 服务端为最近通过间隔检查的输入时间；客户端为最近成功Build的真实报告时间。
	double LastSubmittedFireTime = -1.0;
	uint32 LastAmmoStateRevision = 0;
	uint32 LastAmmoIncludedShotId = 0;
	uint32 LastReliableShotResultRevision = 0;
	struct FFastShotReceipt
	{
		uint32 Session;
		uint32 ShotId;
		uint32 Revision;
	};
	// 只记录先于可靠副本到达的快速收据；可靠水位经过后即可回收，长阻塞时保持有界。
	TArray<FFastShotReceipt, TInlineAllocator<64>> FastShotReceipts;
	static constexpr int32 RecentShotReceiptCount = 4;
	// 服务端仅保留同一装备代际最近4条已终结裁决，复用缓冲；没有额外重发时钟。
	TArray<FFPShotReceipt> RecentShotReceipts;
	TArray<uint32, TInlineAllocator<32>> OutstandingShots;
	// 8 发窗口在确认前保留输入，不因丢包无限增长；每发最多 6 次常规发送。
	static constexpr int32 ShotInputWindowSize = 8;
	static constexpr int32 MaxShotInputAttempts = 6;
	static constexpr float ShotRetryInterval = 0.04f;
	static constexpr float ShotReorderWaitSeconds = 0.075f;
	static constexpr float ShotInputExpireSeconds = 0.40f;
	static constexpr double MaxNetworkShotAgeSeconds = 0.30;
	struct FClientShotInput
	{
		FFPShotRequest Request;
		FGameplayAbilitySpecHandle Handle;
		FPredictionKey Key;
		uint32 FirstShotId = 0;
		int32 Attempts = 0;
		double CreatedSeconds = 0.0; // 客户端FPlatformTime，不能和服务器/本地WorldTime相减。
		bool bExpirationSent = false;
		double LastSentSeconds = -1.0;
	};
	TArray<FClientShotInput, TInlineAllocator<ShotInputWindowSize>> ClientShotInputs;
	TArray<FFPShotRequest> ShotInputScratch;
	struct FOrderedShotInput
	{
		FFPShotRequest Request;
		bool bCancelled = false;
		bool bExpired = false;
	};
	TArray<FOrderedShotInput, TInlineAllocator<ShotInputWindowSize>> OrderedShotInputs;
	uint32 InputWindowFirstShotId = 0;
	uint32 InputWindowSession = 0;
	uint32 NextOrderedShotId = 0;
	double ShotHoleStartedSeconds = -1.0;
	bool bReleasedInputWindow = false;
	double InputClosedServerSeconds = 0.0;
	// 最近结果已在 UE 可靠重传队列，重复输入不能再制造一份 ACK/属性发布。
	TArray<uint32, TInlineAllocator<64>> FinalizedShotInputs;
	// 仅拥有者使用的实际表现单调时刻；不复用帧起点WorldTime作为真实连射预算。
	double LastPredictedShotTime = -1.0;
	bool bPredictingFire = false;
	struct FPredictedFirePresentation
	{
		int32 Key = 0;
		uint32 Generation = 0;
		uint32 Session = 0;
		int32 PresentedShots = 0;
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
	FTimerHandle ShotRetryTimerHandle;
	FTimerHandle ShotReorderTimerHandle;
};

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
