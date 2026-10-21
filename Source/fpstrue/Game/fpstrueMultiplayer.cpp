// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstrueMultiplayer.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Engine/PendingNetGame.h"
#include "GameFramework/GameSession.h"
#include "HAL/FileManager.h"
#include "IPlatformCrypto.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlatformCryptoTypes.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavigationData.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Canvas.h"
#include "Engine/GameViewportClient.h"
#include "EngineGlobals.h"
#include "Misc/App.h"
#include "UnrealClient.h"
#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"

/*
 * 联机装配链：GameInstance 发起旅行 -> Controller 握手 -> GameMode 恢复/生成 Pawn -> 本地等待复制就绪。
 * GameMode 独占重生、重连保留表和 PvE 登记；PlayerState/GameState 复制计分与对局事实；HUD 只订阅显示。
 * 所有连接状态、Timer 和 RPC 处理都在 Game Thread；这里不执行 AI 决策、伤害计算或 Worker 动画求值。
 * 服务端保留的是真实 Pawn/PlayerState，不是客户端上传的快照；客户端缓存仅保存地址和不透明票据。
 */

namespace
{
constexpr int32 MaxRejoinSlots = 16;
constexpr int32 MaxPendingHandshakes = 8;
constexpr int32 MaxPlayerSpawnAttempts = 4;

// 回收登记前先核对真实持有者；死亡掉落枪由自己的有界寿命回收，不因重生/重连提前消失。
void ReleaseNetworkPlayerWeapon(AActor* Weapon, AfpstrueCharacter* ExpectedPawn,
	APlayerController* ExpectedController = nullptr, bool bDestroyDropped = false)
{
	if (!IsValid(Weapon)) return;
	UfpstrueWeaponComponent* Component = Weapon->FindComponentByClass<UfpstrueWeaponComponent>();
	AfpstrueCharacter* Holder = Component != nullptr ? Component->GetOwningCharacter() : nullptr;
	if (Holder != nullptr && Holder != ExpectedPawn &&
		(ExpectedController == nullptr || Holder->GetController() != ExpectedController)) return;
	if (Component != nullptr && Component->IsDroppedOnDeath())
	{
		// ★【资源所有权】只释放匹配死 Pawn 的装备槽；终态卸下只清登记，不重置已经开始的掉落物理。
		if (IsValid(ExpectedPawn) && ExpectedPawn->IsDead() && ExpectedPawn->GetEquippedWeaponComponent() == Component)
			ExpectedPawn->ClearEquippedWeaponComponent(Component);
		if (!bDestroyDropped) return;
	}
	Weapon->Destroy();
}

// 为恢复日志读取引擎网络身份；仅服务器可以分配 GUID，客户端只查询已知映射。
uint64 GetResumeNetGuid(const AActor* Actor)
{
	UNetDriver* Driver = IsValid(Actor) ? Actor->GetNetDriver() : nullptr;
	if (Driver == nullptr || !Driver->GuidCache.IsValid()) return 0;
	return (Actor->HasAuthority() ? Driver->GuidCache->GetOrAssignNetGUID(const_cast<AActor*>(Actor))
		: Driver->GuidCache->GetNetGUID(Actor)).ObjectId;
}

// 只校验票据编码格式；持有权、有效期和是否已占用仍由服务器保留表判断。
bool IsResumeToken(const FString& Token)
{
	if (Token.Len() != 64) return false;
	for (TCHAR Character : Token) if (!FChar::IsHexDigit(Character)) return false;
	return true;
}

// 使用平台安全随机源生成 256 位不透明票据；随机源失败时返回空串，不生成可预测替代品。
FString GenerateResumeToken()
{
	const TUniquePtr<FEncryptionContext> Crypto = IPlatformCrypto::Get().CreateContext();
	uint8 Bytes[32];
	if (!Crypto.IsValid() || Crypto->CreateRandomBytes(MakeArrayView(Bytes)) != EPlatformCryptoResult::Success) return FString();
	return BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes));
}
}

// 只接收已提交的显示快照，数值无效时安全回退；不拥有武器、Pawn 或另一份 Gameplay 状态。
void UfpstruePvpIngameWidget::ApplySnapshot(float Health, float MaxHealth, bool bEquipped,
	int32 Ammo, int32 Reserve, const FText& WeaponName)
{
	const float Fraction = FMath::IsFinite(Health) && FMath::IsFinite(MaxHealth) && MaxHealth > 0.0f
		? FMath::Clamp(Health / MaxHealth, 0.0f, 1.0f) : 0.0f;
	const int32 Magazine = bEquipped ? FMath::Max(0, Ammo) : 0;
	const int32 ReserveCount = bEquipped ? FMath::Max(0, Reserve) : 0;
	const FText Name = bEquipped ? WeaponName : FText::GetEmpty();
	if (HealthFraction == Fraction && bHasEquipment == bEquipped && CurrentAmmo == Magazine
		&& ReserveAmmo == ReserveCount && CurrentWeaponName.EqualTo(Name)) return;
	HealthFraction = Fraction;
	bHasEquipment = bEquipped;
	CurrentAmmo = Magazine;
	ReserveAmmo = ReserveCount;
	CurrentWeaponName = Name;
	if (++SnapshotRevision == 0) ++SnapshotRevision;
	RefreshControls();
	InvalidateLayoutAndVolatility();
}

// 保留原布局、字体和动画；控件名称是已经审查的 ingame 资产合同，不重建另一套界面。
void UfpstruePvpIngameWidget::ResolveAuthoredControls()
{
	HealthBar = Cast<UProgressBar>(GetWidgetFromName(TEXT("血量组件")));
	MagazineText = Cast<UTextBlock>(GetWidgetFromName(TEXT("当前子弹")));
	ReserveText = Cast<UTextBlock>(GetWidgetFromName(TEXT("剩余子弹")));
	WeaponNameText = Cast<UTextBlock>(GetWidgetFromName(TEXT("TextBlock_2")));
	MatchHintText = Cast<UTextBlock>(GetWidgetFromName(TEXT("TextBlock_65")));
	AmmoContainer = GetWidgetFromName(TEXT("子弹数"));
	CountdownRow = GetWidgetFromName(TEXT("HorizontalBox_327"));
}

// 不增加逐帧业务更新；只有事件与构建时写入控件，UMG 自身的绘制和淡入动画仍由引擎执行。
void UfpstruePvpIngameWidget::RefreshControls()
{
	if (HealthBar != nullptr)
	{
		HealthBar->SetPercent(HealthFraction);
		HealthBar->SetFillColorAndOpacity(HealthFraction <= 0.3f
			? FLinearColor(0.85f, 0.1f, 0.08f, 0.85f) : FLinearColor(0.104616f, 0.296138f, 0.158961f, 0.701961f));
	}
	if (MagazineText != nullptr)
	{
		MagazineText->SetText(FText::AsNumber(CurrentAmmo));
		MagazineText->SetColorAndOpacity(CurrentAmmo <= 5 ? FLinearColor(0.9f, 0.15f, 0.1f)
			: FLinearColor(0.651406f, 0.617207f, 0.473532f));
	}
	if (ReserveText != nullptr) ReserveText->SetText(FText::AsNumber(ReserveAmmo));
	if (WeaponNameText != nullptr) WeaponNameText->SetText(CurrentWeaponName);
	if (AmmoContainer != nullptr) AmmoContainer->SetVisibility(bHasEquipment ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	// PvPvE 没有单机生存倒计时；隐藏时间行而非从客户端查询不存在的单机 GameMode。
	if (CountdownRow != nullptr) CountdownRow->SetVisibility(ESlateVisibility::Collapsed);
	if (MatchHintText != nullptr) MatchHintText->SetText(NSLOCTEXT("fpstrue", "PvpSurvivalHint", "联机生存：抵御敌人，玩家之间可互相伤害"));
}

// 检查原布局约定的控件是否全部接通，供 HUD 选择 UMG 或 Canvas 回退。
bool UfpstruePvpIngameWidget::HasAuthoredControls() const
{
	return HealthBar != nullptr && MagazineText != nullptr && ReserveText != nullptr && WeaponNameText != nullptr
		&& MatchHintText != nullptr && AmmoContainer != nullptr && CountdownRow != nullptr;
}

// 原 Construct 业务链和属性绑定由资产接线工具停用；这里仅恢复同一布局的显示缓存与作者淡入动画。
void UfpstruePvpIngameWidget::NativeConstruct()
{
	Super::NativeConstruct();
	ResolveAuthoredControls();
	RefreshControls();
	if (!bPlayedFadeIn)
	{
		if (const auto* BlueprintClass = Cast<UWidgetBlueprintGeneratedClass>(GetClass()))
			for (const auto& Animation : BlueprintClass->Animations)
				if (Animation != nullptr && Animation->GetName().StartsWith(TEXT("淡入")))
				{
					PlayAnimation(Animation);
					bPlayedFadeIn = true;
					break;
				}
	}
}

// 保留原生绘制，并记下实际画出的快照版本；不在绘制回调内更新玩法数值。
int32 UfpstruePvpIngameWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Result = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	LastPaintFrame = GFrameCounter;
	LastPaintedSnapshotRevision = SnapshotRevision;
	return Result;
}

// 当前快照至少真实绘制过一次才算恢复；允许 Slate 复用未变化的绘制缓存。
bool UfpstruePvpIngameWidget::HasPaintedSnapshot() const
{
	return IsInViewport() && IsVisible() && HasAuthoredControls() && LastPaintFrame > 0
		&& LastPaintedSnapshotRevision == SnapshotRevision;
}

// 选择原 HUD 的联网布局副本，不改动单机界面资产。
AfpstruePvpHUD::AfpstruePvpHUD()
{
	IngameWidgetClass = FSoftObjectPath(TEXT("/Game/FirstPerson/UI/ingame_network.ingame_network_C"));
}

// 原生 Controller 在权威占有和客户端 OnRep_Pawn 时均广播；HUD 晚创建时补读一次当前 Pawn。
void AfpstruePvpHUD::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (PlayerOwner != nullptr)
	{
		PlayerOwner->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandlePossessedPawnChanged);
		BindPawn(Cast<AfpstrueCharacter>(PlayerOwner->GetPawn()));
	}
}

// 占有或 OnRep_Pawn 变化统一转到重新绑定入口。
void AfpstruePvpHUD::HandlePossessedPawnChanged(APawn*, APawn* NewPawn)
{
	BindPawn(Cast<AfpstrueCharacter>(NewPawn));
}

// 本地视口才创建原 UMG；父类/布局合同不符时明确回退 Canvas，不把两套 HUD 同时叠在画面上。
bool AfpstruePvpHUD::EnsureIngameWidget()
{
	if (IngameWidget != nullptr) return IngameWidget->IsInViewport();
	if (bAttemptedIngameWidget || PlayerOwner == nullptr || !PlayerOwner->IsLocalController() || !FApp::CanEverRender()) return false;
	bAttemptedIngameWidget = true;
	UClass* WidgetClass = IngameWidgetClass.LoadSynchronous();
	if (WidgetClass != nullptr) IngameWidget = CreateWidget<UfpstruePvpIngameWidget>(PlayerOwner, WidgetClass);
	if (IngameWidget != nullptr)
	{
		RefreshIngameSnapshot();
		IngameWidget->AddToViewport();
		if (IngameWidget->IsInViewport() && IngameWidget->HasAuthoredControls()) return true;
		IngameWidget->RemoveFromParent();
		IngameWidget = nullptr;
	}
	UE_LOG(LogTemp, Warning, TEXT("PvP original ingame HUD is unavailable or not connected to its native bridge; using Canvas fallback."));
	return false;
}

// 把 HUD 已缓存的生命、装备和弹药一次推给 UMG，不再次查询世界。
void AfpstruePvpHUD::RefreshIngameSnapshot()
{
	if (IngameWidget != nullptr)
		IngameWidget->ApplySnapshot(CurrentHealth, CurrentMaxHealth, ObservedWeapon.IsValid(), CurrentAmmo, ReserveAmmo, CurrentWeaponName);
}

// HUD 的绘制仍逐帧执行；生命与弹药只在事件或 Pawn 切换时更新缓存，不逐帧搜索世界。
void AfpstruePvpHUD::DrawHUD()
{
	Super::DrawHUD();
	auto* Pawn = ObservedPawn.Get();
	if (Pawn == nullptr || !bShowHUD)
	{
		if (IngameWidget != nullptr) IngameWidget->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	if (EnsureIngameWidget())
	{
		IngameWidget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		return;
	}
	if (Canvas == nullptr) return;
	const auto* State = Pawn->GetPlayerState<AfpstruePvpPlayerState>();
	DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.72f), 20.0f, Canvas->SizeY - 92.0f, 470.0f, 72.0f);
	DrawText(FString::Printf(TEXT("HEALTH  %.0f     AMMO  %d / %d"), CurrentHealth, CurrentAmmo, ReserveAmmo),
		FLinearColor::White, 36.0f, Canvas->SizeY - 76.0f, nullptr, 1.25f);
	DrawText(FString::Printf(TEXT("PvPvE     KILLS %d   DEATHS %d"), State != nullptr ? State->GetKills() : 0,
		State != nullptr ? State->GetDeaths() : 0), FLinearColor(0.65f, 0.85f, 1.0f), 36.0f, Canvas->SizeY - 48.0f);
	DrawLine(Canvas->SizeX * 0.5f - 5.0f, Canvas->SizeY * 0.5f, Canvas->SizeX * 0.5f + 5.0f,
		Canvas->SizeY * 0.5f, FLinearColor::White);
	DrawLine(Canvas->SizeX * 0.5f, Canvas->SizeY * 0.5f - 5.0f, Canvas->SizeX * 0.5f,
		Canvas->SizeY * 0.5f + 5.0f, FLinearColor::White);
	LastDrawFrame = GFrameCounter;
}

// ★【恢复边界】先解绑旧 Pawn，再订阅新对象并补读当前值，不依赖断线期间是否收到过通知。
void AfpstruePvpHUD::BindPawn(AfpstrueCharacter* Pawn)
{
	if (auto* Previous = ObservedPawn.Get())
	{
		Previous->OnEquippedWeaponChanged.RemoveDynamic(this, &ThisClass::HandleWeaponChanged);
		if (auto* Health = Previous->GetHealthComponent()) Health->OnHealthChanged.RemoveDynamic(this, &ThisClass::HandleHealthChanged);
	}
	HandleWeaponChanged(nullptr);
	ObservedPawn = Pawn;
	CurrentHealth = 0.0f;
	CurrentMaxHealth = 100.0f;
	LastDrawFrame = 0;
	if (Pawn == nullptr) { RefreshIngameSnapshot(); return; }
	Pawn->OnEquippedWeaponChanged.AddUniqueDynamic(this, &ThisClass::HandleWeaponChanged);
	if (auto* Health = Pawn->GetHealthComponent()) Health->OnHealthChanged.AddUniqueDynamic(this, &ThisClass::HandleHealthChanged);
	HandleHealthChanged(Pawn->GetCurrentHealth());
	HandleWeaponChanged(Pawn->GetEquippedWeaponComponent());
}

// 生命事件只更新本地显示缓存，不参与伤害结算。
void AfpstruePvpHUD::HandleHealthChanged(float Health)
{
	CurrentHealth = Health;
	if (const auto* Pawn = ObservedPawn.Get()) CurrentMaxHealth = Pawn->GetMaxHealth();
	RefreshIngameSnapshot();
}

