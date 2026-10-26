// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "fpstrueCharacterMovementComponent.generated.h"

// 每次移动携带当时的模式，不把当前 GAS 标签错误套用到历史移动；这里只保存计算上下文，不执行能力。
// 【设计取舍】只占用 CMC 两个自定义 bit，预测、ACK 和纠正仍用原生协议，不另发一套移动 RPC。
// 模式切换时拒绝合并会多保留少量 Move，换来历史重放的速度上下文正确；模式位本身不是服务器授权。
class FPSTRUE_API FSavedMove_fpstrueCharacter : public FSavedMove_Character
{
public:
	using Super = FSavedMove_Character;
	bool bSavedAiming = false;
	bool bSavedSprinting = false;
	// 回收原生 SavedMove 时清空附加模式位，避免复用上一次动作。
	virtual void Clear() override;
	// 在原生移动压缩标志中携带 Aim/Sprint 两个业务输入位。
	virtual uint8 GetCompressedFlags() const override;
	// 不合并模式不同的移动，保留加减速切换发生的时间边界。
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* Character, float MaxDelta) const override;
	// 保存本次移动实际采用的模式，不能在日后回放时改用当前标签。
	virtual void SetMoveFor(ACharacter* Character, float InDeltaTime, const FVector& Accel, FNetworkPredictionData_Client_Character& ClientData) override;
	// 回放前恢复历史模式，只写移动计算上下文，不激活能力或通知蓝图。
	virtual void PrepMoveFor(ACharacter* Character) override;
};

// 移动扩展独立于角色和动画表现：GAS 授权动作，SavedMove 负责模式的保存、传输与校正重放。
UCLASS()
class FPSTRUE_API UfpstrueCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()
public:
	// 以本次移动的 Aim/Sprint 上下文选择速度，其他移动模式复用 CMC 原生规则。
	virtual float GetMaxSpeed() const override;
	// 延迟创建使用项目 SavedMove 的原生客户端预测缓存，不替换 UE 的 ACK/纠正协议。
	virtual FNetworkPredictionData_Client* GetPredictionData_Client() const override;
	// 解包模式位；服务器只接受当前 GAS 授权或有界历史授权，不信任客户端速度。
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;
	// UE 完成历史 Move 重放后恢复当前模式，防止旧 Move 覆盖已取消的冲刺/瞄准。
	virtual bool ClientUpdatePositionAfterServerUpdate() override;
	// 诊断读取真正收到位置纠正的累计次数；不是逐帧调用次数，也不改变 CMC 协议。
	uint64 GetNetworkCorrectionCount() const { return NetworkCorrectionCount; }
	// 复用 CMC 的线性平滑和原生变换时间戳，显示与服务器历史使用同一条时间线。
	void ConfigureServerTimelineSmoothing();
	void SetServerSnapshotTime(double ServerTime);
	// 只对已接收权威快照的模拟副本有效；不把本地预测或客户端累计移动时间当服务器时间。
	bool GetRenderedServerTime(double& OutTime) const;
	// 远端采用原生快照插值；禁止另一次速度前推把显示位置带离所报告的快照时间。
	virtual void MoveSmooth(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult = nullptr) override;
	// 角色提交当前 GAS 派生意图；权威端保留刚结束模式的短暂合法时间窗。
	void SetMovementIntent(bool bAiming, bool bSprinting);
	// SavedMove 读取当前移动计算使用的瞄准模式，而非直接查询 GAS。
	bool IsAimMovementRequested() const { return bMoveAiming; }
	// SavedMove 读取当前移动计算使用的冲刺模式，瞄准优先级由写入口统一约束。
	bool IsSprintMovementRequested() const { return bMoveSprinting; }
private:
	bool UsesServerSnapshotMovement() const;
	friend class FSavedMove_fpstrueCharacter;
	// 回放只恢复这两个值，不调用 SetMovementIntent，因此不改当前输入、GAS 或蓝图。
	bool bMoveAiming = false;
	bool bMoveSprinting = false;
	struct FRecentMovementGrant
	{
		uint8 Mode = 0;
		float LastAllowedClientTime = 0.0f;
		double ExpiresAt = 0.0;
	};
	FRecentMovementGrant RecentGrants[4];
	uint8 NextGrant = 0;
	uint8 GrantedMode = 0;
	uint64 NetworkCorrectionCount = 0;
};
