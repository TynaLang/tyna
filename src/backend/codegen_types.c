#include <llvm-c/Core.h>

#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/semantic.h"
#include "tyl/utils.h"

LLVMTypeRef cg_get_llvm_type(Codegen *cg, TypeKind t) {
  switch (t) {
  case TYPE_I8:
  case TYPE_U8:
    return LLVMInt8TypeInContext(cg->context);
  case TYPE_I16:
  case TYPE_U16:
    return LLVMInt16TypeInContext(cg->context);
  case TYPE_I32:
  case TYPE_U32:
    return LLVMInt32TypeInContext(cg->context);
  case TYPE_I64:
  case TYPE_U64:
    return LLVMInt64TypeInContext(cg->context);
  case TYPE_F32:
    return LLVMFloatTypeInContext(cg->context);
  case TYPE_F64:
    return LLVMDoubleTypeInContext(cg->context);
  case TYPE_CHAR:
    return LLVMInt8TypeInContext(cg->context);
  case TYPE_BOOL:
    return LLVMInt1TypeInContext(cg->context);
  case TYPE_VOID:
    return LLVMVoidTypeInContext(cg->context);

  case TYPE_STRING:
    return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);

  case TYPE_UNKNOWN:
  default:
    printf("Unknown type kind: %d\n", t);
    panic("Unknown type kind: %d", type_to_name(t));
  }
}

LLVMValueRef cg_cast_value(Codegen *cg, LLVMValueRef value, LLVMTypeRef to_ty) {
  if (!value) {
    fprintf(stderr, "Critical: Attempted to cast a NULL LLVMValueRef!\n");
    return NULL;
  }
  LLVMTypeRef from_ty = LLVMTypeOf(value);

  if (from_ty == to_ty)
    return value;

  LLVMTypeKind from_kind = LLVMGetTypeKind(from_ty);
  LLVMTypeKind to_kind = LLVMGetTypeKind(to_ty);

  if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMIntegerTypeKind) {
    unsigned from_width = LLVMGetIntTypeWidth(from_ty);
    unsigned to_width = LLVMGetIntTypeWidth(to_ty);

    if (from_width > to_width) {
      return LLVMBuildTrunc(cg->builder, value, to_ty, "trunctmp");
    } else if (from_width < to_width) {
      // TODO: Handle signed vs unsigned properly here (currently just
      // sign-extends)
      return LLVMBuildSExt(cg->builder, value, to_ty, "sexttmp");
    }
  }

  if (from_kind == LLVMDoubleTypeKind || from_kind == LLVMFloatTypeKind) {
    if (to_kind == LLVMIntegerTypeKind) {
      return LLVMBuildFPToSI(cg->builder, value, to_ty, "fptositmp");
    }
    if (to_kind == LLVMDoubleTypeKind || to_kind == LLVMFloatTypeKind) {
      return LLVMBuildFPCast(cg->builder, value, to_ty, "fpcasttmp");
    }
  }

  if (from_kind == LLVMIntegerTypeKind) {
    if (to_kind == LLVMDoubleTypeKind || to_kind == LLVMFloatTypeKind) {
      return LLVMBuildSIToFP(cg->builder, value, to_ty, "sitofptmp");
    }
  }

  if (to_kind == LLVMPointerTypeKind) {
    return LLVMBuildBitCast(cg->builder, value, to_ty, "bitcasttmp");
  }

  return value;
}

void cg_binary_sync_types(Codegen *cg, LLVMValueRef *lhs, LLVMValueRef *rhs) {
  LLVMTypeRef l_ty = LLVMTypeOf(*lhs);
  LLVMTypeRef r_ty = LLVMTypeOf(*rhs);

  if (l_ty == r_ty)
    return;

  // Promote to Floating Point if either side is FP
  if (LLVMGetTypeKind(l_ty) == LLVMDoubleTypeKind ||
      LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind) {
    *rhs = cg_cast_value(cg, *rhs, l_ty);
  } else if (LLVMGetTypeKind(r_ty) == LLVMDoubleTypeKind ||
             LLVMGetTypeKind(r_ty) == LLVMFloatTypeKind) {
    *lhs = cg_cast_value(cg, *lhs, r_ty);
  } else {
    // Otherwise, promote the smaller integer to the larger integer's type
    // (Simplified: for now, just pick the RHS type)
    *lhs = cg_cast_value(cg, *lhs, r_ty);
  }
}
