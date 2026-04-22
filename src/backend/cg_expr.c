#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>

#include "cg_internal.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"
#include "tyl/sema.h"
#include "tyl/utils.h"
#include "llvm-c/Types.h"

static LLVMValueRef cg_make_param_addressable(Codegen *cg, CGSymbol *sym) {
  LLVMValueRef alloca =
      cg_alloca_in_entry_uninitialized(cg, sym->type, sym->name);
  LLVMBuildStore(cg->builder, sym->value, alloca);
  sym->value = alloca;
  sym->is_direct_value = false;
  return alloca;
}

LLVMValueRef cg_expression_addr(Codegen *cg, AstNode *node) {
  switch (node->tag) {
  case NODE_VAR: {
    // Find the alloca or direct parameter value in our symbol table
    CGSymbol *sym = CGSymbolTable_find(cg->current_scope, node->var.value);
    if (!sym)
      panic("Undefined variable in codegen: " SV_FMT, SV_ARG(node->var.value));
    if (sym->is_direct_value)
      return cg_make_param_addressable(cg, sym);
    return sym->value; // Return the alloca pointer
  }

  case NODE_FIELD: {
    // Get the address of the parent struct or pointer-to-struct
    LLVMValueRef obj_ptr = cg_expression_addr(cg, node->field.object);
    Type *obj_type = node->field.object->resolved_type;

    if (obj_type && obj_type->kind == KIND_POINTER) {
      if (!obj_type->data.pointer_to) {
        panic("Pointer object has no target type");
      }
      LLVMTypeRef load_ty = cg_get_llvm_type(cg, obj_type);
      obj_ptr = LLVMBuildLoad2(cg->builder, load_ty, obj_ptr, "deref_ptr");
      obj_type = obj_type->data.pointer_to;
    }

    // Find member index
    int index = -1;
    for (size_t i = 0; i < obj_type->members.len; i++) {
      Member *m = obj_type->members.items[i];
      if (sv_eq_cstr(node->field.field, m->name)) {
        index = i;
        break;
      }
    }

    if (index < 0) {
      panic("Field '%.*s' not found on type %s", (int)node->field.field.len,
            node->field.field.data, type_to_name(obj_type));
    }

    return LLVMBuildStructGEP2(cg->builder, cg_get_llvm_type(cg, obj_type),
                               obj_ptr, index, "field_addr");
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
        cg_get_llvm_type(cg, node->index.array->resolved_type);

    // 1. Load rank
    LLVMValueRef rank_ptr = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 3, "rank_ptr");
    LLVMValueRef rank_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), rank_ptr, "rank_val");

    // 2. Load dims pointer
    LLVMValueRef dims_ptr_ptr = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 4, "dims_ptr_ptr");
    LLVMValueRef dims_ptr = LLVMBuildLoad2(
        cg->builder, LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0),
        dims_ptr_ptr, "dims_ptr");

    // 3. Current index
    LLVMValueRef index_val = cg_expression(cg, node->index.index);
    LLVMValueRef index_i64 =
        cg_cast_value(cg, index_val, node->index.index->resolved_type,
                      LLVMInt64TypeInContext(cg->context));

    // 4. Bounds check: index_i64 < dims[0]
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

    // Fail block
    LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
    // Call panic("Panic: Array Index Out of Bounds")
    CGFunction *panic_fn =
        cg_find_system_function(cg, sv_from_parts("panic", 5));
    if (!panic_fn) {
      panic("Internal error: system panic helper missing");
    }
    LLVMValueRef msg_ptr = LLVMBuildGlobalStringPtr(
        cg->builder, "Panic: Array Index Out of Bounds", "bounds_err_msg");
    LLVMBuildCall2(cg->builder, panic_fn->type, panic_fn->value,
                   (LLVMValueRef[]){msg_ptr}, 1, "");
    LLVMBuildUnreachable(cg->builder);

    // Ok block
    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);

    LLVMValueRef data_ptr_gep = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 0, "data_field_ptr");
    LLVMTypeRef elem_ptr_ty = cg_get_llvm_type(
        cg, type_get_pointer(cg->type_ctx, node->resolved_type));
    LLVMValueRef actual_data_ptr =
        LLVMBuildLoad2(cg->builder, elem_ptr_ty, data_ptr_gep, "load_data_ptr");

    LLVMValueRef element_addr = LLVMBuildGEP2(
        cg->builder, cg_get_llvm_type(cg, node->resolved_type), actual_data_ptr,
        (LLVMValueRef[]){index_i64}, 1, "element_ptr");

    // If the element is itself an array struct, return its address directly.
    // The load path will adjust rank/dims as needed for the resulting view.
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
    // Use the type to determine the correct alloca size
    LLVMValueRef temp_alloca = cg_alloca_in_entry(
        cg, node->resolved_type, sv_from_cstr("temp_expr_spill"));
    LLVMBuildStore(cg->builder, val, temp_alloca);
    return temp_alloca;
  }
  }
}

static LLVMTypeRef cg_const_target_type(Codegen *cg, AstNode *node) {
  if (node->resolved_type)
    return cg_get_llvm_type(cg, node->resolved_type);

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
    return cg_get_llvm_type(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
  case NODE_NULL:
    return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  default:
    panic("cg_const_expr received unknown constant node without resolved type");
  }
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
      cg_get_llvm_type(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
  LLVMValueRef len =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), name_sv.len, false);
  LLVMValueRef fields[] = {ptr, len};
  return LLVMConstNamedStruct(string_ty, fields, 2);
}

