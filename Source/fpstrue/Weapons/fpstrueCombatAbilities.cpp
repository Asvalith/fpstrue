// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueCombatAbilities.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "Abilities/Tasks/AbilityTask_WaitInputRelease.h"
#include "AbilitySystemComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/PackageMapClient.h"

// Character 提交按下/松开 -> GAS 能力持有标签与任务 -> WeaponComponent 执行逐发或换弹事务。
// 能力不另存弹药、伤害或动画时钟；只在可能调用外部监听者的边界保护当前激活身份。
// 【设计取舍】InstancedPerActor 复用能力对象；外部同步回调可结束并重启它，所以保留 ActivationRevision。
// IsActive 只能说明“当前有激活”，不能证明仍是旧调用那轮；版本只跨回调核对，不是多线程锁。

// UE PackageMap 处理角色引用，剩余字段按固定协议往返；返回映射结果，bOutSuccess 报告字段序列化错误。
bool FGameplayAbilityTargetData_FPShot::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	UObject* Avatar = Request.Avatar.Get();
	const bool bMapped = Map != nullptr && Map->SerializeObject(Ar, AfpstrueCharacter::StaticClass(), Avatar);
	if (Ar.IsLoading()) Request.Avatar = Cast<AfpstrueCharacter>(Avatar);
	Ar << Request.EquipmentGeneration << Request.FireSession << Request.ShotId << Request.ServerFireTime;
	bool bDirectionValid = false;
	Request.AimDirection.NetSerialize(Ar, Map, bDirectionValid);
	bOutSuccess = bDirectionValid && !Ar.IsError();
	return bMapped;
}

// 互斥由 GAS 标签管理，武器仍在服务器逐发检查弹药、射速和伤害。
UfpstrueFireAbility::UfpstrueFireAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	bRetriggerInstancedAbility = false;
	bReplicateInputDirectly = false;
	// 正常松开走输入释放事件；强取消由服务器裁决。
	bServerRespectsRemoteAbilityCancellation = false;
	SetAssetTags(FGameplayTagContainer(FpstrueGameplayTags::Ability_Fire));
	ActivationOwnedTags.AddTag(FpstrueGameplayTags::State_Firing);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Reloading);
}

bool UfpstrueFireAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)) return false;
	const AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	const UfpstrueWeaponComponent* Weapon = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	return IsValid(Character) && IsValid(Weapon) && Weapon->CanExecuteShot();
}

// 松开任务同时运行于拥有者和服务器；远端观察者的枪口/弹道仍由武器表现同步。
void UfpstrueFireAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData*)
{
	if (!IsActive()) return;
	const uint32 RequestedActivation = ++ActivationRevision;
	bEnding = false;
	AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	UfpstrueWeaponComponent* Weapon = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	UAbilitySystemComponent* ASC = ActorInfo != nullptr ? ActorInfo->AbilitySystemComponent.Get() : nullptr;
	if (!IsValid(Weapon) || ASC == nullptr)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	// CommitAbility -> K2_CommitExecute / NotifyAbilityCommit，同步监听者可结束或重启能力。
	if (ActivationRevision != RequestedActivation || !IsActive() || bEnding) return;
	if (!bCommitted || !IsValid(Weapon) || !Weapon->CanExecuteShot())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	ActiveWeapon = Weapon;
	UAbilityTask_WaitInputRelease* ReleaseTask = UAbilityTask_WaitInputRelease::WaitInputRelease(this, true);
	ReleaseTask->OnRelease.AddDynamic(this, &ThisClass::HandleInputReleased);
	ReleaseTask->ReadyForActivation();
	// ReadyForActivation 会立即处理已松开的输入；返回时能力可能已经结束。
	if (ActivationRevision != RequestedActivation || !IsActive() || bEnding) return;
	const FPredictionKey ActivationKey = ActivationInfo.GetActivationPredictionKey();
	Weapon->BeginAbilityFire(ActivationKey.Current);
	if (!ActorInfo->IsLocallyControlled())
	{
		// 先挂委托再消费缓存，覆盖 TargetData 比服务端能力激活更早到达的情况。
		ShotDataHandle = ASC->AbilityTargetDataSetDelegate(Handle, ActivationKey).AddUObject(this, &ThisClass::HandleShotData);
		ASC->CallReplicatedTargetDataDelegatesIfSet(Handle, ActivationKey);
		if (ActivationRevision == RequestedActivation && IsActive() && ShotDataTimeoutTask == nullptr)
		{
			ShotDataTimeoutTask = UAbilityTask_WaitDelay::WaitDelay(this, 3.0f);
			ShotDataTimeoutTask->OnFinish.AddDynamic(this, &ThisClass::HandleShotDataTimeout);
			ShotDataTimeoutTask->ReadyForActivation();
		}
		return;
	}
	// 初始预测键只标识本轮 GA；联网拥有者每发发送独立 ShotId，单机保留原始直接结算。
	Pulse();
}

