// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/HUD.h"
#include "fpstruePvpHUD.generated.h"

class AfpstrueCharacter;
class UfpstrueWeaponComponent;
class UProgressBar;
class UTextBlock;

// 本地只读表现：HUD 绑定当前 Pawn/武器并推送快照，Widget 沿用原布局；不参与玩法结算。

// 原 ingame 布局的只读表现桥；由 HUD 推送当前值，不查询单机 GameMode 或在 UMG 属性绑定里轮询角色。
UCLASS(Blueprintable)
class FPSTRUE_API UfpstruePvpIngameWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	//构建前也可接收快照；NativeConstruct 会补读缓存，重连或晚创建不会漏掉之前的事件。
	void ApplySnapshot(float Health, float MaxHealth, bool bEquipped, int32 Ammo, int32 Reserve, const FText& WeaponName);
	//防呆测试
	bool HasAuthoredControls() const;
	bool HasPaintedSnapshot() const;
protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//刷新布局时也会触发绘制；不在绘制回调内更新玩法数值。
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	// 唯一表现入口：蓝图读取快照并更新作者控件；不反向修改生命、弹药或装备。
	UFUNCTION(BlueprintImplementableEvent, Category = "Presentation") void PresentSnapshot();
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") float HealthFraction = 1.0f;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") int32 CurrentAmmo = 0;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") int32 ReserveAmmo = 0;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") bool bHasEquipment = false;
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Presentation") FText CurrentWeaponName;
	// 编译器和网络资产准备器均强制检查唯一网络布局的语义控件名。
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UTextBlock> MagazineText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UTextBlock> ReserveText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UTextBlock> WeaponNameText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UTextBlock> MatchHintText;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UWidget> AmmoContainer;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Presentation") TObjectPtr<UWidget> CountdownRow;
private:
	friend class FFpstrueMultiplayerHudRebindTest;
	bool bPresentationReady = false;
	mutable uint64 LastPaintFrame = 0;
	uint32 SnapshotRevision = 0;
	mutable uint32 LastPaintedSnapshotRevision = 0;
};

// 联机界面只展示拥有者当前 Pawn 的状态；换 Pawn 或重连时重新订阅并补读快照。
UCLASS()
class FPSTRUE_API AfpstruePvpHUD : public AHUD
{
	GENERATED_BODY()
public:
	AfpstruePvpHUD();
	virtual void DrawHUD() override;
	bool HasDrawnCurrentState(const AfpstrueCharacter* Pawn) const;
protected:
	virtual void PostInitializeComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 只在本地有画面时加载；专服不为 HUD 保活整套 UMG/动画资源。
	UPROPERTY(EditDefaultsOnly, Category = "Multiplayer|UI") TSoftClassPtr<UfpstruePvpIngameWidget> IngameWidgetClass;
private:
	friend class FFpstrueMultiplayerHudRebindTest;
	void BindPawn(AfpstrueCharacter* Pawn);
	bool EnsureIngameWidget();
	void RefreshIngameSnapshot();
	UFUNCTION() void HandlePossessedPawnChanged(APawn* OldPawn, APawn* NewPawn);
	UFUNCTION() void HandleHealthChanged(float Health);
	UFUNCTION() void HandleWeaponChanged(UfpstrueWeaponComponent* Weapon);
	UFUNCTION() void HandleAmmoChanged(int32 Ammo, int32 Magazine, int32 Reserve);
	TWeakObjectPtr<AfpstrueCharacter> ObservedPawn;
	TWeakObjectPtr<UfpstrueWeaponComponent> ObservedWeapon;
	UPROPERTY(Transient) TObjectPtr<UfpstruePvpIngameWidget> IngameWidget;
	float CurrentHealth = 0.0f;
	float CurrentMaxHealth = 100.0f;
	int32 CurrentAmmo = 0;
	int32 ReserveAmmo = 0;
	bool bAttemptedIngameWidget = false;
	FText CurrentWeaponName;
};
