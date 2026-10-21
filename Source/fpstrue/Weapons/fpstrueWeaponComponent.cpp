// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "AbilitySystemComponent.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Runtime/fpstrueRuntime.h"
#include "GameFramework/PlayerController.h"
#include "Game/fpstrueMultiplayer.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Animation/AnimInstance.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "Engine/PackageMapClient.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

/*
 * 玩家武器的单发结算与换弹事务实现。
 * GAS 独占动作互斥和本地连射输入；组件拥有已准入网络执行队列、装备、弹药、射速约束与后坐力恢复。
 * 角色向 ASC 提交输入；单机动画通过 Notify 提交换弹，联网由服务器时长完成。
 * 原生/蓝图表现桥选择素材并消费事实；C++ 拥有命中/弹药结算以及明确的播放身份和结束边界。
 *
 * 主要调用链：
 *   输入 -> Character -> ASC / Fire GA -> 单机 Fire / 联网 TargetData -> 准入队列 -> 执行复核 -> Fire -> Hitscan
 *   权威结算 -> 弹药/动作快照 + 逐发 ACK + 单发表现多播；拥有者只预测表现和未确认弹药缺口。
 *   输入 -> Character::RequestWeaponReload -> ASC / Reload GA -> BeginAbilityReload -> PlaybackRequested(ReloadId)
 *   单机：已绑定 ReloadCommit Notify -> 提交弹药；全部播放正常结束 -> Completed；主动中断 -> Cancelled。
 *   单机播放链缺失 -> TimedOut，仅解除动作锁；播放失败 -> PlaybackFailed。联网由服务器 Timer 完成事务。
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
constexpr double MaxNetworkShotAgeSeconds = 0.30;
// 本版远端呈现采用有限的固定回退，不声称复原 UE 平滑器每一帧的实际显示时间。
constexpr double TargetInterpolationSeconds = 0.05;

// 准入按延迟收紧时间窗；排队执行仍不得超过历史判定的最长窗口。
bool IsShotTimeWithinWindow(double FireTime, double Now, double MaxAge)
{
	return FMath::IsFinite(FireTime) && FireTime <= Now + MaxShotFutureSeconds && FireTime >= Now - MaxAge;
}

// 素材替换只复用已验证的声音/特效反馈，不复制另一套射击、弹药或命中逻辑。
void ConfigureNativeWeaponFeedback(UfpstrueWeaponComponent& Weapon, uint32 AssetLoadFlags)
{
	Weapon.FireSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Sounds/Weapon/AK74/WAV_AK74M_Fire_Cue.WAV_AK74M_Fire_Cue"), nullptr, AssetLoadFlags);
	Weapon.MuzzleEffect = LoadObject<UParticleSystem>(nullptr, TEXT("/Game/FPS_Assault_Pack/Particles/Emitters/AR74/P_RU74_Muzzleflash.P_RU74_Muzzleflash"), nullptr, AssetLoadFlags);
	Weapon.ImpactEffect = LoadObject<UParticleSystem>(nullptr, TEXT("/Game/Particles/bulletvfx/Particles/VFX_Impact_Concrete.VFX_Impact_Concrete"), nullptr, AssetLoadFlags);
	Weapon.FleshImpactEffect = LoadObject<UParticleSystem>(nullptr, TEXT("/Game/Particles/bulletvfx/Particles/VFX_Impact_Flesh.VFX_Impact_Flesh"), nullptr, AssetLoadFlags);
	Weapon.ImpactSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Sounds/BulletImpact/Concrete/bullet_impact_concrete_brick_01_Cue.bullet_impact_concrete_brick_01_Cue"), nullptr, AssetLoadFlags);
	Weapon.ImpactDecal = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Particles/Bullet_Impact_Decal.Bullet_Impact_Decal"), nullptr, AssetLoadFlags);
}

// 两种原生素材使用同一个拾取组件和重叠规则；组件自行保护同帧重复提交。
void ConfigureNativeWeaponPickup(UfpstruePickUpComponent& Pickup, UfpstrueWeaponComponent& Weapon)
{
	Pickup.SetupAttachment(&Weapon);
	Pickup.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Pickup.SetCollisionResponseToAllChannels(ECR_Ignore);
	Pickup.SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Pickup.SetGenerateOverlapEvents(true);
}

#if !UE_BUILD_SHIPPING
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
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest"))) return;
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
	USceneComponent* Source = IsValid(MuzzleSource) ? MuzzleSource.Get() : this;
	FTransform Muzzle = Source->GetSocketTransform(MuzzleSocketName);
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
	if (!bFlesh && ImpactDecal != nullptr) UGameplayStatics::SpawnDecalAtLocation(this, ImpactDecal, FVector(5.0f), HitResult.ImpactPoint, Rotation, 10.0f);
}

// 手臂/身体由角色监听同一事件；枪械仅负责自己的弹匣、枪栓等骨骼播放。
void UfpstrueWeaponComponent::PlayWeaponReloadPresentation(int32 ReloadId, bool bWasEmptyReload)
{
	if (GetNetMode() == NM_DedicatedServer) return;
	UAnimMontage* Montage = bWasEmptyReload ? WeaponEmptyReloadMontage.Get() : WeaponReloadMontage.Get();
	if (Montage == nullptr) return; // 未配置时保留原武器蓝图的显式播放桥。
	const float Duration = GetReloadDuration();
	const float PlayRate = Duration > 0.0f ? Montage->GetPlayLength() / Duration : 1.0f;
	PlayReloadMontage(ReloadId, this, Montage, PlayRate);
}

void UfpstrueWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UfpstrueWeaponComponent, NetworkState);
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
	NetworkState.Ammo = CurrentAmmo;
	NetworkState.Reserve = ReserveAmmo;
	NetworkState.ReloadId = ReloadSerial;
	NetworkState.bEmptyReload = bNetworkEmptyReload;
	NetworkState.bCommitted = bReloadAmmoCommitted;
	NetworkState.EndReason = LastNetworkReloadEndReason;
	NetworkState.FirePresentationKey = FirePresentationKey;
	NetworkState.FirePresentationShot = FirePresentationShot;
	NetworkState.EquipmentGeneration = EquipmentRevision;
	NetworkState.FireSession = FireSession;
	NetworkState.LastProcessedShotId = LastProcessedShotId;
	NetworkState.ShotResult = LastShotResult;
	NetworkState.bDroppedOnDeath = bDroppedOnDeath;
	++NetworkState.Revision;
	OwnerActor->ForceNetUpdate();
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
	if (!GetOwner()->HasAuthority() && IsReloading() && LastPresentedReloadId != ActiveReloadId && IsOperational())
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
	// 同一武器通道的 ACK 可已带来更新弹药；旧属性快照也不能撤销新动作的本地表现。
	if (NetworkState.Revision < LastAmmoStateRevision) return;
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
	if (EquipmentRevision != NetworkState.EquipmentGeneration) OutstandingShots.Reset();
	EquipmentRevision = NetworkState.EquipmentGeneration;
	CurrentAmmo = NetworkState.Ammo;
	ReserveAmmo = NetworkState.Reserve;
	LastAmmoStateRevision = NetworkState.Revision;
	if (FPredictedFirePresentation* Presented = PredictedPresentations.FindByPredicate(
		[this](const FPredictedFirePresentation& P) { return P.Generation == EquipmentRevision && P.Session == NetworkState.FireSession; }))
		Presented->ConfirmedShots = FMath::Max(Presented->ConfirmedShots, NetworkState.FirePresentationShot);
	ReloadSerial = NetworkState.ReloadId;
	ActiveReloadId = NetworkState.Action == EFPWeaponActionState::Reloading ? NetworkState.ReloadId : INDEX_NONE;
	bReloadAmmoCommitted = NetworkState.bCommitted;
	if (Previous == EFPWeaponActionState::Reloading && (NetworkState.Action != Previous || PreviousReloadId != ActiveReloadId))
	{
		const auto Ended = MoveTemp(ReloadPlaybacks);
		for (const FFPActionPlayback& Playback : Ended) Playback.Stop();
		// 若已经跨到新编号，当前快照不能证明上一轮是否提交；只撤销旧表现，不借用新一轮的结算标记。
		const bool bSameReload = PreviousReloadId == NetworkState.ReloadId;
		OnWeaponReloadEnded.Broadcast(PreviousReloadId,
			bSameReload ? NetworkState.EndReason : EFPReloadEndReason::Cancelled, bSameReload && NetworkState.bCommitted);
	}
	// 先结束旧播放，才能启动新编号的播放；复制允许跳过中间 Ready，不能把新播放一起停止。
	RefreshNetworkEquipment();
	BroadcastAmmoChanged();
	if (NetworkState.Action == EFPWeaponActionState::Reloading && LastPresentedReloadId != ActiveReloadId && IsOperational())
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
				if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
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
	return Request.Avatar == Character.Get() && Request.EquipmentGeneration == EquipmentRevision;
}

// 同一 GA 在拥有者先播放表现，在服务器执行原有结算；两端绝不各自生成一份权威伤害。
void UfpstrueWeaponComponent::BeginAbilityFire(int32 PredictionKey)
{
	FirePresentationKey = PredictionKey;
	FirePresentationShot = 0;
	FireSession = 0;
	if (GetOwner()->HasAuthority()) { PublishNetworkState(); return; }
	if (!Character->IsLocallyControlled() || PredictionKey == 0) return;
	if (LocalFireSessionSerial == MAX_uint32) return;
	FireSession = ++LocalFireSessionSerial;
	FirstPredictedShotForCurrentFire = LocalShotSerial + 1;
	if (PredictedPresentations.Num() == 8) PredictedPresentations.RemoveAt(0);
	PredictedPresentations.Add({PredictionKey, EquipmentRevision, FireSession, 0, 0});
	bPredictingFire = true;
	// GA 决定何时生成下一发输入；服务器唯一队列 Timer 只执行已经合法准入的请求。
}

// 结束能力只停本地预测表现；不撤销已结算子弹，不清迟到表现的去重记录。
void UfpstrueWeaponComponent::EndAbilityFire(bool bDiscardOutstandingShots)
{
	bPredictingFire = false;
	if (bDiscardOutstandingShots) CancelPendingNetworkShots();
	// 正常松开不抹掉仍在途中的逐发事实，否则 ACK 前会误判满仓并拦截换弹。
	// 未确认账本与本轮持续开火分离；强取消或装备代际更替才丢弃估计，不回滚权威弹药。
	if (bDiscardOutstandingShots && GetOwner() != nullptr && !GetOwner()->HasAuthority())
	{
		// 新激活预测失败只撤销自己的区间；上一轮已准入输入仍须等待各自的终结 ACK。
		const uint32 First = FirstPredictedShotForCurrentFire;
		const uint32 Last = LocalShotSerial;
		OutstandingShots.RemoveAll([First, Last](uint32 ShotId) { return First != 0 && ShotId >= First && ShotId <= Last; });
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
	// 本地弹药估计不能结束服务器连射；快照可能迟到，达到预测上限只停表现，等待权威空仓或真实松开。
	const int32 PresentationKey = FirePresentationKey;
	FirePresentationShot = static_cast<int32>(Request.ShotId);
	Presented->PresentedShots = FirePresentationShot;
	LastPredictedShotTime = GetWorld()->GetTimeSeconds();
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
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
		|| LocalShotSerial >= MAX_int32 || OutstandingShots.Num() >= 32 || CurrentAmmo <= OutstandingShots.Num()) return false;
	const auto* Controller = Cast<AfpstruePvpPlayerController>(Character->GetController());
	if (Controller == nullptr || !Controller->HasServerTimeSample() || Controller->GetClockUncertaintySeconds() > 0.30) return false;
	OutRequest.Avatar = Character.Get();
	OutRequest.EquipmentGeneration = EquipmentRevision;
	OutRequest.FireSession = FireSession;
	OutRequest.ShotId = ++LocalShotSerial;
	OutRequest.ServerFireTime = Controller->GetEstimatedServerTime();
	OutRequest.AimDirection = Controller->GetControlRotation().Vector().GetSafeNormal();
	OutstandingShots.Add(OutRequest.ShotId);
#if !UE_BUILD_SHIPPING
	// 这里只标记请求构造完成；ASC 可靠调用紧随其后，不把它称为网卡发送，也不虚构应用层重试。
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SHOT_SUBMIT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu Attempt=1 MaxAttempts=1 RetryDelayMs=0 LocalSeconds=%.9f"),
			OutRequest.ShotId, OutRequest.FireSession, OutRequest.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), FPlatformTime::Seconds());
#endif
	return true;
}

// 准入：验证新输入并入队；只有 ExecuteNetworkShot 能把它交给 Fire 提交。
// 【设计取舍】准入与执行是两个时点，不是重复结算：有界队列吸收射速节拍与到包时机的差异，
// 等待期间仍可能死亡、换枪或换弹，所以执行前必须复核；只有 Fire 扣弹和施加伤害。
EFPShotResult UfpstrueWeaponComponent::AcceptShotRequest(const FFPShotRequest& Request)
{
	auto Reject = [this, &Request](EFPShotResult Result)
	{
#if !UE_BUILD_SHIPPING
		const int32 AmmoBefore = CurrentAmmo, ReserveBefore = ReserveAmmo;
		const uint32 ExecutedBefore = LastExecutedShotId;
#endif
		AcknowledgeShot(Request, Result);
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_REJECT_BOUNDARY ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu AmmoBefore=%d AmmoAfter=%d ReserveBefore=%d ReserveAfter=%d ExecutedBefore=%u ExecutedAfter=%u"),
				Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()),
				AmmoBefore, CurrentAmmo, ReserveBefore, ReserveAmmo, ExecutedBefore, LastExecutedShotId);
#endif
		return Result;
	};
	const UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || !CanExecuteShot()
		|| ASC == nullptr || !ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Firing)) return Reject(EFPShotResult::InvalidContext);
	if (!MatchesShotEquipment(Request) || Request.EquipmentGeneration == 0 || Request.FireSession == 0 || Request.ShotId == 0
		|| Request.ShotId > MAX_int32) return Reject(EFPShotResult::InvalidIdentity);
	if (Request.ShotId <= LastReceivedShotId)
	{
		// 同一请求仍在 GA 冷却等待时，重复输入不是原请求的终结结果，不能提前释放它的弹药估计。
		if (PendingNetworkShots.ContainsByPredicate([&Request](const FPendingNetworkShot& Pending)
			{ return Pending.Request.ShotId == Request.ShotId && Pending.Request.FireSession == Request.FireSession; })) return EFPShotResult::Queued;
		return Reject(EFPShotResult::Duplicate);
	}
	if ((FireSession != 0 && Request.FireSession != FireSession)
		|| (FireSession == 0 && Request.FireSession <= LastRemoteFireSession)) return Reject(EFPShotResult::InvalidIdentity);
	// 即使本发随后被拒绝也消费编号；重复包不能等冷却结束后再次提交。
	LastReceivedShotId = Request.ShotId;
	const double Now = GetWorld()->GetTimeSeconds();
	const APlayerState* PlayerState = Character->GetPlayerState();
	const double OneWayDelay = PlayerState != nullptr ? PlayerState->GetPingInMilliseconds() * 0.0005 : 0.0;
	const double MaxRequestAge = FMath::Clamp(OneWayDelay * 1.5 + 0.05, 0.10, MaxNetworkShotAgeSeconds);
	if (!IsShotTimeWithinWindow(Request.ServerFireTime, Now, MaxRequestAge))
	{
#if !UE_BUILD_SHIPPING
		LogShotTimeRejection(Request, Character.Get(), GetOwner(), TEXT("Admission"), Now, MaxRequestAge);
#endif
		return Reject(EFPShotResult::InvalidTime);
	}
	const FVector Aim = Request.AimDirection;
	if (Aim.ContainsNaN() || !Aim.IsNormalized()) return Reject(EFPShotResult::InvalidAim);
	const double Interval = 60.0 / Settings.RoundsPerMinute;
	if (LastSubmittedFireTime >= 0.0 && Request.ServerFireTime - LastSubmittedFireTime < Interval * 0.9)
		return Reject(EFPShotResult::RateLimited);
	if (FireSession == 0) { FireSession = Request.FireSession; LastRemoteFireSession = FireSession; }
	LastSubmittedFireTime = Request.ServerFireTime;
	if (!HasAmmo()) return Reject(EFPShotResult::NoAmmo);
	if (PendingNetworkShots.Num() >= MaxQueuedNetworkShots) return Reject(EFPShotResult::Busy);
	const float Delay = GetNextShotDelay(true);
	if (Delay > FMath::Min(Interval + 0.002, 0.15)) return Reject(EFPShotResult::RateLimited);
	const bool bMustWait = HasPendingNetworkShots() || Delay > KINDA_SMALL_NUMBER;
	// 只记录服务器当前已授权的瞄准条件，不能让排队期间切换 ADS 改写本发散布。
	PendingNetworkShots.Add({Request, Character->IsAiming(), FPlatformTime::Seconds()});
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
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
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
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
		LogShotTimeRejection(Request, Character.Get(), GetOwner(), TEXT("Execution"), GetWorld()->GetTimeSeconds(), MaxNetworkShotAgeSeconds);
#endif
	}
	else if (GetNextShotDelay(true) > KINDA_SMALL_NUMBER) Result = EFPShotResult::RateLimited;
	else
	{
		FVector Origin;
		if (GetNetMode() != NM_Standalone && !Character->GetHistoricalViewOrigin(Request.ServerFireTime, Origin))
		{
			Result = EFPShotResult::InvalidTime;
#if !UE_BUILD_SHIPPING
			LogShotTimeRejection(Request, Character.Get(), GetOwner(), TEXT("Execution"), GetWorld()->GetTimeSeconds(), MaxNetworkShotAgeSeconds, TEXT("MissingHistory"));
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
			if (Result == EFPShotResult::Accepted && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_COMMIT ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu AmmoBefore=%d AmmoAfter=%d"),
					Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), AmmoBefore, CurrentAmmo);
#endif
		}
	}
	AcknowledgeShot(Request, Result);
#if !UE_BUILD_SHIPPING
	if (Result != EFPShotResult::Accepted && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
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
	// SetTimer 原生替换同一句柄，保留空队列清理即可。
	World->GetTimerManager().SetTimer(NetworkShotTimerHandle, this, &ThisClass::ProcessPendingNetworkShot,
		FMath::Max(GetNextShotDelay(true), 0.001f), false);
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

// ACK 同时给出该发裁决和权威弹药；快照负责当前状态，可靠 ACK 负责每个请求的结束事实。
void UfpstrueWeaponComponent::RejectNetworkShot(const FFPShotRequest& Request, EFPShotResult Result)
{
	AcknowledgeShot(Request, Result == EFPShotResult::Accepted || Result == EFPShotResult::Queued
		? EFPShotResult::InvalidContext : Result);
}

void UfpstrueWeaponComponent::AcknowledgeShot(const FFPShotRequest& Request, EFPShotResult Result)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	// 只有本代际实际消费过的新编号才推进快照；重复 ACK 或别人的旧代际不能覆盖已接受的裁决。
	if (Result != EFPShotResult::Duplicate && MatchesShotEquipment(Request)
		&& Request.ShotId > LastProcessedShotId && Request.ShotId <= LastReceivedShotId)
	{
		LastProcessedShotId = Request.ShotId;
		LastShotResult = Result;
	}
	PublishNetworkState();
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SHOT_ACK_SEND ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu ServerTime=%.6f Result=%d"),
			Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetWeaponShotNetGuid(GetOwner()), GetWorld()->GetTimeSeconds(), static_cast<int32>(Result));
#endif
	ClientShotResult(Request.EquipmentGeneration, Request.FireSession, Request.ShotId, Result, CurrentAmmo, ReserveAmmo, NetworkState.Revision);
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_SHOT_%s ShotId=%u Session=%u Generation=%u Result=%d FireTime=%.6f ServerTime=%.6f Ammo=%d WeaponNetGuid=%llu PlayerId=%d NextDeadline=%.6f Pending=%d"),
			Result == EFPShotResult::Accepted ? TEXT("ACCEPT") : TEXT("REJECT"), Request.ShotId, Request.FireSession,
			Request.EquipmentGeneration, static_cast<int32>(Result), Request.ServerFireTime,
			GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0, CurrentAmmo, GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(Character.Get()),
			NextShotTimeSeconds, PendingNetworkShots.Num());
#endif
}

void UfpstrueWeaponComponent::ClientShotResult_Implementation(uint32 EquipmentGeneration, uint32 Session, uint32 ShotId,
	EFPShotResult Result, int32 Ammo, int32 Reserve, uint32 StateRevision)
{
	// 【设计取舍】可靠 ACK 终结每发输入账本，属性复制恢复最新持久状态，二者不能互相替代。
	// Revision 只防迟到 ACK 回写旧弹药；被拒绝的预测不回放反向枪声，也不冒充完整状态回滚。
	if (GetOwner() == nullptr || GetOwner()->HasAuthority() || EquipmentGeneration != EquipmentRevision) return;
	if (Result != EFPShotResult::Queued && Result != EFPShotResult::Duplicate) OutstandingShots.Remove(ShotId);
	if (StateRevision >= LastAmmoStateRevision)
	{
		LastAmmoStateRevision = StateRevision;
		CurrentAmmo = Ammo;
		ReserveAmmo = Reserve;
		OnAmmoChanged.Broadcast(CurrentAmmo, Settings.MagazineSize, ReserveAmmo);
	}
	FPredictedFirePresentation* Presented = PredictedPresentations.FindByPredicate(
		[EquipmentGeneration, Session](const FPredictedFirePresentation& P) { return P.Generation == EquipmentGeneration && P.Session == Session; });
	if (Presented != nullptr)
		Presented->ConfirmedShots = FMath::Max(Presented->ConfirmedShots, static_cast<int32>(ShotId));
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_SHOT_ACK ShotId=%u Session=%u Generation=%u Result=%d Ammo=%d Predicted=%d WeaponNetGuid=%llu PlayerId=%d Outstanding=%d StateRevision=%u LocalSeconds=%.9f"),
			ShotId, Session, EquipmentGeneration, static_cast<int32>(Result), Ammo, Presented != nullptr && static_cast<int32>(ShotId) <= Presented->PresentedShots ? 1 : 0,
			GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(Character.Get()), OutstandingShots.Num(), StateRevision, FPlatformTime::Seconds());
#endif
	// 已播放的枪声不反向播放“撤销动画”；被拒绝的预测只释放弹药估计，不生成命中反馈。
}

// 与真实事务相同的资格检查供 GA 使用；标签负责动作互斥，弹匣和备弹仍由武器独占。
bool UfpstrueWeaponComponent::HasReloadAmmo() const
{
	// 服务器弹药快照尚未到时，刚预测过一发也可提交 R；它只放行请求，服务器仍检查真实缺口。
	const bool bPredictedGap = GetOwner() != nullptr && !GetOwner()->HasAuthority()
		&& !OutstandingShots.IsEmpty();
	return CanAcceptOwnerInput() && GetWorld() != nullptr
		&& (CurrentAmmo < Settings.MagazineSize || bPredictedGap) && ReserveAmmo > 0;
}

// ==================== 武器拾取入口 ====================
/*
 * 一次性武器拾取入口。
 * Sphere 只负责发现候选玩家，真正的装备规则由 WeaponComponent::AttachWeapon 决定；成功后立即关闭重叠、
 * 广播表现事件并销毁本组件。bConsumed 同时保护同帧多次 Overlap，避免一把武器重复提交装备事务。
 */

