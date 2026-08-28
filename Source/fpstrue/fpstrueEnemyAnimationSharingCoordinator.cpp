// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyAnimationSharingCoordinator.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyCharacter.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationSharingManager.h"
#include "AnimationSharingSetup.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

namespace
{
constexpr float SharedMovingSpeedThreshold = 10.0f;
}

// ==================== 状态适配：复用现有 AI FSM ====================

void UfpstrueEnemyAnimationSharingStateProcessor::ProcessActorState_Implementation(int32& OutState, AActor* InActor, uint8 CurrentState,
																				   uint8 OnDemandState, bool& bShouldProcess)
{
	bShouldProcess = IsValid(InActor);
	if (!bShouldProcess)
	{
		OutState = static_cast<int32>(EFPEnemyAIState::Idle);
		return;
	}

	const AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(InActor);
	const AfpstrueEnemyAIController* EnemyController = Enemy != nullptr ? Cast<AfpstrueEnemyAIController>(Enemy->GetController()) : nullptr;
	const EFPEnemyAIState AIState = EnemyController != nullptr ? EnemyController->GetAIState() : EFPEnemyAIState::Idle;
	const bool bMoving =
		AIState == EFPEnemyAIState::Chase && InActor->GetVelocity().SizeSquared2D() > FMath::Square(SharedMovingSpeedThreshold);

	// Chase 可能已经到达包围槽位并停止，因此速度只负责把“静止 Chase”映射回 Idle。
	OutState = static_cast<int32>(bMoving ? EFPEnemyAIState::Chase : EFPEnemyAIState::Idle);
}

UEnum* UfpstrueEnemyAnimationSharingStateProcessor::GetAnimationStateEnum_Implementation()
{
	return StaticEnum<EFPEnemyAIState>();
}

// ==================== 初始化与生命周期 ====================

UfpstrueEnemyAnimationSharingCoordinator::UfpstrueEnemyAnimationSharingCoordinator()
{
	PrimaryComponentTick.bCanEverTick = false;

	IdleAnimation = TSoftObjectPtr<UAnimSequence>(
		FSoftObjectPath(TEXT("/Game/EnemyWarriorAnimPack/Animations/InPlace/Misc/EnemyWarrior_Idle_InP.EnemyWarrior_Idle_InP")));
	MovingAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(
		TEXT("/Game/EnemyWarriorAnimPack/Animations/InPlace/Movement/EnemyWarrior_Running_Forward_InP.EnemyWarrior_Running_Forward_InP")));
}

void UfpstrueEnemyAnimationSharingCoordinator::Start(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	if (bRunning)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!bEnableAnimationSharing || BenchmarkConfig.bDisableEnemyAnimationSharing || BenchmarkConfig.bDisableAnimationOptimizations)
	{
		UE_LOG(LogTemp, Display, TEXT("Enemy Animation Sharing disabled: feature=%d ablation=%d animationOptimizationsOff=%d"),
			   bEnableAnimationSharing ? 1 : 0, BenchmarkConfig.bDisableEnemyAnimationSharing ? 1 : 0,
			   BenchmarkConfig.bDisableAnimationOptimizations ? 1 : 0);
		return;
	}

	if (!UAnimationSharingManager::AnimationSharingEnabled())
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing skipped: a.Sharing.Enabled is false."));
		return;
	}

	if (UAnimationSharingManager::GetManagerForWorld(GetWorld()) != nullptr)
	{
		// 一个 World 只能有一个 Manager；不覆盖关卡或其他系统已经创建的 Setup。
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing skipped: this World already owns an Animation Sharing manager."));
		return;
	}

	if (!BuildRuntimeSetup(InEnemyClass) || !UAnimationSharingManager::CreateAnimationSharingManager(this, RuntimeSetup))
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing setup could not be created."));
		RuntimeSetup = nullptr;
		SharingSkeleton = nullptr;
		return;
	}

	SharingManager = UAnimationSharingManager::GetManagerForWorld(GetWorld());
	if (SharingManager == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing manager was not available after creation."));
		return;
	}
	bRunning = true;
	UE_LOG(LogTemp, Display, TEXT("Enemy Animation Sharing: running=%d skeleton=%s idleLeaders=%d movingLeaders=%d tickThreshold=%.2f"),
		   bRunning ? 1 : 0, *GetNameSafe(SharingSkeleton), FMath::Max(IdleRandomizedInstances, 1),
		   FMath::Max(MovingRandomizedInstances, 1), FMath::Clamp(LeaderTickSignificanceThreshold, 0.0f, 1.0f));
}

