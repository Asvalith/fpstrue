// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "fpstrueEnemySignificanceCoordinator.generated.h"

class AfpstrueGameMode;

/** 敌人显著性调度模块：集中采样所有敌人，排序后分配 LOD、动画、阴影和骨骼 RT 预算。 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueEnemySignificanceCoordinator : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建由 Timer 驱动、无组件 Tick 的协调器。
	UfpstrueEnemySignificanceCoordinator();

	// ==================== 生命周期 ====================

	// GameMode 保留可编辑配置作为兼容层，Coordinator 独占评分、预算分配和应用流程。
	void Start(AfpstrueGameMode* InGameMode);
	// 停止集中更新 Timer；GameMode 结束和组件销毁都会调用。
	void Stop();

protected:
	// 组件退出时清理 Timer。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// ==================== 集中更新与策略校验 ====================

	// 一次完成 Gameplay 更新、Render 采样、预算排序、组件应用和 CSV 统计。
	void Update();
	// 修正权重、阈值、距离和名额的非法配置。
	void SanitizePolicy();

	TWeakObjectPtr<AfpstrueGameMode> GameMode;
	bool bPolicyInitialized = false;
	FTimerHandle UpdateTimerHandle;
};
