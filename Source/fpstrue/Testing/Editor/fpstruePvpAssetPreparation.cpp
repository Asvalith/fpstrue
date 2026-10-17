// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "Weapons/fpstrueWeaponComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/DefaultValueHelper.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/MetaData.h"
#include "UObject/UnrealType.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimGraphNode_ApplyMeshSpaceAdditive.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet/KismetMathLibrary.h"

namespace FpstruePvpAssetPreparation
{
constexpr const TCHAR* MapPackage = TEXT("/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked");
constexpr const TCHAR* WeaponObject = TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon.BP_Weapon");
constexpr const TCHAR* GateMarker = TEXT("PvP: preserve the original menu chain only in Standalone");

// Only this fixed pair of shipped assets is in scope. Check mode never edits or compiles graphs.
bool Fail(const FString& Reason)
{
	UE_LOG(LogTemp, Error, TEXT("PVP_ASSET_PREP FAIL %s"), *Reason);
	return false;
}

// Added nodes receive their own positions/comments; no authored node or comment is deleted.
template <typename TNode>
TNode* AddNode(UEdGraph* Graph, int32 X, int32 Y)
{
	TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transactional);
	Node->CreateNewGuid();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	Graph->AddNode(Node, false, false);
	return Node;
}

// A genuine already-connected Standalone guard is idempotent, not merely a matching comment.
bool HasStandaloneGuard(const UEdGraphPin* BeginOutput)
{
	if (BeginOutput == nullptr || BeginOutput->LinkedTo.Num() != 1) return false;
	const UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(BeginOutput->LinkedTo[0]->GetOwningNode());
	if (Branch == nullptr || Branch->GetElsePin()->LinkedTo.Num() != 0) return false;
	const UEdGraphPin* Condition = Branch->GetConditionPin();
	if (Condition->LinkedTo.Num() != 1) return false;
	const UK2Node_CallFunction* Check = Cast<UK2Node_CallFunction>(Condition->LinkedTo[0]->GetOwningNode());
	return Check != nullptr && Check->FunctionReference.GetMemberName() == TEXT("IsStandalone")
		&& Check->GetTargetFunction() == UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsStandalone"));
}

// Preserve the on-disk originals in a unique run directory; existing backups are never overwritten.
bool BackupAsset(const FString& Filename, const FString& BackupDirectory)
{
	if (!IFileManager::Get().FileExists(*Filename)) return Fail(TEXT("Missing asset file: ") + Filename);
	const FString Relative = FPaths::GetPathLeaf(FPaths::GetPath(Filename)) / FPaths::GetCleanFilename(Filename);
	const FString Destination = BackupDirectory / Relative;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
	if (IFileManager::Get().Copy(*Destination, *Filename, false, false) != COPY_OK)
		return Fail(TEXT("Could not create non-overwriting backup: ") + Destination);
	UE_LOG(LogTemp, Display, TEXT("PVP_ASSET_PREP BACKUP %s"), *Destination);
	return true;
}

// The execution edge alone is rerouted. The entire existing single-player chain remains intact.
bool GuardBeginPlay(UK2Node_Event* Begin)
{
	UEdGraph* Graph = Begin->GetGraph();
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (Schema == nullptr) return Fail(TEXT("Level graph does not use the K2 schema."));
	UEdGraphPin* BeginOutput = Begin->FindPin(UEdGraphSchema_K2::PN_Then);
	const TArray<UEdGraphPin*> OldTargets = BeginOutput->LinkedTo;
	Graph->Modify();
	Begin->Modify();
	UK2Node_IfThenElse* Branch = AddNode<UK2Node_IfThenElse>(Graph, Begin->NodePosX + 240, Begin->NodePosY);
	Branch->NodeComment = GateMarker;
	Branch->AllocateDefaultPins();
	UK2Node_CallFunction* Check = AddNode<UK2Node_CallFunction>(Graph, Begin->NodePosX, Begin->NodePosY + 180);
	Check->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("IsStandalone")));
	Check->AllocateDefaultPins();
	UK2Node_Self* Self = AddNode<UK2Node_Self>(Graph, Begin->NodePosX - 180, Begin->NodePosY + 240);
	Self->AllocateDefaultPins();
	UEdGraphPin* Context = Check->FindPin(TEXT("WorldContextObject"));
	UEdGraphPin* SelfOutput = Self->FindPin(UEdGraphSchema_K2::PN_Self);
	if (Context == nullptr || SelfOutput == nullptr
		|| !Schema->TryCreateConnection(SelfOutput, Context)
		|| !Schema->TryCreateConnection(Check->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Branch->GetConditionPin()))
		return Fail(TEXT("Could not connect the explicit level-script WorldContext."));
	BeginOutput->BreakAllPinLinks();
	if (!Schema->TryCreateConnection(BeginOutput, Branch->GetExecPin())) return Fail(TEXT("Could not insert BeginPlay guard."));
	for (UEdGraphPin* Target : OldTargets)
	{
		Target->GetOwningNode()->Modify();
		if (!Schema->TryCreateConnection(Branch->GetThenPin(), Target)) return Fail(TEXT("Could not preserve original BeginPlay successor."));
	}
	return true;
}

// Reads use the replicated holder through the component; the old pickup setter remains untouched.
bool ReplaceOwnerRead(UK2Node_VariableGet* Read, FName ComponentName, UFunction* Getter)
{
	UEdGraph* Graph = Read->GetGraph();
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (Schema == nullptr) return Fail(TEXT("Weapon graph does not use the K2 schema."));
	Graph->Modify();
	Read->Modify();
	UK2Node_VariableGet* Component = AddNode<UK2Node_VariableGet>(Graph, Read->NodePosX - 220, Read->NodePosY + 100);
	Component->VariableReference.SetSelfMember(ComponentName);
	Component->AllocateDefaultPins();
	UK2Node_CallFunction* Query = AddNode<UK2Node_CallFunction>(Graph, Read->NodePosX, Read->NodePosY + 100);
	Query->SetFromFunction(Getter);
	Query->AllocateDefaultPins();
	UEdGraphPin* ComponentOutput = Component->FindPin(ComponentName);
	UEdGraphPin* QuerySelf = Query->FindPin(UEdGraphSchema_K2::PN_Self);
	UEdGraphPin* Output = Query->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
	UEdGraphPin* OldOutput = Read->FindPin(Read->GetVarName());
	if (ComponentOutput == nullptr || QuerySelf == nullptr || Output == nullptr || OldOutput == nullptr
		|| !Schema->TryCreateConnection(ComponentOutput, QuerySelf))
		return Fail(TEXT("Could not connect the weapon component holder getter."));
	const TArray<UEdGraphPin*> OldTargets = OldOutput->LinkedTo;
	for (UEdGraphPin* Target : OldTargets)
	{
		if (!Schema->ArePinTypesCompatible(Output->PinType, Target->PinType))
			return Fail(TEXT("Holder getter has an incompatible authored target pin."));
	}
	for (UEdGraphPin* Target : OldTargets)
	{
		Target->GetOwningNode()->Modify();
		OldOutput->BreakLinkTo(Target);
		if (!Schema->TryCreateConnection(Output, Target)) return Fail(TEXT("Could not preserve an owningactor consumer."));
	}
	return true;
}

// Compile both edited blueprints before publishing either package. A compile error never saves a partial graph.
bool Compile(UBlueprint* Blueprint)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FCompilerResultsLog Results;
	Results.bSilentMode = false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	UE_LOG(LogTemp, Display, TEXT("PVP_ASSET_PREP COMPILE Asset=%s Errors=%d Warnings=%d"),
		*Blueprint->GetPathName(), Results.NumErrors, Results.NumWarnings);
	return Results.NumErrors == 0 && Blueprint->Status != BS_Error;
}

