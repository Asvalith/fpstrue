// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstrueMultiplayerGameInstance.h"
#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpGameState.h"
#include "UI/fpstruePvpHUD.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavigationData.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/App.h"
#include "UnrealClient.h"

// 拥有者网络通道：转交握手、离场和时钟采样；真实恢复裁决仍由 GameMode 完成。

namespace
{
constexpr double TimeSampleFreshnessSeconds = 5.0;
}

// ---- 控制器生命周期 ----

// 客户端进入 Playing 后启动本控制器的时钟采样；显式开发参数才启动验收观察 Timer。
void AfpstruePvpPlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();
	if (IsLocalController() && GetNetMode() == NM_Client)
	{
		ResetTimeSamples();
		RequestClockSample();
		GetWorldTimerManager().SetTimer(ClockSampleTimer, this, &ThisClass::RequestClockSample, 0.5f, true);
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")))
		{
			// 仅显式链路中断验收缩短真实引擎超时；不直接触发失败委托，也不修改正式连接策略。
			if (UNetDriver* Driver = GetWorld()->GetNetDriver())
			{
				Driver->ConnectionTimeout = 4.0f;
				Driver->TimeoutMultiplierForUnoptimizedBuilds = 1.0f;
				Driver->bNoTimeouts = false;
			}
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
			GetWorldTimerManager().SetTimer(SmokeMovementTimer, this, &ThisClass::SampleSmokeMovement, 0.05f, true);
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeVisual")))
			GetWorldTimerManager().SetTimer(VisualRecoveryTimer, this, &ThisClass::CheckVisualRecovery, 0.1f, true);
#endif
	}
}

// 在引擎销毁 Pawn 前选择保留或释放装备；Logout 只负责余下的控制器登记清理。
void AfpstruePvpPlayerController::PawnLeavingGame()
{
	if (auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>())
	{
		if (!bIntentionalLeave && Mode->PreserveDisconnectedPlayer(this)) return;
		// 主动退出或无法保留时，必须在 Super 销毁 Pawn 之前取得其场景拾取枪并清理。
		// Logout 此时可能已经拿不到 Pawn，不能只依赖默认发枪记录兜底。
		Mode->ReleasePlayerWeapon(this);
	}
	Super::PawnLeavingGame();
}

// 控制器退出时停止时钟与验收采样，解除截图回调，不让旧世界继续产生样本。
void AfpstruePvpPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(ClockSampleTimer);
	ResetTimeSamples();
#if !UE_BUILD_SHIPPING
	GetWorldTimerManager().ClearTimer(SmokeMovementTimer);
	SmokeRemoteOrigins.Reset();
	GetWorldTimerManager().ClearTimer(VisualRecoveryTimer);
	FScreenshotRequest::OnScreenshotRequestProcessed().Remove(VisualScreenshotHandle);
#endif
	Super::EndPlay(EndPlayReason);
}

// ---- 所属玩家握手与主动离场 RPC ----

// 由服务器邀请拥有者提交票据；票据只走所属 Controller 的 RPC，不进入 Travel URL 或日志。
void AfpstruePvpPlayerController::ClientBeginConnectionHandshake_Implementation()
{
	if (auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance())) Instance->BeginConnectionHandshake(this);
}

// 将拥有者提交的票据交给 GameMode 唯一验证入口；RPC 本身不恢复对象。
void AfpstruePvpPlayerController::ServerSubmitConnectionToken_Implementation(const FString& Token)
{
	if (auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>()) Mode->CompleteConnectionHandshake(this, Token);
}

// 接收服务器轮换后的票据和身份，继续由 GameInstance 等待复制状态就绪。
void AfpstruePvpPlayerController::ClientConnectionAccepted_Implementation(const FString& Token, int32 InResumeId, bool bRestored)
{
	if (auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance())) Instance->AcceptConnectionToken(this, Token, InResumeId, bRestored);
}

