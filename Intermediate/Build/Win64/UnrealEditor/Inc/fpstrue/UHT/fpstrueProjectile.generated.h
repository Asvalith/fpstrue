// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "fpstrueProjectile.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
class AActor;
class UPrimitiveComponent;
struct FHitResult;
#ifdef FPSTRUE_fpstrueProjectile_generated_h
#error "fpstrueProjectile.generated.h already included, missing '#pragma once' in fpstrueProjectile.h"
#endif
#define FPSTRUE_fpstrueProjectile_generated_h

#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_RPC_WRAPPERS_NO_PURE_DECLS \
	DECLARE_FUNCTION(execOnHit);


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_INCLASS_NO_PURE_DECLS \
private: \
	static void StaticRegisterNativesAfpstrueProjectile(); \
	friend struct Z_Construct_UClass_AfpstrueProjectile_Statics; \
public: \
	DECLARE_CLASS(AfpstrueProjectile, AActor, COMPILED_IN_FLAGS(0 | CLASS_Config), CASTCLASS_None, TEXT("/Script/fpstrue"), NO_API) \
	DECLARE_SERIALIZER(AfpstrueProjectile) \
	static const TCHAR* StaticConfigName() {return TEXT("Game");} \



#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_ENHANCED_CONSTRUCTORS \
private: \
	/** Private move- and copy-constructors, should never be used */ \
	AfpstrueProjectile(AfpstrueProjectile&&); \
	AfpstrueProjectile(const AfpstrueProjectile&); \
public: \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, AfpstrueProjectile); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(AfpstrueProjectile); \
	DEFINE_DEFAULT_CONSTRUCTOR_CALL(AfpstrueProjectile) \
	NO_API virtual ~AfpstrueProjectile();


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_12_PROLOG
#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_RPC_WRAPPERS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_INCLASS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h_15_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


template<> FPSTRUE_API UClass* StaticClass<class AfpstrueProjectile>();

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueProjectile_h


PRAGMA_ENABLE_DEPRECATION_WARNINGS