// Console registration happens when the editor module loads, before editor -ExecCmds are consumed.
void Run(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if (Args.Num() > 0 && !bApply) { Fail(TEXT("Usage: fp.PreparePvpAssets [Apply]; no argument is read-only.")); return; }
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr) { Fail(TEXT("Requires an editor world with no active PIE.")); return; }
	UWorld* PreviousWorld = GEditor->GetEditorWorldContext().World();
	if (PreviousWorld != nullptr && PreviousWorld->GetOutermost()->IsDirty())
	{
		Fail(TEXT("Current editor map is dirty. Save it before loading the fixed audit target."));
		return;
	}
	const FString MapFilename = FPackageName::LongPackageNameToFilename(MapPackage, FPackageName::GetMapPackageExtension());
	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(MapFilename);
	if (World == nullptr || World->PersistentLevel == nullptr || World->GetOutermost()->GetName() != MapPackage)
	{
		Fail(TEXT("Could not load the exact target map."));
		return;
	}
	if (World->GetWorldPartition() != nullptr || World->PersistentLevel->IsUsingExternalActors()
		|| World->PersistentLevel->IsUsingExternalObjects())
	{
		Fail(TEXT("External/partitioned map is outside this single-package repair tool."));
		return;
	}
	ULevelScriptBlueprint* LevelBlueprint = World->PersistentLevel->GetLevelScriptBlueprint(true);
	TStrongObjectPtr<UBlueprint> Weapon(LoadObject<UBlueprint>(nullptr, WeaponObject));
	if (LevelBlueprint == nullptr || !Weapon.IsValid() || Weapon->SimpleConstructionScript == nullptr)
	{
		Fail(TEXT("Target level script or weapon construction script is missing."));
		return;
	}
	if (Weapon->GetOutermost()->IsDirty()) { Fail(TEXT("Weapon package has unsaved edits; refusing to publish them incidentally.")); return; }
	UK2Node_Event* Begin = nullptr;
	TArray<UEdGraph*> Graphs;
	LevelBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event == nullptr || Event->EventReference.GetMemberName() != TEXT("ReceiveBeginPlay")) continue;
			if (Begin != nullptr) { Fail(TEXT("Multiple BeginPlay nodes require manual inspection.")); return; }
			Begin = Event;
		}
	}
	UEdGraphPin* BeginOutput = Begin != nullptr ? Begin->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr;
	if (BeginOutput == nullptr || BeginOutput->LinkedTo.Num() != 1)
	{
		Fail(TEXT("Expected exactly one existing level BeginPlay successor; refusing a custom layout."));
		return;
	}
	const bool bNeedsGate = !HasStandaloneGuard(BeginOutput);
	if (bNeedsGate)
	{
		for (UEdGraph* Graph : Graphs)
			for (UEdGraphNode* Node : Graph->Nodes)
				if (Node->NodeComment == GateMarker) { Fail(TEXT("An incomplete prior guard requires manual inspection.")); return; }
	}
	FName ComponentName = NAME_None;
	for (USCS_Node* Node : Weapon->SimpleConstructionScript->GetAllNodes())
	{
		if (Node == nullptr || Node->ComponentClass == nullptr || !Node->ComponentClass->IsChildOf(UfpstrueWeaponComponent::StaticClass())) continue;
		if (!ComponentName.IsNone()) { Fail(TEXT("Multiple weapon components require an explicit asset decision.")); return; }
		ComponentName = Node->GetVariableName();
	}
	if (ComponentName.IsNone()) { Fail(TEXT("Weapon component SCS variable was not found.")); return; }
	UFunction* Getter = UfpstrueWeaponComponent::StaticClass()->FindFunctionByName(TEXT("GetOwningCharacter"));
	if (Getter == nullptr) { Fail(TEXT("Native GetOwningCharacter must be available before applying the graph repair.")); return; }
	TArray<UK2Node_VariableGet*> Reads;
	Graphs.Reset();
	Weapon->GetAllGraphs(Graphs);
	int32 RewiredLinks = 0;
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_VariableGet* Read = Cast<UK2Node_VariableGet>(Node);
			if (Read == nullptr || Read->GetVarName() != TEXT("owningactor")) continue;
			UEdGraphPin* Output = Read->FindPin(Read->GetVarName());
			if (Output == nullptr || Output->LinkedTo.IsEmpty()) continue;
			UEdGraphPin* Self = Read->FindPin(UEdGraphSchema_K2::PN_Self);
			if (!Read->IsNodePure() || (Self != nullptr && (!Self->LinkedTo.IsEmpty() || Self->DefaultObject != nullptr)))
			{
				Fail(TEXT("Non-pure or external owningactor read requires manual inspection."));
				return;
			}
			Reads.Add(Read);
			RewiredLinks += Output->LinkedTo.Num();
			UE_LOG(LogTemp, Display, TEXT("PVP_ASSET_PREP READ Graph=%s Node=%s Consumers=%d"), *Graph->GetName(), *Read->GetName(), Output->LinkedTo.Num());
		}
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_ASSET_PREP AUDIT Mode=%s Map=%s Monolithic=true NeedsGuard=%s WeaponComponent=%s OwnerReads=%d ConsumerLinks=%d"),
		bApply ? TEXT("Apply") : TEXT("Check"), MapPackage, bNeedsGate ? TEXT("true") : TEXT("false"), *ComponentName.ToString(), Reads.Num(), RewiredLinks);
	if (!bApply || (!bNeedsGate && Reads.IsEmpty()))
	{
		UE_LOG(LogTemp, Display, TEXT("PVP_ASSET_PREP PASS %s"), bApply ? TEXT("Already prepared; no assets changed.") : TEXT("Read-only audit complete; no assets changed."));
		return;
	}
	const FString BackupDirectory = FPaths::ProjectSavedDir() / TEXT("Backups/PvpAssetPreparation")
		/ (FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString WeaponFilename = FPackageName::LongPackageNameToFilename(Weapon->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	if ((bNeedsGate && !BackupAsset(MapFilename, BackupDirectory)) || (!Reads.IsEmpty() && !BackupAsset(WeaponFilename, BackupDirectory))) return;
	if (bNeedsGate && !GuardBeginPlay(Begin)) return;
	for (UK2Node_VariableGet* Read : Reads) if (!ReplaceOwnerRead(Read, ComponentName, Getter)) return;
	if ((bNeedsGate && !Compile(LevelBlueprint)) || (!Reads.IsEmpty() && !Compile(Weapon.Get())))
	{
		Fail(TEXT("Compilation failed; edited graphs remain unsaved. Originals are in ") + BackupDirectory);
		return;
	}
	if (bNeedsGate && !UEditorLoadingAndSavingUtils::SaveMap(World, MapPackage)) { Fail(TEXT("Could not save the target map; backup retained.")); return; }
	if (!Reads.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages({Weapon->GetOutermost()}, false))
	{
		Fail(TEXT("Could not save the target weapon package; backup retained."));
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("PVP_ASSET_PREP PASS GuardAdded=%s OwnerReadsRewired=%d Backups=%s"),
		bNeedsGate ? TEXT("true") : TEXT("false"), Reads.Num(), *BackupDirectory);
}

FAutoConsoleCommand PreparePvpAssets(
	TEXT("fp.PreparePvpAssets"),
	TEXT("Audit the shipped PvP map/weapon graphs. Add Apply to back up, minimally rewire, compile and save only those assets."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Run));

// 只读输出 ShouldMove 的上游值流；节点与引脚逐一标明，检查不会 Modify、编译或保存蓝图。
void AuditRARBodyNode(const UEdGraphNode* Node, TSet<const UEdGraphNode*>& Visited, int32 Depth = 0)
{
	if (Node == nullptr || Depth > 8 || Visited.Contains(Node)) return;
	Visited.Add(Node);
	UE_LOG(LogTemp, Display, TEXT("RAR_BODY NODE Depth=%d Graph=%s Node=%s Class=%s Title=%s Comment=%s"),
		Depth, *GetNameSafe(Node->GetGraph()), *Node->GetName(), *Node->GetClass()->GetName(),
		*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Replace(TEXT("\n"), TEXT(" ")),
		*Node->NodeComment.Replace(TEXT("\n"), TEXT(" ")));
	if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		UE_LOG(LogTemp, Display, TEXT("RAR_BODY FUNCTION Node=%s Name=%s"),
			*Node->GetName(), *Call->FunctionReference.GetMemberName().ToString());
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin == nullptr) continue;
		FString Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked != nullptr && Linked->GetOwningNode() != nullptr)
				Links += FString::Printf(TEXT("%s%s.%s"), Links.IsEmpty() ? TEXT("") : TEXT(";"),
					*Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString());
		}
		UE_LOG(LogTemp, Display, TEXT("RAR_BODY PIN Node=%s Pin=%s Direction=%s Type=%s Default=%s Links=%s"),
			*Node->GetName(), *Pin->PinName.ToString(), Pin->Direction == EGPD_Input ? TEXT("In") : TEXT("Out"),
			*Pin->PinType.PinCategory.ToString(), *Pin->DefaultValue, *Links);
		if (Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
			if (Linked != nullptr) AuditRARBodyNode(Linked->GetOwningNode(), Visited, Depth + 1);
	}
}

// 只接受作者现有 GroundSpeed > 非负常量的比较，不构造新的动画条件或猜测变量名。
UEdGraphPin* GetRARBodySpeedComparison(UEdGraphPin* Output)
{
	UK2Node_CallFunction* Compare = Output != nullptr ? Cast<UK2Node_CallFunction>(Output->GetOwningNode()) : nullptr;
	if (Compare == nullptr || Output->PinName != UEdGraphSchema_K2::PN_ReturnValue) return nullptr;
	const FName Function = Compare->FunctionReference.GetMemberName();
	if (Function != TEXT("Greater_DoubleDouble") && Function != TEXT("Greater_FloatFloat")) return nullptr;
	UEdGraphPin* A = Compare->FindPin(TEXT("A"));
	UEdGraphPin* B = Compare->FindPin(TEXT("B"));
	if (A == nullptr || B == nullptr || A->LinkedTo.Num() != 1 || !B->LinkedTo.IsEmpty()) return nullptr;
	const UK2Node_VariableGet* Speed = Cast<UK2Node_VariableGet>(A->LinkedTo[0]->GetOwningNode());
	double Threshold = 0.0;
	if (Speed == nullptr || Speed->GetVarName() != TEXT("GroundSpeed")
		|| !LexTryParseString(Threshold, *B->DefaultValue) || !FMath::IsFinite(Threshold) || Threshold < 0.0) return nullptr;
	return Output;
}

