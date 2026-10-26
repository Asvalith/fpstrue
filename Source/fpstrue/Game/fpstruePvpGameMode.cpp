// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpGameState.h"
#include "Game/fpstruePvpPlayerController.h"
#include "Game/fpstruePvpPlayerState.h"
#include "Game/fpstrueConnectionHelpers.h"
#include "UI/fpstruePvpHUD.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueHealthComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueWeaponActors.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/ConstructorHelpers.h"
#include "IPlatformCrypto.h"
#include "PlatformCryptoTypes.h"

// 唯一服务器规则入口：生成与重生、PvE 登记、恢复资格和对象保留；不接管客户端 UI 或连接重试。

using FpstrueConnection::GetResumeNetGuid;
using FpstrueConnection::IsResumeToken;

namespace
{
constexpr int32 MaxRejoinSlots = 16;
constexpr int32 MaxPendingHandshakes = 8;
constexpr int32 MaxPlayerSpawnAttempts = 4;

// 回收登记前先核对真实持有者；死亡掉落枪由自己的有界寿命回收，不因重生/重连提前消失。
void ReleaseNetworkPlayerWeapon(AActor* Weapon, AfpstrueCharacter* ExpectedPawn,
	APlayerController* ExpectedController = nullptr, bool bDestroyDropped = false)
{
	if (!IsValid(Weapon)) return;
	UfpstrueWeaponComponent* Component = Weapon->FindComponentByClass<UfpstrueWeaponComponent>();
	AfpstrueCharacter* Holder = Component != nullptr ? Component->GetOwningCharacter() : nullptr;
	if (Holder != nullptr && Holder != ExpectedPawn &&
		(ExpectedController == nullptr || Holder->GetController() != ExpectedController)) return;
	if (Component != nullptr && Component->IsDroppedOnDeath())
	{
		// ★【资源所有权】只释放匹配死 Pawn 的装备槽；终态卸下只清登记，不重置已经开始的掉落物理。
		if (IsValid(ExpectedPawn) && ExpectedPawn->IsDead() && ExpectedPawn->GetEquippedWeaponComponent() == Component)
			ExpectedPawn->ClearEquippedWeaponComponent(Component);
		if (!bDestroyDropped) return;
	}
	Weapon->Destroy();
}

// 使用平台安全随机源生成 256 位不透明票据；随机源失败时返回空串，不生成可预测替代品。
FString GenerateResumeToken()
{
	const TUniquePtr<FEncryptionContext> Crypto = IPlatformCrypto::Get().CreateContext();
	uint8 Bytes[32];
	if (!Crypto.IsValid() || Crypto->CreateRandomBytes(MakeArrayView(Bytes)) != EPlatformCryptoResult::Success) return FString();
	return BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes));
}
}

// ---- 装配、对局开始与退出 ----

// 使用项目真正的玩家蓝图路径，旧模板路径只是重定向资产。
AfpstruePvpGameMode::AfpstruePvpGameMode()
{
	PlayerControllerClass = AfpstruePvpPlayerController::StaticClass();
	PlayerStateClass = AfpstruePvpPlayerState::StaticClass();
	GameStateClass = AfpstruePvpGameState::StaticClass();
	HUDClass = AfpstruePvpHUD::StaticClass();
#if !UE_BUILD_SHIPPING
	bSmokeTest = FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest"));
#endif
	// 正式角色必须来自已配置的蓝图；资源缺失沿既有出生失败流程退出，不生成无素材后备角色。
	DefaultPawnClass = LoadClass<AfpstrueCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	// 仅引导到真实项目武器蓝图；网格、动作、反馈与握持参数由资产保存，不回退到空原生实体。
	DefaultWeaponClass = LoadClass<AfpstrueTemplateWeapon>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/Weapons/BP_TemplateWeapon.BP_TemplateWeapon_C"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	static ConstructorHelpers::FClassFinder<AfpstrueEnemyCharacter> EnemyBlueprint(TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP"));
	if (EnemyBlueprint.Succeeded()) EnemyClass = EnemyBlueprint.Class;
}

// 同场维护玩家对战与敌人 AI；单个玩家死亡不结束其他玩家仍在进行的对局。
void AfpstruePvpGameMode::BeginPlay()
{
	Super::BeginPlay();
	// 唯一玩法入口：本地预览和真实服务器均采用同一套拾取、PvE 与重生规则。
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeLiveReconnect")))
		if (UNetDriver* Driver = GetWorld()->GetNetDriver())
		{
			Driver->ConnectionTimeout = 6.0f;
			Driver->TimeoutMultiplierForUnoptimizedBuilds = 1.0f;
		}
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity")))
	{
		FParse::Value(FCommandLine::Get(), TEXT("PvpNetworkEnemies="), EnemyPopulation);
		EnemyPopulation = FMath::Clamp(EnemyPopulation, 0, 160);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline")))
	{
		// 测试入口单独放宽人口，不修改蓝图/Config默认值，也不另建简化敌人或生成器。
		PopulationLimit = 1024;
		FParse::Value(FCommandLine::Get(), TEXT("PvpBaselineEnemies="), EnemyPopulation);
	}
#endif
	// 地图拾取物已离线迁为真实武器蓝图；运行时不再扫描和替换旧素材 Actor。
	if (AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState)) State->SetMatchActive(true);
	TArray<AActor*> Points;
	UGameplayStatics::GetAllActorsWithTag(this, EnemySpawnTag, Points);
	for (AActor* Point : Points) if (IsValid(Point)) PveSpawnPoints.Add(Point);
	for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It) RegisterPveEnemy(*It);
	GetWorldTimerManager().SetTimer(PvePopulationTimer, this, &ThisClass::UpdatePvePopulation, FMath::Max(EnemySpawnInterval, 0.05f), true);
#if !UE_BUILD_SHIPPING
	if (bSmokeTest)
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_IDENTITY Dedicated=%d LocalPlayers=%d"),
			GetNetMode() == NM_DedicatedServer, GetGameInstance() != nullptr ? GetGameInstance()->GetNumLocalPlayers() : -1);
		GetWorldTimerManager().SetTimer(SmokeMovementTimer, this, &ThisClass::SampleSmokeMovement, 0.05f, true);
	}
#endif
}

// 退出标记先于Timer和装备清理，销毁回调不能再创建Pawn或累加成绩。
void AfpstruePvpGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPvp = true;
	StopPve();
	ClearRejoinSlots();
#if !UE_BUILD_SHIPPING
	GetWorldTimerManager().ClearTimer(SmokeMovementTimer);
	SmokeSpawnOrigins.Reset();
	SmokeMovedPlayers.Reset();