// 装备可晚于 Pawn 到达；切换订阅后立即补读弹药，不保留失效武器。
void AfpstruePvpHUD::HandleWeaponChanged(UfpstrueWeaponComponent* Weapon)
{
	if (auto* Previous = ObservedWeapon.Get()) Previous->OnAmmoChanged.RemoveDynamic(this, &ThisClass::HandleAmmoChanged);
	ObservedWeapon = Weapon;
	CurrentAmmo = ReserveAmmo = 0;
	CurrentWeaponName = Weapon != nullptr
		? (Weapon->WeaponDisplayName.IsEmpty() ? NSLOCTEXT("fpstrue", "GenericRifleName", "突击步枪") : Weapon->WeaponDisplayName)
		: FText::GetEmpty();
	if (Weapon == nullptr) { RefreshIngameSnapshot(); return; }
	Weapon->OnAmmoChanged.AddUniqueDynamic(this, &ThisClass::HandleAmmoChanged);
	HandleAmmoChanged(Weapon->GetCurrentAmmo(), Weapon->GetMagazineSize(), Weapon->GetReserveAmmo());
}

// 弹药事件携带已提交的数值，HUD 不自行预测或修改武器。
void AfpstruePvpHUD::HandleAmmoChanged(int32 Ammo, int32, int32 Reserve)
{
	CurrentAmmo = Ammo;
	ReserveAmmo = Reserve;
	RefreshIngameSnapshot();
}

// 验收必须见过真实绘制，且所画值仍属于当前 Pawn；仅创建 HUD 不算画面恢复。
bool AfpstruePvpHUD::HasDrawnCurrentState(const AfpstrueCharacter* Pawn) const
{
	const auto* Weapon = ObservedWeapon.Get();
	return bShowHUD && Pawn != nullptr && ObservedPawn.Get() == Pawn
		&& Pawn->GetEquippedWeaponComponent() == Weapon
		&& (IngameWidget != nullptr ? IngameWidget->HasPaintedSnapshot() : LastDrawFrame > 0 && GFrameCounter - LastDrawFrame <= 5)
		&& FMath::IsNearlyEqual(CurrentHealth, Pawn->GetCurrentHealth())
		&& FMath::IsNearlyEqual(CurrentMaxHealth, Pawn->GetMaxHealth())
		&& CurrentAmmo == (Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0)
		&& ReserveAmmo == (Weapon != nullptr ? Weapon->GetReserveAmmo() : 0);
}

// HUD 比 Pawn 晚退出也可安全解除绑定；弱引用不延长旧角色与武器寿命。
void AfpstruePvpHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PlayerOwner != nullptr)
		PlayerOwner->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandlePossessedPawnChanged);
	BindPawn(nullptr);
	if (IngameWidget != nullptr) IngameWidget->RemoveFromParent();
	IngameWidget = nullptr;
	Super::EndPlay(EndPlayReason);
}

// 由服务器邀请拥有者提交票据；票据只走所属 Controller 的 RPC，不进入 Travel URL 或日志。
void AfpstruePvpPlayerController::ClientBeginConnectionHandshake_Implementation()
{
	if (auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance())) Instance->BeginConnectionHandshake(this);
}

// 将拥有者提交的票据交给 GameMode 唯一验证入口；RPC 本身不恢复对象。
void AfpstruePvpPlayerController::ServerSubmitConnectionToken_Implementation(const FString& Token)
{
	if (auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>()) Mode->CompleteConnectionHandshake(this, Token);
}

// 接收服务器轮换后的票据和身份，继续由 GameInstance 等待复制状态就绪。
void AfpstruePvpPlayerController::ClientConnectionAccepted_Implementation(const FString& Token, int32 InResumeId, bool bRestored)
{
	if (auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance())) Instance->AcceptConnectionToken(this, Token, InResumeId, bRestored);
}

// 将服务器拒绝原因交给本地连接流程，决定重试或返回本地世界。
void AfpstruePvpPlayerController::ClientConnectionRejected_Implementation(const FString& Reason)
{
	if (auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance())) Instance->RejectConnection(Reason);
}

// 仅开发验收保存服务器规划的有界路线；输入驱动器随后仍通过正常移动输入执行。
void AfpstruePvpPlayerController::ClientNormalVisualRoute_Implementation(const TArray<FVector>& Points)
{
#if !UE_BUILD_SHIPPING
	if (!IsLocalController() || !FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPairWalk")) || Points.IsEmpty() || Points.Num() > 128) return;
	NormalVisualRoute = Points;
	++NormalVisualRouteRevision;
	UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE CLIENT Points=%d Revision=%d"), NormalVisualRoute.Num(), NormalVisualRouteRevision);
#endif
}

// 路线规划只属于显式开发验收驱动；实际行走和攻击仍走原有 CMC/GAS 协议。
void AfpstruePvpPlayerController::ServerRequestNormalVisualEnemyRoute_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay"))) return;
	auto* Avatar = Cast<AfpstrueCharacter>(GetPawn());
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!Avatar || Avatar->IsDead() || !Avatar->HasEquippedWeapon() || !Navigation) return;
	bNormalFullRouteRequested = true;
	TArray<AfpstruePvpPlayerController*> Ready;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		if (auto* Peer = Cast<AfpstruePvpPlayerController>(It->Get()); Peer && Peer->bNormalFullRouteRequested) Ready.Add(Peer);
	if (Ready.Num() != 2) return; // 双端完成常规动作后才一起出发，不能一端提前杀掉另一端尚未登记的目标。
	AfpstrueEnemyCharacter* Nearest = nullptr;
	double Distance = TNumericLimits<double>::Max();
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
		if (!It->IsDead() && FVector::DistSquared2D(Avatar->GetActorLocation(), It->GetActorLocation()) < Distance)
		{ Nearest = *It; Distance = FVector::DistSquared2D(Avatar->GetActorLocation(), It->GetActorLocation()); }
	if (!Nearest) return;
	// 显式开发验收读回服务器活体 Pawn 与 CMC 的尺寸及实际 NavData，不手工改任何导航/碰撞属性。
	auto LogNavigationCoverage = [Navigation](ACharacter* MovingPawn)
	{
		const auto* Movement = MovingPawn->GetCharacterMovement();
		const auto* Capsule = MovingPawn->GetCapsuleComponent();
		const FNavAgentProperties& PawnProps = MovingPawn->GetNavAgentPropertiesRef();
		const FNavAgentProperties& MovementProps = Movement->GetNavAgentPropertiesRef();
		const ANavigationData* Selected = Navigation->GetNavDataForProps(PawnProps, MovingPawn->GetActorLocation());
		const FNavDataConfig* Config = Selected ? &Selected->GetConfig() : nullptr;
		const float Radius = Capsule->GetScaledCapsuleRadius(), Height = Capsule->GetScaledCapsuleHalfHeight() * 2.0f;
		const bool bCovered = Config && Config->AgentRadius >= Radius && Config->AgentHeight >= Height
			&& PawnProps.AgentRadius >= Radius && PawnProps.AgentHeight >= Height;
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY NAV_PAWN Pawn=%s Capsule=%.1f/%.1f PawnProps=%.1f/%.1f CMCProps=%.1f/%.1f UpdateFromCollision=%d Selected=%s Config=%.1f/%.1f Extent=%s Covered=%d"),
			*MovingPawn->GetName(), Radius, Height, PawnProps.AgentRadius, PawnProps.AgentHeight, MovementProps.AgentRadius, MovementProps.AgentHeight,
			Movement->ShouldUpdateNavAgentWithOwnersCollision(), *GetNameSafe(Selected), Config ? Config->AgentRadius : -1.0f,
			Config ? Config->AgentHeight : -1.0f, Config ? *Config->DefaultQueryExtent.ToCompactString() : TEXT("None"), bCovered);
		return bCovered;
	};
	if (!LogNavigationCoverage(Nearest))
	{ UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE FAIL Reason=EnemyNavAgentCoverage")); return; }
	int32 RouteSlot = 0;
	for (AfpstruePvpPlayerController* Peer : Ready)
	{
		auto* MovingPawn = Cast<AfpstrueCharacter>(Peer->GetPawn());
		if (!MovingPawn || MovingPawn->IsDead()) return;
		if (!LogNavigationCoverage(MovingPawn))
		{ UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE FAIL Reason=PlayerNavAgentCoverage")); return; }
		const FVector Direction = (MovingPawn->GetActorLocation() - Nearest->GetActorLocation()).GetSafeNormal2D().RotateAngleAxis(RouteSlot++ == 0 ? 35.0 : -35.0, FVector::UpVector);
		FNavLocation Destination;
		if (!Navigation->ProjectPointToNavigation(Nearest->GetActorLocation() + Direction * 500.0, Destination, FVector(150,150,200))) return;
		UNavigationPath* Path = Navigation->FindPathToLocationSynchronously(GetWorld(), MovingPawn->GetActorLocation(), Destination.Location, MovingPawn);
		if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.IsEmpty() || Path->PathPoints.Num() > 128)
		{ UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ROUTE_FAIL Player=%s"), *Peer->GetName()); return; }
		Peer->ClientNormalVisualRoute(Path->PathPoints);
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_ROUTE Player=%s Enemy=%s Points=%d DistanceCm=%.1f Teleport=0"),
			*Peer->GetName(), *Nearest->GetName(), Path->PathPoints.Num(), FVector::Dist2D(MovingPawn->GetActorLocation(), Nearest->GetActorLocation()));
	}
#endif
}

// 主动离场先取消恢复资格，再通知拥有者离开；后续 PawnLeavingGame 不再保留角色。
void AfpstruePvpPlayerController::ServerLeavePvp_Implementation()
{
	bIntentionalLeave = true;
	ClientConnectionRejected(TEXT("manual_leave"));
}

// 在引擎销毁 Pawn 前选择保留或释放装备；Logout 只负责余下的控制器登记清理。
void AfpstruePvpPlayerController::PawnLeavingGame()
{
	if (auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>())
	{
		if (!bIntentionalLeave && Mode->PreserveDisconnectedPlayer(this)) return;
		// 主动退出或无法保留时，必须在 Super 销毁 Pawn 之前取得其场景拾取枪并清理。
		// Logout 此时可能已经拿不到 Pawn，不能只依赖默认发枪记录兜底。
		Mode->ReleasePlayerWeapon(this);
	}
	Super::PawnLeavingGame();
}

// 客户端进入 Playing 后启动本控制器的时钟采样；显式开发参数才启动验收观察 Timer。
void AfpstruePvpPlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();
	if (IsLocalController() && GetNetMode() == NM_Client)
	{
		PendingClockSamples.Reset();
		LastClockSampleLocal = -1.0;
		BestClockRoundTrip = TNumericLimits<double>::Max();
		RequestClockSample();
		GetWorldTimerManager().SetTimer(ClockSampleTimer, this, &ThisClass::RequestClockSample, 0.5f, true);
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
			GetWorldTimerManager().SetTimer(SmokeMovementTimer, this, &ThisClass::SampleSmokeMovement, 0.05f, true);
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeVisual")))
			GetWorldTimerManager().SetTimer(VisualRecoveryTimer, this, &ThisClass::CheckVisualRecovery, 0.1f, true);
#endif
	}
}

// 保存本机单调发送时间并请求服务器时间；丢失样本到期淘汰，未完成请求数量有上限。
void AfpstruePvpPlayerController::RequestClockSample()
{
	if (!IsLocalController() || GetNetMode() != NM_Client) return;
	const double Now = FPlatformTime::Seconds();
	for (auto It = PendingClockSamples.CreateIterator(); It; ++It)
		if (Now - It.Value() > 4.0) It.RemoveCurrent();
	if (PendingClockSamples.Num() >= 8) return;
	PendingClockSamples.Add(++ClockSequence, Now);
	ServerRequestClockSample(ClockSequence);
}

// 限流后回传当前服务器世界时间与原序号，不接收客户端自报的发送时间。
void AfpstruePvpPlayerController::ServerRequestClockSample_Implementation(uint32 Sequence)
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (LastServerClockRequest >= 0.0 && Now - LastServerClockRequest < 0.1) return;
	LastServerClockRequest = Now;
	ClientReceiveClockSample(Sequence, Now);
}

// 用本地保存的发送记录估计 RTT、时钟偏移和不确定度；迟到或严重排队样本不拉偏现有估计。
void AfpstruePvpPlayerController::ClientReceiveClockSample_Implementation(uint32 Sequence, double ServerTime)
{
	double SentAt = 0.0;
	if (!PendingClockSamples.RemoveAndCopyValue(Sequence, SentAt) || !FMath::IsFinite(ServerTime)) return;
	const double Now = FPlatformTime::Seconds();
	const double RoundTrip = Now - SentAt;
	if (RoundTrip < 0.0 || RoundTrip > 2.0) return;
	// 排队严重的样本不能反过来拉偏较好估计；超时后允许重新建立基准，避免网络改变后永久拒绝。
	if (LastClockSampleLocal >= 0.0 && Now - LastClockSampleLocal > 5.0)
		BestClockRoundTrip = TNumericLimits<double>::Max();
	if (RoundTrip > BestClockRoundTrip + 0.1) return;
	const double Offset = ServerTime + RoundTrip * 0.5 - Now;
	const double Residual = LastClockSampleLocal < 0.0 ? 0.0 : FMath::Abs(Offset - ServerClockOffset);
	ServerClockOffset = LastClockSampleLocal < 0.0 ? Offset : FMath::Lerp(ServerClockOffset, Offset, 0.25);
	BestClockRoundTrip = FMath::Min(BestClockRoundTrip, RoundTrip);
	ClockUncertainty = FMath::Clamp(RoundTrip * 0.5 + Residual, 0.001, 1.0);
	LastClockSampleLocal = Now;
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLOCK_SAMPLE PASS RTT=%.4f Uncertainty=%.4f Offset=%.6f Residual=%.6f BestRTT=%.6f Sequence=%u"),
			RoundTrip, ClockUncertainty, ServerClockOffset, Residual, BestClockRoundTrip, Sequence);
#endif
}

// 服务端自身时间始终可用；客户端样本超过五秒未刷新就不再用于射击时间估计。
bool AfpstruePvpPlayerController::HasServerTimeSample() const
{
	return HasAuthority() || (LastClockSampleLocal >= 0.0 && FPlatformTime::Seconds() - LastClockSampleLocal <= 5.0);
}

double AfpstruePvpPlayerController::GetEstimatedServerTime() const
{
	if (HasAuthority()) return GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0;
	return HasServerTimeSample() ? FPlatformTime::Seconds() + ServerClockOffset : 0.0;
}

// 返回射击时间校验所需的估计误差；失去新鲜样本时按一秒的不确定度处理。
double AfpstruePvpPlayerController::GetClockUncertaintySeconds() const
{
	return HasAuthority() ? 0.0 : (HasServerTimeSample() ? ClockUncertainty : 1.0);
}

// 真实代理位置变化检查，与原来的姿态/MaxWalkSpeed检查分开，不把两者混为移动精度验收。
#if !UE_BUILD_SHIPPING
void AfpstruePvpPlayerController::SampleSmokeMovement()
{
	if (bSmokeRemoteMovementReported || GetWorld() == nullptr) return;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsLocallyControlled() || It->IsDead()) continue;
		const TWeakObjectPtr<AfpstrueCharacter> Key(*It);
		const FVector Location = It->GetActorLocation();
		if (const FVector* Origin = SmokeRemoteOrigins.Find(Key))
		{
			const float Distance = FVector::Dist2D(*Origin, Location);
			if (Distance >= 5.0f)
			{
				bSmokeRemoteMovementReported = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_REMOTE_DISPLACEMENT PASS Pawn=%s Distance=%.2f"), *It->GetName(), Distance);
				GetWorldTimerManager().ClearTimer(SmokeMovementTimer);
				break;
			}
		}
		else SmokeRemoteOrigins.Add(Key, Location);
	}
}
#endif