void UfpstrueEnemyAnimationSharingCoordinator::Stop()
{
	TArray<TWeakObjectPtr<AfpstrueEnemyCharacter>> RegisteredEnemies;
	RegisteredActorHandles.GenerateKeyArray(RegisteredEnemies);
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		SuspendEnemy(EnemyPtr.Get());
	}

	RegisteredActorHandles.Reset();
	bRunning = false;
}

void UfpstrueEnemyAnimationSharingCoordinator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Stop();
	SharingManager = nullptr;
	RuntimeSetup = nullptr;
	SharingSkeleton = nullptr;
	Super::EndPlay(EndPlayReason);
}

// ==================== 运行时 Setup 构建 ====================

bool UfpstrueEnemyAnimationSharingCoordinator::BuildRuntimeSetup(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	const AfpstrueEnemyCharacter* EnemyDefaults = InEnemyClass ? InEnemyClass->GetDefaultObject<AfpstrueEnemyCharacter>() : nullptr;
	const USkeletalMeshComponent* DefaultMeshComponent = EnemyDefaults != nullptr ? EnemyDefaults->GetMesh() : nullptr;
	USkeletalMesh* SkeletalMesh = DefaultMeshComponent != nullptr ? DefaultMeshComponent->GetSkeletalMeshAsset() : nullptr;
	SharingSkeleton = SkeletalMesh != nullptr ? SkeletalMesh->GetSkeleton() : nullptr;

	UAnimSequence* IdleSequence = IdleAnimation.LoadSynchronous();
	UAnimSequence* MovingSequence = MovingAnimation.LoadSynchronous();
	if (SkeletalMesh == nullptr || SharingSkeleton == nullptr || IdleSequence == nullptr || MovingSequence == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing assets invalid: mesh=%s skeleton=%s idle=%s moving=%s"),
			   *GetNameSafe(SkeletalMesh), *GetNameSafe(SharingSkeleton), *GetNameSafe(IdleSequence), *GetNameSafe(MovingSequence));
		return false;
	}

	if (IdleSequence->GetSkeleton() != SharingSkeleton || MovingSequence->GetSkeleton() != SharingSkeleton)
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing skeleton mismatch: enemy=%s idle=%s moving=%s"),
			   *GetNameSafe(SharingSkeleton), *GetNameSafe(IdleSequence->GetSkeleton()), *GetNameSafe(MovingSequence->GetSkeleton()));
		return false;
	}

	RuntimeSetup = NewObject<UAnimationSharingSetup>(this, TEXT("EnemyAnimationSharingRuntimeSetup"));
	RuntimeSetup->ScalabilitySettings.UseBlendTransitions.Default = false;
	RuntimeSetup->ScalabilitySettings.MaximumNumberConcurrentBlends.Default = 1;
	RuntimeSetup->ScalabilitySettings.TickSignificanceValue.Default = FMath::Clamp(LeaderTickSignificanceThreshold, 0.0f, 1.0f);

	FPerSkeletonAnimationSharingSetup& SkeletonSetup = RuntimeSetup->SkeletonSetups.AddDefaulted_GetRef();
	SkeletonSetup.Skeleton = SharingSkeleton;
	SkeletonSetup.SkeletalMesh = SkeletalMesh;
	SkeletonSetup.StateProcessorClass = UfpstrueEnemyAnimationSharingStateProcessor::StaticClass();

	FAnimationStateEntry& IdleState = SkeletonSetup.AnimationStates.AddDefaulted_GetRef();
	IdleState.State = static_cast<uint8>(EFPEnemyAIState::Idle);
	FAnimationSetup& IdleSetup = IdleState.AnimationSetups.AddDefaulted_GetRef();
	IdleSetup.AnimSequence = IdleSequence;
	IdleSetup.NumRandomizedInstances.Default = FMath::Max(IdleRandomizedInstances, 1);
	IdleSetup.Enabled.Default = true;

	FAnimationStateEntry& MovingState = SkeletonSetup.AnimationStates.AddDefaulted_GetRef();
	MovingState.State = static_cast<uint8>(EFPEnemyAIState::Chase);
	FAnimationSetup& MovingSetup = MovingState.AnimationSetups.AddDefaulted_GetRef();
	MovingSetup.AnimSequence = MovingSequence;
	MovingSetup.NumRandomizedInstances.Default = FMath::Max(MovingRandomizedInstances, 1);
	MovingSetup.Enabled.Default = true;

	return true;
}

