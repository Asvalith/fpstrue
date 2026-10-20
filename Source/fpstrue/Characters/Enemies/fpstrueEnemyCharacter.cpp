// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/ProgressBar.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Net/UnrealNetwork.h"
#include "Math/RotationMatrix.h"
#include "SignificanceManager.h"
#include "Camera/PlayerCameraManager.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

CSV_DEFINE_CATEGORY(fpstrueEnemyUI, true);

namespace
{
	// 同一 Gameplay 档位有不同消费者，共用映射但不混合决策倍率与 Movement 更新时机。
	float GameplayTierValue(EFPEnemySignificanceTier Tier, float Full, float Reduced, float Background)
	{
		switch (Tier)
		{
		case EFPEnemySignificanceTier::Reduced: return Reduced;
		case EFPEnemySignificanceTier::Background: return Background;
		default: return Full;
		}
	}
}

// 没有全局 Tick 或逐帧 Actor 扫描；World 清理事件精确移除对应缓存，弱键也不会保活旧关卡。
UfpstrueEnemyHealthBarComponent::FViewCache::FViewCache()
{
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FViewCache::OnWorldCleanup);
}

UfpstrueEnemyHealthBarComponent::FViewCache::~FViewCache() { FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle); }

void UfpstrueEnemyHealthBarComponent::FViewCache::OnWorldCleanup(UWorld* World, bool, bool) { Worlds.Remove(World); }

UfpstrueEnemyHealthBarComponent::FViewCache& UfpstrueEnemyHealthBarComponent::GetViewCache()
{
	static FViewCache Cache;
	return Cache;
}

// 相机矩阵、FOV 正切及朝向只与视点有关，不需要由每条血条重复构造。
UfpstrueEnemyHealthBarComponent::FViewSnapshot UfpstrueEnemyHealthBarComponent::BuildViewSnapshot(
	const FVector& Location, const FRotator& Rotation, float HorizontalFOV, float AspectRatio)
{
	FViewSnapshot View;
	View.Location = Location;
	const FRotationMatrix Basis(Rotation);
	View.Forward = Basis.GetUnitAxis(EAxis::X);
	View.Right = Basis.GetUnitAxis(EAxis::Y);
	View.Up = Basis.GetUnitAxis(EAxis::Z);
	View.HalfFOVTan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(HorizontalFOV, 5.0f, 170.0f) * 0.5f));
	View.InverseAspect = 1.0 / FMath::Max(AspectRatio, 0.1f);
	View.Facing = FRotationMatrix::MakeFromXZ(-View.Forward, View.Up).Rotator();
	return View;
}

bool UfpstrueEnemyHealthBarComponent::SampleLocalView(UWorld* World, FViewSnapshot& OutView)
{
	APlayerController* Viewer = World != nullptr ? World->GetFirstPlayerController() : nullptr;
	if (!IsValid(Viewer) || !Viewer->IsLocalController() || !IsValid(Viewer->PlayerCameraManager)) return false;
	int32 Width = 0, Height = 0;
	Viewer->GetViewportSize(Width, Height);
	if (Width <= 0 || Height <= 0) return false;
	APlayerCameraManager* Camera = Viewer->PlayerCameraManager;
	OutView = BuildViewSnapshot(Camera->GetCameraLocation(), Camera->GetCameraRotation(), Camera->GetFOVAngle(), static_cast<float>(Width) / Height);
	OutView.Viewer = Viewer;
	OutView.CameraManager = Camera;
	return true;
}

// ★【缓存边界】仅主线程、同 World、同帧且视点仍有效时复用；失败不缓存，视点迟到或失效后可在同帧立即恢复。
bool UfpstrueEnemyHealthBarComponent::ResolveViewSnapshot(UWorld* World, uint64 Frame, bool bUseCache,
	FViewSnapshot& OutView, bool& bCacheHit, TFunctionRef<bool(UWorld*, FViewSnapshot&)> Sampler)
{
	check(IsInGameThread());
	bCacheHit = false;
	if (!IsValid(World)) return false;
	if (!bUseCache) return Sampler(World, OutView);
	FViewCache& Cache = GetViewCache();
	const TWeakObjectPtr<UWorld> Key(World);
	if (const FViewCache::FEntry* Entry = Cache.Worlds.Find(Key); Entry != nullptr && Entry->Frame == Frame)
	{
		APlayerController* Viewer = Entry->View.Viewer.Get();
		APlayerCameraManager* Camera = Entry->View.CameraManager.Get();
		if (IsValid(Viewer) && Viewer->IsLocalController() && IsValid(Camera) && Viewer->GetWorld() == World && Camera->GetWorld() == World
			&& Viewer->PlayerCameraManager == Camera)
		{
			OutView = Entry->View;
			bCacheHit = true;
			return true;
		}
	}
	if (!Sampler(World, OutView))
	{
		Cache.Worlds.Remove(Key);
		return false;
	}
	FViewCache::FEntry& Entry = Cache.Worlds.FindOrAdd(Key);
	Entry.Frame = Frame;
	Entry.View = OutView;
	return true;
}

UfpstrueEnemyHealthBarComponent::UfpstrueEnemyHealthBarComponent()
{
	SetWidgetSpace(EWidgetSpace::World);
	SetManuallyRedraw(true);
	SetRedrawTime(0.0f);
	SetTickMode(ETickMode::Enabled);
	// 自己的视锥决定是否重绘；不再等待上次 RenderTime，避免屏外返回后延迟恢复。
	SetTickWhenOffscreen(true);
	SetBlendMode(EWidgetBlendMode::Masked);
	SetTwoSided(true);
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

EFPEnemyHealthBarMode UfpstrueEnemyHealthBarComponent::GetConfiguredMode()
{
#if !UE_BUILD_SHIPPING
	static const EFPEnemyHealthBarMode Configured = []
	{
		FString Value;
		FParse::Value(FCommandLine::Get(), TEXT("EnemyHealthBars="), Value);
		if (Value.Equals(TEXT("Off"), ESearchCase::IgnoreCase)) return EFPEnemyHealthBarMode::Off;
		if (Value.Equals(TEXT("Screen"), ESearchCase::IgnoreCase)) return EFPEnemyHealthBarMode::Screen;
		if (Value.Equals(TEXT("WorldAlways"), ESearchCase::IgnoreCase)) return EFPEnemyHealthBarMode::WorldAlways;
		return EFPEnemyHealthBarMode::WorldEvent;
	}();
	return Configured;
#else
	return EFPEnemyHealthBarMode::WorldEvent;
#endif
}

const TCHAR* UfpstrueEnemyHealthBarComponent::GetModeName(EFPEnemyHealthBarMode InMode)
{
	switch (InMode)
	{
	case EFPEnemyHealthBarMode::Off: return TEXT("Off");
	case EFPEnemyHealthBarMode::Screen: return TEXT("Screen");
	case EFPEnemyHealthBarMode::WorldAlways: return TEXT("WorldAlways");
	default: return TEXT("WorldEvent");
	}
}

void UfpstrueEnemyHealthBarComponent::Configure(EFPEnemyHealthBarMode InMode)
{
	ReleasePresentation();
	Mode = GetNetMode() == NM_DedicatedServer ? EFPEnemyHealthBarMode::Off : InMode;
	if (Mode == EFPEnemyHealthBarMode::Off) return;
	bReleased = false;
	LastFraction = -1.0f;
	SetWidgetSpace(Mode == EFPEnemyHealthBarMode::Screen ? EWidgetSpace::Screen : EWidgetSpace::World);
	SetManuallyRedraw(Mode == EFPEnemyHealthBarMode::WorldEvent);
	SetTickMode(ETickMode::Enabled);
	SetComponentTickEnabled(true);
	SetHiddenInGame(false);
	SetVisibility(true);
	// ReleaseResources 会销毁 World Widget 的 Renderer；沿引擎注册路径重建，不能只重建 UObject 而留下空白贴片。
	if (IsRegistered()) ReregisterComponent();
	SetWidgetClass(UfpstrueEnemyHealthBarWidget::StaticClass());
	InitWidget();
	if (GetUserWidgetObject() != nullptr) ++Created;
}

void UfpstrueEnemyHealthBarComponent::SetHealthFraction(float Fraction)
{
	if (bReleased) return;
	const float SafeFraction = FMath::IsFinite(Fraction) ? FMath::Clamp(Fraction, 0.0f, 1.0f) : 0.0f;
	if (FMath::IsNearlyEqual(LastFraction, SafeFraction)) return;
	LastFraction = SafeFraction;
	if (auto* HealthWidget = Cast<UfpstrueEnemyHealthBarWidget>(GetUserWidgetObject()))
	{
		HealthWidget->SetHealthFraction(SafeFraction);
		++ValueUpdates;
		CSV_CUSTOM_STAT(fpstrueEnemyUI, ValueUpdates, 1, ECsvCustomStatOp::Accumulate);
		RequestEventRedraw();
	}
}

void UfpstrueEnemyHealthBarComponent::RequestEventRedraw()
{
	if (bReleased || Mode != EFPEnemyHealthBarMode::WorldEvent) return;
	++RedrawRequests;
	RequestRenderUpdate();
	CSV_CUSTOM_STAT(fpstrueEnemyUI, RedrawRequests, 1, ECsvCustomStatOp::Accumulate);
}

void UfpstrueEnemyHealthBarComponent::ReleasePresentation()
{
	bReleased = true;
	bInLocalView = false;
	SetHiddenInGame(true);
	SetVisibility(false);
	SetWidgetClass(nullptr);
	ReleaseResources();
	RenderTarget = nullptr;
	UpdateMaterialInstanceParameters();
	MarkRenderStateDirty();
	SetTickMode(ETickMode::Disabled);
	SetComponentTickEnabled(false);
}

void UfpstrueEnemyHealthBarComponent::UpdateForView(const FVector& CameraLocation, const FRotator& CameraRotation,
	float HorizontalFOV, float AspectRatio)
{
	if (bReleased) return;
	UpdateForView(BuildViewSnapshot(CameraLocation, CameraRotation, HorizontalFOV, AspectRatio));
}

void UfpstrueEnemyHealthBarComponent::UpdateForView(const FViewSnapshot& View)
{
	if (bReleased) return;
	// 位置仍从每个血条自己的平滑 Mesh 读取，缓存不冻结敌人，也不降低转镜头时的刷新频率。
	const FVector Delta = GetComponentLocation() - View.Location;
	const double Depth = FVector::DotProduct(Delta, View.Forward);
	const double HalfWidth = FMath::Max(0.0, Depth) * View.HalfFOVTan;
	// 保守包含血条矩形，不把“中心在视锥内”冒充真实无遮挡/屏幕像素可见。
	const bool bInsideView = Depth > 0.0 &&
		FMath::Abs(FVector::DotProduct(Delta, View.Right)) <= HalfWidth + 40.0 &&
		FMath::Abs(FVector::DotProduct(Delta, View.Up)) <= HalfWidth * View.InverseAspect + 8.0;
	const bool bReturnedToView = bInsideView && !bInLocalView;
	bInLocalView = bInsideView;
	if (Mode != EFPEnemyHealthBarMode::Screen)
	{
		SetVisibility(bInsideView);
		// 四边形平行于真实视平面，保留相机的上轴；逐位置 look-at 在俯视下会让血条径向倾斜。
		// 只旋转表现组件，仍使用 World 深度测试，不改变敌人/平滑 Mesh 的朝向与位置。
		if (bInsideView)
			SetWorldRotation(View.Facing);
	}
	if (bReturnedToView) RequestEventRedraw();
}

void UfpstrueEnemyHealthBarComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (bReleased) return;
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyUI_ViewAndWidgetTick);
	CSV_SCOPED_TIMING_STAT(fpstrueEnemyUI, ViewAndWidgetTick);
	// 同 DLL 的公平 A/B：回退只改变视点采样策略，不改变血条模式、Gameplay 或网络规则。
