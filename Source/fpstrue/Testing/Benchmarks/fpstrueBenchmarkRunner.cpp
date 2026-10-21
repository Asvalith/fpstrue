// Copyright Epic Games, Inc. All Rights Reserved.

// 采集入口：命令行配置 → 等待真实人口与视角就绪 → 预热 → 记录 → 校验写盘 → 退出。
// 单机 Runner、多端 CSV/Trace Profile、跨构建 Baseline 各自持有采集状态，不接管正式玩法。
// World wall-time、OS 线程 CPU、引擎 unit 与网络计数分别输出，不能相加当成整帧耗时。

#include "Testing/Benchmarks/fpstrueBenchmarkRunner.h"
#include "Async/Future.h"
#include "Animation/AnimInstance.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Game/fpstrueGameMode.h"
#include "Game/fpstrueMultiplayer.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "ChartCreation.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InputComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/PendingNetGame.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameUserSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "Navigation/CrowdManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTLS.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/StringBuilder.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

CSV_DECLARE_CATEGORY_EXTERN(fpstrueEnemyUI);

// Copyright Epic Games, Inc. All Rights Reserved.

/*
 * 自动测试的只读命令行快照。
 * 只供采集流程使用；玩法开关复用 FFPRuntimeOptions 的进程快照，不再次解析。
 * 人数/生命值用独立存在标志区分未传参与显式赋值，其余参数保留默认值。
 */

// 集中返回进程级采集配置，后续消费者不会各自重复解析或修改参数。
const FFPBenchmarkConfig& FFPBenchmarkConfig::Get()
{
	// 采集器共享同一份配置；运行时覆盖由基类快照提供，不重新解析。
	static const FFPBenchmarkConfig Config;
	return Config;
}

// 复用玩法启动覆盖快照，仅额外解析采集人数、时长、资源报告和输出参数。
FFPBenchmarkConfig::FFPBenchmarkConfig()
	: FFPRuntimeOptions(FFPRuntimeOptions::Get())
{
#if !UE_BUILD_SHIPPING
	// 所有开关只在首次读取时解析一次；正式基线不传 BenchmarkDisable*，消融脚本才显式启用单项关闭。
	const TCHAR* CommandLine = FCommandLine::Get();

	// 布尔参数用 FParse::Param 判断“是否出现”，不需要再为 true/false 解析字符串值。
	bAutoBenchmark = FParse::Param(CommandLine, TEXT("AutoBenchmark"));
	bCollectTextureStats = FParse::Param(CommandLine, TEXT("BenchmarkTextureStats"));
	bTakeScreenshot = FParse::Param(CommandLine, TEXT("BenchmarkScreenshot"));
	bAutoQuit = FParse::Param(CommandLine, TEXT("BenchmarkAutoQuit"));

	// 带值参数用 FParse::Value 解析，并在进入 Gameplay 前钳制到可执行范围。
	bHasEnemyCountOverride = FParse::Value(CommandLine, TEXT("BenchmarkEnemies="), EnemyCount);
	EnemyCount = FMath::Max(EnemyCount, 0);
	bHasPlayerHealthOverride = FParse::Value(CommandLine, TEXT("BenchmarkPlayerHealth="), PlayerHealth);
	if (bHasPlayerHealthOverride)
	{
		PlayerHealth = FMath::Max(PlayerHealth, 1.0f);
	}
	FParse::Value(CommandLine, TEXT("BenchmarkSeed="), Seed);
	FParse::Value(CommandLine, TEXT("BenchmarkWarmup="), WarmupSeconds);
	FParse::Value(CommandLine, TEXT("BenchmarkDuration="), DurationSeconds);
	WarmupSeconds = FMath::Max(WarmupSeconds, 0.0f);
	DurationSeconds = FMath::Max(DurationSeconds, 1.0f);

	if (FParse::Value(CommandLine, TEXT("BenchmarkTraceFile="), TraceFile))
	{
		TraceFile.TrimQuotesInline();
	}
#endif
}

// CSV 的 BeginCapture 只排队，尚未开始时 EndCapture 无法撤销它。
// 业务超时不能撤销底层排队命令：租约保留到实际首帧和写盘结束，不能按秒数自行释放。
struct FFPBenchmarkCsvCapture
{
	enum class EState : uint8 { StartRequested, Capturing, CancelPendingStart, StopRequested, Released };
	FString Filename;
	FDelegateHandle FirstFrameHandle;
	EState State = EState::StartRequested;
	TSharedFuture<FString> WriteComplete;
};

// 固定含义的计数快照，不持有场景对象；验证和日志不能各自计算出不同的消费者集合。
struct UfpstrueBenchmarkRunner::FEnemySnapshot
{
	int32 Enemies = 0;
	int32 RVO = 0;
	int32 Crowd = 0;
	int32 ValidCrowd = 0;
	bool bRegistryValid = true;
	bool bDiagnosticStateValid = true;
	// 以下仅在正式采集前统计。各档按 Full / Reduced / Background，LOD 按 0 / 1 / 2+。
	int32 Movement[3] = {};
	int32 Render[3] = {};
	int32 MinLOD[3] = {};
	int32 MovementTickEnabled = 0;
	int32 SkeletalMeshTickEnabled = 0;
	int32 Attacking = 0;
	int32 ShadowCasting = 0;
	int32 RayTracingVisible = 0;
};

namespace
{
// 允许胶囊接触造成的厘米级微动，但拒绝窗口抢焦点或残留输入导致的实际移动/转向。
constexpr float BenchmarkPlayerLocationTolerance = 10.0f;
constexpr float BenchmarkPlayerRotationToleranceDegrees = 2.0f;
constexpr double ProfilerTimeoutSeconds = 30.0;
// 此句柄不依赖 Runner/World 寿命；尚有迟到启动可能时，整个进程的 Benchmark 入口保持隔离。
TSharedPtr<FFPBenchmarkCsvCapture> OutstandingCsvCapture;
bool bProcessBenchmarkConsumed = false;

// 将结果枚举转成稳定日志标记，供人工排查与外部批处理读取。
const TCHAR* ResultName(EFPBenchmarkResult Result)
{
	switch (Result)
	{
	case EFPBenchmarkResult::Completed: return TEXT("Completed");
	case EFPBenchmarkResult::InvalidSample: return TEXT("InvalidSample");
	case EFPBenchmarkResult::Failed: return TEXT("Failed");
	case EFPBenchmarkResult::Cancelled: return TEXT("Cancelled");
	case EFPBenchmarkResult::Unsupported: return TEXT("Unsupported");
	default: return TEXT("None");
	}
}

// 三种 Tick 消融共用保存/关闭规则；未请求的组件既不修改，也不占用恢复状态。
void DisableTickForCapture(UActorComponent* Component, bool bDisable, bool& bWasEnabled)
{
	if (Component != nullptr && bDisable)
	{
		bWasEnabled = Component->IsComponentTickEnabled();
		Component->SetComponentTickEnabled(false);
	}
}
}

#if !UE_BUILD_SHIPPING
namespace
{
// A profiling process is disposable: no settings/assets are saved. Weak World
// ownership lets CoreTicker finish queued CSV cleanup after a World disappears.
struct FPvpPerformanceProfile : TSharedFromThis<FPvpPerformanceProfile>
{
	TWeakObjectPtr<UWorld> World;
	FString Role, ControlPath, OutputDirectory, CsvPath, TracePath, OwnedTracePath;
	TSharedPtr<FFPBenchmarkCsvCapture> Capture;
	TSet<TWeakObjectPtr<AfpstrueCharacter>> PreparedPlayers;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> PreparedEnemies;
	TWeakObjectPtr<ACameraActor> FixedCamera;
	double StartedAt = FPlatformTime::Seconds();
	double ReadyAt = 0.0, CaptureAt = 0.0, FlushAt = 0.0;
	double FixedViewStableAt = 0.0;
	double ControlUnreadableAt = -1.0;
	double WorldTickAt = 0.0;
	FDelegateHandle WorldTickStartHandle, WorldTickEndHandle;
	bool bReady = false, bStarting = false, bCapturing = false, bFlushing = false;
	bool bFailed = false, bOwnsTrace = false, bTraceStopRequested = false, bQuarantineLogged = false;
	bool bCollectTrace = true, bFinalized = false;
	bool bFixedView = false;
	bool bHealthAndDeath = false, bDeathBatchApplied = false, bProfileSettingsChecked = false;
	bool bCapturedPostDeath = false;
	double NextHealthPulse = 2.0;
	int32 Enemies = 0, Players = 0, Width = 0, Height = 0;
	FFPEnemyHealthBarMetrics HealthBars;
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;

	// Profile 的最终所有者消失时解除 World 采样订阅；异步写盘由采集租约继续持有。
	~FPvpPerformanceProfile()
	{
		FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
		FWorldDelegates::OnWorldTickEnd.Remove(WorldTickEndHandle);
	}

	// 仅为当前 World 的有效采集帧记录 Tick 起点，不包含引擎外层节拍等待。
	void BeginWorldTick(UWorld* TickWorld, ELevelTick, float)
	{
		if (TickWorld == World.Get() && bCapturing && !bFlushing) WorldTickAt = FPlatformTime::Seconds();
	}

	// 将配对的 World Tick wall-time 写入 CSV；其他 World 或缺失起点不产生样本。
	void EndWorldTick(UWorld* TickWorld, ELevelTick, float)
	{
		if (TickWorld != World.Get() || WorldTickAt == 0.0) return;
		// DS 不写引擎 stat-unit GameThreadTime。单独记录完整 World::Tick 的实测 wall-time，
		// 不含外部帧率等待，不冒充 CPU 占用率或扣除 Wait 后的 stat-unit GT。
		CSV_CUSTOM_STAT_GLOBAL(PvpProfileWorldTickMs, static_cast<float>((FPlatformTime::Seconds() - WorldTickAt) * 1000.0), ECsvCustomStatOp::Set);
		WorldTickAt = 0.0;
	}

	// 统一绝对路径与分隔符，用于采集文件归属比较；非法父目录折叠返回空串。
	static FString Canonical(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		if (!FPaths::CollapseRelativeDirectories(Path)) return FString();
		return Path;
	}

	// 只接受显式采集根目录下的子路径，不允许盘根、根目录本身或越界写入。
	static bool IsProfilePath(const FString& Path)
	{
		FString RequestedRoot;
		FParse::Value(FCommandLine::Get(), TEXT("PvpProfileRoot="), RequestedRoot);
		RequestedRoot.TrimQuotesInline();
		const FString Root = RequestedRoot.IsEmpty() ? Canonical(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling"))) : Canonical(RequestedRoot);
		// 显式本机采集目录可位于另一块盘；仍拒绝盘根和目录本身，不覆写用户已有产物。
		if (Root.Len() <= 3 || FPaths::IsRelative(Root)) return false;
		return !Path.IsEmpty() && !Path.Equals(Root, ESearchCase::IgnoreCase) && FPaths::IsUnderDirectory(Path, Root);
	}

	// 可选 UI 实验负载：服务器定时扣血并杀死固定批次，始终调用真实生命事务。
	void ApplyHealthWorkload()
	{
		if (!bHealthAndDeath || Role != TEXT("Server") || !bCapturing || bDeathBatchApplied) return;
		const double Elapsed = FPlatformTime::Seconds() - CaptureAt;
		if (Elapsed >= 20.0)
		{
			TArray<AfpstrueEnemyCharacter*> EnemiesToKill;
			for (const auto& WeakEnemy : PreparedEnemies)
				if (auto* Enemy = WeakEnemy.Get(); IsValid(Enemy) && !Enemy->IsDead()) EnemiesToKill.Add(Enemy);
			EnemiesToKill.Sort([](const AfpstrueEnemyCharacter& A, const AfpstrueEnemyCharacter& B) { return A.GetUniqueID() < B.GetUniqueID(); });
			if (EnemiesToKill.Num() != 160) { Fail(TEXT("DeathWorkloadInitialPopulationChanged")); return; }
			// 只进入本项目真实 Health/GAS 伤害路径，不广播伪造死亡或直接隐藏 UI。
			for (int32 Index = 0; Index < 32; ++Index)
			{
				if (auto* Health = EnemiesToKill[Index]->FindComponentByClass<UfpstrueHealthComponent>())
					UGameplayStatics::ApplyDamage(EnemiesToKill[Index], Health->GetHealth() + 1.0f, nullptr, nullptr, nullptr);
				// 保留本次死亡 Actor 到采集结束，避免对象移除令累计 UI 计数丢失；不保留 Widget/渲染资源。
				if (IsValid(EnemiesToKill[Index])) EnemiesToKill[Index]->SetLifeSpan(60.0f);
			}
			bDeathBatchApplied = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE WORKLOAD DeathBatch=32 ExpectedAlive=128 Elapsed=%.3f"), Elapsed);
		}
		else if (Elapsed >= NextHealthPulse && Elapsed <= 18.5)
		{
			if (Elapsed > NextHealthPulse + 1.0) { Fail(TEXT("HealthPulseScheduleMissedNoBurstCatchup")); return; }
			int32 Changed = 0;
			for (const auto& WeakEnemy : PreparedEnemies)
				if (auto* Enemy = WeakEnemy.Get(); IsValid(Enemy) && !Enemy->IsDead())
				{
					UGameplayStatics::ApplyDamage(Enemy, 0.5f, nullptr, nullptr, nullptr);
					++Changed;
				}
			UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE WORKLOAD HealthPulse=%d Damage=0.5 Elapsed=%.3f"), Changed, Elapsed);
			// 卡顿后不在同一采样里补发多次伤害，避免把不相同的尖峰当作 UI 模式差异。
			NextHealthPulse += 1.0;
		}
	}

	// 首次记录失败原因并转入异步收尾；不在出错点直接抛弃尚未写完的产物。
	void Fail(const TCHAR* Reason)
	{
		if (!bFailed) UE_LOG(LogTemp, Error, TEXT("PVP_PROFILE FAIL Role=%s Reason=%s"), *Role, Reason);
		bFailed = true;
		if (!bFlushing) FlushAt = FPlatformTime::Seconds();
		bFlushing = true;
	}

	// 读取外部原子替换的控制文件；短暂不可读留给下一次 Tick，非法命令进入失败收尾。
	bool ReadControl(FString& Command)
	{
		const int64 Bytes = IFileManager::Get().FileSize(*ControlPath);
		if (Bytes > 64)
		{
			Fail(TEXT("ControlMissingUnreadableOrOversized"));
			return false;
		}
		// 原子替换保证不会读取半条命令，但 Windows 的替换瞬间仍可能暂时拒绝打开。
		// 不在游戏线程 sleep，不伪造命令；下次 ticker 重试，持续不可读一秒仍判测试失败。
		if (Bytes < 1 || !FFileHelper::LoadFileToString(Command, *ControlPath))
		{
			const double Now = FPlatformTime::Seconds();
			if (ControlUnreadableAt < 0.0) ControlUnreadableAt = Now;
			if (Now - ControlUnreadableAt >= 1.0) Fail(TEXT("ControlMissingUnreadableOrOversized"));
			return false;
		}
		ControlUnreadableAt = -1.0;
		Command.TrimStartAndEndInline();
		Command.ToUpperInline();
		if (Command != TEXT("WAIT") && Command != TEXT("START") && Command != TEXT("STOP") && Command != TEXT("EXIT"))
		{
			Fail(TEXT("InvalidControlCommand"));
			return false;
		}
		return true;
	}

	// 核对人口、血条与本地视角；Out 参数区分尚未就绪的镜头和真正的无效场景。
	bool SampleWorld(bool& bWaitingForLocalView)
	{
		bWaitingForLocalView = false;
		UWorld* CurrentWorld = World.Get();
		if (CurrentWorld == nullptr || CurrentWorld->GetWorldSettings() == nullptr || CurrentWorld->IsPaused() ||
			!FMath::IsNearlyEqual(CurrentWorld->GetWorldSettings()->GetEffectiveTimeDilation(), 1.0f)) return false;
		const bool bServer = Role == TEXT("Server");
		auto* Mode = CurrentWorld->GetAuthGameMode<AfpstruePvpGameMode>();
		Enemies = Players = Width = Height = 0;
		HealthBars = FFPEnemyHealthBarMetrics{};
		for (TActorIterator<AfpstrueEnemyCharacter> It(CurrentWorld); It; ++It)
		{
			if (const auto* Bar = It->FindComponentByClass<UfpstrueEnemyHealthBarComponent>()) Bar->AppendMetrics(HealthBars);
			if (It->IsActorBeingDestroyed() || It->IsDead()) continue;
			++Enemies;
			if (bServer && !PreparedEnemies.Contains(*It))
			{
				// Explicit benchmark relevancy: all 160 replicas remain eligible,
				// rather than silently comparing different client populations.
				It->SetNetCullDistanceSquared(1.0e12f);
				It->ForceNetUpdate();
				PreparedEnemies.Add(*It);
			}
		}
		if (bServer)
		{
			if (Mode == nullptr) return false;
			if (!bProfileSettingsChecked)
			{
				const auto* DelayProperty = FindFProperty<FFloatProperty>(Mode->GetClass(), TEXT("EnemyRespawnDelay"));
				const float Delay = DelayProperty != nullptr ? DelayProperty->GetPropertyValue_InContainer(Mode) : -1.0f;
				UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE SETTINGS HealthAndDeath=%d EnemyRespawnDelay=%.3f"), bHealthAndDeath, Delay);
				if (bHealthAndDeath && Delay < 600.0f) { Fail(TEXT("DeathWorkloadRequiresRespawnDelay600")); return false; }
				bProfileSettingsChecked = true;
			}
			for (TActorIterator<AfpstrueCharacter> It(CurrentWorld); It; ++It)
			{
				if (!Mode->IsCombatParticipant(*It) || Cast<APlayerController>(It->GetController()) == nullptr) continue;
				++Players;
				if (!PreparedPlayers.Contains(*It))
				{
					UfpstrueHealthComponent* Health = It->GetHealthComponent();
					if (Health == nullptr) return false;
					// One initialization per Pawn, not invulnerability or per-frame
					// healing. Real AI attacks and damage callbacks remain enabled.
					Health->SetMaxHealthAndReset(1000000.0f);
					PreparedPlayers.Add(*It);
					UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE HEALTH Role=Server Pawn=%s Max=%.1f Current=%.1f"),
						*It->GetName(), Health->GetMaxHealth(), Health->GetHealth());
				}
				ViewLocation = It->GetActorLocation();
				ViewRotation = It->GetActorRotation();
			}
			return true;
		}
		const AGameStateBase* State = CurrentWorld->GetGameState();
		if (State != nullptr)
			for (const APlayerState* Player : State->PlayerArray) if (IsValid(Player)) ++Players;
		APlayerController* Local = UGameplayStatics::GetPlayerController(CurrentWorld, 0);
		if (Local == nullptr || !Local->IsLocalController() || Local->GetPawn() == nullptr)
		{
			// GameState BeginPlay can precede local Controller/Pawn replication.
			// Only the initial WAIT phase may defer this view-readiness check.
			bWaitingForLocalView = true;
			return false;
		}
		if (Local->AcknowledgedPawn != Local->GetPawn())
		{
			// ClientRestart 先确认占有再自动切回 Pawn；CoreTicker 要等这一整轮初始化完成后才安装专项镜头。
			bWaitingForLocalView = true;
			return false;
		}
		Local->GetPlayerViewPoint(ViewLocation, ViewRotation);
		if (bFixedView && !FixedCamera.IsValid())
		{
			// 仅此一次性采集进程固定镜头；仍保留服务器 AI、玩家受伤与真实副本更新。
			ACameraActor* Camera = nullptr;
			for (TActorIterator<ACameraActor> It(CurrentWorld); It; ++It)
				if (It->ActorHasTag(TEXT("EnemyUIProfileCamera"))) { Camera = *It; break; }
			if (Camera == nullptr)
			{
				Local->GetPawn()->GetActorEyesViewPoint(ViewLocation, ViewRotation);
				Camera = CurrentWorld->SpawnActor<ACameraActor>(ViewLocation, ViewRotation);
			}
			if (Camera == nullptr) return false;
			Camera->GetCameraComponent()->SetFieldOfView(90.0f);
			Local->SetViewTarget(Camera);
			FixedCamera = Camera;
			// 等 PlayerCameraManager 真正更新一帧，再记录实际视图；不能把意图镜头当作已渲染镜头。
			bWaitingForLocalView = true;
			return false;
		}
		if (FixedCamera.IsValid())
		{
			Local->GetPlayerViewPoint(ViewLocation, ViewRotation);
			if (Local->GetViewTarget() != FixedCamera.Get() ||
				!ViewLocation.Equals(FixedCamera->GetActorLocation(), 1.0f) ||
				!ViewRotation.Equals(FixedCamera->GetActorRotation(), 0.1f))
			{
				if (!bReady && !bStarting && !bCapturing)
				{
					// ClientRestart 可晚于 Pawn 首次复制；只在 WAIT 初始化阶段完成专项相机安装。
					// READY 后绝不重设镜头来掩盖漂移，下面仍保留实际视图的硬失败门禁。
					UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE INITIAL_VIEW_RESELECT PreviousTarget=%s"), *GetNameSafe(Local->GetViewTarget()));
					Local->SetViewTarget(FixedCamera.Get());
					FixedViewStableAt = 0.0;
					bWaitingForLocalView = true;
					return false;
				}
				UE_LOG(LogTemp, Error, TEXT("PVP_PROFILE VIEW_MISMATCH Target=%s ActualLocation=%s ActualRotation=%s"),
					*GetNameSafe(Local->GetViewTarget()), *ViewLocation.ToCompactString(), *ViewRotation.ToCompactString());
				Fail(TEXT("FixedCameraNotActualPlayerView"));
				return false;
			}
			if (!bReady)
			{
				if (FixedViewStableAt == 0.0) FixedViewStableAt = FPlatformTime::Seconds();
				if (FPlatformTime::Seconds() - FixedViewStableAt < 0.5)
				{
					bWaitingForLocalView = true;
					return false;
				}
			}
		}
		Local->GetViewportSize(Width, Height);
		return true;
	}

	// 用实际输出路径确认 CSV 仍属于本轮，避免停止其他采集者接管的新会话。
	bool OwnsCsv() const
	{
#if CSV_PROFILER
		return Capture.IsValid() && Canonical(FCsvProfiler::Get()->GetOutputFilename()).Equals(CsvPath, ESearchCase::IgnoreCase);
#else
		return false;
#endif
	}

	// 独占申请 CSV/可选 Trace，并建立跨 World 生命周期的写盘租约；不覆盖旧文件。
	void BeginCapture()
	{
#if CSV_PROFILER
		FCsvProfiler* Csv = FCsvProfiler::Get();
		if ((OutstandingCsvCapture.IsValid() && OutstandingCsvCapture->State != FFPBenchmarkCsvCapture::EState::Released) ||
			Csv->IsCapturing() || Csv->IsWritingFile() || Csv->IsEndCapturePending() || FTraceAuxiliary::IsConnected())
		{
			Fail(TEXT("ProfilerAlreadyOwned"));
			return;
		}
		if (IFileManager::Get().FileExists(*CsvPath) || IFileManager::Get().FileExists(*TracePath))
		{
			Fail(TEXT("RefusingToOverwriteCaptureArtifacts"));
			return;
		}
		// UE 5.5 CsvProfiler 的 Trace 元数据在启动时注册；中途打开 Counters 会漏掉类别定义，
		// 导致 Insights 的 CSV analyzer 断言。独立 CSV 文件仍正常采集，仅关闭这条冗余 Trace 通道。
		// Start 只启用所列通道，不会关闭已经启用的通道，因此先显式禁用 Counters。
		if (bCollectTrace) FTraceAuxiliary::DisableChannels(TEXT("counters"));
		if (bCollectTrace && !FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *TracePath,
			Role == TEXT("Server") ? TEXT("cpu,frame,bookmark,task,stats") : TEXT("cpu,gpu,frame,bookmark,task,stats")))
		{
			Fail(TEXT("TraceStartFailed"));
			return;
		}
		if (bCollectTrace)
		{
			TStringBuilder<512> ActiveChannels;
			FTraceAuxiliary::GetActiveChannelsString(ActiveChannels);
			UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE TRACE_CHANNELS Role=%s Active=%s"), *Role, ActiveChannels.ToString());
			OwnedTracePath = FTraceAuxiliary::GetTraceDestinationString();
			bOwnsTrace = true;
		}
		Capture = MakeShared<FFPBenchmarkCsvCapture>();
		Capture->Filename = Role + TEXT(".csv");
		OutstandingCsvCapture = Capture;
		const TSharedRef<FFPBenchmarkCsvCapture> Lease = Capture.ToSharedRef();
		const FString ExpectedPath = CsvPath;
		Lease->FirstFrameHandle = Csv->OnCSVProfileFirstFrame().AddLambda([Lease, ExpectedPath]()
		{
			FCsvProfiler* Profiler = FCsvProfiler::Get();
			if (!Canonical(Profiler->GetOutputFilename()).Equals(ExpectedPath, ESearchCase::IgnoreCase)) return;
			const bool bCancelled = Lease->State == FFPBenchmarkCsvCapture::EState::CancelPendingStart;
			Lease->State = FFPBenchmarkCsvCapture::EState::Capturing;
			Profiler->OnCSVProfileFirstFrame().Remove(Lease->FirstFrameHandle);
			Lease->FirstFrameHandle.Reset();
			if (bCancelled)
			{
				Lease->WriteComplete = Profiler->EndCapture();
				Lease->State = FFPBenchmarkCsvCapture::EState::StopRequested;
			}
		});
		// Like the single-player lease, cleanup survives World teardown and a
		// cancelled queued start. Never release it merely because a timer expired.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Lease](float)
		{
			if (Lease->State == FFPBenchmarkCsvCapture::EState::StopRequested && Lease->WriteComplete.IsValid() && Lease->WriteComplete.IsReady())
				Lease->State = FFPBenchmarkCsvCapture::EState::Released;
			if (Lease->State != FFPBenchmarkCsvCapture::EState::Released) return true;
			if (OutstandingCsvCapture == Lease) OutstandingCsvCapture.Reset();
			return false;
		}), 0.1f);
		bStarting = true;
		Csv->BeginCapture(-1, OutputDirectory, Capture->Filename);