#if !UE_BUILD_SHIPPING
// 开发验收只等待真实视口恢复，不授权连接、不修改伤害或角色状态。
void AfpstruePvpPlayerController::CheckVisualRecovery()
{
	if (!IsLocalController() || !FApp::CanEverRender() || bVisualCaptureRequested) return;
	auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance());
	auto* LocalPawn = Cast<AfpstrueCharacter>(GetPawn());
	auto* HUD = Cast<AfpstruePvpHUD>(GetHUD());
	auto* State = GetWorld()->GetGameState<AfpstruePvpGameState>();
	auto* Weapon = LocalPawn != nullptr ? LocalPawn->GetEquippedWeaponComponent() : nullptr;
	auto* Viewport = GetWorld()->GetGameViewport();
	const auto* Arms = LocalPawn != nullptr ? LocalPawn->GetMesh1P() : nullptr;
	// 正常出生可以空手；只有实际装备后才要求视模可见，不能把隐藏双手误判为画面未恢复。
	const bool bEquipmentReady = Arms != nullptr && Arms->IsRegistered()
		&& (Weapon == nullptr ? Arms->bHiddenInGame
			: !Arms->bHiddenInGame && Arms->IsVisible() && Weapon->IsRegistered() && Weapon->IsVisible()
				&& !Weapon->bHiddenInGame && Weapon->GetSkeletalMeshAsset() != nullptr && Weapon->GetAttachParent() == Arms);
	if (Instance == nullptr || Instance->ConnectionState != EFPConnectionState::Connected || LocalPawn == nullptr || LocalPawn->IsDead()
		|| GetViewTarget() != LocalPawn || PlayerCameraManager == nullptr || !FMath::IsFinite(PlayerCameraManager->GetFOVAngle())
		|| LocalPawn->GetFirstPersonCameraComponent() == nullptr || !LocalPawn->GetFirstPersonCameraComponent()->IsActive()
		|| !bEquipmentReady || HUD == nullptr || !HUD->HasDrawnCurrentState(LocalPawn)
		|| Viewport == nullptr || Viewport->Viewport == nullptr || Viewport->Viewport->GetSizeXY().GetMin() <= 0
		|| State == nullptr || !State->GetEnemySignificanceCoordinator()->HasRenderedEnemies())
	{ VisualReadyAt = 0.0; return; }
	bool bHasEnemyReplica = false;
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
	{
		const auto* Mesh = It->GetMesh();
		if (!It->HasAuthority() && Mesh != nullptr && Mesh->IsRegistered() && Mesh->IsVisible() && Mesh->GetSkeletalMeshAsset() != nullptr)
		{ bHasEnemyReplica = true; break; }
	}
	if (!bHasEnemyReplica) { VisualReadyAt = 0.0; return; }
	// 等待若干实际渲染帧，让相机、骨骼与 HUD 在状态就绪后参与绘制。
	if (VisualReadyAt == 0.0) { VisualReadyAt = FPlatformTime::Seconds(); return; }
	if (FPlatformTime::Seconds() - VisualReadyAt < 0.4 || FScreenshotRequest::IsScreenshotRequested()) return;
	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), Directory) || Directory.IsEmpty()) return;
	IFileManager::Get().MakeDirectory(*Directory, true);
	VisualResumeId = Instance->ResumeId;
	bVisualRestored = Instance->bRestoredConnection;
	VisualScreenshotPath = FPaths::Combine(Directory, bVisualRestored ? TEXT("restored.png") : TEXT("initial.png"));
	UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_READY PASS Restored=%d ResumeId=%d"), bVisualRestored, VisualResumeId);
	VisualScreenshotHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &ThisClass::HandleVisualScreenshotProcessed);
	bVisualCaptureRequested = true;
	FScreenshotRequest::RequestScreenshot(VisualScreenshotPath, true, false, false);
}

// 请求处理结束后仍检查实际 PNG 文件，发出请求不等于截图成功。
void AfpstruePvpPlayerController::HandleVisualScreenshotProcessed()
{
	FScreenshotRequest::OnScreenshotRequestProcessed().Remove(VisualScreenshotHandle);
	VisualScreenshotHandle.Reset();
	GetWorldTimerManager().ClearTimer(VisualRecoveryTimer);
	TArray<uint8> Header;
	if (!FFileHelper::LoadFileToArray(Header, *VisualScreenshotPath) || Header.Num() < 24
		|| Header[0] != 137 || Header[1] != 80 || Header[2] != 78 || Header[3] != 71)
	{
		UE_LOG(LogTemp, Error, TEXT("PVP_VISUAL_CAPTURE FAIL Reason=missing_png"));
		return;
	}
	const uint32 Width = (uint32(Header[16]) << 24) | (uint32(Header[17]) << 16) | (uint32(Header[18]) << 8) | Header[19];
	const uint32 Height = (uint32(Header[20]) << 24) | (uint32(Header[21]) << 16) | (uint32(Header[22]) << 8) | Header[23];
	UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_CAPTURE PASS Restored=%d ResumeId=%d File=%s Width=%u Height=%u"),
		bVisualRestored, VisualResumeId, *FPaths::GetCleanFilename(VisualScreenshotPath), Width, Height);
}
#endif

// 控制器退出时停止时钟与验收采样，解除截图回调，不让旧世界继续产生样本。
void AfpstruePvpPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(ClockSampleTimer);
	PendingClockSamples.Reset();
	LastClockSampleLocal = -1.0;
#if !UE_BUILD_SHIPPING
	GetWorldTimerManager().ClearTimer(SmokeMovementTimer);
	SmokeRemoteOrigins.Reset();
	GetWorldTimerManager().ClearTimer(VisualRecoveryTimer);
	FScreenshotRequest::OnScreenshotRequestProcessed().Remove(VisualScreenshotHandle);
#endif
	Super::EndPlay(EndPlayReason);
}

// 复制成绩到所有客户端；玩家状态在一次Pawn重生期间不被重新创建。
void AfpstruePvpPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstruePvpPlayerState, Kills);
	DOREPLIFETIME(AfpstruePvpPlayerState, Deaths);
	DOREPLIFETIME(AfpstruePvpPlayerState, bDisconnected);
}

// 服务器提交离线保留标记并尽快复制；成绩继续保存在同一个 PlayerState。
void AfpstruePvpPlayerState::SetRetainedForReconnect(bool bRetained)
{
	if (!HasAuthority()) return;
	bDisconnected = bRetained;
	ForceNetUpdate();
}

// 离线保留时阻止引擎销毁 PlayerState；普通离场仍采用父类清理。
void AfpstruePvpPlayerState::OnDeactivated()
{
	if (bDisconnected) { SetOwner(nullptr); return; }
	Super::OnDeactivated();
}

// 服务器累加击杀并及时发布复制更新，客户端调用不会改变成绩。
void AfpstruePvpPlayerState::AddKill()
{
	if (!HasAuthority()) return;
	++Kills;
	SetScore(static_cast<float>(Kills));
	ForceNetUpdate();
}

// 服务器累加死亡数；去重由GameMode的待重生记录负责。
void AfpstruePvpPlayerState::AddDeath()
{
	if (!HasAuthority()) return;
	++Deaths;
	ForceNetUpdate();
}

// GameState 在服务端与客户端都有实例，组件只运行本端策略，不复制本地相机或预算结果。
AfpstruePvpGameState::AfpstruePvpGameState()
{
	EnemySignificanceCoordinator = CreateDefaultSubobject<UfpstrueEnemySignificanceCoordinator>(TEXT("EnemySignificanceCoordinator"));
	EnemyAnimationSharingCoordinator = CreateDefaultSubobject<UfpstrueEnemyAnimationSharingCoordinator>(TEXT("EnemyAnimationSharingCoordinator"));
}

// 客户端不等待 GameMode；晚加入和重新连接随新 GameState 自动建立本端协调器。
void AfpstruePvpGameState::BeginPlay()
{
	Super::BeginPlay();
	EnemySignificanceCoordinator->StartMultiplayer(EnemyAnimationSharingCoordinator);
	StartPvpPerformanceProfile(GetWorld());
}

// 地图结束时先停止新采样，再注销共享 Follower；组件 EndPlay 仍可幂等兜底。
void AfpstruePvpGameState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EnemySignificanceCoordinator->Stop();
	EnemyAnimationSharingCoordinator->Stop();
	Super::EndPlay(EndPlayReason);
}

void AfpstruePvpGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstruePvpGameState, bMatchActive);
	DOREPLIFETIME(AfpstruePvpGameState, EliminationCount);
}

// 只整理当前玩家状态视图，不把成绩从Pawn或客户端输入重新计算一遍。
TArray<AfpstruePvpPlayerState*> AfpstruePvpGameState::GetScoreboard() const
{
	TArray<AfpstruePvpPlayerState*> Players;
	for (APlayerState* State : PlayerArray)
	{
		if (AfpstruePvpPlayerState* Player = Cast<AfpstruePvpPlayerState>(State)) Players.Add(Player);
	}
	Players.Sort([](const AfpstruePvpPlayerState& A, const AfpstruePvpPlayerState& B)
	{
		if (A.GetKills() != B.GetKills()) return A.GetKills() > B.GetKills();
		if (A.GetDeaths() != B.GetDeaths()) return A.GetDeaths() < B.GetDeaths();
		return A.GetPlayerId() < B.GetPlayerId();
	});
	return Players;
}

// 比赛开放资格只由服务器写入，客户端读取复制快照。
void AfpstruePvpGameState::SetMatchActive(bool bActive)
{
	if (!HasAuthority()) return;
	bMatchActive = bActive;
	ForceNetUpdate();
}

// 一次已去重的角色死亡只增加一次全局计数。
void AfpstruePvpGameState::AddElimination()
{
	if (!HasAuthority()) return;
	++EliminationCount;
	ForceNetUpdate();
}

// 恢复期 Pawn 仍在服务器世界中且可受伤；只有失效、死亡或过期退出后才从 AI 候选中移除。
bool AfpstruePvpGameMode::IsCombatParticipant(const AfpstrueCharacter* Target) const
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Target) || Target->GetWorld() != GetWorld() || Target->IsDead()) return false;
	const APlayerController* Player = Cast<APlayerController>(Target->GetController());
	return (IsValid(Player) && Player->GetPawn() == Target) || IsRetainedDisconnectedPawn(Target);
}

// 为有效战斗目标复用或创建一个围攻管理器；目标选择与单敌人决策不在此执行。
AfpstrueSurroundManager* AfpstruePvpGameMode::GetOrCreateSurroundManager(AfpstrueCharacter* Target)
{
	if (!IsCombatParticipant(Target)) return nullptr;
	const TWeakObjectPtr<AfpstrueCharacter> Key(Target);
	if (auto* Existing = TargetSurroundManagers.Find(Key); Existing != nullptr && Existing->IsValid()) return Existing->Get();
	AfpstrueSurroundManager* Manager = GetWorld()->SpawnActor<AfpstrueSurroundManager>();
	if (!IsValid(Manager) || !IsCombatParticipant(Target)) { if (IsValid(Manager)) Manager->Destroy(); return nullptr; }
	TargetSurroundManagers.Add(Key, Manager);
	Manager->SetTargetCharacter(Target);
	return Manager;
}

// 按权威敌人登记表统计仍有效且存活的对象，不把延迟销毁的尸体计入生成缺口。
int32 AfpstruePvpGameMode::GetAliveEnemyCount() const
{
	int32 Count = 0;
	for (const auto& Enemy : PveEnemies) if (Enemy.IsValid() && !Enemy->IsDead()) ++Count;
	return Count;
}

