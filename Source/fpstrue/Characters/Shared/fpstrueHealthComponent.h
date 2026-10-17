// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "NativeGameplayTags.h"
#include "fpstrueHealthComponent.generated.h"

class AController;
class UDamageType;
class FLifetimeProperty;
class UfpstrueHealthComponent;

// 项目统一的原生标签；能力只描述动作与互斥，伤害和初始化数值由 GE 的 SetByCaller 提供。
namespace FpstrueGameplayTags
{
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Fire)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Reload)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Aim)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Sprint)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Firing)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Reloading)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Aiming)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Sprinting)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Health)
	FPSTRUE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_MaxHealth)
}

// GAS 的生命数值唯一存储；复制属性而非重复复制旧组件快照，客户端不能通过兼容接口自行扣血。
UCLASS()
class FPSTRUE_API UfpstrueHealthAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UfpstrueHealthAttributeSet();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
	// 一个网络批次的属性 RepNotify 全部结束后，再刷新旧 BP 接口，避免一次接收中重复通知。
	virtual void PostRepNotifies() override;

	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UfpstrueHealthAttributeSet, Health)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(Health)
	GAMEPLAYATTRIBUTE_VALUE_SETTER(Health)
	GAMEPLAYATTRIBUTE_VALUE_INITTER(Health)
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UfpstrueHealthAttributeSet, MaxHealth)
	GAMEPLAYATTRIBUTE_VALUE_GETTER(MaxHealth)
	GAMEPLAYATTRIBUTE_VALUE_SETTER(MaxHealth)
	GAMEPLAYATTRIBUTE_VALUE_INITTER(MaxHealth)

private:
	UPROPERTY(ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;
	UPROPERTY(ReplicatedUsing = OnRep_MaxHealth)
	FGameplayAttributeData MaxHealth;
	UFUNCTION() void OnRep_Health(const FGameplayAttributeData& Previous);
	UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& Previous);
	bool bReceivedHealthAttributes = false;
};

// 瞬时伤害只修改 Health；事务队列负责来源、通知顺序和死亡去重，不再另做一遍减法。
UCLASS()
class UfpstrueHealthDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UfpstrueHealthDamageEffect();
};

// 数值重置先更新上限再回满；它不是角色复活，不恢复动画、移动、装备或 AI。
UCLASS()
class UfpstrueHealthResetEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UfpstrueHealthResetEffect();
};

// 旧蓝图接口使用的本地兼容镜像；网络真实数值由 GAS AttributeSet 复制，版本仅标记本地提交。
USTRUCT()
struct FPSTRUE_API FFPReplicatedHealthSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	float CurrentHealth = 100.0f;

	UPROPERTY()
	float MaxHealth = 100.0f;

	UPROPERTY()
	uint32 Revision = 0;
};

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
// 上述原有说明中的 CurrentHealth 现在是兼容只读镜像；真实权威值由 AttributeSet + GE 维护。
class FPSTRUE_API UfpstrueHealthComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	// 创建无 Tick 的生命值组件。
	UfpstrueHealthComponent();
	// 只复制完整生命快照；伤害来源和本地事务队列不作为客户端事件重放。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 调试与测试读取真正的 GAS 属性集；不能把旧显示镜像误认为第二套生命状态。
	const UfpstrueHealthAttributeSet* GetHealthAttributeSet() const { return HealthAttributes != nullptr ? HealthAttributes.Get() : GetSet<UfpstrueHealthAttributeSet>(); }
	// 外部瞬时生命 GE 也按事务执行；不能绕过旧 BP 通知、死亡结算及同线程重入顺序。
	virtual FActiveGameplayEffectHandle ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& GameplayEffect,
		FPredictionKey PredictionKey = FPredictionKey()) override;
	// ★【迟到输入边界】已经结束或不属于当前激活的射击数据不能重新创建 ASC 旧缓存。
	virtual void ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
		FPredictionKey AbilityOriginalPredictionKey, const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey) override;

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
	// 读取因同步重入链超过容量而被拒绝的请求数，供诊断和自动化测试使用。
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
	friend class UfpstrueHealthAttributeSet;
	// 属性集以所属 Actor 为 Outer，ASC 只登记它；不能把 AttributeSet 嵌套在组件下面。
	void EnsureHealthAttributes();
	// Standalone 与服务器拥有生命写权限；客户端 API 不隐式转成服务器 RPC。
	bool HasHealthAuthority() const;
	// 在本地通知前提交兼容镜像，并为网络 Actor 请求更新；真实生命数值的复制归 GAS 属性集。
	void CommitHealthSnapshot();
	// 客户端先提交所有数值和死亡边沿，再发布血量与首次死亡通知。
	UFUNCTION()
	void OnRep_HealthSnapshot();

	// GAS 接管后仅保留反射兼容镜像；不再独立复制，OnRep 入口只从真实属性集补齐通知。
	UPROPERTY(Transient)
	FFPReplicatedHealthSnapshot HealthSnapshot;
	UPROPERTY(Transient)
	TObjectPtr<UfpstrueHealthAttributeSet> HealthAttributes;

	// 一个伤害事务先提交数值，再按 Damage -> Health -> Death 通知。
	// 监听者重入的伤害/重置排队，不能改写外层死亡判定或令 HUD 收到倒序快照。
	enum class EMutation : uint8 { Damage, Reset, ResetMaximum, Effect };
	struct FMutation
	{
		EMutation Type;
		float Value = 0.0f;
		TWeakObjectPtr<AActor> Causer;
		TWeakObjectPtr<AController> Instigator;
		FFPDamageContext Context;
		FGameplayEffectSpecHandle Effect;
		FPredictionKey PredictionKey;
	};
	// 按值接纳伤害或重置；通知期间的新请求排队，最外层调用在当前线程依序排空。
	void EnqueueMutation(const FMutation& Mutation);
	// 执行一个已接纳事务，先提交血量和死亡边沿，再按固定顺序发送状态通知。
	void ApplyMutation(const FMutation& Mutation);
	TArray<FMutation, TInlineAllocator<4>> PendingMutations;
	FFPDamageContext IncomingDamageContext;
	int32 RejectedMutationCount = 0;
	bool bReportedMutationLimit = false;
	bool bDispatchingMutation = false;
	bool bEndingHealthPlay = false;
};
