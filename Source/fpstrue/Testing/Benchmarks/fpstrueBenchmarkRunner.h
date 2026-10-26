// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;
class UGameInstance;

// GameState 只提供当前 World；显式 PvpBaseline 复用同一套采集和验证。
FPSTRUE_API void StartPvpBaseline(UWorld* World);

// Shipping intentionally ignores general command-line map/URL overrides. This
// explicit diagnostic bootstrap accepts only a local baseline peer; it neither
// exposes arbitrary travel nor changes the normal JoinPvp/reconnect protocol.
enum class EFPPvpBaselineBootstrap : uint8 { NotRequested, Started, Rejected };
// 返回是否启动或拒绝了本机基线连接；未请求时不改正式 GameInstance 的连接流程。
FPSTRUE_API EFPPvpBaselineBootstrap StartPvpBaselineClient(UGameInstance* Instance);