static LLVMValueRef cg_const_expr(Codegen *cg, AstNode *node) {
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

static LLVMValueRef cg_new_expr(Codegen *cg, AstNode *node) {
  Type *target_type = node->new_expr.target_type;
  if (!target_type)
    panic("new expression missing target type");

  if (target_type->kind == KIND_ERROR) {
    if (node->new_expr.args.len > 0 && node->new_expr.field_inits.len > 0) {
      panic("Cannot mix constructor arguments and error literal fields");
    }

    LLVMTypeRef struct_ty = cg_get_llvm_type(cg, target_type);
    LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, struct_ty, "error_tmp");
    LLVMBuildStore(cg->builder, LLVMConstNull(struct_ty), tmp);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
      AstNode *assign = node->new_expr.field_inits.items[i];
      if (assign->tag != NODE_ASSIGN_EXPR ||
          assign->assign_expr.target->tag != NODE_VAR) {
        panic("Invalid error field initializer in codegen");
      }
      StringView field_name = assign->assign_expr.target->var.value;
      Member *member = type_get_member(target_type, field_name);
      if (!member)
        panic("Field '%.*s' not found on type %s", (int)field_name.len,
              field_name.data, type_to_name(target_type));
      LLVMValueRef field_ptr = LLVMBuildStructGEP2(cg->builder, struct_ty, tmp,
                                                   member->index, "field_ptr");
      LLVMValueRef value = cg_expression(cg, assign->assign_expr.value);
      LLVMTypeRef field_ty = cg_get_llvm_type(cg, member->type);
      LLVMValueRef casted = cg_cast_value(
          cg, value, assign->assign_expr.value->resolved_type, field_ty);
      LLVMBuildStore(cg->builder, casted, field_ptr);
    }
    return LLVMBuildLoad2(cg->builder, struct_ty, tmp, "error_value");
  }

  LLVMTypeRef ptr_ty =
      cg_get_llvm_type(cg, type_get_pointer(cg->type_ctx, target_type));
  CGFunction *malloc_fn = cg_find_function(cg, sv_from_cstr("malloc"));
  if (!malloc_fn)
    panic("malloc not found for new expression");

  LLVMValueRef size_arg = LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                                       target_type->size, false);
  LLVMValueRef malloc_args[] = {size_arg};
  LLVMValueRef raw_ptr =
      LLVMBuildCall2(cg->builder, malloc_fn->type, malloc_fn->value,
                     malloc_args, 1, "malloc_res");
  LLVMValueRef obj_ptr = cg_cast_value(cg, raw_ptr, NULL, ptr_ty);

  if (node->new_expr.args.len > 0) {
    Symbol *constructor = cg_find_method(target_type, sv_from_cstr("init"));
    if (!constructor || !constructor->name.data)
      panic("Missing constructor 'init' for type %s",
            type_to_name(target_type));
    CGFunction *fn = cg_find_function(cg, constructor->name);
    if (!fn)
      panic("Constructor function not found for %s", type_to_name(target_type));

    unsigned param_count = LLVMCountParams(fn->value);
    unsigned arg_count = (unsigned)node->new_expr.args.len + 1;
    LLVMTypeRef *param_types =
        param_count > 0 ? malloc(sizeof(LLVMTypeRef) * param_count) : NULL;
    if (param_count > 0)
      LLVMGetParamTypes(fn->type, param_types);

    LLVMValueRef *args = xmalloc(sizeof(LLVMValueRef) * arg_count);
    args[0] = obj_ptr;
    for (unsigned i = 0; i < node->new_expr.args.len; i++) {
      AstNode *arg_node = node->new_expr.args.items[i];
      LLVMValueRef arg_val = cg_expression(cg, arg_node);
      LLVMTypeRef param_ty = param_types[i + 1];
      args[i + 1] =
          cg_cast_value(cg, arg_val, arg_node->resolved_type, param_ty);
    }

    LLVMBuildCall2(cg->builder, fn->type, fn->value, args, arg_count, "");
    if (param_types)
      free(param_types);
    free(args);
    return obj_ptr;
  }

  if (node->new_expr.field_inits.len > 0) {
    LLVMTypeRef struct_ty = cg_get_llvm_type(cg, target_type);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
      AstNode *assign = node->new_expr.field_inits.items[i];
      if (assign->tag != NODE_ASSIGN_EXPR ||
          assign->assign_expr.target->tag != NODE_VAR) {
        panic("Invalid struct field initializer in codegen");
      }
      StringView field_name = assign->assign_expr.target->var.value;
      Member *member = type_get_member(target_type, field_name);
      if (!member)
        panic("Field '%.*s' not found on type %s", (int)field_name.len,
              field_name.data, type_to_name(target_type));
      LLVMValueRef field_ptr = LLVMBuildStructGEP2(
          cg->builder, struct_ty, obj_ptr, member->index, "field_ptr");
      LLVMValueRef value = cg_expression(cg, assign->assign_expr.value);
      LLVMTypeRef field_ty = cg_get_llvm_type(cg, member->type);
      LLVMValueRef casted = cg_cast_value(
          cg, value, assign->assign_expr.value->resolved_type, field_ty);
      LLVMBuildStore(cg->builder, casted, field_ptr);
    }
  }

  return obj_ptr;
}

static LLVMValueRef cg_var_expr(Codegen *cg, AstNode *node) {
  CGSymbol *cgsym = CGSymbolTable_find(cg->current_scope, node->var.value);
  if (!node->resolved_type) {
    if (cgsym) {
      node->resolved_type = cgsym->type;
    }
  }

  if (!cgsym && node->resolved_type &&
      node->resolved_type->kind == KIND_ERROR) {
    LLVMTypeRef ty = cg_get_llvm_type(cg, node->resolved_type);
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
  LLVMTypeRef type = cg_get_llvm_type(cg, node->resolved_type);
  return LLVMBuildLoad2(cg->builder, type, ptr, "var_load");
}

static int cg_tagged_union_variant_index(Type *union_type, Type *variant) {
  if (!union_type || union_type->kind != KIND_UNION ||
      !union_type->is_tagged_union)
    return -1;
  for (size_t i = 0; i < union_type->members.len; i++) {
    Member *m = union_type->members.items[i];
    if (type_equals(m->type, variant))
      return (int)i;
  }
  return -1;
}

static int cg_tagged_union_variant_index_llvm(Codegen *cg, Type *union_type,
                                              LLVMTypeRef llvm_ty) {
  if (!union_type || union_type->kind != KIND_UNION ||
      !union_type->is_tagged_union)
    return -1;
  for (size_t i = 0; i < union_type->members.len; i++) {
    Member *m = union_type->members.items[i];
    if (cg_get_llvm_type(cg, m->type) == llvm_ty)
      return (int)i;
  }
  return -1;
}

LLVMValueRef cg_make_tagged_union(Codegen *cg, LLVMValueRef val, Type *from_t,
                                  Type *union_t) {
  int variant_index = cg_tagged_union_variant_index(union_t, from_t);
  if (variant_index < 0)
    return val;

  LLVMTypeRef union_ty = cg_get_llvm_type(cg, union_t);
  LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, union_ty, "tagged_union_tmp");
  LLVMValueRef tag_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 0,
                                             "tagged_union_tag_ptr");
  LLVMBuildStore(
      cg->builder,
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), variant_index, 0),
      tag_ptr);

  LLVMValueRef payload_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 1,
                                                 "tagged_union_payload_ptr");
  LLVMTypeRef from_ty = cg_get_llvm_type(cg, from_t);
  LLVMValueRef store_ptr =
      LLVMBuildBitCast(cg->builder, payload_ptr, LLVMPointerType(from_ty, 0),
                       "tagged_union_payload_cast");
  LLVMBuildStore(cg->builder, val, store_ptr);
  return LLVMBuildLoad2(cg->builder, union_ty, tmp, "tagged_union_val");
}

static LLVMValueRef cg_extract_tagged_union(Codegen *cg, LLVMValueRef union_val,
                                            Type *union_t, Type *target_t,
                                            LLVMTypeRef target_ty) {
  int variant_index =
      cg_tagged_union_variant_index_llvm(cg, union_t, target_ty);
  if (variant_index < 0)
    return union_val;

  LLVMTypeRef union_ty = cg_get_llvm_type(cg, union_t);
  LLVMValueRef tag =
      LLVMBuildExtractValue(cg->builder, union_val, 0, "tagged_union_tag");
  LLVMValueRef expected_tag =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), variant_index, 0);
  LLVMValueRef cond = LLVMBuildICmp(cg->builder, LLVMIntEQ, tag, expected_tag,
                                    "tagged_union_check");

  LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "tagged_union_ok");
  LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "tagged_union_fail");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "tagged_union_merge");

  LLVMBuildCondBr(cg->builder, cond, ok_bb, fail_bb);

  LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
  LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, union_ty, "tagged_union_tmp");
  LLVMBuildStore(cg->builder, union_val, tmp);
  LLVMValueRef payload_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 1,
                                                 "tagged_union_payload_ptr");
  LLVMValueRef data_ptr =
      LLVMBuildBitCast(cg->builder, payload_ptr, LLVMPointerType(target_ty, 0),
                       "tagged_union_data_ptr");
  LLVMValueRef result =
      LLVMBuildLoad2(cg->builder, target_ty, data_ptr, "tagged_union_value");
  LLVMBuildBr(cg->builder, merge_bb);
  LLVMBasicBlockRef ok_bb_end = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
  CGFunction *panic_fn = cg_find_system_function(cg, sv_from_parts("panic", 5));
  if (!panic_fn) {
    panic("Internal error: system panic helper missing");
  }
  const char *target_name = target_t ? type_to_name(target_t) : "<unknown>";
  const char *union_name = type_to_name(union_t);
  char panic_msg[256];
  snprintf(panic_msg, sizeof(panic_msg),
           "Unwrapping tagged union %s as %s failed: the runtime tag does "
           "not match the requested variant",
           union_name, target_name);
  LLVMValueRef msg_ptr =
      LLVMBuildGlobalStringPtr(cg->builder, panic_msg, "tagged_union_err_msg");
  LLVMBuildCall2(cg->builder, panic_fn->type, panic_fn->value,
                 (LLVMValueRef[]){msg_ptr}, 1, "");
  LLVMBuildUnreachable(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, target_ty, "tagged_union_phi");
  LLVMAddIncoming(phi, &result, &ok_bb_end, 1);
  return phi;
}

