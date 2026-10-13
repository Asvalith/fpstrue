// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Runtime/fpstrueRuntimeOptions.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "DrawDebugHelpers.h"
#include "EngineGlobals.h"
#include "Engine/World.h"
#include "NavigationSystem.h"

/*
 * 多敌人共享的群体协调器。
 * 它不替代单个敌人的行为树决策，只集中维护必须全局一致的资源：玩家位置快照、稳定槽位、
 * 并发攻击名额和每帧 MoveTo 提交预算，从而减少围攻拥堵和重复导航请求。
 *
 * 数据结构职责：
 *   TArray<Slot> 保存固定内外环布局；TMap<Enemy, Index> 保存稳定占位；TSet<Enemy> 保存攻击许可。
 * 敌人键使用 TWeakObjectPtr，Manager 不拥有敌人生命周期；死亡/销毁由主动释放和失效项清理双重兜底。
 * 玩家位置与槽位 NavMesh 投影按 Timer/位移阈值批量刷新，单个 AIController 只读取缓存结果。
 */

// ==================== 生命周期与目标注入 ====================

// Manager 不需要逐帧 Tick；目标快照和调试绘制分别由低频 Timer 驱动。
AfpstrueSurroundManager::AfpstrueSurroundManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

// 构建稳定内外环槽位；只有调试开关开启时才启动可视化 Timer。
void AfpstrueSurroundManager::BeginPlay()
{
	// 关卡开始时建立固定槽位拓扑；调试绘制只有显式开启后才产生 Timer 成本。
	Super::BeginPlay();
	BuildSlots();

	if (bDrawDebugSlots)
	{
		GetWorldTimerManager().SetTimer(DebugDrawTimerHandle, this, &AfpstrueSurroundManager::DrawDebugSlots, DebugDrawInterval, true);
	}
}

// 停止调试与共享快照更新，并清除当前对局的槽位、许可和缓存。
void AfpstrueSurroundManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 清理两个 Timer、弱引用容器和槽位占用，避免关卡切换后保留旧玩家或敌人。
	GetWorldTimerManager().ClearTimer(DebugDrawTimerHandle);
	ResetManager();
	Super::EndPlay(EndPlayReason);
}

// 注入唯一共享目标；换目标时撤销全部旧分配，有效目标建立快照并启动低频刷新。
void AfpstrueSurroundManager::SetTargetCharacter(AfpstrueCharacter* NewTargetCharacter)
{
	// GameMode 在开局注入唯一玩家；目标变化时重建快照并重新启动共享刷新 Timer。
	if (TargetCharacter == NewTargetCharacter && bHasSharedTargetSnapshot)
	{
		return;
	}

	// 槽位、许可和投影属于同一个目标；换目标不能继承上一目标的分配。
	if (TargetCharacter != NewTargetCharacter)
	{
		ResetManager();
	}
	TargetCharacter = NewTargetCharacter;
	if (!IsValid(TargetCharacter))
	{
		bHasSharedTargetSnapshot = false;
		GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
		return;
	}

	UpdateSharedTargetSnapshot(true);
	// Timer 回调只负责触发带位移阈值的刷新，具体缓存更新集中在下层函数。
	GetWorldTimerManager().SetTimer(SharedTargetTimerHandle,
		FTimerDelegate::CreateUObject(this, &AfpstrueSurroundManager::UpdateSharedTargetSnapshot, false),
		FMath::Max(SharedTargetRefreshInterval, 0.05f), true);
}

// ==================== 共享目标与 NavMesh 投影缓存 ====================

