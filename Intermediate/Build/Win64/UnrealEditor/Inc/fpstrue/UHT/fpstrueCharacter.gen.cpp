// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "fpstrue/fpstrueCharacter.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodefpstrueCharacter() {}

// Begin Cross Module References
ENGINE_API UClass* Z_Construct_UClass_ACharacter();
ENGINE_API UClass* Z_Construct_UClass_UCameraComponent_NoRegister();
ENGINE_API UClass* Z_Construct_UClass_USkeletalMeshComponent_NoRegister();
ENGINE_API UClass* Z_Construct_UClass_USpringArmComponent_NoRegister();
ENHANCEDINPUT_API UClass* Z_Construct_UClass_UInputAction_NoRegister();
ENHANCEDINPUT_API UClass* Z_Construct_UClass_UInputMappingContext_NoRegister();
FPSTRUE_API UClass* Z_Construct_UClass_AfpstrueCharacter();
FPSTRUE_API UClass* Z_Construct_UClass_AfpstrueCharacter_NoRegister();
FPSTRUE_API UClass* Z_Construct_UClass_UfpstrueHealthComponent_NoRegister();
FPSTRUE_API UEnum* Z_Construct_UEnum_fpstrue_EFPCharacterState();
UPackage* Z_Construct_UPackage__Script_fpstrue();
// End Cross Module References

// Begin Enum EFPCharacterState
static FEnumRegistrationInfo Z_Registration_Info_UEnum_EFPCharacterState;
static UEnum* EFPCharacterState_StaticEnum()
{
	if (!Z_Registration_Info_UEnum_EFPCharacterState.OuterSingleton)
	{
		Z_Registration_Info_UEnum_EFPCharacterState.OuterSingleton = GetStaticEnum(Z_Construct_UEnum_fpstrue_EFPCharacterState, (UObject*)Z_Construct_UPackage__Script_fpstrue(), TEXT("EFPCharacterState"));
	}
	return Z_Registration_Info_UEnum_EFPCharacterState.OuterSingleton;
}
template<> FPSTRUE_API UEnum* StaticEnum<EFPCharacterState>()
{
	return EFPCharacterState_StaticEnum();
}
struct Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Enum_MetaDataParams[] = {
		{ "BlueprintType", "true" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "// ----------------------\n// Character states\n// ----------------------\n//\xef\xbf\xbd\xef\xbf\xbd\xcd\xbc\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\n" },
#endif
		{ "Dead.DisplayName", "Dead" },
		{ "Dead.Name", "EFPCharacterState::Dead" },
		{ "Idle.DisplayName", "Idle" },
		{ "Idle.Name", "EFPCharacterState::Idle" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
		{ "Moving.DisplayName", "Moving" },
		{ "Moving.Name", "EFPCharacterState::Moving" },
		{ "Reloading.DisplayName", "Reloading" },
		{ "Reloading.Name", "EFPCharacterState::Reloading" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Character states\n----------------------\n\xef\xbf\xbd\xef\xbf\xbd\xcd\xbc\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" },
#endif
	};
#endif // WITH_METADATA
	static constexpr UECodeGen_Private::FEnumeratorParam Enumerators[] = {
		{ "EFPCharacterState::Idle", (int64)EFPCharacterState::Idle },
		{ "EFPCharacterState::Moving", (int64)EFPCharacterState::Moving },
		{ "EFPCharacterState::Reloading", (int64)EFPCharacterState::Reloading },
		{ "EFPCharacterState::Dead", (int64)EFPCharacterState::Dead },
	};
	static const UECodeGen_Private::FEnumParams EnumParams;
};
const UECodeGen_Private::FEnumParams Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics::EnumParams = {
	(UObject*(*)())Z_Construct_UPackage__Script_fpstrue,
	nullptr,
	"EFPCharacterState",
	"EFPCharacterState",
	Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics::Enumerators,
	RF_Public|RF_Transient|RF_MarkAsNative,
	UE_ARRAY_COUNT(Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics::Enumerators),
	EEnumFlags::None,
	(uint8)UEnum::ECppForm::EnumClass,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics::Enum_MetaDataParams), Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics::Enum_MetaDataParams)
};
UEnum* Z_Construct_UEnum_fpstrue_EFPCharacterState()
{
	if (!Z_Registration_Info_UEnum_EFPCharacterState.InnerSingleton)
	{
		UECodeGen_Private::ConstructUEnum(Z_Registration_Info_UEnum_EFPCharacterState.InnerSingleton, Z_Construct_UEnum_fpstrue_EFPCharacterState_Statics::EnumParams);
	}
	return Z_Registration_Info_UEnum_EFPCharacterState.InnerSingleton;
}
// End Enum EFPCharacterState

