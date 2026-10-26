// Copyright Epic Games, Inc. All Rights Reserved.
#include "Testing/Networking/fpstrueNetworkDiagnostics.h"
#if !UE_BUILD_SHIPPING
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Player/fpstrueCharacterMovementComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Game/fpstrueMultiplayerGameInstance.h"
#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "ChartCreation.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
bool ReadCurrentThreadCpu(uint64& Value)
{
#if PLATFORM_WINDOWS
	FILETIME Created, Exited, Kernel, User;
	if (!::GetThreadTimes(::GetCurrentThread(), &Created, &Exited, &Kernel, &User)) return false;
	Value = (uint64(Kernel.dwHighDateTime) << 32 | Kernel.dwLowDateTime)
		+ (uint64(User.dwHighDateTime) << 32 | User.dwLowDateTime);
	return true;
#else
	return false;
#endif
}

struct FNetworkUnits : IPerformanceDataConsumer
{
	struct FSample { double FlushMs = -1.0, IdleMs = -1.0, OvershootMs = -1.0; };
	double TickFlushMs = -1.0;
	uint64 Frame = 0;
	TMap<uint64, FSample> Samples;
	virtual void StartCharting() override {}
	virtual void StopCharting() override {}
	virtual void ProcessFrame(const FFrameData& Data) override
	{
		TickFlushMs = FMath::IsFinite(Data.GameDriverTickFlushTimeSeconds)
			? Data.GameDriverTickFlushTimeSeconds * 1000.0 : -1.0;
		// UE5.5 TickPerformanceMonitoring 在 GEngine->Tick 之前消费上一帧的 TickFlush。
		Frame = GFrameCounter > 0 ? GFrameCounter - 1 : 0;
		if (Samples.Num() < 65534)
		{
			Samples.FindOrAdd(Frame).FlushMs = TickFlushMs;
			// 限频等待已在本帧开头完成，和上一帧的网络作用域不能使用同一个帧号。
			auto& Current = Samples.FindOrAdd(GFrameCounter);
			Current.IdleMs = Data.IdleSeconds * 1000.0;
			Current.OvershootMs = Data.IdleOvershootSeconds * 1000.0;
		}
	}
};

// 采集状态只在显式测试中存在。连接累计计数独立输出，绝不把全 Driver 字节平均成“每玩家流量”。
struct FNetworkDiagnostics : TSharedFromThis<FNetworkDiagnostics>
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UEngine> UnitEngine;
	TSharedPtr<FNetworkUnits> Units;
	FDelegateHandle TickStart, TickEnd;
	TSet<TWeakObjectPtr<AfpstrueCharacter>> PreparedPawns;
	struct FFrameSample { uint64 Frame, UnitFrame; double Seconds, FlushMs, CpuMs, WorldWallMs; };
	TArray<FFrameSample> Frames;
	double LastSample = 0.0, LastFrameLog = 0.0;
	double DrainStarted = 0.0;
	double WorldTickBeginLocal = 0.0;
	double CaptureStartServerTime = 0.0;
	uint64 CpuStart = 0;
	bool bCpuStartValid = false, bCapacity = false, bReady = false, bCapturing = false, bStopped = false;
	bool bFiring = false, bDone = false, bOverflowed = false;
	int32 ExpectedClients = 2, ClientIndex = 0, Enemies = 160, LastReloadCycle = -1;
	float WarmupSeconds = 10.0f, CaptureSeconds = 45.0f;
#if DO_ENABLE_NET_TEST
	TWeakObjectPtr<UNetConnection> ActivationLossConnection;
	int32 ActivationLossPreviousPercent = 0;
	bool bActivationLossInjected = false;
	void RestoreActivationLoss()
	{
		if (auto* Connection = ActivationLossConnection.Get())
		{
			Connection->PacketSimulationSettings.PktLoss = ActivationLossPreviousPercent;
			UE_LOG(LogTemp, Display, TEXT("PVP_NET_FAULT Kind=FirstFireActivation State=Restored Client=%d Frame=%llu Loss=%d"), ClientIndex, GFrameCounter, ActivationLossPreviousPercent);
		}
		ActivationLossConnection.Reset();
	}
	void InjectActivationLoss()
	{
		if (bActivationLossInjected || ClientIndex != 1 || !FParse::Param(FCommandLine::Get(), TEXT("PvpDropFirstFireActivation"))) return;
		UNetDriver* Driver = World.IsValid() ? World->GetNetDriver() : nullptr;
		UNetConnection* Connection = Driver != nullptr ? Driver->ServerConnection : nullptr;
		if (Connection == nullptr) return;
		ActivationLossConnection = Connection;
		ActivationLossPreviousPercent = Connection->PacketSimulationSettings.PktLoss;
		Connection->PacketSimulationSettings.PktLoss = 100;
		bActivationLossInjected = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_NET_FAULT Kind=FirstFireActivation State=Started Client=%d Frame=%llu PreviousLoss=%d Loss=100 Duration=OneWorldFlush"), ClientIndex, GFrameCounter, ActivationLossPreviousPercent);
	}
