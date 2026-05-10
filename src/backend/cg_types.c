#include <ctype.h>
#include <execinfo.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <stdlib.h>

#include "cg_internal.h"
#include "tyna/utils.h"

static void cg_sanitize_type_name(const char *src, char *dst, size_t dst_size) {
  size_t di = 0;
  for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; si++) {
    char c = src[si];
    if (isalnum((unsigned char)c) || c == '_' || c == '$') {
      dst[di++] = c;
    } else if (di > 0 && dst[di - 1] != '_') {
      dst[di++] = '_';
    }
  }
  while (di > 0 && dst[di - 1] == '_')
    di--;
  dst[di] = '\0';
}

static void cg_get_struct_name(Type *t, char *buf, size_t buf_size) {
  if (t->data.instance.from_template) {
    const char *raw_name = type_to_name(t);
    cg_sanitize_type_name(raw_name, buf, buf_size);
  } else {
    snprintf(buf, buf_size, SV_FMT, SV_ARG(t->name));
  }
}

LLVMTypeRef cg_type_get_llvm(Codegen *cg, Type *t) {
  if (!t) {
    void *bt[16];
    int bt_size = backtrace(bt, 16);
    char **bt_syms = backtrace_symbols(bt, bt_size);
    fprintf(stderr, "[DEBUG cg_type_get_llvm] NULL type call stack:\n");
    for (int i = 0; i < bt_size; i++) {
      fprintf(stderr, "  %s\n", bt_syms[i]);
    }
    free(bt_syms);
    panic("cg_type_get_llvm received NULL type");
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
      const char *name = "tyna_string";
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
    case PRIM_UNKNOWN:
      return LLVMInt8TypeInContext(cg->context);
    default:
      panic("Unknown primitive kind: %d", t->data.primitive);
    }
  }

  if (t->kind == KIND_POINTER) {
    if (!t->data.pointer_to) {
      panic("cg_type_get_llvm received pointer type with NULL target");
    }
    LLVMTypeRef pointee = cg_type_get_llvm(cg, t->data.pointer_to);
    if (!pointee || LLVMGetTypeKind(pointee) == LLVMVoidTypeKind) {
      // Lower void* and generic placeholder pointers as i8*
      pointee = LLVMInt8TypeInContext(cg->context);
    }
    return LLVMPointerType(pointee, 0);
  }

  if (t->kind == KIND_STRUCT && type_is_heap_or_ref(t)) {
    Type *inner = t->data.instance.generic_args.items[0];
    LLVMTypeRef pointee = cg_type_get_llvm(cg, inner);
    if (!pointee || LLVMGetTypeKind(pointee) == LLVMVoidTypeKind) {
      pointee = LLVMInt8TypeInContext(cg->context);
    }
    return LLVMPointerType(pointee, 0);
  }

  if (t->kind == KIND_STRUCT && t->is_transparent && type_is_option(t)) {
    Type *payload = type_get_option_payload(t);
    if (!payload)
      panic("Transparent Option without payload");
    return cg_type_get_llvm(cg, payload);
  }

  if (t->kind == KIND_TEMPLATE) {
    // Generic placeholders do not have a concrete LLVM type yet.
    // Lower them to a byte-sized placeholder type for codegen safety.
    return LLVMInt8TypeInContext(cg->context);
  }

  if (t->kind == KIND_STRUCT && type_is_array_struct(t) &&
      t->fixed_array_len > 0) {
    if (t->data.instance.generic_args.len == 0) {
      panic("Fixed array type is missing element type");
    }
    Type *elem_t = t->data.instance.generic_args.items[0];
    LLVMTypeRef elem_ty = cg_type_get_llvm(cg, elem_t);
    return LLVMArrayType(elem_ty, (unsigned)t->fixed_array_len);
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
      if (t->is_tagged_union) {
        LLVMTypeRef tag_ty = LLVMInt64TypeInContext(cg->context);
        size_t payload_size = t->size > 8 ? t->size - 8 : 0;
        LLVMTypeRef payload = LLVMArrayType(LLVMInt8TypeInContext(cg->context),
                                            (unsigned)payload_size);
        LLVMTypeRef fields[2] = {tag_ty, payload};
        LLVMStructSetBody(union_ty, fields, 2, false);
      } else {
        unsigned count = 1;
        LLVMTypeRef bytes =
            LLVMArrayType(LLVMInt8TypeInContext(cg->context),
                          (unsigned)(t->size > 0 ? t->size : 1));
        LLVMStructSetBody(union_ty, &bytes, count, false);
      }
    }
    return union_ty;
  }

  if (t->kind == KIND_STRUCT || t->kind == KIND_ERROR) {
    // For structs and error payloads, use named types to support recursion
    // and cache them. Error payloads are structurally equivalent to structs.
    char buf[512];
    cg_get_struct_name(t, buf, sizeof(buf));

    LLVMTypeRef struct_ty = LLVMGetTypeByName2(cg->context, buf);
    if (!struct_ty) {
      struct_ty = LLVMStructCreateNamed(cg->context, buf);
    }

    if (!struct_ty || LLVMGetTypeKind(struct_ty) != LLVMStructTypeKind) {
      panic("Expected LLVM struct type for '%s'", buf);
    }

    if (LLVMIsOpaqueStruct(struct_ty)) {
      for (size_t i = 0; i < cg->struct_types_in_progress.len; i++) {
        if (cg->struct_types_in_progress.items[i] == t)
          return struct_ty;
      }

      List_push(&cg->struct_types_in_progress, t);
      
      // If this struct instance has template placeholders in its generic args
      // (e.g., MapEntry<K, V>), look for a concrete instance with the same
      // template and use its members for the LLVM type body. This ensures
      // correct GEP offsets even when the codegen uses the generic type.
      unsigned count = t->members.len;
      LLVMTypeRef *fields = NULL;
      
      if (t->data.instance.from_template &&
          t->data.instance.generic_args.len > 0) {
        bool has_placeholder = false;
        for (size_t gi = 0; gi < t->data.instance.generic_args.len; gi++) {
          Type *arg = (Type *)t->data.instance.generic_args.items[gi];
          if (arg->kind == KIND_TEMPLATE) { has_placeholder = true; break; }
        }
        if (has_placeholder) {
          // Look for a concrete instance with the same template.
          // Use the one with the largest size to ensure correct GEP offsets.
          Type *best_inst = NULL;
          for (size_t ii = 0; ii < cg->type_ctx->instances.len; ii++) {
            Type *inst = (Type *)cg->type_ctx->instances.items[ii];
            if (inst->kind != KIND_STRUCT) continue;
            if (inst->data.instance.from_template != t->data.instance.from_template) continue;
            if (inst->data.instance.generic_args.len != t->data.instance.generic_args.len) continue;
            bool all_concrete = true;
            for (size_t gi = 0; gi < inst->data.instance.generic_args.len; gi++) {
              Type *ia = (Type *)inst->data.instance.generic_args.items[gi];
              if (ia->kind == KIND_TEMPLATE) { all_concrete = false; break; }
            }
            if (all_concrete && inst->members.len > 0) {
              if (!best_inst || inst->size > best_inst->size) {
                best_inst = inst;
              }
            }
          }
          if (best_inst) {
            count = best_inst->members.len;
            fields = xmalloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));
            for (size_t mi = 0; mi < count; mi++) {
              Member *m = best_inst->members.items[mi];
              fields[mi] = cg_type_get_llvm(cg, m->type);
            }
          }
        }
      }
      
      if (!fields) {
        fields = xmalloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));
        for (size_t i = 0; i < count; i++) {
          Member *m = t->members.items[i];
          fields[i] = cg_type_get_llvm(cg, m->type);
        }
      }
      
      LLVMStructSetBody(struct_ty, fields, count, false);
      free(fields);
      List_pop(&cg->struct_types_in_progress);
    } else {
      // If the struct type already exists but has fewer fields than expected,
      // it was created before all members were added. Recreate the body.
      unsigned existing_count = LLVMCountStructElementTypes(struct_ty);
      if (existing_count < t->members.len) {
        unsigned count = t->members.len;
        LLVMTypeRef *fields =
            xmalloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));
        for (size_t i = 0; i < count; i++) {
          Member *m = t->members.items[i];
          fields[i] = cg_type_get_llvm(cg, m->type);
        }
        LLVMStructSetBody(struct_ty, fields, count, false);
        free(fields);
      }
    }
    return struct_ty;
  }

  if (t->kind == KIND_ERROR_SET) {
    char buf[512];
    if (t->name.len > 0) {
      snprintf(buf, sizeof(buf), "errors_%.*s", (int)t->name.len, t->name.data);
    } else {
      snprintf(buf, sizeof(buf), "anon_errors_%p", (void *)t);
    }
    LLVMTypeRef set_ty = LLVMGetTypeByName2(cg->context, buf);
    if (!set_ty) {
      set_ty = LLVMStructCreateNamed(cg->context, buf);
    }
    if (!set_ty || LLVMGetTypeKind(set_ty) != LLVMStructTypeKind) {
      panic("Expected LLVM struct type for '%s'", buf);
    }
    if (LLVMIsOpaqueStruct(set_ty)) {
      // Error sets do not currently carry a direct runtime representation.
      // We lower them to an empty named struct so they can still participate
      // in type lowering and naming.
      LLVMStructSetBody(set_ty, NULL, 0, false);
    }
    return set_ty;
  }

  if (t->kind == KIND_RESULT) {
    char success_name[256] = {0};
    char error_name[256] = {0};
    cg_sanitize_type_name(type_to_name(t->data.result.success), success_name,
                          sizeof(success_name));
    cg_sanitize_type_name(type_to_name(t->data.result.error_set), error_name,
                          sizeof(error_name));
    char buf[512];
    snprintf(buf, sizeof(buf), "result_%s_%s", success_name, error_name);
    LLVMTypeRef result_ty = LLVMGetTypeByName2(cg->context, buf);
    if (!result_ty) {
      result_ty = LLVMStructCreateNamed(cg->context, buf);
    }
    if (!result_ty || LLVMGetTypeKind(result_ty) != LLVMStructTypeKind) {
      panic("Expected LLVM struct type for '%s'", buf);
    }
    if (LLVMIsOpaqueStruct(result_ty)) {
      LLVMTypeRef success_ty = cg_type_get_llvm(cg, t->data.result.success);
      if (LLVMGetTypeKind(success_ty) == LLVMVoidTypeKind) {
        success_ty = LLVMArrayType(LLVMInt8TypeInContext(cg->context), 0);
      }
      LLVMTypeRef tag_ty = LLVMInt16TypeInContext(cg->context);
      LLVMTypeRef payload_ty =
          LLVMArrayType(LLVMInt8TypeInContext(cg->context), 46);
      LLVMTypeRef fields[3] = {success_ty, tag_ty, payload_ty};
      LLVMStructSetBody(result_ty, fields, 3, false);
    }
    return result_ty;
  }

  if (t->kind == KIND_TEMPLATE) {
    // Templates themselves don't have a direct LLVM representation.
    // They are lowered only when instantiated.
    return NULL;
  }

  if (t->kind == KIND_STRING_BUFFER) {
    const char *name = "tyna_string_buf";
    LLVMTypeRef buf_ty = LLVMGetTypeByName2(cg->context, name);
    if (!buf_ty) {
      buf_ty = LLVMStructCreateNamed(cg->context, name);
      LLVMTypeRef fields[3] = {
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
          LLVMInt64TypeInContext(cg->context),
          LLVMInt64TypeInContext(cg->context),
      };
      LLVMStructSetBody(buf_ty, fields, 3, false);
    }
    return buf_ty;
  }

  panic("Unknown type kind: %d", t->kind);
}

