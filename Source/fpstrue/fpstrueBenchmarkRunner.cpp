// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueBenchmarkRunner.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyAnimationSharingCoordinator.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueGameMode.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"

UfpstrueBenchmarkRunner::UfpstrueBenchmarkRunner()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueBenchmarkRunner::StartIfRequested(AfpstrueGameMode* InGameMode)
{
	GameMode = InGameMode;
	if (!FFPBenchmarkConfig::Get().bAutoBenchmark || !GameMode.IsValid())
	{
		return;
	}

	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UfpstrueBenchmarkRunner::BeginBenchmark);
}

void UfpstrueBenchmarkRunner::Cancel()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	FTimerManager& TimerManager = World->GetTimerManager();
	TimerManager.ClearTimer(ReadyTimerHandle);
	TimerManager.ClearTimer(StartTimerHandle);
	TimerManager.ClearTimer(StopTimerHandle);
	TimerManager.ClearTimer(ExitTimerHandle);

	if (bTraceActive)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionEnd AutomatedBenchmarkCapture"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.Stop"));
		bTraceActive = false;
	}
}

void UfpstrueBenchmarkRunner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Cancel();
	Super::EndPlay(EndPlayReason);
}

void UfpstrueBenchmarkRunner::BeginBenchmark()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	FMath::RandInit(BenchmarkConfig.Seed);
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark random seed: %d"), BenchmarkConfig.Seed);

	OwnerGameMode->StartGameMode();
	if (!OwnerGameMode->bGameRunning)
	{
		return;
	}

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTarget(OwnerGameMode->PlayerCharacter);
	}
	OwnerGameMode->PlayerCharacter->SetCanBeDamaged(false);
	UWidgetLayoutLibrary::RemoveAllWidgets(this);

	GetWorld()->GetTimerManager().SetTimer(ReadyTimerHandle, this, &UfpstrueBenchmarkRunner::WaitForBenchmarkReady, 0.25f, true, 0.0f);
}

void UfpstrueBenchmarkRunner::WaitForBenchmarkReady()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr || !OwnerGameMode->bGameRunning)
	{
		GetWorld()->GetTimerManager().ClearTimer(ReadyTimerHandle);
		return;
	}

	if (OwnerGameMode->PendingEnemySpawnCount > 0)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	GetWorld()->GetTimerManager().ClearTimer(ReadyTimerHandle);
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark ready: requested=%d alive=%d warmup=%.1fs"), BenchmarkConfig.EnemyCount,
		   OwnerGameMode->RegisteredEnemies.Num(), BenchmarkConfig.WarmupSeconds);

	GetWorld()->GetTimerManager().SetTimer(StartTimerHandle, this, &UfpstrueBenchmarkRunner::StartCapture, BenchmarkConfig.WarmupSeconds,
										   false);
}

void UfpstrueBenchmarkRunner::StartCapture()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	ApplyDiagnosticOverrides();

	if (!BenchmarkConfig.TraceFile.IsEmpty())
	{
		const FString TraceCommand = FString::Printf(TEXT("Trace.File %s cpu,frame,bookmark,task,stats"), *BenchmarkConfig.TraceFile);
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TraceCommand);
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionBegin AutomatedBenchmarkCapture"));
		bTraceActive = true;
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace started: %s"), *BenchmarkConfig.TraceFile);
	}

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

	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile start"));
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture started: requested=%d alive=%d duration=%.1fs"), BenchmarkConfig.EnemyCount,
		   OwnerGameMode->RegisteredEnemies.Num(), BenchmarkConfig.DurationSeconds);

	GetWorld()->GetTimerManager().SetTimer(StopTimerHandle, this, &UfpstrueBenchmarkRunner::StopCapture, BenchmarkConfig.DurationSeconds,
										   false);
}

