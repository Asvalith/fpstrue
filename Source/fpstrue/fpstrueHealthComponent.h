// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "fpstrueHealthComponent.generated.h"

class AController;
class UDamageType;

//受伤害、血量变化、死亡事件的动态多播委托
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDamageReceived, float, DamageAmount, AActor*, DamageCauser, AController*, InstigatedBy);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHealthChanged, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDeath);

/** 通用生命值模块：统一接收 UE 伤害、维护血量并广播受伤、血量变化和死亡事件。 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建无 Tick 的生命值组件。
	UfpstrueHealthComponent();

	// ==================== 生命值管理 ====================
	// 把当前血量恢复到最大值，并重置死亡广播状态。
	UFUNCTION(BlueprintCallable, Category = "Health")
	void ResetHealth();

	// 返回当前血量。
	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealth() const { return CurrentHealth; }

	// 返回配置的最大血量。
	UFUNCTION(BlueprintPure, Category = "Health")
	float GetMaxHealth() const { return MaxHealth; }

	// 返回 0 到 1 的血量比例。
	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthNormalized() const;

	// 判断当前血量是否已经耗尽。
	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const { return CurrentHealth <= 0.0f; }

	UPROPERTY(BlueprintAssignable, Category = "Health")
	FOnDamageReceived OnDamageReceived;

	UPROPERTY(BlueprintAssignable, Category = "Health")
	FOnHealthChanged OnHealthChanged;

	UPROPERTY(BlueprintAssignable, Category = "Health")
	FOnDeath OnDeath;

protected:
	// 初始化血量并订阅 Owner 的通用伤害事件。
	virtual void BeginPlay() override;
	// 解除 Owner 伤害事件订阅。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 把 Owner 收到的 UE 伤害转入统一扣血流程。
	UFUNCTION()
	void HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType, AController* InstigatedBy,
								  AActor* DamageCauser);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health", meta = (ClampMin = "1.0"))
	float MaxHealth = 100.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	float CurrentHealth = 100.0f;

	bool bDeathBroadcast = false;

private:
	// 执行扣血、Clamp 和事件广播，保证死亡只广播一次。
	void ApplyDamageInternal(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);
};
