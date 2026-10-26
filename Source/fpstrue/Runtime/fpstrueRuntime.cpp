// Copyright Epic Games, Inc. All Rights Reserved.

#include "Runtime/fpstrueRuntime.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Components/SkeletalMeshComponent.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// 播放身份与启动配置工具；调用方保持动作状态，工具只解析明确身份或读取配置，不发网络消息。
// 
//发身份
// 【设计取舍】同步绑定只查调用者明确指定资产的活动实例；异步 Notify/结束回调只认已保存的实例 ID。
// 同资产重播产生新 ID，不能在旧回调里重新查询“当前 Montage”，把它冒领为新一轮播放。
FAnimMontageInstance* FFPActionPlayback::TryBind(uint32 InActionId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* PlayedMontage)
{
	// 先确认资源和当前活动实例，再决定复用已有绑定还是首次记录身份。
	if (InActionId == 0 || !IsValid(PlaybackMesh) || !IsValid(PlayedMontage)) return nullptr;
	UAnimInstance* Anim = PlaybackMesh->GetAnimInstance();
	FAnimMontageInstance* Instance = Anim != nullptr ? Anim->GetActiveInstanceForMontage(PlayedMontage) : nullptr;
	if (Instance == nullptr || !Instance->IsActive()) return nullptr;
	if (MontageInstanceId != INDEX_NONE)
	{
		return ActionId == InActionId && Mesh.Get() == PlaybackMesh && AnimInstance.Get() == Anim
			&& Montage.Get() == PlayedMontage && MontageInstanceId == Instance->GetInstanceID() ? Instance : nullptr;
	}
	// 首次绑定保存弱引用与实例编号，不延长角色、动画实例或资源的生命周期。
	Mesh = PlaybackMesh;
	AnimInstance = Anim;
	Montage = PlayedMontage;
	MontageInstanceId = Instance->GetInstanceID();
	ActionId = InActionId;
	return Instance;
}