#if !UE_BUILD_SHIPPING
	static const bool bUseSharedView = !FParse::Param(FCommandLine::Get(), TEXT("EnemyUIViewPerComponent"));
#else
	constexpr bool bUseSharedView = true;
#endif
	FViewSnapshot View;
	bool bCacheHit = false;
	const bool bHasView = ResolveViewSnapshot(GetWorld(), GFrameCounter, bUseSharedView, View, bCacheHit, &SampleLocalView);
	if (bCacheHit)
	{
		++ViewCacheHits;
		CSV_CUSTOM_STAT(fpstrueEnemyUI, ViewCacheHits, 1, ECsvCustomStatOp::Accumulate);
	}
	else
	{
		++ViewSamples;
		CSV_CUSTOM_STAT(fpstrueEnemyUI, ViewSamples, 1, ECsvCustomStatOp::Accumulate);
	}
	if (bHasView) UpdateForView(View);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

bool UfpstrueEnemyHealthBarComponent::ShouldDrawWidget() const
{
	return !bReleased && bInLocalView && Super::ShouldDrawWidget();
}

void UfpstrueEnemyHealthBarComponent::DrawWidgetToRenderTarget(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemyUI_RenderTargetRedraw);
	CSV_SCOPED_TIMING_STAT(fpstrueEnemyUI, RenderTargetRedrawTime);
	Super::DrawWidgetToRenderTarget(DeltaTime);
	// NullRHI 没有真实 RT 绘制；只统计有实际 RenderTarget 的调用，不以请求数冒充重绘数。
	if (!GUsingNullRHI && SlateWindow.IsValid() && WidgetRenderer != nullptr && GetRenderTarget() != nullptr)
	{
		++RenderTargetDraws;
		CSV_CUSTOM_STAT(fpstrueEnemyUI, RenderTargetDraws, 1, ECsvCustomStatOp::Accumulate);
	}
}

void UfpstrueEnemyHealthBarComponent::AppendMetrics(FFPEnemyHealthBarMetrics& Metrics) const
{
	++Metrics.Components;
	if (GetUserWidgetObject() != nullptr)
	{
		++Metrics.Widgets;
		if (bInLocalView && !bReleased) ++Metrics.FrustumVisible;
		if (GetWidgetSpace() == EWidgetSpace::Screen) ++Metrics.ScreenWidgets;
		else
		{
			++Metrics.WorldWidgets;
			if (GetManuallyRedraw()) ++Metrics.ManualWorldWidgets;
		}
	}
	Metrics.Created += Created;
	Metrics.ValueUpdates += ValueUpdates;
	Metrics.RedrawRequests += RedrawRequests;
	Metrics.RenderTargetDraws += RenderTargetDraws;
	Metrics.ViewSamples += ViewSamples;
	Metrics.ViewCacheHits += ViewCacheHits;
}

FFPEnemyHealthBarMetrics UfpstrueEnemyHealthBarComponent::CollectMetrics(UWorld* World)
{
	FFPEnemyHealthBarMetrics Metrics;
	if (World != nullptr)
		for (TActorIterator<AfpstrueEnemyCharacter> It(World); It; ++It)
			if (const auto* Bar = It->FindComponentByClass<UfpstrueEnemyHealthBarComponent>()) Bar->AppendMetrics(Metrics);
	return Metrics;
}

// 不接受鼠标/键盘输入；交互与焦点仍由玩家自己的 HUD 管理。
UfpstrueEnemyHealthBarWidget::UfpstrueEnemyHealthBarWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	SetIsFocusable(false);
}

// 仅设置已结算数值的显示比例；在 WidgetTree 尚未创建时保留最后一份有效值。
void UfpstrueEnemyHealthBarWidget::SetHealthFraction(float Fraction)
{
	HealthFraction = FMath::IsFinite(Fraction) ? FMath::Clamp(Fraction, 0.0f, 1.0f) : 0.0f;
	if (HealthProgress != nullptr) HealthProgress->SetPercent(HealthFraction);
}

// 原生构建一个无输入、无逐帧数值绑定的血条，不改动原 HUD 或新增蓝图资产。
TSharedRef<SWidget> UfpstrueEnemyHealthBarWidget::RebuildWidget()
{
	if (WidgetTree == nullptr) WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	if (HealthProgress == nullptr)
	{
		HealthProgress = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("HealthProgress"));
		HealthProgress->SetFillColorAndOpacity(FLinearColor(0.12f, 0.85f, 0.22f, 1.0f));
		WidgetTree->RootWidget = HealthProgress;
	}
	HealthProgress->SetPercent(HealthFraction);
	return Super::RebuildWidget();
}

/*
 * 敌人 Pawn 与各子系统之间的桥接层。
 * HealthComponent/CombatComponent 分别拥有生命和攻击事务，AIController 持有目标并执行行为树动作；本类负责组件装配、
 * 受击死亡表现，以及把 Gameplay/Render Significance 结果转换成移动、骨骼、阴影和 RT 组件设置。
 *
 * 本类刻意不保存第二份 AI、血量或攻击状态：对外查询都转发到真正所有者，跨模块操作通过窄接口完成。
 * Gameplay Significance 影响决策/移动节奏及网络端复制调度上限；Render Significance 只影响骨骼、动画、阴影、RT 和动画共享。
 * 攻击与近期战斗通过 GameplayAnimationProtection 临时恢复完整动画，防止性能策略破坏 Notify 和 Socket 判定。
 */

// ==================== 组件初始化与生命周期 ====================

// 构造默认 Mesh/Movement 行为以及可复用生命、战斗组件；高层决策由自动生成的 AIController 承担。
AfpstrueEnemyCharacter::AfpstrueEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	AIControllerClass = AfpstrueEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 96.0f);
	// 每个敌人的 CharacterMovement 使用 RVO 做局部避让；AIController 仍负责路径请求和高层状态。
	// OnPossess 会再次显式开启，避免已保存蓝图中的旧默认值让同一构建出现混合模式。
	GetCharacterMovement()->bUseRVOAvoidance = true;

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
		CharacterMesh->SetDisablePostProcessBlueprint(true);
	}

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
	CombatComponent = CreateDefaultSubobject<UfpstrueEnemyCombatComponent>(TEXT("CombatComponent"));
	EnemyHeadHitProxy = CreateDefaultSubobject<USphereComponent>(TEXT("EnemyHeadHitProxy"));
	EnemyBodyHitProxy = CreateDefaultSubobject<UCapsuleComponent>(TEXT("EnemyBodyHitProxy"));
	for (UShapeComponent* Proxy : {static_cast<UShapeComponent*>(EnemyHeadHitProxy.Get()), static_cast<UShapeComponent*>(EnemyBodyHitProxy.Get())})
	{
		Proxy->SetupAttachment(GetCapsuleComponent());
		Proxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Proxy->SetCollisionObjectType(ECC_Pawn);
		// 构造阶段保持停用；BeginPlay按原Mesh的完整查询响应配置，不能把活体代理限制成仅WeaponTrace。
		Proxy->SetCollisionResponseToAllChannels(ECR_Ignore);
		Proxy->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
		Proxy->SetGenerateOverlapEvents(false);
		Proxy->SetCanEverAffectNavigation(false);
		Proxy->SetHiddenInGame(true);
		Proxy->PrimaryComponentTick.bCanEverTick = false;
	}
	HealthBar = CreateDefaultSubobject<UfpstrueEnemyHealthBarComponent>(TEXT("EnemyHealthBar"));
	HealthBar->SetupAttachment(GetMesh());
	HealthBar->SetRelativeLocation(GetMesh()->GetRelativeTransform().InverseTransformPosition(
		FVector(0.0f, 0.0f, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 28.0f)));
	HealthBar->SetWidgetSpace(EWidgetSpace::World);
	HealthBar->SetDrawSize(FVector2D(80.0f, 8.0f));
	HealthBar->SetPivot(FVector2D(0.5f, 1.0f));
	HealthBar->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HealthBar->SetWindowFocusable(false);
	HealthBar->SetCastShadow(false);
	HealthBar->SetVisibleInRayTracing(false);
	// WidgetClass 留到有画面的端再设置，专用服务器不创建纯 UI 对象。
}

