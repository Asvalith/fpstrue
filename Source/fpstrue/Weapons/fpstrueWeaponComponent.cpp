// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "fpstrue.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Animation/AnimInstance.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"

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

namespace
{
constexpr float RecoilRecoveryTickInterval = 1.0f / 60.0f;
constexpr float GaussianSpreadSigmaCount = 3.0f;
constexpr float MaxTotalSpreadAngle = 45.0f;

// 根据高斯分布生成散布方向；唯一调用点已校验总散布角，这里只负责方向计算。
FVector MakeGaussianSpreadDirection(const FVector& Forward, float SpreadAngleDegrees)
{
	const FVector AimDirection = Forward.GetSafeNormal();
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

// ==================== Equipment ====================

// 装备前返回所选配置以供预览，首次装备后返回冻结快照，避免共享资产改动影响当前动作。
const FFPWeaponSettings& UfpstrueWeaponComponent::GetWeaponSettings() const
{
	return !bAmmoInitialized && WeaponConfiguration != nullptr ? WeaponConfiguration->Settings : Settings;
}

// 完整装备入口：校验角色、配置和挂点，挂接成功后登记双方关系，重装不补满弹药。
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

	// 在挂接或发布装备事件之前整组校验，配置失败不会留下半装备或半初始化的弹药。
	const FFPWeaponSettings& RequestedSettings = GetWeaponSettings();
	if (!bAmmoInitialized)
	{
		FString Error;
		if (!RequestedSettings.Validate(Error))
		{
			UE_LOG(LogTemp, Error, TEXT("Weapon configuration rejected on %s (%s): %s"),
				*GetPathName(), *GetPathNameSafe(WeaponConfiguration), *Error);
			return false;
		}
	}

	USkeletalMeshComponent* TargetMesh = TargetCharacter->GetMesh1P();
	if (TargetMesh == nullptr || !TargetMesh->DoesSocketExist(RequestedSettings.GripSocketName))
	{
		UE_LOG(LogTemp, Error, TEXT("AttachWeapon failed: character %s does not provide socket/bone %s."), *GetNameSafe(TargetCharacter),
			   *RequestedSettings.GripSocketName.ToString());
		return false;
	}

	FAttachmentTransformRules AttachmentRules(EAttachmentRule::SnapToTarget, true);
	if (!AttachToComponent(TargetMesh, AttachmentRules, RequestedSettings.GripSocketName))
	{
		return false;
	}

	// ★【重要边界】[状态提交] 先完成配置校验和挂接，再提交装备关系；重装只复用已有弹药，不重复初始化。
	// 所有外部操作成功后才提交角色引用和运行时状态，失败路径不会留下半装备状态。
	Character = TargetCharacter;
	if (!bAmmoInitialized)
	{
		Settings = RequestedSettings;
		CurrentAmmo = Settings.MagazineSize;
		ReserveAmmo = Settings.StartingReserveAmmo;
		bAmmoInitialized = true;
	}
	SetActionState(EFPWeaponActionState::Ready);
	TargetCharacter->SetEquippedWeaponComponent(this);
	// ★【重要边界】[回调重入] 装备通知会同步执行监听者；返回后必须复核双方关系，不能沿用广播前的成功判断。
	// 装备广播可同步卸下/销毁角色；只有双方关系仍成立才发布弹药或让拾取物消费成功。
	if (!IsOperational() || Character.Get() != TargetCharacter)
	{
		return false;
	}
	BroadcastAmmoChanged();

	return IsOperational() && Character.Get() == TargetCharacter;
}

// 幂等卸下武器：先停止本组件动作，再释放双方关系，回调不能重复进入卸下事务。
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

// 动作状态唯一切换入口；清理离开状态的计时与播放记录，并推进版本使旧回调失效。
void UfpstrueWeaponComponent::SetActionState(EFPWeaponActionState NewState)
{
	// 禁用清理独立于上一个动作；重复 Disable 也清理残留，不在各生命周期出口复制规则。
	if (NewState == EFPWeaponActionState::Disabled)
	{
		ClearRecoilState();
		StopVisualRecoil();
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
	// ★【重要边界】[异步身份] 每次真实状态切换推进版本；已清计时器不能代替回调自身的版本校验。
	ActionState = NewState;
	++ActionRevision;
}

// 判断武器是否仍与存活角色正确装备且未禁用；生命周期失效与临时输入中断分开处理。
bool UfpstrueWeaponComponent::IsOperational() const
{
	// 汇总装备关系与角色生命周期；已经卸下的组件不能再接受射击或装填。
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	return !bEndingPlay && IsValid(OwningCharacter) && OwningCharacter->CanMaintainEquipment()
		&& ActionState != EFPWeaponActionState::Disabled
		&& OwningCharacter->GetEquippedWeaponComponent() == this;
}

// 核对状态、动作版本和装备资格，防止同步通知返回后旧调用栈继续推进新动作。
bool UfpstrueWeaponComponent::IsCurrentAction(EFPWeaponActionState ExpectedState, uint32 ExpectedRevision) const
{
	return ActionState == ExpectedState && ActionRevision == ExpectedRevision && IsOperational();
}

// 在装备资格之外检查角色输入和组件内部迁移保护，拒绝清理或播放变化期间的新动作。
bool UfpstrueWeaponComponent::CanAcceptOwnerInput() const
{
	return IsOperational() && Character->CanAcceptGameplayInput() && !bInterruptingOwnerInput && !bChangingReloadPlayback;
}

// ==================== Fire System ====================

// 仅从 Ready 接纳开火输入，先提交 Firing 状态，再执行第一发。
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

// 只结束正在进行的连射；松开扳机不能解除换弹或重新启用已禁用武器。
void UfpstrueWeaponComponent::StopFire()
{
	// 只退出 Firing；松开扳机不能解除 Reloading/Disabled。
	if (ActionState == EFPWeaponActionState::Firing)
	{
		SetActionState(EFPWeaponActionState::Ready);
	}
}

// 一次射击的提交入口：校验射速与装备，先扣弹并命中结算，再发事件和安排下一发。
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
	// 语法复习：弱指针解析为局部裸指针后，只在当前 Game Thread 调用栈内临时使用，不保存到下一帧。
	AfpstrueCharacter* OwningCharacter = Character.Get();
	UCameraComponent* Camera = OwningCharacter->GetFirstPersonCameraComponent();
	if (OwningCharacter->GetController() == nullptr || Camera == nullptr)
	{
		StopFire();
		return;
	}

	const double CurrentTimeSeconds = World->GetTimeSeconds();
	const double FireInterval = 60.0 / static_cast<double>(Settings.RoundsPerMinute);
	if (LastShotTimeSeconds >= 0.0 && CurrentTimeSeconds - LastShotTimeSeconds + KINDA_SMALL_NUMBER < FireInterval)
	{
		ScheduleNextShot();
		return;
	}
	// 同一个“已接受射击时间”服务射速和散布；先读取上次时间，再写入这次提交。
	if (LastShotTimeSeconds < 0.0 || CurrentTimeSeconds - LastShotTimeSeconds > Settings.SpreadResetDelay)
	{
		ConsecutiveShotCount = 0;
	}
	LastShotTimeSeconds = CurrentTimeSeconds;

	const uint32 FiringRevision = ActionRevision;
	// ★【重要边界】[状态提交] 扣弹和本发命中先成为事实，再通知表现；回调可以中断后续连射，不能撤销这一发。
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

	// ★【重要边界】[回调重入] 回调可能卸枪、死亡或开始新动作；旧射击栈只有身份仍匹配才安排下一发。
	// 回调之后只校验一次动作归属；旧射击不能为新动作换弹或重设连射 Timer。
	if (!IsCurrentAction(EFPWeaponActionState::Firing, FiringRevision)) return;
	// 最后一发完成命中和表现后再退出射击状态，避免 Firing 残留。
	if (!HasAmmo())
	{
		HandleEmptyMagazine();
	}
	else
	{
		ScheduleNextShot();
	}
}

// 按上一发的实际时间安排剩余冷却；回调保存动作版本，取消后的旧计时不能继续开枪。
void UfpstrueWeaponComponent::ScheduleNextShot()
{
	if (UWorld* World = GetWorld())
	{
		const double Interval = 60.0 / static_cast<double>(Settings.RoundsPerMinute);
		const float Remaining = static_cast<float>(FMath::Max(LastShotTimeSeconds + Interval - World->GetTimeSeconds(), 0.001));
		const uint32 ScheduledRevision = ActionRevision;
		World->GetTimerManager().SetTimer(AutomaticFireTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, ScheduledRevision]()
			{
				if (IsCurrentAction(EFPWeaponActionState::Firing, ScheduledRevision)) Fire();
			}), Remaining, false);
	}
}

