// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Shared/fpstrueFootIK.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
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
 * Gameplay Significance 只影响决策/移动节奏；Render Significance 只影响骨骼、动画、阴影、RT 和动画共享。
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
	}

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
	CombatComponent = CreateDefaultSubobject<UfpstrueEnemyCombatComponent>(TEXT("CombatComponent"));
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

// 初始化碰撞、移动与性能策略，绑定生命组件通知；角色本身不承担逐帧 AI 决策。
void AfpstrueEnemyCharacter::BeginPlay()
{
	//初始化顺序：读取Benchmark覆盖 -> 配置Mesh/Movement -> 绑定 Health -> 注册 Gameplay Significance。
	Super::BeginPlay();
	UfpstrueFootIKAnimInstance::Install(this, true);
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

		//默认网格仅查询，忽略pawn；移动碰撞由胶囊体完成；WeaponTrace通道 Block
		CharacterMesh->SetSimulatePhysics(false);
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		// 射击绕过移动胶囊并命中 Physics Asset，HitResult 才能携带用于部位伤害的 BoneName。
		CharacterMesh->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	}
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
	if (!HasAuthority()) OnRep_NetworkPresentation();
}

void AfpstrueEnemyCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstrueEnemyCharacter, NetworkPresentation);
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
		CharacterMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetEnableGravity(true);
		CharacterMesh->SetSimulatePhysics(true);
	}

	// 内部死亡状态提交后再通知外部；蓝图新增的 Mesh 同样撤销阴影及光追资格。
	OnEnemyDeathReported.Broadcast(this);
	OnEnemyDied();
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

	CharacterMesh->AddImpulseAtLocation(ImpulseDirection * FMath::Clamp(DeathImpulseStrength, 0.0f, 15000.0f), DeathDamageContext.Location,
										DeathDamageContext.BoneName);
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
	// 档位未变化立即返回；变化时一次性下发 Movement Tick 间隔和 AI 决策倍率。
	if (SignificanceTier == NewTier)
	{
		return;
	}

	SignificanceTier = NewTier;
	ApplyGameplaySignificanceIntervals();

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

// ==================== Render Significance：可见性、相机距离与渲染预算 ====================

