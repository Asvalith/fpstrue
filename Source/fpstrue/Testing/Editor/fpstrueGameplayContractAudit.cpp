// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/IConsoleManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace FpstrueGameplayContractAudit
{
// 关键词只是人工复核线索，不能证明节点可达、只执行一次或具有正确的网络权限。
FString ReviewTags(const FString& Text)
{
	FString Tags;
	if (Text.Contains(TEXT("fpstrueGameMode")) || Text.Contains(TEXT("BP_FPGameMode")) || Text.Contains(TEXT("BP_FirstPersonGameMode")))
		Tags += TEXT("LegacyGameMode;");
	if (Text.Contains(TEXT("Reload")) || Text.Contains(TEXT("Ammo"))) Tags += TEXT("ReloadOrAmmo;");
	if (Text.Contains(TEXT("Damage")) || Text.Contains(TEXT("Health"))) Tags += TEXT("DamageOrHealth;");
	return Tags;
}

void AuditBlueprint(UBlueprint& Blueprint, int32& CompileFailures)
{
	UPackage* Package = Blueprint.GetOutermost();
	UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT BLUEPRINT Asset=%s Parent=%s DirtyBefore=%d ParentReview=%s"),
		*Blueprint.GetPathName(), *GetPathNameSafe(Blueprint.ParentClass), Package->IsDirty(), *ReviewTags(GetPathNameSafe(Blueprint.ParentClass)));
	if (const UWidgetBlueprint* Widget = Cast<UWidgetBlueprint>(&Blueprint))
	{
		UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT WIDGET_BINDINGS Asset=%s Count=%d"),
			*Blueprint.GetPathName(), Widget->Bindings.Num());
		for (const FDelegateEditorBinding& Binding : Widget->Bindings)
			UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT WIDGET_BINDING Asset=%s Object=\"%s\" Property=\"%s\" Function=\"%s\""),
				*Blueprint.GetPathName(), *Binding.ObjectName, *Binding.PropertyName.ToString(), *Binding.FunctionName.ToString());
	}
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph == nullptr) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr) continue;
			UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT NODE Asset=%s Graph=%s Node=%s Class=%s Title=\"%s\""),
				*Blueprint.GetPathName(), *Graph->GetName(), *Node->NodeGuid.ToString(), *Node->GetClass()->GetPathName(),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			FString Reference = Node->GetClass()->GetPathName() + TEXT(" ") + Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				const UFunction* Function = Call->GetTargetFunction();
				Reference += TEXT(" ") + (Function != nullptr ? Function->GetPathName() : Call->FunctionReference.GetMemberName().ToString());
				if (Function == nullptr || Function->HasAnyFunctionFlags(FUNC_Native))
					UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT CALL Asset=%s Graph=%s Node=%s Function=%s Native=%d Resolved=%d"),
						*Blueprint.GetPathName(), *Graph->GetName(), *Node->NodeGuid.ToString(),
						*(Function != nullptr ? Function->GetPathName() : Call->FunctionReference.GetMemberName().ToString()),
						Function != nullptr && Function->HasAnyFunctionFlags(FUNC_Native), Function != nullptr);
			}
			if (const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
			{
				const FProperty* Property = Set->GetPropertyForVariable();
				const UClass* Owner = Property != nullptr ? Property->GetOwnerClass() : nullptr;
				Reference += TEXT(" ") + (Property != nullptr ? Property->GetPathName() : Set->GetVarName().ToString());
				UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT VARIABLE_SET Asset=%s Graph=%s Node=%s Property=%s NativeOwner=%d"),
					*Blueprint.GetPathName(), *Graph->GetName(), *Node->NodeGuid.ToString(),
					*(Property != nullptr ? Property->GetPathName() : Set->GetVarName().ToString()), Owner != nullptr && Owner->HasAnyClassFlags(CLASS_Native));
			}
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin == nullptr) continue;
				Reference += FString::Printf(TEXT(" %s %s %s %s %s"), *Pin->PinName.ToString(), *Pin->DefaultValue,
					*GetPathNameSafe(Pin->DefaultObject), *GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get()),
					*GetPathNameSafe(Pin->PinType.PinValueType.TerminalSubCategoryObject.Get()));
				// 同时记录零连接 pin，事件节点存在不能代替其执行出口已经接通的证据。
				UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT PIN Asset=%s Graph=%s Node=%s Pin=\"%s\" Direction=%s Exec=%d Category=%s Links=%d Default=\"%s\" DefaultObject=%s"),
					*Blueprint.GetPathName(), *Graph->GetName(), *Node->NodeGuid.ToString(), *Pin->PinName.ToString(),
					Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec,
					*Pin->PinType.PinCategory.ToString(), Pin->LinkedTo.Num(), *Pin->DefaultValue, *GetPathNameSafe(Pin->DefaultObject));
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin == nullptr) continue;
					const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT LINK Asset=%s Graph=%s Node=%s Pin=\"%s\" Direction=%s Exec=%d ToGraph=%s ToNode=%s ToPin=\"%s\" ToDirection=%s ToExec=%d"),
						*Blueprint.GetPathName(), *Graph->GetName(), *Node->NodeGuid.ToString(), *Pin->PinName.ToString(),
						Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec,
						LinkedNode != nullptr ? *GetNameSafe(LinkedNode->GetGraph()) : TEXT("-"),
						LinkedNode != nullptr ? *LinkedNode->NodeGuid.ToString() : TEXT("-"), *LinkedPin->PinName.ToString(),
						LinkedPin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"), LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				}
			}
			const FString Tags = ReviewTags(Reference);
			if (!Tags.IsEmpty())
				UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT REVIEW Asset=%s Graph=%s Node=%s Tags=%s Reference=%s"),
					*Blueprint.GetPathName(), *Graph->GetName(), *Node->NodeGuid.ToString(), *Tags, *Reference);
		}
	}
	FCompilerResultsLog Results;
	Results.bSilentMode = true;
	Results.bAnnotateMentionedNodes = false;
	// 正常编译会重建内存中的生成类；禁止 Save-on-Compile，不改图、不保存、不掩盖原有 dirty 标志。
	FKismetEditorUtilities::CompileBlueprint(&Blueprint,
		EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	const bool bFailed = Results.NumErrors != 0 || Blueprint.Status == BS_Error;
	CompileFailures += bFailed ? 1 : 0;
	UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT COMPILE Asset=%s Parent=%s Status=%d Errors=%d Warnings=%d DirtyAfter=%d"),
		*Blueprint.GetPathName(), *GetPathNameSafe(Blueprint.ParentClass), static_cast<int32>(Blueprint.Status), Results.NumErrors, Results.NumWarnings, Package->IsDirty());
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		if (Message->GetSeverity() == EMessageSeverity::Error || Message->GetSeverity() == EMessageSeverity::Warning)
			UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT DIAGNOSTIC Asset=%s Severity=%d Message=%s"),
				*Blueprint.GetPathName(), static_cast<int32>(Message->GetSeverity()), *Message->ToText().ToString());
}