#else
		Fail(TEXT("CsvProfilerUnavailable"));
#endif
	}

	// 只停止自己持有的 CSV/Trace；排队中的 CSV 启动改为启动后立即收尾。
	void StopOwnedResources()
	{
#if CSV_PROFILER
		if (Capture.IsValid())
		{
			FCsvProfiler* Csv = FCsvProfiler::Get();
			if (Capture->State == FFPBenchmarkCsvCapture::EState::StartRequested)
				Capture->State = FFPBenchmarkCsvCapture::EState::CancelPendingStart;
			else if (Capture->State == FFPBenchmarkCsvCapture::EState::Capturing && Csv->IsCapturing() && OwnsCsv())
			{
				Capture->WriteComplete = Csv->EndCapture();
				Capture->State = FFPBenchmarkCsvCapture::EState::StopRequested;
			}
		}
#endif
		if (bOwnsTrace && !bTraceStopRequested)
		{
			if (FTraceAuxiliary::GetTraceDestinationString() == OwnedTracePath)
				bTraceStopRequested = FTraceAuxiliary::Stop();
			else
			{
				Fail(TEXT("TraceOwnershipLost"));
				bOwnsTrace = false; // Never stop a replacement session.
			}
		}
	}

	// 等待落盘、验证产物并等待外部 EXIT；返回 true 保留 Ticker，false 表示已请求退出。
	bool PollFlush()
	{
		StopOwnedResources();
		const bool bCsvDone = !Capture.IsValid() || (Capture->WriteComplete.IsValid() && Capture->WriteComplete.IsReady());
		const bool bTraceDone = !bOwnsTrace || (bTraceStopRequested && !FTraceAuxiliary::IsConnected());
		if (!bCsvDone || !bTraceDone)
		{
			if (!bQuarantineLogged && FPlatformTime::Seconds() - FlushAt >= 30.0)
			{
				bQuarantineLogged = true;
				Fail(TEXT("ArtifactFlushTimeoutLeaseRetained"));
				UE_LOG(LogTemp, Error, TEXT("PVP_PROFILE QUARANTINED Role=%s CsvDone=%d TraceDone=%d"), *Role, bCsvDone, bTraceDone);
			}
			return true;
		}
		if (!bFailed && !bFinalized)
		{
			const FString ActualCsv = Capture.IsValid() ? Canonical(Capture->WriteComplete.Get()) : FString();
			if (!ActualCsv.Equals(CsvPath, ESearchCase::IgnoreCase) || IFileManager::Get().FileSize(*CsvPath) <= 0 ||
				(bCollectTrace && IFileManager::Get().FileSize(*TracePath) <= 0)) Fail(TEXT("ArtifactMissingEmptyOrWrongPath"));
			if (Role != TEXT("Server") && !GUsingNullRHI &&
				IFileManager::Get().FileSize(*FPaths::Combine(OutputDirectory, Role + TEXT(".png"))) <= 0)
				Fail(TEXT("RenderedClientScreenshotMissing"));
			if (bHealthAndDeath && Role != TEXT("Server") && !GUsingNullRHI &&
				IFileManager::Get().FileSize(*FPaths::Combine(OutputDirectory, Role + TEXT("_AfterDeath.png"))) <= 0)
				Fail(TEXT("DeathWorkloadScreenshotMissing"));
		}
		if (!bFailed && !bFinalized)
		{
			// 写盘完成后才检查 UObject 释放，不把强制 GC 停顿混入正式帧时；仍不把驻留量当作分配归因。
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			int32 RemainingWidgets = 0;
			UWorld* CurrentWorld = World.Get();
			ForEachObjectOfClass(UfpstrueEnemyHealthBarWidget::StaticClass(), [CurrentWorld, &RemainingWidgets](UObject* Object)
			{
				if (!Object->HasAnyFlags(RF_ClassDefaultObject) && CastChecked<UfpstrueEnemyHealthBarWidget>(Object)->GetWorld() == CurrentWorld)
					++RemainingWidgets;
			});
			const int32 ExpectedWidgets = Role == TEXT("Server") || UfpstrueEnemyHealthBarComponent::GetConfiguredMode() == EFPEnemyHealthBarMode::Off
				? 0 : (bHealthAndDeath ? 128 : 160);
			UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE UI_GC Role=%s Widgets=%d Expected=%d PhysicalMB=%.3f CaptureStopped=1"),
				*Role, RemainingWidgets, ExpectedWidgets, FPlatformMemory::GetStats().UsedPhysical / (1024.0 * 1024.0));
			if (RemainingWidgets != ExpectedWidgets) Fail(TEXT("ReleasedEnemyWidgetsRetainedAfterCaptureGC"));
		}
		if (!bFailed && !bFinalized)
		{
			bFinalized = true;
			// 三端都写盘与释放检查完成后才允许退出，避免 DS 先走导致慢端掉线换图污染校验。
			UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE FLUSHED Role=%s AwaitingExit=1"), *Role);
		}
		if (!bFailed)
		{
			FString Command;
			if (!ReadControl(Command)) { if (!bFailed) return true; }
			else if (Command != TEXT("EXIT")) return true;
		}
		if (!bFailed) UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE COMPLETE Role=%s Csv=%s Trace=%s Duration=%.3f"),
			*Role, *CsvPath, *TracePath, FPlatformTime::Seconds() - CaptureAt);
		FPlatformMisc::RequestExitWithStatus(false, bFailed ? 2 : 0, TEXT("PvpPerformanceProfile"));
		return false;
	}

	// 推进多端 Profile 的就绪、预热、采集和收尾阶段，拒绝人口变化及控制命令倒退。
	bool Tick(float)
	{
		if (FPlatformTime::Seconds() - StartedAt >= 300.0) Fail(TEXT("Watchdog300Seconds"));
		if (bFlushing) return PollFlush();
		FString Command;
		if (!ReadControl(Command)) return bFailed ? PollFlush() : true;
		if (Command == TEXT("EXIT")) { Fail(TEXT("ExitBeforeAllPeersFlushed")); return PollFlush(); }
		bool bWaitingForLocalView = false;
		if (!SampleWorld(bWaitingForLocalView))
		{
			if (bWaitingForLocalView && !bReady && !bStarting && !bCapturing && Command == TEXT("WAIT")) return true;
			Fail(TEXT("WorldViewOrTimeInvalid"));
			return PollFlush();
		}
		const double CaptureElapsed = bCapturing ? FPlatformTime::Seconds() - CaptureAt : 0.0;
		const bool bDeathTransition = bHealthAndDeath && bCapturing && CaptureElapsed >= 19.5;
		const bool bNowReady = Players == 2 && (bDeathTransition ? Enemies >= 128 && Enemies <= 160 : Enemies == 160);
		if (bReady && (!bNowReady || (bDeathTransition && CaptureElapsed >= 23.0 && Enemies != 128)))
		{ Fail(TEXT("PopulationChangedOutsideExplicitWorkload")); return PollFlush(); }
		if (!bReady && bNowReady)
		{
			bReady = true;
			ReadyAt = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE READY Role=%s Enemies=%d Players=%d Location=%s Rotation=%s Width=%d Height=%d"),
				*Role, Enemies, Players, *ViewLocation.ToCompactString(), *ViewRotation.ToCompactString(), Width, Height);
		}
		if (!bStarting && !bCapturing)
		{
			if (Command == TEXT("STOP")) { Fail(TEXT("StopBeforeCapture")); return PollFlush(); }
			if (Command == TEXT("START"))
			{
				if (!bReady || FPlatformTime::Seconds() - ReadyAt < 14.8) { Fail(TEXT("StartBeforeReadyAnd15SecondWarmup")); return PollFlush(); }
				BeginCapture();
			}
		}
		else
		{
			if (Command == TEXT("WAIT")) { Fail(TEXT("ControlRegressedAfterStart")); return PollFlush(); }
			if (bStarting && Capture.IsValid() && Capture->State == FFPBenchmarkCsvCapture::EState::Capturing)
			{
				bStarting = false;
				bCapturing = true;
				CaptureAt = FPlatformTime::Seconds();
				UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE CAPTURING Role=%s Enemies=%d Players=%d Csv=%s Trace=%s"),
					*Role, Enemies, Players, *CsvPath, *TracePath);
				if (Role != TEXT("Server") && !GUsingNullRHI)
					FScreenshotRequest::RequestScreenshot(FPaths::Combine(OutputDirectory, Role + TEXT(".png")), true, false);
			}
			if (bCapturing)
			{
				ApplyHealthWorkload();
				if (bHealthAndDeath && !bCapturedPostDeath && Role != TEXT("Server") && !GUsingNullRHI && CaptureElapsed >= 24.0)
				{
					bCapturedPostDeath = true;
					FScreenshotRequest::RequestScreenshot(FPaths::Combine(OutputDirectory, Role + TEXT("_AfterDeath.png")), true, false);
				}
#if CSV_PROFILER
				if (!OwnsCsv() || !FCsvProfiler::Get()->IsCapturing() || (bCollectTrace && FTraceAuxiliary::GetTraceDestinationString() != OwnedTracePath))
				{ Fail(TEXT("CaptureOwnershipInterrupted")); return PollFlush(); }
#endif
				UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE SAMPLE Role=%s Enemies=%d Players=%d Elapsed=%.3f Location=%s Rotation=%s Width=%d Height=%d"),
					*Role, Enemies, Players, FPlatformTime::Seconds() - CaptureAt, *ViewLocation.ToCompactString(), *ViewRotation.ToCompactString(), Width, Height);
				const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
				const double PhysicalMB = Memory.UsedPhysical / (1024.0 * 1024.0);
				CSV_CUSTOM_STAT(fpstrueEnemyUI, ActiveWidgets, HealthBars.Widgets, ECsvCustomStatOp::Set);
				CSV_CUSTOM_STAT(fpstrueEnemyUI, FrustumVisible, HealthBars.FrustumVisible, ECsvCustomStatOp::Set);
				CSV_CUSTOM_STAT(fpstrueEnemyUI, ProcessPhysicalMB, PhysicalMB, ECsvCustomStatOp::Set);
				UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE UI Role=%s Mode=%s Components=%d Widgets=%d Frustum=%d World=%d Screen=%d Manual=%d Created=%llu Updates=%llu Requests=%llu Draws=%llu PhysicalMB=%.3f PeakPhysicalMB=%.3f Elapsed=%.3f"),
					*Role, UfpstrueEnemyHealthBarComponent::GetModeName(UfpstrueEnemyHealthBarComponent::GetConfiguredMode()),
					HealthBars.Components, HealthBars.Widgets, HealthBars.FrustumVisible, HealthBars.WorldWidgets, HealthBars.ScreenWidgets, HealthBars.ManualWorldWidgets,
					HealthBars.Created, HealthBars.ValueUpdates, HealthBars.RedrawRequests, HealthBars.RenderTargetDraws, PhysicalMB,
					Memory.PeakUsedPhysical / (1024.0 * 1024.0), FPlatformTime::Seconds() - CaptureAt);
			}
			if (Command == TEXT("STOP"))
			{
				if (!bCapturing || FPlatformTime::Seconds() - CaptureAt < 29.8) Fail(TEXT("StopBefore30SecondCapture"));
				else { bFlushing = true; FlushAt = FPlatformTime::Seconds(); }
				return PollFlush();
			}
		}
		return true;
	}
};
}
#endif

namespace
{
TRACE_DECLARE_FLOAT_COUNTER(FpBaselineWorldWall, TEXT("Fpstrue/Baseline/WorldTickWallMs"));
TRACE_DECLARE_INT_COUNTER(FpBaselineGTUser, TEXT("Fpstrue/Baseline/GTUser100nsCumulative"));
TRACE_DECLARE_INT_COUNTER(FpBaselineGTKernel, TEXT("Fpstrue/Baseline/GTKernel100nsCumulative"));
TRACE_DECLARE_FLOAT_COUNTER(FpBaselineWorldCpu, TEXT("Fpstrue/Baseline/WorldGameThreadCpuMs"));
TRACE_DECLARE_INT_COUNTER(FpBaselineWorldCpuValid, TEXT("Fpstrue/Baseline/WorldGameThreadCpuValid"));
// 复用引擎跨构建的性能消费者；不启用 Shipping 中已裁掉的 Trace/CSV 宏。
// 与原 frames.csv 分开记录：这些 unit 值可能来自前一渲染管线帧，不是 OS 执行时间。
struct FPvpBaselineUnits : IPerformanceDataConsumer
{
	struct FSample
	{
		uint64 Frame = 0, Qpc = 0;
		double Elapsed = 0;
		FFrameData Data;
	};
	static constexpr int32 MaxSamples = 262144;
	TArray<FSample> Samples;
	double CaptureAt = 0;
	bool bCollect = false, bOverflowed = false;
	// 引擎注册消费者时预留存储；正式采样仍由 bCollect 在 START 边界开启。
	virtual void StartCharting() override { Samples.Reserve(8192); }
	// 引擎停止回调不决定本轮产物归属，实际注销和写盘由 Baseline 管理。
	virtual void StopCharting() override {}
	// 保存有界的原生 unit 样本及回调帧号；溢出标记失败，不丢尾部后伪造完整覆盖。
	virtual void ProcessFrame(const FFrameData& Data) override
	{
		if (!bCollect) return;
		if (Samples.Num() >= MaxSamples) { bOverflowed = true; return; }
		Samples.Add({GFrameCounter, FPlatformTime::Cycles64(), FPlatformTime::Seconds() - CaptureAt, Data});
	}
};
// Shared instrumentation, not a Shipping cheat/RPC surface. It observes a local
// World and frame boundaries; only the disposable Editor DS prepares test HP and
// relevancy once. Diagnostic unit counters come from the native engine consumer.
// 【设计取舍】正式基线与带采集开销的诊断分开：前者回答是否变快，后者解释时间花在哪里。
// 同帧号只证明样本关联；不同线程可重叠、嵌套 scope 可重复包含，不能直接相加当整帧耗时。
struct FPvpBaselineCapture : TSharedFromThis<FPvpBaselineCapture>
{
	struct FThreadCpuSample
	{
		uint64 User100ns = 0, Kernel100ns = 0;
		uint32 ThreadId = 0, Error = 0;
		bool bValid = false;
	};
	struct FTraceAnchor
	{
		uint64 Frame = 0, Qpc = 0;
		double SecondsPerCycle = 0;
		uint32 ThreadId = 0;
	};
	struct FFrame
	{
		uint64 Frame;
		double Elapsed, Interval, WorldTick;
		bool bWorldTick;
	};
	struct FFrameCpu
	{
		FThreadCpuSample PreviousEndCpu, EndCpu, WorldBeginCpu, WorldEndCpu;
	};
	struct FFramePacing
	{
		uint64 Frame = 0;
		double DeltaSeconds = 0, IdleSeconds = 0, IdleOvershootSeconds = 0;
		float QueryInputDeltaSeconds = 0, QueriedEngineMaxTickRate = 0;
		int32 NetServerMaxTickRate = 0, MaxNetTickRate = 0;
		bool bNetDriverValid = false, bEngineQueryValid = false;
	};
	struct FConnectionCounters
	{
		TWeakObjectPtr<UNetConnection> Connection;
		uint32 ObjectId = 0, EngineConnectionId = 0;
		FString ObjectName, RemoteAddress;
		int64 InBytes = 0, OutBytes = 0, InPackets = 0, OutPackets = 0;
	};
	struct FNetworkBoundary
	{
		double Seconds = 0;
		uint64 Qpc = 0;
		TArray<FConnectionCounters> Connections;
	};
	static constexpr int32 MaxFrames = 262144;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<ACameraActor> Camera;
	TSet<TWeakObjectPtr<AfpstrueCharacter>> PreparedPlayers;
	TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>> PreparedEnemies;
	TArray<FFrame> Frames;
	// 原始 OS 样本只在诊断开关开启时分配，不增加普通基线每个 FFrame 的驻留尺寸。
	TArray<FFrameCpu> CpuFrames;
	// 逐帧节拍字段仅随独立诊断启用；不扩充历史 schema-2 frames.csv 或普通基线热路径。
	TArray<FFramePacing> PacingFrames;
	TSharedPtr<FPvpBaselineUnits> UnitConsumer;
	TWeakObjectPtr<UEngine> UnitEngine;
	FString Role, ControlPath, Directory, Phase = TEXT("STARTING"), Reason, ViewPolicy = TEXT("Player");
	FString EnemyReplication = TEXT("Tiered");
	FString AnimationStartSnapshot = TEXT("null");
	FString PacingStartSnapshot = TEXT("null");
	TWeakObjectPtr<UNetDriver> NetworkDriver;
	FNetworkBoundary NetworkStart, NetworkStop;
	uint32 NetworkIdentitySamples = 0;
	FDelegateHandle StartHandle, EndHandle, FrameHandle;
	double StartedAt = FPlatformTime::Seconds(), ReadyAt = 0, CaptureAt = 0, StopAt = 0;
	double LastEndAt = 0, WorldStartAt = 0, LastWorldMs = 0, UnreadableAt = -1, ViewStableAt = 0;
	uint64 LastWorldFrame = MAX_uint64;
	FThreadCpuSample PreviousEndCpu, WorldBeginCpu, LastWorldBeginCpu, LastWorldEndCpu;
	FTraceAnchor CaptureStartAnchor, CaptureStopAnchor;
	bool bCpuAccounting = false;
	uint64 CpuQueryCount = 0, CpuQueryFailures = 0;
	bool bCapturing = false, bRendered = false, bFailed = false, bStatusFailed = false;
	int32 Enemies = 0, Players = 0, Width = 0, Height = 0, ValidationSamples = 0;
	int32 MinEnemies = MAX_int32, MaxEnemies = 0, MinPlayers = MAX_int32, MaxPlayers = 0;
	// Same population scan also records authority-side activity. Clients do not
	// infer AI decisions from pose; their activity fields remain explicitly null.
	int32 AIControllers = 0, BehaviorReady = 0, Chasing = 0, Attacking = 0, Moving = 0;
	int32 MinAIControllers = MAX_int32, MaxAIControllers = 0, MinChasing = MAX_int32, MaxChasing = 0;
	int32 MinBehaviorReady = MAX_int32, MaxBehaviorReady = 0;
	int32 MinAttacking = MAX_int32, MaxAttacking = 0, MinMoving = MAX_int32, MaxMoving = 0;
	float ViewFOV = 0;
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	FFPEnemyHealthBarMetrics Bars;
	bool bQualityApplied = false;
	bool bScreenshotRequested = false;
	bool bContinuousFloorChecks = false, bContinuousFloorChecksPrepared = false;

	// ★ 诊断边界：只在显式开关下直接读取当前 GT 的 OS 累计计时，不使用 UE 的缓存 CPU 百分比。
	// 100ns 是 FILETIME 单位而非逐帧精度；保留原值与有符号差值，不把量化零值/跨边界突增 clamp 掉。
	// 【设计取舍】wall 减线程 CPU 只能说明该线程未执行，不能区分等依赖、节拍休眠或就绪未获调度。
	// 精确归因仍需对齐线程身份与调度记录；GT 等 Worker 完成也不代表整机没有执行工作。
	FThreadCpuSample ReadGameThreadCpu()
	{
		FThreadCpuSample Sample;
		if (!bCpuAccounting || !IsInGameThread()) return Sample;
#if PLATFORM_WINDOWS
		Sample.ThreadId = FPlatformTLS::GetCurrentThreadId();
		FILETIME Creation = {}, Exit = {}, Kernel = {}, User = {};
		++CpuQueryCount;
		if (::GetThreadTimes(::GetCurrentThread(), &Creation, &Exit, &Kernel, &User))
		{
			Sample.User100ns = (uint64(User.dwHighDateTime) << 32) | User.dwLowDateTime;
			Sample.Kernel100ns = (uint64(Kernel.dwHighDateTime) << 32) | Kernel.dwLowDateTime;
			Sample.bValid = true;
		}
		else { Sample.Error = uint32(::GetLastError()); ++CpuQueryFailures; }
#endif
		return Sample;
	}

	// 只对同一有效线程的累计计数做有符号差分；负差保留给报告诊断，不静默钳零。
	static bool CpuDifference(const FThreadCpuSample& Begin, const FThreadCpuSample& End, int64& User, int64& Kernel)
	{
		if (!Begin.bValid || !End.bValid || Begin.ThreadId != End.ThreadId) return false;
		User = int64(End.User100ns) - int64(Begin.User100ns);
		Kernel = int64(End.Kernel100ns) - int64(Begin.Kernel100ns);
		return true;
	}

	// 输出用户态或内核态原始累计计数；缺失样本写空字段而非零耗时。
	static FString CpuRaw(const FThreadCpuSample& Sample, bool bUser)
	{
		return Sample.bValid ? FString::Printf(TEXT("%llu"), bUser ? Sample.User100ns : Sample.Kernel100ns) : FString();
	}
	// 格式化可用的有符号 CPU 差值，保留缺失与真实零的区别。
	static FString CpuDelta(bool bValid, int64 Value)
	{
		return bValid ? FString::Printf(TEXT("%lld"), Value) : FString();
	}
	// 将 FILETIME 的 100ns 用户态/内核态差值换算成 ms；无效时使用调用者指定占位。
	static FString CpuMs(bool bValid, int64 User, int64 Kernel, const TCHAR* Missing = TEXT(""))
	{
		return bValid ? FString::Printf(TEXT("%.9f"), double(User + Kernel) * 0.0001) : FString(Missing);
	}

	// 同时写帧号、QPC、比例和线程身份，供离线校准 START/STOP 与 Trace/ETL 时间轴。
	FTraceAnchor MarkCaptureBoundary(const TCHAR* Boundary) const
	{
		const FTraceAnchor Anchor{GFrameCounter, FPlatformTime::Cycles64(), FPlatformTime::GetSecondsPerCycle64(), FPlatformTLS::GetCurrentThreadId()};
		// 配对 Trace bookmark 与日志中的原始 QPC/frequency scale；不能将未校准的启动秒数直接当成 ETL/Trace 时间。
		TRACE_BOOKMARK_CYCLES(Anchor.Qpc, TEXT("PVP_BASELINE %s role=%s frame=%llu qpc=%llu secondsPerCycle64=%.17g threadId=%u"),
			Boundary, *Role, Anchor.Frame, Anchor.Qpc, Anchor.SecondsPerCycle, Anchor.ThreadId);
		UE_LOG(LogTemp, Display, TEXT("PVP_BASELINE CLOCK_ANCHOR boundary=%s role=%s frame=%llu qpc=%llu secondsPerCycle64=%.17g threadId=%u"),
			Boundary, *Role, Anchor.Frame, Anchor.Qpc, Anchor.SecondsPerCycle, Anchor.ThreadId);
		return Anchor;
	}

