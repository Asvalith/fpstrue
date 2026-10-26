// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueWeaponActors.h"
#include "Weapons/fpstruePickUpComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Player/fpstrueCharacterMovementComponent.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "AbilitySystemComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Runtime/fpstrueRuntime.h"
#include "GameFramework/PlayerController.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstruePvpGameMode.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Animation/AnimInstance.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Engine/SkeletalMesh.h"
#include "Components/PrimitiveComponent.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "Engine/PackageMapClient.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/MiscTrace.h"

/*
 * 玩家武器的单发结算与换弹事务实现。
 * GAS 独占动作互斥和本地连射输入；组件拥有已准入网络执行队列、装备、弹药、射速约束与后坐力恢复。
 * 角色向 ASC 提交输入；所有模式都由原生唯一 Timer 按服务器时长完成换弹。
 * 原生/蓝图表现桥选择素材并消费事实；C++ 拥有命中/弹药结算以及明确的播放身份和结束边界。
 *
 * 主要调用链：
 *   输入 -> Character -> ASC / Fire GA -> 单机 Fire / 联网有界输入批次 -> 重排与准入队列 -> 执行复核 -> Fire -> Hitscan
 *   权威结算 -> 弹药/动作快照 + 逐发 ACK + 单发表现多播；拥有者只预测表现和未确认弹药缺口。
 *   输入 -> Character::RequestWeaponReload -> ASC / Reload GA -> BeginAbilityReload -> PlaybackRequested(ReloadId)
 *   Reload Timer -> FinishReloadForTransaction -> 提交弹药并完成；主动取消只终止未完成事务。
 *   动画结束只回收播放身份；缺少/中断表现不反向提前装弹或延长服务器换弹。
 *
 * CurrentAmmo、ReserveAmmo 和后坐力累计量都只由本组件写入；Character/HUD 通过只读接口和
 * Delegate 观察结果，从结构上避免蓝图、角色和武器各保存一份可变状态。
 */

namespace
{
constexpr float RecoilRecoveryTickInterval = 1.0f / 60.0f;
constexpr float GaussianSpreadSigmaCount = 3.0f;
constexpr float MaxTotalSpreadAngle = 45.0f;
constexpr double MaxShotFutureSeconds = 0.025;
// 显示缓冲由服务器配置；RTT/2 仅是对称路径估计，不声称复原 UE 平滑器每帧的实际显示时间。
constexpr double DefaultTargetInterpolationSeconds = 0.05;

// 准入按延迟收紧时间窗；排队执行仍不得超过历史判定的最长窗口。
bool IsShotTimeWithinWindow(double FireTime, double Now, double MaxAge)
{
	return FMath::IsFinite(FireTime) && FireTime <= Now + MaxShotFutureSeconds && FireTime >= Now - MaxAge;
}



// 两种原生素材使用同一个拾取组件和重叠规则；组件自行保护同帧重复提交。


#if !UE_BUILD_SHIPPING
bool ShouldLogShotProtocol()
{
	return FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest"))
		|| FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity"));
}

// 逐发核账使用跨端一致的武器身份，不能把两个玩家相同的本地发号混为一发。
uint64 GetWeaponShotNetGuid(const AActor* Actor)
{
	UNetDriver* Driver = IsValid(Actor) ? Actor->GetNetDriver() : nullptr;
	if (Driver == nullptr || !Driver->GuidCache.IsValid()) return 0;
	return (Actor->HasAuthority() ? Driver->GuidCache->GetOrAssignNetGUID(const_cast<AActor*>(Actor))
		: Driver->GuidCache->GetNetGUID(Actor)).ObjectId;
}

// 诊断日志取得跨进程可核对的 PlayerId；无 PlayerState 时明确返回无效编号。
int32 GetShotPlayerId(const AfpstrueCharacter* Character)
{
	const APlayerState* PlayerState = IsValid(Character) ? Character->GetPlayerState() : nullptr;
	return PlayerState != nullptr ? PlayerState->GetPlayerId() : INDEX_NONE;
}

// 只在显式烟测记录拒绝阶段、时间窗与连接统计，不放宽准入或执行的规则。
void LogShotTimeRejection(const FFPShotRequest& Request, const AfpstrueCharacter* Character, const AActor* Weapon,
	const TCHAR* Stage, double Now, double AllowedAge, const TCHAR* SpecificReason = nullptr)
{
	if (!ShouldLogShotProtocol()) return;
	const APlayerState* Player = Character ? Character->GetPlayerState() : nullptr;
	const UNetConnection* Connection = Character ? Character->GetNetConnection() : nullptr;
	const TCHAR* Reason = SpecificReason ? SpecificReason : !FMath::IsFinite(Request.ServerFireTime) ? TEXT("NonFinite")
		: Request.ServerFireTime > Now + MaxShotFutureSeconds ? TEXT("Future") : TEXT("Stale");
	UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_TIME_REJECT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu Stage=%s Reason=%s AgeMs=%.3f AllowedAgeMs=%.3f PingMs=%.3f InLostTotal=%d OutLostTotal=%d"),
		Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(Weapon), Stage, Reason,
		(Now - Request.ServerFireTime) * 1000.0, AllowedAge * 1000.0, Player ? Player->GetPingInMilliseconds() : 0.0,
		Connection ? Connection->InTotalPacketsLost : -1, Connection ? Connection->OutTotalPacketsLost : -1);
}
#endif

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

// 批次复用同一输入格式；仅序列化输入身份、时间与量化方向，不接收客户端命中结论。
bool FFPShotRequest::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	bOutSuccess = false;
	if (Ar.IsSaving() && RenderedViews.Num() > MaxRenderedViews) { Ar.SetError(); return false; }
	UObject* AvatarObject = Avatar.Get();
	UObject* WeaponObject = Weapon.Get();
	const bool bAvatarMapped = Map != nullptr && Map->SerializeObject(Ar, AfpstrueCharacter::StaticClass(), AvatarObject);
	const bool bWeaponMapped = Map != nullptr && Map->SerializeObject(Ar, AActor::StaticClass(), WeaponObject);
	if (Ar.IsLoading()) { Avatar = Cast<AfpstrueCharacter>(AvatarObject); Weapon = Cast<AActor>(WeaponObject); }
	Ar << EquipmentGeneration << FireSession << ShotId << ServerFireTime;
	bool bDirectionValid = false;
	AimDirection.NetSerialize(Ar, Map, bDirectionValid);
	uint8 Count = Ar.IsSaving() ? static_cast<uint8>(RenderedViews.Num()) : 0;
	Ar.SerializeBits(&Count, 5);
	if (Count > MaxRenderedViews) { RenderedViews.Reset(); Ar.SetError(); return false; }
	if (Ar.IsLoading()) RenderedViews.SetNum(Count);
	bool bAllMapped = bAvatarMapped && bWeaponMapped;
	for (FFPShotRenderedView& View : RenderedViews)
	{
		UObject* TargetObject = View.Target.Get();
		const bool bTargetMapped = Map != nullptr && Map->SerializeObject(Ar, AfpstrueCharacter::StaticClass(), TargetObject);
		bAllMapped &= bTargetMapped;
		if (Ar.IsLoading()) View.Target = Cast<AfpstrueCharacter>(TargetObject);
		Ar << View.AgeMilliseconds;
	}
	bOutSuccess = bDirectionValid && !Ar.IsError();
	return bAllMapped;
}

double FFPShotRequest::GetTargetViewTime(const AfpstrueCharacter* Target, double Fallback, bool* bOutNativeView) const
{
	if (bOutNativeView != nullptr) *bOutNativeView = false;
	if (Target == nullptr) return Fallback;
	for (const FFPShotRenderedView& View : RenderedViews)
		if (View.Target.Get() == Target && View.AgeMilliseconds >= MinRenderedViewAgeMilliseconds
			&& View.AgeMilliseconds <= MaxRenderedViewAgeMilliseconds)
		{
			if (bOutNativeView != nullptr) *bOutNativeView = true;
			return ServerFireTime - static_cast<double>(View.AgeMilliseconds) * .001;
		}
	return Fallback;
}

// 默认武器组件的复制由所属 Actor 的网络通道承载，不创建第二个业务状态机。
UfpstrueWeaponComponent::UfpstrueWeaponComponent()
{
	SetIsReplicatedByDefault(true);
	// 专服物理许可必须在身体初始化前确定；生前仍不模拟，只有死亡掉落才启用 SimulatePhysics。
	bEnablePhysicsOnDedicatedServer = true;
}

// 只订阅已经去重的射击事实和带事务身份的换弹请求；组件不从动画重新发起能力。
void UfpstrueWeaponComponent::BeginPlay()
{
	Super::BeginPlay();
	OnWeaponFirePerformed.AddUniqueDynamic(this, &ThisClass::PlayWeaponFirePresentation);
	OnWeaponReloadPlaybackRequested.AddUniqueDynamic(this, &ThisClass::PlayWeaponReloadPresentation);
	OnWeaponTraceFinished.AddUniqueDynamic(this, &ThisClass::PlayWeaponImpactPresentation);
}

void UfpstrueWeaponComponent::PostRepNotifies()
{
	Super::PostRepNotifies();
	// 掉落由首次状态/权威物理通知处理，不在同一批组件通知末尾重复配置碰撞、重力或物理体。
	if (GetNetMode() == NM_Client && !bDroppedOnDeath) RefreshNetworkEquipment();
}

// 纯枪械脉冲，不改变控制旋转、弹药或伤害；拥有者预测与多播的去重仍沿用现有入口。
void UfpstrueWeaponComponent::PlayWeaponFirePresentation()
{
	if (GetNetMode() == NM_DedicatedServer || !IsOperational()) return;
	// 迟到的旧射击事实仍可响枪声，但不能打断已经开始的新一轮换弹动画。
	if (UAnimInstance* Anim = GetAnimInstance(); Anim != nullptr && WeaponFireMontage != nullptr && !IsReloading()) Anim->Montage_Play(WeaponFireMontage);
	if (!IsValid(MuzzleSource)) return; // 枪口只来自实际蓝图装配，不回退到另一个组件。
	FTransform Muzzle = MuzzleSource->GetSocketTransform(MuzzleSocketName);
	Muzzle.SetRotation(Muzzle.TransformVectorNoScale(MuzzleForwardAxis).Rotation().Quaternion());
	if (FireSound != nullptr) UGameplayStatics::PlaySoundAtLocation(this, FireSound, Muzzle.GetLocation());
	if (MuzzleEffect != nullptr) UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), MuzzleEffect, Muzzle, true, EPSCPoolMethod::AutoRelease);
}

// 只消费服务器已经结算的命中结果；预测拥有者不在本地猜中弹，也不会重复 ApplyDamage。
void UfpstrueWeaponComponent::PlayWeaponImpactPresentation(bool bHit, FVector, FVector, FVector, FHitResult HitResult)
{
	if (GetNetMode() == NM_DedicatedServer || !bHit) return;
	const bool bFlesh = HitResult.GetActor() != nullptr && HitResult.GetActor()->IsA<APawn>();
	UParticleSystem* Effect = bFlesh ? FleshImpactEffect.Get() : ImpactEffect.Get();
	const FRotator Rotation = HitResult.ImpactNormal.Rotation();
	if (Effect != nullptr) UGameplayStatics::SpawnEmitterAtLocation(this, Effect, HitResult.ImpactPoint, Rotation, FVector::OneVector, true, EPSCPoolMethod::AutoRelease);
	if (!bFlesh && ImpactSound != nullptr) UGameplayStatics::PlaySoundAtLocation(this, ImpactSound, HitResult.ImpactPoint);
	if (!bFlesh && ImpactDecal != nullptr) UGameplayStatics::SpawnDecalAtLocation(this, ImpactDecal, ImpactDecalSize, HitResult.ImpactPoint, Rotation, ImpactDecalLifeSeconds);
}

// 手臂/身体由角色监听同一事件；枪械仅负责自己的弹匣、枪栓等骨骼播放。
void UfpstrueWeaponComponent::PlayWeaponReloadPresentation(int32 ReloadId, bool bWasEmptyReload)
{
	if (GetNetMode() == NM_DedicatedServer) return;
	UAnimMontage* Montage = bWasEmptyReload ? WeaponEmptyReloadMontage.Get() : WeaponReloadMontage.Get();
	if (Montage == nullptr) return; // 未配置则不播放该素材；权威换弹事务不依赖表现补偿。
	const float Duration = GetReloadDuration();
	const float PlayRate = Duration > 0.0f ? Montage->GetPlayLength() / Duration : 1.0f;
	PlayReloadMontage(ReloadId, this, Montage, PlayRate);
}

void UfpstrueWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UfpstrueWeaponComponent, NetworkState);
	DOREPLIFETIME_CONDITION(UfpstrueWeaponComponent, OwnerState, COND_OwnerOnly);
}

void UfpstrueWeaponComponent::GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const
{
	Super::GetReplicatedCustomConditionState(OutActiveState);
	const AActor* WeaponActor = GetOwner();
	if (WeaponActor == nullptr || WeaponActor->GetRootComponent() != this
		|| (!WeaponActor->IsA<AfpstrueRARWeapon>() && !WeaponActor->IsA<AfpstrueTemplateWeapon>())) return;
	// ★【复制唯一性】按实例关闭原生根网格的第二套位姿/附着通道，不按 Owner 分支修改共享 Class 复制布局。
	// 否则 SceneComponent::PostRepNotifies 会先消费迟到手部相对变换，把已落地枪重算回手边；旧非原生蓝图不受影响。
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, RelativeLocation, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, RelativeRotation, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, RelativeScale3D, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, AttachParent, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, AttachSocketName, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, AttachChildren, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, bShouldBeAttached, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, bShouldSnapLocationWhenAttached, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, bShouldSnapRotationWhenAttached, false);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(USceneComponent, bShouldSnapScaleWhenAttached, false);
}

// 先提交完整结果再通知监听者；强制网络更新不是可靠事件队列，只保证最新状态可继续发送。
void UfpstrueWeaponComponent::PublishNetworkState()
{
	AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority() || GetNetMode() == NM_Standalone) return;
	const UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	NetworkState.Holder = Character.Get();
	// 只读展示由能力标签派生；这里没有可反向改变能力资格的第二份 ActionState。
	NetworkState.Action = !bEquipmentEnabled ? EFPWeaponActionState::Disabled
		: ASC != nullptr && ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading) ? EFPWeaponActionState::Reloading
		: ASC != nullptr && ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing) ? EFPWeaponActionState::Firing
		: EFPWeaponActionState::Ready;
	NetworkState.ReloadId = ReloadSerial;
	NetworkState.bEmptyReload = bNetworkEmptyReload;
	NetworkState.bCommitted = bReloadAmmoCommitted;
	NetworkState.EndReason = LastNetworkReloadEndReason;
	NetworkState.EquipmentGeneration = EquipmentRevision;
	NetworkState.bDroppedOnDeath = bDroppedOnDeath;
	++NetworkState.Revision;
	// 同一次提交派生公共/拥有者快照；逐发裁决另用近期快速窗口与可靠兜底，不塞进公共快照。
	OwnerState.EquipmentGeneration = EquipmentRevision;
	OwnerState.Revision = NetworkState.Revision;
	OwnerState.Ammo = CurrentAmmo;
	OwnerState.Reserve = ReserveAmmo;
	OwnerState.IncludedShotId = LastExecutedShotId;
	OwnerActor->ForceNetUpdate();
}

bool UfpstrueWeaponComponent::ApplyOwnerNetworkState()
{
	if (GetOwner() == nullptr || GetOwner()->HasAuthority() || bDroppedOnDeath || !Character.IsValid()
		|| OwnerState.Revision == 0 || OwnerState.Revision <= LastAmmoStateRevision
		|| OwnerState.EquipmentGeneration != EquipmentRevision
		|| OwnerState.EquipmentGeneration != NetworkState.EquipmentGeneration
		|| LastPresentedStateRevision == 0) return false;
	// COND_OwnerOnly 已限定受众；不再要求 Controller 先到，也不让网格/Socket 就绪阻塞 HUD。
	CurrentAmmo = OwnerState.Ammo;
	ReserveAmmo = OwnerState.Reserve;
	LastAmmoStateRevision = OwnerState.Revision;
	LastAmmoIncludedShotId = FMath::Max(LastAmmoIncludedShotId, OwnerState.IncludedShotId);
	return true;
}

