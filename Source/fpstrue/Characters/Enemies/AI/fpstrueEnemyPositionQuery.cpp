// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueEnemyPositionQuery.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Pathfinding.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

CSV_DECLARE_CATEGORY_EXTERN(fpstrueEQS);

namespace
{
	// 参数是真实 float，不能使用 SetIntParam 的位模式编码；缺少或非有限位置拒绝生成结果。
	float ReadParam(const FEnvQueryInstance& QueryInstance, const TCHAR* Name, float DefaultValue)
	{
		const float* Value = QueryInstance.NamedParams.Find(FName(Name));
		return Value != nullptr && FMath::IsFinite(*Value) ? *Value : DefaultValue;
	}

	bool ReadSnapshot(const FEnvQueryInstance& QueryInstance, const TCHAR* XName, const TCHAR* YName,
		const TCHAR* ZName, FVector& OutLocation)
	{
		const float* X = QueryInstance.NamedParams.Find(FName(XName));
		const float* Y = QueryInstance.NamedParams.Find(FName(YName));
		const float* Z = QueryInstance.NamedParams.Find(FName(ZName));
		if (X == nullptr || Y == nullptr || Z == nullptr ||
			!FMath::IsFinite(*X) || !FMath::IsFinite(*Y) || !FMath::IsFinite(*Z))
		{
			return false;
		}
		OutLocation = FVector(*X, *Y, *Z);
		return true;
	}

	int32 ReadCount(const FEnvQueryInstance& QueryInstance, const TCHAR* Name, int32 DefaultCount,
		int32 MinCount, int32 MaxCount)
	{
		return FMath::RoundToInt(FMath::Clamp(ReadParam(QueryInstance, Name, static_cast<float>(DefaultCount)),
			static_cast<float>(MinCount), static_cast<float>(MaxCount)));
	}

	float ReadRadius(const FEnvQueryInstance& QueryInstance, const TCHAR* Name, float DefaultRadius)
	{
		return FMath::Clamp(ReadParam(QueryInstance, Name, DefaultRadius), 50.0f, 5000.0f);
	}
}

UfpstrueEnvQueryItemType_Position::UfpstrueEnvQueryItemType_Position(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ValueSize = sizeof(FFPEnemyQueryPoint);
}

FFPEnemyQueryPoint UfpstrueEnvQueryItemType_Position::GetValue(const uint8* RawData)
{
	return GetValueFromMemory<FFPEnemyQueryPoint>(RawData);
}

void UfpstrueEnvQueryItemType_Position::SetValue(uint8* RawData, const FFPEnemyQueryPoint& Value)
{
	SetValueInMemory<FFPEnemyQueryPoint>(RawData, Value);
}

FVector UfpstrueEnvQueryItemType_Position::GetItemLocation(const uint8* RawData) const
{
	return GetValue(RawData).Nav.Location;
}

FNavLocation UfpstrueEnvQueryItemType_Position::GetItemNavLocation(const uint8* RawData) const
{
	return GetValue(RawData).Nav;
}

void UfpstrueEnvQueryItemType_Position::SetItemNavLocation(uint8* RawData, const FNavLocation& Value) const
{
	FFPEnemyQueryPoint Point = GetValue(RawData);
	Point.Nav = Value;
	SetValue(RawData, Point);
}

UfpstrueEnvQueryGenerator_Positions::UfpstrueEnvQueryGenerator_Positions(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ItemType = UfpstrueEnvQueryItemType_Position::StaticClass();
	bAutoSortTests = true;
	OptionName = TEXT("Enemy bounded position candidates");
}