// 根据统一相机快照计算渲染重要性；使用 Bounds 近似视锥和屏占比，不是硬件遮挡查询。
FFPEnemyRenderSignificanceSample AfpstrueEnemyCharacter::EvaluateRenderSignificance(const FFPEnemyRenderViewContext& ViewContext,
																					const FFPEnemyRenderSignificancePolicy& Policy)
{
	// 这里只采样并返回纯渲染数据，不改 AI 状态；预算排序与最终应用由 Coordinator 在全体候选收集后完成。
	FFPEnemyRenderSignificanceSample Sample;
	if (IsDead())
	{
		return Sample;
	}

	// 将包围球转换到相机坐标，估算屏幕中心位置及半径，保留边缘部分可见的候选。
	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	const FVector BoundsOrigin = CharacterMesh != nullptr ? CharacterMesh->Bounds.Origin : GetActorLocation();
	const float BoundsRadius = CharacterMesh != nullptr ? FMath::Max(CharacterMesh->Bounds.SphereRadius, 1.0f) : 100.0f;

	const FVector ToEnemy = BoundsOrigin - ViewContext.ViewLocation;
	Sample.Distance = ToEnemy.Size();

	const FRotationMatrix ViewRotationMatrix(ViewContext.ViewRotation);
	const float ForwardDistance = FVector::DotProduct(ToEnemy, ViewRotationMatrix.GetUnitAxis(EAxis::X));
	const float HorizontalDistance = FVector::DotProduct(ToEnemy, ViewRotationMatrix.GetUnitAxis(EAxis::Y));
	const float VerticalDistance = FVector::DotProduct(ToEnemy, ViewRotationMatrix.GetUnitAxis(EAxis::Z));

	const float HalfHorizontalFOVRadians = FMath::DegreesToRadians(FMath::Clamp(ViewContext.HorizontalFOVDegrees, 5.0f, 170.0f) * 0.5f);
	const float HorizontalTangent = FMath::Max(FMath::Tan(HalfHorizontalFOVRadians), 0.01f);
	const float VerticalTangent = HorizontalTangent / FMath::Max(ViewContext.AspectRatio, 0.1f);
	const float SafeForwardDistance = FMath::Max(ForwardDistance, 1.0f);
	const float ProjectedHorizontalRadius = BoundsRadius / (SafeForwardDistance * HorizontalTangent);
	const float ProjectedVerticalRadius = BoundsRadius / (SafeForwardDistance * VerticalTangent);
	const float NormalizedHorizontalPosition = HorizontalDistance / (SafeForwardDistance * HorizontalTangent);
	const float NormalizedVerticalPosition = VerticalDistance / (SafeForwardDistance * VerticalTangent);

	// 主视锥给予更高优先级；扩展视锥作为镜头边缘缓冲，不检查墙体是否遮住敌人。
	const bool bBoundsInFront = ForwardDistance + BoundsRadius > 0.0f;
	// 用归一化屏幕范围加包围球半径检测相交；Margin 控制额外的镜头边缘宽限。
	const auto IntersectsFrustum = [bBoundsInFront, NormalizedHorizontalPosition, NormalizedVerticalPosition, ProjectedHorizontalRadius,
									ProjectedVerticalRadius](float Margin)
	{
		return bBoundsInFront && FMath::Abs(NormalizedHorizontalPosition) <= 1.0f + Margin + ProjectedHorizontalRadius &&
			   FMath::Abs(NormalizedVerticalPosition) <= 1.0f + Margin + ProjectedVerticalRadius;
	};

	Sample.bInPrimaryFrustum = IntersectsFrustum(0.0f);
	Sample.bInExpandedFrustum = IntersectsFrustum(FMath::Max(Policy.ExpandedFrustumMargin, 0.0f));
	Sample.FrustumFactor = Sample.bInPrimaryFrustum ? 1.0f : (Sample.bInExpandedFrustum ? 0.5f : 0.0f);

	// 刚离开主视锥的敌人短暂保留部分分数，降低转镜时的档位抖动。
	const float RecentGraceSeconds = FMath::Max(Policy.RecentFrustumGraceSeconds, 0.0f);
	if (Sample.bInPrimaryFrustum)
	{
		LastPrimaryFrustumTime = ViewContext.TimeSeconds;
		Sample.RecentFrustumFactor = 1.0f;
	}
	else if (RecentGraceSeconds > 0.0f)
	{
		const float TimeSincePrimaryFrustum = ViewContext.TimeSeconds - LastPrimaryFrustumTime;
		Sample.RecentFrustumFactor = 1.0f - FMath::Clamp(TimeSincePrimaryFrustum / RecentGraceSeconds, 0.0f, 1.0f);
	}

	// 屏占比只奖励扩展视锥中的对象；距离独立平滑衰减，各输入最终限制到零至一。
	const float ProjectedScreenRadius = ForwardDistance > 0.0f ? FMath::Max(ProjectedHorizontalRadius, ProjectedVerticalRadius) : 0.0f;
	Sample.ScreenCoverageFactor =
		Sample.bInExpandedFrustum ? FMath::Clamp(ProjectedScreenRadius / FMath::Max(Policy.ScreenRadiusForFullScore, 0.001f), 0.0f, 1.0f)
								  : 0.0f;

	const float NearDistance = FMath::Max(Policy.NearDistance, 0.0f);
	const float FarDistance = FMath::Max(Policy.FarDistance, NearDistance + 1.0f);
	Sample.DistanceFactor = 1.0f - FMath::Clamp((Sample.Distance - NearDistance) / (FarDistance - NearDistance), 0.0f, 1.0f);

	// 权重归一化后合成纯渲染分数，攻击或受击不会提高这个分数和预算排序。
	const float FrustumWeight = FMath::Max(Policy.FrustumWeight, 0.0f);
	const float ScreenCoverageWeight = FMath::Max(Policy.ScreenCoverageWeight, 0.0f);
	const float RecentFrustumWeight = FMath::Max(Policy.RecentFrustumWeight, 0.0f);
	const float DistanceWeight = FMath::Max(Policy.DistanceWeight, 0.0f);
	const float WeightSum = FMath::Max(FrustumWeight + ScreenCoverageWeight + RecentFrustumWeight + DistanceWeight, KINDA_SMALL_NUMBER);
	Sample.Score = FMath::Clamp((Sample.FrustumFactor * FrustumWeight + Sample.ScreenCoverageFactor * ScreenCoverageWeight +
								 Sample.RecentFrustumFactor * RecentFrustumWeight + Sample.DistanceFactor * DistanceWeight) /
									WeightSum,
								0.0f, 1.0f);

	RenderSignificanceScore = Sample.Score;
	return Sample;
}