// 空仓共用入口：尝试进入统一换弹流程，不能换弹时停止连射。
void UfpstrueWeaponComponent::HandleEmptyMagazine()
{
	if (!RequestReload())
	{
		StopFire();
	}
}

// 按当前瞄准和连射散布执行一条 Hitscan，结算部位伤害及物理冲量，再报告弹道结果。
void UfpstrueWeaponComponent::FireLineTrace(UWorld& World, AfpstrueCharacter& OwningCharacter, UCameraComponent& Camera)
{
	// 根据瞄准状态和连续射击次数计算散布，并在同一入口完成单射线查询。
	// Fire 已校验本发的角色/相机；在外部伤害或表现回调之前直接使用这些依赖，不重复解析装备关系。
	// 配置在首次装备时已校验并冻结，不逐发修正同一字段；运行时累计值仍需限制。
	const float ContinuousSpreadAngle = FMath::Min(ConsecutiveShotCount * Settings.ContinuousFireSpreadStep, Settings.MaxContinuousFireSpreadAngle);
	// 基础与连射配置各自合法不代表总和合法；总角度统一有限且不超过 45 度，避免 tan 接近 90 度。
	const float TotalSpreadAngle = (OwningCharacter.IsAiming() ? Settings.AimFireSpreadAngle : Settings.HipFireSpreadAngle)
		+ ContinuousSpreadAngle;
	const float SpreadAngle = FMath::IsFinite(TotalSpreadAngle) ? FMath::Clamp(TotalSpreadAngle, 0.0f, MaxTotalSpreadAngle) : 0.0f;
	++ConsecutiveShotCount;

	// 从相机发出射击专用射线，返回第一个阻挡命中，并据骨骼名称结算点伤害。
	const FVector Start = Camera.GetComponentLocation();
	const FVector Forward = Camera.GetForwardVector();
	const FVector ShotDirection = SpreadAngle > 0.0f ? MakeGaussianSpreadDirection(Forward, SpreadAngle) : Forward;
	const FVector End = Start + ShotDirection * Settings.LineTraceRange;

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
		const bool bCriticalHit = Settings.CriticalHitBones.Contains(HitResult.BoneName);
		const float DamageToApply = bCriticalHit ? Settings.LineTraceHeadDamage : Settings.LineTraceDamage;

		UGameplayStatics::ApplyPointDamage(HitActor, DamageToApply, ShotDirection, HitResult, OwningCharacter.GetController(), GetOwner(),
									   nullptr);

		// 受伤事件可能销毁目标；仅对仍有效的非角色物理组件追加冲量。
		UPrimitiveComponent* HitComponent = HitResult.GetComponent();
		if (IsValid(HitActor) && !HitActor->IsA<ACharacter>() && IsValid(HitComponent) && HitComponent->IsSimulatingPhysics())
		{
			HitComponent->AddImpulseAtLocation(ShotDirection * Settings.LineTraceImpulse, HitResult.ImpactPoint);
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

// 接纳手动或空仓换弹：建立事务编号与兜底计时，再请求蓝图播放，不立即装弹。
bool UfpstrueWeaponComponent::RequestReload()
{
	// Request 只开启换弹事务，不立刻搬运弹药；真正提交点由动画 Notify 决定，使数值变化与装填动作对齐。
	// 换弹必须同时满足：武器可用、当前不在换弹、弹匣未满且仍有备弹。
	if (!CanAcceptOwnerInput() || (ActionState != EFPWeaponActionState::Ready && ActionState != EFPWeaponActionState::Firing)
		|| GetWorld() == nullptr || CurrentAmmo >= Settings.MagazineSize || ReserveAmmo <= 0)
	{
		return false;
	}

	const bool bWasEmptyReload = CurrentAmmo <= 0;
	// ★【重要边界】[状态提交] 换弹先建立动作锁、事务编号和超时，再触发瞄准退出及蓝图播放，禁止暴露半初始化状态。
	SetActionState(EFPWeaponActionState::Reloading);
	ActiveReloadId = ActiveReloadId == MAX_int32 ? 1 : ActiveReloadId + 1;
	const int32 RequestedReloadId = ActiveReloadId;
	// 先建立本次兜底，再广播；监听者同步 Finish/Cancel 时才能一并清除正确的 Timer。
	const float SelectedReloadDuration = bWasEmptyReload ? Settings.EmptyReloadDuration : Settings.ReloadDuration;
	// 超时 Timer 是动作锁的容错，不替代装填 Notify；动画链断开时不把失败当成补弹成功。
	// 使用唯一超时句柄，显式播放可按真实长度延长截止点；排队的旧回调只能结束它保存的那次换弹。
	const float Timeout = FMath::Max(0.01f, FMath::Max(SelectedReloadDuration, Settings.ReloadFailSafeDuration) + Settings.ReloadCompletionGracePeriod);
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

// 诊断旧的无身份装弹调用；保留反射兼容入口，但拒绝替旧通知借用当前换弹编号。
bool UfpstrueWeaponComponent::CommitReload()
{
	UE_LOG(LogTemp, Warning, TEXT("Legacy identityless CommitReload rejected on %s. Use a saved ReloadId or native ReloadCommit notify."), *GetPathName());
	return false;
}

// 核对当前 Reloading 状态及事务编号；装备资格在具体播放或提交入口另行检查。
bool UfpstrueWeaponComponent::IsCurrentReload(int32 ReloadId) const
{
	return ReloadId != INDEX_NONE && IsReloading() && ActiveReloadId == ReloadId;
}

// 显式播放并绑定本次换弹的手臂或武器 Montage；失败结束事务，较长播放延长兜底等待。
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
	// ★【重要边界】[回调重入] 播放会打断旧 Montage 并触发回调；随后绑定再次校验本次事务，不仅判断播放返回值。
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
	const float Timeout = FMath::Max(Remaining, Duration + Settings.ReloadCompletionGracePeriod);
	GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
		FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::EndReload, ReloadId, EFPReloadEndReason::TimedOut), FMath::Max(0.01f, Timeout), false);
	return true;
}