void UfpstrueEnvQueryGenerator_Positions::GenerateItems(FEnvQueryInstance& QueryInstance) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEQS_GeneratePositions);
	CSV_SCOPED_TIMING_STAT(fpstrueEQS, GeneratePositions);
	FVector Origin;
	FVector Threat;
	if (!ReadSnapshot(QueryInstance, TEXT("OriginX"), TEXT("OriginY"), TEXT("OriginZ"), Origin) ||
		!ReadSnapshot(QueryInstance, TEXT("ThreatX"), TEXT("ThreatY"), TEXT("ThreatZ"), Threat))
	{
		return;
	}

	const bool bCover = ReadParam(QueryInstance, TEXT("Cover"), bCoverQuery ? 1.0f : 0.0f) > 0.5f;
	const int32 InnerCount = ReadCount(QueryInstance, TEXT("InnerCount"), bCover ? 12 : 8, bCover ? 1 : 4, 24);
	const int32 OuterCount = ReadCount(QueryInstance, TEXT("OuterCount"), 12, bCover ? 1 : 4, 32);
	const float InnerRadius = ReadRadius(QueryInstance, TEXT("InnerRadius"), bCover ? 450.0f : 250.0f);
	const float OuterRadius = ReadRadius(QueryInstance, TEXT("OuterRadius"), bCover ? 800.0f : 430.0f);
	const float InnerApproach = ReadRadius(QueryInstance, TEXT("InnerApproach"), 120.0f);
	const float OuterApproach = ReadRadius(QueryInstance, TEXT("OuterApproach"), 220.0f);
	QueryInstance.ReserveItemData((InnerCount + OuterCount) * (bCover ? 1 : 4));

	// 所有请求复用无状态节点；SlotIndex 固定采用先内环、后外环的布局顺序。
	const auto AddRing = [&QueryInstance, &Origin, bCover](int32 Count, int32 SlotBase, float Radius, float ApproachRadius)
	{
		const float Sector = 2.0f * PI / static_cast<float>(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float BaseAngle = Sector * static_cast<float>(Index);
			const auto AddPoint = [&QueryInstance, &Origin, SlotBase, Index](float Angle, float PointRadius,
				int32 Kind, float Preference)
			{
				FFPEnemyQueryPoint Point;
				Point.Nav = FNavLocation(Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * PointRadius);
				Point.SlotIndex = SlotBase + Index;
				Point.Kind = Kind;
				Point.Preference = Preference;
				QueryInstance.AddItemData<UfpstrueEnvQueryItemType_Position>(Point);
			};

			AddPoint(BaseAngle, Radius, bCover ? 2 : 0, 0.0f);
			if (!bCover)
			{
				// 每槽一个原角接近点和左右两个小偏移，避免障碍恰好挡住唯一候选。
				for (const float Offset : { 0.0f, -0.2f, 0.2f })
				{
					AddPoint(BaseAngle + Offset * Sector, ApproachRadius, 1, Offset);
				}
			}
		}
	};
	AddRing(InnerCount, 0, InnerRadius, InnerApproach);
	AddRing(OuterCount, InnerCount, OuterRadius, OuterApproach);
}

UfpstrueEnvQueryTest_Position::UfpstrueEnvQueryTest_Position(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ValidItemType = UfpstrueEnvQueryItemType_Position::StaticClass();
	Cost = EEnvTestCost::Medium;
	SetWorkOnFloatValues(true);
	TestPurpose = EEnvTestPurpose::FilterAndScore;
	FilterType = EEnvTestFilterType::Minimum;
	FloatValueMin.DefaultValue = 0.01f;
	ScoringEquation = EEnvTestScoreEquation::Linear;
	ClampMinType = EEnvQueryTestClamping::SpecifiedValue;
	ClampMaxType = EEnvQueryTestClamping::SpecifiedValue;
	ScoreClampMin.DefaultValue = 0.0f;
	ScoreClampMax.DefaultValue = 1.0f;
}