#endif

	const TCHAR* Role() const { return World.IsValid() && World->GetNetMode() == NM_Client ? TEXT("Client") : TEXT("Server"); }
	~FNetworkDiagnostics()
	{
#if DO_ENABLE_NET_TEST
		RestoreActivationLoss();
#endif
		FWorldDelegates::OnWorldTickStart.Remove(TickStart);
		FWorldDelegates::OnWorldTickEnd.Remove(TickEnd);
		if (UnitEngine.IsValid() && Units.IsValid()) UnitEngine->RemovePerformanceDataConsumer(Units);
	}
	void BeginTick(UWorld* TickWorld, ELevelTick, float)
	{
		if (TickWorld == World.Get())
		{
			WorldTickBeginLocal = FPlatformTime::Seconds();
			bCpuStartValid = ReadCurrentThreadCpu(CpuStart);
		}
	}
	void EndTick(UWorld* TickWorld, ELevelTick, float)
	{
		if (TickWorld != World.Get()) return;
#if DO_ENABLE_NET_TEST
		// 此事件在本帧 TickFlush 之后；原生丢包模拟已处理数据报，再恢复正常弱网配置。
		RestoreActivationLoss();
#endif
		if (bCapacity && !bCapturing) return;
		uint64 CpuEnd = 0;
		const double CpuMs = bCpuStartValid && ReadCurrentThreadCpu(CpuEnd) && CpuEnd >= CpuStart
			? (CpuEnd - CpuStart) * 0.0001 : -1.0;
		const double Now = FPlatformTime::Seconds();
		// 完整逐帧记录，窗口后再写盘，避免日志 I/O 改变测量结果。上限溢出明确失败。
		if (Frames.Num() >= 65536) { bOverflowed = true; return; }
		Frames.Add({GFrameCounter, Units.IsValid() ? Units->Frame : 0, Now, Units.IsValid() ? Units->TickFlushMs : -1.0, CpuMs,
			WorldTickBeginLocal > 0.0 ? (Now - WorldTickBeginLocal) * 1000.0 : -1.0});
	}
	void FlushFrames()
	{
		for (const auto& Frame : Frames)
		{
			const auto* Unit = Units.IsValid() ? Units->Samples.Find(Frame.Frame) : nullptr;
			UE_LOG(LogTemp, Display, TEXT("PVP_NET_FRAME Role=%s Client=%d Frame=%llu LocalSeconds=%.6f ReplicationTickFlushMs=%.6f WorldTickCpuMs=%.6f UnitFrame=%llu PacingIdleMs=%.6f IdleOvershootMs=%.6f WorldTickWallMs=%.6f"),
				Role(), ClientIndex, Frame.Frame, Frame.Seconds, Unit != nullptr ? Unit->FlushMs : -1.0, Frame.CpuMs, Frame.Frame,
				Unit != nullptr ? Unit->IdleMs : -1.0, Unit != nullptr ? Unit->OvershootMs : -1.0, Frame.WorldWallMs);
		}
		Frames.Reset();
		if (Units.IsValid()) Units->Samples.Reset();
	}
	void SampleConnections(double BoundarySeconds = 0.0)
	{
		UWorld* W = World.Get();
		UNetDriver* Driver = W != nullptr ? W->GetNetDriver() : nullptr;
		if (Driver == nullptr) return;
		// 探测 RTT 属于拥有者本机观测；专服不能把它当作自己测得的连接 Ping。
		const auto* Clock = Driver->ServerConnection != nullptr
			? Cast<AfpstruePvpPlayerController>(W->GetFirstPlayerController()) : nullptr;
		const bool bRttFresh = Clock != nullptr && Clock->HasRoundTripTimeSample();
		const double ProbeRttMs = bRttFresh ? Clock->GetSmoothedRoundTripTimeSeconds() * 1000.0 : -1.0;
		const double ProbeVariationMs = bRttFresh ? Clock->GetRoundTripVariationSeconds() * 1000.0 : -1.0;
		int64 Corrections = -1;
		if (auto* PC = W->GetFirstPlayerController(); PC != nullptr && PC->IsLocalController())
			if (auto* Pawn = Cast<AfpstrueCharacter>(PC->GetPawn()))
				if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(Pawn->GetCharacterMovement()))
					Corrections = Movement->GetNetworkCorrectionCount();
		const auto Sample = [&](UNetConnection* Connection)
		{
			if (Connection == nullptr) return;
			UE_LOG(LogTemp, Display, TEXT("PVP_NET_SAMPLE Role=%s Client=%d LocalSeconds=%.6f Connection=%u OutBytes=%d OutPackets=%d InBytes=%d InPackets=%d MovementCorrections=%lld OutLost=%d InLost=%d NetSpeed=%d ProbeRttFresh=%d ProbeRttMs=%.3f ProbeRttVariationMs=%.3f"),
				Role(), ClientIndex, BoundarySeconds > 0.0 ? BoundarySeconds : FPlatformTime::Seconds(), Connection->GetUniqueID(), Connection->OutTotalBytes,
				Connection->OutTotalPackets, Connection->InTotalBytes, Connection->InTotalPackets, Corrections,
				Connection->OutTotalPacketsLost, Connection->InTotalPacketsLost, Connection->CurrentNetSpeed,
				bRttFresh, ProbeRttMs, ProbeVariationMs);
		};
		if (Driver->ServerConnection != nullptr) Sample(Driver->ServerConnection);
		else for (UNetConnection* Connection : Driver->ClientConnections) Sample(Connection);
	}
	void DriveCapacity(AfpstruePvpPlayerController* PC)
	{
		auto* Pawn = PC != nullptr ? Cast<AfpstrueCharacter>(PC->GetPawn()) : nullptr;
		if (Pawn == nullptr || Pawn->IsDead()) return;
		UfpstrueWeaponComponent* Weapon = Pawn->GetEquippedWeaponComponent();
		if (Weapon == nullptr) return;
		const double Elapsed = PC->GetEstimatedServerTime() - CaptureStartServerTime;
		// 上空射击也执行正常服务器射线；大血量仅防 AI 终止负载，并非禁用伤害或无敌。
		PC->SetControlRotation(FRotator(75.0f, ClientIndex * 45.0f, 0.0f));
		const float Direction = (static_cast<int32>(Elapsed / 3.0) % 2) == 0 ? 1.0f : -1.0f;
		Pawn->AddMovementInput(FVector(FMath::Cos(ClientIndex * .7), FMath::Sin(ClientIndex * .7), 0.0f), Direction * .4f);
		const bool bWantFire = FMath::Fmod(Elapsed, 3.0) < 2.0 && !Weapon->IsReloading();
		if (auto* ASC = Pawn->GetAbilitySystemComponent())
		{
			if (bWantFire && !bFiring)
			{
#if DO_ENABLE_NET_TEST
				InjectActivationLoss();
#endif
				ASC->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
			}
			if (!bWantFire && bFiring) ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
		}
		bFiring = bWantFire;
		const int32 ReloadCycle = static_cast<int32>(Elapsed / 6.0);
		if (!bWantFire && ReloadCycle != LastReloadCycle && Weapon->GetCurrentAmmo() < Weapon->GetMagazineSize()
			&& !Weapon->IsReloading() && Weapon->GetPendingPredictedShotCount() == 0)
		{ LastReloadCycle = ReloadCycle; Pawn->GetAbilitySystemComponent()->TryActivateAbilityByClass(UfpstrueReloadAbility::StaticClass()); }
	}
	bool Tick(float)
	{
		UWorld* W = World.Get();
		if (!IsValid(W) || W->bIsTearingDown) return false;
		const double LocalNow = FPlatformTime::Seconds();
		if (!bCapacity)
		{
			if (LocalNow - LastSample >= 1.0) { LastSample = LocalNow; SampleConnections(); FlushFrames(); }
			return true;
		}
		const bool bServer = W->GetNetMode() != NM_Client;
		auto* PC = Cast<AfpstruePvpPlayerController>(W->GetFirstPlayerController());
		if (bStopped)
		{
			if (bDone) return true;
			const auto* Pawn = PC != nullptr ? Cast<AfpstrueCharacter>(PC->GetPawn()) : nullptr;
			const auto* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
			bool bSettled = Weapon != nullptr && Weapon->GetPendingPredictedShotCount() == 0 && !Weapon->IsReloading() && !Weapon->IsFiring();
			if (bServer)
			{
				bSettled = true;
				for (TActorIterator<AfpstrueCharacter> It(W); It; ++It)
					if (const auto* Equipped = It->GetEquippedWeaponComponent())
						bSettled &= Equipped->GetPendingAuthoritativeShotCount() == 0 && !Equipped->IsReloading() && !Equipped->IsFiring();
			}
			if (LocalNow - DrainStarted >= (bServer ? 8.0 : 2.0) && bSettled)
			{
				bDone = true;
				FlushFrames();
				UE_LOG(LogTemp, Display, TEXT("PVP_CAPACITY DONE Role=%s Client=%d LocalSeconds=%.6f Outstanding=0 Overflow=%d"), Role(), ClientIndex, LocalNow, bOverflowed);
			}
			else if (LocalNow - DrainStarted > 10.0)
			{
				bDone = true;
				UE_LOG(LogTemp, Error, TEXT("PVP_CAPACITY FAIL Role=%s Client=%d Reason=UnsettledAfterDrain"), Role(), ClientIndex);
			}
			return true;
		}
		if (bServer)
		{
			for (TActorIterator<AfpstrueCharacter> It(W); It; ++It)
				if (!PreparedPawns.Contains(*It) && It->GetController() != nullptr)
				{
					It->GetHealthComponent()->SetMaxHealthAndReset(1000000.0f);
					PreparedPawns.Add(*It);
				}
			if (!bReady)
			{
				auto* Mode = W->GetAuthGameMode<AfpstruePvpGameMode>();
				UNetDriver* Driver = W->GetNetDriver();
				if (Mode == nullptr || Driver == nullptr || Driver->ClientConnections.Num() != ExpectedClients || Mode->GetAliveEnemyCount() != Enemies) return true;
				TArray<AfpstruePvpPlayerController*> Players;
				for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
					if (auto* Player = Cast<AfpstruePvpPlayerController>(It->Get()); Player != nullptr)
						if (const auto* Pawn = Cast<AfpstrueCharacter>(Player->GetPawn()); Pawn != nullptr && Pawn->HasEquippedWeapon()) Players.Add(Player);
				if (Players.Num() != ExpectedClients) return true;
				CaptureStartServerTime = W->GetTimeSeconds() + WarmupSeconds;
				for (auto* Player : Players) Player->ClientBeginNetworkCapacity(CaptureStartServerTime);
				bReady = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_CAPACITY READY Role=Server Client=0 Clients=%d Enemies=%d LocalSeconds=%.6f StartServerTime=%.6f"), ExpectedClients, Enemies, LocalNow, CaptureStartServerTime);
			}
		}
		else
		{
			if (PC == nullptr || !PC->HasServerTimeSample()) return true;
			CaptureStartServerTime = PC->GetNetworkCapacityStartServerTime();
			if (CaptureStartServerTime <= 0.0) return true;
			if (!bReady)
			{
				bReady = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_CAPACITY READY Role=Client Client=%d LocalSeconds=%.6f StartServerTime=%.6f"), ClientIndex, LocalNow, CaptureStartServerTime);
			}
		}
		const double ServerNow = bServer ? W->GetTimeSeconds() : PC->GetEstimatedServerTime();
		if (!bCapturing && ServerNow >= CaptureStartServerTime)
		{
			bCapturing = true;
			Frames.Reset();
			if (Units.IsValid()) Units->Samples.Reset();
			UE_LOG(LogTemp, Display, TEXT("PVP_CAPACITY START Role=%s Client=%d LocalSeconds=%.6f ServerTime=%.6f"), Role(), ClientIndex, LocalNow, ServerNow);
			SampleConnections(LocalNow); LastSample = LocalNow;
		}
		if (!bCapturing) return true;
		if (ServerNow >= CaptureStartServerTime + CaptureSeconds)
		{
			if (!bServer && PC != nullptr) if (auto* Pawn = Cast<AfpstrueCharacter>(PC->GetPawn()))
				Pawn->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
			SampleConnections(LocalNow); bCapturing = false; bStopped = true; DrainStarted = LocalNow;
			UE_LOG(LogTemp, Display, TEXT("PVP_CAPACITY STOP Role=%s Client=%d LocalSeconds=%.6f ServerTime=%.6f"), Role(), ClientIndex, LocalNow, ServerNow);
			return true;
		}
		if (bServer)
		{
			const auto* Driver = W->GetNetDriver();
			const auto* Mode = W->GetAuthGameMode<AfpstruePvpGameMode>();
			if (Driver == nullptr || Driver->ClientConnections.Num() != ExpectedClients || Mode == nullptr || Mode->GetAliveEnemyCount() != Enemies)
			{
				UE_LOG(LogTemp, Error, TEXT("PVP_CAPACITY FAIL Role=Server Client=0 Reason=PopulationOrConnectionChanged LocalSeconds=%.6f"), LocalNow);
				bStopped = bDone = true;
				return true;
			}
		}
		if (!bServer) DriveCapacity(PC);
		if (LocalNow - LastSample >= 1.0) { LastSample = LocalNow; SampleConnections(); }
		return true;
	}
};
}
#endif

