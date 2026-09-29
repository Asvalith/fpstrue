// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueCollisionChannels.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"

namespace
{
constexpr float RecoilRecoveryTickInterval = 1.0f / 60.0f;
constexpr float GaussianSpreadSigmaCount = 3.0f;
constexpr float MaxTotalSpreadAngle = 45.0f;

float SanitizeSpreadAngle(float Degrees)
{
	return FMath::IsFinite(Degrees) ? FMath::Clamp(Degrees, 0.0f, MaxTotalSpreadAngle) : 0.0f;
}

/*
 * 玩家武器的核心状态与射击实现。
 * 组件拥有弹药、开火/换弹互斥状态和后坐力恢复；角色只转发输入，动画通过 Notify 提交换弹，
 * 蓝图负责枪口、音效和换弹素材选择；C++ 拥有命中/弹药结算以及明确的播放身份和结束边界。
 *
 * 主要调用链：
 *   输入 -> Character::StartWeaponFire -> StartFire -> Fire -> Hitscan -> ApplyPointDamage
 *   输入 -> Character::RequestWeaponReload -> RequestReload -> PlaybackRequested(ReloadId) -> PlayReloadMontage
 *   已绑定实例的 ReloadCommit Notify -> 提交弹药；全部播放正常结束 -> Completed；主动中断 -> Cancelled。
 *   播放链缺失 -> TimedOut，仅解除动作锁，不伪装成正常装填；播放失败 -> PlaybackFailed。
 *
 * ActionState、CurrentAmmo、ReserveAmmo 和后坐力累计量都只由本组件写入；Character/HUD 通过只读接口和
 * Delegate 观察结果，从结构上避免蓝图、角色和武器各保存一份可变状态。
 */

// 根据高斯分布生成散布方向，供每发 Hitscan 共用。
FVector MakeGaussianSpreadDirection(const FVector& Forward, float SpreadAngleDegrees)
{
	const FVector AimDirection = Forward.GetSafeNormal();
	SpreadAngleDegrees = SanitizeSpreadAngle(SpreadAngleDegrees);
	if (AimDirection.IsNearlyZero() || SpreadAngleDegrees <= KINDA_SMALL_NUMBER)
	{
		return AimDirection;
	}

	FVector Right;
	FVector Up;
	AimDirection.FindBestAxisVectors(Right, Up);

	const float MaxRadius = FMath::Tan(FMath::DegreesToRadians(SpreadAngleDegrees));
	const float Sigma = MaxRadius / GaussianSpreadSigmaCount;
	const float TruncatedProbability = 1.0f - FMath::Exp(-0.5f * FMath::Square(GaussianSpreadSigmaCount));
	const float Radius = Sigma * FMath::Sqrt(-2.0f * FMath::Loge(1.0f - FMath::FRand() * TruncatedProbability));
	const float Angle = FMath::FRand() * 2.0f * PI;
	const FVector Offset = Right * (FMath::Cos(Angle) * Radius) + Up * (FMath::Sin(Angle) * Radius);

	return (AimDirection + Offset).GetSafeNormal();
}
} // namespace

// 构造默认的关键骨骼集合；可随具体武器蓝图和目标骨架覆盖，不把素材名称写死在射击流程中。
UfpstrueWeaponComponent::UfpstrueWeaponComponent()
{
	// 这里只提供当前 Mannequin 的默认骨骼约定；武器蓝图可针对其他目标骨架覆盖名单。
	// FName 适合反复比较的标识符；初始化时构造，避免每次命中再创建 FString 并执行 ToLower。
	CriticalHitBones = {FName(TEXT("neck_01")), FName(TEXT("head"))};
}

// ==================== Equipment ====================