// Begin Class AfpstrueCharacter Function HandleDeath
struct Z_Construct_UFunction_AfpstrueCharacter_HandleDeath_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_AfpstrueCharacter_HandleDeath_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_AfpstrueCharacter, nullptr, "HandleDeath", nullptr, nullptr, nullptr, 0, 0, RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x00080401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_AfpstrueCharacter_HandleDeath_Statics::Function_MetaDataParams), Z_Construct_UFunction_AfpstrueCharacter_HandleDeath_Statics::Function_MetaDataParams) };
UFunction* Z_Construct_UFunction_AfpstrueCharacter_HandleDeath()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_AfpstrueCharacter_HandleDeath_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(AfpstrueCharacter::execHandleDeath)
{
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->HandleDeath();
	P_NATIVE_END;
}
// End Class AfpstrueCharacter Function HandleDeath

// Begin Class AfpstrueCharacter Function HandleHealthChanged
struct Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics
{
	struct fpstrueCharacter_eventHandleHealthChanged_Parms
	{
		float NewHealth;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FFloatPropertyParams NewProp_NewHealth;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::NewProp_NewHealth = { "NewHealth", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(fpstrueCharacter_eventHandleHealthChanged_Parms, NewHealth), METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::NewProp_NewHealth,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_AfpstrueCharacter, nullptr, "HandleHealthChanged", nullptr, nullptr, Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::PropPointers), sizeof(Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::fpstrueCharacter_eventHandleHealthChanged_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x00080401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::Function_MetaDataParams), Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::fpstrueCharacter_eventHandleHealthChanged_Parms) < MAX_uint16);
UFunction* Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(AfpstrueCharacter::execHandleHealthChanged)
{
	P_GET_PROPERTY(FFloatProperty,Z_Param_NewHealth);
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->HandleHealthChanged(Z_Param_NewHealth);
	P_NATIVE_END;
}
// End Class AfpstrueCharacter Function HandleHealthChanged