// 登记明确的活动播放实例并接管结束委托；同 Mesh 已有不同播放时拒绝覆盖。
bool UfpstrueWeaponComponent::BindReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage)
{
	if (!IsCurrentReload(ReloadId) || !IsOperational() || !IsValid(PlaybackMesh)
		|| (PlaybackMesh != this && PlaybackMesh != Character->GetMesh1P())) return false;
	FFPActionPlayback* Existing = ReloadPlaybacks.FindByPredicate([PlaybackMesh](const FFPActionPlayback& P) { return P.Mesh.Get() == PlaybackMesh; });
	if (Existing != nullptr) return Existing->TryBind(ReloadId, PlaybackMesh, Montage) != nullptr;
	FFPActionPlayback Playback;
	FAnimMontageInstance* Instance = Playback.TryBind(ReloadId, PlaybackMesh, Montage); // TryBind 后没有外部回调，实例仍有效。
	if (Instance == nullptr) return false;
	ReloadPlaybacks.Add(Playback);
	// UObject 委托弱引用组件，并把本次身份作为固定 payload 保存；回调不查询“当前动画”。
	Instance->OnMontageEnded.BindUObject(this, &UfpstrueWeaponComponent::HandleReloadPlaybackEnded,
		ReloadId, Playback.Mesh, Playback.MontageInstanceId);
	return true;
}

