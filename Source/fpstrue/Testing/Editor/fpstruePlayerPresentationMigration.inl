// Editor-only migration of six actual player presentation assets. Never edit AnimGraph or level instances.
#include "Characters/Player/fpstruePlayerAnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"

namespace FpstruePlayerPresentationMigration
{
const TCHAR* Packages[] = {TEXT("/Game/FirstPerson/Animations/RAR/ABP_RARBody"), TEXT("/Game/FirstPerson/Animations/RAR/ABP_RARArms"),
	TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateBody"), TEXT("/Game/FirstPerson/Animations/FPSMPT/ABP_TemplateArms"),
	TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_RARCharacter"), TEXT("/Game/FirstPerson/Blueprints/firstperson/BP_TemplateCharacter")};
const TCHAR* ResourceFields[] = {TEXT("FireMontage"), TEXT("AimedFireMontage"), TEXT("ReloadMontage"), TEXT("EmptyReloadMontage"), TEXT("EquipMontage"), TEXT("TurnLeftAnimation"), TEXT("TurnRightAnimation")};
const TCHAR* JsonFields[] = {TEXT("fire_montage"), TEXT("aimed_fire_montage"), TEXT("reload_montage"), TEXT("empty_reload_montage"), TEXT("equip_montage"), TEXT("turn_left_animation"), TEXT("turn_right_animation")};
const TCHAR* FOVCollectionPath = TEXT("/Game/InfimaGames/ArtCore/MaterialLibrary/MPC_IG_FOV.MPC_IG_FOV");
bool Fail(const FString& Reason) { UE_LOG(LogTemp, Error, TEXT("PLAYER_PRESENTATION FAIL %s"), *Reason); return false; }
UEdGraphPin* Pin(UEdGraphNode* Node, FName Name, EEdGraphPinDirection Direction)
{ if (Node) for (UEdGraphPin* Value : Node->Pins) if (Value && Value->PinName == Name && Value->Direction == Direction) return Value; return nullptr; }
bool Edge(const UEdGraphPin* Input, const UEdGraphPin* Output)
{ return Input && Output && Input->LinkedTo.Num() == 1 && Input->LinkedTo[0] == Output; }

struct FGraph
{
	UEdGraph* Graph = nullptr; UClass* Native = nullptr; bool bOK = true;
	template<class T> T* Node(const TCHAR* Name, int32 X = 0, int32 Y = 0)
	{ auto* Result = NewObject<T>(Graph, FName(Name), RF_Transactional); Graph->AddNode(Result, false, false); Result->CreateNewGuid(); Result->NodePosX = X; Result->NodePosY = Y; return Result; }
	void Link(UEdGraphPin* Output, UEdGraphPin* Input)
	{ if (!Output || !Input || !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input)) bOK = Fail(TEXT("Missing or incompatible presentation pin")); }
	void Default(UEdGraphNode* Node, const TCHAR* Name, const FString& Value)
	{ auto* Input = Pin(Node, Name, EGPD_Input); if (!Input) { bOK = false; return; } GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Input, Value); }
	UK2Node_Event* Event(const TCHAR* Function)
	{ auto* Result = Node<UK2Node_Event>(TEXT("PresentationEntry"), 0, 0); Result->EventReference.SetExternalMember(Function, Native); Result->bOverrideFunction = true; Result->PostPlacedNewNode(); Result->AllocateDefaultPins(); return Result; }
	UK2Node_CallFunction* Call(UClass* Owner, const TCHAR* Function, const TCHAR* Name, int32 X)
	{ auto* Result = Node<UK2Node_CallFunction>(Name, X, 0); UFunction* Target = Owner->FindFunctionByName(Function); if (!Target) { bOK = false; return Result; } Result->SetFromFunction(Target); Result->PostPlacedNewNode(); Result->AllocateDefaultPins(); return Result; }
	UK2Node_VariableGet* Read(FName Field, const TCHAR* Name, int32 X, int32 Y)
	{ auto* Result = Node<UK2Node_VariableGet>(Name, X, Y); const FProperty* Property = FindFProperty<FProperty>(Native, Field); if (!Property) { bOK = false; return Result; } Result->SetFromProperty(Property, true, Native); Result->PostPlacedNewNode(); Result->AllocateDefaultPins(); return Result; }
};

