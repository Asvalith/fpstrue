// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Testing/Benchmarks/fpstrueBenchmarkConfig.h"
#include "Characters/Enemies/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationSharingManager.h"
#include "AnimationSharingSetup.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
constexpr float SharedMovingSpeedThreshold = 10.0f;
}

/*
 * Animation Sharing 插件的项目适配层。
 * 它把现有 AI 的 Idle/Chase 状态映射为共享动画状态，并只让满足渲染分级且不处于战斗保护期的普通敌人
 * 成为 Follower；攻击 Montage、Notify 和死亡表现仍使用敌人自己的骨骼动画实例。
 *
 * 共享的只是 Idle/Moving 姿态计算，不共享 Actor Transform、AIController、CharacterMovement、生命值或攻击状态。
 * Coordinator 统一拥有 World 级 Manager、运行时 Setup 和 Actor Handle；EnemyCharacter 只通过加入/退出接口参与，
 * 避免每个敌人各自创建一套 Sharing 配置。
 */

// ==================== 状态适配：复用现有 AI FSM ====================

void UfpstrueEnemyAnimationSharingStateProcessor::ProcessActorState_Implementation(int32& OutState, AActor* InActor, uint8 CurrentState,
																				   uint8 OnDemandState, bool& bShouldProcess)
{
	// AIState 给出玩法意图，实际速度修正表现：已到槽位的 Chase 敌人应共享 Idle，而不是原地播放跑步。
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
	// 把项目 AI 状态枚举交给插件，插件据此解释 ProcessActorState 输出的整数状态。
	return StaticEnum<EFPEnemyAIState>();
}

// ==================== 初始化与生命周期 ====================

// Coordinator 不逐帧运行；默认动画使用软引用，只有真正启用 Sharing 时才同步加载。
// 项目级素材路径来自 DefaultGame.ini；组件蓝图保存的同名属性仍可覆盖这些构造默认值。
UfpstrueEnemyAnimationSharingCoordinator::UfpstrueEnemyAnimationSharingCoordinator()
{
	PrimaryComponentTick.bCanEverTick = false;

	// 这里只读配置，不加载资源、不写配置；路径缺失时由原有 Setup 校验安全回退到独立 AnimBP。
	if (GConfig != nullptr)
	{
		FString IdlePath;
		FString MovingPath;
		GConfig->GetString(TEXT("fpstrue.EnemyAnimationSharing"), TEXT("IdleAnimation"), IdlePath, GGameIni);
		GConfig->GetString(TEXT("fpstrue.EnemyAnimationSharing"), TEXT("MovingAnimation"), MovingPath, GGameIni);
		IdleAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(IdlePath));
		MovingAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(MovingPath));
	}
}

void UfpstrueEnemyAnimationSharingCoordinator::Start(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	// 一个 World 只允许本项目创建一个 Manager；任一资产或骨架校验失败都保持原有 AnimBP，不留下半初始化状态。
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
	// 先停止接纳新对象，再逐个注销；插件在 RemoveAtSwap 期间仍会同步更新其他对象的句柄。
	bRunning = false;
	TArray<TWeakObjectPtr<AfpstrueEnemyCharacter>> RegisteredEnemies;
	RegisteredActorHandles.GenerateKeyArray(RegisteredEnemies);
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : PendingActorRegistrations)
	{
		RegisteredEnemies.AddUnique(EnemyPtr);
	}
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		SuspendEnemy(EnemyPtr.Get());
	}

	RegisteredActorHandles.Reset();
	PendingActorRegistrations.Reset();
}

void UfpstrueEnemyAnimationSharingCoordinator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 世界退出时释放 Manager、运行时 Setup 和 Skeleton 的强引用，允许 UObject GC 回收临时配置。
	Stop();
	SharingManager = nullptr;
	RuntimeSetup = nullptr;
	SharingSkeleton = nullptr;
	Super::EndPlay(EndPlayReason);
}

// ==================== 运行时 Setup 构建 ====================