	// 停止 unit 收集并解除所有采样委托；不触碰外部调度录制器。
	~FPvpBaselineCapture()
	{
		StopUnitConsumer();
		FWorldDelegates::OnWorldTickStart.Remove(StartHandle);
		FWorldDelegates::OnWorldTickEnd.Remove(EndHandle);
		FCoreDelegates::OnEndFrame.Remove(FrameHandle);
	}

	// 先关闭收集，再向仍有效的 Engine 注销消费者；已收集样本保留用于写盘。
	void StopUnitConsumer()
	{
		if (UnitConsumer.IsValid()) UnitConsumer->bCollect = false;
		if (UnitEngine.IsValid() && UnitConsumer.IsValid()) UnitEngine->RemovePerformanceDataConsumer(UnitConsumer);
		UnitEngine.Reset();
	}

	// 去掉参数外引号并规范化绝对路径，供 Baseline 产物和允许目录核对。
	static FString Canonical(FString Path)
	{
		Path.TrimQuotesInline();
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		return FPaths::CollapseRelativeDirectories(Path) ? Path : FString();
	}

	// 对路径、对象名等文本做 JSON 字符串转义，不把日志文本直接拼成结构字段。
	static FString Quote(FString Text)
	{
		Text.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Text.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Text.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Text.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Text.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return TEXT("\"") + Text + TEXT("\"");
	}

	// 在 GT 只读实际节拍和安全可查询的限速值，不执行限速等待或推进引擎节拍状态。
	FFramePacing ReadFramePacing() const
	{
		check(IsInGameThread());
		FFramePacing Sample;
		Sample.Frame = GFrameCounter;
		Sample.DeltaSeconds = FApp::GetDeltaTime();
		Sample.IdleSeconds = FApp::GetIdleTime();
		Sample.IdleOvershootSeconds = FApp::GetIdleTimeOvershoot();
		Sample.QueryInputDeltaSeconds = static_cast<float>(Sample.DeltaSeconds);
		if (const UWorld* Current = World.Get())
			if (const UNetDriver* Driver = Current->GetNetDriver())
			{
				Sample.bNetDriverValid = true;
				Sample.NetServerMaxTickRate = Driver->GetNetServerMaxTickRate();
				Sample.MaxNetTickRate = Driver->MaxNetTickRate;
			}
		// UE5.5 的 const GetMaxTickRate 仍包含故障注入：CauseHitches 会 Sleep，
		// t.UnsteadyFPS 会推进随机/静态状态。只对已审计的精确引擎类且两项关闭时重读，
		// 不调用 UpdateTimeAndHandleMaxTickRate，也不更新 RunningAverageDeltaTime。
		const IConsoleVariable* CauseHitches = IConsoleManager::Get().FindConsoleVariable(TEXT("CauseHitches"));
		const IConsoleVariable* UnsteadyFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.UnsteadyFPS"));
		if (GEngine != nullptr && GEngine->GetClass() == UGameEngine::StaticClass() &&
			CauseHitches != nullptr && CauseHitches->GetInt() == 0 && UnsteadyFPS != nullptr && UnsteadyFPS->GetInt() == 0 &&
			FMath::IsFinite(Sample.QueryInputDeltaSeconds) && Sample.QueryInputDeltaSeconds >= 0)
		{
			Sample.QueriedEngineMaxTickRate = GEngine->GetMaxTickRate(Sample.QueryInputDeltaSeconds, true);
			Sample.bEngineQueryValid = FMath::IsFinite(Sample.QueriedEngineMaxTickRate);
		}
		return Sample;
	}

	// 输出限速配置与实际读回快照；配置值、重读结果和真正等待时间不混为一谈。
	FString BuildPacingSnapshot() const
	{
		const FFramePacing Sample = ReadFramePacing();
		const auto Number = [](bool bValid, double Value) { return bValid ? FString::Printf(TEXT("%.9f"), Value) : FString(TEXT("null")); };
		FString Json = FString::Printf(TEXT("{\"frame\":%llu,\"engineClass\":%s,\"netDriverValid\":%s,\"netServerMaxTickRate\":%s,\"maxNetTickRate\":%s,\"queryInputDeltaSeconds\":%.9f,\"queriedEngineMaxTickRateHz\":%s,\"engineQueryValid\":%s,\"appDeltaSeconds\":%.9f,\"appIdleSeconds\":%.9f,\"appIdleOvershootSeconds\":%.9f"),
			Sample.Frame, *Quote(GEngine != nullptr ? GEngine->GetClass()->GetPathName() : FString()),
			Sample.bNetDriverValid ? TEXT("true") : TEXT("false"), *Number(Sample.bNetDriverValid, Sample.NetServerMaxTickRate),
			*Number(Sample.bNetDriverValid, Sample.MaxNetTickRate), Sample.QueryInputDeltaSeconds,
			*Number(Sample.bEngineQueryValid, Sample.QueriedEngineMaxTickRate), Sample.bEngineQueryValid ? TEXT("true") : TEXT("false"),
			Sample.DeltaSeconds, Sample.IdleSeconds, Sample.IdleOvershootSeconds);
		Json += FString::Printf(TEXT(",\"smoothFrameRateConfigured\":%s,\"frameRateSmoothingAllowed\":%s,\"useFixedFrameRate\":%s,\"fixedFrameRateHz\":%s,\"appUseFixedTimeStep\":%s,\"appFixedDeltaSeconds\":%.9f,\"appBenchmarking\":%s,\"cvars\":{"),
			GEngine != nullptr && GEngine->bSmoothFrameRate ? TEXT("true") : TEXT("false"),
			GEngine != nullptr && GEngine->IsAllowedFramerateSmoothing() ? TEXT("true") : TEXT("false"),
			GEngine != nullptr && GEngine->bUseFixedFrameRate ? TEXT("true") : TEXT("false"),
			*Number(GEngine != nullptr, GEngine != nullptr ? GEngine->FixedFrameRate : 0),
			FApp::UseFixedTimeStep() ? TEXT("true") : TEXT("false"), FApp::GetFixedDeltaTime(), FApp::IsBenchmarking() ? TEXT("true") : TEXT("false"));
		const TCHAR* Names[] = {TEXT("t.MaxFPS"), TEXT("CauseHitches"), TEXT("t.UnsteadyFPS")};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
		{
			if (Index > 0) Json += TEXT(",");
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Names[Index]);
			Json += Quote(Names[Index]) + TEXT(":") + Number(Variable != nullptr, Variable != nullptr ? Variable->GetFloat() : 0);
		}
		return Json + TEXT("}}");
	}

