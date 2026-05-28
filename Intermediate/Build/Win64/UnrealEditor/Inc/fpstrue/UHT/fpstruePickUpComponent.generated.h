// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "fpstruePickUpComponent.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
class AActor;
class AfpstrueCharacter;
class UPrimitiveComponent;
struct FHitResult;
#ifdef FPSTRUE_fpstruePickUpComponent_generated_h
#error "fpstruePickUpComponent.generated.h already included, missing '#pragma once' in fpstruePickUpComponent.h"
#endif
#define FPSTRUE_fpstruePickUpComponent_generated_h

#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_12_DELEGATE \
FPSTRUE_API void FOnPickUp_DelegateWrapper(const FMulticastScriptDelegate& OnPickUp, AfpstrueCharacter* PickUpCharacter);


#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_RPC_WRAPPERS_NO_PURE_DECLS \
	DECLARE_FUNCTION(execOnSphereBeginOverlap);


#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_INCLASS_NO_PURE_DECLS \
private: \
	static void StaticRegisterNativesUfpstruePickUpComponent(); \
	friend struct Z_Construct_UClass_UfpstruePickUpComponent_Statics; \
public: \
	DECLARE_CLASS(UfpstruePickUpComponent, USphereComponent, COMPILED_IN_FLAGS(0 | CLASS_Config), CASTCLASS_None, TEXT("/Script/fpstrue"), NO_API) \
	DECLARE_SERIALIZER(UfpstruePickUpComponent)


#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_ENHANCED_CONSTRUCTORS \
private: \
	/** Private move- and copy-constructors, should never be used */ \
	UfpstruePickUpComponent(UfpstruePickUpComponent&&); \
	UfpstruePickUpComponent(const UfpstruePickUpComponent&); \
public: \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, UfpstruePickUpComponent); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(UfpstruePickUpComponent); \
	DEFINE_DEFAULT_CONSTRUCTOR_CALL(UfpstruePickUpComponent) \
	NO_API virtual ~UfpstruePickUpComponent();


#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_14_PROLOG
#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_RPC_WRAPPERS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_INCLASS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h_17_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


template<> FPSTRUE_API UClass* StaticClass<class UfpstruePickUpComponent>();

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_ueprojrct_fpstrue_Source_fpstrue_fpstruePickUpComponent_h


PRAGMA_ENABLE_DEPRECATION_WARNINGS
