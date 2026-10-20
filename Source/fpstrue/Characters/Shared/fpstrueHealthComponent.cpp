// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueCombatAbilities.h"

// Fire 的能力激活和逐发数据使用同一 ASC 可靠通道；只为当前激活消费数据，不保留已取消轮次的晚包。
void UfpstrueHealthComponent::ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
	FPredictionKey AbilityOriginalPredictionKey, const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey)
{
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(AbilityHandle);
	UGameplayAbility* Instance = Spec != nullptr ? Spec->GetPrimaryInstance() : nullptr;
	const bool bCurrent = Instance != nullptr && Instance->IsActive()
		&& Instance->GetCurrentActivationInfo().GetActivationPredictionKey() == AbilityOriginalPredictionKey;
	if (!bCurrent || IsDead())
	{
		// 当前项目均为同 ASC 通道的实例化能力；未知/非活动能力不能借目标数据 RPC 占用旧缓存。
		ConsumeAllReplicatedData(AbilityHandle, AbilityOriginalPredictionKey);
		// 不能把旧能力的数据交给当前装备发送拒绝 ACK：换枪后的新武器可能恰好使用相同发号。
		// 强取消/换装备会撤销相应估计；普通松开保留已准入射击等待原武器 ACK。
		// 这里无法证明晚包属于哪把旧武器，只丢弃未知轮次，不伪造当前装备的逐发终结结果。
		return;
	}
	Super::ServerSetReplicatedTargetData_Implementation(AbilityHandle, AbilityOriginalPredictionKey, TargetData,
		ApplicationTag, CurrentPredictionKey);
}

namespace FpstrueGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG(Ability_Fire, "Ability.Fire")
	UE_DEFINE_GAMEPLAY_TAG(Ability_Reload, "Ability.Reload")
	UE_DEFINE_GAMEPLAY_TAG(Ability_Aim, "Ability.Aim")
	UE_DEFINE_GAMEPLAY_TAG(Ability_Sprint, "Ability.Sprint")
	UE_DEFINE_GAMEPLAY_TAG(State_Firing, "State.Firing")
	UE_DEFINE_GAMEPLAY_TAG(State_Reloading, "State.Reloading")
	UE_DEFINE_GAMEPLAY_TAG(State_Aiming, "State.Aiming")
	UE_DEFINE_GAMEPLAY_TAG(State_Sprinting, "State.Sprinting")
	UE_DEFINE_GAMEPLAY_TAG(State_Dead, "State.Dead")
	UE_DEFINE_GAMEPLAY_TAG(Data_Damage, "Data.Damage")
	UE_DEFINE_GAMEPLAY_TAG(Data_Health, "Data.Health")
	UE_DEFINE_GAMEPLAY_TAG(Data_MaxHealth, "Data.MaxHealth")
}

// 属性集只保存和约束数值；旧生命组件的通知队列仍负责完整的伤害事务，不在这里重复广播。
UfpstrueHealthAttributeSet::UfpstrueHealthAttributeSet()
	: Health(100.0f), MaxHealth(100.0f)
{
}

void UfpstrueHealthAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UfpstrueHealthAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UfpstrueHealthAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
}

// 同时约束 Current 与 Base 更新；拒绝非有限数值，最大生命至少为一，当前生命始终在合法范围。
void UfpstrueHealthAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	PreAttributeBaseChange(Attribute, NewValue);
}

void UfpstrueHealthAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);
	if (Attribute == GetMaxHealthAttribute()) NewValue = FMath::IsFinite(NewValue) ? FMath::Max(NewValue, 1.0f) : 100.0f;
	else if (Attribute == GetHealthAttribute()) NewValue = FMath::IsFinite(NewValue) ? FMath::Clamp(NewValue, 0.0f, GetMaxHealth()) : 0.0f;
}

