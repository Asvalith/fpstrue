// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "fpstrue/fpstrueWeaponComponent.h"
#include "Runtime/Engine/Classes/Engine/HitResult.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodefpstrueWeaponComponent() {}

// Begin Cross Module References
COREUOBJECT_API UClass* Z_Construct_UClass_UClass();
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FVector();
ENGINE_API UClass* Z_Construct_UClass_UAnimMontage_NoRegister();
ENGINE_API UClass* Z_Construct_UClass_USkeletalMeshComponent();
ENGINE_API UClass* Z_Construct_UClass_USoundBase_NoRegister();
ENGINE_API UEnum* Z_Construct_UEnum_Engine_EEndPlayReason();
ENGINE_API UScriptStruct* Z_Construct_UScriptStruct_FHitResult();
ENHANCEDINPUT_API UClass* Z_Construct_UClass_UInputAction_NoRegister();
ENHANCEDINPUT_API UClass* Z_Construct_UClass_UInputMappingContext_NoRegister();
FPSTRUE_API UClass* Z_Construct_UClass_AfpstrueCharacter_NoRegister();
FPSTRUE_API UClass* Z_Construct_UClass_AfpstrueProjectile_NoRegister();
FPSTRUE_API UClass* Z_Construct_UClass_UfpstrueWeaponComponent();
FPSTRUE_API UClass* Z_Construct_UClass_UfpstrueWeaponComponent_NoRegister();
FPSTRUE_API UFunction* Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature();
FPSTRUE_API UFunction* Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature();
FPSTRUE_API UFunction* Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature();
UPackage* Z_Construct_UPackage__Script_fpstrue();
// End Cross Module References

// Begin Delegate FWeaponFireEvent
struct Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FFunctionParams Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature_Statics::FuncParams = { (UObject*(*)())Z_Construct_UPackage__Script_fpstrue, nullptr, "WeaponFireEvent__DelegateSignature", nullptr, nullptr, nullptr, 0, 0, RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x00130000, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature_Statics::Function_MetaDataParams), Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature_Statics::Function_MetaDataParams) };
UFunction* Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature_Statics::FuncParams);
	}
	return ReturnFunction;
}
void FWeaponFireEvent_DelegateWrapper(const FMulticastScriptDelegate& WeaponFireEvent)
{
	WeaponFireEvent.ProcessMulticastDelegate<UObject>(NULL);
}
// End Delegate FWeaponFireEvent

// Begin Delegate FWeaponReloadEvent
struct Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics
{
	struct _Script_fpstrue_eventWeaponReloadEvent_Parms
	{
		bool bWasEmptyReload;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
#endif // WITH_METADATA
	static void NewProp_bWasEmptyReload_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bWasEmptyReload;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
void Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::NewProp_bWasEmptyReload_SetBit(void* Obj)
{
	((_Script_fpstrue_eventWeaponReloadEvent_Parms*)Obj)->bWasEmptyReload = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::NewProp_bWasEmptyReload = { "bWasEmptyReload", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(_Script_fpstrue_eventWeaponReloadEvent_Parms), &Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::NewProp_bWasEmptyReload_SetBit, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::NewProp_bWasEmptyReload,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::FuncParams = { (UObject*(*)())Z_Construct_UPackage__Script_fpstrue, nullptr, "WeaponReloadEvent__DelegateSignature", nullptr, nullptr, Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::PropPointers), sizeof(Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::_Script_fpstrue_eventWeaponReloadEvent_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x00130000, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::Function_MetaDataParams), Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::_Script_fpstrue_eventWeaponReloadEvent_Parms) < MAX_uint16);
UFunction* Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature_Statics::FuncParams);
	}
	return ReturnFunction;
}
void FWeaponReloadEvent_DelegateWrapper(const FMulticastScriptDelegate& WeaponReloadEvent, bool bWasEmptyReload)
{
	struct _Script_fpstrue_eventWeaponReloadEvent_Parms
	{
		bool bWasEmptyReload;
	};
	_Script_fpstrue_eventWeaponReloadEvent_Parms Parms;
	Parms.bWasEmptyReload=bWasEmptyReload ? true : false;
	WeaponReloadEvent.ProcessMulticastDelegate<UObject>(&Parms);
}
// End Delegate FWeaponReloadEvent

