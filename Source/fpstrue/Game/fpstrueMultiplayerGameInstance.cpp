// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstrueMultiplayerGameInstance.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstruePvpPlayerState.h"
#include "Game/fpstrueConnectionHelpers.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "GameMapsSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"

// 跨世界连接流程：发起旅行、等待就绪、有限重试和本地票据缓存；不裁决伤害或恢复角色。

using FpstrueConnection::GetResumeNetGuid;
using FpstrueConnection::IsResumeToken;

// ---- 引擎生命周期与本地入口 ----

// 只让显式回环基线接管启动 Browse；普通入口仍走引擎启动流程，诊断失败不静默回退。
void UfpstrueMultiplayerGameInstance::StartGameInstance()
{
	const EFPPvpBaselineBootstrap Bootstrap = StartPvpBaselineClient(this);
	if (Bootstrap == EFPPvpBaselineBootstrap::NotRequested)
	{
		Super::StartGameInstance();
		return;
	}
	// 仅已成功发起的 Browse 广播启动完成；拒绝的诊断不发布成功事件。
	if (Bootstrap == EFPPvpBaselineBootstrap::Started) BroadcastOnStart();
}

// 连接监督器属于 GameInstance：Ticker 跨 World 存活，旧操作只有编号匹配才可以推进下一次旅行。
void UfpstrueMultiplayerGameInstance::Init()
{
	Super::Init();
	if (IsRunningDedicatedServer()) return;
	FString PrivateUserDir;
	const bool bExplicitUserDir = FParse::Value(FCommandLine::Get(), TEXT("UserDir="), PrivateUserDir);
	const FString SavedRoot = bExplicitUserDir || FPaths::ShouldSaveToUserDir() ? FPaths::ProjectSavedDir()
		: FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("fpstrue"), TEXT("Saved"));
	ResumeCacheFile = FPaths::Combine(SavedRoot, TEXT("Private"), TEXT("PvpResume.dat"));
	LoadResumeCache();
	if (GEngine != nullptr)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &ThisClass::HandleTravelFailure);
	}
	PostLoadHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);
	ConnectionTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ThisClass::TickConnection), 0.1f);
}

// 终止当前连接操作并解除跨世界委托/Ticker；不在退出期间启动重连。
void UfpstrueMultiplayerGameInstance::Shutdown()
{
	++OperationRevision;
	bPendingLocalReturn = false;
	FTSTicker::GetCoreTicker().RemoveTicker(ConnectionTicker);
	if (GEngine != nullptr)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadHandle);
	Super::Shutdown();
}

// 只接纳包路径作为地图，URL中的PvP模式和监听选项由本入口固定生成。
void UfpstrueMultiplayerGameInstance::HostPvp()
{
	if (ConnectionState != EFPConnectionState::Idle && ConnectionState != EFPConnectionState::Failed) return;
	const FString Map = ResolvePvpMapPackage();
	if (GetWorld() == nullptr || !FPackageName::IsValidLongPackageName(Map))
	{
		UE_LOG(LogTemp, Warning, TEXT("HostPvp requires a valid game map package, got %s."), *Map);
		return;
	}
	// 主机是新会话，不继承上一次直连的重试、凭证或接纳结果。
	const uint32 Revision = ++OperationRevision;
	bAutoReconnect = false;
	bPendingLocalReturn = false;
	ConnectingController.Reset();
	AttemptNetDriver.Reset();
	ClearResumeCache();
	LastConnectionError.Reset();
	SetConnectionState(EFPConnectionState::Idle);
	if (Revision == OperationRevision && ConnectionState == EFPConnectionState::Idle)
		UGameplayStatics::OpenLevel(this, FName(*Map), true, TEXT("listen?game=/Script/fpstrue.fpstruePvpGameMode"));
}

