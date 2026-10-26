// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"
#include "Abilities/GameplayAbility.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/MiscTrace.h"

// 保留原生缓存边界保护；Fire 已迁到唯一输入批次入口，旧 TargetData 不再执行或积压。
void UfpstrueHealthComponent::ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
	FPredictionKey AbilityOriginalPredictionKey, const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey)
{
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(AbilityHandle);
	UGameplayAbility* Instance = Spec != nullptr ? Spec->GetPrimaryInstance() : nullptr;
	const bool bCurrent = Instance != nullptr && Instance->IsActive()
		&& Instance->GetCurrentActivationInfo().GetActivationPredictionKey() == AbilityOriginalPredictionKey;
	if (!bCurrent || IsDead() || (Spec->Ability != nullptr && Spec->Ability->IsA<UfpstrueFireAbility>()))
	{
		// 当前项目均为同 ASC 通道的实例化能力；未知/非活动能力不能借目标数据 RPC 占用旧缓存。
		ConsumeAllReplicatedData(AbilityHandle, AbilityOriginalPredictionKey);
		return;
	}
	Super::ServerSetReplicatedTargetData_Implementation(AbilityHandle, AbilityOriginalPredictionKey, TargetData,
		ApplicationTag, CurrentPredictionKey);
}

void UfpstrueHealthComponent::RememberClosedShotStream(FGameplayAbilitySpecHandle Handle, FPredictionKey Key,
	UfpstrueWeaponComponent* Weapon, bool bCancelled)
{
	if (!HasHealthAuthority() || !IsValid(Weapon) || Weapon->Character.Get() != GetAvatarActor() || GetWorld() == nullptr) return;
	ClosedShotStreams.RemoveAll([this, Handle, Key](const FClosedShotStream& Stream)
		{ return GetWorld()->GetTimeSeconds() - Stream.ClosedAt > 2.0 || (Stream.Handle == Handle && Stream.Key == Key); });
	if (ClosedShotStreams.Num() == 8) ClosedShotStreams.RemoveAt(0, 1, EAllowShrinking::No);
	ClosedShotStreams.Add({Handle, Key, GetAvatarActor(), Weapon, Weapon->EquipmentRevision,
		Weapon->InputWindowSession != 0 ? Weapon->InputWindowSession : Weapon->FireSession, GetWorld()->GetTimeSeconds(), bCancelled});
}

void UfpstrueHealthComponent::ServerSubmitShotBatch_Implementation(FGameplayAbilitySpecHandle Handle, FPredictionKey Key,
	uint32 FirstShotId, const TArray<FFPShotRequest>& Requests)
{
	RouteShotInputBatch(Handle, Key, FirstShotId, Requests, false, false, false);
}

void UfpstrueHealthComponent::ServerFinishShotInputs_Implementation(FGameplayAbilitySpecHandle Handle, FPredictionKey Key,
	uint32 FirstShotId, const TArray<FFPShotRequest>& Requests, bool bCancelled, bool bExpired)
{
	RouteShotInputBatch(Handle, Key, FirstShotId, Requests, true, bCancelled, bExpired);
}

