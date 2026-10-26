// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "Weapons/fpstrueWeaponComponent.h"
#include "Weapons/fpstrueWeaponActors.h"
#include "Weapons/fpstrueCombatAbilities.h"
#include "AbilitySystemComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Player/fpstruePlayerAnimInstance.h"
#include "Characters/Player/fpstrueCharacterMovementComponent.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelScriptActor.h"
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
#include "Game/fpstruePvpGameMode.h"
#include "Game/fpstruePvpPlayerController.h"
#include "UI/fpstruePvpHUD.h"
#include "GameFramework/WorldSettings.h"
#include "K2Node_InputKey.h"
#include "K2Node_MacroInstance.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/ProgressBar.h"
#include "Animation/WidgetAnimation.h"
#include "Kismet/KismetTextLibrary.h"
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

// 地图合并/重存可能遗留同一个 Level Blueprint 的多个 Actor。只保留 ULevel 正式登记的对象。
// 默认只读；Apply 备份当前地图后删除同类副本，不按名字后缀猜测谁是主对象。
void RepairDuplicateLevelScripts(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	if (!GEditor || GEditor->PlayWorld || (Args.Num() && !bApply)) return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	ULevel* Level = World ? World->PersistentLevel : nullptr;
	ALevelScriptActor* Official = Level ? Level->GetLevelScriptActor() : nullptr;
	if (!Official) { Fail(TEXT("Current editor map has no registered LevelScriptActor.")); return; }
	TArray<ALevelScriptActor*> Duplicates;
	for (TActorIterator<ALevelScriptActor> It(World); It; ++It)
		if (It->GetLevel() == Level && *It != Official)
		{
			if (It->GetClass() != Official->GetClass()) { Fail(TEXT("Different script classes require manual review; no map edited.")); return; }
			Duplicates.Add(*It);
		}
	UE_LOG(LogTemp, Display, TEXT("LEVEL_SCRIPT_REPAIR Map=%s Official=%s Duplicates=%d Apply=%d"),
		*World->GetOutermost()->GetName(), *Official->GetPathName(), Duplicates.Num(), bApply);
	if (!bApply || Duplicates.IsEmpty()) return;
	const FString Filename = FPackageName::LongPackageNameToFilename(World->GetOutermost()->GetName(), FPackageName::GetMapPackageExtension());
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/LevelScripts") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!BackupAsset(Filename, Backup)) return;
	for (ALevelScriptActor* Extra : Duplicates)
		if (!World->EditorDestroyActor(Extra, true)) { Fail(TEXT("Editor refused duplicate script removal; map not saved.")); return; }
	if (Level->GetLevelScriptActor() != Official) { Fail(TEXT("Registered script changed; map not saved.")); return; }
	World->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({World->GetOutermost()}, false)) { Fail(TEXT("Script repair save failed; backup retained.")); return; }
	UE_LOG(LogTemp, Display, TEXT("LEVEL_SCRIPT_REPAIR SAVED Removed=%d Backup=%s"), Duplicates.Num(), *Backup);
}
FAutoConsoleCommand RepairLevelScriptsCommand(TEXT("fp.RepairDuplicateLevelScripts"),
	TEXT("Check duplicate level scripts in the current editor map. Apply preserves the ULevel-registered actor, backs up and saves only this map."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&RepairDuplicateLevelScripts));

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

void Run(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0].Equals(TEXT("Apply"), ESearchCase::IgnoreCase);
	const bool bCheck = Args.IsEmpty() || (Args.Num() == 1 && Args[0].Equals(TEXT("Check"), ESearchCase::IgnoreCase));
	if ((!bApply && !bCheck) || !GEditor || GEditor->PlayWorld)
	{ Fail(TEXT("Usage: fp.MigrateMultiplayerOnly [Check|Apply], outside PIE.")); return; }
	UWorld* Previous = GEditor->GetEditorWorldContext().World();
	if (Previous && Previous->GetOutermost()->IsDirty()) { Fail(TEXT("Current map has unsaved edits.")); return; }
	bool bSafe = true;
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
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY CHECK_COMPLETE Safe=%d MapsToChange=%d NoAssetsSaved=1"), bSafe, ChangedMaps.Num());
	if (!bApply) return;
	if (!bSafe) { Fail(TEXT("Unrecognized scene logic touches the old UI chain. Review Check output; no assets changed.")); return; }
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/MultiplayerOnly") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	for (const FString& Package : ChangedMaps)
		if (!BackupAsset(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension()), Backup)) return;
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
	UE_LOG(LogTemp, Display, TEXT("MP_ONLY APPLIED Maps=%d Backup=%s UnrelatedSceneLogicRetained=1"), ChangedMaps.Num(), *Backup);
}