// Only our named graph is replaced. Existing AnimGraph, unrelated events and all Designer/component overrides stay in place.
bool OpenGraph(UBlueprint* Blueprint, UClass* Native, const TCHAR* GraphName, const TCHAR* EventName, FGraph& B)
{
	TArray<UEdGraph*> All; Blueprint->GetAllGraphs(All);
	for (UEdGraph* Graph : All) for (UEdGraphNode* Node : Graph->Nodes)
		if (const auto* Event = Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName() == EventName && Graph->GetFName() != GraphName)
			return Fail(Blueprint->GetPathName() + TEXT(" already owns this event outside the migration graph"));
	B.Native = Native;
	for (UEdGraph* Graph : Blueprint->UbergraphPages) if (Graph->GetFName() == GraphName) B.Graph = Graph;
	if (!B.Graph) { B.Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, GraphName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()); FBlueprintEditorUtils::AddUbergraphPage(Blueprint, B.Graph); }
	const auto Previous = B.Graph->Nodes; for (UEdGraphNode* Node : Previous) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
	return true;
}

bool CameraGraph(UBlueprint* Blueprint)
{
	FGraph B; if (!OpenGraph(Blueprint, AfpstrueCharacter::StaticClass(), TEXT("PlayerCameraPresentation"), TEXT("ApplyCameraPresentation"), B)) return false;
	auto* Entry = B.Event(TEXT("ApplyCameraPresentation"));
	auto* Hip = B.Read(TEXT("HipFieldOfView"), TEXT("HipFOV"), 0, 160); auto* Aim = B.Read(TEXT("AimFieldOfView"), TEXT("AimFOV"), 0, 230);
	auto* Camera = B.Read(TEXT("FirstPersonCameraComponent"), TEXT("LocalCamera"), 400, 200);
	auto* Lerp = B.Call(UKismetMathLibrary::StaticClass(), TEXT("Lerp"), TEXT("BlendFOV"), 350);
	auto* Set = B.Call(UCameraComponent::StaticClass(), TEXT("SetFieldOfView"), TEXT("ApplyFOV"), 750);
	B.Link(Pin(Entry, TEXT("then"), EGPD_Output), Pin(Set, TEXT("execute"), EGPD_Input));
	B.Link(Pin(Hip, TEXT("HipFieldOfView"), EGPD_Output), Pin(Lerp, TEXT("A"), EGPD_Input)); B.Link(Pin(Aim, TEXT("AimFieldOfView"), EGPD_Output), Pin(Lerp, TEXT("B"), EGPD_Input));
	B.Link(Pin(Entry, TEXT("AimBlend"), EGPD_Output), Pin(Lerp, TEXT("Alpha"), EGPD_Input));
	B.Link(Pin(Lerp, TEXT("ReturnValue"), EGPD_Output), Pin(Set, TEXT("InFieldOfView"), EGPD_Input));
	B.Link(Pin(Camera, TEXT("FirstPersonCameraComponent"), EGPD_Output), Pin(Set, TEXT("self"), EGPD_Input)); return B.bOK;
}

bool InitializationGraph(UBlueprint* Blueprint, bool bRAR)
{
	FGraph B; if (!OpenGraph(Blueprint, AfpstrueCharacter::StaticClass(), TEXT("PlayerVisualInitialization"), TEXT("InitializeVisualPresentation"), B)) return false;
	auto* Entry = B.Event(TEXT("InitializeVisualPresentation")); auto* Arms = B.Read(TEXT("Mesh1P"), TEXT("ViewMesh"), 0, 200);
	UEdGraphPin* Previous = Pin(Entry, TEXT("then"), EGPD_Output);
	for (int32 Index = 0; Index < (bRAR ? 2 : 3); ++Index)
	{
		auto* Call = B.Call(bRAR ? UMeshComponent::StaticClass() : USkinnedMeshComponent::StaticClass(), bRAR ? TEXT("SetScalarParameterValueOnMaterials") : TEXT("HideBoneByName"), *FString::Printf(TEXT("Initialize_%d"), Index), 350 + Index * 350);
		B.Link(Previous, Pin(Call, TEXT("execute"), EGPD_Input)); Previous = Pin(Call, TEXT("then"), EGPD_Output);
		UK2Node_VariableGet* Mesh = Arms; FName Field(TEXT("Mesh1P"));
		if (bRAR && Index == 0) { Field = TEXT("Mesh"); Mesh = B.Read(Field, TEXT("WorldMesh"), 0, 300); }
		B.Link(Pin(Mesh, Field, EGPD_Output), Pin(Call, TEXT("self"), EGPD_Input));
		if (bRAR) { B.Default(Call, TEXT("ParameterName"), TEXT("Field Of View Alpha")); B.Default(Call, TEXT("ParameterValue"), Index == 0 ? TEXT("0") : TEXT("1")); }
		else { const TCHAR* Bones[] = {TEXT("head"), TEXT("thigh_l"), TEXT("thigh_r")}; B.Default(Call, TEXT("BoneName"), Bones[Index]); B.Default(Call, TEXT("PhysBodyOption"), TEXT("PBO_None")); }
	}
	return B.bOK;
}

