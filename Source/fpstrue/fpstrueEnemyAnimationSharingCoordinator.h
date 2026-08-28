// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimationSharingTypes.h"
#include "Components/ActorComponent.h"
#include "fpstrueEnemyAnimationSharingCoordinator.generated.h"

class AfpstrueEnemyCharacter;
class UAnimationSharingManager;
class UAnimationSharingSetup;
class UAnimSequence;
class USkeleton;

/**
 * Animation Sharing 的原生状态处理器。
 * 共享层直接复用 AI FSM 的 Idle / Chase 状态；攻击、受击和死亡会先退出共享系统，
 * 继续由敌人原有 AnimBP、Montage、Notify 和布料/物理链处理。
 */
UCLASS()
class FPSTRUE_API UfpstrueEnemyAnimationSharingStateProcessor final : public UAnimationSharingStateProcessor
{
	GENERATED_BODY()

public:
	// 根据 AI FSM 和实际速度，把敌人映射到共享待机或跑步状态。
	virtual void ProcessActorState_Implementation(int32& OutState, AActor* InActor, uint8 CurrentState, uint8 OnDemandState,
												  bool& bShouldProcess) override;
	// 告诉插件共享状态使用敌人的 AI FSM 枚举。
	virtual UEnum* GetAnimationStateEnum_Implementation() override;
};

/**
 * Animation Sharing 的唯一运行时所有者。
 * Render Significance 决定谁可进入共享池以及共享 Leader 是否值得更新，
 * Gameplay Significance 不读取视锥，也不依赖本组件。
 */
UCLASS(ClassGroup = (Performance))
class FPSTRUE_API UfpstrueEnemyAnimationSharingCoordinator final : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建不参与逐帧 Tick 的共享动画协调器。
	UfpstrueEnemyAnimationSharingCoordinator();

	// ==================== 生命周期与 Follower 接口 ====================

	// GameMode 在首个敌人生成前调用，创建本 World 唯一共享池。
	void Start(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass);
	// GameMode 结束时注销全部 Follower 并停止接纳敌人。
	void Stop();
	// Render Significance 应用档位后调用，决定敌人加入、保留或退出共享池。
	void RefreshEnemyRegistration(AfpstrueEnemyCharacter* Enemy);
	// 攻击、受击和死亡前立即退出共享池，恢复独立 AnimBP 与 LOD 控制。
	void SuspendEnemy(AfpstrueEnemyCharacter* Enemy);

	// CSV Benchmark 读取当前 Follower 数量。
	int32 GetRegisteredEnemyCount() const { return RegisteredActorHandles.Num(); }

protected:
	// World 退出时释放共享 Manager、运行时 Setup 和 Skeleton 引用。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ==================== 共享池配置 ====================

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing")
	bool bEnableAnimationSharing = true;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing")
	TSoftObjectPtr<UAnimSequence> IdleAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing")
	TSoftObjectPtr<UAnimSequence> MovingAnimation;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing", meta = (ClampMin = "1"))
	int32 IdleRandomizedInstances = 4;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing", meta = (ClampMin = "1"))
	int32 MovingRandomizedInstances = 4;

	UPROPERTY(EditDefaultsOnly, Category = "Performance|Animation Sharing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LeaderTickSignificanceThreshold = 0.20f;

private:
	// ==================== Setup 与 Handle 维护 ====================

	// 从敌人骨架和待机/移动动画构建插件所需的运行时 Setup。
	bool BuildRuntimeSetup(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass);
	// 保存插件返回的 Actor Handle，后续用于更新 Leader Tick 显著性。
	void HandleActorHandleUpdated(int32 NewHandle, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy);

	// ==================== 运行时状态 ====================

	UPROPERTY(Transient)
	TObjectPtr<UAnimationSharingManager> SharingManager;

	UPROPERTY(Transient)
	TObjectPtr<UAnimationSharingSetup> RuntimeSetup;

	UPROPERTY(Transient)
	TObjectPtr<USkeleton> SharingSkeleton;

	TMap<TWeakObjectPtr<AfpstrueEnemyCharacter>, uint32> RegisteredActorHandles;
	bool bRunning = false;
};
