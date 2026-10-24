// Copyright Epic Games, Inc. All Rights Reserved.

// 玩家脚部 IK 回归：独立地面夹具 → 真实动画求值 → 足底几何采样 → 离地/死亡复位。
// 不启用敌人 IK，也不把临时测试地面或手动推进姿态称为性能、联机或画面验收。

#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/Shared/fpstrueFootIK.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimNode_LinkedInputPose.h"
#include "BonePose.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Tests/AutomationCommon.h"

namespace
{
constexpr const TCHAR* PlayerClassPath = TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter.BP_TemplateCharacter_C");
constexpr const TCHAR* EnemyClassPath = TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP_C");

// 创建无额外玩法的临时世界，将初始化失败转为当前自动化用例的错误。
bool BeginFootWorld(FTestWorldWrapper& Fixture, FAutomationTestBase& Test)
{
	if (Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.GetTestWorld()->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
		if (Fixture.BeginPlayInTestWorld()) return true;
	}
	Fixture.ForwardErrorMessages(&Test);
	return false;
}

// 几何测试只改变独立临时地面；真实角色始终由自己的 CMC/动画组件拥有，不改项目关卡或供应商资产。
UBoxComponent* SpawnFootFloor(UWorld& World, const FVector& Extent = FVector(2000.0f, 2000.0f, 20.0f))
{
	AActor* Owner = World.SpawnActor<AActor>();
	if (!Owner) return nullptr;
	auto* Floor = NewObject<UBoxComponent>(Owner);
	Owner->SetRootComponent(Floor); Owner->AddInstanceComponent(Floor);
	Floor->SetBoxExtent(Extent); Floor->SetMobility(EComponentMobility::Movable);
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetWorldLocation(FVector(0.0f, 0.0f, -Extent.Z));
	Floor->RegisterComponent();
	return Floor;
}

// 加载实际角色资产并关闭自动移动/网格 Tick，让夹具显式控制每次动画步进。
ACharacter* SpawnFootCharacter(UWorld& World, bool bEnemy, const FVector& Location, FAutomationTestBase& Test)
{
	UClass* Class = bEnemy ? LoadClass<AfpstrueEnemyCharacter>(nullptr, EnemyClassPath)
		: LoadClass<AfpstrueCharacter>(nullptr, PlayerClassPath);
	if (!Test.TestNotNull(TEXT("Foot IK fixture resolves its actual project character Blueprint"), Class)) return nullptr;
	const FTransform Transform(Location);
	auto* Character = World.SpawnActorDeferred<ACharacter>(Class, Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Test.TestNotNull(TEXT("Foot IK fixture spawns its actual character class"), Character)) return nullptr;
	Character->AutoPossessPlayer = EAutoReceiveInput::Disabled; Character->AutoPossessAI = EAutoPossessAI::Disabled;
	Character->FinishSpawning(Transform);
	Character->GetCharacterMovement()->SetComponentTickEnabled(false);
	Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Character->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	Character->GetMesh()->SetComponentTickEnabled(false);
	Character->GetMesh()->bEnableUpdateRateOptimizations = false;
	Character->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	return Character;
}

// 先推进世界，再执行一次角色动画与骨骼刷新，供断言读取最终姿态。
bool StepFootPose(FTestWorldWrapper& Fixture, ACharacter& Character, FAutomationTestBase& Test, float Delta = 1.0f / 60.0f)
{
	if (!Fixture.TickTestWorld(Delta)) { Fixture.ForwardErrorMessages(&Test); return false; }
	Character.GetMesh()->TickAnimation(Delta, false);
	Character.GetMesh()->RefreshBoneTransforms();
	return true;
}

struct FFootSoleSample
{
	double Gap = TNumericLimits<double>::Max();
	FVector Center = FVector::ZeroVector;
	FVector Minimum = FVector(TNumericLimits<double>::Max());
	FVector Maximum = FVector(TNumericLimits<double>::Lowest());
	int32 HighSupportVertices = 0, HighSupportContactVertices = 0;
};

// 用最终蒙皮顶点与已知真实碰撞平面独立计量；不把踝骨、IK目标或诊断快照当鞋底。
bool ReadFootSoles(ACharacter& Character, const FVector& FloorPoint, const FVector& FloorNormal,
	FFootSoleSample& Left, FFootSoleSample& Right, FAutomationTestBase& Test, const UPrimitiveComponent* HighSupport = nullptr)
{
	USkeletalMeshComponent* Mesh = Character.GetMesh();
	const auto* IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
	const USkeletalMesh* Body = Mesh->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* Data = Body ? Body->GetResourceForRendering() : nullptr;
	FSkinWeightVertexBuffer* Weights = Mesh->GetSkinWeightBuffer(0);
	if (!Test.TestNotNull(TEXT("Final-pose sole measurement has its installed IK instance"), IK)
		|| !Test.TestTrue(TEXT("Final-pose sole measurement has real LOD0 CPU geometry and skin weights"),
			Data && Data->LODRenderData.IsValidIndex(0) && Weights && Weights->GetDataVertexBuffer()
			&& Weights->GetDataVertexBuffer()->GetWeightData())) return false;
	const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
	TArray<FMatrix44f> Matrices; Mesh->GetCurrentRefToLocalMatrices(Matrices, 0);
	for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
		for (const FBoneIndexType Bone : Section.BoneMap)
			if (!Test.TestTrue(TEXT("Final-pose skin matrices include every LOD section bone"), Matrices.IsValidIndex(Bone))) return false;
	const TArray<int32>* Vertices[] = {&IK->LeftSoleVertices, &IK->RightSoleVertices};
	FFootSoleSample* Samples[] = {&Left, &Right};
	FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueFootIKSoleTest), true, &Character);
	const double CapsuleBottomZ = Character.GetActorLocation().Z - Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	for (int32 Side = 0; Side < 2; ++Side)
	{
		if (!Test.TestTrue(TEXT("Each foot supplies its own bounded real shoe-sole vertex subset"),
			Vertices[Side]->Num() > 0 && Vertices[Side]->Num() <= 48)) return false;
		*Samples[Side] = FFootSoleSample();
		for (const int32 Vertex : *Vertices[Side])
		{
			if (!Test.TestTrue(TEXT("Each cached shoe vertex belongs to the actual body LOD"), Vertex >= 0 && uint32(Vertex) < LOD.GetNumVertices())) return false;
			const FVector Position = Mesh->GetComponentTransform().TransformPosition(FVector(
				USkinnedMeshComponent::GetSkinnedVertexPosition(Mesh, uint32(Vertex), LOD, *Weights, Matrices)));
			if (!Test.TestTrue(TEXT("The actual skinned sole remains finite"), !Position.ContainsNaN())) return false;
			double Gap = FVector::DotProduct(Position - FloorPoint, FloorNormal);
			if (HighSupport)
			{
				TArray<FHitResult> Hits;
				Character.GetWorld()->LineTraceMultiByObjectType(Hits, FVector(Position.X, Position.Y, CapsuleBottomZ + 45.0),
					FVector(Position.X, Position.Y, CapsuleBottomZ - 45.0), Objects, Query);
				bool bFoundSupport = false;
				for (const FHitResult& Hit : Hits)
				{
					if (Cast<APawn>(Hit.GetActor()) || !Hit.GetComponent()) continue;
					Gap = FVector::DotProduct(Position - Hit.ImpactPoint, Hit.ImpactNormal);
					if (Hit.GetComponent() == HighSupport)
					{
						++Samples[Side]->HighSupportVertices;
						if (Gap >= -2.0 && Gap <= 3.0) ++Samples[Side]->HighSupportContactVertices;
					}
					bFoundSupport = true; break;
				}
				if (!Test.TestTrue(TEXT("Every measured curb-scene shoe vertex has its actual collision support below its own XY"), bFoundSupport)) return false;
			}
			Samples[Side]->Gap = FMath::Min(Samples[Side]->Gap, Gap);
			Samples[Side]->Center += Position;
			Samples[Side]->Minimum = Samples[Side]->Minimum.ComponentMin(Position);
			Samples[Side]->Maximum = Samples[Side]->Maximum.ComponentMax(Position);
		}
		Samples[Side]->Center /= Vertices[Side]->Num();
	}
	return true;
}