// 将服务器拒绝原因交给本地连接流程，决定重试或返回本地世界。
void AfpstruePvpPlayerController::ClientConnectionRejected_Implementation(const FString& Reason)
{
	if (auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance())) Instance->RejectConnection(Reason);
}

// 主动离场先取消恢复资格，再通知拥有者离开；后续 PawnLeavingGame 不再保留角色。
void AfpstruePvpPlayerController::ServerLeavePvp_Implementation()
{
	bIntentionalLeave = true;
	ClientConnectionRejected(TEXT("manual_leave"));
}

// ---- 射击意图使用的服务器时钟估计 ----

void AfpstruePvpPlayerController::ResetTimeSamples()
{
	PendingClockSamples.Reset();
	bHasClockReplySequence = false;
	SmoothedRoundTripTime = RoundTripVariation = ServerClockOffset = 0.0;
	LastRoundTripSampleLocal = LastClockSampleLocal = -1.0;
	BestClockRoundTrip = TNumericLimits<double>::Max();
	ClockUncertainty = 1.0;
}

// 保存本机单调发送时间并请求服务器时间；丢失样本到期淘汰，未完成请求数量有上限。
void AfpstruePvpPlayerController::RequestClockSample()
{
	if (!IsLocalController() || GetNetMode() != NM_Client) return;
	const double Now = FPlatformTime::Seconds();
	for (auto It = PendingClockSamples.CreateIterator(); It; ++It)
		if (Now - It.Value() > 4.0) It.RemoveCurrent();
	if (PendingClockSamples.Num() >= 8) return;
	PendingClockSamples.Add(++ClockSequence, Now);
	ServerRequestClockSample(ClockSequence);
}

// 限流后回传当前服务器世界时间与原序号，不接收客户端自报的发送时间。
void AfpstruePvpPlayerController::ServerRequestClockSample_Implementation(uint32 Sequence)
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (LastServerClockRequest >= 0.0 && Now - LastServerClockRequest < 0.1) return;
	LastServerClockRequest = Now;
	ClientReceiveClockSample(Sequence, Now);
}

// 同一份有序号的探测分两步使用：RTT 记录拥塞变化；时钟只采纳质量合格的中点。
// RTT / 2 假设上下行对称，无法知道真实单向延迟；因此保留不确定度与服务器独立时间校验。
void AfpstruePvpPlayerController::ClientReceiveClockSample_Implementation(uint32 Sequence, double ServerTime)
{
	double SentAt = 0.0;
	if (!PendingClockSamples.RemoveAndCopyValue(Sequence, SentAt) || !FMath::IsFinite(ServerTime) || ServerTime < 0.0) return;
	const double Now = FPlatformTime::Seconds();
	const double RoundTrip = Now - SentAt;
	if (!FMath::IsFinite(RoundTrip) || RoundTrip < 0.0 || RoundTrip > 2.0) return;
	// pending 身份先消费，重复/未知应答不会更新任何状态。半区间比较支持 uint32 序号回绕；
	// 同时在途最多 8 份，不会跨过半个序号空间。过期只重建估计，不能让旧应答重新变成新样本。
	const uint32 SequenceDelta = Sequence - LastClockReplySequence;
	if (bHasClockReplySequence && (SequenceDelta == 0 || SequenceDelta >= 0x80000000u)) return;
	LastClockReplySequence = Sequence;
	bHasClockReplySequence = true;
	if (LastRoundTripSampleLocal < 0.0 || Now - LastRoundTripSampleLocal > TimeSampleFreshnessSeconds)
	{
		SmoothedRoundTripTime = RoundTrip;
		RoundTripVariation = RoundTrip * 0.5;
	}
	else
	{
		// 先以旧 SRTT 求绝对残差，再平滑均值。排队样本可提高 RTT/波动，不能拖偏下方时钟。
		RoundTripVariation = FMath::Lerp(RoundTripVariation, FMath::Abs(RoundTrip - SmoothedRoundTripTime), 0.25);
		SmoothedRoundTripTime = FMath::Lerp(SmoothedRoundTripTime, RoundTrip, 0.125);
	}
	LastRoundTripSampleLocal = Now;
	// 排队严重的样本不能反过来拉偏较好估计；超时后重新建立基准，避免网络改变后永久拒绝。
	const bool bBaselineExpired = LastClockSampleLocal < 0.0 || Now - LastClockSampleLocal > TimeSampleFreshnessSeconds;
	if (bBaselineExpired) BestClockRoundTrip = TNumericLimits<double>::Max();
	if (RoundTrip > BestClockRoundTrip + 0.1) return;
	const double Offset = ServerTime + RoundTrip * 0.5 - Now;
	// 启动加载可能把首样本 RTT 拉长。明显更短的往返更能约束偏移，不能让旧的低质量
	// 中点继续经四分之一平滑拖延数秒；重定基准也不把旧偏差算进新样本不确定度。
	const bool bRebase = bBaselineExpired || RoundTrip + 0.05 < BestClockRoundTrip;
	const double Residual = bRebase ? 0.0 : FMath::Abs(Offset - ServerClockOffset);
	ServerClockOffset = bRebase ? Offset : FMath::Lerp(ServerClockOffset, Offset, 0.25);
	BestClockRoundTrip = FMath::Min(BestClockRoundTrip, RoundTrip);
	ClockUncertainty = FMath::Clamp(RoundTrip * 0.5 + Residual, 0.001, 1.0);
	LastClockSampleLocal = Now;
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLOCK_SAMPLE PASS RTT=%.4f Uncertainty=%.4f Offset=%.6f Residual=%.6f BestRTT=%.6f Sequence=%u Rebased=%d SRTT=%.6f RTTVAR=%.6f"),
			RoundTrip, ClockUncertainty, ServerClockOffset, Residual, BestClockRoundTrip, Sequence, bRebase, SmoothedRoundTripTime, RoundTripVariation);
