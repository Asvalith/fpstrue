// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "NavMesh/RecastNavMesh.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "fpstrueTestObservers.generated.h"

class AfpstrueCharacter;

// 只计数生产受击 RPC 的 ProcessEvent 分派，不替换扣血或 RPC 实现，不代表真实网络送达。
UCLASS(Transient, NotBlueprintable)
class AfpstrueDamagePresentationTestCharacter : public AfpstrueCharacter
{
	GENERATED_BODY()
public:
	int32 NativeHitRequests = 0;
	// 记录唯一身体受击反馈后继续原分派，用于证明一次权威扣血只派发一次表现。
	virtual void ProcessEvent(UFunction* Function, void* Parameters) override;
};

// 只替代导航投影；本回归仍走生产 SpawnActor 碰撞规则、Possess 与 BeginPlay。
UCLASS(Transient, NotBlueprintable)
class AfpstrueSpawnTestNavigation : public ARecastNavMesh
{
	GENERATED_BODY()
public:
	// 将查询点原样写入导航结果，隔离导航构建成本而不替换敌人的真实生成链。
	virtual bool ProjectPoint(const FVector& Point, FNavLocation& OutLocation, const FVector& Extent,
		FSharedConstNavQueryFilter Filter = nullptr, const UObject* Querier = nullptr) const override;
};

// 在敌人 BeginPlay 内注入同步重入，验证生成返回后仍会复核敌人的存活状态。
UCLASS(Transient, NotBlueprintable)
class AfpstrueSpawnReentryTestEnemy : public AfpstrueEnemyCharacter
{
	GENERATED_BODY()
public:
	static TFunction<void(AfpstrueSpawnReentryTestEnemy&)> OnTestBeginPlay;
protected:
	// 先走生产初始化，再触发测试指定的销毁或清理行为。
	virtual void BeginPlay() override;
};

class UfpstrueHealthComponent;

// 在生命值委托中继续提交生命修改，专门检查同线程重入的排队顺序与终止边界。
UCLASS(Transient, NotBlueprintable)
class UfpstrueHealthMutationTestObserver : public UObject
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<UfpstrueHealthComponent> Health;
	bool bResetDuringDamage = false;
	bool bResetDuringDeath = false;
	bool bDamageDuringHealthChange = false;
	bool bAlwaysResetDuringHealthChange = false;
	bool bAlternateMaximumDuringHealthChange = false;
	int32 Deaths = 0;
	TArray<float> HealthSnapshots;
	// 可在伤害通知中请求重置并继续扣血，不直接篡改已提交的血量快照。
	UFUNCTION() void OnDamage(float Amount, AActor* Causer, AController* Instigator);
	// 保存通知顺序，并按开关构造有限重入或持续循环请求。
	UFUNCTION() void OnHealth(float Value);
	// 只计数死亡边沿，验证每次生命会话最多结算一次死亡。
	UFUNCTION() void OnDeath();
};

// Copyright Epic Games, Inc. All Rights Reserved.

class UfpstrueWeaponComponent;
class AfpstrueCharacter;

// 记录实际射击 RPC 的事实与去重身份，不为测试保留生产协议的冗余字段。
struct FObservedShotPresentation
{
	FHitResult Hit;
	int32 ShotIndex = 0;
	uint32 EquipmentGeneration = 0;
	uint32 Session = 0;
};

// 只观察生产 RPC 分派的实参，不替换开火、伤害或网络发送逻辑。
UCLASS(Transient, NotBlueprintable)
class UfpstrueShotPresentationTestWeapon : public UfpstrueWeaponComponent
{
	GENERATED_BODY()
public:
	TArray<FObservedShotPresentation> ShotPresentations;
	// 在真实 MulticastShot 进入 ProcessEvent 时保存身份，随后继续执行原实现。
	virtual void ProcessEvent(UFunction* Function, void* Parameters) override;
};

// A reflected receiver is required by OnAmmoChanged's dynamic multicast signature.
// Only automation tests instantiate this transient object; it adds no gameplay API.
UCLASS(Transient, NotBlueprintable)
class UfpstrueReloadReentryObserver : public UObject
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<UfpstrueWeaponComponent> Weapon;
	int32 CallbackCount = 0;
	bool bDisableWeaponOnAmmoChanged = false;
	TWeakObjectPtr<AfpstrueCharacter> Player;
	bool bUnequipOnEquipped = false;
	int32 FireEventCount = 0;
	int32 ReloadEndCount = 0;
	EFPReloadEndReason LastReloadEndReason = EFPReloadEndReason::Completed;
	bool bLastReloadCommitted = false;
	bool bRestartReloadOnEnd = false;
	bool bFireOnReloadEnd = false;
	bool bReloadRestartAccepted = false;
	TWeakObjectPtr<UAnimInstance> ReloadAnimInstance;
	TWeakObjectPtr<UAnimMontage> ReloadMontage;
	bool bAmmoChangedWhileMontageActive = false;
	TFunction<void()> OnTraceReceived;
	TFunction<void()> OnReloadEnded;

	// 记录弹药通知，检查通知时动画是否仍在播放，并可同步禁用武器。
	UFUNCTION()
	void HandleAmmoChanged(int32 CurrentAmmo, int32 MagazineSize, int32 ReserveAmmo);
	// 在装备广播中同步卸下武器，暴露装备提交后的重入边界。
	UFUNCTION()
	void HandleEquippedWeaponChanged(UfpstrueWeaponComponent* EquippedWeapon);
	// 统计已完成的单发事实，区别于可能被取消的开火请求。
	UFUNCTION()
	void HandleFirePerformed();
	// 可在真实弹道广播内同步重启开火，复现同一调用栈覆盖新旧表现身份的边界。
	UFUNCTION()
	void HandleTraceFinished(bool bHit, FVector TraceStart, FVector TraceEnd, FVector TraceTarget, FHitResult Hit);
	// 记录换弹结束原因，并可从结束通知立即重启换弹或开火，检查旧清理不伤新动作。
	UFUNCTION()
	void HandleReloadEnded(int32 ReloadId, EFPReloadEndReason Reason, bool bAmmoCommitted);
	// 在播放请求中创建并绑定测试 Montage，检验真实动画实例的事务身份。
	UFUNCTION()
	void HandleReloadPlaybackRequested(int32 ReloadId, bool bWasEmptyReload);
};

namespace FpstrueTest
{
void SetFireInput(UfpstrueWeaponComponent* Weapon, bool bPressed);
}
