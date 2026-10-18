// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Editor/fpstrueNavigationRepairCommandlet.h"

#if WITH_EDITOR
#include "AI/NavigationSystemBase.h"
#include "AI/Navigation/NavigationElement.h"
#include "AI/Navigation/NavigationRelevantData.h"
#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "NavigationOctree.h"
#include "NavCollision.h"
#include "NavModifierComponent.h"
#include "NavModifierVolume.h"
#include "NavAreas/NavArea_Null.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshCompiler.h"
#endif

UfpstrueNavigationRepairCommandlet::UfpstrueNavigationRepairCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UfpstrueNavigationRepairCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	const FString Package = TEXT("/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked");
	const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension());
	if (FParse::Param(*Params, TEXT("AuditNavigation")))
	{
		// 现场复现点的只读审计：读取已保存导航和真实物理，不 Build、不保存、不改任何碰撞/配置。
		UWorld* AuditWorld = GEditor ? UEditorLoadingAndSavingUtils::LoadMap(Filename) : nullptr;
		if (AuditWorld == nullptr) return 1;
		FStaticMeshCompilingManager::Get().FinishAllCompilation();
		AuditWorld->UpdateWorldComponents(true, false);
		auto* AuditNav = UNavigationSystemV1::GetCurrent(AuditWorld);
		if (AuditNav == nullptr) return 1;
		const FVector ActualStart(2603.51, -5077.64, 233.14), ActualGoal(4990, -7330, 233.06);
		const FNavAgentProperties Props(55.0f, 192.0f);
		const ANavigationData* AuditData = AuditNav->GetNavDataForProps(Props);
		if (AuditData == nullptr) return 1;
		UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT BEGIN Start=%s Goal=%s Radius=55 HalfHeight=96 Nav=%s NavRadius=%.1f NavHeight=%.1f Build=0 Save=0 CollisionSettingsChanged=0"),
			*ActualStart.ToString(), *ActualGoal.ToString(), *GetNameSafe(AuditData), AuditData->GetConfig().AgentRadius, AuditData->GetConfig().AgentHeight);
		if (const auto* AuditRecast = Cast<ARecastNavMesh>(AuditData))
			UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT RESOLUTION CellSize=%.3f CellHeight=%.3f MaxSimplificationError=%.3f"),
				AuditRecast->GetCellSize(ENavigationDataResolution::Default), AuditRecast->GetCellHeight(ENavigationDataResolution::Default), AuditRecast->MaxSimplificationError);
		int32 RegisteredLightModifiers = 0;
		for (TActorIterator<AActor> It(AuditWorld); It; ++It)
			if (const auto* Modifier = FindObject<UNavModifierComponent>(*It, TEXT("LightStreetPhysicsNavModifier"));
				Modifier && Modifier->IsRegistered() && Modifier->AreaClass == UNavArea_Null::StaticClass()) ++RegisteredLightModifiers;
		UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT SAVED_LIGHT_MODIFIERS Registered=%d"), RegisteredLightModifiers);
		int32 SavedLightVolumes = 0;
		for (TActorIterator<ANavModifierVolume> It(AuditWorld); It; ++It)
			if (It->ActorHasTag(TEXT("LightStreetPhysicsNav")) && It->GetAreaClass() == UNavArea_Null::StaticClass()) ++SavedLightVolumes;
		UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT SAVED_LIGHT_VOLUMES Count=%d"), SavedLightVolumes);
		auto* AuditPath = UNavigationSystemV1::FindPathToLocationSynchronously(AuditWorld, ActualStart, ActualGoal, const_cast<ANavigationData*>(AuditData));
		if (AuditPath == nullptr || !AuditPath->IsValid() || AuditPath->IsPartial()) return 1;
		FCollisionQueryParams AuditQuery(SCENE_QUERY_STAT(FpsNavigationReadOnlyAudit), false);
		for (TActorIterator<AfpstrueCharacter> It(AuditWorld); It; ++It)
			if (FVector::Dist2D(It->GetActorLocation(), ActualStart) < 110.0f)
			{
				// 关卡中已放置的本玩家等价于运行时 Sweep 的 ignored self，不把自身胶囊误报为障碍。
				AuditQuery.AddIgnoredActor(*It);
				UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT IGNORE_SELF Actor=%s Location=%s"), *It->GetPathName(), *It->GetActorLocation().ToString());
			}
		const FCollisionShape AuditShape = FCollisionShape::MakeCapsule(55.0f, 96.0f);
		auto SweepAudit = [&](const TCHAR* Kind, int32 Index, const FVector& From, const FVector& To)
		{
			TArray<FHitResult> Hits;
			AuditWorld->SweepMultiByChannel(Hits, From, To, FQuat::Identity, ECC_Pawn, AuditShape, AuditQuery);
			int32 Blocking = 0;
			for (const FHitResult& Hit : Hits)
			{
				if (!Hit.bBlockingHit) continue;
				++Blocking;
				const UPrimitiveComponent* Component = Hit.GetComponent();
				UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT HIT Kind=%s Index=%d Actor=%s Component=%s NavAffect=%d NavRelevant=%d InitialPenetration=%d PenetrationDepth=%.3f Normal=%s Impact=%s Distance=%.3f"),
					Kind, Index, *GetPathNameSafe(Hit.GetActor()), *GetPathNameSafe(Component), Component && Component->CanEverAffectNavigation(),
					Component && Component->IsNavigationRelevant(), Hit.bStartPenetrating, Hit.PenetrationDepth, *Hit.ImpactNormal.ToString(), *Hit.ImpactPoint.ToString(), Hit.Distance);
				if (auto* MeshComponent = Cast<UStaticMeshComponent>(Hit.GetComponent()))
				{
					const UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
					const UBodySetup* Body = MeshComponent->GetBodySetup();
					const auto* NavCollision = Mesh ? Cast<UNavCollision>(Mesh->GetNavCollision()) : nullptr;
					UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT GEOMETRY Mesh=%s Transform=%s ComponentBounds=%s CustomNav=%d ExportOverride=%d ForceObstacle=%d CollisionProfile=%s TraceFlag=%d"),
						*GetPathNameSafe(Mesh), *MeshComponent->GetComponentTransform().ToHumanReadableString(), *MeshComponent->Bounds.GetBox().ToString(),
						static_cast<int32>(MeshComponent->HasCustomNavigableGeometry()), MeshComponent->bOverrideNavigationExport, MeshComponent->bForceNavigationObstacle,
						*MeshComponent->GetCollisionProfileName().ToString(), Body ? static_cast<int32>(Body->GetCollisionTraceFlag()) : -1);
					if (Body)
						UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT PHYSICS Body=%s Guid=%s Boxes=%d Spheres=%d Capsules=%d Convex=%d Bounds=%s"),
							*Body->GetPathName(), *Body->BodySetupGuid.ToString(), Body->AggGeom.BoxElems.Num(), Body->AggGeom.SphereElems.Num(),
							Body->AggGeom.SphylElems.Num(), Body->AggGeom.ConvexElems.Num(), *Body->AggGeom.CalcAABB(MeshComponent->GetComponentTransform()).ToString());
					if (NavCollision)
						UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT NAV_GEOMETRY Source=%s BodyGuid=%s CachedGuid=%s DynamicObstacle=%d HasConvex=%d GatherConvex=%d Cylinders=%d Boxes=%d ConvexVertices=%d ConvexIndices=%d TriVertices=%d TriIndices=%d LocalBounds=%s WorldBounds=%s"),
							*NavCollision->GetPathName(), Body ? *Body->BodySetupGuid.ToString() : TEXT("None"), *NavCollision->BodySetupGuid.ToString(),
							NavCollision->IsDynamicObstacle(), NavCollision->HasConvexGeometry(), NavCollision->bGatherConvexGeometry, NavCollision->CylinderCollision.Num(), NavCollision->BoxCollision.Num(),
							NavCollision->GetConvexCollision().VertexBuffer.Num(), NavCollision->GetConvexCollision().IndexBuffer.Num(),
							NavCollision->GetTriMeshCollision().VertexBuffer.Num(), NavCollision->GetTriMeshCollision().IndexBuffer.Num(),
							*NavCollision->GetBounds().ToString(), *NavCollision->GetBounds().TransformBy(MeshComponent->GetComponentTransform()).ToString());
				}
			}
			UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT SWEEP Kind=%s Index=%d From=%s To=%s BlockingHits=%d"), Kind, Index, *From.ToString(), *To.ToString(), Blocking);
		};
		int32 Next = 0;
		while (AuditPath->PathPoints.IsValidIndex(Next) && FVector::Dist2D(ActualStart, AuditPath->PathPoints[Next]) < 20.0f) ++Next;
		if (!AuditPath->PathPoints.IsValidIndex(Next)) return 1;
		const FVector Direction = (AuditPath->PathPoints[Next] - ActualStart).GetSafeNormal2D();
		SweepAudit(TEXT("ActualStartForward100"), Next, ActualStart, ActualStart + Direction * 100.0f);
		for (int32 Index = 0; Index < AuditPath->PathPoints.Num(); ++Index)
		{
			UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT PATH_POINT Index=%d Location=%s Distance2DFromStart=%.3f"), Index,
				*AuditPath->PathPoints[Index].ToString(), FVector::Dist2D(ActualStart, AuditPath->PathPoints[Index]));
			if (Index > 0)
			{
				// 路径点在 Nav 表面，抬高一个真实胶囊半高+2cm；实际起点仍用上面未改动的世界坐标。
				const FVector Lift(0, 0, 98.0f);
				SweepAudit(TEXT("NavSegmentCapsule"), Index, AuditPath->PathPoints[Index - 1] + Lift, AuditPath->PathPoints[Index] + Lift);
			}
		}
		UE_LOG(LogTemp, Display, TEXT("NAV_AUDIT COMPLETE Points=%d Save=0 Build=0"), AuditPath->PathPoints.Num());
		return 0;
	}
	FString BackupRoot;
	FParse::Value(*Params, TEXT("BackupRoot="), BackupRoot);
	BackupRoot.TrimQuotesInline();
	BackupRoot = FPaths::ConvertRelativePathToFull(BackupRoot);
	// 必须先有精确备份；工具不在内存修改后才补一份所谓“原稿”。
	const FString BackupMap = FPaths::Combine(BackupRoot, TEXT("Content/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked.umap"));
	if (GEditor == nullptr || !FParse::Param(*Params, TEXT("RepairNavigation")) ||
		!FPaths::FileExists(FPaths::Combine(BackupRoot, TEXT("manifest.json"))) || !FPaths::FileExists(BackupMap))
	{
		UE_LOG(LogTemp, Error, TEXT("NAV_REPAIR FAIL MissingExplicitRepairOrOriginalBackup Saved=0"));
		return 1;
	}
	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Filename);
	if (World == nullptr) return 1;
	auto* ExistingNavigation = UNavigationSystemV1::GetCurrent(World);
	const auto* ExistingData = ExistingNavigation ? ExistingNavigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) : nullptr;
	const bool bOriginalGameWorld = World->IsGameWorld();
	const bool bOriginalStatic = ExistingData && ExistingData->GetRuntimeGenerationMode() == ERuntimeGenerationType::Static;
	const bool bOriginalOctree = ExistingNavigation && ExistingNavigation->GetNavOctree() != nullptr;
	const int32 OriginalWorldType = static_cast<int32>(World->WorldType.GetValue());
	// 某些离线 LoadMap 留下 Game+Static 世界；只在确实没有 Octree 时临时使用标准 Editor 构建生命周期。
	const bool bTemporaryEditorWorld = bOriginalGameWorld && bOriginalStatic && !bOriginalOctree;
	TGuardValue<TEnumAsByte<EWorldType::Type>> EditorBuildWorldType(World->WorldType,
		bTemporaryEditorWorld ? TEnumAsByte<EWorldType::Type>(EWorldType::Editor) : World->WorldType);
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	World->UpdateWorldComponents(true, false);
	FNavigationSystem::AddNavigationSystemToWorld(*World, FNavigationSystemRunMode::EditorMode);
	auto* Navigation = UNavigationSystemV1::GetCurrent(World);
	if (Navigation == nullptr || Navigation->GetSupportedAgents().Num() != 1) return 1;
	UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR WORLD_INIT OriginalType=%d OriginalGame=%d OriginalStatic=%d OriginalOctree=%d TemporaryEditor=%d CurrentType=%d CurrentGame=%d CurrentOctree=%d OperationMode=%d"),
		OriginalWorldType, bOriginalGameWorld, bOriginalStatic, bOriginalOctree, bTemporaryEditorWorld,
		static_cast<int32>(World->WorldType.GetValue()), World->IsGameWorld(), Navigation->GetNavOctree() != nullptr, static_cast<int32>(Navigation->GetOperationMode()));
	const FNavDataConfig Supported = Navigation->GetSupportedAgents()[0];
	if (!FMath::IsNearlyEqual(Supported.AgentRadius, 55.0f) || !FMath::IsNearlyEqual(Supported.AgentHeight, 192.0f))
	{
		UE_LOG(LogTemp, Error, TEXT("NAV_REPAIR FAIL SupportedAgent Radius=%.1f Height=%.1f Saved=0"), Supported.AgentRadius, Supported.AgentHeight);
		return 1;
	}

	// 完成 Mesh 编译后重新注册既有真实碰撞到导航 Octree，不改 CollisionProfile/BodySetup/美术组件。
	int32 StaticMeshes = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components(*It);
		for (auto* Component : Components)
		{
			Component->RecreatePhysicsState();
			FNavigationSystem::UpdateComponentData(*Component);
			++StaticMeshes;
		}
	}
	const bool bRepairLights = FParse::Param(*Params, TEXT("RepairLightNavigation"));
	int32 VerifiedLightVolumes = 0;
	if (bRepairLights)
	{
		int32 VerifiedLights = 0, AddedVolumes = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components(*It);
			for (auto* Component : Components)
			{
				const UStaticMesh* Mesh = Component->GetStaticMesh();
				if (Mesh == nullptr || Mesh->GetPathName() != TEXT("/Game/FactoryDistrict/Meshes/Light_Street.Light_Street")) continue;
				const UBodySetup* Body = Component->GetBodySetup();
				if (Body == nullptr || Body->AggGeom.BoxElems.Num() != 3 || Body->AggGeom.GetElementCount() != 3) return 1;
				++VerifiedLights;
				FHitResult Ground;
				FCollisionQueryParams GroundQuery(SCENE_QUERY_STAT(LightNavigationGround), false, *It);
				const FVector Base = Component->GetComponentLocation();
				if (!World->LineTraceSingleByChannel(Ground, Base + FVector(0, 0, 300), Base - FVector(0, 0, 300), ECC_WorldStatic, GroundQuery) || Ground.ImpactNormal.Z < 0.7f)
				{
					UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR LIGHT_SKIP Actor=%s Reason=NoPhysicalWalkableGroundWithin600Cm"), *It->GetName());
					break;
				}
				for (int32 Index = 0; Index < Body->AggGeom.BoxElems.Num(); ++Index)
				{
					const FKBoxElem& Box = Body->AggGeom.BoxElems[Index];
					const FTransform BoxWorld = Box.GetTransform() * Component->GetComponentTransform();
					const FVector Scale = BoxWorld.GetScale3D().GetAbs();
					if (!FMath::IsNearlyEqual(Scale.X, Scale.Y) || !FMath::IsNearlyEqual(Scale.Y, Scale.Z)) return 1;
					const FVector Size = FVector(Box.X, Box.Y, Box.Z) * Scale;
					const FBox Bounds = FBox(-FVector(Box.X, Box.Y, Box.Z) * 0.5, FVector(Box.X, Box.Y, Box.Z) * 0.5).TransformBy(BoxWorld);
					// 只修玩家高度带中真正存在的阻挡 Box；高处灯臂不能用整体包围盒堵住地面。
					if (Bounds.Min.Z > Ground.ImpactPoint.Z + 192.0 || Bounds.Max.Z < Ground.ImpactPoint.Z) continue;
					const FName VolumeName(*FString::Printf(TEXT("LightPhysicsNav_%s_Box%d"), *It->GetName(), Index));
					auto* Volume = FindObject<ANavModifierVolume>(World->PersistentLevel, *VolumeName.ToString());
					if (Volume == nullptr)
					{
						FActorSpawnParameters Spawn;
						Spawn.Name = VolumeName;
						Spawn.OverrideLevel = World->PersistentLevel;
						Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
						Volume = World->SpawnActor<ANavModifierVolume>(BoxWorld.GetLocation(), BoxWorld.Rotator(), Spawn);
						if (Volume == nullptr) return 1;
						auto* Builder = NewObject<UCubeBuilder>();
						Builder->X = static_cast<float>(Size.X); Builder->Y = static_cast<float>(Size.Y); Builder->Z = static_cast<float>(Size.Z);
						UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
						Volume->SetAreaClass(UNavArea_Null::StaticClass());
						Volume->Tags.Add(TEXT("LightStreetPhysicsNav"));
						Volume->SetActorLabel(VolumeName.ToString());
						++AddedVolumes;
					}
					if (Volume->GetAreaClass() != UNavArea_Null::StaticClass()) return 1;
					Volume->RebuildNavigationData();
					++VerifiedLightVolumes;
					UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR LIGHT_VOLUME Actor=%s Box=%d Volume=%s Center=%s Rotation=%s Size=%s GroundZ=%.3f"),
						*It->GetName(), Index, *Volume->GetName(), *BoxWorld.GetLocation().ToString(), *BoxWorld.Rotator().ToString(), *Size.ToString(), Ground.ImpactPoint.Z);
				}
				break;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR LIGHTS Verified=%d Volumes=%d Added=%d PhysicsChanged=0 SourceMeshChanged=0"), VerifiedLights, VerifiedLightVolumes, AddedVolumes);
		if (VerifiedLights == 0 || VerifiedLightVolumes == 0) return 1;
	}
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It) Navigation->OnNavigationBoundsUpdated(*It);
	Navigation->Tick(0.0f);
	auto* Recast = Cast<ARecastNavMesh>(Navigation->GetDefaultNavDataInstance(FNavigationSystem::Create));
	if (Recast == nullptr) return 1;
	// 引擎按 SupportedAgents 拒绝旧尺寸并创建匹配的新 NavData；不要注销已匹配的实例，
	// 否则编辑器的 CrowdManager 会在延迟重新注册前看到一个人为制造的空注册表。
	if (!FMath::IsNearlyEqual(Recast->GetConfig().AgentRadius, Supported.AgentRadius) ||
		!FMath::IsNearlyEqual(Recast->GetConfig().AgentHeight, Supported.AgentHeight))
	{
		UE_LOG(LogTemp, Error, TEXT("NAV_REPAIR FAIL DefaultNavDataDoesNotMatchSupportedAgent Saved=0"));
		return 1;
	}
	if (Navigation->GetNavDataForProps(Supported) != Recast) return 1;
	for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
	{
		UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR NAV_ACTOR Actor=%s Package=%s External=%d Radius=%.1f Height=%.1f"),
			*It->GetPathName(), *It->GetPackage()->GetName(), It->IsPackageExternal(), It->GetConfig().AgentRadius, It->GetConfig().AgentHeight);
		if (It->IsPackageExternal())
		{
			// 盘点若发现了未备份的外置 Nav 包，拒绝保存；不通过 SaveDirtyPackages 覆写其他资产。
			UE_LOG(LogTemp, Error, TEXT("NAV_REPAIR FAIL ExternalNavigationPackageNeedsExplicitBackup Saved=0"));
			return 1;
		}
	}
	Navigation->Build();
	int32 OctreeEntries = 0, GeometryEntries = 0, AreaCount = 0;
	if (const FNavigationOctree* Octree = Navigation->GetNavOctree())
		Octree->FindAllElements([&](const FNavigationOctreeElement& Element)
		{
			++OctreeEntries;
			if (Element.Data->HasGeometry()) ++GeometryEntries;
			AreaCount += Element.Data->Modifiers.GetAreas().Num();
		});
	UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR BUILD_INPUT OctreeEntries=%d GeometryEntries=%d Areas=%d"), OctreeEntries, GeometryEntries, AreaCount);
	if (OctreeEntries == 0 || GeometryEntries == 0 || AreaCount < VerifiedLightVolumes) return 1;
	UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR CONFIG Radius=%.1f Height=%.1f Extent=%s StaticMeshes=%d"),
		Recast->GetConfig().AgentRadius, Recast->GetConfig().AgentHeight, *Recast->GetConfig().DefaultQueryExtent.ToCompactString(), StaticMeshes);
	for (const auto* Character : {Cast<const ACharacter>(GetDefault<AfpstrueCharacter>()), Cast<const ACharacter>(GetDefault<AfpstrueEnemyCharacter>())})
	{
		const auto* Capsule = Character->GetCapsuleComponent();
		const float Radius = Capsule->GetScaledCapsuleRadius(), Height = Capsule->GetScaledCapsuleHalfHeight() * 2.0f;
		FNavAgentProperties Properties(Radius, Height);
		const auto* Selected = Navigation->GetNavDataForProps(Properties);
		UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR CAPSULE Source=NativeCDO Class=%s Radius=%.1f HalfHeight=%.1f Selected=%s"),
			*Character->GetClass()->GetName(), Radius, Height * 0.5f, *GetNameSafe(Selected));
		if (Selected != Recast || Recast->GetConfig().AgentRadius < Radius || Recast->GetConfig().AgentHeight < Height) return 1;
	}

	// 使用已实机复现的同一挂车卡点和同一起终点。先证明物理障碍仍在，再要求新路径不穿它。
	const FVector OriginalStart(-3584.48, -2783.36, 232.80), OriginalGoal(-2190.0, 8860.0, 220.57);
	const FCollisionShape PlayerShape = FCollisionShape::MakeCapsule(55.0f, 96.0f);
	FHitResult OldObstacle;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FpsNavigationRepair), false);
	const bool bOldBlocked = World->SweepSingleByChannel(OldObstacle, FVector(-3587, -1000, 222.57), FVector(-3587, -800, 222.57),
		FQuat::Identity, ECC_Pawn, PlayerShape, Query);
	UPrimitiveComponent* Obstacle = OldObstacle.GetComponent();
	UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR ORIGINAL_OBSTACLE Blocked=%d Component=%s Impact=%s Normal=%s"),
		bOldBlocked, *GetPathNameSafe(Obstacle), *OldObstacle.ImpactPoint.ToCompactString(), *OldObstacle.ImpactNormal.ToCompactString());
	if (!bOldBlocked || Obstacle == nullptr || OldObstacle.ImpactNormal.Z > 0.7f) return 1;
	FNavLocation Start, Goal;
	if (!Navigation->ProjectPointToNavigation(OriginalStart, Start, FVector(150, 150, 300)) ||
		!Navigation->ProjectPointToNavigation(OriginalGoal, Goal, FVector(150, 150, 300))) return 1;
	auto* Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, Start.Location, Goal.Location, Recast);
	if (Path == nullptr || !Path->IsValid() || Path->IsPartial()) return 1;
	int32 BlockedSegments = 0;
	for (int32 Index = 1; Index < Path->PathPoints.Num(); ++Index)
	{
		FHitResult Hit;
		const FVector Lift(0, 0, 98.0f);
		if (Obstacle->SweepComponent(Hit, Path->PathPoints[Index - 1] + Lift, Path->PathPoints[Index] + Lift, FQuat::Identity, PlayerShape, false)) ++BlockedSegments;
		UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR PATH_POINT Index=%d Location=%s"), Index, *Path->PathPoints[Index].ToCompactString());
	}
	UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR PATH Complete=1 Points=%d OldObstacleBlockedSegments=%d"), Path->PathPoints.Num(), BlockedSegments);
	if (BlockedSegments != 0) return 1;
	if (bRepairLights || FParse::Param(*Params, TEXT("VerifyLightPath")))
	{
		const FVector LightStart(2603.51, -5077.64, 233.14), LightGoal(4990, -7330, 233.06);
		FHitResult PhysicalLightHit;
		const bool bLightStillBlocks = World->SweepSingleByChannel(PhysicalLightHit, LightStart, FVector(2509.812, -5112.579, 233.14),
			FQuat::Identity, ECC_Pawn, PlayerShape, Query);
		auto* Light = Cast<UStaticMeshComponent>(PhysicalLightHit.GetComponent());
		if (!bLightStillBlocks || Light == nullptr || Light->GetOwner()->GetName() != TEXT("Light_Street51")) return 1;
		auto* LightPath = UNavigationSystemV1::FindPathToLocationSynchronously(World, LightStart, LightGoal, Recast);
		if (LightPath == nullptr || !LightPath->IsValid() || LightPath->IsPartial() || LightPath->PathPoints.Num() < 2) return 1;
		int32 LightBlockedSegments = 0;
		for (int32 Index = 0; Index < LightPath->PathPoints.Num(); ++Index)
		{
			FHitResult Hit;
			const FVector To = LightPath->PathPoints[Index] + FVector(0, 0, 98.0f);
			const FVector From = Index == 0 ? LightStart : LightPath->PathPoints[Index - 1] + FVector(0, 0, 98.0f);
			if (Light->SweepComponent(Hit, From, To, FQuat::Identity, PlayerShape, false)) ++LightBlockedSegments;
			UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR LIGHT_PATH_POINT Index=%d Location=%s"), Index, *LightPath->PathPoints[Index].ToString());
		}
		UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR LIGHT_PATH Complete=1 Points=%d PhysicalOriginalDirectionStillBlocked=%d BlockedSegments=%d"),
			LightPath->PathPoints.Num(), bLightStillBlocks, LightBlockedSegments);
		if (LightBlockedSegments != 0) return 1;
	}
	if (!FEditorFileUtils::SaveMap(World, Filename)) return 1;
	UE_LOG(LogTemp, Display, TEXT("NAV_REPAIR SAVED Map=%s Radius=55 Height=192 PhysicsSettingsChanged=0 FactorySourceChanged=0 RuntimeCMCVerificationPending=1"), *Package);
	return 0;
#else
	return 1;
#endif
}