	// 仅在采集边界读取实例与共享资产配置，不在逐帧/人口校验循环遍历 PhysicsAsset。
	// 这些值描述门禁与实际组件配置，不宣称任何一次动画任务已经派发到 Worker。
	FString BuildAnimationSnapshot() const
	{
		check(IsInGameThread());
		UWorld* Current = World.Get();
		int32 Alive = 0, Meshes = 0, MeshTickEnabled = 0, EveryFrameMeshTicks = 0, URO = 0, LeaderFollowers = 0;
		int32 MainInstances = 0, MainMultithreaded = 0, MainCanParallel = 0, MainRootMotionEverything = 0;
		int32 PostInstances = 0, PostCanParallel = 0, PostDisabled = 0, SimpleEnemies = 0, FullAuthorityPose = 0;
		int32 MeshCollisionEnabled = 0, MeshPhysicsStates = 0, ValidMeshBodyInstances = 0, ConstraintEntries = 0;
		int32 HitProxyComponents = 0, QueryEnabledHitProxies = 0, HitProxyPhysicsStates = 0;
		int32 HitProxyBodySetups = 0, InvalidHitProxyBodySetups = 0, BoxHitElements = 0, ConvexHitElements = 0, OtherHitElements = 0;
		int32 BoxComponents = 0, ConvexMeshComponents = 0, UnexpectedHitProxyComponents = 0;
		int32 MinHitBoxesPerEnemy = MAX_int32, MaxHitBoxesPerEnemy = 0, AlwaysCheckFloorEnemies = 0;
		TSet<const UStaticMesh*> SharedHullAssets;
		int32 GameplayTiers[3] = {}, RenderTiers[3] = {}, VisibilityOptions[5] = {};
		int32 GameplayReasonSampled = 0, GameplayDistanceValid = 0;
		int32 FullByDistance = 0, FullByAttack = 0, FullByMeleeRange = 0, FullReasonUnknown = 0;
		FString EnemyDetails;
		const auto Boolean = [](bool bValue) { return bValue ? TEXT("true") : TEXT("false"); };
		const double SnapshotWorldTime = Current != nullptr ? Current->GetTimeSeconds() : -1.0;
		TMap<const UPhysicsAsset*, int32> AssetReferences;
		if (Current != nullptr) for (TActorIterator<AfpstrueEnemyCharacter> It(Current); It; ++It)
		{
			const AfpstrueEnemyCharacter* Enemy = *It;
			if (Enemy->IsActorBeingDestroyed() || Enemy->IsDead()) continue;
			++Alive;
			AlwaysCheckFloorEnemies += Enemy->GetCharacterMovement() != nullptr && Enemy->GetCharacterMovement()->bAlwaysCheckFloor ? 1 : 0;
			SimpleEnemies += Enemy->UsesSimpleHitProxies() ? 1 : 0;
			FullAuthorityPose += Enemy->RequiresFullAuthorityPose() ? 1 : 0;
			++GameplayTiers[FMath::Clamp(int32(Enemy->GetGameplaySignificanceTier()), 0, 2)];
			++RenderTiers[FMath::Clamp(int32(Enemy->GetRenderSignificanceTier()), 0, 2)];
			const FFPEnemyGameplayTierReasonSnapshot& PolicyReason = Enemy->GetGameplayTierReasonSnapshot();
			GameplayReasonSampled += PolicyReason.bHasPolicySample ? 1 : 0;
			GameplayDistanceValid += PolicyReason.bDistanceSampleValid ? 1 : 0;
			const bool bGameplayFull = Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Full;
			const bool bReasonKnown = PolicyReason.bHasPolicySample && PolicyReason.AppliedTier == Enemy->GetGameplaySignificanceTier();
			if (bGameplayFull)
			{
				FullByDistance += bReasonKnown && PolicyReason.bFullByDistance ? 1 : 0;
				FullByAttack += bReasonKnown && PolicyReason.bFullByAttack ? 1 : 0;
				FullByMeleeRange += bReasonKnown && PolicyReason.bFullByMeleeRange ? 1 : 0;
				FullReasonUnknown += !bReasonKnown || (!PolicyReason.bFullByDistance && !PolicyReason.bFullByAttack && !PolicyReason.bFullByMeleeRange) ? 1 : 0;
			}
			FString FullReasons;
			const auto AddReason = [&FullReasons](const TCHAR* Name)
			{
				if (!FullReasons.IsEmpty()) FullReasons += TEXT(",");
				FullReasons += Quote(Name);
			};
			if (!bReasonKnown) AddReason(TEXT("Unknown"));
			else
			{
				if (PolicyReason.bFullByDistance) AddReason(TEXT("Distance"));
				if (PolicyReason.bFullByAttack) AddReason(TEXT("Attacking"));
				if (PolicyReason.bFullByMeleeRange) AddReason(TEXT("MeleeRangeWhileNotAttacking"));
			}
			const USkeletalMeshComponent* DetailMesh = Enemy->GetMesh();
			const UCharacterMovementComponent* DetailMovement = Enemy->GetCharacterMovement();
			const FString Leader = DetailMesh != nullptr && DetailMesh->LeaderPoseComponent.IsValid()
				? Quote(DetailMesh->LeaderPoseComponent->GetPathName()) : TEXT("null");
			if (!EnemyDetails.IsEmpty()) EnemyDetails += TEXT(",");
			EnemyDetails += FString::Printf(TEXT("{\"actor\":%s,\"objectId\":%u,\"gameplayTier\":%d,\"renderTier\":%d,\"hasPolicySample\":%s,\"sampleWorldTimeSeconds\":%s,\"sampleAgeSeconds\":%s,\"distanceSampleValid\":%s,\"nearestPlayerDistance2D\":%s,\"sampleSignificanceScore\":%s,\"sampleAppliedTier\":%s,\"usedScopedCombatSample\":%s,\"lastSampleFullReasons\":[%s],\"movementTickIntervalSeconds\":%s,\"animationTickIntervalSeconds\":%s,\"leaderPose\":%s,\"attackingNow\":%s,\"fullAuthorityPoseNow\":%s,\"meshTickEnabled\":%s,\"meshLODCount\":%d,\"appliedMinimumLOD\":%d,\"predictedLOD\":%d}"),
				*Quote(Enemy->GetPathName()), Enemy->GetUniqueID(), int32(Enemy->GetGameplaySignificanceTier()), int32(Enemy->GetRenderSignificanceTier()),
				Boolean(PolicyReason.bHasPolicySample),
				*(PolicyReason.bHasPolicySample ? FString::Printf(TEXT("%.6f"), PolicyReason.SampleWorldTimeSeconds) : FString(TEXT("null"))),
				*(PolicyReason.bHasPolicySample ? FString::Printf(TEXT("%.6f"), FMath::Max(0.0, SnapshotWorldTime - PolicyReason.SampleWorldTimeSeconds)) : FString(TEXT("null"))),
				Boolean(PolicyReason.bDistanceSampleValid),
				*(PolicyReason.bDistanceSampleValid ? FString::Printf(TEXT("%.6f"), PolicyReason.NearestPlayerDistance2D) : FString(TEXT("null"))),
				*(PolicyReason.bHasPolicySample && FMath::IsFinite(PolicyReason.SignificanceScore) ? FString::Printf(TEXT("%.9g"), PolicyReason.SignificanceScore) : FString(TEXT("null"))),
				*(PolicyReason.bHasPolicySample ? FString::FromInt(int32(PolicyReason.AppliedTier)) : FString(TEXT("null"))),
				Boolean(PolicyReason.bUsedScopedCombatSample), *FullReasons,
				*(DetailMovement != nullptr ? FString::Printf(TEXT("%.9f"), DetailMovement->GetComponentTickInterval()) : FString(TEXT("null"))),
				*(DetailMesh != nullptr ? FString::Printf(TEXT("%.9f"), DetailMesh->GetComponentTickInterval()) : FString(TEXT("null"))),
				*Leader, Boolean(Enemy->IsAttacking()), Boolean(Enemy->RequiresFullAuthorityPose()),
				Boolean(DetailMesh != nullptr && DetailMesh->IsComponentTickEnabled()), DetailMesh != nullptr ? DetailMesh->GetNumLODs() : 0,
				Enemy->GetAppliedMinimumLOD(), DetailMesh != nullptr ? DetailMesh->GetPredictedLODLevel() : INDEX_NONE);
			// 同一观察器服务旧凸包/新盒体 A/B；按实际组件分类，不以启动参数猜几何。
			// 原生盒只读 ShapeBodySetup；静态网格只读资产现有 BodySetup，不触发 Cook。
			int32 EnemyHitBoxes = 0;
			TInlineComponentArray<UPrimitiveComponent*> Shapes;
			Enemy->GetComponents(Shapes);
			for (const UPrimitiveComponent* Component : Shapes)
			{
				if (!Component->ComponentHasTag(TEXT("EnemyHitProxy"))) continue;
				const UBoxComponent* Box = Cast<UBoxComponent>(Component);
				const UStaticMeshComponent* Hull = Cast<UStaticMeshComponent>(Component);
				if (Box == nullptr && Hull == nullptr)
				{
					++UnexpectedHitProxyComponents;
					continue;
				}
				++EnemyHitBoxes;
				++HitProxyComponents;
				BoxComponents += Box != nullptr ? 1 : 0;
				ConvexMeshComponents += Hull != nullptr ? 1 : 0;
				const ECollisionEnabled::Type Collision = Component->GetCollisionEnabled();
				QueryEnabledHitProxies += Collision == ECollisionEnabled::QueryOnly || Collision == ECollisionEnabled::QueryAndPhysics ? 1 : 0;
				HitProxyPhysicsStates += Component->IsPhysicsStateCreated() ? 1 : 0;
				const UStaticMesh* HullAsset = Hull != nullptr ? Hull->GetStaticMesh() : nullptr;
				if (HullAsset != nullptr) SharedHullAssets.Add(HullAsset);
				const UBodySetup* Body = Box != nullptr ? Box->ShapeBodySetup.Get() : HullAsset != nullptr ? HullAsset->GetBodySetup() : nullptr;
				if (Body != nullptr)
				{
					++HitProxyBodySetups;
					BoxHitElements += Body->AggGeom.BoxElems.Num();
					ConvexHitElements += Body->AggGeom.ConvexElems.Num();
					OtherHitElements += Body->AggGeom.GetElementCount() - Body->AggGeom.BoxElems.Num() - Body->AggGeom.ConvexElems.Num();
					InvalidHitProxyBodySetups += (Box != nullptr ? Body->AggGeom.BoxElems.Num() != 1 : Body->AggGeom.ConvexElems.Num() != 1)
						|| Body->AggGeom.GetElementCount() != 1 ? 1 : 0;
				}
				else if (Hull != nullptr || Component->IsPhysicsStateCreated()) ++InvalidHitProxyBodySetups;
			}
			MinHitBoxesPerEnemy = FMath::Min(MinHitBoxesPerEnemy, EnemyHitBoxes);
			MaxHitBoxesPerEnemy = FMath::Max(MaxHitBoxesPerEnemy, EnemyHitBoxes);
			const USkeletalMeshComponent* Mesh = Enemy->GetMesh();
			if (Mesh == nullptr) continue;
			++Meshes;
			MeshTickEnabled += Mesh->IsComponentTickEnabled() ? 1 : 0;
			EveryFrameMeshTicks += Mesh->IsComponentTickEnabled() && Mesh->GetComponentTickInterval() <= KINDA_SMALL_NUMBER ? 1 : 0;
			URO += Mesh->bEnableUpdateRateOptimizations ? 1 : 0;
			LeaderFollowers += Mesh->LeaderPoseComponent.IsValid() ? 1 : 0;
			PostDisabled += Mesh->GetDisablePostProcessBlueprint() ? 1 : 0;
			MeshCollisionEnabled += Mesh->GetCollisionEnabled() != ECollisionEnabled::NoCollision ? 1 : 0;
			MeshPhysicsStates += Mesh->IsPhysicsStateCreated() ? 1 : 0;
			for (const FBodyInstance* Body : Mesh->Bodies) ValidMeshBodyInstances += Body != nullptr && Body->IsValidBodyInstance() ? 1 : 0;
			ConstraintEntries += Mesh->Constraints.Num();
			++VisibilityOptions[FMath::Clamp(int32(Mesh->VisibilityBasedAnimTickOption), 0, 4)];
			if (const UAnimInstance* Anim = Mesh->GetAnimInstance())
			{
				++MainInstances;
				MainMultithreaded += Anim->bUseMultiThreadedAnimationUpdate ? 1 : 0;
				MainCanParallel += Anim->CanRunParallelWork() ? 1 : 0;
				MainRootMotionEverything += Anim->RootMotionMode == ERootMotionMode::RootMotionFromEverything ? 1 : 0;
			}
			if (const UAnimInstance* Post = Mesh->GetPostProcessInstance())
			{
				++PostInstances;
				PostCanParallel += Post->CanRunParallelWork() ? 1 : 0;
			}
			if (const UPhysicsAsset* Asset = Mesh->GetPhysicsAsset()) ++AssetReferences.FindOrAdd(Asset);
		}
		FString Json = FString::Printf(TEXT("{\"netMode\":%d,\"dedicatedServer\":%s,\"shouldUseThreadingForPerformance\":%s,\"forceUsePerfThreads\":%s,\"oneThread\":%s,\"noPerfThreads\":%s,\"projectAllowsMultithreadedAnimation\":%s,\"enemies\":%d,\"simpleHitProxyEnemies\":%d,\"fullAuthorityPoseEnemies\":%d,\"cvars\":{"),
			Current != nullptr ? int32(Current->GetNetMode()) : -1, Boolean(IsRunningDedicatedServer()), Boolean(FApp::ShouldUseThreadingForPerformance()),
			Boolean(FParse::Param(FCommandLine::Get(), TEXT("useperfthreads"))), Boolean(FParse::Param(FCommandLine::Get(), TEXT("onethread"))),
			Boolean(FParse::Param(FCommandLine::Get(), TEXT("noperfthreads"))), Boolean(GetDefault<UEngine>()->bAllowMultiThreadedAnimationUpdate),
			Alive, SimpleEnemies, FullAuthorityPose);
		const TCHAR* Names[] = {TEXT("a.ParallelAnimUpdate"), TEXT("a.ParallelAnimEvaluation"), TEXT("a.ForceParallelAnimUpdate"), TEXT("a.ParallelAnimInterpolation")};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
		{
			if (Index > 0) Json += TEXT(",");
			const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Names[Index]);
			Json += Quote(Names[Index]) + TEXT(":") + (CVar != nullptr ? FString::FromInt(CVar->GetInt()) : TEXT("null"));
		}
		Json += FString::Printf(TEXT("},\"mesh\":{\"count\":%d,\"tickEnabled\":%d,\"everyFrameTick\":%d,\"uroEnabled\":%d,\"leaderPoseFollowers\":%d,\"postProcessDisabled\":%d,\"collisionEnabled\":%d,\"physicsStatesCreated\":%d,\"validBodyInstances\":%d,\"constraintInstanceEntries\":%d,\"gameplayTiers\":[%d,%d,%d],\"renderTiers\":[%d,%d,%d],\"visibilityOptionsByEnum\":[%d,%d,%d,%d,%d]},"),
			Meshes, MeshTickEnabled, EveryFrameMeshTicks, URO, LeaderFollowers, PostDisabled, MeshCollisionEnabled, MeshPhysicsStates, ValidMeshBodyInstances, ConstraintEntries,
			GameplayTiers[0], GameplayTiers[1], GameplayTiers[2], RenderTiers[0], RenderTiers[1], RenderTiers[2], VisibilityOptions[0], VisibilityOptions[1], VisibilityOptions[2], VisibilityOptions[3], VisibilityOptions[4]);
		const TCHAR* HitGeometry = BoxComponents > 0 && ConvexMeshComponents == 0 && UnexpectedHitProxyComponents == 0 ? TEXT("NativeBox")
			: ConvexMeshComponents > 0 && BoxComponents == 0 && UnexpectedHitProxyComponents == 0 ? TEXT("SharedConvex") : TEXT("MixedOrMissing");
		Json += FString::Printf(TEXT("\"instances\":{\"main\":%d,\"mainMultithreadedFlag\":%d,\"mainCanRunParallelWork\":%d,\"mainRootMotionFromEverything\":%d,\"postProcess\":%d,\"postCanRunParallelWork\":%d},\"movement\":{\"alwaysCheckFloorEnemies\":%d},\"hitProxyComponents\":{\"schemaVersion\":3,\"geometry\":\"%s\",\"nativeBoxComponents\":%d,\"staticMeshComponents\":%d,\"uniqueHullAssets\":%d,\"count\":%d,\"minPerEnemy\":%d,\"maxPerEnemy\":%d,\"unexpectedTaggedComponents\":%d,\"queryEnabled\":%d,\"physicsStatesCreated\":%d,\"bodySetupsPresent\":%d,\"invalidBodySetups\":%d,\"boxElements\":%d,\"convexElements\":%d,\"otherElements\":%d},\"physicsAssetConfiguration\":["),
			MainInstances, MainMultithreaded, MainCanParallel, MainRootMotionEverything, PostInstances, PostCanParallel, AlwaysCheckFloorEnemies,
			HitGeometry, BoxComponents, ConvexMeshComponents, SharedHullAssets.Num(),
			HitProxyComponents, Alive > 0 ? MinHitBoxesPerEnemy : 0, MaxHitBoxesPerEnemy, UnexpectedHitProxyComponents,
			QueryEnabledHitProxies, HitProxyPhysicsStates, HitProxyBodySetups, InvalidHitProxyBodySetups, BoxHitElements, ConvexHitElements, OtherHitElements);
		TArray<const UPhysicsAsset*> Assets;
		AssetReferences.GetKeys(Assets);
		Assets.Sort([](const UPhysicsAsset& Left, const UPhysicsAsset& Right) { return Left.GetPathName() < Right.GetPathName(); });
		for (int32 Index = 0; Index < Assets.Num(); ++Index)
		{
			const UPhysicsAsset* Asset = Assets[Index];
			int32 Bodies = 0, Shapes = 0;
			for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
				if (Body != nullptr) { ++Bodies; Shapes += Body->AggGeom.GetElementCount(); }
			if (Index > 0) Json += TEXT(",");
			Json += FString::Printf(TEXT("{\"asset\":%s,\"meshReferences\":%d,\"authoredBodies\":%d,\"authoredShapes\":%d,\"authoredConstraints\":%d}"),
				*Quote(Asset->GetPathName()), AssetReferences.FindChecked(Asset), Bodies, Shapes, Asset->ConstraintSetup.Num());
		}
		Json += FString::Printf(TEXT("],\"gameplayFullReasons\":{\"sampleWorldTimeSeconds\":%.6f,\"sampledEnemies\":%d,\"unsampledEnemies\":%d,\"validDistanceSamples\":%d,\"distance\":%d,\"attacking\":%d,\"meleeRangeWhileNotAttacking\":%d,\"unknown\":%d,\"countsCanOverlap\":true,\"distanceSource\":\"Inverse of last applied max-player 1/(1+2D-distance) score, not a fresh player query\"},\"enemyDetails\":[%s]"),
			SnapshotWorldTime, GameplayReasonSampled, Alive - GameplayReasonSampled, GameplayDistanceValid,
			FullByDistance, FullByAttack, FullByMeleeRange, FullReasonUnknown, *EnemyDetails);
		// Only observe the authoritative ledger at capture boundaries; never request a
		// seat here, or a performance observer would change the workload it measures.
		Json += TEXT(",\"tacticalManagers\":[");
		bool bFirstManager = true;
		if (Current != nullptr) for (TActorIterator<AfpstrueSurroundManager> It(Current); It; ++It)
		{
			if (It->IsActorBeingDestroyed()) continue;
			const FFPEnemyTacticalDebugSnapshot Slots = It->GetTacticalDebugSnapshot();
			if (!bFirstManager) Json += TEXT(",");
			bFirstManager = false;
			Json += FString::Printf(TEXT("{\"manager\":%s,\"innerCapacity\":%d,\"outerCapacity\":%d,\"usableInnerSlots\":%d,\"usableOuterSlots\":%d,\"occupiedInnerSlots\":%d,\"occupiedOuterSlots\":%d,\"queuedEnemies\":%d,\"activeAttackers\":%d,\"grantCount\":%llu,\"releaseCount\":%llu,\"timeoutCount\":%llu}"),
				*Quote(It->GetName()), Slots.InnerCapacity, Slots.OuterCapacity, Slots.UsableInnerSlots, Slots.UsableOuterSlots,
				Slots.OccupiedInnerSlots, Slots.OccupiedOuterSlots, Slots.QueuedEnemies, Slots.ActiveAttackers,
				Slots.GrantCount, Slots.ReleaseCount, Slots.TimeoutCount);
		}
		Json += TEXT("]");
		Json += TEXT(",\"boundary\":\"Two GameThread boundary snapshots outside timed frame capture. Per-enemy reasons describe the last applied gameplay sample, not fresh LOS or current target acquisition; null means unknown, never zero distance. Authored PhysicsAsset counts are configuration, not active Chaos shape counts; valid BodyInstance/created-state counts are separate. Eligibility flags and LeaderPose links do not prove dispatch or worker execution; verify in an independent Trace. No CVar, instance, collision or LOD is changed by this observer.\"}");
		return Json;
	}

	// ★ 两个边界才读累计流量；运行中只校验连接身份/状态，不能把重连后的新计数拼接成同一窗口。
	bool CollectNetworkConnections(TArray<UNetConnection*>& Connections)
	{
		UNetDriver* Driver = World.IsValid() ? World->GetNetDriver() : nullptr;
		if (!Driver || (NetworkDriver.IsValid() && NetworkDriver.Get() != Driver)) return Fail(TEXT("NetworkDriverChanged"));
		if (Role == TEXT("Server"))
		{
			if (Driver->ServerConnection != nullptr || Driver->ClientConnections.Num() != 2) return Fail(TEXT("ExpectedTwoServerConnections"));
			for (UNetConnection* Connection : Driver->ClientConnections) Connections.Add(Connection);
		}
		else
		{
			if (!Driver->ServerConnection || !Driver->ClientConnections.IsEmpty()) return Fail(TEXT("ExpectedOneClientServerConnection"));
			Connections.Add(Driver->ServerConnection);
		}
		for (int32 Index = 0; Index < Connections.Num(); ++Index)
		{
			UNetConnection* Connection = Connections[Index];
			if (!IsValid(Connection) || Connection->GetDriver() != Driver || Connection->GetConnectionState() != USOCK_Open
				|| Connection->GetUChildConnection() || !Connection->Children.IsEmpty() || Connection->IsInternalAck())
				return Fail(TEXT("NetworkConnectionNotOpenOrNotDedicatedPeer"));
			for (int32 Previous = 0; Previous < Index; ++Previous)
				if (Connections[Previous] == Connection) return Fail(TEXT("DuplicateNetworkConnection"));
		}
		if (!NetworkStart.Connections.IsEmpty())
		{
			if (NetworkStart.Connections.Num() != Connections.Num()) return Fail(TEXT("NetworkConnectionCountChanged"));
			for (const FConnectionCounters& Initial : NetworkStart.Connections)
				if (!Initial.Connection.IsValid() || !Connections.Contains(Initial.Connection.Get())) return Fail(TEXT("NetworkConnectionReplaced"));
		}
		NetworkDriver = Driver;
		return true;
	}

	// 在 START/STOP 读取稳定父连接的累计计数；连接身份和计数异常使本轮无效。
	bool ReadNetworkBoundary(FNetworkBoundary& Boundary)
	{
		TArray<UNetConnection*> Connections;
		if (!CollectNetworkConnections(Connections)) return false;
		Boundary.Seconds = FPlatformTime::Seconds(); Boundary.Qpc = FPlatformTime::Cycles64();
		for (UNetConnection* Connection : Connections)
		{
			FConnectionCounters Counters;
			Counters.Connection = Connection; Counters.ObjectId = Connection->GetUniqueID();
			Counters.EngineConnectionId = Connection->GetConnectionHandle().GetParentConnectionId();
			Counters.ObjectName = Connection->GetPathName(); Counters.RemoteAddress = Connection->LowLevelGetRemoteAddress(true);
			Counters.InBytes = Connection->InTotalBytes; Counters.OutBytes = Connection->OutTotalBytes;
			Counters.InPackets = Connection->InTotalPackets; Counters.OutPackets = Connection->OutTotalPackets;
			// UE5.5 累计字段为 int32；任何负值/倒退均使本轮无效，不能用无符号转换隐藏回绕或重置。
			if (Counters.RemoteAddress.IsEmpty() || Counters.InBytes < 0 || Counters.OutBytes < 0 || Counters.InPackets < 0 || Counters.OutPackets < 0)
				return Fail(TEXT("NetworkCounterNegativeOrAddressMissing"));
			Boundary.Connections.Add(MoveTemp(Counters));
		}
		return true;
	}

	// 配对起止连接并输出字节/包速率；不合并重连计数，也不声称是每 Actor 或网卡流量。
	bool BuildNetworkJson(FString& Json)
	{
		const double Duration = NetworkStop.Seconds - NetworkStart.Seconds;
		if (!NetworkDriver.IsValid() || NetworkStart.Connections.IsEmpty() || NetworkIdentitySamples < 100
			|| !FMath::IsFinite(Duration) || Duration < 59.8 || Duration > 75
			|| FMath::Abs(Duration - (StopAt - CaptureAt)) > 0.25) return Fail(TEXT("NetworkCounterWindowInvalid"));
		Json = FString::Printf(TEXT("{\"valid\":true,\"driverObjectId\":%u,\"driverName\":%s,\"expectedConnections\":%d,\"identitySamples\":%u,\"startSeconds\":%.9f,\"stopSeconds\":%.9f,\"windowSeconds\":%.9f,\"startQpc\":%llu,\"stopQpc\":%llu,\"secondsPerCycle64\":%.17g,\"connections\":["),
			NetworkDriver->GetUniqueID(), *Quote(NetworkDriver->GetPathName()), Role == TEXT("Server") ? 2 : 1, NetworkIdentitySamples,
			NetworkStart.Seconds, NetworkStop.Seconds, Duration, NetworkStart.Qpc, NetworkStop.Qpc, FPlatformTime::GetSecondsPerCycle64());
		int64 Totals[4] = {};
		for (int32 Index = 0; Index < NetworkStart.Connections.Num(); ++Index)
		{
			const FConnectionCounters& Begin = NetworkStart.Connections[Index];
			const FConnectionCounters* End = NetworkStop.Connections.FindByPredicate([&](const FConnectionCounters& Item) { return Item.Connection == Begin.Connection; });
			if (!End || Begin.ObjectId != End->ObjectId || Begin.EngineConnectionId != End->EngineConnectionId
				|| Begin.ObjectName != End->ObjectName || Begin.RemoteAddress != End->RemoteAddress) return Fail(TEXT("NetworkIdentityChangedAtStop"));
			if (Index > 0) Json += TEXT(",");
			Json += FString::Printf(TEXT("{\"objectId\":%u,\"engineConnectionId\":%u,\"objectName\":%s,\"remoteAddress\":%s"),
				Begin.ObjectId, Begin.EngineConnectionId, *Quote(Begin.ObjectName), *Quote(Begin.RemoteAddress));
			const int64 Starts[] = {Begin.InBytes, Begin.OutBytes, Begin.InPackets, Begin.OutPackets};
			const int64 Ends[] = {End->InBytes, End->OutBytes, End->InPackets, End->OutPackets};
			const TCHAR* Names[] = {TEXT("inBytes"), TEXT("outBytes"), TEXT("inPackets"), TEXT("outPackets")};
			for (int32 Counter = 0; Counter < 4; ++Counter)
			{
				if (Ends[Counter] < Starts[Counter] || Ends[Counter] > MAX_int32) return Fail(TEXT("NetworkCounterRegressedOrOverflowed"));
				const int64 Delta = Ends[Counter] - Starts[Counter]; Totals[Counter] += Delta;
				Json += FString::Printf(TEXT(",\"%s\":{\"start\":%lld,\"end\":%lld,\"delta\":%lld,\"perSecond\":%.9f}"), Names[Counter], Starts[Counter], Ends[Counter], Delta, double(Delta) / Duration);
			}
			Json += TEXT("}");
		}
		Json += TEXT("],\"totals\":{");
		const TCHAR* Names[] = {TEXT("inBytes"), TEXT("outBytes"), TEXT("inPackets"), TEXT("outPackets")};
		for (int32 Counter = 0; Counter < 4; ++Counter)
		{
			if (Counter > 0) Json += TEXT(",");
			Json += FString::Printf(TEXT("\"%s\":{\"delta\":%lld,\"perSecond\":%.9f}"), Names[Counter], Totals[Counter], double(Totals[Counter]) / Duration);
		}
		Json += TEXT("},\"accounting\":\"Selected World's UNetDriver parent connections, direct UNetConnection In/OutTotalBytes and In/OutTotalPackets snapshots once at START and STOP on GameThread; includes connection protocol/ACK and engine PacketOverhead accounting, not NIC capture, payload-only bytes or per-Actor traffic. Roles have independently observed START/STOP boundaries; do not require exact cross-role byte equality. int32 wrap/reset/replaced/disconnected connections invalidate the run; no counter clamping or reconnect concatenation.\"}");
		return true;
	}

	// 用编译宏报告真实构建配置，不根据启动参数或文件名猜测 Shipping。
	static const TCHAR* Configuration()
	{
#if UE_BUILD_SHIPPING
		return TEXT("Shipping");
#elif UE_BUILD_DEVELOPMENT
		return TEXT("Development");
#elif UE_BUILD_TEST
		return TEXT("Test");
#else
		return TEXT("Debug");
#endif
	}

	// File replacement is atomic at the protocol boundary. The parent tolerates
	// short sharing violations; each process is the sole writer of its role files.
	bool SaveAtomic(const FString& Name, const FString& Contents)
	{
		const FString Path = FPaths::Combine(Directory, Role + Name);
		const FString Temporary = Path + TEXT(".tmp");
		return FFileHelper::SaveStringToFile(Contents, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) &&
			IFileManager::Get().Move(*Path, *Temporary, true, false, false, true);
	}

	// 为状态文件和最终报告生成同一份角色、阶段、视角及样本数量元数据。
	FString CommonJson() const
	{
		const double Duration = CaptureAt > 0 ? (StopAt > 0 ? StopAt : FPlatformTime::Seconds()) - CaptureAt : 0;
		return FString::Printf(TEXT("\"schemaVersion\":2,\"phase\":%s,\"reason\":%s,\"role\":%s,\"pid\":%u,\"enemies\":%d,\"players\":%d,\"width\":%d,\"height\":%d,\"viewLocation\":[%.6f,%.6f,%.6f],\"viewRotation\":[%.6f,%.6f,%.6f],\"frames\":%d,\"captureSeconds\":%.6f,\"buildConfiguration\":%s,\"isShipping\":%s,\"nullRhi\":%s,\"rendered\":%s,\"viewPolicy\":%s,\"fov\":%.6f,\"enemyReplication\":%s"),
			*Quote(Phase), *Quote(Reason), *Quote(Role), FPlatformProcess::GetCurrentProcessId(), Enemies, Players, Width, Height,
			ViewLocation.X, ViewLocation.Y, ViewLocation.Z, ViewRotation.Pitch, ViewRotation.Yaw, ViewRotation.Roll,
			Frames.Num(), Duration, *Quote(Configuration()), UE_BUILD_SHIPPING ? TEXT("true") : TEXT("false"),
			GUsingNullRHI ? TEXT("true") : TEXT("false"), bRendered ? TEXT("true") : TEXT("false"), *Quote(ViewPolicy), ViewFOV,
			*Quote(EnemyReplication));
	}

	// 原子发布角色阶段；状态文件写失败时退出，不能让父脚本等待一份失真的状态。
	bool Publish(const TCHAR* NewPhase)
	{
		Phase = NewPhase;
		if (!SaveAtomic(TEXT(".status.json"), TEXT("{") + CommonJson() + TEXT("}\n")))
		{
			bStatusFailed = bFailed = true;
			FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("PvpBaselineStatusWrite"));
			return false;
		}
		return true;
	}

	// 保留首个失败原因，封存失败边界并停止收集，以非零退出码拒收该轮。
	bool Fail(const TCHAR* Failure)
	{
		if (!bFailed) Reason = Failure;
		bFailed = true;
		if (bCapturing) CaptureStopAnchor = MarkCaptureBoundary(TEXT("FAILED"));
		bCapturing = false;
		StopUnitConsumer();
		StopAt = FPlatformTime::Seconds();
		Publish(TEXT("FAILED"));
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("PvpBaselineFailed"));
		return false;
	}

	// 为当前采样 World 记录 wall/线程 CPU 起点，不计入其他 World 的工作。
	void BeginWorld(UWorld* InWorld, ELevelTick, float)
	{
		if (bCapturing && InWorld == World.Get())
		{
			WorldStartAt = FPlatformTime::Seconds();
			WorldBeginCpu = ReadGameThreadCpu();
		}
	}

	// 配对 World 结束样本并记录帧身份，供 EndFrame 判断 wall/CPU 是否属于同一帧。
	void EndWorld(UWorld* InWorld, ELevelTick, float)
	{
		if (bCapturing && InWorld == World.Get() && WorldStartAt > 0)
		{
			LastWorldEndCpu = ReadGameThreadCpu();
			LastWorldBeginCpu = WorldBeginCpu;
			LastWorldMs = (FPlatformTime::Seconds() - WorldStartAt) * 1000;
			LastWorldFrame = GFrameCounter;
			TRACE_COUNTER_SET(FpBaselineWorldWall, LastWorldMs);
			int64 User = 0, Kernel = 0;
			const bool bWorldCpu = CpuDifference(LastWorldBeginCpu, LastWorldEndCpu, User, Kernel);
			TRACE_COUNTER_SET(FpBaselineWorldCpuValid, bWorldCpu ? 1 : 0);
			if (LastWorldEndCpu.bValid)
			{
				TRACE_COUNTER_SET(FpBaselineGTUser, int64(LastWorldEndCpu.User100ns));
				TRACE_COUNTER_SET(FpBaselineGTKernel, int64(LastWorldEndCpu.Kernel100ns));
				if (bWorldCpu)
				{
					TRACE_COUNTER_SET(FpBaselineWorldCpu, double(User + Kernel) * 0.0001);
				}
			}
			WorldStartAt = 0;
		}
	}

	// 保存连续完整帧间隔，并关联同帧 World、CPU 和节拍样本；不把无效配对补成零。
	void EndFrame()
	{
		if (!bCapturing) return;
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueBaseline_RecordEndFrame);
		const double Now = FPlatformTime::Seconds();
		const FThreadCpuSample EndCpu = ReadGameThreadCpu();
		if (LastEndAt > 0)
		{
			if (Frames.Num() >= MaxFrames) { Fail(TEXT("BoundedFrameBufferExceeded")); return; }
			const double Interval = (Now - LastEndAt) * 1000;
			if (!FMath::IsFinite(Interval) || Interval <= 0) { Fail(TEXT("InvalidFrameClock")); return; }
			const bool bSameWorldFrame = LastWorldFrame == GFrameCounter;
			Frames.Add({GFrameCounter, Now - CaptureAt, Interval, LastWorldMs, bSameWorldFrame});
			if (bCpuAccounting) CpuFrames.Add({PreviousEndCpu, EndCpu,
				bSameWorldFrame ? LastWorldBeginCpu : FThreadCpuSample{}, bSameWorldFrame ? LastWorldEndCpu : FThreadCpuSample{}});
			if (bCpuAccounting) PacingFrames.Add(ReadFramePacing());
		}
		LastEndAt = Now;
		PreviousEndCpu = EndCpu;
	}

	// 非阻塞读取 WAIT/START/STOP/EXIT；文件短暂被占用时下轮重试，持续故障判失败。
	bool ReadControl(FString& Command)
	{
		const int64 Bytes = IFileManager::Get().FileSize(*ControlPath);
		if (Bytes > 64) return Fail(TEXT("OversizedControlFile"));
		if (Bytes < 1 || !FFileHelper::LoadFileToString(Command, *ControlPath))
		{
			if (UnreadableAt < 0) UnreadableAt = FPlatformTime::Seconds();
			if (FPlatformTime::Seconds() - UnreadableAt >= 1) return Fail(TEXT("ControlUnavailable"));
			return false;
		}
		UnreadableAt = -1;
		Command.TrimStartAndEndInline();
		if (Command != TEXT("WAIT") && Command != TEXT("START") && Command != TEXT("STOP") && Command != TEXT("EXIT"))
			return Fail(TEXT("InvalidControlCommand"));
		return true;
	}

	// Count replicas, not just server intent. No fake AI, damage suppression or
	// Shipping test RPCs. A fixed tagged camera must actually become the local view.
	bool SampleWorld()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueBaseline_ValidateWorldAndView);
		UWorld* Current = World.Get();
		if (Current == nullptr || Current->GetWorldSettings() == nullptr || Current->IsPaused() ||
			!FMath::IsNearlyEqual(Current->GetWorldSettings()->GetEffectiveTimeDilation(), 1.0f))
			return Fail(TEXT("WorldOrTimeChanged"));
		Enemies = Players = 0;
		AIControllers = BehaviorReady = Chasing = Attacking = Moving = 0;
		Bars = {};
		for (TActorIterator<AfpstrueEnemyCharacter> It(Current); It; ++It)
		{
			if (const auto* Bar = It->FindComponentByClass<UfpstrueEnemyHealthBarComponent>()) Bar->AppendMetrics(Bars);
			if (It->IsActorBeingDestroyed() || It->IsDead()) continue;
			++Enemies;
			if (Role == TEXT("Server"))
			{
				if (const auto* AI = Cast<AfpstrueEnemyAIController>(It->GetController()))
				{
					++AIControllers;
					BehaviorReady += AI->IsBehaviorReady() ? 1 : 0;
					if (AI->GetAIState() == EFPEnemyAIState::Chase) ++Chasing;
					if (AI->GetAIState() == EFPEnemyAIState::Attack) ++Attacking;
				}
				if (It->GetVelocity().SizeSquared2D() > FMath::Square(5.f)) ++Moving;
			}
#if !UE_BUILD_SHIPPING
			if (Role == TEXT("Server") && !PreparedEnemies.Contains(*It))
			{
				It->SetNetCullDistanceSquared(1.0e12f);
				It->ForceNetUpdate();
				PreparedEnemies.Add(*It);
			}
#endif
		}
		if (Role == TEXT("Server"))
		{
#if !UE_BUILD_SHIPPING
			auto* Mode = Current->GetAuthGameMode<AfpstruePvpGameMode>();
			if (Mode == nullptr) return Fail(TEXT("WrongServerGameMode"));
			for (TActorIterator<AfpstrueCharacter> It(Current); It; ++It)
			{
				if (!Mode->IsCombatParticipant(*It) || Cast<APlayerController>(It->GetController()) == nullptr) continue;
				++Players;
				if (!PreparedPlayers.Contains(*It))
				{
					if (auto* Health = It->GetHealthComponent()) Health->SetMaxHealthAndReset(1000000.f);
					else return Fail(TEXT("PlayerHealthMissing"));
					PreparedPlayers.Add(*It);
				}
			}
			return true;
#else
			return Fail(TEXT("ShippingServerPreparationUnsupported"));
#endif
		}
		if (bRendered && !bQualityApplied)
		{
			// Apply through the supported Shipping API, not ExecCmds/cheat CVars.
			// The launcher supplies a fresh UserDir; project Config is never saved.
			UGameUserSettings* Settings = GEngine != nullptr ? GEngine->GetGameUserSettings() : nullptr;
			if (Settings == nullptr) return Fail(TEXT("GameUserSettingsUnavailable"));
			Settings->SetScreenResolution(FIntPoint(1600, 900));
			Settings->SetFullscreenMode(EWindowMode::Windowed);
			Settings->SetVSyncEnabled(false);
			Settings->SetFrameRateLimit(0);
			Settings->SetOverallScalabilityLevel(2);
			Settings->ApplySettings(false);
			bQualityApplied = true;
			return false;
		}
		if (const auto* State = Current->GetGameState())
			for (const APlayerState* Player : State->PlayerArray) if (IsValid(Player)) ++Players;
		APlayerController* Local = UGameplayStatics::GetPlayerController(Current, 0);
		if (Local == nullptr || !Local->IsLocalController() || Local->GetPawn() == nullptr || Local->AcknowledgedPawn != Local->GetPawn())
			return false;
		if (!Camera.IsValid())
		{
			if (ViewPolicy == TEXT("Overview"))
			{
				for (TActorIterator<ACameraActor> It(Current); It; ++It)
					if (It->ActorHasTag(TEXT("EnemyUIProfileCamera"))) { Camera = *It; break; }
				if (!Camera.IsValid()) return Fail(TEXT("TaggedBaselineCameraMissing"));
			}
			else
			{
				// 保留真实第一人称组件高度和FOV，但以唯一出生点为锚；不把已被推移Pawn、
				// SpringArm运行时socket或CameraManager表现缓存的世界位置冻结成不同实验视点。
				if (Local->GetViewTarget() != Local->GetPawn() || Local->PlayerCameraManager == nullptr ||
					Local->PlayerCameraManager->GetCameraCacheTime() <= 0) return false;
				const AfpstrueCharacter* Player = Cast<AfpstrueCharacter>(Local->GetPawn());
				const UCameraComponent* PlayerCamera = Player != nullptr ? Player->GetFirstPersonCameraComponent() : nullptr;
				const USceneComponent* PawnRoot = Player != nullptr ? Player->GetRootComponent() : nullptr;
				if (PlayerCamera == nullptr || PawnRoot == nullptr) return Fail(TEXT("PlayerBaselineCameraComponentMissing"));
				const FName StartTag(*FString::Printf(TEXT("GroupBaseline%s"), *Role));
				const APlayerStart* PlayerStart = nullptr;
				for (TActorIterator<APlayerStart> It(Current); It; ++It)
				{
					if (It->PlayerStartTag != StartTag) continue;
					if (PlayerStart != nullptr) return Fail(TEXT("DuplicatePlayerBaselineStart"));
					PlayerStart = *It;
				}
				if (PlayerStart == nullptr) return Fail(TEXT("TaggedPlayerBaselineStartMissing"));
				// 只累积作者局部变换，不读取会随Pawn移动/相机延迟变化的ComponentToWorld。
				// 当前第一人称臂长为0；不支持的socket/伸臂配置明确拒绝，不悄悄近似。
				FTransform CameraRelativeToPawn = FTransform::Identity;
				TSet<const USceneComponent*> Visited;
				for (const USceneComponent* Segment = PlayerCamera; Segment != PawnRoot; Segment = Segment->GetAttachParent())
				{
					if (Segment == nullptr || Segment->GetOwner() != Player || Visited.Contains(Segment))
						return Fail(TEXT("PlayerBaselineCameraHierarchyInvalid"));
					Visited.Add(Segment);
					if (Segment->IsUsingAbsoluteLocation() || Segment->IsUsingAbsoluteRotation() || Segment->IsUsingAbsoluteScale())
						return Fail(TEXT("PlayerBaselineAbsoluteCameraUnsupported"));
					if (const USpringArmComponent* Arm = Cast<USpringArmComponent>(Segment))
						if (!FMath::IsNearlyZero(Arm->TargetArmLength) || !Arm->SocketOffset.IsNearlyZero() || !Arm->TargetOffset.IsNearlyZero())
							return Fail(TEXT("PlayerBaselineSpringArmOffsetsUnsupported"));
					if (!Segment->GetAttachSocketName().IsNone() &&
						(Cast<USpringArmComponent>(Segment->GetAttachParent()) == nullptr || Segment->GetAttachSocketName() != USpringArmComponent::SocketName))
						return Fail(TEXT("PlayerBaselineAnimatedSocketUnsupported"));
					CameraRelativeToPawn = CameraRelativeToPawn * Segment->GetRelativeTransform();
				}
				const FTransform StartTransform = PlayerStart->GetActorTransform();
				const FTransform AnchoredCamera = CameraRelativeToPawn * StartTransform;
				if (StartTransform.ContainsNaN() || CameraRelativeToPawn.ContainsNaN() || AnchoredCamera.ContainsNaN() ||
					!ensureMsgf(AnchoredCamera.GetRelativeTransform(StartTransform).Equals(CameraRelativeToPawn, 0.001f),
						TEXT("Baseline camera must preserve its authored relative transform at the tagged PlayerStart")))
					return Fail(TEXT("PlayerBaselineCameraTransformInvalid"));
				ViewLocation = AnchoredCamera.GetLocation();
				ViewRotation = AnchoredCamera.Rotator();
				const float PlayerFOV = Local->PlayerCameraManager->GetFOVAngle();
				if (!FMath::IsFinite(PlayerFOV) || PlayerFOV <= 0 || PlayerFOV >= 180) return Fail(TEXT("PlayerViewFovInvalid"));
				Camera = Current->SpawnActor<ACameraActor>(ViewLocation, ViewRotation);
				if (!Camera.IsValid()) return Fail(TEXT("PlayerBaselineCameraSpawnFailed"));
				Camera->GetCameraComponent()->SetFieldOfView(PlayerFOV);
				UE_LOG(LogTemp, Display, TEXT("PVP_BASELINE VIEW_ANCHOR role=%s startTag=%s relative=%s location=%s rotation=%s fov=%.3f"),
					*Role, *StartTag.ToString(), *CameraRelativeToPawn.ToHumanReadableString(), *ViewLocation.ToString(), *ViewRotation.ToString(), PlayerFOV);
			}
			Local->SetViewTarget(Camera.Get());
			return false;
		}
		Local->GetPlayerViewPoint(ViewLocation, ViewRotation);
		if (Local->PlayerCameraManager == nullptr) return false;
		ViewFOV = Local->PlayerCameraManager->GetFOVAngle();
		if (Local->GetViewTarget() != Camera.Get() || !ViewLocation.Equals(Camera->GetActorLocation(), 1.f) ||
			!ViewRotation.Equals(Camera->GetActorRotation(), .1f) || !FMath::IsFinite(ViewFOV) ||
			!FMath::IsNearlyEqual(ViewFOV, Camera->GetCameraComponent()->FieldOfView, .1f))
		{
			if (Phase == TEXT("STARTING")) { Local->SetViewTarget(Camera.Get()); ViewStableAt = 0; return false; }
			return Fail(TEXT("RecordedViewDrifted"));
		}
		if (ViewStableAt == 0) ViewStableAt = FPlatformTime::Seconds();
		if (FPlatformTime::Seconds() - ViewStableAt < .5) return false;
		Local->GetViewportSize(Width, Height);
		if (bRendered && (GUsingNullRHI || Width != 1600 || Height != 900)) return Fail(TEXT("RenderedViewportMismatch"));
		if (bRendered)
		{
			const auto* Settings = GEngine->GetGameUserSettings();
			if (Settings == nullptr || Settings->IsVSyncEnabled() || Settings->GetFrameRateLimit() != 0 ||
				Settings->GetOverallScalabilityLevel() != 2 || Settings->GetScreenResolution() != FIntPoint(1600, 900))
				return Fail(TEXT("BaselineQualitySettingsChanged"));
		}
		return true;
	}

	// 写独立 units.csv 并按回调帧核对覆盖；不可用/未启用计时项在 JSON 中保留 null。
	bool WriteUnitArtifacts(FString& Json)
	{
		Json = FString::Printf(TEXT("{\"schemaVersion\":1,\"enabled\":%s"), bCpuAccounting ? TEXT("true") : TEXT("false"));
		if (!bCpuAccounting) { Json += TEXT("}"); return true; }
		if (!UnitConsumer.IsValid() || UnitConsumer->bOverflowed) return Fail(TEXT("NativeUnitConsumerMissingOrOverflowed"));
		const IConsoleVariable* TickFlushCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.ReportGameTickFlushTime"));
		// Shipping 默认不启用该原生计时器；不把 disabled 状态下的静态 0 当作网络零成本。
		const bool bTickFlushTimer = STATS || (TickFlushCVar != nullptr && TickFlushCVar->GetInt() != 0);
		using FData = IPerformanceDataConsumer::FFrameData;
		struct FMetric { const TCHAR* Name; double FData::* Value; bool bRenderedOnly; bool bZeroIsMissing; };
		const FMetric Metrics[] = {
			{TEXT("GameThreadMs"), &FData::GameThreadTimeSeconds, true, true},
			{TEXT("RenderThreadMs"), &FData::RenderThreadTimeSeconds, true, true},
			{TEXT("RHIThreadMs"), &FData::RHIThreadTimeSeconds, true, true},
			{TEXT("GPUFrameMs"), &FData::GPUTimeSeconds, true, true},
			{TEXT("IdleMs"), &FData::IdleSeconds, false, false},
			{TEXT("IdleOvershootMs"), &FData::IdleOvershootSeconds, false, false},
			{TEXT("GameDriverTickFlushMs"), &FData::GameDriverTickFlushTimeSeconds, false, false},
			{TEXT("DemoDriverTickFlushMs"), &FData::DemoDriverTickFlushTimeSeconds, false, false},
			{TEXT("FlushAsyncLoadingMs"), &FData::FlushAsyncLoadingTime, false, false}};
		int32 Finite[UE_ARRAY_COUNT(Metrics)] = {}, Positive[UE_ARRAY_COUNT(Metrics)] = {}, Zero[UE_ARRAY_COUNT(Metrics)] = {};
		double Totals[UE_ARRAY_COUNT(Metrics)] = {};
		TSet<uint64> RecordedFrames, SeenFrames;
		for (const FFrame& Frame : Frames) RecordedFrames.Add(Frame.Frame);
		FString Csv(TEXT("Frame,Qpc,ElapsedSeconds,RecordedFrameMatch,NativeDeltaSeconds,NativeTrueDeltaSeconds"));
		for (const FMetric& Metric : Metrics) Csv += FString(TEXT(",")) + Metric.Name;
		Csv += TEXT(",FlushAsyncLoadingCount,SyncLoadCount,NumAsyncPackages,NativeBinThisFrame\n");
		int32 Matched = 0, Duplicates = 0;
		for (const FPvpBaselineUnits::FSample& Sample : UnitConsumer->Samples)
		{
			const bool bMatched = RecordedFrames.Contains(Sample.Frame);
			const bool bDuplicate = SeenFrames.Contains(Sample.Frame);
			SeenFrames.Add(Sample.Frame);
			Duplicates += bDuplicate ? 1 : 0;
			Matched += bMatched && !bDuplicate ? 1 : 0;
			const FData& Data = Sample.Data;
			Csv += FString::Printf(TEXT("%llu,%llu,%.9f,%d,%s,%s"), Sample.Frame, Sample.Qpc, Sample.Elapsed, bMatched ? 1 : 0,
				FMath::IsFinite(Data.DeltaSeconds) ? *FString::Printf(TEXT("%.9f"), Data.DeltaSeconds) : TEXT(""),
				FMath::IsFinite(Data.TrueDeltaSeconds) ? *FString::Printf(TEXT("%.9f"), Data.TrueDeltaSeconds) : TEXT(""));
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Metrics); ++Index)
			{
				const double Value = Data.*(Metrics[Index].Value) * 1000.0;
				const bool bFinite = FMath::IsFinite(Value) && Value >= 0;
				Csv += TEXT(",") + (bFinite ? FString::Printf(TEXT("%.9f"), Value) : FString());
				if (bMatched && !bDuplicate && bFinite)
				{
					++Finite[Index]; Totals[Index] += Value;
					Positive[Index] += Value > 0 ? 1 : 0;
					Zero[Index] += Value == 0 ? 1 : 0;
				}
			}
			Csv += FString::Printf(TEXT(",%u,%u,%d,%d\n"), Data.FlushAsyncLoadingCount, Data.SyncLoadCount, Data.NumAsyncPackages, Data.bBinThisFrame ? 1 : 0);
		}
		if (!SaveAtomic(TEXT(".units.csv"), Csv)) return Fail(TEXT("NativeUnitArtifactWriteFailed"));
		const bool bCoverage = Matched == Frames.Num() && Matched > 0 && Duplicates == 0;
		Json += FString::Printf(TEXT(",\"valid\":%s,\"recordedFrames\":%d,\"sourceSamples\":%d,\"matchedSamples\":%d,\"missingFrames\":%d,\"extraSamples\":%d,\"duplicateFrameSamples\":%d,\"tickFlushTimerEnabled\":%s,\"tickFlushReportCvar\":%s,\"metrics\":{"),
			bCoverage ? TEXT("true") : TEXT("false"), Frames.Num(), UnitConsumer->Samples.Num(), Matched, Frames.Num() - Matched,
			UnitConsumer->Samples.Num() - Matched - Duplicates, Duplicates, bTickFlushTimer ? TEXT("true") : TEXT("false"),
			TickFlushCVar != nullptr ? *FString::FromInt(TickFlushCVar->GetInt()) : TEXT("null"));
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Metrics); ++Index)
		{
			if (Index > 0) Json += TEXT(",");
			const bool bApplicable = (!Metrics[Index].bRenderedOnly || (bRendered && !GUsingNullRHI))
				&& (Index != 6 || bTickFlushTimer);
			const bool bComplete = bCoverage && bApplicable && Finite[Index] == Matched && (!Metrics[Index].bZeroIsMissing || Positive[Index] == Matched);
			Json += Quote(Metrics[Index].Name) + FString::Printf(TEXT(":{\"applicable\":%s,\"complete\":%s,\"finiteSamples\":%d,\"positiveSamples\":%d,\"zeroSamples\":%d,\"invalidSamples\":%d,\"zeroMayMeanUnavailable\":%s,\"meanMs\":%s}"),
				bApplicable ? TEXT("true") : TEXT("false"), bComplete ? TEXT("true") : TEXT("false"), Finite[Index], Positive[Index], Zero[Index], Matched - Finite[Index],
				Metrics[Index].bZeroIsMissing ? TEXT("true") : TEXT("false"), bComplete ? *FString::Printf(TEXT("%.9f"), Totals[Index] / Matched) : TEXT("null"));
		}
		Json += TEXT("},\"source\":\"UEngine::AddPerformanceDataConsumer / IPerformanceDataConsumer::FFrameData\",\"interpretation\":\"Bounded diagnostic-only native unit samples. CSV preserves raw finite zero values; zero GT/RT/RHI/GPU does not establish zero cost and suppresses its mean. NullRHI unit timings are not applicable. Idle, TickFlush and loading zero can be a measured zero. Global Frame matches the consumer callback, not exact execution or GPU frame identity: renderer counters can lag or repeat across pipeline frames. Unit times are engine wall/idle-derived counters, not GetThreadTimes execution CPU or OS Ready/Blocked states; GPU timing can be inferred/platform dependent. No sum across threads or parent/child modules is a frame-time total. NativeBinThisFrame is retained but never used to discard samples.\"}");
		return true;
	}

	// STOP 后封存边界、计数和逐帧产物，校验采样时长及完整性后才发布 FLUSHED。
	bool Flush()
	{
		bCapturing = false;
		StopUnitConsumer();
		StopAt = FPlatformTime::Seconds();
		CaptureStopAnchor = MarkCaptureBoundary(TEXT("STOP"));
		if (!ReadNetworkBoundary(NetworkStop)) return false;
		const FString AnimationStopSnapshot = BuildAnimationSnapshot();
		const FString PacingStopSnapshot = BuildPacingSnapshot();
		FString NetworkJson;
		if (!BuildNetworkJson(NetworkJson)) return false;
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueBaseline_FlushArtifacts);
		if (Frames.Num() < 100 || StopAt - CaptureAt < 59.8 || StopAt - CaptureAt > 75)
			return Fail(TEXT("CaptureDurationOrSamplesInvalid"));
		FString UnitJson;
		if (!WriteUnitArtifacts(UnitJson)) return false;
		FString Csv(TEXT("Frame,ElapsedSeconds,FrameEndIntervalMs,WorldTickWallMs,WorldTickValid,CpuAccountingEnabled,GTEndCpuValid,GTEndThreadId,GTEndCpuQueryError,GTPreviousEndUser100ns,GTPreviousEndKernel100ns,GTEndUser100ns,GTEndKernel100ns,GTFrameCpuValid,GTFrameUser100nsDelta,GTFrameKernel100nsDelta,GTFrameCpuMs,GTWorldCpuValid,GTWorldBeginUser100ns,GTWorldBeginKernel100ns,GTWorldEndUser100ns,GTWorldEndKernel100ns,GTWorldUser100nsDelta,GTWorldKernel100nsDelta,GTWorldCpuMs\n"));
		Csv.Reserve(Frames.Num() * (bCpuAccounting ? 400 : 165));
		int32 CpuValidFrames = 0, CpuValidWorlds = 0, NegativeFrameCpuDeltas = 0, NegativeWorldCpuDeltas = 0;
		int64 FrameUser100ns = 0, FrameKernel100ns = 0, WorldUser100ns = 0, WorldKernel100ns = 0;
		double CpuPairedFrameWallMs = 0, CpuPairedWorldWallMs = 0;
		for (int32 FrameIndex = 0; FrameIndex < Frames.Num(); ++FrameIndex)
		{
			const FFrame& Frame = Frames[FrameIndex];
			const FFrameCpu EmptyCpu;
			const FFrameCpu& Cpu = CpuFrames.IsValidIndex(FrameIndex) ? CpuFrames[FrameIndex] : EmptyCpu;
			Csv += FString::Printf(TEXT("%llu,%.9f,%.6f,%s,%d"), Frame.Frame, Frame.Elapsed, Frame.Interval,
				Frame.bWorldTick ? *FString::Printf(TEXT("%.6f"), Frame.WorldTick) : TEXT(""), Frame.bWorldTick ? 1 : 0);
			int64 FrameUser = 0, FrameKernel = 0, WorldUser = 0, WorldKernel = 0;
			const bool bFrameCpu = CpuDifference(Cpu.PreviousEndCpu, Cpu.EndCpu, FrameUser, FrameKernel);
			const bool bWorldCpu = Frame.bWorldTick && CpuDifference(Cpu.WorldBeginCpu, Cpu.WorldEndCpu, WorldUser, WorldKernel);
			Csv += FString::Printf(TEXT(",%d,%d,%u,%u,%s,%s,%s,%s,%d,%s,%s,%s,%d,%s,%s,%s,%s,%s,%s,%s"),
				bCpuAccounting ? 1 : 0, Cpu.EndCpu.bValid ? 1 : 0, Cpu.EndCpu.ThreadId, Cpu.EndCpu.Error,
				*CpuRaw(Cpu.PreviousEndCpu, true), *CpuRaw(Cpu.PreviousEndCpu, false), *CpuRaw(Cpu.EndCpu, true), *CpuRaw(Cpu.EndCpu, false),
				bFrameCpu ? 1 : 0, *CpuDelta(bFrameCpu, FrameUser), *CpuDelta(bFrameCpu, FrameKernel), *CpuMs(bFrameCpu, FrameUser, FrameKernel),
				bWorldCpu ? 1 : 0, *CpuRaw(Cpu.WorldBeginCpu, true), *CpuRaw(Cpu.WorldBeginCpu, false),
				*CpuRaw(Cpu.WorldEndCpu, true), *CpuRaw(Cpu.WorldEndCpu, false), *CpuDelta(bWorldCpu, WorldUser), *CpuDelta(bWorldCpu, WorldKernel), *CpuMs(bWorldCpu, WorldUser, WorldKernel));
			Csv += TEXT("\n");
			if (bFrameCpu)
			{
				++CpuValidFrames; FrameUser100ns += FrameUser; FrameKernel100ns += FrameKernel; CpuPairedFrameWallMs += Frame.Interval;
				NegativeFrameCpuDeltas += FrameUser < 0 || FrameKernel < 0 ? 1 : 0;
			}
			if (bWorldCpu)
			{
				++CpuValidWorlds; WorldUser100ns += WorldUser; WorldKernel100ns += WorldKernel; CpuPairedWorldWallMs += Frame.WorldTick;
				NegativeWorldCpuDeltas += WorldUser < 0 || WorldKernel < 0 ? 1 : 0;
			}

		}
		if (!SaveAtomic(TEXT(".frames.csv"), Csv)) return Fail(TEXT("FrameArtifactWriteFailed"));
		if (bCpuAccounting)
		{
			if (PacingFrames.Num() != Frames.Num()) return Fail(TEXT("PacingFrameCountMismatch"));
			FString PacingCsv(TEXT("Frame,AppDeltaSeconds,AppIdleSeconds,AppIdleOvershootSeconds,NetDriverValid,NetServerMaxTickRate,MaxNetTickRate,EngineQueryValid,QueryInputDeltaSeconds,QueriedEngineMaxTickRateHz\n"));
			PacingCsv.Reserve(PacingFrames.Num() * 160);
			for (int32 Index = 0; Index < PacingFrames.Num(); ++Index)
			{
				const FFramePacing& Sample = PacingFrames[Index];
				if (Sample.Frame != Frames[Index].Frame) return Fail(TEXT("PacingFrameIdentityMismatch"));
				PacingCsv += FString::Printf(TEXT("%llu,%.9f,%.9f,%.9f,%d,%s,%s,%d,%.9f,%s\n"),
					Sample.Frame, Sample.DeltaSeconds, Sample.IdleSeconds, Sample.IdleOvershootSeconds, Sample.bNetDriverValid ? 1 : 0,
					Sample.bNetDriverValid ? *FString::FromInt(Sample.NetServerMaxTickRate) : TEXT(""),
					Sample.bNetDriverValid ? *FString::FromInt(Sample.MaxNetTickRate) : TEXT(""), Sample.bEngineQueryValid ? 1 : 0,
					Sample.QueryInputDeltaSeconds, Sample.bEngineQueryValid ? *FString::Printf(TEXT("%.9f"), Sample.QueriedEngineMaxTickRate) : TEXT(""));
			}
			if (!SaveAtomic(TEXT(".pacing.csv"), PacingCsv)) return Fail(TEXT("PacingArtifactWriteFailed"));
		}
		const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
		Phase = TEXT("FLUSHED");
		FString Json = TEXT("{") + CommonJson();
		Json += TEXT(",\"networkConnections\":") + NetworkJson;
		Json += TEXT(",\"nativeUnits\":") + UnitJson;
		Json += TEXT(",\"animationDiagnostics\":{\"start\":") + AnimationStartSnapshot + TEXT(",\"stop\":") + AnimationStopSnapshot + TEXT("}");
		Json += TEXT(",\"pacingDiagnostics\":{\"start\":") + PacingStartSnapshot + TEXT(",\"stop\":") + PacingStopSnapshot
			+ TEXT(",\"perFrameDiagnostic\":") + (bCpuAccounting ? TEXT("true") : TEXT("false"))
			+ TEXT(",\"interpretation\":\"NetServerMaxTickRate is read from the live World's NetDriver. QueriedEngineMaxTickRateHz is a safe boundary/frame-end re-evaluation using the recorded FApp delta, not the local GivenMaxTickRate inside UpdateTimeAndHandleMaxTickRate; fixed-rate/benchmark overrides and platform handlers may differ. Query is null for custom engine classes, missing fault-injection CVars or enabled CauseHitches/t.UnsteadyFPS to avoid getter side effects. FApp idle/overshoot are engine-reported pacing, not scheduler Ready/Wait classification. Separate diagnostic pacing.csv retains raw values and exact global frame pairing; never subtract idle and relabel the remainder CPU execution. Default launches retain inherited pacing.\"}");
		const bool bFrameCpuPresent = CpuValidFrames > 0, bWorldCpuPresent = CpuValidWorlds > 0;
		const bool bCpuComplete = bCpuAccounting && PLATFORM_WINDOWS && CpuFrames.Num() == Frames.Num() && CpuValidFrames == Frames.Num()
			&& CpuValidWorlds == Frames.Num() && CpuQueryFailures == 0 && NegativeFrameCpuDeltas == 0 && NegativeWorldCpuDeltas == 0;
		const double FrameCpuTotalMs = double(FrameUser100ns + FrameKernel100ns) * 0.0001;
		const double WorldCpuTotalMs = double(WorldUser100ns + WorldKernel100ns) * 0.0001;
		const auto JsonNumber = [](bool bValid, double Value) { return bValid ? FString::Printf(TEXT("%.9f"), Value) : FString(TEXT("null")); };
		const auto JsonInteger = [](bool bValid, int64 Value) { return bValid ? FString::Printf(TEXT("%lld"), Value) : FString(TEXT("null")); };
		Json += FString::Printf(TEXT(",\"cpuAccounting\":{\"enabled\":%s,\"platformSupported\":%s,\"valid\":%s,\"threadId\":%u,\"osQueries\":%llu,\"osQueryFailures\":%llu,\"sampledFrameIntervals\":%d,\"validFrameCpuIntervals\":%d,\"validWorldCpuIntervals\":%d,\"negativeFrameCpuDeltas\":%d,\"negativeWorldCpuDeltas\":%d,"),
			bCpuAccounting ? TEXT("true") : TEXT("false"), PLATFORM_WINDOWS ? TEXT("true") : TEXT("false"),
			bCpuAccounting ? (bCpuComplete ? TEXT("true") : TEXT("false")) : TEXT("null"), CaptureStartAnchor.ThreadId, CpuQueryCount, CpuQueryFailures,
			Frames.Num(), CpuValidFrames, CpuValidWorlds, NegativeFrameCpuDeltas, NegativeWorldCpuDeltas);
		Json += TEXT("\"frameUserTotal100ns\":") + JsonInteger(bFrameCpuPresent, FrameUser100ns)
			+ TEXT(",\"frameKernelTotal100ns\":") + JsonInteger(bFrameCpuPresent, FrameKernel100ns)
			+ TEXT(",\"frameCpuTotalMs\":") + JsonNumber(bFrameCpuPresent, FrameCpuTotalMs)
			+ TEXT(",\"meanFrameCpuMs\":") + JsonNumber(bFrameCpuPresent, FrameCpuTotalMs / FMath::Max(1, CpuValidFrames))
			+ TEXT(",\"pairedFrameWallTotalMs\":") + JsonNumber(bFrameCpuPresent, CpuPairedFrameWallMs)
			+ TEXT(",\"frameWallMinusCpuMs\":") + JsonNumber(bFrameCpuPresent, CpuPairedFrameWallMs - FrameCpuTotalMs)
			+ TEXT(",\"worldUserTotal100ns\":") + JsonInteger(bWorldCpuPresent, WorldUser100ns)
			+ TEXT(",\"worldKernelTotal100ns\":") + JsonInteger(bWorldCpuPresent, WorldKernel100ns)
			+ TEXT(",\"worldCpuTotalMs\":") + JsonNumber(bWorldCpuPresent, WorldCpuTotalMs)
			+ TEXT(",\"meanWorldCpuMs\":") + JsonNumber(bWorldCpuPresent, WorldCpuTotalMs / FMath::Max(1, CpuValidWorlds))
			+ TEXT(",\"pairedWorldWallTotalMs\":") + JsonNumber(bWorldCpuPresent, CpuPairedWorldWallMs)
			+ TEXT(",\"worldWallMinusCpuMs\":") + JsonNumber(bWorldCpuPresent, CpuPairedWorldWallMs - WorldCpuTotalMs);
		Json += TEXT(",\"source\":\"Direct Windows GetThreadTimes for the current GameThread; user+kernel cumulative FILETIME in 100ns units. No UE cached CPUTime and no QueryThreadCycleTime conversion.\",\"interpretation\":\"Per-frame deltas retain OS timer quantization, including zero or greater-than-wall samples; no clamping. Prefer cumulative/mean results over single frames. Missing samples are blank in CSV and aggregates null without valid intervals. World samples bracket only this World; frame samples bracket consecutive OnEndFrame callbacks. Measurement overhead is included. Wall minus thread CPU is NOT blocked time: it also includes runnable/preempted time, other-thread work and timer error; it does not measure worker, RenderThread or GPU execution.\"}");
		Json += FString::Printf(TEXT(",\"traceAlignment\":{\"start\":{\"frame\":%llu,\"qpc\":%llu,\"secondsPerCycle64\":%.17g,\"threadId\":%u},\"stop\":{\"frame\":%llu,\"qpc\":%llu,\"secondsPerCycle64\":%.17g,\"threadId\":%u},\"boundary\":\"Control-phase START/STOP anchors, not individual frame boundaries. Pair bookmarks and raw Windows QPC ticks with ETL/Trace clock calibration; do not concatenate uncalibrated startup seconds.\"}"),
			CaptureStartAnchor.Frame, CaptureStartAnchor.Qpc, CaptureStartAnchor.SecondsPerCycle, CaptureStartAnchor.ThreadId,
			CaptureStopAnchor.Frame, CaptureStopAnchor.Qpc, CaptureStopAnchor.SecondsPerCycle, CaptureStopAnchor.ThreadId);
		Json += FString::Printf(TEXT(",\"valid\":true,\"seed\":1337,\"map\":%s,\"validationSamples\":%d,\"minEnemies\":%d,\"maxEnemies\":%d,\"minPlayers\":%d,\"maxPlayers\":%d,\"widgets\":%d,\"worldWidgets\":%d,\"manualWorldWidgets\":%d,\"renderTargetDraws\":%llu,\"physicalMB\":%.3f,\"peakPhysicalMB\":%.3f"),
			*Quote(World.IsValid() ? World->GetMapName() : FString()), ValidationSamples, MinEnemies, MaxEnemies, MinPlayers, MaxPlayers,
			Bars.Widgets, Bars.WorldWidgets, Bars.ManualWorldWidgets, Bars.RenderTargetDraws,
			Memory.UsedPhysical / 1048576.0, Memory.PeakUsedPhysical / 1048576.0);
		Json += TEXT(",\"actualMapPackage\":") + Quote(World.IsValid() ? World->GetOutermost()->GetName() : FString());
		if (Role == TEXT("Server"))
			Json += FString::Printf(TEXT(",\"aiActivity\":{\"validationSamples\":%d,\"movingSpeedThresholdCmPerSecond\":5,\"controllers\":%d,\"minControllers\":%d,\"maxControllers\":%d,\"behaviorReady\":%d,\"minBehaviorReady\":%d,\"maxBehaviorReady\":%d,\"chasing\":%d,\"minChasing\":%d,\"maxChasing\":%d,\"attacking\":%d,\"minAttacking\":%d,\"maxAttacking\":%d,\"moving\":%d,\"minMoving\":%d,\"maxMoving\":%d}"),
				ValidationSamples, AIControllers, MinAIControllers, MaxAIControllers, BehaviorReady, MinBehaviorReady, MaxBehaviorReady, Chasing, MinChasing, MaxChasing,
				Attacking, MinAttacking, MaxAttacking, Moving, MinMoving, MaxMoving);
		else Json += TEXT(",\"aiActivity\":null");
		Json += TEXT(",\"quality\":{");
		const TCHAR* QualityNames[] = {TEXT("sg.ResolutionQuality"), TEXT("sg.ViewDistanceQuality"), TEXT("sg.AntiAliasingQuality"),
			TEXT("sg.ShadowQuality"), TEXT("sg.GlobalIlluminationQuality"), TEXT("sg.ReflectionQuality"), TEXT("sg.PostProcessQuality"),
			TEXT("sg.TextureQuality"), TEXT("sg.EffectsQuality"), TEXT("sg.FoliageQuality"), TEXT("sg.ShadingQuality"), TEXT("r.VSync"), TEXT("t.MaxFPS"),
			TEXT("r.ScreenPercentage"), TEXT("r.DynamicRes.OperationMode"), TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.ReflectionMethod"), TEXT("r.RayTracing"),
			TEXT("r.NumBufferedOcclusionQueries"), TEXT("r.HZBOcclusion"), TEXT("r.OneFrameThreadLag"), TEXT("r.RHICmdBypass")};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(QualityNames); ++Index)
		{
			if (Index > 0) Json += TEXT(",");
			const auto* CVar = IConsoleManager::Get().FindConsoleVariable(QualityNames[Index]);
			Json += Quote(QualityNames[Index]) + TEXT(":") + (CVar != nullptr ? FString::Printf(TEXT("%.6f"), CVar->GetFloat()) : TEXT("null"));
		}
		Json += TEXT("}");
		Json += TEXT(",\"GameThreadTimeMs\":null,\"RenderThreadTimeMs\":null,\"RHIThreadTimeMs\":null,\"GPUTimeMs\":null,");
		Json += TEXT("\"timingPolicy\":\"FrameEndIntervalMs=wall interval between consecutive Core OnEndFrame callbacks; includes waits; not Present interval or stat-unit GT. WorldTickWallMs=selected World OnWorldTickStart/End same engine frame; includes dependent waits; missing is blank. No explicit GC or screenshot injected during capture. This observer does not start Trace or engine CSV; external diagnostic launch options, if present, add overhead and are not the uninstrumented baseline.\",");
		Json += TEXT("\"gameplayPolicy\":\"Same Editor dedicated server for both client configurations; server prepares each player HP1000000 and enemy cull distance squared1e12 once; real AI, movement, attacks and damage remain. Client observes and fixes the recorded Player view by default; Overview requires an explicit option. No automated input or test RPC. Not Shipping dedicated-server capacity.\"}\n");
		if (!SaveAtomic(TEXT(".summary.json"), Json)) return Fail(TEXT("SummaryArtifactWriteFailed"));
		return Publish(TEXT("FLUSHED"));
	}

	// 按外部控制推进 Baseline，采样期低频核对人口和连接身份，FLUSHED 后仅等待退出。
	bool Tick(float)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - StartedAt >= 600) return Fail(TEXT("Watchdog600Seconds"));
		FString Command;
		if (!ReadControl(Command)) return !bFailed;
		if (Phase == TEXT("FLUSHED"))
		{
			if (Command == TEXT("STOP")) return true;
			if (Command != TEXT("EXIT")) return Fail(TEXT("ControlRegressedAfterFlush"));
			if (!Publish(TEXT("COMPLETE"))) return false;
			FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("PvpBaselineComplete"));
			return false;
		}
		if (Command == TEXT("EXIT")) return Fail(TEXT("ExitBeforeFlushed"));
		const bool bWorldReady = SampleWorld();
		if (bFailed) return false;
		if (bCapturing)
		{
			TArray<UNetConnection*> Connections;
			if (!CollectNetworkConnections(Connections)) return false;
			++NetworkIdentitySamples;
		}
		const bool bCountsReady = Enemies == 160 && Players == 2;
		if (Phase != TEXT("STARTING") && (!bWorldReady || !bCountsReady)) return Fail(TEXT("WorldPopulationOrViewChanged"));
		if (Phase == TEXT("STARTING") && bWorldReady && bCountsReady)
		{
			if (bCpuAccounting)
			{
				if (GEngine == nullptr) return Fail(TEXT("NativeUnitEngineUnavailable"));
				UnitConsumer = MakeShared<FPvpBaselineUnits>();
				UnitEngine = GEngine;
				// 首个原生 consumer 会重置 GPU 统计；在既有 20s 预热前注册，不将这个启动扰动塞进采集窗口。
				GEngine->AddPerformanceDataConsumer(UnitConsumer);
			}
			// 反向消融只在显式基线 DS、完整 160 人口就绪后应用一次，发生在预热前。
			// 默认不写此属性；不在生成过程中或采集循环每帧覆盖真实 CMC 配置。
			if (Role == TEXT("Server") && bContinuousFloorChecks && !bContinuousFloorChecksPrepared)
			{
				if (PreparedEnemies.Num() != 160) return Fail(TEXT("FloorReferenceRequiresPrepared160Enemies"));
				for (const auto& WeakEnemy : PreparedEnemies)
				{
					const AfpstrueEnemyCharacter* Enemy = WeakEnemy.Get();
					if (!IsValid(Enemy) || Enemy->IsActorBeingDestroyed() || Enemy->IsDead() || Enemy->GetCharacterMovement() == nullptr)
						return Fail(TEXT("FloorReferenceEnemyInvalidBeforePreparation"));
				}
				for (const auto& WeakEnemy : PreparedEnemies) WeakEnemy->GetCharacterMovement()->bAlwaysCheckFloor = true;
				bContinuousFloorChecksPrepared = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_BASELINE FLOOR_REFERENCE role=Server enemies=160 alwaysCheckFloor=1 appliedOnceBeforeWarmup=1"));
			}
			if (bRendered && bScreenshotRequested)
				FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, Role + TEXT("_Ready.png")), true, false);
			ReadyAt = Now;
			if (!Publish(TEXT("READY"))) return false;
		}
		if (Phase == TEXT("STARTING") || Phase == TEXT("READY"))
		{
			if (Command == TEXT("WAIT")) return true;
			if (Command != TEXT("START") || Phase != TEXT("READY") || Now - ReadyAt < 19.8)
				return Fail(TEXT("StartBeforeReadyOr20SecondWarmup"));
			AnimationStartSnapshot = BuildAnimationSnapshot();
			PacingStartSnapshot = BuildPacingSnapshot();
			CaptureAt = FPlatformTime::Seconds();
			if (!ReadNetworkBoundary(NetworkStart)) return false;
			LastEndAt = WorldStartAt = 0;
			LastWorldFrame = MAX_uint64;
			PreviousEndCpu = WorldBeginCpu = LastWorldBeginCpu = LastWorldEndCpu = {};
			bCapturing = true;
			if (bCpuAccounting)
			{
				if (!UnitEngine.IsValid() || !UnitConsumer.IsValid()) return Fail(TEXT("NativeUnitEngineUnavailable"));
				UnitConsumer->CaptureAt = CaptureAt;
				UnitConsumer->bCollect = true;
			}
			CaptureStartAnchor = MarkCaptureBoundary(TEXT("START"));
			return Publish(TEXT("CAPTURING"));
		}
		if (Phase == TEXT("CAPTURING"))
		{
			++ValidationSamples;
			MinEnemies = FMath::Min(MinEnemies, Enemies); MaxEnemies = FMath::Max(MaxEnemies, Enemies);
			MinPlayers = FMath::Min(MinPlayers, Players); MaxPlayers = FMath::Max(MaxPlayers, Players);
			if (Role == TEXT("Server"))
			{
				MinAIControllers = FMath::Min(MinAIControllers, AIControllers); MaxAIControllers = FMath::Max(MaxAIControllers, AIControllers);
				MinBehaviorReady = FMath::Min(MinBehaviorReady, BehaviorReady); MaxBehaviorReady = FMath::Max(MaxBehaviorReady, BehaviorReady);
				MinChasing = FMath::Min(MinChasing, Chasing); MaxChasing = FMath::Max(MaxChasing, Chasing);
				MinAttacking = FMath::Min(MinAttacking, Attacking); MaxAttacking = FMath::Max(MaxAttacking, Attacking);
				MinMoving = FMath::Min(MinMoving, Moving); MaxMoving = FMath::Max(MaxMoving, Moving);
			}
			if (Now - CaptureAt > 75) return Fail(TEXT("StopNotReceivedWithin75Seconds"));
			if (Command == TEXT("STOP")) return Flush();
			if (Command != TEXT("START")) return Fail(TEXT("ControlRegressedDuringCapture"));
		}
		return true;
	}
};