// 权威基准只写一次；显示扣除基准尚未包含的输入，快照先于 ACK 也不会重复扣弹。
int32 UfpstrueWeaponComponent::GetCurrentAmmo() const
{
	if (GetOwner() == nullptr || GetOwner()->HasAuthority()) return CurrentAmmo;
	int32 PendingDebit = 0;
	for (uint32 ShotId : OutstandingShots) PendingDebit += ShotId > LastAmmoIncludedShotId ? 1 : 0;
	return FMath::Max(0, CurrentAmmo - PendingDebit);
}

void UfpstrueWeaponComponent::OnRep_OwnerState()
{
	RefreshNetworkEquipment();
}

// 复制的组件引用不保证与持有者引用同一时刻解析；持有者就绪后再次调用可完成附着。
void UfpstrueWeaponComponent::RefreshNetworkEquipment()
{
	AfpstrueCharacter* Holder = Character.Get();
	if (bDroppedOnDeath) { ApplyDeathDropPhysics(); return; }
	if (IsValid(Holder) && Holder->IsDead() && Holder->UsesNativePresentation())
	{
		DropForDeath(Holder->GetVelocity());
		return;
	}
	if (ApplyOwnerNetworkState()) BroadcastAmmoChanged();
	if (!IsValid(Holder) || Holder->GetEquippedWeaponComponent() != this) return;
	USkeletalMeshComponent* TargetMesh = Holder->IsLocallyControlled() ? Holder->GetMesh1P() : Holder->GetMesh();
	if (TargetMesh == nullptr) return;
	const FName Socket = Settings.GripSocketName;
	// 未就绪/错误骨架不能静默改挂角色根部；保留原附着，待真实装备或附着就绪通知重试。
	if (!TargetMesh->DoesSocketExist(Socket)
		|| !AttachToComponent(TargetMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket)) return;
	// ★【复制顺序边界】同父级/插槽时 AttachTo 会直接返回；迟到的相对变换仍须纠正，不能只检查父子关系。
	// 保留网格缩放并应用作者握持偏移；四元数入口同时刷新世界变换，避免枪已附着却仍停在旧生成坐标。
	SetRelativeLocationAndRotation(GripLocationOffset, GripRotationOffset.Quaternion(), false, nullptr, ETeleportType::TeleportPhysics);
	SetOnlyOwnerSee(false);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// 死亡终态已在入口处理；正常装备只显示存活角色的本地手臂。
	Holder->GetMesh1P()->SetHiddenInGame(Holder->IsDead() || !Holder->IsLocallyControlled(), true);
	SetHiddenInGame(Holder->IsDead(), true);
	if (AfpstrueRARWeapon* Rifle = Cast<AfpstrueRARWeapon>(GetOwner())) Rifle->SetViewmodelFOVEnabled(Holder->IsLocallyControlled());
	if (!GetOwner()->HasAuthority() && ActiveReloadId != INDEX_NONE && IsReloading()
		&& LastPresentedReloadId != ActiveReloadId && IsOperational())
	{
		LastPresentedReloadId = ActiveReloadId;
		OnWeaponReloadPlaybackRequested.Broadcast(ActiveReloadId, NetworkState.bEmptyReload);
	}
}

// ★【网络边界】客户端恢复最新弹药与动作，不执行 Fire、Commit 或服务器换弹计时。
void UfpstrueWeaponComponent::OnRep_NetworkState()
{
	// ★【复制顺序边界】掉落事实不能被先到的弹药 ACK 或旧装备覆盖；后续位姿优先采用权威物理。
	if (NetworkState.bDroppedOnDeath)
	{
		Character = NetworkState.Holder;
		const bool bFirstDrop = !bDroppedOnDeath;
		bDroppedOnDeath = true;
		if (bFirstDrop) DisableWeapon();
		if (!bAppliedAuthoritativeDrop && !bReceivedAuthoritativePhysics)
		{
			const FVector Velocity = NetworkState.DropVelocity;
			ApplyDeathDropPhysics(&NetworkState.DropTransform, &Velocity);
		}
		else ApplyDeathDropPhysics();
		bAppliedAuthoritativeDrop = true;
		return;
	}
	if (bDroppedOnDeath) { ApplyDeathDropPhysics(); return; }
	// 公共动作只与公共版本比较；较新的可靠弹药 ACK 不能挡住此处的换弹/装备表现。
	if (NetworkState.Revision < LastPresentedStateRevision || NetworkState.EquipmentGeneration < EquipmentRevision) return;
	const EFPWeaponActionState Previous = LastPresentedAction;
	const int32 PreviousReloadId = ActiveReloadId;
	Character = NetworkState.Holder;
	if (!bAmmoInitialized)
	{
		Settings = GetWeaponSettings();
		bAmmoInitialized = true;
	}
	LastPresentedAction = NetworkState.Action;
	bEquipmentEnabled = NetworkState.Action != EFPWeaponActionState::Disabled;
	if (EquipmentRevision != NetworkState.EquipmentGeneration)
	{
		EndPredictedReload(LocalReloadPredictionKey, EFPReloadEndReason::Disabled);
		OutstandingShots.Reset();
		ClientShotInputs.Reset();
		FastShotReceipts.Reset();
		LastReliableShotResultRevision = 0;
		GetWorld()->GetTimerManager().ClearTimer(ShotRetryTimerHandle);
		LastAmmoIncludedShotId = 0;
		LastSubmittedFireTime = LastPredictedShotTime = -1.0;
		LastEndedReloadPredictionKey = 0;
		// 换人/重新占有时不借用上一代拥有者的弹药；匹配的私有快照随后补齐。
		CurrentAmmo = ReserveAmmo = 0;
	}
	EquipmentRevision = NetworkState.EquipmentGeneration;
	LastPresentedStateRevision = NetworkState.Revision;
	const bool bPendingPrediction = LocalReloadPredictionKey > 0 && ActiveReloadId == -LocalReloadPredictionKey - 1;
	const bool bMatchingReload = LocalReloadPredictionKey > 0 && NetworkState.ReloadPredictionKey == LocalReloadPredictionKey;
	ReloadSerial = NetworkState.ReloadId;
	// 同代际中途的 Ready 快照可能早于本次预测请求；只有同 key 的裁决或另一个权威换弹才能替换它。
	const bool bPreservePrediction = bPendingPrediction && !bMatchingReload
		&& NetworkState.Action != EFPWeaponActionState::Reloading && NetworkState.Action != EFPWeaponActionState::Disabled;
	if (bPendingPrediction && bMatchingReload && NetworkState.Action == EFPWeaponActionState::Reloading)
		PromotePredictedReload(NetworkState.ReloadId);
	else if (!bPreservePrediction)
	{
		if (bPendingPrediction) EndPredictedReload(LocalReloadPredictionKey,
			bMatchingReload ? NetworkState.EndReason : EFPReloadEndReason::Cancelled);
		ActiveReloadId = NetworkState.Action == EFPWeaponActionState::Reloading ? NetworkState.ReloadId : INDEX_NONE;
	}
	// GAS 结束 RPC 可以先于最后一个属性快照到达；旧 Reloading 不得复活已结束的本地动作。
	if (LocalReloadPredictionKey == 0 && NetworkState.ReloadPredictionKey != 0
		&& NetworkState.ReloadPredictionKey == LastEndedReloadPredictionKey) ActiveReloadId = INDEX_NONE;
	bReloadAmmoCommitted = NetworkState.bCommitted;
	if (!bPreservePrediction && PreviousReloadId >= 0 && Previous == EFPWeaponActionState::Reloading
		&& (NetworkState.Action != Previous || PreviousReloadId != ActiveReloadId))
	{
		// 先释放旧预测归属，再停止播放并通知；回调可立即创建新预测，旧栈不能把新 key 清零。
		LocalReloadPredictionKey = 0;
		const auto Ended = MoveTemp(ReloadPlaybacks);
		for (const FFPActionPlayback& Playback : Ended) Playback.Stop();
		// 若已经跨到新编号，当前快照不能证明上一轮是否提交；只撤销旧表现，不借用新一轮的结算标记。
		const bool bSameReload = PreviousReloadId == NetworkState.ReloadId;
		OnWeaponReloadEnded.Broadcast(PreviousReloadId,
			bSameReload ? NetworkState.EndReason : EFPReloadEndReason::Cancelled, bSameReload && NetworkState.bCommitted);
	}
	// 先结束旧播放，才能启动新编号的播放；复制允许跳过中间 Ready，不能把新播放一起停止。
	RefreshNetworkEquipment();
	if (NetworkState.Action == EFPWeaponActionState::Reloading && ActiveReloadId != INDEX_NONE && IsReloading()
		&& LastPresentedReloadId != ActiveReloadId && IsOperational())
	{
		LastPresentedReloadId = ActiveReloadId;
		OnWeaponReloadPlaybackRequested.Broadcast(ActiveReloadId, NetworkState.bEmptyReload);
	}
	if (NetworkState.Action == EFPWeaponActionState::Disabled)
	{
		EndAbilityFire(true);
		OutstandingShots.Reset();
		ClearRecoilState();
		StopVisualRecoil();
	}
	else if (NetworkState.Action == EFPWeaponActionState::Reloading && bPredictingFire)
	{
		// 仅停本地表现；复制结果绝不反向结束/激活 GAS。
		EndAbilityFire(true);
		OutstandingShots.Reset();
	}
}

// 服务器只广播已完成的射线；客户端处理音效、弹道和本地后坐力，不从结果中再次扣血。
void UfpstrueWeaponComponent::MulticastShot_Implementation(const FHitResult& Hit, int32 ShotIndex,
	uint32 EquipmentGeneration, uint32 Session)
{
	if (GetOwner() == nullptr || GetOwner()->HasAuthority()) return;
	// 【设计取舍】命中特效未做本地预测，必须先接收权威落点；后面的去重只抑制已预测的枪声/后坐力。
	// 不能把整段广播放到预测去重之后，否则拥有者会漏掉合法命中特效。
	const bool bHit = Hit.bBlockingHit;
	OnWeaponTraceFinished.Broadcast(bHit, Hit.TraceStart, Hit.TraceEnd,
		bHit ? FVector(Hit.ImpactPoint) : FVector(Hit.TraceEnd), Hit);
	AfpstrueCharacter* Holder = Character.Get();
	// 松开之后仍保留有界的已播放记录，迟到的服务器表现不能让同一发再响一次、再抖一次。
	if (Holder != nullptr && Holder->IsLocallyControlled() && Session != 0)
	{
		if (FPredictedFirePresentation* Presented = PredictedPresentations.FindByPredicate(
			[EquipmentGeneration, Session](const FPredictedFirePresentation& P) { return P.Generation == EquipmentGeneration && P.Session == Session; }))
		{
			if (ShotIndex <= Presented->PresentedShots)
			{
#if !UE_BUILD_SHIPPING
				if (ShouldLogShotProtocol())
					UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SHOT_SUPPRESSED ShotId=%d Session=%u Generation=%u"), ShotIndex, Session, EquipmentGeneration);
#endif
				return;
			}
			Presented->PresentedShots = ShotIndex;
			if (FireSession == Session && EquipmentRevision == EquipmentGeneration) FirePresentationShot = FMath::Max(FirePresentationShot, ShotIndex);
		}
		else return; // 已淘汰的旧会话也不重新播放拥有者枪声/后坐力。
	}
	OnWeaponFirePerformed.Broadcast();
	if (Holder != nullptr && Holder->IsLocallyControlled() && !Holder->IsDead())
		ApplyRecoil(Cast<APlayerController>(Holder->GetController()));
}

// 只检查执行资源和装备资格，不使用可能迟到的动作快照决定能力互斥。
bool UfpstrueWeaponComponent::CanExecuteShot() const
{
	return CanAcceptOwnerInput() && GetWorld() != nullptr && Character->GetController() != nullptr
		&& Character->GetFirstPersonCameraComponent() != nullptr;
}

bool UfpstrueWeaponComponent::MatchesShotEquipment(const FFPShotRequest& Request) const
{
	return Request.Avatar == Character.Get() && Request.Weapon == GetOwner() && Request.EquipmentGeneration == EquipmentRevision;
}

bool UfpstrueWeaponComponent::IsShotRequestTimeValid(const FFPShotRequest& Request, double ServerNow, double& OutAllowedAge) const
{
	const APlayerState* PlayerState = Character.IsValid() ? Character->GetPlayerState() : nullptr;
	const double OneWayDelay = PlayerState != nullptr ? PlayerState->GetPingInMilliseconds() * 0.0005 : 0.0;
	OutAllowedAge = FMath::Clamp(OneWayDelay * 1.5 + 0.05, 0.10, MaxNetworkShotAgeSeconds);
	return IsShotTimeWithinWindow(Request.ServerFireTime, ServerNow, OutAllowedAge);
}

void UfpstrueWeaponComponent::ReportShotTimeRejection(const FFPShotRequest& Request, const TCHAR* Stage, double Now,
	double AllowedAge, const TCHAR* SpecificReason) const
{
#if !UE_BUILD_SHIPPING
	LogShotTimeRejection(Request, Character.Get(), GetOwner(), Stage, Now, AllowedAge, SpecificReason);
#endif
}

EFPShotResult UfpstrueWeaponComponent::ValidateRenderedViews(const FFPShotRequest& Request, double ServerNow) const
{
	if (Request.RenderedViews.Num() > FFPShotRequest::MaxRenderedViews) return EFPShotResult::InvalidIdentity;
	TArray<const AfpstrueCharacter*, TInlineAllocator<FFPShotRequest::MaxRenderedViews>> Seen;
	for (const FFPShotRenderedView& View : Request.RenderedViews)
	{
		if (View.AgeMilliseconds < FFPShotRequest::MinRenderedViewAgeMilliseconds
			|| View.AgeMilliseconds > FFPShotRequest::MaxRenderedViewAgeMilliseconds) return EFPShotResult::InvalidTime;
		const AfpstrueCharacter* Target = View.Target.Get();
		if (Target == nullptr) continue; // 目标可在可靠尾控制或重发途中离场，不能因此拒绝其它目标的合法输入。
		if (Target == Request.Avatar || Target->GetWorld() != GetWorld() || Seen.Contains(Target)) return EFPShotResult::InvalidIdentity;
		Seen.Add(Target);
		if (Target->IsDead()) continue;
		const double ViewTime = Request.ServerFireTime - static_cast<double>(View.AgeMilliseconds) * .001;
		if (!FMath::IsFinite(ViewTime) || ViewTime > ServerNow + MaxShotFutureSeconds) return EFPShotResult::InvalidTime;
		// 不把过旧的真实显示时间改成较新的fallback；原目标历史查询会因.35s界限而miss。
	}
	return EFPShotResult::Accepted;
}

// 同一 GA 在拥有者先播放表现，在服务器执行原有结算；两端绝不各自生成一份权威伤害。
void UfpstrueWeaponComponent::BeginAbilityFire(int32 PredictionKey)
{
	if (GetOwner()->HasAuthority())
	{
		CancelOrderedShotInputs();
		InputWindowFirstShotId = InputWindowSession = NextOrderedShotId = 0;
		bReleasedInputWindow = false;
		InputClosedServerSeconds = 0.0;
	}
	FirePresentationKey = PredictionKey;
	FirePresentationShot = 0;
	FireSession = 0;
	if (GetOwner()->HasAuthority()) { PublishNetworkState(); return; }
	if (!Character->IsLocallyControlled() || PredictionKey == 0) return;
	if (LocalFireSessionSerial == MAX_uint32) return;
	FireSession = ++LocalFireSessionSerial;
	FirstPredictedShotForCurrentFire = LocalShotSerial + 1;
	if (PredictedPresentations.Num() == 8) PredictedPresentations.RemoveAt(0);
	PredictedPresentations.Add({PredictionKey, EquipmentRevision, FireSession, 0, FirstPredictedShotForCurrentFire});
	bPredictingFire = true;
	// GA 决定何时生成下一发输入；服务器唯一队列 Timer 只执行已经合法准入的请求。
}

