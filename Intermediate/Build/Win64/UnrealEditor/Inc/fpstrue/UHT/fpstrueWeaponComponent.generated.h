// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "fpstrueWeaponComponent.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
class AfpstrueCharacter;
struct FHitResult;
#ifdef FPSTRUE_fpstrueWeaponComponent_generated_h
#error "fpstrueWeaponComponent.generated.h already included, missing '#pragma once' in fpstrueWeaponComponent.h"
#endif
#define FPSTRUE_fpstrueWeaponComponent_generated_h

#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_14_DELEGATE \
FPSTRUE_API void FWeaponFireEvent_DelegateWrapper(const FMulticastScriptDelegate& WeaponFireEvent);


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_15_DELEGATE \
FPSTRUE_API void FWeaponReloadEvent_DelegateWrapper(const FMulticastScriptDelegate& WeaponReloadEvent, bool bWasEmptyReload);


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_23_DELEGATE \
FPSTRUE_API void FWeaponTraceEvent_DelegateWrapper(const FMulticastScriptDelegate& WeaponTraceEvent, bool bHit, FVector TraceStart, FVector TraceEnd, FVector TraceTarget, FHitResult HitResult);


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_RPC_WRAPPERS_NO_PURE_DECLS \
	DECLARE_FUNCTION(execEndPlay); \
	DECLARE_FUNCTION(execFire); \
	DECLARE_FUNCTION(execAttachWeapon);


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_INCLASS_NO_PURE_DECLS \
private: \
	static void StaticRegisterNativesUfpstrueWeaponComponent(); \
	friend struct Z_Construct_UClass_UfpstrueWeaponComponent_Statics; \
public: \
	DECLARE_CLASS(UfpstrueWeaponComponent, USkeletalMeshComponent, COMPILED_IN_FLAGS(0 | CLASS_Config), CASTCLASS_None, TEXT("/Script/fpstrue"), NO_API) \
	DECLARE_SERIALIZER(UfpstrueWeaponComponent)


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_ENHANCED_CONSTRUCTORS \
private: \
	/** Private move- and copy-constructors, should never be used */ \
	UfpstrueWeaponComponent(UfpstrueWeaponComponent&&); \
	UfpstrueWeaponComponent(const UfpstrueWeaponComponent&); \
public: \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, UfpstrueWeaponComponent); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(UfpstrueWeaponComponent); \
	DEFINE_DEFAULT_CONSTRUCTOR_CALL(UfpstrueWeaponComponent) \
	NO_API virtual ~UfpstrueWeaponComponent();


#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_25_PROLOG
#define FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_RPC_WRAPPERS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_INCLASS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h_28_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


template<> FPSTRUE_API UClass* StaticClass<class UfpstrueWeaponComponent>();

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_ueprojrct_fpstrue_safe2_Source_fpstrue_fpstrueWeaponComponent_h


PRAGMA_ENABLE_DEPRECATION_WARNINGS
