// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "fpstrueBenchmarkRunner.generated.h"

class AfpstrueGameMode;

/** 自动性能测试模块：负责准备场景、采集 CSV/Trace、应用消融并按需退出。 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueBenchmarkRunner : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建不参与逐帧 Tick 的 Benchmark Runner。
	UfpstrueBenchmarkRunner();

	// GameMode 只负责决定何时挂接 Runner，采集状态和计时器全部由 Runner 自己维护。
	void StartIfRequested(AfpstrueGameMode* InGameMode);
	// 取消所有待执行的 Benchmark 阶段和计时器。
	void Cancel();

protected:
	// 组件退出时确保采集和计时器全部停止。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 启动固定场景并进入等待阶段。
	void BeginBenchmark();
	// 等待敌人生成完成，再进入预热。
	void WaitForBenchmarkReady();
	// 开启 CSV、Insights Trace 和可选截图采集。
	void StartCapture();
	// 把命令行消融开关应用到当前敌人和 AI 组件。
	void ApplyDiagnosticOverrides();
	// 校验玩家与敌人规模在采集前后保持有效，拒绝把变化中的场景写入结果表。
	bool ValidateBenchmarkState(const TCHAR* Phase) const;
	// 停止采集并保存输出。
	void StopCapture();
	// 正常结束和取消共用幂等的采集收尾；不处理阶段 Timer、输入或成功日志。
	void StopActiveProfilers();
	// 在启用自动退出时关闭测试进程。
	void ExitBenchmark();

	TWeakObjectPtr<AfpstrueGameMode> GameMode;
	bool bAbortReported = false;
	// 采集窗口已结束后，正常退出的 EndPlay 不得再把样本标记为提前中止。
	bool bCaptureFinished = false;
	bool bCaptureActive = false;
	bool bTraceActive = false;
	// 自动测试期间屏蔽真实键鼠输入，并保存固定视点用于拒绝发生位姿漂移的样本。
	bool bBenchmarkInputLocked = false;
	FVector BenchmarkPlayerLocation = FVector::ZeroVector;
	FRotator BenchmarkControlRotation = FRotator::ZeroRotator;

	FTimerHandle ReadyTimerHandle;
	FTimerHandle StartTimerHandle;
	FTimerHandle StopTimerHandle;
	FTimerHandle ExitTimerHandle;
};
