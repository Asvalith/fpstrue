// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "AbilitySystemComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Shared/fpstrueFootIK.h"
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
#include "Engine/SkeletalMeshSocket.h"
#include "PhysicsEngine/PhysicsAsset.h"
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
#include "GameFramework/PlayerInput.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerState.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "BrainComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "Components/WidgetComponent.h"
#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/MetaData.h"
#include "UObject/UnrealType.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/BlendSpace.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_ApplyAdditive.h"
#include "AnimGraphNode_ApplyMeshSpaceAdditive.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_RotationOffsetBlendSpace.h"
#include "AnimGraphNode_RotateRootBone.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_LinkedInputPose.h"
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
#include "Game/fpstrueMultiplayer.h"
#include "GameFramework/WorldSettings.h"
#include "K2Node_InputKey.h"
#include "K2Node_MacroInstance.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Containers/Ticker.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavigationData.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Tests/AutomationCommon.h"

/*
 * 编辑器资源工具与显式画面验收入口，不是运行时玩法的一部分（WITH_EDITOR）。
 * Prepare/Repair/Calibrate 命令默认检查，只有 Apply 才在限定路径备份、改图/默认值并保存；
 * RAR/Template 首次生成还会备份并保存所用骨架的兼容声明与动作槽，不是完全零源资产写入。
 * 资源 Audit 只读取/采样，不编译或保存资源；AuditPickupWalk 则显式驱动真实
 * 游戏输入、等待正常结算并写截图，不能当作只读玩法检查。所有入口在编辑器 Game Thread 执行。
 * 主链：检查来源与已有结构 -> Apply 时备份/构造 -> 编译 -> 保存；验收链不替代 GAS/服务器结算。
 */

// 多人唯一入口迁移；先检查全图，Apply 仅删除可确认的旧开局/UI链，保留场景逻辑和控件布局。
namespace FpstruePvpAssetPreparation
{
constexpr const TCHAR* MapPackages[] = {
	TEXT("/Game/PerformanceCandidates/SplineBake_Tracks20260928/Demonstration_Baked"),
	TEXT("/Game/PerformanceCandidates/Nanite_HardSurface20260929/Demonstration_Nanite"),
	TEXT("/Game/FactoryDistrict/Maps/Demonstration"),
	TEXT("/Game/FirstPerson/Maps/FirstPersonMap")};
constexpr const TCHAR* HUDObjects[] = {TEXT("/Game/FirstPerson/UI/ingame_network.ingame_network"),
	TEXT("/Game/FirstPerson/UI/ingame.ingame")};

bool Fail(const FString& Reason)
{
	UE_LOG(LogTemp, Error, TEXT("PVP_ASSET_PREP FAIL %s"), *Reason);
	return false;
}

// 其他表现工具继续使用以下建节点、备份、编译助手。
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

// 编译一个已编辑蓝图并返回实际错误状态；只编译，不在此保存包。
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

struct FLegacyGraphPlan
{
	TSet<UEdGraphNode*> Nodes;
	TArray<UEdGraph*> Functions;
	TArray<FName> Variables;
	bool bSafe = true;
};

bool IsLegacyReference(const FString& Path)
{
	for (const TCHAR* Prefix : {TEXT("/Game/FirstPerson/Blueprints/gamemode/fpstruegamemode."),
		TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonGameMode."), TEXT("/Script/fpstrue.fpstrueGameMode"),
		TEXT("/Game/FirstPerson/UI/mainmenu."), TEXT("/Game/FirstPerson/UI/mainmenushake."),
		TEXT("/Game/FirstPerson/UI/ingame."), TEXT("/Game/FirstPerson/UI/winorfail.")})
		if (Path.Contains(Prefix)) return true;
	return false;
}

bool ReferencesLegacy(const UEdGraphNode& Node)
{
	if (const auto* Call = Cast<UK2Node_CallFunction>(&Node))
	{
		// CoreRedirect 会把旧 OwnerClass 改成新 GameMode；函数名仍是已删除的单机规则。
		const FName Member = Call->FunctionReference.GetMemberName();
		if (Member == TEXT("GetRemainingTime") || Member == TEXT("StartGameMode")) return true;
		if (const UFunction* Function = Call->GetTargetFunction())
			if (IsLegacyReference(Function->GetOwnerClass()->GetPathName())) return true;
	}
	for (const UEdGraphPin* Pin : Node.Pins)
		if (Pin && (IsLegacyReference(GetPathNameSafe(Pin->DefaultObject)) ||
			IsLegacyReference(GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get())) ||
			IsLegacyReference(GetPathNameSafe(Pin->PinType.PinValueType.TerminalSubCategoryObject.Get())) ||
			IsLegacyReference(Pin->DefaultValue))) return true;
	return false;
}

// 数据节点可随旧UI移除；遇到其他有副作用的场景行为则拒绝，不从 BeginPlay 盲目清空整图。
bool IsLegacyUINode(const UEdGraphNode& Node)
{
	if (const auto* Call = Cast<UK2Node_CallFunction>(&Node))
	{
		const UFunction* Function = Call->GetTargetFunction();
		if (Function && (Function->HasAnyFunctionFlags(FUNC_BlueprintPure) ||
			Function->GetOwnerClass()->IsChildOf(UWidget::StaticClass()) ||
			IsLegacyReference(Function->GetOwnerClass()->GetPathName()))) return true;
		const FName Name = Call->FunctionReference.GetMemberName();
		for (const FName Allowed : {FName(TEXT("IsStandalone")), FName(TEXT("AddToViewport")), FName(TEXT("AddToPlayerScreen")),
			// Check 日志确认这些节点仅属于旧菜单/倒计时/胜负 UI 链；不删除场景中的相机 Actor。
			FName(TEXT("GetRemainingTime")), FName(TEXT("StartGameMode")), FName(TEXT("Entergameplay")),
			FName(TEXT("SetViewTargetWithBlend")), FName(TEXT("EnableInput")), FName(TEXT("DisableInput")),
			FName(TEXT("SetText")), FName(TEXT("SetColorAndOpacity")),
			FName(TEXT("RemoveFromParent")), FName(TEXT("RemoveAllWidgets")), FName(TEXT("SetGamePaused")),
			FName(TEXT("SetInputMode_GameAndUIEx")), FName(TEXT("SetInputMode_UIOnlyEx")), FName(TEXT("SetInputMode_GameOnly")),
			FName(TEXT("PlayAnimation")), FName(TEXT("PlayAnimationForward")), FName(TEXT("PlayAnimationReverse")),
			FName(TEXT("StopAnimation")), FName(TEXT("SetVisibility")), FName(TEXT("Delay")),
			FName(TEXT("K2_SetTimerDelegate")), FName(TEXT("K2_SetTimer")), FName(TEXT("K2_ClearTimerHandle")),
			FName(TEXT("K2_ClearAndInvalidateTimerHandle"))})
			if (Name == Allowed) return true;
		return false;
	}
	if (const auto* Event = Cast<UK2Node_Event>(&Node); Event && Node.GetClass() == UK2Node_Event::StaticClass())
	{
		const FName Name = Event->EventReference.GetMemberName();
		return Name == TEXT("ReceiveBeginPlay") || Name == TEXT("Construct") || Name == TEXT("PreConstruct") || Name == TEXT("Destruct");
	}
	if (const auto* Input = Cast<UK2Node_InputKey>(&Node)) return Input->InputKey == EKeys::AnyKey;
	if (const auto* Macro = Cast<UK2Node_MacroInstance>(&Node))
		return Macro->GetMacroGraph() && Macro->GetMacroGraph()->GetOutermost()->GetName() == TEXT("/Engine/EditorBlueprintResources/StandardMacros");
	for (const FName Allowed : {FName(TEXT("K2Node_VariableGet")), FName(TEXT("K2Node_VariableSet")),
		FName(TEXT("K2Node_MakeStruct")), FName(TEXT("K2Node_Literal")),
		FName(TEXT("K2Node_DynamicCast")), FName(TEXT("K2Node_Self")), FName(TEXT("K2Node_CreateWidget")),
		FName(TEXT("K2Node_Knot")), FName(TEXT("K2Node_IfThenElse")), FName(TEXT("K2Node_ExecutionSequence")),
		FName(TEXT("K2Node_CustomEvent")), FName(TEXT("K2Node_AddDelegate")), FName(TEXT("K2Node_RemoveDelegate")),
		FName(TEXT("K2Node_AssignDelegate")), FName(TEXT("K2Node_ClearDelegate")), FName(TEXT("K2Node_CreateDelegate")),
		FName(TEXT("K2Node_CallDelegate")), FName(TEXT("K2Node_ComponentBoundEvent")),
		FName(TEXT("K2Node_FunctionEntry")), FName(TEXT("K2Node_FunctionResult"))})
		if (Node.GetClass()->GetFName() == Allowed) return true;
	return false;
}

FLegacyGraphPlan InspectLegacyGraphs(UBlueprint& Blueprint)
{
	FLegacyGraphPlan Plan;
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	TArray<UEdGraphNode*> Pending;
	for (UEdGraph* Graph : Graphs)
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			UE_LOG(LogTemp, Display, TEXT("MP_ONLY NODE Asset=%s Graph=%s Node=%s Guid=%s Class=%s Title=%s LegacySeed=%d"),
				*Blueprint.GetPathName(), *Graph->GetName(), *Node->GetName(), *Node->NodeGuid.ToString(),
				*Node->GetClass()->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), ReferencesLegacy(*Node));
			if (ReferencesLegacy(*Node)) Pending.Add(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				UE_LOG(LogTemp, Display, TEXT("MP_ONLY PIN Node=%s Name=%s Direction=%d Type=%s Object=%s Default=%s Links=%d"),
					*Node->GetName(), *Pin->PinName.ToString(), static_cast<int32>(Pin->Direction),
					*GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get()), *GetPathNameSafe(Pin->DefaultObject), *Pin->DefaultValue, Pin->LinkedTo.Num());
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
					if (Linked) UE_LOG(LogTemp, Display, TEXT("MP_ONLY EDGE From=%s.%s To=%s.%s"), *Node->GetName(),
						*Pin->PinName.ToString(), *Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString());
			}
		}
	while (!Pending.IsEmpty())
	{
		UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
		if (Plan.Nodes.Contains(Node)) continue;
		Plan.Nodes.Add(Node);
		if (!IsLegacyUINode(*Node))
		{
			Plan.bSafe = false;
			UE_LOG(LogTemp, Warning, TEXT("MP_ONLY REVIEW_REQUIRED Asset=%s Graph=%s Node=%s Class=%s Title=%s"),
				*Blueprint.GetPathName(), *Node->GetGraph()->GetName(), *Node->GetName(), *Node->GetClass()->GetName(),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		}
		for (UEdGraphPin* Pin : Node->Pins)
			if (Pin) for (UEdGraphPin* Linked : Pin->LinkedTo) if (Linked) Pending.Add(Linked->GetOwningNode());
	}
	for (UEdGraph* Graph : Blueprint.FunctionGraphs)
	{
		if (!Graph->Nodes.ContainsByPredicate([&Plan](UEdGraphNode* Node) { return Plan.Nodes.Contains(Node); })) continue;
		const bool bWholeFunction = !Graph->Nodes.ContainsByPredicate([&Plan](UEdGraphNode* Node)
			{ return Node && Node->GetClass()->GetFName() != TEXT("EdGraphNode_Comment") && !Plan.Nodes.Contains(Node); });
		if (bWholeFunction) Plan.Functions.Add(Graph);
		else { Plan.bSafe = false; UE_LOG(LogTemp, Warning, TEXT("MP_ONLY REVIEW_REQUIRED MixedFunction=%s"), *Graph->GetPathName()); }
	}
	for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
		if (IsLegacyReference(GetPathNameSafe(Variable.VarType.PinSubCategoryObject.Get()))) Plan.Variables.Add(Variable.VarName);
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY PLAN Asset=%s RemoveNodes=%d RemoveFunctions=%d RemoveVariables=%d Safe=%d"),
		*Blueprint.GetPathName(), Plan.Nodes.Num(), Plan.Functions.Num(), Plan.Variables.Num(), Plan.bSafe);
	return Plan;
}

void RemoveLegacyGraphs(UBlueprint& Blueprint, const FLegacyGraphPlan& Plan)
{
	Blueprint.Modify();
	for (UEdGraphNode* Node : Plan.Nodes)
		if (!Plan.Functions.Contains(Node->GetGraph())) FBlueprintEditorUtils::RemoveNode(&Blueprint, Node, true);
	for (UEdGraph* Graph : Plan.Functions) FBlueprintEditorUtils::RemoveGraph(&Blueprint, Graph, EGraphRemoveFlags::None);
	for (const FName Variable : Plan.Variables) FBlueprintEditorUtils::RemoveMemberVariable(&Blueprint, Variable);
}

bool InspectMap(UWorld& World, FLegacyGraphPlan& Plan)
{
	AWorldSettings* Settings = World.GetWorldSettings();
	ULevelScriptBlueprint* Level = World.PersistentLevel ? World.PersistentLevel->GetLevelScriptBlueprint(false) : nullptr;
	if (!Settings || Settings->GetOutermost() != World.GetOutermost() || (Level && Level->GetOutermost() != World.GetOutermost()))
		return Fail(TEXT("World settings and level script must belong to the exact map package."));
	const UClass* CurrentMode = Settings->DefaultGameMode.Get();
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY MAP Package=%s DefaultGameMode=%s LevelScript=%s"),
		*World.GetOutermost()->GetName(), *GetPathNameSafe(CurrentMode), *GetPathNameSafe(Level));
	if (Level) Plan = InspectLegacyGraphs(*Level);
	if (CurrentMode && CurrentMode != AfpstruePvpGameMode::StaticClass() && !IsLegacyReference(CurrentMode->GetPathName()))
		return Fail(TEXT("Unexpected authored GameMode requires review: ") + CurrentMode->GetPathName());
	return Plan.bSafe;
}

bool InspectNetworkHUD(UWidgetBlueprint& HUD, FLegacyGraphPlan& Plan)
{
	Plan = InspectLegacyGraphs(HUD);
	for (const FDelegateEditorBinding& Binding : HUD.Bindings)
		UE_LOG(LogTemp, Display, TEXT("MP_ONLY HUD_BINDING Object=%s Property=%s Function=%s"),
			*Binding.ObjectName, *Binding.PropertyName.ToString(), *Binding.FunctionName.ToString());
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY HUD Parent=%s Bindings=%d LayoutUnchanged=1"), *GetPathNameSafe(HUD.ParentClass.Get()), HUD.Bindings.Num());
	return Plan.bSafe;
}

void Run(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	const bool bCheck = Args.IsEmpty() || (Args.Num() == 1 && Args[0].Equals(TEXT("Check"), ESearchCase::IgnoreCase));
	if ((!bApply && !bCheck) || !GEditor || GEditor->PlayWorld)
	{ Fail(TEXT("Usage: fp.MigrateMultiplayerOnly [Check|Apply], outside PIE.")); return; }
	UWorld* Previous = GEditor->GetEditorWorldContext().World();
	if (Previous && Previous->GetOutermost()->IsDirty()) { Fail(TEXT("Current map has unsaved edits.")); return; }
	TArray<TStrongObjectPtr<UWidgetBlueprint>> HUDs;
	TArray<bool> NeedsHUD;
	bool bSafe = true;
	for (const TCHAR* Object : HUDObjects)
	{
		TStrongObjectPtr<UWidgetBlueprint> HUD(LoadObject<UWidgetBlueprint>(nullptr, Object));
		if (!HUD.IsValid() || HUD->GetOutermost()->IsDirty()) { Fail(FString(TEXT("Saved HUD is missing or dirty: ")) + Object); return; }
		FLegacyGraphPlan Plan;
		bSafe &= InspectNetworkHUD(*HUD.Get(), Plan);
		NeedsHUD.Add(!Plan.Nodes.IsEmpty() || !Plan.Variables.IsEmpty() || !HUD->Bindings.IsEmpty() ||
			HUD->ParentClass != UfpstruePvpIngameWidget::StaticClass());
		HUDs.Add(MoveTemp(HUD));
	}
	TArray<FString> ChangedMaps;
	for (const TCHAR* Package : MapPackages)
	{
		UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()));
		if (!World || World->GetOutermost()->GetName() != Package) { Fail(FString(TEXT("Cannot load exact map: ")) + Package); return; }
		FLegacyGraphPlan Plan;
		bSafe &= InspectMap(*World, Plan);
		if (World->GetOutermost()->IsDirty()) { Fail(TEXT("Loading changed a map; inspect it before migration.")); return; }
		if (World->GetWorldSettings()->DefaultGameMode != AfpstruePvpGameMode::StaticClass() || !Plan.Nodes.IsEmpty() || !Plan.Variables.IsEmpty())
			ChangedMaps.Add(Package);
	}
	int32 ChangedHUDs = 0;
	for (const bool bNeeds : NeedsHUD) if (bNeeds) ++ChangedHUDs;
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY CHECK_COMPLETE Safe=%d MapsToChange=%d HUDsToChange=%d NoAssetsSaved=1"), bSafe, ChangedMaps.Num(), ChangedHUDs);
	if (!bApply) return;
	if (!bSafe) { Fail(TEXT("Unrecognized scene logic touches the old UI chain. Review Check output; no assets changed.")); return; }
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/MultiplayerOnly") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	for (const FString& Package : ChangedMaps)
		if (!BackupAsset(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()), Backup)) return;
	for (int32 Index = 0; Index < HUDs.Num(); ++Index)
		if (NeedsHUD[Index] && !BackupAsset(FPackageName::LongPackageNameToFilename(HUDs[Index]->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	for (const FString& Package : ChangedMaps)
	{
		UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()));
		if (!World) { Fail(TEXT("Map reload failed after backup.")); return; }
		FLegacyGraphPlan Plan;
		if (!InspectMap(*World, Plan)) return;
		World->GetWorldSettings()->Modify();
		World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
		if (ULevelScriptBlueprint* Level = World->PersistentLevel->GetLevelScriptBlueprint(false))
		{
			RemoveLegacyGraphs(*Level, Plan);
			if (!Compile(Level)) { Fail(TEXT("Level compile failed; no save. Backup=") + Backup); return; }
		}
		World->MarkPackageDirty();
		if (!UEditorLoadingAndSavingUtils::SaveMap(World, Package)) { Fail(TEXT("Map save failed. Backup=") + Backup); return; }
	}
	for (int32 Index = 0; Index < HUDs.Num(); ++Index)
	{
		if (!NeedsHUD[Index]) continue;
		UWidgetBlueprint* HUD = HUDs[Index].Get();
		// LoadMap 可触发蓝图重建；应用前重新解析，不能使用加载地图前的节点指针。
		FLegacyGraphPlan CurrentPlan;
		if (!InspectNetworkHUD(*HUD, CurrentPlan)) { Fail(TEXT("HUD changed since inspection. Backup=") + Backup); return; }
		RemoveLegacyGraphs(*HUD, CurrentPlan);
		if (!InspectLegacyGraphs(*HUD).Nodes.IsEmpty()) { Fail(TEXT("HUD still references a removed gameplay entry. Backup=") + Backup); return; }
		HUD->ParentClass = UfpstruePvpIngameWidget::StaticClass();
		HUD->Bindings.Reset();
		if (!Compile(HUD)) { Fail(TEXT("HUD compile failed; no save. Backup=") + Backup); return; }
		HUD->MarkPackageDirty();
		if (!UEditorLoadingAndSavingUtils::SavePackages({HUD->GetOutermost()}, false)) { Fail(TEXT("HUD save failed. Backup=") + Backup); return; }
	}
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY APPLIED Maps=%d HUDs=%d Backup=%s UnrelatedSceneLogicAndWidgetLayoutRetained=1"),
		ChangedMaps.Num(), ChangedHUDs, *Backup);
}

FAutoConsoleCommand PreparePvpAssets(TEXT("fp.PreparePvpAssets"),
	TEXT("Check/Apply multiplayer-only migration of fixed map GameModes, legacy level UI chains and network HUD graphs."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
FAutoConsoleCommand MigrateMultiplayerOnly(TEXT("fp.MigrateMultiplayerOnly"),
	TEXT("Check logs all level/HUD nodes. Apply backs up exact packages and removes only recognized legacy UI chains."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Run));

// 换弹已统一权威时钟；逐资产删除旧原生装弹 Notify，不删除音效或其他通知。
void RemoveLegacyReloadNotifies(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((!Args.IsEmpty() && !bApply) || !GEditor || GEditor->PlayWorld)
	{ Fail(TEXT("Usage: fp.RemoveLegacyReloadNotifies [Apply], outside PIE.")); return; }
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UAnimSequenceBase::StaticClass()->GetClassPathName(), Assets, true);
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/ReloadNotify") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	int32 Changed = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/"))) continue;
		UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Asset.GetAsset());
		if (!Animation) { Fail(TEXT("Could not inspect animation: ") + Asset.GetObjectPathString()); return; }
		const auto IsLegacy = [](const FAnimNotifyEvent& Event)
		{
			return Event.Notify && Event.Notify->GetClass()->GetPathName() == TEXT("/Script/fpstrue.fpstrueAnimNotify_ReloadCommit");
		};
		const int32 Count = Animation->Notifies.FilterByPredicate(IsLegacy).Num();
		if (!Count) continue;
		UE_LOG(LogTemp, Display, TEXT("RELOAD_NOTIFY LEGACY Asset=%s Count=%d"), *Animation->GetPathName(), Count);
		if (bApply)
		{
			if (Animation->GetOutermost()->IsDirty()) { Fail(TEXT("Animation has unsaved changes: ") + Animation->GetPathName()); return; }
			if (!BackupAsset(FPackageName::LongPackageNameToFilename(Asset.PackageName.ToString(), FPackageName::GetAssetPackageExtension()), Backup)) return;
			Animation->Modify();
			Animation->Notifies.RemoveAll(IsLegacy);
			Animation->PostEditChange();
			Animation->MarkPackageDirty();
			if (!UEditorLoadingAndSavingUtils::SavePackages({Animation->GetOutermost()}, false))
			{ Fail(TEXT("Animation save failed. Backup=") + Backup); return; }
		}
		++Changed;
	}
	UE_LOG(LogTemp, Display, TEXT("RELOAD_NOTIFY COMPLETE Apply=%d Assets=%d Backup=%s"), bApply, Changed, *Backup);
}

FAutoConsoleCommand RemoveReloadNotifies(TEXT("fp.RemoveLegacyReloadNotifies"),
	TEXT("Check, or Apply with backups, removal of the obsolete native reload commit notifies."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RemoveLegacyReloadNotifies));

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

// 生成项目自己的 RAR 表现资产，不导入供应方玩法；Apply 会额外备份并更新所用骨架的兼容声明和动作槽。
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

// 记录表现资源工具的具体失败原因，并将失败交还当前构造/检查流程。
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

// 比较实际原始骨名、父子关系和参考变换，不能用资源名称或兼容列表代替骨架一致性。
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

// 在调用方已完成路径不存在检查后创建项目资产，登记待保存包；此 helper 本身不写盘。
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

// 返回生成动画节点的结构型输出引脚；缺失时由图构造器记录连接失败。
UEdGraphPin* PoseOutput(UEdGraphNode* Node)
{
	for (UEdGraphPin* Pin : Node->Pins)
		if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			return Pin;
	return nullptr;
}

