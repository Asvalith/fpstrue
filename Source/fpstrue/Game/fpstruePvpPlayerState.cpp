// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstruePvpPlayerState.h"
#include "Net/UnrealNetwork.h"

// 玩家本局身份与计分：死亡换 Pawn 不丢成绩，服务器保留事实并复制给客户端。


// 复制成绩到所有客户端；玩家状态在一次Pawn重生期间不被重新创建。
void AfpstruePvpPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstruePvpPlayerState, Kills);
	DOREPLIFETIME(AfpstruePvpPlayerState, Deaths);
	DOREPLIFETIME(AfpstruePvpPlayerState, bDisconnected);
}

// 服务器提交离线保留标记并尽快复制；成绩继续保存在同一个 PlayerState。
void AfpstruePvpPlayerState::SetRetainedForReconnect(bool bRetained)
{
	if (!HasAuthority()) return;
	bDisconnected = bRetained;
	ForceNetUpdate();
}

// 离线保留时阻止引擎销毁 PlayerState；普通离场仍采用父类清理。
void AfpstruePvpPlayerState::OnDeactivated()
{
	if (bDisconnected) { SetOwner(nullptr); return; }
	Super::OnDeactivated();
}

// 服务器累加击杀并及时发布复制更新，客户端调用不会改变成绩。
void AfpstruePvpPlayerState::AddKill()
{
	if (!HasAuthority()) return;
	++Kills;
	SetScore(static_cast<float>(Kills));
	ForceNetUpdate();
}

// 服务器累加死亡数；去重由GameMode的待重生记录负责。
void AfpstruePvpPlayerState::AddDeath()
{
	if (!HasAuthority()) return;
	++Deaths;
	ForceNetUpdate();
}
