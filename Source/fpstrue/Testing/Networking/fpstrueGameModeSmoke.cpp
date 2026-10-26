// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstruePvpGameMode.h"

#if !UE_BUILD_SHIPPING

#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstruePvpPlayerState.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// GameMode 的显式实机验收场景。仍调用同一套生成、AI、伤害和连接流程；Shipping 不编译这些准备/观察。

// 真实单连接断包；既不调用失败委托，也不销毁客户端。旧连接销毁后计数不移植给新连接。
void AfpstruePvpGameMode::TickLiveReconnectOutage()
{
	if (!bSmokeTest || !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect"))) return;
	const double Now = FPlatformTime::Seconds();
	if (bLiveOutageStarted)
	{
		if (!bLiveOutageRestored && Now >= LiveOutageRestoreAt)
		{
			UNetConnection* Connection = LiveOutageConnection.Get();
			if (Connection != nullptr) Connection->PacketSimulationSettings.PktLoss = LiveOutagePreviousLoss;
			bLiveOutageRestored = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE LIVE_OUTAGE Restored ResumeId=%d Connection=%u Alive=%d"),
				LiveOutageResumeId, Connection != nullptr ? Connection->GetUniqueID() : 0, Connection != nullptr);
		}
		return;
	}
	if (!bSmokeFinalReported || !bSmokePveCompleted) return;
	AfpstruePvpPlayerController* Candidate = nullptr;
	int32 HighestId = INDEX_NONE;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		if (auto* Player = Cast<AfpstruePvpPlayerController>(It->Get()); Player != nullptr && Player->bConnectionHandshakeComplete)
			if (const auto* State = Player->GetPlayerState<AfpstruePvpPlayerState>(); State != nullptr && State->GetPlayerId() > HighestId)
			{ HighestId = State->GetPlayerId(); Candidate = Player; }
	UNetConnection* Connection = Candidate != nullptr ? Candidate->GetNetConnection() : nullptr;
	if (Connection == nullptr) return;
	LiveOutageConnection = Connection;
	LiveOutagePreviousLoss = Connection->PacketSimulationSettings.PktLoss;
	LiveOutageResumeId = HighestId;
	LiveOutageRestoreAt = Now + 8.0;
	bLiveOutageStarted = true;
	Connection->PacketSimulationSettings.PktLoss = 100;
	UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE LIVE_OUTAGE Started ResumeId=%d Connection=%u Duration=8 Direction=ServerOutbound"),
		HighestId, Connection->GetUniqueID());
}

// 原 PvP 门禁通过后，显式烟测才开放敌人阶段；普通玩法与 Shipping 不采用这条控制链。
void AfpstruePvpGameMode::StartPveSmoke(AfpstruePvpPlayerController* Requester)
{
	if (!bSmokeTest || !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokePve")) || bSmokePveStarted ||
		!IsValid(Requester) || !IsCombatParticipant(Cast<AfpstrueCharacter>(Requester->GetPawn()))) return;
	bSmokePveStarted = true;
	SmokePvePlayer = Cast<AfpstrueCharacter>(Requester->GetPawn());
	NextEnemySpawnTime = GetWorld()->GetTimeSeconds();
}