#endif
	if (AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState)) State->SetMatchActive(false);
	for (auto& Entry : PendingRespawns) GetWorldTimerManager().ClearTimer(Entry.Value);
	PendingRespawns.Reset();
	for (auto& Entry : SpawnRecoveries) GetWorldTimerManager().ClearTimer(Entry.Value.Timer);
	SpawnRecoveries.Reset();
	const auto EndingPickups = MoveTemp(SpawnPickups);
	SpawnPickups.Reset();
	for (const auto& Entry : EndingPickups)
		if (AActor* Pickup = Entry.Value.Get(); Pickup && !Pickup->GetOwner()) Pickup->Destroy();
	const TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AActor>> EndingWeapons = MoveTemp(PlayerWeapons);
	PlayerWeapons.Reset();
	for (const auto& Entry : EndingWeapons) if (AActor* Weapon = Entry.Value.Get()) Weapon->Destroy();
	Super::EndPlay(EndPlayReason);
}

// ---- 玩家出生、死亡与装备资源 ----

// 所有玩家出生统一保留碰撞检查；蓝图不能以AlwaysSpawn把角色挤进敌人或关卡碰撞。
APawn* AfpstruePvpGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	FActorSpawnParameters Spawn;
	Spawn.Instigator = GetInstigator();
	Spawn.ObjectFlags |= RF_Transient;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
	return GetWorld()->SpawnActor<APawn>(GetDefaultPawnClassForController(NewPlayer), SpawnTransform, Spawn);
}

// 失败只重试正常出生事务；观战不生成Pawn，基线保留原StartSpot，不能换锚点刷过固定场景门禁。
void AfpstruePvpGameMode::RestartPlayer(AController* NewPlayer)
{
	APlayerController* Player = Cast<APlayerController>(NewPlayer);
	if (!HasAuthority() || bEndingPvp || !IsValid(Player) || MustSpectate(Player)) return;
	if (const auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player); PvpPlayer != nullptr && PvpPlayer->bIntentionalLeave) return;
	if (Player->GetPawn() == nullptr)
	{
		FSpawnRecovery& Recovery = SpawnRecoveries.FindOrAdd(Player);
		if (Recovery.Attempts >= MaxPlayerSpawnAttempts)
		{
			FailedToRestartPlayer(Player);
			return;
		}
		GetWorldTimerManager().ClearTimer(Recovery.Timer);
		if (Recovery.Attempts > 0 && !FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline"))) Player->StartSpot.Reset();
		++Recovery.Attempts;
	}
	Super::RestartPlayer(NewPlayer);
}

// 出生失败采用有界 Timer 重试；耗尽次数则明确拒绝连接，不把缺少 Pawn 当作就绪。
void AfpstruePvpGameMode::FailedToRestartPlayer(AController* NewPlayer)
{
	Super::FailedToRestartPlayer(NewPlayer);
	APlayerController* Player = Cast<APlayerController>(NewPlayer);
	if (bEndingPvp || !HasAuthority() || !IsValid(Player) || Player->GetPawn() != nullptr || MustSpectate(Player)) return;
	FSpawnRecovery& Recovery = SpawnRecoveries.FindOrAdd(Player);
	Recovery.Attempts = FMath::Max(Recovery.Attempts, 1);
	if (GetWorldTimerManager().IsTimerActive(Recovery.Timer)) return;
	if (Recovery.Attempts >= MaxPlayerSpawnAttempts)
	{
		UE_LOG(LogTemp, Warning, TEXT("PVP_SPAWN_FAILED Player=%s Attempts=%d PawnReady=0"), *Player->GetName(), Recovery.Attempts);
		if (auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player); PvpPlayer != nullptr && PvpPlayer->bConnectionHandshakeComplete)
		{
			PvpPlayer->bIntentionalLeave = true;
			PvpPlayer->ClientConnectionRejected(TEXT("pawn_spawn_failed"));
		}
		return;
	}
	const TWeakObjectPtr<APlayerController> Key(Player);
	GetWorldTimerManager().SetTimer(Recovery.Timer, FTimerDelegate::CreateWeakLambda(this, [this, Key]()
	{
		if (Key.IsValid()) RestartPlayer(Key.Get());
	}), 0.5f, false);
}

// 引擎完成Possess、控制旋转和默认Pawn设置之后，再执行服务器装备事务。
void AfpstruePvpGameMode::FinishRestartPlayer(AController* NewPlayer, const FRotator& StartRotation)
{
	Super::FinishRestartPlayer(NewPlayer, StartRotation);
	APlayerController* Player = Cast<APlayerController>(NewPlayer);
	AfpstrueCharacter* Pawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	if (!IsValid(Pawn) || Pawn->IsDead() || Pawn->GetController() != Player) return;
	FSpawnRecovery Recovery;
	if (SpawnRecoveries.RemoveAndCopyValue(Player, Recovery)) GetWorldTimerManager().ClearTimer(Recovery.Timer);
	bool bNormalSpawn = true;
#if !UE_BUILD_SHIPPING
	bNormalSpawn = !bSmokeTest && !FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity"));
#endif
	if (bNormalSpawn && bProvideNearbySpawnPickup && !bEndingPvp && HasAuthority()) SpawnNearbyWeaponPickup(Cast<APlayerController>(NewPlayer));
#if !UE_BUILD_SHIPPING
	// 自动发枪仅是联网回归的显式前置；正常玩法必须经过场景拾取，不能修改出生装备规则。
	if ((bSmokeTest || FParse::Param(FCommandLine::Get(), TEXT("PvpNetworkCapacity"))) && !bEndingPvp && HasAuthority())
		EquipDefaultWeapon(Cast<APlayerController>(NewPlayer));
	const AfpstrueCharacter* Character = IsValid(NewPlayer) ? Cast<AfpstrueCharacter>(NewPlayer->GetPawn()) : nullptr;
	if (bSmokeTest && !bEndingPvp && HasAuthority() && IsValid(Character) && !Character->IsDead() && Character->HasEquippedWeapon())
	{
		SmokeSpawnOrigins.Add(TWeakObjectPtr<AfpstrueCharacter>(const_cast<AfpstrueCharacter*>(Character)), Character->GetActorLocation());
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Spawn player=%s pawn=%s weapon=%s"), *GetNameSafe(NewPlayer), *Character->GetName(), *GetNameSafe(Character->GetEquippedWeaponComponent()));
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE SERVER_CONTEXT PC=%s PS=%s PSRep=%d PawnPS=%s GS=%s Players=%d"),
			*GetNameSafe(NewPlayer), *GetNameSafe(NewPlayer->PlayerState), NewPlayer->PlayerState != nullptr && NewPlayer->PlayerState->GetIsReplicated(),
			*GetNameSafe(Character->GetPlayerState()), *GetNameSafe(GameState), GameState != nullptr ? GameState->PlayerArray.Num() : -1);
	}
