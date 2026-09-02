// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrue.h"
#include "Modules/ModuleManager.h"

// 注册项目主模块；Gameplay 类由 UE 反射和关卡配置实例化，这里不保存运行时状态。
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, fpstrue, "fpstrue");