bool ProjectionGraph(UBlueprint* Blueprint)
{
	FGraph B; if (!OpenGraph(Blueprint, UfpstruePlayerAnimInstance::StaticClass(), TEXT("ViewmodelProjectionPresentation"), TEXT("PresentViewmodelProjection"), B)) return false;
	auto* Entry = B.Event(TEXT("PresentViewmodelProjection"));
	auto* Self = B.Node<UK2Node_Self>(TEXT("LocalAnimationWorld"), 0, 180); Self->AllocateDefaultPins();
	auto* Collection = B.Read(TEXT("ViewmodelFOVCollection"), TEXT("AuthoredFOVCollection"), 0, 300);
	auto* Set = B.Call(UKismetMaterialLibrary::StaticClass(), TEXT("SetScalarParameterValue"), TEXT("ApplyViewmodelFOV"), 500);
	B.Link(Pin(Entry, TEXT("then"), EGPD_Output), Pin(Set, TEXT("execute"), EGPD_Input));
	B.Link(Pin(Self, TEXT("self"), EGPD_Output), Pin(Set, TEXT("WorldContextObject"), EGPD_Input));
	B.Link(Pin(Collection, TEXT("ViewmodelFOVCollection"), EGPD_Output), Pin(Set, TEXT("Collection"), EGPD_Input));
	B.Link(Pin(Entry, TEXT("FieldOfView"), EGPD_Output), Pin(Set, TEXT("ParameterValue"), EGPD_Input));
	B.Default(Set, TEXT("ParameterName"), TEXT("Field Of View")); return B.bOK;
}

UEdGraph* FindGraph(UBlueprint* Blueprint, const TCHAR* GraphName, const TCHAR* EventName, UClass* Native, UK2Node_Event*& Entry)
{
	UEdGraph* Found = nullptr; int32 Count = 0; TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes) if (auto* Event = Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName() == EventName)
	{ ++Count; Found = Graph; Entry = Event; }
	UFunction* Function = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->FindFunctionByName(EventName) : nullptr;
	if (Count != 1 || !Found || Found->GetFName() != GraphName || !Function || Function->GetOuterUClass() != Blueprint->GeneratedClass.Get()
		|| Entry->EventReference.ResolveMember<UFunction>(Native) != Native->FindFunctionByName(EventName)) { Fail(Blueprint->GetPathName() + TEXT(" missing unique compiled presentation event")); return nullptr; }
	return Found;
}
UK2Node_CallFunction* CheckedCall(UEdGraph* Graph, const TCHAR* Name, UClass* Owner, const TCHAR* Function)
{ auto* Call = FindObject<UK2Node_CallFunction>(Graph, Name); return Call && Call->GetTargetFunction() == Owner->FindFunctionByName(Function) ? Call : nullptr; }
UK2Node_VariableGet* CheckedRead(UEdGraph* Graph, const TCHAR* Name, UClass* Native, FName Field)
{ auto* Read = FindObject<UK2Node_VariableGet>(Graph, Name); return Read && Read->VariableReference.IsSelfContext() && Read->VariableReference.GetMemberName() == Field && Read->GetPropertyForVariable() == FindFProperty<FProperty>(Native, Field) ? Read : nullptr; }
bool Literal(UEdGraphNode* Node, const TCHAR* Name, const FString& Expected)
{ const auto* Value = Pin(Node, Name, EGPD_Input); return Value && Value->LinkedTo.IsEmpty() && !Value->DefaultObject && Value->DefaultValue == Expected; }
bool NumberLiteral(UEdGraphNode* Node, const TCHAR* Name, float Expected)
{ const auto* Value=Pin(Node,Name,EGPD_Input); float Number; return Value && Value->LinkedTo.IsEmpty() && !Value->DefaultObject && FDefaultValueHelper::ParseFloat(Value->DefaultValue,Number) && FMath::IsFinite(Number) && Number==Expected; }
bool NoPhysicsLiteral(UEdGraphNode* Node)
{ const auto* Value=Pin(Node,TEXT("PhysBodyOption"),EGPD_Input); const auto* Enum=Value?Cast<UEnum>(Value->PinType.PinSubCategoryObject.Get()):nullptr; return Value && Value->LinkedTo.IsEmpty() && !Value->DefaultObject && Enum && Enum->GetFName()==TEXT("EPhysBodyOp") && Enum->GetValueByNameString(Value->DefaultValue)==PBO_None; }
bool Ends(UEdGraphNode* Node) { const auto* Value=Pin(Node,TEXT("then"),EGPD_Output); return Value && Value->LinkedTo.IsEmpty(); }