// Begin Delegate FWeaponTraceEvent
struct Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics
{
	struct _Script_fpstrue_eventWeaponTraceEvent_Parms
	{
		bool bHit;
		FVector TraceStart;
		FVector TraceEnd;
		FVector TraceTarget;
		FHitResult HitResult;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
#endif // WITH_METADATA
	static void NewProp_bHit_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bHit;
	static const UECodeGen_Private::FStructPropertyParams NewProp_TraceStart;
	static const UECodeGen_Private::FStructPropertyParams NewProp_TraceEnd;
	static const UECodeGen_Private::FStructPropertyParams NewProp_TraceTarget;
	static const UECodeGen_Private::FStructPropertyParams NewProp_HitResult;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
void Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_bHit_SetBit(void* Obj)
{
	((_Script_fpstrue_eventWeaponTraceEvent_Parms*)Obj)->bHit = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_bHit = { "bHit", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(_Script_fpstrue_eventWeaponTraceEvent_Parms), &Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_bHit_SetBit, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_TraceStart = { "TraceStart", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(_Script_fpstrue_eventWeaponTraceEvent_Parms, TraceStart), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_TraceEnd = { "TraceEnd", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(_Script_fpstrue_eventWeaponTraceEvent_Parms, TraceEnd), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_TraceTarget = { "TraceTarget", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(_Script_fpstrue_eventWeaponTraceEvent_Parms, TraceTarget), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_HitResult = { "HitResult", nullptr, (EPropertyFlags)0x0010008000000080, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(_Script_fpstrue_eventWeaponTraceEvent_Parms, HitResult), Z_Construct_UScriptStruct_FHitResult, METADATA_PARAMS(0, nullptr) }; // 4100991306
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_bHit,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_TraceStart,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_TraceEnd,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_TraceTarget,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::NewProp_HitResult,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::FuncParams = { (UObject*(*)())Z_Construct_UPackage__Script_fpstrue, nullptr, "WeaponTraceEvent__DelegateSignature", nullptr, nullptr, Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::PropPointers), sizeof(Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::_Script_fpstrue_eventWeaponTraceEvent_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x00130000, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::Function_MetaDataParams), Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::_Script_fpstrue_eventWeaponTraceEvent_Parms) < MAX_uint16);
UFunction* Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature_Statics::FuncParams);
	}
	return ReturnFunction;
}
void FWeaponTraceEvent_DelegateWrapper(const FMulticastScriptDelegate& WeaponTraceEvent, bool bHit, FVector TraceStart, FVector TraceEnd, FVector TraceTarget, FHitResult HitResult)
{
	struct _Script_fpstrue_eventWeaponTraceEvent_Parms
	{
		bool bHit;
		FVector TraceStart;
		FVector TraceEnd;
		FVector TraceTarget;
		FHitResult HitResult;
	};
	_Script_fpstrue_eventWeaponTraceEvent_Parms Parms;
	Parms.bHit=bHit ? true : false;
	Parms.TraceStart=TraceStart;
	Parms.TraceEnd=TraceEnd;
	Parms.TraceTarget=TraceTarget;
	Parms.HitResult=HitResult;
	WeaponTraceEvent.ProcessMulticastDelegate<UObject>(&Parms);
}
// End Delegate FWeaponTraceEvent

// Begin Class UfpstrueWeaponComponent Function AttachWeapon
struct Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics
{
	struct fpstrueWeaponComponent_eventAttachWeapon_Parms
	{
		AfpstrueCharacter* TargetCharacter;
		bool ReturnValue;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Attaches the actor to a FirstPersonCharacter */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Attaches the actor to a FirstPersonCharacter" },
#endif
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FObjectPropertyParams NewProp_TargetCharacter;
	static void NewProp_ReturnValue_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_ReturnValue;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::NewProp_TargetCharacter = { "TargetCharacter", nullptr, (EPropertyFlags)0x0010000000000080, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(fpstrueWeaponComponent_eventAttachWeapon_Parms, TargetCharacter), Z_Construct_UClass_AfpstrueCharacter_NoRegister, METADATA_PARAMS(0, nullptr) };
void Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::NewProp_ReturnValue_SetBit(void* Obj)
{
	((fpstrueWeaponComponent_eventAttachWeapon_Parms*)Obj)->ReturnValue = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::NewProp_ReturnValue = { "ReturnValue", nullptr, (EPropertyFlags)0x0010000000000580, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(fpstrueWeaponComponent_eventAttachWeapon_Parms), &Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::NewProp_ReturnValue_SetBit, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::NewProp_TargetCharacter,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::NewProp_ReturnValue,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_UfpstrueWeaponComponent, nullptr, "AttachWeapon", nullptr, nullptr, Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::PropPointers), sizeof(Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::fpstrueWeaponComponent_eventAttachWeapon_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x04020401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::Function_MetaDataParams), Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::fpstrueWeaponComponent_eventAttachWeapon_Parms) < MAX_uint16);
UFunction* Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(UfpstrueWeaponComponent::execAttachWeapon)
{
	P_GET_OBJECT(AfpstrueCharacter,Z_Param_TargetCharacter);
	P_FINISH;
	P_NATIVE_BEGIN;
	*(bool*)Z_Param__Result=P_THIS->AttachWeapon(Z_Param_TargetCharacter);
	P_NATIVE_END;
}
// End Class UfpstrueWeaponComponent Function AttachWeapon

// Begin Class UfpstrueWeaponComponent Function EndPlay
struct Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics
{
	struct fpstrueWeaponComponent_eventEndPlay_Parms
	{
		TEnumAsByte<EEndPlayReason::Type> EndPlayReason;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Ends gameplay for this component. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Ends gameplay for this component." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndPlayReason_MetaData[] = {
		{ "NativeConst", "" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FBytePropertyParams NewProp_EndPlayReason;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FBytePropertyParams Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::NewProp_EndPlayReason = { "EndPlayReason", nullptr, (EPropertyFlags)0x0010000000000082, UECodeGen_Private::EPropertyGenFlags::Byte, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(fpstrueWeaponComponent_eventEndPlay_Parms, EndPlayReason), Z_Construct_UEnum_Engine_EEndPlayReason, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndPlayReason_MetaData), NewProp_EndPlayReason_MetaData) }; // 2448981352
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::NewProp_EndPlayReason,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::PropPointers) < 2048);
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_UfpstrueWeaponComponent, nullptr, "EndPlay", nullptr, nullptr, Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::PropPointers, UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::PropPointers), sizeof(Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::fpstrueWeaponComponent_eventEndPlay_Parms), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x00080400, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::Function_MetaDataParams), Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::Function_MetaDataParams) };
static_assert(sizeof(Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::fpstrueWeaponComponent_eventEndPlay_Parms) < MAX_uint16);
UFunction* Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(UfpstrueWeaponComponent::execEndPlay)
{
	P_GET_PROPERTY(FByteProperty,Z_Param_EndPlayReason);
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->EndPlay(EEndPlayReason::Type(Z_Param_EndPlayReason));
	P_NATIVE_END;
}
// End Class UfpstrueWeaponComponent Function EndPlay