// 直连只接纳主机地址和端口，不接受额外地图或对局 URL 选项，更不会把重连票据放进 URL。
void UfpstrueMultiplayerGameInstance::JoinPvp(const FString& Address)
{
	if (ConnectionState != EFPConnectionState::Idle && ConnectionState != EFPConnectionState::Failed) return;
	FString Normalized = Address.TrimStartAndEnd().ToLower();
	if (Normalized.IsEmpty() || Normalized.Len() > 300) return;
	for (TCHAR Character : Normalized)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('.') && Character != TEXT('-') && Character != TEXT('_') &&
			Character != TEXT(':') && Character != TEXT('[') && Character != TEXT(']'))
		{
			UE_LOG(LogTemp, Warning, TEXT("JoinPvp expects a server IP or hostname with optional port."));
			return;
		}
	}
	int32 Colon = INDEX_NONE;
	if (Normalized.StartsWith(TEXT("[")))
	{
		int32 Closing;
		if (!Normalized.FindChar(TEXT(']'), Closing) || Closing <= 1) return;
		if (Closing == Normalized.Len() - 1) Normalized += TEXT(":7777");
		Colon = Closing + 1;
		if (!Normalized.IsValidIndex(Colon) || Normalized[Colon] != TEXT(':')) return;
	}
	else
	{
		if (!Normalized.FindLastChar(TEXT(':'), Colon)) { Normalized += TEXT(":7777"); Normalized.FindLastChar(TEXT(':'), Colon); }
		if (Normalized.Left(Colon).Contains(TEXT(":")) || Colon <= 0) return;
	}
	const FString Port = Normalized.Mid(Colon + 1);
	if (Port.IsEmpty() || Port.Len() > 5) return;
	for (TCHAR Digit : Port) if (!FChar::IsDigit(Digit)) return;
	if (FCString::Atoi(*Port) < 1 || FCString::Atoi(*Port) > 65535) return;
	ServerAddress = Normalized;
	bPendingLocalReturn = false;
	LastConnectionError.Reset();
	RetryCount = 0;
	bAutoReconnect = true;
	const uint32 Revision = ++OperationRevision;
	StartClientTravel(Revision);
}

// 控制台取消与主动离场共用同一清理链，不另建取消状态。
void UfpstrueMultiplayerGameInstance::CancelPvpConnection()
{
	LeavePvp();
}

// 使旧重试失效并清票据，再通知服务器主动退出；三秒未完成则由 Ticker 返回本地世界。
void UfpstrueMultiplayerGameInstance::LeavePvp()
{
	++OperationRevision;
	bAutoReconnect = false;
	bPendingLocalReturn = false;
	ScheduledRetryRevision = 0;
	ClearResumeCache();
	SetConnectionState(EFPConnectionState::Leaving);
	AttemptDeadline = FPlatformTime::Seconds() + 3.0;
	if (auto* Player = Cast<AfpstruePvpPlayerController>(GetFirstLocalPlayerController())) Player->ServerLeavePvp();
	else ReturnToLocalWorld();
}

// 仅在空闲或失败时重用上次地址重新连接，沿用 JoinPvp 的地址校验与操作初始化。
void UfpstrueMultiplayerGameInstance::RetryPvpConnection()
{
	if (ConnectionState == EFPConnectionState::Failed || ConnectionState == EFPConnectionState::Idle) JoinPvp(ServerAddress);
}

// ---- 当前连接的握手与复制就绪 ----

// 仅当前操作发起连接旅行并设置期限；状态广播返回后复核，避免 UI 取消后继续旅行。
void UfpstrueMultiplayerGameInstance::StartClientTravel(uint32 Revision)
{
	if (Revision != OperationRevision || !bAutoReconnect || ServerAddress.IsEmpty()) return;
	APlayerController* Player = GetFirstLocalPlayerController();
	if (Player == nullptr) { LastConnectionError = TEXT("Local player is not ready."); SetConnectionState(EFPConnectionState::Failed); return; }
	ConnectingController.Reset();
	AttemptNetDriver.Reset();
	ResumeId = INDEX_NONE; // 缓存票据可用于申请恢复，但不能证明本次连接已经接纳。
	bReturningToLocalWorld = false;
	AttemptDeadline = FPlatformTime::Seconds() + 20.0;
	SetConnectionState(EFPConnectionState::Connecting);
#if !UE_BUILD_SHIPPING
	if (RetryCount > 0 && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE LIVE_RECONNECT RetryStarted Pid=%u Operation=%u Attempt=%d"),
			FPlatformProcess::GetCurrentProcessId(), Revision, RetryCount);
#endif
	if (Revision == OperationRevision && ConnectionState == EFPConnectionState::Connecting) Player->ClientTravel(ServerAddress, TRAVEL_Absolute);
}