// UE5.5's Lerp is double while the authored values/camera setter are float. Only official real conversions on checked edges are accepted.
bool RealEdge(const UEdGraphPin* Input, const UEdGraphPin* Output, TSet<const UEdGraphNode*>& Allowed)
{
	if (Edge(Input, Output)) return true;
	if (!Input || !Output || Input->LinkedTo.Num() != 1) return false;
	auto* Conversion = Cast<UK2Node_CallFunction>(Input->LinkedTo[0]->GetOwningNode());
	if (!Conversion || Input->LinkedTo[0] != Pin(Conversion, TEXT("ReturnValue"), EGPD_Output)) return false;
	UFunction* Function = Conversion->GetTargetFunction(); if (!Function || Function->GetOuterUClass() != UKismetMathLibrary::StaticClass()) return false;
	const bool bUp = Function->GetFName() == TEXT("Conv_FloatToDouble"), bDown = Function->GetFName() == TEXT("Conv_DoubleToFloat");
	if ((!bUp && !bDown) || !Edge(Pin(Conversion, bUp ? TEXT("InFloat") : TEXT("InDouble"), EGPD_Input), Output)) return false;
	Allowed.Add(Conversion); return true;
}
bool CameraValid(UBlueprint* Blueprint)
{
	UK2Node_Event* Entry = nullptr; UEdGraph* Graph = FindGraph(Blueprint, TEXT("PlayerCameraPresentation"), TEXT("ApplyCameraPresentation"), AfpstrueCharacter::StaticClass(), Entry); if (!Graph) return false;
	auto* Hip = CheckedRead(Graph, TEXT("HipFOV"), AfpstrueCharacter::StaticClass(), TEXT("HipFieldOfView")); auto* Aim = CheckedRead(Graph, TEXT("AimFOV"), AfpstrueCharacter::StaticClass(), TEXT("AimFieldOfView"));
	auto* Camera = CheckedRead(Graph, TEXT("LocalCamera"), AfpstrueCharacter::StaticClass(), TEXT("FirstPersonCameraComponent"));
	auto* Lerp = CheckedCall(Graph, TEXT("BlendFOV"), UKismetMathLibrary::StaticClass(), TEXT("Lerp")); auto* Set = CheckedCall(Graph, TEXT("ApplyFOV"), UCameraComponent::StaticClass(), TEXT("SetFieldOfView"));
	if (!Hip || !Aim || !Camera || !Lerp || !Set) return false;
	TSet<const UEdGraphNode*> Allowed{Entry,Hip,Aim,Camera,Lerp,Set};
	if (!Edge(Pin(Set,TEXT("execute"),EGPD_Input),Pin(Entry,TEXT("then"),EGPD_Output)) || !Ends(Set)
		|| !Edge(Pin(Set,TEXT("self"),EGPD_Input),Pin(Camera,TEXT("FirstPersonCameraComponent"),EGPD_Output))
		|| !RealEdge(Pin(Lerp,TEXT("A"),EGPD_Input),Pin(Hip,TEXT("HipFieldOfView"),EGPD_Output),Allowed)
		|| !RealEdge(Pin(Lerp,TEXT("B"),EGPD_Input),Pin(Aim,TEXT("AimFieldOfView"),EGPD_Output),Allowed)
		|| !RealEdge(Pin(Lerp,TEXT("Alpha"),EGPD_Input),Pin(Entry,TEXT("AimBlend"),EGPD_Output),Allowed)
		|| !RealEdge(Pin(Set,TEXT("InFieldOfView"),EGPD_Input),Pin(Lerp,TEXT("ReturnValue"),EGPD_Output),Allowed)) return false;
	return Graph->Nodes.Num() == Allowed.Num();
}
bool InitializationValid(UBlueprint* Blueprint, bool bRAR)
{
	UK2Node_Event* Entry = nullptr; UEdGraph* Graph = FindGraph(Blueprint,TEXT("PlayerVisualInitialization"),TEXT("InitializeVisualPresentation"),AfpstrueCharacter::StaticClass(),Entry); if (!Graph || Graph->Nodes.Num() != 5) return false;
	auto* Arms = CheckedRead(Graph,TEXT("ViewMesh"),AfpstrueCharacter::StaticClass(),TEXT("Mesh1P")); if (!Arms) return false;
	UEdGraphPin* Previous = Pin(Entry,TEXT("then"),EGPD_Output);
	for (int32 Index=0; Index<(bRAR?2:3); ++Index)
	{
		auto* Call = CheckedCall(Graph,*FString::Printf(TEXT("Initialize_%d"),Index),bRAR?UMeshComponent::StaticClass():USkinnedMeshComponent::StaticClass(),bRAR?TEXT("SetScalarParameterValueOnMaterials"):TEXT("HideBoneByName"));
		auto* Mesh = bRAR && Index==0 ? CheckedRead(Graph,TEXT("WorldMesh"),AfpstrueCharacter::StaticClass(),TEXT("Mesh")) : Arms;
		if (!Call || !Mesh || !Edge(Pin(Call,TEXT("execute"),EGPD_Input),Previous) || !Edge(Pin(Call,TEXT("self"),EGPD_Input),Pin(Mesh,bRAR&&Index==0?TEXT("Mesh"):TEXT("Mesh1P"),EGPD_Output))) return false;
		if (bRAR) { if (!Literal(Call,TEXT("ParameterName"),TEXT("Field Of View Alpha")) || !NumberLiteral(Call,TEXT("ParameterValue"),float(Index))) return false; }
		else { const TCHAR* Bones[]={TEXT("head"),TEXT("thigh_l"),TEXT("thigh_r")}; if (!Literal(Call,TEXT("BoneName"),Bones[Index]) || !NoPhysicsLiteral(Call)) return false; }
		Previous=Pin(Call,TEXT("then"),EGPD_Output);
	}
	return Previous && Previous->LinkedTo.IsEmpty();
}
bool ProjectionValid(UBlueprint* Blueprint, bool bRequired)
{
	if (!bRequired) { TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs); for (UEdGraph* Graph:Graphs) for (UEdGraphNode* Node:Graph->Nodes) if (const auto* Event=Cast<UK2Node_Event>(Node); Event && Event->EventReference.GetMemberName()==TEXT("PresentViewmodelProjection")) return false; return true; }
	UK2Node_Event* Entry=nullptr; UEdGraph* Graph=FindGraph(Blueprint,TEXT("ViewmodelProjectionPresentation"),TEXT("PresentViewmodelProjection"),UfpstruePlayerAnimInstance::StaticClass(),Entry); if (!Graph || Graph->Nodes.Num()!=4) return false;
	auto* Self=FindObject<UK2Node_Self>(Graph,TEXT("LocalAnimationWorld")); auto* Collection=CheckedRead(Graph,TEXT("AuthoredFOVCollection"),UfpstruePlayerAnimInstance::StaticClass(),TEXT("ViewmodelFOVCollection"));
	auto* Set=CheckedCall(Graph,TEXT("ApplyViewmodelFOV"),UKismetMaterialLibrary::StaticClass(),TEXT("SetScalarParameterValue"));
	return Self && Collection && Set && Edge(Pin(Set,TEXT("execute"),EGPD_Input),Pin(Entry,TEXT("then"),EGPD_Output)) && Ends(Set)
		&& Edge(Pin(Set,TEXT("WorldContextObject"),EGPD_Input),Pin(Self,TEXT("self"),EGPD_Output))
		&& Edge(Pin(Set,TEXT("Collection"),EGPD_Input),Pin(Collection,TEXT("ViewmodelFOVCollection"),EGPD_Output))
		&& Edge(Pin(Set,TEXT("ParameterValue"),EGPD_Input),Pin(Entry,TEXT("FieldOfView"),EGPD_Output)) && Literal(Set,TEXT("ParameterName"),TEXT("Field Of View"));
}