// Begin Class UfpstrueWeaponComponent Function Fire
struct Z_Construct_UFunction_UfpstrueWeaponComponent_Fire_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Function_MetaDataParams[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Make the weapon Fire a Projectile */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Make the weapon Fire a Projectile" },
#endif
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FFunctionParams Z_Construct_UFunction_UfpstrueWeaponComponent_Fire_Statics::FuncParams = { (UObject*(*)())Z_Construct_UClass_UfpstrueWeaponComponent, nullptr, "Fire", nullptr, nullptr, nullptr, 0, 0, RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x04020401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UFunction_UfpstrueWeaponComponent_Fire_Statics::Function_MetaDataParams), Z_Construct_UFunction_UfpstrueWeaponComponent_Fire_Statics::Function_MetaDataParams) };
UFunction* Z_Construct_UFunction_UfpstrueWeaponComponent_Fire()
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, Z_Construct_UFunction_UfpstrueWeaponComponent_Fire_Statics::FuncParams);
	}
	return ReturnFunction;
}
DEFINE_FUNCTION(UfpstrueWeaponComponent::execFire)
{
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->Fire();
	P_NATIVE_END;
}
// End Class UfpstrueWeaponComponent Function Fire

// Begin Class UfpstrueWeaponComponent
void UfpstrueWeaponComponent::StaticRegisterNativesUfpstrueWeaponComponent()
{
	UClass* Class = UfpstrueWeaponComponent::StaticClass();
	static const FNameNativePtrPair Funcs[] = {
		{ "AttachWeapon", &UfpstrueWeaponComponent::execAttachWeapon },
		{ "EndPlay", &UfpstrueWeaponComponent::execEndPlay },
		{ "Fire", &UfpstrueWeaponComponent::execFire },
	};
	FNativeFunctionRegistrar::RegisterFunctions(Class, Funcs, UE_ARRAY_COUNT(Funcs));
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(UfpstrueWeaponComponent);
UClass* Z_Construct_UClass_UfpstrueWeaponComponent_NoRegister()
{
	return UfpstrueWeaponComponent::StaticClass();
}
struct Z_Construct_UClass_UfpstrueWeaponComponent_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "BlueprintSpawnableComponent", "" },
		{ "BlueprintType", "true" },
		{ "ClassGroupNames", "Custom" },
		{ "HideCategories", "Object Mesh|SkeletalAsset Object Mobility Trigger" },
		{ "IncludePath", "fpstrueWeaponComponent.h" },
		{ "IsBlueprintBase", "true" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_ProjectileClass_MetaData[] = {
		{ "Category", "Projectile" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Projectile class to spawn */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Projectile class to spawn" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FireSound_MetaData[] = {
		{ "Category", "Gameplay" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Sound to play each time we fire */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Sound to play each time we fire" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FireAnimation_MetaData[] = {
		{ "Category", "Gameplay" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** AnimMontage to play each time we fire */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "AnimMontage to play each time we fire" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_MuzzleOffset_MetaData[] = {
		{ "Category", "Gameplay" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Gun muzzle's offset from the characters location */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Gun muzzle's offset from the characters location" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FireMappingContext_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** MappingContext */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "MappingContext" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_FireAction_MetaData[] = {
		{ "AllowPrivateAccess", "true" },
		{ "Category", "Input" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Fire Input Action */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Fire Input Action" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bUseLineTrace_MetaData[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Use LineTrace as the main fire mode. Projectile remains as a fallback. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Use LineTrace as the main fire mode. Projectile remains as a fallback." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_LineTraceRange_MetaData[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Linetrace length*/" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Linetrace length" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_LineTraceImpulse_MetaData[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Impulse applied to physics objects hit by LineTrace */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Impulse applied to physics objects hit by LineTrace" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_LineTraceDamage_MetaData[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Damage applied to enemy body hits. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Damage applied to enemy body hits." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_LineTraceHeadDamage_MetaData[] = {
		{ "Category", "Weapon" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Damage applied when LineTrace hits an enemy head bone. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Damage applied when LineTrace hits an enemy head bone." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_HipFireSpreadAngle_MetaData[] = {
		{ "Category", "Weapon|Spread" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Random bullet spread when hip firing, in degrees. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Random bullet spread when hip firing, in degrees." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_AimFireSpreadAngle_MetaData[] = {
		{ "Category", "Weapon|Spread" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Random bullet spread while aiming, in degrees. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Random bullet spread while aiming, in degrees." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_RecoilPitch_MetaData[] = {
		{ "Category", "Weapon|Recoil" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Upward camera kick applied after each shot. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Upward camera kick applied after each shot." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_RecoilYaw_MetaData[] = {
		{ "Category", "Weapon|Recoil" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Random left/right camera kick applied after each shot. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Random left/right camera kick applied after each shot." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_AimRecoilMultiplier_MetaData[] = {
		{ "Category", "Weapon|Recoil" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Multiplier applied to recoil while aiming. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Multiplier applied to recoil while aiming." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bShowDebugTrace_MetaData[] = {
		{ "Category", "Weapon|Debug" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/** Draw LineTrace debug lines and hit messages. Disabled for normal gameplay. */" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Draw LineTrace debug lines and hit messages. Disabled for normal gameplay." },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponFireStarted_MetaData[] = {
		{ "Category", "Weapon|Events" },
#if !UE_BUILD_SHIPPING
		{ "Comment", "/*Add fire VFX interface*/" },
#endif
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
#if !UE_BUILD_SHIPPING
		{ "ToolTip", "Add fire VFX interface" },
#endif
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponFireStopped_MetaData[] = {
		{ "Category", "Weapon|Events" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponFirePerformed_MetaData[] = {
		{ "Category", "Weapon|Events" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponDryFire_MetaData[] = {
		{ "Category", "Weapon|Events" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponReloadStarted_MetaData[] = {
		{ "Category", "Weapon|Events" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponReloadFinished_MetaData[] = {
		{ "Category", "Weapon|Events" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_OnWeaponTraceFinished_MetaData[] = {
		{ "Category", "Weapon|Events" },
		{ "ModuleRelativePath", "fpstrueWeaponComponent.h" },
	};
#endif // WITH_METADATA
	static const UECodeGen_Private::FClassPropertyParams NewProp_ProjectileClass;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_FireSound;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_FireAnimation;
	static const UECodeGen_Private::FStructPropertyParams NewProp_MuzzleOffset;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_FireMappingContext;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_FireAction;
	static void NewProp_bUseLineTrace_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bUseLineTrace;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_LineTraceRange;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_LineTraceImpulse;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_LineTraceDamage;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_LineTraceHeadDamage;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_HipFireSpreadAngle;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_AimFireSpreadAngle;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_RecoilPitch;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_RecoilYaw;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_AimRecoilMultiplier;
	static void NewProp_bShowDebugTrace_SetBit(void* Obj);
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bShowDebugTrace;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponFireStarted;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponFireStopped;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponFirePerformed;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponDryFire;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponReloadStarted;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponReloadFinished;
	static const UECodeGen_Private::FMulticastDelegatePropertyParams NewProp_OnWeaponTraceFinished;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
	static UObject* (*const DependentSingletons[])();
	static constexpr FClassFunctionLinkInfo FuncInfo[] = {
		{ &Z_Construct_UFunction_UfpstrueWeaponComponent_AttachWeapon, "AttachWeapon" }, // 149670482
		{ &Z_Construct_UFunction_UfpstrueWeaponComponent_EndPlay, "EndPlay" }, // 2829093116
		{ &Z_Construct_UFunction_UfpstrueWeaponComponent_Fire, "Fire" }, // 349404746
	};
	static_assert(UE_ARRAY_COUNT(FuncInfo) < 2048);
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<UfpstrueWeaponComponent>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
const UECodeGen_Private::FClassPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_ProjectileClass = { "ProjectileClass", nullptr, (EPropertyFlags)0x0014000000010001, UECodeGen_Private::EPropertyGenFlags::Class, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, ProjectileClass), Z_Construct_UClass_UClass, Z_Construct_UClass_AfpstrueProjectile_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_ProjectileClass_MetaData), NewProp_ProjectileClass_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireSound = { "FireSound", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, FireSound), Z_Construct_UClass_USoundBase_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FireSound_MetaData), NewProp_FireSound_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireAnimation = { "FireAnimation", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, FireAnimation), Z_Construct_UClass_UAnimMontage_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FireAnimation_MetaData), NewProp_FireAnimation_MetaData) };
const UECodeGen_Private::FStructPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_MuzzleOffset = { "MuzzleOffset", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, MuzzleOffset), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_MuzzleOffset_MetaData), NewProp_MuzzleOffset_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireMappingContext = { "FireMappingContext", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, FireMappingContext), Z_Construct_UClass_UInputMappingContext_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FireMappingContext_MetaData), NewProp_FireMappingContext_MetaData) };
const UECodeGen_Private::FObjectPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireAction = { "FireAction", nullptr, (EPropertyFlags)0x0010000000000015, UECodeGen_Private::EPropertyGenFlags::Object, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, FireAction), Z_Construct_UClass_UInputAction_NoRegister, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_FireAction_MetaData), NewProp_FireAction_MetaData) };
void Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bUseLineTrace_SetBit(void* Obj)
{
	((UfpstrueWeaponComponent*)Obj)->bUseLineTrace = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bUseLineTrace = { "bUseLineTrace", nullptr, (EPropertyFlags)0x0010000000000001, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(UfpstrueWeaponComponent), &Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bUseLineTrace_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bUseLineTrace_MetaData), NewProp_bUseLineTrace_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceRange = { "LineTraceRange", nullptr, (EPropertyFlags)0x0010000000000001, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, LineTraceRange), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_LineTraceRange_MetaData), NewProp_LineTraceRange_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceImpulse = { "LineTraceImpulse", nullptr, (EPropertyFlags)0x0010000000000001, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, LineTraceImpulse), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_LineTraceImpulse_MetaData), NewProp_LineTraceImpulse_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceDamage = { "LineTraceDamage", nullptr, (EPropertyFlags)0x0010000000000001, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, LineTraceDamage), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_LineTraceDamage_MetaData), NewProp_LineTraceDamage_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceHeadDamage = { "LineTraceHeadDamage", nullptr, (EPropertyFlags)0x0010000000000001, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, LineTraceHeadDamage), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_LineTraceHeadDamage_MetaData), NewProp_LineTraceHeadDamage_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_HipFireSpreadAngle = { "HipFireSpreadAngle", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, HipFireSpreadAngle), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_HipFireSpreadAngle_MetaData), NewProp_HipFireSpreadAngle_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_AimFireSpreadAngle = { "AimFireSpreadAngle", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, AimFireSpreadAngle), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_AimFireSpreadAngle_MetaData), NewProp_AimFireSpreadAngle_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_RecoilPitch = { "RecoilPitch", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, RecoilPitch), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_RecoilPitch_MetaData), NewProp_RecoilPitch_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_RecoilYaw = { "RecoilYaw", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, RecoilYaw), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_RecoilYaw_MetaData), NewProp_RecoilYaw_MetaData) };
const UECodeGen_Private::FFloatPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_AimRecoilMultiplier = { "AimRecoilMultiplier", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, AimRecoilMultiplier), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_AimRecoilMultiplier_MetaData), NewProp_AimRecoilMultiplier_MetaData) };
void Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bShowDebugTrace_SetBit(void* Obj)
{
	((UfpstrueWeaponComponent*)Obj)->bShowDebugTrace = 1;
}
const UECodeGen_Private::FBoolPropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bShowDebugTrace = { "bShowDebugTrace", nullptr, (EPropertyFlags)0x0010000000000001, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, sizeof(bool), sizeof(UfpstrueWeaponComponent), &Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bShowDebugTrace_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bShowDebugTrace_MetaData), NewProp_bShowDebugTrace_MetaData) };
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponFireStarted = { "OnWeaponFireStarted", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponFireStarted), Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponFireStarted_MetaData), NewProp_OnWeaponFireStarted_MetaData) }; // 3613188963
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponFireStopped = { "OnWeaponFireStopped", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponFireStopped), Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponFireStopped_MetaData), NewProp_OnWeaponFireStopped_MetaData) }; // 3613188963
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponFirePerformed = { "OnWeaponFirePerformed", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponFirePerformed), Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponFirePerformed_MetaData), NewProp_OnWeaponFirePerformed_MetaData) }; // 3613188963
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponDryFire = { "OnWeaponDryFire", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponDryFire), Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponDryFire_MetaData), NewProp_OnWeaponDryFire_MetaData) }; // 3613188963
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponReloadStarted = { "OnWeaponReloadStarted", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponReloadStarted), Z_Construct_UDelegateFunction_fpstrue_WeaponReloadEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponReloadStarted_MetaData), NewProp_OnWeaponReloadStarted_MetaData) }; // 3406654283
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponReloadFinished = { "OnWeaponReloadFinished", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponReloadFinished), Z_Construct_UDelegateFunction_fpstrue_WeaponFireEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponReloadFinished_MetaData), NewProp_OnWeaponReloadFinished_MetaData) }; // 3613188963
const UECodeGen_Private::FMulticastDelegatePropertyParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponTraceFinished = { "OnWeaponTraceFinished", nullptr, (EPropertyFlags)0x0010000010080000, UECodeGen_Private::EPropertyGenFlags::InlineMulticastDelegate, RF_Public|RF_Transient|RF_MarkAsNative, nullptr, nullptr, 1, STRUCT_OFFSET(UfpstrueWeaponComponent, OnWeaponTraceFinished), Z_Construct_UDelegateFunction_fpstrue_WeaponTraceEvent__DelegateSignature, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_OnWeaponTraceFinished_MetaData), NewProp_OnWeaponTraceFinished_MetaData) }; // 2597797821
const UECodeGen_Private::FPropertyParamsBase* const Z_Construct_UClass_UfpstrueWeaponComponent_Statics::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_ProjectileClass,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireSound,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireAnimation,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_MuzzleOffset,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireMappingContext,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_FireAction,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bUseLineTrace,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceRange,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceImpulse,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceDamage,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_LineTraceHeadDamage,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_HipFireSpreadAngle,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_AimFireSpreadAngle,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_RecoilPitch,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_RecoilYaw,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_AimRecoilMultiplier,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_bShowDebugTrace,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponFireStarted,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponFireStopped,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponFirePerformed,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponDryFire,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponReloadStarted,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponReloadFinished,
	(const UECodeGen_Private::FPropertyParamsBase*)&Z_Construct_UClass_UfpstrueWeaponComponent_Statics::NewProp_OnWeaponTraceFinished,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UfpstrueWeaponComponent_Statics::PropPointers) < 2048);