#endif
	// 装备/补给的蓝图构造也是外部回调；即时死亡归正常重生事务，不能发布虚假的Pawn就绪。
	if (!IsValid(Pawn) || Pawn->IsDead() || Player->GetPawn() != Pawn) return;
	if (auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player))
	{
		if (FRejoinSlot* Slot = RejoinSlots.Find(PvpPlayer->AssignedRejoinToken))
		{
			Slot->Pawn = Pawn;
			Slot->Weapon = Pawn->HasEquippedWeapon() ? Pawn->GetEquippedWeaponComponent()->GetOwner() : nullptr;
			Slot->bDeathRecorded = false;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_PAWN_READY Player=%s Pawn=%s PawnReady=1"), *Player->GetName(), *Pawn->GetName());
}

// 死亡资格和待重生记录先提交，再计分；外部回调不能重复接纳同一次死亡。
void AfpstruePvpGameMode::HandlePlayerEliminated(AfpstrueCharacter* Victim, AController* Killer)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Victim) || !Victim->IsDead()) return;
	if (HandleRetainedPawnEliminated(Victim, Killer)) return;
	APlayerController* Player = Cast<APlayerController>(Victim->GetController());
	const TWeakObjectPtr<APlayerController> PlayerKey(Player);
	if (!IsValid(Player) || Player->GetPawn() != Victim || PendingRespawns.Contains(PlayerKey)) return;
	AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState);
	if (State == nullptr || !State->IsMatchActive()) return;
	FTimerHandle& RespawnTimer = PendingRespawns.Add(PlayerKey);
	AfpstruePvpPlayerState* VictimState = Player->GetPlayerState<AfpstruePvpPlayerState>();
#if !UE_BUILD_SHIPPING
	const int32 PreviousDeaths = VictimState != nullptr ? VictimState->GetDeaths() : -1;
#endif
	if (VictimState != nullptr) VictimState->AddDeath();
	AfpstruePvpPlayerState* KillerState = nullptr;
	if (IsValid(Killer) && Killer != Player && Killer->GetWorld() == GetWorld())
	{
		if (APlayerController* KillerPlayer = Cast<APlayerController>(Killer))
		{
			KillerState = KillerPlayer->GetPlayerState<AfpstruePvpPlayerState>();
			if (KillerState != nullptr)
			{
#if !UE_BUILD_SHIPPING
				const int32 PreviousKills = KillerState->GetKills();
#endif
				KillerState->AddKill();
#if !UE_BUILD_SHIPPING
				bSmokeSawPlayerKill |= KillerState->GetKills() == PreviousKills + 1;
#endif
			}
		}
	}
	State->AddElimination();
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")))
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY SERVER_DEATH Victim=%s Deaths=%d Killer=%s Eliminations=%d"),
			*Victim->GetName(), VictimState ? VictimState->GetDeaths() : -1, *GetNameSafe(Killer), State->GetEliminationCount());
	if (bSmokeTest && VictimState != nullptr && VictimState->GetDeaths() == PreviousDeaths + 1)
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Death victim=%s deaths=%d killer=%s kills=%d eliminations=%d"),
			*Player->GetName(), VictimState->GetDeaths(), *GetNameSafe(Killer), KillerState != nullptr ? KillerState->GetKills() : 0, State->GetEliminationCount());
#endif
	const TWeakObjectPtr<AfpstrueCharacter> EliminatedPawn(Victim);
	const float Delay = FMath::IsFinite(RespawnDelay) ? FMath::Max(RespawnDelay, 0.1f) : 3.0f;
	GetWorldTimerManager().SetTimer(RespawnTimer, FTimerDelegate::CreateWeakLambda(this, [this, PlayerKey, EliminatedPawn]()
	{
		RespawnPlayer(PlayerKey, EliminatedPawn);
	}), Delay, false);
}

// 弱引用和旧Pawn身份共同保护重生；已被外部换成新角色时只丢弃旧Timer。
void AfpstruePvpGameMode::RespawnPlayer(TWeakObjectPtr<APlayerController> PlayerKey, TWeakObjectPtr<AfpstrueCharacter> EliminatedPawn)
{
	FTimerHandle Timer;
	if (!PendingRespawns.RemoveAndCopyValue(PlayerKey, Timer)) return;
	GetWorldTimerManager().ClearTimer(Timer);
	APlayerController* Player = PlayerKey.Get();
	if (bEndingPvp || !HasAuthority() || !IsValid(Player)) return;
	APawn* OldPawn = Player->GetPawn();
	if (OldPawn != nullptr && OldPawn != EliminatedPawn.Get()) return;
	ReleasePlayerWeapon(Player);
	if (OldPawn != nullptr)
	{
		Player->UnPossess();
		OldPawn->Destroy();
	}
	RestartPlayer(Player);
#if !UE_BUILD_SHIPPING
	const AfpstrueCharacter* NewPawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")) && NewPawn && !NewPawn->IsDead())
		UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY SERVER_RESPAWN Pawn=%s Health=%.1f EmptyHands=%d"),
			*NewPawn->GetName(), NewPawn->GetCurrentHealth(), !NewPawn->HasEquippedWeapon());
	if (bSmokeTest && IsValid(NewPawn) && NewPawn != EliminatedPawn.Get() && !NewPawn->IsDead() && NewPawn->HasEquippedWeapon())
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Respawn player=%s pawn=%s"), *Player->GetName(), *NewPawn->GetName());
		const AfpstruePvpGameState* State = Cast<AfpstruePvpGameState>(GameState);
		if (!bSmokeFinalReported && bSmokeSawPlayerKill && State != nullptr && State->GetScoreboard().Num() >= 2)
		{
			bSmokeFinalReported = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=Final players=%d eliminations=%d"), State->GetScoreboard().Num(), State->GetEliminationCount());
		}
	}
#endif
}

// 找到近处同层可走到的地面，并检查中间没有墙；找不到就不生成，不把枪丢进深渊或隔墙另一侧。
void AfpstruePvpGameMode::SpawnNearbyWeaponPickup(APlayerController* Player)
{
	ReleaseUnclaimedSpawnPickup(Player);
	AfpstrueCharacter* Pawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	UWorld* World = GetWorld();
	if (!HasAuthority() || bEndingPvp || !IsValid(Pawn) || Pawn->IsDead() || Pawn->HasEquippedWeapon() || !DefaultWeaponClass || !World) return;
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!Navigation) return;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueSpawnPickup), false, Pawn);
	const FVector Forward = Pawn->GetActorForwardVector();
	const FVector Right = Pawn->GetActorRightVector();
	for (const FVector Offset : {Forward * 210.0, Right * 210.0, -Right * 210.0, -Forward * 210.0})
	{
		FNavLocation Floor;
		if (!Navigation->ProjectPointToNavigation(Pawn->GetActorLocation() + Offset, Floor, FVector(60, 60, 150))) continue;
		FHitResult Ground;
		if (!World->LineTraceSingleByChannel(Ground, Floor.Location + FVector(0, 0, 150), Floor.Location - FVector(0, 0, 100), ECC_Visibility, Query)
			|| Ground.ImpactNormal.Z < 0.7f || FMath::Abs(Ground.ImpactPoint.Z - (Pawn->GetActorLocation().Z - Pawn->GetSimpleCollisionHalfHeight())) > 100.0) continue;
		const FVector Location = Ground.ImpactPoint + FVector(0, 0, 45);
		FHitResult Wall;
		if (World->SweepSingleByChannel(Wall, Pawn->GetActorLocation(), Location, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(12.0f), Query)) continue;
		// 同伴胶囊通常忽略 Visibility，却会阻挡玩家；补给不能放在近处同伴背后让新玩家一直顶着人走。
		if (World->SweepSingleByObjectType(Wall, Pawn->GetActorLocation(), Location, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
			FCollisionShape::MakeSphere(Pawn->GetSimpleCollisionRadius()), Query)) continue;
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AActor* Pickup = World->SpawnActor<AActor>(DefaultWeaponClass, Location, FRotator(0, Pawn->GetActorRotation().Yaw, 0), Spawn))
		{
			Pickup->SetReplicates(true);
			SpawnPickups.Add(Player, Pickup);
			UE_LOG(LogTemp, Display, TEXT("PVP_SPAWN_PICKUP Player=%s Pickup=%s DistanceCm=%.1f Grounded=1 NeedsOverlap=1"),
				*Player->GetName(), *Pickup->GetName(), FVector::Dist2D(Pawn->GetActorLocation(), Location));
		}
		return;
	}
}