// Begin Class AfpstrueCharacter
void AfpstrueCharacter::StaticRegisterNativesAfpstrueCharacter()
{
	UClass* Class = AfpstrueCharacter::StaticClass();
	static const FNameNativePtrPair Funcs[] = {
		{ "HandleDeath", &AfpstrueCharacter::execHandleDeath },
		{ "HandleHealthChanged", &AfpstrueCharacter::execHandleHealthChanged },
	};
	FNativeFunctionRegistrar::RegisterFunctions(Class, Funcs, UE_ARRAY_COUNT(Funcs));
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(AfpstrueCharacter);
UClass* Z_Construct_UClass_AfpstrueCharacter_NoRegister()
{
	return AfpstrueCharacter::StaticClass();
}
struct Z_Construct_UClass_AfpstrueCharacter_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
#if !UE_BUILD_SHIPPING
		{ "Comment", "// ----------------------\n// AfpstrueCharacter \xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\n// ----------------------\n" },
#endif
		{ "HideCategories", "Navigation" },
		{ "IncludePath", "fpstrueCharacter.h" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "AfpstrueCharacter \xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Mesh1P_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Mesh" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Pawn mesh: 1st person view (arms; seen only by self) */" },
#endif
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Pawn mesh: 1st person view (arms; seen only by self)" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_CameraBoom_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Camera" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Camera boom used for first person camera lag */" },
#endif
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Camera boom used for first person camera lag" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FirstPersonCameraComponent_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Camera" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** First person camera */" },
#endif
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "First person camera" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_DefaultMappingContext_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** MappingContext */" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "MappingContext" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_JumpAction_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Jump Input Action */" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Jump Input Action" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_MoveAction_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Move Input Action */" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Move Input Action" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_LookAction_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Look Input Action */" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Look Input Action" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ReloadAction_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Reload Input Action */" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Reload Input Action" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_RunAction_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Run Input Action */" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Run Input Action" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_HealthComponent_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Health" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Player health */" },
#endif
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Player health" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_MagazineSize_MetaData[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "//\xef\xbf\xbd\xef\xbf\xbd\xd2\xa9\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xd0\xba\xef\xbf\xbd\xd7\xb4\xcc\xac\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\n" },
#endif
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "\xef\xbf\xbd\xef\xbf\xbd\xd2\xa9\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xd0\xba\xef\xbf\xbd\xd7\xb4\xcc\xac\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_CurrentAmmo_MetaData[] = {
		{ "Category", "Weapon" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ReserveAmmo_MetaData[] = {
		{ "Category", "Weapon" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ReloadDuration_MetaData[] = {
		{ "Category", "Weapon" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_CharacterState_MetaData[] = {
		{ "Category", "State" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_WalkSpeed_MetaData[] = {
		{ "Category", "Movement" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_SprintSpeed_MetaData[] = {
		{ "Category", "Movement" },
		{ "ModuleRelativePath", "fpstrueCharacter.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FObjectPropertyParams NewProp_Mesh1P;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_CameraBoom;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_FirstPersonCameraComponent;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_DefaultMappingContext;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_JumpAction;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_MoveAction;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_LookAction;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_ReloadAction;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_RunAction;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_HealthComponent;
	static const UECodeGen_Private::FIntPropertyParams NewProp_MagazineSize;
	static const UECodeGen_Private::FIntPropertyParams NewProp_CurrentAmmo;
	static const UECodeGen_Private::FIntPropertyParams NewProp_ReserveAmmo;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_ReloadDuration;
	static const UECodeGen_Private::FBytePropertyParams NewProp_CharacterState_Underlying;
	static const UECodeGen_Private::FEnumPropertyParams NewProp_CharacterState;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_WalkSpeed;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_SprintSpeed;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static UObject* (*const DependentSingletons[])();
	static constexpr FClassFunctionLinkInfo FuncInfo[] = {
		{ &Z_Construct_UFunction_AfpstrueCharacter_HandleDeath, "HandleDeath" }, // 1553382207
		{ &Z_Construct_UFunction_AfpstrueCharacter_HandleHealthChanged, "HandleHealthChanged" }, // 2613618882
	};
	static_assert(UE_ARRAY_COUNT(FuncInfo) < 2048);
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<AfpstrueCharacter>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_Mesh1P = { "Mesh1P", nullptr, (EPropertyFlags)0x00400000000a001d, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, Mesh1P), Z_Construct_UClass_USkeletalMeshComponent_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Mesh1P_MetaData), NewProp_Mesh1P_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CameraBoom = { "CameraBoom", nullptr, (EPropertyFlags)0x00400000000a001d, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, CameraBoom), Z_Construct_UClass_USpringArmComponent_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_CameraBoom_MetaData), NewProp_CameraBoom_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_FirstPersonCameraComponent = { "FirstPersonCameraComponent", nullptr, (EPropertyFlags)0x00400000000a001d, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, FirstPersonCameraComponent), Z_Construct_UClass_UCameraComponent_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FirstPersonCameraComponent_MetaData), NewProp_FirstPersonCameraComponent_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_DefaultMappingContext = { "DefaultMappingContext", nullptr, (EPropertyFlags)0x0040000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, DefaultMappingContext), Z_Construct_UClass_UInputMappingContext_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_DefaultMappingContext_MetaData), NewProp_DefaultMappingContext_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_JumpAction = { "JumpAction", nullptr, (EPropertyFlags)0x0040000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, JumpAction), Z_Construct_UClass_UInputAction_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_JumpAction_MetaData), NewProp_JumpAction_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_MoveAction = { "MoveAction", nullptr, (EPropertyFlags)0x0040000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, MoveAction), Z_Construct_UClass_UInputAction_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_MoveAction_MetaData), NewProp_MoveAction_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_LookAction = { "LookAction", nullptr, (EPropertyFlags)0x0040000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, LookAction), Z_Construct_UClass_UInputAction_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_LookAction_MetaData), NewProp_LookAction_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_ReloadAction = { "ReloadAction", nullptr, (EPropertyFlags)0x0040000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, ReloadAction), Z_Construct_UClass_UInputAction_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ReloadAction_MetaData), NewProp_ReloadAction_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_RunAction = { "RunAction", nullptr, (EPropertyFlags)0x0040000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, RunAction), Z_Construct_UClass_UInputAction_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_RunAction_MetaData), NewProp_RunAction_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_HealthComponent = { "HealthComponent", nullptr, (EPropertyFlags)0x00400000000a001d, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, HealthComponent), Z_Construct_UClass_UfpstrueHealthComponent_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_HealthComponent_MetaData), NewProp_HealthComponent_MetaData) };
const UECodeGen_Private::FIntPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_MagazineSize = { "MagazineSize", nullptr, (EPropertyFlags)0x0020080000000015, UECodeGen_Private::EPropertyGenFlags::Int, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, MagazineSize), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_MagazineSize_MetaData), NewProp_MagazineSize_MetaData) };
const UECodeGen_Private::FIntPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CurrentAmmo = { "CurrentAmmo", nullptr, (EPropertyFlags)0x0020080000020015, UECodeGen_Private::EPropertyGenFlags::Int, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, CurrentAmmo), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_CurrentAmmo_MetaData), NewProp_CurrentAmmo_MetaData) };
const UECodeGen_Private::FIntPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_ReserveAmmo = { "ReserveAmmo", nullptr, (EPropertyFlags)0x0020080000000015, UECodeGen_Private::EPropertyGenFlags::Int, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, ReserveAmmo), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ReserveAmmo_MetaData), NewProp_ReserveAmmo_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_ReloadDuration = { "ReloadDuration", nullptr, (EPropertyFlags)0x0020080000000015, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, ReloadDuration), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ReloadDuration_MetaData), NewProp_ReloadDuration_MetaData) };
const UECodeGen_Private::FBytePropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CharacterState_Underlying = { "UnderlyingType", nullptr, (EPropertyFlags)0x0000000000000000, UECodeGen_Private::EPropertyGenFlags::Byte, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, 0, nullptr, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FEnumPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CharacterState = { "CharacterState", nullptr, (EPropertyFlags)0x0020080000020015, UECodeGen_Private::EPropertyGenFlags::Enum, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, CharacterState), Z_Construct_UEnum_fpstrue_EFPCharacterState, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_CharacterState_MetaData), NewProp_CharacterState_MetaData) }; // 1862084594
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_WalkSpeed = { "WalkSpeed", nullptr, (EPropertyFlags)0x0020080000000015, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, WalkSpeed), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_WalkSpeed_MetaData), NewProp_WalkSpeed_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_SprintSpeed = { "SprintSpeed", nullptr, (EPropertyFlags)0x0020080000000015, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(AfpstrueCharacter, SprintSpeed), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_SprintSpeed_MetaData), NewProp_SprintSpeed_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UClass_AfpstrueCharacter_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_Mesh1P,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CameraBoom,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_FirstPersonCameraComponent,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_DefaultMappingContext,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_JumpAction,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_MoveAction,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_LookAction,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_ReloadAction,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_RunAction,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_HealthComponent,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_MagazineSize,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CurrentAmmo,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_ReserveAmmo,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_ReloadDuration,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CharacterState_Underlying,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_CharacterState,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_WalkSpeed,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_AfpstrueCharacter_Statics::NewProp_SprintSpeed,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_AfpstrueCharacter_Statics::PropPointers) < 2048);
UObject* (*const Z_Construct_UClass_AfpstrueCharacter_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_ACharacter,
	(UObject* (*)())Z_Construct_UPackage__Script_fpstrue,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_AfpstrueCharacter_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_AfpstrueCharacter_Statics::ClassParams = {
	&AfpstrueCharacter::StaticClass,
	"Game",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	FuncInfo,
	Z_Construct_UClass_AfpstrueCharacter_Statics::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	UE_ARRAY_COUNT(FuncInfo),
	UE_ARRAY_COUNT(Z_Construct_UClass_AfpstrueCharacter_Statics::PropPointers),
	0,
	0x008000A4u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_AfpstrueCharacter_Statics::Class_MetaDataParams), Z_Construct_UClass_AfpstrueCharacter_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_AfpstrueCharacter()
{
	if (!Z_Registration_Info_UClass_AfpstrueCharacter.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_AfpstrueCharacter.OuterSingleton, Z_Construct_UClass_AfpstrueCharacter_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_AfpstrueCharacter.OuterSingleton;
}
template<> FPSTRUE_API UClass* StaticClass<AfpstrueCharacter>()
{
	return AfpstrueCharacter::StaticClass();
}
DEFINE_VTABLE_PTR_HELPER_CTOR(AfpstrueCharacter);
AfpstrueCharacter::~AfpstrueCharacter() {}
// End Class AfpstrueCharacter

// Begin Registration
struct Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_Statics
{
	static constexpr FEnumRegisterCompiledInInfo EnumInfo[] = {
		{ EFPCharacterState_StaticEnum, TEXT("EFPCharacterState"), &Z_Registration_Info_UEnum_EFPCharacterState, CONSTRUCT_RELOAD_VERSION_INFO(FEnumReloadVersionInfo, 1862084594U) },
	};
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_AfpstrueCharacter, AfpstrueCharacter::StaticClass, TEXT("AfpstrueCharacter"), &Z_Registration_Info_UClass_AfpstrueCharacter, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(AfpstrueCharacter), 1451675894U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_169596721(TEXT("/Script/fpstrue"),
	Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_Statics::ClassInfo),
	nullptr, 0,
	Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_Statics::EnumInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_Statics::EnumInfo));
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
