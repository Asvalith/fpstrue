// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Containers/Ticker.h"
#include "fpstrueMultiplayerGameInstance.generated.h"

class AfpstrueCharacter;
class AfpstruePvpPlayerController;
class AfpstruePvpPlayerState;
class UNetDriver;

// 跨世界连接流程：发起旅行、等待就绪、有限重试和本地票据缓存；不裁决伤害或恢复角色。

UENUM(BlueprintType)
enum class EFPConnectionState : uint8
{
	Idle, Connecting, WaitingReady, Connected, RetryWait, Leaving, Failed
};

// 跨世界的本地连接状态机；使用引擎地址直连，不代替在线账号认证或服务器玩法权威。
UCLASS(Config = Game)
class FPSTRUE_API UfpstrueMultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;
	// 仅显式回环基线接管 Shipping 启动 URL，普通启动与旅行仍由引擎负责。
	virtual void StartGameInstance() override;
	// 使用配置的PvP地图或当前GameDefaultMap开启监听服务器。
	UFUNCTION(Exec)
	void HostPvp();
	// 当前本地玩家连接IP或主机名，可附带端口。
	UFUNCTION(Exec)
	void JoinPvp(const FString& Address);
	UFUNCTION(Exec) void CancelPvpConnection();
	UFUNCTION(Exec) void LeavePvp();
	UFUNCTION(Exec) void RetryPvpConnection();
	// UI 读取本地连接阶段；服务端接纳后仍可能等待必要复制数据到达。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") EFPConnectionState GetConnectionState() const { return ConnectionState; }
	// UI 读取最近一次连接/旅行错误，不含恢复票据。
	UFUNCTION(BlueprintPure, Category = "Multiplayer|Connection") FString GetConnectionError() const { return LastConnectionError; }
	void BeginConnectionHandshake(AfpstruePvpPlayerController* Player);
	void AcceptConnectionToken(AfpstruePvpPlayerController* Player, const FString& Token, int32 ResumeId, bool bRestored);
	void RejectConnection(const FString& Reason);
	// 本次服务器接纳是否恢复了旧身份；显示/开发输入读取结果，不据此授权玩法。
	bool IsRestoredConnection() const { return bRestoredConnection; }

protected:
	UPROPERTY(Config, EditDefaultsOnly, BlueprintReadOnly, Category = "Multiplayer")
	FString PvpMap;

private:
	friend class AfpstruePvpPlayerController;
	friend class FFpstrueMultiplayerConnectionRevisionTest;
	friend class FFpstrueMultiplayerResumeSessionTest;
	friend class FFpstrueMultiplayerSpawnReadinessTest;
	friend class FFpstrueLiveReconnectRetryTest;
	// 空手是合法就绪状态；有装备时才要求其持有者关系已经解析，能力与生命仍必须就绪。
	static bool IsPawnStateReady(const AfpstrueCharacter* Pawn, const AfpstruePvpPlayerState* State);
	// 地图配置读取沿用引擎设置；业务入口仍只接纳合法包路径，不传额外 URL 选项。
	FString ResolvePvpMapPackage() const;
	bool TickConnection(float DeltaSeconds);
	void StartClientTravel(uint32 Revision);
	void SetConnectionState(EFPConnectionState State);
	void HandleConnectionFailure(double MinimumRetryDelay = 0.0);
	void ReturnToLocalWorld();
	bool OwnsNetworkFailure(UWorld* World, class UNetDriver* Driver) const;
	void HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Failure, const FString& Error);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type Failure, const FString& Error);
	void HandlePostLoadMap(UWorld* World);
	void SaveResumeCache();
	void LoadResumeCache();
	void ClearResumeCache();
	EFPConnectionState ConnectionState = EFPConnectionState::Idle;
	FString LastConnectionError;
	FString ServerAddress;
	FString ResumeServerAddress;
	FString ResumeToken;
	FString ResumeCacheFile;
	TWeakObjectPtr<AfpstruePvpPlayerController> ConnectingController;
	TWeakObjectPtr<UNetDriver> AttemptNetDriver;
	uint32 OperationRevision = 0;
	uint32 ScheduledRetryRevision = 0;
	int32 RetryCount = 0;
	int32 ResumeId = INDEX_NONE;
	bool bRestoredConnection = false;
	bool bAutoReconnect = false;
	bool bReturningToLocalWorld = false;
	// 网络失败广播返回后引擎还会写入 ?closed；下一次 Ticker 才提交本项目的本地旅行。
	bool bPendingLocalReturn = false;
	double AttemptDeadline = 0.0;
	double RetryAt = 0.0;
	FTSTicker::FDelegateHandle ConnectionTicker;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoadHandle;
#if !UE_BUILD_SHIPPING
	bool bLiveReconnectFailureObserved = false;
	int32 LiveReconnectInitialResumeId = INDEX_NONE;
#endif
};