// 结束能力只停本地预测表现；不撤销已结算子弹，不清迟到表现的去重记录。
void UfpstrueWeaponComponent::EndAbilityFire(bool bCancelQueuedShots, bool bDiscardPredictedShots)
{
	bPredictingFire = false;
	if (GetOwner() != nullptr && GetOwner()->HasAuthority())
	{
		bReleasedInputWindow = !bCancelQueuedShots;
		InputClosedServerSeconds = GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0;
	}
	if (bCancelQueuedShots) { CancelPendingNetworkShots(); CancelOrderedShotInputs(); }
	// 正常松开/动作替换保留已经发送的逐发事实；只有激活被拒绝才撤销这一轮预测。
	// 装备代际更替在生命周期入口整体清理；任何取消都不回滚服务器已扣的弹药。
	if (bCancelQueuedShots && bDiscardPredictedShots && GetOwner() != nullptr && !GetOwner()->HasAuthority())
	{
		// 新激活预测失败只撤销自己的区间；上一轮已准入输入仍须等待各自的终结 ACK。
		const uint32 First = FirstPredictedShotForCurrentFire;
		const uint32 Last = LocalShotSerial;
		OutstandingShots.RemoveAll([First, Last](uint32 ShotId) { return First != 0 && ShotId >= First && ShotId <= Last; });
		ClientShotInputs.RemoveAll([First, Last](const FClientShotInput& Input)
			{ return First != 0 && Input.Request.ShotId >= First && Input.Request.ShotId <= Last; });
		if (ClientShotInputs.IsEmpty() && GetWorld() != nullptr) GetWorld()->GetTimerManager().ClearTimer(ShotRetryTimerHandle);
		BroadcastAmmoChanged();
	}
	// 不反向取消 GA、不清历史去重；能力结束后的快照由 GA 在标签移除后发布。
}

// 表现序号在广播前提交；回调若取消或卸枪，返回后不再给旧能力安排下一发。
bool UfpstrueWeaponComponent::PresentPredictedShot(const FFPShotRequest& Request)
{
	if (!bPredictingFire || !CanExecuteShot() || Request.FireSession != FireSession || Request.EquipmentGeneration != EquipmentRevision) return false;
	FPredictedFirePresentation* Presented = PredictedPresentations.FindByPredicate(
		[this](const FPredictedFirePresentation& P) { return P.Generation == EquipmentRevision && P.Session == FireSession; });
	if (Presented == nullptr || static_cast<int32>(Request.ShotId) <= Presented->PresentedShots) return false;
	// 显示扣弹和权威扣弹分离；达到本地预测上限后等待确认，不凭表现授权服务器射击。
	const int32 PresentationKey = FirePresentationKey;
	FirePresentationShot = static_cast<int32>(Request.ShotId);
	Presented->PresentedShots = FirePresentationShot;
	LastPredictedShotTime = FPlatformTime::Seconds();
	BroadcastAmmoChanged();
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_AMMO_PREDICT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu BaseAmmo=%d DisplayAmmo=%d Outstanding=%d IncludedShotId=%u"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), CurrentAmmo, GetCurrentAmmo(), OutstandingShots.Num(), LastAmmoIncludedShotId);
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SHOT_PRESENT ShotId=%u Session=%u Generation=%u Count=1 WeaponNetGuid=%llu PlayerId=%d LocalSeconds=%.9f"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(Character.Get()), FPlatformTime::Seconds());
#endif
	OnWeaponFirePerformed.Broadcast();
	if (bPredictingFire && FirePresentationKey == PresentationKey && CanExecuteShot())
		ApplyRecoil(Cast<APlayerController>(Character->GetController()));
	return true;
}

// 客户端只生成输入记录；没有稳定时间样本、装备代际或可用弹药估计时暂缓，不编造过去的射击。
bool UfpstrueWeaponComponent::BuildShotRequest(FFPShotRequest& OutRequest)
{
	if (!bPredictingFire || !CanExecuteShot() || FireSession == 0 || EquipmentRevision == 0
		|| LocalShotSerial >= MAX_int32 || OutstandingShots.Num() >= 32
		|| ClientShotInputs.Num() >= ShotInputWindowSize || GetCurrentAmmo() <= 0) return false;
	auto* Controller = Cast<AfpstruePvpPlayerController>(Character->GetController());
	if (Controller == nullptr || !Controller->HasServerTimeSample() || Controller->GetClockUncertaintySeconds() > 0.30) return false;
	OutRequest.Avatar = Character.Get();
	OutRequest.Weapon = GetOwner();
	OutRequest.EquipmentGeneration = EquipmentRevision;
	OutRequest.FireSession = FireSession;
	OutRequest.ShotId = ++LocalShotSerial;
	OutRequest.ServerFireTime = Controller->GetEstimatedServerTime();
	OutRequest.AimDirection = Controller->GetControlRotation().Vector().GetSafeNormal();
#if !UE_BUILD_SHIPPING
	// 明确政策专项只构造一次非法输入；仍走相同批次/时间窗/裁决，正常和严格体验配置不受影响。
	if (LocalShotSerial == 1 && FParse::Param(FCommandLine::Get(), TEXT("PvpWeakPolicyCorrectness")))
	{
		OutRequest.ServerFireTime -= 1.0;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE POLICY_STALE_INPUT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu OffsetSeconds=1.0"),
			OutRequest.ShotId, OutRequest.FireSession, OutRequest.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()));
	}
#endif
	OutRequest.RenderedViews.Reset();
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It && OutRequest.RenderedViews.Num() < FFPShotRequest::MaxRenderedViews; ++It)
	{
		AfpstrueCharacter* Target = *It;
		if (Target == Character.Get() || Target->IsDead() || Target->GetLocalRole() != ROLE_SimulatedProxy) continue;
		const auto* Movement = Cast<UfpstrueCharacterMovementComponent>(Target->GetCharacterMovement());
		double ViewTime = 0.0;
		if (Movement == nullptr || !Movement->GetRenderedServerTime(ViewTime)) continue;
		const double AgeMilliseconds = (OutRequest.ServerFireTime - ViewTime) * 1000.0;
		if (!FMath::IsFinite(AgeMilliseconds) || AgeMilliseconds < FFPShotRequest::MinRenderedViewAgeMilliseconds
			|| AgeMilliseconds > FFPShotRequest::MaxRenderedViewAgeMilliseconds) continue;
		OutRequest.RenderedViews.Add({Target, static_cast<int16>(FMath::RoundToInt(AgeMilliseconds))});
	}
	// 客户端记住真正提交的报告时间；校时后移只能继续等待，不能捏造一个较新的射击时间。
	LastSubmittedFireTime = OutRequest.ServerFireTime;
	OutstandingShots.Add(OutRequest.ShotId);
#if !UE_BUILD_SHIPPING
	TRACE_BOOKMARK(TEXT("FPShot Build Weapon=%llu Generation=%u Session=%u ShotId=%u"),
		GetWeaponShotNetGuid(GetOwner()), OutRequest.EquipmentGeneration, OutRequest.FireSession, OutRequest.ShotId);
	Controller->RecordSmokeMovingShotInput(OutRequest);
#endif
	return true;
}

void UfpstrueWeaponComponent::SubmitShotInput(const FFPShotRequest& Request, FGameplayAbilitySpecHandle Handle, FPredictionKey Key)
{
	if (GetOwner() == nullptr || GetOwner()->HasAuthority() || ClientShotInputs.Num() >= ShotInputWindowSize) return;
	ClientShotInputs.Add({Request, Handle, Key, FirstPredictedShotForCurrentFire, 0, FPlatformTime::Seconds(), false});
	SendPendingShotInputs();
}

// 同一批探测只影响本地发送安排；服务器仍独立检查年龄、射速、装备和能力授权。
UfpstrueWeaponComponent::FShotRetryTiming UfpstrueWeaponComponent::GetShotRetryTiming() const
{
	FShotRetryTiming Timing;
#if !UE_BUILD_SHIPPING
	// 单变量实验共用发送及400ms收尾，仅把请求间隔固定为40ms。
	Timing.bFixedReference = FParse::Param(FCommandLine::Get(), TEXT("PvpFixedShotRetry"));
#endif
	const auto* Controller = Character.IsValid() ? Cast<AfpstruePvpPlayerController>(Character->GetController()) : nullptr;
	if (Controller != nullptr && Controller->HasRoundTripTimeSample())
	{
		const double RoundTrip = Controller->GetSmoothedRoundTripTimeSeconds();
		const double Variation = Controller->GetRoundTripVariationSeconds();
		if (FMath::IsFinite(RoundTrip) && FMath::IsFinite(Variation) && RoundTrip >= 0.0 && Variation >= 0.0)
		{
			Timing.RoundTripSeconds = RoundTrip;
			Timing.VariationSeconds = Variation;
			if (!Timing.bFixedReference)
				// 等待的是往返确认；用一份RTT加波动预留，仍受实时输入20–60ms预算限制，并非UE传输RTO。
				Timing.IntervalSeconds = FMath::Clamp(RoundTrip + Variation * 2.0, MinShotRetryInterval, MaxShotRetryInterval);
		}
	}
	return Timing;
}

// 单次Timer唤醒最近的发送/过期点；WorldTime早醒只重排，不补跑多次，也不忙轮询。
void UfpstrueWeaponComponent::ScheduleShotInputRetry(double Now, const FShotRetryTiming& Timing)
{
	UWorld* World = GetWorld();
	if (World == nullptr) return;
	double NextWake = TNumericLimits<double>::Max();
	for (const FClientShotInput& Input : ClientShotInputs)
	{
		if (Input.bExpirationSent) continue;
		NextWake = FMath::Min(NextWake, Input.CreatedSeconds + ShotInputExpireSeconds);
		if (Input.Attempts >= MaxShotInputAttempts) continue;
		const double NextSend = Input.Attempts == 0 ? Now : Input.LastSentSeconds + Timing.IntervalSeconds;
		NextWake = FMath::Min(NextWake, NextSend);
	}
	if (NextWake == TNumericLimits<double>::Max()) World->GetTimerManager().ClearTimer(ShotRetryTimerHandle);
	else World->GetTimerManager().SetTimer(ShotRetryTimerHandle, this, &ThisClass::SendPendingShotInputs,
		static_cast<float>(FMath::Max(NextWake - Now, .001)), false);
}

// 最多8发/6次。新输入可顺带尚未终结的旧输入；无新输入时按RTT策略独立补发。
void UfpstrueWeaponComponent::SendPendingShotInputs()
{
	UWorld* World = GetWorld();
	auto* ASC = Character.IsValid() ? Cast<UfpstrueHealthComponent>(Character->GetAbilitySystemComponent()) : nullptr;
	if (World == nullptr || ASC == nullptr || GetOwner() == nullptr || GetOwner()->HasAuthority()) return;
	const double Now = FPlatformTime::Seconds();
	const FShotRetryTiming Timing = GetShotRetryTiming();
	TArray<int32, TInlineAllocator<ShotInputWindowSize>> Grouped;
	for (int32 Index = 0; Index < ClientShotInputs.Num(); ++Index)
	{
		if (Grouped.Contains(Index)) continue;
		const FClientShotInput Header = ClientShotInputs[Index];
		const bool bHasNewInput = ClientShotInputs.ContainsByPredicate([&Header](const FClientShotInput& Input)
		{
			return Input.Handle == Header.Handle && Input.Key == Header.Key && Input.FirstShotId == Header.FirstShotId
				&& Input.Attempts == 0 && !Input.bExpirationSent;
		});
		ShotInputScratch.Reset();
		TArray<FFPShotRequest, TInlineAllocator<ShotInputWindowSize>> Expired;
		for (int32 Peer = Index; Peer < ClientShotInputs.Num(); ++Peer)
		{
			FClientShotInput& Input = ClientShotInputs[Peer];
			if (Input.Handle != Header.Handle || Input.Key != Header.Key || Input.FirstShotId != Header.FirstShotId) continue;
			Grouped.Add(Peer);
			if (Input.Request.ShotId <= LastAmmoIncludedShotId) { Input.bExpirationSent = true; continue; }
			if (Now - Input.CreatedSeconds >= ShotInputExpireSeconds)
			{
				if (!Input.bExpirationSent) { Input.bExpirationSent = true; Expired.Add(Input.Request); }
				continue;
			}
			if (Input.bExpirationSent || Input.Attempts >= MaxShotInputAttempts) continue;
			const bool bFirstSend = Input.Attempts == 0;
			double RetryInterval = Timing.IntervalSeconds;
			const bool bPiggyback = !bFirstSend && bHasNewInput && Now - Input.LastSentSeconds + UE_DOUBLE_SMALL_NUMBER >= MinShotRetryInterval;
			if (bPiggyback) RetryInterval = MinShotRetryInterval;
			if (!bFirstSend && Now - Input.LastSentSeconds + UE_DOUBLE_SMALL_NUMBER < RetryInterval) continue;
			++Input.Attempts;
			Input.LastSentSeconds = Now;
			ShotInputScratch.Add(Input.Request);
#if !UE_BUILD_SHIPPING
			TRACE_BOOKMARK(TEXT("FPShot Submit Weapon=%llu Generation=%u Session=%u ShotId=%u Attempt=%d"),
				GetWeaponShotNetGuid(GetOwner()), Input.Request.EquipmentGeneration, Input.Request.FireSession, Input.Request.ShotId, Input.Attempts);
			if (ShouldLogShotProtocol())
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SHOT_SUBMIT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu Attempt=%d MaxAttempts=%d RetryDelayMs=%.3f LocalSeconds=%.9f Transport=UnreliableBatch Policy=%s RequestedRetryMs=%.3f InputAgeMs=%.3f SRTTms=%.3f RTTVARms=%.3f Piggyback=%d"),
					Input.Request.ShotId, Input.Request.FireSession, Input.Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), Input.Attempts, MaxShotInputAttempts,
					RetryInterval * 1000.0, Now, Timing.bFixedReference ? TEXT("FixedReference") : Timing.RoundTripSeconds >= 0.0 ? TEXT("Adaptive") : TEXT("Fallback"),
					Timing.IntervalSeconds * 1000.0, (Now - Input.CreatedSeconds) * 1000.0,
					Timing.RoundTripSeconds >= 0.0 ? Timing.RoundTripSeconds * 1000.0 : -1.0,
					Timing.VariationSeconds >= 0.0 ? Timing.VariationSeconds * 1000.0 : -1.0, bPiggyback);
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpShotLifecycleTrace")))
			{
				TRACE_BOOKMARK(TEXT("FPShotLifecycle Submit Spec=%u Key=%d First=%u ShotId=%u Attempt=%d Generation=%u Session=%u Weapon=%llu"),
					GetTypeHash(Input.Handle), Input.Key.Current, Input.FirstShotId, Input.Request.ShotId, Input.Attempts,
					Input.Request.EquipmentGeneration, Input.Request.FireSession, GetWeaponShotNetGuid(GetOwner()));
				UE_LOG(LogTemp, Display, TEXT("PVP_SHOT_LIFECYCLE SUBMIT Spec=%u Key=%d First=%u ShotId=%u Attempt=%d Generation=%u Session=%u WeaponNetGuid=%llu LocalSeconds=%.9f"),
					GetTypeHash(Input.Handle), Input.Key.Current, Input.FirstShotId, Input.Request.ShotId, Input.Attempts,
					Input.Request.EquipmentGeneration, Input.Request.FireSession, GetWeaponShotNetGuid(GetOwner()), Now);
			}
#endif
		}
		if (!ShotInputScratch.IsEmpty()) ASC->ServerSubmitShotBatch(Header.Handle, Header.Key, Header.FirstShotId, ShotInputScratch);
		if (!Expired.IsEmpty())
		{
			ShotInputScratch.Reset();
			for (const FFPShotRequest& Request : Expired) ShotInputScratch.Add(Request);
			FFPShotStreamIdentity Stream;
			Stream.Weapon = Header.Request.Weapon;
			Stream.EquipmentGeneration = Header.Request.EquipmentGeneration;
			Stream.FireSession = Header.Request.FireSession;
			Stream.FirstShotId = Header.FirstShotId;
			ASC->ServerFinishShotInputs(Header.Handle, Header.Key, Stream, ShotInputScratch, false, true);
#if !UE_BUILD_SHIPPING
			if (ShouldLogShotProtocol()) UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_INPUT_EXPIRE_CONTROL Count=%d Key=%d"), Expired.Num(), Header.Key.Current);
#endif
		}
	}
	ScheduleShotInputRetry(Now, Timing);
}

