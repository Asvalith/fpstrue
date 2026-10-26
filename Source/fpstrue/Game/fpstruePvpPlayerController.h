// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "fpstruePvpPlayerController.generated.h"

class AfpstrueCharacter;
class AfpstruePvpGameMode;
class UfpstrueMultiplayerGameInstance;
struct FFPShotRequest;

// 拥有者网络通道：转交握手、离场和时钟采样；真实恢复裁决仍由 GameMode 完成。

// 拥有者时钟只用于给射击意图标注估计时间；时间和命中仍由服务器校验，不构成客户端裁决权限。
UCLASS()
class FPSTRUE_API AfpstruePvpPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// 使用往返采样建立本机单调时钟与服务器世界时间的对应；服务端直接返回自己的世界时间。
	double GetEstimatedServerTime() const;
	bool HasServerTimeSample() const;
	// 本地探测的应用往返时间，包含两端调度；过期时指标返回 0，调用者先检查是否有效。
	// 这里只提供观测，不替代 UE 传输重发，也不用于扩大服务器命中回溯权限。
	bool HasRoundTripTimeSample() const;
	double GetSmoothedRoundTripTimeSeconds() const;
	// RTTVAR 是 |本次 RTT - 旧 SRTT| 的指数平滑，单位秒，不是统计学方差。
	double GetRoundTripVariationSeconds() const;
	// 开发版联机验收先完成原玩家战斗，再进入敌人阶段；Shipping 不执行自动场景。
	UFUNCTION(Server, Reliable) void ServerBeginPveSmoke();
	UFUNCTION(Client, Reliable) void ClientAllowPveSmokeCounterattack();
	// 供开发验收输入驱动器读取服务器已允许的反击阶段，不代表正式战斗权限。
	bool IsPveSmokeCounterattackReady() const { return bSmokePveCounterattackReady; }
	double GetClockUncertaintySeconds() const;
	// 网络断开会先进入 PawnLeavingGame，再进入 GameMode Logout；不能在 Logout 才尝试保留已经销毁的 Pawn。
	virtual void PawnLeavingGame() override;
	UFUNCTION(Server, Reliable) void ServerSubmitConnectionToken(const FString& Token);
	UFUNCTION(Server, Reliable) void ServerLeavePvp();
	// 仅开发实机验收读取服务器导航路线；客户端仍通过正常 CMC 输入行走，不传送 Pawn。
	const TArray<FVector>& GetNormalVisualRoute() const { return NormalVisualRoute; }
	// 路线接收版本用于输入驱动器识别新规划，不参与移动复制。
	int32 GetNormalVisualRouteRevision() const { return NormalVisualRouteRevision; }
	// 只返回开发验收路线数据；不移动角色、不生成敌人，也不接纳客户端伤害或目标坐标。
	UFUNCTION(Server, Reliable) void ServerRequestNormalVisualEnemyRoute();
	// 显式容量验收由服务器在人数/人口就绪后统一给出采样起点，不改变正常玩法时钟。
	UFUNCTION(Client, Reliable) void ClientBeginNetworkCapacity(double CaptureStartServerTime);
	double GetNetworkCapacityStartServerTime() const { return NetworkCapacityStartServerTime; }
#if !UE_BUILD_SHIPPING
	// 只观察移动命中专项的第一发输入与权威回执；不参与命中、伤害或移动裁决。
	void SetSmokeMovingHitTarget(AfpstrueCharacter* Target);
	bool GetSmokeMovingHitAim(FVector& OutPoint, FName& OutBone, uint8& OutShapeType) const;
	void RecordSmokeMovingShotInput(const FFPShotRequest& Request);
	void RecordSmokeShotVerdict(AActor* Weapon, uint32 Generation, uint32 Session, uint32 ShotId, bool bAccepted);
	bool LogSmokeMovingHitIfReplicated(AfpstrueCharacter* Target, float InitialHealth) const;
#endif

protected:
	virtual void BeginPlayingState() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend class AfpstruePvpGameMode;
	friend class UfpstrueMultiplayerGameInstance;
	friend class FFpstrueMultiplayerResumeSessionTest;
	friend class FFpstrueMultiplayerSpawnReadinessTest;
	friend class FFpstrueClockQualityRebaseTest;
	friend class FFpstrueDynamicRoundTripTest;
	friend class FFpstrueWeaponShotTransportTest;
	UFUNCTION(Client, Reliable) void ClientBeginConnectionHandshake();
	UFUNCTION(Client, Reliable) void ClientConnectionAccepted(const FString& Token, int32 ResumeId, bool bRestored);
	UFUNCTION(Client, Reliable) void ClientConnectionRejected(const FString& Reason);
	UFUNCTION(Client, Reliable) void ClientNormalVisualRoute(const TArray<FVector>& Points);
	TArray<FVector> NormalVisualRoute;
	int32 NormalVisualRouteRevision = 0;
	bool bNormalFullRouteRequested = false;
	FString AssignedRejoinToken;
	bool bConnectionHandshakeComplete = false;
	bool bIntentionalLeave = false;
	bool bSmokePveCounterattackReady = false;
	double NetworkCapacityStartServerTime = 0.0;
	void ResetTimeSamples();
	void RequestClockSample();
	UFUNCTION(Server, Unreliable) void ServerRequestClockSample(uint32 Sequence);
	UFUNCTION(Client, Unreliable) void ClientReceiveClockSample(uint32 Sequence, double ServerTime);
	FTimerHandle ClockSampleTimer;
	TMap<uint32, double> PendingClockSamples;
	uint32 ClockSequence = 0;
	uint32 LastClockReplySequence = 0;
	bool bHasClockReplySequence = false;
	double SmoothedRoundTripTime = 0.0;
	double RoundTripVariation = 0.0;
	double LastRoundTripSampleLocal = -1.0;
	double ServerClockOffset = 0.0;
	double LastClockSampleLocal = -1.0;
	double BestClockRoundTrip = TNumericLimits<double>::Max();
	double ClockUncertainty = 1.0;
	double LastServerClockRequest = -1.0;
#if !UE_BUILD_SHIPPING
	TWeakObjectPtr<AfpstrueCharacter> SmokeMovingHitTarget;
	TWeakObjectPtr<AActor> SmokeMovingHitWeapon;
	uint32 SmokeMovingHitGeneration = 0;
	uint32 SmokeMovingHitSession = 0;
	uint32 SmokeMovingHitShotId = 0;
	bool bSmokeMovingFirstShotAccepted = false;
	bool bSmokeMovingFirstNativeView = false;
	void SampleSmokeMovement();
	// 有画面门禁等待真实相机、装备、HUD 和敌人副本，再申请引擎视口截图。
	void CheckVisualRecovery();
	void HandleVisualScreenshotProcessed();
	FTimerHandle VisualRecoveryTimer;
	FDelegateHandle VisualScreenshotHandle;
	FString VisualScreenshotPath;
	int32 VisualResumeId = INDEX_NONE;
	bool bVisualRestored = false;
	bool bVisualCaptureRequested = false;
	double VisualReadyAt = 0.0;
	FTimerHandle SmokeMovementTimer;
	TMap<TWeakObjectPtr<AfpstrueCharacter>, FVector> SmokeRemoteOrigins;
	bool bSmokeRemoteMovementReported = false;
#endif
};