// 只构造当前新建/限定修复的 AnimGraph；bOK 汇总连接错误，不拥有玩法状态，也不自行编译保存。
struct FGraphBuilder
{
	UEdGraph* Graph = nullptr;
	const UEdGraphSchema_K2* Schema = nullptr;
	bool bOK = true;
	int32 Serial = 0;
	// 创建带说明和稳定布局的新节点，并补齐动画编译扩展所需的放置生命周期。
	template <typename T> T* Node(const FString& Comment)
	{
		T* Result = FpstruePvpAssetPreparation::AddNode<T>(Graph, (Serial % 6) * 320, (Serial / 6) * 250);
		++Serial;
		Result->NodeComment = Comment;
		// 正常编辑器放置节点会登记 CachedPose/Slot 等编译扩展，脚本生成也必须走同一生命周期。
		if constexpr (TIsDerivedFrom<T, UAnimGraphNode_Base>::IsDerived) Result->PostPlacedNewNode();
		return Result;
	}
	// 通过图 Schema 接线；任一缺失或类型不兼容都累计到 bOK，不悄悄跳过错误。
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
	// 将上游姿态输出连接到指定姿态输入。
	void Pose(UEdGraphNode* Out, UEdGraphNode* In, FName Pin) { Link(PoseOutput(Out), In->FindPin(Pin)); }
	// 生成对原生动画快照属性的只读引脚，不生成角色查询或业务状态写入。
	void Value(FName Property, UEdGraphNode* In, FName Pin)
	{
		auto* Read = Node<UK2Node_VariableGet>(TEXT("只读取 NativeUpdateAnimation 的本帧快照，不查询或修改玩法规则。"));
		Read->VariableReference.SetSelfMember(Property);
		Read->AllocateDefaultPins();
		Link(Read->FindPin(Property), In->FindPin(Pin));
	}
	// 创建序列播放器；按需绑定移动播放速率，循环选择由动作类型明确传入。
	UAnimGraphNode_SequencePlayer* Sequence(UAnimSequence* Asset, bool bLoop = true, bool bMovementRate = false)
	{
		auto* Player = Node<UAnimGraphNode_SequencePlayer>(TEXT("本项目安全序列副本；无模板业务 Notify，动作资格与结算仍归原 GAS/Weapon。"));
		Player->Node.SetSequence(Asset);
		Player->Node.SetLoopAnimation(bLoop);
		Player->AllocateDefaultPins();
		if (bMovementRate) { Expose(Player, TEXT("PlayRate")); Value(TEXT("LocomotionPlayRate"), Player, TEXT("PlayRate")); }
		return Player;
	}
	// 创建显式 HitTime 受击采样器；时间跳转不触发 Notify 或提取 Root Motion。
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
	// 按原生快照权重混合两份完整姿态。
	UAnimGraphNode_TwoWayBlend* Blend(UEdGraphNode* A, UEdGraphNode* B, FName Alpha)
	{
		auto* Result = Node<UAnimGraphNode_TwoWayBlend>(TEXT("以原生派生权重混合表现，不建立第二套冲刺/瞄准状态。"));
		Result->AllocateDefaultPins();
		Pose(A, Result, TEXT("A")); Pose(B, Result, TEXT("B")); Value(Alpha, Result, TEXT("Alpha"));
		return Result;
	}
	// 按快照布尔值选择姿态；重新激活子项时重置其非循环动画时间。
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
	// 在完整基准姿态上应用 LocalSpace 增量，未指定权重时完整叠加。
	UAnimGraphNode_ApplyAdditive* Add(UEdGraphNode* Base, UEdGraphNode* Delta, FName Alpha = NAME_None)
	{
		auto* Result = Node<UAnimGraphNode_ApplyAdditive>(TEXT("先建立非叠加基准姿态，再应用 LocalSpace 增量；不能把增量直接输出为完整姿态。"));
		Result->Node.Alpha = 1.0f;
		Result->AllocateDefaultPins();
		Pose(Base, Result, TEXT("Base")); Pose(Delta, Result, TEXT("Additive"));
		if (!Alpha.IsNone()) Value(Alpha, Result, TEXT("Alpha"));
		return Result;
	}
	// 使用匹配的 MeshSpace 节点叠加旋转增量，避免误用 LocalSpace 解释受击资源。
	UAnimGraphNode_ApplyMeshSpaceAdditive* AddMesh(UEdGraphNode* Base, UEdGraphNode* Delta, FName Alpha)
	{
		auto* Result = Node<UAnimGraphNode_ApplyMeshSpaceAdditive>(TEXT("受击资源为 MeshSpace Rotation 增量，使用匹配的叠加节点；不改源素材叠加类型，也不占换弹动作槽。"));
		Result->Node.Alpha = 1.0f;
		Result->AllocateDefaultPins();
		Pose(Base, Result, TEXT("Base")); Pose(Delta, Result, TEXT("Additive")); Value(Alpha, Result, TEXT("Alpha"));
		return Result;
	}
	// 在基础姿态上接入指定动作槽；播放资格和换弹提交仍属于原生业务模块。
	UAnimGraphNode_Slot* Slot(UEdGraphNode* Source, FName SlotName = TEXT("RARAction"))
	{
		auto* Result = Node<UAnimGraphNode_Slot>(TEXT("RARAction 只承载本项目 Fire/Reload/Equip 表现；无模板状态或弹药提交。"));
		Result->Node.SlotName = SlotName;
		Result->AllocateDefaultPins(); Pose(Source, Result, TEXT("Source"));
		return Result;
	}
};

// 用指定原生父类和实际网格骨架创建新 AnimBP，交调用方建图、编译并决定保存。
UAnimBlueprint* AnimBlueprint(const FString& Name, UClass* Parent, USkeletalMesh* Mesh, TArray<UPackage*>& Packages, const FString& DestinationRoot = Root)
{
	const FString Path = DestinationRoot + Name;
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

// 解析蓝图的 AnimGraph 与 K2 Schema，返回仅作用于该图的构造上下文。
FGraphBuilder Builder(UAnimBlueprint* Blueprint)
{
	FGraphBuilder Result;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		if (Graph != nullptr && Graph->GetFName() == TEXT("AnimGraph")) Result.Graph = Graph;
	if (Result.Graph != nullptr) Result.Schema = Cast<UEdGraphSchema_K2>(Result.Graph->GetSchema());
	Result.bOK = Result.Graph != nullptr && Result.Schema != nullptr;
	return Result;
}

// 把最终姿态接到工厂生成的 Root；缺少 Root 则标记构造失败。
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

// 第一人称手臂只输出统一持枪上半身链，不生成移动、伤害或装备逻辑。
bool BuildArms(UAnimBlueprint* Blueprint, const TMap<FName, UAnimSequence*>& Clips)
{
	FGraphBuilder B = Builder(Blueprint);
	if (!B.bOK) return Error(TEXT("Arms factory is missing AnimGraph."));
	FinishGraph(B, UpperBody(B, Clips));
	return B.bOK;
}

// 全身图把真实四向移动/起落与持枪上身分层，再应用复制瞄准对应的本地骨骼表现。
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

// 以单个安全序列创建单动作槽 Montage，移除构造器空轨道和通知；此处不承担业务结算。
UAnimMontage* Montage(const FString& Name, UAnimSequence* Clip, TArray<UPackage*>& Packages, const FString& DestinationRoot = Root, FName SlotName = TEXT("RARAction"))
{
	auto* Result = NewAsset<UAnimMontage>(DestinationRoot + Name, Packages);
	Result->SetSkeleton(Clip->GetSkeleton());
	// UAnimMontage 构造器已有空 DefaultSlot；本项目只使用 RARAction，不能保留第二条空轨道。
	Result->SlotAnimTracks.Reset();
	FSlotAnimationTrack& Slot = Result->SlotAnimTracks.AddDefaulted_GetRef(); Slot.SlotName = SlotName;
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

// 检查输入是否唯一来自本 AnimInstance 的指定原生属性读取，不修改节点。
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

// 只读确认唯一受击叠加链，输出待替换的旧播放器或识别已修复的显式采样器。
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

// 默认检查两个项目 AnimBP 和八个 Montage；Apply 备份后修重复受击采样/空轨道，编译并只保存这些包。
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

// 只切换项目玩家蓝图的全身网格；不覆盖模板、动画图、相机、第一人称手臂或作者注释。
void UseFullBody(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((Args.Num() > 0 && !bApply) || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{ Error(TEXT("Usage: fp.UseRARFullBody [Apply], no active PIE.")); return; }
	TStrongObjectPtr<UBlueprint> Player(Load<UBlueprint>(PlayerPackage));
	TStrongObjectPtr<USkeletalMesh> FullBody(Load<USkeletalMesh>(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny")));
	TStrongObjectPtr<UAnimBlueprint> Animation(Load<UAnimBlueprint>(Root + TEXT("ABP_RARBody")));
	AfpstrueCharacter* Defaults = Player.IsValid() && Player->GeneratedClass != nullptr
		? Cast<AfpstrueCharacter>(Player->GeneratedClass->GetDefaultObject()) : nullptr;
	USkeletalMeshComponent* Body = Defaults != nullptr ? Defaults->GetMesh() : nullptr;
	USkeletalMeshComponent* Arms = Defaults != nullptr ? Defaults->GetMesh1P() : nullptr;
	if (Body == nullptr || Arms == nullptr || Body->GetSkeletalMeshAsset() == nullptr || !Defaults->UsesRARPresentation()
		|| !FullBody.IsValid() || FullBody->GetPhysicsAsset() == nullptr || FullBody->GetPostProcessAnimBlueprint() == nullptr
		|| !Animation.IsValid() || Animation->GeneratedClass == nullptr || Body->GetAnimClass() != Animation->GeneratedClass
		|| Arms->GetSkeletalMeshAsset() == nullptr || FullBody->GetSkeleton() == nullptr
		|| FullBody->GetSkeleton() != Animation->TargetSkeleton || FullBody->GetSkeleton() != Arms->GetSkeletalMeshAsset()->GetSkeleton())
	{ Error(TEXT("Full Manny, PhysicsAsset, post-process, project body graph or matching arms skeleton is missing; no edit.")); return; }
	for (const FName Bone : {FName(TEXT("head")), FName(TEXT("spine_03")), FName(TEXT("hand_r")), FName(TEXT("ik_hand_root")), FName(TEXT("ik_hand_gun"))})
		if (FullBody->GetRefSkeleton().FindBoneIndex(Bone) == INDEX_NONE)
		{ Error(TEXT("Full Manny lacks a required body/weapon bone: ") + Bone.ToString()); return; }
	const USkeletalMeshSocket* Grip = FullBody->FindSocket(TEXT("SOCKET_Weapon"));
	if (Grip == nullptr || Grip->BoneName != TEXT("ik_hand_gun"))
	{ Error(TEXT("Full Manny must retain the authored SOCKET_Weapon on ik_hand_gun; no edit.")); return; }
	const FString Previous = Body->GetSkeletalMeshAsset()->GetPathName();
	if (Previous != TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")
		&& Body->GetSkeletalMeshAsset() != FullBody.Get())
	{ Error(TEXT("Player has an unexpected authored body; refusing to replace it: ") + Previous); return; }
	UE_LOG(LogTemp, Display, TEXT("RAR_FULL_BODY AUDIT Mode=%s Before=%s Target=%s Skeleton=%s PhysicsAsset=%s PostProcess=%s"),
		bApply ? TEXT("Apply") : TEXT("Check"), *Previous, *FullBody->GetPathName(), *GetPathNameSafe(FullBody->GetSkeleton()),
		*GetPathNameSafe(FullBody->GetPhysicsAsset()), *GetPathNameSafe(FullBody->GetPostProcessAnimBlueprint().Get()));
	if (!bApply || Body->GetSkeletalMeshAsset() == FullBody.Get())
	{ UE_LOG(LogTemp, Display, TEXT("RAR_FULL_BODY PASS NoAssetsChanged=true")); return; }
	if (Player->GetOutermost()->IsDirty()) { Error(TEXT("Player has unsaved changes; no edit.")); return; }
	const FString BackupDirectory = FPaths::ProjectSavedDir() / TEXT("Backups/RARFullBody")
		/ (FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(PlayerPackage, FPackageName::GetAssetPackageExtension()), BackupDirectory)) return;
	Player->Modify(); Defaults->Modify(); Body->Modify();
	Body->SetSkeletalMesh(FullBody.Get());
	FBlueprintEditorUtils::MarkBlueprintAsModified(Player.Get());
	if (!FpstruePvpAssetPreparation::Compile(Player.Get()))
	{ Error(TEXT("Player compile failed; disk asset retained. Backup: ") + BackupDirectory); return; }
	Defaults = Cast<AfpstrueCharacter>(Player->GeneratedClass->GetDefaultObject());
	if (Defaults == nullptr || Defaults->GetMesh()->GetSkeletalMeshAsset() != FullBody.Get()
		|| Defaults->GetMesh()->GetAnimClass() != Animation->GeneratedClass)
	{ Error(TEXT("Compiled defaults did not retain the full body and project graph; no save. Backup: ") + BackupDirectory); return; }
	Player->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({Player->GetOutermost()}, false))
	{ Error(TEXT("Player save failed. Backup: ") + BackupDirectory); return; }
	UE_LOG(LogTemp, Display, TEXT("RAR_FULL_BODY PASS Player=%s Mesh=%s Backup=%s CameraArmsGraphsAndSourceAssetsUntouched=true"),
		PlayerPackage, *FullBody->GetPathName(), *BackupDirectory);
}

FAutoConsoleCommand FullBodyCommand(TEXT("fp.UseRARFullBody"),
	TEXT("Check the project RAR player's full Manny mesh/skeleton. Add Apply to back up and replace only its Simple body, retaining camera, arms and project animation graphs."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&UseFullBody));

// 显式诊断真实客户端世界，不改变装备或玩法；标签用于区分远端玩家与无枪的近战 AI。
void AuditWorld(UWorld* World)
{
	if (World == nullptr || !World->IsGameWorld()) { Error(TEXT("fp.AuditRARWorld requires a running game world.")); return; }
	int32 Players = 0;
	for (TActorIterator<AfpstrueCharacter> It(World); It; ++It)
	{
		AfpstrueCharacter* Player = *It;
		USkeletalMeshComponent* Body = Player->GetMesh();
		UfpstrueWeaponComponent* Weapon = Player->GetEquippedWeaponComponent();
		const APlayerState* State = Player->GetPlayerState();
		const int32 Id = State != nullptr ? State->GetPlayerId() : INDEX_NONE;
		const FVector Hand = Body->GetSocketLocation(TEXT("hand_r"));
		const FVector Grip = Body->GetSocketLocation(TEXT("SOCKET_Weapon"));
		const FVector Gun = Weapon != nullptr ? Weapon->GetComponentLocation() : FVector::ZeroVector;
		float FOV = -1.0f;
		if (Weapon != nullptr && Weapon->GetMaterial(0) != nullptr)
			Weapon->GetMaterial(0)->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Field Of View Alpha")), FOV);
		UE_LOG(LogTemp, Display, TEXT("RAR_WORLD Pawn=%s PlayerId=%d Role=%d Local=%d Body=%s Anim=%s PostProcess=%s BodyHidden=%d Armed=%d WeaponActor=%s WeaponClass=%s WeaponMesh=%s Parent=%s Socket=%s Visible=%d Hidden=%d ActorHidden=%d OwnerNoSee=%d OnlyOwnerSee=%d FOVAlpha=%.2f Scale=%s Hand=%s Grip=%s Gun=%s GripHandCm=%.3f GunGripCm=%.3f"),
			*Player->GetName(), Id, static_cast<int32>(Player->GetLocalRole()), Player->IsLocallyControlled(),
			*GetPathNameSafe(Body->GetSkeletalMeshAsset()), *GetPathNameSafe(Body->GetAnimClass()), *GetPathNameSafe(Body->GetPostProcessInstance()),
			Body->bHiddenInGame, Player->HasEquippedWeapon(), *GetNameSafe(Weapon != nullptr ? Weapon->GetOwner() : nullptr),
			*GetPathNameSafe(Weapon != nullptr && Weapon->GetOwner() != nullptr ? Weapon->GetOwner()->GetClass() : nullptr),
			*GetPathNameSafe(Weapon != nullptr ? Weapon->GetSkeletalMeshAsset() : nullptr), *GetPathNameSafe(Weapon != nullptr ? Weapon->GetAttachParent() : nullptr),
			Weapon != nullptr ? *Weapon->GetAttachSocketName().ToString() : TEXT("None"), Weapon != nullptr && Weapon->IsVisible(),
			Weapon != nullptr && Weapon->bHiddenInGame, Weapon != nullptr && Weapon->GetOwner()->IsHidden(),
			Weapon != nullptr && Weapon->bOwnerNoSee, Weapon != nullptr && Weapon->bOnlyOwnerSee, FOV,
			Weapon != nullptr ? *Weapon->GetComponentScale().ToCompactString() : TEXT("None"), *Hand.ToCompactString(), *Grip.ToCompactString(), *Gun.ToCompactString(),
			FVector::Distance(Grip, Hand), Weapon != nullptr ? FVector::Distance(Gun, Grip) : -1.0);
		DrawDebugString(World, FVector(0.0f, 0.0f, 115.0f), FString::Printf(TEXT("PLAYER %d"), Id), Player, FColor::Green, 60.0f, true);
		if (!Player->IsLocallyControlled() && Weapon != nullptr) DrawDebugLine(World, Hand, Gun, FColor::Red, false, 60.0f, 0, 2.0f);
		++Players;
	}
	int32 Enemies = 0;
	for (TActorIterator<AfpstrueEnemyCharacter> It(World); It; ++It)
	{
		AfpstrueEnemyCharacter* Enemy = *It;
		auto* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController());
		const auto* Brain = Controller != nullptr ? Controller->GetBrainComponent() : nullptr;
		const auto* Blackboard = Controller != nullptr ? Controller->GetBlackboardComponent() : nullptr;
		const auto* Perception = Controller != nullptr ? Controller->GetPerceptionComponent() : nullptr;
		const auto* Movement = Enemy->GetCharacterMovement();
		const auto* Bar = Enemy->FindComponentByClass<UWidgetComponent>();
		TArray<AActor*> Visible;
		if (Perception != nullptr) Perception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Visible);
		const AfpstrueCharacter* Nearest = nullptr;
		double DistanceSquared = TNumericLimits<double>::Max();
		for (TActorIterator<AfpstrueCharacter> PlayerIt(World); PlayerIt; ++PlayerIt)
		{
			if (PlayerIt->IsDead()) continue;
			const double Candidate = FVector::DistSquared(Enemy->GetActorLocation(), PlayerIt->GetActorLocation());
			if (Candidate < DistanceSquared) { DistanceSquared = Candidate; Nearest = *PlayerIt; }
		}
		const FVector Direction = Nearest != nullptr ? (Nearest->GetActorLocation() - Enemy->GetActorLocation()).GetSafeNormal2D() : FVector::ZeroVector;
		const double Angle = Nearest != nullptr ? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Enemy->GetActorForwardVector(), Direction), -1.0, 1.0))) : -1.0;
		UE_LOG(LogTemp, Display, TEXT("AI_WORLD Enemy=%s NetMode=%d Pos=%s Yaw=%.1f Dead=%d Controller=%s BrainRunning=%d Accepting=%d SightEnabled=%d Visible=%d Target=%s BBHasTarget=%d AIState=%d MoveStatus=%d MoveMode=%d MovementTick=%d Speed=%.1f MaxSpeed=%.1f Nearest=%s DistanceCm=%.1f ViewAngle=%.1f BarParent=%s BarPos=%s BarHidden=%d Widget=%s"),
			*Enemy->GetName(), static_cast<int32>(World->GetNetMode()), *Enemy->GetActorLocation().ToCompactString(), Enemy->GetActorRotation().Yaw, Enemy->IsDead(),
			*GetNameSafe(Controller), Brain != nullptr && Brain->IsRunning(), Controller != nullptr && Controller->AcceptsCombatCommands(),
			Perception != nullptr && Perception->IsSenseEnabled(UAISense_Sight::StaticClass()), Visible.Num(), *GetNameSafe(Controller != nullptr ? Controller->GetTargetCharacter() : nullptr),
			Blackboard != nullptr && Blackboard->GetValueAsBool(TEXT("HasTarget")), Controller != nullptr ? static_cast<int32>(Controller->GetAIState()) : -1,
			Controller != nullptr ? static_cast<int32>(Controller->GetMoveStatus()) : -1, Movement != nullptr ? static_cast<int32>(Movement->MovementMode) : -1,
			Movement != nullptr && Movement->IsComponentTickEnabled(), Enemy->GetVelocity().Size(), Movement != nullptr ? Movement->MaxWalkSpeed : 0.0f,
			*GetNameSafe(Nearest), Nearest != nullptr ? FMath::Sqrt(DistanceSquared) : -1.0, Angle, *GetNameSafe(Bar != nullptr ? Bar->GetAttachParent() : nullptr),
			Bar != nullptr ? *Bar->GetComponentLocation().ToCompactString() : TEXT("None"), Bar != nullptr && Bar->bHiddenInGame, *GetNameSafe(Bar != nullptr ? Bar->GetUserWidgetObject() : nullptr));
		DrawDebugString(World, FVector(0.0f, 0.0f, 115.0f), TEXT("MELEE AI"), Enemy, FColor::Yellow, 60.0f, true);
		++Enemies;
	}
	UE_LOG(LogTemp, Display, TEXT("RAR_WORLD END Players=%d MeleeAI=%d NetMode=%d"), Players, Enemies, static_cast<int32>(World->GetNetMode()));
}

FAutoConsoleCommandWithWorld WorldAuditCommand(TEXT("fp.AuditRARWorld"),
	TEXT("Inspect actual player equipment, visibility and hand/socket/world-gun positions; temporarily label players and melee AI without changing gameplay."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&AuditWorld));