// 旧蓝图可覆盖构造默认值；在实例组件就绪后重申无敌人后处理，并监听后续主动画重新初始化。
void AfpstrueEnemyCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
		CharacterMesh->OnAnimInitialized.AddUniqueDynamic(this, &ThisClass::DisableEnemyPostProcessAnimation);
	DisableEnemyPostProcessAnimation();
}

// 仅作用于本敌人的组件实例。SetDisable本身不会销毁旧实例，残留的同步Proxy仍可能禁止主动画并行求值。
void AfpstrueEnemyCharacter::DisableEnemyPostProcessAnimation()
{
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr || bRemovingEnemyPostProcess) return;
	TGuardValue<bool> RemovalScope(bRemovingEnemyPostProcess, true);
	CharacterMesh->SetDisablePostProcessBlueprint(true);
	CharacterMesh->SetOverridePostProcessAnimBP(nullptr, false); // 不重新初始化主动画，不改资源级PostProcess绑定。
	if (CharacterMesh->GetPostProcessInstance() != nullptr)
	{
		// 仅在旧实例存在时完成正在运行的求值，正常无后处理路径不新增同步等待。
		CharacterMesh->HandleExistingParallelEvaluationTask(true, true);
		if (UAnimInstance* RetiredPostProcess = CharacterMesh->GetPostProcessInstance())
		{
			RetiredPostProcess->UninitializeAnimation();
			if (CharacterMesh->GetPostProcessInstance() == RetiredPostProcess) CharacterMesh->PostProcessAnimInstance = nullptr;
		}
	}
}

// 初始化碰撞、移动与性能策略，绑定生命组件通知；角色本身不承担逐帧 AI 决策。
void AfpstrueEnemyCharacter::BeginPlay()
{
	//初始化顺序：读取Benchmark覆盖 -> 配置Mesh/Movement -> 绑定 Health -> 注册 Gameplay Significance。
	DisableEnemyPostProcessAnimation();
	Super::BeginPlay();
	DisableEnemyPostProcessAnimation(); // 蓝图BeginPlay亦不得重新安装已撤除的敌人后处理。
	// 旧蓝图可能保存过未复制默认值；联网敌人只能由服务器生成，客户端消费复制副本。
	if (HasAuthority()) { SetReplicates(true); SetReplicateMovement(true); }

	//从全局 Benchmark 配置读入诊断开关，缓存成成员变量，避免每帧查配置
	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (BenchmarkConfig.bDisableMovementTiering)
	{
		bEnableMovementUpdateTiering = false;
	}
	bDisableEnemyRayTracingForBenchmark = BenchmarkConfig.bDisableEnemyRayTracing;
	bDisableEnemyShadowsForBenchmark = BenchmarkConfig.bDisableEnemyShadows;
	bDisableAnimationOptimizationsForBenchmark = BenchmarkConfig.bDisableAnimationOptimizations;
	// 在诊断开关修改组件之前记录蓝图原始资格，之后只按预算收紧。
	RefreshRenderBudgetMeshes();

	// 动画诊断单独处理；阴影和光追已统一应用到预算 Mesh 集合。
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		bAuthoredUpdateRateOptimizations = CharacterMesh->bEnableUpdateRateOptimizations;
		AuthoredAnimTickOption = CharacterMesh->VisibilityBasedAnimTickOption;

		// 先保留参考配置；ConfigureHitProxies会在Simple模式退出整套活体骨骼物理，而非新旧形状叠加。
		CharacterMesh->SetSimulatePhysics(false);
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		// Reference射击命中PhysicsAsset取得BoneName；Simple由根查询代理归一相同伤害身份。
		CharacterMesh->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	}
	FString HitProxyMode;
	bool bSimpleMode = bEnableSimpleHitProxies;
	if (FParse::Value(FCommandLine::Get(), TEXT("EnemyHitProxies="), HitProxyMode))
	{
		if (HitProxyMode.Equals(TEXT("Reference"), ESearchCase::IgnoreCase)) bSimpleMode = false;
		else if (HitProxyMode.Equals(TEXT("Simple"), ESearchCase::IgnoreCase)) bSimpleMode = true;
		else UE_LOG(LogTemp, Warning, TEXT("Unknown EnemyHitProxies=%s; retaining authored mode"), *HitProxyMode);
	}
	ConfigureHitProxies(bSimpleMode);
	ApplyRenderSignificanceSettings();
	GetCapsuleComponent()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageResolved.AddUObject(this, &AfpstrueEnemyCharacter::HandleDamageResolved);
		if (GetNetMode() != NM_DedicatedServer)
		{
			// ★【网络表现边界】胶囊体先应用权威位置，身体 Mesh 才做网络平滑；血条跟随 Mesh，不能提前跳到胶囊体的新位置。
			// 初始化仍保留原来的胶囊顶 +28cm 高度；不绑具体头骨，避免不同骨架和动作造成额外跳动。
			const FVector AnchorWorld = GetCapsuleComponent()->GetComponentTransform().TransformPosition(
				FVector(0.0f, 0.0f, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 28.0f));
			const FVector AnchorInMesh = GetMesh()->GetComponentTransform().InverseTransformPosition(AnchorWorld);
			// 旧蓝图可能保存胶囊体父节点或绝对变换；必须在组件实例就绪后显式纠正，不能只依赖 C++ 构造默认值。
			HealthBar->AttachToComponent(GetMesh(), FAttachmentTransformRules::KeepRelativeTransform);
			HealthBar->SetAbsolute(false, false, false);
			HealthBar->SetRelativeLocationAndRotation(AnchorInMesh, FRotator::ZeroRotator);
			HealthBar->SetRelativeScale3D(FVector::OneVector);
			CastChecked<UfpstrueEnemyHealthBarComponent>(HealthBar)->Configure(UfpstrueEnemyHealthBarComponent::GetConfiguredMode());
			HealthComponent->OnHealthChanged.AddUniqueDynamic(this, &ThisClass::RefreshHealthBar);
			RefreshHealthBar(HealthComponent->GetHealth());
		}
		else
		{
			CastChecked<UfpstrueEnemyHealthBarComponent>(HealthBar)->ReleasePresentation();
		}
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = MoveSpeed;
		Movement->bOrientRotationToMovement = false;
		Movement->bUseControllerDesiredRotation = false;
		// 角色配置是转速的唯一来源；旧蓝图未保存此新字段时继续使用原来的 540 度/秒。
		const float SafeMovementYawRotationRate =
			FMath::IsFinite(MovementYawRotationRate) ? FMath::Clamp(MovementYawRotationRate, 1.0f, 3600.0f) : 540.0f;
		Movement->RotationRate = FRotator(0.0f, SafeMovementYawRotationRate, 0.0f);
	}
	bUseControllerRotationYaw = false;
	//注册SignificanceManager
	SignificancePositionSnapshot = GetActorLocation();
	bHasSignificancePositionSnapshot = true;
	RegisterWithSignificanceManager();
	ApplyNetworkReplicationPolicy();
	if (!HasAuthority()) OnRep_NetworkPresentation();
}

void AfpstrueEnemyCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstrueEnemyCharacter, NetworkPresentation);
}

// 两个解析形状采用统一近远规则；本地偏移从角色胶囊尺寸生成，不采样任何骨骼或客户端相机。
void AfpstrueEnemyCharacter::ConfigureHitProxies(bool bSimpleMode)
{
	bUseSimpleHitProxies = bSimpleMode;
	DisableHitProxies();
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr || IsDead()) return;
	CharacterMesh->SetSimulatePhysics(false);
	if (!bUseSimpleHitProxies)
	{
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
		return;
	}
	// Simple只替换查询几何，不把角色从Sight/近战遮挡等通道中移除。
	// 使用作者Mesh的完整响应和对象类型；保留Reference入口的Pawn忽略与WeaponTrace阻挡契约。
	// 两个简化形状不等于原PhysicsAsset轮廓，恢复通道语义不保证遮挡几何逐点一致。
	FCollisionResponseContainer QueryResponses = CharacterMesh->GetCollisionResponseToChannels();
	QueryResponses.SetResponse(ECC_Pawn, ECR_Ignore);
	QueryResponses.SetResponse(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	const ECollisionChannel QueryObjectType = CharacterMesh->GetCollisionObjectType();
	// NoCollision仅是查询配置；显式撤销可能已创建的旧Body，确保Chaos不继续维护旧代理。
	// 保留原PhysicsAsset引用供死亡恢复，不改共享SkeletalMesh资产，也不在每轮LOD切换时重建。
	CharacterMesh->bAlwaysCreatePhysicsState = false;
	CharacterMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (CharacterMesh->IsPhysicsStateCreated()) CharacterMesh->DestroyPhysicsState();
	const float HalfHeight = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float Radius = GetCapsuleComponent()->GetUnscaledCapsuleRadius();
	const float HeadRadius = FMath::Max(Radius * 0.43f, 1.0f);
	EnemyHeadHitProxy->SetSphereRadius(HeadRadius, false);
	EnemyHeadHitProxy->SetRelativeLocation(FVector(0.0f, 0.0f, HalfHeight * 0.82f));
	EnemyBodyHitProxy->SetCapsuleSize(FMath::Max(Radius * 0.78f, 1.0f), FMath::Max(HalfHeight * 0.82f, Radius * 0.78f), false);
	EnemyBodyHitProxy->SetRelativeLocation(FVector(0.0f, 0.0f, -HalfHeight * 0.18f));
	for (UShapeComponent* Proxy : {static_cast<UShapeComponent*>(EnemyHeadHitProxy.Get()), static_cast<UShapeComponent*>(EnemyBodyHitProxy.Get())})
	{
		Proxy->SetCollisionObjectType(QueryObjectType);
		Proxy->SetCollisionResponseToChannels(QueryResponses);
		Proxy->SetGenerateOverlapEvents(false);
		Proxy->SetCanEverAffectNavigation(false);
		Proxy->SetSimulatePhysics(false);
	}
	// 非拥有端只消费服务器命中事实，不为每个远端敌人再创建一套查询/模拟物理对象。
	if (HasAuthority())
	{
		EnemyHeadHitProxy->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		EnemyBodyHitProxy->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}
}