// 清理保存的武器，而不是重新读取当前装备，防止旧能力停止新武器。
void UfpstrueFireAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (bEnding || !IsEndAbilityValid(Handle, ActorInfo)) return;
	bEnding = true;
	if (UAbilitySystemComponent* ASC = ActorInfo != nullptr ? ActorInfo->AbilitySystemComponent.Get() : nullptr)
	{
		ASC->AbilityTargetDataSetDelegate(Handle, ActivationInfo.GetActivationPredictionKey()).Remove(ShotDataHandle);
		ASC->ConsumeClientReplicatedTargetData(Handle, ActivationInfo.GetActivationPredictionKey());
	}
	ShotDataHandle.Reset();
	if (ShotDataTimeoutTask != nullptr)
	{
		ShotDataTimeoutTask->EndTask();
		ShotDataTimeoutTask = nullptr;
	}
	UfpstrueWeaponComponent* Weapon = ActiveWeapon.Get();
	ActiveWeapon.Reset();
	if (Weapon != nullptr)
	{
		// 正常松开只停止新输入，不撤销已准入事实；死亡、卸枪和换弹强取消时才清执行队列。
		Weapon->EndAbilityFire(bWasCancelled);
	}
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	// 先由 GAS 解除 Firing，再派生远端表现快照；武器不反向取消能力或重建动作状态机。
	if (IsValid(Weapon)) Weapon->PublishNetworkState();
}

// 松开只结束本次能力，释放事件已由 WaitInputRelease 同步给服务器。
void UfpstrueFireAbility::HandleInputReleased(float)
{
	// EndAbility 先阻止旧调用栈并移除 TargetData 订阅，不再维护第二份“已松开”状态。
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

// 能力任务生成连射输入；武器 Timer 仅消费已准入的网络请求。
void UfpstrueFireAbility::Pulse()
{
	if (!IsActive() || bEnding) return;
	const uint32 RequestedActivation = ActivationRevision;
	UfpstrueWeaponComponent* Weapon = ActiveWeapon.Get();
	if (!IsValid(Weapon) || !Weapon->CanExecuteShot())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}
	const bool bAuthority = HasAuthority(&CurrentActivationInfo);
	// 远端拥有者的服务器 GA 只消费逐发输入，绝不再用独立 Pulse 同时结算第二发。
	if (bAuthority && !CurrentActorInfo->IsLocallyControlled()) return;
	float Delay = Weapon->GetNextShotDelay();
	if ((!bAuthority || Weapon->GetCurrentAmmo() > 0) && Delay <= KINDA_SMALL_NUMBER)
	{
		if (bAuthority)
		{
			if (!Weapon->Fire())
			{
				if (ActivationRevision == RequestedActivation && IsActive())
					EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
				return;
			}
		}
		else
		{
			FFPShotRequest Request;
			if (Weapon->BuildShotRequest(Request))
			{
				FGameplayAbilityTargetDataHandle Data(new FGameplayAbilityTargetData_FPShot());
				static_cast<FGameplayAbilityTargetData_FPShot*>(Data.Get(0))->Request = Request;
				UAbilitySystemComponent* ASC = CurrentActorInfo->AbilitySystemComponent.Get();
				// ASC 的可靠数据与松开事件共用角色通道；仅按武器射速发送，不逐帧发送。
				ASC->ServerSetReplicatedTargetData(CurrentSpecHandle, CurrentActivationInfo.GetActivationPredictionKey(),
					Data, FGameplayTag(), ASC->ScopedPredictionKey);
				Weapon->PresentPredictedShot(Request);
			}
			else
			{
				// 弹药/装备复制尚未就绪时继续有界等待，确认到达后可以恢复，不代替服务器触发装弹。
				Delay = 60.0f / Weapon->GetWeaponSettings().RoundsPerMinute;
			}
		}
		// Fire / PresentPredictedShot 会广播伤害或表现事件，返回后才能安排下一发。
		if (ActivationRevision != RequestedActivation || !IsActive() || bEnding || ActiveWeapon.Get() != Weapon) return;
		Delay = FMath::Max(Delay, Weapon->GetNextShotDelay());
	}
	// 服务器一发现空仓即可切到换弹，不等待客户端估计，也不再绕一次客户端激活往返。
	if (bAuthority && Weapon->GetCurrentAmmo() <= 0)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		if (ActivationRevision == RequestedActivation && !IsActive() && IsValid(Weapon) && Weapon->CanExecuteShot())
			Weapon->RequestReload();
		return;
	}
	// WaitDelay 注册在当前能力上；松开、死亡和卸枪结束 GA 时，GAS 自动销毁等待任务。
	UAbilityTask_WaitDelay* NextShot = UAbilityTask_WaitDelay::WaitDelay(this, FMath::Max(Delay, 0.001f));
	NextShot->OnFinish.AddDynamic(this, &ThisClass::Pulse);
	NextShot->ReadyForActivation();
}