// 按位移、年龄或导航重试刷新目标快照；只有位置或导航缓存需要更新时才批量重新投影。
void AfpstrueSurroundManager::UpdateSharedTargetSnapshot(bool bForce)
{
	// 位移阈值、最大陈旧时间和导航重试共同限定缓存寿命。
	if (!IsValid(TargetCharacter))
	{
		ResetManager();
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	const FVector CurrentTargetLocation = TargetCharacter->GetActorLocation();
	const bool bMovedEnough =
		FVector::DistSquared2D(CurrentTargetLocation, CachedTargetLocation) >= FMath::Square(SharedTargetMoveThreshold);
	const bool bSnapshotExpired = Now - SharedTargetSnapshotTime >= FMath::Max(0.05f, SharedTargetMaxAge);
	const bool bRetryNavigation = bNeedsNavigationRetry && Now >= NextNavigationRetryTime;
	const bool bNavigationExpired = Now - NavigationCacheTime >= FMath::Max(0.05f, NavigationCacheMaxAge);
	if (!bForce && bHasSharedTargetSnapshot && !bMovedEnough && !bSnapshotExpired && !bRetryNavigation && !bNavigationExpired)
	{
		return;
	}

	// 发布新位置后再更新关联的槽位投影，成功投影与失败重试各自有有限缓存寿命。
	const bool bLocationChanged = !CurrentTargetLocation.Equals(CachedTargetLocation, 1.0f);
	CachedTargetLocation = CurrentTargetLocation;
	SharedTargetSnapshotTime = Now;
	const bool bReproject = bForce || !bHasSharedTargetSnapshot || bLocationChanged || bRetryNavigation || bNavigationExpired;
	bHasSharedTargetSnapshot = true;
	if (bReproject)
	{
		RebuildProjectedSlotCache();
	}
}

// 创建均匀极角分布的内外环槽位；固定拓扑不因每轮 AI 决策重新分配。
void AfpstrueSurroundManager::BuildSlots()
{
	// 槽位只保存相对角度和半径；世界位置基于最新玩家快照统一计算，保证所有敌人使用同一参考系。
	SurroundSlots.Reset();

	// 为一层环创建角度和半径，实际世界坐标稍后根据共享玩家快照计算。
	const auto AddRing = [this](int32 RingIndex, int32 SlotCount, float Radius)
	{
		const int32 SafeSlotCount = FMath::Max(SlotCount, 1);
		for (int32 SlotIndex = 0; SlotIndex < SafeSlotCount; ++SlotIndex)
		{
			FfpstrueSurroundSlot& Slot = SurroundSlots.AddDefaulted_GetRef();
			Slot.RingIndex = RingIndex;
			Slot.AngleRadians = 2.0f * PI * static_cast<float>(SlotIndex) / static_cast<float>(SafeSlotCount);
			Slot.Radius = Radius;
		}
	};

	AddRing(0, InnerSlotCount, InnerRadius);
	AddRing(1, OuterSlotCount, OuterRadius);
	if (bHasSharedTargetSnapshot)
	{
		RebuildProjectedSlotCache();
	}
}

// 一次性投影所有站位点和攻击接近点；失败保留重试标记，各 AI 仅消费投影结果而非共享路径。
void AfpstrueSurroundManager::RebuildProjectedSlotCache()
{
	// 同时缓存“站位点”和更靠近玩家的“攻击接近点”，AI 决策阶段不再重复 ProjectPointToNavigation。
	const UWorld* World = GetWorld();
	const UNavigationSystemV1* NavigationSystem =
		World != nullptr ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	// 导航系统存在且投影成功时才输出可用位置，失败不伪造可达点。
	const auto ProjectLocation = [this, NavigationSystem](const FVector& RawLocation, FVector& OutLocation)
	{
		if (NavigationSystem == nullptr)
		{
			return false;
		}

		FNavLocation ProjectedLocation;
		if (!NavigationSystem->ProjectPointToNavigation(RawLocation, ProjectedLocation, NavigationProjectionExtent))
		{
			return false;
		}

		OutLocation = ProjectedLocation.Location;
		return true;
	};

	bNeedsNavigationRetry = false;
	// 每次只集中投影一次；失败标志独立于位置快照有效性。
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.bHasProjectedSlotLocation = ProjectLocation(CalculateRawSlotLocation(Slot), Slot.ProjectedSlotLocation);

		const float ApproachRadius = Slot.RingIndex == 0 ? AttackApproachRadius : OuterAttackApproachRadius;
		Slot.bHasProjectedApproachLocation =
			ProjectLocation(CalculateRawSlotLocation(Slot, ApproachRadius), Slot.ProjectedApproachLocation);
		bNeedsNavigationRetry |= !Slot.bHasProjectedSlotLocation || !Slot.bHasProjectedApproachLocation;
	}
	NextNavigationRetryTime = GetWorld()->GetTimeSeconds() + FMath::Max(0.05f, NavigationRetryInterval);
	NavigationCacheTime = GetWorld()->GetTimeSeconds();
}

// 返回有效共享玩家位置；调用者仍须通过自己的感知状态决定是否允许消费该位置。
bool AfpstrueSurroundManager::GetSharedTargetSnapshot(FVector& OutLocation) const
{
	// AIController 读取同一份玩家位置快照，避免一批敌人在相邻时刻使用不同目标点。
	if (!bHasSharedTargetSnapshot || !IsValid(TargetCharacter))
	{
		return false;
	}

	OutLocation = CachedTargetLocation;
	return true;
}

// ==================== 稳定槽位的申请与释放 ====================