FFpstruePlayerAnimationSettings Settings()
{ FFpstruePlayerAnimationSettings Value; Value.SpinePivotBone=TEXT("spine_03"); Value.GunRootBone=TEXT("ik_hand_root"); Value.ViewmodelFOVCurve=TEXT("Field Of View Viewmodel"); Value.ViewmodelFOVAddCurve=TEXT("Field Of View Viewmodel Add"); return Value; }
bool AnimDefaults(UAnimBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Snapshot, bool bApply, bool bProjection)
{
	auto* Defaults=Cast<UfpstruePlayerAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject()); const TSharedPtr<FJsonObject>* Assets=nullptr;
	if (!Defaults || !Blueprint->TargetSkeleton || !Snapshot->TryGetObjectField(TEXT("assets"),Assets)) return false;
	for (int32 Index=0;Index<UE_ARRAY_COUNT(ResourceFields);++Index)
	{
		const auto* Field=FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(),ResourceFields[Index]); const auto* Value=(*Assets)->Values.Find(JsonFields[Index]); if (!Field || !Value || !Value->IsValid()) return false;
		UObject* Desired=nullptr;
		if ((*Value)->Type!=EJson::Null) { FString Path; if (!(*Value)->TryGetString(Path) || Path.IsEmpty()) return false; Desired=StaticLoadObject(Index<5?UAnimMontage::StaticClass():UAnimSequence::StaticClass(),nullptr,*Path); if (!Desired) return false; const auto* Animation=Cast<UAnimationAsset>(Desired); if (!Animation || Animation->GetSkeleton()!=Blueprint->TargetSkeleton) return false; }
		if (bApply) Field->SetObjectPropertyValue_InContainer(Defaults,Desired);
		if (Field->GetObjectPropertyValue_InContainer(Defaults)!=Desired) return false;
	}
	const auto Expected=Settings(); if (bApply) { Defaults->Modify(); Defaults->AnimationSettings=Expected; Defaults->ViewmodelFOVCollection=bProjection?LoadObject<UMaterialParameterCollection>(nullptr,FOVCollectionPath):nullptr; }
	if (!FFpstruePlayerAnimationSettings::StaticStruct()->CompareScriptStruct(&Defaults->AnimationSettings,&Expected,0)) return false;
	if (bProjection) { if (!Defaults->ViewmodelFOVCollection || Defaults->ViewmodelFOVCollection->GetPathName()!=FOVCollectionPath) return false; TArray<FName> Names; Defaults->ViewmodelFOVCollection->GetParameterNames(Names,false); if (!Names.Contains(TEXT("Field Of View"))) return false; }
	else if (Defaults->ViewmodelFOVCollection) return false;
	return ProjectionValid(Blueprint,bProjection);
}