// 消费后保留自己的数据副本；查询、伤害或 ACK 均可能同步结束能力，旧激活不能再排任务。
void UfpstrueFireAbility::HandleShotData(const FGameplayAbilityTargetDataHandle& Data, FGameplayTag)
{
	if (!IsActive() || bEnding || !HasAuthority(&CurrentActivationInfo)) return;
	const uint32 RequestedActivation = ActivationRevision;
	// Consume 会清除 ASC 缓存；先保留共享数据句柄，避免继续借用已被清空的参数。
	const FGameplayAbilityTargetDataHandle SavedData = Data;
	UAbilitySystemComponent* ASC = CurrentActorInfo->AbilitySystemComponent.Get();
	ASC->ConsumeClientReplicatedTargetData(CurrentSpecHandle, CurrentActivationInfo.GetActivationPredictionKey());
	const FGameplayAbilityTargetData* Item = SavedData.Num() == 1 ? SavedData.Get(0) : nullptr;
	const auto* Shot = Item != nullptr && Item->GetScriptStruct() == FGameplayAbilityTargetData_FPShot::StaticStruct()
		? static_cast<const FGameplayAbilityTargetData_FPShot*>(Item) : nullptr;
	UfpstrueWeaponComponent* Weapon = ActiveWeapon.Get();
	if (Shot == nullptr || !IsValid(Weapon))
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}
	const FFPShotRequest Request = Shot->Request;
	const EFPShotResult Result = Weapon->AcceptShotRequest(Request);
	if (ActivationRevision != RequestedActivation || !IsActive() || bEnding || ActiveWeapon.Get() != Weapon) return;
	if (Result == EFPShotResult::Accepted || Result == EFPShotResult::Queued)
	{
		if (ShotDataTimeoutTask != nullptr) ShotDataTimeoutTask->EndTask();
		ShotDataTimeoutTask = UAbilityTask_WaitDelay::WaitDelay(this, 1.0f);
		ShotDataTimeoutTask->OnFinish.AddDynamic(this, &ThisClass::HandleShotDataTimeout);
		ShotDataTimeoutTask->ReadyForActivation();
	}
	if ((Result == EFPShotResult::Accepted || Result == EFPShotResult::NoAmmo) && Weapon->GetCurrentAmmo() == 0)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		if (ActivationRevision == RequestedActivation && !IsActive() && IsValid(Weapon) && Weapon->CanExecuteShot()) Weapon->RequestReload();
	}
}

// 没有有效逐发输入的激活不能长期占据 Firing；超时取消也会解除目标数据订阅。
void UfpstrueFireAbility::HandleShotDataTimeout()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