// 显式 Baseline 开关才建立一次进程级采集器；参数非法仍消费本次请求并退出，不回退普通游戏。
bool StartPvpBaselineIfRequested(UWorld* World)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline"))) return false;
	const auto Capture = MakeShared<FPvpBaselineCapture>();
	Capture->World = World;
	FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineRole="), Capture->Role);
	FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineControl="), Capture->ControlPath);
	FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineOutput="), Capture->Directory);
	Capture->ControlPath = FPvpBaselineCapture::Canonical(Capture->ControlPath);
	Capture->Directory = FPvpBaselineCapture::Canonical(Capture->Directory);
	Capture->bRendered = FParse::Param(FCommandLine::Get(), TEXT("PvpBaselineRendered"));
	Capture->bScreenshotRequested = FParse::Param(FCommandLine::Get(), TEXT("PvpBaselineScreenshot"));
	Capture->bCpuAccounting = FParse::Param(FCommandLine::Get(), TEXT("PvpBaselineCpuAccounting"));
	Capture->bContinuousFloorChecks = FParse::Param(FCommandLine::Get(), TEXT("PvpBaselineAlwaysCheckFloor"));
	FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineView="), Capture->ViewPolicy);
	FParse::Value(FCommandLine::Get(), TEXT("EnemyReplication="), Capture->EnemyReplication);
	FString Root;
	FParse::Value(FCommandLine::Get(), TEXT("PvpProfileRoot="), Root);
	Root = FPvpBaselineCapture::Canonical(Root.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling")) : Root);
	const bool bServer = Capture->Role == TEXT("Server");
	const bool bValidRole = bServer || Capture->Role == TEXT("Client1") || Capture->Role == TEXT("Client2");
	const bool bPathsValid = Root.Len() > 3 && !Capture->Directory.IsEmpty() && !Capture->Directory.Equals(Root, ESearchCase::IgnoreCase) &&
		FPaths::IsUnderDirectory(Capture->Directory, Root) && FPaths::IsUnderDirectory(Capture->ControlPath, Capture->Directory);
	if (!bPathsValid || !bValidRole || (Capture->ViewPolicy != TEXT("Player") && Capture->ViewPolicy != TEXT("Overview")) ||
		(Capture->EnemyReplication != TEXT("Tiered") && Capture->EnemyReplication != TEXT("Reference")) ||
		World == nullptr || World->WorldType != EWorldType::Game || GIsEditor || bProcessBenchmarkConsumed ||
		(bServer ? (World->GetNetMode() != NM_DedicatedServer || UE_BUILD_SHIPPING) : World->GetNetMode() != NM_Client) ||
		(bServer && Capture->bRendered) || (!bServer && Capture->bContinuousFloorChecks) || FParse::Param(FCommandLine::Get(), TEXT("PvpProfile")) ||
		FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")) || FParse::Param(FCommandLine::Get(), TEXT("AutoBenchmark")) ||
		!IFileManager::Get().DirectoryExists(*Capture->Directory))
	{
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("PvpBaselineInvalidArguments"));
		return true;
	}
	// Fresh output is mandatory; a previous successful file cannot satisfy a new
	// failed run. The parent creates a new case directory and WAIT file first.
	for (const TCHAR* Suffix : {TEXT(".status.json"), TEXT(".frames.csv"), TEXT(".summary.json"), TEXT(".pacing.csv"), TEXT(".units.csv")})
		if (IFileManager::Get().FileExists(*FPaths::Combine(Capture->Directory, Capture->Role + Suffix)))
		{
			FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("PvpBaselineOutputAlreadyExists"));
			return true;
		}
	bProcessBenchmarkConsumed = true;
	FMath::RandInit(1337);
	Capture->Frames.Reserve(FPvpBaselineCapture::MaxFrames);
	if (Capture->bCpuAccounting)
	{
		Capture->CpuFrames.Reserve(FPvpBaselineCapture::MaxFrames);
		Capture->PacingFrames.Reserve(FPvpBaselineCapture::MaxFrames);
	}
	Capture->StartHandle = FWorldDelegates::OnWorldTickStart.AddSP(Capture, &FPvpBaselineCapture::BeginWorld);
	Capture->EndHandle = FWorldDelegates::OnWorldTickEnd.AddSP(Capture, &FPvpBaselineCapture::EndWorld);
	Capture->FrameHandle = FCoreDelegates::OnEndFrame.AddSP(Capture, &FPvpBaselineCapture::EndFrame);
	if (!Capture->Publish(TEXT("STARTING"))) return true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Capture](float Delta) { return Capture->Tick(Delta); }), .2f);
	return true;
}
}