#endif
}

// 服务端自身时间始终可用；客户端样本超过五秒未刷新就不再用于射击时间估计。
bool AfpstruePvpPlayerController::HasServerTimeSample() const
{
	return HasAuthority() || (LastClockSampleLocal >= 0.0 && FPlatformTime::Seconds() - LastClockSampleLocal <= TimeSampleFreshnessSeconds);
}

bool AfpstruePvpPlayerController::HasRoundTripTimeSample() const
{
	return LastRoundTripSampleLocal >= 0.0 && FPlatformTime::Seconds() - LastRoundTripSampleLocal <= TimeSampleFreshnessSeconds;
}

double AfpstruePvpPlayerController::GetSmoothedRoundTripTimeSeconds() const
{
	return HasRoundTripTimeSample() ? SmoothedRoundTripTime : 0.0;
}

double AfpstruePvpPlayerController::GetRoundTripVariationSeconds() const
{
	return HasRoundTripTimeSample() ? RoundTripVariation : 0.0;
}

double AfpstruePvpPlayerController::GetEstimatedServerTime() const
{
	if (HasAuthority()) return GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0;
	return HasServerTimeSample() ? FPlatformTime::Seconds() + ServerClockOffset : 0.0;
}

// 返回射击时间校验所需的估计误差；失去新鲜样本时按一秒的不确定度处理。
double AfpstruePvpPlayerController::GetClockUncertaintySeconds() const
{
	return HasAuthority() ? 0.0 : (HasServerTimeSample() ? ClockUncertainty : 1.0);
}

// ---- 显式开发验收的路线与画面观察 ----

void AfpstruePvpPlayerController::ClientBeginNetworkCapacity_Implementation(double CaptureStartServerTime)
{
#if !UE_BUILD_SHIPPING
	if (IsLocalController() && FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity"))
		&& FMath::IsFinite(CaptureStartServerTime) && CaptureStartServerTime > 0.0)
		NetworkCapacityStartServerTime = CaptureStartServerTime;
#endif
}