static LLVMValueRef cg_arith_expr(Codegen *cg, LLVMValueRef lhs,
                                  LLVMValueRef rhs, ArithmOp op) {
  LLVMTypeRef type = LLVMTypeOf(lhs);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  bool is_float = (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind);

  switch (op) {
  case OP_ADD:
    return is_float ? LLVMBuildFAdd(cg->builder, lhs, rhs, "faddtmp")
                    : LLVMBuildAdd(cg->builder, lhs, rhs, "addtmp");
  case OP_SUB:
    return is_float ? LLVMBuildFSub(cg->builder, lhs, rhs, "fsubtmp")
                    : LLVMBuildSub(cg->builder, lhs, rhs, "subtmp");
  case OP_MUL:
    return is_float ? LLVMBuildFMul(cg->builder, lhs, rhs, "fmultmp")
                    : LLVMBuildMul(cg->builder, lhs, rhs, "multmp");
  case OP_DIV:
    return is_float ? LLVMBuildFDiv(cg->builder, lhs, rhs, "fdivtmp")
                    : LLVMBuildSDiv(cg->builder, lhs, rhs, "sdivtmp");
  case OP_MOD:
    return is_float ? LLVMBuildFRem(cg->builder, lhs, rhs, "fremtmp")
                    : LLVMBuildSRem(cg->builder, lhs, rhs, "sremtmp");
  case OP_POW: {
    CGFunction *pow_fn = cg_find_function(cg, sv_from_parts("pow", 3));
    LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
    lhs = cg_cast_value(cg, lhs, NULL, double_ty);
    rhs = cg_cast_value(cg, rhs, NULL, double_ty);
    LLVMValueRef args[] = {lhs, rhs};
    return LLVMBuildCall2(cg->builder, pow_fn->type, pow_fn->value, args, 2,
                          "powtmp");
  }
  default:
    return NULL;
  }
}

static LLVMValueRef cg_compare_expr(Codegen *cg, LLVMValueRef lhs,
                                    LLVMValueRef rhs, CompareOp op) {
  LLVMTypeRef type = LLVMTypeOf(lhs);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  bool is_float = (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind);

  if (is_float) {
    LLVMRealPredicate pred;
    switch (op) {
    case OP_LT:
      pred = LLVMRealOLT;
      break;
    case OP_GT:
      pred = LLVMRealOGT;
      break;
    case OP_LE:
      pred = LLVMRealOLE;
      break;
    case OP_GE:
      pred = LLVMRealOGE;
      break;
    default:
      return NULL;
    }
    return LLVMBuildFCmp(cg->builder, pred, lhs, rhs, "fcmptmp");
  } else {
    LLVMIntPredicate pred;
    switch (op) {
    case OP_LT:
      pred = LLVMIntSLT;
      break;
    case OP_GT:
      pred = LLVMIntSGT;
      break;
    case OP_LE:
      pred = LLVMIntSLE;
      break;
    case OP_GE:
      pred = LLVMIntSGE;
      break;
    default:
      return NULL;
    }
    return LLVMBuildICmp(cg->builder, pred, lhs, rhs, "icmptmp");
  }
}

static LLVMValueRef cg_logical_expr(Codegen *cg, AstNode *node,
                                    LLVMValueRef lhs) {
  LLVMBasicBlockRef original_block = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef function = cg->current_function;

  LLVMBasicBlockRef rhs_block =
      LLVMAppendBasicBlockInContext(cg->context, function, "log_rhs");
  LLVMBasicBlockRef merge_block =
      LLVMAppendBasicBlockInContext(cg->context, function, "log_merge");

  if (node->binary_logical.op == OP_AND) {
    LLVMBuildCondBr(cg->builder, lhs, rhs_block, merge_block);
  } else {
    LLVMBuildCondBr(cg->builder, lhs, merge_block, rhs_block);
  }

  LLVMPositionBuilderAtEnd(cg->builder, rhs_block);
  LLVMValueRef rhs = cg_expression(cg, node->binary_logical.right);
  LLVMBuildBr(cg->builder, merge_block);
  LLVMBasicBlockRef rhs_end_block = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_block);
  LLVMValueRef phi =
      LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->context), "log_phi");

  LLVMValueRef incoming_values[2];
  LLVMBasicBlockRef incoming_blocks[2];

  incoming_values[0] = LLVMConstInt(LLVMInt1TypeInContext(cg->context),
                                    (node->binary_logical.op == OP_OR), false);
  incoming_blocks[0] = original_block;
  incoming_values[1] = rhs;
  incoming_blocks[1] = rhs_end_block;

  LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
  return phi;
}