bool UfpstrueWeaponComponent::AttachWeapon(AfpstrueCharacter* TargetCharacter)
{
	// 任何前置条件失败都不修改双方状态，挂接成功后才提交关系；重新装备不补满弹药。
	if (bEndingPlay || bDetaching || !IsValid(TargetCharacter) || !TargetCharacter->CanMaintainEquipment())
	{
		return false;
	}

	if (AfpstrueCharacter* ExistingCharacter = Character.Get())
	{
		return ExistingCharacter == TargetCharacter && IsOperational();
	}

	if (TargetCharacter->HasEquippedWeapon())
	{
		return false;
	}

	USkeletalMeshComponent* TargetMesh = TargetCharacter->GetMesh1P();
	if (TargetMesh == nullptr || GripSocketName.IsNone() || !TargetMesh->DoesSocketExist(GripSocketName))
	{
		UE_LOG(LogTemp, Error, TEXT("AttachWeapon failed: character %s does not provide socket/bone %s."), *GetNameSafe(TargetCharacter),
			   *GripSocketName.ToString());
		return false;
	}

	FAttachmentTransformRules AttachmentRules(EAttachmentRule::SnapToTarget, true);
	if (!AttachToComponent(TargetMesh, AttachmentRules, GripSocketName))
	{
		return false;
	}

	// 所有外部操作成功后才提交角色引用和运行时状态，失败路径不会留下半装备状态。
	Character = TargetCharacter;
	MagazineSize = FMath::Max(1, MagazineSize);
	if (!bAmmoInitialized)
	{
		CurrentAmmo = MagazineSize;
		ReserveAmmo = FMath::Max(0, StartingReserveAmmo);
		bAmmoInitialized = true;
	}
	SetActionState(EFPWeaponActionState::Ready);
	TargetCharacter->SetEquippedWeaponComponent(this);
	// 装备广播可同步卸下/销毁角色；只有双方关系仍成立才发布弹药或让拾取物消费成功。
	if (!IsOperational() || Character.Get() != TargetCharacter)
	{
		return false;
	}
	BroadcastAmmoChanged();

	return IsOperational() && Character.Get() == TargetCharacter;
}

void UfpstrueWeaponComponent::DetachWeapon()
{
	if (bDetaching) return;
	TGuardValue<bool> DetachingGuard(bDetaching, true);
	AfpstrueCharacter* PreviousCharacter = Character.Get();
	DisableWeapon();
	Character.Reset();
	DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	if (PreviousCharacter != nullptr)
	{
		PreviousCharacter->ReleaseEquippedWeaponComponent(this);
	}
}

// ==================== Action State / Rules ====================

void UfpstrueWeaponComponent::SetActionState(EFPWeaponActionState NewState)
{
	// 禁用清理独立于上一个动作；重复 Disable 也清理残留，不在各生命周期出口复制规则。
	if (NewState == EFPWeaponActionState::Disabled)
	{
		ClearRecoilState();
		ConsecutiveShotCount = 0;
	}
	if (ActionState == NewState)
	{
		return;
	}

	// 出口清理只写一次：停止射击、结束/取消换弹、死亡和卸下都经过这里。
	if (UWorld* World = GetWorld())
	{
		if (ActionState == EFPWeaponActionState::Firing)
		{
			World->GetTimerManager().ClearTimer(AutomaticFireTimerHandle);
		}
		else if (ActionState == EFPWeaponActionState::Reloading)
		{
			World->GetTimerManager().ClearTimer(ReloadTimerHandle);
		}
	}
	bReloadAmmoCommitted = false;
	ReloadPlaybacks.Reset();
	ActionState = NewState;
	++ActionRevision;
}

bool UfpstrueWeaponComponent::IsOperational() const
{
	// 汇总装备关系与角色生命周期；已经卸下的组件不能再接受射击或装填。
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	return !bEndingPlay && IsValid(OwningCharacter) && OwningCharacter->CanMaintainEquipment()
		&& ActionState != EFPWeaponActionState::Disabled
		&& OwningCharacter->GetEquippedWeaponComponent() == this;
}

bool UfpstrueWeaponComponent::IsCurrentAction(EFPWeaponActionState ExpectedState, uint32 ExpectedRevision) const
{
	return ActionState == ExpectedState && ActionRevision == ExpectedRevision && IsOperational();
}

bool UfpstrueWeaponComponent::CanAcceptOwnerInput() const
{
	return IsOperational() && Character->CanAcceptGameplayInput() && !bInterruptingOwnerInput && !bChangingReloadPlayback;
}

// ==================== Fire System ====================

