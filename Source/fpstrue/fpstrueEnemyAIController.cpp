// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyAIController.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueSurroundManager.h"
#include "AITypes.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"

AfpstrueEnemyAIController::AfpstrueEnemyAIController()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AfpstrueEnemyAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	ControlledEnemy = Cast<AfpstrueEnemyCharacter>(InPawn);
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return;
	}

	if (ControlledEnemy->IsDead())
	{
		SetAIState(EFPEnemyAIState::Dead);
		StopAI();
		return;
	}

	SurroundManager = ResolveSurroundManager();
	if (SurroundManager != nullptr)
	{
		SurroundManager->RequestSurroundSlot(ControlledEnemy);
	}

	bHasMoveGoal = false;
	StartDecisionTimer();
	UpdateAI();
}

void AfpstrueEnemyAIController::OnUnPossess()
{
	ClearDecisionTimer();
	ReleaseSurroundResources(true);
	ControlledEnemy = nullptr;
	TargetCharacter = nullptr;
	SurroundManager = nullptr;
	bHasMoveGoal = false;

	Super::OnUnPossess();
}

void AfpstrueEnemyAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearDecisionTimer();
	ReleaseSurroundResources(true);
	Super::EndPlay(EndPlayReason);
}

void AfpstrueEnemyAIController::StopAI()
{
	StopMovement();
	ClearDecisionTimer();
	TargetCharacter = nullptr;
	bHasMoveGoal = false;
	ReleaseSurroundResources(true);

	if (ControlledEnemy != nullptr)
	{
		ControlledEnemy->SetTargetCharacter(nullptr);
		SetAIState(ControlledEnemy->IsDead() ? EFPEnemyAIState::Dead : EFPEnemyAIState::Idle);
	}
}

void AfpstrueEnemyAIController::StartDecisionTimer()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	World->GetTimerManager().SetTimer(
		DecisionTimerHandle,
		this,
		&AfpstrueEnemyAIController::UpdateAI,
		DecisionInterval,
		true
	);
}

void AfpstrueEnemyAIController::ClearDecisionTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DecisionTimerHandle);
	}
}

