// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Shared/fpstrueHealthComponent.h"
#include "GameFramework/Controller.h"

/*
 * 玩家和敌人共用的生命值组件。
 * Owner 仍通过 UE 的 ApplyDamage/OnTakeAnyDamage 进入系统，本组件只保存权威血量并广播只发生一次的死亡事件，
 * 具体的布娃娃、停止 AI、HUD 等表现由各自订阅者处理。
 *
 * 数据流：GameplayStatics::ApplyDamage/ApplyPointDamage -> Owner::OnTakeAnyDamage -> 本组件 Clamp 血量
 *       -> OnDamageReceived / OnHealthChanged -> 血量归零时 OnDeath。
 * 组件不认识玩家、敌人、HUD 或 GameMode，因此同一套伤害和死亡语义可以被不同 Actor 复用。
 */

// ==================== 生命周期与伤害入口 ====================

// 生命组件不需要 Tick，血量只在伤害、重置和生命周期事件发生时变化。
UfpstrueHealthComponent::UfpstrueHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueHealthComponent::BeginPlay()
{
	Super::BeginPlay();
	bEndingHealthPlay = false;

	// Blueprint 覆盖的 MaxHealth 到 BeginPlay 才最终可用；Owner 会在绑定委托后主动读取初始快照。
	ResetHealth();

	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.AddUniqueDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}
}

void UfpstrueHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingHealthPlay = true;
	PendingMutations.Reset();
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
	if (!FMath::IsFinite(Damage) || Damage <= 0.0f)
	{
		return;
	}
	EnqueueMutation({EMutation::Damage, Damage, DamageCauser, InstigatedBy, IncomingDamageContext});
}

void UfpstrueHealthComponent::EnqueueMutation(const FMutation& Mutation)
{
	if (bEndingHealthPlay)
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

void UfpstrueHealthComponent::ApplyMutation(const FMutation& Mutation)
{
	if (Mutation.Type != EMutation::Damage)
	{
		const float PreviousHealth = CurrentHealth;
		const float PreviousMaximum = MaxHealth;
		if (Mutation.Type == EMutation::ResetMaximum)
		{
			MaxHealth = Mutation.Value;
		}
		MaxHealth = FMath::IsFinite(MaxHealth) ? FMath::Max(MaxHealth, 1.0f) : 100.0f;
		CurrentHealth = MaxHealth;
		bDeathBroadcast = false;
		// Reset 是数值操作，不是生命会话事件。无变化不再广播，避免 Reset -> Changed -> Reset 自激。
		if (CurrentHealth != PreviousHealth || MaxHealth != PreviousMaximum)
		{
			OnHealthChanged.Broadcast(CurrentHealth);
		}
		return;
	}
	if (IsDead())
	{
		return;
	}

	const float PreviousHealth = CurrentHealth;
	CurrentHealth = FMath::Clamp(CurrentHealth - Mutation.Value, 0.0f, MaxHealth);
	const float AppliedDamage = PreviousHealth - CurrentHealth;
	if (AppliedDamage <= 0.0f) return; // 浮点精度下未改变数值，也不发出可重复生成同类请求的通知。
	const bool bDied = IsDead() && !bDeathBroadcast;
	bDeathBroadcast |= bDied;

	OnDamageResolved.Broadcast(AppliedDamage, Mutation.Causer.Get(), Mutation.Instigator.Get(), Mutation.Context);
	if (bEndingHealthPlay) return;
	OnDamageReceived.Broadcast(AppliedDamage, Mutation.Causer.Get(), Mutation.Instigator.Get());
	if (bEndingHealthPlay)
	{
		return;
	}
	OnHealthChanged.Broadcast(CurrentHealth);

	if (bDied && !bEndingHealthPlay)
	{
		// 死亡是边沿事件而不是持续状态：只在首次从存活跨到 0 时广播一次。
		OnDeath.Broadcast();
	}
}

// ==================== 状态重置与只读查询 ====================

void UfpstrueHealthComponent::SetMaxHealthAndReset(float NewMaxHealth)
{
	// 只通过组件修改生命上限，避免测试器绕过生命值唯一写入者直接改成员。
	EnqueueMutation({EMutation::ResetMaximum, NewMaxHealth});
}

void UfpstrueHealthComponent::ResetHealth()
{
	// 把配置值修正到合法范围，并重建本组件的“存活”状态；可供对象复用或新一局初始化。
	// 这里只恢复生命数据；角色自身的移动、武器和死亡表现标志仍需由 Owner 配套恢复。
	EnqueueMutation({EMutation::Reset});
}

float UfpstrueHealthComponent::GetHealthNormalized() const
{
	// HUD 读取的比例在组件内统一计算，MaxHealth 异常时安全返回 0。
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}