// GE 修改 Base 后再次 Clamp；只写 GAS 属性，不操作旧显示缓存，也不在 GE 中复活角色。
void UfpstrueHealthAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	if (Data.EvaluatedData.Attribute == GetHealthAttribute() || Data.EvaluatedData.Attribute == GetMaxHealthAttribute())
	{
		const float SafeMaximum = FMath::IsFinite(GetMaxHealth()) ? FMath::Max(GetMaxHealth(), 1.0f) : 100.0f;
		if (GetMaxHealth() != SafeMaximum) SetMaxHealth(SafeMaximum);
		const float SafeHealth = FMath::IsFinite(GetHealth()) ? FMath::Clamp(GetHealth(), 0.0f, SafeMaximum) : 0.0f;
		if (GetHealth() != SafeHealth) SetHealth(SafeHealth);
	}
}

void UfpstrueHealthAttributeSet::OnRep_Health(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UfpstrueHealthAttributeSet, Health, Previous);
	bReceivedHealthAttributes = true;
}

void UfpstrueHealthAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UfpstrueHealthAttributeSet, MaxHealth, Previous);
	bReceivedHealthAttributes = true;
}

// 跨网络对象不承诺全世界原子快照；这里只合并本属性集同一接收批次内的 BP 通知。
void UfpstrueHealthAttributeSet::PostRepNotifies()
{
	Super::PostRepNotifies();
	if (!bReceivedHealthAttributes) return;
	bReceivedHealthAttributes = false;
	if (UfpstrueHealthComponent* HealthComponent = Cast<UfpstrueHealthComponent>(GetOwningAbilitySystemComponent()))
	{
		HealthComponent->HealthAttributes = this;
		HealthComponent->OnRep_HealthSnapshot();
	}
}

UfpstrueHealthDamageEffect::UfpstrueHealthDamageEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UfpstrueHealthAttributeSet::GetHealthAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat Damage;
	Damage.DataTag = FpstrueGameplayTags::Data_Damage;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Damage);
}

UfpstrueHealthResetEffect::UfpstrueHealthResetEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayModifierInfo& Maximum = Modifiers.AddDefaulted_GetRef();
	Maximum.Attribute = UfpstrueHealthAttributeSet::GetMaxHealthAttribute();
	Maximum.ModifierOp = EGameplayModOp::Override;
	FSetByCallerFloat MaximumValue;
	MaximumValue.DataTag = FpstrueGameplayTags::Data_MaxHealth;
	Maximum.ModifierMagnitude = FGameplayEffectModifierMagnitude(MaximumValue);
	FGameplayModifierInfo& Current = Modifiers.AddDefaulted_GetRef();
	Current.Attribute = UfpstrueHealthAttributeSet::GetHealthAttribute();
	Current.ModifierOp = EGameplayModOp::Override;
	FSetByCallerFloat HealthValue;
	HealthValue.DataTag = FpstrueGameplayTags::Data_Health;
	Current.ModifierMagnitude = FGameplayEffectModifierMagnitude(HealthValue);
}

/*
 * 玩家和敌人共用的 ASC 生命适配。
 * UE 伤害或外部生命 GE -> 同线程事务队列 -> Instant GE/AttributeSet 提交 -> 兼容镜像与通知。
 * AttributeSet 原生复制是客户端数值来源；客户端不重放伤害来源或服务器事务队列。
 *
 * 本组件只补充固定通知顺序、同步重入排队和死亡边沿；布娃娃、停止 AI、HUD 由订阅者处理。
 */

// ==================== 生命周期与伤害入口 ====================

// 生命事务没有逐帧查询；能力任务和效果保留 ASC 自身的原生调度。
UfpstrueHealthComponent::UfpstrueHealthComponent()
{
	SetIsReplicatedByDefault(true);
	SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
}

// AddSet 内部以 Owner Actor 为 Outer 创建属性集，符合 UAttributeSet::GetOwningActor 的约束。
void UfpstrueHealthComponent::EnsureHealthAttributes()
{
	if (HealthAttributes == nullptr && GetOwner() != nullptr)
	{
		HealthAttributes = const_cast<UfpstrueHealthAttributeSet*>(GetSet<UfpstrueHealthAttributeSet>());
		// 动态属性集只在服务器创建，客户端等待 ASC 的子对象复制，避免同一类型出现本地/网络两份实例。
		if (HealthAttributes == nullptr && HasHealthAuthority())
		{
			HealthAttributes = const_cast<UfpstrueHealthAttributeSet*>(AddSet<UfpstrueHealthAttributeSet>());
		}
	}
}