// 处理本次已绑定实例的结束：任一中断取消换弹，最后一个自然结束时完成换弹。
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

// 换弹装填帧携带原始 Montage 播放身份；旧动画通知不能通过“当前装备”提交新一轮换弹。
void UfpstrueAnimNotify_ReloadCommit::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
											 const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (MeshComp == nullptr)
	{
		return;
	}

	// 语法复习：UE Cast 只做 UObject 继承关系检查；两个无继承关系的组件之间 Cast 永远失败。
	// MeshComp 是播放动画的骨骼组件，应从它的 Owner 取得装备关系。
	UfpstrueWeaponComponent* WeaponComponent = Cast<UfpstrueWeaponComponent>(MeshComp);
	if (const AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(MeshComp->GetOwner()))
	{
		WeaponComponent = Character->GetEquippedWeaponComponent();
	}

	if (WeaponComponent != nullptr)
	{
		WeaponComponent->CommitReloadFromNotify(MeshComp, EventReference);
	}
}

// 保留分支点的原始 Montage 实例编号，再转入与普通 Notify 共用的装弹校验。
void UfpstrueAnimNotify_ReloadCommit::BranchingPointNotify(FBranchingPointNotifyPayload& Payload)
{
	// ★【重要边界】[异步身份] 分支点必须传递原始播放实例编号；装填的即时执行不等于可以省略来源校验。
	// UE 的默认桥接会丢弃 MontageInstanceID；显式携带，Queued/BranchingPoint 使用同一身份校验。
	FAnimNotifyEventReference EventReference(Payload.NotifyEvent, Payload.SequenceAsset);
	EventReference.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(Payload.MontageInstanceID);
	Notify(Payload.SkelMeshComponent, Payload.SequenceAsset, EventReference);
}

