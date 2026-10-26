// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstruePvpGameState.h"
#include "Game/fpstruePvpPlayerState.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Net/UnrealNetwork.h"
#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"
#include "Testing/Networking/fpstrueNetworkDiagnostics.h"

// 发布对局公共事实；性能协调器在各端独立运行，不复制本地相机评分或渲染档位。


// GameState 在服务端与客户端都有实例，组件只运行本端策略，不复制本地相机或预算结果。
AfpstruePvpGameState::AfpstruePvpGameState()
{
	EnemySignificanceCoordinator = CreateDefaultSubobject<UfpstrueEnemySignificanceCoordinator>(TEXT("EnemySignificanceCoordinator"));
	EnemyAnimationSharingCoordinator = CreateDefaultSubobject<UfpstrueEnemyAnimationSharingCoordinator>(TEXT("EnemyAnimationSharingCoordinator"));
}

// 客户端不等待 GameMode；晚加入和重新连接随新 GameState 自动建立本端协调器。
void AfpstruePvpGameState::BeginPlay()
{
	Super::BeginPlay();
	EnemySignificanceCoordinator->StartMultiplayer(EnemyAnimationSharingCoordinator);
	StartPvpBaseline(GetWorld());
	StartPvpNetworkDiagnostics(GetWorld());
}

// 地图结束时先停止新采样，再注销共享 Follower；组件 EndPlay 仍可幂等兜底。
void AfpstruePvpGameState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EnemySignificanceCoordinator->Stop();
	EnemyAnimationSharingCoordinator->Stop();
	Super::EndPlay(EndPlayReason);
}

void AfpstruePvpGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AfpstruePvpGameState, bMatchActive);
	DOREPLIFETIME(AfpstruePvpGameState, EliminationCount);
}

// 只整理当前玩家状态视图，不把成绩从Pawn或客户端输入重新计算一遍。
TArray<AfpstruePvpPlayerState*> AfpstruePvpGameState::GetScoreboard() const
{
	TArray<AfpstruePvpPlayerState*> Players;
	for (APlayerState* State : PlayerArray)
	{
		if (AfpstruePvpPlayerState* Player = Cast<AfpstruePvpPlayerState>(State)) Players.Add(Player);
	}
	Players.Sort([](const AfpstruePvpPlayerState& A, const AfpstruePvpPlayerState& B)
	{
		if (A.GetKills() != B.GetKills()) return A.GetKills() > B.GetKills();
		if (A.GetDeaths() != B.GetDeaths()) return A.GetDeaths() < B.GetDeaths();
		return A.GetPlayerId() < B.GetPlayerId();
	});
	return Players;
}

// 比赛开放资格只由服务器写入，客户端读取复制快照。
void AfpstruePvpGameState::SetMatchActive(bool bActive)
{
	if (!HasAuthority()) return;
	bMatchActive = bActive;
	ForceNetUpdate();
}

// 一次已去重的角色死亡只增加一次全局计数。
void AfpstruePvpGameState::AddElimination()
{
	if (!HasAuthority()) return;
	++EliminationCount;
	ForceNetUpdate();
}
