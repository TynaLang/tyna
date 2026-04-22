#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/sema.h"
#include "tyna/utils.h"
#include "llvm-c/Types.h"

static LLVMValueRef cg_make_param_addressable(Codegen *cg, CgSym *sym) {
  LLVMValueRef alloca =
      cg_alloca_in_entry_uninitialized(cg, sym->type, sym->name);
  LLVMBuildStore(cg->builder, sym->value, alloca);
  sym->value = alloca;
  sym->is_direct_value = false;
  return alloca;
}

static LLVMTypeRef cg_const_target_type(Codegen *cg, AstNode *node) {
  if (node->resolved_type)
    return cg_type_get_llvm(cg, node->resolved_type);

  switch (node->tag) {
  case NODE_NUMBER: {
    if (sv_contains(node->number.raw_text, '.') ||
        sv_ends_with(node->number.raw_text, "f") ||
        sv_ends_with(node->number.raw_text, "F")) {
      return LLVMDoubleTypeInContext(cg->context);
    }
    return LLVMInt32TypeInContext(cg->context);
  }
  case NODE_BOOL:
    return LLVMInt1TypeInContext(cg->context);
  case NODE_CHAR:
    return LLVMInt8TypeInContext(cg->context);
  case NODE_STRING:
    return cg_type_get_llvm(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
  case NODE_NULL:
    return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  default:
    panic("cg_const_expr received unknown constant node without resolved type");
  }
}

static LLVMValueRef cg_const_expr_impl(Codegen *cg, AstNode *node) {
  LLVMTypeRef target_ty = cg_const_target_type(cg, node);

  switch (node->tag) {
  case NODE_NUMBER: {
    double val = node->number.value;
    if (LLVMGetTypeKind(target_ty) == LLVMDoubleTypeKind ||
        LLVMGetTypeKind(target_ty) == LLVMFloatTypeKind) {
      return LLVMConstReal(target_ty, val);
    }
    return LLVMConstInt(target_ty, (long long)val, true);
  }
  case NODE_BOOL:
    return LLVMConstInt(target_ty, node->boolean.value, false);
  case NODE_CHAR:
    return LLVMConstInt(target_ty, node->char_lit.value, false);
  case NODE_STRING: {
    size_t pool_idx = cg_string_pool_insert(cg, node->string.value);
    LLVMValueRef const_str_global = cg->string_globals.items[pool_idx];
    LLVMValueRef zero =
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false);
    LLVMValueRef indices[] = {zero, zero};
    LLVMValueRef ptr = LLVMConstInBoundsGEP2(LLVMInt8TypeInContext(cg->context),
                                             const_str_global, indices, 2);
    LLVMValueRef len = LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                                    node->string.value.len, false);
    LLVMValueRef fields[] = {ptr, len};
    return LLVMConstNamedStruct(target_ty, fields, 2);
  }
  case NODE_NULL:
    return LLVMConstNull(target_ty);
  default:
    return NULL;
  }
}

static Symbol *cg_find_method(Type *type, StringView name) {
  if (!type)
    return NULL;
  for (size_t i = 0; i < type->methods.len; i++) {
    Symbol *method = type->methods.items[i];
    if (!method)
      continue;
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, name))
      return method;
  }
  return NULL;
}

static LLVMValueRef cg_typeof_string(Codegen *cg, Type *type) {
  const char *type_name = type_to_name(type);
  StringView name_sv = sv_from_cstr(type_name);
  size_t pool_idx = cg_string_pool_insert(cg, name_sv);
  LLVMValueRef const_str_global = cg->string_globals.items[pool_idx];
  LLVMValueRef zero =
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false);
  LLVMValueRef indices[] = {zero, zero};
  LLVMValueRef ptr = LLVMConstInBoundsGEP2(LLVMInt8TypeInContext(cg->context),
                                           const_str_global, indices, 2);
  LLVMTypeRef string_ty =
      cg_type_get_llvm(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
  LLVMValueRef len =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), name_sv.len, false);
  LLVMValueRef fields[] = {ptr, len};
  return LLVMConstNamedStruct(string_ty, fields, 2);
}

