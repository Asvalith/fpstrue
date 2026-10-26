// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "fpstruePvpGameState.generated.h"

class AfpstruePvpPlayerState;
class UfpstrueEnemySignificanceCoordinator;
class UfpstrueEnemyAnimationSharingCoordinator;

// 发布对局公共事实；性能协调器在各端独立运行，不复制本地相机评分或渲染档位。

// 【设计取舍】GameMode 在服务器接纳、计分和重生，GameState/PlayerState 发布公共事实，客户端 UI 不读 GameMode。
// 协调器即使挂在复制 Actor 上，也各端独立计算预算；不因此复制相机评分或本地渲染档位。
UCLASS()
class FPSTRUE_API AfpstruePvpGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AfpstruePvpGameState();
	// 返回本端性能分级协调器；实例存在于两端，评分和预算不经网络复制。
	UfpstrueEnemySignificanceCoordinator* GetEnemySignificanceCoordinator() const { return EnemySignificanceCoordinator; }
	// 返回本端动画共享协调器，不拥有 AI 战斗或命中权限。
	UfpstrueEnemyAnimationSharingCoordinator* GetEnemyAnimationSharingCoordinator() const { return EnemyAnimationSharingCoordinator; }
	// 注册比赛活动状态和已结算死亡总数的复制。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 返回按击杀、死亡和玩家编号稳定排序的计分表。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	TArray<AfpstruePvpPlayerState*> GetScoreboard() const;
	// 客户端读取对局是否接受战斗和重生。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Match")
	bool IsMatchActive() const { return bMatchActive; }
	// 读取服务器已经接纳的死亡事务数量。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	int32 GetEliminationCount() const { return EliminationCount; }
	// 由服务器GameMode开放或关闭当前对局。
	void SetMatchActive(bool bActive);
	// 由服务器登记一次已去重的死亡事务。
	void AddElimination();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 每端各自运行：服务器计算 Gameplay 距离，客户端分配自己的表现预算；不复制相机评分。
	UPROPERTY(VisibleAnywhere, Category = "Performance") TObjectPtr<UfpstrueEnemySignificanceCoordinator> EnemySignificanceCoordinator;
	UPROPERTY(VisibleAnywhere, Category = "Performance") TObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> EnemyAnimationSharingCoordinator;
	UPROPERTY(Replicated)
	bool bMatchActive = false;
	UPROPERTY(Replicated)
	int32 EliminationCount = 0;
};