static LLVMValueRef cg_binary_expr(Codegen *cg, AstNode *node) {
  AstNode *left_node, *right_node;
  switch (node->tag) {
  case NODE_BINARY_ARITH:
    left_node = node->binary_arith.left;
    right_node = node->binary_arith.right;
    break;
  case NODE_BINARY_COMPARE:
    left_node = node->binary_compare.left;
    right_node = node->binary_compare.right;
    break;
  case NODE_BINARY_EQUALITY:
    left_node = node->binary_equality.left;
    right_node = node->binary_equality.right;
    break;
  case NODE_BINARY_LOGICAL:
    left_node = node->binary_logical.left;
    right_node = node->binary_logical.right;
    break;
  default:
    return NULL;
  }

  LLVMValueRef lhs = cg_expression(cg, left_node);
  if (!lhs)
    return NULL;

  if (node->tag == NODE_BINARY_LOGICAL) {
    LLVMTypeRef i1_ty = LLVMInt1TypeInContext(cg->context);
    lhs = cg_cast_value(cg, lhs, left_node->resolved_type, i1_ty);
    return cg_logical_expr(cg, node, lhs);
  }

  LLVMValueRef rhs = cg_expression(cg, right_node);
  if (!rhs)
    return NULL;

  if (node->tag == NODE_BINARY_ARITH) {
    LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->resolved_type);

    LLVMTypeKind lhs_kind = LLVMGetTypeKind(LLVMTypeOf(lhs));
    LLVMTypeKind rhs_kind = LLVMGetTypeKind(LLVMTypeOf(rhs));

    if ((lhs_kind == LLVMPointerTypeKind && rhs_kind == LLVMIntegerTypeKind) ||
        (rhs_kind == LLVMPointerTypeKind && lhs_kind == LLVMIntegerTypeKind)) {
      LLVMValueRef ptr_val = lhs_kind == LLVMPointerTypeKind ? lhs : rhs;
      LLVMValueRef int_val = lhs_kind == LLVMIntegerTypeKind ? lhs : rhs;

      Type *ptr_sem_type = lhs_kind == LLVMPointerTypeKind
                               ? left_node->resolved_type
                               : right_node->resolved_type;
      Type *elem_sem_type = NULL;
      if (ptr_sem_type && ptr_sem_type->kind == KIND_POINTER) {
        elem_sem_type = ptr_sem_type->data.pointer_to;
      }
      LLVMTypeRef elem_ty = elem_sem_type
                                ? cg_get_llvm_type(cg, elem_sem_type)
                                : LLVMGetElementType(LLVMTypeOf(ptr_val));

      LLVMValueRef index = cg_cast_value(cg, int_val,
                                         lhs_kind == LLVMIntegerTypeKind
                                             ? left_node->resolved_type
                                             : right_node->resolved_type,
                                         LLVMInt64TypeInContext(cg->context));
      if (node->binary_arith.op == OP_ADD) {
        return LLVMBuildGEP2(cg->builder, elem_ty, ptr_val,
                             (LLVMValueRef[]){index}, 1, "ptraddtmp");
      }
      if (node->binary_arith.op == OP_SUB && lhs_kind == LLVMPointerTypeKind) {
        LLVMValueRef neg_index = LLVMBuildNeg(cg->builder, index, "negidx");
        return LLVMBuildGEP2(cg->builder, elem_ty, ptr_val,
                             (LLVMValueRef[]){neg_index}, 1, "ptrsubtmp");
      }
    }

    lhs = cg_cast_value(cg, lhs, left_node->resolved_type, target_ty);
    rhs = cg_cast_value(cg, rhs, right_node->resolved_type, target_ty);
    return cg_arith_expr(cg, lhs, rhs, node->binary_arith.op);
  }

  cg_binary_sync_types(cg, &lhs, left_node->resolved_type, &rhs,
                       right_node->resolved_type);

  switch (node->tag) {
  case NODE_BINARY_COMPARE:
    return cg_compare_expr(cg, lhs, rhs, node->binary_compare.op);
  case NODE_BINARY_EQUALITY: {
    Type *lt = left_node->resolved_type;
    Type *rt = right_node->resolved_type;
    LLVMValueRef lh = cg_coerce_rvalue_to_str_slice(cg, lhs, lt);
    LLVMValueRef rh = cg_coerce_rvalue_to_str_slice(cg, rhs, rt);
    return cg_equality_expr(cg, lh, rh, node->binary_equality.op, lt, rt);
  }
  default:
    return NULL;
  }
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

static LLVMValueRef cg_binary_is_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef left = cg_expression(cg, node->binary_is.left);
  if (!left)
    return NULL;

  Type *left_type = node->binary_is.left->resolved_type;
  Type *right_type = node->binary_is.right->resolved_type;
  LLVMTypeRef bool_ty = LLVMInt1TypeInContext(cg->context);

  if (left_type && left_type->kind == KIND_RESULT) {
    LLVMValueRef tag =
        LLVMBuildExtractValue(cg->builder, left, 1, "result_tag");
    LLVMTypeRef tag_ty = LLVMInt16TypeInContext(cg->context);
    if (node->binary_is.right->tag == NODE_VAR &&
        sv_eq_cstr(node->binary_is.right->var.value, "Error")) {
      return LLVMBuildICmp(cg->builder, LLVMIntNE, tag,
                           LLVMConstInt(tag_ty, 0, false), "is_error");
    }
    if (right_type && right_type->kind == KIND_ERROR) {
      int idx = cg_result_error_tag_index(left_type, right_type);
      return LLVMBuildICmp(cg->builder, LLVMIntEQ, tag,
                           LLVMConstInt(tag_ty, idx, false), "is_error_type");
    }
    if (right_type && right_type->kind == KIND_ERROR_SET) {
      return LLVMBuildICmp(cg->builder, LLVMIntNE, tag,
                           LLVMConstInt(tag_ty, 0, false), "is_error_set");
    }
  }

  if (left_type && left_type->kind == KIND_ERROR) {
    if (node->binary_is.right->tag == NODE_VAR &&
        sv_eq_cstr(node->binary_is.right->var.value, "Error")) {
      return LLVMConstInt(bool_ty, 1, false);
    }
    if (right_type && right_type->kind == KIND_ERROR_SET) {
      return LLVMConstInt(bool_ty, 1, false);
    }
    if (right_type && right_type->kind == KIND_ERROR) {
      LLVMValueRef right = cg_expression(cg, node->binary_is.right);
      return cg_equality_expr(cg, left, right, OP_EQ, left_type, right_type);
    }
  }

  return LLVMConstInt(bool_ty, 0, false);
}

static LLVMValueRef cg_binary_else_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef left = cg_expression(cg, node->binary_else.left);
  if (!left)
    return NULL;
  Type *left_type = node->binary_else.left->resolved_type;
  if (!left_type || left_type->kind != KIND_RESULT)
    return NULL;

  LLVMValueRef tag = LLVMBuildExtractValue(cg->builder, left, 1, "result_tag");
  LLVMTypeRef tag_ty = LLVMInt16TypeInContext(cg->context);
  LLVMValueRef is_error =
      LLVMBuildICmp(cg->builder, LLVMIntNE, tag, LLVMConstInt(tag_ty, 0, false),
                    "result_is_error");

  LLVMValueRef right = cg_expression(cg, node->binary_else.right);
  LLVMValueRef success_raw =
      LLVMBuildExtractValue(cg->builder, left, 0, "result_success");
  LLVMValueRef success_val =
      cg_cast_value(cg, success_raw, left_type->data.result.success,
                    cg_get_llvm_type(cg, node->resolved_type));

  return LLVMBuildSelect(cg->builder, is_error, right, success_val, "else_val");
}

static LLVMValueRef cg_unary_expr(Codegen *cg, AstNode *node) {
  UnaryOp op = node->unary.op;
  AstNode *expr = node->unary.expr;
  LLVMTypeRef res_ty = NULL;
  if (op == OP_NEG || op == OP_DEREF) {
    if (!node->resolved_type) {
      panic("Unary expression has no resolved type");
    }
    res_ty = cg_get_llvm_type(cg, node->resolved_type);
  }

  if (op == OP_NEG) {
    LLVMValueRef val = cg_expression(cg, expr);
    val = cg_cast_value(cg, val, expr->resolved_type, res_ty);
    if (LLVMGetTypeKind(res_ty) == LLVMDoubleTypeKind ||
        LLVMGetTypeKind(res_ty) == LLVMFloatTypeKind) {
      return LLVMBuildFNeg(cg->builder, val, "fnegtmp");
    }
    return LLVMBuildNeg(cg->builder, val, "negtmp");
  }

  if (op == OP_NOT) {
    LLVMValueRef val = cg_expression(cg, expr);
    LLVMValueRef bool_val = cg_cast_value(cg, val, expr->resolved_type,
                                          LLVMInt1TypeInContext(cg->context));
    return LLVMBuildNot(cg->builder, bool_val, "nottmp");
  }

  if (op == OP_DEREF) {
    LLVMValueRef ptr = cg_expression(cg, expr);
    return LLVMBuildLoad2(cg->builder, res_ty, ptr, "deref_load");
  }

  if (op == OP_ADDR_OF) {
    return cg_get_address(cg, expr);
  }

  LLVMValueRef ptr = cg_get_address(cg, expr);
  if (!ptr)
    panic("Invalid unary operand target");

  LLVMTypeRef type = LLVMGetAllocatedType(ptr);
  LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, type, ptr, "u_load");
  LLVMValueRef one = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind ||
                      LLVMGetTypeKind(type) == LLVMFloatTypeKind)
                         ? LLVMConstReal(type, 1.0)
                         : LLVMConstInt(type, 1, false);

  LLVMValueRef new_val;
  if (op == OP_PRE_INC || op == OP_POST_INC) {
    new_val = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind ||
               LLVMGetTypeKind(type) == LLVMFloatTypeKind)
                  ? LLVMBuildFAdd(cg->builder, old_val, one, "inc")
                  : LLVMBuildAdd(cg->builder, old_val, one, "inc");
  } else {
    new_val = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind ||
               LLVMGetTypeKind(type) == LLVMFloatTypeKind)
                  ? LLVMBuildFSub(cg->builder, old_val, one, "dec")
                  : LLVMBuildSub(cg->builder, old_val, one, "dec");
  }

  LLVMBuildStore(cg->builder, new_val, ptr);
  return (op == OP_PRE_INC || op == OP_PRE_DEC) ? new_val : old_val;
}

