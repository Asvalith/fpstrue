// Copyright Epic Games, Inc. All Rights Reserved.
#include "Characters/Player/fpstrueCharacterMovementComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Runtime/fpstrueRuntime.h"
#include "Engine/World.h"

namespace
{
	// 预测数据只改变 SavedMove 的分配类型，仍复用 UE 的丢包恢复、ACK、时间戳和位置校正协议。
	class FNetworkPredictionData_Client_fpstrue final : public FNetworkPredictionData_Client_Character
	{
	public:
		// 保留 CMC 原生预测缓存初始化，只替换本项目移动记录的分配类型。
		explicit FNetworkPredictionData_Client_fpstrue(const UCharacterMovementComponent& Movement)
			: FNetworkPredictionData_Client_Character(Movement) {}
		// 每条历史移动创建带两个动作模式位的记录，仍交给原生合并/ACK/重放流程。
		virtual FSavedMovePtr AllocateNewMove() override { return MakeShared<FSavedMove_fpstrueCharacter>(); }
	};
}

void UfpstrueCharacterMovementComponent::ConfigureServerTimelineSmoothing()
{
	NetworkSmoothingMode = ENetworkSmoothingMode::Linear;
	bNetworkAlwaysReplicateTransformUpdateTimestamp = true;
}

void UfpstrueCharacterMovementComponent::SetServerSnapshotTime(double ServerTime)
{
	if (CharacterOwner != nullptr && CharacterOwner->HasAuthority() && FMath::IsFinite(ServerTime) && ServerTime >= 0.0)
		ServerLastTransformUpdateTimeStamp = static_cast<float>(ServerTime);
}

bool UfpstrueCharacterMovementComponent::GetRenderedServerTime(double& OutTime) const
{
	OutTime = 0.0;
	if (!UsesServerSnapshotMovement()) return false;
	const double PublishedTime = CharacterOwner->GetReplicatedServerLastTransformUpdateTimeStamp();
	if (!FMath::IsFinite(PublishedTime) || PublishedTime <= 0.0) return false;
	if (bNetworkSmoothingComplete) { OutTime = PublishedTime; return true; }
	const auto* Data = GetPredictionData_Client_Character();
	if (Data == nullptr || !FMath::IsFinite(Data->SmoothingServerTimeStamp) || Data->SmoothingServerTimeStamp <= 0.0
		|| !FMath::IsFinite(Data->SmoothingClientTimeStamp) || Data->SmoothingClientTimeStamp <= 0.0) return false;
	// Linear 可能小幅外推；命中历史只采用已经由服务器发布过的姿态时刻。
	OutTime = FMath::Min(Data->SmoothingClientTimeStamp, Data->SmoothingServerTimeStamp);
	return true;
}

bool UfpstrueCharacterMovementComponent::UsesServerSnapshotMovement() const
{
	return CharacterOwner != nullptr && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy
		&& GetNetMode() == NM_Client && NetworkSmoothingMode == ENetworkSmoothingMode::Linear
		&& !CharacterOwner->IsPlayingRootMotion() && !HasRootMotionSources()
		&& !MovementBaseUtility::IsDynamicBase(CharacterOwner->GetMovementBase());
}

void UfpstrueCharacterMovementComponent::MoveSmooth(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	if (UsesServerSnapshotMovement()) return;
	Super::MoveSmooth(InVelocity, DeltaSeconds, OutStepDownResult);
}

void FSavedMove_fpstrueCharacter::Clear()
{
	Super::Clear();
	bSavedAiming = bSavedSprinting = false;
}

uint8 FSavedMove_fpstrueCharacter::GetCompressedFlags() const
{
	return Super::GetCompressedFlags() | (bSavedAiming ? FLAG_Custom_0 : 0) | (bSavedSprinting ? FLAG_Custom_1 : 0);
}

bool FSavedMove_fpstrueCharacter::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* Character, float MaxDelta) const
{
	const auto* Other = static_cast<const FSavedMove_fpstrueCharacter*>(NewMove.Get());
	return Other != nullptr && bSavedAiming == Other->bSavedAiming && bSavedSprinting == Other->bSavedSprinting
		&& Super::CanCombineWith(NewMove, Character, MaxDelta);
}

void FSavedMove_fpstrueCharacter::SetMoveFor(ACharacter* Character, float InDeltaTime, const FVector& Accel,
	FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(Character, InDeltaTime, Accel, ClientData);
	if (const auto* Movement = Cast<UfpstrueCharacterMovementComponent>(Character->GetCharacterMovement()))
	{
		bSavedAiming = Movement->IsAimMovementRequested();
		bSavedSprinting = Movement->IsSprintMovementRequested();
	}
}

void FSavedMove_fpstrueCharacter::PrepMoveFor(ACharacter* Character)
{
	Super::PrepMoveFor(Character);
	if (auto* Movement = Cast<UfpstrueCharacterMovementComponent>(Character->GetCharacterMovement()))
	{
		// ★【重放边界】不重新激活 GA，不刷新 UI，只恢复这段历史移动实际使用的模式。
		Movement->bMoveAiming = bSavedAiming;
		Movement->bMoveSprinting = bSavedSprinting;
	}
}

