// Development-only inspection of the final pose. Never runs in ordinary play.
#include "CoreMinimal.h"
#if WITH_EDITOR && !UE_BUILD_SHIPPING
#include "Characters/Shared/fpstrueFootIK.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "UnrealClient.h"

namespace FpstrueFootIKAcceptance
{
struct FPlantSample
{
	TWeakObjectPtr<UPrimitiveComponent> Support;
	FVector AnchorLocal = FVector::ZeroVector;
	int32 Vertex = INDEX_NONE;
	uint32 Revision = 0;
	float Since = 0;
	bool bActive = false;
};
struct FVertexMotionSample
{
	FVector Position = FVector::ZeroVector;
	int32 Vertex = INDEX_NONE;
	float Time = 0;
	bool bValid = false;
};
struct FMeshSample
{
	FPlantSample Feet[2];
	FVertexMotionSample Motion[2];
	TWeakObjectPtr<UfpstrueFootIKAnimInstance> IKInstance;
	float LastYaw = 0;
	bool bHasYaw = false;
};
struct FWorldSample
{
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FMeshSample> Meshes;
	float NextSample = 0;
	float NextFrame = 0;
	int32 Frame = 0;
	bool bStarted = false;
	FString CaptureDirectory;
	FString PendingFrame;
};
TMap<TWeakObjectPtr<UWorld>, FWorldSample> Worlds;

// Each index is an authored sole vertex, not an ankle-height approximation.
bool SkinSole(USkeletalMeshComponent* Mesh, const TArray<int32>& Vertices,
	TArray<FMatrix44f>& Matrices, TArray<FVector>& Positions)
{
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* Render = Asset ? Asset->GetResourceForRendering() : nullptr;
	if (!Render || !Render->LODRenderData.IsValidIndex(0) || Vertices.IsEmpty()) return false;
	const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
	FSkinWeightVertexBuffer* Weights = Mesh->GetSkinWeightBuffer(0);
	if (!Weights || !Weights->GetDataVertexBuffer()->GetWeightData()
		|| !LOD.StaticVertexBuffers.PositionVertexBuffer.GetVertexData()
		|| Weights->GetNumVertices() != LOD.GetNumVertices()
		|| Matrices.Num() != Asset->GetRefBasesInvMatrix().Num()) return false;
	for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
		for (FBoneIndexType Bone : Section.BoneMap)
			if (!Matrices.IsValidIndex(Bone) || !Mesh->GetComponentSpaceTransforms().IsValidIndex(Bone)) return false;
	for (int32 Index : Vertices)
	{
		if (Index < 0 || static_cast<uint32>(Index) >= LOD.GetNumVertices()) return false;
		const FVector Point = Mesh->GetComponentTransform().TransformPosition(FVector(
			USkinnedMeshComponent::GetSkinnedVertexPosition(Mesh, Index, LOD, *Weights, Matrices)));
		if (Point.ContainsNaN()) return false;
		Positions.Add(Point);
	}
	return Positions.Num() == Vertices.Num();
}

struct FFloorSample
{
	FVector Sole = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	TWeakObjectPtr<UPrimitiveComponent> Support;
	double Gap = 9999;
	int32 Vertex = INDEX_NONE;
	bool bHit = false;
};

// The diagnostic performs its own geometry query; runtime bPlanted is not proof.
FFloorSample MeasureFloor(UWorld* World, ACharacter* Character, const TArray<int32>& Vertices,
	const TArray<FVector>& Positions)
{
	FFloorSample Result;
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FpstrueFootIKReviewFloor), true, Character);
	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		FHitResult Hit;
		const FVector& Point = Positions[Index];
		if (!World->LineTraceSingleByObjectType(Hit, Point + FVector(0,0,40), Point - FVector(0,0,100), Objects, Params)
			|| !Character->GetCharacterMovement()->IsWalkable(Hit)) continue;
		const double Gap = FVector::DotProduct(Point - Hit.ImpactPoint, Hit.ImpactNormal);
		if (Gap < Result.Gap)
		{
			Result.bHit = true; Result.Gap = Gap; Result.Sole = Point; Result.Normal = Hit.ImpactNormal;
			Result.Support = Hit.GetComponent(); Result.Vertex = Vertices[Index];
		}
	}
	return Result;
}