// 仅修复新导入的通用身体 AnimBP。原第一人称、武器、HUD、GAS 和网络规则不在此工具范围内。
void PrepareRARBody(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((Args.Num() > 0 && !bApply) || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Usage: fp.PrepareRARBody [Apply]; requires no active PIE."));
		return;
	}
	TStrongObjectPtr<UAnimBlueprint> Blueprint(LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/InfimaGames/ArtCore/Mannequin/Animations/ABP_Manny.ABP_Manny")));
	TStrongObjectPtr<USkeletalMesh> Mesh(LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
	if (!Blueprint.IsValid() || !Mesh.IsValid() || Blueprint->TargetSkeleton == nullptr
		|| Blueprint->TargetSkeleton != Mesh->GetSkeleton())
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Imported AnimBP/mesh is missing or TargetSkeleton does not match."));
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("RAR_BODY ASSETS Mode=%s Blueprint=%s Mesh=%s Skeleton=%s Dirty=%d"),
		bApply ? TEXT("Apply") : TEXT("Check"), *Blueprint->GetPathName(), *Mesh->GetPathName(),
		*GetPathNameSafe(Mesh->GetSkeleton()), Blueprint->GetOutermost()->IsDirty());
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	UK2Node_VariableSet* ShouldMove = nullptr;
	bool bGenericCharacter = false;
	bool bGenericMovement = false;
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node); Set != nullptr && Set->GetVarName() == TEXT("ShouldMove"))
			{
				if (ShouldMove != nullptr) { UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Multiple ShouldMove writers; no edit.")); return; }
				ShouldMove = Set;
			}
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin == nullptr) continue;
				const UClass* Class = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
				if (Class == nullptr) continue;
				if (Class == ACharacter::StaticClass()) bGenericCharacter = true;
				else if (Class->IsChildOf(ACharacter::StaticClass()))
				{
					UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Requires custom Character class %s; no edit."), *Class->GetPathName());
					return;
				}
				if (Class == UCharacterMovementComponent::StaticClass()) bGenericMovement = true;
			}
		}
	}
	if (ShouldMove == nullptr || !bGenericCharacter || !bGenericMovement
		|| !Blueprint->UbergraphPages.Contains(ShouldMove->GetGraph()))
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Expected one ShouldMove writer and generic Character/Movement references. Character=%d Movement=%d"), bGenericCharacter, bGenericMovement);
		return;
	}
	TSet<const UEdGraphNode*> Upstream;
	AuditRARBodyNode(ShouldMove, Upstream);
	UEdGraphPin* Value = ShouldMove->FindPin(TEXT("ShouldMove"));
	if (Value == nullptr || Value->Direction != EGPD_Input || Value->LinkedTo.Num() != 1)
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL ShouldMove value has unexpected wiring; no edit."));
		return;
	}
	if (GetRARBodySpeedComparison(Value->LinkedTo[0]) != nullptr)
	{
		UE_LOG(LogTemp, Display, TEXT("RAR_BODY PASS Already uses GroundSpeed threshold only; no assets changed."));
		return;
	}
	UK2Node_CallFunction* And = Cast<UK2Node_CallFunction>(Value->LinkedTo[0]->GetOwningNode());
	UEdGraphPin* SpeedOutput = nullptr;
	UEdGraphPin* AccelerationOutput = nullptr;
	if (And != nullptr && And->FunctionReference.GetMemberName() == TEXT("BooleanAND"))
	{
		for (UEdGraphPin* Input : And->Pins)
		{
			if (Input == nullptr || Input->Direction != EGPD_Input || Input->PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean) continue;
			if (Input->LinkedTo.Num() != 1) { SpeedOutput = nullptr; break; }
			if (UEdGraphPin* Candidate = GetRARBodySpeedComparison(Input->LinkedTo[0]))
			{
				if (SpeedOutput != nullptr) { SpeedOutput = nullptr; break; }
				SpeedOutput = Candidate;
			}
			else if (AccelerationOutput == nullptr) AccelerationOutput = Input->LinkedTo[0];
			else { SpeedOutput = nullptr; break; }
		}
	}
	TSet<const UEdGraphNode*> AccelerationChain;
	AuditRARBodyNode(AccelerationOutput != nullptr ? AccelerationOutput->GetOwningNode() : nullptr, AccelerationChain);
	const UK2Node_CallFunction* AccelerationCompare = AccelerationOutput != nullptr
		? Cast<UK2Node_CallFunction>(AccelerationOutput->GetOwningNode()) : nullptr;
	const UEdGraphPin* AccelerationValue = AccelerationCompare != nullptr ? AccelerationCompare->FindPin(TEXT("A")) : nullptr;
	const UEdGraphPin* ZeroValue = AccelerationCompare != nullptr ? AccelerationCompare->FindPin(TEXT("B")) : nullptr;
	const UK2Node_CallFunction* AccelerationRead = AccelerationValue != nullptr && AccelerationValue->LinkedTo.Num() == 1
		? Cast<UK2Node_CallFunction>(AccelerationValue->LinkedTo[0]->GetOwningNode()) : nullptr;
	FVector ExpectedZero;
	const bool bExpectedAccelerationGate = AccelerationCompare != nullptr
		&& AccelerationCompare->FunctionReference.GetMemberName() == TEXT("NotEqual_VectorVector")
		&& AccelerationRead != nullptr && AccelerationRead->FunctionReference.GetMemberName() == TEXT("GetCurrentAcceleration")
		&& ZeroValue != nullptr && ZeroValue->LinkedTo.IsEmpty()
		// UE5 的未连接 Vector 常量引脚允许空字符串，编译时按零向量构造；显式非零值仍拒绝。
		&& (ZeroValue->DefaultValue.IsEmpty()
			|| (FDefaultValueHelper::ParseVector(ZeroValue->DefaultValue, ExpectedZero) && ExpectedZero.IsZero()));
	if (SpeedOutput == nullptr || AccelerationOutput == nullptr || !bExpectedAccelerationGate
		|| And->GetGraph() != ShouldMove->GetGraph() || SpeedOutput->GetOwningNode()->GetGraph() != ShouldMove->GetGraph())
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Expected GroundSpeed threshold AND current acceleration; no edit."));
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("RAR_BODY VERIFIED GenericCharacter=true GenericMovement=true SkeletonMatch=true ShouldMove=%s Speed=%s Acceleration=%s"),
		*ShouldMove->GetName(), *SpeedOutput->GetOwningNode()->GetName(), *AccelerationOutput->GetOwningNode()->GetName());
	if (!bApply) { UE_LOG(LogTemp, Display, TEXT("RAR_BODY PASS Read-only audit complete; no assets changed.")); return; }
	if (Blueprint->GetOutermost()->IsDirty()) { UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL AnimBP has unsaved changes; no edit.")); return; }
	const FString BackupDirectory = FPaths::ProjectSavedDir() / TEXT("Backups/RARBodyPreparation")
		/ (FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Filename = FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	if (!BackupAsset(Filename, BackupDirectory)) return;
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(ShouldMove->GetGraph()->GetSchema());
	if (Schema == nullptr || !Schema->ArePinTypesCompatible(SpeedOutput->PinType, Value->PinType))
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Speed pin is incompatible; no edit."));
		return;
	}
	ShouldMove->GetGraph()->Modify();
	ShouldMove->Modify();
	And->Modify();
	SpeedOutput->GetOwningNode()->Modify();
	Value->BreakAllPinLinks();
	if (!Schema->TryCreateConnection(SpeedOutput, Value))
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Could not rewire ShouldMove; edited graph remains unsaved. Backup=%s"), *BackupDirectory);
		return;
	}
	// 原节点和作者注释全部保留；仅当前赋值不再依赖远端代理没有的输入加速度。
	if (!Compile(Blueprint.Get())) { UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Compile failed; original disk asset retained. Backup=%s"), *BackupDirectory); return; }
	if (!UEditorLoadingAndSavingUtils::SavePackages({Blueprint->GetOutermost()}, false))
	{
		UE_LOG(LogTemp, Error, TEXT("RAR_BODY FAIL Save failed; backup retained at %s"), *BackupDirectory);
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("RAR_BODY PASS ShouldMove now follows the existing speed threshold. Only ABP_Manny saved; original nodes/comments retained. Backup=%s"), *BackupDirectory);
}

FAutoConsoleCommand PrepareRARBodyCommand(
	TEXT("fp.PrepareRARBody"),
	TEXT("Read-only audit of imported Manny ShouldMove. Add Apply to back up and remove only the input-acceleration gate, compile and save ABP_Manny."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&PrepareRARBody));
} // namespace FpstruePvpAssetPreparation

// 只生成本项目自己的表现资产。模板和旧玩家蓝图不被重设父类、不删除节点，也不承担 GAS 规则。
namespace FpstrueRARPresentation
{
const FString Root = TEXT("/Game/FirstPerson/Animations/RAR/");
const FString CharacterArt = TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Character/Animations/");
const FString WeaponArt = TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Animations/");
const FString MannyArt = TEXT("/Game/InfimaGames/ArtCore/Mannequin/Animations/Manny/");
const FString TppArt = TEXT("/Game/FPS/Animations/TPP/Locomotion/RootLocomotion/");
constexpr const TCHAR* PlayerPackage = TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter");

struct FClip
{
	FName Key;
	FString Source;
	bool bAdditive;
};

bool Error(const FString& Message)
{
	UE_LOG(LogTemp, Error, TEXT("RAR_PRESENTATION FAIL %s"), *Message);
	return false;
}

// 所有资源路径先验证；空指针或叠加类型不符时不进入写入阶段。
template <typename T> T* Load(const FString& Package)
{
	return LoadObject<T>(nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package)));
}

bool SameReferencePose(const USkeletalMesh* OldMesh, const USkeletalMesh* NewMesh)
{
	const FReferenceSkeleton& A = OldMesh->GetRefSkeleton();
	const FReferenceSkeleton& B = NewMesh->GetRefSkeleton();
	if (A.GetRawBoneNum() != B.GetRawBoneNum()) return Error(TEXT("Manny raw bone counts differ."));
	for (int32 Index = 0; Index < A.GetRawBoneNum(); ++Index)
	{
		const FTransform& X = A.GetRawRefBonePose()[Index];
		const FTransform& Y = B.GetRawRefBonePose()[Index];
		if (A.GetBoneName(Index) != B.GetBoneName(Index) || A.GetParentIndex(Index) != B.GetParentIndex(Index)
			|| !X.GetTranslation().Equals(Y.GetTranslation(), 0.001)
			|| !X.GetScale3D().Equals(Y.GetScale3D(), 0.0001)
			|| FMath::RadiansToDegrees(X.GetRotation().AngularDistance(Y.GetRotation())) > 0.05)
			return Error(FString::Printf(TEXT("Manny raw reference pose differs at %s."), *A.GetBoneName(Index).ToString()));
	}
	return true;
}