// 为测试客户端执行受限本机连接；仅允许已约定角色/端口，正常加入与重连仍走正式协议。
EFPPvpBaselineBootstrap StartPvpBaselineClient(UGameInstance* Instance)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline")) || IsRunningDedicatedServer())
		return EFPPvpBaselineBootstrap::NotRequested;
	FString Role, Address;
	FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineRole="), Role);
	if (Role == TEXT("Server")) return EFPPvpBaselineBootstrap::NotRequested;
	const auto Reject = []()
	{
		// No default-map fallback: a standalone world is not a valid multiplayer
		// baseline. Parent rejects any exit before COMPLETE, including an early
		// initialization exit whose Windows message-loop status may still be zero.
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("PvpBaselineClientBootstrapRejected"));
		return EFPPvpBaselineBootstrap::Rejected;
	};
	if ((Role != TEXT("Client1") && Role != TEXT("Client2")) ||
		!FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineConnect="), Address)) return Reject();
	Address.TrimQuotesInline();
	FString HostPort, Portal;
	const FString ExpectedPortal = Role == TEXT("Client1") ? TEXT("GroupBaselineClient1") : TEXT("GroupBaselineClient2");
	if (!Address.Split(TEXT("#"), &HostPort, &Portal) || Portal != ExpectedPortal ||
		!HostPort.RemoveFromStart(TEXT("127.0.0.1:"), ESearchCase::CaseSensitive) || HostPort.IsEmpty() || HostPort.Len() > 5)
		return Reject();
	for (const TCHAR Character : HostPort)
		if (Character < TEXT('0') || Character > TEXT('9')) return Reject();
	const int32 Port = FCString::Atoi(*HostPort);
	if (Port < 1024 || Port > 65000 || Address != FString::Printf(TEXT("127.0.0.1:%d#%s"), Port, *ExpectedPortal)) return Reject();
	UEngine* Engine = Instance != nullptr ? Instance->GetEngine() : nullptr;
	FWorldContext* Context = Instance != nullptr ? Instance->GetWorldContext() : nullptr;
	if (Engine == nullptr || Context == nullptr || Context->OwningGameInstance.Get() != Instance ||
		Context->WorldType != EWorldType::Game || Context->World() == nullptr || Context->PendingNetGame != nullptr || GIsEditor)
		return Reject();
	// Absolute travel deliberately does not inherit DefaultPlayer URL options.
	// InitializeStandalone already supplied the dummy World; Browse owns PendingNetGame
	// and the ordinary engine handshake/loading flow, exactly as initial URL travel.
	const FURL URL(nullptr, *Address, TRAVEL_Absolute);
	if (!URL.Valid || !URL.IsInternal() || URL.IsLocalInternal() || URL.Host != TEXT("127.0.0.1") ||
		URL.Port != Port || URL.Portal != ExpectedPortal || URL.Op.Num() != 0) return Reject();
	FString Error;
	const EBrowseReturnVal::Type Result = Engine->Browse(*Context, URL, Error);
	if (Result != EBrowseReturnVal::Pending && Result != EBrowseReturnVal::Success) return Reject();
	return EFPPvpBaselineBootstrap::Started;
}

// GameState 的采集分流入口：优先 Baseline，否则仅在非 Shipping 构建接受旧 Profile 请求。
void StartPvpPerformanceProfile(UWorld* World)
{
	if (StartPvpBaselineIfRequested(World)) return;
#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpProfile"))) return;
	if (World == nullptr || World->WorldType != EWorldType::Game || GIsEditor || bProcessBenchmarkConsumed)
	{
		UE_LOG(LogTemp, Error, TEXT("PVP_PROFILE FAIL Role=Unknown Reason=InvalidWorldOrDuplicateProcessRun"));
		return;
	}
	bProcessBenchmarkConsumed = true;
	const TSharedRef<FPvpPerformanceProfile> Profile = MakeShared<FPvpPerformanceProfile>();
	Profile->World = World;
	FParse::Value(FCommandLine::Get(), TEXT("PvpProfileRole="), Profile->Role);
	FParse::Value(FCommandLine::Get(), TEXT("PvpProfileControl="), Profile->ControlPath);
	FParse::Value(FCommandLine::Get(), TEXT("PvpProfileOutput="), Profile->OutputDirectory);
	Profile->bCollectTrace = !FParse::Param(FCommandLine::Get(), TEXT("PvpProfileNoTrace"));
	Profile->bFixedView = FParse::Param(FCommandLine::Get(), TEXT("PvpProfileFixedView"));
	Profile->bHealthAndDeath = FParse::Param(FCommandLine::Get(), TEXT("PvpProfileHealthAndDeath"));
	Profile->ControlPath.TrimQuotesInline();
	Profile->OutputDirectory.TrimQuotesInline();
	Profile->ControlPath = FPvpPerformanceProfile::Canonical(Profile->ControlPath);
	Profile->OutputDirectory = FPvpPerformanceProfile::Canonical(Profile->OutputDirectory);
	const bool bServer = Profile->Role == TEXT("Server");
	const bool bRoleValid = bServer || Profile->Role == TEXT("Client1") || Profile->Role == TEXT("Client2");
	const auto& Config = FFPBenchmarkConfig::Get();
	if (!bRoleValid || (bServer ? World->GetNetMode() != NM_DedicatedServer : World->GetNetMode() != NM_Client) ||
		!Config.HasEnemyCountOverride() || Config.EnemyCount != 160 || Config.Seed != 1337 ||
		FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")) || Config.bAutoBenchmark ||
		!FPvpPerformanceProfile::IsProfilePath(Profile->ControlPath) || !FPvpPerformanceProfile::IsProfilePath(Profile->OutputDirectory) ||
		!IFileManager::Get().MakeDirectory(*Profile->OutputDirectory, true))
	{
		UE_LOG(LogTemp, Error, TEXT("PVP_PROFILE FAIL Role=%s Reason=InvalidProfileArgumentsOrPath"), *Profile->Role);
		FPlatformMisc::RequestExitWithStatus(false, 2, TEXT("PvpPerformanceProfileArguments"));
		return;
	}
	Profile->CsvPath = FPvpPerformanceProfile::Canonical(FPaths::Combine(Profile->OutputDirectory, Profile->Role + TEXT(".csv")));
	Profile->TracePath = FPvpPerformanceProfile::Canonical(FPaths::Combine(Profile->OutputDirectory, Profile->Role + TEXT(".utrace")));
	Profile->WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddSP(Profile, &FPvpPerformanceProfile::BeginWorldTick);
	Profile->WorldTickEndHandle = FWorldDelegates::OnWorldTickEnd.AddSP(Profile, &FPvpPerformanceProfile::EndWorldTick);
	FMath::RandInit(1337);
	UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE START Role=%s Enemies=160 Seed=1337 Warmup=15 Capture=30 Control=%s Output=%s HealthPolicy=ServerOnce1000000 RelevancyPolicy=ServerCullSquared1e12 ViewPolicy=RecordedNotFrozen"),
		*Profile->Role, *Profile->ControlPath, *Profile->OutputDirectory);
	UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE UI_CONFIG Role=%s Mode=%s Trace=%d NullRHI=%d FixedView=%d"), *Profile->Role,
		UfpstrueEnemyHealthBarComponent::GetModeName(UfpstrueEnemyHealthBarComponent::GetConfiguredMode()), Profile->bCollectTrace, GUsingNullRHI, Profile->bFixedView);
	UE_LOG(LogTemp, Display, TEXT("PVP_PROFILE TIMING_SCOPE WorldTick=OnWorldTickStartToEndWallMs StatUnitThreads=RenderedClientOnly FrameIncludesLimiterWait=1"));
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Profile](float Delta) { return Profile->Tick(Delta); }), 0.2f);
#endif
}