static LLVMValueRef cg_ternary_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef cond = cg_expression(cg, node->ternary.condition);
  LLVMTypeRef i1_ty = LLVMInt1TypeInContext(cg->context);
  cond = cg_cast_value(cg, cond, node->ternary.condition->resolved_type, i1_ty);

  LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "ternary_then");
  LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "ternary_else");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "ternary_cont");

  LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);
  LLVMTypeRef res_type = cg_get_llvm_type(cg, node->resolved_type);

  LLVMPositionBuilderAtEnd(cg->builder, then_bb);
  LLVMValueRef then_val = cg_expression(cg, node->ternary.true_expr);
  then_val = cg_cast_value(cg, then_val, node->ternary.true_expr->resolved_type,
                           res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  LLVMBasicBlockRef then_bb_end = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, else_bb);
  LLVMValueRef else_val = cg_expression(cg, node->ternary.false_expr);
  else_val = cg_cast_value(cg, else_val,
                           node->ternary.false_expr->resolved_type, res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  LLVMBasicBlockRef else_bb_end = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, res_type, "ternary_tmp");
  LLVMValueRef incoming_values[] = {then_val, else_val};
  LLVMBasicBlockRef incoming_blocks[] = {then_bb_end, else_bb_end};
  LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
  return phi;
}

static LLVMValueRef cg_assign_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef ptr = cg_get_address(cg, node->assign_expr.target);
  if (!ptr)
    panic("Invalid assignment target");
  LLVMValueRef val = cg_expression(cg, node->assign_expr.value);

  Type *target_type = node->assign_expr.target->resolved_type;
  if (!target_type) {
    AstNode *target = node->assign_expr.target;
    if (target->tag == NODE_VAR) {
      CGSymbol *sym = CGSymbolTable_find(cg->current_scope, target->var.value);
      if (sym)
        target_type = sym->type;
    } else if (target->tag == NODE_FIELD) {
      Type *obj_type = target->field.object->resolved_type;
      if (!obj_type && target->field.object->tag == NODE_VAR) {
        CGSymbol *sym = CGSymbolTable_find(cg->current_scope,
                                           target->field.object->var.value);
        if (sym)
          obj_type = sym->type;
      }
      if (obj_type && obj_type->kind == KIND_STRUCT) {
        Member *m = type_get_member(obj_type, target->field.field);
        if (m)
          target_type = m->type;
      } else if (obj_type && obj_type->kind == KIND_UNION) {
        Type *owner = NULL;
        Member *m =
            type_find_union_field(obj_type, target->field.field, &owner);
        if (m)
          target_type = m->type;
      }
    } else if (target->tag == NODE_INDEX) {
      Type *array_type = target->index.array->resolved_type;
      if (array_type) {
        if (array_type->kind == KIND_POINTER) {
          target_type = array_type->data.pointer_to;
        } else if (type_is_array_struct(array_type) &&
                   array_type->data.instance.generic_args.len > 0) {
          target_type = array_type->data.instance.generic_args.items[0];
        }
      }
    }
  }

  if (!target_type) {
    target_type = node->assign_expr.target->resolved_type;
  }
  if (!target_type) {
    panic("Assignment target has no resolved type");
  }
  if (!node->assign_expr.value->resolved_type) {
    panic("Assignment RHS has no resolved type");
  }
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, target_type);
  if (target_type->kind == KIND_UNION && target_type->is_tagged_union) {
    int variant_index = cg_tagged_union_variant_index(
        target_type, node->assign_expr.value->resolved_type);
    if (variant_index >= 0) {
      val = cg_make_tagged_union(
          cg, val, node->assign_expr.value->resolved_type, target_type);
    } else {
      val = cg_cast_value(cg, val, node->assign_expr.value->resolved_type,
                          target_ty);
    }
  } else {
    val = cg_cast_value(cg, val, node->assign_expr.value->resolved_type,
                        target_ty);
  }

  if (node->assign_expr.target->tag == NODE_INDEX) {
    Type *array_type = node->assign_expr.target->index.array->resolved_type;
    if (type_is_array_struct(array_type)) {
      LLVMValueRef array_struct_ptr =
          cg_get_address(cg, node->assign_expr.target->index.array);
      if (!array_struct_ptr)
        panic("Invalid array assignment target");

      LLVMTypeRef array_ty = cg_get_llvm_type(cg, array_type);
      LLVMValueRef len_ptr = LLVMBuildStructGEP2(
          cg->builder, array_ty, array_struct_ptr, 1, "array_len_ptr");
      LLVMValueRef curr_len =
          LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                         len_ptr, "array_curr_len");
      LLVMValueRef idx_val =
          cg_expression(cg, node->assign_expr.target->index.index);
      LLVMValueRef idx_i64 = cg_cast_value(
          cg, idx_val, node->assign_expr.target->index.index->resolved_type,
          LLVMInt64TypeInContext(cg->context));
      LLVMValueRef is_append = LLVMBuildICmp(cg->builder, LLVMIntEQ, idx_i64,
                                             curr_len, "array_is_append");
      LLVMBasicBlockRef append_bb = LLVMAppendBasicBlockInContext(
          cg->context, cg->current_function, "array_append");
      LLVMBasicBlockRef skip_append_bb = LLVMAppendBasicBlockInContext(
          cg->context, cg->current_function, "array_skip_append");
      LLVMBasicBlockRef append_merge_bb = LLVMAppendBasicBlockInContext(
          cg->context, cg->current_function, "array_append_merge");
      LLVMBuildCondBr(cg->builder, is_append, append_bb, skip_append_bb);

      LLVMPositionBuilderAtEnd(cg->builder, append_bb);
      LLVMValueRef next_len =
          LLVMBuildAdd(cg->builder, curr_len,
                       LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0),
                       "array_next_len");
      LLVMBuildStore(cg->builder, next_len, len_ptr);
      LLVMBuildBr(cg->builder, append_merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, skip_append_bb);
      LLVMBuildBr(cg->builder, append_merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, append_merge_bb);
    }
  }

  LLVMBuildStore(cg->builder, val, ptr);
  return val;
}

static LLVMValueRef cg_cast_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef val = cg_expression(cg, node->cast_expr.expr);
  Type *expr_type = node->cast_expr.expr->resolved_type;
  Type *target_type = node->cast_expr.target_type;
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, target_type);

  if (expr_type && expr_type->kind == KIND_UNION &&
      expr_type->is_tagged_union) {
    int variant_index =
        cg_tagged_union_variant_index_llvm(cg, expr_type, target_ty);
    if (variant_index >= 0)
      return cg_extract_tagged_union(cg, val, expr_type, target_type,
                                     target_ty);
  }

  if (target_type && target_type->kind == KIND_UNION &&
      target_type->is_tagged_union) {
    int variant_index = cg_tagged_union_variant_index(target_type, expr_type);
    if (variant_index >= 0)
      return cg_make_tagged_union(cg, val, expr_type, target_type);
  }

  return cg_cast_value(cg, val, expr_type, target_ty);
}

