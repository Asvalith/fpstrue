// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Editor/fpstrueEnemyUIArenaCommandlet.h"

#if WITH_EDITOR
#include "ActorFactories/ActorFactory.h"
#include "AI/NavigationSystemBase.h"
#include "Builders/CubeBuilder.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
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
#include "FileHelpers.h"
#include "Game/fpstrueMultiplayer.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "StaticMeshCompiler.h"
#include "UObject/UObjectGlobals.h"

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
	// 1. 输出仅为这个固定专项包；已存在时不加载重写，也不隐式删除或替换地图。
	const FString Package = TEXT("/Game/PerformanceCandidates/EnemyUI160/EnemyUI160_ArenaOverview55x192");
	const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension());
	// 首次创建后只读复用；需要改场地时先显式保留旧证据，不能在 A/B 中途悄悄覆写。
	if (FPackageName::DoesPackageExist(Package))
	{
		UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA EXISTS Map=%s OriginalMapsUntouched=1"), *Package);
		return 0;
	}
	if (GEditor == nullptr) return 1;
	// 2. 在新的内存 World 搭建场地、出生点及专项镜头，尚不提交任何资产文件。
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
		Start->PlayerStartTag = FName(*FString::Printf(TEXT("GroupBaselineClient%d"), Index + 1));
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
	// 3. 仅接受 55×192 的单导航代理配置，避免把不同胶囊条件混进同一性能场地。
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
	// 投影成功不等于路径可达；每个格点还必须存在从固定玩家起点到达的完整路径。
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
	// 4. 全部点通过后才保存这个新地图；本工具不通过 SaveDirtyPackages 提交其他脏资产。
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	if (!FEditorFileUtils::SaveMap(World, Filename)) return 1;
	UE_LOG(LogTemp, Display, TEXT("ENEMY_UI_ARENA CREATED Map=%s Navigation=160/160 Grid=16x10 SpacingCm=120 Camera=%s OriginalMapsUntouched=1"),
		*Package, *CameraPosition.ToCompactString());
	return 0;
#else
	return 1;
#endif
}