// 单个 Timer 维护整个敌人生成队列；没有有效玩家时不生成，人数增长也不会另开一套生成器。
void AfpstruePvpGameMode::UpdatePvePopulation()
{
	if (bEndingPvp || !HasAuthority()) return;
	for (auto It = PveEnemies.CreateIterator(); It; ++It) if (!It->IsValid()) It.RemoveCurrent();
	for (auto It = TargetSurroundManagers.CreateIterator(); It; ++It)
	{
		if (!IsCombatParticipant(It.Key().Get()))
		{
			if (auto* Manager = It.Value().Get()) { Manager->ResetManager(); Manager->Destroy(); }
			It.RemoveCurrent();
		}
	}
	for (auto It = ObservedPvePlayers.CreateIterator(); It; ++It) if (!It->IsValid()) It.RemoveCurrent();
	AfpstrueCharacter* FirstPlayer = nullptr;
	int32 ReadyPlayers = 0;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
	{
		if (!IsCombatParticipant(*It)) continue;
		if (FirstPlayer == nullptr) FirstPlayer = *It;
		const auto* Player = Cast<AfpstruePvpPlayerController>(It->GetController());
		if (Player != nullptr && Player->bConnectionHandshakeComplete) ++ReadyPlayers;
		const TWeakObjectPtr<AfpstrueCharacter> Key(*It);
		if (!ObservedPvePlayers.Contains(Key))
		{
			ObservedPvePlayers.Add(Key);
			if (auto* Health = It->FindComponentByClass<UfpstrueHealthComponent>())
				Health->OnDamageResolved.AddWeakLambda(this, [this, Key](float Damage, AActor* Causer, AController* DamageInstigator, const FFPDamageContext& Context)
				{ HandlePvePlayerDamaged(Key, Damage, Causer, DamageInstigator, Context); });
		}
	}
	if (FirstPlayer == nullptr) return;
	if (!bBaselinePopulationStarted && FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline")))
	{
		if (ReadyPlayers != 2) return;
		bBaselinePopulationStarted = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_BASELINE ADMISSION Players=2 PawnReady=1 EnemyPopulation=%d"), EnemyPopulation);
	}
#if !UE_BUILD_SHIPPING
	if (!bNormalVisualRoutesSent && FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPairWalk"))) SendNormalVisualRoutes();
#endif
	AfpstrueCharacter* SmokeNearPlayer = nullptr;
#if !UE_BUILD_SHIPPING
	if (bSmokeTest)
	{
		if (!bSmokePveStarted) return; // 原 PvP 门禁完成之前不让敌人干扰玩家计分和自动输入。
		if (!bSmokePveCompleted) SmokeNearPlayer = SmokePvePlayer.Get();
		else
		{
			if (!FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeReconnect")) || bSmokeOfflineDamage || bSmokeOfflineSceneFailed) return;
			for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
				if (IsCombatParticipant(*It) && IsRetainedDisconnectedPawn(*It)) { SmokeNearPlayer = *It; break; }
			if (SmokeNearPlayer == nullptr) return;
			SmokeOfflineTarget = SmokeNearPlayer;
			if (!bSmokeOfflineScenePrepared)
			{
				// 只隔离测试场景中的其他玩家，仍由真实视觉、行为树和刀刃检测攻击保留 Pawn，不指定或伪造 AI 目标。
				UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
				if (Navigation == nullptr)
				{
					bSmokeOfflineSceneFailed = true;
					UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL ResumeOfflineIsolation Reason=NoNavigation"));
					return;
				}
				const FVector RetainedLocation = SmokeNearPlayer->GetActorLocation();
				TArray<FVector, TInlineAllocator<56>> IsolationCandidates;
				for (float Radius : {7000.0f, 10000.0f, 14000.0f})
					for (int32 Direction = 0; Direction < 8; ++Direction)
					{
						const float Angle = FMath::DegreesToRadians(static_cast<float>(Direction * 45));
						IsolationCandidates.Add(RetainedLocation + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * Radius);
					}
				for (int32 Index = 0; Index < FMath::Min(PveSpawnPoints.Num(), 32); ++Index)
					if (IsValid(PveSpawnPoints[Index])) IsolationCandidates.Add(PveSpawnPoints[Index]->GetActorLocation());
				for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
				{
					AfpstrueCharacter* OnlinePlayer = *It;
					if (OnlinePlayer == SmokeNearPlayer || !IsCombatParticipant(OnlinePlayer) ||
						Cast<APlayerController>(OnlinePlayer->GetController()) == nullptr) continue;
					bool bIsolated = false;
					for (const FVector& Candidate : IsolationCandidates)
					{
						FNavLocation Projected;
						if (!Navigation->ProjectPointToNavigation(Candidate, Projected, FVector(600.0f, 600.0f, 1000.0f)) ||
							FVector::DistSquared2D(Projected.Location, RetainedLocation) <= FMath::Square(6500.0f)) continue;
						const FVector Destination = Projected.Location + FVector::UpVector *
							(OnlinePlayer->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0f);
						if (!OnlinePlayer->TeleportTo(Destination, OnlinePlayer->GetActorRotation(), false, false) ||
							FVector::DistSquared2D(OnlinePlayer->GetActorLocation(), RetainedLocation) <= FMath::Square(6000.0f)) continue;
						OnlinePlayer->GetCharacterMovement()->StopMovementImmediately();
						OnlinePlayer->ForceNetUpdate();
						bIsolated = true;
						UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE RECONNECT_ISOLATE Player=%s Distance=%.1f Location=%s"),
							*OnlinePlayer->GetName(), FVector::Dist2D(OnlinePlayer->GetActorLocation(), RetainedLocation),
							*OnlinePlayer->GetActorLocation().ToCompactString());
						break;
					}
					if (!bIsolated)
					{
						bSmokeOfflineSceneFailed = true;
						UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL ResumeOfflineIsolation Reason=NoNavigableCollisionFreeLocation Player=%s"), *OnlinePlayer->GetName());
						return;
					}
				}
				bSmokeOfflineScenePrepared = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE RECONNECT_SCENE_PREPARED RetainedPawn=%s"), *SmokeNearPlayer->GetName());
			}
		}
	}
#endif
	const int32 Desired =
#if !UE_BUILD_SHIPPING
		bSmokeTest ? 1 :
#endif
		FMath::Clamp(EnemyPopulation, 0, 160);
	if (GetAliveEnemyCount() >= Desired || GetWorld()->GetTimeSeconds() < NextEnemySpawnTime) return;
	NextEnemySpawnTime = GetWorld()->GetTimeSeconds() + FMath::Max(EnemySpawnInterval, 0.05f);
	if (AfpstrueEnemyCharacter* Enemy = SpawnPveEnemy(SmokeNearPlayer))
	{
		RegisterPveEnemy(Enemy);
#if !UE_BUILD_SHIPPING
		if (bSmokeTest && !bSmokePveCompleted)
		{
			SmokePveEnemy = Enemy;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PveSpawn Enemy=%s"), *Enemy->GetName());
		}
#endif
	}
}

// 导航投影和碰撞调整与正常生成共用；测试只把出生候选放近玩家，不直接伪造攻击或伤害。
AfpstrueEnemyCharacter* AfpstruePvpGameMode::SpawnPveEnemy(AfpstrueCharacter* NearPlayer)
{
	if (!HasAuthority() || EnemyClass == nullptr || bEndingPvp) return nullptr;
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (Navigation == nullptr || (NearPlayer == nullptr && PveSpawnPoints.IsEmpty())) return nullptr;
	const auto* Defaults = EnemyClass.GetDefaultObject();
	const float HalfHeight = Defaults != nullptr ? Defaults->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 96.0f;
	for (int32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		FVector Candidate;
		FRotator Rotation = FRotator::ZeroRotator;
		if (NearPlayer != nullptr)
		{
			const float Angle = FMath::DegreesToRadians(static_cast<float>(Attempt * 45));
			Candidate = NearPlayer->GetActorLocation() + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * 180.0f;
		}
		else
		{
			AActor* Point = PveSpawnPoints[NextPveSpawnIndex++ % PveSpawnPoints.Num()];
			if (!IsValid(Point)) continue;
			Candidate = Point->GetActorLocation();
			// 保留关卡作者设置的警戒方向；导航只调整位置，不能把全部敌人的视锥重置到世界 +X。
			Rotation.Yaw = Point->GetActorRotation().Yaw;
			if (Attempt > 0) Candidate += FVector(FMath::FRandRange(-400.0f, 400.0f), FMath::FRandRange(-400.0f, 400.0f), 0.0f);
		}
		FNavLocation Projected;
		if (!Navigation->ProjectPointToNavigation(Candidate, Projected, FVector(150.0f, 150.0f, 500.0f))) continue;
		const FVector Location = Projected.Location + FVector::UpVector * (HalfHeight + 2.0f);
		if (NearPlayer != nullptr) Rotation.Yaw = (NearPlayer->GetActorLocation() - Location).Rotation().Yaw;
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
		AfpstrueEnemyCharacter* Enemy = GetWorld()->SpawnActor<AfpstrueEnemyCharacter>(EnemyClass, Location, Rotation, Parameters);
		if (!IsValid(Enemy)) continue;
		if (Enemy->GetController() == nullptr) Enemy->SpawnDefaultController();
		if (!IsValid(Enemy)) continue;
		const auto* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController());
		if (IsValid(Enemy) && !Enemy->IsDead() && Controller != nullptr && Controller->IsBehaviorReady()) return Enemy;
		if (AController* InvalidController = Enemy->GetController()) InvalidController->Destroy();
		Enemy->Destroy();
	}
	return nullptr;
}

// 只登记已启动行为树的存活敌人，并连接生命周期和开发验收观察；不替 AI 指定战斗目标。
void AfpstruePvpGameMode::RegisterPveEnemy(AfpstrueEnemyCharacter* Enemy)
{
	const auto* Controller = IsValid(Enemy) ? Cast<AfpstrueEnemyAIController>(Enemy->GetController()) : nullptr;
	if (!HasAuthority() || Controller == nullptr || !Controller->IsBehaviorReady() || Enemy->IsDead() || PveEnemies.Contains(Enemy)) return;
	PveEnemies.Add(Enemy);
	Enemy->OnEnemyDeathReported.AddUniqueDynamic(this, &ThisClass::HandlePveEnemyDied);
	Enemy->OnEndPlay.AddUniqueDynamic(this, &ThisClass::HandlePveEnemyEndPlay);
	if (auto* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>())
	{
		const TWeakObjectPtr<AfpstrueEnemyCharacter> WeakEnemy(Enemy);
		Health->OnDamageResolved.AddWeakLambda(this, [this, WeakEnemy](float Damage, AActor*, AController* DamageInstigator, const FFPDamageContext&)
		{
#if !UE_BUILD_SHIPPING
			if (bSmokeTest && WeakEnemy == SmokePveEnemy && Damage > 0.0f && IsValid(DamageInstigator) && DamageInstigator->GetPawn() == SmokePvePlayer)
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PvePlayerHit Enemy=%s Health=%.1f"), *GetNameSafe(WeakEnemy.Get()), WeakEnemy->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth());
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")) && WeakEnemy.IsValid() && Damage > 0.0f)
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_DAMAGE Enemy=%s Health=%.1f Instigator=%s Damage=%.1f"),
					*WeakEnemy->GetName(), WeakEnemy->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth(), *GetNameSafe(DamageInstigator), Damage);
#endif
		});
	}
}

// 死亡后延迟补充人口并限制尸体寿命；保留登记到 EndPlay，以便统一解除订阅。
void AfpstruePvpGameMode::HandlePveEnemyDied(AfpstrueEnemyCharacter* Enemy)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Enemy)) return;
	NextEnemySpawnTime = GetWorld()->GetTimeSeconds() + FMath::Max(EnemyRespawnDelay, 0.1f);
	if (Enemy->GetLifeSpan() <= 0.0f) Enemy->SetLifeSpan(15.0f); // 联网尸体必须有回收边界。
#if !UE_BUILD_SHIPPING
	if (bSmokeTest && Enemy == SmokePveEnemy)
	{
		bSmokePveCompleted = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PveEnemyDeath Enemy=%s"), *Enemy->GetName());
	}
#endif
}

// 敌人真正离场时移出登记并解除死亡、离场及伤害观察。
void AfpstruePvpGameMode::HandlePveEnemyEndPlay(AActor* Actor, EEndPlayReason::Type)
{
	AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(Actor);
	if (Enemy == nullptr) return;
	PveEnemies.Remove(Enemy);
	Enemy->OnEnemyDeathReported.RemoveDynamic(this, &ThisClass::HandlePveEnemyDied);
	Enemy->OnEndPlay.RemoveDynamic(this, &ThisClass::HandlePveEnemyEndPlay);
	if (auto* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>()) Health->OnDamageResolved.RemoveAll(this);
}

// 仅开发烟测观察真实伤害结果，推进敌人反击与离线受伤门禁；Shipping 不执行验收逻辑。
void AfpstruePvpGameMode::HandlePvePlayerDamaged(TWeakObjectPtr<AfpstrueCharacter> Player, float Damage,
	AActor* DamageCauser, AController*, const FFPDamageContext&)
{
#if !UE_BUILD_SHIPPING
	if (!bSmokeTest || Damage <= 0.0f || !Player.IsValid() || !IsValid(Cast<AfpstrueEnemyCharacter>(DamageCauser))) return;
	if (bSmokePveStarted && !bSmokePveEnemyHit)
	{
		bSmokePveEnemyHit = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PveEnemyHit Player=%s Health=%.1f"), *Player->GetName(), Player->GetCurrentHealth());
		if (SmokePvePlayer.IsValid())
			if (auto* Shooter = Cast<AfpstruePvpPlayerController>(SmokePvePlayer->GetController())) Shooter->ClientAllowPveSmokeCounterattack();
	}
	if (!bSmokeOfflineDamage && Player == SmokeOfflineTarget && IsRetainedDisconnectedPawn(Player.Get()))
	{
		for (const auto& Entry : RejoinSlots)
			if (Entry.Value.Pawn == Player && Entry.Value.State.IsValid())
			{
				bSmokeOfflineDamage = true;
				const float After = Player->GetCurrentHealth();
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=ResumeOfflineDamage ResumeId=%d Before=%.3f After=%.3f"), Entry.Value.State->GetPlayerId(), After + Damage, After);
				if (auto* EnemyAI = Cast<AfpstrueEnemyAIController>(CastChecked<AfpstrueEnemyCharacter>(DamageCauser)->GetController())) EnemyAI->StopAI();
				break;
			}
	}
#endif
}

// 停止人口补充和 AI，解除伤害观察并归还每个目标的围攻资源；不在这里销毁全部敌人。
void AfpstruePvpGameMode::StopPve()
{
	GetWorldTimerManager().ClearTimer(PvePopulationTimer);
	const auto Enemies = PveEnemies;
	for (const auto& Weak : Enemies) if (auto* Enemy = Weak.Get())
	{
		if (auto* AI = Cast<AfpstrueEnemyAIController>(Enemy->GetController())) AI->StopAI();
		HandlePveEnemyEndPlay(Enemy, EEndPlayReason::RemovedFromWorld);
	}
	PveEnemies.Reset();
	for (const auto& Player : ObservedPvePlayers) if (Player.IsValid())
		if (auto* Health = Player->FindComponentByClass<UfpstrueHealthComponent>()) Health->OnDamageResolved.RemoveAll(this);
	ObservedPvePlayers.Reset();
	const auto Managers = MoveTemp(TargetSurroundManagers);
	for (const auto& Entry : Managers) if (auto* Manager = Entry.Value.Get()) { Manager->ResetManager(); Manager->Destroy(); }
}

// 原 PvP 门禁通过后，显式烟测才开放敌人阶段；普通玩法与 Shipping 不采用这条控制链。
void AfpstruePvpGameMode::StartPveSmoke(AfpstruePvpPlayerController* Requester)
{
#if !UE_BUILD_SHIPPING
	if (!bSmokeTest || !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokePve")) || bSmokePveStarted ||
		!IsValid(Requester) || !IsCombatParticipant(Cast<AfpstrueCharacter>(Requester->GetPawn()))) return;
	bSmokePveStarted = true;
	SmokePvePlayer = Cast<AfpstrueCharacter>(Requester->GetPawn());
	NextEnemySpawnTime = GetWorld()->GetTimeSeconds();
#endif
}

#if !UE_BUILD_SHIPPING
// 只给显式普通入口验收下发一次可行路线；初始出生点和实际武器/伤害/移动协议均不改。
void AfpstruePvpGameMode::SendNormalVisualRoutes()
{
	TArray<AfpstrueCharacter*> Players;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
		if (IsCombatParticipant(*It) && It->HasEquippedWeapon() && It->GetPlayerState()) Players.Add(*It);
	if (Players.Num() != 2) return;
	Players.Sort([](const AfpstrueCharacter& A, const AfpstrueCharacter& B) { return A.GetPlayerState()->GetPlayerId() < B.GetPlayerState()->GetPlayerId(); });
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!Navigation) return;
	FNavLocation Destination;
	if (!Navigation->ProjectPointToNavigation(Players[0]->GetActorLocation() + Players[0]->GetActorRightVector() * 350.0, Destination, FVector(200, 200, 200))) return;
	UNavigationPath* Path = Navigation->FindPathToLocationSynchronously(GetWorld(), Players[1]->GetActorLocation(), Destination.Location, Players[1]);
	if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.IsEmpty() || Path->PathPoints.Num() > 128)
	{
		bNormalVisualRoutesSent = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE FAIL Reason=NoCompleteBoundedPath"));
		return;
	}
	auto* First = Cast<AfpstruePvpPlayerController>(Players[0]->GetController());
	auto* Second = Cast<AfpstruePvpPlayerController>(Players[1]->GetController());
	if (!First || !Second) return;
	bNormalVisualRoutesSent = true;
	First->ClientNormalVisualRoute({Players[0]->GetActorLocation()});
	Second->ClientNormalVisualRoute(Path->PathPoints);
	UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE SERVER Points=%d StartDistanceCm=%.1f EnemyPopulation=%d Teleport=0"),
		Path->PathPoints.Num(), FVector::Dist2D(Players[0]->GetActorLocation(), Players[1]->GetActorLocation()), EnemyPopulation);
}
#endif

// 将开发验收的阶段请求转交服务器 GameMode，不直接生成敌人。
void AfpstruePvpPlayerController::ServerBeginPveSmoke_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>()) Mode->StartPveSmoke(this);
#endif
}

// 服务器确认敌人真实命中后，才允许本地烟测驱动器反击。
void AfpstruePvpPlayerController::ClientAllowPveSmokeCounterattack_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokePve"))) bSmokePveCounterattackReady = true;
#endif
}