// 新资产从不覆盖现有文件；骨架兼容与槽位修改在唯一备份后才执行。
template <typename T> T* NewAsset(const FString& Package, TArray<UPackage*>& Packages)
{
	UPackage* Outer = CreatePackage(*Package);
	T* Asset = NewObject<T>(Outer, *FPackageName::GetShortName(Package), RF_Public | RF_Standalone | RF_Transactional);
	Packages.AddUnique(Outer);
	FAssetRegistryModule::AssetCreated(Asset);
	Asset->MarkPackageDirty();
	return Asset;
}

// OptionalPin 必须在分配后显式显示；在任何连线之前重建，不破坏作者或已连好的引脚。
void Expose(UAnimGraphNode_Base* Node, FName Name)
{
	for (FOptionalPinFromProperty& Pin : Node->ShowPinForProperties)
		if (Pin.PropertyName == Name) Pin.bShowPin = true;
	Node->ReconstructNode();
}

UEdGraphPin* PoseOutput(UEdGraphNode* Node)
{
	for (UEdGraphPin* Pin : Node->Pins)
		if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			return Pin;
	return nullptr;
}

struct FGraphBuilder
{
	UEdGraph* Graph = nullptr;
	const UEdGraphSchema_K2* Schema = nullptr;
	bool bOK = true;
	int32 Serial = 0;
	template <typename T> T* Node(const FString& Comment)
	{
		T* Result = FpstruePvpAssetPreparation::AddNode<T>(Graph, (Serial % 6) * 320, (Serial / 6) * 250);
		++Serial;
		Result->NodeComment = Comment;
		// 正常编辑器放置节点会登记 CachedPose/Slot 等编译扩展，脚本生成也必须走同一生命周期。
		if constexpr (TIsDerivedFrom<T, UAnimGraphNode_Base>::IsDerived) Result->PostPlacedNewNode();
		return Result;
	}
	void Link(UEdGraphPin* Out, UEdGraphPin* In)
	{
		if (Out == nullptr || In == nullptr || !Schema->TryCreateConnection(Out, In))
		{
			bOK = false;
			Error(FString::Printf(TEXT("Cannot connect %s.%s to %s.%s."),
				Out != nullptr ? *GetNameSafe(Out->GetOwningNode()) : TEXT("null"), Out != nullptr ? *Out->PinName.ToString() : TEXT("null"),
				In != nullptr ? *GetNameSafe(In->GetOwningNode()) : TEXT("null"), In != nullptr ? *In->PinName.ToString() : TEXT("null")));
		}
	}
	void Pose(UEdGraphNode* Out, UEdGraphNode* In, FName Pin) { Link(PoseOutput(Out), In->FindPin(Pin)); }
	void Value(FName Property, UEdGraphNode* In, FName Pin)
	{
		auto* Read = Node<UK2Node_VariableGet>(TEXT("只读取 NativeUpdateAnimation 的本帧快照，不查询或修改玩法规则。"));
		Read->VariableReference.SetSelfMember(Property);
		Read->AllocateDefaultPins();
		Link(Read->FindPin(Property), In->FindPin(Pin));
	}
	UAnimGraphNode_SequencePlayer* Sequence(UAnimSequence* Asset, bool bLoop = true, bool bMovementRate = false)
	{
		auto* Player = Node<UAnimGraphNode_SequencePlayer>(TEXT("本项目安全序列副本；无模板业务 Notify，动作资格与结算仍归原 GAS/Weapon。"));
		Player->Node.SetSequence(Asset);
		Player->Node.SetLoopAnimation(bLoop);
		Player->AllocateDefaultPins();
		if (bMovementRate) { Expose(Player, TEXT("PlayRate")); Value(TEXT("LocomotionPlayRate"), Player, TEXT("PlayRate")); }
		return Player;
	}
	UAnimGraphNode_SequenceEvaluator* Hit(UAnimSequence* Asset)
	{
		auto* Evaluator = Node<UAnimGraphNode_SequenceEvaluator>(TEXT("每次受击由 Native PresentHit 将 HitTime 归零；按显式时间采样，连续受击不会停留在上次尾帧，也不触发 Notify 或提取 Root Motion。"));
		Evaluator->Node.SetSequence(Asset);
		Evaluator->Node.SetShouldLoop(false);
		Evaluator->Node.SetShouldUseExplicitFrame(false);
		Evaluator->Node.SetExplicitTime(0.0f);
		// UE5.5 常量折叠节点没有覆写基类的 Teleport setter，必须设置其编辑器反射属性。
		FBoolProperty* Teleport = FindFProperty<FBoolProperty>(FAnimNode_SequenceEvaluator::StaticStruct(), TEXT("bTeleportToExplicitTime"));
		if (Teleport == nullptr) { bOK = Error(TEXT("SequenceEvaluator teleport property is unavailable.")); return Evaluator; }
		Teleport->SetPropertyValue_InContainer(&Evaluator->Node, true);
		Evaluator->AllocateDefaultPins();
		Value(TEXT("HitTime"), Evaluator, TEXT("ExplicitTime"));
		return Evaluator;
	}
	UAnimGraphNode_TwoWayBlend* Blend(UEdGraphNode* A, UEdGraphNode* B, FName Alpha)
	{
		auto* Result = Node<UAnimGraphNode_TwoWayBlend>(TEXT("以原生派生权重混合表现，不建立第二套冲刺/瞄准状态。"));
		Result->AllocateDefaultPins();
		Pose(A, Result, TEXT("A")); Pose(B, Result, TEXT("B")); Value(Alpha, Result, TEXT("Alpha"));
		return Result;
	}
	UAnimGraphNode_BlendListByBool* Select(UEdGraphNode* TruePose, UEdGraphNode* FalsePose, FName ValueName)
	{
		auto* Result = Node<UAnimGraphNode_BlendListByBool>(TEXT("实际移动/落地状态选择；重新激活时重置非循环起落序列。"));
		if (FBoolProperty* Reset = FindFProperty<FBoolProperty>(FAnimNode_BlendListBase::StaticStruct(), TEXT("bResetChildOnActivation")))
			Reset->SetPropertyValue_InContainer(&Result->Node, true);
		Result->AllocateDefaultPins();
		Pose(TruePose, Result, TEXT("BlendPose_0")); Pose(FalsePose, Result, TEXT("BlendPose_1"));
		Value(ValueName, Result, TEXT("bActiveValue"));
		return Result;
	}
	UAnimGraphNode_ApplyAdditive* Add(UEdGraphNode* Base, UEdGraphNode* Delta, FName Alpha = NAME_None)
	{
		auto* Result = Node<UAnimGraphNode_ApplyAdditive>(TEXT("先建立非叠加基准姿态，再应用 LocalSpace 增量；不能把增量直接输出为完整姿态。"));
		Result->Node.Alpha = 1.0f;
		Result->AllocateDefaultPins();
		Pose(Base, Result, TEXT("Base")); Pose(Delta, Result, TEXT("Additive"));
		if (!Alpha.IsNone()) Value(Alpha, Result, TEXT("Alpha"));
		return Result;
	}
	UAnimGraphNode_ApplyMeshSpaceAdditive* AddMesh(UEdGraphNode* Base, UEdGraphNode* Delta, FName Alpha)
	{
		auto* Result = Node<UAnimGraphNode_ApplyMeshSpaceAdditive>(TEXT("受击资源为 MeshSpace Rotation 增量，使用匹配的叠加节点；不改源素材叠加类型，也不占换弹动作槽。"));
		Result->Node.Alpha = 1.0f;
		Result->AllocateDefaultPins();
		Pose(Base, Result, TEXT("Base")); Pose(Delta, Result, TEXT("Additive")); Value(Alpha, Result, TEXT("Alpha"));
		return Result;
	}
	UAnimGraphNode_Slot* Slot(UEdGraphNode* Source)
	{
		auto* Result = Node<UAnimGraphNode_Slot>(TEXT("RARAction 只承载本项目 Fire/Reload/Equip 表现；无模板状态或弹药提交。"));
		Result->Node.SlotName = TEXT("RARAction");
		Result->AllocateDefaultPins(); Pose(Source, Result, TEXT("Source"));
		return Result;
	}
};

UAnimBlueprint* AnimBlueprint(const FString& Name, UClass* Parent, USkeletalMesh* Mesh, TArray<UPackage*>& Packages)
{
	const FString Path = Root + Name;
	UPackage* Package = CreatePackage(*Path);
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->ParentClass = Parent;
	Factory->TargetSkeleton = Mesh->GetSkeleton();
	Factory->PreviewSkeletalMesh = Mesh;
	auto* Blueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package,
		*Name, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (Blueprint == nullptr) return nullptr;
	Packages.AddUnique(Package); FAssetRegistryModule::AssetCreated(Blueprint);
	return Blueprint;
}

FGraphBuilder Builder(UAnimBlueprint* Blueprint)
{
	FGraphBuilder Result;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		if (Graph != nullptr && Graph->GetFName() == TEXT("AnimGraph")) Result.Graph = Graph;
	if (Result.Graph != nullptr) Result.Schema = Cast<UEdGraphSchema_K2>(Result.Graph->GetSchema());
	Result.bOK = Result.Graph != nullptr && Result.Schema != nullptr;
	return Result;
}

void FinishGraph(FGraphBuilder& B, UEdGraphNode* Result)
{
	for (UEdGraphNode* Node : B.Graph->Nodes)
		if (Cast<UAnimGraphNode_Root>(Node) != nullptr) { B.Pose(Result, Node, TEXT("Result")); return; }
	B.bOK = Error(TEXT("Factory did not create an AnimGraph result node."));
}