// RAR 只替换可见网格和纯表现动画，不继承旧 RU74 蓝图的开火/换弹事件或另一套玩法代码。
AfpstrueRARWeapon::AfpstrueRARWeapon()
{
	bReplicates = true;
	Weapon = CreateDefaultSubobject<UfpstrueWeaponComponent>(TEXT("Weapon"));
	SetRootComponent(Weapon);
	Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Weapon->WeaponDisplayName = FText::FromString(TEXT("RAR 突击步枪"));
	// 生成项目专用资产的首次 Editor 进程允许资源暂缺；发布与自动化资产门禁必须确认全部实际绑定，不能靠回退交付。
	constexpr uint32 AssetLoadFlags = LOAD_Quiet | LOAD_NoWarn;
	Weapon->SetSkeletalMesh(LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SK_RAR_AssaultRifle.SK_RAR_AssaultRifle"), nullptr, AssetLoadFlags));
	if (UClass* Animation = StaticLoadClass(UAnimInstance::StaticClass(), nullptr,
		TEXT("/Game/FirstPerson/Animations/RAR/ABP_RARWeapon.ABP_RARWeapon_C"), nullptr, AssetLoadFlags)) Weapon->SetAnimInstanceClass(Animation);
	Weapon->WeaponFireMontage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/FirstPerson/Animations/RAR/AM_RARWeapon_Fire.AM_RARWeapon_Fire"), nullptr, AssetLoadFlags);
	Weapon->WeaponReloadMontage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/FirstPerson/Animations/RAR/AM_RARWeapon_Reload.AM_RARWeapon_Reload"), nullptr, AssetLoadFlags);
	Weapon->WeaponEmptyReloadMontage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/FirstPerson/Animations/RAR/AM_RARWeapon_ReloadEmpty.AM_RARWeapon_ReloadEmpty"), nullptr, AssetLoadFlags);
	Weapon->WeaponConfiguration = LoadObject<UfpstrueWeaponConfig>(nullptr,
		TEXT("/Game/Config/DA_FPWeapon_RAR.DA_FPWeapon_RAR"), nullptr, AssetLoadFlags);
	// 枪体与可动弹匣分开提供；静态附件跟随相应枪械骨骼，不能只加载裸枪网格。
	const auto AddPart = [this, AssetLoadFlags](const TCHAR* Name, const TCHAR* Path, USceneComponent* Parent, FName Socket)
	{
		UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(Parent, Socket);
		Part->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Path, nullptr, AssetLoadFlags));
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return Part;
	};
	Forestock = AddPart(TEXT("Forestock"), TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SM_RAR_AssaultRifle_Forestock_Default.SM_RAR_AssaultRifle_Forestock_Default"), Weapon, TEXT("SOCKET_Forestock"));
	Magazine = AddPart(TEXT("Magazine"), TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SM_RAR_AssaultRifle_Magazine_Default_Full.SM_RAR_AssaultRifle_Magazine_Default_Full"), Weapon, TEXT("SOCKET_Magazine"));
	FrontSight = AddPart(TEXT("FrontSight"), TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SM_RAR_AssaultRifle_IronSight_F.SM_RAR_AssaultRifle_IronSight_F"), Forestock, TEXT("SOCKET_Ironsight_F"));
	RearSight = AddPart(TEXT("RearSight"), TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SM_RAR_AssaultRifle_Ironsight_B.SM_RAR_AssaultRifle_Ironsight_B"), Weapon, TEXT("SOCKET_Default"));
	Weapon->MuzzleSource = Forestock;
	Weapon->MuzzleSocketName = TEXT("SOCKET_Muzzle");
	Weapon->MuzzleForwardAxis = FVector::RightVector;
	// 沿用已验证的 Cascade/声音反馈，不引入 vendor 的 GameplayCore 或第二条命中结算路径。
	ConfigureNativeWeaponFeedback(*Weapon, AssetLoadFlags);
	Pickup = CreateDefaultSubobject<UfpstruePickUpComponent>(TEXT("Pickup"));
	ConfigureNativeWeaponPickup(*Pickup, *Weapon);
}

void AfpstrueRARWeapon::OnRep_AttachmentReplication()
{
	// 旧手部附着不可先由 Super 应用，否则即使随后 Detach 也会把已落地枪瞬移回手边。
	if (Weapon->IsDroppedOnDeath()) { Weapon->RefreshNetworkEquipment(); return; }
	Super::OnRep_AttachmentReplication();
	Weapon->RefreshNetworkEquipment();
}

void AfpstrueRARWeapon::OnRep_ReplicatedMovement()
{
	if (IsReplicatingMovement() && GetReplicatedMovement().bRepPhysics) Weapon->RefreshNetworkPhysics();
	else if (Weapon->IsDroppedOnDeath()) return; // 丢弃死亡前的非物理移动快照。
	Super::OnRep_ReplicatedMovement();
}

void AfpstrueRARWeapon::GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const
{
	Super::GetReplicatedCustomConditionState(OutActiveState);
	// 根组件仍复制 NetworkState；启用移动时明确允许 Actor 物理流，运行中由原生 PreReplication 继续维护。
	DOREPCUSTOMCONDITION_ACTIVE_FAST(AActor, ReplicatedMovement, IsReplicatingMovement());
}

void AfpstrueRARWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (GetNetMode() == NM_DedicatedServer)
	{
		// 枪的动画仅表现弹匣/枪栓；专服保留网格插槽和业务组件，不创建纯表现动画实例。
		Weapon->SetAnimInstanceClass(nullptr);
		Weapon->SetDisablePostProcessBlueprint(true);
		Weapon->SetComponentTickEnabled(false);
		Weapon->PrimaryComponentTick.bStartWithTickEnabled = false;
	}
	if (GetWorld() != nullptr && GetWorld()->IsGameWorld()) SetViewmodelFOVEnabled(false);
}

// MID 由网格组件按需创建并复用；重复装备/附着快照不重复生成材质实例，也不写全局参数。
void AfpstrueRARWeapon::SetViewmodelFOVEnabled(bool bEnabled)
{
	if (GetWorld() == nullptr || !GetWorld()->IsGameWorld() || GetNetMode() == NM_DedicatedServer) return;
	const int8 Mode = bEnabled ? 1 : 0;
	if (AppliedViewmodelFOVMode == Mode) return;
	UMeshComponent* Meshes[] = {Weapon.Get(), Forestock.Get(), Magazine.Get(), FrontSight.Get(), RearSight.Get()};
	for (UMeshComponent* Mesh : Meshes) Mesh->SetScalarParameterValueOnMaterials(TEXT("Field Of View Alpha"), static_cast<float>(Mode));
	AppliedViewmodelFOVMode = Mode;
}

// 成套模板的枪体已包含弹匣/枪栓骨骼，无 RAR 静态附件或供应方玩法蓝图。
AfpstrueTemplateWeapon::AfpstrueTemplateWeapon()
{
	bReplicates = true;
	Weapon = CreateDefaultSubobject<UfpstrueWeaponComponent>(TEXT("Weapon"));
	SetRootComponent(Weapon);
	Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Weapon->WeaponDisplayName = FText::FromString(TEXT("突击步枪"));
	Weapon->GripLocationOffset = FVector(-6.236122, 3.008201, 1.059424);
	Weapon->GripRotationOffset = FRotator(3.4989, 105.08198, 4.0575);
	constexpr uint32 AssetLoadFlags = LOAD_Quiet | LOAD_NoWarn;
	Weapon->SetSkeletalMesh(LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/FPSMPT55/Weapons/AssaultRifle/SKM_AssaultRifle.SKM_AssaultRifle"), nullptr, AssetLoadFlags));
	if (UClass* Animation = StaticLoadClass(UAnimInstance::StaticClass(), nullptr,
		TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateWeapon.ABP_TemplateWeapon_C"), nullptr, AssetLoadFlags)) Weapon->SetAnimInstanceClass(Animation);
	Weapon->WeaponFireMontage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/FirstPerson/Animations/FPSMPT/AM_TemplateWeapon_Fire.AM_TemplateWeapon_Fire"), nullptr, AssetLoadFlags);
	Weapon->WeaponReloadMontage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/FirstPerson/Animations/FPSMPT/AM_TemplateWeapon_Reload.AM_TemplateWeapon_Reload"), nullptr, AssetLoadFlags);
	Weapon->WeaponEmptyReloadMontage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/FirstPerson/Animations/FPSMPT/AM_TemplateWeapon_ReloadEmpty.AM_TemplateWeapon_ReloadEmpty"), nullptr, AssetLoadFlags);
	Weapon->WeaponConfiguration = LoadObject<UfpstrueWeaponConfig>(nullptr,
		TEXT("/Game/Config/DA_FPWeapon_Template.DA_FPWeapon_Template"), nullptr, AssetLoadFlags);
	// muzzleFlash 的本地 X 轴已由插槽旋转至枪管 +Y；不能再套用 RAR 附件的 RightVector。
	Weapon->MuzzleSource = Weapon;
	Weapon->MuzzleSocketName = TEXT("muzzleFlash");
	Weapon->MuzzleForwardAxis = FVector::ForwardVector;
	ConfigureNativeWeaponFeedback(*Weapon, AssetLoadFlags);
	Pickup = CreateDefaultSubobject<UfpstruePickUpComponent>(TEXT("Pickup"));
	ConfigureNativeWeaponPickup(*Pickup, *Weapon);
}

void AfpstrueTemplateWeapon::OnRep_AttachmentReplication()
{
	if (Weapon->IsDroppedOnDeath()) { Weapon->RefreshNetworkEquipment(); return; }
	Super::OnRep_AttachmentReplication();
	Weapon->RefreshNetworkEquipment();
}

void AfpstrueTemplateWeapon::OnRep_ReplicatedMovement()
{
	if (IsReplicatingMovement() && GetReplicatedMovement().bRepPhysics) Weapon->RefreshNetworkPhysics();
	else if (Weapon->IsDroppedOnDeath()) return;
	Super::OnRep_ReplicatedMovement();
}

void AfpstrueTemplateWeapon::GetReplicatedCustomConditionState(FCustomPropertyConditionState& OutActiveState) const
{
	Super::GetReplicatedCustomConditionState(OutActiveState);
	DOREPCUSTOMCONDITION_ACTIVE_FAST(AActor, ReplicatedMovement, IsReplicatingMovement());
}

void AfpstrueTemplateWeapon::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (GetNetMode() == NM_DedicatedServer)
	{
		Weapon->SetAnimInstanceClass(nullptr);
		Weapon->SetDisablePostProcessBlueprint(true);
		Weapon->SetComponentTickEnabled(false);
		Weapon->PrimaryComponentTick.bStartWithTickEnabled = false;
	}
}

//初始化UfpstruePickUpComponent组件，设置SphereRadius为32.f
UfpstruePickUpComponent::UfpstruePickUpComponent()
{
	SphereRadius = 32.f;
}

// 装备恢复、第一/第三人称切换和死亡掉落都会递归显示枪械；碰撞球不能随之成为可见模型。
void UfpstruePickUpComponent::OnHiddenInGameChanged()
{
	// 仅约束渲染隐藏，不修改可见附件、碰撞开关或重叠事件；直接恢复标记避免递归调用自身。
	bHiddenInGame = true;
	Super::OnHiddenInGameChanged();
}

// 组件启动时只绑定一次权威拾取入口，装备资格仍由重叠回调和武器事务检查。
void UfpstruePickUpComponent::BeginPlay()
{
	Super::BeginPlay();
	OnComponentBeginOverlap.AddUniqueDynamic(this, &UfpstruePickUpComponent::OnSphereBeginOverlap);
}

// 服务器发现玩家候选后申请完整装备，成功才消费并广播；先占用以阻止装备通知重入拾取。
void UfpstruePickUpComponent::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (bConsumed || GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}
	AfpstrueCharacter* Character = Cast<AfpstrueCharacter>(OtherActor);
	//拾取组件的 Owner 是武器 Actor，不是刚进入范围的玩家。
	AActor* OwnerActor = GetOwner();
	UfpstrueWeaponComponent* WeaponComponent = OwnerActor != nullptr ? OwnerActor->FindComponentByClass<UfpstrueWeaponComponent>() : nullptr;

	if (Character == nullptr || WeaponComponent == nullptr)
	{
		return;
	}

	// ★【重要边界】[结算去重] 外部装备调用前先占用拾取物；失败才释放，成功后关闭重叠再通知表现。
	// AttachWeapon 会同步广播装备事件，先占用再调用，防止监听者重入拾取。
	bConsumed = true;
	if (!WeaponComponent->AttachWeapon(Character))
	{
		bConsumed = false;
		return;
	}
	// 成功后先关闭入口再广播；监听者即使同步销毁拾取物，也不会再次装备。
	SetGenerateOverlapEvents(false);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OnComponentBeginOverlap.RemoveAll(this);
	OnPickUp.Broadcast(Character);

	if (IsValid(this))
	{
		DestroyComponent();
	}
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
	return (ASC != nullptr && ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading))
		|| (GetOwner() != nullptr && !GetOwner()->HasAuthority() && LastPresentedAction == EFPWeaponActionState::Reloading);
}

// 在装备资格之外检查角色输入和组件内部迁移保护，拒绝清理或播放变化期间的新动作。
bool UfpstrueWeaponComponent::CanAcceptOwnerInput() const
{
	return IsOperational() && Character->CanAcceptGameplayInput() && !bInterruptingOwnerInput && !bChangingReloadPlayback;
}

// ==================== Fire System ====================

// 将同步开火命令交给 ASC，由 Fire GA 检查资格并持有动作生命周期。
void UfpstrueWeaponComponent::StartFire()
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	if (UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr)
		ASC->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
}

// 只结束正在进行的连射；松开扳机不能解除换弹或重新启用已禁用武器。
void UfpstrueWeaponComponent::StopFire()
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return;
	if (UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr)
		ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
}

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
	return LastPredictedShotTime < 0.0 ? 0.0f
		: static_cast<float>(FMath::Max(0.0, LastPredictedShotTime + 60.0 / Settings.RoundsPerMinute - Now));
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
	const bool bHit = Request != nullptr
		? AfpstrueCharacter::TraceHistoricalHit(&World, &OwningCharacter,
			Request->ServerFireTime - (GetNetMode() != NM_Standalone ? TargetInterpolationSeconds : 0.0), Start, End, HitResult)
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
	if (Request != nullptr && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE REWIND ShotId=%u FireTime=%.6f TargetTime=%.6f Hit=%s Bone=%s WeaponNetGuid=%llu PlayerId=%d"),
			Request->ShotId, Request->ServerFireTime, Request->ServerFireTime - TargetInterpolationSeconds,
			*GetNameSafe(HitResult.GetActor()), *HitResult.BoneName.ToString(), GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(&OwningCharacter));
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
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_SERVER PASS ShotId=%u TargetSpeed=%.2f HealthBefore=%.2f HealthAfter=%.2f WeaponNetGuid=%llu PlayerId=%d"),
				Request->ShotId, TargetSpeed, HealthBefore, MovingTarget->GetCurrentHealth(), GetWeaponShotNetGuid(GetOwner()), GetShotPlayerId(&OwningCharacter));
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
	if (bHit && HitActor != nullptr && HitActor->IsA<AfpstrueCharacter>() && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Hit Shooter=%s Target=%s"), *OwningCharacter.GetName(), *HitActor->GetName());
#endif
}