void UfpstrueHealthComponent::RouteShotInputBatch(FGameplayAbilitySpecHandle Handle, FPredictionKey Key,
	uint32 FirstShotId, const TArray<FFPShotRequest>& Requests, bool bControl, bool bCancelled, bool bExpired)
{
	if (!HasHealthAuthority() || GetWorld() == nullptr || Requests.IsEmpty() || Requests.Num() > 8
		|| FirstShotId == 0 || FirstShotId > MAX_int32 || !Key.IsValidKey()) return;
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (Spec == nullptr || Spec->Ability == nullptr || !Spec->Ability->IsA<UfpstrueFireAbility>()) return;
	const FFPShotRequest& Header = Requests[0];
	if (Header.Avatar != GetAvatarActor() || !IsValid(Header.Weapon) || Header.FireSession == 0 || Header.EquipmentGeneration == 0) return;
	for (const FFPShotRequest& Request : Requests)
		if (Request.Avatar != Header.Avatar || Request.Weapon != Header.Weapon || Request.EquipmentGeneration != Header.EquipmentGeneration
			|| Request.FireSession != Header.FireSession || Request.ShotId < FirstShotId || Request.ShotId > MAX_int32) return;
	auto* Fire = Cast<UfpstrueFireAbility>(Spec->GetPrimaryInstance());
#if !UE_BUILD_SHIPPING
	const auto TraceRoute = [this, Handle, Key, FirstShotId, &Requests, &Header, Fire, bControl, bCancelled, bExpired](const TCHAR* Decision)
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("PvpShotLifecycleTrace"))) return;
		const bool bActive = Fire != nullptr && Fire->IsActive();
		const int32 ActiveKey = Fire != nullptr ? Fire->GetCurrentActivationInfo().GetActivationPredictionKey().Current : 0;
		TRACE_BOOKMARK(TEXT("FPShotLifecycle Route Spec=%u Key=%d First=%u Count=%d Active=%d ActiveKey=%d Decision=%s Generation=%u Session=%u"),
			GetTypeHash(Handle), Key.Current, FirstShotId, Requests.Num(), bActive, ActiveKey, Decision, Header.EquipmentGeneration, Header.FireSession);
		UE_LOG(LogTemp, Display, TEXT("PVP_SHOT_LIFECYCLE ROUTE Spec=%u Key=%d First=%u Count=%d HeaderShot=%u LastShot=%u Active=%d ActiveKey=%d Decision=%s Generation=%u Session=%u Weapon=%s Control=%d Cancelled=%d Expired=%d ServerTime=%.6f LocalSeconds=%.9f"),
			GetTypeHash(Handle), Key.Current, FirstShotId, Requests.Num(), Header.ShotId, Requests.Last().ShotId, bActive, ActiveKey,
			Decision, Header.EquipmentGeneration, Header.FireSession, *GetNameSafe(Header.Weapon), bControl, bCancelled, bExpired,
			GetWorld()->GetTimeSeconds(), FPlatformTime::Seconds());
	};
	TraceRoute(TEXT("Entry"));
#endif
	if (Fire != nullptr && Fire->IsActive() && Fire->GetCurrentActivationInfo().GetActivationPredictionKey() == Key)
	{
#if !UE_BUILD_SHIPPING
		TraceRoute(TEXT("Active"));
#endif
		UfpstrueWeaponComponent* Weapon = Fire->ActiveWeapon.Get();
		if (!IsValid(Weapon) || !Weapon->MatchesShotEquipment(Header)) return;
		const uint32 ActivationRevision = Fire->ActivationRevision;
		if (bControl && (bCancelled || bExpired)) Weapon->ReceiveShotInputBatch(FirstShotId, Requests, false, bCancelled, bExpired);
		else Fire->HandleShotInputs(FirstShotId, Requests);
		// 出手/伤害通知可在同一调用栈取消并重启同一个 GA 对象；旧结束控制只拥有进入时的那轮。
		if (bControl && !bExpired && Fire->IsActive() && Fire->ActivationRevision == ActivationRevision
			&& Fire->GetCurrentAbilitySpecHandle() == Handle && Fire->GetCurrentActivationInfo().GetActivationPredictionKey() == Key
			&& Fire->ActiveWeapon.Get() == Weapon) Fire->FinishRemoteShotInputs(bCancelled);
		return;
	}
	// 激活前的包不占缓存，随后只能靠拥有者的有限重发；可靠激活过晚时，本轮仍可能失去有效出手机会。
	const double Now = GetWorld()->GetTimeSeconds();
	auto FinalizeWithoutExecution = [&Requests, Now](UfpstrueWeaponComponent* Weapon, EFPShotResult Result, bool bRequireExpired)
	{
		for (const FFPShotRequest& Request : Requests)
		{
			if (!Weapon->MatchesShotEquipment(Request)) continue;
			if (bRequireExpired && (!FMath::IsFinite(Request.ServerFireTime)
				|| Now - Request.ServerFireTime <= UfpstrueWeaponComponent::MaxNetworkShotAgeSeconds)) continue;
			// 已执行/已准入的原结果仍由同一个执行器和可靠重传负责，取消收据不能改写承诺。
			if (Request.ShotId <= Weapon->LastExecutedShotId || Weapon->PendingNetworkShots.ContainsByPredicate([&Request](const UfpstrueWeaponComponent::FPendingNetworkShot& Pending)
				{ return Pending.Request.ShotId == Request.ShotId && Pending.Request.FireSession == Request.FireSession
					&& Pending.Request.EquipmentGeneration == Request.EquipmentGeneration; })) continue;
			Weapon->AcknowledgeShot(Request, Result);
		}
	};
	ClosedShotStreams.RemoveAll([Now](const FClosedShotStream& Stream) { return Now - Stream.ClosedAt > 2.0; });
	FClosedShotStream* Closed = ClosedShotStreams.FindByPredicate([this, Handle, Key, &Header](const FClosedShotStream& Stream)
	{
		return Stream.Handle == Handle && Stream.Key == Key && Stream.Avatar.Get() == GetAvatarActor()
			&& Stream.Weapon.IsValid() && Stream.Weapon->GetOwner() == Header.Weapon && Stream.Generation == Header.EquipmentGeneration
			&& (Stream.Session == 0 || Stream.Session == Header.FireSession);
	});
	if (Closed == nullptr)
	{
#if !UE_BUILD_SHIPPING
		TraceRoute(TEXT("NoAuthorization"));
#endif
		// 执行授权已过期/被有界回执淘汰，不代表允许把本端账永远悬空。
		// 只接受当前真实持有者的过期控制作终结；未知新键/新鲜输入仍不回复，更不执行。
		UfpstrueWeaponComponent* Weapon = Header.Weapon->FindComponentByClass<UfpstrueWeaponComponent>();
		if (bControl && IsValid(Weapon) && Weapon->MatchesShotEquipment(Header)
			&& Weapon->GetOwner()->GetOwner() == GetAvatarActor() && Weapon->Character.IsValid()
			&& Weapon->Character->GetEquippedWeaponComponent() == Weapon)
			FinalizeWithoutExecution(Weapon, EFPShotResult::InvalidTime, true);
		return;
	}