static LLVMValueRef cg_call_expr(Codegen *cg, AstNode *node) {
  if (node->call.func->tag != NODE_VAR)
    panic("Function calls must be by name");
  StringView fn_name = node->call.func->var.value;
  if (sv_eq(fn_name, sv_from_cstr("typeof"))) {
    Type *arg_type = NULL;
    if (node->call.args.len > 0) {
      AstNode *arg_node = node->call.args.items[0];
      arg_type = arg_node->resolved_type;
    }
    if (!arg_type)
      arg_type = type_get_primitive(cg->type_ctx, PRIM_UNKNOWN);
    return cg_typeof_string(cg, arg_type);
  }

  CGFunction *fn = cg_find_function(cg, fn_name);
  if (!fn)
    panic("Call to undefined function '" SV_FMT "'", SV_ARG(fn_name));

  unsigned arg_count = (unsigned)node->call.args.len;
  LLVMValueRef *args =
      arg_count > 0 ? malloc(sizeof(LLVMValueRef) * arg_count) : NULL;
  unsigned param_count = LLVMCountParams(fn->value);
  LLVMTypeRef *param_types =
      param_count > 0 ? malloc(sizeof(LLVMTypeRef) * param_count) : NULL;
  if (param_count > 0)
    LLVMGetParamTypes(fn->type, param_types);

  for (unsigned i = 0; i < arg_count; i++) {
    AstNode *arg_node = node->call.args.items[i];
    LLVMValueRef arg_val = cg_expression(cg, arg_node);

    if (i < param_count) {
      args[i] =
          cg_cast_value(cg, arg_val, arg_node->resolved_type, param_types[i]);
    } else {
      args[i] = arg_val;
    }
  }

  LLVMTypeRef ret_ty = LLVMGetReturnType(fn->type);
  const char *call_name =
      (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind) ? "" : "calltmp";
  LLVMValueRef result = LLVMBuildCall2(cg->builder, fn->type, fn->value, args,
                                       arg_count, call_name);

  if (args)
    free(args);
  if (param_types)
    free(param_types);

  if (node->resolved_type) {
    LLVMTypeRef expected_ty = cg_get_llvm_type(cg, node->resolved_type);
    if (expected_ty != ret_ty) {
      result = cg_cast_value(cg, result, NULL, expected_ty);
    }
  }
  return result;
}

static LLVMValueRef cg_field_expr(Codegen *cg, AstNode *node) {
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
    obj_ptr = LLVMBuildAlloca(cg->builder, cg_get_llvm_type(cg, obj_type),
                              "tmp_obj_field");
    LLVMBuildStore(cg->builder, obj_val, obj_ptr);
  } else {
    Type *resolved = obj_node->resolved_type;
    while (resolved && resolved->kind == KIND_POINTER) {
      LLVMTypeRef load_ty = cg_get_llvm_type(cg, resolved);
      obj_ptr = LLVMBuildLoad2(cg->builder, load_ty, obj_ptr, "deref_ptr");
      resolved = resolved->data.pointer_to;
    }
  }

  if (obj_type->kind == KIND_UNION) {
    LLVMTypeRef owner_ty = cg_get_llvm_type(cg, owner);
    LLVMValueRef owner_ptr = LLVMBuildBitCast(
        cg->builder, obj_ptr, LLVMPointerType(owner_ty, 0), "union_owner_ptr");
    LLVMTypeRef field_type_llvm = cg_get_llvm_type(cg, m->type);
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

  LLVMTypeRef obj_type_llvm = cg_get_llvm_type(cg, obj_type);
  LLVMTypeRef field_type_llvm = cg_get_llvm_type(cg, m->type);
  LLVMValueRef field_ptr = LLVMBuildStructGEP2(cg->builder, obj_type_llvm,
                                               obj_ptr, m->index, "field_gep");
  LLVMValueRef field_val =
      LLVMBuildLoad2(cg->builder, field_type_llvm, field_ptr, "field_load");
  return field_val;
}