bool Triple(const TSharedPtr<FJsonObject>& Object,const TCHAR* Key,FVector& Out)
{ const TArray<TSharedPtr<FJsonValue>>* Array=nullptr; if (!Object->TryGetArrayField(Key,Array) || Array->Num()!=3) return false; double X,Y,Z; if (!(*Array)[0]->TryGetNumber(X)||!(*Array)[1]->TryGetNumber(Y)||!(*Array)[2]->TryGetNumber(Z)||!FMath::IsFinite(X)||!FMath::IsFinite(Y)||!FMath::IsFinite(Z)) return false; Out=FVector(X,Y,Z); return true; }
bool CharacterDefaults(UBlueprint* Blueprint,const TSharedPtr<FJsonObject>& Snapshot,bool bApply,bool bRAR,UAnimBlueprint* Body,UAnimBlueprint* Arms)
{
	auto* Defaults=Cast<AfpstrueCharacter>(Blueprint->GeneratedClass->GetDefaultObject()); const TArray<TSharedPtr<FJsonValue>>* Components=nullptr;
	if (!Defaults || !Defaults->UsesNativePresentation() || Defaults->UsesRARPresentation()!=bRAR || !Snapshot->TryGetArrayField(TEXT("components"),Components) || Components->Num()!=6) return false;
	TSet<FName> Seen;
	for (const auto& Value:*Components)
	{
		const auto Entry=Value->AsObject(); FString Name,Class; FVector Location,Rotation,Scale;
		if (!Entry || !Entry->TryGetStringField(TEXT("name"),Name)||!Entry->TryGetStringField(TEXT("class"),Class)||!Triple(Entry,TEXT("location"),Location)||!Triple(Entry,TEXT("rotation"),Rotation)||!Triple(Entry,TEXT("scale"),Scale)||Seen.Contains(FName(*Name))) return false;
		Seen.Add(FName(*Name)); auto* Component=Cast<USceneComponent>(Defaults->GetDefaultSubobjectByName(FName(*Name))); if (!Component || Component->GetClass()->GetPathName()!=Class) return false;
		const FRotator Euler(Rotation.X,Rotation.Y,Rotation.Z); if (bApply) { Component->Modify(); Component->SetRelativeLocationAndRotation(Location,Euler); Component->SetRelativeScale3D(Scale); }
		if (!Component->GetRelativeLocation().Equals(Location,.001)||!Component->GetRelativeRotation().Equals(Euler,.001)||!Component->GetRelativeScale3D().Equals(Scale,.00001)) return false;
		if (auto* Boom=Cast<USpringArmComponent>(Component))
		{
			double Length,Lag,RotationLag; bool bLag,bRotationLag;
			if (!Entry->TryGetNumberField(TEXT("target_arm_length"),Length)||!Entry->TryGetNumberField(TEXT("camera_lag_speed"),Lag)||!Entry->TryGetNumberField(TEXT("camera_rotation_lag_speed"),RotationLag)||!Entry->TryGetBoolField(TEXT("enable_camera_lag"),bLag)||!Entry->TryGetBoolField(TEXT("enable_camera_rotation_lag"),bRotationLag)) return false;
			if (bApply) { Boom->TargetArmLength=Length; Boom->CameraLagSpeed=Lag; Boom->CameraRotationLagSpeed=RotationLag; Boom->bEnableCameraLag=bLag; Boom->bEnableCameraRotationLag=bRotationLag; }
			if (!FMath::IsNearlyEqual(double(Boom->TargetArmLength),Length,.0001)||!FMath::IsNearlyEqual(double(Boom->CameraLagSpeed),Lag,.0001)||!FMath::IsNearlyEqual(double(Boom->CameraRotationLagSpeed),RotationLag,.0001)||Boom->bEnableCameraLag!=bLag||Boom->bEnableCameraRotationLag!=bRotationLag) return false;
		}
	}
	if (!Defaults->GetFirstPersonCameraComponent()) return false;
	if (bApply) { Defaults->Modify(); Defaults->HipFieldOfView=90; Defaults->AimFieldOfView=70; Defaults->PresentationNearClipPlane=1; Defaults->GetFirstPersonCameraComponent()->SetFieldOfView(90); }
	if (Defaults->HipFieldOfView!=90 || Defaults->AimFieldOfView!=70 || Defaults->PresentationNearClipPlane!=1 || !Defaults->GetFirstPersonCameraComponent() || Defaults->GetFirstPersonCameraComponent()->FieldOfView!=90) return false;
	auto* BodyMesh=Defaults->GetMesh(); auto* ViewMesh=Defaults->GetMesh1P();
	return BodyMesh && ViewMesh && BodyMesh->GetSkeletalMeshAsset() && ViewMesh->GetSkeletalMeshAsset() && BodyMesh->GetSkeletalMeshAsset()->GetSkeleton()==Body->TargetSkeleton && ViewMesh->GetSkeletalMeshAsset()->GetSkeleton()==Arms->TargetSkeleton
		&& BodyMesh->GetAnimClass()==Body->GeneratedClass.Get() && ViewMesh->GetAnimClass()==Arms->GeneratedClass.Get() && CameraValid(Blueprint) && InitializationValid(Blueprint,bRAR);
}