void UfpstrueWeaponComponent::StartFire()
{
	// StartFire 只提交 Ready -> Firing；首次和后续每一发的有效性统一由 Fire 校验。
	if (ActionState != EFPWeaponActionState::Ready || !CanAcceptOwnerInput())
	{
		return;
	}

	SetActionState(EFPWeaponActionState::Firing);
	Fire();
}

void UfpstrueWeaponComponent::ScheduleNextShot()
{
	if (UWorld* World = GetWorld(); World != nullptr && IsOperational() && IsFiring() && HasAmmo())
	{
		const double Interval = 60.0 / FMath::Max(static_cast<double>(RoundsPerMinute), 1.0);
		const float Remaining = static_cast<float>(FMath::Max(LastShotTimeSeconds + Interval - World->GetTimeSeconds(), 0.001));
		const uint32 ScheduledRevision = ActionRevision;
		World->GetTimerManager().SetTimer(AutomaticFireTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, ScheduledRevision]()
			{
				if (IsCurrentAction(EFPWeaponActionState::Firing, ScheduledRevision)) Fire();
			}), Remaining, false);
	}
}

void UfpstrueWeaponComponent::StopFire()
{
	// 只退出 Firing；松开扳机不能解除 Reloading/Disabled。
	if (ActionState == EFPWeaponActionState::Firing)
	{
		SetActionState(EFPWeaponActionState::Ready);
	}
}

void UfpstrueWeaponComponent::Fire()
{
	// 每发射击的提交顺序固定为“校验 -> 扣弹 -> 表现/伤害”，保证一次请求最多结算一次弹药。
	if (ActionState != EFPWeaponActionState::Firing)
	{
		return;
	}

	UWorld* const World = GetWorld();
	if (World == nullptr || !IsOperational())
	{
		StopFire();
		return;
	}

	// 空弹匣在解析射击依赖前直接转入统一换弹入口；无备弹时停止连射。
	if (!HasAmmo())
	{
		HandleEmptyMagazine();
		return;
	}

	// 射击入口先校验持有者，再读取相机与控制器。
	// 弱指针解析为局部裸指针后，只在当前 Game Thread 调用栈内临时使用，不保存到下一帧。
	AfpstrueCharacter* OwningCharacter = Character.Get();
	UCameraComponent* Camera = OwningCharacter->GetFirstPersonCameraComponent();
	if (OwningCharacter->GetController() == nullptr || Camera == nullptr)
	{
		StopFire();
		return;
	}

	const double CurrentTimeSeconds = World->GetTimeSeconds();
	const double FireInterval = 60.0 / FMath::Max(static_cast<double>(RoundsPerMinute), 1.0);
	if (LastShotTimeSeconds >= 0.0 && CurrentTimeSeconds - LastShotTimeSeconds + KINDA_SMALL_NUMBER < FireInterval)
	{
		ScheduleNextShot();
		return;
	}
	// 同一个“已接受射击时间”服务射速和散布；先读取上次时间，再写入这次提交。
	if (LastShotTimeSeconds < 0.0 || CurrentTimeSeconds - LastShotTimeSeconds > SpreadResetDelay)
	{
		ConsecutiveShotCount = 0;
	}
	LastShotTimeSeconds = CurrentTimeSeconds;

	const uint32 FiringRevision = ActionRevision;
	// 先完成扣弹与命中结算，再通知 HUD/开火表现；监听者重入不能取消一发已扣弹的命中。
	--CurrentAmmo;
	FireLineTrace(*World, *OwningCharacter, *Camera);
	if (IsCurrentAction(EFPWeaponActionState::Firing, FiringRevision))
	{
		ApplyRecoil(Cast<APlayerController>(OwningCharacter->GetController()));
	}
	BroadcastAmmoChanged();

	// 这是已发生的一发，而不是继续开火的许可；命中/弹药回调中断动作也不能吞掉事实事件。
	OnWeaponFirePerformed.Broadcast();

	//最后一发完成命中和表现后再退出射击状态，避免Firing残留
	if (IsCurrentAction(EFPWeaponActionState::Firing, FiringRevision) && !HasAmmo())
	{
		HandleEmptyMagazine();
	}
	else if (IsCurrentAction(EFPWeaponActionState::Firing, FiringRevision))
	{
		ScheduleNextShot();
	}
}