// ==================== Reload System ====================

/*
 * Reload GA 持有 Reloading 标签；执行器创建带 ID 的事务和播放请求。
 * 单机绑定 Notify 在装填帧转移弹药；联网由服务器时长提交，客户端 Notify 不授权装弹。
 * 单机由原生实例结束委托收口：全部正常完成才 Completed，一项被打断即 Cancelled；联网播放不改服务器事务。
 * 显式正常完成可补交缺失的 Commit；取消、失败和超时不补交也不回滚已提交的弹药。
 * 播放链未结束时，唯一兜底 Timer 以 TimedOut 解除锁；无动画玩法可保存 ID 调用 ForTransaction 接口。
 *
 * Delegate 在当前调用栈同步执行，监听者可能取消换弹、死亡或开始下一次换弹。
 * 因此开始事件之前必须先设置事务和 Timer；Finish 在提交事件返回后还要确认 ReloadId 未变。
 */

// 将手动或空仓换弹请求交给同一个 Reload GA，不直接创建事务或装弹。
bool UfpstrueWeaponComponent::RequestReload()
{
	// 手动、空仓和原蓝图接口都激活同一个 Reload GA，不能旁路能力互斥另开一次事务。
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return false;
	UAbilitySystemComponent* ASC = Character.IsValid() ? Character->GetAbilitySystemComponent() : nullptr;
	return ASC != nullptr && ASC->TryActivateAbilityByClass(UfpstrueReloadAbility::StaticClass());
}

