// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/fpstruePvpHUD.h"
#include "GameFramework/PlayerController.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "EngineGlobals.h"
#include "Misc/App.h"

// 本地只读表现：HUD 绑定当前 Pawn/武器并推送快照，Widget 沿用原布局；不参与玩法结算。
// 只接收已提交的显示快照，数值无效时安全回退；不拥有武器、Pawn 或另一份 Gameplay 状态。
void UfpstruePvpIngameWidget::ApplySnapshot(float Health, float MaxHealth, bool bEquipped,
	int32 Ammo, int32 Reserve, const FText& WeaponName)
{
	const float Fraction = FMath::IsFinite(Health) && FMath::IsFinite(MaxHealth) && MaxHealth > 0.0f
		? FMath::Clamp(Health / MaxHealth, 0.0f, 1.0f) : 0.0f;
	const int32 Magazine = bEquipped ? FMath::Max(0, Ammo) : 0;
	const int32 ReserveCount = bEquipped ? FMath::Max(0, Reserve) : 0;
	const FText Name = bEquipped ? WeaponName : FText::GetEmpty();
	//脏更新
	if (HealthFraction == Fraction && bHasEquipment == bEquipped && CurrentAmmo == Magazine
		&& ReserveAmmo == ReserveCount && CurrentWeaponName.EqualTo(Name)) return;
	//记录检查脏标记
	HealthFraction = Fraction;
	bHasEquipment = bEquipped;
	CurrentAmmo = Magazine;
	ReserveAmmo = ReserveCount;
	CurrentWeaponName = Name;
	//版本好
	if (++SnapshotRevision == 0) ++SnapshotRevision;
	//ui创建好之后，通知蓝图刷新一次
	if (bPresentationReady) PresentSnapshot();
	InvalidateLayoutAndVolatility();
}

// UE 在初始化时绑定语义控件名；不在运行时按作者旧名称搜索控件。
bool UfpstruePvpIngameWidget::HasAuthoredControls() const
{
	return HealthBar != nullptr && MagazineText != nullptr && ReserveText != nullptr && WeaponNameText != nullptr
		&& MatchHintText != nullptr && AmmoContainer != nullptr && CountdownRow != nullptr;
}

// 构建后补发表现事件，恢复构建前已收到的最新快照。
void UfpstruePvpIngameWidget::NativeConstruct()
{
	Super::NativeConstruct();
	bPresentationReady = true;
	PresentSnapshot();
}

//销毁后更新标记
void UfpstruePvpIngameWidget::NativeDestruct()
{
	bPresentationReady = false;
	Super::NativeDestruct();
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

// 只选择已经准备好的网络 Widget Blueprint。
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

// 本地视口只创建已准备的网络 Widget Blueprint；资产合同失败时明确报告，不生成第二套界面。
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
	UE_LOG(LogTemp, Error, TEXT("PvP network Widget Blueprint is unavailable or missing its required bound controls."));
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
	CurrentWeaponName = Weapon != nullptr ? Weapon->WeaponDisplayName : FText::GetEmpty();
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
		&& IngameWidget != nullptr && IngameWidget->HasPaintedSnapshot()
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