void UfpstrueWeaponComponent::HandleEmptyMagazine()
{
	if (!RequestReload())
	{
		StopFire();
	}
}

void UfpstrueWeaponComponent::FireLineTrace(UWorld& World, AfpstrueCharacter& OwningCharacter, UCameraComponent& Camera)
{
	// 根据瞄准状态和连续射击次数计算散布，并在同一入口完成单射线查询。
	// Fire 已校验本发的角色/相机；在外部伤害或表现回调之前直接使用这些依赖，不重复解析装备关系。
	const float ContinuousSpreadAngle = FMath::Min(ConsecutiveShotCount * SanitizeSpreadAngle(ContinuousFireSpreadStep),
		SanitizeSpreadAngle(MaxContinuousFireSpreadAngle));
	// 基础与连射配置各自合法不代表总和合法；总角度统一有限且不超过 45 度，避免 tan 接近 90 度。
	const float SpreadAngle = SanitizeSpreadAngle(SanitizeSpreadAngle(OwningCharacter.IsAiming() ? AimFireSpreadAngle : HipFireSpreadAngle)
		+ ContinuousSpreadAngle);
	++ConsecutiveShotCount;

	// 从相机发出射击专用射线，返回第一个阻挡命中，并据骨骼名称结算点伤害。
	const FVector Start = Camera.GetComponentLocation();
	const FVector Forward = Camera.GetForwardVector();
	const FVector ShotDirection = SpreadAngle > 0.0f ? MakeGaussianSpreadDirection(Forward, SpreadAngle) : Forward;
	const FVector End = Start + ShotDirection * LineTraceRange;

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(&OwningCharacter);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bTraceComplex = true;

	// WeaponTrace 与相机可见性解耦：透明表现、交互射线和玩家子弹可以分别配置响应。
	const bool bHit = World.LineTraceSingleByChannel(HitResult, Start, End, FpstrueCollisionChannels::WeaponTrace, QueryParams);

	// 只有查询真正命中后才读取目标、骨骼和物理组件；未命中只广播弹道终点供表现层使用。
	AActor* HitActor = HitResult.GetActor();
	if (bHit && IsValid(HitActor))
	{
		const bool bCriticalHit = CriticalHitBones.Contains(HitResult.BoneName);
		const float DamageToApply = bCriticalHit ? LineTraceHeadDamage : LineTraceDamage;

		UGameplayStatics::ApplyPointDamage(HitActor, DamageToApply, ShotDirection, HitResult, OwningCharacter.GetController(), GetOwner(),
									   nullptr);

		// 受伤事件可能销毁目标；仅对仍有效的非角色物理组件追加冲量。
		UPrimitiveComponent* HitComponent = HitResult.GetComponent();
		if (IsValid(HitActor) && !HitActor->IsA<ACharacter>() && IsValid(HitComponent) && HitComponent->IsSimulatingPhysics())
		{
			HitComponent->AddImpulseAtLocation(ShotDirection * LineTraceImpulse, HitResult.ImpactPoint);
		}
	}
	const FVector TraceTarget = bHit ? HitResult.ImpactPoint : End;
	OnWeaponTraceFinished.Broadcast(bHit, Start, End, TraceTarget, HitResult);
}

// ==================== Reload System ====================

/*
 * Request 进入 Reloading 并发出带 ID 的播放请求；明确绑定的 Notify 在装填帧转移弹药，仍保持 Reloading。
 * 原生实例结束委托负责收口：全部正常完成才 Completed，一项被打断即 Cancelled。
 * 显式正常完成可补交缺失的 Commit；取消、失败和超时不补交也不回滚已提交的弹药。
 * 播放链未结束时，唯一兜底 Timer 以 TimedOut 解除锁；无动画玩法可保存 ID 调用 ForTransaction 接口。
 *
 * Delegate 在当前调用栈同步执行，监听者可能取消换弹、死亡或开始下一次换弹。
 * 因此开始事件之前必须先设置状态和 Timer；Finish 在提交事件返回后还要确认动作版本未变。
 */