// 使用项目真正的玩家蓝图路径，旧模板路径只是重定向资产。
AfpstruePvpGameMode::AfpstruePvpGameMode()
{
	PlayerControllerClass = AfpstruePvpPlayerController::StaticClass();
	PlayerStateClass = AfpstruePvpPlayerState::StaticClass();
	GameStateClass = AfpstruePvpGameState::StaticClass();
	HUDClass = AfpstruePvpHUD::StaticClass();
#if !UE_BUILD_SHIPPING
	bSmokeTest = FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest"));
#endif
	DefaultPawnClass = AfpstrueCharacter::StaticClass();
	// RAR 表现蓝图不继承旧 RU74 事件图；能力、伤害、输入与网络仍复用原生实现。
	// 首次编辑器生成前可为空，交付门禁要求该类及完整动画资源存在。
	if (UClass* PlayerBlueprint = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C"), nullptr, LOAD_Quiet | LOAD_NoWarn))
		DefaultPawnClass = PlayerBlueprint;
	DefaultWeaponClass = AfpstrueTemplateWeapon::StaticClass();
	static ConstructorHelpers::FClassFinder<AfpstrueEnemyCharacter> EnemyBlueprint(TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP"));
	if (EnemyBlueprint.Succeeded()) EnemyClass = EnemyBlueprint.Class;
}

// 同场维护玩家对战与敌人 AI；单个玩家死亡不结束其他玩家仍在进行的对局。
void AfpstruePvpGameMode::BeginPlay()
{
	Super::BeginPlay();
	// 仅正常联机替换关卡里的旧素材拾取物；不改原地图、单机资产或出生装备规则。
	bool bNormalPlay = true;
#if !UE_BUILD_SHIPPING
	bNormalPlay = !bSmokeTest;
#endif
	if (HasAuthority() && GetNetMode() != NM_Standalone && bNormalPlay && DefaultWeaponClass != nullptr)
	{
		const auto IsUnheldLegacyPickup = [](AActor* Actor)
		{
			if (!IsValid(Actor) || Actor->GetOwner() != nullptr) return false;
			UClass* Class = Actor->GetClass();
			if (Class != AfpstrueRARWeapon::StaticClass()
				&& Class->GetPathName() != TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon.BP_Weapon_C")) return false;
			const UfpstrueWeaponComponent* Weapon = Actor->FindComponentByClass<UfpstrueWeaponComponent>();
			return Weapon != nullptr && Weapon->GetOwningCharacter() == nullptr;
		};
		// 先收集再生成/销毁，避免遍历 Actor 列表时把新枪再次纳入替换。
		TArray<TWeakObjectPtr<AActor>> Pickups;
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			if (IsUnheldLegacyPickup(*It)) Pickups.Add(*It);
		for (const TWeakObjectPtr<AActor>& Pickup : Pickups)
		{
			AActor* Previous = Pickup.Get();
			if (!IsUnheldLegacyPickup(Previous)) continue;
			// 只复制 Actor 的关卡变换，不抄旧 RU74 网格组件的独立缩放或附着偏移。
			const FTransform Transform = Previous->GetActorTransform();
			if (Transform.ContainsNaN())
			{
				UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_REPLACE Rejected=%s Reason=NonFiniteActorTransform"), *Previous->GetName());
				continue;
			}
			FActorSpawnParameters Parameters;
			Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AActor* Replacement = GetWorld()->SpawnActor<AActor>(DefaultWeaponClass, Transform, Parameters))
			{
				UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_REPLACE Old=%s New=%s Class=%s ActorScale=%s"),
					*Previous->GetName(), *Replacement->GetName(), *Replacement->GetClass()->GetPathName(), *Transform.GetScale3D().ToCompactString());
				// 原单机拾取物可能未开启复制；先登记网络销毁，连已加载或晚加入客户端的关卡副本一起移除。
				Previous->SetReplicates(true);
				Previous->Destroy();
			}
			else UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_REPLACE Failed=%s OriginalRetained=true"), *Previous->GetName());
		}
	}
	if (AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState)) State->SetMatchActive(true);
	TArray<AActor*> Points;
	UGameplayStatics::GetAllActorsWithTag(this, EnemySpawnTag, Points);
	for (AActor* Point : Points) if (IsValid(Point)) PveSpawnPoints.Add(Point);
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It) RegisterPveEnemy(*It);
	GetWorldTimerManager().SetTimer(PvePopulationTimer, this, &ThisClass::UpdatePvePopulation, FMath::Max(EnemySpawnInterval, 0.05f), true);
#if !UE_BUILD_SHIPPING
	if (bSmokeTest)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_IDENTITY Dedicated=%d LocalPlayers=%d"),
			GetNetMode() == NM_DedicatedServer, GetGameInstance() != nullptr ? GetGameInstance()->GetNumLocalPlayers() : -1);
		GetWorldTimerManager().SetTimer(SmokeMovementTimer, this, &ThisClass::SampleSmokeMovement, 0.05f, true);
	}
#endif
}

#if !UE_BUILD_SHIPPING
// 观察服务器真实位移是否越过验收阈值，不修改角色位置或移动参数。
void AfpstruePvpGameMode::SampleSmokeMovement()
{
	for (auto It = SmokeSpawnOrigins.CreateIterator(); It; ++It)
	{
		const AfpstrueCharacter* Pawn = It.Key().Get();
		if (Pawn == nullptr) { It.RemoveCurrent(); continue; }
		if (Pawn->IsDead() || SmokeMovedPlayers.Contains(It.Key())) continue;
		const float Distance = FVector::Dist2D(It.Value(), Pawn->GetActorLocation());
		if (Distance >= 5.0f)
		{
			SmokeMovedPlayers.Add(It.Key());
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_DISPLACEMENT PASS Pawn=%s Distance=%.2f"), *Pawn->GetName(), Distance);
		}
	}
}
#endif

// 所有玩家出生统一保留碰撞检查；蓝图不能以AlwaysSpawn把角色挤进敌人或关卡碰撞。
APawn* AfpstruePvpGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	FActorSpawnParameters Spawn;
	Spawn.Instigator = GetInstigator();
	Spawn.ObjectFlags |= RF_Transient;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
	return GetWorld()->SpawnActor<APawn>(GetDefaultPawnClassForController(NewPlayer), SpawnTransform, Spawn);
}

// 失败只重试正常出生事务；观战不生成Pawn，基线保留原StartSpot，不能换锚点刷过固定场景门禁。
void AfpstruePvpGameMode::RestartPlayer(AController* NewPlayer)
{
	APlayerController* Player = Cast<APlayerController>(NewPlayer);
	if (!HasAuthority() || bEndingPvp || !IsValid(Player) || MustSpectate(Player)) return;
	if (const auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player); PvpPlayer != nullptr && PvpPlayer->bIntentionalLeave) return;
	if (Player->GetPawn() == nullptr)
	{
		FSpawnRecovery& Recovery = SpawnRecoveries.FindOrAdd(Player);
		if (Recovery.Attempts >= MaxPlayerSpawnAttempts)
		{
			FailedToRestartPlayer(Player);
			return;
		}
		GetWorldTimerManager().ClearTimer(Recovery.Timer);
		if (Recovery.Attempts > 0 && !FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline"))) Player->StartSpot.Reset();
		++Recovery.Attempts;
	}
	Super::RestartPlayer(NewPlayer);
}

// 出生失败采用有界 Timer 重试；耗尽次数则明确拒绝连接，不把缺少 Pawn 当作就绪。
void AfpstruePvpGameMode::FailedToRestartPlayer(AController* NewPlayer)
{
	Super::FailedToRestartPlayer(NewPlayer);
	APlayerController* Player = Cast<APlayerController>(NewPlayer);
	if (bEndingPvp || !HasAuthority() || !IsValid(Player) || Player->GetPawn() != nullptr || MustSpectate(Player)) return;
	FSpawnRecovery& Recovery = SpawnRecoveries.FindOrAdd(Player);
	Recovery.Attempts = FMath::Max(Recovery.Attempts, 1);
	if (GetWorldTimerManager().IsTimerActive(Recovery.Timer)) return;
	if (Recovery.Attempts >= MaxPlayerSpawnAttempts)
	{
		UE_LOG(LogTemp, Warning, TEXT("PVP_SPAWN_FAILED Player=%s Attempts=%d PawnReady=0"), *Player->GetName(), Recovery.Attempts);
		if (auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player); PvpPlayer != nullptr && PvpPlayer->bConnectionHandshakeComplete)
		{
			PvpPlayer->bIntentionalLeave = true;
			PvpPlayer->ClientConnectionRejected(TEXT("pawn_spawn_failed"));
		}
		return;
	}
	const TWeakObjectPtr<APlayerController> Key(Player);
	GetWorldTimerManager().SetTimer(Recovery.Timer, FTimerDelegate::CreateWeakLambda(this, [this, Key]()
	{
		if (Key.IsValid()) RestartPlayer(Key.Get());
	}), 0.5f, false);
}

// 引擎完成Possess、控制旋转和默认Pawn设置之后，再执行服务器装备事务。
void AfpstruePvpGameMode::FinishRestartPlayer(AController* NewPlayer, const FRotator& StartRotation)
{
	Super::FinishRestartPlayer(NewPlayer, StartRotation);
	APlayerController* Player = Cast<APlayerController>(NewPlayer);
	AfpstrueCharacter* Pawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	if (!IsValid(Pawn) || Pawn->IsDead() || Pawn->GetController() != Player) return;
	FSpawnRecovery Recovery;
	if (SpawnRecoveries.RemoveAndCopyValue(Player, Recovery)) GetWorldTimerManager().ClearTimer(Recovery.Timer);
	bool bNormalSpawn = true;
#if !UE_BUILD_SHIPPING
	bNormalSpawn = !bSmokeTest;
#endif
	if (bNormalSpawn && bProvideNearbySpawnPickup && !bEndingPvp && HasAuthority()) SpawnNearbyWeaponPickup(Cast<APlayerController>(NewPlayer));
#if !UE_BUILD_SHIPPING
	// 自动发枪仅是联网回归的显式前置；正常玩法必须经过场景拾取，不能修改出生装备规则。
	if (bSmokeTest && !bEndingPvp && HasAuthority()) EquipDefaultWeapon(Cast<APlayerController>(NewPlayer));
	const AfpstrueCharacter* Character = IsValid(NewPlayer) ? Cast<AfpstrueCharacter>(NewPlayer->GetPawn()) : nullptr;
	if (bSmokeTest && !bEndingPvp && HasAuthority() && IsValid(Character) && !Character->IsDead() && Character->HasEquippedWeapon())
	{
		SmokeSpawnOrigins.Add(TWeakObjectPtr<AfpstrueCharacter>(const_cast<AfpstrueCharacter*>(Character)), Character->GetActorLocation());
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Spawn player=%s pawn=%s weapon=%s"), *GetNameSafe(NewPlayer), *Character->GetName(), *GetNameSafe(Character->GetEquippedWeaponComponent()));
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_CONTEXT PC=%s PS=%s PSRep=%d PawnPS=%s GS=%s Players=%d"),
			*GetNameSafe(NewPlayer), *GetNameSafe(NewPlayer->PlayerState), NewPlayer->PlayerState != nullptr && NewPlayer->PlayerState->GetIsReplicated(),
			*GetNameSafe(Character->GetPlayerState()), *GetNameSafe(GameState), GameState != nullptr ? GameState->PlayerArray.Num() : -1);
	}
#endif
	// 装备/补给的蓝图构造也是外部回调；即时死亡归正常重生事务，不能发布虚假的Pawn就绪。
	if (!IsValid(Pawn) || Pawn->IsDead() || Player->GetPawn() != Pawn) return;
	if (auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player))
	{
		if (FRejoinSlot* Slot = RejoinSlots.Find(PvpPlayer->AssignedRejoinToken))
		{
			Slot->Pawn = Pawn;
			Slot->Weapon = Pawn->HasEquippedWeapon() ? Pawn->GetEquippedWeaponComponent()->GetOwner() : nullptr;
			Slot->bDeathRecorded = false;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_PAWN_READY Player=%s Pawn=%s PawnReady=1"), *Player->GetName(), *Pawn->GetName());
}

// 每个控制器至多登记一把尚未拾取的出生补给；重生/离场只删除无主补给，不删除别人已拾走或死亡掉落的枪。
void AfpstruePvpGameMode::ReleaseUnclaimedSpawnPickup(APlayerController* Player)
{
	TWeakObjectPtr<AActor> Previous;
	SpawnPickups.RemoveAndCopyValue(Player, Previous);
	if (AActor* Actor = Previous.Get())
		if (const UfpstrueWeaponComponent* Weapon = Actor->FindComponentByClass<UfpstrueWeaponComponent>();
			Weapon && !Actor->GetOwner() && !Weapon->GetOwningCharacter() && !Weapon->IsDroppedOnDeath()) Actor->Destroy();
}

// 找到近处同层可走到的地面，并检查中间没有墙；找不到就不生成，不把枪丢进深渊或隔墙另一侧。
void AfpstruePvpGameMode::SpawnNearbyWeaponPickup(APlayerController* Player)
{
	ReleaseUnclaimedSpawnPickup(Player);
	AfpstrueCharacter* Pawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	UWorld* World = GetWorld();
	if (!HasAuthority() || bEndingPvp || !IsValid(Pawn) || Pawn->IsDead() || Pawn->HasEquippedWeapon() || !DefaultWeaponClass || !World) return;
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!Navigation) return;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueSpawnPickup), false, Pawn);
	const FVector Forward = Pawn->GetActorForwardVector();
	const FVector Right = Pawn->GetActorRightVector();
	for (const FVector Offset : {Forward * 210.0, Right * 210.0, -Right * 210.0, -Forward * 210.0})
	{
		FNavLocation Floor;
		if (!Navigation->ProjectPointToNavigation(Pawn->GetActorLocation() + Offset, Floor, FVector(60, 60, 150))) continue;
		FHitResult Ground;
		if (!World->LineTraceSingleByChannel(Ground, Floor.Location + FVector(0, 0, 150), Floor.Location - FVector(0, 0, 100), ECC_Visibility, Query)
			|| Ground.ImpactNormal.Z < 0.7f || FMath::Abs(Ground.ImpactPoint.Z - (Pawn->GetActorLocation().Z - Pawn->GetSimpleCollisionHalfHeight())) > 100.0) continue;
		const FVector Location = Ground.ImpactPoint + FVector(0, 0, 45);
		FHitResult Wall;
		if (World->SweepSingleByChannel(Wall, Pawn->GetActorLocation(), Location, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(12.0f), Query)) continue;
		// 同伴胶囊通常忽略 Visibility，却会阻挡玩家；补给不能放在近处同伴背后让新玩家一直顶着人走。
		if (World->SweepSingleByObjectType(Wall, Pawn->GetActorLocation(), Location, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
			FCollisionShape::MakeSphere(Pawn->GetSimpleCollisionRadius()), Query)) continue;
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AActor* Pickup = World->SpawnActor<AActor>(DefaultWeaponClass, Location, FRotator(0, Pawn->GetActorRotation().Yaw, 0), Spawn))
		{
			Pickup->SetReplicates(true);
			SpawnPickups.Add(Player, Pickup);
			UE_LOG(LogTemp, Display, TEXT("PVP_SPAWN_PICKUP Player=%s Pickup=%s DistanceCm=%.1f Grounded=1 NeedsOverlap=1"),
				*Player->GetName(), *Pickup->GetName(), FVector::Dist2D(Pawn->GetActorLocation(), Location));
		}
		return;
	}
}

// 死亡资格和待重生记录先提交，再计分；外部回调不能重复接纳同一次死亡。
void AfpstruePvpGameMode::HandlePlayerEliminated(AfpstrueCharacter* Victim, AController* Killer)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Victim) || !Victim->IsDead()) return;
	if (HandleRetainedPawnEliminated(Victim, Killer)) return;
	APlayerController* Player = Cast<APlayerController>(Victim->GetController());
	const TWeakObjectPtr<APlayerController> PlayerKey(Player);
	if (!IsValid(Player) || Player->GetPawn() != Victim || PendingRespawns.Contains(PlayerKey)) return;
	AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState);
	if (State == nullptr || !State->IsMatchActive()) return;
	FTimerHandle& RespawnTimer = PendingRespawns.Add(PlayerKey);
	AfpstruePvpPlayerState* VictimState = Player->GetPlayerState<AfpstruePvpPlayerState>();