// Body 与 Arms 使用相同持枪基准和增量；身体另与移动腿部做分骨骼混合。
UEdGraphNode* UpperBody(FGraphBuilder& B, const TMap<FName, UAnimSequence*>& Clips)
{
	auto* NormalDelta = B.Blend(B.Sequence(Clips[TEXT("IdleLoop")]), B.Sequence(Clips[TEXT("WalkLoop")], true, true), TEXT("MovementAlpha"));
	auto* AimedDelta = B.Blend(B.Sequence(Clips[TEXT("AimIdle")]), B.Sequence(Clips[TEXT("AimWalk")], true, true), TEXT("MovementAlpha"));
	// ADS 增量基于 Aim_Pose 而不是 Idle_Pose；先各自还原完整姿态再混合，避免瞄准只剩轻微抖动。
	auto* Normal = B.Add(B.Sequence(Clips[TEXT("IdlePose")]), NormalDelta);
	auto* Aimed = B.Add(B.Sequence(Clips[TEXT("AimPose")]), AimedDelta);
	auto* Aiming = B.Blend(Normal, Aimed, TEXT("AimAlpha"));
	auto* Sprinting = B.Add(B.Sequence(Clips[TEXT("IdlePose")]), B.Sequence(Clips[TEXT("Sprint")], true, true));
	auto* Full = B.Blend(Aiming, Sprinting, TEXT("SprintAlpha"));
	return B.AddMesh(B.Slot(Full), B.Hit(Clips[TEXT("Hit")]), TEXT("HitAlpha"));
}

bool BuildArms(UAnimBlueprint* Blueprint, const TMap<FName, UAnimSequence*>& Clips)
{
	FGraphBuilder B = Builder(Blueprint);
	if (!B.bOK) return Error(TEXT("Arms factory is missing AnimGraph."));
	FinishGraph(B, UpperBody(B, Clips));
	return B.bOK;
}

bool BuildBody(UAnimBlueprint* Blueprint, UBlendSpace* Directions, UBlendSpace* Unarmed, const TMap<FName, UAnimSequence*>& Clips)
{
	FGraphBuilder B = Builder(Blueprint);
	if (!B.bOK) return Error(TEXT("Body factory is missing AnimGraph."));
	auto* ArmedMovement = B.Node<UAnimGraphNode_BlendSpacePlayer>(TEXT("按本地前/右速度采样四向走跑，侧移不再播放正向走路。"));
	ArmedMovement->Node.SetBlendSpace(Directions); ArmedMovement->AllocateDefaultPins();
	B.Value(TEXT("ForwardSpeed"), ArmedMovement, TEXT("X")); B.Value(TEXT("RightSpeed"), ArmedMovement, TEXT("Y"));
	auto* UnarmedMovement = B.Node<UAnimGraphNode_BlendSpacePlayer>(TEXT("空手保留 Manny 基础走跑；拾取后才切换持枪移动姿态。"));
	UnarmedMovement->Node.SetBlendSpace(Unarmed); UnarmedMovement->AllocateDefaultPins();
	B.Value(TEXT("GroundSpeed"), UnarmedMovement, TEXT("X"));
	auto* Grounded = B.Select(ArmedMovement, UnarmedMovement, TEXT("bArmed"));
	auto* GroundCache = B.Node<UAnimGraphNode_SaveCachedPose>(TEXT("缓存本帧地面姿态，落地增量与普通移动共用同一计算结果，避免姿态引脚一接多造成原连接被断开。"));
	GroundCache->CacheName = TEXT("RARGrounded"); GroundCache->Node.CachePoseName = TEXT("RARGrounded");
	GroundCache->AllocateDefaultPins(); B.Pose(Grounded, GroundCache, TEXT("Pose"));
	auto* GroundForLanding = B.Node<UAnimGraphNode_UseCachedPose>(TEXT("落地以当前地面移动为基准，而非把 MM_Land 增量当作完整姿态。"));
	GroundForLanding->SaveCachedPoseNode = GroundCache; GroundForLanding->Node.CachePoseName = TEXT("RARGrounded"); GroundForLanding->AllocateDefaultPins();
	auto* GroundForMovement = B.Node<UAnimGraphNode_UseCachedPose>(TEXT("非落地阶段直接使用相同地面缓存。"));
	GroundForMovement->SaveCachedPoseNode = GroundCache; GroundForMovement->Node.CachePoseName = TEXT("RARGrounded"); GroundForMovement->AllocateDefaultPins();
	auto* InAir = B.Select(B.Sequence(Clips[TEXT("Jump")], false), B.Sequence(Clips[TEXT("Fall")] ), TEXT("bJumpRising"));
	auto* LandPose = B.Add(GroundForLanding, B.Sequence(Clips[TEXT("Land")], false));
	auto* Landing = B.Select(LandPose, GroundForMovement, TEXT("bLanding"));
	auto* LowerBody = B.Select(InAir, Landing, TEXT("bFalling"));
	auto* Layer = B.Node<UAnimGraphNode_LayeredBoneBlend>(TEXT("只覆盖持枪上半身与独立 ik_hand_root 枪链；腿部继续真实方向移动/起落。"));
	FBranchFilter Spine; Spine.BoneName = TEXT("spine_01"); Spine.BlendDepth = 1;
	FBranchFilter Gun; Gun.BoneName = TEXT("ik_hand_root"); Gun.BlendDepth = 1;
	Layer->Node.LayerSetup[0].BranchFilters = {Spine, Gun};
	Layer->Node.bMeshSpaceRotationBlend = true;
	Layer->AllocateDefaultPins();
	B.Pose(LowerBody, Layer, TEXT("BasePose")); B.Pose(UpperBody(B, Clips), Layer, TEXT("BlendPoses_0"));
	auto* ArmedWeight = B.Node<UK2Node_CallFunction>(TEXT("装备布尔值仅转为分层权重，不回写任何玩法状态。"));
	ArmedWeight->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_BoolToDouble")));
	ArmedWeight->AllocateDefaultPins(); B.Value(TEXT("bArmed"), ArmedWeight, TEXT("InBool"));
	B.Link(ArmedWeight->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Layer->FindPin(TEXT("BlendWeights_0")));
	auto* ToComponent = B.Node<UAnimGraphNode_LocalToComponentSpace>(TEXT("骨骼控制前显式转换姿态空间。"));
	ToComponent->AllocateDefaultPins(); B.Pose(Layer, ToComponent, TEXT("LocalPose"));
	auto* Aim = B.Node<UAnimGraphNode_ModifyBone>(TEXT("视角俯仰只改变脊柱表现；不改移动、控制旋转或服务器命中历史。"));
	Aim->Node.BoneToModify.BoneName = TEXT("spine_03");
	Aim->Node.RotationMode = BMM_Additive; Aim->Node.RotationSpace = BCS_ComponentSpace;
	Aim->Node.TranslationMode = BMM_Ignore; Aim->Node.ScaleMode = BMM_Ignore;
	Aim->AllocateDefaultPins(); B.Pose(ToComponent, Aim, TEXT("ComponentPose")); B.Value(TEXT("AimRotation"), Aim, TEXT("Rotation"));
	auto* GunAim = B.Node<UAnimGraphNode_ModifyBone>(TEXT("独立 ik_hand_root 枪链以同一 ComponentSpace 旋转并补偿绕脊柱支点的平移，维持枪与双手共同俯仰。"));
	GunAim->Node.BoneToModify.BoneName = TEXT("ik_hand_root");
	GunAim->Node.RotationMode = BMM_Additive; GunAim->Node.RotationSpace = BCS_ComponentSpace;
	GunAim->Node.TranslationMode = BMM_Additive; GunAim->Node.TranslationSpace = BCS_ComponentSpace;
	GunAim->Node.ScaleMode = BMM_Ignore;
	GunAim->AllocateDefaultPins(); B.Pose(Aim, GunAim, TEXT("ComponentPose"));
	B.Value(TEXT("AimRotation"), GunAim, TEXT("Rotation")); B.Value(TEXT("GunAimTranslation"), GunAim, TEXT("Translation"));
	auto* ToLocal = B.Node<UAnimGraphNode_ComponentToLocalSpace>(TEXT("骨骼控制后转换回本地姿态，供最终动画输出。"));
	ToLocal->AllocateDefaultPins(); B.Pose(GunAim, ToLocal, TEXT("ComponentPose"));
	FinishGraph(B, ToLocal);
	return B.bOK;
}

UAnimMontage* Montage(const FString& Name, UAnimSequence* Clip, TArray<UPackage*>& Packages)
{
	auto* Result = NewAsset<UAnimMontage>(Root + Name, Packages);
	Result->SetSkeleton(Clip->GetSkeleton());
	// UAnimMontage 构造器已有空 DefaultSlot；本项目只使用 RARAction，不能保留第二条空轨道。
	Result->SlotAnimTracks.Reset();
	FSlotAnimationTrack& Slot = Result->SlotAnimTracks.AddDefaulted_GetRef(); Slot.SlotName = TEXT("RARAction");
	FAnimSegment& Segment = Slot.AnimTrack.AnimSegments.AddDefaulted_GetRef();
	Segment.SetAnimReference(Clip); Segment.AnimStartTime = 0.0f; Segment.AnimEndTime = Clip->GetPlayLength();
	Segment.AnimPlayRate = 1.0f; Segment.LoopingCount = 1; Segment.StartPos = 0.0f;
	Result->SetCompositeLength(Clip->GetPlayLength()); Result->AddAnimCompositeSection(TEXT("Default"), 0.0f);
	Result->BlendIn.SetBlendTime(0.06f); Result->BlendOut.SetBlendTime(0.08f);
	Result->Notifies.Reset(); Result->RefreshCacheData();
	return Result;
}

