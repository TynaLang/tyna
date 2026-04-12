#include <llvm-c/Core.h>

#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/semantic.h"
#include "tyl/utils.h"

LLVMTypeRef cg_get_llvm_type(Codegen *cg, Type *t) {
  if (!t) {
    panic("cg_get_llvm_type received NULL type");
  }

  if (t->kind == KIND_PRIMITIVE) {
    switch (t->data.primitive) {
    case PRIM_I8:
    case PRIM_U8:
    case PRIM_CHAR:
      return LLVMInt8TypeInContext(cg->context);
    case PRIM_I16:
    case PRIM_U16:
      return LLVMInt16TypeInContext(cg->context);
    case PRIM_I32:
    case PRIM_U32:
      return LLVMInt32TypeInContext(cg->context);
    case PRIM_I64:
    case PRIM_U64:
      return LLVMInt64TypeInContext(cg->context);
    case PRIM_F32:
      return LLVMFloatTypeInContext(cg->context);
    case PRIM_F64:
      return LLVMDoubleTypeInContext(cg->context);
    case PRIM_BOOL:
      return LLVMInt1TypeInContext(cg->context);
    case PRIM_VOID:
      return LLVMVoidTypeInContext(cg->context);
    case PRIM_STRING: {
      const char *name = "tyl_string";
      LLVMTypeRef str_ty = LLVMGetTypeByName2(cg->context, name);
      if (!str_ty) {
        str_ty = LLVMStructCreateNamed(cg->context, name);
        LLVMTypeRef fields[2] = {
            LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
            LLVMInt64TypeInContext(cg->context),
        };
        LLVMStructSetBody(str_ty, fields, 2, false);
      }
      return str_ty;
    }
    default:
      panic("Unknown primitive kind: %d", t->data.primitive);
    }
  }

  if (t->kind == KIND_POINTER) {
    if (!t->data.pointer_to) {
      panic("cg_get_llvm_type received pointer type with NULL target");
    }
    return LLVMPointerType(cg_get_llvm_type(cg, t->data.pointer_to), 0);
  }

  if (t->kind == KIND_UNION) {
    char buf[256];
    if (t->name.len > 0) {
      snprintf(buf, sizeof(buf), "union_%.*s", (int)t->name.len, t->name.data);
    } else {
      snprintf(buf, sizeof(buf), "anon_union_%p", (void *)t);
    }
    LLVMTypeRef union_ty = LLVMGetTypeByName2(cg->context, buf);
    if (!union_ty) {
      union_ty = LLVMStructCreateNamed(cg->context, buf);
    }
    if (!union_ty || LLVMGetTypeKind(union_ty) != LLVMStructTypeKind) {
      panic("Expected LLVM struct type for union '%s'", buf);
    }
    if (LLVMIsOpaqueStruct(union_ty)) {
      unsigned count = 1;
      LLVMTypeRef bytes = LLVMArrayType(LLVMInt8TypeInContext(cg->context),
                                        (unsigned)(t->size > 0 ? t->size : 1));
      LLVMStructSetBody(union_ty, &bytes, count, false);
    }
    return union_ty;
  }

  if (t->kind == KIND_STRUCT) {
    // For structs, we use named types to support recursion and cache them
    char buf[256];
    if (t->data.instance.from_template) {
      snprintf(buf, sizeof(buf), "%.*s_instance_%p", (int)t->name.len,
               t->name.data, (void *)t);
    } else {
      snprintf(buf, sizeof(buf), SV_FMT, SV_ARG(t->name));
    }
    LLVMTypeRef struct_ty = LLVMGetTypeByName2(cg->context, buf);
    if (!struct_ty) {
      struct_ty = LLVMStructCreateNamed(cg->context, buf);
    }

    if (!struct_ty || LLVMGetTypeKind(struct_ty) != LLVMStructTypeKind) {
      panic("Expected LLVM struct type for '%s'", buf);
    }

    if (LLVMIsOpaqueStruct(struct_ty)) {
      unsigned count = t->members.len;
      LLVMTypeRef *fields =
          xmalloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));
      for (size_t i = 0; i < count; i++) {
        Member *m = t->members.items[i];
        fields[i] = cg_get_llvm_type(cg, m->type);
      }
      LLVMStructSetBody(struct_ty, fields, count, false);
      free(fields);
    }
    return struct_ty;
  }

  if (t->kind == KIND_TEMPLATE) {
    // Templates themselves don't have a direct LLVM representation.
    // They are lowered only when instantiated.
    return NULL;
  }

  panic("Unknown type kind: %d", t->kind);
}

void cg_lower_all_structs(Codegen *cg) {
  // First pass: Create all named types to support recursion
  for (size_t i = 0; i < cg->type_ctx->structs.len; i++) {
    Type *t = cg->type_ctx->structs.items[i];
    char buf[256];
    snprintf(buf, sizeof(buf), SV_FMT, SV_ARG(t->name));
    if (!LLVMGetTypeByName2(cg->context, buf)) {
      LLVMStructCreateNamed(cg->context, buf);
    }
  }

  // Also include template instances
  for (size_t i = 0; i < cg->type_ctx->instances.len; i++) {
    Type *t = cg->type_ctx->instances.items[i];
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s_instance_%p", (int)t->name.len,
             t->name.data, (void *)t);
    if (!LLVMGetTypeByName2(cg->context, buf)) {
      LLVMStructCreateNamed(cg->context, buf);
    }
  }

  // Second pass: Populate bodies
  for (size_t i = 0; i < cg->type_ctx->structs.len; i++) {
    cg_get_llvm_type(cg, cg->type_ctx->structs.items[i]);
  }
  for (size_t i = 0; i < cg->type_ctx->instances.len; i++) {
    cg_get_llvm_type(cg, cg->type_ctx->instances.items[i]);
  }
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
