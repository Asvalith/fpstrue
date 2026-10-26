// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"

// 仅供 GameInstance/GameMode 的连接实现共用；无状态，不新增管理器或另一条握手入口。
namespace FpstrueConnection
{
// 为恢复日志读取引擎网络身份；仅服务器可以分配 GUID，客户端只查询已知映射。
inline uint64 GetResumeNetGuid(const AActor* Actor)
{
	UNetDriver* Driver = IsValid(Actor) ? Actor->GetNetDriver() : nullptr;
	if (Driver == nullptr || !Driver->GuidCache.IsValid()) return 0;
	return (Actor->HasAuthority() ? Driver->GuidCache->GetOrAssignNetGUID(const_cast<AActor*>(Actor))
		: Driver->GuidCache->GetNetGUID(Actor)).ObjectId;
}

// 只校验票据编码格式；持有权、有效期和是否已占用仍由服务器保留表判断。
inline bool IsResumeToken(const FString& Token)
{
	if (Token.Len() != 64) return false;
	for (TCHAR Character : Token) if (!FChar::IsHexDigit(Character)) return false;
	return true;
}
}