void UfpstrueWeaponComponent::FinishShotInputStream(FGameplayAbilitySpecHandle Handle, FPredictionKey Key, bool bCancelled)
{
	auto* ASC = Character.IsValid() ? Cast<UfpstrueHealthComponent>(Character->GetAbilitySystemComponent()) : nullptr;
	if (ASC == nullptr || GetOwner() == nullptr || GetOwner()->HasAuthority()) return;
	// 按GA传入的旧预测键捕获自己的轮次。终结ACK可能已清空尾发，不能因此漏发结束控制。
	const FPredictedFirePresentation* Presentation = PredictedPresentations.FindByPredicate([this, Key](const FPredictedFirePresentation& P)
		{ return P.Key == Key.Current && P.Generation == EquipmentRevision; });
	if (Presentation == nullptr || Presentation->FirstShotId == 0) return;
	FFPShotStreamIdentity Stream;
	Stream.Weapon = GetOwner();
	Stream.EquipmentGeneration = Presentation->Generation;
	Stream.FireSession = Presentation->Session;
	Stream.FirstShotId = Presentation->FirstShotId;
	ShotInputScratch.Reset();
	for (const FClientShotInput& Input : ClientShotInputs)
		if (Input.Handle == Handle && Input.Key == Key && Input.Request.EquipmentGeneration == Stream.EquipmentGeneration
			&& Input.Request.FireSession == Stream.FireSession && Input.FirstShotId == Stream.FirstShotId)
			ShotInputScratch.Add(Input.Request);
	ASC->ServerFinishShotInputs(Handle, Key, Stream, ShotInputScratch, bCancelled, false);
}

// 已退款的连续前缀不再占8发接收窗口；只跳过真实终结记录，不把没收到的输入当作丢失。
uint32 UfpstrueWeaponComponent::SkipFinalizedShotPrefix(uint32 FirstShotId) const
{
	uint32 Next = FirstShotId;
	for (int32 Skipped = 0; Skipped < FinalizedShotInputs.Num() && FinalizedShotInputs.Contains(Next); ++Skipped) ++Next;
	return Next;
}

bool UfpstrueWeaponComponent::ReceiveShotInputBatch(uint32 FirstShotId, const TArray<FFPShotRequest>& Requests,
	bool bReleasedAuthorization, bool bCancelled, bool bExpired)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || Requests.IsEmpty()
		|| Requests.Num() > ShotInputWindowSize || FirstShotId == 0 || FirstShotId > MAX_int32) return false;
	const FFPShotRequest& Header = Requests[0];
	if (!MatchesShotEquipment(Header) || Header.FireSession == 0) return false;
	if (InputWindowFirstShotId == 0)
	{
		if (FirstShotId <= LastReceivedShotId) return false;
		InputWindowFirstShotId = NextOrderedShotId = FirstShotId;
		InputWindowSession = Header.FireSession;
	}
	if (InputWindowFirstShotId != FirstShotId || InputWindowSession != Header.FireSession) return false;
	NextOrderedShotId = SkipFinalizedShotPrefix(NextOrderedShotId);
	bReleasedInputWindow |= bReleasedAuthorization;
	bool bReceived = false;
	for (const FFPShotRequest& Request : Requests)
	{
		if (!MatchesShotEquipment(Request) || Request.FireSession != InputWindowSession || Request.ShotId < NextOrderedShotId
			|| Request.ShotId - NextOrderedShotId >= ShotInputWindowSize || FinalizedShotInputs.Contains(Request.ShotId)) continue;
		if (FOrderedShotInput* Existing = OrderedShotInputs.FindByPredicate([&Request](const FOrderedShotInput& Input) { return Input.Request.ShotId == Request.ShotId; }))
		{
			Existing->bCancelled |= bCancelled;
			Existing->bExpired |= bExpired;
			continue;
		}
		if (OrderedShotInputs.Num() == ShotInputWindowSize) continue;
		OrderedShotInputs.Add({Request, bCancelled, bExpired});
		bReceived = true;
	}
	DrainOrderedShotInputs();
	return bReceived;
}

// 有界重排先补洞再交唯一准入入口；缺失超过75ms显式取消该输入，后来重传也不能再次执行。
void UfpstrueWeaponComponent::DrainOrderedShotInputs()
{
	UWorld* World = GetWorld();
	if (World == nullptr || GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	const uint32 Generation = EquipmentRevision, Session = InputWindowSession;
	OrderedShotInputs.RemoveAll([this](const FOrderedShotInput& Input)
		{ return Input.Request.ShotId < NextOrderedShotId || FinalizedShotInputs.Contains(Input.Request.ShotId); });
	for (int32 Work = 0; Work < ShotInputWindowSize && NextOrderedShotId != 0; ++Work)
	{
		if (FinalizedShotInputs.Contains(NextOrderedShotId)) { ++NextOrderedShotId; continue; }
		const int32 Index = OrderedShotInputs.IndexOfByPredicate([this](const FOrderedShotInput& Input) { return Input.Request.ShotId == NextOrderedShotId; });
		if (Index == INDEX_NONE)
		{
			if (OrderedShotInputs.IsEmpty()) break;
			const double Now = World->GetTimeSeconds();
			if (ShotHoleStartedSeconds < 0.0) ShotHoleStartedSeconds = Now;
			if (Now - ShotHoleStartedSeconds < ShotReorderWaitSeconds) break;
			FFPShotRequest Missing = OrderedShotInputs[0].Request;
			Missing.ShotId = NextOrderedShotId++;
			Missing.ServerFireTime = 0.0; // 未收到输入，没有可声明的客户端时间；此身份只终结，绝不执行。
			LastReceivedShotId = FMath::Max(LastReceivedShotId, Missing.ShotId);
			ShotHoleStartedSeconds = -1.0;
#if !UE_BUILD_SHIPPING
			if (ShouldLogShotProtocol()) UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_INPUT_HOLE_FINAL ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu Result=%d Reason=MissingPayload"),
				Missing.ShotId, Missing.FireSession, Missing.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), static_cast<int32>(EFPShotResult::Cancelled));
#endif
			AcknowledgeShot(Missing, EFPShotResult::Cancelled);
			continue;
		}
		const FOrderedShotInput Input = OrderedShotInputs[Index];
		OrderedShotInputs.RemoveAt(Index, 1, EAllowShrinking::No);
		++NextOrderedShotId;
		ShotHoleStartedSeconds = -1.0;
		if (Input.bCancelled || Input.bExpired)
		{
			LastReceivedShotId = FMath::Max(LastReceivedShotId, Input.Request.ShotId);
			AcknowledgeShot(Input.Request, Input.bExpired ? EFPShotResult::InvalidTime : EFPShotResult::Cancelled);
		}
		else AcceptShotRequest(Input.Request, bReleasedInputWindow);
		if (EquipmentRevision != Generation || InputWindowSession != Session || bEndingPlay) return;
	}
	if (OrderedShotInputs.IsEmpty())
	{
		ShotHoleStartedSeconds = -1.0;
		World->GetTimerManager().ClearTimer(ShotReorderTimerHandle);
	}
	else
		World->GetTimerManager().SetTimer(ShotReorderTimerHandle, this, &ThisClass::DrainOrderedShotInputs,
			FMath::Max(0.001f, ShotReorderWaitSeconds - float(World->GetTimeSeconds() - ShotHoleStartedSeconds)), false);
}

void UfpstrueWeaponComponent::CancelOrderedShotInputs()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(ShotReorderTimerHandle);
	const auto Cancelled = MoveTemp(OrderedShotInputs);
	OrderedShotInputs.Reset();
	ShotHoleStartedSeconds = -1.0;
	for (const FOrderedShotInput& Input : Cancelled)
	{
		LastReceivedShotId = FMath::Max(LastReceivedShotId, Input.Request.ShotId);
		AcknowledgeShot(Input.Request, EFPShotResult::Cancelled);
	}
}

// 只复用拒绝终结与诊断，不增加另一套规则或结算；ACK仍拒绝覆盖已经准入的事实。
EFPShotResult UfpstrueWeaponComponent::RejectShotRequest(const FFPShotRequest& Request, EFPShotResult Result)
{
#if !UE_BUILD_SHIPPING
	TRACE_BOOKMARK(TEXT("FPShot Admission Weapon=%llu Generation=%u Session=%u ShotId=%u Result=%d"),
		GetWeaponShotNetGuid(GetOwner()), Request.EquipmentGeneration, Request.FireSession, Request.ShotId, static_cast<int32>(Result));
	const int32 AmmoBefore = CurrentAmmo, ReserveBefore = ReserveAmmo;
	const uint32 ExecutedBefore = LastExecutedShotId;
#endif
	AcknowledgeShot(Request, Result);
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_REJECT_BOUNDARY ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu AmmoBefore=%d AmmoAfter=%d ReserveBefore=%d ReserveAfter=%d ExecutedBefore=%u ExecutedAfter=%u"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()),
			AmmoBefore, CurrentAmmo, ReserveBefore, ReserveAmmo, ExecutedBefore, LastExecutedShotId);
#endif
	return Result;
}

// 准入：验证新输入并入队；只有 ExecuteNetworkShot 能把它交给 Fire 提交。
// 【设计取舍】准入与执行是两个时点，不是重复结算：有界队列吸收射速节拍与到包时机的差异，
// 等待期间仍可能死亡、换枪或换弹，所以执行前必须复核；只有 Fire 扣弹和施加伤害。
EFPShotResult UfpstrueWeaponComponent::AcceptShotRequest(const FFPShotRequest& Request, bool bReleasedAuthorization)
{
	const UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	const bool bValidReleasedInput = bReleasedAuthorization && GetWorld() != nullptr
		&& GetWorld()->GetTimeSeconds() - InputClosedServerSeconds <= MaxNetworkShotAgeSeconds
		&& Request.ServerFireTime <= InputClosedServerSeconds + MaxShotFutureSeconds;
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || !CanExecuteShot() || ASC == nullptr
		|| ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading) || ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Dead)
		|| (!ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing) && !bValidReleasedInput)) return RejectShotRequest(Request, EFPShotResult::InvalidContext);
	if (!MatchesShotEquipment(Request) || Request.EquipmentGeneration == 0 || Request.FireSession == 0 || Request.ShotId == 0
		|| Request.ShotId > MAX_int32) return RejectShotRequest(Request, EFPShotResult::InvalidIdentity);
	if (Request.ShotId <= LastReceivedShotId)
	{
		// 同一请求仍在 GA 冷却等待时，重复输入不是原请求的终结结果，不能提前释放它的弹药估计。
		if (PendingNetworkShots.ContainsByPredicate([&Request](const FPendingNetworkShot& Pending)
			{ return Pending.Request.ShotId == Request.ShotId && Pending.Request.FireSession == Request.FireSession; })) return EFPShotResult::Queued;
		return RejectShotRequest(Request, EFPShotResult::Duplicate);
	}
	if ((FireSession != 0 && Request.FireSession != FireSession)
		|| (FireSession == 0 && Request.FireSession <= LastRemoteFireSession)) return RejectShotRequest(Request, EFPShotResult::InvalidIdentity);
	// 即使本发随后被拒绝也消费编号；重复包不能等冷却结束后再次提交。
	LastReceivedShotId = Request.ShotId;
	const double Now = GetWorld()->GetTimeSeconds();
	double MaxRequestAge = 0.0;
	if (!IsShotRequestTimeValid(Request, Now, MaxRequestAge))
	{
#if !UE_BUILD_SHIPPING
		ReportShotTimeRejection(Request, TEXT("Admission"), Now, MaxRequestAge);
#endif
		return RejectShotRequest(Request, EFPShotResult::InvalidTime);
	}
	const EFPShotResult ViewValidation = ValidateRenderedViews(Request, Now);
	if (ViewValidation != EFPShotResult::Accepted) return RejectShotRequest(Request, ViewValidation);
	const FVector Aim = Request.AimDirection;
	if (Aim.ContainsNaN() || !Aim.IsNormalized()) return RejectShotRequest(Request, EFPShotResult::InvalidAim);
	const double Interval = 60.0 / Settings.RoundsPerMinute;
	if (LastSubmittedFireTime >= 0.0 && Request.ServerFireTime - LastSubmittedFireTime < Interval * 0.9)
		return RejectShotRequest(Request, EFPShotResult::RateLimited);
	if (FireSession == 0) { FireSession = Request.FireSession; LastRemoteFireSession = FireSession; }
	LastSubmittedFireTime = Request.ServerFireTime;
	if (!HasAmmo()) return RejectShotRequest(Request, EFPShotResult::NoAmmo);
	if (PendingNetworkShots.Num() >= MaxQueuedNetworkShots) return RejectShotRequest(Request, EFPShotResult::Busy);
	const float Delay = GetNextShotDelay(true);
	if (Delay > FMath::Min(Interval + 0.002, 0.15)) return RejectShotRequest(Request, EFPShotResult::RateLimited);
	const bool bMustWait = HasPendingNetworkShots() || Delay > KINDA_SMALL_NUMBER;
	// 只记录服务器当前已授权的瞄准条件，不能让排队期间切换 ADS 改写本发散布。
	PendingNetworkShots.Add({Request, Character->IsAiming(), FPlatformTime::Seconds()});
#if !UE_BUILD_SHIPPING
	TRACE_BOOKMARK(TEXT("FPShot Admission Weapon=%llu Generation=%u Session=%u ShotId=%u Result=%d"),
		GetWeaponShotNetGuid(GetOwner()), Request.EquipmentGeneration, Request.FireSession, Request.ShotId,
		static_cast<int32>(bMustWait ? EFPShotResult::Queued : EFPShotResult::Accepted));
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_ADMISSION ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu ServerTime=%.6f AgeMs=%.3f AllowedAgeMs=%.3f PendingBefore=%d PendingAfter=%d QueueDelayMs=%.3f FrameDeltaMs=%.3f"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), Now,
			(Now - Request.ServerFireTime) * 1000.0, MaxRequestAge * 1000.0, PendingNetworkShots.Num() - 1, PendingNetworkShots.Num(), Delay * 1000.0, GetWorld()->GetDeltaSeconds() * 1000.0);
#endif
	if (bMustWait)
	{
		// 尾部入队不重排现有队头 Timer，避免网络到达反复推迟其截止时间。
		if (PendingNetworkShots.Num() == 1 || !GetWorld()->GetTimerManager().TimerExists(NetworkShotTimerHandle))
			SchedulePendingNetworkShot();
		return EFPShotResult::Queued;
	}
	return ExecuteNetworkShot(Request) ? EFPShotResult::Accepted : LastShotResult;
}

// 执行：队头出队后重新验证，不能假定准入后装备、生命、弹药和冷却仍然有效。
bool UfpstrueWeaponComponent::ExecuteNetworkShot(const FFPShotRequest& Requested)
{
	// 只有队头可以执行；先取出自己的快照，再允许伤害/表现回调强取消余下队列。
	if (PendingNetworkShots.IsEmpty() || PendingNetworkShots[0].Request.ShotId != Requested.ShotId
		|| PendingNetworkShots[0].Request.EquipmentGeneration != Requested.EquipmentGeneration
		|| PendingNetworkShots[0].Request.FireSession != Requested.FireSession)
	{
		AcknowledgeShot(Requested, EFPShotResult::Duplicate);
		return false;
	}
	const FFPShotRequest Request = PendingNetworkShots[0].Request;
	const bool bAimingAtAdmission = PendingNetworkShots[0].bAimingAtAdmission;
#if !UE_BUILD_SHIPPING
	const double AdmissionMonotonicSeconds = PendingNetworkShots[0].AdmissionMonotonicSeconds;
	const int32 PendingBeforeExecution = PendingNetworkShots.Num();
#endif
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(NetworkShotTimerHandle);
	PendingNetworkShots.RemoveAt(0, 1, EAllowShrinking::No);
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_EXECUTION ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu ServerTime=%.6f QueueWaitMs=%.3f PendingBefore=%d PendingAfter=%d FrameDeltaMs=%.3f"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), GetWorld()->GetTimeSeconds(),
			(FPlatformTime::Seconds() - AdmissionMonotonicSeconds) * 1000.0, PendingBeforeExecution, PendingNetworkShots.Num(), GetWorld()->GetDeltaSeconds() * 1000.0);
#endif
	EFPShotResult Result = EFPShotResult::Accepted;
#if !UE_BUILD_SHIPPING
	const int32 AmmoAtExecution = CurrentAmmo, ReserveAtExecution = ReserveAmmo;
	const uint32 ExecutedAtEntry = LastExecutedShotId;