// Keep the same material vertex throughout a plant; switching lowest vertices
// during heel/toe roll must not manufacture an apparent slide measurement.
float MeasurePlant(FPlantSample& Plant, const FFloorSample& Floor, bool bPlanted, uint32 Revision,
	const TArray<int32>& Vertices, const TArray<FVector>& Positions, float Now, float& Age, bool& bChanged)
{
	Age = 0; bChanged = false;
	if (!bPlanted || !Floor.bHit || !Floor.Support.IsValid()) { Plant.bActive = false; return 0; }
	UPrimitiveComponent* Support = Floor.Support.Get();
	if (!Plant.bActive || Plant.Support != Floor.Support || Plant.Revision != Revision)
	{
		bChanged = true;
		Plant.bActive = true; Plant.Support = Support; Plant.Vertex = Floor.Vertex; Plant.Since = Now; Plant.Revision = Revision;
		Plant.AnchorLocal = Support->GetComponentTransform().InverseTransformPosition(Floor.Sole);
	}
	const int32 Index = Vertices.IndexOfByKey(Plant.Vertex);
	if (!Positions.IsValidIndex(Index)) { Plant.bActive = false; return 0; }
	Age = Now - Plant.Since;
	return FVector::Dist(Plant.AnchorLocal, Support->GetComponentTransform().InverseTransformPosition(Positions[Index]));
}

// This independent interval survives a new plant. Compare the SAME material
// vertex at adjacent samples before selecting the next interval's vertex;
// legitimate steps and stopping-pose jumps remain visible, not labelled slip.
float MeasureVertexMotion(FVertexMotionSample& Previous, const FFloorSample& Floor,
	const TArray<int32>& Vertices, const TArray<FVector>& Positions, float Now, float& Delta, int32& Vertex)
{
	Delta = 0; Vertex = INDEX_NONE; float Displacement = -1;
	const int32 Index = Vertices.IndexOfByKey(Previous.Vertex);
	if (Previous.bValid && Now > Previous.Time && Positions.IsValidIndex(Index))
	{
		Delta = Now - Previous.Time; Vertex = Previous.Vertex;
		Displacement = FVector::Dist(Previous.Position, Positions[Index]);
	}
	Previous.bValid = Floor.bHit;
	if (Previous.bValid)
	{
		Previous.Position = Floor.Sole; Previous.Vertex = Floor.Vertex; Previous.Time = Now;
	}
	return Displacement;
}

FString PosePhase(ACharacter* Character, const FMeshSample& Previous, float NowYaw)
{
	if (Character->GetCharacterMovement()->IsFalling()) return TEXT("Air");
	if (const AfpstrueCharacter* Player = Cast<AfpstrueCharacter>(Character))
	{
		if (Player->GetBodyFacingState().TurnDirection != 0) return TEXT("Turn");
	}
	else if (Previous.bHasYaw && FMath::Abs(FMath::FindDeltaAngleDegrees(Previous.LastYaw, NowYaw)) > 3.0f)
		return TEXT("Turn");
	const double Speed = Character->GetVelocity().Size2D();
	return Speed <= 3.0 ? TEXT("Idle") : (Speed > 450.0 ? TEXT("Sprint") : TEXT("Walk"));
}

// A recently rendered body is not enough: both measured soles must actually
// project inside this peer's viewport and not be hidden behind map geometry.
bool FeetVisible(UWorld* World, ACharacter* Character, const FVector Soles[2])
{
	APlayerController* Viewer = World->GetFirstPlayerController();
	if (!Viewer || !Viewer->IsLocalController()) return false;
	int32 Width = 0, Height = 0; Viewer->GetViewportSize(Width, Height);
	if (Width <= 0 || Height <= 0) return false;
	FVector Eye; FRotator View; Viewer->GetPlayerViewPoint(Eye, View);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueFootIKReviewVisibility), true, Character);
	Query.AddIgnoredActor(Viewer->GetPawn());
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FVector2D Pixel;
		if (!Viewer->ProjectWorldLocationToScreen(Soles[Side], Pixel, true)
			|| Pixel.X < 20 || Pixel.Y < 20 || Pixel.X > Width - 20 || Pixel.Y > Height - 20) return false;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Eye, Soles[Side] + FVector(0,0,3), ECC_Visibility, Query)) return false;
	}
	return true;
}