// 用蓝图最终配置初始化血量，再订阅所属 Actor 的 UE 伤害事件；自身不处理表现。
void UfpstrueHealthComponent::BeginPlay()
{
	Super::BeginPlay();
	bEndingHealthPlay = false;
	EnsureHealthAttributes();
	if (GetOwner() != nullptr) InitAbilityActorInfo(GetOwner(), GetOwner());
	// 客户端保留已经收到的初始快照；不使用本地蓝图默认值重新回满或订阅扣血。
	if (!HasHealthAuthority()) return;

	// Blueprint 覆盖的 MaxHealth 到 BeginPlay 才最终可用；Owner 会在绑定委托后主动读取初始快照。
	ResetHealth();

	if (AActor* Owner = GetOwner(); Owner != nullptr && !bEndingHealthPlay)
	{
		Owner->OnTakeAnyDamage.AddUniqueDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}
}

// 先拒绝新的生命事务并丢弃待处理请求，再解除 Owner 委托，防止退出阶段继续通知。
void UfpstrueHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// ★【重要边界】[生命周期] 退出标记先于委托解绑；正在分发的通知返回后也会停止，待处理伤害不跨生命周期继续执行。
	bEndingHealthPlay = true;
	PendingMutations.Reset();
	CancelAllAbilities();
	// Owner 生命周期结束前成对解除伤害委托，防止退出阶段再次进入组件逻辑。
	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.RemoveDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}

	Super::EndPlay(EndPlayReason);
}

// UE 伤害委托入口，统一完成扣血和伤害/死亡广播。
void UfpstrueHealthComponent::HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType,
													   AController* InstigatedBy, AActor* DamageCauser)
{
	// 先拒绝无效伤害和尸体重复伤害，再统一 Clamp；外部系统不能绕过这里直接写 CurrentHealth。
	if (!HasHealthAuthority() || DamagedActor != GetOwner() || !FMath::IsFinite(Damage) || Damage <= 0.0f)
	{
		return;
	}
	EnqueueMutation({EMutation::Damage, Damage, DamageCauser, InstigatedBy, IncomingDamageContext});
}

// GE 的公共入口也必须经过生命事务；效果副本与来源在入队时固定，不借用调用者可变的 Spec。
FActiveGameplayEffectHandle UfpstrueHealthComponent::ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& GameplayEffect,
	FPredictionKey PredictionKey)
{
	const bool bChangesHealth = GameplayEffect.Def != nullptr && GameplayEffect.Def->Modifiers.ContainsByPredicate(
		[](const FGameplayModifierInfo& Modifier)
		{
			return Modifier.Attribute == UfpstrueHealthAttributeSet::GetHealthAttribute()
				|| Modifier.Attribute == UfpstrueHealthAttributeSet::GetMaxHealthAttribute();
		});
	if (!bChangesHealth) return Super::ApplyGameplayEffectSpecToSelf(GameplayEffect, PredictionKey);
	if (bEndingHealthPlay || !HasHealthAuthority()) return FActiveGameplayEffectHandle();
	if (GameplayEffect.Def->DurationPolicy != EGameplayEffectDurationType::Instant || GameplayEffect.GetPeriod() > 0.0f)
	{
		UE_LOG(LogTemp, Warning, TEXT("Health GAS only accepts instant non-periodic Health/MaxHealth effects on %s; refusing %s."),
			*GetNameSafe(GetOwner()), *GetNameSafe(GameplayEffect.Def));
		return FActiveGameplayEffectHandle();
	}
	FMutation Mutation{EMutation::Effect};
	Mutation.Effect = FGameplayEffectSpecHandle(new FGameplayEffectSpec(GameplayEffect));
	Mutation.Effect.Data->DuplicateEffectContext();
	Mutation.PredictionKey = PredictionKey;
	const FGameplayEffectContextHandle& EffectContext = Mutation.Effect.Data->GetContext();
	Mutation.Causer = EffectContext.GetEffectCauser();
	AActor* Instigator = EffectContext.GetOriginalInstigator();
	if (const APawn* Pawn = Cast<APawn>(Instigator)) Mutation.Instigator = Pawn->GetController();
	else Mutation.Instigator = Cast<AController>(Instigator);
	if (const FHitResult* Hit = EffectContext.GetHitResult())
	{
		Mutation.Context.Direction = (Hit->TraceEnd - Hit->TraceStart).GetSafeNormal();
		Mutation.Context.Location = Hit->ImpactPoint;
		Mutation.Context.BoneName = Hit->BoneName;
		Mutation.Context.bHasHitData = true;
	}
	EnqueueMutation(Mutation);
	// Instant GE 本来没有持续效果句柄；重入调用只表示已接纳，实际通知由最外层事务依序完成。
	return FActiveGameplayEffectHandle();
}