void AfpstrueEnemyCharacter::DisableHitProxies()
{
	EnemyHeadHitProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	EnemyBodyHitProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AfpstrueEnemyCharacter::ResolveHitProxyBone(FHitResult& Hit) const
{
	if (!bUseSimpleHitProxies || Hit.GetActor() != this) return;
	if (Hit.GetComponent() == EnemyHeadHitProxy) Hit.BoneName = TEXT("head");
	else if (Hit.GetComponent() == EnemyBodyHitProxy) Hit.BoneName = TEXT("spine_03");
}

bool AfpstrueEnemyCharacter::RequiresFullAuthorityPose() const
{
	if (!HasAuthority() || GetNetMode() == NM_Standalone || IsDead()) return false;
	const bool bHitProtected = GetWorld() != nullptr && GetWorld()->GetTimeSeconds() < HitAnimationProtectionUntil;
	return !bUseSimpleHitProxies || IsAttacking() || bHitProtected || IsPlayingRootMotion();
}

// 攻击是否发生由服务器事务决定；客户端只读取用于动画的当前快照。
void AfpstrueEnemyCharacter::PublishAttackPresentation(uint32 AttackId, UAnimMontage* Montage, float PlayRate, bool bPlaying)
{
	if (!HasAuthority() || GetNetMode() == NM_Standalone) return;
	NetworkPresentation.AttackId = AttackId;
	NetworkPresentation.bAttacking = bPlaying && !IsDead();
	NetworkPresentation.Montage = Montage;
	NetworkPresentation.PlayRate = PlayRate;
	NetworkPresentation.ServerStartTime = GetWorld()->GetTimeSeconds();
	ForceNetUpdate();
}

// 属性到达顺序不保证生命先于播放快照；死亡表现只使用同一快照中的击杀信息。
void AfpstrueEnemyCharacter::OnRep_NetworkPresentation()
{
	if (HasAuthority() || !HasActorBegunPlay()) return;
	if (NetworkPresentation.bDead)
	{
		LastAppliedDamage.Direction = NetworkPresentation.DeathDirection;
		LastAppliedDamage.Location = NetworkPresentation.DeathLocation;
		LastAppliedDamage.BoneName = NetworkPresentation.DeathBone;
		LastAppliedDamage.bHasHitData = true;
		HandleDeath();
		return;
	}
	if (IsDead()) return;
	if (RemoteAttackPlayback.ActionId == NetworkPresentation.AttackId && RemoteAttackPlayback.IsSet() &&
		NetworkPresentation.bAttacking) return;
	RemoteAttackPlayback.Stop();
	RemoteAttackPlayback.Reset();
	if (!NetworkPresentation.bAttacking || !IsValid(NetworkPresentation.Montage))
	{
		ApplyRenderSignificanceSettings();
		RefreshAnimationSharingRegistration();
		return;
	}
	// ★【重要边界】共享 Follower 必须先恢复自己的 AnimInstance，再绑定本次网络攻击播放。
	LastCombatRelevantTime = GetWorld()->GetTimeSeconds();
	SuspendAnimationSharing();
	ApplyRenderSignificanceSettings();
	UAnimInstance* Anim = GetMesh()->GetAnimInstance();
	if (Anim == nullptr) return;
	const AGameStateBase* State = GetWorld()->GetGameState();
	const double ServerNow = State != nullptr ? State->GetServerWorldTimeSeconds() : NetworkPresentation.ServerStartTime;
	const double EffectiveRate = NetworkPresentation.PlayRate * NetworkPresentation.Montage->RateScale;
	if (!FMath::IsFinite(EffectiveRate) || EffectiveRate <= 0.0) return;
	const float Position = static_cast<float>(FMath::Max(0.0, ServerNow - NetworkPresentation.ServerStartTime) * EffectiveRate);
	if (Position >= NetworkPresentation.Montage->GetPlayLength()) return;
	if (Anim->Montage_Play(NetworkPresentation.Montage, NetworkPresentation.PlayRate,
		EMontagePlayReturnType::MontageLength, Position, true) > 0.0f)
	{
		RemoteAttackPlayback.TryBind(NetworkPresentation.AttackId, GetMesh(), NetworkPresentation.Montage);
		ApplyRenderSignificanceSettings();
	}
}

// 受击是可丢弃的瞬时反馈；生命和死亡仍由复制属性恢复，不依赖收到这条通知。
void AfpstrueEnemyCharacter::MulticastHitReaction_Implementation(float DamageAmount)
{
	if (HasAuthority() || IsDead()) return;
	LastCombatRelevantTime = GetWorld()->GetTimeSeconds();
	HitAnimationProtectionUntil = LastCombatRelevantTime + 0.3f;
	SuspendAnimationSharing();
	ApplyRenderSignificanceSettings();
	GetWorldTimerManager().SetTimer(HitAnimationProtectionTimer, this, &ThisClass::RefreshHitAnimationProtection, 0.3f, false);
	OnEnemyDamaged(DamageAmount, nullptr, nullptr);
}

// 无论死亡销毁还是关卡退出，都解除外部登记、受击计时和事件绑定。
void AfpstrueEnemyCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	//先解除动画共享、注销登记、移除委托
	SuspendAnimationSharing();
	UnregisterFromSignificanceManager();
	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
		CharacterMesh->OnAnimInitialized.RemoveDynamic(this, &ThisClass::DisableEnemyPostProcessAnimation);
	GetWorldTimerManager().ClearTimer(HitAnimationProtectionTimer);

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageResolved.RemoveAll(this);
		HealthComponent->OnHealthChanged.RemoveDynamic(this, &ThisClass::RefreshHealthBar);
	}
	CastChecked<UfpstrueEnemyHealthBarComponent>(HealthBar)->ReleasePresentation();

	Super::EndPlay(EndPlayReason);
}

// ★【UI/复制边界】只读本敌人的最新比例，不依赖可能丢包的受击多播，也不根据镜头方向更新世界位置。
void AfpstrueEnemyCharacter::RefreshHealthBar(float)
{
	if (GetNetMode() == NM_DedicatedServer || HealthBar == nullptr || HealthComponent == nullptr) return;
	if (bDeathEffectsApplied || IsDead())
	{
		CastChecked<UfpstrueEnemyHealthBarComponent>(HealthBar)->ReleasePresentation();
		return;
	}
	CastChecked<UfpstrueEnemyHealthBarComponent>(HealthBar)->SetHealthFraction(HealthComponent->GetHealthNormalized());
}

// ==================== 状态查询与玩法接口 ====================

// 查询生命组件中的死亡事实，不在角色侧维护第二份死亡状态。
bool AfpstrueEnemyCharacter::IsDead() const
{
	// 死亡状态始终转发到 HealthComponent，EnemyCharacter 不保存第二份生命事实。
	return (HealthComponent != nullptr && HealthComponent->IsDead()) || (!HasAuthority() && NetworkPresentation.bDead);
}

// 查询战斗组件的当前攻击事务，用于 AI 选择和动画保护。
bool AfpstrueEnemyCharacter::IsAttacking() const
{
	// 攻击事务状态属于 CombatComponent，角色只提供给 AI 和显著性系统读取。
	return HasAuthority() ? CombatComponent != nullptr && CombatComponent->IsAttacking()
		: NetworkPresentation.bAttacking && !NetworkPresentation.bDead;
}

// 判断攻击、近战范围和近期交互是否需要恢复完整动画；保护不增加阴影或光追预算。
bool AfpstrueEnemyCharacter::RequiresGameplayAnimationProtection(float CurrentTime, float GraceSeconds) const
{
	// 攻击中、已贴近目标或刚发生交互时禁止动画降级，保护 Montage、Notify 和刀刃 Socket 更新。
	const bool bRecentlyInteracted = CurrentTime - LastCombatRelevantTime <= FMath::Max(GraceSeconds, 0.0f);
	return IsAttacking() || (SignificanceCombatProtectionSample != nullptr
		? *SignificanceCombatProtectionSample : IsTargetInAttackRange()) || bRecentlyInteracted;
}

// 复用战斗组件的攻击范围判定，使 AI 与性能保护使用同一组规则。
bool AfpstrueEnemyCharacter::IsTargetInAttackRange() const
{
	// 统一复用 CombatComponent 的二维距离规则，避免 AI、角色和显著性各写一套阈值判断。
	return CombatComponent != nullptr && CombatComponent->IsTargetInAttackRange();
}

// 从当前 AIController 读取可用目标，不绕过感知和目标丢失逻辑直接查玩家位置。
AfpstrueCharacter* AfpstrueEnemyCharacter::GetCombatTarget() const
{
	// AIController 是目标状态的唯一拥有者；角色只在执行战斗和重要性判断时读取。
	const AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(GetController());
	return EnemyController != nullptr ? EnemyController->GetTargetCharacter() : nullptr;
}

// ==================== 战斗接口与动画优先级桥接 ====================

// 兼容旧蓝图符号并转交诊断；无播放身份的通知不能直接结束当前攻击。
void AfpstrueEnemyCharacter::HandleAttackFinishedNotify()
{
	// 保留反射符号帮助旧资产给出迁移诊断；无身份的动画回调不再猜测当前攻击。
	if (CombatComponent != nullptr)
	{
		CombatComponent->HandleAttackFinishedNotify();
	}
}

