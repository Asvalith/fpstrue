// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Ticker.h"
#include "Runtime/fpstrueRuntimeOptions.h"
#include "fpstrueBenchmarkRunner.generated.h"

class AfpstrueGameMode;
class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class APlayerController;
class UInputComponent;
struct FFPBenchmarkCsvCapture;

// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * Benchmark 采集配置：人数、时长、输出及采集开关。
 * 外部实验预设维护在 Tools/ExperimentProfiles；脚本生成命令行，本类只解析一次。
 * Runner 管理采集流程；玩法只消费 Runtime/fpstrueRuntimeOptions，不依赖本文件。
 * 正式玩法默认配置仍由 Config/ 和玩法资产维护，不在这里修改。
 */
struct FPSTRUE_API FFPBenchmarkConfig : public FFPRuntimeOptions
{
public:
	// 命令行是进程级只读输入，集中解析一次，避免 Gameplay 类各自维护同一组开关。
	static const FFPBenchmarkConfig& Get();

	// 判断命令行是否指定了固定敌人数。
	bool HasEnemyCountOverride() const { return bHasEnemyCountOverride; }
	// 判断自动测试是否显式要求提高玩家最大生命值。
	bool HasPlayerHealthOverride() const { return bHasPlayerHealthOverride; }

	bool bAutoBenchmark = false;
	bool bCollectTextureStats = false;
	bool bTakeScreenshot = false;
	bool bAutoQuit = false;

	int32 EnemyCount = 0;
	int32 Seed = 1337;
	float PlayerHealth = 0.0f;
	float WarmupSeconds = 10.0f;
	float DurationSeconds = 30.0f;
	FString TraceFile;

private:
	// 首次访问单例时解析一次命令行。
	FFPBenchmarkConfig();

	bool bHasEnemyCountOverride = false;
	bool bHasPlayerHealthOverride = false;
};

// 采集阶段与业务结果分开：Flushing 等待写盘，Quarantined 表示失败后的资源隔离，不是成功。
enum class EFPBenchmarkPhase : uint8
{
	Idle, Queued, Preparing, WarmingUp, StartingCapture, Capturing, Flushing, Quarantined, Finished
};

// Completed 仍须经过产物存在和写盘确认；外部脚本还会继续校验样本有效性。
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
	// 清除阶段 Timer 并推进运行编号，让已经进入调用栈的旧阶段回调也失效。
	void ClearStageTimers();
	// 记录结果并进入异步写盘收尾，只有验盘结束后才发布最终成功。
	void Finish(EFPBenchmarkResult Result, const TCHAR* Reason);
	// 撤销本 Runner 取得的输入屏蔽、场景覆盖和诊断组件修改，不覆盖后继所有者。
	void ReleaseSceneOwnership();
	// 等待属于本次文件的 CSV 首帧确认，再验证场景并开始计时。
	void PollCaptureStart();
	// 低频检查采集窗口、场景门禁和 Profiler 所有权；时长到达后进入写盘阶段。
	void PollCapture();
	// 等待 CSV Future 和 Trace 停止读回，检查文件存在；超时失败但保留迟到启动清理。
	void PollArtifacts();
	// 正式实验只允许进程内一次；下一组 A/B 由外部脚本启动新进程，不能复用已改变的 World。
	bool CanStartFreshRun() const;
	// 检查进程级 CSV 租约，防止旧采集尚未回收时启动新实验。
	static bool HasOutstandingCsvLease();
	// 仅隔离 CSV 的提交时机，生产仍调用真实 FCsvProfiler；故障测试可延迟提交而不伪造完成。
	void RequestCsvCapture(TFunctionRef<void(const FString&)> SubmitStart);
	// 建立 CoreTicker 轮询；不依赖世界时间，但也不是自建 Worker 线程。
	void StartWatchdog();
	// 依据当前阶段推进启动、采集或写盘，并拒收暂停、时间缩放与阶段停滞样本。
	bool TickWatchdog();
	// 将内部结果映射为脚本退出码，零仅表示 Runner 完成，不替代离线门禁。
	static uint8 GetResultExitCode(EFPBenchmarkResult InResult);
	struct FEnemySnapshot;
	// 一次读取敌人注册、避让和实际组件开关；详细模式额外记录表现档位与最低 LOD。
	FEnemySnapshot ReadEnemySnapshot(bool bDetailed) const;
	// 用已有快照记录消费者配置，并一次遍历世界单独读回尸体 Mesh 标志。
	void LogConsumerSnapshot(const FEnemySnapshot& Snapshot) const;
	// 非空场景要求完整启用 RVO 或有效 Detour Crowd，拒绝两套混用及不完整代理登记。
	static bool IsAvoidanceConfigurationValid(int32 Enemies, int32 RVO, int32 Crowd, int32 ValidCrowd);
	// 锁存采集期间的人数变化，即使之后补回相同数量也不能通过稳态检查。
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

	// Runner 持有阶段事实和本次运行编号；弱 GameMode 引用不延长关卡生命周期。
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
	// CSV 租约可独立于 Runner 存活；Trace 记录目的路径，停止时只处理本次拥有的会话。
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