// 返回动画编辑器显示名称，不读取或修改换弹事务。
FString UfpstrueAnimNotify_ReloadCommit::GetNotifyName_Implementation() const
{
	// 为动画编辑器提供可读名称，不参与运行时换弹状态判断。
	return TEXT("Reload Commit");
}

// 先核对 Notify 的 Mesh、AnimInstance 与播放实例，匹配后才提交当前换弹编号。
bool UfpstrueWeaponComponent::CommitReloadFromNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference)
{
	const bool bMatchesPlayback = ReloadPlaybacks.ContainsByPredicate([this, PlaybackMesh, &EventReference](const FFPActionPlayback& Playback)
	{
		return Playback.Matches(ActiveReloadId, PlaybackMesh, EventReference);
	});
	return bMatchesPlayback && CommitReloadForTransaction(ActiveReloadId);
}

// 按弹匣缺口与备弹量转移一次弹药；先标记已提交再通知 HUD，防止回调重复装弹。
bool UfpstrueWeaponComponent::CommitReloadForTransaction(int32 ReloadId)
{
	// ★【重要边界】[结算去重] 当前事务、装备资格和单次提交标记共同保护装弹；重复通知不能再次搬运备弹。
	// bReloadAmmoCommitted 是单次提交保护：同一轮换弹的重复 Notify 只会第一次搬运弹药。
	if (!IsCurrentReload(ReloadId) || bReloadAmmoCommitted || !IsOperational())
	{
		return false;
	}

	const int32 AmmoNeeded = Settings.MagazineSize - CurrentAmmo;
	const int32 AmmoToLoad = FMath::Min(AmmoNeeded, ReserveAmmo);
	CurrentAmmo += AmmoToLoad;
	ReserveAmmo -= AmmoToLoad;
	// 先标记已提交再通知 HUD，防止监听者重入 CommitReload 导致重复装弹。
	bReloadAmmoCommitted = true;
	BroadcastAmmoChanged();
	return true;
}