#if !UE_BUILD_SHIPPING
	const int32 PreviousDeaths = VictimState != nullptr ? VictimState->GetDeaths() : -1;
#endif
	if (VictimState != nullptr) VictimState->AddDeath();
	AfpstruePvpPlayerState* KillerState = nullptr;
	if (IsValid(Killer) && Killer != Player && Killer->GetWorld() == GetWorld())
	{
		if (APlayerController* KillerPlayer = Cast<APlayerController>(Killer))
		{
			KillerState = KillerPlayer->GetPlayerState<AfpstruePvpPlayerState>();
			if (KillerState != nullptr)
			{
#if !UE_BUILD_SHIPPING
				const int32 PreviousKills = KillerState->GetKills();
#endif
				KillerState->AddKill();
#if !UE_BUILD_SHIPPING
				bSmokeSawPlayerKill |= KillerState->GetKills() == PreviousKills + 1;
#endif
			}
		}
	}
	State->AddElimination();
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")))
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY SERVER_DEATH Victim=%s Deaths=%d Killer=%s Eliminations=%d"),
			*Victim->GetName(), VictimState ? VictimState->GetDeaths() : -1, *GetNameSafe(Killer), State->GetEliminationCount());
	if (bSmokeTest && VictimState != nullptr && VictimState->GetDeaths() == PreviousDeaths + 1)
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Death victim=%s deaths=%d killer=%s kills=%d eliminations=%d"),
			*Player->GetName(), VictimState->GetDeaths(), *GetNameSafe(Killer), KillerState != nullptr ? KillerState->GetKills() : 0, State->GetEliminationCount());
#endif
	const TWeakObjectPtr<AfpstrueCharacter> EliminatedPawn(Victim);
	const float Delay = FMath::IsFinite(RespawnDelay) ? FMath::Max(RespawnDelay, 0.1f) : 3.0f;
	GetWorldTimerManager().SetTimer(RespawnTimer, FTimerDelegate::CreateWeakLambda(this, [this, PlayerKey, EliminatedPawn]()
	{
		RespawnPlayer(PlayerKey, EliminatedPawn);
	}), Delay, false);
}

// 弱引用和旧Pawn身份共同保护重生；已被外部换成新角色时只丢弃旧Timer。
void AfpstruePvpGameMode::RespawnPlayer(TWeakObjectPtr<APlayerController> PlayerKey, TWeakObjectPtr<AfpstrueCharacter> EliminatedPawn)
{
	FTimerHandle Timer;
	if (!PendingRespawns.RemoveAndCopyValue(PlayerKey, Timer)) return;
	GetWorldTimerManager().ClearTimer(Timer);
	APlayerController* Player = PlayerKey.Get();
	if (bEndingPvp || !HasAuthority() || !IsValid(Player)) return;
	APawn* OldPawn = Player->GetPawn();
	if (OldPawn != nullptr && OldPawn != EliminatedPawn.Get()) return;
	ReleasePlayerWeapon(Player);
	if (OldPawn != nullptr)
	{
		Player->UnPossess();
		OldPawn->Destroy();
	}
	RestartPlayer(Player);
#if !UE_BUILD_SHIPPING
	const AfpstrueCharacter* NewPawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")) && NewPawn && !NewPawn->IsDead())
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY SERVER_RESPAWN Pawn=%s Health=%.1f EmptyHands=%d"),
			*NewPawn->GetName(), NewPawn->GetCurrentHealth(), !NewPawn->HasEquippedWeapon());
	if (bSmokeTest && IsValid(NewPawn) && NewPawn != EliminatedPawn.Get() && !NewPawn->IsDead() && NewPawn->HasEquippedWeapon())
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Respawn player=%s pawn=%s"), *Player->GetName(), *NewPawn->GetName());
		const AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState);
		if (!bSmokeFinalReported && bSmokeSawPlayerKill && State != nullptr && State->GetScoreboard().Num() >= 2)
		{
			bSmokeFinalReported = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Final players=%d eliminations=%d"), State->GetScoreboard().Num(), State->GetEliminationCount());
		}
	}
#endif
}

// 默认武器是独立Actor，网络、碰撞和Owner先就绪，再完成蓝图构造及完整装备。
void AfpstruePvpGameMode::EquipDefaultWeapon(APlayerController* Player)
{
	AfpstrueCharacter* Character = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	if (!HasAuthority() || !IsValid(Character) || Character->IsDead() || Character->HasEquippedWeapon() || !DefaultWeaponClass) return;
	ReleasePlayerWeapon(Player);
	const FTransform Transform = Character->GetActorTransform();
	AActor* Weapon = GetWorld()->SpawnActorDeferred<AActor>(DefaultWeaponClass, Transform, Character, Character,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Weapon == nullptr) return;
	Weapon->SetReplicateMovement(true);
	Weapon->SetActorEnableCollision(false);
	UGameplayStatics::FinishSpawningActor(Weapon, Transform);
	if (!IsValid(Weapon)) return;
	Weapon->SetReplicates(true);
	UfpstrueWeaponComponent* Component = Weapon->FindComponentByClass<UfpstrueWeaponComponent>();
	// 旧蓝图模板可能序列化过关闭复制；只改原生构造默认值不足以覆盖已保存资产。
	if (Component != nullptr) Component->SetIsReplicated(true);
	if (Component == nullptr || !Component->AttachWeapon(Character) || !IsValid(Player) || Player->GetPawn() != Character || Character->IsDead())
	{
		UE_LOG(LogTemp, Warning, TEXT("PvP default weapon could not equip on %s."), *GetNameSafe(Character));
		Weapon->Destroy();
		return;
	}
	PlayerWeapons.Add(TWeakObjectPtr<APlayerController>(Player), TWeakObjectPtr<AActor>(Weapon));
	Weapon->ForceNetUpdate();
}

// 外部武器Actor不由Pawn销毁自动回收，所以由对局在换Pawn和离场时显式释放。
void AfpstruePvpGameMode::ReleasePlayerWeapon(APlayerController* Player)
{
	TWeakObjectPtr<AActor> RecordedWeapon;
	PlayerWeapons.RemoveAndCopyValue(TWeakObjectPtr<APlayerController>(Player), RecordedWeapon);
	AfpstrueCharacter* Pawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	UfpstrueWeaponComponent* Equipped = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	AActor* CurrentWeapon = IsValid(Equipped) && Equipped->GetOwningCharacter() == Pawn ? Equipped->GetOwner() : nullptr;
	// ★【资源所有权】场景拾取不经过默认发枪记录；以当前装备补齐清理，不能留下已脱离旧 Pawn 的枪。
	// 记录先移除，实际引用先捕获；销毁回调不能重新消费旧记录，也不能误删已交给另一位玩家的武器。
	AActor* RecordedActor = RecordedWeapon.Get();
	ReleaseNetworkPlayerWeapon(RecordedActor, Pawn, Player);
	if (CurrentWeapon != RecordedActor) ReleaseNetworkPlayerWeapon(CurrentWeapon, Pawn, Player);
}

// 在引擎预登录检查后限制本局连接和待握手容量；此时不接纳或验证恢复票据。
void AfpstruePvpGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (ErrorMessage.IsEmpty() && (bEndingPvp || PendingConnectionHandshakes.Num() >= MaxPendingHandshakes || GetNumPlayers() >= MaxRejoinSlots + MaxPendingHandshakes))
		ErrorMessage = TEXT("connection_capacity_reached");
}

// 先登记五秒握手期限，握手完成后才恢复或生成 Pawn；监听主机的本地玩家直接进入同一入口。
void AfpstruePvpGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	auto* Player = Cast<AfpstruePvpPlayerController>(NewPlayer);
	if (!IsValid(Player) || bEndingPvp) return;
	PendingConnectionHandshakes.Add(Player, GetWorld()->GetTimeSeconds() + 5.0);
	if (!GetWorldTimerManager().IsTimerActive(RejoinCleanupTimer))
		GetWorldTimerManager().SetTimer(RejoinCleanupTimer, this, &ThisClass::SweepRejoinSlots, 0.5f, true);
	if (Player->IsLocalController()) CompleteConnectionHandshake(Player, FString());
	else Player->ClientBeginConnectionHandshake();
}

// 【设计取舍】恢复服务器保留的 Pawn/PlayerState，不接纳客户端上传血量、弹药或成绩；离线仍可受伤，死亡则重生。
// 先消费旧票据、占用槽位并轮换票据，再 Possess/通知，防止并发或重入重复认领同一身份。
// 票据只证明会话持有权，不证明客户端状态可信；保留表和超时释放的资源成本由服务器承担。
void AfpstruePvpGameMode::CompleteConnectionHandshake(AfpstruePvpPlayerController* Player, const FString& Token)
{
	if (!HasAuthority() || bEndingPvp || !IsValid(Player) || Player->bConnectionHandshakeComplete) return;
	const double* Deadline = PendingConnectionHandshakes.Find(Player);
	const double Now = GetWorld()->GetTimeSeconds();
	if (Deadline == nullptr || *Deadline < Now) return;
	FRejoinSlot* Existing = Token.IsEmpty() ? nullptr : RejoinSlots.Find(Token);
	const bool bRestored = Existing != nullptr;
	FString Error;
	if (!Token.IsEmpty() && (!IsResumeToken(Token) || Existing == nullptr)) Error = TEXT("resume_not_available");
	else if (Existing != nullptr && (!Existing->bDisconnected || Existing->Controller.IsValid())) Error = TEXT("resume_slot_active");
	else if (Existing != nullptr && (Existing->ExpiresAt <= Now || !Existing->State.IsValid())) Error = TEXT("resume_expired");
	else if (Existing == nullptr && RejoinSlots.Num() >= MaxRejoinSlots) Error = TEXT("session_capacity_reached");
	const FString NewToken = Error.IsEmpty() ? GenerateResumeToken() : FString();
	if (Error.IsEmpty() && NewToken.IsEmpty()) Error = TEXT("secure_ticket_unavailable");
	if (!Error.IsEmpty())
	{
		// 只记录原因，不输出票据；区分正常过期、并发占用与真正的恢复链路错误。
		UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_REJECT Reason=%s"), *Error);
		PendingConnectionHandshakes.Remove(Player);
		Player->bIntentionalLeave = true;
		Player->ClientConnectionRejected(Error);
		if (GameSession != nullptr) GameSession->KickPlayer(Player, FText::FromString(TEXT("Connection rejected.")));
		else Player->Destroy();
		return;
	}
	// 先消费旧票据并占用槽位，再做 Possess/委托，重入不能把同一份保留状态交给第二个玩家。
	FRejoinSlot Slot;
	if (bRestored) RejoinSlots.RemoveAndCopyValue(Token, Slot);
	else Slot.State = Player->GetPlayerState<AfpstruePvpPlayerState>();
	Slot.Controller = Player;
	Slot.bDisconnected = false;
	Slot.ExpiresAt = 0.0;
	PendingConnectionHandshakes.Remove(Player);
	Player->bConnectionHandshakeComplete = true;
	Player->AssignedRejoinToken = NewToken;
	RejoinSlots.Add(NewToken, Slot);
	AfpstruePvpPlayerState* State = Slot.State.Get();
	if (bRestored && State != nullptr)
	{
		APlayerState* TemporaryState = Player->PlayerState;
		Player->SetPlayerState(State);
		State->SetOwner(Player);
		State->SetRetainedForReconnect(false);
		if (IsValid(TemporaryState) && TemporaryState != State) TemporaryState->Destroy();
	}
	AfpstrueCharacter* Pawn = Slot.Pawn.Get();
	UfpstrueWeaponComponent* RetainedWeapon = IsValid(Pawn) ? Pawn->GetEquippedWeaponComponent() : nullptr;
	const bool bValidEquipment = RetainedWeapon == nullptr || (IsValid(RetainedWeapon)
		&& RetainedWeapon->GetOwningCharacter() == Pawn && IsValid(RetainedWeapon->GetOwner()));
	if (bRestored && IsValid(Pawn) && !Pawn->IsDead() && bValidEquipment)
	{
		Player->Possess(Pawn);
		Pawn->GetCharacterMovement()->bRunPhysicsWithNoController = Slot.bPreviousPhysicsWithoutController;
		Pawn->GetAbilitySystemComponent()->InitAbilityActorInfo(Pawn, Pawn);
		// 空手不需要制造一把枪来完成恢复；拾取枪以 Pawn 的实际装备为准，不依赖默认发枪登记。
		if (RetainedWeapon != nullptr)
		{
			RetainedWeapon->ResetNetworkInputGeneration();
			AActor* WeaponActor = RetainedWeapon->GetOwner();
			WeaponActor->SetOwner(Pawn);
			PlayerWeapons.Add(Player, WeaponActor);
			WeaponActor->ForceNetUpdate();
		}
		Player->SetControlRotation(Slot.ViewRotation);
		Player->ClientSetRotation(Slot.ViewRotation, true);
		Pawn->ForceNetUpdate();
	}
	else
	{
		// 离线期间死亡或 Pawn/装备已被销毁，只恢复服务器当前成绩并按重生规则生成，不回滚到离线前满血。
		ReleaseNetworkPlayerWeapon(Slot.Weapon.Get(), Pawn);
		if (Pawn != nullptr) Pawn->Destroy();
		RestartPlayer(Player);
		Pawn = Cast<AfpstrueCharacter>(Player->GetPawn());
	}
	// 已耗尽的出生可能同步拒绝连接；同一个握手调用栈不能随后再发送Accepted。
	if (Player->bIntentionalLeave) return;
	if (FRejoinSlot* Current = RejoinSlots.Find(NewToken))
	{
		Current->Pawn = Pawn;
		Current->State = Player->GetPlayerState<AfpstruePvpPlayerState>();
		Current->Weapon = Pawn != nullptr && Pawn->GetEquippedWeaponComponent() != nullptr ? Pawn->GetEquippedWeaponComponent()->GetOwner() : nullptr;
		Current->bDeathRecorded = false;
	}
	State = Player->GetPlayerState<AfpstruePvpPlayerState>();
	const UfpstrueWeaponComponent* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	Player->ClientConnectionAccepted(NewToken, State != nullptr ? State->GetPlayerId() : INDEX_NONE, bRestored);
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_ACCEPT ResumeId=%d Restored=%d Pawn=%s PS=%s Health=%.1f Ammo=%d Kills=%d Deaths=%d Dead=%d PawnNetGuid=%llu StateNetGuid=%llu PawnReady=%d"),
		State != nullptr ? State->GetPlayerId() : INDEX_NONE, bRestored, *GetNameSafe(Pawn), *GetNameSafe(State), Pawn != nullptr ? Pawn->GetCurrentHealth() : 0.0f,
		Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0, State != nullptr ? State->GetKills() : 0, State != nullptr ? State->GetDeaths() : 0, Pawn == nullptr || Pawn->IsDead(),
		GetResumeNetGuid(Pawn), GetResumeNetGuid(State), IsValid(Pawn) && !Pawn->IsDead() && Pawn->GetController() == Player);
}