// 槽位位置和攻击接近点都来自同一缓存，避免 AIController 在每次决策中重复导航投影。
bool AfpstrueSurroundManager::GetOrAssignAttackApproachLocation(AfpstrueEnemyCharacter* Enemy, FVector& OutLocation)
{
	// 已分配的敌人直接复用原槽位；只有首次申请才寻找空位，避免每轮决策改变目标导致来回晃动。
	if (!IsValid(Enemy) || !IsValid(TargetCharacter))
	{
		return false;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	const int32* AssignedSlot = EnemyToSlot.Find(EnemyKey);
	if (AssignedSlot != nullptr && (!SurroundSlots.IsValidIndex(*AssignedSlot) ||
		SurroundSlots[*AssignedSlot].Occupant != EnemyKey || !SurroundSlots[*AssignedSlot].bHasProjectedApproachLocation))
	{
		ReleaseSurroundSlot(Enemy);
		AssignedSlot = nullptr;
	}
	int32 SlotIndex = AssignedSlot != nullptr ? *AssignedSlot : INDEX_NONE;
	if (AssignedSlot == nullptr)
	{
		// 为尚未占槽的敌人选择并记录一个可用槽位。
		if (EnemyToSlot.Num() >= SurroundSlots.Num())
		{
			CleanupInvalidEntries();
			if (EnemyToSlot.Num() >= SurroundSlots.Num())
			{
				return false;
			}
		}

		SlotIndex = FindBestFreeSlot(Enemy->GetActorLocation());
		if (!SurroundSlots.IsValidIndex(SlotIndex))
		{
			return false;
		}
		SurroundSlots[SlotIndex].Occupant = Enemy;
		EnemyToSlot.Add(EnemyKey, SlotIndex);
	}

	// 槽位和共享追踪使用同一份目标快照，避免同一决策周期混用实时位置与缓存位置。
	// 复用路径已校验身份/投影，新分配路径只选择有有效接近点的槽位；此处不重复检查同一结果。
	OutLocation = SurroundSlots[SlotIndex].ProjectedApproachLocation;
	return true;
}

// 优先选内环可导航空位，再选外环；同环以最近二维距离择位，无可用位置返回 INDEX_NONE。
int32 AfpstrueSurroundManager::FindBestFreeSlot(const FVector& EnemyLocation)
{
	// 先扫描内环再扫描外环，并选择与敌人二维距离最近的可导航空位；槽位数固定，线性扫描成本可控。
	for (int32 RingIndex = 0; RingIndex <= 1; ++RingIndex)
	{
		int32 BestSlotIndex = INDEX_NONE;
		float BestDistanceSquared = TNumericLimits<float>::Max();

		for (int32 SlotIndex = 0; SlotIndex < SurroundSlots.Num(); ++SlotIndex)
		{
			const FfpstrueSurroundSlot& Slot = SurroundSlots[SlotIndex];
			if (Slot.RingIndex != RingIndex || Slot.Occupant.IsValid() || !Slot.bHasProjectedApproachLocation)
			{
				continue;
			}

			const float DistanceSquared = FVector::DistSquared2D(EnemyLocation, Slot.ProjectedApproachLocation);
			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestSlotIndex = SlotIndex;
			}
		}

		if (BestSlotIndex != INDEX_NONE)
		{
			return BestSlotIndex;
		}
	}

	return INDEX_NONE;
}

// 幂等移除敌人的占位和攻击许可；只有占用身份仍一致时才清槽并安排内环补位。
void AfpstrueSurroundManager::ReleaseSurroundSlot(AfpstrueEnemyCharacter* Enemy)
{
	// 释放槽位时同时归还攻击许可；若释放的是内环，再尝试从外环稳定补位。
	if (!IsValid(Enemy))
	{
		return;
	}
	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	ActiveAttackers.Remove(EnemyKey);
	int32 ReleasedSlotIndex = INDEX_NONE;
	if (!EnemyToSlot.RemoveAndCopyValue(EnemyKey, ReleasedSlotIndex))
	{
		return;
	}

	// 弱引用身份比较包含对象序号；旧映射不能清掉被重新占用的同一下标。
	// ★【重要边界】[资源归还] 释放槽位必须核对当前占用者身份，旧登记不能清空已被其他敌人占用的位置。
	if (!SurroundSlots.IsValidIndex(ReleasedSlotIndex) || SurroundSlots[ReleasedSlotIndex].Occupant != EnemyKey)
	{
		return;
	}
	const bool bReleasedInnerSlot = SurroundSlots[ReleasedSlotIndex].RingIndex == 0;
	SurroundSlots[ReleasedSlotIndex].Occupant.Reset();
	if (bReleasedInnerSlot)
	{
		PromoteOuterOccupantToInnerSlot(ReleasedSlotIndex);
	}
}