bool UfpstrueEnemyAnimationSharingCoordinator::BuildRuntimeSetup(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	// Setup 在运行时从敌人 CDO 的 SkeletalMesh/Skeleton 构建，确保共享动画和实际敌人使用同一骨架。
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
	// 该函数是幂等的：资格不变时只更新 Significance；资格改变时才真正注册或注销 Actor。
	if (!bRunning || SharingManager == nullptr || !IsValid(Enemy))
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	const bool bIsRegisteredWithSharing = RegisteredActorHandles.Contains(EnemyKey);
	const bool bRegistrationPending = PendingActorRegistrations.Contains(EnemyKey);
	const bool bShouldShare = Enemy->CanUseAnimationSharing();
	if (!bShouldShare)
	{
		if (bIsRegisteredWithSharing || bRegistrationPending)
		{
			SuspendEnemy(Enemy);
		}
		return;
	}

	USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh();
	USkeletalMesh* SkeletalMesh = CharacterMesh != nullptr ? CharacterMesh->GetSkeletalMeshAsset() : nullptr;
	if (SkeletalMesh == nullptr || SkeletalMesh->GetSkeleton() != SharingSkeleton)
	{
		SuspendEnemy(Enemy);
		return;
	}

	if (!bIsRegisteredWithSharing && !bRegistrationPending)
	{
		PendingActorRegistrations.Add(EnemyKey);
		SharingManager->RegisterActorWithSkeleton(
			Enemy, SharingSkeleton,
			FUpdateActorHandle::CreateUObject(this, &UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated, EnemyKey));
		// UE 5.5 在本次调用内同步交付句柄。失败时可能不回调，必须释放 pending 以允许下一轮重试。
		PendingActorRegistrations.Remove(EnemyKey);
		// 只有插件调用返回后才能撤销注册，不能在其正在修改内部数组的句柄回调里反注册。
		if (!bRunning || !IsValid(Enemy) || !Enemy->CanUseAnimationSharing())
		{
			SuspendEnemy(Enemy);
			return;
		}
	}

	if (const uint32* Handle = RegisteredActorHandles.Find(EnemyKey))
	{
		// 插件内部的 Leader Tick / Blend 预算只读取渲染重要性，不读取 AI 距离分层。
		SharingManager->UpdateSignificanceForActorHandle(*Handle, FMath::Clamp(Enemy->GetRenderSignificanceScore(), 0.0f, 1.0f));
	}
}

void UfpstrueEnemyAnimationSharingCoordinator::SuspendEnemy(AfpstrueEnemyCharacter* Enemy)
{
	// 战斗/死亡退出共享后把 LOD 控制权还给 EnemyCharacter，避免插件残留标志覆盖项目自己的骨骼 LOD 策略。
	if (SharingManager == nullptr || Enemy == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	const bool bWasPending = PendingActorRegistrations.Remove(EnemyKey) > 0;
	const bool bWasRegistered = RegisteredActorHandles.Remove(EnemyKey) > 0;
	if (bWasPending || bWasRegistered)
	{
		SharingManager->UnregisterActor(Enemy);

		// Animation Sharing 注册或待注册时都可能接管此标志；退出后必须还给项目的骨骼 LOD 分级。
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
	// UE 5.5 在注册和注销的 RemoveAtSwap 中同步回调，此时插件数组可能尚未完成更新。
	// 这里只维护已知登记的句柄，绝不调用 Register/Unregister 或更新插件数据，避免重入。
	// Stop 期间也必须接收尚未注销对象的 swap 句柄；资格检查和注销由外层入口执行。
	AfpstrueEnemyCharacter* EnemyCharacter = Enemy.Get();
	if (EnemyCharacter == nullptr)
	{
		PendingActorRegistrations.Remove(Enemy);
		RegisteredActorHandles.Remove(Enemy);
		return;
	}

	const bool bKnownRegistration = PendingActorRegistrations.Remove(Enemy) > 0 || RegisteredActorHandles.Contains(Enemy);
	if (NewHandle == INDEX_NONE)
	{
		RegisteredActorHandles.Remove(Enemy);
		return;
	}

	if (!bKnownRegistration)
	{
		// 已经退出的登记不能由未知回调重新建立。
		return;
	}

	RegisteredActorHandles.FindOrAdd(Enemy) = static_cast<uint32>(NewHandle);
}
