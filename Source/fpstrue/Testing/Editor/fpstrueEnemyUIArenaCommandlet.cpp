// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Editor/fpstrueEnemyUIArenaCommandlet.h"

#if WITH_EDITOR
#include "ActorFactories/ActorFactory.h"
#include "AI/NavigationSystemBase.h"
#include "Builders/CubeBuilder.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CollisionQueryParams.h"
#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Game/fpstrueMultiplayer.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	const TCHAR* GroupTerrainPackage = TEXT("/Game/PerformanceCandidates/GroupAITerrain/GroupAITerrain160");
	const TCHAR* GroupTerrainMoundPackage = TEXT("/Game/PerformanceCandidates/GroupAITerrain/SM_GroupAITerrain_Mound");
	constexpr float TerrainCapsuleRadius = 55.0f;
	constexpr float TerrainCapsuleHalfHeight = 96.0f;
	const FName TerrainPlayerStartTags[2] = {TEXT("GroupBaselineClient1"), TEXT("GroupBaselineClient2")};

	// 替换只允许精确备份后的本工具产物；输入路径不能把备份指回正在替换的 Content。
	bool ValidateTerrainReplacementBackup(const FString& BackupRoot, const FString& MapFilename)
	{
		FString Root = FPaths::ConvertRelativePathToFull(BackupRoot);
		FPaths::NormalizeDirectoryName(Root);
		if (BackupRoot.IsEmpty() || !FPaths::CollapseRelativeDirectories(Root) ||
			FPaths::IsUnderDirectory(Root, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()))) return false;
		FString Manifest;
		if (!FFileHelper::LoadFileToString(Manifest, *FPaths::Combine(Root, TEXT("manifest.json")))) return false;
		auto ManifestHash = [&](const TCHAR* Key)
		{
			const FString Field = FString::Printf(TEXT("\"%s\""), Key);
			const int32 KeyPosition = Manifest.Find(Field, ESearchCase::CaseSensitive);
			if (KeyPosition == INDEX_NONE) return FString();
			const int32 Colon = Manifest.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, KeyPosition + Field.Len());
			const int32 Begin = Colon == INDEX_NONE ? INDEX_NONE : Manifest.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Colon + 1);
			const int32 End = Begin == INDEX_NONE ? INDEX_NONE : Manifest.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Begin + 1);
			return End == INDEX_NONE ? FString() : Manifest.Mid(Begin + 1, End - Begin - 1);
		};
		const FString MeshFilename = FPackageName::LongPackageNameToFilename(GroupTerrainMoundPackage, FPackageName::GetAssetPackageExtension());
		const FString CurrentFiles[2] = {FPaths::ConvertRelativePathToFull(MapFilename), FPaths::ConvertRelativePathToFull(MeshFilename)};
		const FString BackupFiles[2] = {FPaths::Combine(Root, TEXT("Content/PerformanceCandidates/GroupAITerrain/GroupAITerrain160.umap")),
			FPaths::Combine(Root, TEXT("Content/PerformanceCandidates/GroupAITerrain/SM_GroupAITerrain_Mound.uasset"))};
		const FString ExpectedHashes[2] = {ManifestHash(TEXT("TerrainMapSHA1")), ManifestHash(TEXT("TerrainMoundSHA1"))};
		for (int32 Index = 0; Index < 2; ++Index)
		{
			TArray<uint8> CurrentBytes, BackupBytes;
			if (ExpectedHashes[Index].Len() != FSHAHash::GetStringLen() || FPaths::IsSamePath(CurrentFiles[Index], BackupFiles[Index]) ||
				!FFileHelper::LoadFileToArray(CurrentBytes, *CurrentFiles[Index]) || !FFileHelper::LoadFileToArray(BackupBytes, *BackupFiles[Index]) ||
				CurrentBytes.IsEmpty() || CurrentBytes.Num() != BackupBytes.Num()) return false;
			const FString CurrentHash = FSHA1::HashBuffer(CurrentBytes.GetData(), CurrentBytes.Num()).ToString();
			const FString BackupHash = FSHA1::HashBuffer(BackupBytes.GetData(), BackupBytes.Num()).ToString();
			if (!CurrentHash.Equals(ExpectedHashes[Index], ESearchCase::IgnoreCase) || !CurrentHash.Equals(BackupHash, ESearchCase::CaseSensitive) ||
				FMemory::Memcmp(CurrentBytes.GetData(), BackupBytes.GetData(), CurrentBytes.Num()) != 0) return false;
		}
		UE_LOG(LogTemp, Display, TEXT("GROUP_AI_TERRAIN BACKUP_VERIFIED Root=%s ExactCurrentMapAndMoundMatch=1 ManifestSHA1Match=1 RecursiveDelete=0"), *Root);
		return true;
	}

	// 生成前和冷重载后走同一套只读检查；导航可达不等于物理空间可站立，两项必须分别成立。
	bool AuditGroupTerrain(UWorld* World, bool bColdLoad)
	{
		if (World == nullptr || !World->GetWorldSettings()->ActorHasTag(TEXT("GroupAITerrainStaggeredRouteV2"))) return false;
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
		World->UpdateWorldComponents(true, false);
		auto* Navigation = UNavigationSystemV1::GetCurrent(World);
		const FNavAgentProperties Properties(TerrainCapsuleRadius, TerrainCapsuleHalfHeight * 2.0f);
		const auto* NavData = Navigation ? Cast<ARecastNavMesh>(Navigation->GetNavDataForProps(Properties)) : nullptr;
		if (NavData == nullptr || Navigation->GetSupportedAgents().Num() != 1 ||
			!FMath::IsNearlyEqual(NavData->GetConfig().AgentRadius, TerrainCapsuleRadius) ||
			!FMath::IsNearlyEqual(NavData->GetConfig().AgentHeight, TerrainCapsuleHalfHeight * 2.0f) ||
			NavData->GetRuntimeGenerationMode() != ERuntimeGenerationType::Static)
		{
			UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL NavigationMissingOrNotStatic55x192 ColdLoad=%d Saved=0"), bColdLoad);
			return false;
		}
		TArray<APlayerStart*> Starts;
		TArray<ATargetPoint*> Spawns;
		TArray<ATargetPoint*> TraversalProbes;
		TArray<AStaticMeshActor*> Mounds, Covers;
		for (TActorIterator<APlayerStart> It(World); It; ++It) Starts.Add(*It);
		for (TActorIterator<ATargetPoint> It(World); It; ++It)
		{
			if (It->ActorHasTag(TEXT("EnemySpawn"))) Spawns.Add(*It);
			if (It->ActorHasTag(TEXT("GroupTerrainTraversalProbe"))) TraversalProbes.Add(*It);
		}
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(TEXT("GroupTerrainMound"))) Mounds.Add(*It);
			if (It->ActorHasTag(TEXT("GroupTerrainCover"))) Covers.Add(*It);
		}
		if (Starts.Num() != 2 || Spawns.Num() != 160 || Mounds.Num() != 16 || Covers.Num() != 8 || TraversalProbes.Num() != 18)
		{
			UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL Counts Starts=%d Spawns=%d Mounds=%d Covers=%d Saved=0"),
				Starts.Num(), Spawns.Num(), Mounds.Num(), Covers.Num());
			return false;
		}
		// PlayerGround 的索引固定对应两客户端 portal 标签，不能依赖 TActorIterator / 随机 ChoosePlayerStart 顺序。
		if (Starts[0]->PlayerStartTag == TerrainPlayerStartTags[1]) Swap(Starts[0], Starts[1]);
		if (Starts[0]->PlayerStartTag != TerrainPlayerStartTags[0] || Starts[1]->PlayerStartTag != TerrainPlayerStartTags[1])
		{
			UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL MissingOrDuplicateDeterministicPlayerStartTag Saved=0"));
			return false;
		}
		FCollisionQueryParams Query(SCENE_QUERY_STAT(FPGroupTerrainAudit), false);
		const FCollisionShape Capsule = FCollisionShape::MakeCapsule(TerrainCapsuleRadius, TerrainCapsuleHalfHeight);
		TArray<FNavLocation> PlayerGround;
		int32 ClearCapsules = 0, CompletePaths = 0, ProjectedSpawns = 0;
		for (const APlayerStart* Start : Starts)
		{
			FNavLocation Projected;
			if (!Navigation->ProjectPointToNavigation(Start->GetActorLocation(), Projected, FVector(40, 40, 200), NavData) ||
				World->OverlapBlockingTestByChannel(Projected.Location + FVector(0, 0, TerrainCapsuleHalfHeight + 2), FQuat::Identity, ECC_Pawn, Capsule, Query))
			{
				UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL PlayerCapsuleOrProjection Actor=%s Saved=0"), *Start->GetActorLabel());
				return false;
			}
			++ClearCapsules;
			PlayerGround.Add(Projected);
		}
		int32 VisibleWithinSight[2] = {0, 0};
		int32 DirectCoverBlocked = 0, DetouredPaths = 0, PathCoverCollisions = 0, DetourTurnCount = 0;
		int32 BlockedPerPlayer[2] = {0, 0}, DetouredPerPlayer[2] = {0, 0};
		double DetourRatioSum = 0, MaxDetourRatio = 1.0;
		for (const ATargetPoint* Point : Spawns)
		{
			FNavLocation Projected;
			if (!Navigation->ProjectPointToNavigation(Point->GetActorLocation(), Projected, FVector(40, 40, 200), NavData)) continue;
			++ProjectedSpawns;
			if (!World->OverlapBlockingTestByChannel(Projected.Location + FVector(0, 0, TerrainCapsuleHalfHeight + 2), FQuat::Identity, ECC_Pawn, Capsule, Query)) ++ClearCapsules;
			for (int32 Player = 0; Player < PlayerGround.Num(); ++Player)
			{
				auto* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Projected.Location, PlayerGround[Player].Location, const_cast<ARecastNavMesh*>(NavData));
				if (Path && Path->IsValid() && !Path->IsPartial()) ++CompletePaths;
				// 同一个真实 55/192 胶囊先试直达，再检查导航折线；不能拿一条视觉射线或“存在路径”冒充需要绕障碍。
				const FVector Lift(0, 0, TerrainCapsuleHalfHeight + 2);
				FHitResult DirectHit;
				const bool bDirectCoverBlocked = World->SweepSingleByChannel(DirectHit, Projected.Location + Lift, PlayerGround[Player].Location + Lift,
					FQuat::Identity, ECC_Pawn, Capsule, Query) && DirectHit.GetActor() && DirectHit.GetActor()->ActorHasTag(TEXT("GroupTerrainCover"));
				if (bDirectCoverBlocked)
				{
					++DirectCoverBlocked;
					++BlockedPerPlayer[Player];
					if (Path && Path->IsValid() && !Path->IsPartial())
					{
						double Length = 0;
						int32 Turns = 0;
						bool bPathHitsCover = false;
						for (int32 Segment = 1; Segment < Path->PathPoints.Num(); ++Segment)
						{
							Length += FVector::Dist2D(Path->PathPoints[Segment - 1], Path->PathPoints[Segment]);
							FHitResult RouteHit;
							bPathHitsCover |= World->SweepSingleByChannel(RouteHit, Path->PathPoints[Segment - 1] + Lift, Path->PathPoints[Segment] + Lift,
								FQuat::Identity, ECC_Pawn, Capsule, Query) && RouteHit.GetActor() && RouteHit.GetActor()->ActorHasTag(TEXT("GroupTerrainCover"));
							if (Segment + 1 < Path->PathPoints.Num())
							{
								const FVector Before = (Path->PathPoints[Segment] - Path->PathPoints[Segment - 1]).GetSafeNormal2D();
								const FVector After = (Path->PathPoints[Segment + 1] - Path->PathPoints[Segment]).GetSafeNormal2D();
								if (!Before.IsNearlyZero() && !After.IsNearlyZero() && FVector::DotProduct(Before, After) < FMath::Cos(FMath::DegreesToRadians(0.1f))) ++Turns;
							}
						}
						const double Ratio = Length / FMath::Max(1.0, FVector::Dist2D(Projected.Location, PlayerGround[Player].Location));
						if (bPathHitsCover) ++PathCoverCollisions;
						if (!bPathHitsCover && Turns > 0 && Ratio > 1.000001)
						{
							++DetouredPaths;
							++DetouredPerPlayer[Player];
							DetourRatioSum += Ratio;
							MaxDetourRatio = FMath::Max(MaxDetourRatio, Ratio);
							DetourTurnCount += Turns;
						}
					}
				}
				const FVector Eyes = Projected.Location + FVector(0, 0, 170);
				// 使用玩家胶囊中心作为保守目标，不因低掩体上方头部可见便宣称运行时 Sight 必定通过。
				const FVector Target = PlayerGround[Player].Location + FVector(0, 0, TerrainCapsuleHalfHeight);
				const FVector ToTarget = Target - Eyes;
				if (ToTarget.SizeSquared() <= FMath::Square(4500.0f) &&
					FVector::DotProduct(Point->GetActorForwardVector(), ToTarget.GetSafeNormal()) >= FMath::Cos(FMath::DegreesToRadians(80.0f)) &&
					!World->LineTraceTestByChannel(Eyes, Target, ECC_Visibility, Query)) ++VisibleWithinSight[Player];
			}
		}
		int32 ValidMoundSamples = 0, ValidCoverChannels = 0, WalkableMounds = 0, CompleteTerrainPaths = 0;
		float MaxSimpleVisualGap = 0.0f;
		for (AStaticMeshActor* Mound : Mounds)
		{
			const FBox Bounds = Mound->GetStaticMeshComponent()->Bounds.GetBox();
			// 不仅看模型凸起；简单物理和复杂可见三角面都必须在地面以上，且贴合误差 <= 5cm。
			for (float Offset : {0.0f, -0.18f, 0.18f})
			{
				const FVector XY(Bounds.GetCenter().X + Bounds.GetSize().X * Offset, Bounds.GetCenter().Y, 0);
				FHitResult Simple, Visual;
				FCollisionQueryParams VisualQuery = Query;
				VisualQuery.bTraceComplex = true;
				const bool bSimple = World->LineTraceSingleByChannel(Simple, XY + FVector(0, 0, 1000), XY - FVector(0, 0, 50), ECC_Visibility, Query);
				const bool bVisual = World->LineTraceSingleByChannel(Visual, XY + FVector(0, 0, 1000), XY - FVector(0, 0, 50), ECC_Visibility, VisualQuery);
				if (bSimple && bVisual && Simple.GetActor() == Mound && Visual.GetActor() == Mound && Simple.ImpactPoint.Z > 2 && Visual.ImpactPoint.Z > 2)
				{
					const float Gap = FMath::Abs(Simple.ImpactPoint.Z - Visual.ImpactPoint.Z);
					MaxSimpleVisualGap = FMath::Max(MaxSimpleVisualGap, Gap);
					if (Gap <= 5.0f) ++ValidMoundSamples;
					if (Offset == 0.0f)
					{
						FNavLocation Ground;
						if (Navigation->ProjectPointToNavigation(Simple.ImpactPoint, Ground, FVector(40, 40, 40), NavData))
						{
							++WalkableMounds;
							for (const FNavLocation& Player : PlayerGround)
							{
								auto* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Ground.Location, Player.Location, const_cast<ARecastNavMesh*>(NavData));
								if (Path && Path->IsValid() && !Path->IsPartial()) ++CompleteTerrainPaths;
							}
						}
					}
				}
			}
		}
		int32 ValidTraversalProbes = 0;
		for (const ATargetPoint* Point : TraversalProbes)
		{
			FHitResult Surface;
			const FVector XY = Point->GetActorLocation();
			FNavLocation Ground;
			if (!World->LineTraceSingleByChannel(Surface, XY + FVector(0, 0, 500), XY - FVector(0, 0, 200), ECC_Pawn, Query) ||
				!Navigation->ProjectPointToNavigation(Surface.ImpactPoint, Ground, FVector(40, 40, 40), NavData)) continue;
			++ValidTraversalProbes;
			for (const FNavLocation& Player : PlayerGround)
			{
				auto* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Ground.Location, Player.Location, const_cast<ARecastNavMesh*>(NavData));
				if (Path && Path->IsValid() && !Path->IsPartial()) ++CompleteTerrainPaths;
			}
		}
		for (AStaticMeshActor* Cover : Covers)
		{
			const UBoxComponent* Collider = Cover->FindComponentByClass<UBoxComponent>();
			if (Collider == nullptr) continue;
			const FVector Center = Collider->GetComponentLocation();
			const float HalfWidth = Collider->GetScaledBoxExtent().X + 50;
			for (ECollisionChannel Channel : {ECC_Visibility, ECC_GameTraceChannel3})
			{
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, Center - FVector(HalfWidth, 0, 0), Center + FVector(HalfWidth, 0, 0), Channel, Query) && Hit.GetActor() == Cover) ++ValidCoverChannels;
			}
		}
		const bool bValid = ProjectedSpawns == 160 && ClearCapsules == 162 && CompletePaths == 320 &&
			ValidMoundSamples == 48 && WalkableMounds == 16 && ValidTraversalProbes == 18 && CompleteTerrainPaths == 68 &&
			ValidCoverChannels == 16 && VisibleWithinSight[0] > 0 && VisibleWithinSight[1] > 0 &&
			DirectCoverBlocked > 0 && DirectCoverBlocked < 320 && DetouredPaths == DirectCoverBlocked && PathCoverCollisions == 0 &&
			BlockedPerPlayer[0] > 0 && BlockedPerPlayer[1] > 0;
		UE_LOG(LogTemp, Display, TEXT("GROUP_AI_TERRAIN AUDIT ColdLoad=%d Valid=%d SpawnsProjected=%d/160 ClearCapsules=%d/162 CompletePathsToBothPlayers=%d/320 MoundCollisionSamples=%d/48 MaxSimpleVisualGapCm=%.3f WalkableMounds=%d/16 RampStepDeckProbes=%d/18 TerrainPathsToBothPlayers=%d/68 SightWeaponCoverChecks=%d/16 InitialGeometricSightCandidates=%d,%d ActualPerceivedOrChasingNotMeasured=1 RuntimeNavigation=Static OriginalMapsUntouched=1"),
			bColdLoad, bValid, ProjectedSpawns, ClearCapsules, CompletePaths, ValidMoundSamples, MaxSimpleVisualGap, WalkableMounds, ValidTraversalProbes, CompleteTerrainPaths, ValidCoverChannels, VisibleWithinSight[0], VisibleWithinSight[1]);
		UE_LOG(LogTemp, Display, TEXT("GROUP_AI_TERRAIN ROUTE_AUDIT DirectCapsuleCoverBlocked=%d/320 DirectClearOfCover=%d/320 BlockedByPlayer=%d,%d VerifiedDetours=%d DetoursByPlayer=%d,%d PathCoverCollisions=%d MeanDetourLengthRatio=%.4f MaxDetourLengthRatio=%.4f DetourTurnCount=%d PlayerStartTags=GroupBaselineClient1,GroupBaselineClient2 RuntimeMovementOrCongestionNotMeasured=1"),
			DirectCoverBlocked, 320 - DirectCoverBlocked, BlockedPerPlayer[0], BlockedPerPlayer[1], DetouredPaths, DetouredPerPlayer[0], DetouredPerPlayer[1], PathCoverCollisions,
			DetouredPaths ? DetourRatioSum / DetouredPaths : 0.0, MaxDetourRatio, DetourTurnCount);
		if (!bValid) UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL AuditThresholdNotMet Saved=0"));
		return bValid;
	}

	// 地形与 UI 采集图严格分离：默认只生成一次；只有显式参数和精确原稿备份才可替换本工具产物。
	int32 CreateGroupTerrain(const FString& Params)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(GroupTerrainPackage, FPackageName::GetMapPackageExtension());
		if (GEditor == nullptr) return 1;
		if (FParse::Param(*Params, TEXT("AuditTerrain")))
		{
			UWorld* SavedWorld = FPackageName::DoesPackageExist(GroupTerrainPackage) ? UEditorLoadingAndSavingUtils::LoadMap(Filename) : nullptr;
			return AuditGroupTerrain(SavedWorld, true) ? 0 : 1;
		}
		FString ReplacementBackupRoot;
		const bool bReplacementRequested = FParse::Value(*Params, TEXT("ReplaceTerrainWithBackupRoot="), ReplacementBackupRoot);
		ReplacementBackupRoot.TrimQuotesInline();
		const bool bMapExists = FPackageName::DoesPackageExist(GroupTerrainPackage);
		if (bMapExists && !bReplacementRequested)
		{
			UE_LOG(LogTemp, Display, TEXT("GROUP_AI_TERRAIN EXISTS Map=%s Overwrite=0 UseAuditTerrainForColdValidation=1"), GroupTerrainPackage);
			return 0;
		}
		if (bReplacementRequested && (!bMapExists || !ValidateTerrainReplacementBackup(ReplacementBackupRoot, Filename)))
		{
			UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL ExplicitReplacementRequiresExactCurrentMapMoundAndManifestBackup Saved=0"));
			return 1;
		}
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		UStaticMesh* Container = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FactoryDistrict/Meshes/CargoContainer_mdl.CargoContainer_mdl"));
		UMaterialInterface* FloorMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FactoryDistrict/Materials/Asphalt_A_Inst.Asphalt_A_Inst"));
		UMaterialInterface* ConcreteMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FactoryDistrict/Materials/Cement_A_TintGrey.Cement_A_TintGrey"));
		if (Cube == nullptr || Sphere == nullptr || Container == nullptr || FloorMaterial == nullptr || ConcreteMaterial == nullptr) return 1;
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
		UStaticMesh* MoundMesh = nullptr;
		bool bNewMoundMesh = false;
		if (FPackageName::DoesPackageExist(GroupTerrainMoundPackage))
			MoundMesh = LoadObject<UStaticMesh>(nullptr, *(FString(GroupTerrainMoundPackage) + TEXT(".SM_GroupAITerrain_Mound")));
		else
		{
			UPackage* MeshPackage = CreatePackage(GroupTerrainMoundPackage);
			MoundMesh = DuplicateObject<UStaticMesh>(Sphere, MeshPackage, TEXT("SM_GroupAITerrain_Mound"));
			if (MoundMesh == nullptr) return 1;
			MoundMesh->SetFlags(RF_Public | RF_Standalone);
			// 派生渲染/三角碰撞数据不能依赖源资产的 transient 缓存；先完成本副本构建，再建立自己的简单凸包。
			TArray<FText> MeshBuildErrors;
			MoundMesh->Build(true, &MeshBuildErrors);
			if (!MeshBuildErrors.IsEmpty())
			{
				for (const FText& Error : MeshBuildErrors) UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN MESH_BUILD %s"), *Error.ToString());
				return 1;
			}
			MoundMesh->CreateBodySetup();
			UBodySetup* Body = MoundMesh->GetBodySetup();
			if (Body == nullptr) return 1;
			Body->RemoveSimpleCollision();
			Body->CollisionTraceFlag = CTF_UseSimpleAndComplex;
			FKConvexElem Convex;
			// 上半部更密的 218 顶点凸包匹配实际椭球表面；静态物理/导航共用，不使用缩放后失真的球碰撞。
			Convex.VertexData.Add(FVector(0, 0, 50));
			Convex.VertexData.Add(FVector(0, 0, -50));
			for (float Latitude : {10.0f, 20.0f, 30.0f, 35.0f, 40.0f, 60.0f, 90.0f, 120.0f, 150.0f})
				for (int32 Segment = 0; Segment < 24; ++Segment)
				{
					const float Phi = FMath::DegreesToRadians(Latitude), Theta = Segment * 2.0f * PI / 24.0f;
					Convex.VertexData.Add(FVector(FMath::Sin(Phi) * FMath::Cos(Theta), FMath::Sin(Phi) * FMath::Sin(Theta), FMath::Cos(Phi)) * 50.0f);
				}
			Convex.UpdateElemBox();
			Body->AggGeom.ConvexElems.Add(MoveTemp(Convex));
			Body->InvalidatePhysicsData();
			Body->CreatePhysicsMeshes();
			MoundMesh->bHasNavigationData = true;
			MoundMesh->RecreateNavCollision();
			MoundMesh->MarkPackageDirty();
			bNewMoundMesh = true;
		}
		if (MoundMesh == nullptr) return 1;
		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
		if (World == nullptr) return 1;
		World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
		World->GetWorldSettings()->Tags.Add(TEXT("GroupAITerrainSeed1337"));
		World->GetWorldSettings()->Tags.Add(TEXT("GroupAITerrainStaggeredRouteV2"));
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		bool bSpawnFailed = false;
		auto TraversalProbe = [&](const FVector& Position)
		{
			auto* Point = World->SpawnActor<ATargetPoint>(Position, FRotator::ZeroRotator, Spawn);
			if (Point == nullptr) { bSpawnFailed = true; return; }
			Point->Tags.Add(TEXT("GroupTerrainTraversalProbe"));
			Point->SetActorLabel(TEXT("Terrain_ReadOnlyTraversalProbe"));
		};
		auto Box = [&](const FString& Label, const FVector& Center, const FVector& Size, const FRotator& Rotation = FRotator(0.0, 0.0, 0.0))
		{
			auto* Actor = World->SpawnActor<AStaticMeshActor>(Center, Rotation, Spawn);
			if (Actor == nullptr) { bSpawnFailed = true; return Actor; }
			Actor->SetActorLabel(Label);
			Actor->SetActorScale3D(Size / 100.0f);
			auto* Component = Actor->GetStaticMeshComponent();
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(Cube);
			Component->SetMaterial(0, ConcreteMaterial);
			Component->SetCollisionProfileName(TEXT("BlockAll"));
			Component->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			Component->SetCollisionResponseToChannel(ECC_GameTraceChannel3, ECR_Block);
			Component->SetCanEverAffectNavigation(true);
			return Actor;
		};
		AStaticMeshActor* Floor = Box(TEXT("Terrain_120x100m_Floor"), FVector(0, 0, -50), FVector(12000, 10000, 100));
		if (Floor == nullptr) return 1;
		Floor->GetStaticMeshComponent()->SetMaterial(0, FloorMaterial);
		Box(TEXT("Terrain_Boundary_West"), FVector(-6000, 0, 300), FVector(100, 10000, 600));
		Box(TEXT("Terrain_Boundary_East"), FVector(6000, 0, 300), FVector(100, 10000, 600));
		Box(TEXT("Terrain_Boundary_North"), FVector(0, 5000, 300), FVector(12000, 100, 600));
		Box(TEXT("Terrain_Boundary_South"), FVector(0, -5000, 300), FVector(12000, 100, 600));
		// 中央错位掩体制造真实绕行，左右均保留宽分流通路；两侧土包/坡台验证落脚，不把 160 AI 挤到单一门洞。
		FRandomStream Random(1337);
		for (int32 Side : {-1, 1})
			for (int32 Row = 0; Row < 4; ++Row)
				for (int32 Column = 0; Column < 2; ++Column)
				{
					const float Height = Column == 0 ? Random.FRandRange(18, 45) : Random.FRandRange(65, 110);
					const float Footprint = Column == 0 ? 260.0f : 650.0f;
					const FVector Center(Side * (Column == 0 ? 2600.0f : 4500.0f), -2400.0f + Row * 1400.0f, -4 * Height);
					auto* Mound = World->SpawnActor<AStaticMeshActor>(Center, FRotator::ZeroRotator, Spawn);
					if (Mound == nullptr) return 1;
					Mound->SetActorLabel(FString::Printf(TEXT("Terrain_Mound_%s_%d_%d_Top%.0fcm"), Side < 0 ? TEXT("West") : TEXT("East"), Row, Column, Height));
					Mound->Tags.Add(TEXT("GroupTerrainMound"));
					Mound->SetActorScale3D(FVector(Footprint / 0.6f / 50.0f, Footprint / 0.6f / 50.0f, Height * 5 / 50.0f));
					auto* Component = Mound->GetStaticMeshComponent();
					Component->SetStaticMesh(MoundMesh);
					Component->SetMaterial(0, ConcreteMaterial);
					Component->SetCollisionProfileName(TEXT("BlockAll"));
					Component->SetCollisionResponseToChannel(ECC_GameTraceChannel3, ECR_Block);
					Component->SetCanEverAffectNavigation(true);
				}
		for (int32 Side : {-1, 1})
		{
			Box(FString::Printf(TEXT("Terrain_RaisedDeck_%d"), Side), FVector(Side * 3100, 4100, 60), FVector(1600, 1000, 120));
			TraversalProbe(FVector(Side * 3100, 4100, 200));
			// 700cm 行程上升 120cm，坡度约 9.7 度；下端埋入地面，上端接齐平台，不造悬空视觉坡。
			const float Pitch = FMath::RadiansToDegrees(FMath::Atan(120.0f / 700.0f));
			Box(FString::Printf(TEXT("Terrain_Ramp_%d"), Side), FVector(Side * 1950, 4100, 60 - 20 * FMath::Cos(FMath::DegreesToRadians(Pitch))),
				FVector(FMath::Sqrt(700.0f * 700.0f + 120.0f * 120.0f), 800, 40), FRotator(Side < 0 ? -Pitch : Pitch, 0, 0));
			TraversalProbe(FVector(Side * 1950, 4100, 150));
			for (int32 Step = 0; Step < 6; ++Step)
			{
				Box(FString::Printf(TEXT("Terrain_LowStep_%d_%d"), Side, Step), FVector(Side * 3100, 3050 + Step * 100, (Step + 1) * 10), FVector(1000, 100, (Step + 1) * 20));
				TraversalProbe(FVector(Side * 3100, 3050 + Step * 100, 200));
			}
			TraversalProbe(FVector(Side * 3100, 3600, 200));
		}
		// 工业掩体用原有素材显示，以匹配包围盒的单一静态箱形碰撞阻挡 Pawn、Sight 和 WeaponTrace。
		const FVector CoverPositions[] = {FVector(-225, -250, 0), FVector(225, -1150, 0), FVector(-2300, -450, 0), FVector(2300, -650, 0),
			FVector(-3300, 700, 0), FVector(3300, 700, 0), FVector(-1600, 3000, 0), FVector(1600, 3000, 0)};
		const FVector CoverSizes[] = {FVector(800, 300, 140), FVector(800, 300, 140), FVector(700, 260, 240), FVector(700, 260, 240),
			FVector(600, 260, 240), FVector(450, 260, 240), FVector(600, 260, 240), FVector(450, 260, 240)};
		const FBoxSphereBounds ContainerBounds = Container->GetBounds();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(CoverPositions); ++Index)
		{
			const FVector Size = CoverSizes[Index];
			const FVector Scale = Size / (ContainerBounds.BoxExtent * 2.0f);
			const FVector Center = CoverPositions[Index] + FVector(0, 0, Size.Z * 0.5f);
			auto* Cover = World->SpawnActor<AStaticMeshActor>(Center - ContainerBounds.Origin * Scale, FRotator::ZeroRotator, Spawn);
			if (Cover == nullptr) return 1;
			Cover->SetActorLabel(FString::Printf(TEXT("Terrain_IndustrialCover_%02d"), Index));
			Cover->Tags.Add(TEXT("GroupTerrainCover"));
			auto* Component = Cover->GetStaticMeshComponent();
			Component->SetStaticMesh(Container);
			Component->SetWorldScale3D(Scale);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			auto* Collision = NewObject<UBoxComponent>(Cover, TEXT("TerrainCoverCollision"));
			Cover->AddInstanceComponent(Collision);
			Collision->SetMobility(EComponentMobility::Static);
			Collision->SetBoxExtent(Size * 0.5f);
			Collision->SetCollisionProfileName(TEXT("BlockAll"));
			Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			Collision->SetCollisionResponseToChannel(ECC_GameTraceChannel3, ECR_Block);
			Collision->SetCanEverAffectNavigation(true);
			Collision->SetupAttachment(Component);
			Collision->SetAbsolute(true, true, true);
			Collision->SetWorldLocation(Center);
			Collision->RegisterComponent();
		}
		for (int32 Index = 0; Index < 2; ++Index)
		{
			auto* Start = World->SpawnActor<APlayerStart>(FVector(Index == 0 ? -400 : 400, -2200, 100), FRotator(0, 90, 0), Spawn);
			if (Start == nullptr) return 1;
			Start->SetActorLabel(FString::Printf(TEXT("Terrain_PlayerStart_%d"), Index));
			Start->PlayerStartTag = TerrainPlayerStartTags[Index];
		}
		for (int32 Row = 0; Row < 10; ++Row)
			for (int32 Column = 0; Column < 16; ++Column)
			{
				auto* Point = World->SpawnActor<ATargetPoint>(FVector((Column - 7.5f) * 220, 400 + Row * 220, 100), FRotator(0, -90, 0), Spawn);
				if (Point == nullptr) return 1;
				Point->SetActorLabel(FString::Printf(TEXT("Terrain_EnemySpawn_%03d"), Row * 16 + Column));
				Point->Tags.Add(TEXT("EnemySpawn"));
			}
		auto* Camera = World->SpawnActor<ACameraActor>(FVector(0, -400, 6200), FRotator(-90, 0, 0), Spawn);
		if (Camera == nullptr) return 1;
		Camera->Tags.Add(TEXT("EnemyUIProfileCamera"));
		Camera->SetActorLabel(TEXT("Terrain_Overview_Camera"));
		Camera->GetCameraComponent()->SetFieldOfView(90);
		auto* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 2000), FRotator(-50, -35, 0), Spawn);
		auto* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 1500), FRotator::ZeroRotator, Spawn);
		if (Sun == nullptr || Sky == nullptr) return 1;
		Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		Sun->GetLightComponent()->SetIntensity(5);
		CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent())->SetAtmosphereSunLight(true);
		Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		Sky->GetLightComponent()->SetIntensity(1);
		Sky->GetLightComponent()->SetRealTimeCaptureEnabled(true);
		World->SpawnActor<ASkyAtmosphere>();
		auto* Bounds = World->SpawnActor<ANavMeshBoundsVolume>(FVector(0, 0, 450), FRotator::ZeroRotator, Spawn);
		if (Bounds == nullptr || bSpawnFailed) return 1;
		auto* Builder = NewObject<UCubeBuilder>();
		Builder->X = 11800; Builder->Y = 9800; Builder->Z = 1300;
		UActorFactory::CreateBrushForVolumeActor(Bounds, Builder);
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
		World->UpdateWorldComponents(true, false);
		FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
		auto* Navigation = UNavigationSystemV1::GetCurrent(World);
		if (Navigation == nullptr) return 1;
		for (TActorIterator<AActor> It(World); It; ++It)
			for (UActorComponent* Component : It->GetComponents())
				if (auto* Primitive = Cast<UPrimitiveComponent>(Component))
				{
					Primitive->RecreatePhysicsState();
					FNavigationSystem::UpdateComponentData(*Primitive);
				}
		Navigation->OnNavigationBoundsUpdated(Bounds);
		Navigation->Tick(0);
		auto* Recast = Cast<ARecastNavMesh>(Navigation->GetDefaultNavDataInstance(FNavigationSystem::Create));
		if (Recast == nullptr) return 1;
		FProperty* RuntimeGeneration = FindFProperty<FProperty>(ANavigationData::StaticClass(), TEXT("RuntimeGeneration"));
		if (RuntimeGeneration == nullptr || RuntimeGeneration->ImportText_Direct(TEXT("Static"), RuntimeGeneration->ContainerPtrToValuePtr<void>(Recast), Recast, PPF_None) == nullptr) return 1;
		Navigation->Build();
		if (!AuditGroupTerrain(World, false)) return 1;
		// 构建/检查耗时期间若另一个进程改过图或备份，则拒绝覆盖；不清理、不删除旧文件。
		if (bReplacementRequested && !ValidateTerrainReplacementBackup(ReplacementBackupRoot, Filename))
		{
			UE_LOG(LogTemp, Error, TEXT("GROUP_AI_TERRAIN FAIL ReplacementInputsChangedBeforeSave Saved=0"));
			return 1;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		if (bNewMoundMesh && !UEditorLoadingAndSavingUtils::SavePackages({MoundMesh->GetOutermost()}, false)) return 1;
		if (!FEditorFileUtils::SaveMap(World, Filename)) return 1;
		UE_LOG(LogTemp, Display, TEXT("GROUP_AI_TERRAIN CREATED Map=%s Seed=1337 Layout=StaggeredRouteV2 Replacement=%d FloorCm=12000x10000 Mounds=16 Covers=8 Ramps=2 Steps=12 PlayerStarts=2 DeterministicPlayerStartTags=1 EnemySpawns=160 RuntimeNavigation=Static OriginalUIAndGameplayMapsUntouched=1 ColdAuditRequired=1"), GroupTerrainPackage, bReplacementRequested);
		return 0;
	}
}
#endif

