// Copyright Epic Games, Inc. All Rights Reserved.
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Runtime/fpstrueRuntime.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// ==================== 权威身体历史与纯查询命中 ====================

// 所有 Actor Tick 和骨骼/世界变换提交后最多 60 Hz 写入固定环；传送、死亡或拓扑变化断代。
// 【设计取舍】只回查 PvP 玩家历史几何，不暂移真实 Pawn/Chaos，避免伤害回调看到回滚中的世界。
// 环境遮挡仍查当前碰撞，动态掩体历史未被重建；这是有界命中补偿，不是全世界回滚。

void AfpstrueCharacter::ResetHitHistory(bool bReleaseMemory)
{
	HitHistoryCount = HitHistoryNext = 0;
	++HitHistoryEpoch;
	for (FHitHistoryFrame& Frame : HitHistory)
	{
		if (bReleaseMemory) Frame.Shapes.Empty();
		else Frame.Shapes.Reset();
	}
	if (bReleaseMemory) RecordedPhysicsAsset.Reset();
}

SIZE_T AfpstrueCharacter::GetHitHistoryAllocatedBytes() const
{
	SIZE_T Bytes = 0;
	for (const FHitHistoryFrame& Frame : HitHistory) Bytes += Frame.Shapes.GetAllocatedSize();
	return Bytes;
}

int32 AfpstrueCharacter::GetHitHistoryReservedShapeCount() const
{
	int32 Shapes = 0;
	for (const FHitHistoryFrame& Frame : HitHistory) Shapes += Frame.Shapes.Max();
	return Shapes;
}

SIZE_T AfpstrueCharacter::GetHitHistoryShapeStride()
{
	return sizeof(FHistoricalShape);
}

bool AfpstrueCharacter::TeleportTo(const FVector& DestLocation, const FRotator& DestRotation, bool bIsATest, bool bNoCheck)
{
	const bool bMoved = Super::TeleportTo(DestLocation, DestRotation, bIsATest, bNoCheck);
	if (bMoved && !bIsATest) ResetHitHistory();
	return bMoved;
}

