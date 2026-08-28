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
	// 停止采集并保存输出。
	void StopCapture();
	// 在启用自动退出时关闭测试进程。
	void ExitBenchmark();

	TWeakObjectPtr<AfpstrueGameMode> GameMode;
	bool bTraceActive = false;

	FTimerHandle ReadyTimerHandle;
	FTimerHandle StartTimerHandle;
	FTimerHandle StopTimerHandle;
	FTimerHandle ExitTimerHandle;
};
