// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Blueprint/UserWidget.h"
#include "AbilitySystemInterface.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Runtime/fpstrueRuntime.h"
#include "fpstrueEnemyCharacter.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyAIController;
class AfpstrueEnemyCharacter;
class UfpstrueEnemyAnimationSharingCoordinator;
class UfpstrueEnemyCombatComponent;
class UfpstrueHealthComponent;
class UMeshComponent;
class UAnimMontage;
class UProgressBar;
class UWidgetComponent;
class APlayerController;
class APlayerCameraManager;
class USphereComponent;
class UCapsuleComponent;

// 四种模式只改变本地血条表现；非 Shipping 的对照参数不改变生命、AI 或网络规则。
enum class EFPEnemyHealthBarMode : uint8
{
	Off,
	Screen,
	WorldAlways,
	WorldEvent
};

struct FPSTRUE_API FFPEnemyHealthBarMetrics
{
	int32 Components = 0;
	int32 Widgets = 0;
	int32 FrustumVisible = 0;
	int32 WorldWidgets = 0;
	int32 ScreenWidgets = 0;
	int32 ManualWorldWidgets = 0;
	uint64 Created = 0;
	uint64 ValueUpdates = 0;
	uint64 RedrawRequests = 0;
	uint64 RenderTargetDraws = 0;
	uint64 ViewSamples = 0;
	uint64 ViewCacheHits = 0;
};

// 数值通过事件写入，WorldEvent 只在数值变化或重新进入视野时重绘纹理；组件 Tick 仅更新本地朝向/视锥。
UCLASS(NotBlueprintable)
class FPSTRUE_API UfpstrueEnemyHealthBarComponent : public UWidgetComponent
{
	GENERATED_BODY()
public:
	UfpstrueEnemyHealthBarComponent();
	void Configure(EFPEnemyHealthBarMode InMode);
	void SetHealthFraction(float Fraction);
	void ReleasePresentation();
	EFPEnemyHealthBarMode GetPresentationMode() const { return Mode; }
	bool IsInLocalView() const { return bInLocalView; }
	// 与运行时相同的视锥/朝向入口，也允许自动化在没有真实 viewport 的世界中验证边界。
	void UpdateForView(const FVector& CameraLocation, const FRotator& CameraRotation, float HorizontalFOV, float AspectRatio);
	void AppendMetrics(FFPEnemyHealthBarMetrics& Metrics) const;
	static EFPEnemyHealthBarMode GetConfiguredMode();
	static const TCHAR* GetModeName(EFPEnemyHealthBarMode InMode);
	static FFPEnemyHealthBarMetrics CollectMetrics(UWorld* World);
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
protected:
	virtual bool ShouldDrawWidget() const override;
	virtual void DrawWidgetToRenderTarget(float DeltaTime) override;
private:
	friend class FFpstrueWorldHealthBarViewCacheTest;
	// 只缓存同一 World 的当前帧数值；对象身份均为弱引用，不延长视点或关卡生命周期。
	struct FViewSnapshot
	{
		FVector Location = FVector::ZeroVector;
		FVector Forward = FVector::ForwardVector, Right = FVector::RightVector, Up = FVector::UpVector;
		FRotator Facing = FRotator::ZeroRotator;
		double HalfFOVTan = 1.0, InverseAspect = 1.0;
		TWeakObjectPtr<APlayerController> Viewer;
		TWeakObjectPtr<APlayerCameraManager> CameraManager;
	};
	struct FViewCache
	{
		struct FEntry { uint64 Frame = MAX_uint64; FViewSnapshot View; };
		TMap<TWeakObjectPtr<UWorld>, FEntry> Worlds;
		FDelegateHandle CleanupHandle;
		FViewCache();
		~FViewCache();
		void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	};
	static FViewCache& GetViewCache();
	static FViewSnapshot BuildViewSnapshot(const FVector& Location, const FRotator& Rotation, float HorizontalFOV, float AspectRatio);
	static bool SampleLocalView(UWorld* World, FViewSnapshot& OutView);
	// 采样器只在本次同步调用中借用；测试可验证真实缓存而不伪造 viewport 或增加公开测试接口。
	static bool ResolveViewSnapshot(UWorld* World, uint64 Frame, bool bUseCache, FViewSnapshot& OutView, bool& bCacheHit,
		TFunctionRef<bool(UWorld*, FViewSnapshot&)> Sampler);
	void UpdateForView(const FViewSnapshot& View);
	void RequestEventRedraw();
	EFPEnemyHealthBarMode Mode = EFPEnemyHealthBarMode::WorldEvent;
	bool bInLocalView = false;
	bool bReleased = true;
	float LastFraction = -1.0f;
	uint64 Created = 0, ValueUpdates = 0, RedrawRequests = 0, RenderTargetDraws = 0;
	uint64 ViewSamples = 0, ViewCacheHits = 0;
};