// 记录当前 Controller/NetDriver 并进入 WaitingReady；只向同地址服务器提交缓存票据。
void UfpstrueMultiplayerGameInstance::BeginConnectionHandshake(AfpstruePvpPlayerController* Player)
{
	if (!IsValid(Player) || !Player->IsLocalController() || Player->GetWorld() != GetWorld()) return;
	if (ConnectionState == EFPConnectionState::Leaving) { Player->ServerLeavePvp(); return; }
	if (ConnectingController == Player && (ConnectionState == EFPConnectionState::WaitingReady || ConnectionState == EFPConnectionState::Connected)) return;
	if (ConnectionState == EFPConnectionState::Idle || ConnectionState == EFPConnectionState::Failed)
	{
		ServerAddress = GetWorld()->URL.Host.ToLower() + FString::Printf(TEXT(":%d"), GetWorld()->URL.Port);
		++OperationRevision;
		RetryCount = 0;
		bAutoReconnect = true;
	}
	ConnectingController = Player;
	AttemptNetDriver = GetWorld()->GetNetDriver();
	ResumeId = INDEX_NONE; // 只有当前 Controller 的 AcceptConnectionToken 才发布本次身份。
	AttemptDeadline = FPlatformTime::Seconds() + 20.0;
	const uint32 HandshakeRevision = OperationRevision;
	SetConnectionState(EFPConnectionState::WaitingReady);
	if (HandshakeRevision == OperationRevision && ConnectionState == EFPConnectionState::WaitingReady && ConnectingController == Player)
		Player->ServerSubmitConnectionToken(ResumeServerAddress.Equals(ServerAddress, ESearchCase::IgnoreCase) ? ResumeToken : FString());
}

// 只接纳当前握手的应答并保存新票据；收到 ACK 不等于 Pawn、成绩和能力已经复制完成。
void UfpstrueMultiplayerGameInstance::AcceptConnectionToken(AfpstruePvpPlayerController* Player, const FString& Token, int32 InResumeId, bool bRestored)
{
	if (!IsValid(Player) || !Player->IsLocalController() || Player->GetWorld() != GetWorld() || !IsResumeToken(Token)
		|| ConnectionState == EFPConnectionState::Leaving) return;
	// 监听主机的本地控制器没有远程握手；只在此明确接纳它，远端旧 ACK 不能重新收养已经失败的流程。
	if (Player->HasAuthority() && GetWorld()->GetNetMode() == NM_ListenServer && Player == GetFirstLocalPlayerController() &&
		ConnectionState == EFPConnectionState::Idle)
	{
		ConnectingController = Player;
		ServerAddress = GetWorld()->URL.Host.ToLower() + FString::Printf(TEXT(":%d"), GetWorld()->URL.Port);
		++OperationRevision;
		SetConnectionState(EFPConnectionState::WaitingReady);
	}
	if (ConnectionState != EFPConnectionState::WaitingReady || ConnectingController != Player) return;
	ResumeToken = Token;
	ResumeServerAddress = ServerAddress;
	ResumeId = InResumeId;
	bRestoredConnection = bRestored;
	SaveResumeCache();
	AttemptDeadline = FPlatformTime::Seconds() + 20.0;
	SetConnectionState(EFPConnectionState::WaitingReady);
}

// 空手与持枪共用生命和能力就绪检查；装备是可选状态，而非连接成功的前置条件。
bool UfpstrueMultiplayerGameInstance::IsPawnStateReady(const AfpstrueCharacter* Pawn, const AfpstruePvpPlayerState* State)
{
	if (!IsValid(Pawn) || Pawn->IsDead() || State == nullptr || Pawn->GetPlayerState() != State) return false;
	const auto* ASC = Pawn->GetHealthComponent();
	const auto* Weapon = Pawn->GetEquippedWeaponComponent();
	return ASC != nullptr && ASC->GetHealthAttributeSet() != nullptr
		&& (Weapon == nullptr || (IsValid(Weapon) && Weapon->GetOwningCharacter() == Pawn))
		&& ASC->FindAbilitySpecFromClass(UfpstrueFireAbility::StaticClass()) != nullptr
		&& ASC->FindAbilitySpecFromClass(UfpstrueReloadAbility::StaticClass()) != nullptr
		&& ASC->FindAbilitySpecFromClass(UfpstrueAimAbility::StaticClass()) != nullptr
		&& ASC->FindAbilitySpecFromClass(UfpstrueSprintAbility::StaticClass()) != nullptr;
}