// 每个控制器至多登记一把尚未拾取的出生补给；重生/离场只删除无主补给，不删除别人已拾走或死亡掉落的枪。
void AfpstruePvpGameMode::ReleaseUnclaimedSpawnPickup(APlayerController* Player)
{
	TWeakObjectPtr<AActor> Previous;
	SpawnPickups.RemoveAndCopyValue(Player, Previous);
	if (AActor* Actor = Previous.Get())
		if (const UfpstrueWeaponComponent* Weapon = Actor->FindComponentByClass<UfpstrueWeaponComponent>();
			Weapon && !Actor->GetOwner() && !Weapon->GetOwningCharacter() && !Weapon->IsDroppedOnDeath()) Actor->Destroy();
}

// 默认武器是独立Actor，网络、碰撞和Owner先就绪，再完成蓝图构造及完整装备。
void AfpstruePvpGameMode::EquipDefaultWeapon(APlayerController* Player)
{
	AfpstrueCharacter* Character = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	if (!HasAuthority() || !IsValid(Character) || Character->IsDead() || Character->HasEquippedWeapon() || !DefaultWeaponClass) return;
	ReleasePlayerWeapon(Player);
	const FTransform Transform = Character->GetActorTransform();
	AActor* Weapon = GetWorld()->SpawnActorDeferred<AActor>(DefaultWeaponClass, Transform, Character, Character,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Weapon == nullptr) return;
	Weapon->SetReplicateMovement(true);
	Weapon->SetActorEnableCollision(false);
	UGameplayStatics::FinishSpawningActor(Weapon, Transform);
	if (!IsValid(Weapon)) return;
	Weapon->SetReplicates(true);
	UfpstrueWeaponComponent* Component = Weapon->FindComponentByClass<UfpstrueWeaponComponent>();
	// 旧蓝图模板可能序列化过关闭复制；只改原生构造默认值不足以覆盖已保存资产。
	if (Component != nullptr) Component->SetIsReplicated(true);
	if (Component == nullptr || !Component->AttachWeapon(Character) || !IsValid(Player) || Player->GetPawn() != Character || Character->IsDead())
	{
		UE_LOG(LogTemp, Warning, TEXT("PvP default weapon could not equip on %s."), *GetNameSafe(Character));
		Weapon->Destroy();
		return;
	}
	PlayerWeapons.Add(TWeakObjectPtr<APlayerController>(Player), TWeakObjectPtr<AActor>(Weapon));
	Weapon->ForceNetUpdate();
}

// 外部武器Actor不由Pawn销毁自动回收，所以由对局在换Pawn和离场时显式释放。
void AfpstruePvpGameMode::ReleasePlayerWeapon(APlayerController* Player)
{
	TWeakObjectPtr<AActor> RecordedWeapon;
	PlayerWeapons.RemoveAndCopyValue(TWeakObjectPtr<APlayerController>(Player), RecordedWeapon);
	AfpstrueCharacter* Pawn = IsValid(Player) ? Cast<AfpstrueCharacter>(Player->GetPawn()) : nullptr;
	UfpstrueWeaponComponent* Equipped = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	AActor* CurrentWeapon = IsValid(Equipped) && Equipped->GetOwningCharacter() == Pawn ? Equipped->GetOwner() : nullptr;
	// ★【资源所有权】场景拾取不经过默认发枪记录；以当前装备补齐清理，不能留下已脱离旧 Pawn 的枪。
	// 记录先移除，实际引用先捕获；销毁回调不能重新消费旧记录，也不能误删已交给另一位玩家的武器。
	AActor* RecordedActor = RecordedWeapon.Get();
	ReleaseNetworkPlayerWeapon(RecordedActor, Pawn, Player);
	if (CurrentWeapon != RecordedActor) ReleaseNetworkPlayerWeapon(CurrentWeapon, Pawn, Player);
}

// ---- PvE 人口与每目标围攻管理器 ----

// 恢复期 Pawn 仍在服务器世界中且可受伤；只有失效、死亡或过期退出后才从 AI 候选中移除。
bool AfpstruePvpGameMode::IsCombatParticipant(const AfpstrueCharacter* Target) const
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Target) || Target->GetWorld() != GetWorld() || Target->IsDead()) return false;
	const APlayerController* Player = Cast<APlayerController>(Target->GetController());
	return (IsValid(Player) && Player->GetPawn() == Target) || IsRetainedDisconnectedPawn(Target);
}

// 为有效战斗目标复用或创建一个围攻管理器；目标选择与单敌人决策不在此执行。
AfpstrueSurroundManager* AfpstruePvpGameMode::GetOrCreateSurroundManager(AfpstrueCharacter* Target)
{
	if (!IsCombatParticipant(Target)) return nullptr;
	const TWeakObjectPtr<AfpstrueCharacter> Key(Target);
	if (auto* Existing = TargetSurroundManagers.Find(Key); Existing != nullptr && Existing->IsValid()) return Existing->Get();
	AfpstrueSurroundManager* Manager = GetWorld()->SpawnActor<AfpstrueSurroundManager>();
	if (!IsValid(Manager) || !IsCombatParticipant(Target)) { if (IsValid(Manager)) Manager->Destroy(); return nullptr; }
	TargetSurroundManagers.Add(Key, Manager);
	Manager->SetTargetCharacter(Target);
	return Manager;
}

// 按权威敌人登记表统计仍有效且存活的对象，不把延迟销毁的尸体计入生成缺口。
int32 AfpstruePvpGameMode::GetAliveEnemyCount() const
{
	int32 Count = 0;
	for (const auto& Enemy : PveEnemies) if (Enemy.IsValid() && !Enemy->IsDead()) ++Count;
	return Count;
}