/*
 * 自动性能测试执行器。
 * GameMode 只负责持有本组件；这里按“启动场景 -> 等待生成 -> 预热 -> 采集 -> 保存/退出”推进一次测试，
 * 并把命令行中的消融开关统一应用到敌人，避免测试逻辑散落进正常 Gameplay 流程。
 */

// ==================== 生命周期与测试入口 ====================

// 禁用组件逐帧 Tick；场景阶段用 World Timer，采集资源轮询用 CoreTicker。
UfpstrueBenchmarkRunner::UfpstrueBenchmarkRunner()
{
	// 准备/预热使用世界 Timer；采集和写盘由独立 Watchdog 推进，不启用组件 Tick。
	PrimaryComponentTick.bCanEverTick = false;
}

// 仅自动测试参数存在时接管开局；一个进程只接纳一次，防止复用已改变的随机流和 World。
void UfpstrueBenchmarkRunner::StartIfRequested(AfpstrueGameMode* InGameMode)
{
#if !UE_BUILD_SHIPPING
	// 只有 -AutoBenchmark 存在时接管开局；普通游玩不会进入任何测试专用流程。
	if (!FFPBenchmarkConfig::Get().bAutoBenchmark || !IsValid(InGameMode) || GetWorld() == nullptr || bEndingPlay)
	{
		return;
	}
	// 同一次运行重复挂接是空操作。Finished 也不重开：场景、血量与随机流都未完整重建。
	if (!CanStartFreshRun())
	{
		UE_LOG(LogTemp, Warning, TEXT("Automated benchmark rejected: one run per process; use a fresh game process (outstandingCsv=%d)."),
			HasOutstandingCsvLease());
		return;
	}
	// 先占有本进程运行资格并提交阶段，再挂接回调；重复入口不能排入第二次开局。
	bProcessBenchmarkConsumed = true;
	GameMode = InGameMode;
	++RunId;
	Phase = EFPBenchmarkPhase::Queued;
	PhaseDeadline = FPlatformTime::Seconds() + ProfilerTimeoutSeconds;
	StartWatchdog();
	ScheduleStage(Phase, &UfpstrueBenchmarkRunner::BeginBenchmark, 0.0f);
#endif
}

// Idle、未消耗进程运行机会且无遗留 CSV 租约三者同时满足，才允许首次启动。
bool UfpstrueBenchmarkRunner::CanStartFreshRun() const
{
	return Phase == EFPBenchmarkPhase::Idle && !bProcessBenchmarkConsumed && !HasOutstandingCsvLease();
}

// CSV 排队或写盘未结束时持续占有租约，不按业务超时自行视为可用。
bool UfpstrueBenchmarkRunner::HasOutstandingCsvLease()
{
	return OutstandingCsvCapture.IsValid() && OutstandingCsvCapture->State != FFPBenchmarkCsvCapture::EState::Released;
}

// 建立弱绑定 CoreTicker 检查，世界暂停仍可收尾；这是回调调度，不是新建工作线程。
void UfpstrueBenchmarkRunner::StartWatchdog()
{
	if (WatchdogHandle.IsValid()) return;
	// ★【重要边界】[生命周期] World Timer 只管准备/预热；CoreTicker 独立推进采集清理，世界暂停也不能困住资源。
	WatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this,
		[this](float) { return TickWatchdog(); }), 0.25f);
}

// 按阶段统一轮询 Profiler 并检查真实时间超时，世界 Timer 只负责准备和预热。
bool UfpstrueBenchmarkRunner::TickWatchdog()
{
	if (bEndingPlay || Phase == EFPBenchmarkPhase::Idle || Phase == EFPBenchmarkPhase::Finished)
	{
		WatchdogHandle.Reset();
		return false;
	}
	// 世界 Timer 只推进准备/预热；Profiler 启动、采集、写盘统一由本处驱动，不再双重轮询。
	// CoreTicker 不随世界暂停/时间缩放停止，采集门禁沿用 0.25 秒检查间隔。
	if (Phase == EFPBenchmarkPhase::Flushing || Phase == EFPBenchmarkPhase::Quarantined)
	{
		PollArtifacts();
	}
	else if (const UWorld* World = GetWorld(); World == nullptr || World->IsPaused() ||
		!FMath::IsNearlyEqual(World->GetWorldSettings()->GetEffectiveTimeDilation(), 1.0f))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("world paused, unavailable or time-dilated"));
	}
	else if (Phase == EFPBenchmarkPhase::StartingCapture)
	{
		PollCaptureStart();
	}
	else if (Phase == EFPBenchmarkPhase::Capturing)
	{
		PollCapture();
	}
	else if (FPlatformTime::Seconds() >= PhaseDeadline)
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("world stage failed to progress before watchdog deadline"));
	}
	return !bEndingPlay && Phase != EFPBenchmarkPhase::Finished;
}

// 排入世界计时器并捕获运行编号、预期阶段；取消和状态流转能拒绝过期回调。
void UfpstrueBenchmarkRunner::ScheduleStage(EFPBenchmarkPhase ExpectedPhase, FStageCallback Callback, float Delay, bool bLoop)
{
	if (UWorld* World = GetWorld(); World != nullptr && !bEndingPlay)
	{
		FTimerManager& Timers = World->GetTimerManager();
		Timers.ClearTimer(StageTimerHandle);
		const uint64 ScheduledRun = RunId;
		// 弱绑定只处理对象存活；额外检查编号和阶段，才保证业务取消后旧操作不会继续。
		FTimerDelegate Delegate = FTimerDelegate::CreateWeakLambda(this, [this, ScheduledRun, ExpectedPhase, Callback]()
		{
			// ★【重要边界】[异步身份] 弱绑定只保证对象仍在；RunId、阶段和退出标记共同拒绝旧轮次回调。
			if (RunId == ScheduledRun && Phase == ExpectedPhase && !bEndingPlay)
			{
				(this->*Callback)();
			}
		});
		if (Delay <= KINDA_SMALL_NUMBER)
		{
			StageTimerHandle = Timers.SetTimerForNextTick(Delegate);
		}
		else
		{
			FTimerManagerTimerParameters Parameters;
			Parameters.bLoop = bLoop;
			Parameters.bMaxOncePerFrame = true;
			Timers.SetTimer(StageTimerHandle, Delegate, Delay, Parameters);
		}
	}
}

// 先推进版本，再清除世界 Timer，使尚未返回的同步回调也失去继续推进的资格。
void UfpstrueBenchmarkRunner::ClearStageTimers()
{
	++RunId;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StageTimerHandle);
	}
}

// 恢复本次实验修改的场景覆盖、输入和诊断组件；不改正式配置，也不接管后来替换的角色。
void UfpstrueBenchmarkRunner::ReleaseSceneOwnership()
{
	if (AfpstrueGameMode* OwnerGameMode = GameMode.Get())
	{
		OwnerGameMode->OnAliveEnemyCountChanged.RemoveDynamic(this, &UfpstrueBenchmarkRunner::HandleEnemyCountChanged);
		if (bOwnsDurationOverride)
		{
			OwnerGameMode->BenchmarkGameDurationOverride = PreviousDurationOverride;
			OwnerGameMode->BenchmarkEnemyCountOverride = PreviousEnemyCountOverride;
		}
	}
	bOwnsDurationOverride = false;
	// 只回收本次运行取得的输入层与原始视点，不修改后来接管玩家的 Controller。
	// ★【重要边界】[资源所有权] 只撤销本次加入的一层输入锁和仍属于本次的视点，不覆盖其他系统或新控制器。
	if (bBenchmarkInputLocked)
	{
		if (APlayerController* PlayerController = LockedController.Get())
		{
			if (UInputComponent* Input = RemovedInputComponent.Get(); Input != nullptr &&
				LockedPlayer.IsValid() && PlayerController->GetPawn() == LockedPlayer.Get())
			{
				PlayerController->PushInputComponent(Input);
			}
			// SetIgnore*Input 使用计数器；这里只撤销 Benchmark 自己增加的一层。
			PlayerController->SetIgnoreMoveInput(false);
			PlayerController->SetIgnoreLookInput(false);
			if (PlayerController->GetViewTarget() == LockedPlayer.Get())
			{
				if (PreviousViewTarget.IsValid())
				{
					PlayerController->SetViewTarget(PreviousViewTarget.Get());
				}
				PlayerController->SetControlRotation(PreviousControlRotation);
			}
		}
		bBenchmarkInputLocked = false;
	}
	LockedController.Reset();
	LockedPlayer.Reset();
	RemovedInputComponent.Reset();
	PreviousViewTarget.Reset();

	// 仅恢复曾保存的存活对象，死亡后的移动/碰撞关闭不能被实验清理重新开启。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	for (const FDiagnosticState& State : DiagnosticStates)
	{
		AfpstrueEnemyCharacter* Enemy = State.Enemy.Get();
		if (Enemy == nullptr || Enemy->IsDead())
		{
			continue;
		}
		if (BenchmarkConfig.bDisableAttackSweep)
		{
			if (UfpstrueEnemyCombatComponent* Combat = Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>())
			{
				Combat->SetAttackSweepDisabledForBenchmark(false);
			}
		}
		if (BenchmarkConfig.bDisableEnemyPawnCollision)
		{
			Enemy->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, static_cast<ECollisionResponse>(State.PawnResponse));
		}
		if (BenchmarkConfig.bDisableCharacterMovementTick && Enemy->GetCharacterMovement())
		{
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(State.bMovementTick);
		}
		if (BenchmarkConfig.bDisableSkeletalMeshTick && Enemy->GetMesh())
		{
			Enemy->GetMesh()->SetComponentTickEnabled(State.bMeshTick);
		}
		if (BenchmarkConfig.bDisablePathFollowingTick && State.PathFollowing.IsValid())
		{
			State.PathFollowing->SetComponentTickEnabled(State.bPathFollowingTick);
		}
	}
	DiagnosticStates.Reset();
}

// 将人工取消和对局提前结束区分为不同结果；已进入写盘的流程继续回收，不重复发起取消。
void UfpstrueBenchmarkRunner::Cancel()
{
	if (Phase == EFPBenchmarkPhase::Idle || Phase == EFPBenchmarkPhase::Finished ||
		Phase == EFPBenchmarkPhase::Flushing || Phase == EFPBenchmarkPhase::Quarantined)
	{
		return;
	}
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	const bool bGameplayEnded = OwnerGameMode != nullptr && OwnerGameMode->IsFinished();
	Finish(bGameplayEnded ? EFPBenchmarkResult::InvalidSample : EFPBenchmarkResult::Cancelled,
		bGameplayEnded ? TEXT("gameplay ended before the requested capture completed") : TEXT("cancel requested"));
}

// 对象退出时撤销 UObject 回调和场景控制，迟到 CSV 与 Trace 清理由不持有对象的回调继续处理。
void UfpstrueBenchmarkRunner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 不安排新的世界 Timer；排队中的 CSV 首帧租约仍负责回收尚未真正启动的采集。
	bEndingPlay = true;
	ClearStageTimers();
	FTSTicker::GetCoreTicker().RemoveTicker(WatchdogHandle);
	WatchdogHandle.Reset();
	if (Phase != EFPBenchmarkPhase::Idle && Phase != EFPBenchmarkPhase::Finished)
	{
		// 已拒收/失败的原因不能被销毁覆盖；只有未决或尚未验盘的成功才转为取消。
		if (Result == EFPBenchmarkResult::None || Result == EFPBenchmarkResult::Completed)
		{
			Result = EFPBenchmarkResult::Cancelled;
		}
		UE_LOG(LogTemp, Warning, TEXT("Automated benchmark result: %s (world ended before artifact verification)."), ResultName(Result));
	}
	StopActiveProfilers();
	// 世界 Timer 已不可依赖；迟到启动仍由进程级租约处理，不以业务超时移除清理回调。
	if (bTraceActive && !bTraceStopRequested)
	{
		const FString OwnedTrace = TraceOutputPath;
		// 只重试停止仍属于本次目的路径的 Trace；不捕获 Runner，避免关卡销毁后访问对象。
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([OwnedTrace](float)
		{
			return FTraceAuxiliary::GetTraceDestinationString() == OwnedTrace && !FTraceAuxiliary::Stop();
		}), 0.1f);
	}
	ReleaseSceneOwnership();
	Phase = EFPBenchmarkPhase::Finished;
	Super::EndPlay(EndPlayReason);
}

// ==================== 场景准备与预热 ====================

// 固定种子并启动正常 Gameplay，再设置测试专用人数、时限、血量和输入隔离。
void UfpstrueBenchmarkRunner::BeginBenchmark()
{
	Phase = EFPBenchmarkPhase::Preparing;
	// 固定随机种子并启动正常 GameMode；测试器只负责观察和采集，不替换正式玩法状态。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("GameMode is unavailable"));
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	FMath::RandInit(BenchmarkConfig.Seed);
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark random seed: %d"), BenchmarkConfig.Seed);

	// 正常对局时限不应截断长驻留实验。覆盖只存在于独立 AutoBenchmark 进程，
	// 额外 60 秒覆盖分帧生成、ready 轮询和退出收尾，不改变正式配置资产。
	PreviousDurationOverride = OwnerGameMode->BenchmarkGameDurationOverride;
	PreviousEnemyCountOverride = OwnerGameMode->BenchmarkEnemyCountOverride;
	bOwnsDurationOverride = true;
	OwnerGameMode->BenchmarkEnemyCountOverride = BenchmarkConfig.HasEnemyCountOverride() ? BenchmarkConfig.EnemyCount : INDEX_NONE;
	OwnerGameMode->BenchmarkGameDurationOverride =
		FMath::CeilToInt(BenchmarkConfig.WarmupSeconds + BenchmarkConfig.DurationSeconds) + 60;
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark game duration override: %d seconds"),
		   OwnerGameMode->BenchmarkGameDurationOverride);

	OwnerGameMode->StartGameMode();
	// StartGameMode 的蓝图/委托可能同步终止对局，此时 Cancel 已经改变阶段。
	if (Phase != EFPBenchmarkPhase::Preparing)
	{
		return;
	}
	if (!OwnerGameMode->IsRunning() || !IsValid(OwnerGameMode->PlayerCharacter))
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("gameplay failed to start with a valid player"));
		return;
	}

	// 可选高血量仍走真实生命组件和受伤流程；通知可能同步改变对局，之后必须复核阶段。
	if (BenchmarkConfig.HasPlayerHealthOverride())
	{
		if (UfpstrueHealthComponent* HealthComponent = OwnerGameMode->PlayerCharacter->GetHealthComponent())
		{
			HealthComponent->SetMaxHealthAndReset(BenchmarkConfig.PlayerHealth);
			UE_LOG(LogTemp, Display, TEXT("Automated benchmark player health override: max=%.1f current=%.1f"),
				   HealthComponent->GetMaxHealth(), HealthComponent->GetHealth());
		}
	}
	if (Phase != EFPBenchmarkPhase::Preparing)
	{
		return;
	}
	if (!OwnerGameMode->IsRunning() || !IsValid(OwnerGameMode->PlayerCharacter))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("player changed during benchmark setup"));
		return;
	}

	// 保存视点和本次输入所有权后暂时锁定真实输入，正常移动组件及战斗伤害仍保持工作。
	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		LockedController = PlayerController;
		LockedPlayer = OwnerGameMode->PlayerCharacter;
		PreviousViewTarget = PlayerController->GetViewTarget();
		PreviousControlRotation = PlayerController->GetControlRotation();
		PlayerController->SetViewTarget(OwnerGameMode->PlayerCharacter);
		// 可见独立窗口会抢占焦点；从输入栈移除玩家 InputComponent，阻止开火、换弹和跳跃等 Action
		// 被人工鼠标/键盘误触发。Enhanced Input 子系统及正式绑定代码保持存在，只有 AutoBenchmark 进程临时隔离输入。
		if (UInputComponent* Input = OwnerGameMode->PlayerCharacter->InputComponent;
			Input != nullptr && PlayerController->PopInputComponent(Input))
		{
			RemovedInputComponent = Input;
		}
		// 额外屏蔽 Controller 的移动/视角入口，避免别的输入组件或控制台命令绕过角色 InputComponent。
		// 这不会关闭移动组件、碰撞或受伤逻辑，AI 仍会围攻同一个正常玩家角色。
		PlayerController->SetIgnoreMoveInput(true);
		PlayerController->SetIgnoreLookInput(true);
		PlayerController->SetControlRotation(OwnerGameMode->PlayerCharacter->GetActorRotation());
		if (UCharacterMovementComponent* Movement = OwnerGameMode->PlayerCharacter->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
		OwnerGameMode->PlayerCharacter->ConsumeMovementInputVector();
		BenchmarkPlayerLocation = OwnerGameMode->PlayerCharacter->GetActorLocation();
		BenchmarkControlRotation = PlayerController->GetControlRotation();
		bBenchmarkInputLocked = true;
		UE_LOG(LogTemp, Display,
			   TEXT("Automated benchmark input locked: location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f)"),
			   BenchmarkPlayerLocation.X, BenchmarkPlayerLocation.Y, BenchmarkPlayerLocation.Z,
			   BenchmarkControlRotation.Pitch, BenchmarkControlRotation.Yaw, BenchmarkControlRotation.Roll);
	}
	else
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("benchmark requires a player controller"));
		return;
	}
	// 正式基线必须保留完整玩法：HUD、受伤/死亡、碰撞、AI、动画和渲染消费者都按正常规则运行。
	// 只有命令行显式传入 BenchmarkDisable* 时，后面的诊断阶段才允许关闭单个消费者。

	PhaseDeadline = FPlatformTime::Seconds() + 60.0;
	ScheduleStage(Phase, &UfpstrueBenchmarkRunner::WaitForBenchmarkReady, 0.25f, true);
}

// 等分帧生成完成后锁存稳态人数，应用诊断并做首次门禁，通过后才进入预热。
void UfpstrueBenchmarkRunner::WaitForBenchmarkReady()
{
	// 等待分帧生成队列清空后再开始预热，使稳态数据不混入批量 Spawn 尖峰。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr || !OwnerGameMode->IsRunning())
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("gameplay stopped before ready"));
		return;
	}

	if (OwnerGameMode->PendingEnemySpawnCount > 0)
	{
		// 超时统一交给独立 Watchdog，世界暂停也不会阻塞拒收。
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	ExpectedEnemyCount = BenchmarkConfig.HasEnemyCountOverride()
		? BenchmarkConfig.EnemyCount : OwnerGameMode->RegisteredEnemies.Num();
	bPopulationChanged = false;
	OwnerGameMode->OnAliveEnemyCountChanged.AddUniqueDynamic(this, &UfpstrueBenchmarkRunner::HandleEnemyCountChanged);
	// 配置先应用，再预热。内存报告/截图等扰动也留在预热之前，不混入正式采集窗口。
	ApplyDiagnosticOverrides();
	if (!ValidateBenchmarkState(TEXT("ready")))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("ready validation failed"));
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark ready: requested=%d alive=%d warmup=%.1fs"), BenchmarkConfig.EnemyCount,
		   OwnerGameMode->RegisteredEnemies.Num(), BenchmarkConfig.WarmupSeconds);

	// 资源报告和截图会扰动开销，安排在正式窗口之前，由后续预热隔离影响。
	if (BenchmarkConfig.bCollectTextureStats)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("DumpTextureStreamingStats"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("ListStreamingTextures"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("MemReport -full"));
	}
	if (BenchmarkConfig.bTakeScreenshot)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Shot"));
	}
	Phase = EFPBenchmarkPhase::WarmingUp;
	PhaseDeadline = FPlatformTime::Seconds() + BenchmarkConfig.WarmupSeconds + ProfilerTimeoutSeconds;
	// 零预热也保存 next-tick 句柄；正常样本仍应配置足够预热让资源与诊断设置稳定。
	ScheduleStage(Phase, &UfpstrueBenchmarkRunner::StartCapture, BenchmarkConfig.WarmupSeconds);
}

// ==================== 正式采集与消融快照 ====================

// 预热后验证场景与 Profiler 空闲状态，提交采集请求；请求时刻不是 CSV 实际首帧。
void UfpstrueBenchmarkRunner::StartCapture()
{
	// 只读消费者快照，不能在预热完成后重新施加另一套配置。
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!ValidateBenchmarkState(TEXT("capture-start"), /*bLogSuccess=*/true, /*bLogConsumers=*/true))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("capture-start validation failed"));
		return;
	}
#if CSV_PROFILER
	FCsvProfiler* Csv = FCsvProfiler::Get();
	if (HasOutstandingCsvLease() || Csv->IsCapturing() || Csv->IsWritingFile() || Csv->IsEndCapturePending() || FTraceAuxiliary::IsConnected())
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("profiler is already owned by another capture"));
		return;
	}
	// Trace 先建立文件会话，CSV 首帧确认后才标记正式分析区域，避免把启动准备计入窗口。
	if (!BenchmarkConfig.TraceFile.IsEmpty())
	{
		if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *BenchmarkConfig.TraceFile,
			TEXT("cpu,frame,bookmark,task,stats")))
		{
			Finish(EFPBenchmarkResult::Failed, TEXT("Trace start was rejected"));
			return;
		}
		bTraceActive = true;
		TraceOutputPath = FTraceAuxiliary::GetTraceDestinationString();
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace started: %s"), *TraceOutputPath);
	}
	// 生产提交器调用真实 BeginCapture；实际启动由租约中的首帧回调确认。
	RequestCsvCapture([](const FString& Filename) { FCsvProfiler::Get()->BeginCapture(-1, FString(), Filename); });
	ClearStageTimers();
	Phase = EFPBenchmarkPhase::StartingCapture;
	PhaseDeadline = FPlatformTime::Seconds() + ProfilerTimeoutSeconds;
#else
	Finish(EFPBenchmarkResult::Unsupported, TEXT("CSV profiler is not compiled in this target"));
#endif
}

// 在提交前建立进程级 CSV 租约和首帧监听，即使 Runner 已取消也能回收迟到启动。
void UfpstrueBenchmarkRunner::RequestCsvCapture(TFunctionRef<void(const FString&)> SubmitStart)
{
#if CSV_PROFILER
	check(!HasOutstandingCsvLease());
	CsvCapture = MakeShared<FFPBenchmarkCsvCapture>();
	CsvCapture->Filename = FString::Printf(TEXT("AutomatedBenchmark_%s.csv"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	// ★【重要边界】[资源所有权] 提交 BeginCapture 前先取得进程级租约；Runner/World 销毁不会取消底层排队启动。
	OutstandingCsvCapture = CsvCapture;
	const TSharedRef<FFPBenchmarkCsvCapture> Capture = CsvCapture.ToSharedRef();
	// 首帧回调仅接纳本次文件；已取消的排队采集一旦启动，立即 EndCapture 并等待其写盘。
	Capture->FirstFrameHandle = FCsvProfiler::Get()->OnCSVProfileFirstFrame().AddLambda([Capture]()
	{
		FCsvProfiler* Profiler = FCsvProfiler::Get();
		// 手动采集若抢先启动，不会被本 Runner 停止，也不会被标记为本次输出。
		if (FPaths::GetCleanFilename(Profiler->GetOutputFilename()) == Capture->Filename)
		{
			const bool bCancelLateStart = Capture->State == FFPBenchmarkCsvCapture::EState::CancelPendingStart;
			Capture->State = FFPBenchmarkCsvCapture::EState::Capturing;
			Profiler->OnCSVProfileFirstFrame().Remove(Capture->FirstFrameHandle);
			Capture->FirstFrameHandle.Reset();
			// ★【重要边界】[异步身份] 迟到首帧只按本次文件身份处理；已取消则立即停止，不误停外部采集。
			if (bCancelLateStart)
			{
				Capture->WriteComplete = Profiler->EndCapture();
				Capture->State = FFPBenchmarkCsvCapture::EState::StopRequested;
			}
		}
	});
	// 不持有 UObject，也不依赖 World Tick。失败的业务已可结束，但租约要等真实产物写完才释放。
	// 独立资源回调只持有租约，写盘 Future 就绪后才释放进程占用，不依赖世界或对象存活。
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Capture](float)
	{
		// ★【重要边界】[资源所有权] 请求停止不等于资源可复用；写盘 Future 真正就绪后才释放 CSV 进程租约。
		if (Capture->State == FFPBenchmarkCsvCapture::EState::StopRequested &&
			Capture->WriteComplete.IsValid() && Capture->WriteComplete.IsReady())
		{
			Capture->State = FFPBenchmarkCsvCapture::EState::Released;
		}
		if (Capture->State != FFPBenchmarkCsvCapture::EState::Released) return true;
		if (OutstandingCsvCapture == Capture) OutstandingCsvCapture.Reset();
		return false;
	}), 0.1f);
	SubmitStart(Capture->Filename);