// 【设计取舍】Login/Accepted 只证明身份接纳；Ready 还等本端 Pawn、ASC、PlayerState 关联和时钟，避免绑定半就绪对象。
// 这会增加必要等待，但不保证 GPU 画面、所有 AI 或全世界同时恢复；完整画面验收另有显式流程。
bool UfpstrueMultiplayerGameInstance::TickConnection(float)
{
	const double Now = FPlatformTime::Seconds();
	if (bPendingLocalReturn)
	{
		bPendingLocalReturn = false;
		ReturnToLocalWorld();
		return true;
	}
	if (ConnectionState == EFPConnectionState::RetryWait)
	{
		if (ScheduledRetryRevision != OperationRevision) return true;
		if (Now >= RetryAt && !bReturningToLocalWorld && GetWorld() != nullptr && GetWorld()->GetNetMode() == NM_Standalone)
			StartClientTravel(ScheduledRetryRevision);
		return true;
	}
	if (ConnectionState == EFPConnectionState::Leaving)
	{
		if (Now >= AttemptDeadline) ReturnToLocalWorld();
		return true;
	}
	if (ConnectionState != EFPConnectionState::Connecting && ConnectionState != EFPConnectionState::WaitingReady) return true;
	if (GetWorld() != nullptr && GetWorld()->GetNetDriver() != nullptr) AttemptNetDriver = GetWorld()->GetNetDriver();
	if (Now >= AttemptDeadline) { LastConnectionError = TEXT("Connection or state preparation timed out."); HandleConnectionFailure(); return true; }
	auto* Player = ConnectingController.Get();
	auto* Pawn = Player != nullptr ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	auto* State = Player != nullptr ? Player->GetPlayerState<AfpstruePvpPlayerState>() : nullptr;
	auto* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	const bool bSpectatorReady = State != nullptr && State->IsOnlyASpectator();
	if (ConnectionState != EFPConnectionState::WaitingReady || ResumeToken.IsEmpty() || ResumeId == INDEX_NONE || State == nullptr
		|| (!bSpectatorReady && !IsPawnStateReady(Pawn, State)) || State->GetPlayerId() != ResumeId
		|| GetWorld()->GetGameState() == nullptr || (!bSpectatorReady && !Player->HasServerTimeSample())) return true;
	RetryCount = 0;
	LastConnectionError.Reset();
	const uint32 ReadyRevision = OperationRevision;
	SetConnectionState(EFPConnectionState::Connected);
	if (ReadyRevision != OperationRevision || ConnectionState != EFPConnectionState::Connected || ConnectingController != Player) return true;
	if (bSpectatorReady)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_RESUME CLIENT_SPECTATOR_READY ResumeId=%d PawnReady=0"), ResumeId);
		return true;
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME CLIENT_READY ResumeId=%d Restored=%d Pawn=%s PS=%s Health=%.1f Ammo=%d Kills=%d Deaths=%d Dead=%d PawnNetGuid=%llu StateNetGuid=%llu"),
		ResumeId, bRestoredConnection, *Pawn->GetName(), *State->GetName(), Pawn->GetCurrentHealth(), Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0, State->GetKills(), State->GetDeaths(), Pawn->IsDead(),
		GetResumeNetGuid(Pawn), GetResumeNetGuid(State));
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")))
	{
		if (!bRestoredConnection && LiveReconnectInitialResumeId == INDEX_NONE)
			LiveReconnectInitialResumeId = ResumeId;
		if (bRestoredConnection)
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE LIVE_RECONNECT Restored Pid=%u Operation=%u ResumeId=%d InitialResumeId=%d FailureObserved=%d PawnNetGuid=%llu StateNetGuid=%llu"),
				FPlatformProcess::GetCurrentProcessId(), OperationRevision, ResumeId, LiveReconnectInitialResumeId,
				bLiveReconnectFailureObserved, GetResumeNetGuid(Pawn), GetResumeNetGuid(State));
	}
#endif
	return true;
}

// 本地连接状态的统一提交入口；UI 通过 GetConnectionState 查询，不保留未订阅的广播。
void UfpstrueMultiplayerGameInstance::SetConnectionState(EFPConnectionState State)
{
	ConnectionState = State;
}

// ---- 失败归属、退避与返回本地世界 ----

