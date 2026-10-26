// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
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
class AfpstrueCharacter;
struct FInputActionValue;
struct FFPShotRequest;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);
//动态多播委托，用于在玩家死亡时通知其他系统:DYNAMIC可以被蓝图绑定
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerDeathReported, AfpstrueCharacter*, DeadPlayer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEquippedWeaponChanged, UfpstrueWeaponComponent*, WeaponComponent);

// 只在停走和转身边界更新；晚加入和动画实例重建也能恢复同一脚部朝向，不另发逐帧视角 RPC。
USTRUCT()
struct FFpstrueBodyFacingState
{
	GENERATED_BODY()
	UPROPERTY() bool bInitialized = false;
	UPROPERTY() bool bStationary = false;
	UPROPERTY() int8 TurnDirection = 0;
	UPROPERTY() float FacingYaw = 0.0f;
	UPROPERTY() float TurnStartedServerTime = 0.0f;
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
	/// 对外接口（查询、请求）

	// 创建相机、第一人称 Mesh 和 HealthComponent，角色本身不启用 Tick。
	AfpstrueCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	// 联机占有只能由 GameMode 分配，必须在父类读取单机蓝图的 Player0 设置之前关闭自动占有。
	virtual void PreInitializeComponents() override;
	// 绑定受击和死亡事实并初始化移动；HUD 独立订阅生命组件。
	virtual void BeginPlay() override;
	// RAR 手部靠近镜头，近裁切仅作用于本角色相机，不修改引擎全局 NearClipPlane。
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

	// C++ 通过以下接口访问 Mesh1P 和 FirstPersonCameraComponent；蓝图通过下方 BlueprintReadOnly 组件属性访问。
	// WeaponComponent 装备时使用第一人称 Mesh 的 GripPoint。
	USkeletalMeshComponent* GetMesh1P() const { return Mesh1P; }
	// 单机 Hitscan 使用该相机；联网由服务器历史视点和逐发瞄准方向确定射线。
	UCameraComponent* GetFirstPersonCameraComponent() const { return FirstPersonCameraComponent; }

	// 由本地动画快照驱动实际角色蓝图中的相机表现，不复制相机或再次授权瞄准。
	UFUNCTION(BlueprintImplementableEvent, Category = "Presentation") void ApplyCameraPresentation(float AimBlend);
	// 实际角色蓝图设置材料投影或隐藏展示骨骼；只在本端初始化，不影响权威骨骼/碰撞。
	UFUNCTION(BlueprintImplementableEvent, Category = "Presentation") void InitializeVisualPresentation();
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation|Camera") float HipFieldOfView = 90.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation|Camera") float AimFieldOfView = 70.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation|Camera") float PresentationNearClipPlane = 1.0f;

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
	// 在本玩家的相邻历史形状间插值求射线最近交点；无有效历史时返回 false，不回退移动胶囊。
	bool TraceHistoricalHit(double ServerTime, const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	// 世界遮挡仍取当前状态（含动态门）；角色用历史形状。没有动态障碍历史时不能宣称恢复了过去整个世界。
	static bool TraceHistoricalHit(UWorld* World, const AfpstrueCharacter* Shooter, double ServerTime,
		const FVector& Start, const FVector& End, FHitResult& OutHit, const FFPShotRequest* RequestContext = nullptr);
	// 返回射手历史视点；只在有界的一帧采样空隙允许取最后视点，不外推目标命中体。
	bool GetHistoricalViewOrigin(double ServerTime, FVector& OutOrigin) const;
	// 所有 Actor Tick 完成、网络发布前采样；显式接口只允许权威端按当前世界时间记录。
	void RecordServerHitHistory();
	// 清空历史并递增代际，避免跨死亡/传送插值；可选释放各帧预留形状内存。
	void ResetHitHistory(bool bReleaseMemory = false);
	// 测试/诊断读取历史断代编号，不参与网络复制或命中授权。
	uint32 GetHitHistoryEpoch() const { return HitHistoryEpoch; }
	// 只读容量诊断：比较实际常驻预算，不把整个进程或着色器编译峰值冒充历史缓存收益。
	SIZE_T GetHitHistoryAllocatedBytes() const;
	// 返回所有环形帧预留的形状槽位数，不等同于当前有效历史样本数。
	int32 GetHitHistoryReservedShapeCount() const;
	// 返回单条历史形状的真实 sizeof，供常驻内存预算核算。
	static SIZE_T GetHitHistoryShapeStride();
	// 成功的实际传送断开命中历史；仅试探 Teleport 时不清空已有历史。
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
	// 查询 GAS 派生/远端复制的冲刺显示镜像，不创建独立动作资格。
	UFUNCTION(BlueprintPure, Category = "Movement")
	bool IsSprinting() const { return bIsSprinting; }
	// 查询该角色是否使用 RAR 成套表现资源。
	bool UsesRARPresentation() const { return bUseRARPresentation; }
	// 查询该角色是否使用 UE5.5 模板成套表现资源。
	bool UsesTemplatePresentation() const { return bUseTemplatePresentation; }
	// 识别两套正式资源配置；基础原生类型只提供规则和组件拓扑。
	bool UsesNativePresentation() const { return bUseRARPresentation || bUseTemplatePresentation; }
	static constexpr float ViewPitchLimit = 75.0f;
	static constexpr float ViewYawLimit = 75.0f;
	static constexpr float TurnTriggerYaw = 55.0f;
	// 只读脚部朝向边界状态，客户端据服务器时钟重建转身表现。
	const FFpstrueBodyFacingState& GetBodyFacingState() const { return BodyFacingState; }
	// 优先使用 GameState 同步服务器时钟，单机/未就绪时使用当前世界时间。
	float GetBodyFacingServerTime() const;
	// 仅服务器在停走/转身边界更新复制状态，不把视觉脚部朝向用于移动或射击授权。
	void UpdateBodyFacing(float TurnDuration);

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

	// 权威受伤事实只记录计分来源并分发当前身体受击表现，不复制第二份生命数值。
	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);
	//绑定到死亡委托
	// 停止移动和武器，执行一次玩家死亡表现并通知 GameMode。
	UFUNCTION()
	void HandleDeath();