#endif
	const UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || !CanExecuteShot()
		|| ASC == nullptr || ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Dead)
		|| ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading)) Result = EFPShotResult::InvalidContext;
	// 入队时已经持有有效的 GAS 授权；正常松开或下一次按下不能篡改旧会话的执行身份。
	else if (!MatchesShotEquipment(Request))
		Result = EFPShotResult::InvalidIdentity;
	else if (Request.ShotId <= LastExecutedShotId || Request.ShotId > LastReceivedShotId) Result = EFPShotResult::Duplicate;
	else if (!HasAmmo()) Result = EFPShotResult::NoAmmo;
	else if (!IsShotTimeWithinWindow(Request.ServerFireTime, GetWorld()->GetTimeSeconds(), MaxNetworkShotAgeSeconds))
	{
		Result = EFPShotResult::InvalidTime;
#if !UE_BUILD_SHIPPING
		ReportShotTimeRejection(Request, TEXT("Execution"), GetWorld()->GetTimeSeconds(), MaxNetworkShotAgeSeconds);
#endif
	}
	else if (GetNextShotDelay(true) > KINDA_SMALL_NUMBER) Result = EFPShotResult::RateLimited;
	else if (const EFPShotResult ViewValidation = ValidateRenderedViews(Request, GetWorld()->GetTimeSeconds());
		ViewValidation != EFPShotResult::Accepted) Result = ViewValidation;
	else
	{
		FVector Origin;
		if (GetNetMode() != NM_Standalone && !Character->GetHistoricalViewOrigin(Request.ServerFireTime, Origin))
		{
			Result = EFPShotResult::InvalidTime;
#if !UE_BUILD_SHIPPING
			ReportShotTimeRejection(Request, TEXT("Execution"), GetWorld()->GetTimeSeconds(), MaxNetworkShotAgeSeconds, TEXT("MissingHistory"));
#endif
		}
		else
		{
			LastExecutedShotId = Request.ShotId;
#if !UE_BUILD_SHIPPING
			const int32 AmmoBefore = CurrentAmmo;
#endif
			if (!Fire(&Request, bAimingAtAdmission)) Result = EFPShotResult::InvalidContext;
#if !UE_BUILD_SHIPPING
			if (Result == EFPShotResult::Accepted && ShouldLogShotProtocol())
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_COMMIT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu AmmoBefore=%d AmmoAfter=%d"),
					Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), AmmoBefore, CurrentAmmo);
#endif
		}
	}
#if !UE_BUILD_SHIPPING
	TRACE_BOOKMARK(TEXT("FPShot Execute Weapon=%llu Generation=%u Session=%u ShotId=%u Result=%d"),
		GetWeaponShotNetGuid(GetOwner()), Request.EquipmentGeneration, Request.FireSession, Request.ShotId, static_cast<int32>(Result));
#endif
	AcknowledgeShot(Request, Result);
#if !UE_BUILD_SHIPPING
	if (Result != EFPShotResult::Accepted && ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_REJECT_BOUNDARY ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu AmmoBefore=%d AmmoAfter=%d ReserveBefore=%d ReserveAfter=%d ExecutedBefore=%u ExecutedAfter=%u"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()),
			AmmoAtExecution, CurrentAmmo, ReserveAtExecution, ReserveAmmo, ExecutedAtEntry, LastExecutedShotId);
#endif
	SchedulePendingNetworkShot();
	return Result == EFPShotResult::Accepted;
}

bool UfpstrueWeaponComponent::ExecuteNextPendingNetworkShot()
{
	if (PendingNetworkShots.IsEmpty()) return false;
	const FFPShotRequest Request = PendingNetworkShots[0].Request;
	return ExecuteNetworkShot(Request);
}

void UfpstrueWeaponComponent::CancelPendingNetworkShots()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(NetworkShotTimerHandle);
	if (PendingNetworkShots.IsEmpty()) return;
	const auto Cancelled = MoveTemp(PendingNetworkShots);
	PendingNetworkShots.Reset();
	for (const FPendingNetworkShot& Pending : Cancelled) AcknowledgeShot(Pending.Request, EFPShotResult::Cancelled);
}

// 只调度已准入事实，不持有 Firing 标签或输入状态；绝对截止时间不会把 Tick 量化误差逐发累加。
void UfpstrueWeaponComponent::SchedulePendingNetworkShot()
{
	UWorld* World = GetWorld();
	if (World == nullptr || GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	if (!HasPendingNetworkShots())
	{
		World->GetTimerManager().ClearTimer(NetworkShotTimerHandle);
		return;
	}
	// 网络派发早于本帧世界时间推进；普通相对 Timer 此时进入 Pending，会到帧末才建立延时基准。
	// 在下一个 Timer 阶段检查同一绝对截止点，避免额外等帧；回调仍禁止提前/同世界时间连射。
	FTimerManager& Timers = World->GetTimerManager();
	Timers.ClearTimer(NetworkShotTimerHandle);
	NetworkShotTimerHandle = Timers.SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::ProcessPendingNetworkShot));
}

// 一次世界更新最多处理一个队头；过期请求显式拒绝，绝不扩大历史窗口或在本栈补射多发。
void UfpstrueWeaponComponent::ProcessPendingNetworkShot()
{
	if (!HasPendingNetworkShots()) return;
	if (GetNextShotDelay(true) > KINDA_SMALL_NUMBER) { SchedulePendingNetworkShot(); return; }
	const uint32 Generation = EquipmentRevision;
	const TWeakObjectPtr<AfpstrueCharacter> Holder = Character;
	ExecuteNextPendingNetworkShot();
	if (EquipmentRevision != Generation || Character != Holder || !CanExecuteShot()) return;
	// 最后一发已结算后仍走同一个服务器 Reload GA；没有活动 Fire GA 时也不能失去空仓恢复。
	if (!HasAmmo()) RequestReload();
}

void UfpstrueWeaponComponent::AcknowledgeShot(const FFPShotRequest& Request, EFPShotResult Result)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	if (Result == EFPShotResult::Queued || Result == EFPShotResult::Duplicate) return;
	if (MatchesShotEquipment(Request) && Request.ShotId != 0)
	{
		if (FinalizedShotInputs.Contains(Request.ShotId)) return;
		if (FinalizedShotInputs.Num() == 64) FinalizedShotInputs.RemoveAt(0, 1, EAllowShrinking::No);
		FinalizedShotInputs.Add(Request.ShotId);
	}
	// 只有本代际实际消费过的新编号才推进快照；重复 ACK 或别人的旧代际不能覆盖已接受的裁决。
	if (Result != EFPShotResult::Duplicate && MatchesShotEquipment(Request)
		&& Request.ShotId > LastProcessedShotId && Request.ShotId <= LastReceivedShotId)
	{
		LastProcessedShotId = Request.ShotId;
		LastShotResult = Result;
	}
	PublishNetworkState();
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_ACK_SEND ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu ServerTime=%.6f Result=%d"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), GetWorld()->GetTimeSeconds(), static_cast<int32>(Result));
#endif
	// 快速窗口只重复已提交裁决；后续新裁决携带此前3条，避免初包丢失后只能等待可靠重传。
	// 正常松开/新会话不丢失近期结果；装备代际变化会清空，旧身份拒绝不能污染新拥有者窗口。
	if (MatchesShotEquipment(Request))
	{
		if (RecentShotReceipts.Num() == RecentShotReceiptCount) RecentShotReceipts.RemoveAt(0, 1, EAllowShrinking::No);
		FFPShotReceipt& Receipt = RecentShotReceipts.AddDefaulted_GetRef();
		Receipt.EquipmentGeneration = Request.EquipmentGeneration;
		Receipt.Session = Request.FireSession;
		Receipt.ShotId = Request.ShotId;
		Receipt.Result = Result;
		Receipt.Ammo = CurrentAmmo;
		Receipt.Reserve = ReserveAmmo;
		Receipt.StateRevision = NetworkState.Revision;
		Receipt.IncludedShotId = LastExecutedShotId;
		ClientFastShotResult(RecentShotReceipts);
	}
	// 仍只为本次新裁决发送一次可靠副本；孤立尾发没有后续快速包时也能最终结束。
	ClientShotResult(Request.EquipmentGeneration, Request.FireSession, Request.ShotId, Result, CurrentAmmo, ReserveAmmo, NetworkState.Revision, LastExecutedShotId);
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_SHOT_%s ShotId=%u Session=%u Generation=%u Result=%d FireTime=%.6f ServerTime=%.6f Ammo=%d WeaponNetGuid=%llu PlayerId=%d NextDeadline=%.6f Pending=%d"),
			Result == EFPShotResult::Accepted ? TEXT("ACCEPT") : TEXT("REJECT"), Request.ShotId, Request.FireSession,
			Request.EquipmentGeneration, static_cast<int32>(Result), Request.ServerFireTime,
			GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0, CurrentAmmo, GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(Character.Get()),
			NextShotTimeSeconds, PendingNetworkShots.Num());
#endif
}

void UfpstrueWeaponComponent::ClientShotResult_Implementation(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId,
	EFPShotResult Result, int32 Ammo, int32 Reserve, uint32 StateRevision, uint32 IncludedShotId)
{
	ConsumeShotResult(EquipmentGeneration, Session, ShotId, Result, Ammo, Reserve, StateRevision, IncludedShotId, true);
}

void UfpstrueWeaponComponent::ClientFastShotResult_Implementation(const TArray<FFPShotReceipt>& Receipts)
{
	if (Receipts.Num() > RecentShotReceiptCount) return;
	for (const FFPShotReceipt& Receipt : Receipts)
		ConsumeShotResult(Receipt.EquipmentGeneration, Receipt.Session, Receipt.ShotId, Receipt.Result,
			Receipt.Ammo, Receipt.Reserve, Receipt.StateRevision, Receipt.IncludedShotId, false);
}

void UfpstrueWeaponComponent::ConsumeShotResult(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId,
	EFPShotResult Result, int32 Ammo, int32 Reserve, uint32 StateRevision, uint32 IncludedShotId, bool bReliableReceipt)
{
	// 【设计取舍】第一份终结收据结束输入账本；属性快照只能恢复当前状态，不能伪装成逐发确认。
	// Revision 拒绝旧基准，IncludedShotId 避免快照与在途输入重复扣弹；只回滚弹药估计，不反向播放枪声。
	if (GetOwner() == nullptr || GetOwner()->HasAuthority() || EquipmentGeneration != EquipmentRevision
		|| ShotId == 0 || Result == EFPShotResult::Queued || Result == EFPShotResult::Duplicate) return;
	const bool bSeenFastReceipt = FastShotReceipts.ContainsByPredicate([Session, ShotId](const FFastShotReceipt& Receipt)
		{ return Receipt.Session == Session && Receipt.ShotId == ShotId; });
	// 不用弹药快照水位代替收据水位；旧版本但尚未结束的真实输入也必须收口。
	bool bFirstReceipt = !bSeenFastReceipt && (OutstandingShots.Contains(ShotId) || StateRevision > LastReliableShotResultRevision);
	if (bReliableReceipt)
	{
		LastReliableShotResultRevision = FMath::Max(LastReliableShotResultRevision, StateRevision);
		FastShotReceipts.RemoveAll([this](const FFastShotReceipt& Receipt) { return Receipt.Revision <= LastReliableShotResultRevision; });
	}
	else if (bFirstReceipt)
	{
		// 可靠链路长时间阻塞时，新增快速确认退回可靠收口；不为快速副本无限保存去重历史。
		if (FastShotReceipts.Num() == 64) bFirstReceipt = false;
		else FastShotReceipts.Add({Session, ShotId, StateRevision});
	}
	const int32 PreviousDisplayAmmo = GetCurrentAmmo(), PreviousReserve = ReserveAmmo;
	if (bFirstReceipt)
	{
		OutstandingShots.Remove(ShotId);
		ClientShotInputs.RemoveAll([ShotId, EquipmentGeneration, Session](const FClientShotInput& Input)
			{ return Input.Request.ShotId == ShotId && Input.Request.EquipmentGeneration == EquipmentGeneration && Input.Request.FireSession == Session; });
		if (ClientShotInputs.IsEmpty() && GetWorld() != nullptr) GetWorld()->GetTimerManager().ClearTimer(ShotRetryTimerHandle);
	}
	if (StateRevision >= LastAmmoStateRevision)
	{
		LastAmmoStateRevision = StateRevision;
		CurrentAmmo = Ammo;
		ReserveAmmo = Reserve;
		LastAmmoIncludedShotId = FMath::Max(LastAmmoIncludedShotId, IncludedShotId);
	}
	// 重复收据可携带更新基准，但不能再结束、反馈或计量一次相同裁决。
	if (!bFirstReceipt)
	{
		if (PreviousDisplayAmmo != GetCurrentAmmo() || PreviousReserve != ReserveAmmo) BroadcastAmmoChanged();
		return;
	}
#if !UE_BUILD_SHIPPING
	const FPredictedFirePresentation* Presented = PredictedPresentations.FindByPredicate(
		[EquipmentGeneration, Session](const FPredictedFirePresentation& P) { return P.Generation == EquipmentGeneration && P.Session == Session; });
	TRACE_BOOKMARK(TEXT("FPShot Ack Weapon=%llu Generation=%u Session=%u ShotId=%u Result=%d Receipt=%s"),
		GetWeaponShotNetGuid(GetOwner()), EquipmentGeneration, Session, ShotId, static_cast<int32>(Result), bReliableReceipt ? TEXT("Reliable") : TEXT("Fast"));
	int32 PendingUnincludedInputs = 0;
	for (uint32 PendingShotId : OutstandingShots) PendingUnincludedInputs += PendingShotId > LastAmmoIncludedShotId ? 1 : 0;
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_AMMO_RECONCILE ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu Result=%d BaseAmmo=%d DisplayAmmo=%d Outstanding=%d IncludedShotId=%u StateRevision=%u PendingUnincludedInputs=%d RejectedStillPending=%d"),
			ShotId, Session, EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), static_cast<int32>(Result), CurrentAmmo, GetCurrentAmmo(), OutstandingShots.Num(), LastAmmoIncludedShotId, LastAmmoStateRevision,
			PendingUnincludedInputs, OutstandingShots.Contains(ShotId) ? 1 : 0);
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SHOT_ACK ShotId=%u Session=%u Generation=%u Result=%d Ammo=%d Predicted=%d WeaponNetGuid=%llu PlayerId=%d Outstanding=%d StateRevision=%u LocalSeconds=%.9f Receipt=%s"),
			ShotId, Session, EquipmentGeneration, static_cast<int32>(Result), Ammo, Presented != nullptr && static_cast<int32>(ShotId) <= Presented->PresentedShots ? 1 : 0,
			GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(Character.Get()), OutstandingShots.Num(), StateRevision, FPlatformTime::Seconds(), bReliableReceipt ? TEXT("Reliable") : TEXT("Fast"));
	if (auto* Controller = Character.IsValid() ? Cast<AfpstruePvpPlayerController>(Character->GetController()) : nullptr)
		Controller->RecordSmokeShotVerdict(GetOwner(), EquipmentGeneration, Session, ShotId, Result == EFPShotResult::Accepted);
#endif
	// 结账与去重先提交，HUD 的同步回调即使再次递送同一收据也不会重入结账。
	BroadcastAmmoChanged();
	// 已播放的枪声不反向播放“撤销动画”；被拒绝的预测只释放弹药估计，不生成命中反馈。
}

// 与真实事务相同的资格检查供 GA 使用；标签负责动作互斥，弹匣和备弹仍由武器独占。
bool UfpstrueWeaponComponent::HasReloadAmmo() const
{
	return CanAcceptOwnerInput() && GetWorld() != nullptr
		&& GetCurrentAmmo() < Settings.MagazineSize && ReserveAmmo > 0;
}

// ==================== Equipment ====================

// 装备前返回所选配置以供预览，首次装备后返回冻结快照，避免共享资产改动影响当前动作。
const FFPWeaponSettings& UfpstrueWeaponComponent::GetWeaponSettings() const
{
	return !bAmmoInitialized && WeaponConfiguration != nullptr ? WeaponConfiguration->Settings : Settings;
}

