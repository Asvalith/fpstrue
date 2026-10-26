// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "fpstrueCombatAbilities.generated.h"

class UAbilityTask_WaitDelay;

// Enhanced Input 只传按下/松开；InputID 与服务器授予的 AbilitySpec 对应。
enum class EFPCombatAbilityInput : uint8
{
	Fire = 1,
	Reload = 2,
	Aim = 3,
	Sprint = 4
};

/** 开火能力负责激活、互斥、连射调度与松开同步；逐发结算和表现仍由武器组件执行。 */
UCLASS()
class FPSTRUE_API UfpstrueFireAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	// 一个角色复用一个能力实例；拥有者先预测，服务器独立校验。
	UfpstrueFireAbility();
	// 保留 GAS 标签/费用校验，再检查当前角色和武器的业务资格。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// ASC 核对原生激活身份后只把批次交给这一轮能力；去重/重排和结算仍在原武器执行器。
	void HandleShotInputs(uint32 FirstShotId, const TArray<FFPShotRequest>& Requests);
	void FinishRemoteShotInputs(bool bCancelled);

protected:
	// 先建立松开任务，再开始射击；连射由能力自己的 WaitDelay 任务推进，同步失败可以立即结束能力。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 先解绑外部事件，再停止捕获的武器；死亡、卸枪、松开都进入同一清理出口。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	friend class UfpstrueHealthComponent;
	friend class FFpstrueWeaponShotTransportTest;
	// WaitInputRelease 自带客户端到服务器的释放事件，不另造一套释放 RPC。
	UFUNCTION()
	void HandleInputReleased(float TimeHeld);
	// 每次只结算/表现一发；空仓或失去执行资格时结束能力，等待下一发的任务与能力同生共灭。
	UFUNCTION()
	void Pulse();
	// 远端拥有者停止发送有效逐发输入时，服务器释放 Firing。
	UFUNCTION()
	void HandleShotInputTimeout();

	TWeakObjectPtr<UfpstrueWeaponComponent> ActiveWeapon;
	UPROPERTY()
	TObjectPtr<UAbilityTask_WaitDelay> ShotInputTimeoutTask;
	// 外部通知可同步结束并重新激活同一个能力实例，旧 Pulse 不能替新动作安排任务。
	uint32 ActivationRevision = 0;
	bool bEnding = false;
};

/** 换弹能力持有动作标签并等待既有换弹事务，不复制计时器或客户端装弹规则。 */
UCLASS()
class FPSTRUE_API UfpstrueReloadAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	// 手动换弹本地预测动作；服务器主动空仓仍进入同一能力，只有权威事务能装弹。
	UfpstrueReloadAbility();
	// 弹匣已满、无备弹、角色失效或已经换弹时，激活前直接拒绝。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	// 服务器订阅真实事务结束；客户端预测一次播放，并由同 key 快照接管播放编号。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 取消只作用于保存的 ReloadId；解绑先于取消，防止结束事件同步重入。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// 普通完成、中断、超时和死亡共用武器的真实事务结果。
	void HandleReloadEnded(int32 ReloadId, EFPReloadEndReason Reason, bool bAmmoCommitted);

	TWeakObjectPtr<UfpstrueWeaponComponent> ActiveWeapon;
	FDelegateHandle ReloadEndedHandle;
	int32 ActiveReloadId = INDEX_NONE;
	// 请求可经蓝图同步结束；编号在回调前写入，版本防止旧激活栈回写监听者开启的新一轮。
	uint32 ActivationRevision = 0;
	bool bEnding = false;
};

/** 瞄准能力只管理按住期间的动作资格，角色根据标签统一更新速度和蓝图表现。 */
UCLASS()
class FPSTRUE_API UfpstrueAimAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	// 瞄准本地预测并取消冲刺，换弹或死亡时不接受新的瞄准。
	UfpstrueAimAbility();
	// 在 GAS 通用资格之外，要求当前角色装备仍可执行武器动作。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	// 松开任务同步输入释放；标签是瞄准状态的唯一来源。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 先让 GAS 移除标签，再让角色读取当前状态，不手工重置另一份瞄准布尔值。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// WaitInputRelease 的本地/服务器释放回调；只结束当前瞄准，状态由 GAS 标签移除。
	UFUNCTION()
	void HandleInputReleased(float TimeHeld);
	// Commit 的同步广播可取消并重启同一实例；IsActive 无法区分新旧激活。
	uint32 ActivationRevision = 0;
};

/** 冲刺能力持续到再次按键或被瞄准、换弹、死亡取消，不再由角色另写一套互斥状态。 */
UCLASS()
class FPSTRUE_API UfpstrueSprintAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	// 角色仍用原按键切换语义，第二次按下取消当前能力。
	UfpstrueSprintAbility();
	// 复用 GAS 标签互斥，并拒绝死亡、退出或控制权迁移中的角色输入。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	// 激活后由角色统一应用当前标签对应的速度。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 能力退出后重新读取全部移动标签，避免结束旧冲刺时覆盖新瞄准。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// 与 Aim 相同，只跨 Commit 回调检查激活身份，不另存冲刺状态。
	uint32 ActivationRevision = 0;
};