// 引擎 PawnLeavingGame 之前保留真实对象；没有 Controller 不代表无敌，生命和骨骼碰撞照常工作。
bool AfpstruePvpGameMode::PreserveDisconnectedPlayer(AfpstruePvpPlayerController* Player)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Player) || Player->bIntentionalLeave || !Player->bConnectionHandshakeComplete) return false;
	FRejoinSlot* Slot = RejoinSlots.Find(Player->AssignedRejoinToken);
	AfpstruePvpPlayerState* State = Player->GetPlayerState<AfpstruePvpPlayerState>();
	if (Slot == nullptr || Slot->Controller.Get() != Player || State == nullptr) return false;
	if (Slot->bDisconnected) return true;
	AfpstrueCharacter* Pawn = Cast<AfpstrueCharacter>(Player->GetPawn());
	Slot->bDisconnected = true;
	Slot->ExpiresAt = GetWorld()->GetTimeSeconds() + FMath::Clamp(ReconnectGraceSeconds, 5.0f, 120.0f);
	Slot->Pawn = Pawn;
	Slot->State = State;
	Slot->bDeathRecorded = Pawn != nullptr && Pawn->IsDead();
	Slot->ViewRotation = Player->GetControlRotation();
	TWeakObjectPtr<AActor> RecordedWeapon;
	PlayerWeapons.RemoveAndCopyValue(Player, RecordedWeapon);
	UfpstrueWeaponComponent* Equipped = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	Slot->Weapon = IsValid(Equipped) && Equipped->GetOwningCharacter() == Pawn ? Equipped->GetOwner() : nullptr;
	// 装备可能来自场景拾取而不是默认发枪；只保留当前枪，失效旧记录不能覆盖真实装备。
	if (AActor* PreviousWeapon = RecordedWeapon.Get(); PreviousWeapon != nullptr && PreviousWeapon != Slot->Weapon.Get())
	{
		const auto* PreviousComponent = PreviousWeapon->FindComponentByClass<UfpstrueWeaponComponent>();
		if (PreviousComponent == nullptr || PreviousComponent->GetOwningCharacter() == nullptr)
			ReleaseNetworkPlayerWeapon(PreviousWeapon, Pawn, Player);
	}
	FTimerHandle Timer;
	if (PendingRespawns.RemoveAndCopyValue(Player, Timer)) GetWorldTimerManager().ClearTimer(Timer);
	State->SetRetainedForReconnect(true);
	State->SetOwner(nullptr);
	if (Pawn != nullptr)
	{
		if (UAbilitySystemComponent* ASC = Pawn->GetAbilitySystemComponent()) ASC->CancelAllAbilities();
		if (auto* Weapon = Pawn->GetEquippedWeaponComponent()) Weapon->InterruptOwnerInput();
		Slot->bPreviousPhysicsWithoutController = Pawn->GetCharacterMovement()->bRunPhysicsWithNoController;
		Pawn->GetCharacterMovement()->StopMovementImmediately();
		Pawn->GetCharacterMovement()->bRunPhysicsWithNoController = true;
		Player->UnPossess();
		Pawn->SetPlayerState(State);
		Pawn->ForceNetUpdate();
	}
	const UfpstrueWeaponComponent* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_RETAIN ResumeId=%d Pawn=%s PS=%s Health=%.1f Ammo=%d Kills=%d Deaths=%d Dead=%d PawnNetGuid=%llu StateNetGuid=%llu"),
		State->GetPlayerId(), *GetNameSafe(Pawn), *GetNameSafe(State), Pawn != nullptr ? Pawn->GetCurrentHealth() : 0.0f,
		Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0, State->GetKills(), State->GetDeaths(), Pawn == nullptr || Pawn->IsDead(),
		GetResumeNetGuid(Pawn), GetResumeNetGuid(State));
	return true;
}

// 判断 Pawn 是否仍属于未过期的离线保留槽，供 AI 候选资格与离线死亡结算复用。
bool AfpstruePvpGameMode::IsRetainedDisconnectedPawn(const AfpstrueCharacter* Pawn) const
{
	if (Pawn == nullptr) return false;
	for (const auto& Entry : RejoinSlots)
		if (Entry.Value.bDisconnected && Entry.Value.ExpiresAt > GetWorld()->GetTimeSeconds() && Entry.Value.Pawn.Get() == Pawn) return true;
	return false;
}

// 为没有 Controller 的保留 Pawn 结算一次死亡；返回 true 表示已由离线槽接管，无需普通重生路径。
bool AfpstruePvpGameMode::HandleRetainedPawnEliminated(AfpstrueCharacter* Pawn, AController* Killer)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Pawn)) return false;
	for (auto& Entry : RejoinSlots)
	{
		FRejoinSlot& Slot = Entry.Value;
		if (!Slot.bDisconnected || Slot.Pawn.Get() != Pawn) continue;
		if (Slot.bDeathRecorded || Pawn == nullptr || !Pawn->IsDead()) return true;
		Slot.bDeathRecorded = true;
		if (auto* State = Slot.State.Get()) State->AddDeath();
		if (auto* KillerPlayer = Cast<APlayerController>(Killer))
			if (auto* State = KillerPlayer->GetPlayerState<AfpstruePvpPlayerState>()) if (State != Slot.State.Get()) State->AddKill();
		if (auto* State = Cast<AfpstruePvpGameState>(GameState)) State->AddElimination();
		return true;
	}
	return false;
}

// 到期先移出握手/保留表再销毁对象；清理回调不能继续认领已经过期的恢复身份。
void AfpstruePvpGameMode::SweepRejoinSlots()
{
	const double Now = GetWorld()->GetTimeSeconds();
	TArray<TWeakObjectPtr<AfpstruePvpPlayerController>> TimedOut;
	for (const auto& Entry : PendingConnectionHandshakes) if (!Entry.Key.IsValid() || Entry.Value <= Now) TimedOut.Add(Entry.Key);
	for (const auto& Key : TimedOut)
	{
		PendingConnectionHandshakes.Remove(Key);
		if (auto* Player = Key.Get()) { Player->bIntentionalLeave = true; Player->ClientConnectionRejected(TEXT("handshake_timeout")); Player->Destroy(); }
	}
	TArray<FRejoinSlot> Expired;
	for (auto It = RejoinSlots.CreateIterator(); It; ++It)
		if (It.Value().bDisconnected && (It.Value().ExpiresAt <= Now || !It.Value().State.IsValid())) { Expired.Add(It.Value()); It.RemoveCurrent(); }
	for (const FRejoinSlot& Slot : Expired)
	{
		ReleaseNetworkPlayerWeapon(Slot.Weapon.Get(), Slot.Pawn.Get());
		if (auto* Pawn = Slot.Pawn.Get()) Pawn->Destroy();
		if (auto* State = Slot.State.Get()) { State->SetRetainedForReconnect(false); State->Destroy(); }
	}
}

// 对局退出时撤销握手与清扫 Timer，回收离线保留对象；在线玩家仍走正常离场清理。
void AfpstruePvpGameMode::ClearRejoinSlots()
{
	GetWorldTimerManager().ClearTimer(RejoinCleanupTimer);
	PendingConnectionHandshakes.Reset();
	const auto EndingSlots = MoveTemp(RejoinSlots);
	RejoinSlots.Reset();
	for (const auto& Entry : EndingSlots)
	{
		if (!Entry.Value.bDisconnected) continue;
		ReleaseNetworkPlayerWeapon(Entry.Value.Weapon.Get(), Entry.Value.Pawn.Get(), nullptr, bEndingPvp);
		if (auto* Pawn = Entry.Value.Pawn.Get()) Pawn->Destroy();
		if (auto* State = Entry.Value.State.Get()) { State->SetRetainedForReconnect(false); State->Destroy(); }
	}
}

// 离线先撤销重生资格；已在 PawnLeavingGame 保留的真实对象不在 Logout 再次销毁。
void AfpstruePvpGameMode::Logout(AController* Exiting)
{
	if (APlayerController* Player = Cast<APlayerController>(Exiting))
	{
		FSpawnRecovery Recovery;
		if (SpawnRecoveries.RemoveAndCopyValue(Player, Recovery)) GetWorldTimerManager().ClearTimer(Recovery.Timer);
		ReleaseUnclaimedSpawnPickup(Player);
		FTimerHandle Timer;
		if (PendingRespawns.RemoveAndCopyValue(TWeakObjectPtr<APlayerController>(Player), Timer)) GetWorldTimerManager().ClearTimer(Timer);
		PendingConnectionHandshakes.Remove(Cast<AfpstruePvpPlayerController>(Player));
		const auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player);
		FRejoinSlot* Slot = PvpPlayer != nullptr ? RejoinSlots.Find(PvpPlayer->AssignedRejoinToken) : nullptr;
		if (Slot == nullptr || !Slot->bDisconnected)
		{
			if (PvpPlayer != nullptr) RejoinSlots.Remove(PvpPlayer->AssignedRejoinToken);
			ReleasePlayerWeapon(Player);
		}
	}
	Super::Logout(Exiting);
}

// 退出标记先于Timer和装备清理，销毁回调不能再创建Pawn或累加成绩。
void AfpstruePvpGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPvp = true;
	StopPve();
	ClearRejoinSlots();
#if !UE_BUILD_SHIPPING
	GetWorldTimerManager().ClearTimer(SmokeMovementTimer);
	SmokeSpawnOrigins.Reset();
	SmokeMovedPlayers.Reset();
#endif
	if (AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState)) State->SetMatchActive(false);
	for (auto& Entry : PendingRespawns) GetWorldTimerManager().ClearTimer(Entry.Value);
	PendingRespawns.Reset();
	for (auto& Entry : SpawnRecoveries) GetWorldTimerManager().ClearTimer(Entry.Value.Timer);
	SpawnRecoveries.Reset();
	const auto EndingPickups = MoveTemp(SpawnPickups);
	SpawnPickups.Reset();
	for (const auto& Entry : EndingPickups)
		if (AActor* Pickup = Entry.Value.Get(); Pickup && !Pickup->GetOwner()) Pickup->Destroy();
	const TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AActor>> EndingWeapons = MoveTemp(PlayerWeapons);
	PlayerWeapons.Reset();
	for (const auto& Entry : EndingWeapons) if (AActor* Weapon = Entry.Value.Get()) Weapon->Destroy();
	Super::EndPlay(EndPlayReason);
}

FString UfpstrueMultiplayerGameInstance::ResolvePvpMapPackage() const
{
	const FString Map = PvpMap.TrimStartAndEnd();
	return FPackageName::ObjectPathToPackageName(Map.IsEmpty()
		? UGameMapsSettings::GetGameDefaultMap(EDefaultMapRequestType::Client) : Map);
}

// 只接纳包路径作为地图，URL中的PvP模式和监听选项由本入口固定生成。
void UfpstrueMultiplayerGameInstance::HostPvp()
{
	const FString Map = ResolvePvpMapPackage();
	if (GetWorld() == nullptr || !FPackageName::IsValidLongPackageName(Map))
	{
		UE_LOG(LogTemp, Warning, TEXT("HostPvp requires a valid game map package, got %s."), *Map);
		return;
	}
	UGameplayStatics::OpenLevel(this, FName(*Map), true, TEXT("listen?game=/Script/fpstrue.fpstruePvpGameMode"));
}

// 只让显式回环基线接管启动 Browse；普通入口仍走引擎启动流程，诊断失败不静默回退。
void UfpstrueMultiplayerGameInstance::StartGameInstance()
{
	const EFPPvpBaselineBootstrap Bootstrap = StartPvpBaselineClient(this);
	if (Bootstrap == EFPPvpBaselineBootstrap::NotRequested)
	{
		Super::StartGameInstance();
		return;
	}
	// 仅已成功发起的 Browse 广播启动完成；拒绝的诊断不发布成功事件。
	if (Bootstrap == EFPPvpBaselineBootstrap::Started) BroadcastOnStart();
}

// 连接监督器属于 GameInstance：Ticker 跨 World 存活，旧操作只有编号匹配才可以推进下一次旅行。
void UfpstrueMultiplayerGameInstance::Init()
{
	Super::Init();
	if (IsRunningDedicatedServer()) return;
	FString PrivateUserDir;
	const bool bExplicitUserDir = FParse::Value(FCommandLine::Get(), TEXT("UserDir="), PrivateUserDir);
	const FString SavedRoot = bExplicitUserDir || FPaths::ShouldSaveToUserDir() ? FPaths::ProjectSavedDir()
		: FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("fpstrue"), TEXT("Saved"));
	ResumeCacheFile = FPaths::Combine(SavedRoot, TEXT("Private"), TEXT("PvpResume.dat"));
	LoadResumeCache();
	if (GEngine != nullptr)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &ThisClass::HandleTravelFailure);
	}
	PostLoadHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);
	ConnectionTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ThisClass::TickConnection), 0.1f);
}

// 终止当前连接操作并解除跨世界委托/Ticker；不在退出期间启动重连。
void UfpstrueMultiplayerGameInstance::Shutdown()
{
	++OperationRevision;
	FTSTicker::GetCoreTicker().RemoveTicker(ConnectionTicker);
	if (GEngine != nullptr)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadHandle);
	Super::Shutdown();
}

// 本地连接状态的统一提交入口；UI 通过 GetConnectionState 查询，不保留未订阅的广播。
void UfpstrueMultiplayerGameInstance::SetConnectionState(EFPConnectionState State)
{
	ConnectionState = State;
}

// 缓存只含服务器地址和 bearer 票据，位于每用户目录；测试 UserDir 的 owner-only 权限由启动脚本建立。
// 同目录临时文件写完再替换，进程被强停不能留下半个新票据；绝不把内容写进日志或公共 Config。
void UfpstrueMultiplayerGameInstance::SaveResumeCache()
{
	if (ResumeCacheFile.IsEmpty() || !IsResumeToken(ResumeToken)) return;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResumeCacheFile), true);
	const FString Temporary = ResumeCacheFile + TEXT(".tmp");
	const FString Content = FString::Printf(TEXT("FPSTRUE_RESUME_1\n%s\n%s\n%lld\n"), *ResumeServerAddress, *ResumeToken,
		(FDateTime::UtcNow() + FTimespan::FromDays(1.0)).GetTicks());
	if (!FFileHelper::SaveStringToFile(Content, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*ResumeCacheFile, *Temporary, true, false, false, true))
		UE_LOG(LogTemp, Warning, TEXT("Resume cache could not be saved; reconnect is available only while this process remains alive."));
}

// 读取尺寸、版本、格式及本地到期时间合格的缓存；真正恢复资格仍由服务器验证。
void UfpstrueMultiplayerGameInstance::LoadResumeCache()
{
	if (ResumeCacheFile.IsEmpty() || IFileManager::Get().FileSize(*ResumeCacheFile) > 4096) return;
	FString Content;
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToString(Content, *ResumeCacheFile)) return;
	Content.ParseIntoArrayLines(Lines, true);
	if (Lines.Num() != 4 || Lines[0] != TEXT("FPSTRUE_RESUME_1") || !IsResumeToken(Lines[2])
		|| FCString::Atoi64(*Lines[3]) <= FDateTime::UtcNow().GetTicks()) return;
	ResumeServerAddress = Lines[1];
	ResumeToken = Lines[2];
}

// 主动离场或不可恢复拒绝时清除内存票据和本地缓存；不会修改服务器游戏状态。
void UfpstrueMultiplayerGameInstance::ClearResumeCache()
{
	ResumeToken.Reset();
	ResumeServerAddress.Reset();
	if (!ResumeCacheFile.IsEmpty())
	{
		IFileManager::Get().Delete(*ResumeCacheFile, false, true, true);
		IFileManager::Get().Delete(*(ResumeCacheFile + TEXT(".tmp")), false, true, true);
	}
}