void UfpstrueBenchmarkRunner::ApplyDiagnosticOverrides()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	int32 AppliedEnemyCount = 0;
	int32 FullRateMovementCount = 0;
	int32 MidRateMovementCount = 0;
	int32 FarRateMovementCount = 0;
	int32 AttackingEnemyCount = 0;
	int32 ShadowCastingEnemyCount = 0;
	int32 RayTracingVisibleEnemyCount = 0;
	int32 MovementTickEnabledCount = 0;
	int32 SkeletalMeshTickEnabledCount = 0;
	int32 RenderFullCount = 0;
	int32 RenderReducedCount = 0;
	int32 RenderBackgroundCount = 0;
	int32 LOD0Count = 0;
	int32 LOD1Count = 0;
	int32 LOD2PlusCount = 0;

	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (Enemy == nullptr)
		{
			continue;
		}

		Enemy->ApplyBenchmarkDiagnosticOverrides(BenchmarkConfig.bDisableAttackSweep, BenchmarkConfig.bDisableEnemyPawnCollision,
												 BenchmarkConfig.bDisableCharacterMovementTick);

		if (AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
		{
			EnemyController->ApplyBenchmarkPathFollowingTickOverride(BenchmarkConfig.bDisablePathFollowingTick);
		}

		if (const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			MovementTickEnabledCount += Movement->IsComponentTickEnabled() ? 1 : 0;
			const float TickInterval = Movement->GetComponentTickInterval();
			if (TickInterval <= KINDA_SMALL_NUMBER)
			{
				++FullRateMovementCount;
			}
			else if (Enemy->GetGameplaySignificanceTier() == EFPEnemySignificanceTier::Reduced)
			{
				++MidRateMovementCount;
			}
			else
			{
				++FarRateMovementCount;
			}
		}

		AttackingEnemyCount += Enemy->IsAttacking() ? 1 : 0;
		switch (Enemy->GetRenderSignificanceTier())
		{
		case EFPEnemyRenderSignificanceTier::Full:
			++RenderFullCount;
			break;
		case EFPEnemyRenderSignificanceTier::Reduced:
			++RenderReducedCount;
			break;
		case EFPEnemyRenderSignificanceTier::Background:
		default:
			++RenderBackgroundCount;
			break;
		}

		const int32 AppliedMinLOD = Enemy->GetAppliedMinimumLOD();
		if (AppliedMinLOD <= 0)
		{
			++LOD0Count;
		}
		else if (AppliedMinLOD == 1)
		{
			++LOD1Count;
		}
		else
		{
			++LOD2PlusCount;
		}

		if (USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh())
		{
			if (BenchmarkConfig.bDisableSkeletalMeshTick)
			{
				CharacterMesh->SetComponentTickEnabled(false);
			}
			SkeletalMeshTickEnabledCount += CharacterMesh->IsComponentTickEnabled() ? 1 : 0;
			ShadowCastingEnemyCount += CharacterMesh->CastShadow ? 1 : 0;
			RayTracingVisibleEnemyCount += CharacterMesh->bVisibleInRayTracing ? 1 : 0;
		}
		++AppliedEnemyCount;
	}

	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark diagnostics applied: enemies=%d attackSweepOff=%d pawnCollisionOff=%d pathFollowingTickOff=%d "
				"characterMovementTickOff=%d skeletalMeshTickOff=%d significanceOff=%d"),
		   AppliedEnemyCount, BenchmarkConfig.bDisableAttackSweep ? 1 : 0, BenchmarkConfig.bDisableEnemyPawnCollision ? 1 : 0,
		   BenchmarkConfig.bDisablePathFollowingTick ? 1 : 0, BenchmarkConfig.bDisableCharacterMovementTick ? 1 : 0,
		   BenchmarkConfig.bDisableSkeletalMeshTick ? 1 : 0, BenchmarkConfig.bDisableEnemySignificance ? 1 : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark enemy snapshot: movementFull=%d movementMid=%d movementFar=%d movementTickEnabled=%d skeletalMeshTickEnabled=%d "
				"attacking=%d castingShadow=%d rayTracingVisible=%d animationSharingFollowers=%d"),
		   FullRateMovementCount, MidRateMovementCount, FarRateMovementCount, MovementTickEnabledCount, SkeletalMeshTickEnabledCount,
		   AttackingEnemyCount, ShadowCastingEnemyCount, RayTracingVisibleEnemyCount,
		   OwnerGameMode->EnemyAnimationSharingCoordinator != nullptr
			   ? OwnerGameMode->EnemyAnimationSharingCoordinator->GetRegisteredEnemyCount()
			   : 0);
	UE_LOG(LogTemp, Display,
		   TEXT("Benchmark render significance snapshot: renderFull=%d renderReduced=%d renderBackground=%d lod0=%d lod1=%d lod2Plus=%d "
				"fullBudget=%d shadowBudget=%d rayTracingBudget=%d"),
		   RenderFullCount, RenderReducedCount, RenderBackgroundCount, LOD0Count, LOD1Count, LOD2PlusCount,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies,
		   OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies);
}

void UfpstrueBenchmarkRunner::StopCapture()
{
	UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("csvprofile stop"));
	if (bTraceActive)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.RegionEnd AutomatedBenchmarkCapture"));
		UKismetSystemLibrary::ExecuteConsoleCommand(this, TEXT("Trace.Stop"));
		bTraceActive = false;
		UE_LOG(LogTemp, Display, TEXT("Automated benchmark Insights trace stopped."));
	}
	UE_LOG(LogTemp, Display, TEXT("Automated benchmark capture stopped."));

	if (FFPBenchmarkConfig::Get().bAutoQuit)
	{
		GetWorld()->GetTimerManager().SetTimer(ExitTimerHandle, this, &UfpstrueBenchmarkRunner::ExitBenchmark, 1.0f, false);
	}
}

void UfpstrueBenchmarkRunner::ExitBenchmark()
{
	UKismetSystemLibrary::QuitGame(this, UGameplayStatics::GetPlayerController(this, 0), EQuitPreference::Quit, false);
}