// 攻击开始前退出共享姿态并重算组件设置；攻击结束后根据当前事实恢复分级与共享资格。
void AfpstrueEnemyCharacter::SetAttackAnimationPriority(bool bHighPriority)
{
	// 攻击前退出动画共享并恢复完整动画/移动更新，结束后再按当前 Significance 重新应用策略。
	if (bHighPriority)
	{
		// 独立 Montage/Notify 开始前先解除 LeaderPose，攻击逻辑不依赖共享动画。
		SuspendAnimationSharing();
	}

	// Combat 已先提交攻击阶段；所有组件设置只从当前事实合成，不在入口重复写另一套策略。
	ApplyGameplaySignificanceIntervals();
	ApplyNetworkReplicationPolicy();
	ApplyRenderSignificanceSettings();
	if (!bHighPriority)
	{
		RefreshAnimationSharingRegistration();
	}
}

// ==================== 受击与死亡 ====================

// 为一次有效伤害构建独立命中快照，再进入引擎伤害入口；不在这里重复扣血。
float AfpstrueEnemyCharacter::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator,
										 AActor* DamageCauser)
{
	// 命中上下文跟随 Health 操作排队；嵌套 B/C 伤害不能互相覆盖延迟使用的方向和骨骼。
	if (IsDead() || !FMath::IsFinite(DamageAmount) || DamageAmount <= 0.0f)
	{
		// 死亡冲量下一帧读取击杀快照，尸体追加命中不能覆盖它。
		return 0.0f;
	}
	// 每次伤害从完整默认快照开始，通用/环境伤害不能沿用上一发的骨骼与命中位置。
	FFPDamageContext Context;
	Context.Direction = -GetActorForwardVector();
	Context.Location = GetActorLocation();
	Context.bHasHitData = true;
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		const FPointDamageEvent* PointDamageEvent = static_cast<const FPointDamageEvent*>(&DamageEvent);
		Context.Direction = PointDamageEvent->ShotDirection.GetSafeNormal();
		Context.Location = PointDamageEvent->HitInfo.ImpactPoint;
		Context.BoneName = PointDamageEvent->HitInfo.BoneName;
	}
	else if (DamageCauser != nullptr)
	{
		Context.Direction = (GetActorLocation() - DamageCauser->GetActorLocation()).GetSafeNormal();
	}

	// 修正无法用于表现的方向和位置；作用域把快照交给生命组件，返回时恢复外层伤害上下文。
	if (Context.Direction.IsNearlyZero() || Context.Direction.ContainsNaN())
	{
		Context.Direction = GetActorForwardVector() * -1.0f;
	}

	if (Context.Location.ContainsNaN())
	{
		Context.Location = GetActorLocation();
	}
	if (HealthComponent != nullptr)
	{
		auto ContextScope = HealthComponent->ScopeDamageContext(Context);
		return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	}
	return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
}

// 扣血已经完成后消费本次伤害快照，立即恢复必要动画并触发受击表现。
void AfpstrueEnemyCharacter::HandleDamageResolved(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy,
	const FFPDamageContext& Context)
{
	// 保存实际结算的命中信息，避免延迟死亡冲量读取到之后另一次命中的数据。
	LastAppliedDamage = Context;
	if (!Context.bHasHitData)
	{
		LastAppliedDamage.Direction = -GetActorForwardVector();
		LastAppliedDamage.Location = GetActorLocation();
	}
	// Health 已完成本次扣血；在同一个通知入口消费不可变伤害快照，再恢复战斗动画并触发表现。
	if (const UWorld* World = GetWorld())
	{
		LastCombatRelevantTime = World->GetTimeSeconds();
		const float Grace = FMath::Max(LastRenderSignificancePolicy.CombatPriorityGraceSeconds, 0.01f);
		HitAnimationProtectionUntil = LastCombatRelevantTime + Grace;
		GetWorldTimerManager().SetTimer(HitAnimationProtectionTimer, this,
			&AfpstrueEnemyCharacter::RefreshHitAnimationProtection, Grace, false);
	}
	// 先退出共享并立即恢复必要动画，再发受击表现；不等待下一轮集中采样，也不赠送阴影/光追名额。
	SuspendAnimationSharing();
	ApplyRenderSignificanceSettings();
	ApplyNetworkReplicationPolicy();
	if (HasAuthority() && GetNetMode() != NM_Standalone) ForceNetUpdate();
	// 根据最近伤害方向给 CharacterMovement 添加轻量受击冲量。
	//活着时通过 CharacterMovement 施加水平冲量，不直接切换物理模拟，保持导航移动仍可继续。
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bSkipHitImpulse = Movement == nullptr || HitReactionImpulseStrength <= 0.0f || Movement->MovementMode == MOVE_None;
	if (!bSkipHitImpulse)
	{
		FVector HitDirection(LastAppliedDamage.Direction.X, LastAppliedDamage.Direction.Y, 0.0f);
		if (HitDirection.Normalize())
		{
			Movement->AddImpulse(HitDirection * HitReactionImpulseStrength, true);
		}
	}
	OnEnemyDamaged(DamageAmount, DamageCauser, InstigatedBy);
	if (HasAuthority() && GetNetMode() != NM_Standalone) MulticastHitReaction(DamageAmount);
}

// 受击保护到期后重新合成动画设置和共享资格，不永久保留全速动画。
void AfpstrueEnemyCharacter::RefreshHitAnimationProtection()
{
	HitAnimationProtectionUntil = -MAX_flt;
	ApplyNetworkReplicationPolicy();
	ApplyRenderSignificanceSettings();
	RefreshAnimationSharingRegistration();
}

// 一次性结束 Gameplay，再切换布娃娃、广播死亡并安排回收；血量死亡事实仍归生命组件。
void AfpstrueEnemyCharacter::HandleDeath()
{
	//死亡顺序先停止会继续回调的系统，再关闭移动/碰撞，最后进入 Ragdoll 并广播给 GameMode/蓝图。
	if (bDeathEffectsApplied || (!HasAuthority() && (!HasActorBegunPlay() || !NetworkPresentation.bDead)))
	{
		return;
	}

	bDeathEffectsApplied = true;
	DisableHitProxies();
	DisableEnemyPostProcessAnimation();
	ApplyNetworkReplicationPolicy(); // 死亡退出存活分档，恢复原始频率；死亡快照仍在下方强制调度。
	HealthBar->SetHiddenInGame(true);
	// Screen 血条必须先从本地屏幕层移除，再禁用 Tick；否则可能留下死亡前最后一帧的悬空 UI。
	// World 与诊断 Screen 模式共用释放入口，同时清除纹理重绘和视图更新资格。
	CastChecked<UfpstrueEnemyHealthBarComponent>(HealthBar)->ReleasePresentation();
	if (HasAuthority() && GetNetMode() != NM_Standalone)
	{
		NetworkPresentation.bDead = true;
		NetworkPresentation.bAttacking = false;
		NetworkPresentation.DeathDirection = LastAppliedDamage.Direction;
		NetworkPresentation.DeathLocation = LastAppliedDamage.Location;
		NetworkPresentation.DeathBone = LastAppliedDamage.BoneName;
		ForceNetUpdate();
	}
	RemoteAttackPlayback.Stop();
	RemoteAttackPlayback.Reset();
	// ★【重要边界】[生命周期] 先冻结击杀快照、撤销预算并停止 AI/攻击，再关闭移动碰撞和启用布娃娃，死亡通知放在内部提交之后。
	DeathDamageContext = LastAppliedDamage;
	GetWorldTimerManager().ClearTimer(HitAnimationProtectionTimer);
	// 尸体不再参与存活敌人 Top-K。退出前撤销全部附属 Mesh 资格，避免名额被新敌人复用后超额。
	RefreshRenderBudgetMeshes();
	SuspendAnimationSharing();
	UnregisterFromSignificanceManager();
	if (AfpstrueEnemyAIController* EnemyAIController = Cast<AfpstrueEnemyAIController>(GetController()))
	{
		// StopAI 先禁止新命令，再清理 Combat；不在这里重复结束同一攻击事务。
		EnemyAIController->StopAI();
	}
	else if (CombatComponent != nullptr)
	{
		CombatComponent->ResetCombat(); // 未被 AI 接管的角色仍需释放播放和许可。
	}

	// 停止决策和攻击后，撤销导航移动与胶囊碰撞，再让骨骼承担布娃娃物理。
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = 0.0f;
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
		Movement->SetComponentTickEnabled(false);
	}

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->SetComponentTickInterval(0.0f);
		// 活体不再维护骨骼物理。死亡仅求值当前动画状态一次，再由原PhysicsAsset接管；
		// 不用累计大Delta补播动画，避免重放攻击Notify或把尸体从初始参考姿态直接弹出。
		if (bUseSimpleHitProxies && CharacterMesh->GetSkeletalMeshAsset() != nullptr)
		{
			CharacterMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			CharacterMesh->bEnableUpdateRateOptimizations = false;
			CharacterMesh->TickAnimation(0.0f, false);
			CharacterMesh->RefreshBoneTransforms();
		}
		CharacterMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetEnableGravity(true);
		CharacterMesh->SetSimulatePhysics(true);
	}

	// 内部死亡状态提交后再通知外部；蓝图新增的 Mesh 同样撤销阴影及光追资格。
	OnEnemyDeathReported.Broadcast(this);
	OnEnemyDied();
	DisableEnemyPostProcessAnimation(); // 死亡蓝图可能更换Mesh/动画，布娃娃仍不重新启用后处理。
	// 蓝图死亡表现可能增添/替换 Mesh，应用相同的尸体策略。
	RefreshRenderBudgetMeshes();
	GetWorldTimerManager().SetTimerForNextTick(this, &AfpstrueEnemyCharacter::ApplyDeathImpulse);

	if (bDestroyOnDeath && HasAuthority())
	{
		SetLifeSpan(DestroyDelay);
	}
}