// 单个 Timer 维护整个敌人生成队列；没有有效玩家时不生成，人数增长也不会另开一套生成器。
void AfpstruePvpGameMode::UpdatePvePopulation()
{
	if (bEndingPvp || !HasAuthority()) return;
#if !UE_BUILD_SHIPPING
	TickLiveReconnectOutage();
#endif
	for (auto It = PveEnemies.CreateIterator(); It; ++It) if (!It->IsValid()) It.RemoveCurrent();
	for (auto It = TargetSurroundManagers.CreateIterator(); It; ++It)
	{
		if (!IsCombatParticipant(It.Key().Get()))
		{
			if (auto* Manager = It.Value().Get()) { Manager->ResetManager(); Manager->Destroy(); }
			It.RemoveCurrent();
		}
	}
#if !UE_BUILD_SHIPPING
	for (auto It = ObservedPvePlayers.CreateIterator(); It; ++It) if (!It->IsValid()) It.RemoveCurrent();
#endif
	AfpstrueCharacter* FirstPlayer = nullptr;
	int32 ReadyPlayers = 0;
	for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
	{
		if (!IsCombatParticipant(*It)) continue;
		if (FirstPlayer == nullptr) FirstPlayer = *It;
		const auto* Player = Cast<AfpstruePvpPlayerController>(It->GetController());
		if (Player != nullptr && Player->bConnectionHandshakeComplete) ++ReadyPlayers;
#if !UE_BUILD_SHIPPING
		const TWeakObjectPtr<AfpstrueCharacter> Key(*It);
		if (!ObservedPvePlayers.Contains(Key))
		{
			ObservedPvePlayers.Add(Key);
			if (auto* Health = It->FindComponentByClass<UfpstrueHealthComponent>())
				Health->OnDamageResolved.AddWeakLambda(this, [this, Key](float Damage, AActor* Causer, AController* DamageInstigator, const FFPDamageContext& Context)
				{ HandlePvePlayerDamaged(Key, Damage, Causer, DamageInstigator, Context); });
		}
#endif
	}
	if (FirstPlayer == nullptr) return;
	if (!bBaselinePopulationStarted && FParse::Param(FCommandLine::Get(), TEXT("PvpBaseline")))
	{
		if (ReadyPlayers != 2) return;
		bBaselinePopulationStarted = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_BASELINE ADMISSION Players=2 PawnReady=1 EnemyPopulation=%d"), EnemyPopulation);
	}
#if !UE_BUILD_SHIPPING
	if (!bNormalVisualRoutesSent && FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPairWalk"))) SendNormalVisualRoutes();
#endif
	AfpstrueCharacter* SmokeNearPlayer = nullptr;
#if !UE_BUILD_SHIPPING
	if (!PreparePveSmokePopulation(SmokeNearPlayer)) return;
#endif
	const int32 Desired =
#if !UE_BUILD_SHIPPING
		bSmokeTest ? 1 :
#endif
		FMath::Clamp(EnemyPopulation, 0, PopulationLimit);
	if (GetAliveEnemyCount() >= Desired || GetWorld()->GetTimeSeconds() < NextEnemySpawnTime) return;
	NextEnemySpawnTime = GetWorld()->GetTimeSeconds() + FMath::Max(EnemySpawnInterval, 0.05f);
	if (AfpstrueEnemyCharacter* Enemy = SpawnPveEnemy(SmokeNearPlayer))
	{
		RegisterPveEnemy(Enemy);
#if !UE_BUILD_SHIPPING
		if (bSmokeTest && !bSmokePveCompleted)
		{
			SmokePveEnemy = Enemy;
			UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PveSpawn Enemy=%s"), *Enemy->GetName());
		}
#endif
	}
}

// 导航投影和碰撞调整与正常生成共用；测试只把出生候选放近玩家，不直接伪造攻击或伤害。
AfpstrueEnemyCharacter* AfpstruePvpGameMode::SpawnPveEnemy(AfpstrueCharacter* NearPlayer)
{
	if (!HasAuthority() || EnemyClass == nullptr || bEndingPvp) return nullptr;
	UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (Navigation == nullptr || (NearPlayer == nullptr && PveSpawnPoints.IsEmpty())) return nullptr;
	const auto* Defaults = EnemyClass.GetDefaultObject();
	const float HalfHeight = Defaults != nullptr ? Defaults->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 96.0f;
	for (int32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		FVector Candidate;
		FRotator Rotation = FRotator::ZeroRotator;
		if (NearPlayer != nullptr)
		{
			const float Angle = FMath::DegreesToRadians(static_cast<float>(Attempt * 45));
			Candidate = NearPlayer->GetActorLocation() + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * 180.0f;
		}
		else
		{
			AActor* Point = PveSpawnPoints[NextPveSpawnIndex++ % PveSpawnPoints.Num()];
			if (!IsValid(Point)) continue;
			Candidate = Point->GetActorLocation();
			// 保留关卡作者设置的警戒方向；导航只调整位置，不能把全部敌人的视锥重置到世界 +X。
			Rotation.Yaw = Point->GetActorRotation().Yaw;
			if (Attempt > 0) Candidate += FVector(FMath::FRandRange(-400.0f, 400.0f), FMath::FRandRange(-400.0f, 400.0f), 0.0f);
		}
		FNavLocation Projected;
		if (!Navigation->ProjectPointToNavigation(Candidate, Projected, FVector(150.0f, 150.0f, 500.0f))) continue;
		const FVector Location = Projected.Location + FVector::UpVector * (HalfHeight + 2.0f);
		if (NearPlayer != nullptr) Rotation.Yaw = (NearPlayer->GetActorLocation() - Location).Rotation().Yaw;
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
		AfpstrueEnemyCharacter* Enemy = GetWorld()->SpawnActor<AfpstrueEnemyCharacter>(EnemyClass, Location, Rotation, Parameters);
		if (!IsValid(Enemy)) continue;
		if (Enemy->GetController() == nullptr) Enemy->SpawnDefaultController();
		if (!IsValid(Enemy)) continue;
		const auto* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController());
		// Spawn/Possess 可同步停止玩法；不能把旧调用栈生成的敌人重新登记进已结束的世界。
		if (!bEndingPvp && IsValid(Enemy) && !Enemy->IsDead() && Controller != nullptr && Controller->IsBehaviorReady()) return Enemy;
		if (AController* InvalidController = Enemy->GetController()) InvalidController->Destroy();
		Enemy->Destroy();
		if (bEndingPvp) return nullptr;
	}
	return nullptr;
}

// 只登记已启动行为树的存活敌人，并连接生命周期和开发验收观察；不替 AI 指定战斗目标。
void AfpstruePvpGameMode::RegisterPveEnemy(AfpstrueEnemyCharacter* Enemy)
{
	const auto* Controller = IsValid(Enemy) ? Cast<AfpstrueEnemyAIController>(Enemy->GetController()) : nullptr;
	if (bEndingPvp || !HasAuthority() || Controller == nullptr || !Controller->IsBehaviorReady() || Enemy->IsDead() || PveEnemies.Contains(Enemy)) return;
	PveEnemies.Add(Enemy);
	Enemy->OnEnemyDeathReported.AddUniqueDynamic(this, &ThisClass::HandlePveEnemyDied);
	Enemy->OnEndPlay.AddUniqueDynamic(this, &ThisClass::HandlePveEnemyEndPlay);
#if !UE_BUILD_SHIPPING
	if (auto* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>())
	{
		const TWeakObjectPtr<AfpstrueEnemyCharacter> WeakEnemy(Enemy);
		Health->OnDamageResolved.AddWeakLambda(this, [this, WeakEnemy](float Damage, AActor*, AController* DamageInstigator, const FFPDamageContext&)
		{
			if (bSmokeTest && WeakEnemy == SmokePveEnemy && Damage > 0.0f && IsValid(DamageInstigator) && DamageInstigator->GetPawn() == SmokePvePlayer)
				UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PvePlayerHit Enemy=%s Health=%.1f"), *GetNameSafe(WeakEnemy.Get()), WeakEnemy->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth());
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")) && WeakEnemy.IsValid() && Damage > 0.0f)
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_DAMAGE Enemy=%s Health=%.1f Instigator=%s Damage=%.1f"),
					*WeakEnemy->GetName(), WeakEnemy->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth(), *GetNameSafe(DamageInstigator), Damage);
		});
	}
