#pragma once

#include "CoreMinimal.h"

// 项目级 CPU/计数器声明：AI、生成和攻击模块写入，stat fpstruePerformance 与 Insights 读取。
DECLARE_STATS_GROUP(TEXT("fpstrue Performance"), STATGROUP_fpstruePerformance, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("AI Decision Time"), STAT_fpstrueAIDecisionTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Decision Count"), STAT_fpstrueAIDecisionCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Move Request Count"), STAT_fpstrueAIMoveRequestCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Move Budget Rejected Count"), STAT_fpstrueAIMoveBudgetRejectedCount,
									  STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Attack Budget Rejected Count"), STAT_fpstrueAIAttackBudgetRejectedCount,
									  STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Wave Spawn Time"), STAT_fpstrueWaveSpawnTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Enemy Spawn Count"), STAT_fpstrueEnemySpawnCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Attack Sweep Time"), STAT_fpstrueAttackSweepTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Attack Sweep Count"), STAT_fpstrueAttackSweepCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Sweep Returned Hit Count"), STAT_fpstrueSweepReturnedHitCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Attack Window Update Count"), STAT_fpstrueAttackWindowUpdateCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);