// 以当前 Socket 世界位置量取两段腿长；返回值仅供检测 IK 是否过度拉伸。
FVector LegLengths(const USkeletalMeshComponent& Mesh, const TCHAR* Side)
{
	const FVector Thigh = Mesh.GetSocketLocation(FName(FString(TEXT("thigh_")) + Side));
	const FVector Calf = Mesh.GetSocketLocation(FName(FString(TEXT("calf_")) + Side));
	const FVector Foot = Mesh.GetSocketLocation(FName(FString(TEXT("foot_")) + Side));
	return FVector(FVector::Distance(Thigh, Calf), FVector::Distance(Calf, Foot), 0.0f);
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueFootIKAssetBindingsTest,
	"fpstrue.Multiplayer.Animation.FootIK.AssetBindingsAndOwnerIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 核对真实蓝图 IK 绑定、实例归属和并行门禁，确保敌人不误用玩家世界查询后处理。
bool FFpstrueFootIKAssetBindingsTest::RunTest(const FString&)
{
	// UE会先读取CanRunParallelWork，再为首帧补TickAnimation/PreUpdate。
	// 无Owner、尚未初始化的实例也必须拒绝并行，不能等第一次贴地资格采样才切换门禁。
	// AnimInstance的ClassWithin要求SkeletalMeshComponent；未初始化不等于允许无效Outer。
	USkeletalMeshComponent* UninitializedMesh = NewObject<USkeletalMeshComponent>();
	const auto* UninitializedIK = NewObject<UfpstrueFootIKAnimInstance>(UninitializedMesh);
	if (!TestNotNull(TEXT("Foot IK thread gate is available before proxy initialization"), UninitializedIK)) return false;
	TestFalse(TEXT("Player world-query IK rejects parallel work before its first PreUpdate"), UninitializedIK->CanRunParallelWork());
	FTestWorldWrapper Fixture;
	if (!BeginFootWorld(Fixture, *this)) return false;
	for (const bool bEnemy : {false, true})
	{
		ACharacter* Character = SpawnFootCharacter(*Fixture.GetTestWorld(), bEnemy,
			FVector(bEnemy ? 1000.0f : 0.0f, 0.0f, 100.0f), *this);
		if (!Character) return false;
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		const USkeletalMesh* Body = Mesh->GetSkeletalMeshAsset();
		if (!TestNotNull(TEXT("Foot IK binds to the actual character body mesh"), Body)) return false;
		const auto SourcePostProcess = Body->GetPostProcessAnimBlueprint();
		UClass* MainAnimationClass = Mesh->GetAnimClass();
		UfpstrueFootIKAnimInstance* IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
		if (bEnemy)
		{
			TestNull(TEXT("Enemy startup never mounts a foot IK post-process instance"), Mesh->GetPostProcessInstance());
			TestTrue(TEXT("Enemy presentation explicitly disables post-process Blueprint evaluation"), Mesh->GetDisablePostProcessBlueprint());
			if (const auto* MainAnimation = Mesh->GetAnimInstance())
				TestTrue(TEXT("Player-only IK thread gate does not disable enemy main-animation parallel eligibility"), MainAnimation->CanRunParallelWork());
			UfpstrueFootIKAnimInstance::Install(Character);
			TestTrue(TEXT("The player-only installer rejects enemies without changing their main animation or body asset"),
				Mesh->GetPostProcessInstance() == nullptr && Mesh->GetAnimClass() == MainAnimationClass
				&& Body->GetPostProcessAnimBlueprint() == SourcePostProcess);
			auto* Enemy = CastChecked<AfpstrueEnemyCharacter>(Character);
			FFPEnemyRenderSignificancePolicy Policy;
			for (const auto Tier : {EFPEnemyRenderSignificanceTier::Full, EFPEnemyRenderSignificanceTier::Reduced, EFPEnemyRenderSignificanceTier::Background})
			{
				Enemy->ApplyRenderSignificanceTier(Tier, false, false, false, Policy);
				for (int32 Frame = 0; Frame < 3; ++Frame) if (!StepFootPose(Fixture, *Character, *this)) return false;
				TestTrue(TEXT("No enemy render-importance tier can re-enable the removed foot IK"),
					Mesh->GetPostProcessInstance() == nullptr && Mesh->GetDisablePostProcessBlueprint());
			}
			continue;
		}
		if (!TestNotNull(TEXT("Normal player startup installs the independent body foot IK instance"), IK)) return false;
		TestFalse(TEXT("The installed player post-process keeps a stable synchronous evaluation contract"), IK->CanRunParallelWork());
		TestEqual(TEXT("Each character installs its explicit project-owned foot IK class"), IK->GetClass()->GetPathName(),
			FString(TEXT("/Game/FirstPerson/Animations/FootIK/ABP_PlayerFootIK.ABP_PlayerFootIK_C")));
		const auto* Graph = IAnimClassInterface::GetFromClass(IK->GetClass());
		TestTrue(TEXT("Post-process IK targets the same skeleton as its actual body"), Graph && Graph->GetTargetSkeleton() == Body->GetSkeleton());
		TestTrue(TEXT("Both calibrated foot-local sole offsets are finite and independent of world placement"),
			!IK->LeftSoleOffset.ContainsNaN() && !IK->RightSoleOffset.ContainsNaN()
			&& IK->LeftSoleOffset.Size() < 50.0 && IK->RightSoleOffset.Size() < 50.0);
		const TArray<FVector>* Probes[] = {&IK->LeftSoleProbes, &IK->RightSoleProbes};
		const TArray<FVector>* ToeProbes[] = {&IK->LeftToeProbes, &IK->RightToeProbes};
		const FVector SoleOffsets[] = {IK->LeftSoleOffset, IK->RightSoleOffset};
		for (int32 Side = 0; Side < 2; ++Side)
		{
			TestEqual(TEXT("Each real sole supplies its calibrated three-by-three ground-query footprint"), Probes[Side]->Num(), 9);
			bool bHasCenter = false;
			for (const FVector& Probe : *Probes[Side])
			{
				TestTrue(TEXT("Every calibrated sole probe is finite and stays within the foot-local shoe extent"), !Probe.ContainsNaN() && Probe.Size() < 50.0);
				bHasCenter |= Probe.Equals(SoleOffsets[Side], 0.01);
			}
			TestTrue(TEXT("Each calibrated footprint retains the original sole-center query"), bHasCenter);
			TestTrue(TEXT("Each independently calibrated ball-dominant toe patch stays bounded by nine queries"), ToeProbes[Side]->Num() <= 9);
			for (const FVector& Probe : *ToeProbes[Side])
				TestTrue(TEXT("Every calibrated toe probe is finite and remains in its own ball-local shoe extent"), !Probe.ContainsNaN() && Probe.Size() < 50.0);
		}
		const TArray<int32> OriginalLeftSoleVertices = IK->LeftSoleVertices, OriginalRightSoleVertices = IK->RightSoleVertices;
		FFootSoleSample Left, Right;
		if (!StepFootPose(Fixture, *Character, *this) || !ReadFootSoles(*Character, FVector::ZeroVector, FVector::UpVector, Left, Right, *this)) return false;
		UfpstrueFootIKAnimInstance::Install(Character);
		TestTrue(TEXT("Repeated installation preserves the live instance and original main animation class"),
			Mesh->GetPostProcessInstance() == IK && Mesh->GetAnimClass() == MainAnimationClass);
		TestFalse(TEXT("Repeated player IK installation never relaxes its world-query thread gate"), IK->CanRunParallelWork());
		TestTrue(TEXT("Footprint probes never replace or mutate the independent real CPU sole-vertex samples"),
			IK->LeftSoleVertices == OriginalLeftSoleVertices && IK->RightSoleVertices == OriginalRightSoleVertices);
		TestTrue(TEXT("Actor-specific IK installation never mutates the supplier skeletal mesh's post-process class"),
			Body->GetPostProcessAnimBlueprint() == SourcePostProcess);
		if (const auto* Player = Cast<AfpstrueCharacter>(Character))
			TestFalse(TEXT("The camera-relative first-person mesh never runs world-grounded leg IK"),
				Player->GetMesh1P()->GetPostProcessInstance() && Player->GetMesh1P()->GetPostProcessInstance()->IsA<UfpstrueFootIKAnimInstance>());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueFootIKGroundPoseTest,
	"fpstrue.Multiplayer.Animation.FootIK.GroundAndSlopePose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueFootIKResetTest,
	"fpstrue.Multiplayer.Animation.FootIK.AirDeathAndReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 在独立平地/斜面上采样实际蒙皮足底，验证贴地、倾斜和腿长约束。
bool FFpstrueFootIKGroundPoseTest::RunTest(const FString&)
{
	{
		FTestWorldWrapper Fixture;
		if (!BeginFootWorld(Fixture, *this)) return false;
		UBoxComponent* Floor = SpawnFootFloor(*Fixture.GetTestWorld());
		if (!TestNotNull(TEXT("Grounding fixture has its real query collision floor"), Floor)) return false;
		UClass* Class = LoadClass<AfpstrueCharacter>(nullptr, PlayerClassPath);
		if (!TestNotNull(TEXT("Grounding fixture resolves its real character defaults"), Class)) return false;
		const float HalfHeight = Class->GetDefaultObject<ACharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		ACharacter* Character = SpawnFootCharacter(*Fixture.GetTestWorld(), false, FVector(0, 0, HalfHeight + 2.15f), *this);
		if (!Character) return false;
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		auto* IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
		if (!TestNotNull(TEXT("Grounding fixture starts its configured post-process IK"), IK)) return false;
		UClass* MainAnimationClass = Mesh->GetAnimClass();
		const EAnimationMode::Type MainAnimationMode = Mesh->GetAnimationMode();
		UAnimSequence* FixedIdle = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/FPSMPT55/Animations/TPP/AR/A_TPP_AR_Idle.A_TPP_AR_Idle"));
		if (!TestNotNull(TEXT("Same-input stability uses the real skeleton-compatible calibrated Idle asset"), FixedIdle)) return false;
		TestTrue(TEXT("The frozen stability input uses the character's actual body skeleton"), FixedIdle->GetSkeleton() == Mesh->GetSkeletalMeshAsset()->GetSkeleton());
		const FTransform OriginalActor = Character->GetActorTransform();
		Mesh->SetDisablePostProcessBlueprint(true);
		if (!StepFootPose(Fixture, *Character, *this)) return false;
		const FVector OriginalLeftLengths = LegLengths(*Mesh, TEXT("l"));
		const FVector OriginalRightLengths = LegLengths(*Mesh, TEXT("r"));
		const float OriginalRootYaw = Mesh->GetSocketTransform(TEXT("root"), RTS_World).Rotator().Yaw;
		Mesh->SetDisablePostProcessBlueprint(false);
		UBoxComponent* Step = nullptr;
		for (int32 Surface = 0; Surface < 4; ++Surface)
		{
			const FVector Normal = Surface == 1 ? FRotator(20.0f, 0.0f, 0.0f).RotateVector(FVector::UpVector) : FVector::UpVector;
			Floor->SetWorldRotation(Surface == 1 ? FRotator(20.0f, 0.0f, 0.0f) : FRotator::ZeroRotator);
			Floor->SetWorldLocation(-Normal * Floor->GetUnscaledBoxExtent().Z);
			if (Surface >= 2)
			{
				if (Step) { Step->SetCollisionEnabled(ECollisionEnabled::NoCollision); Step = nullptr; }
				IK->ResetFootPlacement();
				for (int32 Frame = 0; Frame < 30; ++Frame) if (!StepFootPose(Fixture, *Character, *this)) return false;
				FFootSoleSample Left, Right;
				if (!ReadFootSoles(*Character, FVector::ZeroVector, Normal, Left, Right, *this)) return false;
				FVector HalfSize = (Left.Maximum - Left.Minimum) * 0.5f + FVector(4.0f, 4.0f, 0.0f); HalfSize.Z = 7.5f;
				FVector StepCenter = (Left.Maximum + Left.Minimum) * 0.5f; StepCenter.Z = 7.5f;
				if (Surface == 3)
				{
					// 沿真实鞋底较长的 XY 轴保留六厘米外缘重叠，鞋心仍在低面上，不能退化成全足台阶。
					const int32 EdgeAxis = Left.Maximum.X - Left.Minimum.X >= Left.Maximum.Y - Left.Minimum.Y ? 0 : 1;
					HalfSize[EdgeAxis] = 3.0f; StepCenter[EdgeAxis] = Left.Maximum[EdgeAxis] - 3.0f;
					const FVector StripMinimum = StepCenter - HalfSize, StripMaximum = StepCenter + HalfSize;
					TestTrue(TEXT("The narrow curb overlaps only the outer edge of the real left sole, not its center"),
						StripMinimum[EdgeAxis] < Left.Maximum[EdgeAxis] && StripMinimum[EdgeAxis] > Left.Center[EdgeAxis]
						&& StripMinimum[EdgeAxis] > Left.Minimum[EdgeAxis]);
					TestTrue(TEXT("The narrow left-foot curb never raises the other foot's support surface"),
						StripMinimum.X > Right.Maximum.X || StripMaximum.X < Right.Minimum.X
						|| StripMinimum.Y > Right.Maximum.Y || StripMaximum.Y < Right.Minimum.Y);
					AddInfo(FString::Printf(TEXT("Foot IK narrow-curb fixture player axis=%s width=%.4fcm height=15cm leftSoleMin=%s leftSoleMax=%s leftSoleCenter=%s stripMin=%s stripMax=%s; per-vertex actual collision support is measured."),
						EdgeAxis == 0 ? TEXT("X") : TEXT("Y"), HalfSize[EdgeAxis] * 2.0, *Left.Minimum.ToString(),
						*Left.Maximum.ToString(), *Left.Center.ToString(), *StripMinimum.ToString(), *StripMaximum.ToString()));
				}
				Step = SpawnFootFloor(*Fixture.GetTestWorld(), HalfSize);
				if (!TestNotNull(TEXT("Grounding fixture creates a real fifteen-centimeter step under one foot"), Step)) return false;
				if (Surface == 3)
				{
					Step->CanCharacterStepUpOn = ECB_No;
					TestTrue(TEXT("The narrow curb forbids capsule side-step-up without removing its walkable top support"), Step->CanCharacterStepUpOn == ECB_No);
				}
				Step->SetWorldLocation(StepCenter);
			}
			IK->ResetFootPlacement();
			double MinGap = TNumericLimits<double>::Max(), MaxGap = TNumericLimits<double>::Lowest();
			double MaximumLegError = 0.0, MaximumPlantMotion = 0.0, MaximumRootYawError = 0.0;
			int32 CurbOverlapFrames = 0, CurbContactFrames = 0;
			struct FSettledFootDiagnostic
			{
				double RawLiftMin = TNumericLimits<double>::Max(), RawLiftMax = TNumericLimits<double>::Lowest();
				double AlphaMin = TNumericLimits<double>::Max(), AlphaMax = TNumericLimits<double>::Lowest(), ReachMax = 0.0;
				double GapMin = TNumericLimits<double>::Max(), GapMax = TNumericLimits<double>::Lowest();
				double WorstSolvedGap = 0.0, WorstNormalAngle = 0.0;
				float WorstPelvis = 0.0f;
				int32 WorstFrame = INDEX_NONE;
				bool bWorstPlanted = false;
			} Diagnostics[2];
			FFootSoleSample FirstLeft, FirstRight, LastLeft, LastRight;
			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				if (!StepFootPose(Fixture, *Character, *this)) return false;
				FFootSoleSample Left, Right;
				if (!ReadFootSoles(*Character, FVector::ZeroVector, Normal, Left, Right, *this, Surface == 3 ? Step : nullptr)) return false;
				if (Surface == 2)
				{
					FFootSoleSample Unused;
					if (!ReadFootSoles(*Character, FVector(0, 0, 15.0f), Normal, Left, Unused, *this)) return false;
				}
				if (Frame < 30) continue;
				if (Surface == 3)
				{
					CurbOverlapFrames += Left.HighSupportVertices > 0;
					CurbContactFrames += Left.HighSupportContactVertices > 0;
					if (Frame == 30 || Frame == 45 || Frame == 59)
					{
						const FVector StepMinimum = Step->GetComponentLocation() - Step->GetScaledBoxExtent();
						const FVector StepMaximum = Step->GetComponentLocation() + Step->GetScaledBoxExtent();
						AddInfo(FString::Printf(TEXT("Foot IK actual-curb support player frame=%d stripMin=%s stripMax=%s leftSoleMin=%s leftSoleMax=%s rightSoleMin=%s rightSoleMax=%s leftHighSupportVertices=%d leftHighContactVertices=%d rightHighSupportVertices=%d."),
							Frame, *StepMinimum.ToString(), *StepMaximum.ToString(), *Left.Minimum.ToString(),
							*Left.Maximum.ToString(), *Right.Minimum.ToString(), *Right.Maximum.ToString(),
							Left.HighSupportVertices, Left.HighSupportContactVertices, Right.HighSupportVertices));
					}
				}
				const auto FrameDebug = IK->GetFootIKDebug();
				const FFootSoleSample* Samples[] = {&Left, &Right};
				const FVector SolvedSoles[] = {FrameDebug.LeftSoleWorld, FrameDebug.RightSoleWorld};
				const FVector SoleNormals[] = {IK->LeftSoleNormal, IK->RightSoleNormal};
				const bool Planted[] = {FrameDebug.bLeftPlanted, FrameDebug.bRightPlanted};
				for (int32 Side = 0; Side < 2; ++Side)
				{
					auto& Diagnostic = Diagnostics[Side];
					const double RawLift = FrameDebug.RawSoleHeights[Side], Alpha = FrameDebug.SupportWeights[Side];
					Diagnostic.RawLiftMin = FMath::Min(Diagnostic.RawLiftMin, RawLift);
					Diagnostic.RawLiftMax = FMath::Max(Diagnostic.RawLiftMax, RawLift);
					Diagnostic.AlphaMin = FMath::Min(Diagnostic.AlphaMin, Alpha);
					Diagnostic.AlphaMax = FMath::Max(Diagnostic.AlphaMax, Alpha);
					Diagnostic.ReachMax = FMath::Max(Diagnostic.ReachMax, double(FrameDebug.ReachErrors[Side]));
					Diagnostic.GapMin = FMath::Min(Diagnostic.GapMin, Samples[Side]->Gap);
					const bool bHighSupport = Surface == 2 && Side == 0 || Surface == 3 && Samples[Side]->HighSupportVertices > 0;
					const FVector SupportPoint = bHighSupport ? FVector(0, 0, 15.0f) : FVector::ZeroVector;
					const double SolvedGap = FVector::DotProduct(SolvedSoles[Side] - SupportPoint, Normal);
					const FTransform FootTransform = Mesh->GetSocketTransform(Side == 0 ? TEXT("foot_l") : TEXT("foot_r"), RTS_World);
					const FVector ActualSoleNormal = FootTransform.TransformVectorNoScale(SoleNormals[Side]).GetSafeNormal();
					const double NormalAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
						FVector::DotProduct(ActualSoleNormal, Normal), -1.0, 1.0)));
					if (Samples[Side]->Gap > Diagnostic.GapMax)
					{
						Diagnostic.GapMax = Samples[Side]->Gap; Diagnostic.WorstFrame = Frame;
						Diagnostic.WorstSolvedGap = SolvedGap; Diagnostic.WorstNormalAngle = NormalAngle;
						Diagnostic.bWorstPlanted = Planted[Side]; Diagnostic.WorstPelvis = FrameDebug.PelvisOffset;
					}
					if (Frame == 30 || Frame == 45 || Frame == 59)
						AddInfo(FString::Printf(TEXT("Foot IK settled snapshot player surface=%d frame=%d side=%s actualSkinGap=%.4fcm solvedOffsetGap=%.4fcm soleNormalAngle=%.4fdeg rawLift=%.4fcm alpha=%.4f reachError=%.4fcm planted=%d pelvis=%.4fcm."),
							Surface, Frame, Side == 0 ? TEXT("left") : TEXT("right"), Samples[Side]->Gap, SolvedGap, NormalAngle,
							RawLift, Alpha, FrameDebug.ReachErrors[Side], Planted[Side], FrameDebug.PelvisOffset));
				}
				if (Frame == 30) { FirstLeft = Left; FirstRight = Right; }
				LastLeft = Left; LastRight = Right;
				MinGap = FMath::Min(MinGap, FMath::Min(Left.Gap, Right.Gap));
				MaxGap = FMath::Max(MaxGap, FMath::Max(Left.Gap, Right.Gap));
				MaximumLegError = FMath::Max(MaximumLegError, FMath::Max(
					(LegLengths(*Mesh, TEXT("l")) - OriginalLeftLengths).GetAbsMax(),
					(LegLengths(*Mesh, TEXT("r")) - OriginalRightLengths).GetAbsMax()));
				MaximumPlantMotion = FMath::Max(MaximumPlantMotion, FMath::Max(
					FVector::VectorPlaneProject(Left.Center - FirstLeft.Center, Normal).Size(),
					FVector::VectorPlaneProject(Right.Center - FirstRight.Center, Normal).Size()));
				MaximumRootYawError = FMath::Max(MaximumRootYawError, double(FMath::Abs(FMath::FindDeltaAngleDegrees(
					OriginalRootYaw, Mesh->GetSocketTransform(TEXT("root"), RTS_World).Rotator().Yaw))));
			}
			const auto Debug = IK->GetFootIKDebug();
			AddInfo(FString::Printf(TEXT("Foot IK final-skin fixture player surface=%d settledFrames=30 gaps=[%.4f,%.4f]cm plantTangentMotion=%.4fcm legLengthError=%.4fcm rootYawError=%.4fdeg pelvis=%.4fcm traces=%u. Not a rendered/network acceptance."),
				Surface, MinGap, MaxGap, MaximumPlantMotion, MaximumLegError, MaximumRootYawError, Debug.PelvisOffset, Debug.TraceCount));
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const auto& Diagnostic = Diagnostics[Side];
				AddInfo(FString::Printf(TEXT("Foot IK settled ranges player surface=%d side=%s actualSkinGap=[%.4f,%.4f]cm rawLift=[%.4f,%.4f]cm alpha=[%.4f,%.4f] reachErrorMax=%.4fcm worstFrame=%d worstSolvedOffsetGap=%.4fcm worstSoleNormalAngle=%.4fdeg worstPlanted=%d worstPelvis=%.4fcm."),
					Surface, Side == 0 ? TEXT("left") : TEXT("right"), Diagnostic.GapMin, Diagnostic.GapMax,
					Diagnostic.RawLiftMin, Diagnostic.RawLiftMax, Diagnostic.AlphaMin, Diagnostic.AlphaMax, Diagnostic.ReachMax,
					Diagnostic.WorstFrame, Diagnostic.WorstSolvedGap, Diagnostic.WorstNormalAngle, Diagnostic.bWorstPlanted, Diagnostic.WorstPelvis));
			}
			TestTrue(TEXT("Independent character IK is active on the actual collision surface"), Debug.bEnabled && Debug.TraceCount > 0);
			if (Surface == 3)
			{
				AddInfo(FString::Printf(TEXT("Foot IK actual-curb coverage player settledFrames=30 actualOverlapFrames=%d actualHighContactFrames=%d."),
					CurbOverlapFrames, CurbContactFrames));
				TestTrue(TEXT("The real left shoe overlaps and contacts the narrow high curb in at least ten settled frames"),
					CurbOverlapFrames >= 10 && CurbContactFrames >= 10);
			}
			TestTrue(TEXT("Both real resting shoe soles stay near their own support surfaces without large penetration"), MinGap >= -2.0 && MaxGap <= 3.0);
			AddInfo(FString::Printf(TEXT("Foot IK dynamic Idle phase change player surface=%d firstGaps=[%.4f,%.4f]cm lastGaps=[%.4f,%.4f]cm; changing animation phases are not a same-input feedback test."),
				Surface, FirstLeft.Gap, FirstRight.Gap, LastLeft.Gap, LastRight.Gap));
			TestTrue(TEXT("Planted final-skin shoe patches remain stable in the support tangent plane"), MaximumPlantMotion < 2.0);
			TestTrue(TEXT("Foot IK preserves both segment lengths instead of stretching the legs"), MaximumLegError < 0.1);
			TestTrue(TEXT("Post-process foot placement does not override the main body's root-yaw facing"), MaximumRootYawError < 0.1);
			TestTrue(TEXT("Foot placement never moves or rotates the owning actor or its CMC collision"), Character->GetActorTransform().Equals(OriginalActor, 0.001f));
			// 冻结的是真实 Idle 输入时间，不暂停组件或后处理；用同一输入姿态检查反馈漂移，不能混入正常抬脚相位。
			Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode); Mesh->SetAnimation(FixedIdle);
			auto* SingleNode = Mesh->GetSingleNodeInstance();
			if (!TestNotNull(TEXT("Same-input stability has its real single-node Idle source"), SingleNode)) return false;
			SingleNode->SetPosition(0.0f, false); SingleNode->SetPlaying(false);
			IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
			if (!TestNotNull(TEXT("Changing the main input preserves a live independent foot post-process instance"), IK)) return false;
			IK->ResetFootPlacement();
			const uint32 FixedBeforeTraces = IK->GetFootIKDebug().TraceCount;
			FFootSoleSample FixedFirstLeft, FixedFirstRight, FixedLastLeft, FixedLastRight;
			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				if (!StepFootPose(Fixture, *Character, *this)) return false;
				if (Frame < 30) continue;
				FFootSoleSample Left, Right;
				if (!ReadFootSoles(*Character, FVector::ZeroVector, Normal, Left, Right, *this, Surface == 3 ? Step : nullptr)) return false;
				if (Surface == 2)
				{
					FFootSoleSample Unused;
					if (!ReadFootSoles(*Character, FVector(0, 0, 15.0f), Normal, Left, Unused, *this)) return false;
				}
				if (Frame == 30) { FixedFirstLeft = Left; FixedFirstRight = Right; }
				FixedLastLeft = Left; FixedLastRight = Right;
			}
			const double LeftDrift = FMath::Abs(FixedLastLeft.Gap - FixedFirstLeft.Gap);
			const double RightDrift = FMath::Abs(FixedLastRight.Gap - FixedFirstRight.Gap);
			const auto FixedDebug = IK->GetFootIKDebug();
			AddInfo(FString::Printf(TEXT("Foot IK same-input stability player surface=%d Idle=%s frozenTime=%.4fs frames=60 settle=30 firstGaps=[%.4f,%.4f]cm lastGaps=[%.4f,%.4f]cm drift=[%.4f,%.4f]cm active=%d tracesBefore=%u tracesAfter=%u; main source paused, post-process receives real frame Delta."),
				Surface, *FixedIdle->GetPathName(), SingleNode->GetCurrentTime(), FixedFirstLeft.Gap, FixedFirstRight.Gap,
				FixedLastLeft.Gap, FixedLastRight.Gap, LeftDrift, RightDrift, FixedDebug.bEnabled, FixedBeforeTraces, FixedDebug.TraceCount));
			TestTrue(TEXT("The same Idle input stays frozen while the independent post-process keeps updating"),
				FMath::IsNearlyZero(SingleNode->GetCurrentTime()) && FixedDebug.bEnabled && FixedDebug.TraceCount > FixedBeforeTraces);
			TestTrue(TEXT("Thirty settled frames of the same real Idle input do not accumulate foot-height feedback drift"), LeftDrift < 0.5 && RightDrift < 0.5);
			Mesh->SetAnimInstanceClass(MainAnimationClass); Mesh->SetAnimationMode(MainAnimationMode, false);
			IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
			if (!TestNotNull(TEXT("The real main animation graph and foot post-process restore after the stability fixture"), IK)) return false;
			TestTrue(TEXT("Frozen-input checks restore the original main animation class and mode"),
				Mesh->GetAnimClass() == MainAnimationClass && Mesh->GetAnimationMode() == MainAnimationMode);
			if (!StepFootPose(Fixture, *Character, *this)) return false;
		}
		if (auto* Player = Cast<AfpstrueCharacter>(Character))
		{
			// 仅在进程内固定一个真实转向快照，检查后处理不反写原图已计算的 RootYawOffset。
			Player->SetActorRotation(FRotator(0.0f, 30.0f, 0.0f));
			for (int32 Frame = 0; Frame < 12; ++Frame) if (!StepFootPose(Fixture, *Player, *this)) return false;
			const auto* Main = Cast<UfpstruePlayerAnimInstance>(Mesh->GetAnimInstance());
			TestTrue(TEXT("Foot IK preserves the main body's nonzero planted-foot root compensation during a small look turn"),
				Main && FMath::IsNearlyEqual(Main->RootYawOffset, -30.0f, 0.1f)
				&& FMath::Abs(FMath::FindDeltaAngleDegrees(OriginalRootYaw,
					Mesh->GetSocketTransform(TEXT("root"), RTS_World).Rotator().Yaw)) < 0.1f);
		}
		// 主图仍真实推进；直接比较引擎缓存的同帧后处理输入与最终姿态，不靠二次 Tick 或改作者动画伪造原姿态。
		if (Step) Step->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Floor->SetWorldRotation(FRotator::ZeroRotator); Floor->SetWorldLocation(FVector(0, 0, -Floor->GetUnscaledBoxExtent().Z));
		const auto CheckDynamicPose = [&](bool bTurn)
		{
			Character->GetCharacterMovement()->Velocity = bTurn ? FVector::ZeroVector : Character->GetActorForwardVector() * 300.0f;
			if (bTurn)
			{
				if (!StepFootPose(Fixture, *Character, *this)) return false;
				Character->SetActorRotation(FRotator(0, Character->GetActorRotation().Yaw + 75.0f, 0));
			}
			IK->ResetFootPlacement();
			int32 MotionFrames = 0, LockedFrames = 0;
			double MaximumXYError = 0.0, MaximumRotationError = 0.0, MinimumPelvisOffset = 0.0;
			double MaximumLegError = 0.0, MaximumRootYawError = 0.0, MinimumSoleGap = TNumericLimits<double>::Max();
			double MaximumRawFootTravel = 0.0;
			FVector FirstRawFeet[2];
			for (int32 Frame = 0; Frame < (bTurn ? 90 : 60); ++Frame)
			{
				if (!StepFootPose(Fixture, *Character, *this)) return false;
				const auto* Main = Cast<UfpstruePlayerAnimInstance>(Mesh->GetAnimInstance());
				if (bTurn && (!Main || Main->TurnAlpha <= 0.05f)) continue;
				FAnimNode_LinkedInputPose* Input = IK->GetLinkedInputPoseNode();
				if (!TestTrue(TEXT("Dynamic grounding compares the actual initialized same-frame main-graph input pose"),
					Input && Input->bIsCachedInputPoseInitialized && Input->CachedInputPose.IsValid())) return false;
				FCSPose<FCompactHeapPose> RawPose; RawPose.InitPose(Input->CachedInputPose);
				const FBoneContainer& Bones = Input->CachedInputPose.GetBoneContainer();
				const auto RawWorld = [&](const TCHAR* Bone)
				{
					const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Bones.GetReferenceSkeleton().FindBoneIndex(Bone)));
					return RawPose.GetComponentSpaceTransform(Index) * Mesh->GetComponentTransform();
				};
				for (int32 Side = 0; Side < 2; ++Side)
				{
					const TCHAR* FootName = Side == 0 ? TEXT("foot_l") : TEXT("foot_r");
					const FTransform RawFoot = RawWorld(FootName), FinalFoot = Mesh->GetSocketTransform(FootName, RTS_World);
					if (MotionFrames == 0) FirstRawFeet[Side] = RawFoot.GetLocation();
					MaximumRawFootTravel = FMath::Max(MaximumRawFootTravel, FVector::Dist(FirstRawFeet[Side], RawFoot.GetLocation()));
					MaximumXYError = FMath::Max(MaximumXYError, FVector::Dist2D(RawFoot.GetLocation(), FinalFoot.GetLocation()));
					MaximumRotationError = FMath::Max(MaximumRotationError, FMath::RadiansToDegrees(RawFoot.GetRotation().AngularDistance(FinalFoot.GetRotation())));
					const FVector RawThigh = RawWorld(Side == 0 ? TEXT("thigh_l") : TEXT("thigh_r")).GetLocation();
					const FVector RawCalf = RawWorld(Side == 0 ? TEXT("calf_l") : TEXT("calf_r")).GetLocation();
					const FVector RawLengths(FVector::Dist(RawThigh, RawCalf), FVector::Dist(RawCalf, RawFoot.GetLocation()), 0);
					MaximumLegError = FMath::Max(MaximumLegError, (LegLengths(*Mesh, Side == 0 ? TEXT("l") : TEXT("r")) - RawLengths).GetAbsMax());
				}
				MinimumPelvisOffset = FMath::Min(MinimumPelvisOffset,
					Mesh->GetSocketLocation(TEXT("pelvis")).Z - RawWorld(TEXT("pelvis")).GetLocation().Z);
				MaximumRootYawError = FMath::Max(MaximumRootYawError, double(FMath::Abs(FMath::FindDeltaAngleDegrees(
					RawWorld(TEXT("root")).Rotator().Yaw, Mesh->GetSocketTransform(TEXT("root"), RTS_World).Rotator().Yaw))));
				const auto Debug = IK->GetFootIKDebug();
				LockedFrames += Debug.bLeftPlanted || Debug.bRightPlanted;
				FFootSoleSample Left, Right;
				if (!ReadFootSoles(*Character, FVector::ZeroVector, FVector::UpVector, Left, Right, *this)) return false;
				MinimumSoleGap = FMath::Min(MinimumSoleGap, FMath::Min(Left.Gap, Right.Gap));
				++MotionFrames;
			}
			AddInfo(FString::Printf(TEXT("Foot IK authored-motion fixture player turn=%d frames=%d rawFootTravel=%.4fcm lockedFrames=%d ankleXYError=%.4fcm footRotationError=%.4fdeg pelvisMinimum=%.4fcm soleMinimum=%.4fcm legLengthError=%.4fcm rootYawError=%.4fdeg; real main graph, same-frame pre-IK input, stationary actor collision, not rendered movement."),
				bTurn, MotionFrames, MaximumRawFootTravel, LockedFrames, MaximumXYError, MaximumRotationError,
				MinimumPelvisOffset, MinimumSoleGap, MaximumLegError, MaximumRootYawError));
			TestTrue(TEXT("Authored-motion coverage contains actual walking or turning foot motion"), MotionFrames >= 10 && MaximumRawFootTravel > 2.0);
			TestTrue(TEXT("Walking and turning never retain Idle XY plant locks or displace authored ankle XY"), LockedFrames == 0 && MaximumXYError < 0.1);
			TestTrue(TEXT("Flat-ground walking and turning preserve authored foot roll instead of flattening the feet"), MaximumRotationError < 0.1);
			TestTrue(TEXT("Non-Idle grounding never sinks the pelvis beyond its six-centimeter motion budget"), MinimumPelvisOffset >= -6.01);
			TestTrue(TEXT("Real swinging shoe soles retain the original non-penetration floor without forcing them down"), MinimumSoleGap >= -2.0);
			TestTrue(TEXT("Motion grounding preserves same-frame authored leg lengths and root facing"), MaximumLegError < 0.1 && MaximumRootYawError < 0.1);
			TestTrue(TEXT("Dynamic pose checks never move the actor's CMC collision"), Character->GetActorLocation().Equals(OriginalActor.GetLocation(), 0.001));
			return true;
		};
		if (!CheckDynamicPose(false) || !CheckDynamicPose(true)) return false;
	}
	return true;
}