// 仅由 Reload GA 调用的服务器执行器，保留原动画身份、提交去重与唯一兜底计时。
// GA 在调用此执行器前已用标签取消 Aim/Sprint，这里不再复制互斥规则。
bool UfpstrueWeaponComponent::BeginAbilityReload(int32& OutReloadId)
{
	OutReloadId = INDEX_NONE;
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority()) return false;
	// 这里只开启事务，不立刻搬运弹药；单机在动画帧提交，联网由服务器计时提交。
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
	// 使用唯一超时句柄，显式播放可按真实长度延长截止点；排队的旧回调只能结束它保存的那次换弹。
	const bool bNetworkReload = GetNetMode() != NM_Standalone;
	// 网络模式的装弹由服务器时长决定，Dedicated Server 没有第一人称动画也能完成；客户端 Notify 无权提交。
	if (bNetworkReload)
	{
		bNetworkEmptyReload = bWasEmptyReload;
		GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
			FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::FinishReloadForTransaction, RequestedReloadId),
			SelectedReloadDuration, false);
		PublishNetworkState();
	}
	else
	{
		// 超时 Timer 是动作锁的容错，不替代装填 Notify；动画链断开时不把失败当成补弹成功。
		const float Timeout = FMath::Max(0.01f, FMath::Max(SelectedReloadDuration, Settings.ReloadFailSafeDuration) + Settings.ReloadCompletionGracePeriod);
		GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
			FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::EndReload, RequestedReloadId, EFPReloadEndReason::TimedOut), Timeout, false);
	}
	if (!bNetworkReload || Character->IsLocallyControlled() || Character->UsesNativePresentation())
	{
		// 蓝图明确提交 ReloadId/Mesh/Montage，禁止通过广播前后列表差分猜测动画归属。
		// 原生权威身体也消费同一事实以刷新命中骨骼；专服的枪械/第一人称表现各自在监听入口跳过。
		OnWeaponReloadPlaybackRequested.Broadcast(RequestedReloadId, bWasEmptyReload);
	}

	// true 表示请求曾被接纳；监听者可能已同步结束换弹，广播之后不再写回状态或重设计时器。
	return true;
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
	if (ActiveReloadId == INDEX_NONE || World == nullptr || NetworkState.ReloadDuration <= 0.0f) return 0.0f;
	const AGameStateBase* State = World->GetGameState();
	const double ServerNow = GetNetMode() == NM_Client
		? (State != nullptr ? State->GetServerWorldTimeSeconds() : NetworkState.ReloadStartServerTime)
		: World->GetTimeSeconds();
	return static_cast<float>(FMath::Clamp((ServerNow - NetworkState.ReloadStartServerTime) / NetworkState.ReloadDuration, 0.0, 1.0));
}

