// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Shared/fpstrueActionPlayback.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Components/SkeletalMeshComponent.h"

bool FFPActionPlayback::TryBind(uint32 InActionId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* PlayedMontage)
{
	if (InActionId == 0 || !IsValid(PlaybackMesh) || !IsValid(PlayedMontage)) return false;
	UAnimInstance* Anim = PlaybackMesh->GetAnimInstance();
	FAnimMontageInstance* Instance = Anim != nullptr ? Anim->GetActiveInstanceForMontage(PlayedMontage) : nullptr;
	if (Instance == nullptr || !Instance->IsActive()) return false;
	if (MontageInstanceId != INDEX_NONE)
	{
		return ActionId == InActionId && Mesh.Get() == PlaybackMesh && AnimInstance.Get() == Anim
			&& Montage.Get() == PlayedMontage && MontageInstanceId == Instance->GetInstanceID();
	}
	Mesh = PlaybackMesh;
	AnimInstance = Anim;
	Montage = PlayedMontage;
	MontageInstanceId = Instance->GetInstanceID();
	ActionId = InActionId;
	return true;
}

bool FFPActionPlayback::Matches(uint32 ExpectedActionId, USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const
{
	const auto* Context = EventReference.GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>();
	return IsSet() && ActionId == ExpectedActionId && Mesh.Get() == PlaybackMesh
		&& AnimInstance.Get() == PlaybackMesh->GetAnimInstance()
		&& Context != nullptr && MontageInstanceId == Context->MontageInstanceID;
}

FAnimMontageInstance* FFPActionPlayback::GetBoundInstance() const
{
	UAnimInstance* Anim = AnimInstance.Get();
	FAnimMontageInstance* Instance = Anim != nullptr ? Anim->GetMontageInstanceForID(MontageInstanceId) : nullptr;
	return Instance != nullptr && Instance->Montage == Montage.Get() ? Instance : nullptr;
}

void FFPActionPlayback::Stop(float BlendOutSeconds) const
{
	if (FAnimMontageInstance* Instance = GetBoundInstance(); Instance != nullptr && Instance->IsActive())
	{
		Instance->Stop(FAlphaBlend(FMath::IsFinite(BlendOutSeconds) ? FMath::Max(0.0f, BlendOutSeconds) : 0.1f), true);
	}
}

bool FFPActionPlayback::IsSet() const
{
	return ActionId != 0 && MontageInstanceId != INDEX_NONE && Mesh.IsValid() && AnimInstance.IsValid() && Montage.IsValid();
}