void UfpstrueEnvQueryTest_Position::RunTest(FEnvQueryInstance& QueryInstance) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEQS_ValidatePositions);
	CSV_SCOPED_TIMING_STAT(fpstrueEQS, ValidatePositions);
	UWorld* World = QueryInstance.World;
	UNavigationSystemV1* NavSystem = World != nullptr ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	const ANavigationData* NavData = NavSystem != nullptr ? FEQSHelpers::FindNavigationDataForQuery(QueryInstance) : nullptr;
	FVector Origin;
	FVector Threat;
	const bool bHasSnapshot =
		ReadSnapshot(QueryInstance, TEXT("OriginX"), TEXT("OriginY"), TEXT("OriginZ"), Origin) &&
		ReadSnapshot(QueryInstance, TEXT("ThreatX"), TEXT("ThreatY"), TEXT("ThreatZ"), Threat);
	if (NavSystem == nullptr || NavData == nullptr || !bHasSnapshot)
	{
		// 导航未生成时全部失败，不能把未经投影的原始位置当成可移动目标。
		for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
		{
			It.ForceItemState(EEnvItemStatus::Failed);
		}
		return;
	}

	const bool bCover = ReadParam(QueryInstance, TEXT("Cover"), bCoverQuery ? 1.0f : 0.0f) > 0.5f;
	float CapsuleRadius = 42.0f;
	float CapsuleHalfHeight = 96.0f;
	if (const ACharacter* Character = Cast<ACharacter>(QueryInstance.Owner.Get()))
	{
		Character->GetCapsuleComponent()->GetScaledCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
	}
	CapsuleRadius = FMath::Max(1.0f, CapsuleRadius);
	CapsuleHalfHeight = FMath::Max(CapsuleRadius, CapsuleHalfHeight);
	const FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight);
	const FVector ProjectionExtent(
		FMath::Max(1.0f, ReadParam(QueryInstance, TEXT("ProjectionX"), 120.0f)),
		FMath::Max(1.0f, ReadParam(QueryInstance, TEXT("ProjectionY"), 120.0f)),
		FMath::Max(1.0f, ReadParam(QueryInstance, TEXT("ProjectionZ"), 250.0f)));
	FCollisionQueryParams OccupancyParams(SCENE_QUERY_STAT(FPEnemyPositionOccupancy), false);
	FCollisionQueryParams SightParams(SCENE_QUERY_STAT(FPEnemyPositionSight), true);
	OccupancyParams.bIgnoreTouches = true;
	SightParams.bIgnoreTouches = true;
	if (const AActor* OwnerActor = Cast<AActor>(QueryInstance.Owner.Get()))
	{
		OccupancyParams.AddIgnoredActor(OwnerActor);
		SightParams.AddIgnoredActor(OwnerActor);
	}
	// 忽略真实 Pawn 及其所有组件，包括玩家；这里只排除碰撞身份，不读取实时目标位置。
	// 每个测试分片构造一次忽略表，避免每个候选枚举友方，动态人群也不会变成虚假的墙或站位障碍。
	for (TActorIterator<APawn> PawnIt(World); PawnIt; ++PawnIt)
	{
		OccupancyParams.AddIgnoredActor(*PawnIt);
		SightParams.AddIgnoredActor(*PawnIt);
	}
	FCollisionResponseParams EnvironmentResponses(ECR_Ignore);
	EnvironmentResponses.CollisionResponse.SetResponse(ECC_WorldStatic, ECR_Block);
	EnvironmentResponses.CollisionResponse.SetResponse(ECC_WorldDynamic, ECR_Block);

	// 正常 EQS 执行在游戏线程分片；每个候选最多一次投影、一次胶囊查询和两条环境射线。
	NavData->BeginBatchQuery();
	for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
	{
		FFPEnemyQueryPoint Point = UfpstrueEnvQueryItemType_Position::GetValue(It.GetItemData());
		const bool bValidKind = bCover ? Point.Kind == 2 : (Point.Kind == 0 || Point.Kind == 1);
		FNavLocation Projected;
		if (!bValidKind || !NavSystem->ProjectPointToNavigation(Point.Nav.Location, Projected,
			ProjectionExtent, NavData))
		{
			It.ForceItemState(EEnvItemStatus::Failed);
			continue;
		}
		Point.Nav = Projected;
		UfpstrueEnvQueryItemType_Position::SetValue(It.GetItemData(), Point);

		// Kind0 只证明布局位置能投影；接近点和掩体才检查完整角色能否站下。
		if (Point.Kind != 0)
		{
			const FVector CapsuleCenter = Projected.Location + FVector(0.0f, 0.0f, CapsuleHalfHeight + 3.0f);
			if (World->OverlapBlockingTestByChannel(CapsuleCenter, FQuat::Identity, ECC_Pawn,
				CapsuleShape, OccupancyParams, EnvironmentResponses))
			{
				It.ForceItemState(EEnvItemStatus::Failed);
				continue;
			}
		}

		const FVector Chest = Projected.Location + FVector(0.0f, 0.0f, CapsuleHalfHeight);
		if (Point.Kind == 1 && World->LineTraceTestByChannel(Chest, Threat, ECC_Visibility, SightParams, EnvironmentResponses))
		{
			It.ForceItemState(EEnvItemStatus::Failed);
			continue;
		}
		if (Point.Kind == 2)
		{
			const FVector Head = Projected.Location + FVector(0.0f, 0.0f, CapsuleHalfHeight * 1.75f);
			// 胸部和头部都必须被场景阻挡；空旷地面或另一名敌人不能冒充掩体。
			if (!World->LineTraceTestByChannel(Threat, Chest, ECC_Visibility, SightParams, EnvironmentResponses) ||
				!World->LineTraceTestByChannel(Threat, Head, ECC_Visibility, SightParams, EnvironmentResponses))
			{
				It.ForceItemState(EEnvItemStatus::Failed);
				continue;
			}
		}

		const float Score = Point.Kind == 2
			? 1.0f / (1.0f + static_cast<float>(FVector::Dist2D(Origin, Projected.Location)) / 800.0f)
			: 1.0f - FMath::Clamp(FMath::Abs(Point.Preference), 0.0f, 0.9f);
		It.SetScore(TestPurpose, FilterType, Score, 0.01f, 1.0f);
	}
	NavData->FinishBatchQuery();
	QueryInstance.Options[QueryInstance.OptionIndex].bHasNavLocations = true;
}