// 诊断旧的无身份完成调用；正常结束必须来源于已绑定实例或保存的事务编号。
void UfpstrueWeaponComponent::FinishReload()
{
	UE_LOG(LogTemp, Warning, TEXT("Legacy identityless FinishReload rejected on %s. Use PlayReloadMontage or a saved ReloadId."), *GetPathName());
}

// 同步取消当前换弹；延迟回调应使用保存编号的 CancelReloadForTransaction。
void UfpstrueWeaponComponent::CancelReload()
{
	CancelReloadForTransaction(GetActiveReloadId());
}

// 请求正常完成指定换弹，统一入口可补交尚未提交的弹药并清理动作。
void UfpstrueWeaponComponent::FinishReloadForTransaction(int32 ReloadId)
{
	EndReload(ReloadId, EFPReloadEndReason::Completed);
}

// 请求取消指定换弹；旧编号无效，取消不补弹也不撤销已提交弹药。
void UfpstrueWeaponComponent::CancelReloadForTransaction(int32 ReloadId)
{
	EndReload(ReloadId, EFPReloadEndReason::Cancelled);
}

// 统一收口换弹完成、中断、超时和禁用；失效旧事务并停止旧播放后再发布结束事件。
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
	// ★【重要边界】[生命周期] 先失效旧状态，再停止其动画，最后广播结束；Stop 触发的旧回调不能再次结算换弹。
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

// 应用本发受限控制后坐力及独立画面脉冲，再建立真实游戏时间驱动的恢复计时。
void UfpstrueWeaponComponent::ApplyRecoil(APlayerController* PlayerController)
{
	// 本发后坐力先累加到受限范围，再启动固定频率的恢复 Timer；瞄准时使用较小倍率。
	if (PlayerController == nullptr)
	{
		return;
	}

	const AfpstrueCharacter* OwningCharacter = Character.Get();
	const float RecoilMultiplier = OwningCharacter != nullptr && OwningCharacter->IsAiming() ? Settings.AimRecoilMultiplier : 1.0f;
	const float PitchKick = -Settings.RecoilPitch * RecoilMultiplier;
	const float YawKick = FMath::FRandRange(-Settings.RecoilYaw, Settings.RecoilYaw) * RecoilMultiplier;
	const float NewPitch = FMath::Clamp(AccumulatedRecoilPitch + PitchKick, -Settings.MaxAccumulatedRecoilPitch, 0.0f);
	const float NewYaw = FMath::Clamp(AccumulatedRecoilYaw + YawKick, -Settings.MaxAccumulatedRecoilYaw, Settings.MaxAccumulatedRecoilYaw);

	PlayerController->AddPitchInput(NewPitch - AccumulatedRecoilPitch);
	PlayerController->AddYawInput(NewYaw - AccumulatedRecoilYaw);
	AccumulatedRecoilPitch = NewPitch;
	AccumulatedRecoilYaw = NewYaw;

	// 控制后坐力与散布保持原样；本层只影响最终 POV，绕过 CameraBoom 对短脉冲的平滑。
	// 不使用 Initializer：UE 对带 Initializer 的单实例 shake 会重建，无法保留连发回弹。
	if (PlayerController->IsLocalController() && PlayerController->PlayerCameraManager != nullptr
		&& PlayerController->GetViewTarget() == OwningCharacter && Settings.VisualRecoilScale > 0.0f)
	{
		const float VisualScale = Settings.VisualRecoilScale
			* (OwningCharacter->IsAiming() ? Settings.AimVisualRecoilMultiplier : 1.0f);
		UCameraShakeBase* Shake = PlayerController->PlayerCameraManager->StartCameraShake(UfpstrueWeaponCameraShake::StaticClass(), VisualScale);
		if (Shake != nullptr)
		{
			CastChecked<UfpstrueWeaponRecoilPattern>(Shake->GetRootShakePattern())->Configure(Settings);
			VisualRecoilShake = Shake;
		}
	}

	if (UWorld* World = GetWorld())
	{
		// 延迟是“不恢复”的时间，不计入恢复步长；卡顿后的步长使用真实经过的游戏时间。
		LastRecoilRecoveryTimeSeconds = World->GetTimeSeconds() + Settings.RecoilRecoveryDelay;
		World->GetTimerManager().SetTimer(RecoilRecoveryTimerHandle, this, &UfpstrueWeaponComponent::UpdateRecoilRecovery,
										  RecoilRecoveryTickInterval, true, Settings.RecoilRecoveryDelay);
	}
}