// 完整装备入口：校验角色、配置和挂点，挂接成功后登记双方关系，重装不补满弹药。
bool UfpstrueWeaponComponent::AttachWeapon(AfpstrueCharacter* TargetCharacter)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || (TargetCharacter != nullptr && !TargetCharacter->HasAuthority())) return false;
	// 任何前置条件失败都不修改双方状态，挂接成功后才提交关系；重新装备不补满弹药。
	if (bEndingPlay || bDetaching || bDroppedOnDeath || !IsValid(TargetCharacter) || !TargetCharacter->CanMaintainEquipment())
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
	// 同一武器还要在观察端挂全身；原生/联网装备不能只验证第一人称手臂然后让世界枪掉到根节点。
	if ((TargetCharacter->UsesNativePresentation() || GetNetMode() != NM_Standalone)
		&& (TargetCharacter->GetMesh() == nullptr || !TargetCharacter->GetMesh()->DoesSocketExist(RequestedSettings.GripSocketName)))
	{
		UE_LOG(LogTemp, Error, TEXT("AttachWeapon failed: third-person body on %s does not provide socket/bone %s."),
			*GetNameSafe(TargetCharacter), *RequestedSettings.GripSocketName.ToString());
		return false;
	}

	const FAttachmentTransformRules AttachmentRules = FAttachmentTransformRules::SnapToTargetNotIncludingScale;
	if (!AttachToComponent(TargetMesh, AttachmentRules, RequestedSettings.GripSocketName))
	{
		return false;
	}
	// 首次附着与复制恢复使用同一作者偏移；SnapTo 同父级时也必须刷新相对位置，且不修改网格缩放。
	SetRelativeLocationAndRotation(GripLocationOffset, GripRotationOffset.Quaternion(), false, nullptr, ETeleportType::TeleportPhysics);

	// ★【重要边界】[状态提交] 先完成配置校验和挂接，再提交装备关系；重装只复用已有弹药，不重复初始化。
	// 所有外部操作成功后才提交角色引用和运行时状态，失败路径不会留下半装备状态。
	Character = TargetCharacter;
	if (GetNetMode() != NM_Standalone)
	{
		GetOwner()->SetReplicates(true);
		GetOwner()->SetOwner(TargetCharacter);
		GetOwner()->SetInstigator(TargetCharacter);
		GetOwner()->bAlwaysRelevant = true;
	}
	if (!bAmmoInitialized)
	{
		Settings = RequestedSettings;
		CurrentAmmo = Settings.MagazineSize;
		ReserveAmmo = Settings.StartingReserveAmmo;
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity")))
		{
			ReserveAmmo = 10000;
			UE_LOG(LogTemp, Display, TEXT("PVP_CAPACITY LOADOUT Weapon=%s InitialReserve=10000"), *GetNameSafe(GetOwner()));
		}
#endif
		bAmmoInitialized = true;
	}
#if !UE_BUILD_SHIPPING
	// 显式移动命中实验只隔离随机散布；不修改配置资产，也不替代正式参数的正常/弱网回归。
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")) && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit")))
	{
		Settings.HipFireSpreadAngle = Settings.AimFireSpreadAngle = 0.0f;
		Settings.ContinuousFireSpreadStep = Settings.MaxContinuousFireSpreadAngle = 0.0f;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE TEST_CONFIG MovingHitTest=1 Spread=0 WeaponNetGuid=%llu PlayerId=%d"),
			GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(TargetCharacter));
	}
#endif
	bEquipmentEnabled = true;
	++EquipmentRevision;
	// 同一武器换人后，新拥有者从自己的编号开始；旧代际仍由请求中的 Generation 拦截。
	FireSession = 0;
	LastRemoteFireSession = 0;
	FinalizedShotInputs.Reset();
	RecentShotReceipts.Reset();
	LastReceivedShotId = 0;
	LastExecutedShotId = 0;
	CancelPendingNetworkShots();
	LastProcessedShotId = 0;
	LastShotResult = EFPShotResult::Accepted;
	LastSubmittedFireTime = -1.0;
	// 不重置 LastShotTimeSeconds：重新装备不能绕过真实的服务器射速预算。
	TargetCharacter->SetEquippedWeaponComponent(this);
	// 原生武器单机预览也使用同一装配入口；RAR 另按需更新实例材质，旧单机蓝图仍保留原来的表现链。
	if (GetNetMode() != NM_Standalone || Cast<AfpstrueRARWeapon>(GetOwner()) != nullptr || Cast<AfpstrueTemplateWeapon>(GetOwner()) != nullptr) RefreshNetworkEquipment();
	// ★【重要边界】[回调重入] 装备通知会同步执行监听者；返回后必须复核双方关系，不能沿用广播前的成功判断。
	// 装备广播可同步卸下/销毁角色；只有双方关系仍成立才发布弹药或让拾取物消费成功。
	if (!IsOperational() || Character.Get() != TargetCharacter)
	{
		return false;
	}
	PublishNetworkState();
	BroadcastAmmoChanged();

	return IsOperational() && Character.Get() == TargetCharacter;
}

// 幂等卸下武器：先停止本组件动作，再释放双方关系，回调不能重复进入卸下事务。
void UfpstrueWeaponComponent::DetachWeapon()
{
	if (bDetaching) return;
	TGuardValue<bool> DetachingGuard(bDetaching, true);
	AfpstrueCharacter* PreviousCharacter = Character.Get();
	// 掉落物只解除反向登记，不重新禁用、重新挂接或重置已经开始的物理运动。
	if (!bDroppedOnDeath) DisableWeapon();
	Character.Reset();
	PublishNetworkState();
	DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	if (PreviousCharacter != nullptr)
	{
		PreviousCharacter->ReleaseEquippedWeaponComponent(this);
	}
}

// 角色只协调松手与布娃娃；枪独占自身动作取消、碰撞、物理复制和限时回收。
void UfpstrueWeaponComponent::DropForDeath(const FVector& InheritedVelocity)
{
	if (bDroppedOnDeath) { ApplyDeathDropPhysics(); return; }
	AfpstrueCharacter* Holder = Character.Get();
	AActor* WeaponActor = GetOwner();
	if (!IsValid(Holder) || !Holder->IsDead() || WeaponActor == nullptr || bEndingPlay) return;
	// 自己看到的是第一人称枪，世界掉落必须从全身的手部开始，避免从相机附近弹出。
	FTransform DropTransform = GetComponentTransform();
	USkeletalMeshComponent* Body = Holder->GetMesh();
	if (Body != nullptr && Body->DoesSocketExist(Settings.GripSocketName))
		DropTransform = FTransform(GripRotationOffset, GripLocationOffset, GetRelativeScale3D())
			* Body->GetSocketTransform(Settings.GripSocketName);
	const FVector Velocity = InheritedVelocity.ContainsNaN() ? FVector::ZeroVector : InheritedVelocity;
	bDroppedOnDeath = true;
	NetworkState.DropTransform = DropTransform;
	NetworkState.DropVelocity = Velocity;
	// 禁用会先清空待执行射击并发送取消 ACK；必须在清 Owner、失去 RPC 路由之前完成。
	DisableWeapon();
	ApplyDeathDropPhysics(&DropTransform, &Velocity);
	if (WeaponActor->HasAuthority())
	{
		WeaponActor->SetOwner(nullptr);
		WeaponActor->SetReplicateMovement(true);
		WeaponActor->SetLifeSpan(8.0f);
		PublishNetworkState();
	}
}

// 权威 FRepMovement 可比组件状态早到；不重放初始条件，不自行逐帧同步位置。
void UfpstrueWeaponComponent::RefreshNetworkPhysics()
{
	bReceivedAuthoritativePhysics = true;
	const bool bFirstDrop = !bDroppedOnDeath;
	bDroppedOnDeath = true;
	if (bFirstDrop) DisableWeapon();
	ApplyDeathDropPhysics();
}

void UfpstrueWeaponComponent::ApplyDeathDropPhysics(const FTransform* InitialTransform, const FVector* InitialVelocity)
{
	AActor* WeaponActor = GetOwner();
	if (WeaponActor == nullptr) return;
	// 物理复制会持续到达，不能每包重建碰撞过滤或唤醒已经落地休眠的枪。
	if (!bDeathDropPhysicsConfigured)
	{
		// 死亡掉落仅作世界表现，不新增回收弹药/再次拾取规则；先停拾取碰撞再恢复 Actor 碰撞。
		TInlineComponentArray<UfpstruePickUpComponent*> Pickups(WeaponActor);
		for (UfpstruePickUpComponent* Pickup : Pickups)
		{
			Pickup->SetGenerateOverlapEvents(false);
			Pickup->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		if (UAnimInstance* Animation = GetAnimInstance()) Animation->Montage_Stop(0.0f);
		SetOnlyOwnerSee(false);
		SetOwnerNoSee(false);
		SetHiddenInGame(false, true);
		SetVisibility(true, true);
		if (AfpstrueRARWeapon* Rifle = Cast<AfpstrueRARWeapon>(WeaponActor)) Rifle->SetViewmodelFOVEnabled(false);
		WeaponActor->SetActorEnableCollision(true);
		SetCollisionObjectType(ECC_PhysicsBody);
		SetCollisionResponseToAllChannels(ECR_Ignore);
		SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		// 不与 Pawn/尸体相互推挤，避免松手第一帧枪和身体重叠造成爆炸性冲量。
		SetEnableGravity(true);
		bDeathDropPhysicsConfigured = true;
	}
	if (GetAttachParent() != nullptr) DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	if (InitialTransform != nullptr) SetWorldTransform(*InitialTransform, false, nullptr, ETeleportType::TeleportPhysics);
	if (!IsSimulatingPhysics()) SetSimulatePhysics(true);
	if (InitialVelocity != nullptr) SetPhysicsLinearVelocity(*InitialVelocity);
}

// ==================== Equipment Eligibility / Read-only Action View ====================

// 判断武器是否仍与存活角色正确装备且未禁用；生命周期失效与临时输入中断分开处理。
bool UfpstrueWeaponComponent::IsOperational() const
{
	const AfpstrueCharacter* OwningCharacter = Character.Get();
	return !bEndingPlay && IsValid(OwningCharacter) && OwningCharacter->CanMaintainEquipment()
		&& bEquipmentEnabled
		&& OwningCharacter->GetEquippedWeaponComponent() == this;
}

// 查询 GAS 标签及远端展示快照；只供观察，不反向授权业务执行。
bool UfpstrueWeaponComponent::IsFiring() const
{
	const UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	return (ASC != nullptr && ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing))
		|| (GetOwner() != nullptr && !GetOwner()->HasAuthority() && LastPresentedAction == EFPWeaponActionState::Firing);
}

bool UfpstrueWeaponComponent::IsReloading() const
{
	const UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	// 初始/晚加入的持久快照可以没有对应的本地 GA；只能屏蔽已明确结束的那次激活，不能屏蔽所有快照恢复。
	const bool bEndedLocalSnapshot = Character.IsValid() && Character->IsLocallyControlled()
		&& LocalReloadPredictionKey == 0 && NetworkState.ReloadPredictionKey != 0
		&& NetworkState.ReloadPredictionKey == LastEndedReloadPredictionKey;
	return (ASC != nullptr && ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading))
		|| LocalReloadPredictionKey != 0
		|| (!bEndedLocalSnapshot && GetOwner() != nullptr && !GetOwner()->HasAuthority()
			&& LastPresentedAction == EFPWeaponActionState::Reloading);
}

// 在装备资格之外检查角色输入和组件内部迁移保护，拒绝清理或播放变化期间的新动作。
bool UfpstrueWeaponComponent::CanAcceptOwnerInput() const
{
	return IsOperational() && Character->CanAcceptGameplayInput() && !bInterruptingOwnerInput && !bChangingReloadPlayback;
}

// ==================== Fire System ====================

// 一次射击的提交入口：校验射速与装备，先扣弹并命中结算，再报告这一发的事实。
bool UfpstrueWeaponComponent::Fire(const FFPShotRequest* Request, bool bAimingAtAdmission)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || !CanExecuteShot() || !HasAmmo()
		|| GetNextShotDelay(Request != nullptr) > KINDA_SMALL_NUMBER) return false;
	UWorld* const World = GetWorld();
	AfpstrueCharacter* OwningCharacter = Character.Get();
	UCameraComponent* Camera = OwningCharacter->GetFirstPersonCameraComponent();
	const double CurrentTimeSeconds = World->GetTimeSeconds();
	// 实际提交时间服务散布恢复与同帧保护；绝对下一发节拍另存，不能逐发累积 Tick 量化误差。
	if (LastShotTimeSeconds < 0.0 || CurrentTimeSeconds - LastShotTimeSeconds > Settings.SpreadResetDelay)
		ConsecutiveShotCount = 0;
	const double Interval = 60.0 / Settings.RoundsPerMinute;
	// 有持续请求时从理想节拍递增，量化误差不累积；空闲/长卡顿后重置锚点，不积攒多发额度。
	NextShotTimeSeconds = Request == nullptr || NextShotTimeSeconds < 0.0 || CurrentTimeSeconds >= NextShotTimeSeconds + Interval
		? CurrentTimeSeconds + Interval : NextShotTimeSeconds + Interval;
	LastShotTimeSeconds = CurrentTimeSeconds;
	const uint32 ShotEquipment = EquipmentRevision;
	// 先提交扣弹和命中，再通知表现；监听者可以中断后续连射，不能撤销已提交的这一发。
	--CurrentAmmo;
	// 听觉使用当前权威枪口位置，不使用历史回溯位置；预测或视觉重播不产生听觉事件。
	AfpstrueEnemyAIController::ReportAuthoritativeGunshot(OwningCharacter, Camera->GetComponentLocation());
	// ★【表现身份】外部伤害/弹道回调可重启开火；本发广播始终携带提交时的编号，不借用新激活的计数。
	const int32 ShotIndex = Request != nullptr ? static_cast<int32>(Request->ShotId) : FirePresentationShot + 1;
	// 旧会话排空时仍广播它自己的身份，不能把旧 ShotId 和新会话的快照进度混配。
	if (Request == nullptr || (Request->FireSession == FireSession && Request->EquipmentGeneration == EquipmentRevision))
		FirePresentationShot = ShotIndex;
	FireLineTrace(*World, *OwningCharacter, *Camera, ShotIndex, Request,
		Request != nullptr ? bAimingAtAdmission : OwningCharacter->IsAiming());
	if (EquipmentRevision == ShotEquipment && IsOperational())
		ApplyRecoil(Cast<APlayerController>(OwningCharacter->GetController()));
	// 网络逐发在 AcknowledgeShot 发布含裁决的最终快照，避免弹药通知隐式发布半成品。
	if (Request == nullptr) PublishNetworkState();
	BroadcastAmmoChanged();
	// 这是已发生的一发，而不是继续开火的许可；命中/弹药回调中断动作也不能吞掉事实事件。
	OnWeaponFirePerformed.Broadcast();
	return true;
}

// 仅已准入网络队列消化 Tick 量化误差；原单机/本地主机的 GA 冷却与回调重入仍按实际提交时刻计算。
float UfpstrueWeaponComponent::GetNextShotDelay(bool bAdmittedNetworkSchedule) const
{
	if (GetWorld() == nullptr) return 0.0f;
	const double Now = GetWorld()->GetTimeSeconds();
	if (GetOwner() != nullptr && GetOwner()->HasAuthority())
	{
		if (!bAdmittedNetworkSchedule)
			return LastShotTimeSeconds < 0.0 ? 0.0f
				: static_cast<float>(FMath::Max(0.0, LastShotTimeSeconds + 60.0 / Settings.RoundsPerMinute - Now));
		const double Delay = NextShotTimeSeconds < 0.0 ? 0.0 : FMath::Max(0.0, NextShotTimeSeconds - Now);
		// Timer 或同步回调不能在同一世界时间内再消费第二发；下一 Tick 才能再检查过期节拍。
		return static_cast<float>(LastShotTimeSeconds >= 0.0 && Now <= LastShotTimeSeconds ? FMath::Max(Delay, 0.001) : Delay);
	}
	// WorldTime按帧起点推进，重负载中同一帧后段的出手可与下一帧压到很短真实间隔。
	// 拥有者用实际单调出手间隔，再约束报告给服务器的时间间隔；原Task早醒只能再等。
	const double Interval = 60.0 / Settings.RoundsPerMinute;
	double Delay = LastPredictedShotTime < 0.0 ? 0.0 : FMath::Max(0.0, LastPredictedShotTime + Interval - FPlatformTime::Seconds());
	const auto* Controller = Character.IsValid() ? Cast<AfpstruePvpPlayerController>(Character->GetController()) : nullptr;
	if (LastSubmittedFireTime >= 0.0 && Controller != nullptr && Controller->HasServerTimeSample())
		Delay = FMath::Max(Delay, LastSubmittedFireTime + Interval - Controller->GetEstimatedServerTime());
	return static_cast<float>(Delay);
}

double UfpstrueWeaponComponent::ComputeTargetTraceTime(double FireTime, double ServerNow, double ObservedRTTMilliseconds,
	double InterpolationSeconds, bool bEnableLatencyCompensation)
{
	const double OneWay = bEnableLatencyCompensation && FMath::IsFinite(ObservedRTTMilliseconds) && ObservedRTTMilliseconds > 0.0
		? ObservedRTTMilliseconds * .0005 : 0.0;
	const double Interpolation = FMath::IsFinite(InterpolationSeconds) ? FMath::Clamp(InterpolationSeconds, 0.0, .10) : 0.0;
	return FMath::Clamp(FireTime - OneWay - Interpolation,
		ServerNow - static_cast<double>(AfpstrueCharacter::MaxHitHistorySeconds), FMath::Min(FireTime, ServerNow));
}