LLVMValueRef cg_expression_addr(Codegen *cg, AstNode *node) {
  switch (node->tag) {
  case NODE_VAR: {
    CgSym *sym = CGSymbolTable_find(cg->current_scope, node->var.value);
    if (!sym)
      panic("Undefined variable in codegen: " SV_FMT, SV_ARG(node->var.value));
    if (sym->is_direct_value)
      return cg_make_param_addressable(cg, sym);
    return sym->value;
  }

  case NODE_FIELD: {
    LLVMValueRef obj_ptr = cg_expression_addr(cg, node->field.object);
    Type *obj_type = node->field.object->resolved_type;

    if (obj_type && obj_type->kind == KIND_POINTER) {
      if (!obj_type->data.pointer_to) {
        panic("Pointer object has no target type");
      }
      LLVMTypeRef load_ty = cg_type_get_llvm(cg, obj_type);
      obj_ptr = LLVMBuildLoad2(cg->builder, load_ty, obj_ptr, "deref_ptr");
      obj_type = obj_type->data.pointer_to;
    }

    int index = -1;
    for (size_t i = 0; i < obj_type->members.len; i++) {
      Member *m = obj_type->members.items[i];
      if (sv_eq_cstr(node->field.field, m->name)) {
        index = (int)i;
        break;
      }
    }

    if (index < 0) {
      panic("Field '%.*s' not found on type %s", (int)node->field.field.len,
            node->field.field.data, type_to_name(obj_type));
    }

    return LLVMBuildStructGEP2(cg->builder, cg_type_get_llvm(cg, obj_type),
                               obj_ptr, (unsigned)index, "field_addr");
  }

  case NODE_INDEX: {
    if (!node->resolved_type) {
      Type *array_type = node->index.array->resolved_type;
      if (array_type) {
        if (array_type->kind == KIND_POINTER) {
          node->resolved_type = array_type->data.pointer_to;
        } else if (type_is_array_struct(array_type) &&
                   array_type->data.instance.generic_args.len > 0) {
          node->resolved_type = array_type->data.instance.generic_args.items[0];
        } else if (array_type->kind == KIND_PRIMITIVE &&
                   array_type->data.primitive == PRIM_STRING) {
          node->resolved_type = type_get_primitive(cg->type_ctx, PRIM_CHAR);
        }
      }
    }
    LLVMValueRef array_struct_ptr = cg_expression_addr(cg, node->index.array);
    LLVMTypeRef array_struct_ty =
        cg_type_get_llvm(cg, node->index.array->resolved_type);

    LLVMValueRef rank_ptr = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 3, "rank_ptr");
    LLVMValueRef rank_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), rank_ptr, "rank_val");

    LLVMValueRef dims_ptr_ptr = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 4, "dims_ptr_ptr");
    LLVMValueRef dims_ptr = LLVMBuildLoad2(
        cg->builder, LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0),
        dims_ptr_ptr, "dims_ptr");

    LLVMValueRef index_val = cg_expression(cg, node->index.index);
    LLVMValueRef index_i64 =
        cg_cast_value(cg, index_val, node->index.index->resolved_type,
                      LLVMInt64TypeInContext(cg->context));

    LLVMValueRef dim0_ptr = LLVMBuildGEP2(
        cg->builder, LLVMInt64TypeInContext(cg->context), dims_ptr,
        (LLVMValueRef[]){
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0)},
        1, "dim0_ptr");
    LLVMValueRef dim0_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), dim0_ptr, "dim0_val");

    LLVMValueRef in_bounds = LLVMBuildICmp(cg->builder, LLVMIntULT, index_i64,
                                           dim0_val, "in_bounds");

    LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "bounds_fail");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "bounds_ok");

    LLVMBuildCondBr(cg->builder, in_bounds, ok_bb, fail_bb);

    LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
    CgFunc *panic_fn = cg_find_system_function(cg, sv_from_parts("panic", 5));
    if (!panic_fn) {
      panic("Internal error: system panic helper missing");
    }
    LLVMValueRef msg_ptr = LLVMBuildGlobalStringPtr(
        cg->builder, "Panic: Array Index Out of Bounds", "bounds_err_msg");
    LLVMBuildCall2(cg->builder, panic_fn->type, panic_fn->value,
                   (LLVMValueRef[]){msg_ptr}, 1, "");
    LLVMBuildUnreachable(cg->builder);

    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);

    LLVMValueRef data_ptr_gep = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 0, "data_field_ptr");
    LLVMTypeRef elem_ptr_ty = cg_type_get_llvm(
        cg, type_get_pointer(cg->type_ctx, node->resolved_type));
    LLVMValueRef actual_data_ptr =
        LLVMBuildLoad2(cg->builder, elem_ptr_ty, data_ptr_gep, "load_data_ptr");

    LLVMValueRef element_addr = LLVMBuildGEP2(
        cg->builder, cg_type_get_llvm(cg, node->resolved_type), actual_data_ptr,
        (LLVMValueRef[]){index_i64}, 1, "element_ptr");

    if (type_is_array_struct(node->resolved_type)) {
      return element_addr;
    }

    return element_addr;
  }

  default: {
    if (!node->resolved_type) {
      fprintf(stderr, "Codegen Error: Node tag %d has no resolved type\n",
              node->tag);
      return NULL;
    }

    LLVMValueRef val = cg_expression(cg, node);
    LLVMValueRef temp_alloca = cg_alloca_in_entry(
        cg, node->resolved_type, sv_from_cstr("temp_expr_spill"));
    LLVMBuildStore(cg->builder, val, temp_alloca);
    return temp_alloca;
  }
  }
}