#endif
}

// 死亡后延迟补充人口并限制尸体寿命；保留登记到 EndPlay，以便统一解除订阅。
void AfpstruePvpGameMode::HandlePveEnemyDied(AfpstrueEnemyCharacter* Enemy)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Enemy)) return;
	NextEnemySpawnTime = GetWorld()->GetTimeSeconds() + FMath::Max(EnemyRespawnDelay, 0.1f);
	if (Enemy->GetLifeSpan() <= 0.0f) Enemy->SetLifeSpan(15.0f); // 联网尸体必须有回收边界。
#if !UE_BUILD_SHIPPING
	if (bSmokeTest && Enemy == SmokePveEnemy)
	{
		bSmokePveCompleted = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_SMOKE PASS=PveEnemyDeath Enemy=%s"), *Enemy->GetName());
	}
#endif
}

// 敌人真正离场时移出登记并解除死亡、离场及伤害观察。
void AfpstruePvpGameMode::HandlePveEnemyEndPlay(AActor* Actor, EEndPlayReason::Type)
{
	AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(Actor);
	if (Enemy == nullptr) return;
	PveEnemies.Remove(Enemy);
	Enemy->OnEnemyDeathReported.RemoveDynamic(this, &ThisClass::HandlePveEnemyDied);
	Enemy->OnEndPlay.RemoveDynamic(this, &ThisClass::HandlePveEnemyEndPlay);
	if (auto* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>()) Health->OnDamageResolved.RemoveAll(this);
}

// 停止人口补充和 AI，解除伤害观察并归还每个目标的围攻资源；不在这里销毁全部敌人。
void AfpstruePvpGameMode::StopPve()
{
	GetWorldTimerManager().ClearTimer(PvePopulationTimer);
	const auto Enemies = PveEnemies;
	for (const auto& Weak : Enemies) if (auto* Enemy = Weak.Get())
	{
		if (auto* AI = Cast<AfpstrueEnemyAIController>(Enemy->GetController())) AI->StopAI();
		HandlePveEnemyEndPlay(Enemy, EEndPlayReason::RemovedFromWorld);
	}
	PveEnemies.Reset();
#if !UE_BUILD_SHIPPING
	for (const auto& Player : ObservedPvePlayers) if (Player.IsValid())
		if (auto* Health = Player->FindComponentByClass<UfpstrueHealthComponent>()) Health->OnDamageResolved.RemoveAll(this);
	ObservedPvePlayers.Reset();
#endif
	const auto Managers = MoveTemp(TargetSurroundManagers);
	for (const auto& Entry : Managers) if (auto* Manager = Entry.Value.Get()) { Manager->ResetManager(); Manager->Destroy(); }
}

// ---- 服务器握手、离线保留与到期回收 ----

// 在引擎预登录检查后限制本局连接和待握手容量；此时不接纳或验证恢复票据。
void AfpstruePvpGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (ErrorMessage.IsEmpty() && (bEndingPvp || PendingConnectionHandshakes.Num() >= MaxPendingHandshakes || GetNumPlayers() >= MaxRejoinSlots + MaxPendingHandshakes))
		ErrorMessage = TEXT("connection_capacity_reached");
}

// 先登记五秒握手期限，握手完成后才恢复或生成 Pawn；监听主机的本地玩家直接进入同一入口。
void AfpstruePvpGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	auto* Player = Cast<AfpstruePvpPlayerController>(NewPlayer);
	if (!IsValid(Player) || bEndingPvp) return;
	PendingConnectionHandshakes.Add(Player, GetWorld()->GetTimeSeconds() + 5.0);
	if (!GetWorldTimerManager().IsTimerActive(RejoinCleanupTimer))
		GetWorldTimerManager().SetTimer(RejoinCleanupTimer, this, &ThisClass::SweepRejoinSlots, 0.5f, true);
	if (Player->IsLocalController()) CompleteConnectionHandshake(Player, FString());
	else Player->ClientBeginConnectionHandshake();
}