FAutoConsoleCommand Command(TEXT("fp.PrepareRARPresentation"),
	TEXT("Check RAR presentation sources. Add Apply to create project-only safe clips, montages, AnimBPs and native-based player BP without overwriting existing assets."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}

// UE5.5 成套素材的项目表现适配及窄修复；Apply 只写明示目标，原供应方玩法蓝图不参与运行。
namespace FpstrueTemplatePresentation
{
using namespace FpstrueRARPresentation;
using FpstrueRARPresentation::Error;
const FString Destination = TEXT("/Game/FirstPerson/Animations/FPSMPT/");
const FString Art = TEXT("/Game/FPSMPT55/Animations/");
constexpr const TCHAR* PlayerPath = TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter");

// 同套 FP/TP 素材保留原始骨架和叠加类型；生成图只消费本项目的移动快照与已授权动作。
bool BuildPlayer(UAnimBlueprint* Blueprint, const TMap<FName, UAnimSequence*>& C, UBlendSpace* Directions, UBlendSpace* Unarmed, UBlendSpace* AimOffset, bool bFirstPerson)
{
	FGraphBuilder B = Builder(Blueprint);
	if (!B.bOK) return Error(TEXT("Template AnimGraph factory failed."));
	const FString Prefix = bFirstPerson ? TEXT("FP") : TEXT("TP");
	const auto Clip = [&C, &Prefix](const TCHAR* Name) { return C[FName(*(Prefix + Name))]; };
	UEdGraphNode* Upper = B.Sequence(Clip(TEXT("Idle")));
	if (bFirstPerson)
		Upper = B.Blend(Upper, B.Sequence(C[TEXT("FPWalk")], true, true), TEXT("MovementAlpha"));
	Upper = B.Blend(Upper, B.Sequence(Clip(TEXT("Aim"))), TEXT("AimAlpha"));
	Upper = B.Blend(Upper, B.Sequence(Clip(TEXT("Sprint")), true, true), TEXT("SprintAlpha"));
	if (bFirstPerson)
	{
		auto* Air = B.Select(B.Sequence(C[TEXT("FPJump")], false), B.Sequence(C[TEXT("FPFall")]), TEXT("bJumpRising"));
		Upper = B.Select(Air, Upper, TEXT("bFalling"));
		FinishGraph(B, B.AddMesh(B.Slot(Upper, TEXT("TemplateAction")), B.Hit(C[TEXT("Hit")]), TEXT("HitAlpha")));
		return B.bOK;
	}
	// 身体下半身保留四向移动与跳跃，持枪/换弹只覆盖脊柱上半身，不让双腿滑着播放待机。
	auto* Armed = B.Node<UAnimGraphNode_BlendSpacePlayer>(TEXT("按实际本地前/右速度播放配套四向走跑。"));
	Armed->Node.SetBlendSpace(Directions); Armed->AllocateDefaultPins();
	B.Value(TEXT("ForwardSpeed"), Armed, TEXT("X")); B.Value(TEXT("RightSpeed"), Armed, TEXT("Y"));
	auto* Empty = B.Node<UAnimGraphNode_BlendSpacePlayer>(TEXT("空手状态没有枪、不播放持枪上半身。"));
	Empty->Node.SetBlendSpace(Unarmed); Empty->AllocateDefaultPins(); B.Value(TEXT("GroundSpeed"), Empty, TEXT("X"));
	auto* Ground = B.Select(Armed, Empty, TEXT("bArmed"));
	auto* Cache = B.Node<UAnimGraphNode_SaveCachedPose>(TEXT("地面姿态仅计算一次，落地叠加与正常移动共用。"));
	Cache->CacheName = TEXT("TemplateGround"); Cache->Node.CachePoseName = TEXT("TemplateGround");
	Cache->AllocateDefaultPins(); B.Pose(Ground, Cache, TEXT("Pose"));
	const auto Cached = [&B, Cache]()
	{
		auto* Use = B.Node<UAnimGraphNode_UseCachedPose>(TEXT("读取本帧地面姿态缓存。"));
		Use->SaveCachedPoseNode = Cache; Use->Node.CachePoseName = TEXT("TemplateGround"); Use->AllocateDefaultPins(); return Use;
	};
	auto* Air = B.Select(B.Sequence(C[TEXT("TPJump")], false), B.Sequence(C[TEXT("TPFall")]), TEXT("bJumpRising"));
	auto* Landing = B.Select(B.Add(Cached(), B.Sequence(C[TEXT("TPLand")], false)), Cached(), TEXT("bLanding"));
	auto* Lower = B.Select(Air, Landing, TEXT("bFalling"));
	auto* Layer = B.Node<UAnimGraphNode_LayeredBoneBlend>(TEXT("下半身走跑，上半身使用同套 AR 姿态；枪直接跟随真实 hand_r，不借用 RAR 独立枪链。"));
	FBranchFilter Spine; Spine.BoneName = TEXT("spine_01"); Spine.BlendDepth = 1;
	Layer->Node.LayerSetup[0].BranchFilters = {Spine}; Layer->Node.bMeshSpaceRotationBlend = true;
	Layer->AllocateDefaultPins(); B.Pose(Lower, Layer, TEXT("BasePose"));
	auto* Action = B.Slot(Upper, TEXT("TemplateAction"));
	auto* Aim = B.Node<UAnimGraphNode_RotationOffsetBlendSpace>(TEXT("使用同骨架作者 AimOffset 跟随复制俯仰，避免手工旋脊柱后枪链留在原处。"));
	Aim->Node.SetBlendSpace(AimOffset); Aim->AllocateDefaultPins(); B.Pose(Action, Aim, TEXT("BasePose"));
	B.Value(TEXT("AimPitch"), Aim, TEXT("Y")); B.Pose(Aim, Layer, TEXT("BlendPoses_0"));
	auto* Weight = B.Node<UK2Node_CallFunction>(TEXT("只读装备镜像转换为持枪上半身权重。"));
	Weight->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_BoolToDouble")));
	Weight->AllocateDefaultPins(); B.Value(TEXT("bArmed"), Weight, TEXT("InBool"));
	B.Link(Weight->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Layer->FindPin(TEXT("BlendWeights_0")));
	// 受击是角色已发生的事实，不属于持枪姿态；空手也必须能播放，且不能叠加两次。
	FinishGraph(B, B.AddMesh(Layer, B.Hit(C[TEXT("Hit")]), TEXT("HitAlpha")));
	return B.bOK;
}

// 只对新命名空间创建一次；保留旧 RAR、原始 HUD 及所有用户蓝图注释，重复运行不会覆盖编辑成果。
void Prepare(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr || (Args.Num() && !bApply)) return;
	auto* Body = Load<USkeletalMesh>(TEXT("/Game/FPSMPT55/Demo/Characters/Mannequins/Meshes/SKM_Manny_Simple"));
	auto* Gun = Load<USkeletalMesh>(TEXT("/Game/FPSMPT55/Weapons/AssaultRifle/SKM_AssaultRifle"));
	auto* OldPlayer = Load<UBlueprint>(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter"));
	auto* OldConfig = Load<UfpstrueWeaponConfig>(TEXT("/Game/Config/DA_FPWeapon_RAR"));
	auto* Unarmed = Load<UBlendSpace>(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Animations/Manny/BS_MM_WalkRun"));
	auto* AimOffset = Load<UBlendSpace>(Art + TEXT("TPP/AR/AO_TPP_AR"));
	if (!Body || !Gun || !OldPlayer || !OldPlayer->GeneratedClass || !OldConfig || !Unarmed || !AimOffset)
	{ Error(TEXT("Template preflight: required source assets missing.")); return; }
	const TArray<FClip> Definitions = {
		{TEXT("FPIdle"), Art + TEXT("FPP/AR/A_FPP_AR_Idle"), false},
		{TEXT("FPWalk"), Art + TEXT("FPP/AR/A_FPP_AR_Walk"), false},
		{TEXT("FPAim"), Art + TEXT("FPP/AR/A_FPP_AR_Ironsight_Pose"), false},
		{TEXT("FPSprint"), Art + TEXT("FPP/AR/A_FPP_AR_Tactical_Sprint"), false},
		{TEXT("FPFire"), Art + TEXT("FPP/AR/A_FPP_AR_Shot"), true},
		{TEXT("FPFireAimed"), Art + TEXT("FPP/AR/A_FPP_AR_Ironsight_Shot"), true},
		{TEXT("FPReload"), Art + TEXT("FPP/AR/A_FPP_AR_Reload"), true},
		{TEXT("FPEquip"), Art + TEXT("FPP/AR/A_FPP_AR_Equip"), false},
		{TEXT("FPJump"), Art + TEXT("FPP/AR/A_FPP_AR_Jump_Start"), false},
		{TEXT("FPFall"), Art + TEXT("FPP/AR/A_FPP_AR_Jump_loop"), false},
		{TEXT("TPIdle"), Art + TEXT("TPP/AR/A_TPP_AR_Idle"), false},
		{TEXT("TPAim"), Art + TEXT("TPP/AR/A_TPP_AR_Ironsight"), false},
		{TEXT("TPSprint"), Art + TEXT("TPP/AR/A_TPP_Ar_Tactical_Sprint"), false},
		{TEXT("TPFire"), Art + TEXT("TPP/AR/A_TPP_AR_Shot"), true},
		{TEXT("TPReload"), Art + TEXT("TPP/AR/A_TPP_AR_Fast_Reload"), false},
		{TEXT("TPJump"), Art + TEXT("TPP/Locomotion/Jump/A_TPP_Jump_Start"), false},
		{TEXT("TPFall"), Art + TEXT("TPP/Locomotion/Jump/A_TPP_Jump_Loop"), false},
		{TEXT("TPLand"), Art + TEXT("TPP/Locomotion/Jump/A_TPP_Jump_End"), true},
		{TEXT("Hit"), Art + TEXT("TPP/HitReact/A_MM_HitReact_Front_Med_01_Additive"), true},
		{TEXT("WalkF"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Walk_Fwd"), false},
		{TEXT("WalkB"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Walk_Bwd"), false},
		{TEXT("WalkL"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Walk_Left"), false},
		{TEXT("WalkR"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Walk_Right"), false},
		{TEXT("JogF"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Jog_Fwd"), false},
		{TEXT("JogB"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Jog_Bwd"), false},
		{TEXT("JogL"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Jog_Left"), false},
		{TEXT("JogR"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Jog_Right"), false},
		{TEXT("GunFire"), Art + TEXT("WeaponAnimations/AssaultRifle/A_AR_Shot"), false},
		{TEXT("GunReload"), Art + TEXT("WeaponAnimations/AssaultRifle/A_AR_Reload"), false}
	};
	TMap<FName, UAnimSequence*> Clips;
	for (const FClip& D : Definitions)
	{
		auto* Clip = Load<UAnimSequence>(D.Source);
		if (!Clip || Clip->IsValidAdditive() != D.bAdditive || Clip->Notifies.Num() != 0)
		{ Error(TEXT("Template clip missing/unsafe/additive mismatch: ") + D.Source); return; }
		Clips.Add(D.Key, Clip);
	}
	// 空手 BlendSpace 使用旧同骨架数据的序列前必须验证实际参考骨骼兼容，不用字符串同名冒充兼容。
	if (!Body->GetSkeleton()->IsCompatibleForEditor(Unarmed->GetSkeleton()))
	{
		auto* ExistingBody = Load<USkeletalMesh>(TEXT("/Game/InfimaGames/ArtCore/Mannequin/Meshes/SKM_Manny_Simple"));
		if (!ExistingBody || !SameReferencePose(Body, ExistingBody)) return;
	}
	if (!bApply) { UE_LOG(LogTemp, Display, TEXT("TEMPLATE_PRESENTATION PASS SourceClips=%d ReadOnly=true"), Clips.Num()); return; }
	TArray<FString> Outputs = {PlayerPath, TEXT("/Game/Config/DA_FPWeapon_Template")};
	for (const TCHAR* Name : {TEXT("ABP_TemplateBody"), TEXT("ABP_TemplateArms"), TEXT("ABP_TemplateWeapon"), TEXT("BS_TemplateDirections"),
		TEXT("AM_Template_FP_Fire"), TEXT("AM_Template_FP_FireAimed"), TEXT("AM_Template_FP_Reload"), TEXT("AM_Template_FP_Equip"),
		TEXT("AM_Template_TP_Fire"), TEXT("AM_Template_TP_Reload"), TEXT("AM_TemplateWeapon_Fire"), TEXT("AM_TemplateWeapon_Reload"), TEXT("AM_TemplateWeapon_ReloadEmpty")})
		Outputs.Add(Destination + Name);
	for (const FString& Path : Outputs)
		if (FPackageName::DoesPackageExist(Path)) { Error(TEXT("Template output exists; refusing overwrite: ") + Path); return; }
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/TemplatePresentation") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	for (USkeleton* Skeleton : {Body->GetSkeleton(), Gun->GetSkeleton()})
	{
		if (Skeleton->GetOutermost()->IsDirty()) { Error(TEXT("Template skeleton has unsaved changes.")); return; }
		if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Skeleton->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	}
	TArray<UPackage*> Packages;
	Body->GetSkeleton()->AddCompatibleSkeleton(Unarmed->GetSkeleton());
	for (USkeleton* Skeleton : {Body->GetSkeleton(), Gun->GetSkeleton()})
	{
		Skeleton->RegisterSlotNode(TEXT("TemplateAction")); Skeleton->SetSlotGroupName(TEXT("TemplateAction"), FAnimSlotGroup::DefaultGroupName);
		Skeleton->MarkPackageDirty(); Packages.AddUnique(Skeleton->GetOutermost());
	}
	auto* Config = NewAsset<UfpstrueWeaponConfig>(TEXT("/Game/Config/DA_FPWeapon_Template"), Packages);
	Config->Settings = OldConfig->Settings; Config->Settings.GripSocketName = TEXT("hand_r");
	Config->Settings.ReloadDuration = Clips[TEXT("FPReload")]->GetPlayLength();
	Config->Settings.EmptyReloadDuration = Config->Settings.ReloadDuration;
	auto* Directions = NewAsset<UBlendSpace>(Destination + TEXT("BS_TemplateDirections"), Packages); Directions->SetSkeleton(Body->GetSkeleton());
	auto* Parameters = FindFProperty<FProperty>(Directions->GetClass(), TEXT("BlendParameters"));
	if (!Parameters) { Error(TEXT("BlendSpace parameters unavailable.")); return; }
	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		auto* P = Parameters->ContainerPtrToValuePtr<FBlendParameter>(Directions, Axis);
		P->DisplayName = Axis == 0 ? TEXT("Forward speed") : TEXT("Right speed"); P->Min = -600; P->Max = 600; P->GridNum = 8;
	}
	Directions->AddSample(Clips[TEXT("TPIdle")], FVector::ZeroVector);
	const FName Walk[] = {TEXT("WalkF"), TEXT("WalkB"), TEXT("WalkL"), TEXT("WalkR")};
	const FName Jog[] = {TEXT("JogF"), TEXT("JogB"), TEXT("JogL"), TEXT("JogR")};
	const FVector Axes[] = {FVector(1,0,0), FVector(-1,0,0), FVector(0,-1,0), FVector(0,1,0)};
	for (int32 I = 0; I < 4; ++I) { Directions->AddSample(Clips[Walk[I]], Axes[I] * 300); Directions->AddSample(Clips[Jog[I]], Axes[I] * 600); }
	Directions->ValidateSampleData(); Directions->ResampleData();
	auto* FP = AnimBlueprint(TEXT("ABP_TemplateArms"), UfpstruePlayerAnimInstance::StaticClass(), Body, Packages, Destination);
	auto* TP = AnimBlueprint(TEXT("ABP_TemplateBody"), UfpstruePlayerAnimInstance::StaticClass(), Body, Packages, Destination);
	auto* WP = AnimBlueprint(TEXT("ABP_TemplateWeapon"), UAnimInstance::StaticClass(), Gun, Packages, Destination);
	if (!FP || !TP || !WP || !BuildPlayer(FP, Clips, Directions, Unarmed, AimOffset, true)
		|| !BuildPlayer(TP, Clips, Directions, Unarmed, AimOffset, false)) return;
	FGraphBuilder W = Builder(WP);
	auto* Idle = W.Node<UAnimGraphNode_SequenceEvaluator>(TEXT("枪械常态固定在完整换弹序列的最后装配姿态，不循环换弹，也不发通知。"));
	Idle->Node.SetSequence(Clips[TEXT("GunReload")]); Idle->Node.SetShouldLoop(false);
	Idle->Node.SetExplicitTime(Clips[TEXT("GunReload")]->GetPlayLength()); Idle->AllocateDefaultPins();
	FinishGraph(W, W.Slot(Idle, TEXT("TemplateAction")));
	for (UAnimBlueprint* BP : {FP, TP, WP}) if (!FpstruePvpAssetPreparation::Compile(BP)) return;
	for (UAnimBlueprint* BP : {FP, TP})
	{
		const bool bFP = BP == FP; const FString Prefix = bFP ? TEXT("FP") : TEXT("TP");
		auto* D = Cast<UfpstruePlayerAnimInstance>(BP->GeneratedClass->GetDefaultObject());
		D->FireMontage = Montage(TEXT("AM_Template_") + Prefix + TEXT("_Fire"), Clips[FName(*(Prefix + TEXT("Fire")))], Packages, Destination, TEXT("TemplateAction"));
		D->AimedFireMontage = bFP ? Montage(TEXT("AM_Template_FP_FireAimed"), Clips[TEXT("FPFireAimed")], Packages, Destination, TEXT("TemplateAction")) : D->FireMontage.Get();
		D->ReloadMontage = Montage(TEXT("AM_Template_") + Prefix + TEXT("_Reload"), Clips[FName(*(Prefix + TEXT("Reload")))], Packages, Destination, TEXT("TemplateAction"));
		D->EmptyReloadMontage = D->ReloadMontage;
		D->EquipMontage = bFP ? Montage(TEXT("AM_Template_FP_Equip"), Clips[TEXT("FPEquip")], Packages, Destination, TEXT("TemplateAction")) : nullptr;
	}
	for (const TCHAR* Name : {TEXT("Fire"), TEXT("Reload"), TEXT("ReloadEmpty")})
		Montage(FString(TEXT("AM_TemplateWeapon_")) + Name, Clips[FString(Name) == TEXT("Fire") ? TEXT("GunFire") : TEXT("GunReload")], Packages, Destination, TEXT("TemplateAction"));
	UPackage* Outer = CreatePackage(PlayerPath);
	auto* Player = FKismetEditorUtilities::CreateBlueprint(AfpstrueCharacter::StaticClass(), Outer, TEXT("BP_TemplateCharacter"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("FpstrueTemplatePresentation"));
	if (!Player || !FpstruePvpAssetPreparation::Compile(Player)) return;
	Packages.AddUnique(Outer); FAssetRegistryModule::AssetCreated(Player);
	auto* D = Cast<AfpstrueCharacter>(Player->GeneratedClass->GetDefaultObject());
	for (const TCHAR* Name : {TEXT("DefaultMappingContext"), TEXT("JumpAction"), TEXT("MoveAction"), TEXT("LookAction"), TEXT("FireAction"), TEXT("ReloadAction"), TEXT("SprintAction"), TEXT("AimAction"), TEXT("WalkSpeed"), TEXT("SprintSpeed"), TEXT("AimWalkSpeed")})
	{
		auto* P = FindFProperty<FProperty>(AfpstrueCharacter::StaticClass(), Name);
		if (!P) return; P->CopyCompleteValue_InContainer(D, OldPlayer->GeneratedClass->GetDefaultObject());
	}
	FindFProperty<FBoolProperty>(AfpstrueCharacter::StaticClass(), TEXT("bUseTemplatePresentation"))->SetPropertyValue_InContainer(D, true);
	D->GetMesh()->SetSkeletalMesh(Body); D->GetMesh()->SetAnimInstanceClass(TP->GeneratedClass);
	D->GetMesh()->SetRelativeLocationAndRotation(FVector(0,0,-90.556555), FRotator(0,-90,0));
	D->GetMesh1P()->SetSkeletalMesh(Body); D->GetMesh1P()->SetAnimInstanceClass(FP->GeneratedClass);
	D->GetMesh1P()->SetRelativeLocationAndRotation(FVector(-0.663123,0,-162.575101), FRotator(0,-90,0));
	for (UPackage* Package : Packages) Package->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) { Error(TEXT("Template save failed.")); return; }
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_PRESENTATION PASS Generated=%d Player=%s"), Packages.Num(), PlayerPath);
}

FAutoConsoleCommand Command(TEXT("fp.PrepareTemplatePresentation"), TEXT("Check UE5.5 art; Apply creates native-based FP/TP presentation without importing supplier gameplay."), FConsoleCommandWithArgsDelegate::CreateStatic(&Prepare));

// 只读实测已保存的模板，不重新生成蓝图、不编译或保存素材；足骨高度不冒充鞋底高度。
void AuditGrounding(const TArray<FString>& Args)
{
	if (Args.Num() != 0 || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		Error(TEXT("Grounding audit is read-only and requires an idle editor; no arguments/Apply supported."));
		return;
	}
	auto* BP = Load<UBlueprint>(PlayerPath);
	auto* CDO = BP && BP->GeneratedClass ? Cast<AfpstrueCharacter>(BP->GeneratedClass->GetDefaultObject()) : nullptr;
	auto* Mesh = CDO ? CDO->GetMesh() : nullptr;
	auto* Body = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	auto* Capsule = CDO ? CDO->GetCapsuleComponent() : nullptr;
	if (!Body || !Capsule || !Body->GetSkeleton())
	{
		Error(TEXT("Grounding audit: saved Template BP/body/capsule/skeleton missing."));
		return;
	}
	const FTransform MeshRelative = Mesh->GetRelativeTransform();
	const double HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND CDO BP=%s CapsuleRadius=%.4f HalfHeight=%.4f CapsuleScale=%s Mesh=%s RelativeLocation=%s RelativeRotation=%s RelativeScale=%s AnimClass=%s MeshPostProcess=%s DisablePostProcess=%d ReadOnly=true"),
		*BP->GetPathName(), Capsule->GetUnscaledCapsuleRadius(), HalfHeight, *Capsule->GetRelativeScale3D().ToString(),
		*Body->GetPathName(), *MeshRelative.GetLocation().ToString(), *MeshRelative.Rotator().ToString(), *MeshRelative.GetScale3D().ToString(),
		*GetPathNameSafe(Mesh->GetAnimClass()), *GetPathNameSafe(Body->GetPostProcessAnimBlueprint()), Mesh->GetDisablePostProcessBlueprint());
	// These are author-space samples before retargeting/root extraction/AnimGraph/postprocess, not runtime sole measurements.
	const TArray<FString> ClipPaths = {
		Art + TEXT("TPP/AR/A_TPP_AR_Idle"),
		Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Walk_Fwd"),
		Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Jog_Fwd")};
	for (const FString& Path : ClipPaths)
	{
		auto* Clip = Load<UAnimSequence>(Path);
		if (!Clip || !Clip->GetSkeleton()) { Error(TEXT("Grounding audit clip missing: ") + Path); return; }
		const FReferenceSkeleton& Ref = Clip->GetSkeleton()->GetReferenceSkeleton();
		const auto RawComponentBone = [Clip, &Ref](int32 Index, double Time)
		{
			FTransform Component = FTransform::Identity;
			for (int32 Bone = Index; Bone != INDEX_NONE; Bone = Ref.GetParentIndex(Bone))
			{
				FTransform Local = Ref.GetRefBonePose()[Bone];
				if (!Clip->GetDataModel() || Clip->GetDataModel()->IsValidBoneTrackName(Ref.GetBoneName(Bone)))
					Clip->GetBoneTransform(Local, FSkeletonPoseBoneIndex(Bone), Time, true);
				Component = Component * Local;
			}
			return Component;
		};
		const double Length = Clip->GetPlayLength();
		const FVector RootStart = RawComponentBone(0, 0).GetLocation();
		const FVector RootEnd = RawComponentBone(0, Length).GetLocation();
		const FTransform Extracted = Clip->ExtractRootMotion(0, Length, false);
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND CLIP Path=%s Length=%.4f Skeleton=%s Additive=%d EnableRootMotion=%d ForceRootLock=%d RootLock=%d RawRootStart=%s RawRootEnd=%s RawRootDelta=%s RawRootPlanarSpeed=%.4f ExtractedRootDelta=%s"),
			*Clip->GetPathName(), Length, *GetPathNameSafe(Clip->GetSkeleton()), static_cast<int32>(Clip->GetAdditiveAnimType()),
			Clip->bEnableRootMotion, Clip->bForceRootLock, static_cast<int32>(Clip->RootMotionRootLock.GetValue()),
			*RootStart.ToString(), *RootEnd.ToString(), *(RootEnd - RootStart).ToString(), Length > 0 ? (RootEnd - RootStart).Size2D() / Length : 0,
			*Extracted.GetTranslation().ToString());
		for (const TCHAR* Name : {TEXT("root"), TEXT("pelvis"), TEXT("foot_l"), TEXT("foot_r"), TEXT("ball_l"), TEXT("ball_r")})
		{
			const int32 Index = Ref.FindBoneIndex(Name);
			if (Index == INDEX_NONE) { Error(FString(TEXT("Grounding bone missing: ")) + Name); return; }
			double RawMinZ = TNumericLimits<double>::Max(), RawMaxZ = TNumericLimits<double>::Lowest();
			double CDOFloorMinZ = TNumericLimits<double>::Max(), CDOFloorMaxZ = TNumericLimits<double>::Lowest();
			for (int32 Sample = 0; Sample <= 30; ++Sample)
			{
				const FVector Raw = RawComponentBone(Index, Length * Sample / 30.0).GetLocation();
				const double FloorZ = MeshRelative.TransformPosition(Raw).Z + HalfHeight;
				RawMinZ = FMath::Min(RawMinZ, Raw.Z); RawMaxZ = FMath::Max(RawMaxZ, Raw.Z);
				CDOFloorMinZ = FMath::Min(CDOFloorMinZ, FloorZ); CDOFloorMaxZ = FMath::Max(CDOFloorMaxZ, FloorZ);
			}
			UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND RAW_BONE Clip=%s Bone=%s MinZ=%.4f MaxZ=%.4f CDOUnscaledCapsuleFloorMinZ=%.4f CDOUnscaledCapsuleFloorMaxZ=%.4f BoneNotSole=true BeforeRootLockRetargetGraphPostProcess=true"),
				*Clip->GetName(), Name, RawMinZ, RawMaxZ, CDOFloorMinZ, CDOFloorMaxZ);
		}
	}
	if (auto* Directions = Load<UBlendSpace>(Destination + TEXT("BS_TemplateDirections")))
		for (const FBlendSample& Sample : Directions->GetBlendSamples())
			UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND BLENDSPACE Sample=%s Position=%s RateScale=%.4f"),
				*GetPathNameSafe(Sample.Animation), *Sample.SampleValue.ToString(), Sample.RateScale);
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND PASS ReadOnly=true AssetsSaved=false RawBoneNotRuntimeSole=true"));
}
FAutoConsoleCommand GroundAuditCommand(TEXT("fp.AuditTemplateGrounding"),
	TEXT("Read saved Template BP mesh/capsule/postprocess and raw idle/walk/jog root/foot metadata without modifying assets."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&AuditGrounding));

// 转身素材先只读验源：比较实际玩家网格的骨架，不把资产名称、兼容列表或脚骨枢轴当成转身与贴地证据。
void AuditTurnSources(const TArray<FString>& Args)
{
	if (Args.Num() != 0 || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{ Error(TEXT("Turn source audit is read-only; usage: fp.AuditTemplateTurnSources, no active PIE.")); return; }
	auto* BP = Load<UBlueprint>(PlayerPath);
	auto* Defaults = BP && BP->GeneratedClass ? Cast<AfpstrueCharacter>(BP->GeneratedClass->GetDefaultObject()) : nullptr;
	auto* TargetMesh = Defaults && Defaults->GetMesh() ? Defaults->GetMesh()->GetSkeletalMeshAsset() : nullptr;
	auto* TargetSkeleton = TargetMesh ? TargetMesh->GetSkeleton() : nullptr;
	if (!TargetSkeleton) { Error(TEXT("Turn source audit: saved Template body skeleton missing.")); return; }
	const FReferenceSkeleton& Target = TargetSkeleton->GetReferenceSkeleton();
	for (const TCHAR* Name : {TEXT("AS_Turn_L_90_Seq"), TEXT("AS_Turn_R_90_Seq")})
	{
		const FString Path = FString(TEXT("/Game/Mage/Animations/Sequence2/09_Turn/01_Turn/")) + Name;
		auto* Clip = Load<UAnimSequence>(Path);
		if (!Clip || !Clip->GetSkeleton() || Clip->GetPlayLength() <= 0 || !Clip->GetDataModel())
		{ Error(TEXT("Turn source audit: migrated source sequence/model missing: ") + Path); return; }
		const FReferenceSkeleton& Source = Clip->GetSkeleton()->GetReferenceSkeleton();
		bool bHierarchyMatch = Source.GetRawBoneNum() == Target.GetRawBoneNum();
		bool bReferenceMatch = bHierarchyMatch;
		int32 HierarchyMismatches = 0, ReferenceMismatches = 0;
		for (int32 Index = 0; Index < Source.GetRawBoneNum(); ++Index)
		{
			const FName BoneName = Source.GetBoneName(Index);
			const int32 TargetIndex = Target.FindBoneIndex(BoneName);
			const int32 SourceParent = Source.GetParentIndex(Index);
			const int32 TargetParent = TargetIndex != INDEX_NONE ? Target.GetParentIndex(TargetIndex) : INDEX_NONE;
			const FName SourceParentName = SourceParent != INDEX_NONE ? Source.GetBoneName(SourceParent) : NAME_None;
			const FName TargetParentName = TargetParent != INDEX_NONE ? Target.GetBoneName(TargetParent) : NAME_None;
			if (TargetIndex == INDEX_NONE || SourceParentName != TargetParentName)
			{
				bHierarchyMatch = false; ++HierarchyMismatches;
				UE_LOG(LogTemp, Display, TEXT("TEMPLATE_TURN HIERARCHY_MISMATCH Clip=%s Bone=%s SourceIndex=%d TargetIndex=%d SourceParent=%s TargetParent=%s"),
					*Clip->GetName(), *BoneName.ToString(), Index, TargetIndex, *SourceParentName.ToString(), *TargetParentName.ToString());
			}
			if (TargetIndex == INDEX_NONE || !Source.GetRefBonePose()[Index].Equals(Target.GetRefBonePose()[TargetIndex], 0.001f))
			{ bReferenceMatch = false; ++ReferenceMismatches; }
		}
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_TURN SKELETON Clip=%s Source=%s Target=%s SourceRawBones=%d TargetRawBones=%d SameBoneParentNames=%d HierarchyMismatches=%d SameReferencePose=%d ReferenceMismatches=%d"),
			*Clip->GetName(), *Clip->GetSkeleton()->GetPathName(), *TargetSkeleton->GetPathName(), Source.GetRawBoneNum(), Target.GetRawBoneNum(),
			bHierarchyMatch, HierarchyMismatches, bReferenceMatch, ReferenceMismatches);
		const auto RawBone = [Clip, &Source](int32 Bone, double Time, bool bComponentSpace)
		{
			FTransform Result = FTransform::Identity;
			for (int32 Index = Bone; Index != INDEX_NONE; Index = bComponentSpace ? Source.GetParentIndex(Index) : INDEX_NONE)
			{
				FTransform Local = Source.GetRefBonePose()[Index];
				if (Clip->GetDataModel()->IsValidBoneTrackName(Source.GetBoneName(Index)))
					Clip->GetBoneTransform(Local, FSkeletonPoseBoneIndex(Index), Time, true);
				Result = Result * Local;
			}
			return Result;
		};
		const int32 RootIndex = Source.FindBoneIndex(TEXT("root"));
		if (RootIndex == INDEX_NONE) { Error(TEXT("Turn source root bone missing: ") + Path); return; }
		const double Length = Clip->GetPlayLength();
		const FTransform RootStart = RawBone(RootIndex, 0, false);
		const FTransform RootEnd = RawBone(RootIndex, Length, false);
		const FTransform Extracted = Clip->ExtractRootMotion(0, Length, false);
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_TURN CLIP Path=%s Length=%.6f RateScale=%.4f Additive=%d EnableRootMotion=%d ForceRootLock=%d RootLock=%d RawRootYawStart=%.4f RawRootYawEnd=%.4f RawRootYawDelta=%.4f ExtractedYaw=%.4f ExtractedTranslation=%s"),
			*Clip->GetPathName(), Length, Clip->RateScale, static_cast<int32>(Clip->GetAdditiveAnimType()), Clip->bEnableRootMotion, Clip->bForceRootLock,
			static_cast<int32>(Clip->RootMotionRootLock.GetValue()), RootStart.Rotator().Yaw, RootEnd.Rotator().Yaw,
			FMath::FindDeltaAngleDegrees(RootStart.Rotator().Yaw, RootEnd.Rotator().Yaw), Extracted.Rotator().Yaw, *Extracted.GetLocation().ToString());
		for (int32 Sample = 0; Sample <= 10; ++Sample)
		{
			const double Time = Length * Sample / 10.0;
			const FTransform RawRoot = RawBone(RootIndex, Time, false);
			const FTransform RootDelta = Clip->ExtractRootMotion(0, Time, false);
			UE_LOG(LogTemp, Display, TEXT("TEMPLATE_TURN SAMPLE Clip=%s Fraction=%.1f Time=%.6f RawRootLocation=%s RawRootRotation=%s RawRootDeltaYaw=%.4f ExtractedYaw=%.4f"),
				*Clip->GetName(), Sample / 10.0, Time, *RawRoot.GetLocation().ToString(), *RawRoot.Rotator().ToString(),
				FMath::FindDeltaAngleDegrees(RootStart.Rotator().Yaw, RawRoot.Rotator().Yaw), RootDelta.Rotator().Yaw);
			for (const TCHAR* BoneName : {TEXT("pelvis"), TEXT("foot_l"), TEXT("foot_r"), TEXT("ball_l"), TEXT("ball_r")})
			{
				const int32 Bone = Source.FindBoneIndex(BoneName);
				if (Bone == INDEX_NONE) { Error(FString(TEXT("Turn source diagnostic bone missing: ")) + BoneName); return; }
				const FTransform Component = RawBone(Bone, Time, true);
				const FTransform Local = RawBone(Bone, Time, false);
				UE_LOG(LogTemp, Display, TEXT("TEMPLATE_TURN BONE Clip=%s Fraction=%.1f Bone=%s ComponentLocation=%s ComponentRotation=%s LocalLocation=%s LocalRotation=%s BeforeRootExtractionRetargetGraph=true BoneNotSole=true"),
					*Clip->GetName(), Sample / 10.0, BoneName, *Component.GetLocation().ToString(), *Component.Rotator().ToString(),
					*Local.GetLocation().ToString(), *Local.Rotator().ToString());
			}
		}
	}
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_TURN AUDIT_PASS ReadOnly=true AssetsSaved=false GraphCompiled=false"));
}
FAutoConsoleCommand TurnSourceAuditCommand(TEXT("fp.AuditTemplateTurnSources"),
	TEXT("Read migrated left/right 90-degree turns, compare raw skeleton parents/reference poses, and sample raw root/pelvis/feet against extracted root yaw. Never modifies assets."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&AuditTurnSources));

struct FFacingRepairGraph
{
	FGraphBuilder Body;
	FGraphBuilder Arms;
	UAnimGraphNode_LayeredBoneBlend* Layer = nullptr;
	UEdGraphPin* LowerOutput = nullptr;
	UAnimGraphNode_TwoWayBlend* ArmsWalk = nullptr;
	bool bRepaired = false;
};

// 只有唯一连接时才返回上游姿态节点，供窄修复核实已有拓扑而非猜测节点名称。
UEdGraphNode* SinglePoseSource(UEdGraphPin* Input)
{
	return Input && Input->LinkedTo.Num() == 1 ? Input->LinkedTo[0]->GetOwningNode() : nullptr;
}

// 只接受现有项目图的已知连接或本修复的完整连接；不按节点注释猜测，不重建用户图。
bool InspectFacingGraphs(UAnimBlueprint* Body, UAnimBlueprint* Arms, FFacingRepairGraph& Result)
{
	for (UAnimBlueprint* Blueprint : {Body, Arms})
		if (!Blueprint || Blueprint->ParentClass != UfpstruePlayerAnimInstance::StaticClass() || !Blueprint->GeneratedClass
			|| Blueprint->GetOutermost()->IsDirty()) return Error(TEXT("Facing repair requires saved native Template Body/Arms AnimBPs."));
	Result.Body = Builder(Body); Result.Arms = Builder(Arms);
	if (!Result.Body.bOK || !Result.Arms.bOK) return Error(TEXT("Facing repair AnimGraph missing."));
	int32 Layers = 0, Roots = 0, RootRotations = 0, WalkBlends = 0;
	UAnimGraphNode_Root* GraphRoot = nullptr;
	for (UEdGraphNode* Node : Result.Body.Graph->Nodes)
	{
		if (auto* Layer = Cast<UAnimGraphNode_LayeredBoneBlend>(Node)) { ++Layers; Result.Layer = Layer; }
		if (auto* RootNode = Cast<UAnimGraphNode_Root>(Node)) { ++Roots; GraphRoot = RootNode; }
		if (Cast<UAnimGraphNode_RotateRootBone>(Node)) ++RootRotations;
	}
	if (Layers != 1 || Roots != 1 || RootRotations > 1 || Result.Layer->Node.LayerSetup.Num() != 1
		|| !Result.Layer->Node.bMeshSpaceRotationBlend || Result.Layer->Node.LayerSetup[0].BranchFilters.Num() != 1
		|| Result.Layer->Node.LayerSetup[0].BranchFilters[0].BoneName != TEXT("spine_01"))
		return Error(TEXT("Facing repair: unexpected Template body layer/root layout; custom graphs are not overwritten."));
	auto* Hit = Cast<UAnimGraphNode_ApplyMeshSpaceAdditive>(SinglePoseSource(GraphRoot->FindPin(TEXT("Result"))));
	auto* Aim = Cast<UAnimGraphNode_RotationOffsetBlendSpace>(SinglePoseSource(Result.Layer->FindPin(TEXT("BlendPoses_0"))));
	auto* Slot = Aim ? Cast<UAnimGraphNode_Slot>(SinglePoseSource(Aim->FindPin(TEXT("BasePose")))) : nullptr;
	if (!Hit || SinglePoseSource(Hit->FindPin(TEXT("Base"))) != Result.Layer || !ReadsNativeValue(Hit->FindPin(TEXT("Alpha")), TEXT("HitAlpha"))
		|| !Aim || !ReadsNativeValue(Aim->FindPin(TEXT("Y")), TEXT("AimPitch")) || !Slot || Slot->Node.SlotName != TEXT("TemplateAction"))
		return Error(TEXT("Facing repair: original Slot/PitchAim/layer/hit chain differs; no changes made."));
	for (UEdGraphNode* Node : Result.Arms.Graph->Nodes)
	{
		auto* Blend = Cast<UAnimGraphNode_TwoWayBlend>(Node);
		auto* Walk = Blend ? Cast<UAnimGraphNode_SequencePlayer>(SinglePoseSource(Blend->FindPin(TEXT("B")))) : nullptr;
		if (Walk && Walk->Node.GetSequence() && Walk->Node.GetSequence()->GetPathName() ==
			Art + TEXT("FPP/AR/A_FPP_AR_Walk.A_FPP_AR_Walk"))
		{ ++WalkBlends; Result.ArmsWalk = Blend; }
	}
	if (WalkBlends != 1) return Error(TEXT("Facing repair: expected exactly one original FP movement-sway blend."));
	UEdGraphPin* Base = Result.Layer->FindPin(TEXT("BasePose"));
	if (Base == nullptr || Base->LinkedTo.Num() != 1) return Error(TEXT("Facing repair: unique lower-body pose input missing."));
	auto* Rotation = Cast<UAnimGraphNode_RotateRootBone>(SinglePoseSource(Base));
	Result.bRepaired = Rotation != nullptr;
	if (!Result.bRepaired)
	{
		auto* Lower = Cast<UAnimGraphNode_BlendListByBool>(SinglePoseSource(Base));
		if (RootRotations != 0 || !Lower || !ReadsNativeValue(Lower->FindPin(TEXT("bActiveValue")), TEXT("bFalling"))
			|| !ReadsNativeValue(Result.ArmsWalk->FindPin(TEXT("Alpha")), TEXT("MovementAlpha")))
			return Error(TEXT("Facing repair: original falling/movement blend inputs differ."));
		Result.LowerOutput = Base->LinkedTo[0];
		return true;
	}
	auto* TurnBlend = Cast<UAnimGraphNode_TwoWayBlend>(SinglePoseSource(Rotation->FindPin(TEXT("BasePose"))));
	auto* Direction = TurnBlend ? Cast<UAnimGraphNode_BlendListByBool>(SinglePoseSource(TurnBlend->FindPin(TEXT("B")))) : nullptr;
	auto* Original = TurnBlend ? Cast<UAnimGraphNode_BlendListByBool>(SinglePoseSource(TurnBlend->FindPin(TEXT("A")))) : nullptr;
	if (RootRotations != 1 || !ReadsNativeValue(Rotation->FindPin(TEXT("Yaw")), TEXT("RootYawOffset"))
		|| !TurnBlend || !ReadsNativeValue(TurnBlend->FindPin(TEXT("Alpha")), TEXT("TurnAlpha"))
		|| !Direction || !ReadsNativeValue(Direction->FindPin(TEXT("bActiveValue")), TEXT("bTurnRight"))
		|| !Original || !ReadsNativeValue(Original->FindPin(TEXT("bActiveValue")), TEXT("bFalling"))
		|| Result.ArmsWalk->FindPin(TEXT("Alpha"))->LinkedTo.Num() != 0
		|| !FMath::IsNearlyZero(Result.ArmsWalk->BlendNode.Alpha))
		return Error(TEXT("Facing repair: partially modified facing chain or FP sway; refusing overwrite."));
	for (int32 Side = 0; Side < 2; ++Side)
	{
		auto* Evaluator = Cast<UAnimGraphNode_SequenceEvaluator>(SinglePoseSource(Direction->FindPin(Side == 0 ? TEXT("BlendPose_0") : TEXT("BlendPose_1"))));
		const FString Expected = Destination + (Side == 0 ? TEXT("A_TemplateTurnRight.A_TemplateTurnRight") : TEXT("A_TemplateTurnLeft.A_TemplateTurnLeft"));
		if (!Evaluator || !Evaluator->Node.GetSequence() || Evaluator->Node.GetSequence()->GetPathName() != Expected
			|| !Evaluator->Node.GetTeleportToExplicitTime() || Evaluator->Node.IsLooping() || Evaluator->Node.ShouldUseExplicitFrame()
			|| Evaluator->Node.GetGroupName() != NAME_None || Evaluator->Node.GetGroupMethod() != EAnimSyncMethod::DoNotSync
			|| !ReadsNativeValue(Evaluator->FindPin(TEXT("ExplicitTime")), TEXT("TurnTime")))
			return Error(TEXT("Facing repair: turn evaluators must use explicit TurnTime without looping, notifies or root extraction."));
	}
	return true;
}

// 复制并重映射的只是本项目转身序列；供货方序列、源骨架及现有走跑/蒙太奇不作修改。
void RepairFacing(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((Args.Num() > 0 && !bApply) || GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{ Error(TEXT("Usage: fp.RepairTemplateFacing [Apply], no active PIE.")); return; }
	TStrongObjectPtr<UAnimBlueprint> Body(Load<UAnimBlueprint>(Destination + TEXT("ABP_TemplateBody")));
	TStrongObjectPtr<UAnimBlueprint> Arms(Load<UAnimBlueprint>(Destination + TEXT("ABP_TemplateArms")));
	TStrongObjectPtr<UBlueprint> Player(Load<UBlueprint>(PlayerPath));
	auto* Defaults = Player.IsValid() && Player->GeneratedClass ? Cast<AfpstrueCharacter>(Player->GeneratedClass->GetDefaultObject()) : nullptr;
	auto* Mesh = Defaults && Defaults->GetMesh() ? Defaults->GetMesh()->GetSkeletalMeshAsset() : nullptr;
	FFacingRepairGraph Graph;
	if (!Mesh || !Mesh->GetSkeleton() || !InspectFacingGraphs(Body.Get(), Arms.Get(), Graph)) return;
	auto* LeftProperty = FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("TurnLeftAnimation"));
	auto* RightProperty = FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("TurnRightAnimation"));
	if (!LeftProperty || !RightProperty || !FindFProperty<FFloatProperty>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("RootYawOffset"))
		|| !FindFProperty<FFloatProperty>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("TurnTime"))
		|| !FindFProperty<FFloatProperty>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("TurnAlpha"))
		|| !FindFProperty<FBoolProperty>(UfpstruePlayerAnimInstance::StaticClass(), TEXT("bTurnRight")))
	{ Error(TEXT("Facing repair: native turn properties are missing.")); return; }
	const FString SourcePath = TEXT("/Game/Mage/Animations/Sequence2/09_Turn/01_Turn/");
	TStrongObjectPtr<UAnimSequence> Sources[] = {
		TStrongObjectPtr<UAnimSequence>(Load<UAnimSequence>(SourcePath + TEXT("AS_Turn_L_90_Seq"))),
		TStrongObjectPtr<UAnimSequence>(Load<UAnimSequence>(SourcePath + TEXT("AS_Turn_R_90_Seq")))};
	const FString Outputs[] = {Destination + TEXT("A_TemplateTurnLeft"), Destination + TEXT("A_TemplateTurnRight")};
	UAnimSequence* Turns[2] = {nullptr, nullptr};
	for (int32 Side = 0; Side < 2; ++Side)
	{
		auto* Source = Sources[Side].Get();
		if (!Source || !Source->GetSkeleton() || Source->IsValidAdditive() || Source->RefPoseSeq != nullptr
			|| Source->GetPlayLength() <= 0 || !Source->GetDataModel())
		{ Error(TEXT("Facing repair: source turn is missing or not an independent full-pose sequence.")); return; }
		const FTransform Delta = Source->ExtractRootMotion(0, Source->GetPlayLength(), false);
		const double ExpectedYaw = Side == 0 ? -90.0 : 90.0;
		if (FMath::Abs(FMath::FindDeltaAngleDegrees(Delta.Rotator().Yaw, ExpectedYaw)) > 1.0 || Delta.GetLocation().Size() > 0.1)
		{ Error(TEXT("Facing repair: source must contain the measured stationary left/right 90-degree turn.")); return; }
		if (Graph.bRepaired)
		{
			Turns[Side] = Load<UAnimSequence>(Outputs[Side]);
			if (!Turns[Side] || Turns[Side]->GetSkeleton() != Mesh->GetSkeleton() || Turns[Side]->bEnableRootMotion
				|| !Turns[Side]->bForceRootLock || !FMath::IsNearlyEqual(Turns[Side]->RateScale, 1.5f)
				|| Turns[Side]->GetPreviewMesh() != Mesh || !Turns[Side]->GetRetargetSourceAsset().IsNull())
			{ Error(TEXT("Facing repair: existing project turns have unexpected settings.")); return; }
		}
		else if (FPackageName::DoesPackageExist(Outputs[Side]))
		{ Error(TEXT("Facing repair refuses to replace an existing project turn output: ") + Outputs[Side]); return; }
	}
	auto* BodyDefaults = Body->GeneratedClass->GetDefaultObject();
	if (Graph.bRepaired && (LeftProperty->GetObjectPropertyValue_InContainer(BodyDefaults) != Turns[0]
		|| RightProperty->GetObjectPropertyValue_InContainer(BodyDefaults) != Turns[1]))
	{ Error(TEXT("Facing repair: body turn configuration does not match its graph.")); return; }
	if (!bApply || Graph.bRepaired)
	{
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_FACING PASS Mode=%s NeedsRepair=%d AssetsSaved=false LowerBodyOnly=true OriginalUpperSlotAimHitPreserved=true"),
			bApply ? TEXT("Apply") : TEXT("Check"), !Graph.bRepaired);
		return;
	}
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/TemplateFacing") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	for (UAnimBlueprint* Blueprint : {Body.Get(), Arms.Get()})
		if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	TArray<UPackage*> Packages;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		UPackage* Package = CreatePackage(*Outputs[Side]);
		Turns[Side] = DuplicateObject<UAnimSequence>(Sources[Side].Get(), Package, *FPackageName::GetShortName(Outputs[Side]));
		if (!Turns[Side]) { Error(TEXT("Facing repair: could not duplicate project turn.")); return; }
		Turns[Side]->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		Turns[Side]->ClearRetargetSourceAsset(); Turns[Side]->RetargetSource = NAME_None;
		Turns[Side]->SetPreviewMesh(Mesh, false);
		// 先重绑复制模型的 FK Rig；不改序列自身的源 Skeleton，随后仍可按原参考姿态转换到目标空间。
		Turns[Side]->GetController().UpdateWithSkeleton(Mesh->GetSkeleton(), false);
		if (!Turns[Side]->ReplaceSkeleton(Mesh->GetSkeleton(), true))
		{ Error(TEXT("Facing repair: project-only skeleton remapping failed; no assets saved.")); return; }
		Turns[Side]->bEnableRootMotion = false; Turns[Side]->bForceRootLock = true;
		Turns[Side]->RootMotionRootLock = ERootMotionRootLock::RefPose; Turns[Side]->RateScale = 1.5f;
		Turns[Side]->Notifies.Reset(); Turns[Side]->RefreshCacheData();
		const FTransform RemappedDelta = Turns[Side]->ExtractRootMotion(0, Turns[Side]->GetPlayLength(), false);
		if (FMath::Abs(FMath::FindDeltaAngleDegrees(RemappedDelta.Rotator().Yaw, Side == 0 ? -90.0 : 90.0)) > 1.0
			|| RemappedDelta.GetLocation().Size() > 0.1)
		{ Error(TEXT("Facing repair: remapping changed the measured stationary turn; no assets saved.")); return; }
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_FACING TURN_COPY Path=%s Skeleton=%s Length=%.4f RateScale=%.4f ExtractedYaw=%.4f Preview=%s NotifyCount=%d"),
			*Turns[Side]->GetPathName(), *Turns[Side]->GetSkeleton()->GetPathName(), Turns[Side]->GetPlayLength(), Turns[Side]->RateScale,
			RemappedDelta.Rotator().Yaw, *GetPathNameSafe(Turns[Side]->GetPreviewMesh()), Turns[Side]->Notifies.Num());
		FAssetRegistryModule::AssetCreated(Turns[Side]); Packages.Add(Package);
	}
	Body->Modify(); Arms->Modify(); Graph.Body.Graph->Modify(); Graph.Arms.Graph->Modify(); Graph.Layer->Modify(); Graph.ArmsWalk->Modify();
	const auto TurnEvaluator = [&Graph](UAnimSequence* Sequence)
	{
		auto* Evaluator = Graph.Body.Node<UAnimGraphNode_SequenceEvaluator>(TEXT("项目转身副本只采样显式 TurnTime；不循环、不发 Notify、不从动画移动胶囊。"));
		Evaluator->Node.SetSequence(Sequence); Evaluator->Node.SetShouldLoop(false);
		Evaluator->Node.SetShouldUseExplicitFrame(false); Evaluator->Node.SetExplicitTime(0.0f);
		FBoolProperty* Teleport = FindFProperty<FBoolProperty>(FAnimNode_SequenceEvaluator::StaticStruct(), TEXT("bTeleportToExplicitTime"));
		if (!Teleport) { Graph.Body.bOK = Error(TEXT("Facing evaluator teleport property unavailable.")); return Evaluator; }
		Teleport->SetPropertyValue_InContainer(&Evaluator->Node, true);
		Evaluator->AllocateDefaultPins(); Graph.Body.Value(TEXT("TurnTime"), Evaluator, TEXT("ExplicitTime")); return Evaluator;
	};
	auto* Left = TurnEvaluator(Turns[0]); auto* Right = TurnEvaluator(Turns[1]);
	auto* Direction = Graph.Body.Select(Right, Left, TEXT("bTurnRight"));
	auto* TurnBlend = Graph.Body.Node<UAnimGraphNode_TwoWayBlend>(TEXT("下半身保留原走跑/跳跃，原地转身只替换腿部姿态；上身与玩法规则不受影响。"));
	TurnBlend->AllocateDefaultPins(); Graph.Body.Link(Graph.LowerOutput, TurnBlend->FindPin(TEXT("A")));
	Graph.Body.Pose(Direction, TurnBlend, TEXT("B")); Graph.Body.Value(TEXT("TurnAlpha"), TurnBlend, TEXT("Alpha"));
	auto* Rotation = Graph.Body.Node<UAnimGraphNode_RotateRootBone>(TEXT("只对下半身应用原生 RootYawOffset，静止小幅瞄准时保留双脚朝向，大幅转向按真实转身曲线跟上。"));
	Rotation->Node.bRotateRootMotionAttribute = false; Rotation->AllocateDefaultPins();
	Graph.Body.Pose(TurnBlend, Rotation, TEXT("BasePose")); Graph.Body.Value(TEXT("RootYawOffset"), Rotation, TEXT("Yaw"));
	UEdGraphPin* Base = Graph.Layer->FindPin(TEXT("BasePose"));
	Graph.LowerOutput->BreakLinkTo(Base); Graph.Body.Pose(Rotation, Graph.Layer, TEXT("BasePose"));
	// 原 Walk、Blend 与作者注释全部保留，只关闭第一人称行走摆枪；瞄准、冲刺、动作 Slot、受击和跳跃原链不动。
	Graph.ArmsWalk->FindPin(TEXT("Alpha"))->BreakAllPinLinks();
	Graph.ArmsWalk->BlendNode.Alpha = 0.0f; Graph.ArmsWalk->FindPin(TEXT("Alpha"))->DefaultValue = TEXT("0.0");
	if (!Graph.Body.bOK || !Graph.Arms.bOK || !FpstruePvpAssetPreparation::Compile(Body.Get()) || !FpstruePvpAssetPreparation::Compile(Arms.Get()))
	{ Error(TEXT("Facing repair compile failed; no assets saved. Backup: ") + Backup); return; }
	BodyDefaults = Body->GeneratedClass->GetDefaultObject();
	LeftProperty->SetObjectPropertyValue_InContainer(BodyDefaults, Turns[0]);
	RightProperty->SetObjectPropertyValue_InContainer(BodyDefaults, Turns[1]);
	Packages.Add(Body->GetOutermost()); Packages.Add(Arms->GetOutermost());
	for (UPackage* Package : Packages) Package->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) { Error(TEXT("Facing repair save failed. Backup: ") + Backup); return; }
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_FACING PASS Mode=Apply PackagesSaved=%d Backup=%s SourceAssetsUntouched=true OriginalNodesAndCommentsPreserved=true LowerBodyOnly=true"), Packages.Num(), *Backup);
}
FAutoConsoleCommand FacingRepairCommand(TEXT("fp.RepairTemplateFacing"),
	TEXT("Check the exact Template facing graph. Apply backs up Body/Arms, creates project-only turn copies and narrowly inserts lower-body explicit-time turning while preserving the upper action chain."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RepairFacing));

// 使用临时组件的最终单序列姿态和 LOD0 真实蒙皮顶点测鞋底；不创建 GPU MeshObject、不修改源素材。
bool MeasureGroundedSkin(USkeletalMeshComponent* SavedMesh, double HalfHeight, const TArray<UAnimSequence*>& Clips, double& OutIdleClearance)
{
	USkeletalMesh* Body = SavedMesh->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* RenderData = Body ? Body->GetResourceForRendering() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0) || Body->GetPostProcessAnimBlueprint())
		return Error(TEXT("Ground skin measurement requires resident LOD0 and this Template mesh's confirmed no-postprocess configuration."));
	FTestWorldWrapper PreviewWorld;
	if (!PreviewWorld.CreateTestWorld(EWorldType::EditorPreview)) return Error(TEXT("Ground skin preview world creation failed."));
	auto* Pose = NewObject<USkeletalMeshComponent>(PreviewWorld.GetTestWorld(), NAME_None, RF_Transient);
	Pose->SetSkeletalMesh(Body);
	Pose->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Pose->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	Pose->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Pose->RegisterComponentWithWorld(PreviewWorld.GetTestWorld());
	ON_SCOPE_EXIT { Pose->DestroyComponent(); };
	FTransform Placement = SavedMesh->GetRelativeTransform();
	Placement.AddToTranslation(FVector(0, 0, HalfHeight)); // synthetic flat floor is capsule bottom Z=0
	Pose->SetWorldTransform(Placement);
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	FSkinWeightVertexBuffer* Weights = Pose->GetSkinWeightBuffer(0);
	if (!Weights || LOD.GetNumVertices() == 0 || Weights->GetNumVertices() != LOD.GetNumVertices()
		|| !Weights->GetDataVertexBuffer()->GetWeightData())
		return Error(TEXT("Ground skin CPU vertex/weight data unavailable; no offset will be inferred from ankle bones."));
	OutIdleClearance = 0;
	for (int32 ClipIndex = 0; ClipIndex < Clips.Num(); ++ClipIndex)
	{
		UAnimSequence* Clip = Clips[ClipIndex];
		Pose->SetAnimation(Clip);
		if (UAnimInstance* Instance = Pose->GetAnimInstance()) Instance->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
		double Lowest = TNumericLimits<double>::Max(), Highest = TNumericLimits<double>::Lowest();
		const int32 SampleCount = ClipIndex == 0 ? 1 : 17;
		for (int32 Sample = 0; Sample < SampleCount; ++Sample)
		{
			Pose->SetPosition(SampleCount == 1 ? 0.0f : Clip->GetPlayLength() * Sample / (SampleCount - 1), false);
			Pose->TickAnimation(0.0f, false);
			Pose->RefreshBoneTransforms();
			TArray<FMatrix44f> RefToLocal;
			Pose->GetCurrentRefToLocalMatrices(RefToLocal, 0);
			// Rendering matrices follow inverse-reference matrices, not the skeleton's extra virtual bones.
			if (Sample == 0)
				UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND SKIN_MATRICES Clip=%s Matrices=%d InverseReference=%d RawBones=%d FullBonesWithVirtual=%d ComponentPose=%d LODSections=%d"),
					*Clip->GetName(), RefToLocal.Num(), Body->GetRefBasesInvMatrix().Num(), Body->GetRefSkeleton().GetRawBoneNum(),
					Body->GetRefSkeleton().GetNum(), Pose->GetComponentSpaceTransforms().Num(), LOD.RenderSections.Num());
			if (RefToLocal.IsEmpty() || RefToLocal.Num() != Body->GetRefBasesInvMatrix().Num())
				return Error(TEXT("Ground skin inverse-reference matrix count invalid."));
			for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
				for (FBoneIndexType Bone : Section.BoneMap)
					if (!RefToLocal.IsValidIndex(Bone) || !Pose->GetComponentSpaceTransforms().IsValidIndex(Bone))
						return Error(TEXT("Ground skin LOD section references an unevaluated bone."));
			double MinWorldZ = TNumericLimits<double>::Max();
			for (uint32 Vertex = 0; Vertex < LOD.GetNumVertices(); ++Vertex)
			{
				const FVector3f Skinned = USkinnedMeshComponent::GetSkinnedVertexPosition(Pose, Vertex, LOD, *Weights, RefToLocal);
				const double Z = Placement.TransformPosition(FVector(Skinned)).Z;
				if (!FMath::IsFinite(Z)) return Error(TEXT("Ground skin vertex is non-finite."));
				MinWorldZ = FMath::Min(MinWorldZ, Z);
			}
			Lowest = FMath::Min(Lowest, MinWorldZ); Highest = FMath::Max(Highest, MinWorldZ);
			if (ClipIndex == 0) OutIdleClearance = MinWorldZ;
		}
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND SKIN Clip=%s Vertices=%u PoseSamples=%d LowestSoleClearance=%.5f HighestSoleClearance=%.5f FloorZ=0 RootLockEvaluated=1 MorphClothExcluded=1 NoPostProcess=1 NoAssetWrite=1"),
			*Clip->GetPathName(), LOD.GetNumVertices(), SampleCount, Lowest, Highest);
	}
	return true;
}

// 仅修已量出的两个数据问题：模板身体离地和走跑采样步速；原动画、AnimGraph、第一人称偏移保持不动。
void CalibrateGrounding(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	if (!GEditor || GEditor->PlayWorld || (Args.Num() && !bApply)) return;
	auto* BP = Load<UBlueprint>(PlayerPath);
	auto* CDO = BP && BP->GeneratedClass ? Cast<AfpstrueCharacter>(BP->GeneratedClass->GetDefaultObject()) : nullptr;
	auto* Directions = Load<UBlendSpace>(Destination + TEXT("BS_TemplateDirections"));
	auto* Mesh = CDO ? CDO->GetMesh() : nullptr;
	auto* Capsule = CDO ? CDO->GetCapsuleComponent() : nullptr;
	if (!Mesh || !Capsule || !Directions || Directions->GetNumberOfBlendSamples() != 9
		|| BP->GetOutermost()->IsDirty() || Directions->GetOutermost()->IsDirty())
	{ Error(TEXT("Ground calibration requires the saved, unmodified Template BP and nine-sample Directions asset.")); return; }
	TArray<UAnimSequence*> Clips;
	for (const FString& Path : {Art + TEXT("TPP/AR/A_TPP_AR_Idle"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Walk_Fwd"), Art + TEXT("TPP/Locomotion/RootLocomotion/A_TPP_SMG_Jog_Fwd")})
	{
		auto* Clip = Load<UAnimSequence>(Path);
		if (!Clip || Clip->IsValidAdditive()) { Error(TEXT("Ground calibration non-additive source unavailable: ") + Path); return; }
		Clips.Add(Clip);
	}
	double IdleClearance = 0;
	if (!MeasureGroundedSkin(Mesh, Capsule->GetUnscaledCapsuleHalfHeight(), Clips, IdleClearance)) return;
	FVector Location = Mesh->GetRelativeLocation();
	const double NewZ = Location.Z - IdleClearance;
	TArray<float> Rates;
	for (const FBlendSample& Sample : Directions->GetBlendSamples())
	{
		if (Sample.SampleValue.IsNearlyZero()) { Rates.Add(Sample.RateScale); continue; }
		UAnimSequence* Clip = Sample.Animation;
		if (!Clip || Clip->GetPlayLength() <= 0 || !Clip->bForceRootLock || Clip->bEnableRootMotion || Clip->IsValidAdditive())
		{ Error(TEXT("Ground calibration locomotion root-lock/source invariant differs; no write.")); return; }
		FTransform Start, End;
		Clip->GetBoneTransform(Start, FSkeletonPoseBoneIndex(0), 0.0, true);
		Clip->GetBoneTransform(End, FSkeletonPoseBoneIndex(0), static_cast<double>(Clip->GetPlayLength()), true);
		const double AuthoredSpeed = (End.GetTranslation() - Start.GetTranslation()).Size2D() / Clip->GetPlayLength();
		const double TargetSpeed = Sample.SampleValue.Size2D();
		if (AuthoredSpeed <= 1 || TargetSpeed <= 0) { Error(TEXT("Ground calibration authored root speed unavailable.")); return; }
		const float Rate = TargetSpeed / (AuthoredSpeed * Clip->RateScale);
		Rates.Add(Rate);
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND RATE Clip=%s TargetSpeed=%.5f AuthoredSpeed=%.5f ClipRate=%.5f PreviousSampleRate=%.5f CalibratedSampleRate=%.5f"),
			*Clip->GetPathName(), TargetSpeed, AuthoredSpeed, Clip->RateScale, Sample.RateScale, Rate);
	}
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND CALIBRATION OldMeshZ=%.6f MeasuredIdleSoleClearance=%.6f ProposedMeshZ=%.6f Apply=%d"), Location.Z, IdleClearance, NewZ, bApply);
	if (!bApply) return;
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/TemplateGrounding") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	for (UPackage* Package : {BP->GetOutermost(), Directions->GetOutermost()})
		if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	BP->Modify(); Mesh->Modify(); Directions->Modify();
	Location.Z = NewZ; Mesh->SetRelativeLocation(Location);
	for (int32 Index = 0; Index < Rates.Num(); ++Index)
		const_cast<FBlendSample&>(Directions->GetBlendSample(Index)).RateScale = Rates[Index];
	Directions->ValidateSampleData(); Directions->ResampleData();
	BP->MarkPackageDirty(); Directions->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({BP->GetOutermost(), Directions->GetOutermost()}, false))
	{ Error(TEXT("Ground calibration save failed; preserved originals in ") + Backup); return; }
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_GROUND REPAIRED Backup=%s Assets=2 SourceAnimationUnchanged=1 GraphUnchanged=1 FirstPersonUnchanged=1"), *Backup);
}
FAutoConsoleCommand GroundCalibrationCommand(TEXT("fp.CalibrateTemplateGrounding"),
	TEXT("Read-only CPU sole and authored-step-speed calibration. Apply backs up and modifies only saved Template BP mesh Z and Directions sample rates."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&CalibrateGrounding));

// 网络 HUD 由原生事件驱动：保留控件布局，删除已识别的旧 GameMode 链，不再复制并遗留不可达节点。
void PrepareHUD(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr || (Args.Num() && !bApply)) return;
	const FString Target = TEXT("/Game/FirstPerson/UI/ingame_network");
	const bool bExists = FPackageName::DoesPackageExist(Target);
	auto* Copy = Load<UWidgetBlueprint>(bExists ? Target : TEXT("/Game/FirstPerson/UI/ingame"));
	if (!Copy || Copy->GetOutermost()->IsDirty()) { Error(TEXT("Saved HUD is missing or has unsaved edits.")); return; }
	FpstruePvpAssetPreparation::FLegacyGraphPlan Plan;
	if (!FpstruePvpAssetPreparation::InspectNetworkHUD(*Copy, Plan))
	{ Error(TEXT("HUD contains an unrecognized old UI chain; inspect the node audit before Apply.")); return; }
	if (!bApply) { UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HUD PASS ReadOnly=true ExistingNetworkHUD=%d"), bExists); return; }
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/NetworkHUD") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (bExists)
	{
		if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Copy->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	}
	else
	{
		UPackage* NewPackage = CreatePackage(*Target);
		Copy = DuplicateObject<UWidgetBlueprint>(Copy, NewPackage, TEXT("ingame_network"));
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Copy);
		Plan = FpstruePvpAssetPreparation::InspectLegacyGraphs(*Copy);
		if (!Plan.bSafe) { Error(TEXT("Copied HUD graph differs from the inspected source; no save.")); return; }
	}
	FpstruePvpAssetPreparation::RemoveLegacyGraphs(*Copy, Plan);
	Copy->ParentClass = UfpstruePvpIngameWidget::StaticClass();
	Copy->Bindings.Reset();
	if (!FpstruePvpAssetPreparation::Compile(Copy)) return;
	UPackage* Package = Copy->GetOutermost();
	Package->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({Package}, false)) { Error(TEXT("HUD copy save failed.")); return; }
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HUD PASS Copy=%s LegacyGameModeNodesRemoved=1 LayoutUnchanged=1 Backup=%s"), *Target, *Backup);
}
FAutoConsoleCommand HUDCommand(TEXT("fp.PrepareNetworkHUD"), TEXT("Check/Apply native network HUD graphs while retaining layout and removing recognized legacy GameMode chains."), FConsoleCommandWithArgsDelegate::CreateStatic(&PrepareHUD));

// 已生成的身体图只移动现有受击节点，不删除节点/注释、不重生成其他资产；改动前保留可恢复备份。
void RepairUnarmedHit(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr || (Args.Num() && !bApply)) return;
	auto* Blueprint = Load<UAnimBlueprint>(Destination + TEXT("ABP_TemplateBody"));
	if (!Blueprint) { Error(TEXT("Template body AnimBP missing.")); return; }
	FGraphBuilder B = Builder(Blueprint);
	if (!B.bOK) return;
	UAnimGraphNode_Root* RootNode = nullptr; UAnimGraphNode_ApplyMeshSpaceAdditive* Hit = nullptr;
	for (UEdGraphNode* Node : B.Graph->Nodes)
	{
		if (auto* FoundRoot = Cast<UAnimGraphNode_Root>(Node)) RootNode = FoundRoot;
		if (auto* Add = Cast<UAnimGraphNode_ApplyMeshSpaceAdditive>(Node))
		{
			if (Hit) { Error(TEXT("Ambiguous template body hit graph; refusing automatic repair.")); return; }
			Hit = Add;
		}
	}
	UEdGraphPin* RootInput = RootNode ? RootNode->FindPin(TEXT("Result")) : nullptr;
	if (!RootInput || RootInput->LinkedTo.Num() != 1 || !Hit) { Error(TEXT("Unexpected template body root.")); return; }
	if (RootInput->LinkedTo[0]->GetOwningNode() == Hit)
	{ UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HIT PASS AlreadyApplied=true")); return; }
	auto* Layer = Cast<UAnimGraphNode_LayeredBoneBlend>(RootInput->LinkedTo[0]->GetOwningNode());
	UEdGraphPin* Base = Hit->FindPin(TEXT("Base")); UEdGraphPin* Output = PoseOutput(Hit);
	if (!Layer || !Base || Base->LinkedTo.Num() != 1 || !Output || Output->LinkedTo.Num() != 1
		|| !Cast<UAnimGraphNode_Slot>(Base->LinkedTo[0]->GetOwningNode())
		|| !Cast<UAnimGraphNode_RotationOffsetBlendSpace>(Output->LinkedTo[0]->GetOwningNode()))
	{ Error(TEXT("Template hit is no longer in the known generated graph; refusing repair.")); return; }
	UEdGraphPin* SlotOutput = Base->LinkedTo[0]; UEdGraphPin* AimInput = Output->LinkedTo[0];
	if (!bApply) { UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HIT PASS NeedsRepair=true ReadOnly=true")); return; }
	if (Blueprint->GetOutermost()->IsDirty()) { Error(TEXT("Template body has unsaved edits.")); return; }
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/TemplateHit") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FpstruePvpAssetPreparation::BackupAsset(FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	Blueprint->Modify(); B.Graph->Modify(); Hit->Modify(); Layer->Modify(); RootNode->Modify();
	Base->BreakAllPinLinks(); Output->BreakAllPinLinks(); RootInput->BreakAllPinLinks();
	B.Link(SlotOutput, AimInput); B.Pose(Layer, Hit, TEXT("Base")); B.Pose(Hit, RootNode, TEXT("Result"));
	Hit->NodeComment = TEXT("服务器受击事实在最终身体姿态上叠加一次；开局空手也有受击反馈，不依赖 bArmed 上半身权重。");
	if (!B.bOK || !FpstruePvpAssetPreparation::Compile(Blueprint)) return;
	Blueprint->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({Blueprint->GetOutermost()}, false)) { Error(TEXT("Template hit save failed.")); return; }
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HIT PASS Repaired=true OriginalNodesAndCommentsRetained=true Backup=%s"), *Backup);
}
FAutoConsoleCommand HitCommand(TEXT("fp.RepairTemplateUnarmedHit"), TEXT("Move the existing template body hit additive after the armed-only layer; Apply backs up and saves this AnimBP only."), FConsoleCommandWithArgsDelegate::CreateStatic(&RepairUnarmedHit));
}