float UfpstrueCharacterMovementComponent::GetMaxSpeed() const
{
	const auto* Character = Cast<AfpstrueCharacter>(CharacterOwner);
	if (Character == nullptr || !(IsMovingOnGround() || IsFalling()) || IsCrouching()) return Super::GetMaxSpeed();
	return bMoveAiming ? Character->AimWalkSpeed : (bMoveSprinting ? Character->SprintSpeed : Character->WalkSpeed);
}

FNetworkPredictionData_Client* UfpstrueCharacterMovementComponent::GetPredictionData_Client() const
{
	if (ClientPredictionData == nullptr)
		const_cast<UfpstrueCharacterMovementComponent*>(this)->ClientPredictionData = new FNetworkPredictionData_Client_fpstrue(*this);
	return ClientPredictionData;
}

void UfpstrueCharacterMovementComponent::SetMovementIntent(bool bAiming, bool bSprinting)
{
	const uint8 NewMode = bAiming ? 1 : (bSprinting ? 2 : 0);
	if (CharacterOwner != nullptr && CharacterOwner->HasAuthority() && NewMode != GrantedMode && GrantedMode != 0)
	{
		// 能力 RPC 与移动包不同时到达：只给刚结束的已授权模式一个有界旧 Move 区间，不接受任意客户端速度。
		const auto* ServerData = GetPredictionData_Server_Character();
		FRecentMovementGrant& Grant = RecentGrants[NextGrant++ % UE_ARRAY_COUNT(RecentGrants)];
		Grant.Mode = GrantedMode;
		Grant.LastAllowedClientTime = ServerData->CurrentClientTimeStamp + FMath::Min(ServerData->MaxMoveDeltaTime, 0.125f);
		Grant.ExpiresAt = GetWorld()->GetTimeSeconds() + 0.25;
	}
	GrantedMode = NewMode;
	bMoveAiming = bAiming;
	bMoveSprinting = !bAiming && bSprinting;
}

void UfpstrueCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);
	bool bAim = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
	bool bSprint = (Flags & FSavedMove_Character::FLAG_Custom_1) != 0;
	const auto* Character = Cast<AfpstrueCharacter>(CharacterOwner);
	if (Character != nullptr && Character->HasAuthority())
	{
		const auto* ASC = Character->GetHealthComponent();
		const uint8 RequestedMode = bAim ? 1 : (bSprint ? 2 : 0);
		const bool bHardBlocked = !Character->CanAcceptGameplayInput() || ASC == nullptr
			|| ASC->HasMatchingGameplayTag(FpstrueGameplayTags::State_Reloading)
			|| (bAim && !Character->HasEquippedWeapon());
		bool bAuthorized = RequestedMode == 0 || RequestedMode == GrantedMode;
		const FCharacterNetworkMoveData* Move = GetCurrentNetworkMoveData();
		if (!bAuthorized && !bHardBlocked && Move != nullptr)
		{
			const double Now = GetWorld()->GetTimeSeconds();
			for (const FRecentMovementGrant& Grant : RecentGrants)
			{
				const float AheadOfEnd = Move->TimeStamp - Grant.LastAllowedClientTime;
				if (Grant.Mode == RequestedMode && Now <= Grant.ExpiresAt && AheadOfEnd <= 0.0f && AheadOfEnd >= -0.25f)
				{
					bAuthorized = true;
					break;
				}
			}
		}
		// GAS 尚未授权的意图保守拒绝，由 UE 校正；这里绝不补激活能力或修改权威标签。
		if (bHardBlocked || !bAuthorized || (bAim && bSprint)) bAim = bSprint = false;
		// 不能靠清掉客户端 Aim 位绕开当前服务器的瞄准减速；合法的旧 Sprint 已受上面的历史时间界限约束。
		if (!bHardBlocked && GrantedMode == 1 && !bSprint) bAim = true;
	}
	bMoveAiming = bAim;
	bMoveSprinting = !bAim && bSprint;
}

bool UfpstrueCharacterMovementComponent::ClientUpdatePositionAfterServerUpdate()
{
	const auto* Prediction = static_cast<const FNetworkPredictionData_Client_Character*>(ClientPredictionData);
	if (Prediction != nullptr && Prediction->bUpdatePosition) ++NetworkCorrectionCount;
	const bool bReplayed = Super::ClientUpdatePositionAfterServerUpdate();
	// 最后一个旧 Move 不能覆盖现在的真实模式，尤其是已被服务器拒绝或因换弹取消的冲刺。
	// 重放中的碰撞/落地回调也可能改变能力；读取最新派生状态，而不是覆盖回调已经提交的新状态。
	const auto* Character = Cast<AfpstrueCharacter>(CharacterOwner);
	bMoveAiming = IsValid(Character) && Character->bIsAiming;
	bMoveSprinting = IsValid(Character) && !bMoveAiming && Character->bIsSprinting;
	return bReplayed;
}