private:
	// 身体受击是瞬时表现；持久生命与死亡仍由现有 HealthComponent 复制，不以 RPC 代替状态。
	UFUNCTION(NetMulticast, Unreliable) void MulticastPresentHit();
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") bool bUseRARPresentation = false;
	UPROPERTY(EditDefaultsOnly, Category = "Presentation") bool bUseTemplatePresentation = false;
	// 装备复制与武器快照可能乱序到达，通过两侧幂等补齐，不由客户端执行拾取结算。
	UFUNCTION() void OnRep_EquippedWeapon();
	// 应用姿态镜像对应的速度，并仅在真实瞄准边沿通知本地蓝图；不激活能力。
	UFUNCTION() void OnRep_MovementModifiers();
#if !UE_BUILD_SHIPPING
	// 只在显式 -PvpSmokeTest 参数下运行客户端脚本输入；不是正常玩法 Tick。
	void RunPvpSmokeStep();
	// 将烟测按下的 W 归还给原 Controller，控制权迁移/死亡也可安全成对释放。
	void ReleaseNetworkSmokeMovementKey();
	TWeakObjectPtr<class APlayerController> NetworkSmokeMovementController;
	FTimerHandle NetworkSmokeTimer;
	double NetworkSmokeStarted = -1.0;
	double NetworkSmokePhaseStarted = 0.0;
	double NetworkSmokeNextDiagnostic = 0.0;
	int32 NetworkSmokePhase = 0;
	int32 NetworkSmokeWarmupRetries = 0;
	int32 NetworkSmokeInitialReserve = 0;
	float NetworkSmokeMovingInitialHealth = -1.0f;
	bool bNetworkSmokeMovingHitAttempted = false;
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
	UPROPERTY(Transient, Replicated) FFpstrueBodyFacingState BodyFacingState;

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

	// 固定 32 帧、每帧最多 64 形状；实际容量按物理资产预留，60 Hz 覆盖 .35 秒且不随游戏时长增长。
	static constexpr int32 HitHistoryCapacity = 32;
	static constexpr int32 MaxHistoricalShapes = 64;
	struct FHistoricalShape
	{
		// 形状缩放已烘入 Extent；只保留命中查询使用的双精度旋转和位置，避免每条历史重复存恒等 Scale。
		FQuat Rotation = FQuat::Identity;
		FVector Location = FVector::ZeroVector;
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
	FDelegateHandle HitHistoryPostTickHandle;
	TWeakObjectPtr<class UPhysicsAsset> RecordedPhysicsAsset;
	bool bLoggedUnsupportedHitGeometry = false;
	// 定位同一历史代际内包围时间点的两帧与插值因子；越界或空历史直接失败。
	bool FindHitHistoryFrames(double ServerTime, const FHitHistoryFrame*& Older, const FHitHistoryFrame*& Newer, float& Alpha) const;
};