// 给可导航的内环空位选取最近外环占用者，更新稳定映射而不重新洗牌整圈敌人。
void AfpstrueSurroundManager::PromoteOuterOccupantToInnerSlot(int32 InnerSlotIndex)
{
	// 内环出现空位时选择离该位置最近的外环敌人迁入，避免随机洗牌造成整圈目标抖动。
	if (!SurroundSlots[InnerSlotIndex].bHasProjectedApproachLocation)
	{
		return;
	}

	const FfpstrueSurroundSlot& InnerSlot = SurroundSlots[InnerSlotIndex];
	const FVector InnerLocation =
		InnerSlot.bHasProjectedSlotLocation ? InnerSlot.ProjectedSlotLocation : CalculateRawSlotLocation(InnerSlot);
	int32 BestOuterSlotIndex = INDEX_NONE;
	float BestDistanceSquared = TNumericLimits<float>::Max();

	for (int32 SlotIndex = 0; SlotIndex < SurroundSlots.Num(); ++SlotIndex)
	{
		const FfpstrueSurroundSlot& Slot = SurroundSlots[SlotIndex];
		if (Slot.RingIndex == 0 || !Slot.Occupant.IsValid())
		{
			continue;
		}

		const float DistanceSquared = FVector::DistSquared2D(Slot.Occupant->GetActorLocation(), InnerLocation);
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			BestOuterSlotIndex = SlotIndex;
		}
	}

	if (!SurroundSlots.IsValidIndex(BestOuterSlotIndex))
	{
		return;
	}

	// 同一占用者从外环移到内环，槽位容器与敌人映射同时提交新位置。
	TWeakObjectPtr<AfpstrueEnemyCharacter> PromotedEnemy = SurroundSlots[BestOuterSlotIndex].Occupant;
	SurroundSlots[BestOuterSlotIndex].Occupant.Reset();
	SurroundSlots[InnerSlotIndex].Occupant = PromotedEnemy;
	EnemyToSlot.FindOrAdd(PromotedEnemy) = InnerSlotIndex;
}

// ==================== 攻击与 MoveTo 全局预算 ====================

// 申请有界并发攻击名额；已有持有者可重查，容量不足返回失败，不关闭敌人的追击行为。
bool AfpstrueSurroundManager::TryAcquireAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	// 当前是容量限制而非 Top-N 排名：名额未满时先申请先获得，攻击结束后由 Controller/CombatComponent 归还。
	if (!IsValid(Enemy) || Enemy->IsDead())
	{
		return false;
	}

	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (!bEnableActiveAttackerBudget || BenchmarkConfig.bDisableActiveAttackerBudget)
	{
		return true;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (ActiveAttackers.Contains(EnemyKey))
	{
		return true;
	}

	const int32 AttackLimit = FMath::Max(MaxActiveAttackers, 1);
	if (ActiveAttackers.Num() >= AttackLimit)
	{
		// 只有预算确实满时才扫描弱引用，避免每次攻击申请都遍历两张表。
		CleanupInvalidEntries();
		if (ActiveAttackers.Num() >= AttackLimit)
		{
			return false;
		}
	}

	// 预算只保护“攻击事务”，没有拿到名额的敌人仍保留槽位并继续追踪。
	// ★【重要边界】[状态提交] 攻击许可用唯一敌人集合登记，已有持有者重查不重复占用，容量不足者不能开始攻击。
	ActiveAttackers.Add(EnemyKey);
	return true;
}

// 移除唯一攻击者登记，结束、死亡和中断重复归还均安全。
void AfpstrueSurroundManager::ReleaseAttackPermission(AfpstrueEnemyCharacter* Enemy)
{
	// TSet::Remove 使重复归还天然幂等，不需要另外维护“已经释放”标志。
	if (Enemy != nullptr)
	{
		ActiveAttackers.Remove(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy));
	}
}

// 消费本帧的新 MoveTo 提交预算；普通追击让出战斗预留额度，拒绝不表示路径不可达。
bool AfpstrueSurroundManager::TryConsumeMoveRequestBudget(bool bCombatPriority)
{
	// GFrameCounter 形成无额外 Tick 的帧级计数器；战斗请求可使用预留额度，普通追击不能挤占全部预算。
	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (!bEnableMoveRequestBudget || BenchmarkConfig.bDisableMoveToRequestBudget)
	{
		return true;
	}

	if (MoveRequestBudgetFrame != GFrameCounter)
	{
		MoveRequestBudgetFrame = GFrameCounter;
		MoveRequestsConsumedThisFrame = 0;
	}

	const int32 SafeMaximum = FMath::Max(MaxMoveRequestsPerFrame, 1);
	const int32 SafeReservedCombat = FMath::Clamp(ReservedCombatMoveRequestsPerFrame, 0, SafeMaximum - 1);
	const int32 RequestLimit = bCombatPriority ? SafeMaximum : SafeMaximum - SafeReservedCombat;
	if (MoveRequestsConsumedThisFrame >= RequestLimit)
	{
		return false;
	}

	// 近战槽位请求可以使用预留额度，远距离追踪不会挤占整帧的导航提交。
	++MoveRequestsConsumedThisFrame;
	return true;
}

