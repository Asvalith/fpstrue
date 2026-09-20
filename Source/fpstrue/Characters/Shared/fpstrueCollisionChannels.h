// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/EngineTypes.h"

/**
 * 项目级碰撞通道约定。
 *
 * 枚举槽位必须与 Config/DefaultEngine.ini 中的 DefaultChannelResponses 保持一致。
 * 业务代码使用语义名称而不是散落 ECC_GameTraceChannel3，后续调整槽位时只需修改这里。
 */
namespace FpstrueCollisionChannels
{
	// 玩家 Hitscan 专用查询通道：场景和敌人骨骼阻挡，角色胶囊、Trigger、UI 和尸体忽略。
	inline constexpr ECollisionChannel WeaponTrace = ECC_GameTraceChannel3;
}