// 该命令只允许固定目的路径：首次生成全部通过后再保存；再次运行不覆盖已经生成或用户编辑过的资产。
void Run(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((Args.Num() > 0 && !bApply) || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{ Error(TEXT("Usage: fp.PrepareRARPresentation [Apply], no active PIE.")); return; }
	USkeletalMesh* Body = Load<USkeletalMesh>(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny_Simple"));
	USkeletalMesh* Arms = Load<USkeletalMesh>(TEXT("/Game/InfimaGames/ArtCore/Characters/Meshes/SK_IG_FP_Arms"));
	USkeletalMesh* Gun = Load<USkeletalMesh>(TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SK_RAR_AssaultRifle"));
	USkeletalMesh* OldBody = Load<USkeletalMesh>(TEXT("/Game/FPS/Demo/Characters/Mannequins/Meshes/SKM_Manny_Simple"));
	UBlueprint* OldPlayer = Load<UBlueprint>(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter"));
	UfpstrueWeaponConfig* OldWeaponConfig = Load<UfpstrueWeaponConfig>(TEXT("/Game/Config/DA_FPWeapon_RU74"));
	UBlendSpace* Unarmed = Load<UBlendSpace>(MannyArt + TEXT("BS_MM_WalkRun"));
	if (Body == nullptr || Arms == nullptr || Gun == nullptr || OldBody == nullptr || Unarmed == nullptr
		|| OldPlayer == nullptr || OldWeaponConfig == nullptr || OldPlayer->GeneratedClass == nullptr || Body->GetSkeleton() != Arms->GetSkeleton()
		|| !SameReferencePose(OldBody, Body)) { Error(TEXT("Missing source resources or incompatible body/arms/reference skeleton.")); return; }
	const TArray<FClip> Definitions = {
		{TEXT("IdlePose"), CharacterArt + TEXT("Poses/A_RAR_FP_PCH_AssaultRifle_Idle_Pose"), false},
		{TEXT("AimPose"), CharacterArt + TEXT("Poses/A_RAR_FP_PCH_AssaultRifle_Aim_Pose"), false},
		{TEXT("IdleLoop"), CharacterArt + TEXT("Movement/A_RAR_FP_PCH_AssaultRifle_Idle_Loop"), true},
		{TEXT("WalkLoop"), CharacterArt + TEXT("Movement/A_RAR_FP_PCH_AssaultRifle_Walk_F_Loop"), true},
		{TEXT("AimIdle"), CharacterArt + TEXT("Movement/A_RAR_FP_PCH_AssaultRifle_Idle_Loop_Aimed"), true},
		{TEXT("AimWalk"), CharacterArt + TEXT("Movement/A_RAR_FP_PCH_AssaultRifle_Walk_F_Loop_Aimed"), true},
		{TEXT("Sprint"), CharacterArt + TEXT("Movement/A_RAR_FP_PCH_AssaultRifle_Sprint_F_Loop"), true},
		{TEXT("Fire"), CharacterArt + TEXT("Actions/A_RAR_FP_PCH_AssaultRifle_Fire"), true},
		{TEXT("FireAimed"), CharacterArt + TEXT("Actions/A_RAR_FP_PCH_AssaultRifle_Fire_Aimed"), true},
		{TEXT("Reload"), CharacterArt + TEXT("Actions/A_RAR_FP_PCH_AssaultRifle_Reload"), true},
		{TEXT("ReloadEmpty"), CharacterArt + TEXT("Actions/A_RAR_FP_PCH_AssaultRifle_Reload_Empty"), true},
		{TEXT("Equip"), CharacterArt + TEXT("Actions/A_RAR_FP_PCH_AssaultRifle_Equip"), true},
		{TEXT("GunIdle"), WeaponArt + TEXT("A_RAR_FP_WEP_AssaultRifle_Reference"), false},
		{TEXT("GunFire"), WeaponArt + TEXT("A_RAR_FP_WEP_AssaultRifle_Fire"), false},
		{TEXT("GunReload"), WeaponArt + TEXT("A_RAR_FP_WEP_AssaultRifle_Reload"), false},
		{TEXT("GunReloadEmpty"), WeaponArt + TEXT("A_RAR_FP_WEP_AssaultRifle_Reload_Empty"), false},
		{TEXT("Jump"), MannyArt + TEXT("MM_Jump"), false},
		{TEXT("Fall"), MannyArt + TEXT("MM_Fall_Loop"), false},
		{TEXT("Land"), MannyArt + TEXT("MM_Land"), true},
		{TEXT("MannyIdle"), MannyArt + TEXT("MM_Idle"), false},
		{TEXT("Hit"), TEXT("/Game/FPS/Animations/TPP/HitReact/A_MM_HitReact_Front_Med_01_Additive"), true},
		{TEXT("WalkF"), TppArt + TEXT("A_TPP_SMG_Walk_Fwd"), false},
		{TEXT("WalkB"), TppArt + TEXT("A_TPP_SMG_Walk_Bwd"), false},
		{TEXT("WalkL"), TppArt + TEXT("A_TPP_SMG_Walk_Left"), false},
		{TEXT("WalkR"), TppArt + TEXT("A_TPP_SMG_Walk_Right"), false},
		{TEXT("JogF"), TppArt + TEXT("A_TPP_SMG_Jog_Fwd"), false},
		{TEXT("JogB"), TppArt + TEXT("A_TPP_SMG_Jog_Bwd"), false},
		{TEXT("JogL"), TppArt + TEXT("A_TPP_SMG_Jog_Left"), false},
		{TEXT("JogR"), TppArt + TEXT("A_TPP_SMG_Jog_Right"), false}
	};
	TMap<FName, UAnimSequence*> Sources;
	TArray<FString> Destinations = {Root + TEXT("ABP_RARBody"), Root + TEXT("ABP_RARArms"), Root + TEXT("ABP_RARWeapon"), Root + TEXT("BS_RARDirections"), PlayerPackage, TEXT("/Game/Config/DA_FPWeapon_RAR")};
	for (const FClip& Definition : Definitions)
	{
		UAnimSequence* Sequence = Load<UAnimSequence>(Definition.Source);
		if (Sequence == nullptr || Sequence->IsValidAdditive() != Definition.bAdditive)
		{ Error(TEXT("Missing source or wrong additive mode: ") + Definition.Source); return; }
		Sources.Add(Definition.Key, Sequence); Destinations.Add(Root + TEXT("Clips/A_RAR_") + Definition.Key.ToString());
		UE_LOG(LogTemp, Display, TEXT("RAR_PRESENTATION SOURCE Key=%s Path=%s Additive=%d Notifies=%d"),
			*Definition.Key.ToString(), *Sequence->GetPathName(), Sequence->IsValidAdditive(), Sequence->Notifies.Num());
	}
	const TArray<FName> BodyActions = {TEXT("Fire"), TEXT("FireAimed"), TEXT("Reload"), TEXT("ReloadEmpty"), TEXT("Equip")};
	const TArray<FName> GunActions = {TEXT("Fire"), TEXT("Reload"), TEXT("ReloadEmpty")};
	for (FName Name : BodyActions) Destinations.Add(Root + TEXT("AM_RAR_") + Name.ToString());
	for (FName Name : GunActions) Destinations.Add(Root + TEXT("AM_RARWeapon_") + Name.ToString());
	int32 Existing = 0;
	for (const FString& Path : Destinations)
	{
		// 启动时对尚未生成资源的静默加载会留下空 Package；空壳不是用户资产，不能阻止首次生成。
		UPackage* ExistingPackage = FindPackage(nullptr, *Path);
		const bool bHasAssetObject = ExistingPackage != nullptr
			&& FindObjectFast<UObject>(ExistingPackage, *FPackageName::GetShortName(Path)) != nullptr;
		if (FPackageName::DoesPackageExist(Path) || bHasAssetObject) { ++Existing; UE_LOG(LogTemp, Display, TEXT("RAR_PRESENTATION EXISTING %s"), *Path); }
	}
	UE_LOG(LogTemp, Display, TEXT("RAR_PRESENTATION AUDIT Mode=%s Sources=%d Outputs=%d Existing=%d ReferencePoseMatch=true"),
		bApply ? TEXT("Apply") : TEXT("Check"), Sources.Num(), Destinations.Num(), Existing);
	if (!bApply) { UE_LOG(LogTemp, Display, TEXT("RAR_PRESENTATION PASS Read-only preflight; no assets changed.")); return; }
	if (Existing > 0) { Error(TEXT("Output paths already exist; refusing to overwrite authored or previously generated assets.")); return; }
	USkeleton* Skeleton = Body->GetSkeleton(); USkeleton* GunSkeleton = Gun->GetSkeleton();
	if (Skeleton == nullptr || GunSkeleton == nullptr || Skeleton->GetOutermost()->IsDirty() || GunSkeleton->GetOutermost()->IsDirty())
	{ Error(TEXT("Source skeleton is missing or has unsaved changes.")); return; }
	const FString BackupDirectory = FPaths::ProjectSavedDir() / TEXT("Backups/RARPresentation")
		/ (FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	for (USkeleton* Value : {Skeleton, GunSkeleton})
		if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Value->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), BackupDirectory)) return;
	TArray<UPackage*> Packages;
	Skeleton->Modify(); Skeleton->AddCompatibleSkeleton(OldBody->GetSkeleton());
	for (USkeleton* Value : {Skeleton, GunSkeleton})
	{
		Value->Modify(); Value->RegisterSlotNode(TEXT("RARAction")); Value->SetSlotGroupName(TEXT("RARAction"), FAnimSlotGroup::DefaultGroupName);
		Value->MarkPackageDirty(); Packages.AddUnique(Value->GetOutermost());
	}
	TMap<FName, UAnimSequence*> Clips;
	for (const FClip& Definition : Definitions)
	{
		const FString Path = Root + TEXT("Clips/A_RAR_") + Definition.Key.ToString();
		UPackage* Package = CreatePackage(*Path);
		auto* Copy = DuplicateObject<UAnimSequence>(Sources[Definition.Key], Package, *FPackageName::GetShortName(Path));
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional); Copy->Notifies.Reset(); Copy->RefreshCacheData();
		Copy->MarkPackageDirty(); FAssetRegistryModule::AssetCreated(Copy); Packages.AddUnique(Package); Clips.Add(Definition.Key, Copy);
	}
	// 配置沿用现有战斗调参，只修改新骨架挂点与成套换弹时长；运行弹药仍在各自武器实例中。
	auto* WeaponConfig = NewAsset<UfpstrueWeaponConfig>(TEXT("/Game/Config/DA_FPWeapon_RAR"), Packages);
	WeaponConfig->Settings = OldWeaponConfig->Settings;
	WeaponConfig->Settings.GripSocketName = TEXT("SOCKET_Weapon");
	WeaponConfig->Settings.ReloadDuration = Clips[TEXT("Reload")]->GetPlayLength();
	WeaponConfig->Settings.EmptyReloadDuration = Clips[TEXT("ReloadEmpty")]->GetPlayLength();
	FString ConfigurationError;
	if (!WeaponConfig->Settings.Validate(ConfigurationError)) { Error(ConfigurationError); return; }
	auto* Directions = NewAsset<UBlendSpace>(Root + TEXT("BS_RARDirections"), Packages); Directions->SetSkeleton(Skeleton);
	FProperty* Parameters = FindFProperty<FProperty>(Directions->GetClass(), TEXT("BlendParameters"));
	if (Parameters == nullptr) { Error(TEXT("BlendSpace axis property is unavailable; no assets saved.")); return; }
	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		FBlendParameter* Parameter = Parameters->ContainerPtrToValuePtr<FBlendParameter>(Directions, Axis);
		Parameter->DisplayName = Axis == 0 ? TEXT("Forward speed") : TEXT("Right speed");
		Parameter->Min = -600.0f; Parameter->Max = 600.0f; Parameter->GridNum = 8;
	}
	Directions->AddSample(Clips[TEXT("MannyIdle")], FVector::ZeroVector);
	const FName WalkKeys[] = {TEXT("WalkF"), TEXT("WalkB"), TEXT("WalkL"), TEXT("WalkR")};
	const FName JogKeys[] = {TEXT("JogF"), TEXT("JogB"), TEXT("JogL"), TEXT("JogR")};
	const FVector Axes[] = {FVector(1,0,0), FVector(-1,0,0), FVector(0,-1,0), FVector(0,1,0)};
	for (int32 Index = 0; Index < 4; ++Index) { Directions->AddSample(Clips[WalkKeys[Index]], Axes[Index] * 300.0); Directions->AddSample(Clips[JogKeys[Index]], Axes[Index] * 600.0); }
	Directions->ValidateSampleData(); Directions->ResampleData();
	TMap<FName, UAnimMontage*> Actions;
	for (FName Name : BodyActions) Actions.Add(Name, Montage(TEXT("AM_RAR_") + Name.ToString(), Clips[Name], Packages));
	for (FName Name : GunActions) Montage(TEXT("AM_RARWeapon_") + Name.ToString(), Clips[FName(*(TEXT("Gun") + Name.ToString()))], Packages);
	UAnimBlueprint* BodyBP = AnimBlueprint(TEXT("ABP_RARBody"), UfpstruePlayerAnimInstance::StaticClass(), Body, Packages);
	UAnimBlueprint* ArmsBP = AnimBlueprint(TEXT("ABP_RARArms"), UfpstruePlayerAnimInstance::StaticClass(), Arms, Packages);
	UAnimBlueprint* WeaponBP = AnimBlueprint(TEXT("ABP_RARWeapon"), UAnimInstance::StaticClass(), Gun, Packages);
	if (BodyBP == nullptr || ArmsBP == nullptr || WeaponBP == nullptr || !BuildBody(BodyBP, Directions, Unarmed, Clips) || !BuildArms(ArmsBP, Clips))
	{ Error(TEXT("Could not construct the presentation graphs; no assets saved.")); return; }
	FGraphBuilder WeaponGraph = Builder(WeaponBP);
	if (!WeaponGraph.bOK) { Error(TEXT("Weapon factory is missing AnimGraph.")); return; }
	FinishGraph(WeaponGraph, WeaponGraph.Slot(WeaponGraph.Sequence(Clips[TEXT("GunIdle")])));
	if (!WeaponGraph.bOK) return;
	for (UAnimBlueprint* Blueprint : {BodyBP, ArmsBP, WeaponBP})
	{
		if (!FpstruePvpAssetPreparation::Compile(Blueprint)) { Error(TEXT("AnimBP compile failed; no assets saved.")); return; }
		if (auto* Defaults = Cast<UfpstruePlayerAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject()))
		{
			Defaults->FireMontage = Actions[TEXT("Fire")]; Defaults->AimedFireMontage = Actions[TEXT("FireAimed")];
			Defaults->ReloadMontage = Actions[TEXT("Reload")]; Defaults->EmptyReloadMontage = Actions[TEXT("ReloadEmpty")]; Defaults->EquipMontage = Actions[TEXT("Equip")];
		}
	}
	UPackage* PlayerOuter = CreatePackage(PlayerPackage);
	UBlueprint* Player = FKismetEditorUtilities::CreateBlueprint(AfpstrueCharacter::StaticClass(), PlayerOuter, TEXT("BP_RARCharacter"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("FpstrueRARPresentation"));
	if (Player == nullptr) { Error(TEXT("Cannot create native-based RAR player blueprint.")); return; }
	Packages.AddUnique(PlayerOuter); FAssetRegistryModule::AssetCreated(Player);
	if (!FpstruePvpAssetPreparation::Compile(Player)) return;
	auto* Defaults = Cast<AfpstrueCharacter>(Player->GeneratedClass->GetDefaultObject());
	const UObject* OldDefaults = OldPlayer->GeneratedClass->GetDefaultObject();
	for (const TCHAR* Name : {TEXT("DefaultMappingContext"), TEXT("JumpAction"), TEXT("MoveAction"), TEXT("LookAction"), TEXT("FireAction"), TEXT("ReloadAction"), TEXT("SprintAction"), TEXT("AimAction"), TEXT("WalkSpeed"), TEXT("SprintSpeed"), TEXT("AimWalkSpeed")})
	{
		FProperty* Property = FindFProperty<FProperty>(AfpstrueCharacter::StaticClass(), Name);
		if (Property == nullptr) { Error(FString(TEXT("Missing native input/config property ")) + Name); return; }
		Property->CopyCompleteValue_InContainer(Defaults, OldDefaults);
	}
	FBoolProperty* UseRAR = FindFProperty<FBoolProperty>(AfpstrueCharacter::StaticClass(), TEXT("bUseRARPresentation"));
	if (UseRAR == nullptr) { Error(TEXT("Native character lacks bUseRARPresentation.")); return; }
	UseRAR->SetPropertyValue_InContainer(Defaults, true);
	Defaults->GetMesh()->SetSkeletalMesh(Body); Defaults->GetMesh()->SetAnimInstanceClass(BodyBP->GeneratedClass);
	Defaults->GetMesh1P()->SetSkeletalMesh(Arms); Defaults->GetMesh1P()->SetAnimInstanceClass(ArmsBP->GeneratedClass);
	// 源相机挂在手臂 head 的 SOCKET_Camera；反转源 Idle/Aim 的相机变换，等效到本项目 Camera 为父的布局（忽略约 0.00046 度误差）。
	Defaults->GetMesh1P()->SetRelativeLocationAndRotation(FVector(-0.66443537, 0.0, -162.57509561), FRotator(0.0, -90.0, 0.0));
	for (UPackage* Package : Packages) Package->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) { Error(TEXT("Save did not complete; skeleton backups retained at ") + BackupDirectory); return; }
	UE_LOG(LogTemp, Display, TEXT("RAR_PRESENTATION PASS Packages=%d NewPlayer=%s SkeletonBackup=%s OriginalBlueprintsAndCommentsUntouched=true"),
		Packages.Num(), PlayerPackage, *BackupDirectory);
}

