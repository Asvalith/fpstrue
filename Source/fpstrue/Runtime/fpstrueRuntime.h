// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

// 运行时公共契约：碰撞通道、单次 Montage 播放身份、性能计数和只读启动覆盖。
// Character/武器/AI 通过这些小型契约协作；此模块不拥有生命、装备、对局或性能采集流程。
// 播放身份由 Game Thread 的动作调用方维护；只读参数不让 Worker 获取可变 UObject 状态。
// 注释导航：搜索“★【重要边界】”可串起关键流程；普通判空和常规计算不使用此标记。
// [状态提交/结算去重]：内部状态先落定，再通知外部；同一动作的结果只提交一次。
// [回调重入/异步身份]：外部调用可能同步改变状态，延迟回调必须证明自己仍属于原动作。
// [生命周期/资源所有权/资源归还]：退出时停止生产新工作，并按所有权释放登记、许可和采集资源。
// [线程交接/预算保护/配置契约/验收边界]：说明在哪个线程执行、哪些行为不能降级，以及配置和测试能证明什么。

/**
 * 项目级碰撞通道约定。
 *
 * 枚举槽位必须与 Config/DefaultEngine.ini 中的 DefaultChannelResponses 保持一致。
 * 业务代码使用语义名称而不是散落 ECC_GameTraceChannel3，后续调整槽位时只需修改这里。
 */
//定义玩家 Hitscan 查询通道：场景和活体命中体阻挡；角色移动胶囊、Trigger、UI 和尸体忽略。	
namespace FpstrueCollisionChannels
{
	// 玩家 Hitscan 查询通道：场景和活体命中体阻挡；角色移动胶囊、Trigger、UI 和尸体忽略。
	inline constexpr ECollisionChannel WeaponTrace = ECC_GameTraceChannel3;
}

class UAnimInstance;
class UAnimMontage;
class USkeletalMeshComponent;
struct FAnimNotifyEventReference;
struct FAnimMontageInstance;

/**
 * 【设计取舍】动作 ID 区分业务事务，Mesh/动画实例/播放 ID 区分同资产重播；多记少量身份换来旧 Notify 不能提交新动作。
 * 只持弱引用，不延长角色或资源生命；绑定失败由业务方取消/兜底，不从“当前 Montage”猜归属。
 */
struct FPSTRUE_API FFPActionPlayback
{
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UAnimInstance> AnimInstance;
	TWeakObjectPtr<UAnimMontage> Montage;
	int32 MontageInstanceId = INDEX_NONE;
	uint32 ActionId = 0;

	//
	// 记录一次动作的播放身份；同一事务只能绑定一次 Montage，旧播放不能覆盖新播放。
	// 
	// 只能绑定调用者明确指定的 Montage；同一事务已有不同播放时拒绝覆盖。
	FAnimMontageInstance* TryBind(uint32 InActionId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* PlayedMontage);
	// 校验调用者期望的动作编号，以及通知来源的 Mesh、AnimInstance 和 Montage 实例；旧播放不能提交当前动作。
	bool Matches(uint32 ExpectedActionId, USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const;
	// 按保存的 AnimInstance、实例 ID 和资产解析播放；返回值仅供当前调用使用，不能跨回调缓存。
	FAnimMontageInstance* GetBoundInstance() const;
	// 停止保存的实例，不用 Montage_Stop(Asset) 误停同资源的新一轮播放；可能同步触发外部回调。
	void Stop(float BlendOutSeconds = 0.1f) const;
	// 清空播放身份和弱引用；只忘记绑定，不主动停止动画。
	void Reset() { *this = FFPActionPlayback(); }
	// 检查是否保存了完整且仍有效的身份；不代表该实例此刻还在播放。
	bool IsSet() const;
};

// 运行时埋点声明：AI、生成和攻击模块写入，stat fpstruePerformance 与 Insights/测试器读取。
DECLARE_STATS_GROUP(TEXT("fpstrue Performance"), STATGROUP_fpstruePerformance, STATCAT_Advanced);

// AI 决策自身耗时与调用量，用来区分“决策函数昂贵”和“决策触发下游工作昂贵”。
DECLARE_CYCLE_STAT_EXTERN(TEXT("AI Decision Time"), STAT_fpstrueAIDecisionTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Decision Count"), STAT_fpstrueAIDecisionCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Move Request Count"), STAT_fpstrueAIMoveRequestCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Move Budget Rejected Count"), STAT_fpstrueAIMoveBudgetRejectedCount,
									  STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Attack Budget Rejected Count"), STAT_fpstrueAIAttackBudgetRejectedCount,
									  STATGROUP_fpstruePerformance, FPSTRUE_API);