// ==================== Follower 注册与退出 ====================

void UfpstrueEnemyAnimationSharingCoordinator::RefreshEnemyRegistration(AfpstrueEnemyCharacter* Enemy)
{
	if (!bRunning || SharingManager == nullptr || !IsValid(Enemy))
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	const bool bIsRegisteredWithSharing = RegisteredActorHandles.Contains(EnemyKey);
	const bool bShouldShare = Enemy->CanUseAnimationSharing();
	if (!bShouldShare)
	{
		if (bIsRegisteredWithSharing)
		{
			SuspendEnemy(Enemy);
		}
		return;
	}

	USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh();
	USkeletalMesh* SkeletalMesh = CharacterMesh != nullptr ? CharacterMesh->GetSkeletalMeshAsset() : nullptr;
	if (SkeletalMesh == nullptr || SkeletalMesh->GetSkeleton() != SharingSkeleton)
	{
		return;
	}

	if (!bIsRegisteredWithSharing)
	{
		SharingManager->RegisterActorWithSkeleton(
			Enemy, SharingSkeleton,
			FUpdateActorHandle::CreateUObject(this, &UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated, EnemyKey));
	}

	if (const uint32* Handle = RegisteredActorHandles.Find(EnemyKey))
	{
		// 插件内部的 Leader Tick / Blend 预算只读取渲染重要性，不读取 AI 距离分层。
		SharingManager->UpdateSignificanceForActorHandle(*Handle, FMath::Clamp(Enemy->GetRenderSignificanceScore(), 0.0f, 1.0f));
	}
}

void UfpstrueEnemyAnimationSharingCoordinator::SuspendEnemy(AfpstrueEnemyCharacter* Enemy)
{
	if (SharingManager == nullptr || Enemy == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (RegisteredActorHandles.Remove(EnemyKey) > 0)
	{
		SharingManager->UnregisterActor(Enemy);

		// Animation Sharing 注册时会接管此标志；退出后必须还给项目的骨骼 LOD 分级。
		TArray<USkeletalMeshComponent*> OwnedMeshComponents;
		Enemy->GetComponents(OwnedMeshComponents);
		for (USkeletalMeshComponent* MeshComponent : OwnedMeshComponents)
		{
			if (MeshComponent != nullptr)
			{
				MeshComponent->bIgnoreLeaderPoseComponentLOD = false;
			}
		}
	}
}

// ==================== Handle 回调 ====================

void UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated(int32 NewHandle, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy)
{
	if (Enemy.IsValid() && NewHandle != INDEX_NONE)
	{
		RegisteredActorHandles.FindOrAdd(Enemy) = static_cast<uint32>(NewHandle);
	}
}