#if !UE_BUILD_SHIPPING
namespace
{
uint64 GetSmokeKnownNetGuid(const AActor* Actor)
{
	const UNetDriver* Driver = IsValid(Actor) ? Actor->GetNetDriver() : nullptr;
	return Driver != nullptr && Driver->GuidCache.IsValid() ? Driver->GuidCache->GetNetGUID(Actor).ObjectId : 0;
}
}

void AfpstruePvpPlayerController::SetSmokeMovingHitTarget(AfpstrueCharacter* Target)
{
	if (!IsLocalController() || HasAuthority() || !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit"))) return;
	SmokeMovingHitTarget = Target;
	SmokeMovingHitWeapon.Reset();
	SmokeMovingHitGeneration = SmokeMovingHitSession = SmokeMovingHitShotId = 0;
	bSmokeMovingFirstShotAccepted = false;
	bSmokeMovingFirstNativeView = false;
}

bool AfpstruePvpPlayerController::GetSmokeMovingHitAim(FVector& OutPoint, FName& OutBone, uint8& OutShapeType) const
{
	const AfpstrueCharacter* Target = SmokeMovingHitTarget.Get();
	const USkeletalMeshComponent* Mesh = IsValid(Target) ? Target->GetMesh() : nullptr;
	const UPhysicsAsset* Asset = Mesh != nullptr ? Mesh->GetPhysicsAsset() : nullptr;
	if (!IsLocalController() || HasAuthority() || Asset == nullptr
		|| !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit"))) return false;
	// 与权威历史使用同一实际 Query 形状及缩放变换；骨骼 pivot 不一定处于身体形状内部。
	for (const FName Bone : {FName(TEXT("spine_03")), FName(TEXT("spine_02")), FName(TEXT("spine_01"))})
	{
		const int32 BoneIndex = Mesh->GetBoneIndex(Bone);
		if (BoneIndex == INDEX_NONE) continue;
		FTransform BoneToWorld = Mesh->GetBoneTransform(BoneIndex);
		const FVector Scale = BoneToWorld.GetScale3D();
		BoneToWorld.SetScale3D(FVector::OneVector);
		for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
		{
			if (Body == nullptr || Body->BoneName != Bone) continue;
			for (const FKBoxElem& Element : Body->AggGeom.BoxElems)
				if (CollisionEnabledHasQuery(Element.GetCollisionEnabled()))
				{
					OutPoint = (Element.GetFinalScaled(Scale, FTransform::Identity).GetTransform() * BoneToWorld).GetLocation();
					OutBone = Bone; OutShapeType = 0; return true;
				}
			for (const FKSphereElem& Element : Body->AggGeom.SphereElems)
				if (CollisionEnabledHasQuery(Element.GetCollisionEnabled()))
				{
					OutPoint = (Element.GetFinalScaled(Scale, FTransform::Identity).GetTransform() * BoneToWorld).GetLocation();
					OutBone = Bone; OutShapeType = 1; return true;
				}
			for (const FKSphylElem& Element : Body->AggGeom.SphylElems)
				if (CollisionEnabledHasQuery(Element.GetCollisionEnabled()))
				{
					OutPoint = (Element.GetFinalScaled(Scale, FTransform::Identity).GetTransform() * BoneToWorld).GetLocation();
					OutBone = Bone; OutShapeType = 2; return true;
				}
		}
	}
	return false;
}

void AfpstruePvpPlayerController::RecordSmokeMovingShotInput(const FFPShotRequest& Request)
{
	AfpstrueCharacter* Target = SmokeMovingHitTarget.Get();
	if (!IsLocalController() || HasAuthority() || SmokeMovingHitShotId != 0 || !IsValid(Target)
		|| !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit")) || Request.Avatar != GetPawn()) return;
	FVector AimPoint;
	FName AimBone;
	uint8 ShapeType = 255;
	if (!GetSmokeMovingHitAim(AimPoint, AimBone, ShapeType)) return;
	SmokeMovingHitWeapon = Request.Weapon.Get();
	SmokeMovingHitGeneration = Request.EquipmentGeneration;
	SmokeMovingHitSession = Request.FireSession;
	SmokeMovingHitShotId = Request.ShotId;
	const double TargetViewTime = Request.GetTargetViewTime(Target, 0.0, &bSmokeMovingFirstNativeView);
	const double ViewAgeMs = bSmokeMovingFirstNativeView ? (Request.ServerFireTime - TargetViewTime) * 1000.0 : -1.0;
	const FVector ViewOrigin = GetPawn()->GetPawnViewLocation();
	const TCHAR* ShapeName = ShapeType == 0 ? TEXT("Box") : ShapeType == 1 ? TEXT("Sphere") : TEXT("Sphyl");
	UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_AIM ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu TargetNetGuid=%llu FireTime=%.6f EstimatedServerTime=%.6f ClockUncertaintyMs=%.3f TargetViewTime=%.6f ViewAgeMs=%.3f NativeView=%d TargetSpeed=%.2f AimBone=%s ShapeType=%s AimPoint=%s ViewOrigin=%s TargetVelocity=%s AimDirection=%s"),
		Request.ShotId, Request.FireSession, Request.EquipmentGeneration, GetSmokeKnownNetGuid(Request.Weapon), GetSmokeKnownNetGuid(Target),
		Request.ServerFireTime, GetEstimatedServerTime(), GetClockUncertaintySeconds() * 1000.0,
		TargetViewTime, ViewAgeMs, bSmokeMovingFirstNativeView, Target->GetVelocity().Size2D(),
		*AimBone.ToString(), ShapeName, *AimPoint.ToCompactString(), *ViewOrigin.ToCompactString(),
		*Target->GetVelocity().ToCompactString(), *FVector(Request.AimDirection).ToCompactString());
}

void AfpstruePvpPlayerController::RecordSmokeShotVerdict(AActor* Weapon, uint32 Generation, uint32 Session, uint32 ShotId, bool bAccepted)
{
	if (!IsLocalController() || HasAuthority() || !FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit"))) return;
	if (Weapon == SmokeMovingHitWeapon.Get() && Generation == SmokeMovingHitGeneration
		&& Session == SmokeMovingHitSession && ShotId == SmokeMovingHitShotId)
		bSmokeMovingFirstShotAccepted = bAccepted;
}

bool AfpstruePvpPlayerController::LogSmokeMovingHitIfReplicated(AfpstrueCharacter* Target, float InitialHealth) const
{
	if (!IsLocalController() || HasAuthority() || !bSmokeMovingFirstShotAccepted || !bSmokeMovingFirstNativeView || !IsValid(Target)
		|| Target != SmokeMovingHitTarget.Get() || !SmokeMovingHitWeapon.IsValid() || InitialHealth <= Target->GetCurrentHealth()) return false;
	UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HIT_CLIENT PASS ShotId=%u Session=%u Generation=%u WeaponNetGuid=%llu TargetNetGuid=%llu HealthBefore=%.2f HealthAfter=%.2f"),
		SmokeMovingHitShotId, SmokeMovingHitSession, SmokeMovingHitGeneration, GetSmokeKnownNetGuid(SmokeMovingHitWeapon.Get()),
		GetSmokeKnownNetGuid(Target), InitialHealth, Target->GetCurrentHealth());
	return true;
}
#endif

// 仅开发验收保存服务器规划的有界路线；输入驱动器随后仍通过正常移动输入执行。
void AfpstruePvpPlayerController::ClientNormalVisualRoute_Implementation(const TArray<FVector>& Points)
{
#if !UE_BUILD_SHIPPING
	if (!IsLocalController() || !FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPairWalk")) || Points.IsEmpty() || Points.Num() > 128) return;
	NormalVisualRoute = Points;
	++NormalVisualRouteRevision;
	UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE CLIENT Points=%d Revision=%d"), NormalVisualRoute.Num(), NormalVisualRouteRevision);
#endif
}

// 路线规划只属于显式开发验收驱动；实际行走和攻击仍走原有 CMC/GAS 协议。
void AfpstruePvpPlayerController::ServerRequestNormalVisualEnemyRoute_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay"))) return;
	auto* Avatar = Cast<AfpstrueCharacter>(GetPawn());
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!Avatar || Avatar->IsDead() || !Avatar->HasEquippedWeapon() || !Navigation) return;
	bNormalFullRouteRequested = true;
	TArray<AfpstruePvpPlayerController*> Ready;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		if (auto* Peer = Cast<AfpstruePvpPlayerController>(It->Get()); Peer && Peer->bNormalFullRouteRequested) Ready.Add(Peer);
	if (Ready.Num() != 2) return; // 双端完成常规动作后才一起出发，不能一端提前杀掉另一端尚未登记的目标。
	AfpstrueEnemyCharacter* Nearest = nullptr;
	double Distance = TNumericLimits<double>::Max();
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
		if (!It->IsDead() && FVector::DistSquared2D(Avatar->GetActorLocation(), It->GetActorLocation()) < Distance)
		{ Nearest = *It; Distance = FVector::DistSquared2D(Avatar->GetActorLocation(), It->GetActorLocation()); }
	if (!Nearest) return;
	// 显式开发验收读回服务器活体 Pawn 与 CMC 的尺寸及实际 NavData，不手工改任何导航/碰撞属性。
	auto LogNavigationCoverage = [Navigation](ACharacter* MovingPawn)
	{
		const auto* Movement = MovingPawn->GetCharacterMovement();
		const auto* Capsule = MovingPawn->GetCapsuleComponent();
		const FNavAgentProperties& PawnProps = MovingPawn->GetNavAgentPropertiesRef();
		const FNavAgentProperties& MovementProps = Movement->GetNavAgentPropertiesRef();
		const ANavigationData* Selected = Navigation->GetNavDataForProps(PawnProps, MovingPawn->GetActorLocation());
		const FNavDataConfig* Config = Selected ? &Selected->GetConfig() : nullptr;
		const float Radius = Capsule->GetScaledCapsuleRadius(), Height = Capsule->GetScaledCapsuleHalfHeight() * 2.0f;
		const bool bCovered = Config && Config->AgentRadius >= Radius && Config->AgentHeight >= Height
			&& PawnProps.AgentRadius >= Radius && PawnProps.AgentHeight >= Height;
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY NAV_PAWN Pawn=%s Capsule=%.1f/%.1f PawnProps=%.1f/%.1f CMCProps=%.1f/%.1f UpdateFromCollision=%d Selected=%s Config=%.1f/%.1f Extent=%s Covered=%d"),
			*MovingPawn->GetName(), Radius, Height, PawnProps.AgentRadius, PawnProps.AgentHeight, MovementProps.AgentRadius, MovementProps.AgentHeight,
			Movement->ShouldUpdateNavAgentWithOwnersCollision(), *GetNameSafe(Selected), Config ? Config->AgentRadius : -1.0f,
			Config ? Config->AgentHeight : -1.0f, Config ? *Config->DefaultQueryExtent.ToCompactString() : TEXT("None"), bCovered);
		return bCovered;
	};
	if (!LogNavigationCoverage(Nearest))
	{ UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE FAIL Reason=EnemyNavAgentCoverage")); return; }
	int32 RouteSlot = 0;
	for (AfpstruePvpPlayerController* Peer : Ready)
	{
		auto* MovingPawn = Cast<AfpstrueCharacter>(Peer->GetPawn());
		if (!MovingPawn || MovingPawn->IsDead()) return;
		if (!LogNavigationCoverage(MovingPawn))
		{ UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_ROUTE FAIL Reason=PlayerNavAgentCoverage")); return; }
		const FVector Direction = (MovingPawn->GetActorLocation() - Nearest->GetActorLocation()).GetSafeNormal2D().RotateAngleAxis(RouteSlot++ == 0 ? 35.0 : -35.0, FVector::UpVector);
		FNavLocation Destination;
		if (!Navigation->ProjectPointToNavigation(Nearest->GetActorLocation() + Direction * 500.0, Destination, FVector(150,150,200))) return;
		UNavigationPath* Path = Navigation->FindPathToLocationSynchronously(GetWorld(), MovingPawn->GetActorLocation(), Destination.Location, MovingPawn);
		if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.IsEmpty() || Path->PathPoints.Num() > 128)
		{ UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ROUTE_FAIL Player=%s"), *Peer->GetName()); return; }
		Peer->ClientNormalVisualRoute(Path->PathPoints);
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_ROUTE Player=%s Enemy=%s Points=%d DistanceCm=%.1f Teleport=0"),
			*Peer->GetName(), *Nearest->GetName(), Path->PathPoints.Num(), FVector::Dist2D(MovingPawn->GetActorLocation(), Nearest->GetActorLocation()));
	}
#endif
}

// 将开发验收的阶段请求转交服务器 GameMode，不直接生成敌人。
void AfpstruePvpPlayerController::ServerBeginPveSmoke_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (auto* Mode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>()) Mode->StartPveSmoke(this);
#endif
}