bool UfpstrueWeaponComponent::CanReload() const
{
	// 换弹必须同时满足：武器可用、当前不在换弹、弹匣未满且仍有备弹。
	return CanAcceptOwnerInput() && (ActionState == EFPWeaponActionState::Ready || ActionState == EFPWeaponActionState::Firing)
		&& GetWorld() != nullptr && CurrentAmmo < MagazineSize && ReserveAmmo > 0;
}

bool UfpstrueWeaponComponent::RequestReload()
{
	// Request 只开启换弹事务，不立刻搬运弹药；真正提交点由动画 Notify 决定，使数值变化与装填动作对齐。
	if (!CanReload())
	{
		return false;
	}

	const bool bWasEmptyReload = CurrentAmmo <= 0;
	SetActionState(EFPWeaponActionState::Reloading);
	ActiveReloadId = ActiveReloadId == MAX_int32 ? 1 : ActiveReloadId + 1;
	const int32 RequestedReloadId = ActiveReloadId;
	// 先建立本次兜底，再广播；监听者同步 Finish/Cancel 时才能一并清除正确的 Timer。
	const float SelectedReloadDuration = bWasEmptyReload ? EmptyReloadDuration : ReloadDuration;
	// 超时 Timer 是动作锁的容错，不替代装填 Notify；动画链断开时不把失败当成补弹成功。
	// 使用唯一超时句柄，显式播放可按真实长度延长截止点；排队的旧回调只能结束它保存的那次换弹。
	const float Timeout = FMath::Max(0.01f, FMath::Max(SelectedReloadDuration, ReloadFailSafeDuration) + ReloadCompletionGracePeriod);
	GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
		FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::EndReload, RequestedReloadId, EFPReloadEndReason::TimedOut), Timeout, false);
	// 手动请求和空仓自动换弹共用角色互斥处理；先上 Reloading 锁，再退出瞄准，防止蓝图回调重新开火。
	Character->ResetMovementModifiers();
	if (IsCurrentReload(RequestedReloadId) && IsOperational())
	{
		// 蓝图明确提交 ReloadId/Mesh/Montage，禁止通过广播前后列表差分猜测动画归属。
		OnWeaponReloadPlaybackRequested.Broadcast(RequestedReloadId, bWasEmptyReload);
		if (IsCurrentReload(RequestedReloadId)) OnWeaponReloadStarted.Broadcast(bWasEmptyReload);
	}

	// true 表示请求曾被接纳；监听者可能已同步结束换弹，广播之后不再写回状态或重设计时器。
	return true;
}

bool UfpstrueWeaponComponent::CommitReload()
{
	UE_LOG(LogTemp, Warning, TEXT("Legacy identityless CommitReload rejected on %s. Use a saved ReloadId or native ReloadCommit notify."), *GetPathName());
	return false;
}

bool UfpstrueWeaponComponent::IsCurrentReload(int32 ReloadId) const
{
	return ReloadId != INDEX_NONE && IsReloading() && ActiveReloadId == ReloadId;
}