// 敌人头顶血条只保存显示比例；真实生命仍由 HealthComponent/GAS 持有，不另做扣血。
UCLASS(NotBlueprintable)
class FPSTRUE_API UfpstrueEnemyHealthBarWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UfpstrueEnemyHealthBarWidget(const FObjectInitializer& ObjectInitializer);
	// 构建前收到的复制值也先缓存，真正创建控件时补上；不使用逐帧属性绑定。
	void SetHealthFraction(float Fraction);
	float GetHealthFraction() const { return HealthFraction; }
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
private:
	UPROPERTY(Transient) TObjectPtr<UProgressBar> HealthProgress;
	float HealthFraction = 1.0f;
};

// 只复制可恢复的表现状态；客户端不据此运行行为树、开伤害窗口或提交伤害。
USTRUCT()
struct FFPEnemyNetworkPresentation
{
	GENERATED_BODY()
	UPROPERTY() uint32 AttackId = 0;
	UPROPERTY() TObjectPtr<UAnimMontage> Montage;
	UPROPERTY() float PlayRate = 1.0f;
	UPROPERTY() float ServerStartTime = 0.0f;
	UPROPERTY() bool bAttacking = false;
	UPROPERTY() bool bDead = false;
	UPROPERTY() FVector_NetQuantizeNormal DeathDirection = FVector::ZeroVector;
	UPROPERTY() FVector_NetQuantize DeathLocation = FVector::ZeroVector;
	UPROPERTY() FName DeathBone;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEnemyDeathReported, AfpstrueEnemyCharacter*, DeadEnemy);

enum class EFPEnemySignificanceTier : uint8
{
	Full,
	Reduced,
	Background
};

/**
 * 敌人实体模块：组合 Health、Combat、移动和渲染分级，并桥接 AI、动画共享与蓝图表现。
 *
 * EnemyCharacter 不复制子系统状态：死亡查询来自 HealthComponent，攻击查询来自 CombatComponent，AI状态来自 Controller。
 * 本类主要负责 Actor/组件生命周期、受击死亡表现，以及把 Gameplay/Render Significance 结果应用到具体组件。
 */
UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueEnemyCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	// 创建健康/战斗组件并设置基础骨骼动画优化。
	AfpstrueEnemyCharacter();
	// CharacterMovement/生命属性负责规则同步，本类补充攻击播放及死亡表现的当前快照。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 敌人与玩家共享 GE 生命结算；现有 AI/攻击事务不因此改为另一套决策系统。
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return HealthComponent.Get(); }

	// 活体使用两个根组件查询代理并继承原Mesh查询通道；简化几何不冒充原PA轮廓，不改玩家历史回溯或伤害权限。
	bool UsesSimpleHitProxies() const { return bUseSimpleHitProxies; }
	// 基线读取实际保护条件；普通服务器姿态不再受本地相机/阴影名额控制。
	bool RequiresFullAuthorityPose() const;
	// 原生射线命中代理后补齐部位身份，沿用现有爆头与死亡冲量入口；不伪造历史姿态。
	void ResolveHitProxyBone(FHitResult& Hit) const;

	// ==================== 状态查询 ====================

	// AI、GameMode 和 Significance 读取 HealthComponent 的死亡事实。
	bool IsDead() const;

	// 行为树采样攻击事务是否仍在进行。
	bool IsAttacking() const;

	// AI 与动画保护判断玩家是否已经进入有效攻击范围。
	bool IsTargetInAttackRange() const;

	// AIController 读取配置的追击范围。
	float GetChaseRange() const { return ChaseRange; }

	// ==================== 战斗与玩法接口 ====================

	// 动画结束 Notify 通过角色入口通知 CombatComponent 完成攻击。
	UFUNCTION(BlueprintCallable, Category = "Combat", meta = (DeprecatedFunction, DeprecationMessage = "Use Enemy Attack Finished native Notify with playback identity"))
	void HandleAttackFinishedNotify();

	// AttackWindow Notify 直接取得已缓存组件，驱动武器 Sweep 的开启、更新与结束。
	UFUNCTION(BlueprintPure, Category = "Combat")
	UfpstrueEnemyCombatComponent* GetCombatComponent() const { return CombatComponent.Get(); }

	// ==================== Render Significance 接口 ====================

	// Coordinator 调用：从相机视锥、屏占比和距离生成纯渲染样本。
	FFPEnemyRenderSignificanceSample EvaluateRenderSignificance(const FFPEnemyRenderScoringContext& ScoringContext);
	// Coordinator 调用：结合滞回和延迟得到自然 Render Tier。
	EFPEnemyRenderSignificanceTier ResolveNaturalRenderSignificanceTier(const FFPEnemyRenderSignificanceSample& Sample,
																		const FFPEnemyRenderSignificancePolicy& Policy);
	// Coordinator 调用：应用全局预算后的 LOD、动画、阴影和 RT 结果。
	void ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier NewTier, bool bShouldCastShadow, bool bShouldBeVisibleInRayTracing,
									 bool bForceFullAnimationAndLOD, const FFPEnemyRenderSignificancePolicy& Policy);
	// 动态增删本 Actor/ChildActor 的 Mesh 后调用；常规采样只用缓存，不遍历附件层级。
	// 独立 Actor 仅 Attach 不属于此预算；所收集的 Mesh 继承上限，不覆盖其原始禁用设置。
	UFUNCTION(BlueprintCallable, Category = "Performance|Render Budget")
	void RefreshRenderBudgetMeshes();
	// 读回已登记 Mesh 的实际标志；这是组件数量，不是 GPU 图元或敌人数。
	void GetRenderBudgetMeshCounts(int32& OutMeshes, int32& OutShadowMeshes, int32& OutRayTracingMeshes) const;
	// 候选资格读取资产原始许可，不读取上一轮已经被预算关闭的实际标志。
	void GetRenderBudgetEligibility(bool& bOutShadow, bool& bOutRayTracing) const;

	// CSV 统计读取当前 Gameplay Tier。
	EFPEnemySignificanceTier GetGameplaySignificanceTier() const { return SignificanceTier; }
	// CSV 统计与共享系统读取当前 Render Tier。
	EFPEnemyRenderSignificanceTier GetRenderSignificanceTier() const { return RenderSignificanceTier; }
	// Animation Sharing 用该分数控制 Leader Tick。
	float GetRenderSignificanceScore() const { return RenderSignificanceScore; }
	// CSV 统计读取最终应用到骨骼 Mesh 的最低 LOD。
	int32 GetAppliedMinimumLOD() const { return AppliedMinimumLOD; }
	// Coordinator 和共享系统检查攻击、范围及最近受击保护期。
	bool RequiresGameplayAnimationProtection(float CurrentTime, float GraceSeconds) const;
	// GameMode 注入同一 World 的共享协调器；弱引用只表达协作关系，不取得所有权。
	// 语法复习：前置声明足以声明指针和函数，但 TWeakObjectPtr 赋值需要看到完整 UObject 继承关系，实现在 .cpp。
	void SetAnimationSharingCoordinator(UfpstrueEnemyAnimationSharingCoordinator* InCoordinator);

	// ==================== Benchmark 诊断接口 ====================

	// BenchmarkRunner 单独关闭攻击 Sweep、Pawn 碰撞或移动 Tick。
	void ApplyBenchmarkDiagnosticOverrides(bool bDisableAttackSweep, bool bDisablePawnCollision, bool bDisableCharacterMovementTick);

	// ==================== 对外事件 ====================

	UPROPERTY(BlueprintAssignable, Category = "AI")
	FOnEnemyDeathReported OnEnemyDeathReported;