UfpstrueEnemyUIArenaCommandlet::UfpstrueEnemyUIArenaCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UfpstrueEnemyUIArenaCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	if (FParse::Param(*Params, TEXT("Terrain")) || FParse::Param(*Params, TEXT("AuditTerrain"))) return CreateGroupTerrain(Params);
	const FString Package = TEXT("/Game/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview55x192");
	const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension());
	// 首次创建后只读复用；需要改场地时先显式保留旧证据，不能在 A/B 中途悄悄覆写。
	if (FPackageName::DoesPackageExist(Package))
	{
		UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA EXISTS Map=%s OriginalMapsUntouched=1"), *Package);
		return 0;
	}
	if (GEditor == nullptr) return 1;
	UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
	if (World == nullptr) return 1;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator, Spawn);
	if (Floor == nullptr) return 1;
	Floor->SetActorLabel(TEXT("EnemyUI_ArenaFloor_80m"));
	Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->SetActorScale3D(FVector(80, 80, 1));
	Floor->GetStaticMeshComponent()->SetCanEverAffectNavigation(true);
	auto* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 2000), FRotator(-45, -30, 0), Spawn);
	auto* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 1500), FRotator::ZeroRotator, Spawn);
	if (Sun == nullptr || Sky == nullptr) return 1;
	Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent())->SetAtmosphereSunLight(true);
	Sun->GetLightComponent()->SetIntensity(5.0f);
	Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sky->GetLightComponent()->SetIntensity(1.0f);
	Sky->GetLightComponent()->SetRealTimeCaptureEnabled(true);
	World->SpawnActor<ASkyAtmosphere>();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Start = World->SpawnActor<APlayerStart>(FVector(Index == 0 ? -350 : 350, -900, 100), FRotator(0, 90, 0), Spawn);
		if (Start == nullptr) return 1;
		Start->SetActorLabel(FString::Printf(TEXT("EnemyUI_PlayerStart_%d"), Index));
	}
	// 固定 16×10 格、120cm 间距；生成仍由正式 GameMode 的限流队列、碰撞和导航投影执行。
	TArray<FVector> SpawnGrid;
	for (int32 Row = 0; Row < 10; ++Row)
		for (int32 Column = 0; Column < 16; ++Column)
		{
			const FVector Position((Column - 7.5) * 120.0, (Row - 4.5) * 120.0, 100);
			auto* Point = World->SpawnActor<ATargetPoint>(Position, FRotator(0, -90, 0), Spawn);
			if (Point == nullptr) return 1;
			Point->Tags.Add(TEXT("EnemySpawn"));
			Point->SetActorLabel(FString::Printf(TEXT("EnemyUI_Spawn_%03d"), SpawnGrid.Num()));
			SpawnGrid.Add(Position);
		}
	// 高位镜头仅存在于这张专项图，正式第一人称 Camera/武器挂点完全不改。
	const FVector CameraPosition(0, -500, 2400);
	auto* Camera = World->SpawnActor<ACameraActor>(CameraPosition, FRotator(-90, 0, 0), Spawn);
	if (Camera == nullptr) return 1;
	Camera->Tags.Add(TEXT("EnemyUIProfileCamera"));
	Camera->SetActorLabel(TEXT("EnemyUI_RecordedOverview"));
	Camera->GetCameraComponent()->SetFieldOfView(90.0f);
	auto* Bounds = World->SpawnActor<ANavMeshBoundsVolume>(FVector(0, 0, 400), FRotator::ZeroRotator, Spawn);
	if (Bounds == nullptr) return 1;
	auto* Builder = NewObject<UCubeBuilder>();
	Builder->X = 6000; Builder->Y = 6000; Builder->Z = 1200;
	UActorFactory::CreateBrushForVolumeActor(Bounds, Builder);
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	World->UpdateWorldComponents(true, false);
	FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
	auto* Navigation = UNavigationSystemV1::GetCurrent(World);
	if (Navigation == nullptr) return 1;
	Floor->GetStaticMeshComponent()->RecreatePhysicsState();
	FNavigationSystem::UpdateComponentData(*Floor->GetStaticMeshComponent());
	Navigation->OnNavigationBoundsUpdated(Bounds);
	// 新空白图的 Bounds 和 Octree 仍在正常更新队列；先消费这轮更新再同步 Build，不能依赖旧关卡已有 NavData。
	Navigation->Tick(0.0f);
	const auto* Recast = Cast<ARecastNavMesh>(Navigation->GetDefaultNavDataInstance(FNavigationSystem::Create));
	if (Recast == nullptr || Navigation->GetSupportedAgents().Num() != 1 ||
		!FMath::IsNearlyEqual(Recast->GetConfig().AgentRadius, 55.0f) || !FMath::IsNearlyEqual(Recast->GetConfig().AgentHeight, 192.0f))
	{
		UE_LOG(LogTemp, Error, TEXT("ENEMY_UI_ARENA FAIL ExpectedSingle55x192Agent Saved=0"));
		return 1;
	}
	UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA NAV_CONFIG Radius=%.1f Height=%.1f QueryExtent=%s"),
		Recast->GetConfig().AgentRadius, Recast->GetConfig().AgentHeight, *Recast->GetConfig().DefaultQueryExtent.ToCompactString());
	UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA BOUNDS Nav=%s Floor=%s"),
		*Bounds->GetComponentsBoundingBox(true).ToString(), *Floor->GetComponentsBoundingBox(true).ToString());
	Navigation->Build();
	FNavLocation StartOnNavigation;
	const bool bStartProjected = Navigation->ProjectPointToNavigation(FVector(-350, -900, 100), StartOnNavigation, FVector(150, 150, 300));
	UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA NAV_STATE RegisteredBounds=%d Agents=%d NavData=%s FloorRelevant=%d StartProjected=%d Start=%s"),
		Navigation->GetNavigationBounds().Num(), Navigation->GetSupportedAgents().Num(), *GetNameSafe(Navigation->GetDefaultNavDataInstance()),
		Floor->GetStaticMeshComponent()->IsNavigationRelevant(), bStartProjected, *StartOnNavigation.Location.ToCompactString());
	int32 ProjectedPoints = 0, NavigablePoints = 0;
	for (const FVector& Position : SpawnGrid)
	{
		FNavLocation Projected;
		if (!Navigation->ProjectPointToNavigation(Position, Projected, FVector(40, 40, 150))) continue;
		++ProjectedPoints;
		auto* Path = bStartProjected ? UNavigationSystemV1::FindPathToLocationSynchronously(World, StartOnNavigation.Location, Projected.Location) : nullptr;
		if (Path != nullptr && Path->IsValid() && !Path->IsPartial()) ++NavigablePoints;
	}
	if (NavigablePoints != 160)
	{
		UE_LOG(LogTemp, Error, TEXT("ENEMY_UI_ARENA FAIL Navigation=%d/160 Projected=%d/160 Saved=0"), NavigablePoints, ProjectedPoints);
		return 1;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	if (!FEditorFileUtils::SaveMap(World, Filename)) return 1;
	UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA CREATED Map=%s Navigation=160/160 Grid=16x10 SpacingCm=120 Camera=%s OriginalMapsUntouched=1"),
		*Package, *CameraPosition.ToCompactString());
	return 0;
#else
	return 1;
#endif
}
