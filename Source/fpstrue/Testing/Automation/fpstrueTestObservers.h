// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "NavMesh/RecastNavMesh.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "fpstrueTestObservers.generated.h"

class AfpstrueGameMode;
class AfpstrueCharacter;

// 只替代导航投影；本回归仍走生产 SpawnActor 碰撞规则、Possess 与 BeginPlay。
UCLASS(Transient, NotBlueprintable)
class AfpstrueSpawnTestNavigation : public ARecastNavMesh
{
	GENERATED_BODY()
public:
	virtual bool ProjectPoint(const FVector& Point, FNavLocation& OutLocation, const FVector& Extent,
		FSharedConstNavQueryFilter Filter = nullptr, const UObject* Querier = nullptr) const override;
};

UCLASS(Transient, NotBlueprintable)
class AfpstrueSpawnReentryTestEnemy : public AfpstrueEnemyCharacter
{
	GENERATED_BODY()
public:
	static TFunction<void(AfpstrueSpawnReentryTestEnemy&)> OnTestBeginPlay;
protected:
	virtual void BeginPlay() override;
};

// 动态多播委托的最小反射接收者，仅由自动化测试创建。
UCLASS(Transient, NotBlueprintable)
class UfpstrueGameModeTestObserver : public UObject
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<AfpstrueGameMode> GameMode;
	TWeakObjectPtr<AfpstrueCharacter> Player;
	bool bReenterStart = false;
	bool bKillOnInitialTime = false;
	bool bKillOnFirstWave = false;
	bool bSawRunningState = false;
	int32 InitialTime = -1;
	int32 TimeEvents = 0;
	int32 WaveEvents = 0;
	int32 AliveEvents = 0;
	int32 ResultEvents = 0;

	UFUNCTION()
	void HandleTime(int32 RemainingTime);
	UFUNCTION()
	void HandleWave(int32 CurrentWave, int32 TotalWaves);
	UFUNCTION()
	void HandleAlive(int32 AliveEnemies);
	UFUNCTION()
	void HandleResult(bool bPlayerWon);
};

class UfpstrueHealthComponent;

UCLASS(Transient, NotBlueprintable)
class UfpstrueHealthMutationTestObserver : public UObject
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<UfpstrueHealthComponent> Health;
	bool bResetDuringDamage = false;
	bool bDamageDuringHealthChange = false;
	bool bAlwaysResetDuringHealthChange = false;
	bool bAlternateMaximumDuringHealthChange = false;
	int32 Deaths = 0;
	TArray<float> HealthSnapshots;
	UFUNCTION() void OnDamage(float Amount, AActor* Causer, AController* Instigator);
	UFUNCTION() void OnHealth(float Value);
	UFUNCTION() void OnDeath();
};

// Copyright Epic Games, Inc. All Rights Reserved.

class UfpstrueWeaponComponent;
class AfpstrueCharacter;

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

	UFUNCTION()
	void HandleAmmoChanged(int32 CurrentAmmo, int32 MagazineSize, int32 ReserveAmmo);
	UFUNCTION()
	void HandleEquippedWeaponChanged(UfpstrueWeaponComponent* EquippedWeapon);
	UFUNCTION()
	void HandleFirePerformed();
	UFUNCTION()
	void HandleReloadEnded(int32 ReloadId, EFPReloadEndReason Reason, bool bAmmoCommitted);
	UFUNCTION()
	void HandleReloadPlaybackRequested(int32 ReloadId, bool bWasEmptyReload);
};