// 玩家后处理图的离线鞋底标定：检查实际骨链/蒙皮，Apply 仅创建或更新项目玩家 IK 包，不处理敌人。
namespace FpstrueFootIKPreparation
{
using namespace FpstrueRARPresentation;
using FpstrueRARPresentation::Error;
const FString FootIKDirectory = TEXT("/Game/FirstPerson/Animations/FootIK/");
constexpr const TCHAR* CalibrationKey = TEXT("fpstrueFootIKCalibration");
constexpr const TCHAR* CalibrationVersion = TEXT("WeightedSoleLOD0_Idle_v3_Toe");
constexpr const TCHAR* PreviousCalibrationVersion = TEXT("WeightedSoleLOD0_Idle_v1");
constexpr const TCHAR* FootprintCalibrationVersion = TEXT("WeightedSoleLOD0_Idle_v2_3x3");

struct FSoleCalibration
{
	FVector Offset[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector Normal[2] = {FVector::UpVector, FVector::UpVector};
	TArray<int32> Vertices[2];
	TArray<FVector> Probes[2];
	TArray<FVector> ToeProbes[2];
};

// 两套骨架分别校验实际骨链，不因同名或带有 ik_foot 骨就推断它们可互换。
bool InspectLegChains(const USkeletalMesh* Mesh)
{
	if (!Mesh || !Mesh->GetSkeleton()) return Error(TEXT("Foot IK requires the actual body mesh and skeleton."));
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	if (Ref.GetRawBoneNum() == 0 || Ref.GetBoneName(0) != TEXT("root")) return Error(TEXT("Foot IK requires the expected root bone."));
	for (const TCHAR* Side : {TEXT("l"), TEXT("r")})
	{
		FName Parent = TEXT("pelvis");
		for (const TCHAR* Prefix : {TEXT("thigh_"), TEXT("calf_"), TEXT("foot_"), TEXT("ball_")})
		{
			const FName Bone(*(FString(Prefix) + Side));
			const int32 Index = Ref.FindBoneIndex(Bone), ParentIndex = Ref.FindBoneIndex(Parent);
			if (Index == INDEX_NONE || ParentIndex == INDEX_NONE || Ref.GetParentIndex(Index) != ParentIndex)
				return Error(FString::Printf(TEXT("Foot IK expected %s -> %s in %s."), *Parent.ToString(), *Bone.ToString(), *Mesh->GetPathName()));
			UE_LOG(LogTemp, Display, TEXT("FOOT_IK CHAIN Mesh=%s Bone=%s Parent=%s BoneIndex=%d"), *Mesh->GetPathName(), *Bone.ToString(), *Parent.ToString(), Index);
			Parent = Bone;
		}
	}
	return true;
}

// 从禁用后处理的真实 Idle 蒙皮鞋底标定；保存的顶点只用于验收，运行时 IK 不遍历网格。
bool MeasureSoles(USkeletalMesh* Mesh, UAnimSequence* Idle, FSoleCalibration& Result)
{
	if (!InspectLegChains(Mesh) || !Idle || Idle->IsValidAdditive() || Idle->GetSkeleton() != Mesh->GetSkeleton())
		return Error(TEXT("Foot IK sole calibration needs the body's own non-additive idle sequence."));
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0)) return Error(TEXT("Foot IK LOD0 render data unavailable."));
	FTestWorldWrapper Preview;
	if (!Preview.CreateTestWorld(EWorldType::EditorPreview)) return Error(TEXT("Foot IK sole preview world creation failed."));
	auto* Pose = NewObject<USkeletalMeshComponent>(Preview.GetTestWorld(), NAME_None, RF_Transient);
	Pose->SetSkeletalMesh(Mesh); Pose->SetDisablePostProcessBlueprint(true);
	Pose->SetCollisionEnabled(ECollisionEnabled::NoCollision); Pose->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	Pose->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Pose->RegisterComponentWithWorld(Preview.GetTestWorld());
	ON_SCOPE_EXIT { Pose->DestroyComponent(); };
	Pose->SetAnimation(Idle); Pose->SetPosition(0.0f, false);
	Pose->TickAnimation(0.0f, false); Pose->RefreshBoneTransforms();
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	FSkinWeightVertexBuffer* Weights = Pose->GetSkinWeightBuffer(0);
	TArray<FMatrix44f> Matrices; Pose->GetCurrentRefToLocalMatrices(Matrices, 0);
	if (!Weights || !Weights->GetDataVertexBuffer()->GetWeightData() || LOD.GetNumVertices() == 0
		|| Weights->GetNumVertices() != LOD.GetNumVertices() || Matrices.Num() != Mesh->GetRefBasesInvMatrix().Num())
		return Error(TEXT("Foot IK CPU skin data unavailable; ankle height is not used as a substitute for shoe soles."));
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const int32 Feet[] = {Ref.FindBoneIndex(TEXT("foot_l")), Ref.FindBoneIndex(TEXT("foot_r"))};
	const int32 Balls[] = {Ref.FindBoneIndex(TEXT("ball_l")), Ref.FindBoneIndex(TEXT("ball_r"))};
	struct FSoleVertex { int32 Index; FVector Position; };
	TArray<FSoleVertex> Candidates[2];
	TArray<FSoleVertex> ToeCandidates[2];
	for (uint32 Vertex = 0; Vertex < LOD.GetNumVertices(); ++Vertex)
	{
		int32 SectionIndex = INDEX_NONE, LocalVertex = INDEX_NONE;
		LOD.GetSectionFromVertexIndex(static_cast<int32>(Vertex), SectionIndex, LocalVertex);
		if (!LOD.RenderSections.IsValidIndex(SectionIndex)) return Error(TEXT("Foot IK sole vertex section invalid."));
		const auto& BoneMap = LOD.RenderSections[SectionIndex].BoneMap;
		uint32 WeightOffset = 0, InfluenceCount = 0;
		Weights->GetVertexInfluenceOffsetCount(Vertex, WeightOffset, InfluenceCount);
		uint32 TotalWeight = 0, FootWeight[2] = {}, BallWeight[2] = {};
		for (uint32 Influence = 0; Influence < InfluenceCount; ++Influence)
		{
			const uint32 Weight = Weights->GetBoneWeight(Vertex, Influence);
			if (Weight == 0) continue;
			const uint32 BoneInSection = Weights->GetBoneIndex(Vertex, Influence);
			if (!BoneMap.IsValidIndex(static_cast<int32>(BoneInSection))) return Error(TEXT("Foot IK vertex influence references an invalid section bone."));
			const int32 Bone = BoneMap[BoneInSection]; TotalWeight += Weight;
			for (int32 Side = 0; Side < 2; ++Side)
			{
				if (Bone == Feet[Side] || Bone == Balls[Side]) FootWeight[Side] += Weight;
				if (Bone == Balls[Side]) BallWeight[Side] += Weight;
			}
		}
		if (Vertex == 2376 || Vertex == 8172 || Vertex == 19550)
		{
			const FVector Position(USkinnedMeshComponent::GetSkinnedVertexPosition(Pose, Vertex, LOD, *Weights, Matrices));
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const FTransform Foot = Pose->GetSocketTransform(Side == 0 ? TEXT("foot_l") : TEXT("foot_r"), RTS_Component);
				const FTransform Ball = Pose->GetSocketTransform(Side == 0 ? TEXT("ball_l") : TEXT("ball_r"), RTS_Component);
				const double Denominator = FMath::Max(1.0, static_cast<double>(TotalWeight));
				UE_LOG(LogTemp, Display, TEXT("FOOT_IK VERTEX_WEIGHT Mesh=%s Vertex=%u Side=%s TotalWeight=%u FootOnlyFraction=%.6f BallFraction=%.6f CombinedSoleFraction=%.6f SourcePositionCS=%s FootLocal=%s BallLocal=%s"),
					*Mesh->GetPathName(), Vertex, Side == 0 ? TEXT("L") : TEXT("R"), TotalWeight,
					(FootWeight[Side] - BallWeight[Side]) / Denominator, BallWeight[Side] / Denominator, FootWeight[Side] / Denominator,
					*Position.ToString(), *Foot.InverseTransformPosition(Position).ToString(), *Ball.InverseTransformPosition(Position).ToString());
			}
		}
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (TotalWeight == 0 || FootWeight[Side] * 2 < TotalWeight) continue;
			const FVector Position(USkinnedMeshComponent::GetSkinnedVertexPosition(Pose, Vertex, LOD, *Weights, Matrices));
			if (Position.ContainsNaN()) return Error(TEXT("Foot IK sole skin position is non-finite."));
			Candidates[Side].Add({static_cast<int32>(Vertex), Position});
			if (BallWeight[Side] * 2 >= TotalWeight) ToeCandidates[Side].Add({static_cast<int32>(Vertex), Position});
		}
	}
	for (int32 Side = 0; Side < 2; ++Side)
	{
		auto& Points = Candidates[Side];
		if (Points.Num() < 3) return Error(TEXT("Foot IK could not isolate enough foot-weighted shoe vertices."));
		Points.Sort([](const FSoleVertex& A, const FSoleVertex& B)
		{ return A.Position.Z != B.Position.Z ? A.Position.Z < B.Position.Z : A.Index < B.Index; });
		const double LowestZ = Points[0].Position.Z;
		int32 BottomCount = 0; FBox BottomBox(ForceInit);
		while (Points.IsValidIndex(BottomCount) && Points[BottomCount].Position.Z <= LowestZ + 0.35)
		{ BottomBox += Points[BottomCount].Position; ++BottomCount; }
		if (BottomCount < 3) return Error(TEXT("Foot IK shoe bottom has fewer than three measurable vertices."));
		FVector SoleCenter = BottomBox.GetCenter(); SoleCenter.Z = LowestZ;
		const FName FootName = Side == 0 ? TEXT("foot_l") : TEXT("foot_r");
		const FTransform Foot = Pose->GetSocketTransform(FootName, RTS_Component);
		Result.Offset[Side] = Foot.InverseTransformPosition(SoleCenter);
		// 踝骨自身轴向不一定垂直鞋底；记录实际标定鞋底平面法线，避免保留原 foot pitch 后再次叠斜坡旋转。
		Result.Normal[Side] = Foot.InverseTransformVectorNoScale(FVector::UpVector).GetSafeNormal();
		// 在真实最低鞋底薄层的投影范围内取九点（含中心），运行时只探测这些标定点，不再读取蒙皮顶点。
		for (int32 Row = 0; Row < 3; ++Row)
			for (int32 Column = 0; Column < 3; ++Column)
			{
				const FVector SolePoint(FMath::Lerp(BottomBox.Min.X, BottomBox.Max.X, Column * 0.5),
					FMath::Lerp(BottomBox.Min.Y, BottomBox.Max.Y, Row * 0.5), LowestZ);
				Result.Probes[Side].Add(Foot.InverseTransformPosition(SolePoint));
			}
		// 趾部只使用 ball 主导的底部薄层，不能把整只鞋跟随趾骨弯折；缺失权重时也不虚构鞋长。
		auto& ToePoints = ToeCandidates[Side];
		FBox ToeBox(ForceInit); int32 ToeBottomCount = 0;
		if (!ToePoints.IsEmpty())
		{
			ToePoints.Sort([](const FSoleVertex& A, const FSoleVertex& B)
			{ return A.Position.Z != B.Position.Z ? A.Position.Z < B.Position.Z : A.Index < B.Index; });
			const double ToeLowestZ = ToePoints[0].Position.Z;
			while (ToePoints.IsValidIndex(ToeBottomCount) && ToePoints[ToeBottomCount].Position.Z <= ToeLowestZ + 0.35)
			{ ToeBox += ToePoints[ToeBottomCount].Position; ++ToeBottomCount; }
			const FTransform Ball = Pose->GetSocketTransform(Side == 0 ? TEXT("ball_l") : TEXT("ball_r"), RTS_Component);
			if (ToeBottomCount >= 3)
				for (int32 Row = 0; Row < 3; ++Row)
					for (int32 Column = 0; Column < 3; ++Column)
					{
						const FVector ToePoint(FMath::Lerp(ToeBox.Min.X, ToeBox.Max.X, Column * 0.5),
							FMath::Lerp(ToeBox.Min.Y, ToeBox.Max.Y, Row * 0.5), ToeLowestZ);
						Result.ToeProbes[Side].Add(Ball.InverseTransformPosition(ToePoint));
					}
			else
				for (int32 Index = 0; Index < ToeBottomCount; ++Index)
					Result.ToeProbes[Side].Add(Ball.InverseTransformPosition(ToePoints[Index].Position));
		}
		UE_LOG(LogTemp, Display, TEXT("FOOT_IK TOE_PATCH Mesh=%s Side=%s BallDominantCandidates=%d BottomVertices=%d ToeProbes=%d MinCS=%s MaxCS=%s BindBone=%s NoGuessedDimensions=true"),
			*Mesh->GetPathName(), Side == 0 ? TEXT("L") : TEXT("R"), ToePoints.Num(), ToeBottomCount, Result.ToeProbes[Side].Num(),
			ToeBottomCount > 0 ? *ToeBox.Min.ToString() : TEXT("None"), ToeBottomCount > 0 ? *ToeBox.Max.ToString() : TEXT("None"),
			Side == 0 ? TEXT("ball_l") : TEXT("ball_r"));
		const int32 Stored = FMath::Min(48, BottomCount);
		for (int32 Index = 0; Index < Stored; ++Index)
			Result.Vertices[Side].Add(Points[Index * (BottomCount - 1) / FMath::Max(1, Stored - 1)].Index);
		UE_LOG(LogTemp, Display, TEXT("FOOT_IK SOLE Mesh=%s Idle=%s Foot=%s FootLocation=%s SoleCenterCS=%s SoleOffsetFootLocal=%s SoleNormalFootLocal=%s WeightedCandidates=%d BottomVertices=%d StoredDebugVertices=%d SoleProbes=%d SoleProjectionMinCS=%s SoleProjectionMaxCS=%s BottomThicknessCm=0.35 CPUSkinned=true AnkleNotSole=true MorphClothExcluded=true"),
			*Mesh->GetPathName(), *Idle->GetPathName(), *FootName.ToString(), *Foot.GetLocation().ToString(), *SoleCenter.ToString(),
			*Result.Offset[Side].ToString(), *Result.Normal[Side].ToString(), Points.Num(), BottomCount, Stored,
			Result.Probes[Side].Num(), *BottomBox.Min.ToString(), *BottomBox.Max.ToString());
	}
	return true;
}