// 将请求按值排队，解决同线程委托重入；不是后台队列，也不把结算延迟到下一帧。
void UfpstrueHealthComponent::EnqueueMutation(const FMutation& Mutation)
{
	// 退出期间不接纳请求；同步通知链达到上限时只拒绝新请求，不丢弃已有事务。
	if (bEndingHealthPlay || !HasHealthAuthority())
	{
		return;
	}
	if (bDispatchingMutation && PendingMutations.Num() >= MaxMutationsPerDispatch)
	{
		// 在入队前拒绝新请求，而不是截断循环丢掉已经接纳的伤害；不把循环推迟到下一帧。
		++RejectedMutationCount;
		if (!bReportedMutationLimit)
		{
			bReportedMutationLimit = true;
			UE_LOG(LogTemp, Warning, TEXT("Health mutation admission limit reached for %s; refusing further reentrant requests in this notification chain."),
				*GetNameSafe(GetOwner()));
		}
		return;
	}
	// ★【重要边界】[回调重入] 这里串行化的是同一游戏线程里的嵌套通知，不是多线程队列；回调追加的伤害由外层接着处理。
	// 重入调用只追加；最外层持有分发保护，继续处理回调期间追加的请求。
	PendingMutations.Add(Mutation);
	if (bDispatchingMutation)
	{
		return;
	}
	bReportedMutationLimit = false;
	TGuardValue<bool> DispatchGuard(bDispatchingMutation, true);
	for (int32 Index = 0; Index < PendingMutations.Num() && !bEndingHealthPlay; ++Index)
	{
		// Broadcast 中的 Add 可能扩容；本轮只保留值快照，不引用数组元素。
		const FMutation Current = PendingMutations[Index];
		ApplyMutation(Current);
	}
	PendingMutations.Reset();
}