// 服务器确认敌人真实命中后，才允许本地烟测驱动器反击。
void AfpstruePvpPlayerController::ClientAllowPveSmokeCounterattack_Implementation()
{
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokePve"))) bSmokePveCounterattackReady = true;
#endif
}

#if !UE_BUILD_SHIPPING
void AfpstruePvpPlayerController::SampleSmokeMovement()
{
	if (bSmokeRemoteMovementReported || GetWorld() == nullptr) return;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsLocallyControlled() || It->IsDead()) continue;
		const TWeakObjectPtr<AfpstrueCharacter> Key(*It);
		const FVector Location = It->GetActorLocation();
		if (const FVector* Origin = SmokeRemoteOrigins.Find(Key))
		{
			const float Distance = FVector::Dist2D(*Origin, Location);
			if (Distance >= 5.0f)
			{
				bSmokeRemoteMovementReported = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE CLIENT_REMOTE_DISPLACEMENT PASS Pawn=%s Distance=%.2f"), *It->GetName(), Distance);
				GetWorldTimerManager().ClearTimer(SmokeMovementTimer);
				break;
			}
		}
		else SmokeRemoteOrigins.Add(Key, Location);
	}
}
#endif

#if !UE_BUILD_SHIPPING
// 开发验收只等待真实视口恢复，不授权连接、不修改伤害或角色状态。
void AfpstruePvpPlayerController::CheckVisualRecovery()
{
	if (!IsLocalController() || !FApp::CanEverRender() || bVisualCaptureRequested) return;
	auto* Instance = Cast<UfpstrueMultiplayerGameInstance>(GetGameInstance());
	auto* LocalPawn = Cast<AfpstrueCharacter>(GetPawn());
	auto* HUD = Cast<AfpstruePvpHUD>(GetHUD());
	auto* State = GetWorld()->GetGameState<AfpstruePvpGameState>();
	auto* Weapon = LocalPawn != nullptr ? LocalPawn->GetEquippedWeaponComponent() : nullptr;
	auto* Viewport = GetWorld()->GetGameViewport();
	const auto* Arms = LocalPawn != nullptr ? LocalPawn->GetMesh1P() : nullptr;
	// 正常出生可以空手；只有实际装备后才要求视模可见，不能把隐藏双手误判为画面未恢复。
	const bool bEquipmentReady = Arms != nullptr && Arms->IsRegistered()
		&& (Weapon == nullptr ? Arms->bHiddenInGame
			: !Arms->bHiddenInGame && Arms->IsVisible() && Weapon->IsRegistered() && Weapon->IsVisible()
				&& !Weapon->bHiddenInGame && Weapon->GetSkeletalMeshAsset() != nullptr && Weapon->GetAttachParent() == Arms);
	if (Instance == nullptr || Instance->ConnectionState != EFPConnectionState::Connected || LocalPawn == nullptr || LocalPawn->IsDead()
		|| GetViewTarget() != LocalPawn || PlayerCameraManager == nullptr || !FMath::IsFinite(PlayerCameraManager->GetFOVAngle())
		|| LocalPawn->GetFirstPersonCameraComponent() == nullptr || !LocalPawn->GetFirstPersonCameraComponent()->IsActive()
		|| !bEquipmentReady || HUD == nullptr || !HUD->HasDrawnCurrentState(LocalPawn)
		|| Viewport == nullptr || Viewport->Viewport == nullptr || Viewport->Viewport->GetSizeXY().GetMin() <= 0
		|| State == nullptr || !State->GetEnemySignificanceCoordinator()->HasRenderedEnemies())
	{ VisualReadyAt = 0.0; return; }
	bool bHasEnemyReplica = false;
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
	{
		const auto* Mesh = It->GetMesh();
		if (!It->HasAuthority() && Mesh != nullptr && Mesh->IsRegistered() && Mesh->IsVisible() && Mesh->GetSkeletalMeshAsset() != nullptr)
		{ bHasEnemyReplica = true; break; }
	}
	if (!bHasEnemyReplica) { VisualReadyAt = 0.0; return; }
	// 等待若干实际渲染帧，让相机、骨骼与 HUD 在状态就绪后参与绘制。
	if (VisualReadyAt == 0.0) { VisualReadyAt = FPlatformTime::Seconds(); return; }
	if (FPlatformTime::Seconds() - VisualReadyAt < 0.4 || FScreenshotRequest::IsScreenshotRequested()) return;
	FString Directory;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), Directory) || Directory.IsEmpty()) return;
	IFileManager::Get().MakeDirectory(*Directory, true);
	VisualResumeId = Instance->ResumeId;
	bVisualRestored = Instance->bRestoredConnection;
	VisualScreenshotPath = FPaths::Combine(Directory, bVisualRestored ? TEXT("restored.png") : TEXT("initial.png"));
	UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_READY PASS Restored=%d ResumeId=%d"), bVisualRestored, VisualResumeId);
	VisualScreenshotHandle = FScreenshotRequest::OnScreenshotRequestProcessed().AddUObject(this, &ThisClass::HandleVisualScreenshotProcessed);
	bVisualCaptureRequested = true;
	FScreenshotRequest::RequestScreenshot(VisualScreenshotPath, true, false, false);
}
#endif