namespace
{
UEnvQuery* CreateEnemyPositionQueryTemplate(UObject& Outer, bool bCover)
{
	const FName QueryName(bCover ? TEXT("FPEnemyCoverPositionQuery") : TEXT("FPEnemyAttackPositionQuery"));
	if (UEnvQuery* Existing = FindObjectFast<UEnvQuery>(&Outer, QueryName))
	{
		return Existing;
	}
	UEnvQuery* Query = NewObject<UEnvQuery>(&Outer, QueryName, RF_Transient);
	UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, TEXT("PositionsOption"), RF_Transient);
	UfpstrueEnvQueryGenerator_Positions* Generator =
		NewObject<UfpstrueEnvQueryGenerator_Positions>(Option, TEXT("PositionGenerator"), RF_Transient);
	Generator->bCoverQuery = bCover;
	Option->Generator = Generator;
	UfpstrueEnvQueryTest_Position* PositionTest =
		NewObject<UfpstrueEnvQueryTest_Position>(Option, TEXT("PositionValidation"), RF_Transient);
	PositionTest->bCoverQuery = bCover;
	PositionTest->TestOrder = 0;
	Option->Tests.Add(PositionTest);

	if (bCover)
	{
		// 便宜的环境检查先淘汰候选，余下掩体再使用原生高成本 PathExist；不为每点计算完整路径分数。
		UEnvQueryTest_Pathfinding* PathTest =
			NewObject<UEnvQueryTest_Pathfinding>(Option, TEXT("CoverReachability"), RF_Transient);
		PathTest->TestMode = EEnvTestPathfinding::PathExist;
		PathTest->SetWorkOnFloatValues(false);
		PathTest->TestPurpose = EEnvTestPurpose::Filter;
		PathTest->FilterType = EEnvTestFilterType::Match;
		PathTest->BoolValue.DefaultValue = true;
		PathTest->PathFromContext.DefaultValue = true;
		PathTest->Context = UEnvQueryContext_Querier::StaticClass();
		PathTest->TestOrder = 1;
		Option->Tests.Add(PathTest);
	}
	Query->GetOptionsMutable().Add(Option);
	return Query;
}
}

UEnvQuery* UfpstrueEnemyPositionQueryTemplates::GetTemplate(bool bCover)
{
	TObjectPtr<UEnvQuery>& Template = bCover ? CoverTemplate : AttackTemplate;
	if (Template == nullptr) Template = CreateEnemyPositionQueryTemplate(*this, bCover);
	return Template;
}

UEnvQuery* FPCreateEnemyPositionQuery(UObject& Outer, bool bCover)
{
	if (UWorld* World = Outer.GetWorld())
	{
		// UE5.5 按模板 FullName 缓存实例，却按短名复制 LocalTemplate 到同一 Manager。
		// 每个请求 Actor 各造同名图会覆写旧缓存根，GC 后留下悬空 Generator；固定共享两图，不复制请求运行状态。
		auto* Templates = World->GetSubsystem<UfpstrueEnemyPositionQueryTemplates>();
		return Templates != nullptr ? Templates->GetTemplate(bCover) : nullptr;
	}
	return CreateEnemyPositionQueryTemplate(Outer, bCover);
}