bool UfpstrueWeaponComponent::PlayReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage, float PlayRate)
{
	if (!IsCurrentReload(ReloadId) || !IsOperational() || bChangingReloadPlayback) return false;
	// 同一 Mesh 一次换弹只拥有一段播放，不能让无关 Montage 覆盖真实装填身份。
	if (ReloadPlaybacks.ContainsByPredicate([PlaybackMesh](const FFPActionPlayback& P) { return P.Mesh.Get() == PlaybackMesh; })) return false;
	TGuardValue<bool> PlaybackGuard(bChangingReloadPlayback, true);
	UAnimInstance* Anim = IsValid(PlaybackMesh) ? PlaybackMesh->GetAnimInstance() : nullptr;
	if (Anim == nullptr || Montage == nullptr || !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f
		|| (PlaybackMesh != this && PlaybackMesh != Character->GetMesh1P()))
	{
		EndReload(ReloadId, EFPReloadEndReason::PlaybackFailed);
		return false;
	}
	// 保留原蓝图的同组互斥：UE 的 true 停同一 Montage Group，不停其他组的表现。
	const float Duration = Anim->Montage_Play(Montage, PlayRate, EMontagePlayReturnType::Duration, 0.0f, true);
	if (Duration <= 0.0f || !BindReloadMontage(ReloadId, PlaybackMesh, Montage))
	{
		// 成功路径只绑定一次；仅在播放成功但事务已失效时回收本次实例，不停止播放失败前的旧实例。
		FFPActionPlayback UnboundPlayback;
		if (Duration > 0.0f && UnboundPlayback.TryBind(ReloadId, PlaybackMesh, Montage)) UnboundPlayback.Stop();
		EndReload(ReloadId, EFPReloadEndReason::PlaybackFailed);
		return false;
	}
	// 延迟开始/较长素材也有真实播放时间；兜底不能在已接纳播放正常结束前抢先超时。
	const float Remaining = GetWorld()->GetTimerManager().GetTimerRemaining(ReloadTimerHandle);
	const float Timeout = FMath::Max(Remaining, Duration + FMath::Max(0.0f, ReloadCompletionGracePeriod));
	GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
		FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::EndReload, ReloadId, EFPReloadEndReason::TimedOut), FMath::Max(0.01f, Timeout), false);
	return true;
}

bool UfpstrueWeaponComponent::BindReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage)
{
	if (!IsCurrentReload(ReloadId) || !IsOperational() || !IsValid(PlaybackMesh)
		|| (PlaybackMesh != this && PlaybackMesh != Character->GetMesh1P())) return false;
	FFPActionPlayback* Existing = ReloadPlaybacks.FindByPredicate([PlaybackMesh](const FFPActionPlayback& P) { return P.Mesh.Get() == PlaybackMesh; });
	if (Existing != nullptr) return Existing->TryBind(ReloadId, PlaybackMesh, Montage);
	FFPActionPlayback Playback;
	if (!Playback.TryBind(ReloadId, PlaybackMesh, Montage)) return false;
	ReloadPlaybacks.Add(Playback);
	FAnimMontageInstance* Instance = Playback.AnimInstance->GetMontageInstanceForID(Playback.MontageInstanceId);
	// UObject 委托弱引用组件，并把本次身份作为固定 payload 保存；回调不查询“当前动画”。
	Instance->OnMontageEnded.BindUObject(this, &UfpstrueWeaponComponent::HandleReloadPlaybackEnded,
		ReloadId, Playback.Mesh, Playback.MontageInstanceId);
	return true;
}

void UfpstrueWeaponComponent::HandleReloadPlaybackEnded(UAnimMontage*, bool bInterrupted,
	int32 ReloadId, TWeakObjectPtr<USkeletalMeshComponent> PlaybackMesh, int32 InstanceId)
{
	if (!IsCurrentReload(ReloadId)) return;
	const int32 Removed = ReloadPlaybacks.RemoveAll([PlaybackMesh, InstanceId](const FFPActionPlayback& P)
	{
		return P.Mesh == PlaybackMesh && P.MontageInstanceId == InstanceId;
	});
	if (Removed == 0) return;
	if (bInterrupted) EndReload(ReloadId, EFPReloadEndReason::Cancelled);
	else if (ReloadPlaybacks.IsEmpty()) EndReload(ReloadId, EFPReloadEndReason::Completed);
}

bool UfpstrueWeaponComponent::CommitReloadFromNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference)
{
	const bool bMatchesPlayback = ReloadPlaybacks.ContainsByPredicate([this, PlaybackMesh, &EventReference](const FFPActionPlayback& Playback)
	{
		return Playback.Matches(ActiveReloadId, PlaybackMesh, EventReference);
	});
	return bMatchesPlayback && CommitReloadForTransaction(ActiveReloadId);
}

