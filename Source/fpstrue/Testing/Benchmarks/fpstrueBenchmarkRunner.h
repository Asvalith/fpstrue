// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Ticker.h"
#include "fpstrueBenchmarkRunner.generated.h"

class AfpstrueGameMode;
class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class APlayerController;
class UInputComponent;
struct FFPBenchmarkCsvCapture;

enum class EFPBenchmarkPhase : uint8
{
	Idle, Queued, Preparing, WarmingUp, StartingCapture, Capturing, Flushing, Quarantined, Finished
};

enum class EFPBenchmarkResult : uint8
{
	None, Completed, InvalidSample, Failed, Cancelled, Unsupported
};

/**
 * 一进程一次的性能采集：排队开局 -> 场景准备 -> 预热 -> 确认启动 -> 采集 -> 验盘。
 * 准备/预热依赖 World Timer；Profiler 推进与超时使用 CoreTicker，世界暂停不能阻塞清理。
 * 写盘超时进入 Quarantined，只判业务失败，不放弃仍可能迟到启动的进程级 CSV 租约。
 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueBenchmarkRunner : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建不参与逐帧 Tick 的 Benchmark Runner。
	UfpstrueBenchmarkRunner();

	// GameMode 只负责决定何时挂接 Runner，采集状态和计时器全部由 Runner 自己维护。
	void StartIfRequested(AfpstrueGameMode* InGameMode);
	// 撤销阶段回调、交还场景控制，再异步清理本次拥有的 Profiler。
	void Cancel();

protected:
	// 停止对象回调；尚未完成的 CSV 清理交给不持有 UObject 的进程租约。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend class FFpstrueBenchmarkLifecycleTest;
	friend class FFpstrueBenchmarkValidationTest;
	friend class FFpstrueBenchmarkLateCsvTest;
	friend class FFpstrueBenchmarkWatchdogTest;
	using FStageCallback = void (UfpstrueBenchmarkRunner::*)();
	// 每次回调同时核对 RunId 与阶段；弱绑定只处理对象销毁，不能代替业务取消。
	void ScheduleStage(EFPBenchmarkPhase ExpectedPhase, FStageCallback Callback, float Delay, bool bLoop = false);
	void ClearStageTimers();
	void Finish(EFPBenchmarkResult Result, const TCHAR* Reason);
	void ReleaseSceneOwnership();
	void PollCaptureStart();
	void PollCapture();
	void PollArtifacts();
	// 正式实验只允许进程内一次；下一组 A/B 由外部脚本启动新进程，不能复用已改变的 World。
	bool CanStartFreshRun() const;
	static bool HasOutstandingCsvLease();
	// 仅隔离 CSV 的提交时机，生产仍调用真实 FCsvProfiler；故障测试可延迟提交而不伪造完成。
	void RequestCsvCapture(TFunctionRef<void(const FString&)> SubmitStart);
	void StartWatchdog();
	bool TickWatchdog();
	static uint8 GetResultExitCode(EFPBenchmarkResult InResult);
	struct FEnemySnapshot;
	FEnemySnapshot ReadEnemySnapshot(bool bDetailed) const;
	void LogConsumerSnapshot(const FEnemySnapshot& Snapshot) const;
	static bool IsAvoidanceConfigurationValid(int32 Enemies, int32 RVO, int32 Crowd, int32 ValidCrowd);
	UFUNCTION()
	void HandleEnemyCountChanged(int32 AliveEnemies);
	// 启动固定场景并进入等待阶段。
	void BeginBenchmark();
	// 等待敌人生成完成，再进入预热。
	void WaitForBenchmarkReady();
	// 预热完成后请求 CSV/Insights，收到实际启动确认才计入采集时长。
	void StartCapture();
	// 把命令行消融开关应用到当前敌人和 AI 组件。
	void ApplyDiagnosticOverrides();
	// 低频验证人数/输入/消融状态；起始详细日志复用同一采样，避免再次遍历消费者。
	bool ValidateBenchmarkState(const TCHAR* ValidationPhase, bool bLogSuccess = true, bool bLogConsumers = false) const;
	// 正常结束和取消共用幂等的采集收尾；不处理阶段 Timer、输入或成功日志。
	void StopActiveProfilers();
	// 在启用自动退出时关闭测试进程。
	void ExitBenchmark();

	TWeakObjectPtr<AfpstrueGameMode> GameMode;
	EFPBenchmarkPhase Phase = EFPBenchmarkPhase::Idle;
	EFPBenchmarkResult Result = EFPBenchmarkResult::None;
	uint64 RunId = 0;
	double PhaseDeadline = 0.0;
	bool bEndingPlay = false;
	bool bPopulationChanged = false;
	int32 ExpectedEnemyCount = INDEX_NONE;
	int32 PreviousDurationOverride = 0;
	int32 PreviousEnemyCountOverride = INDEX_NONE;
	bool bOwnsDurationOverride = false;
	TSharedPtr<FFPBenchmarkCsvCapture> CsvCapture;
	FString TraceOutputPath;
	bool bTraceActive = false;
	bool bTraceStopRequested = false;
	bool bTraceRegionActive = false;
	// 自动测试期间屏蔽真实键鼠输入，并保存固定视点用于拒绝发生位姿漂移的样本。
	bool bBenchmarkInputLocked = false;
	TWeakObjectPtr<APlayerController> LockedController;
	TWeakObjectPtr<AfpstrueCharacter> LockedPlayer;
	TWeakObjectPtr<UInputComponent> RemovedInputComponent;
	TWeakObjectPtr<AActor> PreviousViewTarget;
	FRotator PreviousControlRotation = FRotator::ZeroRotator;
	FVector BenchmarkPlayerLocation = FVector::ZeroVector;
	FRotator BenchmarkControlRotation = FRotator::ZeroRotator;

	// 只缓存本 Runner 真正修改的消费者，取消时恢复，不把整份测试策略写回正常玩法。
	struct FDiagnosticState
	{
		TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy;
		TWeakObjectPtr<UActorComponent> PathFollowing;
		bool bMovementTick = false;
		bool bMeshTick = false;
		bool bPathFollowingTick = false;
		uint8 PawnResponse = 0;
	};
	TArray<FDiagnosticState> DiagnosticStates;
	// 包括 SetTimerForNextTick 的返回句柄，所有阶段都能被 Cancel 撤销。
	FTimerHandle StageTimerHandle;
	FTSTicker::FDelegateHandle WatchdogHandle;
};
