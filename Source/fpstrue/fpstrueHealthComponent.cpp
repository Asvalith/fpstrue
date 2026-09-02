// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueHealthComponent.h"

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

UfpstrueHealthComponent::UfpstrueHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// Blueprint 覆盖的 MaxHealth 到 BeginPlay 才最终可用；Owner 会在绑定委托后主动读取初始快照。
	ResetHealth();

	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.AddUniqueDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}
}

void UfpstrueHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AActor* Owner = GetOwner())
	{
		Owner->OnTakeAnyDamage.RemoveDynamic(this, &UfpstrueHealthComponent::HandleOwnerTakeAnyDamage);
	}

	Super::EndPlay(EndPlayReason);
}

void UfpstrueHealthComponent::ApplyDamageInternal(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	// 先拒绝无效伤害和尸体重复伤害，再统一 Clamp；外部系统不能绕过这里直接写 CurrentHealth。
	if (DamageAmount <= 0.0f || IsDead())
	{
		return;
	}

	const float PreviousHealth = CurrentHealth;
	CurrentHealth = FMath::Clamp(CurrentHealth - DamageAmount, 0.0f, MaxHealth);
	const float AppliedDamage = PreviousHealth - CurrentHealth;

	OnDamageReceived.Broadcast(AppliedDamage, DamageCauser, InstigatedBy);
	OnHealthChanged.Broadcast(CurrentHealth);

	if (IsDead() && !bDeathBroadcast)
	{
		// 死亡是边沿事件而不是持续状态：只在首次从存活跨到 0 时广播一次。
		bDeathBroadcast = true;
		OnDeath.Broadcast();
	}
}

// ==================== 状态重置与只读查询 ====================

void UfpstrueHealthComponent::ResetHealth()
{
	MaxHealth = FMath::Max(1.0f, MaxHealth);
	CurrentHealth = MaxHealth;
	bDeathBroadcast = false;
	OnHealthChanged.Broadcast(CurrentHealth);
}

float UfpstrueHealthComponent::GetHealthNormalized() const
{
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}

// UE 伤害委托只负责适配参数，真正的扣血、Clamp 和幂等死亡都走 ApplyDamageInternal。
void UfpstrueHealthComponent::HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType,
													   AController* InstigatedBy, AActor* DamageCauser)
{
	ApplyDamageInternal(Damage, DamageCauser, InstigatedBy);
}