void SampleCharacter(UWorld* World, FWorldSample& Session, ACharacter* Character, const TCHAR* Kind)
{
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	if (!Mesh) return;
	// Diagnostic synchronization only, sampled at 10 Hz. This is not a perf run.
	Mesh->HandleExistingParallelEvaluationTask(true, true);
	auto* IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
	const float Now = World->GetTimeSeconds();
	FMeshSample& History = Session.Meshes.FindOrAdd(Mesh);
	const float Yaw = Character->GetActorRotation().Yaw;
	const FString Phase = PosePhase(Character, History, Yaw);
	History.LastYaw = Yaw; History.bHasYaw = true;
	if (!IK)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_FOOT_IK_REVIEW MISSING Peer=%s Kind=%s Pawn=%s Phase=%s Time=%.4f PostProcess=%s Leader=%s"),
			World->GetNetMode() == NM_DedicatedServer ? TEXT("Server") : TEXT("Client"), Kind, *Character->GetName(), *Phase, Now,
			*GetNameSafe(Mesh->GetPostProcessInstance()), *GetNameSafe(Mesh->LeaderPoseComponent.Get()));
		return;
	}
	if (History.IKInstance.Get() != IK)
	{
		History.Feet[0].bActive = History.Feet[1].bActive = false;
		History.IKInstance = IK;
	}
	const auto& Debug = IK->GetFootIKDebug();
	TArray<FMatrix44f> Matrices;
	Mesh->GetCurrentRefToLocalMatrices(Matrices, 0);
	FVector MeasuredSoles[2]; bool bMeasuredFeet[2] = {};
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const TArray<int32>& Vertices = Side == 0 ? IK->LeftSoleVertices : IK->RightSoleVertices;
		const bool bPlanted = Side == 0 ? Debug.bLeftPlanted : Debug.bRightPlanted;
		const uint32 PlantRevision = Side == 0 ? Debug.LeftPlantRevision : Debug.RightPlantRevision;
		TArray<FVector> Positions;
		const bool bCPU = SkinSole(Mesh, Vertices, Matrices, Positions);
		const FFloorSample Floor = bCPU ? MeasureFloor(World, Character, Vertices, Positions) : FFloorSample();
		MeasuredSoles[Side] = Floor.Sole; bMeasuredFeet[Side] = bCPU && Floor.bHit;
		float PlantAge = 0, SampleDelta = 0; bool bPlantChanged = false; int32 DisplacementVertex = INDEX_NONE;
		const float Displacement = MeasureVertexMotion(History.Motion[Side], Floor, Vertices, Positions, Now, SampleDelta, DisplacementVertex);
		const float Slip = MeasurePlant(History.Feet[Side], Floor, bPlanted, PlantRevision, Vertices, Positions, Now, PlantAge, bPlantChanged);
		const FVector DebugSole = Side == 0 ? Debug.LeftSoleWorld : Debug.RightSoleWorld;
		const UPrimitiveComponent* Support = Floor.Support.Get();
		UE_LOG(LogTemp, Display, TEXT("PVP_FOOT_IK_REVIEW SAMPLE Peer=%s Kind=%s Pawn=%s Phase=%s Side=%s Time=%.4f Speed=%.4f Grounded=%d Enabled=%d Planted=%d Cpu=%d Vertices=%d Floor=%d Gap=%.5f NormalZ=%.5f Slip=%.5f PlantAge=%.5f Pelvis=%.5f TraceCount=%d SkinLOD=0 RenderLOD=%d Visible=%d Vertex=%d Support=%s Sole=%s DebugSole=%s Leader=%s PlantRevision=%u PlantChanged=%d Displacement=%.5f Delta=%.5f DisplacementVertex=%d SupportActor=%s SupportPath=%s StepUp=%d"),
			World->GetNetMode() == NM_DedicatedServer ? TEXT("Server") : TEXT("Client"), Kind, *Character->GetName(), *Phase,
			Side == 0 ? TEXT("L") : TEXT("R"), Now, Character->GetVelocity().Size2D(), Character->GetCharacterMovement()->IsMovingOnGround(),
			Debug.bEnabled, bPlanted, bCPU, Vertices.Num(), Floor.bHit, Floor.Gap, Floor.Normal.Z, Slip, PlantAge,
			static_cast<double>(Debug.PelvisOffset), static_cast<int32>(Debug.TraceCount), Mesh->GetPredictedLODLevel(),
			Mesh->WasRecentlyRendered(0.2f), Floor.Vertex, *GetNameSafe(Floor.Support.Get()), *Floor.Sole.ToCompactString(),
			*DebugSole.ToCompactString(), *GetNameSafe(Mesh->LeaderPoseComponent.Get()), PlantRevision, bPlantChanged,
			Displacement, SampleDelta, DisplacementVertex, *GetNameSafe(Support ? Support->GetOwner() : nullptr),
			*GetPathNameSafe(Support), Support ? static_cast<int32>(Support->CanCharacterStepUpOn.GetValue()) : INDEX_NONE);
	}
	// Facing already owns its >=40 consecutive PNGs. Never contend with its
	// FScreenshotRequest or compress duplicate waiting/idle frames a second time.
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpFacingReview"))
		&& FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")) && FCString::Strcmp(Kind, TEXT("Enemy")) == 0
		&& Session.Frame < 20 && Now >= Session.NextFrame && bMeasuredFeet[0] && bMeasuredFeet[1]
		&& Mesh->WasRecentlyRendered(0.2f) && FeetVisible(World, Character, MeasuredSoles)
		&& !Session.CaptureDirectory.IsEmpty() && !FScreenshotRequest::IsScreenshotRequested()
		&& World->GetNetMode() != NM_DedicatedServer && GEngine && GEngine->GameViewport)
	{
		Session.NextFrame = Now + 1.0f;
		const TCHAR* Motion = Character->GetVelocity().Size2D() > 3.0 ? TEXT("Moving") : TEXT("Standing");
		Session.PendingFrame = Session.CaptureDirectory / FString::Printf(TEXT("FootIK_%05d_%s_%s_%s.png"), ++Session.Frame, Kind, *Phase, Motion);
		FScreenshotRequest::RequestScreenshot(Session.PendingFrame, false, false);
		UE_LOG(LogTemp, Display, TEXT("PVP_FOOT_IK_REVIEW FRAME_REQUEST Kind=%s Phase=%s Motion=%s Time=%.4f FeetInViewport=1 FeetUnoccluded=1 LimitHz=1 LimitFrames=20 File=%s"),
			Kind, *Phase, Motion, Now, *Session.PendingFrame);
	}
}