void cg_type_lower_structs(Codegen *cg) {
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
    if (!type_is_concrete(t))
      continue;
    char buf[512];
    cg_get_struct_name(t, buf, sizeof(buf));
    if (!LLVMGetTypeByName2(cg->context, buf)) {
      LLVMStructCreateNamed(cg->context, buf);
    }
  }

  // Second pass: Populate bodies
  for (size_t i = 0; i < cg->type_ctx->structs.len; i++) {
    cg_type_get_llvm(cg, cg->type_ctx->structs.items[i]);
  }
  for (size_t i = 0; i < cg->type_ctx->instances.len; i++) {
    Type *t = cg->type_ctx->instances.items[i];
    if (!type_is_concrete(t))
      continue;
    cg_type_get_llvm(cg, t);
  }
  cg_type_get_llvm(cg, type_get_string_buffer(cg->type_ctx));
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
    if (from_kind == LLVMIntegerTypeKind) {
      return LLVMBuildIntToPtr(cg->builder, value, to_ty, "inttoptrtmp");
    }

    if (from_kind == LLVMStructTypeKind) {
      if (from_t && from_t->kind == KIND_PRIMITIVE &&
          from_t->data.primitive == PRIM_STRING) {
        return LLVMBuildExtractValue(cg->builder, value, 0, "string_data_ptr");
      }

      if (from_t && from_t->kind == KIND_STRING_BUFFER) {
        return LLVMBuildExtractValue(cg->builder, value, 0, "string_data_ptr");
      }

      // For struct-to-pointer casts, use alloca + bitcast instead of
      // direct bitcast (which LLVM doesn't allow for struct types).
      LLVMValueRef tmp =
          LLVMBuildAlloca(cg->builder, from_ty, "struct_to_ptr_tmp");
      LLVMBuildStore(cg->builder, value, tmp);
      return LLVMBuildBitCast(cg->builder, tmp, to_ty, "struct_to_ptr");
    }

    return LLVMBuildBitCast(cg->builder, value, to_ty, "bitcasttmp");
  }

  if (from_kind == LLVMPointerTypeKind && to_kind == LLVMIntegerTypeKind) {
    return LLVMBuildPtrToInt(cg->builder, value, to_ty, "ptrtointtmp");
  }

  if (from_kind == LLVMPointerTypeKind && to_kind == LLVMIntegerTypeKind &&
      LLVMGetIntTypeWidth(to_ty) == 1) {
    return LLVMBuildICmp(cg->builder, LLVMIntNE, value, LLVMConstNull(from_ty),
                         "ptrnonnull");
  }

  if (from_kind == LLVMStructTypeKind && from_t &&
      from_t->kind == KIND_STRING_BUFFER) {
    LLVMTypeRef str_ty =
        cg_type_get_llvm(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
    if (to_ty == str_ty) {
      LLVMValueRef undef = LLVMGetUndef(str_ty);
      LLVMValueRef p = LLVMBuildExtractValue(cg->builder, value, 0, "sbuf_ptr");
      LLVMValueRef l = LLVMBuildExtractValue(cg->builder, value, 1, "sbuf_len");
      LLVMValueRef s0 =
          LLVMBuildInsertValue(cg->builder, undef, p, 0, "sbuf_slice0");
      return LLVMBuildInsertValue(cg->builder, s0, l, 1, "sbuf_slice");
    }
  }

  if (from_kind == LLVMStructTypeKind && to_kind == LLVMStructTypeKind) {
    // Instead of bitcasting (which LLVM doesn't allow between different
    // struct types), use memcpy to copy the bytes.
    // Use the LARGER of the two sizes to ensure all data is preserved
    // when casting between generic and concrete types.
    LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, to_ty, "structcast_tmp");
    LLVMBuildStore(cg->builder, LLVMConstNull(to_ty), tmp);
    LLVMValueRef src = LLVMBuildBitCast(
        cg->builder,
        LLVMBuildAlloca(cg->builder, LLVMTypeOf(value), "structcast_src"),
        LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0), "cast_src");
    LLVMBuildStore(cg->builder, value, src);
    LLVMValueRef dst = LLVMBuildBitCast(
        cg->builder, tmp,
        LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0), "cast_dst");
    const char *data_layout = LLVMGetDataLayout(cg->module);
    LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
    unsigned long long src_size = LLVMABISizeOfType(td, LLVMTypeOf(value));
    unsigned long long dst_size = LLVMABISizeOfType(td, to_ty);
    LLVMDisposeTargetData(td);
    // Use the larger size to ensure all data is preserved
    unsigned long long copy_size = src_size > dst_size ? src_size : dst_size;
    LLVMValueRef size_val =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), copy_size, 0);
    LLVMBuildMemCpy(cg->builder, dst, 1, src, 1, size_val);
    return LLVMBuildLoad2(cg->builder, to_ty, tmp, "structcast_load");
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