void AfpstrueCharacter::RecordServerHitHistory()
{
	if (!HasAuthority() || !CanMaintainEquipment() || GetWorld() == nullptr) return;
	USkeletalMeshComponent* BodyMesh = GetMesh();
	UPhysicsAsset* Asset = BodyMesh != nullptr ? BodyMesh->GetPhysicsAsset() : nullptr;
	if (Asset == nullptr)
	{
		if (HitHistoryCount > 0 || GetHitHistoryAllocatedBytes() > 0) ResetHitHistory(true);
		RecordedPhysicsAsset.Reset();
		if (!bLoggedUnsupportedHitGeometry)
			UE_LOG(LogTemplateCharacter, Warning, TEXT("%s has no PhysicsAsset; historical player hits are disabled, not replaced by the movement capsule."), *GetName());
		bLoggedUnsupportedHitGeometry = true;
		return;
	}
	if (RecordedPhysicsAsset.Get() != Asset)
	{
		ResetHitHistory();
		RecordedPhysicsAsset = Asset;
		// 只在物理资产变更时建立容量；32 个循环帧按真实有效形状预留，不按上限各占 64 条。
		int32 ShapeCapacity = 0;
		for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
		{
			if (Body == nullptr || BodyMesh->GetBoneIndex(Body->BoneName) == INDEX_NONE) continue;
			for (const FKBoxElem& Shape : Body->AggGeom.BoxElems) ShapeCapacity += CollisionEnabledHasQuery(Shape.GetCollisionEnabled()) ? 1 : 0;
			for (const FKSphereElem& Shape : Body->AggGeom.SphereElems) ShapeCapacity += CollisionEnabledHasQuery(Shape.GetCollisionEnabled()) ? 1 : 0;
			for (const FKSphylElem& Shape : Body->AggGeom.SphylElems) ShapeCapacity += CollisionEnabledHasQuery(Shape.GetCollisionEnabled()) ? 1 : 0;
		}
		ShapeCapacity = FMath::Min(ShapeCapacity, MaxHistoricalShapes);
		for (FHitHistoryFrame& HistoryFrame : HitHistory) HistoryFrame.Shapes.Empty(ShapeCapacity);
	}
	const double Now = GetWorld()->GetTimeSeconds();
	if (HitHistoryCount > 0)
	{
		const FHitHistoryFrame& Last = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
		const double Elapsed = Now - Last.ServerTime;
		// SetActorLocation 一类未走 TeleportTo 的不连续位移也不允许插值出一条虚构的身体轨迹。
		const double AllowedDistance = FMath::Max(200.0, GetCharacterMovement()->GetMaxSpeed() * FMath::Max(Elapsed, 0.0) * 3.0 + 40.0);
		if (Elapsed < 0.0 || FVector::DistSquared(Last.ActorLocation, GetActorLocation()) > FMath::Square(AllowedDistance)) ResetHitHistory();
		// 世界时间存为 float；只给采样间隔 0.1ms 数值容差，避免正常 60Hz 被误判成尚未到时而隔帧记录。
		else if (Elapsed + 0.0001 < 1.0 / 60.0) return;
	}
	FHitHistoryFrame& Frame = HitHistory[HitHistoryNext];
	Frame.ServerTime = Now;
	Frame.ActorLocation = GetActorLocation();
	FRotator ViewRotation;
	GetActorEyesViewPoint(Frame.ViewOrigin, ViewRotation);
	Frame.Shapes.Reset();
	bool bUnsupported = false;
	for (const USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
	{
		if (Body == nullptr) continue;
		const int32 BoneIndex = BodyMesh->GetBoneIndex(Body->BoneName);
		if (BoneIndex == INDEX_NONE) continue;
		FTransform BoneToWorld = BodyMesh->GetBoneTransform(BoneIndex);
		const FVector Scale = BoneToWorld.GetScale3D();
		BoneToWorld.SetScale3D(FVector::OneVector);
		const auto AddShape = [&](const FTransform& Local, const FVector& Extent, uint8 Type)
		{
			if (Frame.Shapes.Num() >= MaxHistoricalShapes) { bUnsupported = true; return; }
			FHistoricalShape& Shape = Frame.Shapes.AddDefaulted_GetRef();
			const FTransform Pose = Local * BoneToWorld;
			Shape.Rotation = Pose.GetRotation();
			Shape.Location = Pose.GetLocation();
			Shape.Extent = Extent;
			Shape.Bone = Body->BoneName;
			Shape.Type = Type;
		};
		for (const FKBoxElem& Element : Body->AggGeom.BoxElems)
		{
			if (!CollisionEnabledHasQuery(Element.GetCollisionEnabled())) continue;
			const FKBoxElem Scaled = Element.GetFinalScaled(Scale, FTransform::Identity);
			AddShape(Scaled.GetTransform(), FVector(Scaled.X, Scaled.Y, Scaled.Z) * 0.5, 0);
		}
		for (const FKSphereElem& Element : Body->AggGeom.SphereElems)
		{
			if (!CollisionEnabledHasQuery(Element.GetCollisionEnabled())) continue;
			const FKSphereElem Scaled = Element.GetFinalScaled(Scale, FTransform::Identity);
			AddShape(Scaled.GetTransform(), FVector(Scaled.Radius, 0.0, 0.0), 1);
		}
		for (const FKSphylElem& Element : Body->AggGeom.SphylElems)
		{
			if (!CollisionEnabledHasQuery(Element.GetCollisionEnabled())) continue;
			const FKSphylElem Scaled = Element.GetFinalScaled(Scale, FTransform::Identity);
			AddShape(Scaled.GetTransform(), FVector(Scaled.Radius, 0.0, Scaled.Length * 0.5), 2);
		}
		// 不用凸体的包围盒冒充精确碰撞；更换 PhysicsAsset 后应补支持或改成明确的球/盒/胶囊。
		bUnsupported |= Body->AggGeom.ConvexElems.Num() > 0 || Body->AggGeom.TaperedCapsuleElems.Num() > 0;
	}
	if ((bUnsupported || Frame.Shapes.IsEmpty()) && !bLoggedUnsupportedHitGeometry)
	{
		UE_LOG(LogTemplateCharacter, Warning, TEXT("%s historical hit geometry: %d supported shapes (limit %d); unsupported convex/tapered or excess shapes are NOT approximated."),
			*GetName(), Frame.Shapes.Num(), MaxHistoricalShapes);
		bLoggedUnsupportedHitGeometry = true;
	}
	if (HitHistoryCount > 0)
	{
		const auto& Last = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
		bool bSameTopology = Last.Shapes.Num() == Frame.Shapes.Num();
		for (int32 I = 0; bSameTopology && I < Frame.Shapes.Num(); ++I)
			bSameTopology = Last.Shapes[I].Bone == Frame.Shapes[I].Bone && Last.Shapes[I].Type == Frame.Shapes[I].Type;
		if (!bSameTopology) { HitHistoryCount = 0; ++HitHistoryEpoch; }
	}
	HitHistoryNext = (HitHistoryNext + 1) % HitHistoryCapacity;
	HitHistoryCount = FMath::Min(HitHistoryCount + 1, HitHistoryCapacity);
}

bool AfpstrueCharacter::FindHitHistoryFrames(double ServerTime, const FHitHistoryFrame*& Older,
	const FHitHistoryFrame*& Newer, float& Alpha) const
{
	Older = Newer = nullptr;
	Alpha = 0.0f;
	if (!HasAuthority() || !CanMaintainEquipment() || HitHistoryCount == 0 || !FMath::IsFinite(ServerTime)) return false;
	const double Now = GetWorld()->GetTimeSeconds();
	if (ServerTime < Now - MaxHitHistorySeconds || ServerTime > Now + 0.025) return false;
	const int32 Oldest = (HitHistoryNext + HitHistoryCapacity - HitHistoryCount) % HitHistoryCapacity;
	const FHitHistoryFrame& First = HitHistory[Oldest];
	const FHitHistoryFrame& Last = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
	if (ServerTime < First.ServerTime - UE_DOUBLE_SMALL_NUMBER || ServerTime > Last.ServerTime + 0.025) return false;
	if (ServerTime >= Last.ServerTime) { Older = Newer = &Last; return true; }
	for (int32 I = 1; I < HitHistoryCount; ++I)
	{
		const FHitHistoryFrame& Next = HitHistory[(Oldest + I) % HitHistoryCapacity];
		if (Next.ServerTime >= ServerTime)
		{
			Older = &HitHistory[(Oldest + I - 1) % HitHistoryCapacity];
			Newer = &Next;
			Alpha = static_cast<float>((ServerTime - Older->ServerTime) / (Next.ServerTime - Older->ServerTime));
			return true;
		}
	}
	Older = Newer = &First;
	return true;
}

bool AfpstrueCharacter::GetHistoricalViewOrigin(double ServerTime, FVector& OutOrigin) const
{
	const FHitHistoryFrame* A;
	const FHitHistoryFrame* B;
	float Alpha;
	if (!FindHitHistoryFrames(ServerTime, A, B, Alpha))
	{
		// NetDispatch 可早于本帧骨骼采样。仅射手视点允许夹到上一帧已记录位置，不给目标命中盒外推。
		// 空历史/Teleport 断代不能借旧位置；也不将一帧宽限扩展成接受任意未来或过旧请求。
		if (!HasAuthority() || !CanMaintainEquipment() || GetWorld() == nullptr || HitHistoryCount == 0 || !FMath::IsFinite(ServerTime)) return false;
		const double Now = GetWorld()->GetTimeSeconds();
		const FHitHistoryFrame& Latest = HitHistory[(HitHistoryNext + HitHistoryCapacity - 1) % HitHistoryCapacity];
		const double FrameTolerance = FMath::Min(0.05, FMath::Max(0.0, static_cast<double>(GetWorld()->GetDeltaSeconds())) + 0.0001);
		if (ServerTime < Latest.ServerTime || ServerTime > Now + 0.025 || ServerTime < Now - MaxHitHistorySeconds
			|| ServerTime - Latest.ServerTime > FrameTolerance || Now - Latest.ServerTime > FrameTolerance) return false;
		OutOrigin = Latest.ViewOrigin;
		return true;
	}
	OutOrigin = FMath::Lerp(A->ViewOrigin, B->ViewOrigin, Alpha);
	return true;
}

bool AfpstrueCharacter::TraceHistoricalHit(double ServerTime, const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	OutHit = FHitResult(Start, End);
	const FHitHistoryFrame* A;
	const FHitHistoryFrame* B;
	float Alpha;
	if (Start.ContainsNaN() || End.ContainsNaN() || !FindHitHistoryFrames(ServerTime, A, B, Alpha)) return false;
	const FVector Ray = End - Start;
	if (Ray.IsNearlyZero()) return false;
#if !UE_BUILD_SHIPPING
	// 专项只观测真实几何与时间；骨骼 pivot 不一定落在 PhysicsAsset 的查询形状中心。
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeMovingHit")))
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HISTORY Target=%s QueryTime=%.6f OlderTime=%.6f NewerTime=%.6f Alpha=%.6f Start=%s ActorNow=%s MeshNow=%s SmoothTime=%.6f Shapes=%d"),
			*GetName(), ServerTime, A->ServerTime, B->ServerTime, Alpha, *Start.ToCompactString(),
			*GetActorLocation().ToCompactString(), *GetMesh()->GetComponentLocation().ToCompactString(),
			GetCharacterMovement()->NetworkSimulatedSmoothLocationTime, A->Shapes.Num());
		for (int32 Index = 0; Index < A->Shapes.Num(); ++Index)
		{
			const auto& Shape = A->Shapes[Index];
			if (!Shape.Bone.ToString().StartsWith(TEXT("spine")) && Shape.Bone != TEXT("pelvis")) continue;
			const FVector Center = FMath::Lerp(Shape.Location, B->Shapes[Index].Location, Alpha);
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE MOVING_HISTORY_SHAPE Target=%s Bone=%s Type=%d Center=%s Extent=%s PivotNow=%s RayCenterDistance=%.4f"),
				*GetName(), *Shape.Bone.ToString(), Shape.Type, *Center.ToCompactString(), *Shape.Extent.ToCompactString(),
				*GetMesh()->GetSocketLocation(Shape.Bone).ToCompactString(), FVector::Distance(Center, FMath::ClosestPointOnSegment(Center, Start, End)));
		}
	}
