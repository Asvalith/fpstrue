// Copyright Epic Games, Inc. All Rights Reserved.
#include "Characters/Shared/fpstrueFootIK.h"
#include "Animation/AnimInstanceProxy.h"
#include "BonePose.h"
#include "TwoBoneIK.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ObjectKey.h"

// 玩家专用求值器：只修改输出姿态与本实例缓存；World 查询依赖 AnimInstance 的同步求值门禁。
// 【设计取舍】PreUpdate 的坐标/资格快照只隔离读取，不会让后续 World 查询自动变成线程安全。
// 复用 AnimationCore 两骨解算，项目只适配支撑与锁脚；解算只写姿态，不回写胶囊或 CMC 移动。
struct FFpstrueFootIKProxy : FAnimInstanceProxy
{
	// 关联所属动画实例；贴地缓存按实例默认初始化，不与其他角色共享。
	explicit FFpstrueFootIKProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}
	struct FGround
	{
		bool bValid = false;
		FVector Position = FVector::ZeroVector, Normal = FVector::UpVector;
		FTransform Surface = FTransform::Identity;
		FObjectKey Identity;
	};
	struct FFoot
	{
		bool bPlanted = false, bHadRaw = false;
		FVector PreviousRaw = FVector::ZeroVector, PlantLocal = FVector::ZeroVector;
		FQuat PlantRotationLocal = FQuat::Identity;
		FObjectKey Surface;
		float Alpha = 0.0f;
	};
	FGround Ground[2];
	FFoot Feet[2];
	uint32 PlantRevision[2] = {};
	FVector SoleOffsets[2];
	FVector SoleNormals[2];
	TArray<FVector> SoleProbes[2];
	TArray<FVector> ToeProbes[2];
	FTransform MeshWorld = FTransform::Identity;
	FVector Bottom = FVector::ZeroVector, Velocity = FVector::ZeroVector;
	float Delta = 0.0f, PelvisZ = 0.0f;
	double PreviousTime = -1.0;
	FVector PreviousLocation = FVector::ZeroVector;
	bool bEligible = false;
	bool bSettledIdle = false;
	TWeakObjectPtr<AfpstrueCharacter> QueryOwner;
	FFpstrueFootIKDebug Debug;
	// 扫描鞋底投影覆盖的薄盒，更新本脚最高可行走支撑，弥补离散探针之间的窄障碍。
	void CoverFootprint(ACharacter* Character, int32 Side, const FBox& Bounds,
		const FCollisionObjectQueryParams& Objects, const FCollisionQueryParams& Query)
	{
		if (!Bounds.IsValid) return;
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueFootIK_FootprintSweep);
		const FVector Center = Bounds.GetCenter(), Extent = Bounds.GetExtent();
		TArray<FHitResult> Hits; ++Debug.TraceCount;
		Character->GetWorld()->SweepMultiByObjectType(Hits, FVector(Center.X, Center.Y, Bottom.Z + 45),
			FVector(Center.X, Center.Y, Bottom.Z - 45), FQuat::Identity, Objects,
			FCollisionShape::MakeBox(FVector(FMath::Max(0.5, Extent.X), FMath::Max(0.5, Extent.Y), 0.25)), Query);
		// 射线之间也可能有窄路沿；薄盒覆盖整个鞋底投影，不以增大角色胶囊来绕过碰撞。
		for (const FHitResult& Hit : Hits)
		{
			if (Cast<APawn>(Hit.GetActor()) || !Hit.GetComponent() || !Character->GetCharacterMovement()->IsWalkable(Hit)) continue;
			if (!Ground[Side].bValid || Hit.ImpactPoint.Z > Ground[Side].Position.Z)
			{
				Ground[Side].bValid = true; Ground[Side].Position = Hit.ImpactPoint; Ground[Side].Normal = Hit.ImpactNormal;
				Ground[Side].Surface = Hit.GetComponent()->GetComponentTransform(); Ground[Side].Identity = FObjectKey(Hit.GetComponent());
			}
		}
	}

	// 沿线返回首个非 Pawn 的可行走支撑；Hits 由调用者复用，查询次数累计进诊断。
	bool FindWorldSupport(ACharacter* Character, const FVector& Start, const FVector& End,
		const FCollisionObjectQueryParams& Objects, const FCollisionQueryParams& Query, FHitResult& Support, TArray<FHitResult>& Hits)
	{
		++Debug.TraceCount;
		Character->GetWorld()->LineTraceMultiByObjectType(Hits, Start, End, Objects, Query);
		for (const FHitResult& Hit : Hits)
			if (!Cast<APawn>(Hit.GetActor()) && Hit.GetComponent() && Character->GetCharacterMovement()->IsWalkable(Hit))
			{ Support = Hit; return true; }
		return false;
	}

	// 先确定玩家是否需要独立贴地；其他角色、空中与共享动画不执行此后处理的世界查询。
	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override
	{
		check(IsInGameThread());
		FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
		auto* IK = CastChecked<UfpstrueFootIKAnimInstance>(Instance);
		auto* Character = Cast<AfpstrueCharacter>(Instance->TryGetPawnOwner());
		auto* Mesh = Instance->GetSkelMeshComponent();
		Delta = FMath::Max(0.0f, DeltaSeconds);
		bool bActive = Character && Mesh == Character->GetMesh() && !Mesh->LeaderPoseComponent.IsValid() && !Mesh->IsSimulatingPhysics()
			&& Character->GetCharacterMovement()->IsMovingOnGround() && !Character->IsDead();
		const double Now = Character ? Character->GetWorld()->GetTimeSeconds() : 0.0;
		const FVector Location = Character ? Character->GetActorLocation() : FVector::ZeroVector;
		if (!bActive || bActive != bEligible || IK->bResetRequested || Now - PreviousTime > 0.15 || FVector::DistSquared(Location, PreviousLocation) > FMath::Square(100.0))
		{
			Feet[0] = FFoot(); Feet[1] = FFoot();
			PelvisZ = 0.0f;
		}
		IK->bResetRequested = false;
		bEligible = bActive; PreviousTime = Now; PreviousLocation = Location;
		// 资格只控制是否查询地面，不改变动画任务的线程许可；本类始终由Game Thread求值。
		QueryOwner = Character;
		Ground[0] = FGround(); Ground[1] = FGround();
		Debug.bEnabled = bEligible; Debug.bLeftPlanted = Debug.bRightPlanted = false; Debug.PelvisOffset = 0.0f;
		if (!bEligible) return;
		MeshWorld = Mesh->GetComponentTransform(); Velocity = Character->GetVelocity();
		const auto* Animation = Cast<UfpstruePlayerAnimInstance>(Mesh->GetAnimInstance());
		bSettledIdle = Velocity.Size2D() < 3.0f && Character->GetBodyFacingState().TurnDirection == 0
			&& (!Animation || Animation->TurnAlpha < 0.05f);
		Bottom = Location - FVector(0, 0, Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		SoleOffsets[0] = IK->LeftSoleOffset; SoleOffsets[1] = IK->RightSoleOffset;
		SoleNormals[0] = IK->LeftSoleNormal; SoleNormals[1] = IK->RightSoleNormal;
		SoleProbes[0] = IK->LeftSoleProbes; SoleProbes[1] = IK->RightSoleProbes;
		ToeProbes[0] = IK->LeftToeProbes; ToeProbes[1] = IK->RightToeProbes;
	}

	// 先求值本帧输入姿态，再查支撑、稳定支撑脚并解腿；无资格时原样保留主图输出。
	virtual bool Evaluate(FPoseContext& Output) override
	{
		// 薄 AnimBP 的 LinkedInputPose 由引擎填入本帧主图结果，不能用上帧后处理后的 Socket 当输入骨骼。
		GetRootNode()->Evaluate_AnyThread(Output);
		if (!bEligible) return true;
		// 每次求值/每脚批次计时；不为每条探针再生成一个事件，避免诊断放大查询开销。
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueFootIK_Solve);
		const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
		const auto Index = [&Bones](const TCHAR* Name)
		{
			return Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Bones.GetReferenceSkeleton().FindBoneIndex(Name)));
		};
		const FCompactPoseBoneIndex Pelvis = Index(TEXT("pelvis"));
		const FCompactPoseBoneIndex FootIndices[] = {Index(TEXT("foot_l")), Index(TEXT("foot_r"))};
		if (!Pelvis.IsValid() || !FootIndices[0].IsValid() || !FootIndices[1].IsValid()) { Debug.bEnabled = false; return true; }
		FCSPose<FCompactPose> Pose; Pose.InitPose(Output.Pose);
		check(IsInGameThread());
		AfpstrueCharacter* Character = QueryOwner.Get();
		if (!Character) return true;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueFootIKRecovery), true, Character);
		if (const auto* Weapon = Character->GetEquippedWeaponComponent()) Query.AddIgnoredActor(Weapon->GetOwner());
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueFootIK_RawSupportBatch);
			// 当前主图姿态求值后才查询地面，避免转身、停步及长帧时使用上一帧脚点造成穿插。
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const FTransform Foot = Pose.GetComponentSpaceTransform(FootIndices[Side]);
				const FCompactPoseBoneIndex Ball = Index(Side == 0 ? TEXT("ball_l") : TEXT("ball_r"));
				const FTransform BallToFoot = Ball.IsValid() ? Pose.GetComponentSpaceTransform(Ball).GetRelativeTransform(Foot) : FTransform::Identity;
				TArray<FVector, TInlineAllocator<18>> Probes; Probes.Append(SoleProbes[Side]);
				for (const FVector& Toe : ToeProbes[Side]) Probes.Add(BallToFoot.TransformPosition(Toe));
				const FVector Center = MeshWorld.TransformPosition(Foot.TransformPosition(SoleOffsets[Side]));
				const FQuat RawRotation = MeshWorld.GetRotation() * Foot.GetRotation();
				FBox ProbeBounds(ForceInit);
				for (const FVector& Probe : Probes)
					ProbeBounds += Center + RawRotation.RotateVector((Probe - SoleOffsets[Side]) * MeshWorld.GetScale3D() * Foot.GetScale3D());
				CoverFootprint(Character, Side, ProbeBounds, Objects, Query);
				TArray<FHitResult> Hits;
				FHitResult Hit;
				if (FindWorldSupport(Character, FVector(Center.X, Center.Y, Bottom.Z + 45),
					FVector(Center.X, Center.Y, Bottom.Z - 45), Objects, Query, Hit, Hits))
				{
					if (!Ground[Side].bValid || Hit.ImpactPoint.Z > Ground[Side].Position.Z)
					{
						Ground[Side].bValid = true; Ground[Side].Position = Hit.ImpactPoint; Ground[Side].Normal = Hit.ImpactNormal;
						Ground[Side].Surface = Hit.GetComponent()->GetComponentTransform(); Ground[Side].Identity = FObjectKey(Hit.GetComponent());
					}
				}
			}
		}
		FTransform Raw[2]; FVector RawSole[2], Targets[2]; FQuat Rotations[2]; float DesiredPelvis = 0.0f;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueFootIK_FootTarget);
			Raw[Side] = Pose.GetComponentSpaceTransform(FootIndices[Side]);
			const FCompactPoseBoneIndex BallIndex = Index(Side == 0 ? TEXT("ball_l") : TEXT("ball_r"));
			const FTransform BallToFoot = BallIndex.IsValid() ? Pose.GetComponentSpaceTransform(BallIndex).GetRelativeTransform(Raw[Side]) : FTransform::Identity;
			const FVector RawSoleCS = Raw[Side].TransformPosition(SoleOffsets[Side]);
			RawSole[Side] = MeshWorld.TransformPosition(RawSoleCS);
			FFoot& Foot = Feet[Side]; const FGround& Floor = Ground[Side];
			const float Lift = RawSole[Side].Z - Bottom.Z;
			const FVector RawVelocity = Foot.bHadRaw && Delta > SMALL_NUMBER ? (RawSole[Side] - Foot.PreviousRaw) / Delta : Velocity;
			const FQuat RawWorldRotation = MeshWorld.GetRotation() * Raw[Side].GetRotation();
			const FQuat Aligned = Floor.bValid ? FQuat::FindBetweenNormals(bSettledIdle
				? RawWorldRotation.RotateVector(SoleNormals[Side]) : FVector::UpVector, Floor.Normal) * RawWorldRotation : RawWorldRotation;
			// 空手 Idle 的原始脚高不等于抬脚；静止且脚姿态稳定时完整贴地，转身/迈步仍按抬脚高度释放。
			const bool bResting = Foot.bHadRaw && Velocity.Size2D() < 3.0f && RawVelocity.Size2D() < 20.0f
				&& FMath::Abs(RawVelocity.Z) < 10.0f && Lift < 20.0f;
			const float Support = Floor.bValid ? (bResting ? 1.0f : 1.0f - FMath::SmoothStep(4.0f, 14.0f, Lift)) : 0.0f;
			// 只有真正 Idle 才完整稳脚；原地转身和攻击虽速度为零，仍必须放开动画中的抬脚。
			Foot.Alpha = bSettledIdle && Floor.bValid ? 1.0f : FMath::FInterpConstantTo(Foot.Alpha, Support, Delta, 12.0f);
			Debug.RawSoleHeights[Side] = Lift; Debug.SupportWeights[Side] = Foot.Alpha;
			Foot.PreviousRaw = RawSole[Side]; Foot.bHadRaw = true;
			// 走跑、转身与攻击保留原动画的水平脚位和脚掌滚动；只在真正静止时锁脚。
			if (!bSettledIdle || !Floor.bValid || Support < 0.65f || Foot.Surface != Floor.Identity) Foot.bPlanted = false;
			if (bSettledIdle && !Foot.bPlanted && Support > 0.95f && RawVelocity.Size2D() < 50.0f)
			{
				Foot.PlantLocal = Floor.Surface.InverseTransformPosition(RawSole[Side]);
				Foot.Surface = Floor.Identity; Foot.bPlanted = true;
				Foot.PlantRotationLocal = Floor.Surface.GetRotation().Inverse() * Aligned;
				++PlantRevision[Side];
			}
			FVector Contact = RawSole[Side];
			if (Foot.bPlanted)
			{
				const FVector Locked = Floor.Surface.TransformPosition(Foot.PlantLocal);
				const FQuat LockedRotation = Floor.Surface.GetRotation() * Foot.PlantRotationLocal;
				if (FVector::Dist2D(Locked, RawSole[Side]) > 15.0f || FMath::RadiansToDegrees(LockedRotation.AngularDistance(Aligned)) > 30.0f) Foot.bPlanted = false;
				else { Contact.X = Locked.X; Contact.Y = Locked.Y; }
			}
			if (Floor.bValid)
			{
				const double FloorZ = Floor.Position.Z - (Floor.Normal.X * (Contact.X - Floor.Position.X) + Floor.Normal.Y * (Contact.Y - Floor.Position.Y)) / Floor.Normal.Z;
				// 移动时保留摆腿，只在明确支撑期补足接地余量，不把整段动作强行拉回地面。
				const float TerrainCorrection = FMath::Clamp(static_cast<float>(FloorZ - Bottom.Z), -6.0f, 15.0f);
				const float ContactCorrection = FMath::Clamp(static_cast<float>(FloorZ + 0.7 - RawSole[Side].Z), -6.0f, 15.0f);
				const float Correction = bSettledIdle
					? FMath::Clamp(static_cast<float>(FloorZ + 0.7 - RawSole[Side].Z), -25.0f, 25.0f)
					: FMath::Lerp(TerrainCorrection, ContactCorrection, FMath::SmoothStep(0.8f, 1.0f, Support));
				Contact.Z = RawSole[Side].Z + Correction;
				DesiredPelvis = FMath::Min(DesiredPelvis, Correction * Foot.Alpha);
			}
			// 支撑脚同时稳住位置和鞋底方向，不能锁脚心却让鞋尖在地面绕圈滑动。
			const FQuat SupportRotation = Foot.bPlanted ? Floor.Surface.GetRotation() * Foot.PlantRotationLocal : Aligned;
			Rotations[Side] = FQuat::Slerp(RawWorldRotation, SupportRotation, Foot.Alpha).GetNormalized();
			const FVector TargetWorld = Contact - Rotations[Side].RotateVector(SoleOffsets[Side] * MeshWorld.GetScale3D() * Raw[Side].GetScale3D());
			FVector BlendedTarget = FMath::Lerp(MeshWorld.TransformPosition(Raw[Side].GetLocation()), TargetWorld, Foot.Alpha);
			TArray<FVector, TInlineAllocator<18>> Probes;
			Probes.Append(SoleProbes[Side]);
			for (const FVector& Toe : ToeProbes[Side]) Probes.Add(BallToFoot.TransformPosition(Toe));
			FBox FinalBounds(ForceInit);
			for (const FVector& Probe : Probes)
				FinalBounds += BlendedTarget + Rotations[Side].RotateVector(Probe * MeshWorld.GetScale3D() * Raw[Side].GetScale3D());
			// 锁脚、旋转和权重混合会改变最终鞋底投影，不能只沿用原始动画脚位查到的支撑面。
			CoverFootprint(Character, Side, FinalBounds, Objects, Query);
			double RequiredLift = 0.0;
			TArray<FHitResult> ProbeHits;
			{
			TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueFootIK_FinalSupportProbeBatch);
			for (const FVector& Probe : Probes)
			{
				const FVector Point = BlendedTarget + Rotations[Side].RotateVector(Probe * MeshWorld.GetScale3D() * Raw[Side].GetScale3D());
				const FVector Start(Point.X, Point.Y, Bottom.Z + 45), End(Point.X, Point.Y, Bottom.Z - 45);
				FHitResult ProbeSupport; ++Debug.ProbeTraceCount;
				const bool bFound = FindWorldSupport(Character, Start, End, Objects, Query, ProbeSupport, ProbeHits);
				// 薄盒可能先擦到路沿侧壁；逐点检查最终鞋底下的台面，只抬升、不压低摆腿。
				if (bFound) RequiredLift = FMath::Max(RequiredLift,
					(0.7 - FVector::DotProduct(Point - ProbeSupport.ImpactPoint, ProbeSupport.ImpactNormal)) / ProbeSupport.ImpactNormal.Z);
			}
			}
			if (bSettledIdle && Floor.bValid)
			{
				// 摆腿可以离地，但权重过渡不能把鞋边插进支撑面；只做非穿透抬升，不把空中脚向下拉平。
				double MinimumGap = DBL_MAX;
				for (const FVector& Probe : Probes)
					MinimumGap = FMath::Min(MinimumGap, FVector::DotProduct(BlendedTarget
						+ Rotations[Side].RotateVector(Probe * MeshWorld.GetScale3D() * Raw[Side].GetScale3D()) - Floor.Position, Floor.Normal));
				RequiredLift = FMath::Max(RequiredLift, (0.7 - MinimumGap) / Floor.Normal.Z);
			}
			BlendedTarget.Z += RequiredLift;
			Targets[Side] = MeshWorld.InverseTransformPosition(BlendedTarget);
		}
		// 足底旋转也会改变踝关节目标高度；只看鞋底的下降量会让坡面低侧超过真实腿长。
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueFootIK_LegSolve);
		float ReachLimit = 0.0f;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FCompactPoseBoneIndex CalfIndex = Bones.GetParentBoneIndex(FootIndices[Side]);
			if (!CalfIndex.IsValid()) continue;
			const FCompactPoseBoneIndex ThighIndex = Bones.GetParentBoneIndex(CalfIndex);
			if (!ThighIndex.IsValid()) continue;
			const FVector Hip = MeshWorld.TransformPosition(Pose.GetComponentSpaceTransform(ThighIndex).GetLocation());
			const FVector Knee = MeshWorld.TransformPosition(Pose.GetComponentSpaceTransform(CalfIndex).GetLocation());
			const FVector Ankle = MeshWorld.TransformPosition(Raw[Side].GetLocation());
			const FVector Target = MeshWorld.TransformPosition(Targets[Side]);
			const double Length = FMath::Max(0.0, FVector::Dist(Hip, Knee) + FVector::Dist(Knee, Ankle) - 0.2);
			const double MaxHipZ = Target.Z + FMath::Sqrt(FMath::Max(0.0, Length * Length - FVector::DistSquared2D(Hip, Target)));
			ReachLimit = FMath::Min(ReachLimit, static_cast<float>(MaxHipZ - Hip.Z));
		}
		const float PelvisLimit = bSettledIdle ? 15.0f : 6.0f;
		ReachLimit = FMath::Clamp(ReachLimit, -PelvisLimit, 0.0f);
		PelvisZ = FMath::Clamp(FMath::Min(ReachLimit, FMath::FInterpTo(PelvisZ,
			FMath::Clamp(FMath::Min(DesiredPelvis, ReachLimit), -PelvisLimit, 0.0f), Delta, 15.0f)), -PelvisLimit, 0.0f);
		TArray<FBoneTransform, TInlineAllocator<8>> Changes;
		FTransform Hips = Pose.GetComponentSpaceTransform(Pelvis);
		Hips.AddToTranslation(MeshWorld.InverseTransformVector(FVector(0, 0, PelvisZ)));
		Changes.Emplace(Pelvis, Hips);
		Pose.LocalBlendCSBoneTransforms(Changes, 1.0f); Changes.Reset();
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FCompactPoseBoneIndex FootIndex = FootIndices[Side], CalfIndex = Bones.GetParentBoneIndex(FootIndex), ThighIndex = Bones.GetParentBoneIndex(CalfIndex);
			if (!CalfIndex.IsValid() || !ThighIndex.IsValid()) continue;
			FTransform Thigh = Pose.GetComponentSpaceTransform(ThighIndex), Calf = Pose.GetComponentSpaceTransform(CalfIndex), Foot = Pose.GetComponentSpaceTransform(FootIndex);
			const FVector Upper = Calf.GetLocation() - Thigh.GetLocation(), Lower = Foot.GetLocation() - Thigh.GetLocation();
			FVector Bend = (Upper - Lower.GetSafeNormal() * FVector::DotProduct(Upper, Lower.GetSafeNormal())).GetSafeNormal();
			if (Bend.IsNearlyZero()) Bend = MeshWorld.InverseTransformVectorNoScale(MeshWorld.GetRotation().RotateVector(FVector::RightVector));
			AnimationCore::SolveTwoBoneIK(Thigh, Calf, Foot, Calf.GetLocation() + Bend * 60.0f, Targets[Side], false, 1.0, 1.0);
			Debug.ReachErrors[Side] = FVector::Dist(Foot.GetLocation(), Targets[Side]);
			Foot.SetRotation((MeshWorld.GetRotation().Inverse() * Rotations[Side]).GetNormalized());
			Changes.Emplace(ThighIndex, Thigh); Changes.Emplace(CalfIndex, Calf); Changes.Emplace(FootIndex, Foot);
			Pose.LocalBlendCSBoneTransforms(Changes, 1.0f); Changes.Reset();
			const FVector Sole = MeshWorld.TransformPosition(Foot.TransformPosition(SoleOffsets[Side]));
			if (Side == 0) { Debug.LeftSoleWorld = Sole; Debug.bLeftPlanted = Feet[Side].bPlanted; Debug.LeftPlantRevision = PlantRevision[Side]; }
			else { Debug.RightSoleWorld = Sole; Debug.bRightPlanted = Feet[Side].bPlanted; Debug.RightPlantRevision = PlantRevision[Side]; }
		}
		Debug.PelvisOffset = PelvisZ;
		FCSPose<FCompactPose>::ConvertComponentPosesToLocalPosesSafe(Pose, Output.Pose);
		return true;
	}
};