UObject* (*const Z_Construct_UClass_UfpstrueWeaponComponent_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_USkeletalMeshComponent,
	(UObject* (*)())Z_Construct_UPackage__Script_fpstrue,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_UfpstrueWeaponComponent_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_UfpstrueWeaponComponent_Statics::ClassParams = {
	&UfpstrueWeaponComponent::StaticClass,
	"Engine",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	FuncInfo,
	Z_Construct_UClass_UfpstrueWeaponComponent_Statics::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	UE_ARRAY_COUNT(FuncInfo),
	UE_ARRAY_COUNT(Z_Construct_UClass_UfpstrueWeaponComponent_Statics::PropPointers),
	0,
	0x00B010A4u,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_UfpstrueWeaponComponent_Statics::Class_MetaDataParams), Z_Construct_UClass_UfpstrueWeaponComponent_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_UfpstrueWeaponComponent()
{
	if (!Z_Registration_Info_UClass_UfpstrueWeaponComponent.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_UfpstrueWeaponComponent.OuterSingleton, Z_Construct_UClass_UfpstrueWeaponComponent_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_UfpstrueWeaponComponent.OuterSingleton;
}
template<> FPSTRUE_API UClass* StaticClass<UfpstrueWeaponComponent>()
{
	return UfpstrueWeaponComponent::StaticClass();
}
DEFINE_VTABLE_PTR_HELPER_CTOR(UfpstrueWeaponComponent);
UfpstrueWeaponComponent::~UfpstrueWeaponComponent() {}
// End Class UfpstrueWeaponComponent

// Begin Registration
struct Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_UfpstrueWeaponComponent, UfpstrueWeaponComponent::StaticClass, TEXT("UfpstrueWeaponComponent"), &Z_Registration_Info_UClass_UfpstrueWeaponComponent, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(UfpstrueWeaponComponent), 1016787729U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_3732406397(TEXT("/Script/fpstrue"),
	Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
