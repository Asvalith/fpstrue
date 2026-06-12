// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodefpstrue_init() {}
	FPSTRUE_API UFunction* Z_Construct_UDelegateFunction_fpstrue_OnDeath__DelegateSignature();
	FPSTRUE_API UFunction* Z_Construct_UDelegateFunction_fpstrue_OnHealthChanged__DelegateSignature();
	FPSTRUE_API UFunction* Z_Construct_UDelegateFunction_fpstrue_OnPickUp__DelegateSignature();
	static FPackageRegistrationInfo Z_Registration_Info_UPackage__Script_fpstrue;
	FORCENOINLINE UPackage* Z_Construct_UPackage__Script_fpstrue()
	{
		if (!Z_Registration_Info_UPackage__Script_fpstrue.OuterSingleton)
		{
			static UObject* (*const SingletonFuncArray[])() = {
				(UObject* (*)())Z_Construct_UDelegateFunction_fpstrue_OnDeath__DelegateSignature,
				(UObject* (*)())Z_Construct_UDelegateFunction_fpstrue_OnHealthChanged__DelegateSignature,
				(UObject* (*)())Z_Construct_UDelegateFunction_fpstrue_OnPickUp__DelegateSignature,
			};
			static const UECodeGen_Private::FPackageParams PackageParams = {
				"/Script/fpstrue",
				SingletonFuncArray,
				UE_ARRAY_COUNT(SingletonFuncArray),
				PKG_CompiledIn | 0x00000000,
				0xF40A91E4,
				0x37D4798B,
				METADATA_PARAMS(0, nullptr)
			};
			UECodeGen_Private::ConstructUPackage(Z_Registration_Info_UPackage__Script_fpstrue.OuterSingleton, PackageParams);
		}
		return Z_Registration_Info_UPackage__Script_fpstrue.OuterSingleton;
	}
	static FRegisterCompiledInInfo Z_CompiledInDeferPackage_UPackage__Script_fpstrue(Z_Construct_UPackage__Script_fpstrue, TEXT("/Script/fpstrue"), Z_Registration_Info_UPackage__Script_fpstrue, CONSTRUCT_RELOAD_VERSION_INFO(FPackageReloadVersionInfo, 0xF40A91E4, 0x37D4798B));
PRAGMA_ENABLE_DEPRECATION_WARNINGS