// 下一 Tick 尝试用击杀快照施加冲量；延迟只是留出物理切换时机，仍须检查模拟状态。
void AfpstrueEnemyCharacter::ApplyDeathImpulse()
{
	// 延迟到下一帧，确保 Ragdoll 刚体已经创建并唤醒后再向实际命中位置施加死亡冲量。
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr || !CharacterMesh->IsSimulatingPhysics())
	{
		return;
	}

	CharacterMesh->SetEnableGravity(true);
	CharacterMesh->WakeAllRigidBodies();

	const FVector ImpulseDirection = (DeathDamageContext.Direction + FVector::UpVector * DeathImpulseUpwardBias).GetSafeNormal();
	if (ImpulseDirection.IsNearlyZero())
	{
		return;
	}

	const FName ImpulseBone = CharacterMesh->GetBodyInstance(DeathDamageContext.BoneName) != nullptr
		? DeathDamageContext.BoneName : NAME_None;
	CharacterMesh->AddImpulseAtLocation(ImpulseDirection * FMath::Clamp(DeathImpulseStrength, 0.0f, 15000.0f), DeathDamageContext.Location, ImpulseBone);
}

// ==================== Gameplay Significance：目标距离与交互状态 ====================

// 向引擎插件登记玩法距离评分和顺序应用回调；不创建每敌人 Tick 或自研工作线程。
void AfpstrueEnemyCharacter::RegisterWithSignificanceManager()
{
	// 每个敌人只注册一个轻量距离函数；集中 Coordinator 调用 Manager::Update，敌人本身没有 Significance Tick。
	if (!HasAuthority() || bRegisteredWithSignificanceManager || GetWorld() == nullptr)
	{
		return;
	}

	USignificanceManager* Manager = USignificanceManager::Get(GetWorld());
	if (Manager == nullptr)
	{
		return;
	}

	// 评分回调只返回距离数值；组件写入放在 Sequential 后处理，而不是评分回调中。
	// ★【重要边界】[线程交接] 主线程先冻结位置；插件同步等待并行评分结束，回调不查询可变的 Actor/Health 状态。
	Manager->RegisterObject(
		this, TEXT("Enemy"),
		[](USignificanceManager::FManagedObjectInfo* ObjectInfo, const FTransform& Viewpoint)
		{
			const auto* Enemy = static_cast<const AfpstrueEnemyCharacter*>(ObjectInfo->GetObject());
			if (!Enemy->bHasSignificancePositionSnapshot)
			{
				return 0.0f;
			}

			const float Distance = FVector::Dist2D(Enemy->SignificancePositionSnapshot, Viewpoint.GetLocation());
			return 1.0f / (1.0f + Distance);
		},
		// ★【重要边界】[线程交接] 并行评分完成后，后处理在 Manager::Update 调用线程顺序执行；本项目由游戏线程 Timer 调用并写入组件。
		USignificanceManager::EPostSignificanceType::Sequential,
		[](USignificanceManager::FManagedObjectInfo* ObjectInfo, float, float NewSignificance, bool bUnregister)
		{
			AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(ObjectInfo->GetObject());
			if (!bUnregister && IsValid(Enemy))
			{
				Enemy->ApplySignificance(NewSignificance);
			}
		});
	bRegisteredWithSignificanceManager = true;
}

// 死亡与 EndPlay 共用的幂等注销入口，阻止集中更新继续向旧敌人下发策略。
void AfpstrueEnemyCharacter::UnregisterFromSignificanceManager()
{
	// EndPlay 和死亡共用的幂等注销入口，阻止 Manager 后续再回调本对象。
	if (!bRegisteredWithSignificanceManager)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (USignificanceManager* Manager = USignificanceManager::Get(World))
		{
			Manager->UnregisterObject(this);
		}
	}
	bRegisteredWithSignificanceManager = false;
}

// 将玩法距离分数映射为移动和决策档位；近战交互优先保留响应，不使用相机视锥。
void AfpstrueEnemyCharacter::ApplySignificance(float Significance)
{
	// 距离决定自然档位，但攻击中或已进入攻击范围必须保持 Full，保证近战响应不被后台档降频。
	if (!HasAuthority() || IsDead())
	{
		return;
	}
	const float FullRateThreshold = 1.0f / (1.0f + FullRateMovementDistance);
	const float MidRateThreshold = 1.0f / (1.0f + MidRateMovementDistance);
	const bool bRequiresFullRate = IsAttacking() || (SignificanceCombatProtectionSample != nullptr
		? *SignificanceCombatProtectionSample : IsTargetInAttackRange());

	EFPEnemySignificanceTier NewTier = EFPEnemySignificanceTier::Background;
	if (bRequiresFullRate || Significance >= FullRateThreshold)
	{
		NewTier = EFPEnemySignificanceTier::Full;
	}
	else if (Significance >= MidRateThreshold)
	{
		NewTier = EFPEnemySignificanceTier::Reduced;
	}

	// 应用 Gameplay Tier，并把决策倍率传给 AIController。
	// 档位未变化不重复下发Movement/AI设置；复制保护独立刷新，变化时再应用移动与决策倍率。
	if (SignificanceTier == NewTier)
	{
		// 即使玩法档位不变，也要收口受击保护到期或Reference/资产关闭后的复制策略。
		ApplyNetworkReplicationPolicy();
		return;
	}

	SignificanceTier = NewTier;
	ApplyGameplaySignificanceIntervals();
	ApplyNetworkReplicationPolicy();
	// 专服也需要消费自身的姿态档位；不能依赖只在本地画面分支运行的Render预算提交。
	if (HasAuthority() && bUseSimpleHitProxies) ApplyRenderSignificanceSettings();

	if (AfpstrueEnemyAIController* EnemyAIController = Cast<AfpstrueEnemyAIController>(GetController()))
	{
		EnemyAIController->SetSignificanceDecisionMultiplier(
			GameplayTierValue(SignificanceTier, 1.0f, ReducedDecisionIntervalMultiplier, BackgroundDecisionIntervalMultiplier));
	}
}

// 仅设置 CharacterMovement 更新间隔；攻击中强制逐帧，不改变渲染档位和预算。
void AfpstrueEnemyCharacter::ApplyGameplaySignificanceIntervals()
{
	// Gameplay 档位只改变 CharacterMovement 的更新间隔；攻击状态始终恢复为逐帧移动更新。
	if (!HasAuthority() || IsDead())
	{
		return;
	}

	float MovementTickInterval = 0.0f;
	if (!IsAttacking() && bEnableMovementUpdateTiering)
	{
		MovementTickInterval = GameplayTierValue(SignificanceTier, 0.0f, MidRateMovementTickInterval, FarRateMovementTickInterval);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetComponentTickInterval(MovementTickInterval);
	}
}

// 命令行只选择调度策略，不关闭复制Actor或更改相关性；Reference能用于Editor及Shipping相同内容的对照。
bool AfpstrueEnemyCharacter::IsReferenceReplicationRequested(const TCHAR* CommandLine)
{
	FString Value;
	return CommandLine != nullptr && FParse::Value(CommandLine, TEXT("EnemyReplication="), Value) &&
		Value.Equals(TEXT("Reference"), ESearchCase::IgnoreCase);
}

// ★【网络预算边界】只收紧网络权威端Actor级频率；近任意有效玩家的Gameplay最高重要性保护所有连接。
void AfpstrueEnemyCharacter::ApplyNetworkReplicationPolicy()
{
	const ENetMode Mode = GetNetMode();
	if (!HasAuthority() || (Mode != NM_DedicatedServer && Mode != NM_ListenServer)) return;
	if (!bCapturedAuthoredReplicationFrequency)
	{
		// BeginPlay时蓝图/实例的配置已生效，不用C++ CDO覆盖资产；同一Actor只捕获一次以便精确恢复。
		AuthoredNetUpdateFrequency = GetNetUpdateFrequency();
		AuthoredMinNetUpdateFrequency = GetMinNetUpdateFrequency();
		bReferenceEnemyReplication = IsReferenceReplicationRequested(FCommandLine::Get());
		bCapturedAuthoredReplicationFrequency = true;
	}
	if (!FMath::IsFinite(AuthoredNetUpdateFrequency) || AuthoredNetUpdateFrequency <= 0.0f ||
		!FMath::IsFinite(AuthoredMinNetUpdateFrequency) || AuthoredMinNetUpdateFrequency < 0.0f) return;

	float DesiredFrequency = AuthoredNetUpdateFrequency;
	float DesiredMinFrequency = AuthoredMinNetUpdateFrequency;
	if (bEnableNetworkReplicationTiering && !bReferenceEnemyReplication && !bDeathEffectsApplied && !IsDead())
	{
		const float FullCap = FMath::IsFinite(FullReplicationFrequencyCap) ? FMath::Max(1.0f, FullReplicationFrequencyCap) : 30.0f;
		const float ReducedCap = FMath::Min(FullCap,
			FMath::IsFinite(ReducedReplicationFrequencyCap) ? FMath::Max(1.0f, ReducedReplicationFrequencyCap) : 15.0f);
		const float BackgroundCap = FMath::Min(ReducedCap,
			FMath::IsFinite(BackgroundReplicationFrequencyCap) ? FMath::Max(1.0f, BackgroundReplicationFrequencyCap) : 5.0f);
		const float Now = GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0f;
		// 战斗开始/受击当帧提升，不必等待下一次集中采样；到期后由已有受击Timer和Significance采样收口。
		// 近距保护来自GameplayTier（所有玩家最大分数），从不读取本地相机或RenderTier。
		const bool bProtectCombat = IsAttacking() || Now < HitAnimationProtectionUntil;
		const EFPEnemySignificanceTier EffectiveTier = bProtectCombat ? EFPEnemySignificanceTier::Full : SignificanceTier;
		DesiredFrequency = FMath::Min(AuthoredNetUpdateFrequency, GameplayTierValue(EffectiveTier, FullCap, ReducedCap, BackgroundCap));
		// 自适应复制启用时，Min也不能超过新上限；不抬高资产原本较低的Min。
		DesiredMinFrequency = FMath::Min(AuthoredMinNetUpdateFrequency, DesiredFrequency);
	}
	if (!FMath::IsNearlyEqual(GetNetUpdateFrequency(), DesiredFrequency) || !FMath::IsNearlyEqual(GetMinNetUpdateFrequency(), DesiredMinFrequency))
	{
		SetNetUpdateFrequency(DesiredFrequency);
		SetMinNetUpdateFrequency(DesiredMinFrequency);
		ForceNetUpdate(); // 提升/降级/恢复都及时交给引擎调度；不是立即发送或可靠保证。
	}
}