// 按本发授权的瞄准条件和连射散布执行一条 Hitscan，结算部位伤害及物理冲量，再报告弹道结果。
void UfpstrueWeaponComponent::FireLineTrace(UWorld& World, AfpstrueCharacter& OwningCharacter, UCameraComponent& Camera,
	int32 ShotIndex, const FFPShotRequest* Request, bool bAimingForShot)
{
	// 根据瞄准状态和连续射击次数计算散布，并在同一入口完成单射线查询。
	// Fire 已校验本发的角色/相机；在外部伤害或表现回调之前直接使用这些依赖，不重复解析装备关系。
	// 配置在首次装备时已校验并冻结，不逐发修正同一字段；运行时累计值仍需限制。
	const float ContinuousSpreadAngle = FMath::Min(ConsecutiveShotCount * Settings.ContinuousFireSpreadStep, Settings.MaxContinuousFireSpreadAngle);
	// 基础与连射配置各自合法不代表总和合法；总角度统一有限且不超过 45 度，避免 tan 接近 90 度。
	const float TotalSpreadAngle = (bAimingForShot ? Settings.AimFireSpreadAngle : Settings.HipFireSpreadAngle)
		+ ContinuousSpreadAngle;
	const float SpreadAngle = FMath::IsFinite(TotalSpreadAngle) ? FMath::Clamp(TotalSpreadAngle, 0.0f, MaxTotalSpreadAngle) : 0.0f;
	++ConsecutiveShotCount;

	// 单机从相机查询；联网使用服务器历史视点和本发瞄准方向，返回最近阻挡并按骨骼结算点伤害。
	FVector Start = Camera.GetComponentLocation();
	FRotator ViewRotation = Camera.GetComponentRotation();
	if (GetNetMode() != NM_Standalone) OwningCharacter.GetActorEyesViewPoint(Start, ViewRotation);
	if (Request != nullptr && GetNetMode() != NM_Standalone) OwningCharacter.GetHistoricalViewOrigin(Request->ServerFireTime, Start);
	const FVector Forward = Request != nullptr ? FVector(Request->AimDirection) : ViewRotation.Vector();
	const FVector ShotDirection = SpreadAngle > 0.0f ? MakeGaussianSpreadDirection(Forward, SpreadAngle) : Forward;
	const FVector End = Start + ShotDirection * Settings.LineTraceRange;

	FHitResult HitResult;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(&OwningCharacter);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bTraceComplex = true;

	// WeaponTrace 与相机可见性解耦：透明表现、交互射线和玩家子弹可以分别配置响应。
	// 输入时间只表示开火时刻；射手当时看到的目标还落后于服务器，须计入服务器观测下行估计与显示缓冲。
	// RTT/2 只适用于对称路径近似，不接纳客户端自报回溯量；历史不足时截断，绝不扩大开火准入/执行窗。
	const AfpstruePvpGameMode* Mode = World.GetAuthGameMode<AfpstruePvpGameMode>();
	const double TargetInterpolationSeconds = GetNetMode() != NM_Standalone
		? (Mode != nullptr ? Mode->GetTargetInterpolationSeconds() : DefaultTargetInterpolationSeconds) : 0.0;
	const APlayerState* ShooterState = OwningCharacter.GetPlayerState();
	const double ObservedRTT = GetNetMode() != NM_Standalone && ShooterState != nullptr ? ShooterState->GetPingInMilliseconds() : 0.0;
	const bool bTargetLatencyCompensation = GetNetMode() != NM_Standalone && (Mode == nullptr || Mode->IsTargetLatencyCompensationEnabled());
	const double TargetTime = Request != nullptr ? ComputeTargetTraceTime(Request->ServerFireTime, World.GetTimeSeconds(),
		ObservedRTT, TargetInterpolationSeconds, bTargetLatencyCompensation) : World.GetTimeSeconds();
	const bool bHit = Request != nullptr
		? AfpstrueCharacter::TraceHistoricalHit(&World, &OwningCharacter, TargetTime, Start, End, HitResult, Request)
		: World.LineTraceSingleByChannel(HitResult, Start, End, FpstrueCollisionChannels::WeaponTrace, QueryParams);
	// 【设计取舍】复用 FHitResult 原生量化与序列化，不重复发送其已有端点，也不另造命中表现协议。
	// 命中、未命中和历史形状共用同一份端点；在伤害回调前固定，避免回调移动射手后串入新位置。
	HitResult.TraceStart = Start;
	HitResult.TraceEnd = End;
	// 纯数学历史查询已经结束，世界未被移动；先固定表现身份，随后才允许伤害/蓝图同步重入。
	const uint32 ShotGeneration = Request != nullptr ? Request->EquipmentGeneration : EquipmentRevision;
	const uint32 ShotSession = Request != nullptr ? Request->FireSession : FireSession;
	// 日志、部位伤害与表现使用同一命中身份；玩家历史形状已经带BoneName，不走敌人代理映射。
	AActor* HitActor = HitResult.GetActor();
	if (bHit && IsValid(HitActor))
		if (const AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(HitActor)) Enemy->ResolveHitProxyBone(HitResult);
#if !UE_BUILD_SHIPPING
	bool bNativeTargetTime = false;
	const double ActualTargetTime = Request != nullptr
		? Request->GetTargetViewTime(Cast<AfpstrueCharacter>(HitActor), TargetTime, &bNativeTargetTime) : TargetTime;
	if (Request != nullptr && ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE REWIND ShotId=%u FireTime=%.6f TargetTime=%.6f Hit=%s Bone=%s WeaponNetGuid=%llu PlayerId=%d Generation=%u Session=%u TargetNetGuid=%llu TimeSource=%s TargetLatencyCompensation=%d ObservedRTTms=%.3f OneWayEstimateMs=%.3f HistoryClamped=%d"),
			Request->ShotId, Request->ServerFireTime, ActualTargetTime, *GetNameSafe(HitResult.GetActor()), *HitResult.BoneName.ToString(),
			GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(&OwningCharacter), Request->EquipmentGeneration, Request->FireSession,
			GetWeaponShotNetGuid(HitResult.GetActor()), bNativeTargetTime ? TEXT("NativeView") : TEXT("Fallback"), bTargetLatencyCompensation, ObservedRTT,
			bTargetLatencyCompensation && FMath::IsFinite(ObservedRTT) && ObservedRTT > 0.0 ? ObservedRTT * 0.5 : 0.0,
			!bNativeTargetTime && TargetTime <= World.GetTimeSeconds() - AfpstrueCharacter::MaxHitHistorySeconds + UE_DOUBLE_SMALL_NUMBER ? 1 : 0);
#endif

	// 只有查询真正命中后才读取目标、骨骼和物理组件；未命中只广播弹道终点供表现层使用。
	if (bHit && IsValid(HitActor))
	{
		const bool bCriticalHit = Settings.CriticalHitBones.Contains(HitResult.BoneName);
		const float DamageToApply = bCriticalHit ? Settings.LineTraceHeadDamage : Settings.LineTraceDamage;
#if !UE_BUILD_SHIPPING
		const TWeakObjectPtr<AfpstrueCharacter> MovingTarget = Cast<AfpstrueCharacter>(HitActor);
		const float TargetSpeed = MovingTarget.IsValid() ? MovingTarget->GetVelocity().Size2D() : 0.0f;
		const float HealthBefore = MovingTarget.IsValid() ? MovingTarget->GetCurrentHealth() : 0.0f;
#endif
		UGameplayStatics::ApplyPointDamage(HitActor, DamageToApply, ShotDirection, HitResult, OwningCharacter.GetController(), GetOwner(),
									   nullptr);
#if !UE_BUILD_SHIPPING
		if (Request != nullptr && MovingTarget.IsValid() && TargetSpeed >= 20.0f && MovingTarget->GetCurrentHealth() < HealthBefore
			&& FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")) && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit")))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_SERVER PASS ShotId=%u TargetSpeed=%.2f HealthBefore=%.2f HealthAfter=%.2f WeaponNetGuid=%llu PlayerId=%d Generation=%u Session=%u TargetNetGuid=%llu"),
				Request->ShotId, TargetSpeed, HealthBefore, MovingTarget->GetCurrentHealth(), GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(&OwningCharacter),
				Request->EquipmentGeneration, Request->FireSession, GetWeaponShotNetGuid(MovingTarget.Get()));
#endif

		// 受伤事件可能销毁目标；仅对仍有效的非角色物理组件追加冲量。
		UPrimitiveComponent* HitComponent = HitResult.GetComponent();
		if (IsValid(HitActor) && !HitActor->IsA<ACharacter>() && IsValid(HitComponent) && HitComponent->IsSimulatingPhysics())
		{
			HitComponent->AddImpulseAtLocation(ShotDirection * Settings.LineTraceImpulse, HitResult.ImpactPoint);
		}
	}
	const FVector TraceTarget = bHit ? HitResult.ImpactPoint : End;
	OnWeaponTraceFinished.Broadcast(bHit, Start, End, TraceTarget, HitResult);
	if (GetNetMode() != NM_Standalone) MulticastShot(HitResult, ShotIndex, ShotGeneration, ShotSession);
#if !UE_BUILD_SHIPPING
	if (bHit && HitActor != nullptr && HitActor->IsA<AfpstrueCharacter>() && ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Hit Shooter=%s Target=%s"), *OwningCharacter.GetName(), *HitActor->GetName());
#endif
}

// ==================== Reload System ====================

/*
 * Reload GA 持有 Reloading 标签；拥有者可先预测播放，服务器建立正 ReloadId 与唯一装弹时钟。
 * 客户端预测键映射到权威事务后接管同一播放实例；动画结束、Notify 和本地时长都不能装弹。
 * 正常完成由权威 Timer 提交一次；取消不补弹，也不回滚已经提交的服务器弹药。
 *
 * Delegate 在当前调用栈同步执行，监听者可能取消换弹、死亡或开始下一次换弹。
 * 因此开始事件之前必须先设置事务和 Timer；Finish 在提交事件返回后还要确认 ReloadId 未变。
 */

// 将手动或空仓换弹请求交给同一个 Reload GA，不直接创建事务或装弹。
bool UfpstrueWeaponComponent::RequestReload()
{
	// 手动、空仓和原蓝图接口都激活同一个 Reload GA，不能旁路能力互斥另开一次事务。
	if (GetOwner() == nullptr) return false;
	UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	if (ASC == nullptr) return false;
	if (GetOwner()->HasAuthority() && !Character->IsLocallyControlled())
	{
		// LocalPredicted 的普通 TryActivate 会先绕客户端一圈；空仓由服务器直接进入同一个 GAS 激活入口。
		// 原生 server-origin key 会通知拥有者建立能力实例，不伪造客户端的预测接受。
		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(UfpstrueReloadAbility::StaticClass());
		return Spec != nullptr && ASC->InternalTryActivateAbility(Spec->Handle, FPredictionKey::CreateNewServerInitiatedKey(ASC));
	}
	return ASC->TryActivateAbilityByClass(UfpstrueReloadAbility::StaticClass());
}

// 仅由 Reload GA 调用的服务器执行器，保留原动画身份、提交去重与唯一兜底计时。
// GA 在调用此执行器前已用标签取消 Aim/Sprint，这里不再复制互斥规则。
bool UfpstrueWeaponComponent::BeginAbilityReload(int32& OutReloadId, int32 PredictionKey)
{
	OutReloadId = INDEX_NONE;
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return false;
	// 所有运行模式共用权威计时；动画只观察事务，不能决定装弹成功或失败。
	// 换弹必须同时满足：武器可用、当前不在换弹、弹匣未满且仍有备弹。
	if (!CanAcceptOwnerInput() || ActiveReloadId != INDEX_NONE
		|| GetWorld() == nullptr || CurrentAmmo >= Settings.MagazineSize || ReserveAmmo <= 0)
	{
		return false;
	}
	// 普通松开后的旧输入可能仍在排队；换弹是强打断，即使旧 Fire GA 已结束也必须清空它们。
	CancelPendingNetworkShots();

	const bool bWasEmptyReload = CurrentAmmo <= 0;
	// ★【重要边界】[状态提交] GA 已建立动作锁并取消冲突能力；先建立事务与计时，再触发蓝图播放。
	ReloadSerial = ReloadSerial == MAX_int32 ? 1 : ReloadSerial + 1;
	ActiveReloadId = ReloadSerial;
	const int32 RequestedReloadId = ActiveReloadId;
	OutReloadId = RequestedReloadId;
	bReloadAmmoCommitted = false;
	ReloadPlaybacks.Reset();
	// 先建立本次计时，再广播；监听者同步 Finish/Cancel 时才能一并清除正确的 Timer。
	const float SelectedReloadDuration = bWasEmptyReload ? Settings.EmptyReloadDuration : Settings.ReloadDuration;
	NetworkState.ReloadStartServerTime = GetWorld()->GetTimeSeconds();
	NetworkState.ReloadDuration = SelectedReloadDuration;
	NetworkState.bEmptyReload = bWasEmptyReload;
	NetworkState.ReloadPredictionKey = PredictionKey;
	// 唯一完成入口捕获事务编号，旧计时不能结束下一次换弹。专服无动画也按同一规则完成。
	bNetworkEmptyReload = bWasEmptyReload;
	GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
		FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::FinishReloadForTransaction, RequestedReloadId),
		SelectedReloadDuration, false);
	PublishNetworkState();
	// 表现接收者自行跳过专服/无素材；播放失败不能取消已经接纳的权威事务。
	OnWeaponReloadPlaybackRequested.Broadcast(RequestedReloadId, bWasEmptyReload);

	// true 表示请求曾被接纳；监听者可能已同步结束换弹，广播之后不再写回状态或重设计时器。
	return true;
}

	// 预测仅建立已有播放合同；客户端没有完成 Timer，也不能调用权威 Commit 装弹。
bool UfpstrueWeaponComponent::BeginPredictedReload(int32 PredictionKey)
{
	if (GetOwner() == nullptr || GetOwner()->HasAuthority() || PredictionKey == 0 || !Character.IsValid()
		|| !Character->IsLocallyControlled()) return false;
	if (PredictionKey < 0)
	{
		// 原生服务器激活已获准；只观察快照，不使用可能过期的本地弹药再否决服务器。
		LocalReloadPredictionKey = PredictionKey;
		RefreshNetworkEquipment();
		return true;
	}
	if (!HasReloadAmmo() || ActiveReloadId != INDEX_NONE) return false;
	LocalReloadPredictionKey = PredictionKey;
	LastEndedReloadPredictionKey = 0;
	ActiveReloadId = -PredictionKey - 1;
	LastPresentedReloadId = ActiveReloadId;
	bPredictedEmptyReload = GetCurrentAmmo() <= 0;
	PredictedReloadDuration = bPredictedEmptyReload ? Settings.EmptyReloadDuration : Settings.ReloadDuration;
	PredictedReloadStartSeconds = GetWorld()->GetTimeSeconds();
	bReloadAmmoCommitted = false;
	const int32 Id = ActiveReloadId;
	OnWeaponReloadPlaybackRequested.Broadcast(Id, bPredictedEmptyReload);
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_RELOAD_PREDICT Key=%d ReloadId=%d Generation=%u WeaponNetGuid=%llu Ammo=%d Reserve=%d LocalSeconds=%.9f"),
			PredictionKey, Id, EquipmentRevision, GetWeaponShotNetGuid(GetOwner()), GetCurrentAmmo(), ReserveAmmo, FPlatformTime::Seconds());
#endif
	return LocalReloadPredictionKey == PredictionKey && ActiveReloadId == Id;
}

void UfpstrueWeaponComponent::PromotePredictedReload(int32 ReloadId)
{
	const int32 OldId = ActiveReloadId;
	ActiveReloadId = ReloadId;
	LastPresentedReloadId = ReloadId;
	// 确认接管同一 MontageInstance，不再发播放事件；只重绑编号并对齐服务器阶段。
	for (FFPActionPlayback& Playback : ReloadPlaybacks)
	{
		Playback.ActionId = static_cast<uint32>(ReloadId);
		if (FAnimMontageInstance* Instance = Playback.GetBoundInstance())
		{
			Instance->OnMontageEnded.BindUObject(this, &ThisClass::HandleReloadPlaybackEnded,
				ReloadId, Playback.Mesh, Playback.MontageInstanceId);
			if (UAnimMontage* Montage = Playback.Montage.Get())
			{
				// 拥有者已提前播放，确认不能倒带造成视觉跳变；用剩余播放速率对齐权威截止点。
				const float RemainingSeconds = NetworkState.ReloadDuration * (1.0f - GetReloadProgress());
				if (RemainingSeconds > KINDA_SMALL_NUMBER)
					Instance->SetPlayRate(FMath::Max(KINDA_SMALL_NUMBER, Montage->GetPlayLength() - Instance->GetPosition()) / RemainingSeconds);
			}
		}
	}
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_RELOAD_PROMOTE Key=%d PredictedId=%d ReloadId=%d Generation=%u WeaponNetGuid=%llu Replayed=0 BoundMeshes=%d LocalSeconds=%.9f"),
			LocalReloadPredictionKey, OldId, ReloadId, EquipmentRevision, GetWeaponShotNetGuid(GetOwner()), ReloadPlaybacks.Num(), FPlatformTime::Seconds());
