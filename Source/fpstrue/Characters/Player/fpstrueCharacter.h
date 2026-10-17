// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Animation/AnimInstance.h"
#include "AbilitySystemInterface.h"
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
class UAnimMontage;
class UMaterialParameterCollection;
class AfpstrueCharacter;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
//动态多播委托，用于在玩家死亡时通知其他系统:DYNAMIC可以被蓝图绑定
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerDeathReported, AfpstrueCharacter*, DeadPlayer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEquippedWeaponChanged, UfpstrueWeaponComponent*, WeaponComponent);

// 玩家动画只消费移动快照、GAS 派生姿态和武器事实事件；不创建另一套动作状态，也不从通知结算伤害或弹药。
// 与玩家放在同一文件，身体和手臂共用数据入口，AnimGraph 分别负责全身分层与第一人称表现。
UCLASS(Blueprintable, BlueprintType)
class FPSTRUE_API UfpstruePlayerAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	UfpstruePlayerAnimInstance();
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUninitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
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
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") FRotator AimRotation = FRotator::ZeroRotator;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") FVector GunAimTranslation = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> FireMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> AimedFireMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> ReloadMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> EmptyReloadMontage;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") TObjectPtr<UAnimMontage> EquipMontage;
	// 已结算的受击表现可由拥有者与观察者调用；幅度仅进入动画图，不改控制旋转或命中历史。
	void PresentHit();
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float HitAlpha = 0.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float HitTime = 0.0f;
private:
	void BindCurrentWeapon();
	UFUNCTION() void PresentFire();
	UFUNCTION() void PresentReload(int32 ReloadId, bool bWasEmpty);
	TWeakObjectPtr<UfpstrueWeaponComponent> AnimationWeapon;
	UPROPERTY() TObjectPtr<UMaterialParameterCollection> ViewmodelFOVCollection;
	float LandingRemaining = 0.0f;
	bool bWasFalling = false;
#if !UE_BUILD_SHIPPING
	void CapturePresentation(const TCHAR* Phase) const;
#endif
};

// 每次移动携带当时的模式，不把当前 GAS 标签错误套用到历史移动；这里只保存计算上下文，不执行能力。
class FPSTRUE_API FSavedMove_fpstrueCharacter : public FSavedMove_Character
{
public:
	using Super = FSavedMove_Character;
	bool bSavedAiming = false;
	bool bSavedSprinting = false;
	virtual void Clear() override;
	virtual uint8 GetCompressedFlags() const override;
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* Character, float MaxDelta) const override;
	virtual void SetMoveFor(ACharacter* Character, float InDeltaTime, const FVector& Accel, FNetworkPredictionData_Client_Character& ClientData) override;
	virtual void PrepMoveFor(ACharacter* Character) override;
};

// 移动扩展与角色放在一起：GAS 授权动作，SavedMove 负责模式的保存、传输与校正重放。
UCLASS()
class FPSTRUE_API UfpstrueCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()
public:
	virtual float GetMaxSpeed() const override;
	virtual FNetworkPredictionData_Client* GetPredictionData_Client() const override;
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;
	virtual bool ClientUpdatePositionAfterServerUpdate() override;
	void SetMovementIntent(bool bAiming, bool bSprinting);
	bool IsAimMovementRequested() const { return bMoveAiming; }
	bool IsSprintMovementRequested() const { return bMoveSprinting; }
private:
	friend class FSavedMove_fpstrueCharacter;
	// 回放只恢复这两个值，不调用 SetMovementIntent，因此不改当前输入、GAS 或蓝图。
	bool bMoveAiming = false;
	bool bMoveSprinting = false;
	struct FRecentMovementGrant
	{
		uint8 Mode = 0;
		float LastAllowedClientTime = 0.0f;
		double ExpiresAt = 0.0;
	};
	FRecentMovementGrant RecentGrants[4];
	uint8 NextGrant = 0;
	uint8 GrantedMode = 0;
};

/**
 * 玩家角色模块：连接输入、移动、武器和通用 HealthComponent，并把状态变化转发给蓝图表现层。
 *
 * 状态边界：本类维护当前装备关系；GAS 管理动作标签，WeaponComponent 管理弹药/换弹事务，HealthComponent 管理生命值。
 * 通信边界：输入使用直接调用，Gameplay 状态变化使用 Delegate，动画和界面表现使用 BlueprintImplementableEvent。
 * GAS 职责：动作资格与互斥由能力管理；本类的瞄准、冲刺布尔值仅为标签派生的蓝图/网络显示镜像。
 */