FAutoConsoleCommand PreparePvpAssets(TEXT("fp.PreparePvpAssets"),
	TEXT("Check/Apply multiplayer-only migration of fixed map GameModes and legacy level UI chains; network HUD uses fp.PrepareNetworkHUD."),
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
constexpr const TCHAR* PlayerPackage = TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter");

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
		UE_LOG(LogTemp, Display, TEXT("AI_WORLD Enemy=%s NetMode=%d Pos=%s Yaw=%.1f Dead=%d Controller=%s BrainRunning=%d Accepting=%d SightEnabled=%d Visible=%d Target=%s BBHasTarget=%d AIState=%d MoveStatus=%d MoveMode=%d MovementTick=%d Speed=%.1f MaxSpeed=%.1f Nearest=%s DistanceCm=%.1f ViewAngle=%.1f"),
			*Enemy->GetName(), static_cast<int32>(World->GetNetMode()), *Enemy->GetActorLocation().ToCompactString(), Enemy->GetActorRotation().Yaw, Enemy->IsDead(),
			*GetNameSafe(Controller), Brain != nullptr && Brain->IsRunning(), Controller != nullptr && Controller->AcceptsCombatCommands(),
			Perception != nullptr && Perception->IsSenseEnabled(UAISense_Sight::StaticClass()), Visible.Num(), *GetNameSafe(Controller != nullptr ? Controller->GetTargetCharacter() : nullptr),
			Blackboard != nullptr && Blackboard->GetValueAsBool(TEXT("HasTarget")), Controller != nullptr ? static_cast<int32>(Controller->GetAIState()) : -1,
			Controller != nullptr ? static_cast<int32>(Controller->GetMoveStatus()) : -1, Movement != nullptr ? static_cast<int32>(Movement->MovementMode) : -1,
			Movement != nullptr && Movement->IsComponentTickEnabled(), Enemy->GetVelocity().Size(), Movement != nullptr ? Movement->MaxWalkSpeed : 0.0f,
			*GetNameSafe(Nearest), Nearest != nullptr ? FMath::Sqrt(DistanceSquared) : -1.0, Angle);
		DrawDebugString(World, FVector(0.0f, 0.0f, 115.0f), TEXT("MELEE AI"), Enemy, FColor::Yellow, 60.0f, true);
		++Enemies;
	}
	UE_LOG(LogTemp, Display, TEXT("RAR_WORLD END Players=%d MeleeAI=%d NetMode=%d"), Players, Enemies, static_cast<int32>(World->GetNetMode()));
}

FAutoConsoleCommandWithWorld WorldAuditCommand(TEXT("fp.AuditRARWorld"),
	TEXT("Inspect actual player equipment, visibility and hand/socket/world-gun positions; temporarily label players and melee AI without changing gameplay."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&AuditWorld));

}