// Reload 可以打断 Fire，但 Fire 不能阻止 Reload 进入，否则无法触发该取消关系。
UfpstrueReloadAbility::UfpstrueReloadAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;
	bRetriggerInstancedAbility = false;
	bServerRespectsRemoteAbilityCancellation = false;
	SetAssetTags(FGameplayTagContainer(FpstrueGameplayTags::Ability_Reload));
	ActivationOwnedTags.AddTag(FpstrueGameplayTags::State_Reloading);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Reloading);
	CancelAbilitiesWithTag.AddTag(FpstrueGameplayTags::Ability_Fire);
	CancelAbilitiesWithTag.AddTag(FpstrueGameplayTags::Ability_Aim);
	CancelAbilitiesWithTag.AddTag(FpstrueGameplayTags::Ability_Sprint);
}

bool UfpstrueReloadAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)) return false;
	const AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	const UfpstrueWeaponComponent* Weapon = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	return IsValid(Character) && IsValid(Weapon) && Weapon->HasReloadAmmo();
}

// 服务器事务是唯一完成依据，Dedicated Server 不需要播放第一人称动画。
void UfpstrueReloadAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData*)
{
	if (!IsActive()) return;
	bEnding = false;
	ActiveReloadId = INDEX_NONE;
	const uint32 RequestedActivation = ++ActivationRevision;
	AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	UfpstrueWeaponComponent* Weapon = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	if (!IsValid(Weapon))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (ActivationRevision != RequestedActivation || !IsActive() || bEnding) return;
	if (!bCommitted || !IsValid(Character) || !Character->CanAcceptGameplayInput()
		|| !IsValid(Weapon) || Character->GetEquippedWeaponComponent() != Weapon
		|| (HasAuthority(&ActivationInfo) && !Weapon->HasReloadAmmo()))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	ActiveWeapon = Weapon;
	if (!HasAuthority(&ActivationInfo)) return; // 客户端只接收服务器动作标签，不伪造 ReloadId 或补弹。
	// 原生能力通知先于蓝图观察事件，让旧 GA 先释放标签，观察者随后才能合法重启换弹。
	ReloadEndedHandle = Weapon->OnAbilityReloadEnded.AddUObject(this, &ThisClass::HandleReloadEnded);
	const bool bAccepted = Weapon->BeginAbilityReload(ActiveReloadId);
	// BeginAbilityReload 广播播放请求，监听者可能同步完成、取消或重启换弹。
	if (ActivationRevision != RequestedActivation || !IsActive() || bEnding) return;
	if (!bAccepted || ActiveReloadId == INDEX_NONE) EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
}

// 先解绑，再取消匹配事务，避免 CancelReload 的同步广播重复进入 GA 结束链。
void UfpstrueReloadAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (bEnding || !IsEndAbilityValid(Handle, ActorInfo)) return;
	bEnding = true;
	UfpstrueWeaponComponent* Weapon = ActiveWeapon.Get();
	const int32 ReloadToCancel = ActiveReloadId;
	ActiveWeapon.Reset();
	ActiveReloadId = INDEX_NONE;
	if (Weapon != nullptr) Weapon->OnAbilityReloadEnded.Remove(ReloadEndedHandle);
	ReloadEndedHandle.Reset();
	// 先释放 GAS 状态，再通知组件取消旧事务；其蓝图观察者可立即发起合法的新动作。
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	if (bWasCancelled && IsValid(Weapon) && HasAuthority(&ActivationInfo)
		&& ReloadToCancel != INDEX_NONE && Weapon->GetActiveReloadId() == ReloadToCancel)
	{
		// 死亡和退出时禁用装备，普通取消才保留装备资格；不能让旧生命的取消重新开放操作。
		const AfpstrueCharacter* Character = Weapon->GetOwningCharacter();
		if (Character == nullptr || !Character->CanMaintainEquipment()) Weapon->DisableWeapon();
		else Weapon->CancelReloadForTransaction(ReloadToCancel);
	}
	if (IsValid(Weapon)) Weapon->PublishNetworkState();
}

// 本回调只结束能力；弹药是否提交已由组件结算，不能在 GA 再补交一次。
void UfpstrueReloadAbility::HandleReloadEnded(int32 ReloadId, EFPReloadEndReason Reason, bool)
{
	if (!IsActive() || bEnding) return;
	if (ReloadId == ActiveReloadId)
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, Reason != EFPReloadEndReason::Completed);
}