#endif
	double Best = 1.0 + UE_DOUBLE_SMALL_NUMBER;
	for (int32 I = 0; I < A->Shapes.Num(); ++I)
	{
		const auto& Older = A->Shapes[I];
		const auto& Newer = B->Shapes[I];
		const FTransform Pose(FQuat::Slerp(Older.Rotation, Newer.Rotation, Alpha).GetNormalized(),
			FMath::Lerp(Older.Location, Newer.Location, Alpha));
		const FVector Extent = FMath::Lerp(Older.Extent, Newer.Extent, Alpha);
		const FVector Origin = Pose.InverseTransformPositionNoScale(Start);
		const FVector Delta = Pose.InverseTransformVectorNoScale(Ray);
		double T = 2.0;
		FVector Normal = FVector::ZeroVector;
		const auto Sphere = [&](const FVector& Center, double Radius)
		{
			const FVector Offset = Origin - Center;
			const double C = Offset.SquaredLength() - Radius * Radius;
			const double Dot = FVector::DotProduct(Offset, Delta);
			const double LengthSquared = Delta.SquaredLength();
			const double Discriminant = Dot * Dot - LengthSquared * C;
			if (Discriminant < 0.0) return;
			const double Hit = C <= 0.0 ? 0.0 : (-Dot - FMath::Sqrt(Discriminant)) / LengthSquared;
			if (Hit >= 0.0 && Hit <= 1.0 && Hit < T)
			{
				T = Hit;
				Normal = (Origin + Delta * T - Center).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, -Delta.GetSafeNormal());
			}
		};
		if (Older.Type == 0)
		{
			double Near = 0.0, Far = 1.0;
			FVector NearNormal = -Delta.GetSafeNormal();
			for (int32 Axis = 0; Axis < 3 && Near <= Far; ++Axis)
			{
				if (FMath::Abs(Delta[Axis]) < UE_DOUBLE_SMALL_NUMBER)
				{
					if (FMath::Abs(Origin[Axis]) > Extent[Axis]) Near = 2.0;
					continue;
				}
				double Enter = (-Extent[Axis] - Origin[Axis]) / Delta[Axis];
				double Exit = (Extent[Axis] - Origin[Axis]) / Delta[Axis];
				const double Sign = Delta[Axis] > 0.0 ? -1.0 : 1.0;
				if (Enter > Exit) Swap(Enter, Exit);
				if (Enter > Near) { Near = Enter; NearNormal = FVector::ZeroVector; NearNormal[Axis] = Sign; }
				Far = FMath::Min(Far, Exit);
			}
			if (Near >= 0.0 && Near <= Far && Near <= 1.0) { T = Near; Normal = NearNormal; }
		}
		else if (Older.Type == 1) Sphere(FVector::ZeroVector, Extent.X);
		else
		{
			// Capsule 是 Z 轴线段与半径球的 Minkowski 和；圆柱侧面与两端球分别取最近交点。
			const double Radius = Extent.X, Half = Extent.Z;
			const FVector NearestAxis(0.0, 0.0, FMath::Clamp(Origin.Z, -Half, Half));
			if ((Origin - NearestAxis).SquaredLength() <= Radius * Radius) { T = 0.0; Normal = -Delta.GetSafeNormal(); }
			const double AA = Delta.X * Delta.X + Delta.Y * Delta.Y;
			const double BB = Origin.X * Delta.X + Origin.Y * Delta.Y;
			const double CC = Origin.X * Origin.X + Origin.Y * Origin.Y - Radius * Radius;
			const double Discriminant = BB * BB - AA * CC;
			if (AA > UE_DOUBLE_SMALL_NUMBER && Discriminant >= 0.0)
			{
				const double Hit = (-BB - FMath::Sqrt(Discriminant)) / AA;
				const FVector Point = Origin + Delta * Hit;
				if (Hit >= 0.0 && Hit <= 1.0 && Hit < T && FMath::Abs(Point.Z) <= Half)
				{
					T = Hit;
					Normal = FVector(Point.X, Point.Y, 0.0).GetSafeNormal();
				}
			}
			Sphere(FVector(0.0, 0.0, Half), Radius);
			Sphere(FVector(0.0, 0.0, -Half), Radius);
		}
		if (T < Best && T <= 1.0)
		{
			Best = T;
			const FVector Point = Start + Ray * T;
			OutHit = FHitResult(const_cast<AfpstrueCharacter*>(this), GetMesh(), Point, Pose.TransformVectorNoScale(Normal).GetSafeNormal());
			OutHit.bBlockingHit = true;
			OutHit.Time = static_cast<float>(T);
			OutHit.Distance = static_cast<float>(Ray.Length() * T);
			OutHit.TraceStart = Start;
			OutHit.TraceEnd = End;
			OutHit.BoneName = Older.Bone;
		}
	}
	return OutHit.bBlockingHit;
}