static LLVMValueRef cg_array_literal(Codegen *cg, AstNode *node) {
  Type *inst_type = node->resolved_type;
  if (!inst_type) {
    if (node->array_literal.items.len == 0)
      panic("Internal error: cannot infer type for empty array literal");

    AstNode *first_item = node->array_literal.items.items[0];
    if (!first_item || !first_item->resolved_type)
      panic("Internal error: cannot infer array element type from first item");

    Type *element_type = first_item->resolved_type;
    Type *array_template =
        type_get_template(cg->type_ctx, sv_from_parts("Array", 5));
    if (!array_template)
      panic("Internal error: Array template not registered in codegen");

    List args;
    List_init(&args);
    List_push(&args, element_type);
    inst_type = type_get_instance(cg->type_ctx, array_template, args);
    List_free(&args, 0);
    node->resolved_type = inst_type;
  }

  size_t count = node->array_literal.items.len;

  // Get the T* type (the type of the 'data' field)
  Member *data_mem = type_get_member(inst_type, sv_from_parts("data", 4));
  if (!data_mem) {
    panic("Internal error: array literal type '%s' has no data member",
          type_to_name(inst_type));
  }
  Type *ptr_type = data_mem->type;
  Type *elem_type = ptr_type->data.pointer_to;

  LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, elem_type);
  LLVMTypeRef array_ty = LLVMArrayType(llvm_elem_ty, count);

  // 1. Allocate space for the elements on the stack
  LLVMValueRef raw_data_ptr =
      LLVMBuildAlloca(cg->builder, array_ty, "array_lit_storage");

  // 2. Initialize the elements, using a single LLVMConstArray store when
  //    all values are constant.
  LLVMValueRef *const_values = xmalloc(sizeof(LLVMValueRef) * count);
  bool all_const = true;
  for (size_t i = 0; i < count; i++) {
    AstNode *item = node->array_literal.items.items[i];
    LLVMValueRef val = cg_expression(cg, item);
    if (!LLVMIsConstant(val)) {
      all_const = false;
    }
    const_values[i] = val;
  }

  if (all_const) {
    LLVMValueRef const_array =
        LLVMConstArray(llvm_elem_ty, const_values, (unsigned)count);
    LLVMBuildStore(cg->builder, const_array, raw_data_ptr);
  } else {
    for (size_t i = 0; i < count; i++) {
      LLVMValueRef indices[] = {
          LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0),
          LLVMConstInt(LLVMInt32TypeInContext(cg->context), i, 0)};
      LLVMValueRef element_ptr = LLVMBuildGEP2(
          cg->builder, array_ty, raw_data_ptr, indices, 2, "item_ptr");
      LLVMBuildStore(cg->builder, const_values[i], element_ptr);
    }
  }
  free(const_values);

  // 3. Create the Array struct { data, len, cap }
  LLVMTypeRef struct_ty = cg_get_llvm_type(cg, inst_type);
  LLVMValueRef array_struct = LLVMGetUndef(struct_ty);

  // Cast raw_data_ptr from [N x T]* to T* for the struct field
  LLVMValueRef data_ptr_cast = LLVMBuildBitCast(
      cg->builder, raw_data_ptr, cg_get_llvm_type(cg, ptr_type), "data_ptr");

  // Insert fields
  array_struct = LLVMBuildInsertValue(cg->builder, array_struct, data_ptr_cast,
                                      0, "set_data");
  array_struct = LLVMBuildInsertValue(
      cg->builder, array_struct,
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, 0), 1,
      "set_len");
  array_struct = LLVMBuildInsertValue(
      cg->builder, array_struct,
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, 0), 2,
      "set_cap");

  // set rank to 1 by default
  LLVMValueRef rank_val =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0);
  LLVMValueRef dims_ptr = NULL;

  if (type_is_array_struct(elem_type)) {
    // Nested arrays: inherit the inner rank and prefix it with this count.
    LLVMTypeRef inner_ty = cg_get_llvm_type(cg, elem_type);
    LLVMTypeRef array_ty = LLVMArrayType(inner_ty, count);
    LLVMValueRef first_elem_ptr = LLVMBuildGEP2(
        cg->builder, array_ty, raw_data_ptr,
        (LLVMValueRef[]){
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0),
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0)},
        2, "first_elem_ptr");

    LLVMValueRef inner_rank_ptr = LLVMBuildStructGEP2(
        cg->builder, inner_ty, first_elem_ptr, 3, "inner_rank_ptr");
    LLVMValueRef inner_rank =
        LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                       inner_rank_ptr, "inner_rank");
    rank_val = LLVMBuildAdd(
        cg->builder, inner_rank,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "outer_rank");

    LLVMValueRef inner_dims_ptr_ptr = LLVMBuildStructGEP2(
        cg->builder, inner_ty, first_elem_ptr, 4, "inner_dims_ptr_ptr");
    LLVMValueRef inner_dims_ptr = LLVMBuildLoad2(
        cg->builder, LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0),
        inner_dims_ptr_ptr, "inner_dims_ptr");

    LLVMValueRef dims_mem =
        LLVMBuildArrayAlloca(cg->builder, LLVMInt64TypeInContext(cg->context),
                             rank_val, "array_lit_dims");

    LLVMValueRef d0_ptr = LLVMBuildGEP2(
        cg->builder, LLVMArrayType(LLVMInt64TypeInContext(cg->context), 0),
        dims_mem,
        (LLVMValueRef[]){
            LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0),
            LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0)},
        2, "d0_ptr");
    LLVMBuildStore(cg->builder,
                   LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, 0),
                   d0_ptr);

    // Copy inner dims to dims[1..]
    LLVMValueRef one = LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0);
    LLVMValueRef idx = LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0);
    LLVMValueRef inner_dim_ptr = inner_dims_ptr;
    LLVMValueRef target_ptr = NULL;

    // We only support copying the inner dims from the first element here.
    // This is sufficient for nested initializer literals that use consistent
    // inner array dimensions.
    for (unsigned i = 1; i <= 1; i++) {
      LLVMValueRef dst_ptr = LLVMBuildGEP2(
          cg->builder, LLVMArrayType(LLVMInt64TypeInContext(cg->context), 0),
          dims_mem,
          (LLVMValueRef[]){
              LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0),
              LLVMConstInt(LLVMInt32TypeInContext(cg->context), i, 0)},
          2, "dst_dim_ptr");
      LLVMValueRef src_ptr = LLVMBuildGEP2(
          cg->builder, LLVMInt64TypeInContext(cg->context), inner_dims_ptr,
          (LLVMValueRef[]){
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), i - 1, 0)},
          1, "src_dim_ptr");
      LLVMValueRef src_val =
          LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                         src_ptr, "src_dim_val");
      LLVMBuildStore(cg->builder, src_val, dst_ptr);
    }

    dims_ptr = LLVMBuildBitCast(
        cg->builder, dims_mem,
        LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0), "dims_ptr");
  } else {
    array_struct = LLVMBuildInsertValue(
        cg->builder, array_struct,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), 3, "set_rank");

    LLVMValueRef dims_alloca = LLVMBuildAlloca(
        cg->builder, LLVMArrayType(LLVMInt64TypeInContext(cg->context), 1),
        "array_lit_dims");
    LLVMValueRef d_idx[] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0),
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0)};
    LLVMValueRef d0_ptr = LLVMBuildGEP2(
        cg->builder, LLVMArrayType(LLVMInt64TypeInContext(cg->context), 1),
        dims_alloca, d_idx, 2, "d0_ptr");
    LLVMBuildStore(cg->builder,
                   LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, 0),
                   d0_ptr);
    dims_ptr = LLVMBuildBitCast(
        cg->builder, dims_alloca,
        LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0), "dims_ptr");
  }

  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, rank_val, 3, "set_rank");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, dims_ptr, 4, "set_dims");

  return array_struct;
}

