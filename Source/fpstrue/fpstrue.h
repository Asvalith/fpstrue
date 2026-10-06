// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

// fpstrue 主模块公共入口；具体玩法职责分别位于 Character、GameMode、AI、Combat 和性能协调组件中。

// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * 项目级碰撞通道约定。
 *
 * 枚举槽位必须与 Config/DefaultEngine.ini 中的 DefaultChannelResponses 保持一致。
 * 业务代码使用语义名称而不是散落 ECC_GameTraceChannel3，后续调整槽位时只需修改这里。
 */
namespace FpstrueCollisionChannels
{
	// 玩家 Hitscan 专用查询通道：场景和敌人骨骼阻挡，角色胶囊、Trigger、UI 和尸体忽略。
	inline constexpr ECollisionChannel WeaponTrace = ECC_GameTraceChannel3;
}

// Copyright Epic Games, Inc. All Rights Reserved.

class UAnimInstance;
class UAnimMontage;
class USkeletalMeshComponent;
struct FAnimNotifyEventReference;
struct FAnimMontageInstance;

/** 一次明确播放的引用，不管理动作状态，也不从全局 Montage 列表推断业务归属。 */
struct FPSTRUE_API FFPActionPlayback
{
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UAnimInstance> AnimInstance;
	TWeakObjectPtr<UAnimMontage> Montage;
	int32 MontageInstanceId = INDEX_NONE;
	uint32 ActionId = 0;

	// 只能绑定调用者明确指定的 Montage；同一事务已有不同播放时拒绝覆盖。
	FAnimMontageInstance* TryBind(uint32 InActionId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* PlayedMontage);
	bool Matches(uint32 ExpectedActionId, USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const;
	// 按保存的 AnimInstance、实例 ID 和资产解析播放；返回值仅供当前调用使用，不能跨回调缓存。
	FAnimMontageInstance* GetBoundInstance() const;
	// 停止保存的实例，不用 Montage_Stop(Asset) 误停同资源的新一轮播放；可能同步触发外部回调。
	void Stop(float BlendOutSeconds = 0.1f) const;
	void Reset() { *this = FFPActionPlayback(); }
	bool IsSet() const;
};