#endif
}

// 收到本次 CSV 的真实首帧确认后复核场景，从确认阶段开始计时并标记 Trace 分析区域。
void UfpstrueBenchmarkRunner::PollCaptureStart()
{
	if (CsvCapture.IsValid() && CsvCapture->State == FFPBenchmarkCsvCapture::EState::Capturing)
	{
		if (!ValidateBenchmarkState(TEXT("capture-start-confirmed")))
		{
			Finish(EFPBenchmarkResult::InvalidSample, TEXT("state changed while starting profilers"));
			return;
		}
		// ★【重要边界】[验收边界] 必须收到本次 CSV 的真实首帧确认并通过场景复核，才开始正式采集计时。
		Phase = EFPBenchmarkPhase::Capturing;
		if (bTraceActive)
		{
			UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionBegin AutomatedBenchmarkCapture"));
			bTraceRegionActive = true;
		}
		PhaseDeadline = FPlatformTime::Seconds() + FFPBenchmarkConfig::Get().DurationSeconds;
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture started: requested=%d alive=%d duration=%.1fs"),
			FFPBenchmarkConfig::Get().EnemyCount, ExpectedEnemyCount, FFPBenchmarkConfig::Get().DurationSeconds);
	}
	else if (FPlatformTime::Seconds() >= PhaseDeadline)
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("CSV capture did not start"));
	}
}

// 采集中持续验证人数、视点和消费者状态，并确认输出仍由本次流程拥有。
void UfpstrueBenchmarkRunner::PollCapture()
{
	const bool bCaptureEnded = FPlatformTime::Seconds() >= PhaseDeadline;
	if (!ValidateBenchmarkState(bCaptureEnded ? TEXT("capture-end") : TEXT("capture-window"), bCaptureEnded))
	{
		Finish(EFPBenchmarkResult::InvalidSample, TEXT("capture validation failed"));
		return;
	}
#if CSV_PROFILER
	if (!FCsvProfiler::Get()->IsCapturing() ||
		FPaths::GetCleanFilename(FCsvProfiler::Get()->GetOutputFilename()) != CsvCapture->Filename ||
		(bTraceActive && FTraceAuxiliary::GetTraceDestinationString() != TraceOutputPath))
	{
		Finish(EFPBenchmarkResult::Failed, TEXT("owned profiler was interrupted"));
		return;
	}
#endif
	// 时长到达只表示窗口完成；Completed 先作为候选结果，后续仍需写盘和文件检查。
	if (bCaptureEnded)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture stopped."));
		Finish(EFPBenchmarkResult::Completed, TEXT("capture window ended; verifying artifacts"));
	}
}

// 预热前保存并关闭显式指定的消费者，正式基线不传这些破坏性关闭项。
void UfpstrueBenchmarkRunner::ApplyDiagnosticOverrides()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr || !DiagnosticStates.IsEmpty())
	{
		return;
	}
	const FFPBenchmarkConfig& Config = FFPBenchmarkConfig::Get();
	if (!Config.bDisableAttackSweep && !Config.bDisableEnemyPawnCollision && !Config.bDisableCharacterMovementTick &&
		!Config.bDisableSkeletalMeshTick && !Config.bDisablePathFollowingTick)
	{
		return;
	}
	DiagnosticStates.Reserve(OwnerGameMode->RegisteredEnemies.Num());
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr || Enemy->IsDead())
		{
			continue;
		}
		FDiagnosticState& State = DiagnosticStates.AddDefaulted_GetRef();
		State.Enemy = Enemy;
		State.PawnResponse = Enemy->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn);
		if (Config.bDisableAttackSweep)
		{
			if (UfpstrueEnemyCombatComponent* Combat = Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>())
			{
				Combat->SetAttackSweepDisabledForBenchmark(true);
			}
		}
		if (Config.bDisableEnemyPawnCollision)
		{
			Enemy->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		}
		DisableTickForCapture(Enemy->GetCharacterMovement(), Config.bDisableCharacterMovementTick, State.bMovementTick);
		DisableTickForCapture(Enemy->GetMesh(), Config.bDisableSkeletalMeshTick, State.bMeshTick);
		if (AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
		{
			State.PathFollowing = Controller->GetPathFollowingComponent();
			DisableTickForCapture(State.PathFollowing.Get(), Config.bDisablePathFollowingTick, State.bPathFollowingTick);
		}
	}
}

// 验证与起始日志共用一次采样；普通采集检查不统计渲染档位，也不遍历世界。
UfpstrueBenchmarkRunner::FEnemySnapshot UfpstrueBenchmarkRunner::ReadEnemySnapshot(bool bDetailed) const
{
	FEnemySnapshot Snapshot;
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr) return Snapshot;
	const FFPBenchmarkConfig& Config = FFPBenchmarkConfig::Get();
	const UCrowdManager* CrowdManager = UCrowdManager::GetCurrent(GetWorld());
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		const AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr || Enemy->IsDead())
		{
			Snapshot.bRegistryValid = false;
			continue;
		}
		++Snapshot.Enemies;
		const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
		const USkeletalMeshComponent* Mesh = Enemy->GetMesh();
		const AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController());
		const UPathFollowingComponent* Path = Controller != nullptr ? Controller->GetPathFollowingComponent() : nullptr;
		Snapshot.bRegistryValid &= Controller != nullptr;
		if (Movement != nullptr)
		{
			Snapshot.RVO += Movement->bUseRVOAvoidance ? 1 : 0;
			Snapshot.bDiagnosticStateValid &= !Config.bDisableCharacterMovementTick || !Movement->IsComponentTickEnabled();
		}
		if (const UCrowdFollowingComponent* Crowd = Cast<UCrowdFollowingComponent>(Path))
		{
			++Snapshot.Crowd;
			// 组件存在不等于已分配 Detour 槽位，必须同时检查真实代理与模拟状态。
			Snapshot.ValidCrowd += CrowdManager && CrowdManager->IsAgentValid(Crowd) && Crowd->IsCrowdSimulationActive() ? 1 : 0;
		}
		Snapshot.bDiagnosticStateValid &= !Config.bDisablePathFollowingTick || (Path && !Path->IsComponentTickEnabled());
		Snapshot.bDiagnosticStateValid &= !Config.bDisableSkeletalMeshTick || (Mesh && !Mesh->IsComponentTickEnabled());
		Snapshot.bDiagnosticStateValid &= !Config.bDisableEnemyPawnCollision ||
			Enemy->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Ignore;
		if (!bDetailed) continue;
		if (Movement != nullptr)
		{
			Snapshot.MovementTickEnabled += Movement->IsComponentTickEnabled() ? 1 : 0;
			const int32 Tier = Movement->GetComponentTickInterval() <= KINDA_SMALL_NUMBER ? 0 :
				(Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Reduced ? 1 : 2);
			++Snapshot.Movement[Tier];
		}
		Snapshot.Attacking += Enemy->IsAttacking() ? 1 : 0;
		++Snapshot.Render[FMath::Clamp(static_cast<int32>(Enemy->GetRenderSignificanceTier()), 0, 2)];
		++Snapshot.MinLOD[FMath::Clamp(Enemy->GetAppliedMinimumLOD(), 0, 2)];
		if (Mesh != nullptr)
		{
			Snapshot.SkeletalMeshTickEnabled += Mesh->IsComponentTickEnabled() ? 1 : 0;
			Snapshot.ShadowCasting += Mesh->CastShadow ? 1 : 0;
			Snapshot.RayTracingVisible += Mesh->bVisibleInRayTracing ? 1 : 0;
		}
	}
	return Snapshot;
}

// 使用验证同一份快照输出开关和档位；额外单次读回尸体 Mesh，区分存活与死亡消费者。
void UfpstrueBenchmarkRunner::LogConsumerSnapshot(const FEnemySnapshot& Snapshot) const
{
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr) return;
	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark diagnostics applied: enemies=%d attackSweepOff=%d pawnCollisionOff=%d pathFollowingTickOff=%d "
				"characterMovementTickOff=%d skeletalMeshTickOff=%d significanceOff=%d"),
		   Snapshot.Enemies, BenchmarkConfig.bDisableAttackSweep ? 1 : 0, BenchmarkConfig.bDisableEnemyPawnCollision ? 1 : 0,
		   BenchmarkConfig.bDisablePathFollowingTick ? 1 : 0, BenchmarkConfig.bDisableCharacterMovementTick ? 1 : 0,
		   BenchmarkConfig.bDisableSkeletalMeshTick ? 1 : 0, BenchmarkConfig.bDisableEnemySignificance ? 1 : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark enemy snapshot: movementFull=%d movementMid=%d movementFar=%d movementTickEnabled=%d skeletalMeshTickEnabled=%d "
				"attacking=%d castingShadow=%d rayTracingVisible=%d animationSharingFollowers=%d"),
		   Snapshot.Movement[0], Snapshot.Movement[1], Snapshot.Movement[2], Snapshot.MovementTickEnabled, Snapshot.SkeletalMeshTickEnabled,
		   Snapshot.Attacking, Snapshot.ShadowCasting, Snapshot.RayTracingVisible,
		   OwnerGameMode->EnemyAnimationSharingCoordinator != nullptr
			   ? OwnerGameMode->EnemyAnimationSharingCoordinator->GetRegisteredEnemyCount()
			   : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark avoidance snapshot: enemies=%d rvoEnabled=%d crowdFollowing=%d crowdValid=%d"),
		   Snapshot.Enemies, Snapshot.RVO, Snapshot.Crowd, Snapshot.ValidCrowd);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark render significance snapshot: renderFull=%d renderReduced=%d renderBackground=%d lod0=%d lod1=%d lod2Plus=%d "
				"fullBudget=%d shadowBudget=%d rayTracingBudget=%d"),
		   Snapshot.Render[0], Snapshot.Render[1], Snapshot.Render[2], Snapshot.MinLOD[0], Snapshot.MinLOD[1], Snapshot.MinLOD[2],
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies);
	UE_LOG(LogTemp, Display, TEXT("Benchmark LOD snapshot semantics: lod0/lod1/lod2Plus count applied minimum-LOD constraints, not actually selected LODs."));

	// 一次性读回实际管理范围，尸体已退出存活注册表，必须单独观察；不在每帧遍历世界。
	int32 AliveMeshes = 0, AliveShadowMeshes = 0, AliveRayTracingMeshes = 0;
	int32 CorpseMeshes = 0, CorpseShadowMeshes = 0, CorpseRayTracingMeshes = 0;
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
	{
		int32 Meshes, ShadowMeshes, RayTracingMeshes;
		It->GetRenderBudgetMeshCounts(Meshes, ShadowMeshes, RayTracingMeshes);
		if (It->IsDead())
		{
			CorpseMeshes += Meshes;
			CorpseShadowMeshes += ShadowMeshes;
			CorpseRayTracingMeshes += RayTracingMeshes;
		}
		else
		{
			AliveMeshes += Meshes;
			AliveShadowMeshes += ShadowMeshes;
			AliveRayTracingMeshes += RayTracingMeshes;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("Benchmark managed Mesh flags: alive=%d shadow=%d rayTracing=%d corpse=%d corpseShadow=%d corpseRayTracing=%d (components, not GPU primitives)"),
		AliveMeshes, AliveShadowMeshes, AliveRayTracingMeshes, CorpseMeshes, CorpseShadowMeshes, CorpseRayTracingMeshes);
}

// 接纳空场景或完整单一避让模式；有 Crowd 组件但没有有效代理不能算启用成功。
bool UfpstrueBenchmarkRunner::IsAvoidanceConfigurationValid(int32 Enemies, int32 RVO, int32 Crowd, int32 ValidCrowd)
{
	// 空场景本来就没有避让消费者，不能被当成错误配置；非空场景要求完整的一套实现。
	return (Enemies == 0 && RVO == 0 && Crowd == 0 && ValidCrowd == 0) ||
		(Enemies > 0 && RVO == Enemies && Crowd == 0 && ValidCrowd == 0) ||
		(Enemies > 0 && RVO == 0 && Crowd == Enemies && ValidCrowd == Enemies);
}

// 记录人数曾经偏离目标的事实，防止中途替换敌人后相同总数掩盖样本变化。
void UfpstrueBenchmarkRunner::HandleEnemyCountChanged(int32 AliveEnemies)
{
	// 锁存中途变化，即使采集结束前人数又恢复，也不能把替换过的样本判为稳定。
	if (ExpectedEnemyCount != INDEX_NONE && AliveEnemies != ExpectedEnemyCount)
	{
		bPopulationChanged = true;
	}
}

// 检查对局、玩家、敌人数、避让和诊断状态；起始详细日志复用同一份采样结果。
bool UfpstrueBenchmarkRunner::ValidateBenchmarkState(const TCHAR* ValidationPhase, bool bLogSuccess, bool bLogConsumers) const
{
	const AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	const AfpstrueCharacter* Player =
		OwnerGameMode != nullptr && IsValid(OwnerGameMode->PlayerCharacter)
			? OwnerGameMode->PlayerCharacter.Get()
			: nullptr;
	const UfpstrueHealthComponent* HealthComponent = Player != nullptr ? Player->GetHealthComponent() : nullptr;
	const FEnemySnapshot Snapshot = ReadEnemySnapshot(bLogConsumers);
	const float PlayerHealth = HealthComponent != nullptr ? HealthComponent->GetHealth() : 0.0f;
	const bool bEnemyCountValid = ExpectedEnemyCount != INDEX_NONE && Snapshot.Enemies == ExpectedEnemyCount &&
		!bPopulationChanged && Snapshot.bRegistryValid;
	const bool bAvoidanceValid = IsAvoidanceConfigurationValid(Snapshot.Enemies, Snapshot.RVO, Snapshot.Crowd, Snapshot.ValidCrowd);
	// 校验输入锁仍属于同一 Controller/Pawn/视点，同时限制位置和转向漂移，不只检查人数。
	const APlayerController* PlayerController = LockedController.Get();
	const FVector CurrentPlayerLocation = Player != nullptr ? Player->GetActorLocation() : FVector::ZeroVector;
	const FRotator CurrentControlRotation = PlayerController != nullptr ? PlayerController->GetControlRotation() : FRotator::ZeroRotator;
	const float LocationDrift = bBenchmarkInputLocked
		? FVector::Distance(CurrentPlayerLocation, BenchmarkPlayerLocation)
		: 0.0f;
	const float PitchDrift = bBenchmarkInputLocked
		? FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentControlRotation.Pitch, BenchmarkControlRotation.Pitch))
		: 0.0f;
	const float YawDrift = bBenchmarkInputLocked
		? FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentControlRotation.Yaw, BenchmarkControlRotation.Yaw))
		: 0.0f;
	const bool bViewTransformValid = bBenchmarkInputLocked && PlayerController != nullptr && Player == LockedPlayer.Get() &&
		PlayerController->GetPawn() == Player && PlayerController->GetViewTarget() == Player &&
		(LocationDrift <= BenchmarkPlayerLocationTolerance && PitchDrift <= BenchmarkPlayerRotationToleranceDegrees &&
		 YawDrift <= BenchmarkPlayerRotationToleranceDegrees);
	const bool bStateValid = OwnerGameMode != nullptr && OwnerGameMode->IsRunning() &&
		HealthComponent != nullptr && !HealthComponent->IsDead() && bEnemyCountValid && bViewTransformValid &&
		bAvoidanceValid && Snapshot.bDiagnosticStateValid;

	// 成功可选择静默，失败始终输出门禁证据；只有有效样本才输出消费者快照。
	if (bStateValid && bLogSuccess)
	{
		UE_LOG(LogTemp, Display,
			   TEXT("Automated benchmark validation: phase=%s requested=%d alive=%d playerHealth=%.1f locationDrift=%.2f pitchDrift=%.2f yawDrift=%.2f"),
			   ValidationPhase, ExpectedEnemyCount, Snapshot.Enemies, PlayerHealth, LocationDrift, PitchDrift, YawDrift);
	}
	else if (!bStateValid)
	{
		UE_LOG(LogTemp, Error,
			   TEXT("Automated benchmark invalid: phase=%s requested=%d alive=%d playerHealth=%.1f running=%d ended=%d locationDrift=%.2f pitchDrift=%.2f yawDrift=%.2f"),
			   ValidationPhase, ExpectedEnemyCount, Snapshot.Enemies, PlayerHealth,
			   OwnerGameMode != nullptr && OwnerGameMode->IsRunning() ? 1 : 0,
			   OwnerGameMode != nullptr && OwnerGameMode->IsFinished() ? 1 : 0,
			   LocationDrift, PitchDrift, YawDrift);
		UE_LOG(LogTemp, Error, TEXT("Benchmark validation gates: populationChanged=%d registryValid=%d avoidanceValid=%d diagnosticStateValid=%d rvo=%d crowd=%d crowdValid=%d"),
			bPopulationChanged, Snapshot.bRegistryValid, bAvoidanceValid, Snapshot.bDiagnosticStateValid, Snapshot.RVO, Snapshot.Crowd, Snapshot.ValidCrowd);
	}

	if (bStateValid && bLogConsumers) LogConsumerSnapshot(Snapshot);
	return bStateValid;
}

// ==================== 采集结束与进程退出 ====================

// 撤销阶段回调、记录业务结果并交还场景，随后等待异步产物完成，不立即宣布成功。
void UfpstrueBenchmarkRunner::Finish(EFPBenchmarkResult InResult, const TCHAR* Reason)
{
	ClearStageTimers();
	Result = InResult;
	// ★【重要边界】[状态提交] 先使旧阶段失效并进入 Flushing，再停采集/归还场景；候选 Completed 还不是最终成功。
	Phase = EFPBenchmarkPhase::Flushing;
	StopActiveProfilers();
	ReleaseSceneOwnership();
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark finishing: result=%s reason=%s"), ResultName(Result), Reason);
	PhaseDeadline = FPlatformTime::Seconds() + ProfilerTimeoutSeconds;
	StartWatchdog();
}

// 幂等请求停止本次拥有的 Profiler；尚未开始的 CSV 标记迟到取消，不能直接释放租约。
void UfpstrueBenchmarkRunner::StopActiveProfilers()
{
#if CSV_PROFILER
	if (CsvCapture.IsValid())
	{
		FCsvProfiler* Csv = FCsvProfiler::Get();
		// ★【重要边界】[资源所有权] 尚未启动的 CSV 只能标记迟到取消，不能假定 EndCapture 已撤销排队请求。
		if (CsvCapture->State == FFPBenchmarkCsvCapture::EState::StartRequested)
		{
			CsvCapture->State = FFPBenchmarkCsvCapture::EState::CancelPendingStart;
		}
		else if (CsvCapture->State == FFPBenchmarkCsvCapture::EState::Capturing && Csv->IsCapturing() &&
			FPaths::GetCleanFilename(Csv->GetOutputFilename()) == CsvCapture->Filename)
		{
			CsvCapture->WriteComplete = Csv->EndCapture();
			CsvCapture->State = FFPBenchmarkCsvCapture::EState::StopRequested;
		}
	}
#endif
	// Trace 路径必须仍属于本次运行；外部替换时拒收样本，不停止新所有者的采集。
	if (bTraceActive && !bTraceStopRequested)
	{
		if (FTraceAuxiliary::GetTraceDestinationString() != TraceOutputPath)
		{
			// 外部手动关闭/替换 Trace：不能替它停止新会话，也不能认证旧样本完整。
			bTraceActive = false;
			Result = EFPBenchmarkResult::Failed;
			return;
		}
		if (bTraceRegionActive)
		{
			UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionEnd AutomatedBenchmarkCapture"));
			bTraceRegionActive = false;
		}
		// UE 5.5 Stop 仅排入停止请求，文件真正关闭由后续 IsConnected 读回确认。
		bTraceStopRequested = FTraceAuxiliary::Stop();
	}
}

// 等待真实写盘和会话关闭，再检查非空文件；超时进入失败隔离，但不放弃遗留资源清理。
void UfpstrueBenchmarkRunner::PollArtifacts()
{
	StopActiveProfilers();
	const bool bCsvFinished = !CsvCapture.IsValid() || (CsvCapture->WriteComplete.IsValid() && CsvCapture->WriteComplete.IsReady());
	const bool bTraceFinished = !bTraceActive || (bTraceStopRequested && !FTraceAuxiliary::IsConnected());
	// 尚未回收的资源继续保留租约；Quarantined 只隔离失败运行，不伪造写盘完成。
	if (!bCsvFinished || !bTraceFinished)
	{
		if (Phase != EFPBenchmarkPhase::Quarantined && FPlatformTime::Seconds() >= PhaseDeadline)
		{
			ClearStageTimers();
			Phase = EFPBenchmarkPhase::Quarantined;
			Result = EFPBenchmarkResult::Failed;
			UE_LOG(LogTemp, Error, TEXT("Automated benchmark artifact flush timed out: csv=%d trace=%d; result=Failed resources=Quarantined (late-start cleanup retained)."),
				bCsvFinished, bTraceFinished);
			ExitBenchmark();
		}
		// 结果已失败，但尚未回收：既不移除 CSV 首帧回调，也不把进程所有权标为空闲。
		return;
	}
	if (bTraceActive && bTraceStopRequested && bTraceFinished)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace stopped."));
	}
	// bCsvFinished 已确认 Future 可读；租约仅由独立 Ticker 释放，Runner 不维护第二套释放路径。
	// Future 已可读后再获取实际路径；文件存在且非空是 Runner 成功的最后一道产物门禁。
	// ★【重要边界】[验收边界] 写盘完成且产物非空才允许发布 Completed；Runner 成功后仍需外部脚本样本门禁。
	const FString CsvPath = CsvCapture.IsValid() ? CsvCapture->WriteComplete.Get() : FString();
	const bool bCsvArtifactValid = !CsvPath.IsEmpty() && IFileManager::Get().FileSize(*CsvPath) > 0;
	const bool bTraceArtifactValid = TraceOutputPath.IsEmpty() || IFileManager::Get().FileSize(*TraceOutputPath) > 0;
	if (Result == EFPBenchmarkResult::Completed && (!bCsvArtifactValid || !bTraceArtifactValid))
	{
		Result = EFPBenchmarkResult::Failed;
		UE_LOG(LogTemp, Error, TEXT("Automated benchmark artifact missing or empty: csv=%s trace=%s"), *CsvPath, *TraceOutputPath);
	}
	ClearStageTimers();
	bTraceActive = false;
	Phase = EFPBenchmarkPhase::Finished;
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark result: %s csv=%s trace=%s"), ResultName(Result), *CsvPath, *TraceOutputPath);
	if (Result == EFPBenchmarkResult::Completed)
	{
		// 保持脚本兼容，但只有写盘完成且文件存在才发出成功标记。
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark completed successfully."));
	}
	ExitBenchmark();
}

// 映射脚本约定的退出码，区分有效窗口、样本拒收、失败、取消和目标不支持。
uint8 UfpstrueBenchmarkRunner::GetResultExitCode(EFPBenchmarkResult InResult)
{
	// 与外部批处理约定：0 仅表示 Runner 完成；最终样本仍须通过脚本离线校验。
	switch (InResult)
	{
	case EFPBenchmarkResult::Completed: return 0;
	case EFPBenchmarkResult::InvalidSample: return 2;
	case EFPBenchmarkResult::Failed: return 3;
	case EFPBenchmarkResult::Cancelled: return 4;
	case EFPBenchmarkResult::Unsupported: return 5;
	default: return 3;
	}
}

// 仅在显式自动退出的独立游戏进程请求结束，不关闭编辑器或 PIE 宿主。
void UfpstrueBenchmarkRunner::ExitBenchmark()
{
	if (!FFPBenchmarkConfig::Get().bAutoQuit) return;
	const uint8 ExitCode = GetResultExitCode(Result);
	// 编辑器编译的 -game 独立进程也支持；PIE/编辑器宿主绝不请求进程退出。
	if (!GIsEditor && GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Game)
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark process exit: code=%d result=%s"), ExitCode, ResultName(Result));
		FPlatformMisc::RequestExitWithStatus(false, ExitCode, TEXT("fpstrueBenchmarkRunner"));
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark auto-quit suppressed for editor/PIE: code=%d result=%s"), ExitCode, ResultName(Result));
	}
}