// 修复范围只有本项目刚生成的两个 AnimBP 和八个 Montage；不重建素材、不保存源模板或骨架。
bool ReadsNativeValue(const UEdGraphPin* Input, FName Name)
{
	if (Input == nullptr || Input->LinkedTo.Num() != 1) return false;
	const UEdGraphPin* Output = Input->LinkedTo[0];
	const auto* Read = Cast<UK2Node_VariableGet>(Output->GetOwningNode());
	return Read != nullptr && Read->VariableReference.IsSelfContext()
		&& Read->VariableReference.GetMemberName() == Name && Read->FindPin(Name) == Output;
}

struct FHitRepair
{
	UAnimBlueprint* Blueprint = nullptr;
	UAnimGraphNode_SequencePlayer* OldPlayer = nullptr;
	UEdGraphPin* AdditiveInput = nullptr;
};

bool InspectHit(UAnimBlueprint* Blueprint, UAnimSequence* Hit, FHitRepair& Repair)
{
	if (Blueprint == nullptr || Blueprint->ParentClass != UfpstruePlayerAnimInstance::StaticClass()
		|| Blueprint->GeneratedClass == nullptr || Blueprint->GetOutermost()->IsDirty())
		return Error(TEXT("Hit repair requires a saved project AnimBP with the exact native presentation parent."));
	FGraphBuilder B = Builder(Blueprint);
	if (!B.bOK) return Error(TEXT("Hit repair requires the generated AnimGraph."));
	int32 ActivePlayers = 0, ActiveEvaluators = 0;
	for (UEdGraphNode* Node : B.Graph->Nodes)
	{
		auto* Player = Cast<UAnimGraphNode_SequencePlayer>(Node);
		auto* Evaluator = Cast<UAnimGraphNode_SequenceEvaluator>(Node);
		if ((Player == nullptr || Player->Node.GetSequence() != Hit)
			&& (Evaluator == nullptr || Evaluator->Node.GetSequence() != Hit)) continue;
		UEdGraphPin* Output = PoseOutput(Node);
		if (Output == nullptr || Output->LinkedTo.IsEmpty()) continue; // 保留的旧节点与原注释不参与执行。
		if (Output->LinkedTo.Num() != 1) return Error(TEXT("Hit pose has unexpected multiple consumers."));
		UEdGraphPin* Target = Output->LinkedTo[0];
		auto* Layer = Cast<UAnimGraphNode_ApplyMeshSpaceAdditive>(Target->GetOwningNode());
		if (Layer == nullptr || Layer->GetGraph() != B.Graph || Target != Layer->FindPin(TEXT("Additive"))
			|| Target->LinkedTo.Num() != 1 || !ReadsNativeValue(Layer->FindPin(TEXT("Alpha")), TEXT("HitAlpha")))
			return Error(TEXT("Hit pose is not the unique MeshSpaceAdditive input weighted by native HitAlpha."));
		if (Player != nullptr)
		{
			++ActivePlayers; Repair.OldPlayer = Player; Repair.AdditiveInput = Target;
		}
		else
		{
			++ActiveEvaluators;
			if (!Evaluator->Node.GetTeleportToExplicitTime() || Evaluator->Node.IsLooping()
				|| Evaluator->Node.ShouldUseExplicitFrame() || Evaluator->Node.GetGroupName() != NAME_None
				|| Evaluator->Node.GetGroupMethod() != EAnimSyncMethod::DoNotSync
				|| !ReadsNativeValue(Evaluator->FindPin(TEXT("ExplicitTime")), TEXT("HitTime")))
				return Error(TEXT("Existing hit evaluator is not the expected explicit HitTime/teleport node."));
		}
	}
	if (ActivePlayers + ActiveEvaluators != 1)
		return Error(TEXT("Expected exactly one connected safe hit player or already-repaired evaluator."));
	Repair.Blueprint = Blueprint;
	UE_LOG(LogTemp, Display, TEXT("RAR_HIT_REPAIR AUDIT Blueprint=%s NeedsEvaluator=%d SafeHit=%s"),
		*Blueprint->GetPathName(), ActivePlayers, *Hit->GetPathName());
	return true;
}