// 显式播放并绑定本次换弹的手臂、身体或武器 Montage；单机失败结束事务，联网表现失败不改变服务器结算。
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
		if (GetNetMode() == NM_Standalone) EndReload(ReloadId, EFPReloadEndReason::PlaybackFailed);
		return false;
	}
	// 保留原蓝图的同组互斥：UE 的 true 停同一 Montage Group，不停其他组的表现。
	// ★【重要边界】[回调重入] 播放会打断旧 Montage 并触发回调；随后绑定再次校验本次事务，不仅判断播放返回值。
	// 联网中途收到同一事务时追到当前阶段；只定位表现，不重新启动服务器计时或提交弹药。
	const float StartPosition = GetNetMode() != NM_Standalone ? GetReloadProgress() * Montage->GetPlayLength() : 0.0f;
	const float Duration = Anim->Montage_Play(Montage, PlayRate, EMontagePlayReturnType::Duration, StartPosition, true);
	if (Duration <= 0.0f || !BindReloadMontage(ReloadId, PlaybackMesh, Montage))
	{
		// 成功路径只绑定一次；仅在播放成功但事务已失效时回收本次实例，不停止播放失败前的旧实例。
		FFPActionPlayback UnboundPlayback;
		if (Duration > 0.0f && UnboundPlayback.TryBind(ReloadId, PlaybackMesh, Montage)) UnboundPlayback.Stop();
		if (GetNetMode() == NM_Standalone) EndReload(ReloadId, EFPReloadEndReason::PlaybackFailed);
		return false;
	}
	// 延迟开始/较长素材也有真实播放时间；兜底不能在已接纳播放正常结束前抢先超时。
	const float Remaining = GetWorld()->GetTimerManager().GetTimerRemaining(ReloadTimerHandle);
	const float Timeout = FMath::Max(Remaining, Duration + Settings.ReloadCompletionGracePeriod);
	if (GetNetMode() == NM_Standalone) GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle,
		FTimerDelegate::CreateUObject(this, &UfpstrueWeaponComponent::EndReload, ReloadId, EFPReloadEndReason::TimedOut), FMath::Max(0.01f, Timeout), false);
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
	if (GetNetMode() != NM_Standalone) return; // 联机播放结束只结束表现，动作事实等待服务器复制。
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
	return TEXT("Reload Commit");
}

