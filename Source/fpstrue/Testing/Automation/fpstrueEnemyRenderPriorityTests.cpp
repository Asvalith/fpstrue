// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Enemies/fpstrueEnemySignificance.h"
#include "Misc/AutomationTest.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderPriorityOrderTest,
	"fpstrue.Performance.Significance.RenderPriorityOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRenderPriorityOrderTest::RunTest(const FString& Parameters)
{
	// A/B 和 B/C 在旧 IsNearlyEqual 比较器中会走 ID，A/C 则会走评分，能够组成比较环。
	const FFPEnemyRenderPriorityKey A{0.050000000f, 1, false, false};
	const FFPEnemyRenderPriorityKey B{0.050000007f, 2, false, false};
	const FFPEnemyRenderPriorityKey C{0.050000014f, 3, false, false};
	const FFPEnemyRenderPriorityKey SameScoreLowerId{A.Score, 0, false, false};
	const FFPEnemyRenderPriorityKey Expanded{0.0f, 4, false, true};
	const FFPEnemyRenderPriorityKey Primary{0.0f, 5, true, true};
	const FFPEnemyRenderPriorityKey InvalidNaN{std::numeric_limits<float>::quiet_NaN(), 6, false, false};
	const FFPEnemyRenderPriorityKey InvalidInfinity{std::numeric_limits<float>::infinity(), 7, false, false};
	const TArray<FFPEnemyRenderPriorityKey> Keys{
		A, B, C, SameScoreLowerId, Expanded, Primary, InvalidNaN, InvalidInfinity};
	const FFPEnemyRenderPriorityLess Before;

	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("A key never sorts before itself (%d)"), Index), Before(Keys[Index], Keys[Index]));
	}
	for (int32 LeftIndex = 0; LeftIndex < Keys.Num(); ++LeftIndex)
	{
		for (int32 MiddleIndex = 0; MiddleIndex < Keys.Num(); ++MiddleIndex)
		{
			for (int32 RightIndex = 0; RightIndex < Keys.Num(); ++RightIndex)
			{
				if (Before(Keys[LeftIndex], Keys[MiddleIndex]) && Before(Keys[MiddleIndex], Keys[RightIndex]))
				{
					TestTrue(
						FString::Printf(TEXT("The order is transitive (%d, %d, %d)"), LeftIndex, MiddleIndex, RightIndex),
						Before(Keys[LeftIndex], Keys[RightIndex]));
				}
			}
		}
	}

	TArray<FFPEnemyRenderPriorityKey> Sorted = Keys;
	Sorted.Sort(Before);
	const TArray<uint32> ExpectedIds{5, 4, 3, 2, 0, 1, 6, 7};
	TestEqual(TEXT("All priority keys remain in the sorted output"), Sorted.Num(), ExpectedIds.Num());
	for (int32 Index = 0; Index < Sorted.Num() && Index < ExpectedIds.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("Priority position %d"), Index), Sorted[Index].TieBreakId, ExpectedIds[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyRenderTopKTest,
	"fpstrue.Performance.Significance.RenderTopKSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFpstrueEnemyRenderTopKTest::RunTest(const FString& Parameters)
{
	// 输入故意打乱；Top-K 集合必须只由优先级键决定，不能依赖注册或遍历顺序。
	const TArray<FFPEnemyRenderPriorityKey> Keys{
		{0.4f, 40, false, false},
		{0.1f, 10, true, true},
		{0.9f, 90, false, false},
		{0.2f, 20, false, true},
		{0.8f, 80, false, false},
		{0.3f, 30, true, true},
	};

	TArray<FFPEnemyRenderPriorityKey> FullySorted = Keys;
	FullySorted.Sort(FFPEnemyRenderPriorityLess{});
	const auto SelectIds = [&Keys](int32 Budget, bool bReverseInput)
	{
		TArray<FFPEnemyRenderTopKEntry, TInlineAllocator<8>> Heap;
		for (int32 Step = 0; Step < Keys.Num(); ++Step)
		{
			const int32 Index = bReverseInput ? Keys.Num() - 1 - Step : Step;
			FPEnemyRenderTopKInsert(Heap, Budget, Index, Keys[Index]);
		}

		TArray<uint32> SelectedIds;
		for (const FFPEnemyRenderTopKEntry& Entry : Heap)
		{
			SelectedIds.Add(Entry.PriorityKey.TieBreakId);
		}
		SelectedIds.Sort();
		return SelectedIds;
	};

	for (int32 Budget = 0; Budget <= Keys.Num() + 1; ++Budget)
	{
		TArray<uint32> ExpectedIds;
		for (int32 Index = 0; Index < FMath::Min(Budget, FullySorted.Num()); ++Index)
		{
			ExpectedIds.Add(FullySorted[Index].TieBreakId);
		}
		ExpectedIds.Sort();

		for (const bool bReverseInput : {false, true})
		{
			const TArray<uint32> SelectedIds = SelectIds(Budget, bReverseInput);
			TestEqual(FString::Printf(TEXT("Budget %d selects the expected count"), Budget), SelectedIds.Num(), ExpectedIds.Num());
			for (int32 Index = 0; Index < SelectedIds.Num() && Index < ExpectedIds.Num(); ++Index)
			{
				TestEqual(FString::Printf(TEXT("Budget %d direction %d selected ID %d"), Budget, bReverseInput ? 1 : 0, Index),
					SelectedIds[Index], ExpectedIds[Index]);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
