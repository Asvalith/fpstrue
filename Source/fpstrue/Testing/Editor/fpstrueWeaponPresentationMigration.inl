// Editor-only migration: authored weapon assets own appearance; native types own rules and component topology.
#include "Weapons/fpstrueWeaponActors.h"
#include "Weapons/fpstruePickUpComponent.h"
#include "Weapons/fpstrueWeaponComponent.h"
#include "ActorFactories/ActorFactoryBlueprint.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetData.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "ActorFactories/ActorFactoryPlayerStart.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "ObjectTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectHash.h"
#include "UObject/StrongObjectPtr.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace FpstrueWeaponPresentationMigration
{
constexpr const TCHAR* RARPackage = TEXT("/Game/FirstPerson/Blueprints/Weapons/BP_RARWeapon");
constexpr const TCHAR* TemplatePackage = TEXT("/Game/FirstPerson/Blueprints/Weapons/BP_TemplateWeapon");

bool Error(const FString& Reason)
{
	UE_LOG(LogTemp, Error, TEXT("WEAPON_PRESENTATION FAIL %s"), *Reason);
	return false;
}

template<class T> T* Asset(const TCHAR* Package)
{
	T* Value = LoadObject<T>(nullptr, Package);
	if (!Value) Error(FString(TEXT("Missing authored source ")) + Package);
	return Value;
}

bool SameAsset(const UObject* Value, const TCHAR* Package)
{
	return Value && Value->GetOutermost()->GetName() == Package;
}

bool Backup(const FString& Package, bool bMap, const FString& Directory, bool bMayBeNew = false)
{
	const FString Filename = FPackageName::LongPackageNameToFilename(Package,
		bMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
	if (!IFileManager::Get().FileExists(*Filename)) return bMayBeNew || Error(TEXT("Backup source missing: ") + Filename);
	const FString Destination = Directory / Package.RightChop(6) + (bMap ? TEXT(".umap") : TEXT(".uasset"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
	return IFileManager::Get().Copy(*Destination, *Filename, false, false) == COPY_OK
		|| Error(TEXT("Non-overwriting backup failed: ") + Destination);
}

bool Compile(UBlueprint* Blueprint)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	return (Results.NumErrors == 0 && Blueprint->Status != BS_Error)
		|| Error(TEXT("Blueprint compile failed: ") + Blueprint->GetPathName());
}

UBlueprint* Blueprint(const TCHAR* Package, UClass* Parent, bool bApply)
{
	UBlueprint* Value = LoadObject<UBlueprint>(nullptr, Package, nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (!Value && bApply)
	{
		Value = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(Package),
			FName(*FPackageName::GetShortName(Package)), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Value)
		{
			FAssetRegistryModule::AssetCreated(Value);
			// UE adds disabled default Actor events after creating a BP. Only remove that exact
			// engine-owned scaffold in this fresh-creation branch; never erase an existing graph.
			for (UEdGraph* Graph : Value->UbergraphPages)
			{
				if (Graph->GetFName() != UEdGraphSchema_K2::GN_EventGraph) continue;
				TArray<UEdGraphNode*> Ghosts;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					FName Member;
					const UFunction* Function = nullptr;
					if (const auto* Event = Cast<UK2Node_Event>(Node))
					{
						Member = Event->EventReference.GetMemberName();
						Function = Event->EventReference.ResolveMember<UFunction>(Value->GeneratedClass.Get());
					}
					else if (const auto* ParentCall = Cast<UK2Node_CallParentFunction>(Node))
					{
						Member = ParentCall->FunctionReference.GetMemberName();
						Function = ParentCall->GetTargetFunction();
					}
					const bool bDefaultActorEvent = Member == TEXT("ReceiveBeginPlay") || Member == TEXT("ReceiveActorBeginOverlap") || Member == TEXT("ReceiveTick");
					if (!Node->IsAutomaticallyPlacedGhostNode() || !bDefaultActorEvent || !Function || Function->GetOuterUClass() != AActor::StaticClass())
					{
						Error(FString::Printf(TEXT("Unexpected fresh BP node Asset=%s Graph=%s Node=%s Class=%s Enabled=%d Ghost=%d Member=%s; not overwritten."),
							*Value->GetPathName(), *Graph->GetName(), *Node->GetName(), *Node->GetClass()->GetPathName(),
							static_cast<int32>(Node->GetDesiredEnabledState()), Node->IsAutomaticallyPlacedGhostNode(), *Member.ToString()));
						return nullptr;
					}
					Ghosts.Add(Node);
				}
				for (const UEdGraphNode* Node : Ghosts)
					for (const UEdGraphPin* Pin : Node->Pins)
						for (const UEdGraphPin* Linked : Pin->LinkedTo)
							if (!Linked || !Ghosts.Contains(Linked->GetOwningNode()))
							{
								Error(TEXT("Default event has a connection outside the engine scaffold; not overwritten.")); return nullptr;
							}
				for (UEdGraphNode* Node : Ghosts) FBlueprintEditorUtils::RemoveNode(Value, Node, true);
				UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION NEW_GHOSTS_REMOVED Asset=%s Count=%d"), *Value->GetPathName(), Ghosts.Num());
			}
		}
	}
	if (!Value || Value->ParentClass != Parent || !Value->GeneratedClass)
	{
		Error(FString(TEXT("Missing/mismatched data-only weapon Blueprint: ")) + Package);
		return nullptr;
	}
	TArray<UEdGraph*> Graphs;
	Value->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			// Native-parent data Blueprints still require the engine-created empty construction entry.
			if (Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript && Graph->Nodes.Num() == 1 && Node->IsA<UK2Node_FunctionEntry>())
			{
				bool bHasConnections = false;
				for (const UEdGraphPin* Pin : Node->Pins) bHasConnections |= Pin && !Pin->LinkedTo.IsEmpty();
				if (!bHasConnections) continue;
			}
			Error(FString::Printf(TEXT("Weapon BP contains unexpected graph Asset=%s Graph=%s Node=%s Class=%s Enabled=%d Ghost=%d; not overwritten."),
				*Value->GetPathName(), *Graph->GetName(), *Node->GetName(), *Node->GetClass()->GetPathName(),
				static_cast<int32>(Node->GetDesiredEnabledState()), Node->IsAutomaticallyPlacedGhostNode())); return nullptr;
		}
	return Value;
}

UfpstrueWeaponComponent* Weapon(AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<UfpstrueWeaponComponent>() : nullptr;
}

UStaticMeshComponent* Part(AActor* Actor, FName Name)
{
	return Actor ? Cast<UStaticMeshComponent>(Actor->GetDefaultSubobjectByName(Name)) : nullptr;
}

const FObjectPropertyBase* ConfigurationField()
{
	return FindFProperty<FObjectPropertyBase>(UfpstrueWeaponComponent::StaticClass(), TEXT("WeaponConfiguration"));
}

bool Configure(UBlueprint& Blueprint, bool bRAR)
{
	AActor* Defaults = Cast<AActor>(Blueprint.GeneratedClass->GetDefaultObject());
	UfpstrueWeaponComponent* Gun = Weapon(Defaults);
	const FObjectPropertyBase* Config = ConfigurationField();
	if (!Gun || !Config) return Error(TEXT("Native weapon topology/configuration field missing."));
	Blueprint.Modify(); Defaults->Modify(); Gun->Modify();
	const TCHAR* Root = bRAR ? TEXT("/Game/FirstPerson/Animations/RAR/") : TEXT("/Game/FirstPerson/Animations/FPSMPT/");
	const FString Prefix = FString(Root) + (bRAR ? TEXT("AM_RARWeapon_") : TEXT("AM_TemplateWeapon_"));
	Gun->SetSkeletalMesh(Asset<USkeletalMesh>(bRAR
		? TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SK_RAR_AssaultRifle")
		: TEXT("/Game/FPSMPT55/Weapons/AssaultRifle/SKM_AssaultRifle")));
	UAnimBlueprint* Animation = Asset<UAnimBlueprint>(*(FString(Root) + (bRAR ? TEXT("ABP_RARWeapon") : TEXT("ABP_TemplateWeapon"))));
	if (!Animation || !Animation->GeneratedClass) return false;
	Gun->SetAnimInstanceClass(Animation->GeneratedClass.Get());
	Gun->WeaponFireMontage = Asset<UAnimMontage>(*(Prefix + TEXT("Fire")));
	Gun->WeaponReloadMontage = Asset<UAnimMontage>(*(Prefix + TEXT("Reload")));
	Gun->WeaponEmptyReloadMontage = Asset<UAnimMontage>(*(Prefix + TEXT("ReloadEmpty")));
	UfpstrueWeaponConfig* Settings = Asset<UfpstrueWeaponConfig>(bRAR ? TEXT("/Game/Config/DA_FPWeapon_RAR") : TEXT("/Game/Config/DA_FPWeapon_Template"));
	if (!Settings) return false;
	FString SettingsError;
	if (!Settings->Settings.Validate(SettingsError)) return Error(TEXT("Existing DA is invalid: ") + SettingsError);
	Config->SetObjectPropertyValue_InContainer(Gun, Settings);
	Gun->WeaponDisplayName = bRAR ? NSLOCTEXT("fpstrue", "RARWeaponName", "RAR 突击步枪") : NSLOCTEXT("fpstrue", "TemplateWeaponName", "突击步枪");
	Gun->GripLocationOffset = bRAR ? FVector::ZeroVector : FVector(-6.236122, 3.008201, 1.059424);
	Gun->GripRotationOffset = bRAR ? FRotator::ZeroRotator : FRotator(3.4989, 105.08198, 4.0575);
	Gun->MuzzleSource = Gun;
	Gun->MuzzleSocketName = bRAR ? TEXT("SOCKET_Muzzle") : TEXT("muzzleFlash");
	Gun->MuzzleForwardAxis = bRAR ? FVector::RightVector : FVector::ForwardVector;
	Gun->FireSound = Asset<USoundBase>(TEXT("/Game/Sounds/Weapon/AK74/WAV_AK74M_Fire_Cue"));
	Gun->MuzzleEffect = Asset<UParticleSystem>(TEXT("/Game/FPS_Assault_Pack/Particles/Emitters/AR74/P_RU74_Muzzleflash"));
	Gun->ImpactEffect = Asset<UParticleSystem>(TEXT("/Game/Particles/bulletvfx/Particles/VFX_Impact_Concrete"));
	Gun->FleshImpactEffect = Asset<UParticleSystem>(TEXT("/Game/Particles/bulletvfx/Particles/VFX_Impact_Flesh"));
	Gun->ImpactSound = Asset<USoundBase>(TEXT("/Game/Sounds/BulletImpact/Concrete/bullet_impact_concrete_brick_01_Cue"));
	Gun->ImpactDecal = Asset<UMaterialInterface>(TEXT("/Game/Particles/Bullet_Impact_Decal"));
	Gun->ImpactDecalSize = FVector(5.0f); Gun->ImpactDecalLifeSeconds = 10.0f;
	if (bRAR)
	{
		struct FPart { const TCHAR* Name; const TCHAR* Mesh; const TCHAR* Parent; const TCHAR* Socket; };
		const FPart Parts[] = {
			{TEXT("Forestock"), TEXT("SM_RAR_AssaultRifle_Forestock_Default"), TEXT("Weapon"), TEXT("SOCKET_Forestock")},
			{TEXT("Magazine"), TEXT("SM_RAR_AssaultRifle_Magazine_Default_Full"), TEXT("Weapon"), TEXT("SOCKET_Magazine")},
			{TEXT("FrontSight"), TEXT("SM_RAR_AssaultRifle_IronSight_F"), TEXT("Forestock"), TEXT("SOCKET_Ironsight_F")},
			{TEXT("RearSight"), TEXT("SM_RAR_AssaultRifle_Ironsight_B"), TEXT("Weapon"), TEXT("SOCKET_Default")}};
		for (const FPart& Item : Parts)
		{
			UStaticMeshComponent* Component = Part(Defaults, Item.Name);
			USceneComponent* Parent = FCString::Strcmp(Item.Parent, TEXT("Weapon")) == 0 ? static_cast<USceneComponent*>(Gun) : Part(Defaults, Item.Parent);
			if (!Component || !Parent) return Error(TEXT("RAR attachment topology missing."));
			Component->Modify();
			Component->SetStaticMesh(Asset<UStaticMesh>(*(FString(TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/")) + Item.Mesh)));
			Component->SetupAttachment(Parent, Item.Socket);
			Component->SetRelativeTransform(FTransform::Identity);
		}
		Gun->MuzzleSource = Part(Defaults, TEXT("Forestock"));
		const FNameProperty* Parameter = FindFProperty<FNameProperty>(Defaults->GetClass(), TEXT("ViewmodelFOVParameterName"));
		if (!Parameter) return Error(TEXT("RAR material parameter configuration missing."));
		Parameter->SetPropertyValue_InContainer(Defaults, FName(TEXT("Field Of View Alpha")));
	}
	return Compile(&Blueprint);
}

bool Validate(UBlueprint& Blueprint, bool bRAR)
{
	const AActor* Defaults = Cast<AActor>(Blueprint.GeneratedClass->GetDefaultObject());
	// UE5.5 GetAnimClass is not const-qualified; validation still only reads component state.
	UfpstrueWeaponComponent* Gun = Weapon(const_cast<AActor*>(Defaults));
	const FObjectPropertyBase* Config = ConfigurationField();
	if (!Gun || !Config || !Gun->MuzzleSource || Gun->MuzzleSource->GetOwner() != Defaults)
		return Error(TEXT("Authored weapon/default subobject reference is invalid: ") + Blueprint.GetPathName());
	const FString AnimationRoot = bRAR ? TEXT("/Game/FirstPerson/Animations/RAR/") : TEXT("/Game/FirstPerson/Animations/FPSMPT/");
	const FString Prefix = AnimationRoot + (bRAR ? TEXT("AM_RARWeapon_") : TEXT("AM_TemplateWeapon_"));
	const FString AnimPackage = AnimationRoot + (bRAR ? TEXT("ABP_RARWeapon") : TEXT("ABP_TemplateWeapon"));
	const bool bResources = SameAsset(Gun->GetSkeletalMeshAsset(), bRAR
		? TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/SK_RAR_AssaultRifle")
		: TEXT("/Game/FPSMPT55/Weapons/AssaultRifle/SKM_AssaultRifle"))
		&& Gun->GetAnimClass() && Gun->GetAnimClass()->GetOutermost()->GetName() == AnimPackage
		&& SameAsset(Gun->WeaponFireMontage, *(Prefix + TEXT("Fire")))
		&& SameAsset(Gun->WeaponReloadMontage, *(Prefix + TEXT("Reload")))
		&& SameAsset(Gun->WeaponEmptyReloadMontage, *(Prefix + TEXT("ReloadEmpty")))
		&& SameAsset(Config->GetObjectPropertyValue_InContainer(Gun), bRAR ? TEXT("/Game/Config/DA_FPWeapon_RAR") : TEXT("/Game/Config/DA_FPWeapon_Template"))
		&& SameAsset(Gun->FireSound, TEXT("/Game/Sounds/Weapon/AK74/WAV_AK74M_Fire_Cue"))
		&& SameAsset(Gun->MuzzleEffect, TEXT("/Game/FPS_Assault_Pack/Particles/Emitters/AR74/P_RU74_Muzzleflash"))
		&& SameAsset(Gun->ImpactEffect, TEXT("/Game/Particles/bulletvfx/Particles/VFX_Impact_Concrete"))
		&& SameAsset(Gun->FleshImpactEffect, TEXT("/Game/Particles/bulletvfx/Particles/VFX_Impact_Flesh"))
		&& SameAsset(Gun->ImpactSound, TEXT("/Game/Sounds/BulletImpact/Concrete/bullet_impact_concrete_brick_01_Cue"))
		&& SameAsset(Gun->ImpactDecal, TEXT("/Game/Particles/Bullet_Impact_Decal"));
	if (!bResources) return Error(TEXT("Authored weapon resources do not match the migration source: ") + Blueprint.GetPathName());
	if (!Gun->GripLocationOffset.Equals(bRAR ? FVector::ZeroVector : FVector(-6.236122, 3.008201, 1.059424), .001)
		|| !Gun->GripRotationOffset.Equals(bRAR ? FRotator::ZeroRotator : FRotator(3.4989, 105.08198, 4.0575), .01)
		|| Gun->WeaponDisplayName.IsEmpty() || Gun->MuzzleSocketName != (bRAR ? TEXT("SOCKET_Muzzle") : TEXT("muzzleFlash"))
		|| !Gun->MuzzleSource->DoesSocketExist(Gun->MuzzleSocketName) || !Gun->ImpactDecalSize.Equals(FVector(5.0f)) || !FMath::IsNearlyEqual(Gun->ImpactDecalLifeSeconds, 10.0f)
		|| !Gun->MuzzleForwardAxis.Equals(bRAR ? FVector::RightVector : FVector::ForwardVector))
		return Error(TEXT("Authored grip/muzzle settings do not match: ") + Blueprint.GetPathName());
	if (bRAR)
	{
		const TCHAR* Names[] = {TEXT("Forestock"), TEXT("Magazine"), TEXT("FrontSight"), TEXT("RearSight")};
		const TCHAR* Meshes[] = {TEXT("SM_RAR_AssaultRifle_Forestock_Default"), TEXT("SM_RAR_AssaultRifle_Magazine_Default_Full"), TEXT("SM_RAR_AssaultRifle_IronSight_F"), TEXT("SM_RAR_AssaultRifle_Ironsight_B")};
		const TCHAR* Sockets[] = {TEXT("SOCKET_Forestock"), TEXT("SOCKET_Magazine"), TEXT("SOCKET_Ironsight_F"), TEXT("SOCKET_Default")};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			UStaticMeshComponent* Component = Part(const_cast<AActor*>(Defaults), Names[Index]);
			const USceneComponent* Parent = Index == 2 ? static_cast<const USceneComponent*>(Part(const_cast<AActor*>(Defaults), TEXT("Forestock"))) : Gun;
			if (!Component || !SameAsset(Component->GetStaticMesh(), *(FString(TEXT("/Game/InfimaGames/RealisticAssaultRifle/Art/Weapon/Meshes/")) + Meshes[Index]))
				|| Component->GetAttachParent() != Parent || Component->GetAttachSocketName() != Sockets[Index]
				|| !Component->GetRelativeTransform().Equals(FTransform::Identity)) return Error(TEXT("RAR authored attachment mismatch."));
		}
		const FNameProperty* Parameter = FindFProperty<FNameProperty>(Defaults->GetClass(), TEXT("ViewmodelFOVParameterName"));
		if (Gun->MuzzleSource != Part(const_cast<AActor*>(Defaults), TEXT("Forestock")) || !Parameter
			|| Parameter->GetPropertyValue_InContainer(Defaults) != TEXT("Field Of View Alpha")) return Error(TEXT("RAR muzzle/material configuration mismatch."));
	}
	else if (Gun->MuzzleSource != Gun || Gun->MuzzleSocketName != TEXT("muzzleFlash")) return Error(TEXT("Template muzzle configuration mismatch."));
	UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION ASSET_PASS Class=%s Parent=%s DataOnly=1 Resources=1 Attachments=1 Config=1"),
		*Blueprint.GeneratedClass->GetPathName(), *Blueprint.ParentClass->GetPathName());
	return true;
}

bool LegacyPickup(const AActor* Actor)
{
	if (!IsValid(Actor) || Actor->GetOwner()) return false;
	const UClass* Type = Actor->GetClass();
	return Type == AfpstrueRARWeapon::StaticClass() || Type == AfpstrueTemplateWeapon::StaticClass()
		|| Type->GetPathName() == TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon.BP_Weapon_C");
}


// Four production worlds plus the historical CoffeeShop material scene. It must not
// retain an independent single-player Pawn/HUD pipeline just to keep source assets alive.
constexpr const TCHAR* HistoricalMap = TEXT("/Game/Scene/CoffeeShopA/Maps/CoffeeShopA/CoffeeShopA_P");
constexpr const TCHAR* OldCharacterClass = TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter.BP_FirstPersonCharacter_C");
TArray<const TCHAR*> WeaponMapPackages()
{
	TArray<const TCHAR*> Packages;
	for (const TCHAR* Package : FpstruePvpAssetPreparation::MapPackages) Packages.Add(Package);
	Packages.Add(HistoricalMap);
	return Packages;
}

bool LegacyPlacedPlayer(const AActor* Actor)
{
	return IsValid(Actor) && Actor->GetClass()->GetPathName() == OldCharacterClass;
}

bool CoffeeScript(UWorld& World, bool bApply, bool& bNeedsUpdate)
{
	ULevelScriptBlueprint* Script = World.PersistentLevel->GetLevelScriptBlueprint(true);
	if (!Script) return Error(TEXT("The audited CoffeeShop LevelScriptBlueprint is missing."));
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : Script->UbergraphPages)
		if (Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph) EventGraph = Graph;
	if (!EventGraph) return Error(TEXT("The audited CoffeeShop EventGraph is missing."));
	struct FNodeIdentity { const TCHAR* Name; const TCHAR* Class; const TCHAR* Guid; };
	// R3 source snapshot: 45 nodes, 49 links, five complete UI/start/pause/camera-input
	// components. There is no independent lighting script in this graph. Map lighting
	// and CineCamera actors are preserved; only the exact obsolete source graph is removed.
	const FNodeIdentity Expected[] = {
		{TEXT("K2Node_Event_2"), TEXT("/Script/BlueprintGraph.K2Node_Event"), TEXT("5F9982D2434E82950228949274BA8BD2")},
		{TEXT("K2Node_Event_1"), TEXT("/Script/BlueprintGraph.K2Node_Event"), TEXT("4B14E87F4034E99ACD7C46B1AF8454D1")},
		{TEXT("K2Node_CallFunction_0"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("FC131F034BA5CC2F61C235AF9DFD2E4C")},
		{TEXT("K2Node_Literal_0"), TEXT("/Script/BlueprintGraph.K2Node_Literal"), TEXT("30CD294F4D49E562985215931DB10A29")},
		{TEXT("K2Node_Literal_1"), TEXT("/Script/BlueprintGraph.K2Node_Literal"), TEXT("A48143C747B22010F75434B4C3131D1A")},
		{TEXT("K2Node_Literal_2"), TEXT("/Script/BlueprintGraph.K2Node_Literal"), TEXT("0247BBB647FC991E700646ADE6775E88")},
		{TEXT("K2Node_Literal_3"), TEXT("/Script/BlueprintGraph.K2Node_Literal"), TEXT("26C79AA2477CB2229457F5A94C07088F")},
		{TEXT("K2Node_Literal_5"), TEXT("/Script/BlueprintGraph.K2Node_Literal"), TEXT("3747F5614302B61D5535719293F75CE0")},
		{TEXT("K2Node_CallFunction_1"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("9A892DAF46014AB0269E4FA2B3E965D3")},
		{TEXT("K2Node_CallFunction_10"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("4DBF62FB4AA10F0FEF504A87C00314E5")},
		{TEXT("K2Node_CallFunction_27"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("7184F8A24E1011C1AF72A1B8FE4CD453")},
		{TEXT("K2Node_CallFunction_20"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("832451804B0EC1BCC764FFA75F1FF97B")},
		{TEXT("K2Node_CreateWidget_1"), TEXT("/Script/UMGEditor.K2Node_CreateWidget"), TEXT("68B4E0064DC540D4BB925887DD3A74E2")},
		{TEXT("K2Node_Select_0"), TEXT("/Script/BlueprintGraph.K2Node_Select"), TEXT("7C32AF274CA65BAC63157DBF70CBB29E")},
		{TEXT("K2Node_CallFunction_6"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("7479E3274E9974F170F98294FEAB33D4")},
		{TEXT("K2Node_CallFunction_26"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("2A8AE61D47E796734B6A1C89B6D889E1")},
		{TEXT("K2Node_InputKey_0"), TEXT("/Script/BlueprintGraph.K2Node_InputKey"), TEXT("7531AAC447CD9305E7492B858CA0AB31")},
		{TEXT("K2Node_CallFunction_24"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("1539EF9B4ECF3C76303AAAAD0C9151EA")},
		{TEXT("K2Node_CallFunction_12"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("F39110864559746909F1119A758EC264")},
		{TEXT("K2Node_CallFunction_14"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("64D0DB304E7D4CF6CF1152B22B36AFFB")},
		{TEXT("K2Node_CallFunction_11"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("B4F36329468F586F9F464B85F12BB4CC")},
		{TEXT("K2Node_CallFunction_5"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("0CB47E0C497DD374B380359F053C01B9")},
		{TEXT("K2Node_CallFunction_9"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("0A04287D4379C0D6F21C14AF732773CC")},
		{TEXT("K2Node_CustomEvent_2"), TEXT("/Script/BlueprintGraph.K2Node_CustomEvent"), TEXT("5335E4CC4E602FA298E3A7AEB3BEB534")},
		{TEXT("K2Node_CallFunction_29"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("F7A78DA64263C9FE93CB80810EB8F588")},
		{TEXT("K2Node_CallFunction_30"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("FC094D2442333A80EDB06BA0E5ACC3D4")},
		{TEXT("K2Node_CallFunction_36"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("89D5130247278992DF458BA7626216F8")},
		{TEXT("K2Node_VariableSet_0"), TEXT("/Script/BlueprintGraph.K2Node_VariableSet"), TEXT("97250F6049EC36C1B9703F884BFBC117")},
		{TEXT("K2Node_DynamicCast_0"), TEXT("/Script/BlueprintGraph.K2Node_DynamicCast"), TEXT("1EB22B234DCB882025B212B0EB74C756")},
		{TEXT("K2Node_VariableGet_0"), TEXT("/Script/BlueprintGraph.K2Node_VariableGet"), TEXT("77266A4A4D06712357C5168C63C3F1D4")},
		{TEXT("K2Node_CallFunction_3"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("8DFED2674BFFEA29DA6866A06E122826")},
		{TEXT("K2Node_CreateWidget_0"), TEXT("/Script/UMGEditor.K2Node_CreateWidget"), TEXT("AEF9D3D54D60B8E2FE8215B71BA6BF54")},
		{TEXT("K2Node_CallFunction_2"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("8DB476C94E75A55E020AC598558ECC61")},
		{TEXT("K2Node_CallFunction_4"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("F068420540F2CB43FEFFECB41B49F674")},
		{TEXT("K2Node_CallFunction_15"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("8CDACE4B44C1A34335F8879F86BF0D84")},
		{TEXT("K2Node_VariableSet_3"), TEXT("/Script/BlueprintGraph.K2Node_VariableSet"), TEXT("2C398652417E0E3E419EC8B01481886B")},
		{TEXT("K2Node_CallFunction_16"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("F35DBC2845C8523614B271BB6DEAE3E5")},
		{TEXT("K2Node_CreateWidget_3"), TEXT("/Script/UMGEditor.K2Node_CreateWidget"), TEXT("54F3F7DE43B28A1269B048BAC51C7CA9")},
		{TEXT("K2Node_VariableSet_4"), TEXT("/Script/BlueprintGraph.K2Node_VariableSet"), TEXT("003A02DF41D30F9E6E5F1DA1469453DF")},
		{TEXT("K2Node_VariableGet_1"), TEXT("/Script/BlueprintGraph.K2Node_VariableGet"), TEXT("E5669A9B45C713AB732E8D922E436FBB")},
		{TEXT("K2Node_InputKey_1"), TEXT("/Script/BlueprintGraph.K2Node_InputKey"), TEXT("0FFBE9BB4A25987FAE950AAA984994D6")},
		{TEXT("K2Node_MacroInstance_0"), TEXT("/Script/BlueprintGraph.K2Node_MacroInstance"), TEXT("5FDAA6CC4A53D0B95C8B5FA1EA46A25E")},
		{TEXT("K2Node_CallFunction_18"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("8423BB564BA0ECAC7F7795AFCCFB462D")},
		{TEXT("K2Node_CallFunction_19"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), TEXT("A798C3054125B9E6241C41A1C83203D5")},
		{TEXT("K2Node_InputKey_2"), TEXT("/Script/BlueprintGraph.K2Node_InputKey"), TEXT("B94FE369442EEC9BC02B0ABC54B980EE")},
	};
	if (!EventGraph->Nodes.IsEmpty())
	{
		if (EventGraph->Nodes.Num() != UE_ARRAY_COUNT(Expected)) return Error(TEXT("CoffeeShop source graph changed; refusing a blanket deletion."));
		int32 Connections = 0;
		for (const UEdGraphNode* Node : EventGraph->Nodes)
		{
			const FNodeIdentity* Identity = nullptr;
			for (const auto& Candidate : Expected) if (Node->GetFName() == Candidate.Name) { Identity = &Candidate; break; }
			if (!Identity || Node->GetClass()->GetPathName() != Identity->Class || Node->NodeGuid.ToString(EGuidFormats::Digits) != Identity->Guid)
				return Error(TEXT("CoffeeShop node does not match the audited name/class/GUID; no graph cleared."));
			for (const UEdGraphPin* Pin : Node->Pins)
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || Linked->GetOwningNode()->GetGraph() != EventGraph) return Error(TEXT("CoffeeShop graph contains an unaudited external link."));
					++Connections;
				}
		}
		if (Connections != 98) return Error(TEXT("CoffeeShop pin connectivity changed; no graph cleared."));
	}
	const FName LegacyVariables[] = {TEXT("mainmenu"), TEXT("ingameumg"), TEXT("pauseumg")};
	int32 VariableCount = 0;
	for (const auto& Variable : Script->NewVariables)
		for (FName Name : LegacyVariables) if (Variable.VarName == Name) ++VariableCount;
	bNeedsUpdate = !EventGraph->Nodes.IsEmpty() || VariableCount > 0;
	if (!bApply || !bNeedsUpdate) return true;
	Script->Modify(); EventGraph->Modify();
	const auto Nodes = EventGraph->Nodes;
	for (UEdGraphNode* Node : Nodes) FBlueprintEditorUtils::RemoveNode(Script, Node, true);
	for (FName Name : LegacyVariables) FBlueprintEditorUtils::RemoveMemberVariable(Script, Name);
	UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION COFFEE_UI_REMOVED Nodes=%d Variables=%d SourceComponents=5 LightingActorsPreserved=1"), Nodes.Num(), VariableCount);
	return EventGraph->Nodes.IsEmpty() || Error(TEXT("Audited CoffeeShop source nodes survived removal."));
}

bool RetireLegacyPresentation(const FString& BackupDirectory)
{
	const TCHAR* Packages[] = {
		TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_FirstPersonCharacter"),
		TEXT("/Game/FirstPerson/Blueprints/PlayerAnim"),
		TEXT("/Game/FirstPerson/Blueprints/weapon/BP_Weapon"),
		TEXT("/Game/FirstPerson/Blueprints/BP_FirstPersonCharacter"),
		TEXT("/Game/FirstPerson/Blueprints/BP_PickUp_Rifle")};
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanPathsSynchronous({TEXT("/Game")}, true);
	TSet<FName> Retiring;
	for (const TCHAR* Package : Packages) Retiring.Add(FName(Package));
	for (const TCHAR* Package : Packages)
	{
		TArray<FName> References;
		Registry.GetReferencers(FName(Package), References, UE::AssetRegistry::EDependencyCategory::Package);
		for (FName Reference : References)
			if (!Retiring.Contains(Reference)) return Error(FString::Printf(TEXT("Legacy presentation is still referenced: %s <- %s; nothing deleted."), Package, *Reference.ToString()));
	}
	TArray<UObject*> Assets;
	for (const TCHAR* Package : Packages)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
		if (!IFileManager::Get().FileExists(*Filename)) continue; // A second Retire run is a verification.
		if (!Backup(Package, false, BackupDirectory)) return false;
		UPackage* Loaded = LoadPackage(nullptr, Package, LOAD_None);
		if (!Loaded) return Error(FString(TEXT("Legacy package cannot be inspected: ")) + Package);
		bool bUnexpectedAsset = false;
		int32 InPackage = 0;
		// Enumerate the actual loaded package; LoadObject on a redirector path would follow
		// it to the destination and is therefore unsuitable for deciding what to delete.
		ForEachObjectWithPackage(Loaded, [Loaded, &Assets, &bUnexpectedAsset, &InPackage](UObject* Object)
		{
			UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION RETIRE_OBJECT Package=%s Object=%s Name=%s Class=%s IsAsset=%d"),
				*Loaded->GetName(), *Object->GetPathName(), *Object->GetName(), *Object->GetClass()->GetPathName(), Object->IsAsset());
			if (!Object->IsAsset()) return true;
			if (const auto* Generated = Cast<UBlueprintGeneratedClass>(Object))
			{
				const UBlueprint* Owner = Cast<UBlueprint>(Generated->ClassGeneratedBy);
				// UE5.5 active BPGCs report IsAsset=true. Their exact owning Blueprint
				// handles generated/skeleton class cleanup through normal asset deletion.
				if (Owner && Owner->GetOutermost() == Loaded && Generated->GetOutermost() == Loaded
					&& (Owner->GeneratedClass.Get() == Generated || Owner->SkeletonGeneratedClass.Get() == Generated))
				{
					UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION RETIRE_GENERATED_CLASS Owner=%s Class=%s DeleteWithBlueprint=1"),
						*Owner->GetPathName(), *Generated->GetPathName());
					return true;
				}
				bUnexpectedAsset = true;
			}
			else if (!Object->IsA<UBlueprint>() && !Object->IsA<UObjectRedirector>()) bUnexpectedAsset = true;
			else { Assets.AddUnique(Object); ++InPackage; }
			return true;
		}, false);
		if (bUnexpectedAsset || InPackage == 0) return Error(FString(TEXT("Unexpected content in retirement package: ")) + Package);
	}
	// Normal deletion retains the engine's in-memory reference checks. No force deletion,
	// redirector replacement, or recursive deletion of shared animation/mesh/DA dependencies.
	const int32 Deleted = Assets.IsEmpty() ? 0 : ObjectTools::DeleteObjects(Assets, false);
	for (const TCHAR* Package : Packages)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
		TArray<FAssetData> Remaining;
		Registry.GetAssetsByPackageName(FName(Package), Remaining);
		if (IFileManager::Get().FileExists(*Filename) || !Remaining.IsEmpty()) return Error(FString(TEXT("Normal deletion did not retire the full package: ")) + Package);
	}
	UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION RETIRE_PASS Packages=5 DeletedObjects=%d SharedResourcesDeleted=0 Backup=%s"), Deleted, *BackupDirectory);
	return true;
}

bool Maps(UBlueprint& Template, bool bApply, const FString& BackupDirectory)
{
	UActorFactory* Factory = bApply ? GEditor->FindActorFactoryByClass(UActorFactoryBlueprint::StaticClass()) : nullptr;
	if (bApply && !Factory) return Error(TEXT("Official Blueprint actor replacement factory is unavailable."));
	for (const TCHAR* Package : WeaponMapPackages())
	{
		UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Package);
		if (!World) return Error(FString(TEXT("Cannot load required map: ")) + Package);
		const bool bHistorical = FString(Package) == HistoricalMap;
		TArray<AActor*> Previous;
		int32 PreviousWeapons = 0, PreviousPlayers = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
			if (LegacyPickup(*It)) { Previous.Add(*It); ++PreviousWeapons; }
			else if (bHistorical && LegacyPlacedPlayer(*It)) { Previous.Add(*It); ++PreviousPlayers; }
		bool bScriptNeedsUpdate = false;
		if (bHistorical && !CoffeeScript(*World, false, bScriptNeedsUpdate)) return false;
		const bool bModeNeedsUpdate = bHistorical && World->GetWorldSettings()->DefaultGameMode != AfpstruePvpGameMode::StaticClass();
		if (bModeNeedsUpdate && World->GetWorldSettings()->DefaultGameMode != AGameModeBase::StaticClass())
			return Error(TEXT("CoffeeShop has an unaudited custom GameMode; no map edited."));
		const bool bChanged = !Previous.IsEmpty() || bScriptNeedsUpdate || bModeNeedsUpdate;
		if (!bApply && bChanged) return Error(FString(TEXT("Legacy map presentation/gameplay entry remains in ")) + Package);
		UActorFactory* StartFactory = bApply && PreviousPlayers > 0 ? GEditor->FindActorFactoryByClass(UActorFactoryPlayerStart::StaticClass()) : nullptr;
		if (PreviousPlayers > 0 && bApply && !StartFactory) return Error(TEXT("Official PlayerStart replacement factory is unavailable."));
		for (const AActor* Actor : Previous)
			if (Actor->GetLevel() != World->PersistentLevel || Actor->GetActorTransform().ContainsNaN())
				return Error(TEXT("Pickup is outside the approved persistent level or has an invalid transform; no map edited."));
		// Loaded external packages include unsaved actors generated while opening the map.
		// Preserve their initial state; existence on disk, not PKG_NewlyCreated alone, determines backup.
		struct FExternalBaseline { bool bOnDisk; bool bDirty; };
		TMap<UPackage*, FExternalBaseline> ExternalBaseline;
		TSet<UPackage*> BackedExternal;
		TSet<UPackage*> SourceExternal;
		TArray<UPackage*> ExternalBefore = World->PersistentLevel->GetLoadedExternalObjectPackages();
		for (const AActor* Actor : Previous)
			if (UPackage* External = Actor->GetExternalPackage())
			{
				ExternalBefore.AddUnique(External); SourceExternal.Add(External);
			}
		if (bApply && bChanged)
			for (UPackage* External : ExternalBefore)
			{
				const FString Filename = FPackageName::LongPackageNameToFilename(External->GetName(), FPackageName::GetAssetPackageExtension());
				const bool bOnDisk = IFileManager::Get().FileExists(*Filename);
				ExternalBaseline.Add(External, {bOnDisk, External->IsDirty()});
				UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION EXTERNAL_BASELINE Package=%s OnDisk=%d Dirty=%d NewlyCreated=%d File=%s"),
					*External->GetName(), bOnDisk, External->IsDirty(), External->HasAnyPackageFlags(PKG_NewlyCreated), *Filename);
				if (bOnDisk)
				{
					if (!External->GetName().StartsWith(TEXT("/Game/")) || !Backup(External->GetName(), false, BackupDirectory))
						return Error(TEXT("External package backup failed; no actors replaced."));
					BackedExternal.Add(External);
				}
				else if (External->IsDirty() && SourceExternal.Contains(External))
					return Error(TEXT("Replacement source has pre-existing unsaved edits; no actors replaced."));
			}
		// The engine broadcasts this even when a package was already dirty. Track the mutation
		// window so merely opening a map never authorizes saving its generated external actors.
		TSet<UPackage*> DirtiedByMigration;
		FDelegateHandle DirtyHandle;
		if (bApply && bChanged)
			DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda([&DirtiedByMigration](UPackage* Modified, bool)
			{ DirtiedByMigration.Add(Modified); });
		ON_SCOPE_EXIT { if (DirtyHandle.IsValid()) UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle); };
		for (AActor* Actor : Previous)
		{
			UPackage* const External = Actor->GetExternalPackage();
			const FTransform Transform = Actor->GetActorTransform();
			const FGuid Guid = Actor->GetActorGuid();
			const FName Name = Actor->GetFName();
			const FString Label = Actor->GetActorLabel();
			const TArray<FName> Tags = Actor->Tags;
			TArray<AActor*> Replaced;
			// Official replacement preserves ActorGuid, level-script references and cross-actor references/attachments.
			// Old per-component presentation overrides are intentionally excluded, matching the previous normal-play rule.
			const bool bPlayer = bHistorical && LegacyPlacedPlayer(Actor);
			UEditorActorSubsystem::ReplaceActors(bPlayer ? StartFactory : Factory, bPlayer ? FAssetData() : FAssetData(&Template), {Actor}, &Replaced, false);
			UClass* ExpectedClass = bPlayer ? APlayerStart::StaticClass() : Template.GeneratedClass.Get();
			if (Replaced.Num() != 1 || !IsValid(Replaced[0]) || Replaced[0]->GetClass() != ExpectedClass
				|| Replaced[0]->GetActorGuid() != Guid || Replaced[0]->GetFName() != Name
				|| Replaced[0]->GetExternalPackage() != External) return Error(TEXT("Official actor replacement changed identity or external package; map not saved."));
			Replaced[0]->SetActorTransform(Transform);
			Replaced[0]->Tags = Tags; Replaced[0]->SetActorLabel(bPlayer ? TEXT("PlayerStart") : Label);
			if (!Replaced[0]->GetActorTransform().Equals(Transform)) return Error(TEXT("Pickup actor transform changed; map not saved."));
			if (External)
				UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION EXTERNAL_REPLACED Actor=%s Guid=%s Package=%s IdentityPreserved=1"),
					*Replaced[0]->GetPathName(), *Guid.ToString(), *External->GetName());
		}
		if (bHistorical && bApply && bChanged)
		{
			if (!CoffeeScript(*World, true, bScriptNeedsUpdate)) return false;
			World->GetWorldSettings()->Modify();
			World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
			World->GetWorldSettings()->MarkPackageDirty();
		}
		for (TActorIterator<AActor> It(World); It; ++It)
			if (LegacyPickup(*It) || (bHistorical && LegacyPlacedPlayer(*It))) return Error(TEXT("Legacy actor survived replacement; map not saved."));
		if (bApply && bChanged)
		{
			if (ULevelScriptBlueprint* Script = World->PersistentLevel->GetLevelScriptBlueprint(true))
				if (!Compile(Script)) return Error(TEXT("Level-script references are incompatible with the replacement; map not saved."));
			UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle); DirtyHandle.Reset();
			TArray<UPackage*> PackagesToSave;
			TArray<UPackage*> ExternalAfter = World->PersistentLevel->GetLoadedExternalObjectPackages();
			for (UPackage* External : ExternalBefore) ExternalAfter.AddUnique(External); // Includes deleted/empty packages.
			for (UPackage* External : ExternalAfter)
			{
				const FExternalBaseline* Baseline = ExternalBaseline.Find(External);
				const bool bPreExistingUnsaved = Baseline && !Baseline->bOnDisk && Baseline->bDirty;
				if (!External->IsDirty() || !DirtiedByMigration.Contains(External) || bPreExistingUnsaved)
				{
					UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION EXTERNAL_SKIP Package=%s Dirty=%d Touched=%d PreExistingUnsaved=%d"),
						*External->GetName(), External->IsDirty(), DirtiedByMigration.Contains(External), bPreExistingUnsaved);
					continue;
				}
				if (!External->GetName().StartsWith(TEXT("/Game/"))) return Error(TEXT("Touched external package is outside project content; map not saved."));
				if (Baseline && Baseline->bOnDisk && !BackedExternal.Contains(External))
					return Error(TEXT("Touched persistent external package was not backed up; map not saved."));
				if (!Baseline && IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(External->GetName(), FPackageName::GetAssetPackageExtension())))
					return Error(TEXT("An unbacked persistent external package appeared during replacement; map not saved."));
				// New packages are allowed only when associated with this loaded world and marked
				// dirty in the mutation window; original unsaved/dirty memory packages stay untouched.
				PackagesToSave.AddUnique(External);
				UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION EXTERNAL_SELECTED Package=%s OnDiskBefore=%d Touched=1"),
					*External->GetName(), Baseline && Baseline->bOnDisk);
			}
			for (UPackage* External : SourceExternal)
				if (!PackagesToSave.Contains(External)) return Error(TEXT("Replacement source external package was not selected for save."));
			const int32 ExternalSaved = PackagesToSave.Num();
			if (!PackagesToSave.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
				return Error(TEXT("Selected external package save failed."));
			UPackage* const MapPackage = World->GetOutermost();
			if (MapPackage->HasAnyPackageFlags(PKG_NewlyCreated)) return Error(TEXT("Expected an existing approved map package."));
			World->MarkPackageDirty();
			// Save the existing writable map through the official filtered fast path. This avoids
			// checkout-prompt package expansion and SaveWorld's automatic saving of unrelated externals.
			if (!FEditorFileUtils::SaveDirtyPackages(false, true, false, true, false, false, nullptr,
				[MapPackage](UPackage* Candidate) { return Candidate != MapPackage; }, true))
				return Error(TEXT("Approved map save failed."));
			PackagesToSave.AddUnique(MapPackage);
			for (const UPackage* Saved : PackagesToSave)
				if (Saved->IsDirty()) return Error(TEXT("Map/external package remained dirty after save: ") + Saved->GetName());
			UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION EXTERNAL_SAVE_PASS Map=%s Packages=%d"), Package, ExternalSaved);
		}
		if (bHistorical)
		{
			if (!CoffeeScript(*World, false, bScriptNeedsUpdate) || bScriptNeedsUpdate || World->GetWorldSettings()->DefaultGameMode != AfpstruePvpGameMode::StaticClass())
				return Error(TEXT("CoffeeShop still has a second gameplay/HUD pipeline."));
		}
		UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION MAP_PASS Map=%s Replaced=%d PlayersToStarts=%d LegacyRemaining=0 Saved=%d Historical=%d"),
			Package, PreviousWeapons, PreviousPlayers, bApply && bChanged, bHistorical);
	}
	return true;
}

void Run(const TArray<FString>& Args)
{
	const bool bApply = Args.Num() == 1 && Args[0] == TEXT("Apply");
	const bool bRetire = Args.Num() == 1 && Args[0] == TEXT("Retire");
	if ((!Args.IsEmpty() && !bApply && !bRetire) || !GEditor) { Error(TEXT("Usage: fp.PrepareWeaponPresentation [Apply|Retire]")); return; }
	FString BackupDirectory;
	if (bApply || bRetire) BackupDirectory = FPaths::ProjectSavedDir() / TEXT("Backups/WeaponPresentation") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (bApply)
	{
		if (!Backup(RARPackage, false, BackupDirectory, true) || !Backup(TemplatePackage, false, BackupDirectory, true)) return;
		for (const TCHAR* Package : WeaponMapPackages()) if (!Backup(Package, true, BackupDirectory)) return;
	}
	UBlueprint* RAR = Blueprint(RARPackage, AfpstrueRARWeapon::StaticClass(), bApply);
	UBlueprint* Template = Blueprint(TemplatePackage, AfpstrueTemplateWeapon::StaticClass(), bApply);
	if (!RAR || !Template) return;
	TStrongObjectPtr<UBlueprint> RARGuard(RAR), TemplateGuard(Template); // Normal asset deletion may collect garbage.
	if (bApply)
	{
		if (!Configure(*RAR, true) || !Configure(*Template, false)) return;
		RAR->MarkPackageDirty(); Template->MarkPackageDirty();
		if (!Validate(*RAR, true) || !Validate(*Template, false)
			|| !UEditorLoadingAndSavingUtils::SavePackages({RAR->GetOutermost(), Template->GetOutermost()}, false)) return;
	}
	if (!Validate(*RAR, true) || !Validate(*Template, false)) return;
	// The native hosts deliberately have no concrete appearance to fall back to.
	const AActor* NativeHosts[] = {GetDefault<AfpstrueRARWeapon>(), GetDefault<AfpstrueTemplateWeapon>()};
	for (const AActor* Native : NativeHosts)
	{
		UfpstrueWeaponComponent* NativeGun = Weapon(const_cast<AActor*>(Native));
		if (!NativeGun || NativeGun->GetSkeletalMeshAsset() || NativeGun->GetAnimClass() || NativeGun->WeaponFireMontage
			|| NativeGun->FireSound || NativeGun->MuzzleEffect || NativeGun->ImpactEffect) { Error(TEXT("Native appearance fallback remains.")); return; }
	}
	const auto* Mode = GetDefault<AfpstruePvpGameMode>();
	const auto* Field = FindFProperty<FClassProperty>(Mode->GetClass(), TEXT("DefaultWeaponClass"));
	// The first generation process may have constructed the native GameMode CDO before the new BP existed.
	// Only a fresh check process can prove that the native bootstrap resolves the saved authored class.
	if (!Field || (!bApply && Field->GetObjectPropertyValue_InContainer(Mode) != Template->GeneratedClass.Get()))
	{
		Error(TEXT("Default GameMode does not select the actual Template weapon Blueprint.")); return;
	}
	if (!Maps(*Template, bApply, BackupDirectory)) return;
	if (bRetire && (!RetireLegacyPresentation(BackupDirectory) || !Validate(*RAR, true) || !Validate(*Template, false))) return;
	UE_LOG(LogTemp, Display, TEXT("WEAPON_PRESENTATION PASS Apply=%d Retire=%d Backup=%s NativeAppearance=0 AuthoredResources=1 ProductionMaps=4 HistoricalMaps=1 NativeDefaultColdChecked=%d"), bApply, bRetire, *BackupDirectory, !bApply);
}

FAutoConsoleCommand Command(TEXT("fp.PrepareWeaponPresentation"),
	TEXT("Check authored weapons, four production maps and the CoffeeShop historical map. Apply backs up and migrates old actors/UI offline; Retire normally deletes only the closed five-package legacy presentation cluster."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