LLVMValueRef cg_const_expr(Codegen *cg, AstNode *node) {
  return cg_const_expr_impl(cg, node);
}

LLVMValueRef cg_var_expr(Codegen *cg, AstNode *node) {
  CgSym *cgsym = CGSymbolTable_find(cg->current_scope, node->var.value);
  if (!node->resolved_type) {
    if (cgsym) {
      node->resolved_type = cgsym->type;
    }
  }

  if (!cgsym && node->resolved_type &&
      node->resolved_type->kind == KIND_ERROR) {
    LLVMTypeRef ty = cg_type_get_llvm(cg, node->resolved_type);
    return LLVMConstNull(ty);
  }

  if (cgsym && cgsym->is_direct_value) {
    return cgsym->value;
  }

  LLVMValueRef ptr = cg_get_address(cg, node);
  if (!ptr) {
    panic("Undefined variable during codegen: " SV_FMT,
          SV_ARG(node->var.value));
  }
  if (!node->resolved_type) {
    panic("Undefined variable type during codegen: " SV_FMT,
          SV_ARG(node->var.value));
  }
  LLVMTypeRef type = cg_type_get_llvm(cg, node->resolved_type);
  return LLVMBuildLoad2(cg->builder, type, ptr, "var_load");
}

LLVMValueRef cg_field_expr(Codegen *cg, AstNode *node) {
  AstNode *obj_node = node->field.object;
  Type *obj_type = obj_node->resolved_type;
  while (obj_type && obj_type->kind == KIND_POINTER) {
    obj_type = obj_type->data.pointer_to;
  }
  if (!obj_type) {
    panic("Field expression object has no resolved type");
  }

  Type *owner = obj_type;
  Member *m = type_get_member(obj_type, node->field.field);
  if (!m && obj_type->kind == KIND_UNION) {
    m = type_find_union_field(obj_type, node->field.field, &owner);
  }
  if (!m)
    panic("Field not found");
  if (!m->type) {
    panic("Field member has no type");
  }

  LLVMValueRef obj_ptr = cg_get_address(cg, obj_node);
  if (!obj_ptr) {
    LLVMValueRef obj_val = cg_expression(cg, obj_node);
    obj_ptr = LLVMBuildAlloca(cg->builder, cg_type_get_llvm(cg, obj_type),
                              "tmp_obj_field");
    LLVMBuildStore(cg->builder, obj_val, obj_ptr);
  } else {
    Type *resolved = obj_node->resolved_type;
    while (resolved && resolved->kind == KIND_POINTER) {
      LLVMTypeRef load_ty = cg_type_get_llvm(cg, resolved);
      obj_ptr = LLVMBuildLoad2(cg->builder, load_ty, obj_ptr, "deref_ptr");
      resolved = resolved->data.pointer_to;
    }
  }

  if (obj_type->kind == KIND_UNION) {
    LLVMTypeRef owner_ty = cg_type_get_llvm(cg, owner);
    LLVMValueRef owner_ptr = LLVMBuildBitCast(
        cg->builder, obj_ptr, LLVMPointerType(owner_ty, 0), "union_owner_ptr");
    LLVMTypeRef field_type_llvm = cg_type_get_llvm(cg, m->type);
    if (owner == obj_type || owner->kind == KIND_UNION) {
      LLVMValueRef field_ptr = LLVMBuildBitCast(
          cg->builder, owner_ptr, LLVMPointerType(field_type_llvm, 0),
          "union_field_ptr");
      return LLVMBuildLoad2(cg->builder, field_type_llvm, field_ptr,
                            "field_load");
    }
    LLVMValueRef field_ptr = LLVMBuildStructGEP2(
        cg->builder, owner_ty, owner_ptr, m->index, "union_field_ptr");
    return LLVMBuildLoad2(cg->builder, field_type_llvm, field_ptr,
                          "field_load");
  }

  LLVMTypeRef obj_type_llvm = cg_type_get_llvm(cg, obj_type);
  LLVMTypeRef field_type_llvm = cg_type_get_llvm(cg, m->type);
  LLVMValueRef field_ptr = LLVMBuildStructGEP2(cg->builder, obj_type_llvm,
                                               obj_ptr, m->index, "field_gep");
  LLVMValueRef field_val =
      LLVMBuildLoad2(cg->builder, field_type_llvm, field_ptr, "field_load");
  return field_val;
}

int cg_result_error_tag_index(Type *result_type, Type *error_type) {
  if (!result_type || result_type->kind != KIND_RESULT ||
      !result_type->data.result.error_set)
    return 0;
  Type *error_set = result_type->data.result.error_set;
  for (size_t i = 0; i < error_set->members.len; i++) {
    Member *m = error_set->members.items[i];
    if (type_equals(m->type, error_type))
      return (int)(i + 1);
  }
  return 0;
}
