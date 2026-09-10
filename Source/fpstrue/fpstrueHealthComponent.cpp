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

// 生命组件不需要 Tick，血量只在伤害、重置和生命周期事件发生时变化。
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
	// Owner 生命周期结束前成对解除伤害委托，防止退出阶段再次进入组件逻辑。
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

void UfpstrueHealthComponent::SetMaxHealthAndReset(float NewMaxHealth)
{
	// 只通过组件修改生命上限，避免测试器绕过生命值唯一写入者直接改成员。
	MaxHealth = FMath::Max(NewMaxHealth, 1.0f);
	ResetHealth();
}

void UfpstrueHealthComponent::ResetHealth()
{
	// 把配置值修正到合法范围，并重建“存活”状态；可供对象复用或新一局初始化。
	MaxHealth = FMath::Max(1.0f, MaxHealth);
	CurrentHealth = MaxHealth;
	bDeathBroadcast = false;
	OnHealthChanged.Broadcast(CurrentHealth);
}

float UfpstrueHealthComponent::GetHealthNormalized() const
{
	// HUD 读取的比例在组件内统一计算，MaxHealth 异常时安全返回 0。
	return MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f;
}

// UE 伤害委托只负责适配参数，真正的扣血、Clamp 和幂等死亡都走 ApplyDamageInternal。
void UfpstrueHealthComponent::HandleOwnerTakeAnyDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType,
													   AController* InstigatedBy, AActor* DamageCauser)
{
	ApplyDamageInternal(Damage, DamageCauser, InstigatedBy);
}