//查身份
// 将动画通知的来源与本次绑定比较，拒绝旧动作、换过 AnimInstance 或不同 Mesh 的通知。
bool FFPActionPlayback::Matches(uint32 ExpectedActionId, USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const
{
	// ★【重要边界】[异步身份] 动作编号与具体播放实例共同匹配；只认 Montage 资产会混入同资源的旧通知。
	const auto* Context = EventReference.GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>();
	return IsSet() && ActionId == ExpectedActionId && Mesh.Get() == PlaybackMesh
		&& AnimInstance.Get() == PlaybackMesh->GetAnimInstance()
		&& Context != nullptr && MontageInstanceId == Context->MontageInstanceID;
}


//按保存的实例编号重新解析播放；资产相同但实例已替换时也返回空指针。
FAnimMontageInstance* FFPActionPlayback::GetBoundInstance() const
{
	UAnimInstance* Anim = AnimInstance.Get();
	FAnimMontageInstance* Instance = Anim != nullptr ? Anim->GetMontageInstanceForID(MontageInstanceId) : nullptr;
	return Instance != nullptr && Instance->Montage == Montage.Get() ? Instance : nullptr;
}

//仅停止本次绑定的活动实例；混出时间限制为有限非负值，停止可能同步触发结束委托。
void FFPActionPlayback::Stop(float BlendOutSeconds) const
{
	if (FAnimMontageInstance* Instance = GetBoundInstance(); Instance != nullptr && Instance->IsActive())
	{
		Instance->Stop(FAlphaBlend(FMath::IsFinite(BlendOutSeconds) ? FMath::Max(0.0f, BlendOutSeconds) : 0.1f), true);
	}
}

//判断身份字段和弱引用是否完整有效，不遍历动画列表，也不推断业务动作状态。
bool FFPActionPlayback::IsSet() const
{
	return ActionId != 0 && MontageInstanceId != INDEX_NONE && Mesh.IsValid() && AnimInstance.IsValid() && Montage.IsValid();
}

namespace
{
// 从命令行读取一个可选数值；未提供时保持为空，避免覆盖编辑器配置。
template <typename TValue> TOptional<TValue> ParseOptionalValue(const TCHAR* CommandLine, const TCHAR* Key)
{
	TValue Value{};
	if (FParse::Value(CommandLine, Key, Value))
	{
		return Value;
	}

	return TOptional<TValue>();
}

// 只在命令行确实提供值时覆盖目标策略字段。
template <typename TValue> void ApplyOptionalValue(const TOptional<TValue>& Override, TValue& OutValue)
{
	if (Override.IsSet())
	{
		OutValue = Override.GetValue();
	}
}
} // namespace

// 函数静态对象只解析一次命令行，玩法消费者共享同一份只读启动配置。
const FFPRuntimeOptions& FFPRuntimeOptions::Get()
{
	static const FFPRuntimeOptions Options;
	return Options;
}

// 解析诊断开关与可选预算参数，不加载玩法资产，也不创建性能采集器。
FFPRuntimeOptions::FFPRuntimeOptions()
{
#if !UE_BUILD_SHIPPING
	// ★【重要边界】[发布隔离] Shipping 只使用资产/INI 的正式策略，不接纳关闭碰撞、攻击或预算的实验参数。
	const TCHAR* CommandLine = FCommandLine::Get();
	bDisableAttackSweep = FParse::Param(CommandLine, TEXT("BenchmarkDisableAttackSweep"));
	bDisableEnemyPawnCollision = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyPawnCollision"));
	bDisablePathFollowingTick = FParse::Param(CommandLine, TEXT("BenchmarkDisablePathFollowingTick"));
	bDisableCharacterMovementTick = FParse::Param(CommandLine, TEXT("BenchmarkDisableCharacterMovementTick"));
	bDisableSkeletalMeshTick = FParse::Param(CommandLine, TEXT("BenchmarkDisableSkeletalMeshTick"));
	bDisableEnemySignificance = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemySignificance"));
	bDisableMovementTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableMovementTiering"));
	bDisableShadowTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableShadowTiering"));
	bDisableEnemyRayTracing = FParse::Param(CommandLine, TEXT("BenchmarkEnemyRayTracingOff"));
	bDisableEnemyShadows = FParse::Param(CommandLine, TEXT("BenchmarkEnemyShadowsOff"));
	bDisableAnimationOptimizations = FParse::Param(CommandLine, TEXT("BenchmarkDisableAnimationOptimizations"));
	bDisableAIThrottling = FParse::Param(CommandLine, TEXT("BenchmarkDisableAIThrottling"));
	bDisableEnemyRenderTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyRenderTiering"));
	bDisableEnemySkeletalLOD = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemySkeletalLOD"));
	bDisableEnemyAnimationTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyAnimationTiering"));
	bDisableEnemyRayTracingTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyRayTracingTiering"));
	bDisableEnemyAnimationSharing = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyAnimationSharing"));
	bDisableActiveAttackerBudget = FParse::Param(CommandLine, TEXT("BenchmarkDisableActiveAttackerBudget"));
	bDisableMoveToRequestBudget = FParse::Param(CommandLine, TEXT("BenchmarkDisableMoveToRequestBudget"));

	// Significance 调参项保留为 TOptional：未传参时继续使用 GameMode/蓝图配置，显式传 0 时则确实覆盖为 0。
	FrustumWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFrustumWeight="));
	ScreenCoverageWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigScreenWeight="));
	RecentFrustumWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigRecentWeight="));
	DistanceWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigDistanceWeight="));
	ExpandedFrustumMargin = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFrustumMargin="));
	RecentFrustumGraceSeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigRecentGrace="));
	ScreenRadiusForFullScore = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigScreenRadius="));
	NearDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigNearDistance="));
	FarDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFarDistance="));
	CombatPriorityGraceSeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigCombatGrace="));
	FullEnterThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFullEnter="));
	FullExitThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFullExit="));
	ReducedEnterThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigReducedEnter="));
	ReducedExitThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigReducedExit="));
	DemotionDelaySeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigDemotionDelay="));
	MinimumTierHoldSeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigMinimumHold="));
	MaxFullRenderEnemies = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigFullBudget="));
	MaxShadowCastingEnemies = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigShadowBudget="));
	ShadowMaxDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigShadowDistance="));
	MaxRayTracingEnemies = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigRayTracingBudget="));
	RayTracingMaxDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigRayTracingDistance="));
	FullMinLOD = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigFullMinLOD="));
	ReducedMinLOD = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigReducedMinLOD="));
	BackgroundMinLOD = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigBackgroundMinLOD="));
#endif
}

// 把本进程显式启动参数投影到渲染策略；未设置项和资产中已禁用的功能保持原样。
void FFPRuntimeOptions::ApplyEnemySignificanceOverrides(FFPEnemyRenderSignificancePolicy& InOutPolicy) const
{
	// ★【重要边界】[配置契约] 启动诊断只覆盖本进程显式参数，不写回资产/INI；关闭碰撞、移动或攻击的消融结果不能当正式玩法收益。
	ApplyOptionalValue(FrustumWeight, InOutPolicy.FrustumWeight);
	ApplyOptionalValue(ScreenCoverageWeight, InOutPolicy.ScreenCoverageWeight);
	ApplyOptionalValue(RecentFrustumWeight, InOutPolicy.RecentFrustumWeight);
	ApplyOptionalValue(DistanceWeight, InOutPolicy.DistanceWeight);
	ApplyOptionalValue(ExpandedFrustumMargin, InOutPolicy.ExpandedFrustumMargin);
	ApplyOptionalValue(RecentFrustumGraceSeconds, InOutPolicy.RecentFrustumGraceSeconds);
	ApplyOptionalValue(ScreenRadiusForFullScore, InOutPolicy.ScreenRadiusForFullScore);
	ApplyOptionalValue(NearDistance, InOutPolicy.NearDistance);
	ApplyOptionalValue(FarDistance, InOutPolicy.FarDistance);
	ApplyOptionalValue(CombatPriorityGraceSeconds, InOutPolicy.CombatPriorityGraceSeconds);
	ApplyOptionalValue(FullEnterThreshold, InOutPolicy.FullEnterThreshold);
	ApplyOptionalValue(FullExitThreshold, InOutPolicy.FullExitThreshold);
	ApplyOptionalValue(ReducedEnterThreshold, InOutPolicy.ReducedEnterThreshold);
	ApplyOptionalValue(ReducedExitThreshold, InOutPolicy.ReducedExitThreshold);
	ApplyOptionalValue(DemotionDelaySeconds, InOutPolicy.DemotionDelaySeconds);
	ApplyOptionalValue(MinimumTierHoldSeconds, InOutPolicy.MinimumTierHoldSeconds);
	ApplyOptionalValue(MaxFullRenderEnemies, InOutPolicy.MaxFullRenderEnemies);
	ApplyOptionalValue(MaxShadowCastingEnemies, InOutPolicy.MaxShadowCastingEnemies);
	ApplyOptionalValue(ShadowMaxDistance, InOutPolicy.ShadowMaxDistance);
	ApplyOptionalValue(MaxRayTracingEnemies, InOutPolicy.MaxRayTracingEnemies);
	ApplyOptionalValue(RayTracingMaxDistance, InOutPolicy.RayTracingMaxDistance);
	ApplyOptionalValue(FullMinLOD, InOutPolicy.FullMinLOD);
	ApplyOptionalValue(ReducedMinLOD, InOutPolicy.ReducedMinLOD);
	ApplyOptionalValue(BackgroundMinLOD, InOutPolicy.BackgroundMinLOD);

	// 消融只允许关闭已有策略，未传参时保留资产原值，绝不反向打开原本禁用的功能。
	InOutPolicy.bEnableRenderTiering &= !bDisableEnemyRenderTiering;
	InOutPolicy.bEnableSkeletalLOD &= !bDisableEnemySkeletalLOD;
	InOutPolicy.bEnableAnimationTickTiering &= !bDisableEnemyAnimationTiering;
	InOutPolicy.bEnableShadowBudget &= !bDisableShadowTiering;
	InOutPolicy.bEnableRayTracingBudget &= !bDisableEnemyRayTracingTiering;
}