#if !UE_BUILD_SHIPPING
	TraceRoute(TEXT("Closed"));
#endif
	// 可能整批均在原生激活前丢失；同 key 第一次关闭消息固定未解析的会话，之后不能更换。
	if (Closed->Session == 0) Closed->Session = Header.FireSession;
	Closed->bCancelled |= bCancelled;
	UfpstrueWeaponComponent* Weapon = Closed->Weapon.Get();
	if (!Weapon->MatchesShotEquipment(Header) || Weapon->GetOwner()->GetOwner() != GetAvatarActor()
		|| !Weapon->Character.IsValid() || Weapon->Character->GetEquippedWeaponComponent() != Weapon) return;
	const bool bCannotExecute = Closed->bCancelled || bCancelled || bExpired || IsDead()
		|| HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading) || HasMatchingGameplayTag(FpstrueGameplayTags::State_Dead)
		|| Weapon->FirePresentationKey != Key.Current || Now - Closed->ClosedAt > UfpstrueWeaponComponent::MaxNetworkShotAgeSeconds;
	if (bCannotExecute)
	{
		if (!bExpired && Weapon->FirePresentationKey == Key.Current && Weapon->InputWindowSession == Closed->Session)
			Weapon->CancelOrderedShotInputs();
		FinalizeWithoutExecution(Weapon,
			bExpired || Now - Closed->ClosedAt > UfpstrueWeaponComponent::MaxNetworkShotAgeSeconds ? EFPShotResult::InvalidTime : EFPShotResult::Cancelled, false);
		return;
	}
	Weapon->InputClosedServerSeconds = Closed->ClosedAt;
	Weapon->ReceiveShotInputBatch(FirstShotId, Requests, true);
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

// 将复制的生命变化交给 GAS 聚合器；旧蓝图通知留到本属性集批次接收完成后统一发布。
void UfpstrueHealthAttributeSet::OnRep_Health(const FGameplayAttributeData& Previous)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UfpstrueHealthAttributeSet, Health, Previous);
	bReceivedHealthAttributes = true;
}