// 按计时回调之间的实际游戏时间逐步抵消累计后坐力，装备失效或恢复完成时停止。
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
	const float NewPitch = FMath::FInterpConstantTo(AccumulatedRecoilPitch, 0.0f, DeltaSeconds, Settings.RecoilRecoverySpeed);
	const float NewYaw = FMath::FInterpConstantTo(AccumulatedRecoilYaw, 0.0f, DeltaSeconds, Settings.RecoilRecoverySpeed);

	PlayerController->AddPitchInput(NewPitch - AccumulatedRecoilPitch);
	PlayerController->AddYawInput(NewYaw - AccumulatedRecoilYaw);
	AccumulatedRecoilPitch = NewPitch;
	AccumulatedRecoilYaw = NewYaw;

	if (FMath::IsNearlyZero(AccumulatedRecoilPitch, KINDA_SMALL_NUMBER) && FMath::IsNearlyZero(AccumulatedRecoilYaw, KINDA_SMALL_NUMBER))
	{
		ClearRecoilState();
	}
}

// 清除控制后坐力恢复计时和累计量，不负责停止其他来源的镜头效果。
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

// 只停止本武器保存的画面脉冲，保留受击等其他来源的 CameraShake。
void UfpstrueWeaponComponent::StopVisualRecoil()
{
	if (UCameraShakeBase* Shake = VisualRecoilShake.Get())
	{
		if (APlayerCameraManager* CameraManager = Shake->GetCameraManager()) CameraManager->StopCameraShake(Shake, true);
	}
	VisualRecoilShake.Reset();
}

// Copyright Epic Games, Inc. All Rights Reserved.

// 配置单实例视觉震动和自定义 Pattern，让连发重触发而不是无限叠加震动层。
UfpstrueWeaponCameraShake::UfpstrueWeaponCameraShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSingleInstance = true;
	SetRootShakePattern(CreateDefaultSubobject<UfpstrueWeaponRecoilPattern>(TEXT("WeaponRecoilPattern")));
}

// 复制当前武器的视觉峰值与时长，保持画面表现和控制后坐力、散射各自独立。
void UfpstrueWeaponRecoilPattern::Configure(const FFPWeaponSettings& Settings)
{
	// 武器首次装备已校验配置。这里仅复制视觉参数，不能读取/修改散布或控制后坐力累计值。
	PeakRotation = Settings.VisualRecoilRotation;
	PeakFOV = Settings.VisualRecoilFOV;
	Duration = Settings.VisualRecoilDuration;
}

// 告知引擎使用自定义时长；脉冲由本 Pattern 的经过时间与完成判断自行收尾。
void UfpstrueWeaponRecoilPattern::GetShakePatternInfoImpl(FCameraShakeInfo& OutInfo) const
{
	// 时长由每件武器的配置决定，脉冲自行计时和收尾，避免再叠一层引擎淡入淡出。
	OutInfo.Duration = FCameraShakeDuration::Custom();
}

// 重触发时从当前偏移继续脉冲，新播放从零开始；交替侧倾不扰动子弹的随机序列。
void UfpstrueWeaponRecoilPattern::StartShakePatternImpl(const FCameraShakePatternStartParams& Params)
{
	// 重触发从当前偏移冲向本发峰值，不先跳回零；新实例/回收实例从零开始。
	StartRotation = Params.bIsRestarting && !bStopped ? CurrentRotation : FRotator::ZeroRotator;
	StartFOV = Params.bIsRestarting && !bStopped ? CurrentFOV : 0.0f;
	CurrentRotation = StartRotation;
	CurrentFOV = StartFOV;
	Elapsed = 0.0f;
	bStopped = false;
	// 不消耗全局随机数：增加纯视觉效果不能扰动后续子弹散布的随机序列。
	Side = -Side;
}