// 先核对 Notify 的 Mesh、AnimInstance 与播放实例，匹配后才提交当前换弹编号。
bool UfpstrueWeaponComponent::CommitReloadFromNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference)
{
	// 联网包括监听主机与专服均只由服务器 Timer 装弹；新增身体动画不能通过通知抢先结算。
	if (GetNetMode() != NM_Standalone) return false;
	const bool bMatchesPlayback = ReloadPlaybacks.ContainsByPredicate([this, PlaybackMesh, &EventReference](const FFPActionPlayback& Playback)
	{
		return Playback.Matches(ActiveReloadId, PlaybackMesh, EventReference);
	});
	return bMatchesPlayback && CommitReloadForTransaction(ActiveReloadId);
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
	if (GetNetMode() != NM_Standalone && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Reload Ammo=%d Reserve=%d"), CurrentAmmo, ReserveAmmo);
	if (bNetworkEmptyReload && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=AutoReload Ammo=%d Reserve=%d"), CurrentAmmo, ReserveAmmo);
#endif
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
	// 超时只释放动作锁；未发生装填 Notify 的动画失败不能被兜底伪装成补弹成功。
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
	ClearRecoilState();
	StopVisualRecoil();
	CancelReload();
}

// 发布已经提交的弹药快照，HUD 和蓝图只观察结果，不在此重复结算。
void UfpstrueWeaponComponent::BroadcastAmmoChanged()
{
	PublishNetworkState();
	// 弹药状态只由 WeaponComponent 写入；角色、HUD 和蓝图通过该委托读取同一份结果。
	OnAmmoChanged.Broadcast(CurrentAmmo, Settings.MagazineSize, ReserveAmmo);
}
