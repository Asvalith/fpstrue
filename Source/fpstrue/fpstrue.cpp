// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Misc/NetworkVersion.h"

// 射击线协议变更必须在引擎握手阶段拒绝旧客户端，不能让旧 RPC 布局进入新执行器。
// 只注册版本，Gameplay 类仍由 UE 反射和关卡配置实例化；不接管底层传输。
class FFpstrueGameModule final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		FNetworkVersion::SetGameNetworkProtocolVersion(7);
		FNetworkVersion::SetGameCompatibleNetworkProtocolVersion(7);
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FFpstrueGameModule, fpstrue, "fpstrue");
