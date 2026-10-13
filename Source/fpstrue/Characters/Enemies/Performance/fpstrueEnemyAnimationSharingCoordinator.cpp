// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/Performance/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Runtime/fpstrueRuntimeOptions.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
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

// ==================== 状态适配：读取 Controller 状态与实际速度 ====================

// 由插件采样敌人的移动表现；意图为追击但速度为零时输出待机，不参与高层行为选择。
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

// 提供共享状态对应的原生枚举，确保状态整数与插件 Setup 使用同一编号。
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

// 启用并校验共享系统；只创建或复用自己拥有、骨架兼容的 World 共享池。
void UfpstrueEnemyAnimationSharingCoordinator::Start(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	// 一个 World 只允许本项目创建一个 Manager；任一资产或骨架校验失败都保持原有 AnimBP，不留下半初始化状态。
	if (bRunning)
	{
		return;
	}

	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
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

	// 优先复用本组件之前创建的池，不覆盖关卡或其他系统已经占有的共享 Manager。
	// ★【重要边界】[资源所有权] 一个 World 的共享 Manager 只复用本组件创建且骨架兼容的实例，不能覆盖其他系统的 Setup。
	UAnimationSharingManager* WorldManager = UAnimationSharingManager::GetManagerForWorld(GetWorld());
	if (WorldManager != nullptr && WorldManager == SharingManager && RuntimeSetup != nullptr)
	{
		// Stop 只注销 Follower，不销毁 World 共享池；只复用自己创建且骨架一致的实例。
		const AfpstrueEnemyCharacter* Defaults = InEnemyClass.GetDefaultObject();
		const USkeletalMesh* Mesh = Defaults != nullptr && Defaults->GetMesh() != nullptr
			? Defaults->GetMesh()->GetSkeletalMeshAsset() : nullptr;
		if (Mesh != nullptr && Mesh->GetSkeleton() == SharingSkeleton)
		{
			bRunning = true;
			return;
		}
	}
	if (WorldManager != nullptr)
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

// 关闭接纳入口并注销全部已登记、待登记 Follower；允许注销过程同步更新其他句柄。
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

// 退出时先注销敌人，再释放运行时配置引用；停止对局不等于立即销毁 World 共享池。
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

// 根据敌人默认骨架和配置动画创建待机、移动共享组；缺失或骨架不匹配时回退独立 AnimBP。
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

	// 资产全部通过校验后才创建临时 Setup；随机实例数是共享姿态池配置，不是敌人数上限。
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

// 资格变化时才加入或退出共享池；已登记敌人仅更新渲染显著性，不重复注册。
void UfpstrueEnemyAnimationSharingCoordinator::RefreshEnemyRegistration(AfpstrueEnemyCharacter* Enemy)
{
	// 该函数是幂等的：资格不变时只更新 Significance；资格改变时才真正注册或注销 Actor。
	if (!bRunning || SharingManager == nullptr || !IsValid(Enemy))
	{
		return;
	}

	USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh();
	USkeletalMesh* SkeletalMesh = CharacterMesh != nullptr ? CharacterMesh->GetSkeletalMeshAsset() : nullptr;
	if (!Enemy->CanUseAnimationSharing() || SkeletalMesh == nullptr || SkeletalMesh->GetSkeleton() != SharingSkeleton)
	{
		SuspendEnemy(Enemy);
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (!RegisteredActorHandles.Contains(EnemyKey) && !PendingActorRegistrations.Contains(EnemyKey))
	{
		PendingActorRegistrations.Add(EnemyKey);
		// ★【重要边界】[回调重入] 注册会同步交付句柄；先登记 pending，等插件调用返回后才能按新资格撤销，不能在句柄回调里反注册。
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

// 注销本敌人的 Follower，并把骨骼 LOD 控制权还给角色；重复调用不重复注销。
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
		// ★【重要边界】[资源所有权] 先移除本地登记再让插件注销；其余敌人的句柄可能同步更新，弱引用和 handle 都不是 Actor 所有权。
		SharingManager->UnregisterActor(Enemy);

		// Animation Sharing 注册或待注册时都可能接管此标志；退出后必须还给项目的骨骼 LOD 分级。
		TInlineComponentArray<USkeletalMeshComponent*, 4> OwnedMeshComponents;
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

// 接收插件注册或紧缩数组后的句柄变化；回调只更新本地登记，不能重入插件增删操作。
void UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated(int32 NewHandle, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy)
{
	// UE 5.5 在注册和注销的 RemoveAtSwap 中同步回调，此时插件数组可能尚未完成更新。
	// 这里只维护已知登记的句柄，绝不调用 Register/Unregister 或更新插件数据，避免重入。
	// Stop 期间也必须接收尚未注销对象的 swap 句柄；资格检查和注销由外层入口执行。
	// ★【重要边界】[回调重入] RemoveAtSwap 的句柄回调只能维护已知登记，不能调用插件增删接口或把已退出对象重新注册。
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