void StartPvpNetworkDiagnostics(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (World == nullptr || !FParse::Param(FCommandLine::Get(), TEXT("PvpNetMetrics"))) return;
#if PLATFORM_WINDOWS && defined(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
	// 单变量平台诊断，仅作用于自有测试进程；默认不改系统策略，不调整全机电源或进程优先级。
	FString TimerPolicy;
	FParse::Value(FCommandLine::Get(), TEXT("PvpTimerResolution="), TimerPolicy);
	bool bTimerApplied = false;
	DWORD TimerError = 0;
	if (TimerPolicy.Equals(TEXT("Respect"), ESearchCase::IgnoreCase))
	{
		PROCESS_POWER_THROTTLING_STATE Policy{};
		Policy.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		Policy.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
		Policy.StateMask = 0;
		// UE5.5 的最低 Windows 编译目标不声明 Win8+ 的此 API，按本机实际可用性解析。
		using FSetProcessInformation = BOOL (WINAPI *)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
		const auto SetInformation = reinterpret_cast<FSetProcessInformation>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
		bTimerApplied = SetInformation != nullptr && SetInformation(::GetCurrentProcess(), ProcessPowerThrottling, &Policy, sizeof(Policy)) != 0;
		if (!bTimerApplied) TimerError = SetInformation != nullptr ? ::GetLastError() : ERROR_PROC_NOT_FOUND;
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_NET_ENV TimerPolicy=%s Applied=%d Error=%u"), TimerPolicy.IsEmpty() ? TEXT("System") : *TimerPolicy, bTimerApplied, TimerError);
#endif
	auto Diagnostics = MakeShared<FNetworkDiagnostics>();
	Diagnostics->World = World;
	Diagnostics->bCapacity = FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity"));
	FParse::Value(FCommandLine::Get(), TEXT("PvpNetworkClients="), Diagnostics->ExpectedClients);
	FParse::Value(FCommandLine::Get(), TEXT("PvpNetworkClient="), Diagnostics->ClientIndex);
	FParse::Value(FCommandLine::Get(), TEXT("PvpNetworkEnemies="), Diagnostics->Enemies);
	FParse::Value(FCommandLine::Get(), TEXT("PvpNetworkWarmupSeconds="), Diagnostics->WarmupSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("PvpNetworkCaptureSeconds="), Diagnostics->CaptureSeconds);
	Diagnostics->ExpectedClients = FMath::Clamp(Diagnostics->ExpectedClients, 2, 16);
	Diagnostics->Enemies = FMath::Clamp(Diagnostics->Enemies, 0, 160);
	Diagnostics->WarmupSeconds = FMath::Clamp(Diagnostics->WarmupSeconds, 5.0f, 60.0f);
	Diagnostics->CaptureSeconds = FMath::Clamp(Diagnostics->CaptureSeconds, 10.0f, 180.0f);
	Diagnostics->Frames.Reserve(16384);
	if (auto* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.ReportGameTickFlushTime"))) CVar->Set(1, ECVF_SetByCode);
	Diagnostics->Units = MakeShared<FNetworkUnits>();
	Diagnostics->UnitEngine = GEngine;
	if (GEngine != nullptr) GEngine->AddPerformanceDataConsumer(Diagnostics->Units);
	Diagnostics->TickStart = FWorldDelegates::OnWorldTickStart.AddSP(Diagnostics, &FNetworkDiagnostics::BeginTick);
	Diagnostics->TickEnd = FWorldDelegates::OnWorldTickEnd.AddSP(Diagnostics, &FNetworkDiagnostics::EndTick);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Diagnostics](float Delta) { return Diagnostics->Tick(Delta); }), .02f);
#endif
}