// 按所属 GameInstance 的世界与待连接 NetDriver 归属过滤引擎全局失败事件，避免跨 PIE 实例串扰。
bool UfpstrueMultiplayerGameInstance::OwnsNetworkFailure(UWorld* World, UNetDriver* Driver) const
{
	if (GEngine == nullptr) return false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.OwningGameInstance.Get() != this) continue;
		if (Driver != nullptr && (Driver == AttemptNetDriver.Get() || (Context.World() != nullptr && Driver == Context.World()->GetNetDriver())
			|| (Context.PendingNetGame != nullptr && Driver == Context.PendingNetGame->NetDriver))) return true;
		if (Driver == nullptr && World != nullptr && World == Context.World()) return true;
	}
	return false;
}

// 仅当前实例活动连接的网络失败进入重试流程；无关世界和已终止操作忽略。
void UfpstrueMultiplayerGameInstance::HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Failure, const FString&)
{
	// 监听主机的远端连接失败不代表本地玩家离线；不能把整场服务器带回本地世界。
	if (Driver != nullptr && Driver->GetNetMode() != NM_Client) return;
	if (!OwnsNetworkFailure(World, Driver) || (ConnectionState != EFPConnectionState::Connected
		&& ConnectionState != EFPConnectionState::Connecting && ConnectionState != EFPConnectionState::WaitingReady)) return;
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")))
	{
		bLiveReconnectFailureObserved = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE LIVE_RECONNECT FailureObserved Pid=%u Operation=%u ResumeId=%d Failure=%s Driver=%s"),
			FPlatformProcess::GetCurrentProcessId(), OperationRevision, ResumeId, ENetworkFailure::ToString(Failure), *GetNameSafe(Driver));
	}
#endif
	LastConnectionError = TEXT("Network connection lost.");
	HandleConnectionFailure();
}

// 当前连接加载地图失败时复用连接失败处理，不接管普通世界旅行错误。
void UfpstrueMultiplayerGameInstance::HandleTravelFailure(UWorld* World, ETravelFailure::Type, const FString&)
{
	if (ConnectionState != EFPConnectionState::Connecting && ConnectionState != EFPConnectionState::WaitingReady) return;
	if (!OwnsNetworkFailure(World, nullptr)) return;
	LastConnectionError = TEXT("Map travel failed.");
	HandleConnectionFailure();
}

// 最多安排三次指数退避；先返回本地世界，Ticker 等待旅行结束后再启动下一次连接。
void UfpstrueMultiplayerGameInstance::HandleConnectionFailure(double MinimumRetryDelay)
{
	ConnectingController.Reset();
	// UEngine 在 NetworkFailure 委托返回后继续安排断线旅行。延后一轮避免两个入口覆盖 TravelURL。
	bPendingLocalReturn = true;
	if (!bAutoReconnect || RetryCount >= 3)
	{
		bAutoReconnect = false;
		SetConnectionState(EFPConnectionState::Failed);
		return;
	}
	RetryAt = FPlatformTime::Seconds() + FMath::Max(double(1 << RetryCount++), MinimumRetryDelay);
	ScheduledRetryRevision = OperationRevision;
	SetConnectionState(EFPConnectionState::RetryWait);
}

// 取消尚未完成的连接并打开本地空 GameMode；重连进度保留在跨世界 GameInstance。
void UfpstrueMultiplayerGameInstance::ReturnToLocalWorld()
{
	UWorld* World = GetWorld();
	if (World == nullptr) { if (ConnectionState == EFPConnectionState::Leaving) SetConnectionState(EFPConnectionState::Idle); return; }
	if (bReturningToLocalWorld) return;
	if (GEngine != nullptr) GEngine->CancelPending(World);
	const FString Map = ResolvePvpMapPackage();
	if (!FPackageName::IsValidLongPackageName(Map)) { SetConnectionState(EFPConnectionState::Failed); return; }
	bReturningToLocalWorld = true;
	UGameplayStatics::OpenLevel(this, FName(*Map), true, TEXT("game=/Script/Engine.GameModeBase"));
}