static LLVMValueRef cg_array_repeat(Codegen *cg, AstNode *node) {
  Type *inst_type = node->resolved_type;
  if (!inst_type) {
    if (!node->array_repeat.value || !node->array_repeat.value->resolved_type)
      panic("Internal error: cannot infer array repeat type from value");

    Type *element_type = node->array_repeat.value->resolved_type;
    Type *array_template =
        type_get_template(cg->type_ctx, sv_from_parts("Array", 5));
    if (!array_template)
      panic("Internal error: Array template not registered in codegen");

    List args;
    List_init(&args);
    List_push(&args, element_type);
    inst_type = type_get_instance(cg->type_ctx, array_template, args);
    List_free(&args, 0);
    node->resolved_type = inst_type;
  }

  LLVMValueRef count_val = cg_expression(cg, node->array_repeat.count);
  count_val =
      cg_cast_value(cg, count_val, node->array_repeat.count->resolved_type,
                    LLVMInt64TypeInContext(cg->context));

  Member *data_mem = type_get_member(inst_type, sv_from_parts("data", 4));
  if (!data_mem) {
    panic("Internal error: array repeat type '%s' has no data member",
          type_to_name(inst_type));
  }
  Type *ptr_type = data_mem->type;
  Type *elem_type = ptr_type->data.pointer_to;

  LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, elem_type);
  LLVMValueRef raw_data_ptr = LLVMBuildArrayAlloca(
      cg->builder, llvm_elem_ty, count_val, "array_rep_storage");

  LLVMBasicBlockRef pre_header = LLVMGetInsertBlock(cg->builder);
  LLVMBasicBlockRef loop_body = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "repeat_loop");
  LLVMBasicBlockRef loop_end = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "repeat_end");

  LLVMPositionBuilderAtEnd(cg->builder, pre_header);
  LLVMValueRef is_zero = LLVMBuildICmp(
      cg->builder, LLVMIntEQ, count_val,
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0), "is_zero");
  LLVMBuildCondBr(cg->builder, is_zero, loop_end, loop_body);

  LLVMPositionBuilderAtEnd(cg->builder, loop_body);

  LLVMValueRef i_phi =
      LLVMBuildPhi(cg->builder, LLVMInt64TypeInContext(cg->context), "i");
  LLVMValueRef zero = LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0);
  LLVMAddIncoming(i_phi, &zero, &pre_header, 1);

  LLVMValueRef val = cg_expression(cg, node->array_repeat.value);
  LLVMValueRef cast_val = cg_cast_value(
      cg, val, node->array_repeat.value->resolved_type, llvm_elem_ty);

  LLVMValueRef element_ptr =
      LLVMBuildGEP2(cg->builder, llvm_elem_ty, raw_data_ptr,
                    (LLVMValueRef[]){i_phi}, 1, "item_ptr");
  LLVMBuildStore(cg->builder, cast_val, element_ptr);

  LLVMValueRef i_next = LLVMBuildAdd(
      cg->builder, i_phi,
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "i_next");
  LLVMValueRef has_more =
      LLVMBuildICmp(cg->builder, LLVMIntULT, i_next, count_val, "has_more");
  LLVMBasicBlockRef loop_latch = LLVMGetInsertBlock(cg->builder);
  LLVMAddIncoming(i_phi, &i_next, &loop_latch, 1);

  LLVMBuildCondBr(cg->builder, has_more, loop_body, loop_end);

  LLVMPositionBuilderAtEnd(cg->builder, loop_end);

  LLVMTypeRef struct_ty = cg_get_llvm_type(cg, inst_type);
  LLVMValueRef array_struct = LLVMGetUndef(struct_ty);

  // Link allocated storage and length
  array_struct = LLVMBuildInsertValue(cg->builder, array_struct, raw_data_ptr,
                                      0, "set_data");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, count_val, 1, "set_len");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, count_val, 2, "set_cap");

  // Multidimensional / Single Array metadata
  if (type_is_array_struct(elem_type)) {
    // We are repeating an array: [ [T; c_in]; c_out ]
    // Get inner array's rank
    LLVMValueRef inner_rank_ptr = LLVMBuildStructGEP2(
        cg->builder, llvm_elem_ty, raw_data_ptr, 3, "inner_rank_ptr");
    LLVMValueRef inner_rank =
        LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                       inner_rank_ptr, "inner_rank");

    LLVMValueRef outer_rank = LLVMBuildAdd(
        cg->builder, inner_rank,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "outer_rank");
    array_struct = LLVMBuildInsertValue(cg->builder, array_struct, outer_rank,
                                        3, "set_rank");

    // Allocate new dims: (inner_rank + 1) * 8
    LLVMValueRef dims_size = LLVMBuildMul(
        cg->builder, outer_rank,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 8, 0), "dims_size");
    LLVMValueRef dims_mem =
        LLVMBuildArrayAlloca(cg->builder, LLVMInt64TypeInContext(cg->context),
                             outer_rank, "repeat_dims_mem");

    // dims[0] = count_val
    LLVMValueRef d0_idx[] = {
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0)};
    LLVMValueRef d0_ptr =
        LLVMBuildGEP2(cg->builder, LLVMInt64TypeInContext(cg->context),
                      dims_mem, d0_idx, 1, "d0_ptr");
    LLVMBuildStore(cg->builder, count_val, d0_ptr);

    // Copy inner dims to dims[1...rank]
    LLVMValueRef inner_dims_ptr_ptr = LLVMBuildStructGEP2(
        cg->builder, llvm_elem_ty, raw_data_ptr, 4, "inner_dims_ptr_ptr");
    LLVMValueRef inner_dims = LLVMBuildLoad2(
        cg->builder, LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0),
        inner_dims_ptr_ptr, "inner_dims");

    // Simple loop to copy dims
    LLVMBasicBlockRef d_pre = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef d_loop = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "copy_dims_loop");
    LLVMBasicBlockRef d_end = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "copy_dims_end");

    LLVMBuildBr(cg->builder, d_loop);
    LLVMPositionBuilderAtEnd(cg->builder, d_loop);
    LLVMValueRef di_phi =
        LLVMBuildPhi(cg->builder, LLVMInt64TypeInContext(cg->context), "di");
    LLVMValueRef zero_di =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0);
    LLVMAddIncoming(di_phi, &zero_di, &d_pre, 1);

    // Load inner_dims[di]
    LLVMValueRef di_ptr =
        LLVMBuildGEP2(cg->builder, LLVMInt64TypeInContext(cg->context),
                      inner_dims, (LLVMValueRef[]){di_phi}, 1, "di_ptr");
    LLVMValueRef dim_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), di_ptr, "dim_val");

    // Store to dims[di + 1]
    LLVMValueRef di_plus_1 = LLVMBuildAdd(
        cg->builder, di_phi,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "di_plus_1");
    LLVMValueRef out_di_ptr =
        LLVMBuildGEP2(cg->builder, LLVMInt64TypeInContext(cg->context),
                      dims_mem, (LLVMValueRef[]){di_plus_1}, 1, "out_di_ptr");
    LLVMBuildStore(cg->builder, dim_val, out_di_ptr);

    LLVMValueRef di_next = LLVMBuildAdd(
        cg->builder, di_phi,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "di_next");
    LLVMValueRef d_more =
        LLVMBuildICmp(cg->builder, LLVMIntULT, di_next, inner_rank, "d_more");
    LLVMBasicBlockRef d_latch = LLVMGetInsertBlock(cg->builder);
    LLVMAddIncoming(di_phi, &di_next, &d_latch, 1);
    LLVMBuildCondBr(cg->builder, d_more, d_loop, d_end);

    LLVMPositionBuilderAtEnd(cg->builder, d_end);
    array_struct = LLVMBuildInsertValue(cg->builder, array_struct, dims_mem, 4,
                                        "set_dims");

  } else {
    // Rank 1
    array_struct = LLVMBuildInsertValue(
        cg->builder, array_struct,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), 3, "set_rank");

    LLVMValueRef dims_mem = LLVMBuildAlloca(
        cg->builder, LLVMArrayType(LLVMInt64TypeInContext(cg->context), 1),
        "repeat_dims_mem");
    LLVMValueRef d_idx[] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0),
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0)};
    LLVMValueRef d0_ptr = LLVMBuildGEP2(
        cg->builder, LLVMArrayType(LLVMInt64TypeInContext(cg->context), 1),
        dims_mem, d_idx, 2, "d0_ptr");
    LLVMBuildStore(cg->builder, count_val, d0_ptr);
    LLVMValueRef dims_ptr_cast = LLVMBuildBitCast(
        cg->builder, dims_mem,
        LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0), "dims_ptr");
    array_struct = LLVMBuildInsertValue(cg->builder, array_struct,
                                        dims_ptr_cast, 4, "set_dims");
  }

  return array_struct;
}

LLVMValueRef cg_expression(Codegen *cg, AstNode *node) {
  if (!node)
    return NULL;
  switch (node->tag) {
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_BOOL:
  case NODE_STRING:
  case NODE_NULL:
    return cg_const_expr(cg, node);
  case NODE_VAR:
    return cg_var_expr(cg, node);
  case NODE_FIELD:
    return cg_field_expr(cg, node);
  case NODE_INDEX: {
    if (!node->resolved_type) {
      panic("Internal error: indexed expression has no resolved type");
    }
    LLVMValueRef element_ptr = cg_expression_addr(cg, node);
    return LLVMBuildLoad2(cg->builder,
                          cg_get_llvm_type(cg, node->resolved_type),
                          element_ptr, "load_element");
  }
  case NODE_BINARY_ARITH:
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    return cg_binary_expr(cg, node);
  case NODE_BINARY_IS:
    return cg_binary_is_expr(cg, node);
  case NODE_BINARY_ELSE:
    return cg_binary_else_expr(cg, node);
  case NODE_UNARY:
    return cg_unary_expr(cg, node);
  case NODE_TERNARY:
    return cg_ternary_expr(cg, node);
  case NODE_ASSIGN_EXPR:
    return cg_assign_expr(cg, node);
  case NODE_CAST_EXPR:
    return cg_cast_expr(cg, node);
  case NODE_CALL:
    return cg_call_expr(cg, node);
  case NODE_NEW_EXPR:
    return cg_new_expr(cg, node);
  case NODE_ARRAY_LITERAL:
    return cg_array_literal(cg, node);
  case NODE_ARRAY_REPEAT:
    return cg_array_repeat(cg, node);
  default:
    fprintf(stderr, "Codegen: Unhandled expression tag %d\n", node->tag);
    return NULL;
  }
}
