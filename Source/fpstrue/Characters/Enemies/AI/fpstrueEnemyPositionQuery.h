// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Point.h"
#include "Subsystems/WorldSubsystem.h"
#include "fpstrueEnemyPositionQuery.generated.h"

class UEnvQuery;

// 每个 World 只有两份不可变参数化图；UPROPERTY 真正持有到 World 退出，不依赖请求 Actor 存活或无界全局强表。
UCLASS()
class UfpstrueEnemyPositionQueryTemplates : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	UEnvQuery* GetTemplate(bool bCover);
private:
	UPROPERTY(Transient) TObjectPtr<UEnvQuery> AttackTemplate;
	UPROPERTY(Transient) TObjectPtr<UEnvQuery> CoverTemplate;
};

/** EQS 原始数据中的一个候选；排序只移动 Item，不改变这里保存的槽位身份。 */
struct FPSTRUE_API FFPEnemyQueryPoint
{
	FNavLocation Nav;
	int32 SlotIndex = INDEX_NONE;
	// 0：包围布局站位；1：仅内圈槽位的攻击接近点；2：撤退掩体。
	int32 Kind = 0;
	// 攻击接近点保存相对扇区的角度偏移；测试按绝对值惩罚偏离原槽位的候选。
	float Preference = 0.0f;
};

/** 扩展原生 Point 的尾部数据；导航更新必须保留 SlotIndex、Kind 和 Preference。 */
UCLASS()
class FPSTRUE_API UfpstrueEnvQueryItemType_Position : public UEnvQueryItemType_Point
{
	GENERATED_BODY()

public:
	typedef FFPEnemyQueryPoint FValueType;

	UfpstrueEnvQueryItemType_Position(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	static FFPEnemyQueryPoint GetValue(const uint8* RawData);
	static void SetValue(uint8* RawData, const FFPEnemyQueryPoint& Value);
	virtual FVector GetItemLocation(const uint8* RawData) const override;
	virtual FNavLocation GetItemNavLocation(const uint8* RawData) const override;
	virtual void SetItemNavLocation(uint8* RawData, const FNavLocation& Value) const override;
};

/**
 * 有界双环候选生成器，不遍历宽网格，也不在生成阶段同步找路径。
 * 请求使用 float NamedParams：OriginX/Y/Z、ThreatX/Y/Z、Cover(1/0)、
 * InnerCount、OuterCount、InnerRadius、OuterRadius、InnerApproach。
 * 内圈生成布局点和三个攻击接近候选；外圈只生成实际消费的布局站位，不做废弃的接近点查询。
 * 攻击 Origin 是玩家位置快照，掩体 Origin 是敌人位置快照；Threat 始终是已知威胁快照。
 */
UCLASS()
class FPSTRUE_API UfpstrueEnvQueryGenerator_Positions : public UEnvQueryGenerator
{
	GENERATED_BODY()

public:
	UfpstrueEnvQueryGenerator_Positions(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual void GenerateItems(FEnvQueryInstance& QueryInstance) const override;

	// 仅作未传 Cover 参数时的模板默认值；每轮位置和配置都从 QueryInstance 读取。
	UPROPERTY()
	bool bCoverQuery = false;
};

/** 导航投影、站立空间和环境遮挡测试；ItemIterator 负责在候选之间遵守 EQS 时间预算。 */
UCLASS()
class FPSTRUE_API UfpstrueEnvQueryTest_Position : public UEnvQueryTest
{
	GENERATED_BODY()

public:
	UfpstrueEnvQueryTest_Position(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override;

	UPROPERTY()
	bool bCoverQuery = false;
};

/**
 * 每个 Outer 按 Attack/Cover 固定对象名只创建一次模板。
 * 调用者须用 UPROPERTY 保存返回值，并通过 FEnvQueryRequest::Execute 提交异步请求。
 * Cover 的 request owner 必须是真实敌人 Pawn，原生 PathExist 才以它为寻路起点。
 * 正式 World 中的模板 Outer 统一为 World 子系统：避免不同 Manager 的同名模板在引擎 EQS 缓存内复制覆盖。
 * 请求 Owner、参数、结果与取消身份仍完全独立；纯对象测试没有 World 时保留原来的 Outer 归属。
 */
FPSTRUE_API UEnvQuery* FPCreateEnemyPositionQuery(UObject& Outer, bool bCover);
