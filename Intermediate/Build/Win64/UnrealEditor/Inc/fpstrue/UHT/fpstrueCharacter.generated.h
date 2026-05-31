// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "fpstrueCharacter.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
#ifdef FPSTRUE_fpstrueCharacter_generated_h
#error "fpstrueCharacter.generated.h already included, missing '#pragma once' in fpstrueCharacter.h"
#endif
#define FPSTRUE_fpstrueCharacter_generated_h

#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_43_INCLASS_NO_PURE_DECLS \
private: \
	static void StaticRegisterNativesAfpstrueCharacter(); \
	friend struct Z_Construct_UClass_AfpstrueCharacter_Statics; \
public: \
	DECLARE_CLASS(AfpstrueCharacter, ACharacter, COMPILED_IN_FLAGS(0 | CLASS_Config), CASTCLASS_None, TEXT("/Script/fpstrue"), NO_API) \
	DECLARE_SERIALIZER(AfpstrueCharacter)


#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_43_ENHANCED_CONSTRUCTORS \
private: \
	/** Private move- and copy-constructors, should never be used */ \
	AfpstrueCharacter(AfpstrueCharacter&&); \
	AfpstrueCharacter(const AfpstrueCharacter&); \
public: \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, AfpstrueCharacter); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(AfpstrueCharacter); \
	DEFINE_DEFAULT_CONSTRUCTOR_CALL(AfpstrueCharacter) \
	NO_API virtual ~AfpstrueCharacter();


#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_40_PROLOG
#define FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_43_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_43_INCLASS_NO_PURE_DECLS \
	FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h_43_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


template<> FPSTRUE_API UClass* StaticClass<class AfpstrueCharacter>();

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_ueprojrct_fpstrue_Source_fpstrue_fpstrueCharacter_h


#define FOREACH_ENUM_EFPCHARACTERSTATE(op) \
	op(EFPCharacterState::Idle) \
	op(EFPCharacterState::Moving) \
	op(EFPCharacterState::Reloading) \
	op(EFPCharacterState::Dead) 

enum class EFPCharacterState : uint8;
template<> struct TIsUEnumClass<EFPCharacterState> { enum { Value = true }; };
template<> FPSTRUE_API UEnum* StaticEnum<EFPCharacterState>();

PRAGMA_ENABLE_DEPRECATION_WARNINGS