// ==================== Render Significance：可见性、相机距离与渲染预算 ====================

// 根据统一相机快照计算渲染重要性；使用 Bounds 近似视锥和屏占比，不是硬件遮挡查询。
FFPEnemyRenderSignificanceSample AfpstrueEnemyCharacter::EvaluateRenderSignificance(const FFPEnemyRenderScoringContext& ScoringContext)
{
	// 相机矩阵、投影系数、策略归一化每轮只算一次；本对象只读取自己的Bounds和视锥历史。
	if (IsDead()) return {};
	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	const FVector BoundsOrigin = CharacterMesh != nullptr ? CharacterMesh->Bounds.Origin : GetActorLocation();
	const float BoundsRadius = CharacterMesh != nullptr ? FMath::Max(CharacterMesh->Bounds.SphereRadius, 1.0f) : 100.0f;
	FFPEnemyRenderSignificanceSample Sample = ScoringContext.EvaluateBounds(BoundsOrigin, BoundsRadius, LastPrimaryFrustumTime);
	RenderSignificanceScore = Sample.Score;
	return Sample;
}
EFPEnemyRenderSignificanceTier AfpstrueEnemyCharacter::ResolveNaturalRenderSignificanceTier(const FFPEnemyRenderSignificanceSample& Sample,
																							const FFPEnemyRenderSignificancePolicy& Policy)
{
	// 自然档位升档立即响应，降档需满足退出阈值、最短保持时间和延迟；全局 Top-K 名额仍由 Coordinator 分配。
	const UWorld* World = GetWorld();
	const float CurrentTime = World != nullptr ? World->GetTimeSeconds() : 0.0f;
	if (!Policy.bEnableRenderTiering)
	{
		if (NaturalRenderSignificanceTier != EFPEnemyRenderSignificanceTier::Full)
		{
			LastNaturalRenderTierChangeTime = CurrentTime;
		}
		NaturalRenderSignificanceTier = EFPEnemyRenderSignificanceTier::Full;
		PendingRenderDemotionTier = NaturalRenderSignificanceTier;
		PendingRenderDemotionStartTime = -MAX_flt;
		return NaturalRenderSignificanceTier;
	}

	const float FullEnterThreshold = FMath::Clamp(Policy.FullEnterThreshold, 0.0f, 1.0f);
	const float FullExitThreshold = FMath::Min(FMath::Clamp(Policy.FullExitThreshold, 0.0f, 1.0f), FullEnterThreshold);
	const float ReducedEnterThreshold = FMath::Min(FMath::Clamp(Policy.ReducedEnterThreshold, 0.0f, 1.0f), FullExitThreshold);
	const float ReducedExitThreshold = FMath::Min(FMath::Clamp(Policy.ReducedExitThreshold, 0.0f, 1.0f), ReducedEnterThreshold);

	// 同一分数在不同原档位使用不同进入、退出阈值，避免阈值附近来回切换。
	EFPEnemyRenderSignificanceTier DesiredTier = NaturalRenderSignificanceTier;
	switch (NaturalRenderSignificanceTier)
	{
	case EFPEnemyRenderSignificanceTier::Full:
		if (Sample.Score < ReducedExitThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Background;
		}
		else if (Sample.Score < FullExitThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Reduced;
		}
		break;

	case EFPEnemyRenderSignificanceTier::Reduced:
		if (Sample.Score >= FullEnterThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Full;
		}
		else if (Sample.Score < ReducedExitThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Background;
		}
		break;

	case EFPEnemyRenderSignificanceTier::Background:
	default:
		if (Sample.Score >= FullEnterThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Full;
		}
		else if (Sample.Score >= ReducedEnterThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Reduced;
		}
		break;
	}

	if (DesiredTier == NaturalRenderSignificanceTier)
	{
		PendingRenderDemotionTier = NaturalRenderSignificanceTier;
		PendingRenderDemotionStartTime = -MAX_flt;
		return NaturalRenderSignificanceTier;
	}

	// 档位已确定发生变化：离开 Full 或进入 Background 是降档，其余变化都是升档。
	const bool bDemoting =
		NaturalRenderSignificanceTier == EFPEnemyRenderSignificanceTier::Full || DesiredTier == EFPEnemyRenderSignificanceTier::Background;
	if (bDemoting)
	{
		if (PendingRenderDemotionTier != DesiredTier || PendingRenderDemotionStartTime < 0.0f)
		{
			PendingRenderDemotionTier = DesiredTier;
			PendingRenderDemotionStartTime = CurrentTime;
		}
		const bool bMinimumHoldElapsed = CurrentTime - LastNaturalRenderTierChangeTime >= FMath::Max(Policy.MinimumTierHoldSeconds, 0.0f);
		const bool bDemotionDelayElapsed = CurrentTime - PendingRenderDemotionStartTime >= FMath::Max(Policy.DemotionDelaySeconds, 0.0f);
		if (!bMinimumHoldElapsed || !bDemotionDelayElapsed)
		{
			return NaturalRenderSignificanceTier;
		}
	}

	// 升档立即走到这里；降档等门禁通过后复用同一提交出口。
	NaturalRenderSignificanceTier = DesiredTier;
	LastNaturalRenderTierChangeTime = CurrentTime;
	PendingRenderDemotionTier = NaturalRenderSignificanceTier;
	PendingRenderDemotionStartTime = -MAX_flt;
	return NaturalRenderSignificanceTier;
}

// 保存协调器已分配的档位与资格，再统一应用组件设置和共享登记，不在这里争抢预算。
void AfpstrueEnemyCharacter::ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier NewTier, bool bShouldCastShadow,
														 bool bShouldBeVisibleInRayTracing, bool bInForceFullAnimationAndLOD,
														 const FFPEnemyRenderSignificancePolicy& Policy)
{
	// Coordinator 在同一轮 Top-K 预算选择后统一提交结果；本对象只把策略转换成 SkeletalMeshComponent 的实际设置。
	if (IsDead())
	{
		return;
	}

	RenderSignificanceTier = Policy.bEnableRenderTiering ? NewTier : EFPEnemyRenderSignificanceTier::Full;
	// 功能关闭时直接归一化为“允许”，下游只消费最终资格，不再重复解释策略开关。
	bRenderShouldCastShadow = !Policy.bEnableShadowBudget || bShouldCastShadow;
	bRenderShouldBeVisibleInRayTracing = !Policy.bEnableRayTracingBudget || bShouldBeVisibleInRayTracing;
	bGameplayAnimationProtection = bInForceFullAnimationAndLOD;
	LastRenderSignificancePolicy = Policy;
	bHasRenderSignificancePolicy = true;
	ApplyRenderSignificanceSettings();
	RefreshAnimationSharingRegistration();
}

// 合成战斗保护、资产原值和渲染策略，是骨骼动画频率及最低 LOD 的统一应用入口。
void AfpstrueEnemyCharacter::ApplyRenderSignificanceSettings()
{
	// 唯一姿态提交入口：服务器消费自身Gameplay档位，客户端消费本地Render档位。
	// 这里只测GT策略应用，不冒充动画求值、骨骼写物理或GPU执行时间。
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemy_ApplyAnimationPolicy);
	DisableEnemyPostProcessAnimation();
	if (IsDead()) return;
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr) return;

	const bool bNetworkAuthority = HasAuthority() && GetNetMode() != NM_Standalone;
	const bool bHitProtected = GetWorld() != nullptr && GetWorld()->GetTimeSeconds() < HitAnimationProtectionUntil;
	// 攻击/受击保护恢复必要姿态，不增加阴影或光追名额；相机不能改变权威姿态策略。
	const bool bProtected = bNetworkAuthority ? RequiresFullAuthorityPose()
		: IsAttacking() || bGameplayAnimationProtection || bHitProtected;
	const bool bRefreshFullPose = bProtected || bDisableAnimationOptimizationsForBenchmark;
	float AnimationTickInterval = 0.0f;
	int32 RequestedMinLOD = 0;

	if (!bRefreshFullPose)
	{
		if (bNetworkAuthority)
		{
			// Simple的命中体由根变换驱动，不再为了被射击而保持全身物理骨骼。
			// Reference由RequiresFullAuthorityPose锁定完整更新，维持同二进制A/B参考。
			AnimationTickInterval = GameplayTierValue(SignificanceTier, 0.0f, MidRateAnimationTickInterval, FarRateAnimationTickInterval);
			RequestedMinLOD = static_cast<int32>(SignificanceTier);
		}
		else if (bHasRenderSignificancePolicy && LastRenderSignificancePolicy.bEnableRenderTiering)
		{
			switch (RenderSignificanceTier)
			{
			case EFPEnemyRenderSignificanceTier::Reduced:
				AnimationTickInterval = MidRateAnimationTickInterval;
				RequestedMinLOD = LastRenderSignificancePolicy.ReducedMinLOD;
				break;
			case EFPEnemyRenderSignificanceTier::Background:
				AnimationTickInterval = FarRateAnimationTickInterval;
				RequestedMinLOD = LastRenderSignificancePolicy.BackgroundMinLOD;
				break;
			default:
				RequestedMinLOD = LastRenderSignificancePolicy.FullMinLOD;
				break;
			}
		}
	}

	CharacterMesh->VisibilityBasedAnimTickOption = bRefreshFullPose
		? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones : AuthoredAnimTickOption;
	CharacterMesh->bEnableUpdateRateOptimizations = bAuthoredUpdateRateOptimizations && !bRefreshFullPose;
	const float TargetInterval = bRefreshFullPose || (!bNetworkAuthority && !LastRenderSignificancePolicy.bEnableAnimationTickTiering)
		? 0.0f : AnimationTickInterval;
	// 取消已有冷却才能让战斗提升在下一可用帧真正执行；无变化不重复提交Tick设置。
	if (TargetInterval == 0.0f && CharacterMesh->GetComponentTickInterval() > 0.0f)
		CharacterMesh->SetComponentTickIntervalAndCooldown(0.0f);
	else if (!FMath::IsNearlyEqual(CharacterMesh->GetComponentTickInterval(), TargetInterval))
		CharacterMesh->SetComponentTickInterval(TargetInterval);

	if (!bNetworkAuthority && !LastRenderSignificancePolicy.bEnableSkeletalLOD) RequestedMinLOD = 0;
	// MinLOD只约束引擎可选范围，零值解除限制，不是强制每个GPU Pass绘制LOD0。
	const int32 SafeMinLOD = FMath::Clamp(RequestedMinLOD, 0, FMath::Max(CharacterMesh->GetNumLODs(), 1) - 1);
	if ((bHasRenderSignificancePolicy || bRefreshFullPose || bNetworkAuthority) && AppliedMinimumLOD != SafeMinLOD)
	{
		CharacterMesh->OverrideMinLOD(SafeMinLOD);
		AppliedMinimumLOD = SafeMinLOD;
	}
	ApplyRenderBudgetMeshFlags();
}