// 本实例本地世界加载完成后解除旅行标记；离场流程至此回到 Idle。
void UfpstrueMultiplayerGameInstance::HandlePostLoadMap(UWorld* World)
{
	if (World == nullptr || World->GetGameInstance() != this || World->GetNetMode() != NM_Standalone) return;
	bReturningToLocalWorld = false;
	AttemptNetDriver.Reset();
	if (ConnectionState == EFPConnectionState::Leaving) SetConnectionState(EFPConnectionState::Idle);
}

// 活跃槽暂不可恢复时保留票据重试；其余拒绝清票据并结束自动重连，主动退出只返回本地。
void UfpstrueMultiplayerGameInstance::RejectConnection(const FString& Reason)
{
	if (Reason == TEXT("manual_leave")) { ReturnToLocalWorld(); return; }
	LastConnectionError = Reason;
	// 半开连接可能尚未被服务器判离线；不能抢占活跃角色，但也不应销毁合法票据。
	// 单向断包时客户端可能先超时；等旧连接判离，不抢占在线身份。
	// 沿当前连接超时作有界退避，仍最多三次；服务器超时配置不同或保留期已过时不保证恢复。
	if (Reason == TEXT("resume_slot_active"))
	{
		const UNetDriver* Driver = AttemptNetDriver.Get();
		const double RetryDelay = Driver != nullptr ? FMath::Clamp(double(Driver->ConnectionTimeout), 3.0, 30.0) : 3.0;
		HandleConnectionFailure(RetryDelay);
		return;
	}
	bAutoReconnect = false;
	ClearResumeCache();
	SetConnectionState(EFPConnectionState::Failed);
	ReturnToLocalWorld();
}

// ---- 地址配置与私有恢复凭证 ----

FString UfpstrueMultiplayerGameInstance::ResolvePvpMapPackage() const
{
	const FString Map = PvpMap.TrimStartAndEnd();
	return FPackageName::ObjectPathToPackageName(Map.IsEmpty()
		? UGameMapsSettings::GetGameDefaultMap(EDefaultMapRequestType::Client) : Map);
}

// 缓存只含服务器地址和 bearer 票据，位于每用户目录；测试 UserDir 的 owner-only 权限由启动脚本建立。
// 同目录临时文件写完再替换，进程被强停不能留下半个新票据；绝不把内容写进日志或公共 Config。
void UfpstrueMultiplayerGameInstance::SaveResumeCache()
{
	if (ResumeCacheFile.IsEmpty() || !IsResumeToken(ResumeToken)) return;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResumeCacheFile), true);
	const FString Temporary = ResumeCacheFile + TEXT(".tmp");
	const FString Content = FString::Printf(TEXT("FPSTRUE_RESUME_1\n%s\n%s\n%lld\n"), *ResumeServerAddress, *ResumeToken,
		(FDateTime::UtcNow() + FTimespan::FromDays(1.0)).GetTicks());
	if (!FFileHelper::SaveStringToFile(Content, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*ResumeCacheFile, *Temporary, true, false, false, true))
		UE_LOG(LogTemp, Warning, TEXT("Resume cache could not be saved; reconnect is available only while this process remains alive."));
}

// 读取尺寸、版本、格式及本地到期时间合格的缓存；真正恢复资格仍由服务器验证。
void UfpstrueMultiplayerGameInstance::LoadResumeCache()
{
	if (ResumeCacheFile.IsEmpty() || IFileManager::Get().FileSize(*ResumeCacheFile) > 4096) return;
	FString Content;
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToString(Content, *ResumeCacheFile)) return;
	Content.ParseIntoArrayLines(Lines, true);
	if (Lines.Num() != 4 || Lines[0] != TEXT("FPSTRUE_RESUME_1") || !IsResumeToken(Lines[2])
		|| FCString::Atoi64(*Lines[3]) <= FDateTime::UtcNow().GetTicks()) return;
	ResumeServerAddress = Lines[1];
	ResumeToken = Lines[2];
}

// 主动离场或不可恢复拒绝时清除内存票据和本地缓存；不会修改服务器游戏状态。
void UfpstrueMultiplayerGameInstance::ClearResumeCache()
{
	ResumeToken.Reset();
	ResumeServerAddress.Reset();
	ResumeId = INDEX_NONE;
	if (!ResumeCacheFile.IsEmpty())
	{
		IFileManager::Get().Delete(*ResumeCacheFile, false, true, true);
		IFileManager::Get().Delete(*(ResumeCacheFile + TEXT(".tmp")), false, true, true);
	}
}