// 前段快速冲向峰值、后段平滑回正，仅向最终画面输出旋转与 FOV 偏移。
void UfpstrueWeaponRecoilPattern::UpdateShakePatternImpl(const FCameraShakePatternUpdateParams& Params, FCameraShakePatternUpdateResult& OutResult)
{
	if (bStopped) return;
	Elapsed = FMath::Min(Elapsed + FMath::Max(Params.DeltaTime, 0.0f), Duration);
	const float Progress = Elapsed / Duration;
	const FRotator TargetRotation(PeakRotation.Pitch, PeakRotation.Yaw * Side, PeakRotation.Roll * Side);
	constexpr float AttackFraction = 0.15f;
	if (Progress < AttackFraction)
	{
		const float Attack = FMath::SmoothStep(0.0f, AttackFraction, Progress);
		CurrentRotation = FMath::Lerp(StartRotation, TargetRotation, Attack);
		CurrentFOV = FMath::Lerp(StartFOV, PeakFOV, Attack);
	}
	else
	{
		const float Return = 1.0f - FMath::SmoothStep(AttackFraction, 1.0f, Progress);
		CurrentRotation = TargetRotation * Return;
		CurrentFOV = PeakFOV * Return;
	}
	// 局部旋转带来抽动/侧倾，微小 FOV 脉冲带来透视冲击；无位置推移，避免镜头穿墙。
	OutResult.Rotation = CurrentRotation;
	OutResult.FOV = CurrentFOV;
}

// 强制停止脉冲并归零视觉偏移，供死亡、卸下与控制权迁移清理使用。
void UfpstrueWeaponRecoilPattern::StopShakePatternImpl(const FCameraShakePatternStopParams& Params)
{
	// 仅供死亡、卸下、失去控制等强制中断；正常松开扳机让最后一发自行回正。
	bStopped = true;
	CurrentRotation = FRotator::ZeroRotator;
	CurrentFOV = 0.0f;
}

// ==================== 生命周期结束与公共清理 ====================

// 组件退出时先阻止新动作，通过完整卸枪入口清理计时和装备关系，再执行父类退出。
void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// ★【重要边界】[生命周期] 先拒绝新动作，再走完整卸枪清理；只清 Timer 不能解除双方装备关系和视觉状态。
	// 组件退出前清理所有 Timer，并通知角色解除装备关系，避免留下跨生命周期的回调和悬空关系。
	bEndingPlay = true;
	DetachWeapon();
	Super::EndPlay(EndPlayReason);
}

// 死亡或卸下时永久禁用动作；若正在换弹，携带 Disabled 原因走统一结束入口。
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

// 控制权迁移只终止旧输入的持续动作与后坐力；角色仍有效时保留装备和弹药。
void UfpstrueWeaponComponent::InterruptOwnerInput()
{
	AfpstrueCharacter* OwningCharacter = Character.Get();
	if (OwningCharacter == nullptr || !OwningCharacter->CanMaintainEquipment())
	{
		DisableWeapon();
		return;
	}
	// Character 自己管理 ADS/冲刺输入资格；武器只拥有本模块清理期间的重入边界。
	// ★【重要边界】[生命周期] 临时丢失输入只清持续动作，不销毁装备或弹药；角色真正失效才转入永久禁用。
	// 不能直接让 IsOperational 返回 false，否则 CancelReload 会把临时中断误变成永久 Disabled。
	TGuardValue<bool> InputTransitionGuard(bInterruptingOwnerInput, true);
	StopFire();
	ClearRecoilState();
	StopVisualRecoil();
	CancelReload();
}

// 发布已经提交的弹药快照，HUD 和蓝图只观察结果，不在此重复结算。
void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	// 弹药状态只由 WeaponComponent 写入；角色、HUD 和蓝图通过该委托读取同一份结果。
	OnAmmoChanged.Broadcast(CurrentAmmo, Settings.MagazineSize, ReserveAmmo);
}
