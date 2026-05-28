// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "fpstrue/fpstrueGameMode.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void EmptyLinkFunctionForGeneratedCodefpstrueGameMode() {}

// Begin Cross Module References
ENGINE_API UClass* Z_Construct_UClass_AGameModeBase();
FPSTRUE_API UClass* Z_Construct_UClass_AfpstrueGameMode();
FPSTRUE_API UClass* Z_Construct_UClass_AfpstrueGameMode_NoRegister();
UPackage* Z_Construct_UPackage__Script_fpstrue();
// End Cross Module References

// Begin Class AfpstrueGameMode
void AfpstrueGameMode::StaticRegisterNativesAfpstrueGameMode()
{
}
IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(AfpstrueGameMode);
UClass* Z_Construct_UClass_AfpstrueGameMode_NoRegister()
{
	return AfpstrueGameMode::StaticClass();
}
struct Z_Construct_UClass_AfpstrueGameMode_Statics
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Class_MetaDataParams[] = {
		{ "HideCategories", "Info Rendering MovementReplication Replication Actor Input Movement Collision Rendering HLOD WorldPartition DataLayers Transformation" },
		{ "IncludePath", "fpstrueGameMode.h" },
		{ "ModuleRelativePath", "fpstrueGameMode.h" },
		{ "ShowCategories", "Input|MouseInput Input|TouchInput" },
	};
#endif // WITH_METADATA
	static UObject* (*const DependentSingletons[])();
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<AfpstrueGameMode>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
};
UObject* (*const Z_Construct_UClass_AfpstrueGameMode_Statics::DependentSingletons[])() = {
	(UObject* (*)())Z_Construct_UClass_AGameModeBase,
	(UObject* (*)())Z_Construct_UPackage__Script_fpstrue,
};
static_assert(UE_ARRAY_COUNT(Z_Construct_UClass_AfpstrueGameMode_Statics::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams Z_Construct_UClass_AfpstrueGameMode_Statics::ClassParams = {
	&AfpstrueGameMode::StaticClass,
	"Game",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	nullptr,
	nullptr,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	0,
	0,
	0,
	0x008802ACu,
	METADATA_PARAMS(UE_ARRAY_COUNT(Z_Construct_UClass_AfpstrueGameMode_Statics::Class_MetaDataParams), Z_Construct_UClass_AfpstrueGameMode_Statics::Class_MetaDataParams)
};
UClass* Z_Construct_UClass_AfpstrueGameMode()
{
	if (!Z_Registration_Info_UClass_AfpstrueGameMode.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_AfpstrueGameMode.OuterSingleton, Z_Construct_UClass_AfpstrueGameMode_Statics::ClassParams);
	}
	return Z_Registration_Info_UClass_AfpstrueGameMode.OuterSingleton;
}
template<> FPSTRUE_API UClass* StaticClass<AfpstrueGameMode>()
{
	return AfpstrueGameMode::StaticClass();
}
DEFINE_VTABLE_PTR_HELPER_CTOR(AfpstrueGameMode);
AfpstrueGameMode::~AfpstrueGameMode() {}
// End Class AfpstrueGameMode

// Begin Registration
struct Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueGameMode_h_Statics
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_AfpstrueGameMode, AfpstrueGameMode::StaticClass, TEXT("AfpstrueGameMode"), &Z_Registration_Info_UClass_AfpstrueGameMode, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(AfpstrueGameMode), 1911514917U) },
	};
};
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueGameMode_h_888992518(TEXT("/Script/fpstrue"),
	Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueGameMode_h_Statics::ClassInfo, UE_ARRAY_COUNT(Z_CompiledInDeferFile_FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueGameMode_h_Statics::ClassInfo),
	nullptr, 0,
	nullptr, 0);
// End Registration
PRAGMA_ENABLE_DEPRECATION_WARNINGS