bool UfpstrueWeaponComponent::CommitReloadForTransaction(int32 ReloadId)
{
	// bReloadAmmoCommitted 是单次提交保护：同一轮换弹的重复 Notify 只会第一次搬运弹药。
	if (!IsCurrentReload(ReloadId) || bReloadAmmoCommitted || !IsOperational())
	{
		return false;
	}

	const int32 AmmoNeeded = MagazineSize - CurrentAmmo;
	const int32 AmmoToLoad = FMath::Min(AmmoNeeded, ReserveAmmo);
	CurrentAmmo += AmmoToLoad;
	ReserveAmmo -= AmmoToLoad;
	// 先标记已提交再通知 HUD，防止监听者重入 CommitReload 导致重复装弹。
	bReloadAmmoCommitted = true;
	BroadcastAmmoChanged();
	return true;
}

void UfpstrueWeaponComponent::FinishReload()
{
	UE_LOG(LogTemp, Warning, TEXT("Legacy identityless FinishReload rejected on %s. Use PlayReloadMontage or a saved ReloadId."), *GetPathName());
}

void UfpstrueWeaponComponent::CancelReload()
{
	CancelReloadForTransaction(GetActiveReloadId());
}

void UfpstrueWeaponComponent::FinishReloadForTransaction(int32 ReloadId)
{
	EndReload(ReloadId, EFPReloadEndReason::Completed);
}

void UfpstrueWeaponComponent::CancelReloadForTransaction(int32 ReloadId)
{
	EndReload(ReloadId, EFPReloadEndReason::Cancelled);
}

void UfpstrueWeaponComponent::EndReload(int32 ReloadId, EFPReloadEndReason Reason)
{
	// 取消只收尾：装填帧之前不补弹，装填帧之后保留已转移的弹药。
	if (!IsCurrentReload(ReloadId)) return;
	if (Reason == EFPReloadEndReason::Completed)
	{
		CommitReloadForTransaction(ReloadId);
		// Commit 的同步广播可以取消、禁用或启动下一轮，旧结束不能覆盖新状态。
		if (!IsCurrentReload(ReloadId)) return;
	}
	const bool bCommitted = bReloadAmmoCommitted;
	const TArray<FFPActionPlayback, TInlineAllocator<2>> EndedPlaybacks = ReloadPlaybacks;
	SetActionState(Reason != EFPReloadEndReason::Disabled && IsOperational()
		? EFPWeaponActionState::Ready : EFPWeaponActionState::Disabled);
	{
		TGuardValue<bool> PlaybackGuard(bChangingReloadPlayback, true);
		for (const FFPActionPlayback& Playback : EndedPlaybacks) Playback.Stop();
	}
	// 超时只释放动作锁；未发生装填 Notify 的动画失败不能被兜底伪装成补弹成功。
	// 发布前已完成所有旧事务清理；监听者开启新事务后，旧栈不能再写状态。
	OnWeaponReloadEnded.Broadcast(ReloadId, Reason, bCommitted);
}

// ==================== Recoil System ====================

void UfpstrueWeaponComponent::ApplyRecoil(APlayerController* PlayerController)
{
	// 本发后坐力先累加到受限范围，再启动固定频率的恢复 Timer；瞄准时使用较小倍率。
	if (PlayerController == nullptr)
	{
		return;
	}

	const AfpstrueCharacter* OwningCharacter = Character.Get();
	const float RecoilMultiplier = OwningCharacter != nullptr && OwningCharacter->IsAiming() ? AimRecoilMultiplier : 1.0f;
	const float PitchKick = -RecoilPitch * RecoilMultiplier;
	const float YawKick = FMath::FRandRange(-RecoilYaw, RecoilYaw) * RecoilMultiplier;
	const float NewPitch = FMath::Clamp(AccumulatedRecoilPitch + PitchKick, -MaxAccumulatedRecoilPitch, 0.0f);
	const float NewYaw = FMath::Clamp(AccumulatedRecoilYaw + YawKick, -MaxAccumulatedRecoilYaw, MaxAccumulatedRecoilYaw);

	PlayerController->AddPitchInput(NewPitch - AccumulatedRecoilPitch);
	PlayerController->AddYawInput(NewYaw - AccumulatedRecoilYaw);
	AccumulatedRecoilPitch = NewPitch;
	AccumulatedRecoilYaw = NewYaw;

	if (UWorld* World = GetWorld())
	{
		// 延迟是“不恢复”的时间，不计入恢复步长；卡顿后的步长使用真实经过的游戏时间。
		LastRecoilRecoveryTimeSeconds = World->GetTimeSeconds() + RecoilRecoveryDelay;
		World->GetTimerManager().SetTimer(RecoilRecoveryTimerHandle, this, &UfpstrueWeaponComponent::UpdateRecoilRecovery,
										  RecoilRecoveryTickInterval, true, RecoilRecoveryDelay);
	}
}