// 【设计取舍】恢复服务器保留的 Pawn/PlayerState，不接纳客户端上传血量、弹药或成绩；离线仍可受伤，死亡则重生。
// 先消费旧票据、占用槽位并轮换票据，再 Possess/通知，防止并发或重入重复认领同一身份。
// 票据只证明会话持有权，不证明客户端状态可信；保留表和超时释放的资源成本由服务器承担。
void AfpstruePvpGameMode::CompleteConnectionHandshake(AfpstruePvpPlayerController* Player, const FString& Token)
{
	if (!HasAuthority() || bEndingPvp || !IsValid(Player) || Player->bConnectionHandshakeComplete) return;
	const double* Deadline = PendingConnectionHandshakes.Find(Player);
	const double Now = GetWorld()->GetTimeSeconds();
	if (Deadline == nullptr || *Deadline < Now) return;
	FRejoinSlot* Existing = Token.IsEmpty() ? nullptr : RejoinSlots.Find(Token);
	const bool bRestored = Existing != nullptr;
	FString Error;
	if (!Token.IsEmpty() && (!IsResumeToken(Token) || Existing == nullptr)) Error = TEXT("resume_not_available");
	else if (Existing != nullptr && (!Existing->bDisconnected || Existing->Controller.IsValid())) Error = TEXT("resume_slot_active");
	else if (Existing != nullptr && (Existing->ExpiresAt <= Now || !Existing->State.IsValid())) Error = TEXT("resume_expired");
	else if (Existing == nullptr && RejoinSlots.Num() >= MaxRejoinSlots) Error = TEXT("session_capacity_reached");
	const FString NewToken = Error.IsEmpty() ? GenerateResumeToken() : FString();
	if (Error.IsEmpty() && NewToken.IsEmpty()) Error = TEXT("secure_ticket_unavailable");
	if (!Error.IsEmpty())
	{
		// 只记录原因，不输出票据；区分正常过期、并发占用与真正的恢复链路错误。
		UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_REJECT Reason=%s"), *Error);
		PendingConnectionHandshakes.Remove(Player);
		Player->bIntentionalLeave = true;
		Player->ClientConnectionRejected(Error);
		if (GameSession != nullptr) GameSession->KickPlayer(Player, FText::FromString(TEXT("Connection rejected.")));
		else Player->Destroy();
		return;
	}
	// 先消费旧票据并占用槽位，再做 Possess/委托，重入不能把同一份保留状态交给第二个玩家。
	FRejoinSlot Slot;
	if (bRestored) RejoinSlots.RemoveAndCopyValue(Token, Slot);
	else Slot.State = Player->GetPlayerState<AfpstruePvpPlayerState>();
	Slot.Controller = Player;
	Slot.bDisconnected = false;
	Slot.ExpiresAt = 0.0;
	PendingConnectionHandshakes.Remove(Player);
	Player->bConnectionHandshakeComplete = true;
	Player->AssignedRejoinToken = NewToken;
	RejoinSlots.Add(NewToken, Slot);
	AfpstruePvpPlayerState* State = Slot.State.Get();
	if (bRestored && State != nullptr)
	{
		APlayerState* TemporaryState = Player->PlayerState;
		Player->SetPlayerState(State);
		State->SetOwner(Player);
		State->SetRetainedForReconnect(false);
		if (IsValid(TemporaryState) && TemporaryState != State) TemporaryState->Destroy();
	}
	AfpstrueCharacter* Pawn = Slot.Pawn.Get();
	UfpstrueWeaponComponent* RetainedWeapon = IsValid(Pawn) ? Pawn->GetEquippedWeaponComponent() : nullptr;
	const bool bValidEquipment = RetainedWeapon == nullptr || (IsValid(RetainedWeapon)
		&& RetainedWeapon->GetOwningCharacter() == Pawn && IsValid(RetainedWeapon->GetOwner()));
	if (bRestored && IsValid(Pawn) && !Pawn->IsDead() && bValidEquipment)
	{
		Player->Possess(Pawn);
		Pawn->GetCharacterMovement()->bRunPhysicsWithNoController = Slot.bPreviousPhysicsWithoutController;
		Pawn->GetAbilitySystemComponent()->InitAbilityActorInfo(Pawn, Pawn);
		// 空手不需要制造一把枪来完成恢复；拾取枪以 Pawn 的实际装备为准，不依赖默认发枪登记。
		if (RetainedWeapon != nullptr)
		{
			RetainedWeapon->ResetNetworkInputGeneration();
			AActor* WeaponActor = RetainedWeapon->GetOwner();
			WeaponActor->SetOwner(Pawn);
			PlayerWeapons.Add(Player, WeaponActor);
			WeaponActor->ForceNetUpdate();
		}
		Player->SetControlRotation(Slot.ViewRotation);
		Player->ClientSetRotation(Slot.ViewRotation, true);
		Pawn->ForceNetUpdate();
	}
	else
	{
		// 离线期间死亡或 Pawn/装备已被销毁，只恢复服务器当前成绩并按重生规则生成，不回滚到离线前满血。
		ReleaseNetworkPlayerWeapon(Slot.Weapon.Get(), Pawn);
		if (Pawn != nullptr) Pawn->Destroy();
		RestartPlayer(Player);
		Pawn = Cast<AfpstrueCharacter>(Player->GetPawn());
	}
	// 已耗尽的出生可能同步拒绝连接；同一个握手调用栈不能随后再发送Accepted。
	if (Player->bIntentionalLeave) return;
	if (FRejoinSlot* Current = RejoinSlots.Find(NewToken))
	{
		Current->Pawn = Pawn;
		Current->State = Player->GetPlayerState<AfpstruePvpPlayerState>();
		Current->Weapon = Pawn != nullptr && Pawn->GetEquippedWeaponComponent() != nullptr ? Pawn->GetEquippedWeaponComponent()->GetOwner() : nullptr;
		Current->bDeathRecorded = false;
	}
	State = Player->GetPlayerState<AfpstruePvpPlayerState>();
	const UfpstrueWeaponComponent* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	Player->ClientConnectionAccepted(NewToken, State != nullptr ? State->GetPlayerId() : INDEX_NONE, bRestored);
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_ACCEPT ResumeId=%d Restored=%d Pawn=%s PS=%s Health=%.1f Ammo=%d Kills=%d Deaths=%d Dead=%d PawnNetGuid=%llu StateNetGuid=%llu PawnReady=%d"),
		State != nullptr ? State->GetPlayerId() : INDEX_NONE, bRestored, *GetNameSafe(Pawn), *GetNameSafe(State), Pawn != nullptr ? Pawn->GetCurrentHealth() : 0.0f,
		Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0, State != nullptr ? State->GetKills() : 0, State != nullptr ? State->GetDeaths() : 0, Pawn == nullptr || Pawn->IsDead(),
		GetResumeNetGuid(Pawn), GetResumeNetGuid(State), IsValid(Pawn) && !Pawn->IsDead() && Pawn->GetController() == Player);
}

// 引擎 PawnLeavingGame 之前保留真实对象；没有 Controller 不代表无敌，生命和骨骼碰撞照常工作。
bool AfpstruePvpGameMode::PreserveDisconnectedPlayer(AfpstruePvpPlayerController* Player)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Player) || Player->bIntentionalLeave || !Player->bConnectionHandshakeComplete) return false;
	FRejoinSlot* Slot = RejoinSlots.Find(Player->AssignedRejoinToken);
	AfpstruePvpPlayerState* State = Player->GetPlayerState<AfpstruePvpPlayerState>();
	if (Slot == nullptr || Slot->Controller.Get() != Player || State == nullptr) return false;
	if (Slot->bDisconnected) return true;
	AfpstrueCharacter* Pawn = Cast<AfpstrueCharacter>(Player->GetPawn());
	Slot->bDisconnected = true;
	Slot->ExpiresAt = GetWorld()->GetTimeSeconds() + FMath::Clamp(ReconnectGraceSeconds, 5.0f, 120.0f);
	Slot->Pawn = Pawn;
	Slot->State = State;
	Slot->bDeathRecorded = Pawn != nullptr && Pawn->IsDead();
	Slot->ViewRotation = Player->GetControlRotation();
	TWeakObjectPtr<AActor> RecordedWeapon;
	PlayerWeapons.RemoveAndCopyValue(Player, RecordedWeapon);
	UfpstrueWeaponComponent* Equipped = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	Slot->Weapon = IsValid(Equipped) && Equipped->GetOwningCharacter() == Pawn ? Equipped->GetOwner() : nullptr;
	// 装备可能来自场景拾取而不是默认发枪；只保留当前枪，失效旧记录不能覆盖真实装备。
	if (AActor* PreviousWeapon = RecordedWeapon.Get(); PreviousWeapon != nullptr && PreviousWeapon != Slot->Weapon.Get())
	{
		const auto* PreviousComponent = PreviousWeapon->FindComponentByClass<UfpstrueWeaponComponent>();
		if (PreviousComponent == nullptr || PreviousComponent->GetOwningCharacter() == nullptr)
			ReleaseNetworkPlayerWeapon(PreviousWeapon, Pawn, Player);
	}
	FTimerHandle Timer;
	if (PendingRespawns.RemoveAndCopyValue(Player, Timer)) GetWorldTimerManager().ClearTimer(Timer);
	State->SetRetainedForReconnect(true);
	State->SetOwner(nullptr);
	if (Pawn != nullptr)
	{
		if (UAbilitySystemComponent* ASC = Pawn->GetAbilitySystemComponent()) ASC->CancelAllAbilities();
		if (auto* Weapon = Pawn->GetEquippedWeaponComponent()) Weapon->InterruptOwnerInput();
		Slot->bPreviousPhysicsWithoutController = Pawn->GetCharacterMovement()->bRunPhysicsWithNoController;
		Pawn->GetCharacterMovement()->StopMovementImmediately();
		Pawn->GetCharacterMovement()->bRunPhysicsWithNoController = true;
		Player->UnPossess();
		Pawn->SetPlayerState(State);
		Pawn->ForceNetUpdate();
	}
	const UfpstrueWeaponComponent* Weapon = Pawn != nullptr ? Pawn->GetEquippedWeaponComponent() : nullptr;
	UE_LOG(LogTemp, Display, TEXT("PVP_RESUME SERVER_RETAIN ResumeId=%d Pawn=%s PS=%s Health=%.1f Ammo=%d Kills=%d Deaths=%d Dead=%d PawnNetGuid=%llu StateNetGuid=%llu"),
		State->GetPlayerId(), *GetNameSafe(Pawn), *GetNameSafe(State), Pawn != nullptr ? Pawn->GetCurrentHealth() : 0.0f,
		Weapon != nullptr ? Weapon->GetCurrentAmmo() : 0, State->GetKills(), State->GetDeaths(), Pawn == nullptr || Pawn->IsDead(),
		GetResumeNetGuid(Pawn), GetResumeNetGuid(State));
	return true;
}

