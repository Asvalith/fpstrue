// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrue.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Components/SkeletalMeshComponent.h"
#include "Modules/ModuleManager.h"

// 注册项目主模块；Gameplay 类由 UE 反射和关卡配置实例化，这里不保存运行时状态。
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, fpstrue, "fpstrue");

// Copyright Epic Games, Inc. All Rights Reserved.

// 绑定调用者指定的有效播放；重复绑定只能命中原身份，不能覆盖另一轮动作。
FAnimMontageInstance* FFPActionPlayback::TryBind(uint32 InActionId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* PlayedMontage)
{
	// 先确认资源和当前活动实例，再决定复用已有绑定还是首次记录身份。
	if (InActionId == 0 || !IsValid(PlaybackMesh) || !IsValid(PlayedMontage)) return nullptr;
	UAnimInstance* Anim = PlaybackMesh->GetAnimInstance();
	FAnimMontageInstance* Instance = Anim != nullptr ? Anim->GetActiveInstanceForMontage(PlayedMontage) : nullptr;
	if (Instance == nullptr || !Instance->IsActive()) return nullptr;
	if (MontageInstanceId != INDEX_NONE)
	{
		return ActionId == InActionId && Mesh.Get() == PlaybackMesh && AnimInstance.Get() == Anim
			&& Montage.Get() == PlayedMontage && MontageInstanceId == Instance->GetInstanceID() ? Instance : nullptr;
	}
	// 首次绑定保存弱引用与实例编号，不延长角色、动画实例或资源的生命周期。
	Mesh = PlaybackMesh;
	AnimInstance = Anim;
	Montage = PlayedMontage;
	MontageInstanceId = Instance->GetInstanceID();
	ActionId = InActionId;
	return Instance;
}

// 将动画通知的来源与本次绑定比较，拒绝旧动作、换过 AnimInstance 或不同 Mesh 的通知。
bool FFPActionPlayback::Matches(uint32 ExpectedActionId, USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const
{
	// ★【重要边界】[异步身份] 动作编号与具体播放实例共同匹配；只认 Montage 资产会混入同资源的旧通知。
	const auto* Context = EventReference.GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>();
	return IsSet() && ActionId == ExpectedActionId && Mesh.Get() == PlaybackMesh
		&& AnimInstance.Get() == PlaybackMesh->GetAnimInstance()
		&& Context != nullptr && MontageInstanceId == Context->MontageInstanceID;
}

// 按保存的实例编号重新解析播放；资产相同但实例已替换时也返回空指针。
FAnimMontageInstance* FFPActionPlayback::GetBoundInstance() const
{
	UAnimInstance* Anim = AnimInstance.Get();
	FAnimMontageInstance* Instance = Anim != nullptr ? Anim->GetMontageInstanceForID(MontageInstanceId) : nullptr;
	return Instance != nullptr && Instance->Montage == Montage.Get() ? Instance : nullptr;
}

// 仅停止本次绑定的活动实例；混出时间限制为有限非负值，停止可能同步触发结束委托。
void FFPActionPlayback::Stop(float BlendOutSeconds) const
{
	if (FAnimMontageInstance* Instance = GetBoundInstance(); Instance != nullptr && Instance->IsActive())
	{
		Instance->Stop(FAlphaBlend(FMath::IsFinite(BlendOutSeconds) ? FMath::Max(0.0f, BlendOutSeconds) : 0.1f), true);
	}
}

// 判断身份字段和弱引用是否完整有效，不遍历动画列表，也不推断业务动作状态。
bool FFPActionPlayback::IsSet() const
{
	return ActionId != 0 && MontageInstanceId != INDEX_NONE && Mesh.IsValid() && AnimInstance.IsValid() && Montage.IsValid();
}