// 根据分数和上一自然档位做滞回判断；自然档位不代表一定能拿到全局 Full 名额。
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
	// 仅测 GT 策略设置/组件提交，不把此处当成主动画求值、骨骼写物理或 GPU 执行。
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemy_ApplyAnimationPolicy);
	// 唯一动画设置入口：合成攻击/受击保护、资产默认值和预算策略。
	// MinLOD=0 只解除最低精度限制，不强制实际选择 LOD0；命中骨骼仍须在资产中保留。
	if (IsDead())
	{
		return;
	}

	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr)
	{
		return;
	}

	// 先确定是否保护当前动作，再决定能否降频；恢复的是必要姿态，不额外开启阴影或光追。
	float AnimationTickInterval = 0.0f;
	int32 RequestedMinLOD = LastRenderSignificancePolicy.FullMinLOD;
	const bool bHitProtected = GetWorld() != nullptr && GetWorld()->GetTimeSeconds() < HitAnimationProtectionUntil;
	// ★【重要边界】[预算保护] 攻击和受击只恢复必要动画与 LOD，不能提高渲染分数或额外获得阴影、硬件光追名额。
	const bool bUseFullAnimationAndLOD = IsAttacking() || bGameplayAnimationProtection || bHitProtected;
	// 联网权威端即使没有画面，也必须刷新物理命中形状；不能让客户端看见跑动、服务器却查询旧骨骼。
	const bool bNetworkAuthority = HasAuthority() && GetNetMode() != NM_Standalone;
	const bool bIndependentFootIK = RenderSignificanceTier == EFPEnemyRenderSignificanceTier::Full
		&& Cast<UfpstrueFootIKAnimInstance>(CharacterMesh->GetPostProcessInstance()) != nullptr;
	const bool bRefreshCombatPose = bUseFullAnimationAndLOD || bDisableAnimationOptimizationsForBenchmark || bNetworkAuthority || bIndependentFootIK;
	CharacterMesh->VisibilityBasedAnimTickOption = bRefreshCombatPose
		? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones : AuthoredAnimTickOption;
	CharacterMesh->bEnableUpdateRateOptimizations = bAuthoredUpdateRateOptimizations && !bRefreshCombatPose;
	if (!bUseFullAnimationAndLOD && bHasRenderSignificancePolicy && LastRenderSignificancePolicy.bEnableRenderTiering)
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

		case EFPEnemyRenderSignificanceTier::Full:
		default:
			break;
		}
	}

	// 完整动画恢复要同时撤销组件已有冷却，不能只改下一次 Tick 的间隔。
	const float TargetInterval = bNetworkAuthority || bDisableAnimationOptimizationsForBenchmark || !LastRenderSignificancePolicy.bEnableAnimationTickTiering
		? 0.0f : AnimationTickInterval;
	if (TargetInterval == 0.0f && CharacterMesh->GetComponentTickInterval() > 0.0f)
	{
		// 仅改 TickInterval 不会撤销已排入的冷却；战斗提升要让下一可用帧真正更新。
		CharacterMesh->SetComponentTickIntervalAndCooldown(0.0f);
	}
	else
	{
		CharacterMesh->SetComponentTickInterval(TargetInterval);
	}

	// MinLOD 仅限制可选择的精度范围；零值解除最低 LOD 限制，不强制 GPU 绘制 LOD0。
	// ★【重要边界】[配置契约] MinLOD 是最低精度约束，不是 GPU 实际选择；零值不能解释为每个渲染 Pass 都绘制 LOD0。
	if (bNetworkAuthority || !LastRenderSignificancePolicy.bEnableSkeletalLOD || bUseFullAnimationAndLOD)
	{
		RequestedMinLOD = 0;
	}
	const int32 LODCount = FMath::Max(CharacterMesh->GetNumLODs(), 1);
	const int32 SafeMinLOD = FMath::Clamp(RequestedMinLOD, 0, LODCount - 1);
	if ((bHasRenderSignificancePolicy || bUseFullAnimationAndLOD || bNetworkAuthority) && AppliedMinimumLOD != SafeMinLOD)
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
	// 联网权威端保留自己的命中骨骼，不能以其他敌人的共享姿态替代物理查询形状。
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
