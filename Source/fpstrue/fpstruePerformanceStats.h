#pragma once

#include "CoreMinimal.h"

DECLARE_STATS_GROUP(TEXT("fpstrue Performance"), STATGROUP_fpstruePerformance, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(
	TEXT("AI Decision Time"),
	STAT_fpstrueAIDecisionTime,
	STATGROUP_fpstruePerformance,
	FPSTRUE_API
);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(
	TEXT("AI Decision Count"),
	STAT_fpstrueAIDecisionCount,
	STATGROUP_fpstruePerformance,
	FPSTRUE_API
);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(
	TEXT("AI Move Request Count"),
	STAT_fpstrueAIMoveRequestCount,
	STATGROUP_fpstruePerformance,
	FPSTRUE_API
);

DECLARE_CYCLE_STAT_EXTERN(
	TEXT("Wave Spawn Time"),
	STAT_fpstrueWaveSpawnTime,
	STATGROUP_fpstruePerformance,
	FPSTRUE_API
);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(
	TEXT("Enemy Spawn Count"),
	STAT_fpstrueEnemySpawnCount,
	STATGROUP_fpstruePerformance,
	FPSTRUE_API
);