// 只给显式普通入口验收下发一次可行路线；初始出生点和实际武器/伤害/移动协议均不改。
void AfpstruePvpGameMode::SendNormalVisualRoutes()
{
	TArray<AfpstrueCharacter*> Players;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
		if (IsCombatParticipant(*It) && It->HasEquippedWeapon() && It->GetPlayerState()) Players.Add(*It);
	if (Players.Num() != 2) return;
	Players.Sort([](const AfpstrueCharacter& A, const AfpstrueCharacter& B) { return A.GetPlayerState()->GetPlayerId() < B.GetPlayerState()->GetPlayerId(); });
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!Navigation) return;
	FNavLocation Destination;
	if (!Navigation->ProjectPointToNavigation(Players[0]->GetActorLocation() + Players[0]->GetActorRightVector() * 350.0, Destination, FVector(200, 200, 200))) return;
	UNavigationPath* Path = Navigation->FindPathToLocationSynchronously(GetWorld(), Players[1]->GetActorLocation(), Destination.Location, Players[1]);
	if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.IsEmpty() || Path->PathPoints.Num() > 128)
	{
		bNormalVisualRoutesSent = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE FAIL Reason=NoCompleteBoundedPath"));
		return;
	}
	auto* First = Cast<AfpstruePvpPlayerController>(Players[0]->GetController());
	auto* Second = Cast<AfpstruePvpPlayerController>(Players[1]->GetController());
	if (!First || !Second) return;
	bNormalVisualRoutesSent = true;
	First->ClientNormalVisualRoute({Players[0]->GetActorLocation()});
	Second->ClientNormalVisualRoute(Path->PathPoints);
	UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE SERVER Points=%d StartDistanceCm=%.1f EnemyPopulation=%d Teleport=0"),
		Path->PathPoints.Num(), FVector::Dist2D(Players[0]->GetActorLocation(), Players[1]->GetActorLocation()), EnemyPopulation);
}

// 观察服务器真实位移是否越过验收阈值，不修改角色位置或移动参数。
void AfpstruePvpGameMode::SampleSmokeMovement()
{
	for (auto It = SmokeSpawnOrigins.CreateIterator(); It; ++It)
	{
		const AfpstrueCharacter* Pawn = It.Key().Get();
		if (Pawn == nullptr) { It.RemoveCurrent(); continue; }
		if (Pawn->IsDead() || SmokeMovedPlayers.Contains(It.Key())) continue;
		const float Distance = FVector::Dist2D(It.Value(), Pawn->GetActorLocation());
		if (Distance >= 5.0f)
		{
			SmokeMovedPlayers.Add(It.Key());
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_DISPLACEMENT PASS Pawn=%s Distance=%.2f"), *Pawn->GetName(), Distance);
		}
	}
}

// 仅开发烟测观察真实伤害结果，推进敌人反击与离线受伤门禁；Shipping 不执行验收逻辑。
void AfpstruePvpGameMode::HandlePvePlayerDamaged(TWeakObjectPtr<AfpstrueCharacter> Player, float Damage,
	AActor* DamageCauser, AController*, const FFPDamageContext&)
{
	if (!bSmokeTest || Damage <= 0.0f || !Player.IsValid() || !IsValid(Cast<AfpstrueEnemyCharacter>(DamageCauser))) return;
	if (bSmokePveStarted && !bSmokePveEnemyHit)
	{
		bSmokePveEnemyHit = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PveEnemyHit Player=%s Health=%.1f"), *Player->GetName(), Player->GetCurrentHealth());
		if (SmokePvePlayer.IsValid())
			if (auto* Shooter = Cast<AfpstruePvpPlayerController>(SmokePvePlayer->GetController())) Shooter->ClientAllowPveSmokeCounterattack();
	}
	if (!bSmokeOfflineDamage && Player == SmokeOfflineTarget && IsRetainedDisconnectedPawn(Player.Get()))
	{
		for (const auto& Entry : RejoinSlots)
			if (Entry.Value.Pawn == Player && Entry.Value.State.IsValid())
			{
				bSmokeOfflineDamage = true;
				const float After = Player->GetCurrentHealth();
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=ResumeOfflineDamage ResumeId=%d Before=%.3f After=%.3f"), Entry.Value.State->GetPlayerId(), After + Damage, After);
				if (auto* EnemyAI = Cast<AfpstrueEnemyAIController>(CastChecked<AfpstrueEnemyCharacter>(DamageCauser)->GetController())) EnemyAI->StopAI();
				break;
			}
	}
}

