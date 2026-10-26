// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "NativeGameplayTags.h"
#include "Weapons/fpstrueWeaponComponent.h"
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
	// 创建默认 GAS 生命属性；运行时初值仍由权威组件按最终蓝图配置提交。
	UfpstrueHealthAttributeSet();
	// 使用 GAS 原生属性复制与 Always RepNotify，不额外复制兼容镜像。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// Current 值变更前复用 Base 值约束，不广播角色表现。
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	// 约束有限值、最大生命下限和当前生命范围；允许 GAS 传入值被钳制。
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	// Instant GE 执行后再次约束生命数值，通知仍由组件事务统一发布。
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
	// 先交给 GAS 更新聚合器，再标记本接收批次需要刷新兼容接口。
	UFUNCTION() void OnRep_Health(const FGameplayAttributeData& Previous);
	// 最大生命与当前生命在 PostRepNotifies 中合并为一次本地状态通知。
	UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& Previous);
	bool bReceivedHealthAttributes = false;
};

// 瞬时伤害只修改 Health；事务队列负责来源、通知顺序和死亡去重，不再另做一遍减法。
UCLASS()
class UfpstrueHealthDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 定义 Data.Damage 的加法修改器；伤害入口传入负值，本类不重复计算伤害。
	UfpstrueHealthDamageEffect();
};

// 数值重置先更新上限再回满；它不是角色复活，不恢复动画、移动、装备或 AI。
UCLASS()
class UfpstrueHealthResetEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 定义按 SetByCaller 先改上限、再回满的 Instant GE，不负责角色复活。
	UfpstrueHealthResetEffect();
};

// 随伤害一起排队的命中数据；不持有角色或布娃娃，只描述这一次命中的事实。
// 【设计取舍】按值保存方向/位置/骨骼以抵抗同步嵌套伤害；它不是攻击去重表，也不是逐次复制的网络快照。
// 是否重复结算由射击编号或近战事务负责，不能用相同命中位置认定两次伤害重复。
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
 * 玩家/敌人共用 ASC：UE 伤害或外部瞬时生命 GE -> 同线程队列 -> AttributeSet -> 状态提交 -> 通知。
 *
 * AttributeSet 是生命数值和网络复制的唯一来源；CurrentHealth/MaxHealth 仅兼容既有蓝图读接口。
 * Owner 订阅事件后处理 HUD、停止 AI、Ragdoll 等副作用；这里不负责复活、动画或后台线程调度。
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class FPSTRUE_API UfpstrueHealthComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	// 保留 ASC 原生任务/效果调度，生命事务本身不进行逐帧查询。
	UfpstrueHealthComponent();
	// 调试与测试读取真正的 GAS 属性集；不能把旧显示镜像误认为第二套生命状态。
	const UfpstrueHealthAttributeSet* GetHealthAttributeSet() const { return HealthAttributes != nullptr ? HealthAttributes.Get() : GetSet<UfpstrueHealthAttributeSet>(); }
	// 外部瞬时生命 GE 也按事务执行；不能绕过旧 BP 通知、死亡结算及同线程重入顺序。
	virtual FActiveGameplayEffectHandle ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& GameplayEffect,
		FPredictionKey PredictionKey = FPredictionKey()) override;
	// ★【迟到输入边界】已经结束或不属于当前激活的射击数据不能重新创建 ASC 旧缓存。
	virtual void ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
		FPredictionKey AbilityOriginalPredictionKey, const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey) override;
	// GAS 可靠激活负责授权；逐发批次走不可靠窗，未激活时只等待客户端的有限重发。
	// 激活晚过输入截止点会退款，不能补算一次过旧命中；本入口没有消除激活控制链的等待。
	UFUNCTION(Server, Unreliable)
	void ServerSubmitShotBatch(FGameplayAbilitySpecHandle Handle, FPredictionKey Key, uint32 FirstShotId,
		const TArray<FFPShotRequest>& Requests);
	// 每次结束/明确超时至多一条控制消息；正常松开携带尾部，取消/过期绝不结算伤害。
	UFUNCTION(Server, Reliable)
	void ServerFinishShotInputs(FGameplayAbilitySpecHandle Handle, FPredictionKey Key, uint32 FirstShotId,
		const TArray<FFPShotRequest>& Requests, bool bCancelled, bool bExpired);
	void RememberClosedShotStream(FGameplayAbilitySpecHandle Handle, FPredictionKey Key,
		UfpstrueWeaponComponent* Weapon, bool bCancelled);

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
	friend class FFpstrueWeaponShotTransportTest;
	struct FClosedShotStream
	{
		FGameplayAbilitySpecHandle Handle;
		FPredictionKey Key;
		TWeakObjectPtr<AActor> Avatar;
		TWeakObjectPtr<UfpstrueWeaponComponent> Weapon;
		uint32 Generation = 0;
		uint32 Session = 0;
		double ClosedAt = 0.0;
		bool bCancelled = false;
	};
	TArray<FClosedShotStream, TInlineAllocator<8>> ClosedShotStreams;
	void RouteShotInputBatch(FGameplayAbilitySpecHandle Handle, FPredictionKey Key, uint32 FirstShotId,
		const TArray<FFPShotRequest>& Requests, bool bControl, bool bCancelled, bool bExpired);
	// 属性集以所属 Actor 为 Outer，ASC 只登记它；不能把 AttributeSet 嵌套在组件下面。
	void EnsureHealthAttributes();
	// Standalone 与服务器拥有生命写权限；客户端 API 不隐式转成服务器 RPC。
	bool HasHealthAuthority() const;
	// 属性集批次结束后提交本地读接口；首包必通知，之后只通知数值变化，不制造伤害事件。
	void ConsumeReplicatedHealthAttributes(const UfpstrueHealthAttributeSet& Attributes);
	// 首包可能与本地默认值相同，仍需向已订阅的 HUD 发布一次初始状态。
	bool bReceivedInitialHealth = false;
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
