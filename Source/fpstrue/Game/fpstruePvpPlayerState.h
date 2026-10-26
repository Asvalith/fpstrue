// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "fpstruePvpPlayerState.generated.h"

// 玩家本局身份与计分：死亡换 Pawn 不丢成绩，服务器保留事实并复制给客户端。

// 计分跟随玩家控制器而不是Pawn，死亡换角色后继续保留本局击杀和死亡数。
UCLASS()
class FPSTRUE_API AfpstruePvpPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	// 注册服务器向所有客户端复制的个人计分字段。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// 界面读取本局击杀数，不允许客户端直接提交成绩。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	int32 GetKills() const { return Kills; }
	// 界面读取本局死亡数。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Score")
	int32 GetDeaths() const { return Deaths; }
	// 仅服务器结算一次有效击杀，同时更新引擎通用Score。
	void AddKill();
	// 仅服务器结算一次死亡。
	void AddDeath();
	// 暂时离线保留同一个服务器 PlayerState，不复制客户端成绩来恢复身份。
	virtual void OnDeactivated() override;
	void SetRetainedForReconnect(bool bRetained);
	// 查询服务器复制的离线保留事实；本地不据此延长恢复期限。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") bool IsDisconnected() const { return bDisconnected; }

private:
	UPROPERTY(Replicated)
	int32 Kills = 0;
	UPROPERTY(Replicated)
	int32 Deaths = 0;
	UPROPERTY(Replicated) bool bDisconnected = false;
};