// 后处理图只传递主图的当前帧姿态；IK 由共享原生 Proxy 在本帧姿态上计算，不重建原主动画图。
bool InspectPassThrough(UAnimBlueprint* Blueprint, USkeletalMesh* Mesh)
{
	if (!Blueprint || Blueprint->ParentClass != UfpstrueFootIKAnimInstance::StaticClass() || !Blueprint->GeneratedClass
		|| Blueprint->TargetSkeleton != Mesh->GetSkeleton() || Blueprint->GetOutermost()->IsDirty()
		|| (Blueprint->GetOutermost()->GetMetaData()->GetValue(Blueprint, CalibrationKey) != CalibrationVersion
			&& Blueprint->GetOutermost()->GetMetaData()->GetValue(Blueprint, CalibrationKey) != PreviousCalibrationVersion
			&& Blueprint->GetOutermost()->GetMetaData()->GetValue(Blueprint, CalibrationKey) != FootprintCalibrationVersion))
		return Error(TEXT("Foot IK refuses an unknown, unsaved or differently parented existing output."));
	FGraphBuilder Graph = Builder(Blueprint);
	if (!Graph.bOK || Graph.Graph->Nodes.Num() != 2) return Error(TEXT("Foot IK expected the project passthrough graph only."));
	UAnimGraphNode_LinkedInputPose* Input = nullptr; UAnimGraphNode_Root* Output = nullptr;
	for (UEdGraphNode* Node : Graph.Graph->Nodes)
	{
		if (auto* Pose = Cast<UAnimGraphNode_LinkedInputPose>(Node)) Input = Pose;
		if (auto* RootNode = Cast<UAnimGraphNode_Root>(Node)) Output = RootNode;
	}
	if (!Input || !Output || Input->Node.Name != FAnimNode_LinkedInputPose::DefaultInputPoseName
		|| FpstrueTemplatePresentation::SinglePoseSource(Output->FindPin(TEXT("Result"))) != Input
		|| !PoseOutput(Input) || PoseOutput(Input)->LinkedTo.Num() != 1)
		return Error(TEXT("Foot IK existing output no longer has its known LinkedInputPose -> Root connection."));
	return true;
}