// 波次生成耗时与数量，主要观察分帧生成是否消除了集中 Spawn 尖峰。
// 对应的旧单机统计已退役；保留原注释，生成仅由多人 GameMode 管理。

// 近战窗口的 Sweep 耗时、执行次数和返回命中数，用于证明碰撞查询是否值得继续优化。
DECLARE_CYCLE_STAT_EXTERN(TEXT("Attack Sweep Time"), STAT_fpstrueAttackSweepTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Attack Sweep Count"), STAT_fpstrueAttackSweepCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Sweep Returned Hit Count"), STAT_fpstrueSweepReturnedHitCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Attack Window Update Count"), STAT_fpstrueAttackWindowUpdateCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);


//消融开关只用于分离消费者成本；关闭碰撞、移动或攻击不能视为可交付玩法优化。
struct FFPEnemyRenderSignificancePolicy;

/**
 * 运行时性能策略的只读启动覆盖：玩法类只依赖此层，不依赖采集器。
 * 默认值不改变资产策略；旧 Benchmark* 命令行保留兼容，正式配置仍来自 Config/ 和玩法资产。
 * 诊断覆盖只在非 Shipping 版本解析；发布版本保持全部缺省，不接受命令行消融玩法。
 * 实验预设在 Tools/ExperimentProfiles，Runner 只负责阶段、采集与验收。
 */
struct FPSTRUE_API FFPRuntimeOptions
{
	// 首次使用时读取进程命令行，后续只返回同一份只读启动快照，不支持运行中改参。
	static const FFPRuntimeOptions& Get();
	// 仅覆盖显式提供的数值及关闭开关，不将诊断参数写回资产或 INI。
	void ApplyEnemySignificanceOverrides(FFPEnemyRenderSignificancePolicy& InOutPolicy) const;

	// 消融开关只用于分离消费者成本；关闭碰撞、移动或攻击不能视为可交付玩法优化。
	bool bDisableAttackSweep = false;
	bool bDisableEnemyPawnCollision = false;
	bool bDisablePathFollowingTick = false;
	bool bDisableCharacterMovementTick = false;
	bool bDisableSkeletalMeshTick = false;
	bool bDisableEnemySignificance = false;
	bool bDisableMovementTiering = false;
	bool bDisableShadowTiering = false;
	bool bDisableEnemyRayTracing = false;
	bool bDisableEnemyShadows = false;
	bool bDisableAnimationOptimizations = false;
	bool bDisableAIThrottling = false;
	bool bDisableEnemyRenderTiering = false;
	bool bDisableEnemySkeletalLOD = false;
	bool bDisableEnemyAnimationTiering = false;
	bool bDisableEnemyRayTracingTiering = false;
	bool bDisableEnemyAnimationSharing = false;
	bool bDisableActiveAttackerBudget = false;
	bool bDisableMoveToRequestBudget = false;

private:
	// 集中解析启动参数；缺省数值保持为空，区分“未指定”和“显式设置为零”。
	FFPRuntimeOptions();
	// 可选覆盖值保留资产默认配置，最终由协调器在启动时统一校验。
	TOptional<float> FrustumWeight;
	TOptional<float> ScreenCoverageWeight;
	TOptional<float> RecentFrustumWeight;
	TOptional<float> DistanceWeight;
	TOptional<float> ExpandedFrustumMargin;
	TOptional<float> RecentFrustumGraceSeconds;
	TOptional<float> ScreenRadiusForFullScore;
	TOptional<float> NearDistance;
	TOptional<float> FarDistance;
	TOptional<float> CombatPriorityGraceSeconds;
	TOptional<float> FullEnterThreshold;
	TOptional<float> FullExitThreshold;
	TOptional<float> ReducedEnterThreshold;
	TOptional<float> ReducedExitThreshold;
	TOptional<float> DemotionDelaySeconds;
	TOptional<float> MinimumTierHoldSeconds;
	TOptional<int32> MaxFullRenderEnemies;
	TOptional<int32> MaxShadowCastingEnemies;
	TOptional<float> ShadowMaxDistance;
	TOptional<int32> MaxRayTracingEnemies;
	TOptional<float> RayTracingMaxDistance;
	TOptional<int32> FullMinLOD;
	TOptional<int32> ReducedMinLOD;
	TOptional<int32> BackgroundMinLOD;
};