// 执行单个生命事务；重置只修改数值，伤害仅通知实际扣除量并保护一次死亡结算。
void UfpstrueHealthComponent::ApplyMutation(const FMutation& Mutation)
{
	if (bEndingHealthPlay || !HasHealthAuthority()) return;
	EnsureHealthAttributes();
	if (HealthAttributes == nullptr) return;
	if (!AbilityActorInfo.IsValid() || !AbilityActorInfo->OwnerActor.IsValid()) InitAbilityActorInfo(GetOwner(), GetOwner());
	const float PreviousHealth = HealthAttributes->GetHealth();
	const float PreviousMaximum = HealthAttributes->GetMaxHealth();
	FGameplayEffectSpecHandle EffectSpec = Mutation.Effect;
	const bool bReset = Mutation.Type == EMutation::Reset || Mutation.Type == EMutation::ResetMaximum;
	const bool bResetNotification = bReset || (EffectSpec.IsValid() && EffectSpec.Data->Def != nullptr
		&& EffectSpec.Data->Def->IsA<UfpstrueHealthResetEffect>());
	// 重置分支校正最大血量并回满，不恢复移动、武器、AI 或布娃娃等角色状态。
	if (bReset)
	{
		const float RequestedMaximum = Mutation.Type == EMutation::ResetMaximum ? Mutation.Value : MaxHealth;
		const float SafeMaximum = FMath::IsFinite(RequestedMaximum) ? FMath::Max(RequestedMaximum, 1.0f) : 100.0f;
		EffectSpec = MakeOutgoingSpec(UfpstrueHealthResetEffect::StaticClass(), 1.0f, MakeEffectContext());
		if (!EffectSpec.IsValid()) return;
		EffectSpec.Data->SetSetByCallerMagnitude(FpstrueGameplayTags::Data_MaxHealth, SafeMaximum);
		EffectSpec.Data->SetSetByCallerMagnitude(FpstrueGameplayTags::Data_Health, SafeMaximum);
	}
	else if (Mutation.Type == EMutation::Damage)
	{
		if (PreviousHealth <= 0.0f) return;
		FGameplayEffectContextHandle EffectContext = MakeEffectContext();
		AActor* SourceActor = Mutation.Instigator.IsValid() ? Mutation.Instigator->GetPawn() : nullptr;
		if (SourceActor == nullptr) SourceActor = Mutation.Instigator.Get();
		EffectContext.AddInstigator(SourceActor, Mutation.Causer.Get());
		EffectSpec = MakeOutgoingSpec(UfpstrueHealthDamageEffect::StaticClass(), 1.0f, EffectContext);
		if (!EffectSpec.IsValid()) return;
		EffectSpec.Data->SetSetByCallerMagnitude(FpstrueGameplayTags::Data_Damage, -Mutation.Value);
	}
	if (!EffectSpec.IsValid()) return;

	// ★【重要边界】[结算去重] 先提交扣血与首次死亡标记，再发送通知；多个敌人的连续伤害不能重复触发同一死亡结算。
	// 伤害先提交数值和死亡边沿；回调中的新伤害无法改写本事务的结果。
	// 内部只调用基类，不能再从公共覆写入口排一次队；外部 GE 与旧 UE 伤害共享后面的结算和通知。
	Super::ApplyGameplayEffectSpecToSelf(*EffectSpec.Data.Get(), Mutation.PredictionKey);
	if (bEndingHealthPlay) return;
	MaxHealth = HealthAttributes->GetMaxHealth();
	CurrentHealth = HealthAttributes->GetHealth();
	const float AppliedDamage = FMath::Max(PreviousHealth - CurrentHealth, 0.0f);
	// Reset 是数值操作，不是生命会话事件。无变化不再广播，避免 Reset -> Changed -> Reset 自激。
	if (CurrentHealth == PreviousHealth && MaxHealth == PreviousMaximum)
	{
		CommitHealthSnapshot();
		return; // 浮点精度下未改变数值，也不发出可重复生成同类请求的通知。
	}
	const bool bDied = IsDead() && !bDeathBroadcast;
	bDeathBroadcast = IsDead();
	CommitHealthSnapshot();
	SetLooseGameplayTagCount(FpstrueGameplayTags::State_Dead, IsDead() ? 1 : 0);
	if (bDied)
	{
		// 死亡标签先阻止新能力，再取消已有能力；取消回调产生的生命请求仍由外层队列顺序处理。
		CancelAllAbilities();
		if (bEndingHealthPlay) return;
	}

	// 先通知携带命中快照的原生消费者，再通知蓝图受击、血量和死亡；退出时立即停止余下通知。
	if (AppliedDamage > 0.0f && !bResetNotification)
	{
		OnDamageResolved.Broadcast(AppliedDamage, Mutation.Causer.Get(), Mutation.Instigator.Get(), Mutation.Context);
		if (bEndingHealthPlay) return;
		OnDamageReceived.Broadcast(AppliedDamage, Mutation.Causer.Get(), Mutation.Instigator.Get());
		if (bEndingHealthPlay)
		{
			return;
		}
	}
	OnHealthChanged.Broadcast(CurrentHealth);

	if (bDied && !bEndingHealthPlay)
	{
		// 死亡是边沿事件而不是持续状态：只在首次从存活跨到 0 时广播一次。
		OnDeath.Broadcast();
	}
}