// 仅维护玩家的薄后处理图；敌人不生成、不绑定脚部IK资产。
void Prepare(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	if ((Args.Num() > 0 && !bApply) || !GEditor || GEditor->PlayWorld)
	{ Error(TEXT("Usage: fp.PrepareFootIK [Apply], no active PIE.")); return; }
	TStrongObjectPtr<UBlueprint> Player(Load<UBlueprint>(TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter")));
	auto* PlayerDefaults = Player.IsValid() && Player->GeneratedClass ? Cast<AfpstrueCharacter>(Player->GeneratedClass->GetDefaultObject()) : nullptr;
	USkeletalMesh* Mesh = PlayerDefaults ? PlayerDefaults->GetMesh()->GetSkeletalMeshAsset() : nullptr;
	TStrongObjectPtr<UAnimSequence> Idle(Load<UAnimSequence>(TEXT("/Game/FPSMPT55/Animations/TPP/AR/A_TPP_AR_Idle")));
	FSoleCalibration Calibration;
	if (!MeasureSoles(Mesh, Idle.Get(), Calibration)) return;
	const TCHAR* Name = TEXT("ABP_PlayerFootIK");
	const FString Path = FootIKDirectory + Name;
	const bool bMissing = !FPackageName::DoesPackageExist(Path);
	TStrongObjectPtr<UAnimBlueprint> Blueprint(bMissing ? nullptr : Load<UAnimBlueprint>(Path));
	bool bNeedsCalibration = bMissing;
	if (!bMissing)
	{
		if (!InspectPassThrough(Blueprint.Get(), Mesh)) return;
		const auto* Defaults = Cast<UfpstrueFootIKAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject());
		if (!Defaults) { Error(TEXT("Player Foot IK output CDO is not the native class.")); return; }
		const auto SameProbes = [](const TArray<FVector>& A, const TArray<FVector>& B)
		{
			if (A.Num() != B.Num()) return false;
			for (int32 Index = 0; Index < A.Num(); ++Index) if (!A[Index].Equals(B[Index], 0.01)) return false;
			return true;
		};
		bNeedsCalibration = Blueprint->GetOutermost()->GetMetaData()->GetValue(Blueprint.Get(), CalibrationKey) != CalibrationVersion
			|| !Defaults->LeftSoleOffset.Equals(Calibration.Offset[0], 0.01)
			|| !Defaults->RightSoleOffset.Equals(Calibration.Offset[1], 0.01)
			|| !Defaults->LeftSoleNormal.Equals(Calibration.Normal[0], 0.001)
			|| !Defaults->RightSoleNormal.Equals(Calibration.Normal[1], 0.001)
			|| !SameProbes(Defaults->LeftSoleProbes, Calibration.Probes[0])
			|| !SameProbes(Defaults->RightSoleProbes, Calibration.Probes[1])
			|| !SameProbes(Defaults->LeftToeProbes, Calibration.ToeProbes[0])
			|| !SameProbes(Defaults->RightToeProbes, Calibration.ToeProbes[1])
			|| Defaults->LeftSoleVertices != Calibration.Vertices[0] || Defaults->RightSoleVertices != Calibration.Vertices[1];
	}
	if (!bApply || !bNeedsCalibration)
	{
		UE_LOG(LogTemp, Display, TEXT("FOOT_IK PREPARE_PASS Mode=%s NeedsRepair=%d Missing=%d AssetsSaved=false PlayerOnly=true MainGraphsAndThirdPartyAssetsUntouched=true"),
			bApply ? TEXT("Apply") : TEXT("Check"), bNeedsCalibration, bMissing);
		return;
	}
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/FootIKCalibration") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (Blueprint.IsValid() && !FpstruePvpAssetPreparation::BackupAsset(
		FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Backup)) return;
	TArray<UPackage*> Packages;
	if (!Blueprint.IsValid())
	{
		Blueprint.Reset(AnimBlueprint(Name, UfpstrueFootIKAnimInstance::StaticClass(), Mesh, Packages, FootIKDirectory));
		if (!Blueprint.IsValid()) { Error(TEXT("Player Foot IK AnimBP creation failed; no assets saved.")); return; }
		FGraphBuilder Graph = Builder(Blueprint.Get());
		if (!Graph.bOK) { Error(TEXT("Player Foot IK generated AnimGraph unavailable.")); return; }
		auto* Input = Graph.Node<UAnimGraphNode_LinkedInputPose>(TEXT("接收玩家主动画图最终姿态；原生Proxy在当前帧输入姿态上修正腿部。"));
		Input->AllocateDefaultPins(); FinishGraph(Graph, Input);
		if (!Graph.bOK || !FpstruePvpAssetPreparation::Compile(Blueprint.Get()))
		{ Error(TEXT("Player Foot IK passthrough graph compile failed; no assets saved.")); return; }
	}
	auto* Defaults = Cast<UfpstrueFootIKAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject());
	if (!Defaults) { Error(TEXT("Player Foot IK generated CDO missing; no assets saved.")); return; }
	Blueprint->Modify(); Defaults->Modify();
	Defaults->LeftSoleOffset = Calibration.Offset[0]; Defaults->RightSoleOffset = Calibration.Offset[1];
	Defaults->LeftSoleNormal = Calibration.Normal[0]; Defaults->RightSoleNormal = Calibration.Normal[1];
	Defaults->LeftSoleProbes = Calibration.Probes[0]; Defaults->RightSoleProbes = Calibration.Probes[1];
	Defaults->LeftToeProbes = Calibration.ToeProbes[0]; Defaults->RightToeProbes = Calibration.ToeProbes[1];
	Defaults->LeftSoleVertices = Calibration.Vertices[0]; Defaults->RightSoleVertices = Calibration.Vertices[1];
	Blueprint->GetOutermost()->GetMetaData()->SetValue(Blueprint.Get(), CalibrationKey, CalibrationVersion);
	Packages.AddUnique(Blueprint->GetOutermost());
	for (UPackage* Package : Packages) Package->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) { Error(TEXT("Player Foot IK save failed. Backup: ") + Backup); return; }
	UE_LOG(LogTemp, Display, TEXT("FOOT_IK PREPARE_PASS Mode=Apply PackagesSaved=%d Created=%d Backup=%s PlayerOnly=true MainGraphsAndThirdPartyAssetsUntouched=true"),
		Packages.Num(), bMissing, *Backup);
}
FAutoConsoleCommand Command(TEXT("fp.PrepareFootIK"),
	TEXT("Check player leg chains and CPU-skinned sole calibration. Apply maintains only the player passthrough FootIK AnimBP."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Prepare));
}

// 正常联网客户端的显式输入驱动验收：空手 -> 行走拾取 -> 双端动作 -> 可选战斗/死亡/重生；不保存资源。
namespace FpstruePickupWalkAudit
{
// 仅显式客户端验收使用；Timer 持有短期状态，所有世界/玩家/武器都是弱引用，世界退出即随 Timer 释放。
struct FWalkState
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<AfpstrueCharacter> Pawn;
	TWeakObjectPtr<AfpstrueTemplateWeapon> Target;
	FTimerHandle Timer;
	FString CaptureDirectory;
	double Deadline = 0.0;
	double EquippedAt = 0.0;
	FVector StartLocation = FVector::ZeroVector;
	bool bLockedPawn = false;
	bool bEmptyCaptureRequested = false;
	bool bWalking = false;
	bool bEquippedCaptureRequested = false;
	bool bPairCaptureRequested = false;
	int32 PairActionPhase = 0;
	double PairActionAt = 0.0;
	double PairHoldingAt = 0.0;
	int32 PairInitialReserve = 0;
	int32 NormalRoutePoint = 0;
	bool bRendezvousReached = false;
	bool bPartnerAimObserved = false;
	bool bForwardKeyHeld = false;
	float PairMovementYaw = 0.0f;
	float PairMovementPeak = 0.0f;
	bool RemoteLocomotionRequested[4] = {};
	bool RemoteLocomotionSaved[4] = {};
	double NextPickupDiagnosticAt = 0.0;
	bool bFinished = false;
	bool bFullGameplayStarted = false;
	bool bCombatShooter = false;
	int32 CombatPhase = 0;
	int32 CombatRouteRevision = 0;
	int32 CombatRoutePoint = 0;
	double CombatPhaseAt = 0.0;
	float CombatEnemyInitialHealth = 0.0f;
	bool bCombatFireHeld = false;
	bool bEnemyHitCaptured = false;
	bool bEnemyDeathCaptured = false;
	bool bEnemyDeathSettledCaptured = false;
	bool bPlayerHitCaptured = false;
	bool bPlayerDeathCaptured = false;
	bool bPlayerDeathSettledCaptured = false;
	double EnemyDeathObservedAt = 0.0;
	double PlayerDeathObservedAt = 0.0;
	bool bRespawnCaptured = false;
	bool bApproachCaptured = false;
	TWeakObjectPtr<AfpstrueEnemyCharacter> CombatEnemy;
	TWeakObjectPtr<AfpstrueCharacter> CombatPartner;
	TWeakObjectPtr<APlayerState> CombatPartnerState;
	bool bFacingReviewStarted = false;
	bool bFacingReviewComplete = false;
	bool bFacingRemoteLeft = false;
	bool bFacingRemoteRight = false;
	bool bFacingPitchUp = false;
	bool bFacingPitchDown = false;
	float FacingReviewEpoch = 0.0f;
	double FacingNextSampleAt = 0.0;
	uint64 FacingLastInputFrame = MAX_uint64;
	double FacingLastInputWorldTime = -1.0;
	int32 FacingCaptureCount = 0;
	FVector FacingStartLocation = FVector::ZeroVector;
	TArray<FString> FacingCaptures;

	// 输入仍经普通 PC RotationInput -> PlayerCameraManager::ProcessViewRotation -> CMC；不直写角色或动画朝向。
	void FacingLookInput(APlayerController* Local, const FRotator& DesiredView)
	{
		// Timer 在低帧率时会同帧补调；输入只提交一次，并按真实世界时间限速，不能把多次旧 ControlRotation 误差累加。
		if (FacingLastInputFrame == GFrameCounter || !Local->GetWorld()) return;
		FacingLastInputFrame = GFrameCounter;
		const double WorldTime = Local->GetWorld()->GetTimeSeconds();
		const double Elapsed = FacingLastInputWorldTime >= 0.0 ? WorldTime - FacingLastInputWorldTime : Local->GetWorld()->GetDeltaSeconds();
		FacingLastInputWorldTime = WorldTime;
		const float InputDeltaSeconds = static_cast<float>(FMath::Clamp(Elapsed, 0.0, 0.25));
		const float MaxYawDelta = 240.0f * InputDeltaSeconds, MaxPitchDelta = 720.0f * InputDeltaSeconds;
		float YawScale = 1.0f, PitchScale = 1.0f;
		if (GetDefault<UInputSettings>()->bEnableLegacyInputScales)
		{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			YawScale = Local->GetDeprecatedInputYawScale(); PitchScale = Local->GetDeprecatedInputPitchScale();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		if (!FMath::IsNearlyZero(YawScale))
			Local->AddYawInput(FMath::Clamp(FMath::FindDeltaAngleDegrees(Local->GetControlRotation().Yaw, DesiredView.Yaw), -MaxYawDelta, MaxYawDelta) / YawScale);
		if (!FMath::IsNearlyZero(PitchScale))
			Local->AddPitchInput(FMath::Clamp(FMath::FindDeltaAngleDegrees(Local->GetControlRotation().Pitch, DesiredView.Pitch), -MaxPitchDelta, MaxPitchDelta) / PitchScale);
	}

	// 记录角色、网络平滑和最终骨骼的真实朝向，区分复制事实与本地动画表现，不写角色姿态。
	void LogFacingPose(AfpstrueCharacter* Character, bool bRemote, const TCHAR* Phase, float ServerTime)
	{
		auto* Mesh = Character->GetMesh();
		auto* Animation = Cast<UfpstruePlayerAnimInstance>(Mesh->GetAnimInstance());
		if (!Animation) return;
		const auto& Facing = Character->GetBodyFacingState();
		const auto* Movement = Character->GetCharacterMovement();
		const auto* Prediction = Movement->GetPredictionData_Client_Character();
		const auto BoneYaw = [Mesh](const TCHAR* Bone) { return Mesh->GetSocketTransform(Bone, RTS_World).Rotator().Yaw; };
		const auto FootDirection = [Mesh](const TCHAR* Foot, const TCHAR* Ball)
		{ return (Mesh->GetSocketLocation(Ball) - Mesh->GetSocketLocation(Foot)).Rotation().Yaw; };
		UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW BODY Remote=%d Phase=%s ServerTime=%.4f Pawn=%s ActorYaw=%.4f BaseAimYaw=%.4f BaseAimPitch=%.4f MeshWorldYaw=%.4f MeshRelativeYaw=%.4f SmoothingMode=%d SmoothingRotationYaw=%.4f SmoothingTranslation=%s StateInitialized=%d Stationary=%d StateFacingYaw=%.4f StateTurnDirection=%d TurnStart=%.4f RootYawOffset=%.4f TurnTime=%.4f TurnAlpha=%.4f TurnRight=%d RootWorldYaw=%.4f PelvisWorldYaw=%.4f SpineWorldYaw=%.4f ThighLeftYaw=%.4f ThighRightYaw=%.4f FootLeftYaw=%.4f FootRightYaw=%.4f FootLeftDirectionYaw=%.4f FootRightDirectionYaw=%.4f FootLeftPosition=%s FootRightPosition=%s Speed=%.4f GroundSpeed=%.4f"),
			bRemote, Phase, ServerTime, *Character->GetName(), Character->GetActorRotation().Yaw, Character->GetBaseAimRotation().Yaw,
			FRotator::NormalizeAxis(Character->GetBaseAimRotation().Pitch), Mesh->GetComponentRotation().Yaw, Mesh->GetRelativeRotation().Yaw,
			static_cast<int32>(Movement->NetworkSmoothingMode), Prediction ? Prediction->MeshRotationOffset.Rotator().Yaw : 0.0,
			Prediction ? *Prediction->MeshTranslationOffset.ToString() : TEXT("None"), Facing.bInitialized, Facing.bStationary,
			Facing.FacingYaw, Facing.TurnDirection, Facing.TurnStartedServerTime, Animation->RootYawOffset, Animation->TurnTime,
			Animation->TurnAlpha, Animation->bTurnRight, BoneYaw(TEXT("root")), BoneYaw(TEXT("pelvis")), BoneYaw(TEXT("spine_03")),
			BoneYaw(TEXT("thigh_l")), BoneYaw(TEXT("thigh_r")), BoneYaw(TEXT("foot_l")), BoneYaw(TEXT("foot_r")),
			FootDirection(TEXT("foot_l"), TEXT("ball_l")), FootDirection(TEXT("foot_r"), TEXT("ball_r")),
			*Mesh->GetSocketLocation(TEXT("foot_l")).ToString(), *Mesh->GetSocketLocation(TEXT("foot_r")).ToString(),
			Character->GetVelocity().Size2D(), Animation->GroundSpeed);
	}