bool AfpstrueCharacter::TraceHistoricalHit(UWorld* World, const AfpstrueCharacter* Shooter, double ServerTime,
	const FVector& Start, const FVector& End, FHitResult& OutHit, const FFPShotRequest* RequestContext)
{
	OutHit = FHitResult(Start, End);
	if (World == nullptr || World->GetNetMode() == NM_Client || Start.ContainsNaN() || End.ContainsNaN()
		|| !FMath::IsFinite(ServerTime) || ServerTime < World->GetTimeSeconds() - MaxHitHistorySeconds
		|| ServerTime > World->GetTimeSeconds() + 0.025) return false;
	// 与普通武器射线保持相同的复杂碰撞约定，不能在联网回溯时穿过仅复杂碰撞的掩体。
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FpstrueHistoricalCover), true);
	if (Shooter != nullptr) Params.AddIgnoredActor(Shooter);
	// 当前玩家的 Mesh 不能提前挡住另一名玩家的历史位置；墙和其他环境仍按当前碰撞参与遮挡。
	for (TActorIterator<AfpstrueCharacter> It(World); It; ++It) Params.AddIgnoredActor(*It);
	World->LineTraceSingleByChannel(OutHit, Start, End, FpstrueCollisionChannels::WeaponTrace, Params);
	for (TActorIterator<AfpstrueCharacter> It(World); It; ++It)
	{
		if (*It == Shooter) continue;
		FHitResult Candidate;
		const double TargetTime = RequestContext != nullptr ? RequestContext->GetTargetViewTime(*It, ServerTime) : ServerTime;
		if (It->TraceHistoricalHit(TargetTime, Start, End, Candidate)
			&& (!OutHit.bBlockingHit || Candidate.Time < OutHit.Time)) OutHit = Candidate;
	}
	return OutHit.bBlockingHit;
}

/*
 * 玩家角色是“输入与表现协调层”，不拥有武器弹药和生命值这两类业务状态：
 *
 *   Enhanced Input -> Character 提交输入 -> ASC/GameplayAbility 管理动作 -> WeaponComponent 执行射击/换弹事务
 *   ApplyDamage -> HealthComponent 修改生命值 -> Delegate 回到 Character -> 蓝图/HUD 表现
 *
 * 角色维护当前装备关系，并从能力标签派生瞄准/冲刺显示；武器保存弹药和换弹事务，HealthComponent 保存生命状态。
 * 本类不启用逐帧 Tick；连射由能力的 WaitDelay 任务推进，换弹超时和后坐力恢复由武器 Timer 管理。
 * GAS 当前流程：输入只提交能力按下/松开，动作标签为唯一决策状态；本类只投影标签为速度和蓝图表现。
 */

// ==================== 默认组件与生命周期 ====================

// 构造角色的胶囊体、第一人称相机、手臂网格和可复用生命组件；构造阶段不访问运行时世界。