#endif
}

void UfpstrueWeaponComponent::EndPredictedReload(int32 PredictionKey, EFPReloadEndReason Reason)
{
	if (GetOwner() == nullptr || GetOwner()->HasAuthority() || PredictionKey == 0 || LocalReloadPredictionKey != PredictionKey) return;
	const int32 EndedId = ActiveReloadId;
	LastEndedReloadPredictionKey = PredictionKey;
	LocalReloadPredictionKey = 0;
	ActiveReloadId = INDEX_NONE;
	const auto Ended = MoveTemp(ReloadPlaybacks);
	ReloadPlaybacks.Reset();
	{
		TGuardValue<bool> PlaybackGuard(bChangingReloadPlayback, true);
		for (const FFPActionPlayback& Playback : Ended) Playback.Stop();
	}
	OnWeaponReloadEnded.Broadcast(EndedId, Reason, false);
#if !UE_BUILD_SHIPPING
	if (ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_RELOAD_END Key=%d ReloadId=%d Generation=%u WeaponNetGuid=%llu Reason=%d Ammo=%d Reserve=%d LocalSeconds=%.9f"),
			PredictionKey, EndedId, EquipmentRevision, GetWeaponShotNetGuid(GetOwner()), static_cast<int32>(Reason), GetCurrentAmmo(), ReserveAmmo, FPlatformTime::Seconds());
#endif
}

float UfpstrueWeaponComponent::GetReloadDuration() const
{
	return LocalReloadPredictionKey > 0 && ActiveReloadId == -LocalReloadPredictionKey - 1
		? PredictedReloadDuration : NetworkState.ReloadDuration;
}

bool UfpstrueWeaponComponent::IsEmptyReloadPresentation() const
{
	return LocalReloadPredictionKey > 0 && ActiveReloadId == -LocalReloadPredictionKey - 1
		? bPredictedEmptyReload : NetworkState.bEmptyReload;
}

// 核对活跃换弹事务编号；装备资格在具体播放或提交入口另行检查。
bool UfpstrueWeaponComponent::IsCurrentReload(int32 ReloadId) const
{
	return ReloadId != INDEX_NONE && ActiveReloadId == ReloadId;
}

// 同一个服务器时钟覆盖拥有者、观察者及重连；没有活动事务时不复活旧动画。
float UfpstrueWeaponComponent::GetReloadProgress() const
{
	const UWorld* World = GetWorld();
	if (LocalReloadPredictionKey > 0 && ActiveReloadId == -LocalReloadPredictionKey - 1 && World != nullptr)
		return static_cast<float>(FMath::Clamp((World->GetTimeSeconds() - PredictedReloadStartSeconds) / PredictedReloadDuration, 0.0, 1.0));
	if (ActiveReloadId == INDEX_NONE || World == nullptr || NetworkState.ReloadDuration <= 0.0f) return 0.0f;
	const AGameStateBase* State = World->GetGameState();
	const double ServerNow = GetNetMode() == NM_Client
		? (State != nullptr ? State->GetServerWorldTimeSeconds() : NetworkState.ReloadStartServerTime)
		: World->GetTimeSeconds();
	return static_cast<float>(FMath::Clamp((ServerNow - NetworkState.ReloadStartServerTime) / NetworkState.ReloadDuration, 0.0, 1.0));
}

// 显式播放并绑定本次换弹的表现；播放失败不改变权威时钟和弹药。
bool UfpstrueWeaponComponent::PlayReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage, float PlayRate)
{
	if (!IsCurrentReload(ReloadId) || !IsOperational() || bChangingReloadPlayback) return false;
	// 同一 Mesh 一次换弹只拥有一段播放，不能让无关 Montage 覆盖真实装填身份。
	if (ReloadPlaybacks.ContainsByPredicate([PlaybackMesh](const FFPActionPlayback& P) { return P.Mesh.Get() == PlaybackMesh; })) return false;
	TGuardValue<bool> PlaybackGuard(bChangingReloadPlayback, true);
	UAnimInstance* Anim = IsValid(PlaybackMesh) ? PlaybackMesh->GetAnimInstance() : nullptr;
	if (Anim == nullptr || Montage == nullptr || !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f
		|| (PlaybackMesh != this && PlaybackMesh != Character->GetMesh1P() && PlaybackMesh != Character->GetMesh())
		|| PlaybackMesh->GetSkeletalMeshAsset() == nullptr
		|| Montage->GetSkeleton() != PlaybackMesh->GetSkeletalMeshAsset()->GetSkeleton())
	{
		return false;
	}
	// 保留原蓝图的同组互斥：UE 的 true 停同一 Montage Group，不停其他组的表现。
	// ★【重要边界】[回调重入] 播放会打断旧 Montage 并触发回调；随后绑定再次校验本次事务，不仅判断播放返回值。
	// 联网中途收到同一事务时追到当前阶段；只定位表现，不重新启动服务器计时或提交弹药。
	const float StartPosition = GetReloadProgress() * Montage->GetPlayLength();
	const float Duration = Anim->Montage_Play(Montage, PlayRate, EMontagePlayReturnType::Duration, StartPosition, true);
	if (Duration <= 0.0f || !BindReloadMontage(ReloadId, PlaybackMesh, Montage))
	{
		// 成功路径只绑定一次；仅在播放成功但事务已失效时回收本次实例，不停止播放失败前的旧实例。
		FFPActionPlayback UnboundPlayback;
		if (Duration > 0.0f && UnboundPlayback.TryBind(ReloadId, PlaybackMesh, Montage)) UnboundPlayback.Stop();
		return false;
	}
	return true;
}

// 登记明确的活动播放实例并接管结束委托；同 Mesh 已有不同播放时拒绝覆盖。
// 【设计取舍】ReloadId 区分装填事务，Montage 实例 ID 区分同一素材的重播；只比较资源指针不够。
// 明确绑定比扫描“当前动画”多保存一份身份，但旧 Notify/结束回调因此不能误提交新一轮装填。
bool UfpstrueWeaponComponent::BindReloadMontage(int32 ReloadId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage)
{
	if (!IsCurrentReload(ReloadId) || !IsOperational() || !IsValid(PlaybackMesh)
		|| (PlaybackMesh != this && PlaybackMesh != Character->GetMesh1P() && PlaybackMesh != Character->GetMesh())) return false;
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

// 只释放已经结束的表现身份；正常结束或动画中断均不结算权威换弹。
void UfpstrueWeaponComponent::HandleReloadPlaybackEnded(UAnimMontage*, bool bInterrupted,
	int32 ReloadId, TWeakObjectPtr<USkeletalMeshComponent> PlaybackMesh, int32 InstanceId)
{
	if (!IsCurrentReload(ReloadId)) return;
	ReloadPlaybacks.RemoveAll([PlaybackMesh, InstanceId](const FFPActionPlayback& P)
	{
		return P.Mesh == PlaybackMesh && P.MontageInstanceId == InstanceId;
	});
}


// 按弹匣缺口与备弹量转移一次弹药；先标记已提交再通知 HUD，防止回调重复装弹。
bool UfpstrueWeaponComponent::CommitReloadForTransaction(int32 ReloadId)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return false;
	// 当前事务、装备资格和单次提交标记共同保护装弹；重复通知不能再次搬运备弹。
	if (!IsCurrentReload(ReloadId) || bReloadAmmoCommitted || !IsOperational())
	{
		return false;
	}

	const int32 AmmoNeeded = Settings.MagazineSize - CurrentAmmo;
	const int32 AmmoToLoad = FMath::Min(AmmoNeeded, ReserveAmmo);
	CurrentAmmo += AmmoToLoad;
	ReserveAmmo -= AmmoToLoad;
	// 先标记已提交再通知 HUD，防止监听者重入同一事务导致重复装弹。
	bReloadAmmoCommitted = true;
#if !UE_BUILD_SHIPPING
	if (GetNetMode() != NM_Standalone && ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Reload Ammo=%d Reserve=%d"), CurrentAmmo, ReserveAmmo);
	if (bNetworkEmptyReload && ShouldLogShotProtocol())
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=AutoReload Ammo=%d Reserve=%d"), CurrentAmmo, ReserveAmmo);
#endif
	PublishNetworkState();
	BroadcastAmmoChanged();
	return true;
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
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	// 取消只收尾：装填帧之前不补弹，装填帧之后保留已转移的弹药。
	if (!IsCurrentReload(ReloadId)) return;
	if (Reason == EFPReloadEndReason::Completed)
	{
		CommitReloadForTransaction(ReloadId);
		// Commit 的同步广播可以取消、禁用或启动下一轮，旧结束不能覆盖新状态。
		if (!IsCurrentReload(ReloadId)) return;
	}
	const bool bCommitted = bReloadAmmoCommitted;
	LastNetworkReloadEndReason = Reason;
	const auto EndedPlaybacks = MoveTemp(ReloadPlaybacks);
	// ★【重要边界】[生命周期] 先失效旧状态，再停止其动画，最后广播结束；Stop 触发的旧回调不能再次结算换弹。
	ActiveReloadId = INDEX_NONE;
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(ReloadTimerHandle);
	{
		TGuardValue<bool> PlaybackGuard(bChangingReloadPlayback, true);
		for (const FFPActionPlayback& Playback : EndedPlaybacks) Playback.Stop();
	}
	// 取消不补弹；正常完成只能由保存事务身份的权威入口提交。
	// 发布前已完成所有旧事务清理；监听者开启新事务后，旧栈不能再写状态。
	OnAbilityReloadEnded.Broadcast(ReloadId, Reason, bCommitted);
	PublishNetworkState();
	OnWeaponReloadEnded.Broadcast(ReloadId, Reason, bCommitted);
}

// ==================== Recoil System ====================

// 应用本发受限控制后坐力及独立画面脉冲，再建立真实游戏时间驱动的恢复计时。
void UfpstrueWeaponComponent::ApplyRecoil(APlayerController* PlayerController)
{
	// 本发后坐力先累加到受限范围，再启动固定频率的恢复 Timer；瞄准时使用较小倍率。
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
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
	if (PlayerController->PlayerCameraManager != nullptr
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
		// 按真实经过时间恢复；原生单帧限次避免卡顿后补调多次零步长回调。
		LastRecoilRecoveryTimeSeconds = World->GetTimeSeconds() + Settings.RecoilRecoveryDelay;
		World->GetTimerManager().SetTimer(RecoilRecoveryTimerHandle, this, &UfpstrueWeaponComponent::UpdateRecoilRecovery,
			RecoilRecoveryTickInterval, FTimerManagerTimerParameters{
				.bLoop = true, .bMaxOncePerFrame = true, .FirstDelay = Settings.RecoilRecoveryDelay});
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

// 连接更替不是重新拾枪：只更换输入代际，权威弹药和真实冷却继续使用原值。
void UfpstrueWeaponComponent::ResetNetworkInputGeneration()
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || !IsOperational()) return;
	const TWeakObjectPtr<AfpstrueCharacter> PreviousHolder = Character;
	InterruptOwnerInput();
	if (!IsOperational() || Character != PreviousHolder) return;
	++EquipmentRevision;
	FireSession = LastRemoteFireSession = 0;
	LastReceivedShotId = LastExecutedShotId = LastProcessedShotId = 0;
	FinalizedShotInputs.Reset();
	RecentShotReceipts.Reset();
	LastSubmittedFireTime = -1.0;
	LastShotResult = EFPShotResult::Accepted;
	PublishNetworkState();
}

// 组件退出时先阻止新动作，通过完整卸枪入口清理计时和装备关系，再执行父类退出。
void UfpstrueWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// ★【重要边界】[生命周期] 先拒绝新动作，再走完整卸枪清理；只清 Timer 不能解除双方装备关系和视觉状态。
	// 组件退出前清理所有 Timer，并通知角色解除装备关系，避免留下跨生命周期的回调和悬空关系。
	bEndingPlay = true;
	OnWeaponFirePerformed.RemoveDynamic(this, &ThisClass::PlayWeaponFirePresentation);
	OnWeaponReloadPlaybackRequested.RemoveDynamic(this, &ThisClass::PlayWeaponReloadPresentation);
	OnWeaponTraceFinished.RemoveDynamic(this, &ThisClass::PlayWeaponImpactPresentation);
	DetachWeapon();
	Super::EndPlay(EndPlayReason);
}

// 死亡或卸下时永久禁用动作；若正在换弹，携带 Disabled 原因走统一结束入口。
void UfpstrueWeaponComponent::DisableWeapon()
{
	// 禁用清理独立于上一个动作；重复 Disable 也清理残留，不在各生命周期出口复制规则。
	// 死亡、卸下和 EndPlay 共用强制中断；先禁用，再清理，后续回调不能恢复 Ready。
	bEquipmentEnabled = false;
	EndPredictedReload(LocalReloadPredictionKey, EFPReloadEndReason::Disabled);
	++EquipmentRevision;
	if (ActiveReloadId != INDEX_NONE) EndReload(ActiveReloadId, EFPReloadEndReason::Disabled);
	if (UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr)
	{
		FGameplayTagContainer WeaponAbilities;
		WeaponAbilities.AddTag(FpstrueGameplayTags::Ability_Fire);
		WeaponAbilities.AddTag(FpstrueGameplayTags::Ability_Reload);
		WeaponAbilities.AddTag(FpstrueGameplayTags::Ability_Aim);
		ASC->CancelAbilities(&WeaponAbilities);
	}
	EndAbilityFire(true);
	OutstandingShots.Reset();
	ClientShotInputs.Reset();
	if (GetWorld() != nullptr) GetWorld()->GetTimerManager().ClearTimer(ShotRetryTimerHandle);
	ClearRecoilState();
	StopVisualRecoil();
	ConsecutiveShotCount = 0;
	PublishNetworkState();
}

// 控制权迁移只终止旧输入的持续动作与后坐力；角色仍有效时保留装备和弹药。
void UfpstrueWeaponComponent::InterruptOwnerInput()
{
	// Character 自己管理 ADS/冲刺输入资格；武器只拥有本模块清理期间的重入边界。

	AfpstrueCharacter* OwningCharacter = Character.Get();
	if (OwningCharacter == nullptr || !OwningCharacter->CanMaintainEquipment())
	{
		DisableWeapon();
		return;
	}
	// Character 的控制权迁移保护输入入口；组件也拥有独立的公开中断接口，因此在这里保护 Cancel 的同步回调。
	// ★【重要边界】[生命周期] 临时丢失输入只清持续动作，不销毁装备或弹药；角色真正失效才转入永久禁用。
	// 不能直接让 IsOperational 返回 false，否则 CancelReload 会把临时中断误变成永久 Disabled。
	TGuardValue<bool> InputTransitionGuard(bInterruptingOwnerInput, true);
	if (UAbilitySystemComponent* ASC = OwningCharacter->GetAbilitySystemComponent())
	{
		FGameplayTagContainer WeaponAbilities;
		WeaponAbilities.AddTag(FpstrueGameplayTags::Ability_Fire);
		WeaponAbilities.AddTag(FpstrueGameplayTags::Ability_Reload);
		ASC->CancelAbilities(&WeaponAbilities);
	}
	EndAbilityFire(true);
	OutstandingShots.Reset();
	ClientShotInputs.Reset();
	if (GetWorld() != nullptr) GetWorld()->GetTimerManager().ClearTimer(ShotRetryTimerHandle);
	ClearRecoilState();
	StopVisualRecoil();
	CancelReload();
}

// 发布已经提交的弹药快照，HUD 和蓝图只观察结果，不在此重复结算。
void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	// 弹药状态只由 WeaponComponent 写入；角色、HUD 和蓝图通过该委托读取同一份结果。
	OnAmmoChanged.Broadcast(GetCurrentAmmo(), Settings.MagazineSize, ReserveAmmo);
}
