// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Game/fpstrueMultiplayerGameInstance.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueLiveReconnectRetryTest,
	"fpstrue.Multiplayer.Reconnect.DeferredTravelAndBoundedActiveSlotRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 无 World/Init 的调度规则用例，不读写真实票据、不调用引擎失败广播，也不冒充链路中断实测。
bool FFpstrueLiveReconnectRetryTest::RunTest(const FString&)
{
	TStrongObjectPtr<UfpstrueMultiplayerGameInstance> Instance(NewObject<UfpstrueMultiplayerGameInstance>());
	Instance->OperationRevision = 17;
	Instance->ConnectionState = EFPConnectionState::Connected;
	Instance->bAutoReconnect = true;
	Instance->ResumeToken = FString::ChrN(64, TEXT('A'));
	Instance->ResumeId = 41;
	const FString OriginalToken = Instance->ResumeToken;
	const double FirstBefore = FPlatformTime::Seconds();
	Instance->HandleConnectionFailure();
	const double FirstAfter = FPlatformTime::Seconds();
	TestTrue(TEXT("Failure defers local travel until outside the engine failure call stack"), Instance->bPendingLocalReturn);
	TestTrue(TEXT("Travel has not started synchronously inside the failure callback"), !Instance->bReturningToLocalWorld);
	TestTrue(TEXT("First network retry keeps the ordinary one-second backoff"),
		Instance->RetryAt >= FirstBefore + 1.0 && Instance->RetryAt <= FirstAfter + 1.0);
	TestEqual(TEXT("First retry belongs to the current operation"), Instance->ScheduledRetryRevision, uint32(17));
	TestEqual(TEXT("Transport failure retains the server-issued credential"), Instance->ResumeToken, OriginalToken);

	// 只暂改抽象 Driver CDO 的这一项配置并在退出时恢复，不创建套接字/连接/World。
	// 服务器尚未判离时不抢角色；等待当前连接的超时尺度，而非用测试场景的秒数写死生产。
	UNetDriver* Driver = GetMutableDefault<UNetDriver>();
	TGuardValue<float> RestoreTimeout(Driver->ConnectionTimeout, 12.0f);
	Instance->AttemptNetDriver = Driver;
	const double ActiveBefore = FPlatformTime::Seconds();
	Instance->RejectConnection(TEXT("resume_slot_active"));
	const double ActiveAfter = FPlatformTime::Seconds();
	TestEqual(TEXT("An active-slot response consumes exactly one bounded retry"), Instance->RetryCount, 2);
	TestTrue(TEXT("An active slot waits on the actual configured connection timeout"),
		Instance->RetryAt >= ActiveBefore + 12.0 && Instance->RetryAt <= ActiveAfter + 12.0);
	TestEqual(TEXT("An active slot never clears the valid credential"), Instance->ResumeToken, OriginalToken);
	TestTrue(TEXT("Active-slot rejection still uses the one local-travel path"), Instance->bPendingLocalReturn);

	Instance->RejectConnection(TEXT("resume_slot_active"));
	TestEqual(TEXT("Third retry remains available"), Instance->RetryCount, 3);
	Instance->RejectConnection(TEXT("resume_slot_active"));
	TestTrue(TEXT("Exhaustion ends automatic reconnect rather than looping forever"),
		Instance->ConnectionState == EFPConnectionState::Failed && !Instance->bAutoReconnect);
	TestEqual(TEXT("Retry exhaustion preserves the token for explicit retry within server retention"), Instance->ResumeToken, OriginalToken);
	Instance->LeavePvp();
	TestFalse(TEXT("Explicit cancel removes a deferred return from the old failure"), Instance->bPendingLocalReturn);
	TestTrue(TEXT("Explicit cancel invalidates the old operation"), Instance->OperationRevision != uint32(17));
	TestTrue(TEXT("Explicit cancel revokes local recovery credentials"), Instance->ResumeToken.IsEmpty());

	for (const float Timeout : {0.5f, 120.0f})
	{
		Driver->ConnectionTimeout = Timeout;
		Instance->RetryCount = 0;
		Instance->bAutoReconnect = true;
		const double Before = FPlatformTime::Seconds();
		Instance->RejectConnection(TEXT("resume_slot_active"));
		const double After = FPlatformTime::Seconds();
		const double Expected = Timeout < 3.0f ? 3.0 : 30.0;
		TestTrue(Timeout < 3.0f ? TEXT("Tiny configured timeout cannot create a retry burst") : TEXT("Large configured timeout cannot wait without a bound"),
			Instance->RetryAt >= Before + Expected && Instance->RetryAt <= After + Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueClockQualityRebaseTest,
	"fpstrue.Multiplayer.Clock.BetterSampleRebasesStartupEstimate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 通过真实接收入口消费有身份的发送记录；合成的是延迟/服务器时间，不调用睡眠、不修改射击年龄窗口。
bool FFpstrueClockQualityRebaseTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	auto* Controller = World->SpawnActor<AfpstruePvpPlayerController>();
	if (!TestNotNull(TEXT("Clock regression uses the real project controller"), Controller)) return false;
	Controller->SetRole(ROLE_SimulatedProxy);
	const auto Receive = [&](uint32 Sequence, double RoundTrip, double Offset)
	{
		const double Now = FPlatformTime::Seconds();
		Controller->PendingClockSamples.Add(Sequence, Now - RoundTrip);
		Controller->ClientReceiveClockSample_Implementation(Sequence, Now + Offset - RoundTrip * 0.5);
	};
	TestFalse(TEXT("A fresh proxy has no usable server-time estimate"), Controller->HasServerTimeSample());
	Receive(1, 0.54, 99.76);
	TestTrue(TEXT("The slow startup sample establishes its deliberately biased midpoint"),
		FMath::Abs(Controller->ServerClockOffset - 99.76) < 0.003 && Controller->ClockUncertainty > 0.26);
	Receive(2, 0.02, 100.0);
	TestTrue(TEXT("A high-quality sample replaces the startup bias immediately, without several smoothing intervals"),
		FMath::Abs(Controller->ServerClockOffset - 100.0) < 0.003);
	TestTrue(TEXT("The new uncertainty comes from the good sample rather than the abandoned240ms startup residual"),
		Controller->ClockUncertainty >= 0.009 && Controller->ClockUncertainty < 0.02);
	TestTrue(TEXT("The proxy now exposes a fresh server-time mapping"), Controller->HasServerTimeSample());
	const double GoodOffset = Controller->ServerClockOffset;
	const double GoodUncertainty = Controller->ClockUncertainty;
	const double GoodSampleTime = Controller->LastClockSampleLocal;
	Receive(3, 0.40, 99.82);
	TestEqual(TEXT("A subsequently queued sample cannot drag the good baseline back"), Controller->ServerClockOffset, GoodOffset);
	TestEqual(TEXT("Discarding a poor sample cannot inflate the accepted uncertainty"), Controller->ClockUncertainty, GoodUncertainty);
	TestEqual(TEXT("A rejected sample cannot pretend the time mapping was refreshed"), Controller->LastClockSampleLocal, GoodSampleTime);
	TestFalse(TEXT("Even a rejected reply consumes its own pending sample record"), Controller->PendingClockSamples.Contains(3));
	Controller->ClientReceiveClockSample_Implementation(2, 1.0);
	TestEqual(TEXT("A duplicate/unknown reply cannot rewrite the time mapping"), Controller->ServerClockOffset, GoodOffset);
	Controller->LastClockSampleLocal = FPlatformTime::Seconds() - 6.0;
	TestFalse(TEXT("An estimate older than five seconds is unavailable to new fire inputs"), Controller->HasServerTimeSample());
	Receive(4, 0.30, 100.05);
	TestTrue(TEXT("After expiry a changed route establishes a new baseline rather than smoothing toward the stale one"),
		FMath::Abs(Controller->ServerClockOffset - 100.05) < 0.003 && Controller->BestClockRoundTrip > 0.29);
	TestTrue(TEXT("The expired-route baseline uses its own RTT uncertainty without old-offset residue"),
		Controller->ClockUncertainty >= 0.149 && Controller->ClockUncertainty < 0.16);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueDynamicRoundTripTest,
	"fpstrue.Multiplayer.Clock.DynamicRoundTripAndFreshness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 用真实接收入口消费 pending 序号；延迟为合成值，不把这个回归称作实机弱网压测。
bool FFpstrueDynamicRoundTripTest::RunTest(const FString&)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	auto* Controller = World->SpawnActor<AfpstruePvpPlayerController>();
	if (!TestNotNull(TEXT("RTT regression uses the project controller"), Controller)) return false;
	Controller->SetRole(ROLE_SimulatedProxy);
	const auto Receive = [&](uint32 Sequence, double RoundTrip, double Offset = 100.0)
	{
		const double Now = FPlatformTime::Seconds();
		Controller->PendingClockSamples.Add(Sequence, Now - RoundTrip);
		Controller->ClientReceiveClockSample_Implementation(Sequence, Now + Offset - RoundTrip * 0.5);
	};
	TestFalse(TEXT("No RTT is advertised before a paired reply"), Controller->HasRoundTripTimeSample());
	TestEqual(TEXT("An absent RTT has no usable numeric value"), Controller->GetSmoothedRoundTripTimeSeconds(), 0.0);
	Receive(1, 0.1);
	TestTrue(TEXT("First reply initializes SRTT and half-RTT absolute deviation"),
		FMath::Abs(Controller->GetSmoothedRoundTripTimeSeconds() - 0.1) < 0.003
		&& FMath::Abs(Controller->GetRoundTripVariationSeconds() - 0.05) < 0.003);
	Receive(2, 0.1);
	const double StableOffset = Controller->ServerClockOffset;
	const double StableClockTime = Controller->LastClockSampleLocal;
	const double BeforeSrtt = Controller->SmoothedRoundTripTime;
	const double BeforeVariation = Controller->RoundTripVariation;
	Receive(3, 0.5, 99.8);
	TestTrue(TEXT("A congestion spike updates SRTT using one eighth of the residual"),
		FMath::Abs(Controller->GetSmoothedRoundTripTimeSeconds() - (BeforeSrtt * 0.875 + 0.5 * 0.125)) < 0.003);
	TestTrue(TEXT("RTTVAR uses the old SRTT absolute residual, not squared variance"),
		FMath::Abs(Controller->GetRoundTripVariationSeconds() - (BeforeVariation * 0.75 + FMath::Abs(0.5 - BeforeSrtt) * 0.25)) < 0.003);
	TestEqual(TEXT("A queued midpoint cannot drag the server-clock estimate"), Controller->ServerClockOffset, StableOffset);
	TestEqual(TEXT("An RTT-only update cannot refresh the accepted clock baseline"), Controller->LastClockSampleLocal, StableClockTime);
	TestTrue(TEXT("RTT remains fresh even when its midpoint is rejected for clock use"), Controller->HasRoundTripTimeSample());
	for (uint32 Sequence = 4; Sequence <= 35; ++Sequence) Receive(Sequence, 0.1);
	TestTrue(TEXT("SRTT and deviation settle when the route recovers"),
		FMath::Abs(Controller->GetSmoothedRoundTripTimeSeconds() - 0.1) < 0.003
		&& Controller->GetRoundTripVariationSeconds() < 0.004);
	Receive(40, 0.11);
	const double LatestSrtt = Controller->SmoothedRoundTripTime;
	const double LatestOffset = Controller->ServerClockOffset;
	const double LatestTime = Controller->LastRoundTripSampleLocal;
	Receive(39, 0.02, 200.0);
	TestEqual(TEXT("An out-of-order older reply cannot replace the latest RTT"), Controller->SmoothedRoundTripTime, LatestSrtt);
	TestEqual(TEXT("An out-of-order midpoint cannot replace the latest clock"), Controller->ServerClockOffset, LatestOffset);
	TestEqual(TEXT("An out-of-order reply cannot extend RTT freshness"), Controller->LastRoundTripSampleLocal, LatestTime);
	TestFalse(TEXT("A late reply still consumes its pending identity"), Controller->PendingClockSamples.Contains(39));
	Controller->ClientReceiveClockSample_Implementation(40, 1.0);
	Controller->ClientReceiveClockSample_Implementation(9999, 1.0);
	TestEqual(TEXT("Duplicate and unknown replies cannot affect RTT or freshness"), Controller->LastRoundTripSampleLocal, LatestTime);
	Controller->PendingClockSamples.Add(41, FPlatformTime::Seconds() - 0.1);
	Controller->ClientReceiveClockSample_Implementation(41, std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("An invalid server timestamp cannot update an RTT observation"), Controller->SmoothedRoundTripTime, LatestSrtt);
	TestFalse(TEXT("An invalid response also consumes its pending identity"), Controller->PendingClockSamples.Contains(41));
	Controller->LastRoundTripSampleLocal = Controller->LastClockSampleLocal = FPlatformTime::Seconds() - 6.0;
	TestFalse(TEXT("Lost replies eventually expire the RTT estimate"), Controller->HasRoundTripTimeSample());
	TestEqual(TEXT("Expired RTT is unavailable to consumers"), Controller->GetSmoothedRoundTripTimeSeconds(), 0.0);
	TestEqual(TEXT("Expired variation is unavailable to consumers"), Controller->GetRoundTripVariationSeconds(), 0.0);
	Receive(38, 0.01, 300.0);
	TestFalse(TEXT("Expiry cannot make an older pending reply fresh again"), Controller->HasRoundTripTimeSample());
	TestFalse(TEXT("The same older reply cannot reestablish a stale clock"), Controller->HasServerTimeSample());
	Receive(42, 2.2);
	TestFalse(TEXT("An excessively old sample cannot reestablish RTT"), Controller->HasRoundTripTimeSample());
	Receive(43, 0.3, 100.05);
	TestTrue(TEXT("A fresh paired reply restarts the new-route baseline without stale smoothing"),
		FMath::Abs(Controller->GetSmoothedRoundTripTimeSeconds() - 0.3) < 0.003
		&& FMath::Abs(Controller->GetRoundTripVariationSeconds() - 0.15) < 0.003
		&& FMath::Abs(Controller->ServerClockOffset - 100.05) < 0.003);
	Controller->ResetTimeSamples();
	Receive(MAX_uint32 - 1, 0.1);
	Receive(MAX_uint32, 0.2);
	Receive(0, 0.3);
	TestEqual(TEXT("The next uint32 sequence after wrap is accepted"), Controller->LastClockReplySequence, uint32(0));
	const double WrappedSrtt = Controller->SmoothedRoundTripTime;
	Receive(MAX_uint32, 0.01, 400.0);
	TestEqual(TEXT("A pre-wrap late reply cannot rewrite the post-wrap baseline"), Controller->SmoothedRoundTripTime, WrappedSrtt);
	Controller->ResetTimeSamples();
	Receive(1, .982);
	Receive(2, .280);
	TestTrue(TEXT("A substantially better startup baseline rebuilds both RTT and its standard half-RTT initial variation"),
		FMath::Abs(Controller->SmoothedRoundTripTime - .280) < .003
		&& FMath::Abs(Controller->RoundTripVariation - .140) < .003);
	Receive(3, .268);
	Receive(4, .284);
	Receive(5, .292);
	TestTrue(TEXT("Ordinary samples after the improved baseline converge without the abandoned startup residual"),
		FMath::Abs(Controller->SmoothedRoundTripTime - .2807890625) < .003
		&& FMath::Abs(Controller->RoundTripVariation - .064984375) < .003);
	const double BeforeCongestion = Controller->SmoothedRoundTripTime;
	const double BeforeCongestionVariation = Controller->RoundTripVariation;
	const double BeforeCongestionClock = Controller->ServerClockOffset;
	Receive(6, .9, 99.7);
	TestTrue(TEXT("A real congestion spike still increases the same RTT estimator"),
		Controller->SmoothedRoundTripTime > BeforeCongestion && Controller->RoundTripVariation > BeforeCongestionVariation);
	TestEqual(TEXT("Network congestion observation remains separate from clock quality selection"), Controller->ServerClockOffset, BeforeCongestionClock);
	const double CongestedSrtt = Controller->SmoothedRoundTripTime;
	Receive(7, .268);
	TestTrue(TEXT("Returning to an existing best baseline keeps normal congestion smoothing rather than resetting it away"),
		FMath::Abs(Controller->SmoothedRoundTripTime - (CongestedSrtt * .875 + .268 * .125)) < .003);
	Controller->ResetTimeSamples();
	TestFalse(TEXT("Leaving the controller clears RTT observations"), Controller->HasRoundTripTimeSample());
	TestFalse(TEXT("Leaving the controller clears the client clock baseline"), Controller->HasServerTimeSample());
	return true;
}

#endif