void AfpstrueEnemyAIController::UpdateAI()
{
	if (ControlledEnemy == nullptr)
	{
		StopAI();
		return;
	}

	if (ControlledEnemy->IsDead())
	{
		SetAIState(EFPEnemyAIState::Dead);
		StopAI();
		return;
	}

	if (SurroundManager == nullptr)
	{
		SurroundManager = ResolveSurroundManager();
		if (SurroundManager != nullptr)
		{
			SurroundManager->RequestSurroundSlot(ControlledEnemy);
		}
	}

	if (!IsTargetUsable(TargetCharacter))
	{
		TargetCharacter = ResolveTarget();
	}

	ControlledEnemy->SetTargetCharacter(TargetCharacter);

	if (!IsTargetUsable(TargetCharacter))
	{
		ReleaseSurroundResources(true);
		SetAIState(EFPEnemyAIState::Idle);
		StopMovement();
		bHasMoveGoal = false;
		return;
	}

	if (ControlledEnemy->IsAttacking())
	{
		bObservedAttackInProgress = true;
		SetAIState(EFPEnemyAIState::Attack);
		StopMovement();
		bHasMoveGoal = false;
		ControlledEnemy->FaceTarget();
		return;
	}

	if (bHasAttackToken && bObservedAttackInProgress)
	{
		ReleaseSurroundResources(false);
	}

	const float DistanceToTarget = ControlledEnemy->GetDistanceToTarget2D();
	if (DistanceToTarget > ControlledEnemy->GetChaseRange())
	{
		ReleaseSurroundResources(false);
		SetAIState(EFPEnemyAIState::Idle);
		StopMovement();
		bHasMoveGoal = false;
		return;
	}

	if (bHasAttackToken)
	{
		const UWorld* World = GetWorld();
		const bool bTokenTimedOut =
			World != nullptr
			&& World->GetTimeSeconds() - AttackTokenAcquiredTime >= AttackTokenTimeout;
		if (bTokenTimedOut)
		{
			ReleaseSurroundResources(false);
		}
		else if (ControlledEnemy->IsTargetInAttackRange())
		{
			SetAIState(EFPEnemyAIState::Attack);
			StopMovement();
			bHasMoveGoal = false;
			ControlledEnemy->FaceTarget();
			if (ControlledEnemy->TryAttackTarget())
			{
				bObservedAttackInProgress = true;
			}
			return;
		}
		else
		{
			FVector AttackGoal;
			if (SurroundManager != nullptr
				&& SurroundManager->GetAttackApproachLocation(ControlledEnemy, AttackGoal))
			{
				SetAIState(EFPEnemyAIState::Chase);
				MoveToGoal(AttackGoal);
				return;
			}

			ReleaseSurroundResources(false);
		}
	}

	if (SurroundManager != nullptr
		&& SurroundManager->RequestSurroundSlot(ControlledEnemy))
	{
		FVector SlotGoal;
		bool bInnerRing = false;
		if (SurroundManager->GetAssignedSlotLocation(ControlledEnemy, SlotGoal, bInnerRing))
		{
			const bool bAtSlot =
				FVector::DistSquared2D(ControlledEnemy->GetActorLocation(), SlotGoal)
				<= FMath::Square(SlotArrivalTolerance);

			if (bInnerRing && bAtSlot && SurroundManager->RequestAttackToken(ControlledEnemy))
			{
				bHasAttackToken = true;
				bObservedAttackInProgress = false;
				AttackTokenAcquiredTime = GetWorld()->GetTimeSeconds();

				FVector AttackGoal;
				if (SurroundManager->GetAttackApproachLocation(ControlledEnemy, AttackGoal))
				{
					SetAIState(EFPEnemyAIState::Chase);
					MoveToGoal(AttackGoal);
					return;
				}

				ReleaseSurroundResources(false);
			}

			SetAIState(EFPEnemyAIState::Chase);
			if (bAtSlot)
			{
				StopMovement();
				bHasMoveGoal = false;
				ControlledEnemy->FaceTarget();
			}
			else
			{
				MoveToGoal(SlotGoal);
			}
			return;
		}
	}

	SetAIState(EFPEnemyAIState::Chase);
	MoveToActor(TargetCharacter, ControlledEnemy->GetAttackRange());
}

void AfpstrueEnemyAIController::MoveToGoal(const FVector& GoalLocation)
{
	const bool bNeedsNewPath =
		!bHasMoveGoal
		|| FVector::DistSquared2D(GoalLocation, LastMoveGoal) >= FMath::Square(PathRefreshDistance)
		|| GetMoveStatus() == EPathFollowingStatus::Idle;

	if (bNeedsNewPath)
	{
		MoveToLocation(GoalLocation, MoveAcceptanceRadius, true, true, true, false, nullptr, true);
		LastMoveGoal = GoalLocation;
		bHasMoveGoal = true;
	}
}

void AfpstrueEnemyAIController::ReleaseSurroundResources(bool bReleaseSlot)
{
	if (SurroundManager != nullptr && ControlledEnemy != nullptr)
	{
		SurroundManager->ReleaseAttackToken(ControlledEnemy);
		if (bReleaseSlot)
		{
			SurroundManager->ReleaseSurroundSlot(ControlledEnemy);
		}
	}

	bHasAttackToken = false;
	bObservedAttackInProgress = false;
	AttackTokenAcquiredTime = 0.0f;
}

void AfpstrueEnemyAIController::SetAIState(EFPEnemyAIState NewState)
{
	AIState = NewState;
}

AfpstrueCharacter* AfpstrueEnemyAIController::ResolveTarget() const
{
	return Cast<AfpstrueCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
}

AfpstrueSurroundManager* AfpstrueEnemyAIController::ResolveSurroundManager() const
{
	return Cast<AfpstrueSurroundManager>(
		UGameplayStatics::GetActorOfClass(this, AfpstrueSurroundManager::StaticClass())
	);
}

bool AfpstrueEnemyAIController::IsTargetUsable(const AfpstrueCharacter* Target) const
{
	return Target != nullptr && !Target->IsDead();
}
