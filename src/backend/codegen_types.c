#include <llvm-c/Core.h>

#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/semantic.h"
#include "tyl/utils.h"

LLVMTypeRef cg_get_llvm_type(Codegen *cg, Type *t) {
  if (t->kind == KIND_PRIMITIVE) {
    switch (t->data.primitive) {
    case PRIM_I32:
      return LLVMInt32TypeInContext(cg->context);
    case PRIM_I64:
      return LLVMInt64TypeInContext(cg->context);
    case PRIM_F32:
      return LLVMFloatTypeInContext(cg->context);
    case PRIM_F64:
      return LLVMDoubleTypeInContext(cg->context);
    case PRIM_U8:
    case PRIM_CHAR:
      return LLVMInt8TypeInContext(cg->context);
    case PRIM_BOOL:
      return LLVMInt1TypeInContext(cg->context);
    case PRIM_VOID:
      return LLVMVoidTypeInContext(cg->context);
    case PRIM_STRING:
      return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
    default:
      panic("Unknown primitive kind: %d", t->data.primitive);
    }
  }

  if (t->kind == KIND_POINTER) {
    return LLVMPointerType(cg_get_llvm_type(cg, t->data.pointer_to), 0);
  }

  if (t->kind == KIND_STRUCT) {
    // For structs, we use named types to support recursion and cache them
    char buf[256];
    snprintf(buf, sizeof(buf), SV_FMT, SV_ARG(t->name));
    LLVMTypeRef struct_ty = LLVMGetTypeByName2(cg->context, buf);
    if (struct_ty)
      return struct_ty;

    struct_ty = LLVMStructCreateNamed(cg->context, buf);

    unsigned count = t->members.len;
    LLVMTypeRef *fields =
        xmalloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));
    for (size_t i = 0; i < count; i++) {
      Member *m = t->members.items[i];
      fields[i] = cg_get_llvm_type(cg, m->type);
    }
    LLVMStructSetBody(struct_ty, fields, count, false);
    free(fields);
    return struct_ty;
  }

  panic("Unknown type kind: %d", t->kind);
}

LLVMValueRef cg_cast_value(Codegen *cg, LLVMValueRef value, Type *from_t,
                           LLVMTypeRef to_ty) {
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

    if (to_width == 1 && from_width > 1) {
      // Boolean Truthiness: x != 0
      return LLVMBuildICmp(cg->builder, LLVMIntNE, value,
                           LLVMConstInt(from_ty, 0, false), "tobool");
    }

    if (from_width > to_width) {
      return LLVMBuildTrunc(cg->builder, value, to_ty, "trunctmp");
    } else if (from_width < to_width) {
      // LLVM Strictness: use ZExt for unsigned, SExt for signed integers
      bool is_unsigned = false;
      if (from_t && from_t->kind == KIND_PRIMITIVE) {
        switch (from_t->data.primitive) {
        case PRIM_U8:
        case PRIM_U16:
        case PRIM_U32:
        case PRIM_U64:
          is_unsigned = true;
          break;
        default:
          is_unsigned = false;
          break;
        }
      }

      if (is_unsigned) {
        return LLVMBuildZExt(cg->builder, value, to_ty, "zexttmp");
      } else {
        return LLVMBuildSExt(cg->builder, value, to_ty, "sexttmp");
      }
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

void cg_binary_sync_types(Codegen *cg, LLVMValueRef *lhs, Type *l_t,
                          LLVMValueRef *rhs, Type *r_t) {
  LLVMTypeRef l_ty = LLVMTypeOf(*lhs);
  LLVMTypeRef r_ty = LLVMTypeOf(*rhs);

  if (l_ty == r_ty)
    return;

  // Promote to Floating Point if either side is FP
  if (LLVMGetTypeKind(l_ty) == LLVMDoubleTypeKind ||
      LLVMGetTypeKind(l_ty) == LLVMFloatTypeKind) {
    *rhs = cg_cast_value(cg, *rhs, r_t, l_ty);
  } else if (LLVMGetTypeKind(r_ty) == LLVMDoubleTypeKind ||
             LLVMGetTypeKind(r_ty) == LLVMFloatTypeKind) {
    *lhs = cg_cast_value(cg, *lhs, l_t, r_ty);
  } else {
    // Otherwise, promote the smaller integer to the larger integer's type
    unsigned l_w = LLVMGetIntTypeWidth(l_ty);
    unsigned r_w = LLVMGetIntTypeWidth(r_ty);
    if (l_w < r_w) {
      *lhs = cg_cast_value(cg, *lhs, l_t, r_ty);
    } else {
      *rhs = cg_cast_value(cg, *rhs, r_t, l_ty);
    }
  }
}