// 瞄准与冲刺的互斥只在能力标签上声明，角色不再复制同一组 if 判断。
UfpstrueAimAbility::UfpstrueAimAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	bRetriggerInstancedAbility = false;
	bReplicateInputDirectly = false;
	SetAssetTags(FGameplayTagContainer(FpstrueGameplayTags::Ability_Aim));
	ActivationOwnedTags.AddTag(FpstrueGameplayTags::State_Aiming);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Reloading);
	CancelAbilitiesWithTag.AddTag(FpstrueGameplayTags::Ability_Sprint);
}

bool UfpstrueAimAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)) return false;
	const AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	const UfpstrueWeaponComponent* Weapon = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	return IsValid(Character) && IsValid(Weapon) && Weapon->CanExecuteShot();
}

// 与开火一样，WaitInputRelease 覆盖正常松开、输入取消和释放早于服务器激活的情况。
void UfpstrueAimAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData*)
{
	if (!IsActive()) return;
	const uint32 RequestedActivation = ++ActivationRevision;
	AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	UfpstrueWeaponComponent* Weapon = Character != nullptr ? Character->GetEquippedWeaponComponent() : nullptr;
	if (!IsValid(Character) || !IsValid(Weapon))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (ActivationRevision != RequestedActivation || !IsActive()) return;
	// Commit 回调之后重查当前装备；PreActivate 已经处理过冲突能力取消。
	if (!bCommitted || !IsValid(Character)
		|| !IsValid(Weapon) || Character->GetEquippedWeaponComponent() != Weapon || !Weapon->CanExecuteShot())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	UAbilityTask_WaitInputRelease* ReleaseTask = UAbilityTask_WaitInputRelease::WaitInputRelease(this, true);
	ReleaseTask->OnRelease.AddDynamic(this, &ThisClass::HandleInputReleased);
	ReleaseTask->ReadyForActivation();
	if (ActivationRevision == RequestedActivation && IsActive()) Character->RefreshMovementAbilityState();
}

// 保存弱角色引用跨过 GAS 的结束广播，结束旧瞄准后只刷新当前标签，不覆盖回调中新建的动作。
void UfpstrueAimAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo)) return;
	TWeakObjectPtr<AfpstrueCharacter> Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	if (Character.IsValid()) Character->RefreshMovementAbilityState();
}

// 输入释放只结束瞄准能力；能力标签的移除统一驱动速度和表现恢复。
void UfpstrueAimAbility::HandleInputReleased(float)
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

// 冲刺保留原先的按键切换行为，但持续状态只由当前能力实例与标签持有。
UfpstrueSprintAbility::UfpstrueSprintAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	bRetriggerInstancedAbility = false;
	bReplicateInputDirectly = false;
	SetAssetTags(FGameplayTagContainer(FpstrueGameplayTags::Ability_Sprint));
	ActivationOwnedTags.AddTag(FpstrueGameplayTags::State_Sprinting);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Reloading);
	ActivationBlockedTags.AddTag(FpstrueGameplayTags::State_Aiming);
}

bool UfpstrueSprintAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags)) return false;
	const AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	return IsValid(Character) && Character->CanAcceptGameplayInput();
}

// 不增加 Tick 或独立计时；角色从当前标签计算并应用速度。
void UfpstrueSprintAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData*)
{
	if (!IsActive()) return;
	const uint32 RequestedActivation = ++ActivationRevision;
	AfpstrueCharacter* Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (!IsValid(Character))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (ActivationRevision != RequestedActivation || !IsActive()) return;
	if (!bCommitted || !IsValid(Character) || !Character->CanAcceptGameplayInput())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	// Refresh 是本次激活的最后一步；其蓝图回调可以合法取消/切换，不再由旧栈回写状态。
	Character->RefreshMovementAbilityState();
}

// 先移除 State.Sprinting，再读取剩余标签；不能用旧冲刺结束回调把瞄准速度恢复成步行。
void UfpstrueSprintAbility::EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo)) return;
	TWeakObjectPtr<AfpstrueCharacter> Character = ActorInfo != nullptr ? Cast<AfpstrueCharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	if (Character.IsValid()) Character->RefreshMovementAbilityState();
}