#if !UE_BUILD_SHIPPING
// 请求处理结束后仍检查实际 PNG 文件，发出请求不等于截图成功。
void AfpstruePvpPlayerController::HandleVisualScreenshotProcessed()
{
	FScreenshotRequest::OnScreenshotRequestProcessed().Remove(VisualScreenshotHandle);
	VisualScreenshotHandle.Reset();
	GetWorldTimerManager().ClearTimer(VisualRecoveryTimer);
	TArray<uint8> Header;
	if (!FFileHelper::LoadFileToArray(Header, *VisualScreenshotPath) || Header.Num() < 24
		|| Header[0] != 137 || Header[1] != 80 || Header[2] != 78 || Header[3] != 71)
	{
		UE_LOG(LogTemp, Error, TEXT("PVP_VISUAL_CAPTURE FAIL Reason=missing_png"));
		return;
	}
	const uint32 Width = (uint32(Header[16]) << 24) | (uint32(Header[17]) << 16) | (uint32(Header[18]) << 8) | Header[19];
	const uint32 Height = (uint32(Header[20]) << 24) | (uint32(Header[21]) << 16) | (uint32(Header[22]) << 8) | Header[23];
	UE_LOG(LogTemp, Display, TEXT("PVP_VISUAL_CAPTURE PASS Restored=%d ResumeId=%d File=%s Width=%u Height=%u"),
		bVisualRestored, VisualResumeId, *FPaths::GetCleanFilename(VisualScreenshotPath), Width, Height);
	if (bVisualRestored && FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")))
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE LIVE_RECONNECT RenderedReady Pid=%u ResumeId=%d Width=%u Height=%u"),
			FPlatformProcess::GetCurrentProcessId(), VisualResumeId, Width, Height);
}
#endif