void Tick(UWorld* World, ELevelTick, float)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("PvpFootIKReview")) || !World || !World->IsGameWorld()
		|| !World->HasBegunPlay()) return;
	FWorldSample& Session = Worlds.FindOrAdd(World);
	const float Now = World->GetTimeSeconds();
	if (!Session.bStarted)
	{
		Session.bStarted = true;
		FString Base;
		if (FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), Base))
		{
			Session.CaptureDirectory = Base / TEXT("FootIK");
			IFileManager::Get().MakeDirectory(*Session.CaptureDirectory, true);
		}
		UE_LOG(LogTemp, Display, TEXT("PVP_FOOT_IK_REVIEW BEGIN SampleHz=10 CpuSubsetOnly=1 ActualFloorQueries=1 NoGameplayMutation=1 MorphClothExcluded=1"));
	}
	if (!Session.PendingFrame.IsEmpty() && IFileManager::Get().FileSize(*Session.PendingFrame) > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_FOOT_IK_REVIEW FRAME_SAVED File=%s"), *Session.PendingFrame);
		Session.PendingFrame.Reset();
	}
	if (Now < Session.NextSample) return;
	Session.NextSample = Now + 0.1f;
	AfpstrueCharacter* Local = nullptr;
	TArray<AfpstrueCharacter*, TInlineAllocator<2>> Players;
	for (TActorIterator<AfpstrueCharacter> It(World); It; ++It)
	{
		if (It->IsDead()) continue;
		if (It->IsLocallyControlled()) Local = *It;
		Players.Add(*It);
	}
	// Functional one-enemy pass samples real nearby AI. Never CPU-skin a crowd.
	TArray<AfpstrueEnemyCharacter*, TInlineAllocator<4>> Nearby;
	for (TActorIterator<AfpstrueEnemyCharacter> It(World); It; ++It)
		if (!It->IsDead() && Local && FVector::DistSquared(It->GetActorLocation(), Local->GetActorLocation()) <= FMath::Square(3000.0f))
			Nearby.Add(*It);
	Nearby.Sort([Local](const AfpstrueEnemyCharacter& A, const AfpstrueEnemyCharacter& B)
		{ return FVector::DistSquared(A.GetActorLocation(), Local->GetActorLocation()) < FVector::DistSquared(B.GetActorLocation(), Local->GetActorLocation()); });
	for (AfpstrueCharacter* Player : Players)
		SampleCharacter(World, Session, Player, Player->IsLocallyControlled() ? TEXT("LocalPlayer") : TEXT("RemotePlayer"));
	for (int32 Index = 0; Index < FMath::Min(2, Nearby.Num()); ++Index) SampleCharacter(World, Session, Nearby[Index], TEXT("Enemy"));
}

void Cleanup(UWorld* World, bool, bool) { Worlds.Remove(World); }
struct FHooks
{
	FDelegateHandle TickHandle, CleanupHandle;
	FHooks()
	{
		TickHandle = FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);
		CleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&Cleanup);
	}
	~FHooks()
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle);
		FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
	}
} Hooks;
}
#endif