void AuditBehaviorNode(const UBTNode* Node, const FString& Branch, TSet<const UBTNode*>& Seen)
{
	if (Node == nullptr || Seen.Contains(Node)) return;
	Seen.Add(Node);
	const UBTDecorator_Blackboard* Condition = Cast<UBTDecorator_Blackboard>(Node);
	UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT BT_NODE Branch=%s Node=%s Class=%s Native=%d BlackboardKey=%s"),
		*Branch, *Node->GetPathName(), *Node->GetClass()->GetPathName(), Node->GetClass()->HasAnyClassFlags(CLASS_Native),
		Condition != nullptr ? *Condition->GetSelectedBlackboardKey().ToString() : TEXT("-"));
	if (const UBTTaskNode* Task = Cast<UBTTaskNode>(Node))
		for (const UBTService* Service : Task->Services) AuditBehaviorNode(Service, Branch + TEXT("/Service"), Seen);
	if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
	{
		for (const UBTService* Service : Composite->Services) AuditBehaviorNode(Service, Branch + TEXT("/Service"), Seen);
		for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
		{
			const FString ChildBranch = Branch + FString::Printf(TEXT("/%d"), Index);
			for (const UBTDecorator* Decorator : Composite->Children[Index].Decorators) AuditBehaviorNode(Decorator, ChildBranch + TEXT("/Decorator"), Seen);
			AuditBehaviorNode(Composite->GetChildNode(Index), ChildBranch, Seen);
		}
	}
}