void UfpstrueWeaponComponent::UpdateRecoilRecovery()
{
	// 每次 Timer 回调只恢复一小步，并把相邻两次累计值的差量写入 Controller 视角。
	UWorld* World = GetWorld();
	AfpstrueCharacter* OwningCharacter = Character.Get();
	APlayerController* PlayerController =
		OwningCharacter != nullptr ? Cast<APlayerController>(OwningCharacter->GetController()) : nullptr;
	if (World == nullptr || PlayerController == nullptr || !IsOperational())
	{
		ClearRecoilState();
		return;
	}

	const double Now = World->GetTimeSeconds();
	const float DeltaSeconds = static_cast<float>(FMath::Max(0.0, Now - LastRecoilRecoveryTimeSeconds));
	LastRecoilRecoveryTimeSeconds = Now;
	const float NewPitch = FMath::FInterpConstantTo(AccumulatedRecoilPitch, 0.0f, DeltaSeconds, RecoilRecoverySpeed);
	const float NewYaw = FMath::FInterpConstantTo(AccumulatedRecoilYaw, 0.0f, DeltaSeconds, RecoilRecoverySpeed);

	PlayerController->AddPitchInput(NewPitch - AccumulatedRecoilPitch);
	PlayerController->AddYawInput(NewYaw - AccumulatedRecoilYaw);
	AccumulatedRecoilPitch = NewPitch;
	AccumulatedRecoilYaw = NewYaw;

	if (FMath::IsNearlyZero(AccumulatedRecoilPitch, KINDA_SMALL_NUMBER) && FMath::IsNearlyZero(AccumulatedRecoilYaw, KINDA_SMALL_NUMBER))
	{
		ClearRecoilState();
	}
}

void UfpstrueWeaponComponent::ClearRecoilState()
{
	// 停止恢复 Timer 并归零累计量，供恢复完成、角色死亡和组件退出共同调用。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RecoilRecoveryTimerHandle);
	}

	AccumulatedRecoilPitch = 0.0f;
	AccumulatedRecoilYaw = 0.0f;
	LastRecoilRecoveryTimeSeconds = 0.0;
}

// ==================== 生命周期结束与公共清理 ====================

void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 组件退出前清理所有 Timer，并通知角色解除装备关系，避免留下跨生命周期的回调和悬空关系。
	bEndingPlay = true;
	DetachWeapon();
	Super::EndPlay(EndPlayReason);
}

void UfpstrueWeaponComponent::DisableWeapon()
{
	// 死亡、卸下和 EndPlay 共用强制中断；先禁用，再清理，后续回调不能恢复 Ready。
	if (IsReloading())
	{
		EndReload(GetActiveReloadId(), EFPReloadEndReason::Disabled);
		return;
	}
	SetActionState(EFPWeaponActionState::Disabled);
}

void UfpstrueWeaponComponent::InterruptOwnerInput()
{
	AfpstrueCharacter* OwningCharacter = Character.Get();
	if (OwningCharacter == nullptr || !OwningCharacter->CanMaintainEquipment())
	{
		DisableWeapon();
		return;
	}
	// Character 自己管理 ADS/冲刺输入资格；武器只拥有本模块清理期间的重入边界。
	// 不能直接让 IsOperational 返回 false，否则 CancelReload 会把临时中断误变成永久 Disabled。
	TGuardValue<bool> InputTransitionGuard(bInterruptingOwnerInput, true);
	StopFire();
	ClearRecoilState();
	CancelReload();
}

void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	// 弹药状态只由 WeaponComponent 写入；角色、HUD 和蓝图通过该委托读取同一份结果。
	OnAmmoChanged.Broadcast(CurrentAmmo, MagazineSize, ReserveAmmo);
}
