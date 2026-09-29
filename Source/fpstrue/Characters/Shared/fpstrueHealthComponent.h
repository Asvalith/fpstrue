// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "fpstrueHealthComponent.generated.h"

class AController;
class UDamageType;

// 随伤害一起排队的命中数据；不持有角色或布娃娃，只描述这一次命中的事实。
struct FFPDamageContext
{
	FVector Direction = FVector::ZeroVector;
	FVector Location = FVector::ZeroVector;
	FName BoneName = NAME_None;
	bool bHasHitData = false;
};

DECLARE_MULTICAST_DELEGATE_FourParams(FOnDamageResolved, float, AActor*, AController*, const FFPDamageContext&);

//受伤害、血量变化、死亡事件的动态多播委托
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDamageReceived, float, DamageAmount, AActor*, DamageCauser, AController*, InstigatedBy);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHealthChanged, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDeath);

/**
 * 通用生命值模块：统一接收 UE 伤害、维护血量并广播受伤、血量变化和死亡事件。
 *
 * 它是 CurrentHealth 的唯一写入者，不依赖玩家或敌人类型；Owner 订阅事件后自行处理 HUD、停止 AI、
 * Ragdoll 等差异化副作用，因此生命数值与角色表现可以分别复用和测试。
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建无 Tick 的生命值组件。
	UfpstrueHealthComponent();

	// ==================== 生命值管理 ====================
	// 只重置生命数据，不复活角色；广播期间的请求排在当前伤害通知之后执行。
	UFUNCTION(BlueprintCallable, Category = "Health")
	void ResetHealth();

	// 运行时更新最大生命并回满；Benchmark 用它保留完整受伤链路，同时避免测试中途死亡。
	void SetMaxHealthAndReset(float NewMaxHealth);

	// Owner 在调用 Super::TakeDamage 的同步范围内提供上下文；嵌套伤害退出后恢复外层上下文。
	// Health 在接收 OnTakeAnyDamage 时复制它，延迟通知不再读取 Owner 的可变 LastDamage。
	TGuardValue<FFPDamageContext> ScopeDamageContext(const FFPDamageContext& Context)
	{
		return TGuardValue<FFPDamageContext>(IncomingDamageContext, Context);
	}

	// 原生表现消费者使用本次不可变上下文；原 Blueprint 数值事件保持兼容。
	FOnDamageResolved OnDamageResolved;
	// 一次同步通知链有界；超限的新请求被拒绝并告警，已接收的请求仍按顺序处理。
	static constexpr int32 MaxMutationsPerDispatch = 128;
	int32 GetRejectedMutationCount() const { return RejectedMutationCount; }

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

	// Owner 的 UE 伤害入口：统一扣血、Clamp 和事件广播，保证死亡只广播一次。
	UFUNCTION()
	void HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType, AController* InstigatedBy,
								  AActor* DamageCauser);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health", meta = (ClampMin = "1.0"))
	float MaxHealth = 100.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	float CurrentHealth = 100.0f;

	bool bDeathBroadcast = false;

private:
	// 一个伤害事务先提交数值，再按 Damage -> Health -> Death 通知。
	// 监听者重入的伤害/重置排队，不能改写外层死亡判定或令 HUD 收到倒序快照。
	enum class EMutation : uint8 { Damage, Reset, ResetMaximum };
	struct FMutation
	{
		EMutation Type;
		float Value = 0.0f;
		TWeakObjectPtr<AActor> Causer;
		TWeakObjectPtr<AController> Instigator;
		FFPDamageContext Context;
	};
	void EnqueueMutation(const FMutation& Mutation);
	void ApplyMutation(const FMutation& Mutation);
	TArray<FMutation, TInlineAllocator<4>> PendingMutations;
	FFPDamageContext IncomingDamageContext;
	int32 RejectedMutationCount = 0;
	bool bReportedMutationLimit = false;
	bool bDispatchingMutation = false;
	bool bEndingHealthPlay = false;
};