UCLASS(config = Game)
class FPSTRUE_API AfpstrueCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	// ★【网络边界】装备关系和远端姿态由服务器复制；能力请求经 ASC，服务器自行读取当前武器，不接受客户端指定伤害或受害者。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 动作激活由 ASC 和 PredictionKey 承载；武器逐发请求另带身份与时间，但不能绕过能力授权。
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	// 持久血量走复制；瞬时受击反馈单发给拥有该角色的客户端，不让本地再扣一次血。
	UFUNCTION(Client, Reliable) void ClientReceiveDamage(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);
	/// 对外接口（查询、请求）

	// 创建相机、第一人称 Mesh 和 HealthComponent，角色本身不启用 Tick。
	AfpstrueCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	// 联机占有只能由 GameMode 分配，必须在父类读取单机蓝图的 Player0 设置之前关闭自动占有。
	virtual void PreInitializeComponents() override;
	// 绑定健康事件、同步 HUD 初始血量并初始化移动速度。
	virtual void BeginPlay() override;
	// RAR 手部靠近镜头，近裁切仅作用于本角色相机，不修改引擎全局 NearClipPlane。
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

	// C++ 通过以下接口访问 Mesh1P 和 FirstPersonCameraComponent；蓝图通过下方 BlueprintReadOnly 组件属性访问。
	// WeaponComponent 装备时使用第一人称 Mesh 的 GripPoint。
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	// 单机 Hitscan 使用该相机；联网由服务器历史视点和逐发瞄准方向确定射线。
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

	// 完整卸下入口：ExpectedWeapon 防止旧回调清除新装备；不能公开只改槽位的 setter。
	void ClearEquippedWeaponComponent(const UfpstrueWeaponComponent* WeaponComponent);
	// 完整取消瞄准/冲刺并刷新步速的公开命令，不影响开火、换弹和弹药。
	void ResetMovementModifiers();
	// 从 GAS 标签刷新姿态镜像、步速和蓝图表现；不能反向写标签或独立决定动作互斥。
	void RefreshMovementAbilityState();
	// 业务资格统一入口；临时输入迁移不会把有效装备误判为永久失效。
	bool CanMaintainEquipment() const;
	// 在装备资格之外排除控制权迁移期间的输入；迁移不会永久禁用当前武器。
	bool CanAcceptGameplayInput() const;

	// ★【命中边界】服务器短期历史只保存 PhysicsAsset 的球、盒、胶囊；不移动真实角色，不重播动画或伤害。
	static constexpr float MaxHitHistorySeconds = 0.35f;
	bool TraceHistoricalHit(double ServerTime, const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	// 世界遮挡仍取当前状态（含动态门）；角色用历史形状。没有动态障碍历史时不能宣称恢复了过去整个世界。
	static bool TraceHistoricalHit(UWorld* World, const AfpstrueCharacter* Shooter, double ServerTime,
		const FVector& Start, const FVector& End, FHitResult& OutHit);
	bool GetHistoricalViewOrigin(double ServerTime, FVector& OutOrigin) const;
	// 正常由骨骼完成回调记录；显式接口也只允许权威端按当前世界时间采样，不能由客户端灌入历史。
	void RecordServerHitHistory();
	void ResetHitHistory();
	uint32 GetHitHistoryEpoch() const { return HitHistoryEpoch; }
	virtual bool TeleportTo(const FVector& DestLocation, const FRotator& DestRotation, bool bIsATest = false, bool bNoCheck = false) override;

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
	UFUNCTION(BlueprintPure, Category = "Movement")
	bool IsSprinting() const { return bIsSprinting; }
	bool UsesRARPresentation() const { return bUseRARPresentation; }

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

	// 输入处理函数按“连续轴、按住、切换、单次请求”区分语义，避免函数名与触发方式不一致。
	// 把二维移动输入转换为前后/左右移动。
	void Move(const FInputActionValue& Value);
	// 把二维视角输入转换为 Yaw/Pitch。
	void Look(const FInputActionValue& Value);
	// 每次按下切换冲刺状态；瞄准、换弹和死亡仍可强制停止冲刺。
	void ToggleSprint();
	// 向 ASC 提交瞄准输入，能力校验当前装备并驱动蓝图表现。
	void StartAim();
	// 向 ASC 提交瞄准松开，能力结束后按当前标签重新计算步速。
	void StopAim();
	// 把开火输入转交 ASC，由能力启动当前武器的逐发执行。
	void StartWeaponFire();
	// 把停止开火输入转交 ASC，由能力结束连射任务。
	void StopWeaponFire();
	// 向 ASC 提交换弹请求；能力统一处理瞄准/冲刺互斥，武器执行换弹事务。
	void RequestWeaponReload();

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
	// 身体受击是瞬时表现；持久生命与死亡仍由现有 HealthComponent 复制，不以 RPC 代替状态。
	UFUNCTION(NetMulticast, Unreliable) void MulticastPresentHit();
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") bool bUseRARPresentation = false;
	// 装备复制与武器快照可能乱序到达，通过两侧幂等补齐，不由客户端执行拾取结算。
	UFUNCTION() void OnRep_EquippedWeapon();
	UFUNCTION() void OnRep_MovementModifiers();
#if !UE_BUILD_SHIPPING
	// 只在显式 -PvpSmokeTest 参数下运行客户端脚本输入；不是正常玩法 Tick。
	void RunPvpSmokeStep();
	FTimerHandle NetworkSmokeTimer;
	double NetworkSmokeStarted = -1.0;
	double NetworkSmokePhaseStarted = 0.0;
	double NetworkSmokeNextDiagnostic = 0.0;
	int32 NetworkSmokePhase = 0;
	int32 NetworkSmokeInitialReserve = 0;
	TWeakObjectPtr<AfpstrueCharacter> NetworkSmokeTarget;
	bool bNetworkSmokeInjectingMove = false;
	bool bNetworkSmokePveSeen = false;
	TWeakObjectPtr<class AfpstrueEnemyCharacter> NetworkSmokePveTarget;
#endif
	TWeakObjectPtr<AController> LastDamageInstigator;
	friend class UfpstrueWeaponComponent;
	friend class UfpstrueCharacterMovementComponent;
	// 仅完整装备事务能登记/释放槽位；外部不得构造只有一侧引用的半装备状态。
	void SetEquippedWeaponComponent(UfpstrueWeaponComponent* WeaponComponent);
	// 只释放仍与 ExpectedWeapon 对应的装备关系，旧卸枪回调不能清掉新武器。
	void ReleaseEquippedWeaponComponent(const UfpstrueWeaponComponent* ExpectedWeapon);
	// MaxWalkSpeed 唯一写入口，按当前瞄准/冲刺状态计算，不依赖各出口调用顺序。
	void ApplyMovementSpeed();
	// 从旧 LocalPlayer 移除本角色添加的 Mapping Context。
	void RemoveInputMappingContexts();

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
	TObjectPtr<UInputAction> SprintAction;

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
	UPROPERTY(ReplicatedUsing = OnRep_MovementModifiers, VisibleAnywhere, BlueprintReadOnly, Category = "Movement", meta = (AllowPrivateAccess = "true"))
	bool bIsSprinting = false;

	UPROPERTY(ReplicatedUsing = OnRep_MovementModifiers, VisibleAnywhere, BlueprintReadOnly, Category = "Weapon", meta = (AllowPrivateAccess = "true"))
	bool bIsAiming = false;
	// 只记录已发送给蓝图的瞄准表现，避免标签预测、服务器复制重复通知同一变化。
	bool bPresentedAiming = false;

	// 装备关系只在运行时存在，不参与序列化。
	UPROPERTY(Transient, ReplicatedUsing = OnRep_EquippedWeapon)
	// 强引用保证装备期间组件不会被 GC 回收。
	TObjectPtr<UfpstrueWeaponComponent> EquippedWeaponComponent;

	//增强输入子系统的弱指针，便于移除 Mapping Context
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;
	// 保存实际添加的资产；运行中更改 DefaultMappingContext 也能移除旧映射。
	TWeakObjectPtr<UInputMappingContext> AppliedMappingContext;
	bool bAddedMappingContext = false;
	bool bEndingPlay = false;
	// 控制权迁移的同步回调期间拒绝新输入动作；不改变装备和弹药的持久状态。
	bool bOwnerInputTransition = false;

	//死亡处理标志，防止重复处理死亡事件
	// HealthComponent 保存死亡事实；这里只防止死亡表现和广播重复执行。
	bool bDeathEffectsApplied = false;

	// 固定 32 帧、每帧最多 64 形状；60 Hz 采样覆盖 .35 秒，环形复用容量，不随游戏时长增长。
	static constexpr int32 HitHistoryCapacity = 32;
	static constexpr int32 MaxHistoricalShapes = 64;
	struct FHistoricalShape
	{
		FTransform Transform;
		FVector Extent = FVector::ZeroVector; // Box 半尺寸；Sphere.X 半径；Capsule.X 半径、Z 轴线半长。
		FName Bone;
		uint8 Type = 0; // 0 Box、1 Sphere、2 Capsule。
	};
	struct FHitHistoryFrame
	{
		double ServerTime = 0.0;
		FVector ActorLocation = FVector::ZeroVector;
		FVector ViewOrigin = FVector::ZeroVector;
		TArray<FHistoricalShape> Shapes;
	};
	FHitHistoryFrame HitHistory[HitHistoryCapacity];
	int32 HitHistoryCount = 0;
	int32 HitHistoryNext = 0;
	uint32 HitHistoryEpoch = 1;
	FDelegateHandle BoneHistoryHandle;
	TWeakObjectPtr<class UPhysicsAsset> RecordedPhysicsAsset;
	bool bLoggedUnsupportedHitGeometry = false;
	bool FindHitHistoryFrames(double ServerTime, const FHitHistoryFrame*& Older, const FHitHistoryFrame*& Newer, float& Alpha) const;
};