void UfpstrueFootIKAnimInstance::Install(ACharacter* Character)
{
	if (!Cast<AfpstrueCharacter>(Character) || !Character->GetMesh()->GetSkeletalMeshAsset()) return;
	const TCHAR* Path = TEXT("/Game/FirstPerson/Animations/FootIK/ABP_PlayerFootIK.ABP_PlayerFootIK_C");
	if (UClass* Class = LoadClass<UfpstrueFootIKAnimInstance>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn))
	{
		if (Character->GetMesh()->GetPostProcessInstance() && Character->GetMesh()->GetPostProcessInstance()->GetClass() == Class
			&& !Character->GetMesh()->GetDisablePostProcessBlueprint()) return;
		Character->GetMesh()->SetOverridePostProcessAnimBP(Class);
		Character->GetMesh()->SetDisablePostProcessBlueprint(false);
	}
}

void UfpstrueFootIKAnimInstance::ResetFootPlacement()
{
	bResetRequested = true;
	Debug.bEnabled = Debug.bLeftPlanted = Debug.bRightPlanted = false;
}

FFpstrueFootIKDebug UfpstrueFootIKAnimInstance::GetFootIKDebug() const
{
	FFpstrueFootIKDebug Result = Debug;
	if (GetSkelMeshComponent() && GetSkelMeshComponent()->LeaderPoseComponent.IsValid())
	{
		Result.bEnabled = Result.bLeftPlanted = Result.bRightPlanted = false;
	}
	return Result;
}

FAnimInstanceProxy* UfpstrueFootIKAnimInstance::CreateAnimInstanceProxy() { return new FFpstrueFootIKProxy(this); }
void UfpstrueFootIKAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) { delete static_cast<FFpstrueFootIKProxy*>(Proxy); }
void UfpstrueFootIKAnimInstance::NativePostEvaluateAnimation()
{
	Super::NativePostEvaluateAnimation();
	Debug = GetProxyOnGameThread<FFpstrueFootIKProxy>().Debug;
}