protected:
	// ==================== Actor 生命周期 ====================

	// 读取 Benchmark 开关、初始化移动/碰撞并注册 Gameplay Significance。
	virtual void BeginPlay() override;
	// 组件与蓝图默认值就绪后封闭敌人后处理通道，不修改共享SkeletalMesh或玩家实例。
	virtual void PostInitializeComponents() override;
	// 退出共享系统、Significance 和组件事件。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 在通用伤害处理前记录命中方向、位置和骨骼，供受击/死亡冲量使用。
	virtual float TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator,
							 AActor* DamageCauser) override;

	// HealthComponent 死亡事件入口：停止 AI、启用布娃娃并通知 GameMode。
	UFUNCTION()
	void HandleDeath();

	// HealthComponent 受伤事件入口：记录保护时间并播放受击反馈。
	void HandleDamageResolved(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy, const FFPDamageContext& Context);

	// ==================== 组件 ====================

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	// 语法复习：UPROPERTY + TObjectPtr 是 UE5 的强 GC 引用；组件内存仍由 UObject 生命周期管理。
	TObjectPtr<UfpstrueHealthComponent> HealthComponent;

	// 位置锚定敌人身体 Mesh，跟随客户端网络平滑；Screen 仅负责各客户端投影/正对屏幕，不挂到玩家或相机。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health|Presentation")
	TObjectPtr<UWidgetComponent> HealthBar;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
	TObjectPtr<UfpstrueEnemyCombatComponent> CombatComponent;

	// 无 Socket 绑定、无重叠事件、无导航影响；客户端只保留组件身份，不创建权威命中查询。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Hit Proxies")
	TObjectPtr<USphereComponent> EnemyHeadHitProxy;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat|Hit Proxies")
	TObjectPtr<UCapsuleComponent> EnemyBodyHitProxy;

	// 近远距离使用相同几何规则。命令行Reference只用于同一二进制的完整骨骼对照。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit Proxies")
	bool bEnableSimpleHitProxies = true;

	// ==================== AI 与战斗配置 ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float MoveSpeed = 500.0f;

	// 在敌人蓝图 Class Defaults 中配置；BeginPlay 写入 CharacterMovement 的 Yaw 转速（度/秒）。
	// AI 仍决定朝向来源，移动和站定面向目标都由 CharacterMovement 按此转速执行。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Rotation",
			  meta = (ClampMin = "1.0", ClampMax = "3600.0", Units = "deg/s"))
	float MovementYawRotationRate = 540.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float ChaseRange = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float DestroyDelay = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	bool bDestroyOnDeath = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "15000.0"))
	float DeathImpulseStrength = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeathImpulseUpwardBias = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Hit Reaction", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float HitReactionImpulseStrength = 120.0f;

	// ==================== 性能分级配置 ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance")
	bool bEnableMovementUpdateTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FullRateMovementDistance = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementDistance = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementTickInterval = 1.0f / 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateMovementTickInterval = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateAnimationTickInterval = 1.0f / 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateAnimationTickInterval = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "1.0"))
	float ReducedDecisionIntervalMultiplier = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "1.0"))
	float BackgroundDecisionIntervalMultiplier = 2.0f;

	// 仅网络权威端收紧 Actor 复制频率；不是客户端渲染档位，也不是每连接独立频率或相关性裁剪。
	// -EnemyReplication=Reference 保留资产原始频率，用于同一160敌人负载的单变量对照。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance|Networking")
	bool bEnableNetworkReplicationTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance|Networking", meta = (ClampMin = "1.0", Units = "Hz"))
	float FullReplicationFrequencyCap = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance|Networking", meta = (ClampMin = "1.0", Units = "Hz"))
	float ReducedReplicationFrequencyCap = 15.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance|Networking", meta = (ClampMin = "1.0", Units = "Hz"))
	float BackgroundReplicationFrequencyCap = 5.0f;

	// ==================== Blueprint 表现事件 ====================

	// 明确的播放命令：蓝图把 AttackId 原样交给 Combat 的 PlayAttackMontageForAttack。
	UFUNCTION(BlueprintImplementableEvent, Category = "AI|Animation")
	void OnAttackPlaybackRequested(int64 AttackId);

	// 攻击已开始的观察通知，不负责隐式选择或绑定动画。
	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackStarted();

	// 蓝图播放敌人受伤表现。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	// 蓝图播放敌人死亡表现。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDied();