	// 仅 opt-in 的连续双端画面证据；原有拾取、换弹、走跑、停步和 ADS 门禁全部通过后才进入。
	void TickFacingReview(APlayerController* Local, AfpstrueCharacter* Character, AfpstrueCharacter* Other, double Now)
	{
		const float ServerTime = Character->GetBodyFacingServerTime();
		if (!bFacingReviewStarted)
		{
			if (!FParse::Value(FCommandLine::Get(), TEXT("PvpFacingStartServerTime="), FacingReviewEpoch))
				FacingReviewEpoch = (FMath::FloorToFloat(ServerTime / 60.0f) + 1.0f) * 60.0f;
			if (FacingReviewEpoch < ServerTime - 0.2f)
			{ Finish(false, TEXT("FacingReviewSharedEpochAlreadyPassed")); return; }
			bFacingReviewStarted = true; FacingStartLocation = Character->GetActorLocation();
			SetForwardKey(false);
			UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW START Pawn=%s PlayerId=%d Epoch=%.4f CurrentServerTime=%.4f Input=PlayerControllerRotationInput AvatarTeleport=0 ActorRotationWrites=0"),
				*Character->GetName(), Character->GetPlayerState()->GetPlayerId(), FacingReviewEpoch, ServerTime);
		}
		const bool bFirst = Character->GetPlayerState()->GetPlayerId() < Other->GetPlayerState()->GetPlayerId();
		const float OwnStart = FacingReviewEpoch + (bFirst ? 0.0f : 8.0f);
		const float OtherStart = FacingReviewEpoch + (bFirst ? 8.0f : 0.0f);
		const float OwnTime = ServerTime - OwnStart, OtherTime = ServerTime - OtherStart;
		const bool bOwnActive = OwnTime >= 0 && OwnTime < 8.0f;
		const bool bOtherActive = OtherTime >= 0 && OtherTime < 8.0f;
		// 观察伙伴时瞄向身体中心以保留完整头脚画面；仍只转普通镜头，不移动相机或改变 FOV。
		const float LookHeight = bOtherActive && !bOwnActive ? 0.0f : 35.0f;
		const FRotator TowardOther = (Other->GetActorLocation() + FVector(0, 0, LookHeight) - Character->GetPawnViewLocation()).Rotation();
		const auto PhaseAt = [](float Time) -> const TCHAR*
		{
			if (Time < 0) return TEXT("Waiting");
			if (Time < 0.8f) return TEXT("BaselineIdle");
			if (Time < 2.0f) return TEXT("SmallYaw30");
			if (Time < 4.0f) return TEXT("RightYaw90");
			if (Time < 6.3f) return TEXT("LeftYaw90");
			if (Time < 7.0f) return TEXT("PitchUp100Input");
			if (Time < 7.5f) return TEXT("PitchDown100Input");
			return TEXT("ReturnIdle");
		};
		FRotator DesiredView = TowardOther;
		if (bOwnActive)
		{
			const float YawOffset = OwnTime < 0.8f ? 0.0f : OwnTime < 1.3f ? FMath::Lerp(0.0f, 30.0f, (OwnTime - 0.8f) / 0.5f)
				: OwnTime < 2.0f ? 30.0f : OwnTime < 2.6f ? FMath::Lerp(30.0f, 90.0f, (OwnTime - 2.0f) / 0.6f)
				: OwnTime < 4.0f ? 90.0f : OwnTime < 4.8f ? FMath::Lerp(90.0f, -90.0f, (OwnTime - 4.0f) / 0.8f) : -90.0f;
			DesiredView.Yaw += YawOffset; DesiredView.Pitch = OwnTime < 6.3f ? 0.0f : OwnTime < 7.0f ? 100.0f : OwnTime < 7.5f ? -100.0f : 0.0f;
			const float Pitch = FRotator::NormalizeAxis(Local->GetControlRotation().Pitch);
			if (OwnTime >= 6.3f && OwnTime < 7.0f && Pitch >= 74.5f && Pitch <= 75.5f) bFacingPitchUp = true;
			if (OwnTime >= 7.0f && OwnTime < 7.5f && Pitch <= -74.5f && Pitch >= -75.5f) bFacingPitchDown = true;
		}
		FacingLookInput(Local, DesiredView);
		if (FVector::Dist2D(FacingStartLocation, Character->GetActorLocation()) > 3.0 || Character->GetVelocity().Size2D() > 3.0)
		{ Finish(false, TEXT("FacingReviewMustRemainStationary")); return; }
		if (Now >= FacingNextSampleAt && (bOwnActive || bOtherActive))
		{
			FacingNextSampleAt = Now + 0.1;
			LogFacingPose(Character, false, PhaseAt(OwnTime), ServerTime); LogFacingPose(Other, true, PhaseAt(OtherTime), ServerTime);
			FVector ViewLocation; FRotator ViewRotation; Local->GetPlayerViewPoint(ViewLocation, ViewRotation);
			UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW CAMERA Phase=%s ControllerYaw=%.4f ControllerPitch=%.4f CameraYaw=%.4f CameraPitch=%.4f Location=%s"),
				PhaseAt(OwnTime), Local->GetControlRotation().Yaw, FRotator::NormalizeAxis(Local->GetControlRotation().Pitch),
				ViewRotation.Yaw, FRotator::NormalizeAxis(ViewRotation.Pitch), *ViewLocation.ToString());
			if (bOtherActive)
			{
				auto* Animation = Cast<UfpstruePlayerAnimInstance>(Other->GetMesh()->GetAnimInstance());
				if (Animation && Animation->TurnAlpha > 0.1f)
				{ bFacingRemoteRight |= Animation->bTurnRight; bFacingRemoteLeft |= !Animation->bTurnRight; }
				const FString Name = FString::Printf(TEXT("FacingRemote_%04d"), FacingCaptureCount);
				if (RequestCapture(*Name)) { FacingCaptures.Add(Name); ++FacingCaptureCount; }
			}
		}
		if (ServerTime < FacingReviewEpoch + 16.5f) return;
		for (const FString& Name : FacingCaptures)
			if (IFileManager::Get().FileSize(*FPaths::Combine(CaptureDirectory, Name + TEXT(".png"))) <= 0) return;
		if (FacingCaptureCount < 40 || !bFacingRemoteLeft || !bFacingRemoteRight || !bFacingPitchUp || !bFacingPitchDown)
		{ Finish(false, TEXT("FacingReviewMissingContinuousTurnOrPitchEvidence")); return; }
		bFacingReviewComplete = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_FACING_REVIEW PASS Pawn=%s Epoch=%.4f SavedFrames=%d RemoteLeft=%d RemoteRight=%d PitchUp75=%d PitchDown75=%d ActualRenderedGraph=1 VisualQualityReviewed=0"),
			*Character->GetName(), FacingReviewEpoch, FacingCaptureCount, bFacingRemoteLeft, bFacingRemoteRight, bFacingPitchUp, bFacingPitchDown);
	}

	// 新增的走跑段走标准 W 绑定；所有退出路径释放同一按键，不能只给 Timer 一次推力。
	void SetForwardKey(bool bPressed)
	{
		if (bForwardKeyHeld == bPressed) return;
		if (APlayerController* Local = Controller.Get())
			Local->InputKey(FInputKeyParams(EKeys::W, bPressed ? IE_Pressed : IE_Released, bPressed ? 1.0 : 0.0));
		bForwardKeyHeld = bPressed;
	}

	// 观察实际远端蒙皮鞋底与真实地面，区别于 root/ankle 骨点，也不修改碰撞或动画。
	bool CaptureRemoteLocomotion(AfpstrueCharacter* Other, UfpstruePlayerAnimInstance* Animation)
	{
		const TCHAR* Names[] = {TEXT("NormalRemoteWalk"), TEXT("NormalRemoteWalkStopIdle"), TEXT("NormalRemoteSprintMoving"), TEXT("NormalRemoteSprintStopIdle")};
		for (int32 Pending = 0; Pending < 4; ++Pending)
			if (RemoteLocomotionRequested[Pending] && !RemoteLocomotionSaved[Pending]
				&& IFileManager::Get().FileSize(*FPaths::Combine(CaptureDirectory, FString(Names[Pending]) + TEXT(".png"))) > 0)
			{
				RemoteLocomotionSaved[Pending] = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION SAVED Phase=%s Remote=1"), Names[Pending]);
			}
		const double Speed = Other->GetVelocity().Size2D();
		const bool bIdlePose = Speed < 2.0 && Animation->GroundSpeed < 2.0f && Animation->SprintAlpha < 0.02f;
		int32 Index = INDEX_NONE;
		if (!Other->IsSprinting() && Speed > 80.0 && Animation->GroundSpeed > 80.0f) Index = 0;
		else if (RemoteLocomotionSaved[0] && !Other->IsSprinting() && bIdlePose) Index = 1;
		else if (Other->IsSprinting() && Speed > 200.0 && Animation->GroundSpeed > 200.0f) Index = 2;
		else if (RemoteLocomotionSaved[2] && bIdlePose) Index = 3;
		if (Index == INDEX_NONE || RemoteLocomotionSaved[Index]) return true;
		if (!RemoteLocomotionRequested[Index])
		{
			USkeletalMeshComponent* Mesh = Other->GetMesh();
			USkeletalMesh* Body = Mesh->GetSkeletalMeshAsset();
			const FSkeletalMeshRenderData* Data = Body ? Body->GetResourceForRendering() : nullptr;
			FSkinWeightVertexBuffer* Weights = Mesh->GetSkinWeightBuffer(0);
			if (!Data || !Data->LODRenderData.IsValidIndex(0) || !Weights || !Weights->GetDataVertexBuffer()->GetWeightData())
			{ Finish(false, TEXT("RemoteLocomotionCPUSkinUnavailable")); return false; }
			const FSkeletalMeshLODRenderData& LOD = Data->LODRenderData[0];
			TArray<FMatrix44f> Matrices; Mesh->GetCurrentRefToLocalMatrices(Matrices, 0);
			FVector Sole = FVector(0, 0, TNumericLimits<double>::Max());
			for (uint32 Vertex = 0; Vertex < LOD.GetNumVertices(); ++Vertex)
			{
				const FVector Position = Mesh->GetComponentTransform().TransformPosition(FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(Mesh, Vertex, LOD, *Weights, Matrices)));
				if (Position.Z < Sole.Z) Sole = Position;
			}
			FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueLocomotionSole), false, Other);
			if (const auto* Weapon = Other->GetEquippedWeaponComponent()) Query.AddIgnoredActor(Weapon->GetOwner());
			FHitResult Floor;
			const bool bFloor = World->LineTraceSingleByChannel(Floor, Sole + FVector(0, 0, 80), Sole - FVector(0, 0, 300), ECC_Visibility, Query);
			const double Clearance = bFloor ? Sole.Z - Floor.ImpactPoint.Z : TNumericLimits<double>::Max();
			const bool bIdle = Index == 1 || Index == 3;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION Phase=%s Remote=1 Pawn=%s ActualSpeed=%.4f SnapshotSpeed=%.4f SprintIntent=%d SprintAlpha=%.5f Sole=%s Floor=%s SoleClearanceCm=%.5f BoneNotSole=0 FloorHit=%d"),
				Names[Index], *Other->GetName(), Speed, Animation->GroundSpeed, Other->IsSprinting(), Animation->SprintAlpha,
				*Sole.ToString(), *GetNameSafe(Floor.GetActor()), Clearance, bFloor);
			// 静止允许胶囊常规离地间隙；移动另容纳脚掌滚动，不把抬起的另一只脚当承重脚。
			if (!bFloor || !FMath::IsFinite(Clearance) || FMath::Abs(Clearance) > (bIdle ? 5.0 : 12.0))
			{ Finish(false, TEXT("RemoteLocomotionSoleNotGrounded")); return false; }
			RemoteLocomotionRequested[Index] = RequestCapture(Names[Index]);
			return !bFinished;
		}
		return true;
	}

	// 实机完整链路只驱动原有本地输入；不写生命、装备、位置或伤害结果。
	void TickFullGameplay(double Now)
	{
		UWorld* CurrentWorld = World.Get();
		APlayerController* Local = Controller.Get();
		AfpstrueCharacter* Original = Pawn.Get();
		if (!CurrentWorld || !Local) { Finish(false, TEXT("FullGameplayControllerLost")); return; }
		auto CaptureOnce = [this](const TCHAR* Name, bool& bRequested)
		{
			if (!bRequested) bRequested = RequestCapture(Name);
			return bRequested && IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, FString(Name) + TEXT(".png")));
		};
		auto SetFire = [this, Original](bool bPressed)
		{
			if (bCombatFireHeld == bPressed) return;
			if (Original && Original->GetAbilitySystemComponent())
			{
				if (bPressed) Original->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
				else Original->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
			}
			bCombatFireHeld = bPressed;
		};
		if (CombatPhase == 0)
		{
			if (!Original || Original->IsDead()) { Finish(false, TEXT("DiedBeforeNormalCombat")); return; }
			auto* Player = Cast<AfpstruePvpPlayerController>(Local);
			if (!Player) { Finish(false, TEXT("FullGameplayControllerType")); return; }
			CombatRouteRevision = Player->GetNormalVisualRouteRevision();
			Player->ServerRequestNormalVisualEnemyRoute();
			CombatPhase = 1; CombatPhaseAt = Now;
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY COMBAT_START Pawn=%s Shooter=%d AwaitNaturalRelevancy=1 NoDirectDamage=1"), *Original->GetName(), bCombatShooter);
			return;
		}
		if (CombatPhase == 1)
		{
			if (!Original || Original->IsDead()) { SetFire(false); Finish(false, TEXT("PlayerKilledBeforeEnemy")); return; }
			auto* Player = Cast<AfpstruePvpPlayerController>(Local);
			if (Player->GetNormalVisualRouteRevision() <= CombatRouteRevision) return;
			const TArray<FVector>& Route = Player->GetNormalVisualRoute();
			// 忠实经过导航拐点；提前 70cm 切角会让实际胶囊离开已经验证的路径走廊。
			while (Route.IsValidIndex(CombatRoutePoint) && FVector::DistSquared2D(Original->GetActorLocation(), Route[CombatRoutePoint]) < FMath::Square(20.0)) ++CombatRoutePoint;
			const bool bDiagnostic = Now >= NextPickupDiagnosticAt;
			if (bDiagnostic)
			{
				NextPickupDiagnosticAt = Now + 2.0;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY PROGRESS Pawn=%s Point=%d/%d Location=%s Speed=%.1f Enemy=%s DistanceCm=%.1f"),
					*Original->GetName(), CombatRoutePoint, Route.Num(), *Original->GetActorLocation().ToCompactString(), Original->GetVelocity().Size2D(),
					*GetNameSafe(CombatEnemy.Get()), CombatEnemy.IsValid() ? FVector::Dist2D(Original->GetActorLocation(), CombatEnemy->GetActorLocation()) : -1.0);
			}
			if (!CombatEnemy.IsValid())
				for (TActorIterator<AfpstrueEnemyCharacter> It(CurrentWorld); It; ++It)
					if (!It->IsDead())
					{
						CombatEnemy = *It;
						CombatEnemyInitialHealth = It->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth();
						UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_RELEVANT Enemy=%s Health=%.1f"), *It->GetName(), CombatEnemyInitialHealth);
						break;
					}
			AfpstrueEnemyCharacter* Enemy = CombatEnemy.Get();
			if (!Enemy)
			{
				// 远处对象可能尚无副本；先沿服务器路线正常走近，不扩大网络相关性或制造假 Actor。
				if (Route.IsValidIndex(CombatRoutePoint))
				{
					const FVector Direction = (Route[CombatRoutePoint] - Original->GetActorLocation()).GetSafeNormal2D();
					Local->SetControlRotation(Direction.Rotation()); Original->AddMovementInput(Direction, 1.0f, false);
				}
				return;
			}
			Local->SetControlRotation((Enemy->GetMesh()->GetSocketLocation(TEXT("spine_03")) - Original->GetPawnViewLocation()).Rotation());
			if (Enemy->IsDead())
			{
				SetFire(false);
				if (EnemyDeathObservedAt <= 0.0) EnemyDeathObservedAt = Now;
				if (!CaptureOnce(TEXT("NormalEnemyDeath"), bEnemyDeathCaptured)) return;
				// 同时保留真实死亡首帧与稳定帧，不能用延迟截图藏起错误的死态/站立表现。
				if (Now - EnemyDeathObservedAt < 0.8 || !CaptureOnce(TEXT("NormalEnemyDeathSettled"), bEnemyDeathSettledCaptured)) return;
				CombatPhase = 2; CombatPhaseAt = Now;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY ENEMY_KILLED Pawn=%s Enemy=%s Captured=1"), *Original->GetName(), *Enemy->GetName());
				return;
			}
			if (Enemy->FindComponentByClass<UfpstrueHealthComponent>()->GetHealth() < CombatEnemyInitialHealth)
				CaptureOnce(TEXT("NormalEnemyHit"), bEnemyHitCaptured);
			FCollisionQueryParams Visibility(SCENE_QUERY_STAT(FpstrueNormalCombat), false, Original);
			FHitResult Hit;
			const FVector AimPoint = Enemy->GetMesh()->GetSocketLocation(TEXT("spine_03"));
			const bool bClear = !CurrentWorld->LineTraceSingleByChannel(Hit, Original->GetPawnViewLocation(), AimPoint, ECC_Visibility, Visibility) || Hit.GetActor() == Enemy;
			if (bDiagnostic)
			{
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY SIGHT Pawn=%s Clear=%d Blocker=%s Ammo=%d"), *Original->GetName(), bClear,
					*GetNameSafe(Hit.GetActor()), Original->GetEquippedWeaponComponent() ? Original->GetEquippedWeaponComponent()->GetCurrentAmmo() : -1);
				if (Now - CombatPhaseAt > 5.0) CaptureOnce(TEXT("NormalEnemyApproach"), bApproachCaptured);
			}
			if (FVector::Dist2D(Original->GetActorLocation(), Enemy->GetActorLocation()) > 1200.0 || !bClear)
			{
				SetFire(false);
				if (Route.IsValidIndex(CombatRoutePoint)) Original->AddMovementInput((Route[CombatRoutePoint] - Original->GetActorLocation()).GetSafeNormal2D(), 1.0f, false);
				else if (bClear) Original->AddMovementInput((Enemy->GetActorLocation() - Original->GetActorLocation()).GetSafeNormal2D(), 1.0f, false);
				return;
			}
			// 两个真实玩家都反击敌人；不能让先到的一人只挨打，等待另一条路线上的观察者。
			{
				if (auto* Weapon = Original->GetEquippedWeaponComponent(); Weapon && Weapon->GetCurrentAmmo() == 0 && !Weapon->IsReloading())
				{
					SetFire(false);
					Original->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Reload));
					Original->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Reload));
				}
				else SetFire(true);
			}
			return;
		}
		if (CombatPhase == 2)
		{
			if (!Original) { Finish(false, TEXT("OriginalPawnLostBeforeDeath")); return; }
			AfpstrueCharacter* Victim = bCombatShooter ? Original : CombatPartner.Get();
			if (!Victim) { Finish(false, TEXT("PvpVictimLostBeforeDeath")); return; }
			if (!bCombatShooter) Local->SetControlRotation((Victim->GetMesh()->GetSocketLocation(TEXT("spine_03")) - Original->GetPawnViewLocation()).Rotation());
			if (Victim->GetCurrentHealth() < Victim->GetMaxHealth() && !Victim->IsDead()) CaptureOnce(TEXT("NormalPlayerHit"), bPlayerHitCaptured);
			if (Victim->IsDead())
			{
				SetFire(false);
				if (PlayerDeathObservedAt <= 0.0) PlayerDeathObservedAt = Now;
				if (!CaptureOnce(TEXT("NormalPlayerDeath"), bPlayerDeathCaptured)) return;
				if (Now - PlayerDeathObservedAt < 0.8 || !CaptureOnce(TEXT("NormalPlayerDeathSettled"), bPlayerDeathSettledCaptured)) return;
				CombatPhase = 3; CombatPhaseAt = Now;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY PLAYER_DIED Victim=%s Observer=%s Captured=1"), *Victim->GetName(), *Local->GetName());
				return;
			}
			if (!bCombatShooter && Now - CombatPhaseAt > 2.0) SetFire(true);
			return;
		}
		// 真实服务器重生换 Pawn；死亡计分和空手新生命由原规则负责，旧驱动绝不复活旧对象。
		if (CombatPhase == 3)
		{
			AfpstrueCharacter* NewPawn = nullptr;
			if (bCombatShooter) NewPawn = Cast<AfpstrueCharacter>(Local->GetPawn());
			else for (TActorIterator<AfpstrueCharacter> It(CurrentWorld); It; ++It)
				if (It->GetPlayerState() == CombatPartnerState.Get() && *It != CombatPartner.Get() && !It->IsDead()) { NewPawn = *It; break; }
			if (!NewPawn || NewPawn == (bCombatShooter ? Original : CombatPartner.Get()) || NewPawn->IsDead()) return;
			if (bCombatShooter)
			{
				auto* HUD = Cast<AfpstruePvpHUD>(Local->GetHUD());
				if (!HUD || !HUD->HasDrawnCurrentState(NewPawn) || NewPawn->HasEquippedWeapon()) return;
				if (!CaptureOnce(TEXT("NormalPlayerRespawn"), bRespawnCaptured)) return;
			}
			UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_GAMEPLAY COMPLETE NewPawn=%s LocalVictim=%d EmptyHands=%d Health=%.1f"),
				*NewPawn->GetName(), bCombatShooter, !NewPawn->HasEquippedWeapon(), NewPawn->GetCurrentHealth());
			Finish(true, TEXT("RealEnemyCombatPlayerDeathAndNormalRespawn"));
		}
	}

	// 统计当前世界尚存的旧武器副本，验证联机替换是否完整；不主动回收它们。
	int32 CountLegacyPickups() const
	{
		int32 Count = 0;
		if (UWorld* CurrentWorld = World.Get())
			for (TActorIterator<AActor> It(CurrentWorld); It; ++It)
				if (IsValid(*It) && (It->GetClass() == AfpstrueRARWeapon::StaticClass()
					|| It->GetClass()->GetPathName() == TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon.BP_Weapon_C"))) ++Count;
		return Count;
	}

	// 单次结束验收，释放已按住的输入和 Timer，保留实际结果与未通过原因。
	void Finish(bool bPassed, const TCHAR* Reason)
	{
		if (bFinished) return;
		bFinished = true;
		SetForwardKey(false);
		if (PairActionPhase > 0)
			if (AfpstrueCharacter* Character = Pawn.Get())
				if (UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent())
				{
					ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
					ASC->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
				}
		if (UWorld* CurrentWorld = World.Get()) CurrentWorld->GetTimerManager().ClearTimer(Timer);
		UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK %s Reason=%s LegacyRemaining=%d Scope=ClientCharacterMovementAndRealOverlap_NotEnhancedInputBinding_NotVisualQualityAcceptance"),
			bPassed ? TEXT("PASS") : TEXT("FAIL"), Reason, CountLegacyPickups());
	}

	// 仅向空闲截图系统提交不覆盖的请求；返回 true 只是已请求，不等于 PNG 已生成。
	bool RequestCapture(const TCHAR* Name)
	{
		if (FScreenshotRequest::IsScreenshotRequested()) return false;
		const FString Filename = FPaths::Combine(CaptureDirectory, FString(Name) + TEXT(".png"));
		if (IFileManager::Get().FileExists(*Filename)) { Finish(false, TEXT("CaptureAlreadyExists")); return false; }
		FScreenshotRequest::RequestScreenshot(Filename, true, false, false);
		UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK CAPTURE_REQUEST Name=%s File=%s"), Name, *Filename);
		return true;
	}

	// 在限定期限内推进真实客户端拾取/双端表现门禁；Pawn 丢失即失败，不传送或伪造装备状态。
	void Tick()
	{
		if (bFinished) return;
		UWorld* CurrentWorld = World.Get();
		const double Now = FPlatformTime::Seconds();
		if (CurrentWorld == nullptr || CurrentWorld->bIsTearingDown || CurrentWorld->GetNetMode() != NM_Client)
		{ Finish(false, TEXT("ClientWorldEnded")); return; }
		if (Now >= Deadline) { Finish(false, TEXT("AcceptanceDeadline")); return; }
		if (bFullGameplayStarted) { TickFullGameplay(Now); return; }
		if (!bLockedPawn)
		{
			APlayerController* Local = CurrentWorld->GetFirstPlayerController();
			if (Local == nullptr || !Local->IsLocalController()) return;
			auto* Character = Cast<AfpstrueCharacter>(Local->GetPawn());
			if (Character == nullptr) return; // 可由启动 ExecCmds 先等待本地连接真正产生 Pawn。
			if (Character->IsDead() || Character->HasEquippedWeapon()) { Finish(false, TEXT("RequiresLivingEmptyHands")); return; }
			Controller = Local; Pawn = Character; bLockedPawn = true;
			StartLocation = Character->GetActorLocation();
			UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK START Pawn=%s LegacyRemaining=%d Scope=AddMovementInputThroughCharacterMovement_NotEnhancedInputBinding"),
				*Character->GetName(), CountLegacyPickups());
		}
		APlayerController* Local = Controller.Get();
		AfpstrueCharacter* Character = Pawn.Get();
		if (Local == nullptr || Character == nullptr || Character->IsDead() || !Local->IsLocalController()
			|| Local->GetWorld() != CurrentWorld || Character->GetWorld() != CurrentWorld
			|| Local->GetPawn() != Character || Character->GetController() != Local || !Character->IsLocallyControlled())
		{ Finish(false, TEXT("OriginalPawnOrControllerLost")); return; }
		auto* HUD = Cast<AfpstruePvpHUD>(Local->GetHUD());
		UfpstrueWeaponComponent* Equipped = Character->GetEquippedWeaponComponent();
		if (!bWalking)
		{
			if (Equipped != nullptr) { Finish(false, TEXT("EquippedBeforeWalking")); return; }
			if (!Character->GetMesh1P()->bHiddenInGame || HUD == nullptr || !HUD->HasDrawnCurrentState(Character)) return;
			if (!bEmptyCaptureRequested) { bEmptyCaptureRequested = RequestCapture(TEXT("EmptyHands")); return; }
			if (!IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, TEXT("EmptyHands.png")))) return;
			double Nearest = TNumericLimits<double>::Max();
			for (TActorIterator<AfpstrueTemplateWeapon> It(CurrentWorld); It; ++It)
			{
				auto* Weapon = It->FindComponentByClass<UfpstrueWeaponComponent>();
				if (!IsValid(*It) || !It->GetIsReplicated() || It->HasAuthority() || It->GetOwner() != nullptr
					|| Weapon == nullptr || Weapon->GetOwningCharacter() != nullptr) continue;
				const double Distance = FVector::DistSquared2D(Character->GetActorLocation(), It->GetActorLocation());
				if (Distance < Nearest) { Nearest = Distance; Target = *It; }
			}
			if (!Target.IsValid()) return;
			bWalking = true;
			UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK WALK Target=%s DistanceCm=%.1f LegacyRemaining=%d EmptyHandsHidden=1"),
				*Target->GetName(), FMath::Sqrt(Nearest), CountLegacyPickups());
		}
		AfpstrueTemplateWeapon* Rifle = Target.Get();
		if (Rifle == nullptr) { Finish(false, TEXT("TargetPickupDestroyed")); return; }
		UfpstrueWeaponComponent* ExpectedWeapon = Rifle->FindComponentByClass<UfpstrueWeaponComponent>();
		if (Equipped == nullptr)
		{
			if (Now >= NextPickupDiagnosticAt)
			{
				NextPickupDiagnosticAt = Now + 2.0;
				UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK PROGRESS Pawn=%s Location=%s Pickup=%s Location=%s DistanceCm=%.1f Owner=%s Holder=%s Speed=%.1f"),
					*Character->GetName(), *Character->GetActorLocation().ToCompactString(), *Rifle->GetName(), *Rifle->GetActorLocation().ToCompactString(),
					FVector::Dist2D(Character->GetActorLocation(), Rifle->GetActorLocation()), *GetNameSafe(Rifle->GetOwner()),
					*GetNameSafe(ExpectedWeapon ? ExpectedWeapon->GetOwningCharacter() : nullptr), Character->GetVelocity().Size2D());
			}
			if (ExpectedWeapon == nullptr || (Rifle->GetOwner() != nullptr && Rifle->GetOwner() != Character)
				|| (ExpectedWeapon->GetOwningCharacter() != nullptr && ExpectedWeapon->GetOwningCharacter() != Character))
			{ Finish(false, TEXT("TargetTakenByAnotherPlayer")); return; }
			// 自己的 Owner/Holder 先到时只等待装备引用，不能把合法复制顺序误报成被别人拾走。
			if (Rifle->GetOwner() == Character || ExpectedWeapon->GetOwningCharacter() == Character) return;
			const FVector Direction = (Rifle->GetActorLocation() - Character->GetActorLocation()).GetSafeNormal2D();
			// 不传送、不直接装备、不发送指定枪支 RPC：服务器真实 Overlap 决定拾取结果。
			Character->AddMovementInput(Direction, 1.0f, false);
			return;
		}
		if (Equipped != ExpectedWeapon) { Finish(false, TEXT("UnexpectedEquipment")); return; }
		const bool bPresented = Equipped->GetOwningCharacter() == Character && Rifle->GetOwner() == Character
			&& Equipped->GetAttachParent() == Character->GetMesh1P() && !Equipped->bHiddenInGame
			&& !Character->GetMesh1P()->bHiddenInGame && HUD != nullptr && HUD->HasDrawnCurrentState(Character);
		if (!bPresented) return; // 装备引用、Owner、弹药快照与本地 HUD 允许分批到达。
		if (EquippedAt <= 0.0)
		{
			EquippedAt = Now;
			UE_LOG(LogTemp, Display, TEXT("PVP_PICKUP_WALK EQUIPPED Pawn=%s Weapon=%s Ammo=%d Reserve=%d MovedCm=%.1f HUDDrawn=1 ArmsHidden=0 LegacyRemaining=%d"),
				*Character->GetName(), *Rifle->GetName(), Equipped->GetCurrentAmmo(), Equipped->GetReserveAmmo(),
				FVector::Dist2D(StartLocation, Character->GetActorLocation()), CountLegacyPickups());
		}
		if (Now - EquippedAt < 0.5) return;
		if (!bEquippedCaptureRequested) { bEquippedCaptureRequested = RequestCapture(TEXT("PickupEquipped")); return; }
		if (!IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, TEXT("PickupEquipped.png")))) return;
		if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPair")))
		{
			// 普通玩法双方都走真实拾取后，再验证观察端的常态持枪；不借烟测自动发枪或直接附着造结果。
			AfpstrueCharacter* Other = nullptr;
			for (TActorIterator<AfpstrueCharacter> It(CurrentWorld); It; ++It)
				if (*It != Character && !It->IsDead() && It->GetPlayerState() && It->HasEquippedWeapon()) { Other = *It; break; }
			if (!Other || !Character->GetPlayerState()) return;
			if (bFacingReviewStarted && !bFacingReviewComplete) { TickFacingReview(Local, Character, Other, Now); return; }
			if (Other->IsAiming()) bPartnerAimObserved = true;
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPairWalk")) && !bRendezvousReached)
			{
				const auto* PvpPlayer = Cast<AfpstruePvpPlayerController>(Local);
				if (!PvpPlayer || PvpPlayer->GetNormalVisualRoute().IsEmpty()) return;
				const TArray<FVector>& Route = PvpPlayer->GetNormalVisualRoute();
				while (Route.IsValidIndex(NormalRoutePoint) && FVector::DistSquared2D(Character->GetActorLocation(), Route[NormalRoutePoint]) < FMath::Square(60.0)) ++NormalRoutePoint;
				if (Route.IsValidIndex(NormalRoutePoint))
				{
					const FVector Direction = (Route[NormalRoutePoint] - Character->GetActorLocation()).GetSafeNormal2D();
					Local->SetControlRotation(Direction.Rotation());
					Character->AddMovementInput(Direction, 1.0f, false);
					return;
				}
				if (FVector::Dist2D(Character->GetActorLocation(), Other->GetActorLocation()) > 600.0) return;
				bRendezvousReached = true;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_PAIR RENDEZVOUS Pawn=%s DistanceCm=%.1f RoutePoints=%d Teleport=0"),
					*Character->GetName(), FVector::Dist2D(Character->GetActorLocation(), Other->GetActorLocation()), Route.Num());
			}
			UfpstrueWeaponComponent* OtherWeapon = Other->GetEquippedWeaponComponent();
			USkeletalMeshComponent* OtherBody = Other->GetMesh();
			auto* OtherAnimation = Cast<UfpstruePlayerAnimInstance>(OtherBody->GetAnimInstance());
			if (!OtherAnimation || !OtherAnimation->bArmed || OtherBody->bHiddenInGame || !OtherBody->IsVisible()
				|| !OtherWeapon || OtherWeapon->GetOwningCharacter() != Other || OtherWeapon->GetAttachParent() != OtherBody
				|| OtherWeapon->bHiddenInGame || !OtherWeapon->IsVisible() || OtherWeapon->bOnlyOwnerSee) return;
			if (!bPairCaptureRequested && FVector::Dist2D(Character->GetActorLocation(), Other->GetActorLocation()) < 250.0) return;
			Local->SetControlRotation((Other->GetActorLocation() + FVector(0, 0, 35) - Character->GetPawnViewLocation()).Rotation());
			if (OtherWeapon->GetReserveAmmo() < OtherWeapon->GetWeaponSettings().StartingReserveAmmo && !OtherWeapon->IsReloading()
				&& !CaptureRemoteLocomotion(Other, OtherAnimation)) return;
			if (PairHoldingAt <= 0.0) { PairHoldingAt = Now; return; }
			if (Now - PairHoldingAt < 0.6) return;
			if (!bPairCaptureRequested)
			{
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_PAIR READY Local=%s Remote=%s PlayerId=%d Body=%s Anim=%s Gun=%s Parent=%s Socket=%s Armed=1 GunVisible=1"),
					*Character->GetName(), *Other->GetName(), Other->GetPlayerState()->GetPlayerId(), *GetPathNameSafe(OtherBody->GetSkeletalMeshAsset()),
					*OtherAnimation->GetClass()->GetPathName(), *GetNameSafe(OtherWeapon->GetOwner()), *GetNameSafe(OtherWeapon->GetAttachParent()),
					*OtherWeapon->GetAttachSocketName().ToString());
				bPairCaptureRequested = RequestCapture(TEXT("NormalRemoteHolding"));
				return;
			}
			if (!IFileManager::Get().FileExists(*FPaths::Combine(CaptureDirectory, TEXT("NormalRemoteHolding.png")))) return;
			if (PairActionPhase == 0)
			{
				if (Character->GetPlayerState()->GetPlayerId() > Other->GetPlayerState()->GetPlayerId()
					&& (!bPartnerAimObserved || Other->IsAiming() || Other->IsSprinting() || OtherWeapon->IsReloading())) return;
				// 等双方普通视角稳定及截图完成，不能把启动/转镜头期间错过的瞬时表现当成同步失败。
				if (PairActionAt <= 0.0) { PairActionAt = Now; return; }
				if (Now - PairActionAt < 3.0) return;
				// 先由较小 PlayerId 动作，另一端看到它真实换弹后再交换，双方都必须展示自己的第三人称动作。
				if (Character->GetPlayerState()->GetPlayerId() > Other->GetPlayerState()->GetPlayerId()
					&& OtherWeapon->GetReserveAmmo() >= OtherWeapon->GetWeaponSettings().StartingReserveAmmo) return;
				PairInitialReserve = Equipped->GetReserveAmmo();
				const float AwayYaw = (Other->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw + 70.0f;
				Local->SetControlRotation(FRotator(25.0f, AwayYaw, 0));
				Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Fire));
				PairActionPhase = 1; PairActionAt = Now;
				return;
			}
			if (PairActionPhase == 1)
			{
				// 首段保持朝空旷方向射击，不能为了演示动画而由测试直接修改生命或弹药。
				const float AwayYaw = (Other->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw + 70.0f;
				Local->SetControlRotation(FRotator(25.0f, AwayYaw, 0));
				if (Now - PairActionAt < 0.35) return;
				Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Fire));
				PairActionPhase = 2; PairActionAt = Now; return;
			}
			if (PairActionPhase == 2)
			{
				if (Now - PairActionAt < 0.35 || Equipped->GetCurrentAmmo() >= Equipped->GetMagazineSize()) return;
				Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Reload));
				Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Reload));
				PairActionPhase = 3; PairActionAt = Now; return;
			}
			if (PairActionPhase == 3)
			{
				if (Equipped->IsReloading() || Equipped->GetCurrentAmmo() != Equipped->GetMagazineSize()
					|| Equipped->GetReserveAmmo() >= PairInitialReserve) return;
				if (!Local->PlayerInput) { Finish(false, TEXT("NormalLocomotionMissingPlayerInput")); return; }
				// 先验证真实胶囊可走的短直线，再走出/跑回，不传送或绕过关卡碰撞。
				FCollisionQueryParams Query(SCENE_QUERY_STAT(FpstrueNormalMove), false, Character);
				const auto* Capsule = Character->GetCapsuleComponent();
				const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
				const float BaseYaw = (Other->GetActorLocation() - Character->GetActorLocation()).Rotation().Yaw + 90.0f;
				bool bClear = false;
				for (int32 Attempt = 0; Attempt < 8; ++Attempt)
				{
					const float Yaw = BaseYaw + Attempt * 45.0f;
					const FVector Direction = FRotator(0, Yaw, 0).Vector();
					bool bBlocked[2] = {};
					for (int32 Side = 0; Side < 2; ++Side)
					{
						FHitResult Hit;
						// 走出约 250cm 后沿原路跑回；起点后方只预留制动余量，不要求整条反向走廊。
						const FVector End = Character->GetActorLocation() + Direction * (Side == 0 ? 450.0f : -100.0f);
						bBlocked[Side] = CurrentWorld->SweepSingleByProfile(Hit, Character->GetActorLocation(), End, FQuat::Identity, Capsule->GetCollisionProfileName(), Shape, Query);
						if (bBlocked[Side])
							UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION CORRIDOR_BLOCK Attempt=%d Side=%d Yaw=%.2f Pawn=%s Start=%s End=%s Radius=%.4f HalfHeight=%.4f Profile=%s HitActor=%s HitComponent=%s HitNormal=%s ImpactNormal=%s StartPenetrating=%d PenetrationDepth=%.4f Distance=%.4f Time=%.5f FloorDist=%.4f CurrentFloor=%s CurrentFloorWalkable=%d"),
								Attempt, Side, Yaw, *Character->GetName(), *Character->GetActorLocation().ToString(), *End.ToString(),
								Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight(), *Capsule->GetCollisionProfileName().ToString(),
								*GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), *Hit.Normal.ToString(), *Hit.ImpactNormal.ToString(), Hit.bStartPenetrating,
								Hit.PenetrationDepth, Hit.Distance, Hit.Time, Character->GetCharacterMovement()->CurrentFloor.FloorDist,
								*GetNameSafe(Character->GetCharacterMovement()->CurrentFloor.HitResult.GetActor()), Character->GetCharacterMovement()->CurrentFloor.IsWalkableFloor());
					}
					if (!bBlocked[0] && !bBlocked[1])
					{ PairMovementYaw = Yaw; bClear = true; break; }
				}
				if (!bClear) { Finish(false, TEXT("NormalLocomotionNoClearCapsuleCorridor")); return; }
				Local->SetControlRotation(FRotator(0, PairMovementYaw, 0));
				PairMovementPeak = 0; SetForwardKey(true);
				PairActionPhase = 7; PairActionAt = Now; return;
			}
			if (PairActionPhase == 7 || PairActionPhase == 9)
			{
				Local->SetControlRotation(FRotator(0, PairMovementYaw + (PairActionPhase == 9 ? 180.0f : 0.0f), 0));
				PairMovementPeak = FMath::Max(PairMovementPeak, static_cast<float>(Character->GetVelocity().Size2D()));
				const bool bFootIKReview = FParse::Param(FCommandLine::Get(), TEXT("PvpFootIKReview"));
				const double MovementDuration = PairActionPhase == 7 ? (bFootIKReview ? 1.4 : 0.9) : (bFootIKReview ? 0.8 : 0.5);
				if (Now - PairActionAt < MovementDuration) return;
				SetForwardKey(false);
				if (PairMovementPeak < Character->GetCharacterMovement()->MaxWalkSpeed * 0.95f)
				{ Finish(false, TEXT("NormalLocomotionTrueWDidNotReachSpeed")); return; }
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION INPUT Phase=%s PeakActualSpeed=%.4f ConfiguredMaxSpeed=%.4f Input=StandardW Released=1"),
					PairActionPhase == 7 ? TEXT("Walk") : TEXT("Sprint"), PairMovementPeak, Character->GetCharacterMovement()->MaxWalkSpeed);
				PairActionPhase = PairActionPhase == 7 ? 8 : 10; PairActionAt = Now; return;
			}
			if (PairActionPhase == 8 || PairActionPhase == 10)
			{
				auto* Animation = Cast<UfpstruePlayerAnimInstance>(Character->GetMesh()->GetAnimInstance());
				if (Now - PairActionAt < 1.0 || Character->GetVelocity().Size2D() >= 2.0 || !Animation
					|| Animation->GroundSpeed >= 2.0f || Animation->SprintAlpha >= 0.02f) return;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_LOCOMOTION STOP Phase=%s ActualSpeed=%.4f SnapshotSpeed=%.4f SprintIntent=%d SprintAlpha=%.5f Idle=1"),
					PairActionPhase == 8 ? TEXT("Walk") : TEXT("Sprint"), Character->GetVelocity().Size2D(), Animation->GroundSpeed, Character->IsSprinting(), Animation->SprintAlpha);
				if (PairActionPhase == 8)
				{
					Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Sprint));
					Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Sprint));
					PairMovementPeak = 0; Local->SetControlRotation(FRotator(0, PairMovementYaw + 180.0f, 0)); SetForwardKey(true);
					PairActionPhase = 9;
				}
				else PairActionPhase = 4;
				PairActionAt = Now; return;
			}
			if (PairActionPhase == 4)
			{
				if (Now - PairActionAt < 0.8) return;
				Character->GetAbilitySystemComponent()->AbilityLocalInputPressed(static_cast<int32>(EFPCombatAbilityInput::Aim));
				PairActionPhase = 5; PairActionAt = Now; return;
			}
			if (PairActionPhase == 5)
			{
				if (Now - PairActionAt < 0.8) return;
				Character->GetAbilitySystemComponent()->AbilityLocalInputReleased(static_cast<int32>(EFPCombatAbilityInput::Aim));
				PairActionPhase = 6; PairActionAt = Now;
				UE_LOG(LogTemp, Display, TEXT("PVP_NORMAL_PAIR ACTIONS_COMPLETE Pawn=%s PlayerId=%d Ammo=%d Reserve=%d Gameplay=ProductionPickupAndOwnedAbilityInputs"),
					*Character->GetName(), Character->GetPlayerState()->GetPlayerId(), Equipped->GetCurrentAmmo(), Equipped->GetReserveAmmo());
				return;
			}
			// 动作完成后仍观察伙伴数秒，让双方都能截到对方的换弹、瞄准和冲刺，不只拍拥有者视模。
			if (PairActionPhase == 6 && (Now - PairActionAt < 8.0 || !RemoteLocomotionSaved[0] || !RemoteLocomotionSaved[1]
				|| !RemoteLocomotionSaved[2] || !RemoteLocomotionSaved[3])) return;
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpFacingReview")) && !bFacingReviewComplete)
			{ TickFacingReview(Local, Character, Other, Now); return; }
			if (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalFullGameplay")))
			{
				bFullGameplayStarted = true;
				bCombatShooter = Character->GetPlayerState()->GetPlayerId() < Other->GetPlayerState()->GetPlayerId();
				CombatPartner = Other; CombatPartnerState = Other->GetPlayerState();
				TickFullGameplay(Now);
				return;
			}
		}
		Finish(CountLegacyPickups() == 0, CountLegacyPickups() == 0 ? TEXT("RealPickupAndPaintedHudVerified") : TEXT("LegacyPickupReplicaRemains"));
	}
};