// 只接受已知的构造器空轨道；其他轨道、片段、通知或用户未保存修改均拒绝，避免扩大资产变更范围。
bool InspectMontage(UAnimMontage* MontageAsset, UAnimSequence* Clip, bool& bNeedsRepair)
{
	if (MontageAsset == nullptr || Clip == nullptr || MontageAsset->GetOutermost()->IsDirty()
		|| MontageAsset->GetSkeleton() != Clip->GetSkeleton() || !MontageAsset->Notifies.IsEmpty())
		return Error(TEXT("Montage repair requires saved generated resources with the expected skeleton and no notifies."));
	int32 RARTracks = 0, EmptyDefaultTracks = 0;
	for (const FSlotAnimationTrack& Track : MontageAsset->SlotAnimTracks)
	{
		if (Track.SlotName == TEXT("DefaultSlot") && Track.AnimTrack.AnimSegments.IsEmpty()) { ++EmptyDefaultTracks; continue; }
		if (Track.SlotName != TEXT("RARAction") || Track.AnimTrack.AnimSegments.Num() != 1
			|| Track.AnimTrack.AnimSegments[0].GetAnimReference() != Clip)
			return Error(TEXT("Refusing montage with an unexpected track or animation segment: ") + MontageAsset->GetPathName());
		++RARTracks;
	}
	if (RARTracks != 1 || EmptyDefaultTracks > 1)
		return Error(TEXT("Expected one RARAction and at most one empty constructor DefaultSlot: ") + MontageAsset->GetPathName());
	bNeedsRepair = EmptyDefaultTracks == 1;
	UE_LOG(LogTemp, Display, TEXT("RAR_HIT_REPAIR AUDIT Montage=%s EmptyDefaultSlot=%d RARActionPreserved=true"),
		*MontageAsset->GetPathName(), bNeedsRepair);
	return true;
}

void RepairHit(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((Args.Num() > 0 && !bApply) || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{ Error(TEXT("Usage: fp.RepairRARHit [Apply], no active PIE.")); return; }
	if (FindFProperty<FFloatProperty>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("HitTime")) == nullptr)
	{ Error(TEXT("Native HitTime float is required before repairing animation graphs.")); return; }
	TStrongObjectPtr<UAnimSequence> Hit(Load<UAnimSequence>(Root + TEXT("Clips/A_RAR_Hit")));
	if (!Hit.IsValid() || Hit->AdditiveAnimType != AAT_RotationOffsetMeshSpace || !Hit->Notifies.IsEmpty())
	{ Error(TEXT("Safe hit clip is missing, has notifies, or is not MeshSpaceRotation additive.")); return; }
	TArray<TStrongObjectPtr<UObject>> KeepAlive;
	KeepAlive.Emplace(Hit.Get());
	TArray<FHitRepair> Repairs;
	TArray<UPackage*> Packages;
	for (const TCHAR* Name : {TEXT("ABP_RARBody"), TEXT("ABP_RARArms")})
	{
		UAnimBlueprint* Blueprint = Load<UAnimBlueprint>(Root + Name);
		if (Blueprint != nullptr) KeepAlive.Emplace(Blueprint);
		FHitRepair Repair;
		if (!InspectHit(Blueprint, Hit.Get(), Repair)) return;
		Repairs.Add(Repair);
		if (Repair.OldPlayer != nullptr) Packages.AddUnique(Blueprint->GetOutermost());
	}
	TArray<UAnimMontage*> MontageRepairs;
	const TArray<FName> BodyActions = {TEXT("Fire"), TEXT("FireAimed"), TEXT("Reload"), TEXT("ReloadEmpty"), TEXT("Equip")};
	const TArray<FName> GunActions = {TEXT("Fire"), TEXT("Reload"), TEXT("ReloadEmpty")};
	for (int32 Kind = 0; Kind < 2; ++Kind)
	{
		for (FName Action : Kind == 0 ? BodyActions : GunActions)
		{
			const FString MontagePath = Root + (Kind == 0 ? TEXT("AM_RAR_") : TEXT("AM_RARWeapon_")) + Action.ToString();
			const FString ClipPath = Root + TEXT("Clips/A_RAR_") + (Kind == 0 ? TEXT("") : TEXT("Gun")) + Action.ToString();
			UAnimMontage* MontageAsset = Load<UAnimMontage>(MontagePath);
			UAnimSequence* Clip = Load<UAnimSequence>(ClipPath);
			if (MontageAsset != nullptr) KeepAlive.Emplace(MontageAsset);
			if (Clip != nullptr) KeepAlive.Emplace(Clip);
			bool bNeedsRepair = false;
			if (!InspectMontage(MontageAsset, Clip, bNeedsRepair)) return;
			if (bNeedsRepair) { MontageRepairs.Add(MontageAsset); Packages.AddUnique(MontageAsset->GetOutermost()); }
		}
	}
	if (!bApply || Packages.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("RAR_HIT_REPAIR PASS Mode=%s PackagesNeedingRepair=%d NoAssetsChanged=true"),
			bApply ? TEXT("Apply") : TEXT("Check"), Packages.Num());
		return;
	}
	const FString BackupDirectory = FPaths::ProjectSavedDir() / TEXT("Backups/RARHitRepair")
		/ (FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	for (UPackage* Package : Packages)
		if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), BackupDirectory)) return;
	for (const FHitRepair& Repair : Repairs)
	{
		if (Repair.OldPlayer == nullptr) continue;
		Repair.Blueprint->Modify(); Repair.OldPlayer->GetGraph()->Modify(); Repair.OldPlayer->Modify();
		Repair.AdditiveInput->GetOwningNode()->Modify();
		FGraphBuilder B = Builder(Repair.Blueprint);
		auto* Evaluator = B.Hit(Hit.Get());
		Evaluator->NodePosX = Repair.OldPlayer->NodePosX;
		Evaluator->NodePosY = Repair.OldPlayer->NodePosY + 160;
		if (!B.bOK) return;
		// 原节点和作者注释完整保留；仅替换原来通往受击叠加层的这一条姿态线。
		PoseOutput(Repair.OldPlayer)->BreakLinkTo(Repair.AdditiveInput);
		B.Link(PoseOutput(Evaluator), Repair.AdditiveInput);
		if (!B.bOK) return;
	}
	for (UAnimMontage* MontageAsset : MontageRepairs)
	{
		MontageAsset->Modify();
		MontageAsset->SlotAnimTracks.RemoveAll([](const FSlotAnimationTrack& Track)
		{ return Track.SlotName == TEXT("DefaultSlot") && Track.AnimTrack.AnimSegments.IsEmpty(); });
		MontageAsset->RefreshCacheData(); MontageAsset->MarkPackageDirty();
	}
	for (const FHitRepair& Repair : Repairs)
		if (Repair.OldPlayer != nullptr && !FpstruePvpAssetPreparation::Compile(Repair.Blueprint))
		{ Error(TEXT("Hit repair compile failed; no edited assets saved. Backup: ") + BackupDirectory); return; }
	for (UPackage* Package : Packages) Package->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
	{ Error(TEXT("Hit repair save did not complete. Backup: ") + BackupDirectory); return; }
	UE_LOG(LogTemp, Display, TEXT("RAR_HIT_REPAIR PASS PackagesSaved=%d EmptyMontageTracksRemoved=%d Backup=%s SourceAssetsUntouched=true"),
		Packages.Num(), MontageRepairs.Num(), *BackupDirectory);
}

FAutoConsoleCommand RepairHitCommand(TEXT("fp.RepairRARHit"),
	TEXT("Check two generated RAR AnimBPs and eight generated montages. Add Apply to back up, repair repeat-hit sampling and remove only empty constructor DefaultSlot tracks."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RepairHit));

FAutoConsoleCommand Command(TEXT("fp.PrepareRARPresentation"),
	TEXT("Check RAR presentation sources. Add Apply to create project-only safe clips, montages, AnimBPs and native-based player BP without overwriting existing assets."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}

#endif // WITH_EDITOR