// 验证离地、死亡及实例重置后清除旧贴地状态，不把上一支撑面的缓存带入新状态。
bool FFpstrueFootIKResetTest::RunTest(const FString&)
{
	{
		FTestWorldWrapper Fixture;
		if (!BeginFootWorld(Fixture, *this)) return false;
		UBoxComponent* Floor = SpawnFootFloor(*Fixture.GetTestWorld());
		if (!TestNotNull(TEXT("IK reset fixture owns its independent collision floor"), Floor)) return false;
		UClass* Class = LoadClass<AfpstrueCharacter>(nullptr, PlayerClassPath);
		if (!TestNotNull(TEXT("IK reset fixture resolves the actual character class"), Class)) return false;
		const float HalfHeight = Class->GetDefaultObject<ACharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		ACharacter* Character = SpawnFootCharacter(*Fixture.GetTestWorld(), false, FVector(0, 0, HalfHeight + 2.15f), *this);
		if (!Character) return false;
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		auto* IK = Cast<UfpstrueFootIKAnimInstance>(Mesh->GetPostProcessInstance());
		if (!TestNotNull(TEXT("IK reset fixture has its configured independent post-process instance"), IK)) return false;
		for (int32 Frame = 0; Frame < 45; ++Frame) if (!StepFootPose(Fixture, *Character, *this)) return false;
		const auto Grounded = IK->GetFootIKDebug();
		TestTrue(TEXT("The reset fixture begins with an enabled independently evaluated foot solver"), Grounded.bEnabled && Grounded.TraceCount > 0);
		TestFalse(TEXT("Grounded player post-process requires Game Thread evaluation"), IK->CanRunParallelWork());
		Character->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		TestFalse(TEXT("Leaving ground cannot open a parallel-evaluation eligibility window"), IK->CanRunParallelWork());
		for (int32 Frame = 0; Frame < 12; ++Frame) if (!StepFootPose(Fixture, *Character, *this)) return false;
		const auto Airborne = IK->GetFootIKDebug();
		TestTrue(TEXT("Airborne motion releases both planted feet and issues no additional ground traces"),
			!Airborne.bEnabled && !Airborne.bLeftPlanted && !Airborne.bRightPlanted && Airborne.TraceCount == Grounded.TraceCount);
		TestFalse(TEXT("An airborne solver stays synchronous even while its world queries are inactive"), IK->CanRunParallelWork());
		Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		TestFalse(TEXT("Landing is safe before the next PreUpdate notices the restored ground eligibility"), IK->CanRunParallelWork());
		for (int32 Frame = 0; Frame < 45; ++Frame) if (!StepFootPose(Fixture, *Character, *this)) return false;
		const auto Landed = IK->GetFootIKDebug();
		TestTrue(TEXT("Landing re-establishes independent grounding rather than preserving an airborne disable flag"),
			Landed.bEnabled && Landed.TraceCount > Airborne.TraceCount);
		TestFalse(TEXT("The resumed grounded solver still rejects Worker evaluation"), IK->CanRunParallelWork());
		const uint32 BeforeDeathTraces = IK->GetFootIKDebug().TraceCount;
		UGameplayStatics::ApplyDamage(Character, 10000.0f, nullptr, nullptr, nullptr);
		for (int32 Frame = 0; Frame < 12; ++Frame) if (!StepFootPose(Fixture, *Character, *this)) return false;
		const auto Dead = IK->GetFootIKDebug();
		TestTrue(TEXT("The normal death pipeline releases the feet and never adds dead-body grounding traces"),
			!Dead.bEnabled && !Dead.bLeftPlanted && !Dead.bRightPlanted && Dead.TraceCount == BeforeDeathTraces);
		TestFalse(TEXT("Dead-body query suppression does not change the player instance's thread contract"), IK->CanRunParallelWork());
		AddInfo(FString::Printf(TEXT("Foot IK eligibility fixture player traces grounded=%u air=%u landed=%u dead=%u; actual player world-query solver; not rendered/network acceptance."),
			Grounded.TraceCount, Airborne.TraceCount, Landed.TraceCount, Dead.TraceCount));
	}
	return true;
}

#endif