private:
	// ==================== 受控协作者 ====================

	// 语法复习：friend 只开放 private 访问权，不改变对象所有权；内部桥接仅开放给性能协调器和 CombatComponent。
	friend class UfpstrueEnemyAnimationSharingCoordinator;
	friend class UfpstrueEnemySignificanceCoordinator;
	friend class UfpstrueEnemyCombatComponent;
	friend class FFpstrueEnemyQueuedDamageContextTest;
	friend class FFpstrueEnemySignificanceCombatSampleTest;
	friend class AfpstruePvpGameMode;
	friend class FFpstrueNetworkEnemyPresentationTest;
	friend class FFpstrueMultiplayerSignificanceCoordinatorTest;
	friend class FFpstrueMultiplayerClientPerformanceTest;
	friend class FFpstrueMultiplayerAuthorityPoseTest;
	friend class FFpstrueEnemyReplicationTieringTest;
	friend class FFpstrueEnemySimpleHitProxyTest;

	// 先订阅再读当前生命，兼容晚加入和属性先到；死亡后迟到的血量事件不能重新显示血条。
	UFUNCTION() void RefreshHealthBar(float NewHealth);

	// ==================== 战斗与受击内部桥接 ====================

	// 从 AIController 读取唯一玩家目标，供战斗和距离判断使用。
	AfpstrueCharacter* GetCombatTarget() const;
	// 攻击前恢复全速动画/LOD，结束后重新应用性能档位。
	void SetAttackAnimationPriority(bool bHighPriority);
	// 布娃娃启用后的下一帧施加死亡冲量。
	void ApplyDeathImpulse();
	// 受击保护有自己的截止时间，即使协调器停用，也不会永久锁在全速动画。
	void RefreshHitAnimationProtection();
	// 服务器播放绑定或取消时发布快照；晚加入只恢复尚未结束的播放，不重放历史事件。
	void PublishAttackPresentation(uint32 AttackId, UAnimMontage* Montage, float PlayRate, bool bPlaying);
	UFUNCTION() void OnRep_NetworkPresentation();
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastHitReaction(float DamageAmount);

	// ==================== Gameplay 分级 ====================

	// 向 UE Significance Manager 注册 Gameplay 距离评分回调。
	void RegisterWithSignificanceManager();
	// 死亡和 EndPlay 时注销 Gameplay Significance。
	void UnregisterFromSignificanceManager();
	// Significance Manager 回调：把距离分数转换为 Gameplay Tier。
	void ApplySignificance(float Significance);
	// 根据 Gameplay Tier 调整 CharacterMovement Tick 间隔。
	void ApplyGameplaySignificanceIntervals();
	// 读取所有参与者聚合后的 GameplayTier，保护攻击/受击，仅修改复制调度上限，不改决策/移动。
	void ApplyNetworkReplicationPolicy();
	// 显式命令行A/B入口可单测，不依赖全局命令行静态缓存或覆盖蓝图默认值。
	static bool IsReferenceReplicationRequested(const TCHAR* CommandLine);
	// BeginPlay配置一次，Simple继承原Mesh对象类型及查询响应，死亡时关闭；切换LOD不重建PhysicsAsset或代理。
	void ConfigureHitProxies(bool bSimpleMode);
	void DisableHitProxies();

	// ==================== Render 分级与动画共享 ====================

	// 把当前 Render Tier 和预算结果写入 SkeletalMeshComponent。
	void ApplyRenderSignificanceSettings();
	// 敌人不再使用脚部后处理；初始化/共享/死亡路径统一清理实例，主AnimInstance与命中骨骼仍保留。
	UFUNCTION() void DisableEnemyPostProcessAnimation();
	// 存活时消费预算，死亡后关闭主 Mesh 与附件的阴影/光追；普通可见性和布娃娃不受影响。
	void ApplyRenderBudgetMeshFlags();
	// 判断本敌人是否满足成为低风险 Animation Sharing Follower 的条件。
	bool CanUseAnimationSharing() const;
	// Render Tier 变化后让 Coordinator 刷新共享注册状态。
	void RefreshAnimationSharingRegistration();
	// 战斗、受击或死亡前立即退出 Animation Sharing。
	void SuspendAnimationSharing();

	// ==================== 运行时状态 ====================

	// HealthComponent 保存死亡事实；这里只记录死亡副作用是否已经执行。
	bool bDeathEffectsApplied = false;
	bool bUseSimpleHitProxies = false;
	bool bRemovingEnemyPostProcess = false;
	UPROPERTY(ReplicatedUsing = OnRep_NetworkPresentation)
	FFPEnemyNetworkPresentation NetworkPresentation;
	// 客户端自己的播放实例只用于停止表现，绝不绑定服务器攻击事务或执行伤害。
	FFPActionPlayback RemoteAttackPlayback;
	bool bDisableAnimationOptimizationsForBenchmark = false;
	// BeginPlay 保存资产原始设置；战斗保护结束后恢复，而不是强制开启优化。
	bool bAuthoredUpdateRateOptimizations = false;
	EVisibilityBasedAnimTickOption AuthoredAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	bool bRegisteredWithSignificanceManager = false;
	bool bCapturedAuthoredReplicationFrequency = false;
	bool bReferenceEnemyReplication = false;
	float AuthoredNetUpdateFrequency = 0.0f;
	float AuthoredMinNetUpdateFrequency = 0.0f;
	// 主线程集中采样后交给插件评分；Update 同步等待评分完成，期间这些值不再修改。
	FVector SignificancePositionSnapshot = FVector::ZeroVector;
	bool bHasSignificancePositionSnapshot = false;
	// 只借用 Coordinator 本轮候选中的近战保护值；Update 退出即撤销，真实攻击与命中从不读取它。
	const bool* SignificanceCombatProtectionSample = nullptr;
	// Gameplay 档位控制 AI/移动节奏；自然 Render 档位经过滞回，再由全局预算决定最终档位。
	EFPEnemySignificanceTier SignificanceTier = EFPEnemySignificanceTier::Full;
	EFPEnemyRenderSignificanceTier NaturalRenderSignificanceTier = EFPEnemyRenderSignificanceTier::Full;
	EFPEnemyRenderSignificanceTier RenderSignificanceTier = EFPEnemyRenderSignificanceTier::Full;
	EFPEnemyRenderSignificanceTier PendingRenderDemotionTier = EFPEnemyRenderSignificanceTier::Full;
	float RenderSignificanceScore = 1.0f;
	// 视锥历史只服务渲染评分；战斗和受击历史只保护动画，不混入渲染优先级。
	float LastPrimaryFrustumTime = -MAX_flt;
	float LastCombatRelevantTime = -MAX_flt;
	float HitAnimationProtectionUntil = -MAX_flt;
	FTimerHandle HitAnimationProtectionTimer;
	float LastNaturalRenderTierChangeTime = 0.0f;
	float PendingRenderDemotionStartTime = -MAX_flt;
	bool bRenderShouldCastShadow = true;
	// 诊断关闭和正常预算分开保存；最终组件资格还要与资产原始许可取交集。
	bool bDisableEnemyShadowsForBenchmark = false;
	bool bDisableEnemyRayTracingForBenchmark = false;
	bool bHasRenderSignificancePolicy = false;
	bool bRenderShouldBeVisibleInRayTracing = true;
	bool bGameplayAnimationProtection = false;
	int32 AppliedMinimumLOD = INDEX_NONE;
	FFPEnemyRenderSignificancePolicy LastRenderSignificancePolicy;
	// 缓存弱组件引用及首次登记时的资产许可，避免每轮遍历附件或丢失原始禁用设置。
	struct FRenderBudgetMesh
	{
		TWeakObjectPtr<UMeshComponent> Mesh;
		bool bAuthoredShadow = false;
		bool bAuthoredRayTracing = false;
	};
	TArray<FRenderBudgetMesh> RenderBudgetMeshes;

	// 语法复习：TWeakObjectPtr 不延长协调器生命周期；使用前通过 Get()/IsValid() 解析。
	UPROPERTY(Transient)
	TWeakObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> AnimationSharingCoordinator;

	// 普通受击快照随结算更新；击杀快照在死亡入口冻结，供下一 Tick 的布娃娃冲量使用。
	FFPDamageContext LastAppliedDamage;
	FFPDamageContext DeathDamageContext;
};
