// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
class UWorld;

// 显式开发采集：只观察真实连接；容量场景仍经正常 CMC/GAS 输入，不向生产玩法挂组件。
void StartPvpNetworkDiagnostics(UWorld* World);