// 组件始终通过所属 Actor 的网络角色判断写权限，无 Owner 的临时对象也不能接纳生命修改。
bool UfpstrueHealthComponent::HasHealthAuthority() const
{
	const AActor* Owner = GetOwner();
	return Owner != nullptr && Owner->HasAuthority();
}

// 快照提交先于所有委托；同步重入仍由原事务队列串行化，不直接重入下一次复制提交。
void UfpstrueHealthComponent::CommitHealthSnapshot()
{
	if (!HasHealthAuthority()) return;
	if (HealthSnapshot.Revision != 0 && HealthSnapshot.CurrentHealth == CurrentHealth && HealthSnapshot.MaxHealth == MaxHealth)
	{
		return;
	}
	HealthSnapshot.CurrentHealth = CurrentHealth;
	HealthSnapshot.MaxHealth = MaxHealth;
	++HealthSnapshot.Revision;
	if (HealthSnapshot.Revision == 0) ++HealthSnapshot.Revision;
	if (AActor* Owner = GetOwner(); Owner != nullptr && Owner->GetIsReplicated() && Owner->GetNetMode() != NM_Standalone)
	{
		Owner->ForceNetUpdate();
	}
}

// 客户端只消费服务器快照，不伪造伤害上下文；回满清除生命组件的死亡通知锁，但不复活角色表现。
void UfpstrueHealthComponent::OnRep_HealthSnapshot()
{
	if (bEndingHealthPlay || HasHealthAuthority()) return;
	// 保留旧反射入口，但不消费外部写入的旧快照；只从已经复制到本地的真实 GAS 属性读取。
	EnsureHealthAttributes();
	if (HealthAttributes == nullptr) return;
	MaxHealth = FMath::IsFinite(HealthAttributes->GetMaxHealth()) ? FMath::Max(HealthAttributes->GetMaxHealth(), 1.0f) : 100.0f;
	CurrentHealth = FMath::IsFinite(HealthAttributes->GetHealth()) ? FMath::Clamp(HealthAttributes->GetHealth(), 0.0f, MaxHealth) : 0.0f;
	HealthSnapshot.CurrentHealth = CurrentHealth;
	HealthSnapshot.MaxHealth = MaxHealth;
	++HealthSnapshot.Revision;
	const bool bDied = IsDead() && !bDeathBroadcast;
	bDeathBroadcast = IsDead();
	SetLooseGameplayTagCount(FpstrueGameplayTags::State_Dead, IsDead() ? 1 : 0);
	if (bDied) CancelAllAbilities();
	if (bEndingHealthPlay) return;
	OnHealthChanged.Broadcast(CurrentHealth);
	if (bDied && !bEndingHealthPlay) OnDeath.Broadcast();
}

// ==================== 状态重置与只读查询 ====================

// 提交修改上限并回满的数值请求；重入时仍遵循当前生命事务队列的顺序。
void UfpstrueHealthComponent::SetMaxHealthAndReset(float NewMaxHealth)
{
	// 只通过组件修改生命上限，避免测试器绕过生命值唯一写入者直接改成员。
	EnqueueMutation({EMutation::ResetMaximum, NewMaxHealth});
}

// 提交回满血量请求，不等于完整复活角色；数值未变化时不重复发布血量事件。
void UfpstrueHealthComponent::ResetHealth()
{
	// ★【重要边界】[生命周期] 回满血只是数值重置，不会复活角色；移动、AI、武器和死亡表现仍由各自所有者恢复。
	// 把配置值修正到合法范围，并重建本组件的“存活”状态；可供对象复用或新一局初始化。
	// 这里只恢复生命数据；角色自身的移动、武器和死亡表现标志仍需由 Owner 配套恢复。
	EnqueueMutation({EMutation::Reset});
}

// 提供 UI 所需的血量比例，统一处理非法或零上限，不修改生命状态。
float UfpstrueHealthComponent::GetHealthNormalized() const
{
	// HUD 读取的比例在组件内统一计算，MaxHealth 异常时安全返回 0。
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}