// 刷新本 Actor 和 ChildActor 拥有的 Mesh 缓存；保留资产原始资格，离开集合时恢复原值。
void AfpstrueEnemyCharacter::RefreshRenderBudgetMeshes()
{
	TInlineComponentArray<UMeshComponent*, 8> CurrentMeshes;
	// 按所有权收集：包括本 Actor 和 ChildActor 组件中的 Mesh；不自动接管外部 Actor。
	// 普通 Attach 不转移所有权，否则两名敌人交换挂件时会互相覆盖缓存的原始资格。
	// ★【重要边界】[资源所有权] 只管理本 Actor/ChildActor 拥有的 Mesh，保存原始许可；外部 Actor 仅 Attach 不自动进入该敌人的预算。
	GetComponents(CurrentMeshes, true);
	// 屏幕 UI 不参与世界网格的阴影/光追名额，不改变既有网格预算统计。
	CurrentMeshes.Remove(HealthBar.Get());
	RenderBudgetMeshes.RemoveAllSwap(
		[&CurrentMeshes](const FRenderBudgetMesh& Entry)
		{
			UMeshComponent* BudgetMesh = Entry.Mesh.Get();
			if (BudgetMesh == nullptr)
			{
				return true;
			}
			if (!CurrentMeshes.Contains(BudgetMesh))
			{
				// 组件从所属集合移除后交回原配置；销毁组件的弱引用在上方直接清除。
				BudgetMesh->SetCastShadow(Entry.bAuthoredShadow);
				BudgetMesh->SetVisibleInRayTracing(Entry.bAuthoredRayTracing);
				return true;
			}
			return false;
		},
		EAllowShrinking::No);
	// 仅新加入集合的组件记录原始资格，已有组件不能把上一轮预算结果误当作资产默认值。
	for (UMeshComponent* BudgetMesh : CurrentMeshes)
	{
		if (IsValid(BudgetMesh) && !RenderBudgetMeshes.ContainsByPredicate([BudgetMesh](const FRenderBudgetMesh& Entry)
																		   { return Entry.Mesh.Get() == BudgetMesh; }))
		{
			RenderBudgetMeshes.Add({BudgetMesh, bool(BudgetMesh->CastShadow), bool(BudgetMesh->bVisibleInRayTracing)});
		}
	}
	ApplyRenderBudgetMeshFlags();
}

// 将存活、诊断和预算条件与资产原始许可取交集；只有标志变化才更新渲染状态。
void AfpstrueEnemyCharacter::ApplyRenderBudgetMeshFlags()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemy_ApplyMeshRenderFlagsCPU);
	// 只控制受管 Mesh 的阴影及硬件光追参与（包括骨骼 BLAS 更新链），不改变普通光栅可见性。
	const bool bAlive = !bDeathEffectsApplied && !IsDead();
	const bool bAllowShadow =
		bAlive && !bDisableEnemyShadowsForBenchmark && (!bHasRenderSignificancePolicy || bRenderShouldCastShadow);
	const bool bAllowRayTracing =
		bAlive && !bDisableEnemyRayTracingForBenchmark && (!bHasRenderSignificancePolicy || bRenderShouldBeVisibleInRayTracing);
	for (const FRenderBudgetMesh& Entry : RenderBudgetMeshes)
	{
		if (UMeshComponent* BudgetMesh = Entry.Mesh.Get())
		{
			const bool bShadow = bAllowShadow && Entry.bAuthoredShadow;
			const bool bRayTracing = bAllowRayTracing && Entry.bAuthoredRayTracing;
			if (BudgetMesh->CastShadow != bShadow)
			{
				BudgetMesh->SetCastShadow(bShadow);
			}
			if (BudgetMesh->bVisibleInRayTracing != bRayTracing)
			{
				BudgetMesh->SetVisibleInRayTracing(bRayTracing);
			}
		}
	}
}

// 读回缓存中有效组件的当前标志，用于统计实际消费者数量，不表示 GPU 执行耗时。
void AfpstrueEnemyCharacter::GetRenderBudgetMeshCounts(int32& OutMeshes, int32& OutShadowMeshes, int32& OutRayTracingMeshes) const
{
	OutMeshes = OutShadowMeshes = OutRayTracingMeshes = 0;
	for (const FRenderBudgetMesh& Entry : RenderBudgetMeshes)
	{
		if (const UMeshComponent* BudgetMesh = Entry.Mesh.Get())
		{
			++OutMeshes;
			OutShadowMeshes += BudgetMesh->CastShadow ? 1 : 0;
			OutRayTracingMeshes += BudgetMesh->bVisibleInRayTracing ? 1 : 0;
		}
	}
}

// 从原始资产许可生成新一轮候选资格，避免上轮被关闭的对象永久失去重新入选机会。
void AfpstrueEnemyCharacter::GetRenderBudgetEligibility(bool& bOutShadow, bool& bOutRayTracing) const
{
	bOutShadow = bOutRayTracing = false;
	if (IsDead())
	{
		return;
	}
	for (const FRenderBudgetMesh& Entry : RenderBudgetMeshes)
	{
		if (Entry.Mesh.IsValid())
		{
			bOutShadow |= Entry.bAuthoredShadow && !bDisableEnemyShadowsForBenchmark;
			bOutRayTracing |= Entry.bAuthoredRayTracing && !bDisableEnemyRayTracingForBenchmark;
		}
	}
}

// ==================== Animation Sharing 接入桥 ====================

// 只有低渲染档、非战斗保护且不模拟物理的存活敌人可复用共享姿态。
bool AfpstrueEnemyCharacter::CanUseAnimationSharing() const
{
	// 只有低渲染档、非战斗保护、非死亡且未模拟物理的敌人才可成为 Follower；攻击/布娃娃必须使用独立姿态。
	// 联网权威端保留独立的攻击姿态、RootMotion与武器Socket；Simple活体命中虽已独立，不能因此共享玩法动作。
	if ((HasAuthority() && GetNetMode() != NM_Standalone) || IsDead() || !bHasRenderSignificancePolicy || bDisableAnimationOptimizationsForBenchmark ||
		RenderSignificanceTier == EFPEnemyRenderSignificanceTier::Full ||
		RequiresGameplayAnimationProtection(GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0f,
											LastRenderSignificancePolicy.CombatPriorityGraceSeconds))
	{
		return false;
	}

	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	return CharacterMesh != nullptr && !CharacterMesh->IsSimulatingPhysics();
}

// 通过弱引用请求共享协调器检查资格，角色不直接修改插件句柄数组。
void AfpstrueEnemyCharacter::RefreshAnimationSharingRegistration()
{
	// 语法复习：弱指针可能在不通知观察者的情况下失效，先 Get() 成局部裸指针再使用。
	if (UfpstrueEnemyAnimationSharingCoordinator* Coordinator = AnimationSharingCoordinator.Get())
	{
		Coordinator->RefreshEnemyRegistration(this);
	}
	DisableEnemyPostProcessAnimation();
}

// 接收 GameMode 注入的共享协作者，仅保存弱引用，不延长其 World 生命周期。
void AfpstrueEnemyCharacter::SetAnimationSharingCoordinator(UfpstrueEnemyAnimationSharingCoordinator* InCoordinator)
{
	// 此处已 include 协调器完整类型，编译器可以验证它能安全转换为 UObject 弱引用。
	AnimationSharingCoordinator = InCoordinator;
}

// 攻击、受击、死亡和退出前立即解除共享姿态，由协调器执行幂等注销。
void AfpstrueEnemyCharacter::SuspendAnimationSharing()
{
	// 攻击、受击、死亡或退出时强制解除 Follower；Coordinator 负责实际注销句柄。
	if (UfpstrueEnemyAnimationSharingCoordinator* Coordinator = AnimationSharingCoordinator.Get())
	{
		Coordinator->SuspendEnemy(this);
	}
	DisableEnemyPostProcessAnimation();
}

// ==================== Benchmark 诊断开关 ====================

// 应用拆分成本所需的破坏性诊断开关；这些关闭项不是正式玩法的性能配置。
void AfpstrueEnemyCharacter::ApplyBenchmarkDiagnosticOverrides(bool bDisableAttackSweep, bool bDisablePawnCollision,
															   bool bDisableCharacterMovementTick)
{
	// 破坏性关闭只用于定位消费者成本上界；正式160敌人基线不会传入这些参数。
	if (CombatComponent != nullptr)
	{
		CombatComponent->SetAttackSweepDisabledForBenchmark(bDisableAttackSweep);
	}

	if (bDisablePawnCollision)
	{
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetComponentTickEnabled(!bDisableCharacterMovementTick);
	}
}