// ==================== 清理、槽位选择与调试 ====================
// 槽位选择与补位算法紧邻申请/释放入口；此处集中清理与调试。

// 对局结束或更换目标时统一清空跨敌人资源并停掉共享刷新，允许之后重新注入目标。
void AfpstrueSurroundManager::ResetManager()
{
	// 对局结束时一次性归零槽位、攻击名额、帧预算和目标缓存，Manager 可安全退出或重新初始化。
	EnemyToSlot.Reset();
	ActiveAttackers.Reset();
	MoveRequestBudgetFrame = MAX_uint64;
	MoveRequestsConsumedThisFrame = 0;
	TargetCharacter = nullptr;
	bHasSharedTargetSnapshot = false;
	bNeedsNavigationRetry = false;
	SharedTargetSnapshotTime = 0.0;
	NextNavigationRetryTime = 0.0;
	NavigationCacheTime = 0.0;
	for (FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		Slot.Occupant.Reset();
		Slot.bHasProjectedSlotLocation = false;
		Slot.bHasProjectedApproachLocation = false;
	}
	GetWorldTimerManager().ClearTimer(SharedTargetTimerHandle);
}

// 删除死亡或失效弱引用，并核对槽位身份后清占用，避免旧登记误清新占用者。
void AfpstrueSurroundManager::CleanupInvalidEntries()
{
	// 弱引用不会阻止 Actor 销毁；清掉已失效或死亡敌人的登记，并同步清空对应槽位占用。
	for (auto Iterator = EnemyToSlot.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Key().IsValid() && !Iterator.Key()->IsDead())
		{
			continue;
		}

		const int32 SlotIndex = Iterator.Value();
		if (SurroundSlots.IsValidIndex(SlotIndex) && SurroundSlots[SlotIndex].Occupant == Iterator.Key())
		{
			SurroundSlots[SlotIndex].Occupant.Reset();
		}
		Iterator.RemoveCurrent();
	}

	// 攻击许可不依赖是否有槽位，需独立清除失效持有者。
	for (auto Iterator = ActiveAttackers.CreateIterator(); Iterator; ++Iterator)
	{
		const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey = *Iterator;
		if (!EnemyKey.IsValid() || EnemyKey->IsDead())
		{
			Iterator.RemoveCurrent();
		}
	}
}

// 把相对极角和半径换算为世界位置；可选半径用于同角度的攻击接近点。
FVector AfpstrueSurroundManager::CalculateRawSlotLocation(const FfpstrueSurroundSlot& Slot, float RadiusOverride) const
{
	// 使用缓存玩家位置、槽位极角和半径计算二维世界坐标；随后由批量 NavMesh 投影修正可达性。
	if (!bHasSharedTargetSnapshot)
	{
		return GetActorLocation();
	}

	const float Radius = RadiusOverride >= 0.0f ? RadiusOverride : Slot.Radius;
	const FVector Direction(FMath::Cos(Slot.AngleRadians), FMath::Sin(Slot.AngleRadians), 0.0f);
	return CachedTargetLocation + Direction * Radius;
}

// 调试时绘制有效投影槽位，以青色和绿色区分占用与空闲；不参与正式决策。
void AfpstrueSurroundManager::DrawDebugSlots()
{
	// 仅开发调试使用：绿色为空位、青色为占用位，不参与正式站位决策。
	if (!bDrawDebugSlots || !IsValid(TargetCharacter))
	{
		return;
	}

	CleanupInvalidEntries();

	for (const FfpstrueSurroundSlot& Slot : SurroundSlots)
	{
		if (!Slot.bHasProjectedSlotLocation)
		{
			continue;
		}

		const FColor SlotColor = Slot.Occupant.IsValid() ? FColor::Cyan : FColor::Green;

		DrawDebugSphere(GetWorld(), Slot.ProjectedSlotLocation, 24.0f, 12, SlotColor, false, DebugDrawInterval * 1.25f, 0, 2.0f);
	}
}
