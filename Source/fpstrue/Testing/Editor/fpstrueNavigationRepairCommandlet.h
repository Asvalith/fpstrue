// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "fpstrueNavigationRepairCommandlet.generated.h"

/**
 * 固定 Baked 地图的显式 Editor 工具：AuditNavigation 只审计，RepairNavigation 才进入重烘/保存流程。
 * 修复要求提供已有备份文件，再校验导航代理、真实物理障碍与完整路径；失败不保存地图。
 * 可选灯柱修复仅增补地图导航禁行体，不修改美术源 Mesh 或原有物理碰撞；不在游戏运行时自动修复。
 */
UCLASS()
class UfpstrueNavigationRepairCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	// 声明离线 Editor 工具身份，不启动客户端或服务器玩法。
	UfpstrueNavigationRepairCommandlet();
	// 分派只读审计或显式修复；成功返回 0，缺少修复参数/备份或任何验证失败返回 1。
	virtual int32 Main(const FString& Params) override;
};