// UE5.5 成套素材的项目表现适配及窄修复；Apply 只写明示目标，原供应方玩法蓝图不参与运行。
namespace FpstrueTemplatePresentation
{
using namespace FpstrueRARPresentation;
using FpstrueRARPresentation::Error;
const FString Destination = TEXT("/Game/FirstPerson/Animations/FPSMPT/");
const FString Art = TEXT("/Game/FPSMPT55/Animations/");
constexpr const TCHAR* PlayerPath = TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter");



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

// 网络 HUD 的 Designer 和执行图是唯一表现来源；这里只离线维护一个白名单包。
bool ValidateNetworkHUD(UWidgetBlueprint& HUD)
{
	using namespace FpstruePvpAssetPreparation;
	if (HUD.GetOutermost()->GetName() != TEXT("/Game/FirstPerson/UI/ingame_network")
		|| HUD.ParentClass != UfpstruePvpIngameWidget::StaticClass() || !HUD.WidgetTree || !HUD.Bindings.IsEmpty())
		return Error(TEXT("Network HUD package, parent, tree or zero-binding contract failed."));
	for (const FName Name : {TEXT("HealthBar"), TEXT("MagazineText"), TEXT("ReserveText"), TEXT("WeaponNameText"),
		TEXT("MatchHintText"), TEXT("AmmoContainer"), TEXT("CountdownRow")})
		if (!HUD.WidgetTree->FindWidget(Name)) return Error(TEXT("Network HUD missing bound widget: ") + Name.ToString());
	if (!Cast<UProgressBar>(HUD.WidgetTree->FindWidget(TEXT("HealthBar"))))
		return Error(TEXT("Network HealthBar must be a ProgressBar."));
	for (const FName Name : {TEXT("MagazineText"), TEXT("ReserveText"), TEXT("WeaponNameText"), TEXT("MatchHintText")})
		if (!Cast<UTextBlock>(HUD.WidgetTree->FindWidget(Name))) return Error(TEXT("Network text widget has wrong type: ") + Name.ToString());
	if (HUD.WidgetTree->FindWidget(TEXT("CountdownRow"))->GetVisibility() != ESlateVisibility::Collapsed)
		return Error(TEXT("Network HUD Designer must hide the retired countdown row."));
	const auto* Hint = CastChecked<UTextBlock>(HUD.WidgetTree->FindWidget(TEXT("MatchHintText")));
	if (!Hint->GetText().ToString().Contains(TEXT("敌人")) || !Hint->GetText().ToString().Contains(TEXT("互相伤害")))
		return Error(TEXT("Network HUD Designer survival hint is missing."));
	TArray<UEdGraph*> Graphs; HUD.GetAllGraphs(Graphs);
	UK2Node_Event* Entry = nullptr;
	for (UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		if (ReferencesLegacy(*Node)) return Error(TEXT("Network HUD still references a retired gameplay entry."));
		if (auto* Event = Cast<UK2Node_Event>(Node))
		{
			if (Entry || Event->EventReference.GetMemberName() != TEXT("PresentSnapshot"))
				return Error(TEXT("Network HUD must have exactly one presentation event and no old Construct business."));
			Entry = Event;
		}
		if (const auto* Write = Cast<UK2Node_VariableSet>(Node); Write && Write->GetVarName() != TEXT("bPlayedFadeIn"))
			return Error(TEXT("Network HUD can only write its own fade presentation flag."));
	}
	if (!Entry) return Error(TEXT("Network HUD PresentSnapshot event is missing."));
	TSet<UEdGraphNode*> Reachable;
	TArray<UEdGraphNode*> Pending = {Entry};
	while (!Pending.IsEmpty())
	{
		UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
		if (Reachable.Contains(Node)) continue;
		Reachable.Add(Node);
		for (UEdGraphPin* Pin : Node->Pins)
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				for (UEdGraphPin* Linked : Pin->LinkedTo) if (Linked) Pending.Add(Linked->GetOwningNode());
	}
	TMap<FString, int32> Setters;
	int32 FadeCalls = 0;
	for (UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes)
		if (const auto* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const UFunction* Function = Call->GetTargetFunction();
			if (!Function) return Error(TEXT("Network HUD contains an unresolved call."));
			const UClass* Owner = Function->GetOwnerClass();
			if (!Owner->IsChildOf(UWidget::StaticClass()) && Owner != UKismetMathLibrary::StaticClass()
				&& Owner != UKismetTextLibrary::StaticClass())
				return Error(TEXT("Network HUD call is outside presentation libraries: ") + Function->GetPathName());
			if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure)) continue;
			if (!Reachable.Contains(Node)) return Error(TEXT("Network HUD contains an unreachable setter/playback call."));
			const FName FunctionName = Function->GetFName();
			if (FunctionName == TEXT("PlayAnimation"))
			{
				const UEdGraphPin* AnimationPin = Call->FindPin(TEXT("InAnimation"));
				const auto* AnimationRead = AnimationPin && AnimationPin->LinkedTo.Num() == 1
					? Cast<UK2Node_VariableGet>(AnimationPin->LinkedTo[0]->GetOwningNode()) : nullptr;
				if (!AnimationRead || !HUD.Animations.ContainsByPredicate([AnimationRead](const UWidgetAnimation* Animation)
					{ return Animation && Animation->GetFName() == AnimationRead->GetVarName(); }))
					return Error(TEXT("Network HUD fade playback is not wired to its authored animation."));
				++FadeCalls; continue;
			}
			const UEdGraphPin* Self = Call->FindPin(UEdGraphSchema_K2::PN_Self);
			const auto* Target = Self && Self->LinkedTo.Num() == 1 ? Cast<UK2Node_VariableGet>(Self->LinkedTo[0]->GetOwningNode()) : nullptr;
			if (!Target) return Error(TEXT("Network HUD setter target is not a bound-widget property."));
			const FString Key = Target->GetVarName().ToString() + TEXT(".") + FunctionName.ToString();
			if (Key == TEXT("HealthBar.SetPercent") && !ReadsNativeValue(Call->FindPin(TEXT("InPercent")), TEXT("HealthFraction")))
				return Error(TEXT("Network health setter must read the normalized snapshot."));
			if (FunctionName == TEXT("SetText"))
			{
				const UEdGraphPin* Input = Call->FindPin(TEXT("InText"));
				if (Target->GetVarName() == TEXT("WeaponNameText"))
				{
					if (!ReadsNativeValue(Input, TEXT("CurrentWeaponName"))) return Error(TEXT("Network weapon name must read the equipment snapshot."));
				}
				else
				{
					const auto* Conversion = Input && Input->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(Input->LinkedTo[0]->GetOwningNode()) : nullptr;
					const FName Source = Target->GetVarName() == TEXT("MagazineText") ? TEXT("CurrentAmmo") : TEXT("ReserveAmmo");
					if (!Conversion || Conversion->FunctionReference.GetMemberName() != TEXT("Conv_IntToText")
						|| !ReadsNativeValue(Conversion->FindPin(TEXT("Value")), Source))
						return Error(TEXT("Network ammo text must format its current snapshot value."));
				}
			}
			++Setters.FindOrAdd(Key);
		}
	const TMap<FString, int32> Expected = {{TEXT("HealthBar.SetPercent"), 1}, {TEXT("HealthBar.SetFillColorAndOpacity"), 2},
		{TEXT("MagazineText.SetText"), 1}, {TEXT("MagazineText.SetColorAndOpacity"), 2},
		{TEXT("ReserveText.SetText"), 1}, {TEXT("WeaponNameText.SetText"), 1}, {TEXT("AmmoContainer.SetVisibility"), 2}};
	if (Setters.Num() != Expected.Num() || FadeCalls != 1) return Error(TEXT("Network HUD presentation setter/playback set differs."));
	for (const auto& Pair : Expected)
		if (Setters.FindRef(Pair.Key) != Pair.Value) return Error(TEXT("Network HUD reachable setter count differs: ") + Pair.Key);
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HUD GRAPH_PASS Asset=%s Entry=PresentSnapshot ReachableExecNodes=%d SetterKinds=%d FadeCalls=%d Bindings=0 OldGameplayReferences=0"),
		*HUD.GetPathName(), Reachable.Num(), Setters.Num(), FadeCalls);
	return true;
}