// 只为显式普通有画面客户端创建一份短期验收状态；Timer 持有状态，世界销毁即停止。
void Start(UWorld* World)
{
	static TWeakPtr<FWalkState> Active;
	if (World == nullptr || !World->IsGameWorld() || World->GetNetMode() != NM_Client
		|| FParse::Param(FCommandLine::Get(), TEXT("PvpSmokeTest")) || !FApp::CanEverRender())
	{ UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_WALK FAIL Reason=RequiresNormalRenderedClient")); return; }
	if (Active.IsValid()) { UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_WALK REJECT Reason=AlreadyRunning")); return; }
	auto State = MakeShared<FWalkState>();
	State->World = World; State->Deadline = FPlatformTime::Seconds() + (FParse::Param(FCommandLine::Get(), TEXT("PvpNormalPair")) ? 240.0 : 30.0);
	if (!FParse::Value(FCommandLine::Get(), TEXT("PvpSmokeCaptureDir="), State->CaptureDirectory)
		|| State->CaptureDirectory.IsEmpty() || FPaths::IsRelative(State->CaptureDirectory)
		|| !IFileManager::Get().MakeDirectory(*State->CaptureDirectory, true))
	{ UE_LOG(LogTemp, Warning, TEXT("PVP_PICKUP_WALK FAIL Reason=RequiresWritableAbsolutePvpSmokeCaptureDir")); return; }
	Active = State;
	World->GetTimerManager().SetTimer(State->Timer, FTimerDelegate::CreateLambda([State]()
	{
		const auto KeepAlive = State;
		KeepAlive->Tick();
	}), 0.03f, true);
}

FAutoConsoleCommandWithWorld PickupWalkCommand(TEXT("fp.AuditPickupWalk"),
	TEXT("Normal rendered client only: walk through CharacterMovement to the nearest replicated template pickup, verify real overlap/HUD, and capture empty/equipped views. Does not test EnhancedInput bindings."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&Start));
}

#endif // WITH_EDITOR