// 测试才隔离在线玩家并选择离线目标；false 与原人口更新的提前退出完全对应。
bool AfpstruePvpGameMode::PreparePveSmokePopulation(AfpstrueCharacter*& NearPlayer)
{
	if (!bSmokeTest) return true;
	if (!bSmokePveStarted) return false; // PvP 验收完成之前不生成干扰玩家计分和自动输入的敌人。
	if (!bSmokePveCompleted)
	{
		NearPlayer = SmokePvePlayer.Get();
		return true;
	}
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeReconnect")) || bSmokeOfflineDamage || bSmokeOfflineSceneFailed) return false;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
		if (IsCombatParticipant(*It) && IsRetainedDisconnectedPawn(*It)) { NearPlayer = *It; break; }
	if (NearPlayer == nullptr) return false;
	SmokeOfflineTarget = NearPlayer;
	if (!bSmokeOfflineScenePrepared)
	{
		// 只隔离测试场景中的其他玩家，仍由真实视觉、行为树和刀刃检测攻击保留 Pawn，不指定或伪造 AI 目标。
		UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
		if (Navigation == nullptr)
		{
			bSmokeOfflineSceneFailed = true;
			UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL ResumeOfflineIsolation Reason=NoNavigation"));
			return false;
		}
		const FVector RetainedLocation = NearPlayer->GetActorLocation();
		TArray<FVector, TInlineAllocator<56>> IsolationCandidates;
		for (float Radius : {7000.0f, 10000.0f, 14000.0f})
			for (int32 Direction = 0; Direction < 8; ++Direction)
			{
				const float Angle = FMath::DegreesToRadians(static_cast<float>(Direction * 45));
				IsolationCandidates.Add(RetainedLocation + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * Radius);
			}
		for (int32 Index = 0; Index < FMath::Min(PveSpawnPoints.Num(), 32); ++Index)
			if (IsValid(PveSpawnPoints[Index])) IsolationCandidates.Add(PveSpawnPoints[Index]->GetActorLocation());
		for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
		{
			AfpstrueCharacter* OnlinePlayer = *It;
			if (OnlinePlayer == NearPlayer || !IsCombatParticipant(OnlinePlayer) ||
				Cast<APlayerController>(OnlinePlayer->GetController()) == nullptr) continue;
			bool bIsolated = false;
			for (const FVector& Candidate : IsolationCandidates)
			{
				FNavLocation Projected;
				if (!Navigation->ProjectPointToNavigation(Candidate, Projected, FVector(600.0f, 600.0f, 1000.0f)) ||
					FVector::DistSquared2D(Projected.Location, RetainedLocation) <= FMath::Square(6500.0f)) continue;
				const FVector Destination = Projected.Location + FVector::UpVector *
					(OnlinePlayer->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0f);
				if (!OnlinePlayer->TeleportTo(Destination, OnlinePlayer->GetActorRotation(), false, false) ||
					FVector::DistSquared2D(OnlinePlayer->GetActorLocation(), RetainedLocation) <= FMath::Square(6000.0f)) continue;
				OnlinePlayer->GetCharacterMovement()->StopMovementImmediately();
				OnlinePlayer->ForceNetUpdate();
				bIsolated = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE RECONNECT_ISOLATE Player=%s Distance=%.1f Location=%s"),
					*OnlinePlayer->GetName(), FVector::Dist2D(OnlinePlayer->GetActorLocation(), RetainedLocation),
					*OnlinePlayer->GetActorLocation().ToCompactString());
				break;
			}
			if (!bIsolated)
			{
				bSmokeOfflineSceneFailed = true;
				UE_LOG(LogTemp, Error, TEXT("PVP_SMOKE FAIL ResumeOfflineIsolation Reason=NoNavigableCollisionFreeLocation Player=%s"), *OnlinePlayer->GetName());
				return false;
			}
		}
		bSmokeOfflineScenePrepared = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE RECONNECT_SCENE_PREPARED RetainedPawn=%s"), *NearPlayer->GetName());
	}
	return true;
}

#endif