void PrepareHUD(const TArray<FString>& Args)
{
	using namespace FpstruePvpAssetPreparation;
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	if (!GEditor || GEditor->PlayWorld || (Args.Num() && !bApply))
	{ Error(TEXT("Usage: fp.PrepareNetworkHUD [Apply], outside PIE.")); return; }
	const FString Target = TEXT("/Game/FirstPerson/UI/ingame_network");
	auto* HUD = Load<UWidgetBlueprint>(Target);
	if (!HUD || HUD->GetOutermost()->GetName() != Target || HUD->GetOutermost()->IsDirty() || !HUD->WidgetTree)
	{ Error(TEXT("Saved network HUD is missing, dirty or outside the package whitelist.")); return; }
	if (!bApply) { ValidateNetworkHUD(*HUD); return; }
	// 只备份、保存联网副本；原 ingame 和其他 UI 包均不修改。
	const FString Backup = FPaths::ProjectSavedDir() / TEXT("Backups/NetworkHUD") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!BackupAsset(FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension()), Backup)) return;
	UWidgetAnimation* Fade = nullptr;
	for (UWidgetAnimation* Animation : HUD->Animations)
		if (Animation && Animation->GetName().StartsWith(TEXT("淡入"))) { Fade = Animation; break; }
	if (!Fade) { Error(TEXT("Network HUD authored fade-in animation is missing; no save.")); return; }
	HUD->Modify(); HUD->WidgetTree->Modify();
	// 网络副本原有的绑定函数和 Construct 链已经由 Native 取代；现在完整改为单一快照表现图。
	TArray<UEdGraph*> OldGraphs; HUD->GetAllGraphs(OldGraphs);
	for (UEdGraph* Graph : OldGraphs) FBlueprintEditorUtils::RemoveGraph(HUD, Graph, EGraphRemoveFlags::None);
	TArray<FName> OldVariables; for (const auto& Variable : HUD->NewVariables) OldVariables.Add(Variable.VarName);
	for (FName Name : OldVariables) FBlueprintEditorUtils::RemoveMemberVariable(HUD, Name);
	HUD->Bindings.Reset();
	HUD->ParentClass = UfpstruePvpIngameWidget::StaticClass();
	const TPair<FName, FName> Names[] = {{TEXT("血量组件"), TEXT("HealthBar")}, {TEXT("当前子弹"), TEXT("MagazineText")},
		{TEXT("剩余子弹"), TEXT("ReserveText")}, {TEXT("TextBlock_2"), TEXT("WeaponNameText")},
		{TEXT("TextBlock_65"), TEXT("MatchHintText")}, {TEXT("子弹数"), TEXT("AmmoContainer")}, {TEXT("HorizontalBox_327"), TEXT("CountdownRow")}};
	for (const auto& Pair : Names)
	{
		UWidget* Widget = HUD->WidgetTree->FindWidget(Pair.Value);
		if (!Widget) Widget = HUD->WidgetTree->FindWidget(Pair.Key);
		if (!Widget) { Error(TEXT("Network HUD source control missing: ") + Pair.Key.ToString()); return; }
		Widget->Modify(); Widget->bIsVariable = true;
		if (Widget->GetFName() != Pair.Value)
		{
			if (!Widget->Rename(*Pair.Value.ToString(), nullptr, REN_DontCreateRedirectors))
			{ Error(TEXT("Network HUD widget rename failed; no save.")); return; }
			for (UWidgetAnimation* Animation : HUD->Animations)
				if (Animation) for (FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
					if (Binding.WidgetName == Pair.Key) { Animation->Modify(); Binding.WidgetName = Pair.Value; }
		}
	}
	CastChecked<UTextBlock>(HUD->WidgetTree->FindWidget(TEXT("MatchHintText")))->SetText(
		NSLOCTEXT("fpstrue", "PvpSurvivalHint", "联机生存：抵御敌人，玩家之间可互相伤害"));
	HUD->WidgetTree->FindWidget(TEXT("CountdownRow"))->SetVisibility(ESlateVisibility::Collapsed);
	const auto CompileHUD = [HUD]()
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(HUD);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(HUD, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HUD COMPILE Asset=%s Errors=%d Warnings=%d SaveOnCompile=0"),
			*HUD->GetPathName(), Results.NumErrors, Results.NumWarnings);
		return Results.NumErrors == 0 && HUD->Status != BS_Error;
	};
	FEdGraphPinType FlagType; FlagType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	if (!FBlueprintEditorUtils::AddMemberVariable(HUD, TEXT("bPlayedFadeIn"), FlagType, TEXT("false")) || !CompileHUD()) return;
	FGraphBuilder B;
	B.Graph = FBlueprintEditorUtils::CreateNewGraph(HUD, TEXT("NetworkPresentation"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(HUD, B.Graph);
	B.Schema = GetDefault<UEdGraphSchema_K2>();
	const auto Call = [&B](UClass* Owner, FName FunctionName)
	{
		auto* Node = B.Node<UK2Node_CallFunction>(TEXT("只消费当前显示快照，不查询或修改 Gameplay 状态。"));
		UFunction* Function = Owner->FindFunctionByName(FunctionName);
		if (!Function) { B.bOK = false; return Node; }
		Node->SetFromFunction(Function); Node->AllocateDefaultPins(); return Node;
	};
	const auto Value = [&B](FName Name)
	{
		auto* Read = B.Node<UK2Node_VariableGet>(TEXT("只读 Native 快照或已绑定的作者控件。"));
		Read->VariableReference.SetSelfMember(Name); Read->AllocateDefaultPins(); return Read->FindPin(Name);
	};
	const auto Setter = [&B, &Call, &Value](UClass* Owner, FName FunctionName, FName Widget)
	{
		auto* Node = Call(Owner, FunctionName); B.Link(Value(Widget), Node->FindPin(UEdGraphSchema_K2::PN_Self)); return Node;
	};
	const auto Default = [&B](UEdGraphNode* Node, FName Name, const FString& Text)
	{
		if (UEdGraphPin* Pin = Node->FindPin(Name)) B.Schema->TrySetDefaultValue(*Pin, Text);
		else B.bOK = false;
	};
	const auto Exec = [&B](UEdGraphPin* Out, UEdGraphNode* In) { B.Link(Out, In->FindPin(UEdGraphSchema_K2::PN_Execute)); };
	const auto Then = [](UEdGraphNode* Node) { return Node->FindPin(UEdGraphSchema_K2::PN_Then); };
	auto* Entry = B.Node<UK2Node_Event>(TEXT("唯一入口：HUD 已提交快照或 Widget 刚完成构建。"));
	Entry->EventReference.SetExternalMember(TEXT("PresentSnapshot"), UfpstruePvpIngameWidget::StaticClass());
	Entry->bOverrideFunction = true; Entry->AllocateDefaultPins();
	auto* Percent = Setter(UProgressBar::StaticClass(), TEXT("SetPercent"), TEXT("HealthBar"));
	B.Link(Value(TEXT("HealthFraction")), Percent->FindPin(TEXT("InPercent"))); Exec(Then(Entry), Percent);
	UEdGraphPin* Tail = Then(Percent);
	for (const auto& Pair : {TPair<FName, FName>(TEXT("CurrentAmmo"), TEXT("MagazineText")), TPair<FName, FName>(TEXT("ReserveAmmo"), TEXT("ReserveText"))})
	{
		auto* Text = Call(UKismetTextLibrary::StaticClass(), TEXT("Conv_IntToText")); B.Link(Value(Pair.Key), Text->FindPin(TEXT("Value")));
		auto* Set = Setter(UTextBlock::StaticClass(), TEXT("SetText"), Pair.Value);
		B.Link(Text->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Set->FindPin(TEXT("InText"))); Exec(Tail, Set); Tail = Then(Set);
	}
	auto* Name = Setter(UTextBlock::StaticClass(), TEXT("SetText"), TEXT("WeaponNameText"));
	B.Link(Value(TEXT("CurrentWeaponName")), Name->FindPin(TEXT("InText"))); Exec(Tail, Name);
	const auto ColorBranch = [&B, &Call, &Value, &Setter, &Default, &Exec, &Then](const TArray<UEdGraphPin*>& Before, bool bHealth)
	{
		auto* Compare = Call(UKismetMathLibrary::StaticClass(), bHealth ? TEXT("LessEqual_DoubleDouble") : TEXT("LessEqual_IntInt"));
		B.Link(Value(bHealth ? TEXT("HealthFraction") : TEXT("CurrentAmmo")), Compare->FindPin(TEXT("A")));
		Default(Compare, TEXT("B"), bHealth ? TEXT("0.3") : TEXT("5"));
		auto* Branch = B.Node<UK2Node_IfThenElse>(bHealth ? TEXT("作者低血阈值与颜色。") : TEXT("作者低弹阈值与颜色。")); Branch->AllocateDefaultPins();
		B.Link(Compare->FindPin(UEdGraphSchema_K2::PN_ReturnValue), Branch->GetConditionPin());
		for (UEdGraphPin* Output : Before) Exec(Output, Branch);
		TArray<UEdGraphPin*> Outputs;
		for (int32 Low = 1; Low >= 0; --Low)
		{
			auto* Set = Setter(bHealth ? UProgressBar::StaticClass() : UTextBlock::StaticClass(),
				bHealth ? TEXT("SetFillColorAndOpacity") : TEXT("SetColorAndOpacity"), bHealth ? TEXT("HealthBar") : TEXT("MagazineText"));
			const FLinearColor Color = bHealth ? (Low ? FLinearColor(0.85f,0.1f,0.08f,0.85f) : FLinearColor(0.104616f,0.296138f,0.158961f,0.701961f))
				: (Low ? FLinearColor(0.9f,0.15f,0.1f) : FLinearColor(0.651406f,0.617207f,0.473532f));
			FString Text;
			if (bHealth) TBaseStructure<FLinearColor>::Get()->ExportText(Text, &Color, nullptr, nullptr, PPF_None, nullptr);
			else { const FSlateColor Slate(Color); FSlateColor::StaticStruct()->ExportText(Text, &Slate, nullptr, nullptr, PPF_None, nullptr); }
			Default(Set, bHealth ? TEXT("InColor") : TEXT("InColorAndOpacity"), Text);
			Exec(Branch->FindPin(Low ? UEdGraphSchema_K2::PN_Then : UEdGraphSchema_K2::PN_Else), Set); Outputs.Add(Then(Set));
		}
		return Outputs;
	};
	const TArray<UEdGraphPin*> HealthOutputs = ColorBranch({Then(Name)}, true);
	// 分支汇合只改变显示；两个互斥颜色出口均通向同一个后续入口。
	auto* AmmoGate = B.Node<UK2Node_IfThenElse>(TEXT("弹药区仅在存在装备时显示。")); AmmoGate->AllocateDefaultPins();
	B.Link(Value(TEXT("bHasEquipment")), AmmoGate->GetConditionPin());
	const TArray<UEdGraphPin*> AmmoColorOutputs = ColorBranch(HealthOutputs, false);
	for (UEdGraphPin* Output : AmmoColorOutputs) Exec(Output, AmmoGate);
	TArray<UEdGraphPin*> VisibleOutputs;
	for (int32 Equipped = 1; Equipped >= 0; --Equipped)
	{
		auto* Set = Setter(UWidget::StaticClass(), TEXT("SetVisibility"), TEXT("AmmoContainer"));
		Default(Set, TEXT("InVisibility"), Equipped ? TEXT("SelfHitTestInvisible") : TEXT("Collapsed"));
		Exec(AmmoGate->FindPin(Equipped ? UEdGraphSchema_K2::PN_Then : UEdGraphSchema_K2::PN_Else), Set); VisibleOutputs.Add(Then(Set));
	}
	auto* FadeGate = B.Node<UK2Node_IfThenElse>(TEXT("作者淡入只在此 Widget 实例首次表现时播放。")); FadeGate->AllocateDefaultPins();
	B.Link(Value(TEXT("bPlayedFadeIn")), FadeGate->GetConditionPin());
	for (UEdGraphPin* Output : VisibleOutputs) Exec(Output, FadeGate);
	auto* Played = B.Node<UK2Node_VariableSet>(TEXT("只保存本 Widget 的一次淡入标记，不是 Gameplay 状态。"));
	Played->VariableReference.SetSelfMember(TEXT("bPlayedFadeIn")); Played->AllocateDefaultPins(); Default(Played, TEXT("bPlayedFadeIn"), TEXT("true"));
	Exec(FadeGate->GetElsePin(), Played);
	auto* Play = Call(UUserWidget::StaticClass(), TEXT("PlayAnimation"));
	B.Link(Value(Fade->GetFName()), Play->FindPin(TEXT("InAnimation"))); Exec(Then(Played), Play);
	if (!B.bOK || !CompileHUD() || !ValidateNetworkHUD(*HUD)) { Error(TEXT("Network HUD graph failed; no save. Backup=") + Backup); return; }
	HUD->MarkPackageDirty();
	if (!UEditorLoadingAndSavingUtils::SavePackages({HUD->GetOutermost()}, false)) { Error(TEXT("Network HUD save failed. Backup=") + Backup); return; }
	UE_LOG(LogTemp, Display, TEXT("TEMPLATE_HUD PASS Copy=%s PackagesSaved=1 BlueprintPresentation=1 Bindings=0 LayoutRetained=1 Backup=%s"), *Target, *Backup);
}
FAutoConsoleCommand HUDCommand(TEXT("fp.PrepareNetworkHUD"), TEXT("Check/Apply the single network HUD Blueprint presentation graph; only ingame_network is saved."), FConsoleCommandWithArgsDelegate::CreateStatic(&PrepareHUD));

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
				const double MovementDuration = PairActionPhase == 7 ? 0.9 : 0.5;
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

#include "Testing/Editor/fpstruePlayerPresentationMigration.inl"

#include "Testing/Editor/fpstrueWeaponPresentationMigration.inl"

#endif // WITH_EDITOR