// 更新 GAS 最大生命聚合器并标记本批次有变化，不在这里单独广播兼容镜像。
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
		HealthComponent->ConsumeReplicatedHealthAttributes(*this);
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
	// 【设计取舍】Minimal 限制活动效果细节的复制，不代替 AttributeSet 数值复制；
	// 客户端兼容读接口只是本地镜像，不能再复制一份生命状态与 GAS 竞争更新顺序。
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
	// 拒绝无效输入；尸体伤害在事务执行时判定，兼容排在前面的生命重置。
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
	// 【设计取舍】本项目把生命写入限制为即时非周期事务，才能统一通知和死亡顺序；
	// 持续/周期生命效果明确拒绝，而非宣称这层适配已经覆盖所有 GAS 效果生命周期。
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
// 【设计取舍】串行提交让伤害/重置监听者看见有序状态；保留容量上限阻止事件自激无限延长通知链。
// 这解决 Game Thread 重入，不提供跨线程访问能力，也不能替代业务攻击身份校验。
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
	// 重入只追加，由最外层按顺序处理；整个队列始终在 Game Thread 内执行。
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
		// 【设计取舍】只跳过已回满且上限相同的内置重置，避免无变化 GE 和 Reset/Changed 反馈环。
		// 外部 GE 不能仅凭数值相同就整体跳过；其余效果职责仍由 GAS 执行。
		if (PreviousMaximum == SafeMaximum && PreviousHealth == SafeMaximum)
		{
			MaxHealth = PreviousMaximum;
			CurrentHealth = PreviousHealth;
			return;
		}
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

	// 内部调用基类提交 GE，不能再走公共入口重复排队。先提交状态，再通知监听者。
	Super::ApplyGameplayEffectSpecToSelf(*EffectSpec.Data.Get(), Mutation.PredictionKey);
	if (bEndingHealthPlay) return;
	MaxHealth = HealthAttributes->GetMaxHealth();
	CurrentHealth = HealthAttributes->GetHealth();
	const float AppliedDamage = FMath::Max(PreviousHealth - CurrentHealth, 0.0f);
	// Reset 是数值操作，不是生命会话事件。无变化不再广播，避免 Reset -> Changed -> Reset 自激。
	if (CurrentHealth == PreviousHealth && MaxHealth == PreviousMaximum)
	{
		return;
	}
	const bool bDied = IsDead() && !bDeathBroadcast;
	bDeathBroadcast = IsDead();
	// GAS 属性是唯一复制数据；真实变化只请求一次 Actor 调度，不维护第二份版本化镜像。
	if (AActor* Owner = GetOwner(); Owner != nullptr && Owner->GetIsReplicated() && Owner->GetNetMode() != NM_Standalone)
	{
		Owner->ForceNetUpdate();
	}
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
		if (bEndingHealthPlay) return;
	}
	OnHealthChanged.Broadcast(CurrentHealth);

	if (bDied && !bEndingHealthPlay)
	{
		OnDeath.Broadcast();
	}
}

// 组件始终通过所属 Actor 的网络角色判断写权限，无 Owner 的临时对象也不能接纳生命修改。
bool UfpstrueHealthComponent::HasHealthAuthority() const
{
	const AActor* Owner = GetOwner();
	return Owner != nullptr && Owner->HasAuthority();
}

// 客户端只消费本属性集的接收批次；回满重置死亡通知边沿，不复活角色表现。
void UfpstrueHealthComponent::ConsumeReplicatedHealthAttributes(const UfpstrueHealthAttributeSet& Attributes)
{
	if (bEndingHealthPlay || HasHealthAuthority()) return;
	const float ReceivedMaximum = FMath::IsFinite(Attributes.GetMaxHealth()) ? FMath::Max(Attributes.GetMaxHealth(), 1.0f) : 100.0f;
	const float ReceivedHealth = FMath::IsFinite(Attributes.GetHealth()) ? FMath::Clamp(Attributes.GetHealth(), 0.0f, ReceivedMaximum) : 0.0f;
	if (bReceivedInitialHealth && CurrentHealth == ReceivedHealth && MaxHealth == ReceivedMaximum) return;
	bReceivedInitialHealth = true;
	MaxHealth = ReceivedMaximum;
	CurrentHealth = ReceivedHealth;
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
	EnqueueMutation({EMutation::Reset});
}

// 提供 UI 所需的血量比例，统一处理非法或零上限，不修改生命状态。
float UfpstrueHealthComponent::GetHealthNormalized() const
{
	// HUD 读取的比例在组件内统一计算，MaxHealth 异常时安全返回 0。
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}
