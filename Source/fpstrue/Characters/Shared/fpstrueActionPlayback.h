// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UAnimInstance;
class UAnimMontage;
class USkeletalMeshComponent;
struct FAnimNotifyEventReference;

/** 一次明确播放的引用，不管理动作状态，也不从全局 Montage 列表推断业务归属。 */
struct FPSTRUE_API FFPActionPlayback
{
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UAnimInstance> AnimInstance;
	TWeakObjectPtr<UAnimMontage> Montage;
	int32 MontageInstanceId = INDEX_NONE;
	uint32 ActionId = 0;

	// 只能绑定调用者明确指定的 Montage；同一事务已有不同播放时拒绝覆盖。
	bool TryBind(uint32 InActionId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* PlayedMontage);
	bool Matches(uint32 ExpectedActionId, USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const;
	// 停止保存的实例，不用 Montage_Stop(Asset) 误停同资源的新一轮播放；可能同步触发外部回调。
	void Stop(float BlendOutSeconds = 0.1f) const;
	void Reset() { *this = FFPActionPlayback(); }
	bool IsSet() const;
};