void Run()
{
	if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("FP_CONTRACT_AUDIT REFUSED: stop PIE before compiling project blueprints."));
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT BEGIN Scope=/Game/FirstPerson,/Game/Config Save=Disabled. Compilation changes generated objects in memory; no source edits or package saves. Keyword hits are review clues, not semantic proof. Redirected references and loaded unsaved graphs may differ from disk."));
	FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Module.Get().ScanPathsSynchronous({TEXT("/Game/FirstPerson"), TEXT("/Game/Config")});
	FARFilter Filter;
	Filter.PackagePaths = {TEXT("/Game/FirstPerson"), TEXT("/Game/Config")};
	Filter.ClassPaths = {UBlueprint::StaticClass()->GetClassPathName(), UBehaviorTree::StaticClass()->GetClassPathName(), UBlackboardData::StaticClass()->GetClassPathName()};
	Filter.bRecursivePaths = Filter.bRecursiveClasses = true;
	TArray<FAssetData> Assets;
	Module.Get().GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	int32 Blueprints = 0, Trees = 0, Blackboards = 0, CompileFailures = 0, LoadFailures = 0;
	for (const FAssetData& Asset : Assets)
	{
		TStrongObjectPtr<UObject> Object(Asset.GetAsset());
		if (!Object.IsValid())
		{
			++LoadFailures;
			UE_LOG(LogTemp, Error, TEXT("FP_CONTRACT_AUDIT LOAD_FAILED Package=%s"), *Asset.PackageName.ToString());
			continue;
		}
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Object.Get())) { ++Blueprints; AuditBlueprint(*Blueprint, CompileFailures); }
		else if (const UBehaviorTree* Tree = Cast<UBehaviorTree>(Object.Get()))
		{
			++Trees;
			UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT BT Asset=%s Blackboard=%s"), *Tree->GetPathName(), *GetPathNameSafe(Tree->BlackboardAsset));
			TSet<const UBTNode*> Seen;
			for (const UBTDecorator* Decorator : Tree->RootDecorators) AuditBehaviorNode(Decorator, TEXT("RootDecorator"), Seen);
			AuditBehaviorNode(Tree->RootNode, TEXT("Root"), Seen);
		}
		else if (const UBlackboardData* Blackboard = Cast<UBlackboardData>(Object.Get()))
		{
			++Blackboards;
			UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT BB Asset=%s Parent=%s Keys=%d"), *Blackboard->GetPathName(), *GetPathNameSafe(Blackboard->Parent), Blackboard->GetNumKeys());
			for (int32 Index = 0; Index < Blackboard->GetNumKeys(); ++Index)
			{
				const FBlackboard::FKey Key = static_cast<FBlackboard::FKey>(Index);
				UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT BB_KEY Asset=%s Id=%d Name=%s Type=%s InstanceSynced=%d"),
					*Blackboard->GetPathName(), Index, *Blackboard->GetKeyName(Key).ToString(), *GetPathNameSafe(Blackboard->GetKeyType(Key).Get()), Blackboard->IsKeyInstanceSynced(Key));
			}
		}
	}
	UE_LOG(LogTemp, Display, TEXT("FP_CONTRACT_AUDIT END Blueprints=%d Trees=%d Blackboards=%d CompileFailures=%d LoadFailures=%d SavedPackages=0 StaticSemanticsVerified=0"),
		Blueprints, Trees, Blackboards, CompileFailures, LoadFailures);
}

FAutoConsoleCommand Command(TEXT("fp.AuditGameplayContracts"),
	TEXT("Load/compile project blueprints with SkipSave and report native graph calls, rule-entry clues and BT/BB structure; does not edit graphs or save assets."),
	FConsoleCommandDelegate::CreateStatic(&Run));
}

#endif