// 直连只接纳主机地址和端口，不接受额外地图或对局 URL 选项，更不会把重连票据放进 URL。
void UfpstrueMultiplayerGameInstance::JoinPvp(const FString& Address)
{
	if (ConnectionState != EFPConnectionState::Idle && ConnectionState != EFPConnectionState::Failed) return;
	FString Normalized = Address.TrimStartAndEnd().ToLower();
	if (Normalized.IsEmpty() || Normalized.Len() > 300) return;
	for (TCHAR Character : Normalized)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('.') && Character != TEXT('-') && Character != TEXT('_') &&
			Character != TEXT(':') && Character != TEXT('[') && Character != TEXT(']'))
		{
			UE_LOG(LogTemp, Warning, TEXT("JoinPvp expects a server IP or hostname with optional port."));
			return;
		}
	}
	int32 Colon = INDEX_NONE;
	if (Normalized.StartsWith(TEXT("[")))
	{
		int32 Closing;
		if (!Normalized.FindChar(TEXT(']'), Closing) || Closing <= 1) return;
		if (Closing == Normalized.Len() - 1) Normalized += TEXT(":7777");
		Colon = Closing + 1;
		if (!Normalized.IsValidIndex(Colon) || Normalized[Colon] != TEXT(':')) return;
	}
	else
	{
		if (!Normalized.FindLastChar(TEXT(':'), Colon)) { Normalized += TEXT(":7777"); Normalized.FindLastChar(TEXT(':'), Colon); }
		if (Normalized.Left(Colon).Contains(TEXT(":")) || Colon <= 0) return;
	}
	const FString Port = Normalized.Mid(Colon + 1);
	if (Port.IsEmpty() || Port.Len() > 5) return;
	for (TCHAR Digit : Port) if (!FChar::IsDigit(Digit)) return;
	if (FCString::Atoi(*Port) < 1 || FCString::Atoi(*Port) > 65535) return;
	ServerAddress = Normalized;
	LastConnectionError.Reset();
	RetryCount = 0;
	bAutoReconnect = true;
	const uint32 Revision = ++OperationRevision;
	StartClientTravel(Revision);
}

// 仅当前操作发起连接旅行并设置期限；状态广播返回后复核，避免 UI 取消后继续旅行。
void UfpstrueMultiplayerGameInstance::StartClientTravel(uint32 Revision)
{
	if (Revision != OperationRevision || !bAutoReconnect || ServerAddress.IsEmpty()) return;
	APlayerController* Player = GetFirstLocalPlayerController();
	if (Player == nullptr) { LastConnectionError = TEXT("Local player is not ready."); SetConnectionState(EFPConnectionState::Failed); return; }
	ConnectingController.Reset();
	AttemptNetDriver.Reset();
	bReturningToLocalWorld = false;
	AttemptDeadline = FPlatformTime::Seconds() + 20.0;
	SetConnectionState(EFPConnectionState::Connecting);
	if (Revision == OperationRevision && ConnectionState == EFPConnectionState::Connecting) Player->ClientTravel(ServerAddress, TRAVEL_Absolute);
}

// 记录当前 Controller/NetDriver 并进入 WaitingReady；只向同地址服务器提交缓存票据。
void UfpstrueMultiplayerGameInstance::BeginConnectionHandshake(AfpstruePvpPlayerController* Player)
{
	if (!IsValid(Player) || !Player->IsLocalController() || Player->GetWorld() != GetWorld()) return;
	if (ConnectionState == EFPConnectionState::Leaving) { Player->ServerLeavePvp(); return; }
	if (ConnectionState == EFPConnectionState::Idle || ConnectionState == EFPConnectionState::Failed)
	{
		ServerAddress = GetWorld()->URL.Host.ToLower() + FString::Printf(TEXT(":%d"), GetWorld()->URL.Port);
		++OperationRevision;
		RetryCount = 0;
		bAutoReconnect = true;
	}
	ConnectingController = Player;
	AttemptNetDriver = GetWorld()->GetNetDriver();
	AttemptDeadline = FPlatformTime::Seconds() + 20.0;
	const uint32 HandshakeRevision = OperationRevision;
	SetConnectionState(EFPConnectionState::WaitingReady);
	if (HandshakeRevision == OperationRevision && ConnectionState == EFPConnectionState::WaitingReady && ConnectingController == Player)
		Player->ServerSubmitConnectionToken(ResumeServerAddress.Equals(ServerAddress, ESearchCase::IgnoreCase) ? ResumeToken : FString());
}

// 只接纳当前握手的应答并保存新票据；收到 ACK 不等于 Pawn、成绩和能力已经复制完成。
void UfpstrueMultiplayerGameInstance::AcceptConnectionToken(AfpstruePvpPlayerController* Player, const FString& Token, int32 InResumeId, bool bRestored)
{
	if (!IsValid(Player) || !Player->IsLocalController() || Player->GetWorld() != GetWorld() || !IsResumeToken(Token)
		|| ConnectionState == EFPConnectionState::Leaving) return;
	// 监听主机的本地控制器没有远程握手；只在此明确接纳它，远端旧 ACK 不能重新收养已经失败的流程。
	if (Player->HasAuthority() && GetWorld()->GetNetMode() == NM_ListenServer && Player == GetFirstLocalPlayerController() &&
		ConnectionState == EFPConnectionState::Idle)
	{
		ConnectingController = Player;
		ServerAddress = GetWorld()->URL.Host.ToLower() + FString::Printf(TEXT(":%d"), GetWorld()->URL.Port);
		++OperationRevision;
		SetConnectionState(EFPConnectionState::WaitingReady);
	}
	if (ConnectionState != EFPConnectionState::WaitingReady || ConnectingController != Player) return;
	ResumeToken = Token;
	ResumeServerAddress = ServerAddress;
	ResumeId = InResumeId;
	bRestoredConnection = bRestored;
	SaveResumeCache();
	AttemptDeadline = FPlatformTime::Seconds() + 20.0;
	SetConnectionState(EFPConnectionState::WaitingReady);
}

// 空手与持枪共用生命和能力就绪检查；装备是可选状态，而非连接成功的前置条件。
bool UfpstrueMultiplayerGameInstance::IsPawnStateReady(const AfpstrueCharacter* Pawn, const AfpstruePvpPlayerState* State)
{
	if (!IsValid(Pawn) || Pawn->IsDead() || State == nullptr || Pawn->GetPlayerState() != State) return false;
	const auto* ASC = Pawn->GetHealthComponent();
	const auto* Weapon = Pawn->GetEquippedWeaponComponent();
	return ASC != nullptr && ASC->GetHealthAttributeSet() != nullptr
		&& (Weapon == nullptr || (IsValid(Weapon) && Weapon->GetOwningCharacter() == Pawn))
		&& ASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass()) != nullptr
		&& ASC->FindAbilitySpecFromClass(UfpstrueReloadAbility::StaticClass()) != nullptr
		&& ASC->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass()) != nullptr
		&& ASC->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass()) != nullptr;
}

// 【设计取舍】Login/Accepted 只证明身份接纳；Ready 还等本端 Pawn、ASC、PlayerState 关联和时钟，避免绑定半就绪对象。
// 这会增加必要等待，但不保证 GPU 画面、所有 AI 或全世界同时恢复；完整画面验收另有显式流程。
bool UfpstrueMultiplayerGameInstance::TickConnection(float)
{
	const double Now = FPlatformTime::Seconds();
	if (ConnectionState == EFPConnectionState::RetryWait)
	{
		if (ScheduledRetryRevision != OperationRevision) return true;
		if (Now >= RetryAt && !bReturningToLocalWorld && GetWorld() != nullptr && GetWorld()->GetNetMode() == NM_Standalone)
			StartClientTravel(ScheduledRetryRevision);
		return true;
	}
	if (ConnectionState == EFPConnectionState::Leaving)
	{
		if (Now >= AttemptDeadline) ReturnToLocalWorld();
		return true;
	}
	if (ConnectionState != EFPConnectionState::Connecting && ConnectionState != EFPConnectionState::WaitingReady) return true;
	if (GetWorld() != nullptr && GetWorld()->GetNetDriver() != nullptr) AttemptNetDriver = GetWorld()->GetNetDriver();
	if (Now >= AttemptDeadline) { LastConnectionError = TEXT("Connection or state preparation timed out."); HandleConnectionFailure(); return true; }
	auto* Player = ConnectingController.Get();
	auto* Pawn = Player != nullptr ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	auto* State = Player != nullptr ? Player->GetPlayerState<AfpstruePvpPlayerState>() : nullptr;
	auto* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	const bool bSpectatorReady = State != nullptr && State->IsOnlyASpectator();
	if (ConnectionState != EFPConnectionState::WaitingReady || ResumeToken.IsEmpty() || State == nullptr
		|| (!bSpectatorReady && !IsPawnStateReady(Pawn, State)) || State->GetPlayerId() != ResumeId
		|| GetWorld()->GetGameState() == nullptr || (!bSpectatorReady && !Player->HasServerTimeSample())) return true;
	RetryCount = 0;
	LastConnectionError.Reset();
	const uint32 ReadyRevision = OperationRevision;
	SetConnectionState(EFPConnectionState::Connected);
	if (ReadyRevision != OperationRevision || ConnectionState != EFPConnectionState::Connected || ConnectingController != Player) return true;
	if (bSpectatorReady)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_RESUME CLIENT_SPECTATOR_READY ResumeId=%d PawnReady=0"), ResumeId);
		return true;
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME CLIENT_READY ResumeId=%d Restored=%d Pawn=%s PS=%s Health=%.1f Ammo=%d Kills=%d Deaths=%d Dead=%d PawnNetGuid=%llu StateNetGuid=%llu"),
		ResumeId, bRestoredConnection, *Pawn->GetName(), *State->GetName(), Pawn->GetCurrentHealth(), Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0, State->GetKills(), State->GetDeaths(), Pawn->IsDead(),
		GetResumeNetGuid(Pawn), GetResumeNetGuid(State));
	return true;
}

// 按所属 GameInstance 的世界与待连接 NetDriver 归属过滤引擎全局失败事件，避免跨 PIE 实例串扰。
bool UfpstrueMultiplayerGameInstance::OwnsNetworkFailure(UWorld* World, UNetDriver* Driver) const
{
	if (GEngine == nullptr) return false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.OwningGameInstance.Get() != this) continue;
		if (Driver != nullptr && (Driver == AttemptNetDriver.Get() || (Context.World() != nullptr && Driver == Context.World()->GetNetDriver())
			|| (Context.PendingNetGame != nullptr && Driver == Context.PendingNetGame->NetDriver))) return true;
		if (Driver == nullptr && World != nullptr && World == Context.World()) return true;
	}
	return false;
}

// 仅当前实例活动连接的网络失败进入重试流程；无关世界和已终止操作忽略。
void UfpstrueMultiplayerGameInstance::HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type, const FString&)
{
	if (!OwnsNetworkFailure(World, Driver) || (ConnectionState != EFPConnectionState::Connected
		&& ConnectionState != EFPConnectionState::Connecting && ConnectionState != EFPConnectionState::WaitingReady)) return;
	LastConnectionError = TEXT("Network connection lost.");
	HandleConnectionFailure();
}

// 当前连接加载地图失败时复用连接失败处理，不接管普通世界旅行错误。
void UfpstrueMultiplayerGameInstance::HandleTravelFailure(UWorld* World, ETravelFailure::Type, const FString&)
{
	if (ConnectionState != EFPConnectionState::Connecting && ConnectionState != EFPConnectionState::WaitingReady) return;
	if (!OwnsNetworkFailure(World, nullptr)) return;
	LastConnectionError = TEXT("Map travel failed.");
	HandleConnectionFailure();
}

// 最多安排三次指数退避；先返回本地世界，Ticker 等待旅行结束后再启动下一次连接。
void UfpstrueMultiplayerGameInstance::HandleConnectionFailure()
{
	ConnectingController.Reset();
	if (!bAutoReconnect || RetryCount >= 3)
	{
		bAutoReconnect = false;
		SetConnectionState(EFPConnectionState::Failed);
		ReturnToLocalWorld();
		return;
	}
	RetryAt = FPlatformTime::Seconds() + (1 << RetryCount++);
	ScheduledRetryRevision = OperationRevision;
	SetConnectionState(EFPConnectionState::RetryWait);
	ReturnToLocalWorld();
}

// 取消尚未完成的连接并打开本地空 GameMode；重连进度保留在跨世界 GameInstance。
void UfpstrueMultiplayerGameInstance::ReturnToLocalWorld()
{
	UWorld* World = GetWorld();
	if (World == nullptr) { if (ConnectionState == EFPConnectionState::Leaving) SetConnectionState(EFPConnectionState::Idle); return; }
	if (bReturningToLocalWorld) return;
	if (GEngine != nullptr) GEngine->CancelPending(World);
	const FString Map = ResolvePvpMapPackage();
	if (!FPackageName::IsValidLongPackageName(Map)) { SetConnectionState(EFPConnectionState::Failed); return; }
	bReturningToLocalWorld = true;
	UGameplayStatics::OpenLevel(this, FName(*Map), true, TEXT("game=/Script/Engine.GameModeBase"));
}

// 本实例本地世界加载完成后解除旅行标记；离场流程至此回到 Idle。
void UfpstrueMultiplayerGameInstance::HandlePostLoadMap(UWorld* World)
{
	if (World == nullptr || World->GetGameInstance() != this || World->GetNetMode() != NM_Standalone) return;
	bReturningToLocalWorld = false;
	AttemptNetDriver.Reset();
	if (ConnectionState == EFPConnectionState::Leaving) SetConnectionState(EFPConnectionState::Idle);
}

// 活跃槽暂不可恢复时保留票据重试；其余拒绝清票据并结束自动重连，主动退出只返回本地。
void UfpstrueMultiplayerGameInstance::RejectConnection(const FString& Reason)
{
	if (Reason == TEXT("manual_leave")) { ReturnToLocalWorld(); return; }
	LastConnectionError = Reason;
	// 半开连接可能尚未被服务器判离线；不能抢占活跃角色，但也不应销毁合法票据。
	if (Reason == TEXT("resume_slot_active")) { HandleConnectionFailure(); return; }
	bAutoReconnect = false;
	ClearResumeCache();
	SetConnectionState(EFPConnectionState::Failed);
	ReturnToLocalWorld();
}

// 控制台取消与主动离场共用同一清理链，不另建取消状态。
void UfpstrueMultiplayerGameInstance::CancelPvpConnection()
{
	LeavePvp();
}

// 使旧重试失效并清票据，再通知服务器主动退出；三秒未完成则由 Ticker 返回本地世界。
void UfpstrueMultiplayerGameInstance::LeavePvp()
{
	++OperationRevision;
	bAutoReconnect = false;
	ScheduledRetryRevision = 0;
	ClearResumeCache();
	SetConnectionState(EFPConnectionState::Leaving);
	AttemptDeadline = FPlatformTime::Seconds() + 3.0;
	if (auto* Player = Cast<AfpstruePvpPlayerController>(GetFirstLocalPlayerController())) Player->ServerLeavePvp();
	else ReturnToLocalWorld();
}

// 仅在空闲或失败时重用上次地址重新连接，沿用 JoinPvp 的地址校验与操作初始化。
void UfpstrueMultiplayerGameInstance::RetryPvpConnection()
{
	if (ConnectionState == EFPConnectionState::Failed || ConnectionState == EFPConnectionState::Idle) JoinPvp(ServerAddress);
}
