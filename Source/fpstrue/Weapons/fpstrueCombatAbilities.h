// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "fpstrueCombatAbilities.generated.h"

class UAbilityTask_WaitDelay;

// 使用 ASC 同一 Actor 通道传送逐发输入；不发送客户端命中结论，也不复用 LocationInfo 偷藏字段。
USTRUCT()
struct FGameplayAbilityTargetData_FPShot : public FGameplayAbilityTargetData
{
	GENERATED_BODY()
	UPROPERTY() FFPShotRequest Request;
	virtual UScriptStruct* GetScriptStruct() const override { return StaticStruct(); }
	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

template<> struct TStructOpsTypeTraits<FGameplayAbilityTargetData_FPShot> : TStructOpsTypeTraitsBase2<FGameplayAbilityTargetData_FPShot>
{
	enum { WithNetSerializer = true, WithCopy = true };
};

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

protected:
	// 先建立松开任务，再开始射击；连射由能力自己的 WaitDelay 任务推进，同步失败可以立即结束能力。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 先解绑外部事件，再停止捕获的武器；死亡、卸枪、松开都进入同一清理出口。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// WaitInputRelease 自带客户端到服务器的释放事件，不另造一套释放 RPC。
	UFUNCTION()
	void HandleInputReleased(float TimeHeld);
	// 每次只结算/表现一发；空仓或失去执行资格时结束能力，等待下一发的任务与能力同生共灭。
	UFUNCTION()
	void Pulse();
	// 逐发 TargetData 消费后立即清缓存；最多等待一发冷却，不让可靠消息积压变成补射。
	void HandleShotData(const FGameplayAbilityTargetDataHandle& Data, FGameplayTag ApplicationTag);
	UFUNCTION() void ExecutePendingShot();
	// 没有逐发输入的服务器能力不能永久持有 Firing；持续输入会重置空闲保护。
	UFUNCTION()
	void HandleAimTimeout();

	TWeakObjectPtr<UfpstrueWeaponComponent> ActiveWeapon;
	FDelegateHandle ShotDataHandle;
	UPROPERTY()
	TObjectPtr<UAbilityTask_WaitDelay> AimTimeoutTask;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> PendingShotTask;
	FFPShotRequest PendingShot;
	bool bHasPendingShot = false;
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
	// 换弹取消开火、瞄准和冲刺；ServerInitiated 让手动请求与服务器空仓共用同一能力，补弹与完成时刻由服务器裁决。
	UfpstrueReloadAbility();
	// 弹匣已满、无备弹、角色失效或已经换弹时，激活前直接拒绝。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	// 服务器订阅事务结束后才请求换弹；客户端等待服务器复制能力结束。
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
	UFUNCTION()
	void HandleInputReleased(float TimeHeld);
	// Commit/表现广播可以取消并重启同一实例，旧激活栈不能给新一轮注册释放任务。
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
	// 身份只保护异步/回调归属，不另外维护冲刺动作状态。
	uint32 ActivationRevision = 0;
};