bool Compile(UBlueprint* Blueprint)
{ FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FCompilerResultsLog Results; FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave|EBlueprintCompileOptions::SkipGarbageCollection,&Results); UE_LOG(LogTemp,Display,TEXT("PLAYER_PRESENTATION COMPILE Asset=%s Errors=%d Warnings=%d"),*Blueprint->GetPathName(),Results.NumErrors,Results.NumWarnings); return Results.NumErrors==0 && Blueprint->Status!=BS_Error; }
void Run(const TArray<FString>& Args)
{
	const bool bApply=Args.Contains(TEXT("Apply")); FString Path,Text; TSharedPtr<FJsonObject> Root; const FString Joined=FString::Join(Args,TEXT(" "));
	if (!FParse::Value(*Joined,TEXT("Snapshot="),Path)||FPaths::IsRelative(Path)||!FFileHelper::LoadFileToString(Text,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root)||!Root.IsValid()) { Fail(TEXT("Requires an absolute readable Snapshot=<preflight JSON>")); return; }
	const TArray<TSharedPtr<FJsonValue>>* Assets=nullptr; double Schema,Saved;
	if (!Root->TryGetNumberField(TEXT("schema"),Schema)||Schema!=1||!Root->TryGetNumberField(TEXT("savedPackages"),Saved)||Saved!=0||!Root->TryGetArrayField(TEXT("assets"),Assets)||Assets->Num()!=6) { Fail(TEXT("Unexpected snapshot schema or asset count")); return; }
	TArray<TSharedPtr<FJsonObject>> Snapshots; TArray<TStrongObjectPtr<UBlueprint>> Blueprints; TSet<FString> Seen;
	for (int32 Index=0;Index<6;++Index)
	{
		TSharedPtr<FJsonObject> Entry; for (const auto& Candidate:*Assets) { const auto Object=Candidate->AsObject(); FString Package; if (Object && Object->TryGetStringField(TEXT("path"),Package)&&Package==Packages[Index]) { if (Entry) { Fail(TEXT("Duplicate snapshot asset")); return; } Entry=Object; } }
		FString Class; if (!Entry || !Entry->TryGetStringField(TEXT("class"),Class)) { Fail(TEXT("Snapshot whitelist mismatch")); return; }
		const FString ObjectPath=FString(Packages[Index])+TEXT(".")+FPackageName::GetLongPackageAssetName(Packages[Index]);
		auto* Blueprint=LoadObject<UBlueprint>(nullptr,*ObjectPath); if (!Blueprint||!Blueprint->GeneratedClass||Blueprint->GeneratedClass->GetPathName()!=Class||Blueprint->ParentClass!=(Index<4?UfpstruePlayerAnimInstance::StaticClass():AfpstrueCharacter::StaticClass())) { Fail(FString(Packages[Index])+TEXT(" unexpected class or parent")); return; }
		Snapshots.Add(Entry); Blueprints.Emplace(Blueprint);
	}
	const auto* Native=GetDefault<UfpstruePlayerAnimInstance>();
	for (const TCHAR* Name:ResourceFields) { const auto* Field=FindFProperty<FObjectPropertyBase>(UfpstruePlayerAnimInstance::StaticClass(),Name); if (!Field || Field->GetObjectPropertyValue_InContainer(Native)) { Fail(TEXT("Native base resource schema mismatch or nonempty animation resource")); return; } }
	if (Native->ViewmodelFOVCollection) { Fail(TEXT("Native base still holds projection collection")); return; }
	const FString Backup=FPaths::ProjectSavedDir()/TEXT("Backups/PlayerPresentation")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (bApply)
	{
		if (!IFileManager::Get().MakeDirectory(*Backup,true)) { Fail(TEXT("Cannot create backup")); return; }
		for (int32 Index=0;Index<6;++Index) { const FString File=FPackageName::LongPackageNameToFilename(Packages[Index],FPackageName::GetAssetPackageExtension()); if (IFileManager::Get().Copy(*(Backup/FPaths::GetCleanFilename(File)),*File)!=COPY_OK) { Fail(TEXT("Backup failed; no assets saved")); return; } }
		for (int32 Index=0;Index<6;++Index)
		{
			auto* Blueprint=Blueprints[Index].Get(); Blueprint->Modify();
			if ((Index==1 && !ProjectionGraph(Blueprint)) || (Index>=4 && (!CameraGraph(Blueprint)||!InitializationGraph(Blueprint,Index==4))) || !Compile(Blueprint)) { Fail(TEXT("Graph build/compile failed; no assets saved")); return; }
		}
	}
	for (int32 Index=0;Index<6;++Index)
	{
		auto* Blueprint=Blueprints[Index].Get();
		const bool bValid=Index<4 ? AnimDefaults(CastChecked<UAnimBlueprint>(Blueprint),Snapshots[Index],bApply,Index==1)
			: CharacterDefaults(Blueprint,Snapshots[Index],bApply,Index==4,CastChecked<UAnimBlueprint>(Blueprints[Index==4?0:2].Get()),CastChecked<UAnimBlueprint>(Blueprints[Index==4?1:3].Get()));
		if (!bValid) { Fail(Blueprint->GetPathName()+TEXT(" resource, component snapshot or presentation graph mismatch; no assets saved")); return; }
		UE_LOG(LogTemp,Display,TEXT("PLAYER_PRESENTATION CONTRACT_PASS Asset=%s SnapshotExact=1 GraphAndFunctionOwner=1"),*Blueprint->GetPathName());
	}
	if (bApply) { TArray<UPackage*> Save; for (const auto& Blueprint:Blueprints) { Blueprint->MarkPackageDirty(); Save.Add(Blueprint->GetOutermost()); } if (!UEditorLoadingAndSavingUtils::SavePackages(Save,false)) { Fail(TEXT("Whitelist save failed; backups preserved")); return; } }
	UE_LOG(LogTemp,Display,TEXT("PLAYER_PRESENTATION PASS Mode=%s PackagesSaved=%d Snapshot=%s Backup=%s VisualPreview=NOT_RUN"),bApply?TEXT("Apply"):TEXT("Check"),bApply?6:0,*Path,bApply?*Backup:TEXT("None"));
}
FAutoConsoleCommand Command(TEXT("fp.PreparePlayerPresentation"),TEXT("Check player presentation against Snapshot=<absolute preflight JSON>. Apply backs up and saves exactly six project assets."),FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