// 判断 Pawn 是否仍属于未过期的离线保留槽，供 AI 候选资格与离线死亡结算复用。
bool AfpstruePvpGameMode::IsRetainedDisconnectedPawn(const AfpstrueCharacter* Pawn) const
{
	if (Pawn == nullptr) return false;
	for (const auto& Entry : RejoinSlots)
		if (Entry.Value.bDisconnected && Entry.Value.ExpiresAt > GetWorld()->GetTimeSeconds() && Entry.Value.Pawn.Get() == Pawn) return true;
	return false;
}

// 为没有 Controller 的保留 Pawn 结算一次死亡；返回 true 表示已由离线槽接管，无需普通重生路径。
bool AfpstruePvpGameMode::HandleRetainedPawnEliminated(AfpstrueCharacter* Pawn, AController* Killer)
{
	if (bEndingPvp || !HasAuthority() || !IsValid(Pawn)) return false;
	for (auto& Entry : RejoinSlots)
	{
		FRejoinSlot& Slot = Entry.Value;
		if (!Slot.bDisconnected || Slot.Pawn.Get() != Pawn) continue;
		if (Slot.bDeathRecorded || Pawn == nullptr || !Pawn->IsDead()) return true;
		Slot.bDeathRecorded = true;
		if (auto* State = Slot.State.Get()) State->AddDeath();
		if (auto* KillerPlayer = Cast<APlayerController>(Killer))
			if (auto* State = KillerPlayer->GetPlayerState<AfpstruePvpPlayerState>()) if (State != Slot.State.Get()) State->AddKill();
		if (auto* State = Cast<AfpstruePvpGameState>(GameState)) State->AddElimination();
		return true;
	}
	return false;
}

// 到期先移出握手/保留表再销毁对象；清理回调不能继续认领已经过期的恢复身份。
void AfpstruePvpGameMode::SweepRejoinSlots()
{
	const double Now = GetWorld()->GetTimeSeconds();
	TArray<TWeakObjectPtr<AfpstruePvpPlayerController>> TimedOut;
	for (const auto& Entry : PendingConnectionHandshakes) if (!Entry.Key.IsValid() || Entry.Value <= Now) TimedOut.Add(Entry.Key);
	for (const auto& Key : TimedOut)
	{
		PendingConnectionHandshakes.Remove(Key);
		if (auto* Player = Key.Get()) { Player->bIntentionalLeave = true; Player->ClientConnectionRejected(TEXT("handshake_timeout")); Player->Destroy(); }
	}
	TArray<FRejoinSlot> Expired;
	for (auto It = RejoinSlots.CreateIterator(); It; ++It)
		if (It.Value().bDisconnected && (It.Value().ExpiresAt <= Now || !It.Value().State.IsValid())) { Expired.Add(It.Value()); It.RemoveCurrent(); }
	for (const FRejoinSlot& Slot : Expired)
	{
		ReleaseNetworkPlayerWeapon(Slot.Weapon.Get(), Slot.Pawn.Get());
		if (auto* Pawn = Slot.Pawn.Get()) Pawn->Destroy();
		if (auto* State = Slot.State.Get()) { State->SetRetainedForReconnect(false); State->Destroy(); }
	}
}

// 对局退出时撤销握手与清扫 Timer，回收离线保留对象；在线玩家仍走正常离场清理。
void AfpstruePvpGameMode::ClearRejoinSlots()
{
	GetWorldTimerManager().ClearTimer(RejoinCleanupTimer);
	PendingConnectionHandshakes.Reset();
	const auto EndingSlots = MoveTemp(RejoinSlots);
	RejoinSlots.Reset();
	for (const auto& Entry : EndingSlots)
	{
		if (!Entry.Value.bDisconnected) continue;
		ReleaseNetworkPlayerWeapon(Entry.Value.Weapon.Get(), Entry.Value.Pawn.Get(), nullptr, bEndingPvp);
		if (auto* Pawn = Entry.Value.Pawn.Get()) Pawn->Destroy();
		if (auto* State = Entry.Value.State.Get()) { State->SetRetainedForReconnect(false); State->Destroy(); }
	}
}

// 离线先撤销重生资格；已在 PawnLeavingGame 保留的真实对象不在 Logout 再次销毁。
void AfpstruePvpGameMode::Logout(AController* Exiting)
{
	if (APlayerController* Player = Cast<APlayerController>(Exiting))
	{
		FSpawnRecovery Recovery;
		if (SpawnRecoveries.RemoveAndCopyValue(Player, Recovery)) GetWorldTimerManager().ClearTimer(Recovery.Timer);
		ReleaseUnclaimedSpawnPickup(Player);
		FTimerHandle Timer;
		if (PendingRespawns.RemoveAndCopyValue(TWeakObjectPtr<APlayerController>(Player), Timer)) GetWorldTimerManager().ClearTimer(Timer);
		PendingConnectionHandshakes.Remove(Cast<AfpstruePvpPlayerController>(Player));
		const auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Player);
		FRejoinSlot* Slot = PvpPlayer != nullptr ? RejoinSlots.Find(PvpPlayer->AssignedRejoinToken) : nullptr;
		if (Slot == nullptr || !Slot->bDisconnected)
		{
			if (PvpPlayer != nullptr) RejoinSlots.Remove(PvpPlayer->AssignedRejoinToken);
			ReleasePlayerWeapon(Player);
		}
	}
	Super::Logout(Exiting);
}
