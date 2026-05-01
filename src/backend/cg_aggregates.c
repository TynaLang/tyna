#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/utils.h"
#include "llvm-c/Target.h"

int cg_tagged_union_variant_index(Type *union_type, Type *variant) {
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

int cg_tagged_union_variant_index_llvm(Codegen *cg, Type *union_type,
                                       LLVMTypeRef llvm_ty) {
  if (!union_type || union_type->kind != KIND_UNION ||
      !union_type->is_tagged_union)
    return -1;
  for (size_t i = 0; i < union_type->members.len; i++) {
    Member *m = union_type->members.items[i];
    if (cg_type_get_llvm(cg, m->type) == llvm_ty)
      return (int)i;
  }
  return -1;
}

static bool cg_is_list_struct(Type *t) {
  if (!t || t->kind != KIND_STRUCT)
    return false;
  if (t->data.instance.from_template &&
      sv_eq(t->data.instance.from_template->name, sv_from_parts("List", 4)))
    return true;
  return false;
}

static bool cg_is_dynamic_array_type(Type *t) {
  if (!t || t->fixed_array_len != 0 || t->data.instance.generic_args.len == 0)
    return false;
  return type_is_array_struct(t) || cg_is_list_struct(t);
}

LLVMValueRef cg_make_tagged_union(Codegen *cg, LLVMValueRef val, Type *from_t,
                                  Type *union_t) {
  int variant_index = cg_tagged_union_variant_index(union_t, from_t);
  if (variant_index < 0)
    return val;

  LLVMTypeRef union_ty = cg_type_get_llvm(cg, union_t);
  LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, union_ty, "tagged_union_tmp");
  LLVMValueRef tag_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 0,
                                             "tagged_union_tag_ptr");
  LLVMBuildStore(
      cg->builder,
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), variant_index, 0),
      tag_ptr);

  if (from_t && from_t->kind == KIND_PRIMITIVE &&
      from_t->data.primitive == PRIM_VOID) {
    return LLVMBuildLoad2(cg->builder, union_ty, tmp, "tagged_union_val");
  }

  LLVMValueRef payload_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 1,
                                                 "tagged_union_payload_ptr");
  LLVMTypeRef from_ty = cg_type_get_llvm(cg, from_t);
  LLVMValueRef store_ptr =
      LLVMBuildBitCast(cg->builder, payload_ptr, LLVMPointerType(from_ty, 0),
                       "tagged_union_payload_cast");
  LLVMBuildStore(cg->builder, val, store_ptr);
  return LLVMBuildLoad2(cg->builder, union_ty, tmp, "tagged_union_val");
}

int cg_tagged_union_variant_index_by_member(Type *union_type, Member *variant) {
  if (!union_type || union_type->kind != KIND_UNION ||
      !union_type->is_tagged_union || !variant)
    return -1;
  for (size_t i = 0; i < union_type->members.len; i++) {
    Member *m = union_type->members.items[i];
    if (m == variant)
      return (int)i;
  }
  return -1;
}

LLVMValueRef cg_extract_tagged_union(Codegen *cg, LLVMValueRef union_val,
                                     Type *union_t, Type *target_t,
                                     LLVMTypeRef target_ty) {
  int variant_index =
      cg_tagged_union_variant_index_llvm(cg, union_t, target_ty);
  if (variant_index < 0)
    return union_val;

  LLVMTypeRef union_ty = cg_type_get_llvm(cg, union_t);
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
  CgFunc *panic_fn = cg_find_system_function(cg, sv_from_parts("panic", 5));
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

LLVMValueRef cg_array_literal(Codegen *cg, AstNode *node) {
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

  if (inst_type->fixed_array_len > 0) {
    if (inst_type->data.instance.generic_args.len == 0) {
      panic("Internal error: fixed array literal missing element type");
    }
    Type *elem_type = inst_type->data.instance.generic_args.items[0];
    LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
    LLVMTypeRef llvm_array_ty = cg_type_get_llvm(cg, inst_type);

    LLVMValueRef array_val = LLVMGetUndef(llvm_array_ty);
    AstNode **items = (AstNode **)node->array_literal.items.items;
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = items[i];
      LLVMValueRef val = cg_expression(cg, item);
      val = cg_cast_value(cg, val, item->resolved_type, llvm_elem_ty);
      array_val = LLVMBuildInsertValue(cg->builder, array_val, val, (unsigned)i,
                                       "array_lit_item");
    }
    return array_val;
  }

  size_t count = node->array_literal.items.len;
  Type *element_type = inst_type->data.instance.generic_args.items[0];
  LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, element_type);

  if (cg_is_dynamic_array_type(inst_type)) {
    LLVMTypeRef struct_ty = cg_type_get_llvm(cg, inst_type);
    LLVMValueRef array_ptr =
        LLVMBuildAlloca(cg->builder, struct_ty, "array_lit_temp");
    LLVMBuildStore(cg->builder, LLVMConstNull(struct_ty), array_ptr);

    const char *data_layout = LLVMGetDataLayout(cg->module);
    LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
    unsigned long long elem_size_bytes = LLVMABISizeOfType(td, llvm_elem_ty);
    LLVMDisposeTargetData(td);
    LLVMValueRef elem_size =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), elem_size_bytes, 0);

    if (count == 0) {
      CgFunc *new_fn =
          cg_find_system_function(cg, sv_from_cstr("__tyna_array_new"));
      if (!new_fn)
        panic("Internal error: __tyna_array_new runtime function missing");
      LLVMBuildCall2(cg->builder, new_fn->type, new_fn->value,
                     (LLVMValueRef[]){array_ptr, elem_size}, 2, "");
    } else {
      CgFunc *from_stack_fn =
          cg_find_system_function(cg, sv_from_cstr("__tyna_array_from_stack"));
      if (!from_stack_fn)
        panic(
            "Internal error: __tyna_array_from_stack runtime function missing");

      LLVMTypeRef array_ty = LLVMArrayType(llvm_elem_ty, (unsigned)count);
      LLVMValueRef raw_data_ptr =
          LLVMBuildAlloca(cg->builder, array_ty, "array_lit_storage");

      LLVMValueRef *const_values = xmalloc(sizeof(LLVMValueRef) * count);
      AstNode **items = (AstNode **)node->array_literal.items.items;
      bool all_const = true;
      for (size_t i = 0; i < count; i++) {
        AstNode *item = items[i];
        LLVMValueRef val = cg_expression(cg, item);
        if (!LLVMIsConstant(val))
          all_const = false;
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
          AstNode *item = items[i];
          LLVMValueRef val = const_values[i];
          val = cg_cast_value(cg, val, item->resolved_type, llvm_elem_ty);
          LLVMBuildStore(cg->builder, val, element_ptr);
        }
      }
      free(const_values);

      LLVMTypeRef i8_ptr =
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
      LLVMValueRef stack_ptr =
          LLVMBuildBitCast(cg->builder, raw_data_ptr, i8_ptr, "stack_ptr");
      LLVMBuildCall2(
          cg->builder, from_stack_fn->type, from_stack_fn->value,
          (LLVMValueRef[]){
              array_ptr, stack_ptr,
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, 0),
              elem_size},
          4, "");
    }
    return LLVMBuildLoad2(cg->builder, struct_ty, array_ptr, "array_lit_dyn");
  }

  LLVMTypeRef array_ty = LLVMArrayType(llvm_elem_ty, count);
  LLVMValueRef raw_data_ptr =
      LLVMBuildAlloca(cg->builder, array_ty, "array_lit_storage");

  LLVMValueRef *const_values = xmalloc(sizeof(LLVMValueRef) * count);
  AstNode **items = (AstNode **)node->array_literal.items.items;
  bool all_const = true;
  for (size_t i = 0; i < count; i++) {
    AstNode *item = items[i];
    LLVMValueRef val = cg_expression(cg, item);
    if (!LLVMIsConstant(val))
      all_const = false;
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

  LLVMTypeRef struct_ty = cg_type_get_llvm(cg, inst_type);
  LLVMValueRef array_struct = LLVMGetUndef(struct_ty);
  LLVMValueRef data_ptr_cast = LLVMBuildBitCast(
      cg->builder, raw_data_ptr, LLVMPointerType(llvm_elem_ty, 0), "data_ptr");
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

  return array_struct;
}

LLVMValueRef cg_array_repeat(Codegen *cg, AstNode *node) {
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

  if (inst_type->fixed_array_len > 0) {
    if (inst_type->data.instance.generic_args.len == 0) {
      panic("Internal error: fixed array repeat missing element type");
    }
    Type *elem_type = inst_type->data.instance.generic_args.items[0];
    LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
    LLVMTypeRef llvm_array_ty = cg_type_get_llvm(cg, inst_type);

    uint64_t count = inst_type->fixed_array_len;
    LLVMValueRef array_storage =
        LLVMBuildAlloca(cg->builder, llvm_array_ty, "array_rep_storage_fixed");

    for (uint64_t i = 0; i < count; i++) {
      LLVMValueRef item_ptr = LLVMBuildGEP2(
          cg->builder, llvm_array_ty, array_storage,
          (LLVMValueRef[]){
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0),
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), i, 0)},
          2, "array_rep_item_ptr");
      LLVMValueRef val = cg_expression(cg, node->array_repeat.value);
      val = cg_cast_value(cg, val, node->array_repeat.value->resolved_type,
                          llvm_elem_ty);
      LLVMBuildStore(cg->builder, val, item_ptr);
    }

    return LLVMBuildLoad2(cg->builder, llvm_array_ty, array_storage,
                          "array_rep_fixed");
  }

  LLVMValueRef count_val = cg_expression(cg, node->array_repeat.count);
  count_val =
      cg_cast_value(cg, count_val, node->array_repeat.count->resolved_type,
                    LLVMInt64TypeInContext(cg->context));

  Type *element_type = inst_type->data.instance.generic_args.items[0];
  LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, element_type);

  if (cg_is_dynamic_array_type(inst_type)) {
    LLVMTypeRef struct_ty = cg_type_get_llvm(cg, inst_type);
    LLVMValueRef array_ptr =
        LLVMBuildAlloca(cg->builder, struct_ty, "array_rep_temp");
    LLVMBuildStore(cg->builder, LLVMConstNull(struct_ty), array_ptr);

    CgFunc *with_capacity_fn =
        cg_find_system_function(cg, sv_from_cstr("__tyna_array_with_capacity"));
    CgFunc *push_fn =
        cg_find_system_function(cg, sv_from_cstr("__tyna_array_push"));
    if (!with_capacity_fn)
      panic("Internal error: __tyna_array_with_capacity runtime function "
            "missing");
    if (!push_fn)
      panic("Internal error: __tyna_array_push runtime function missing");

    const char *data_layout = LLVMGetDataLayout(cg->module);
    LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
    unsigned long long elem_size_bytes = LLVMABISizeOfType(td, llvm_elem_ty);
    LLVMDisposeTargetData(td);
    LLVMValueRef elem_size =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), elem_size_bytes, 0);

    LLVMBuildCall2(cg->builder, with_capacity_fn->type, with_capacity_fn->value,
                   (LLVMValueRef[]){array_ptr, count_val, elem_size}, 3, "");

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

    LLVMValueRef elem_tmp =
        LLVMBuildAlloca(cg->builder, llvm_elem_ty, "array_rep_elem_tmp");
    LLVMBuildStore(cg->builder, cast_val, elem_tmp);
    LLVMTypeRef i8_ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
    LLVMValueRef elem_ptr_i8 =
        LLVMBuildBitCast(cg->builder, elem_tmp, i8_ptr, "elem_ptr");
    LLVMBuildCall2(cg->builder, push_fn->type, push_fn->value,
                   (LLVMValueRef[]){array_ptr, elem_ptr_i8, elem_size}, 3, "");

    LLVMValueRef i_next = LLVMBuildAdd(
        cg->builder, i_phi,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "i_next");
    LLVMValueRef has_more =
        LLVMBuildICmp(cg->builder, LLVMIntULT, i_next, count_val, "has_more");
    LLVMBasicBlockRef loop_latch = LLVMGetInsertBlock(cg->builder);
    LLVMAddIncoming(i_phi, &i_next, &loop_latch, 1);

    LLVMBuildCondBr(cg->builder, has_more, loop_body, loop_end);

    LLVMPositionBuilderAtEnd(cg->builder, loop_end);
    return LLVMBuildLoad2(cg->builder, struct_ty, array_ptr, "array_rep_dyn");
  }

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

  LLVMTypeRef struct_ty = cg_type_get_llvm(cg, inst_type);
  LLVMValueRef array_struct = LLVMGetUndef(struct_ty);
  LLVMValueRef data_ptr_cast = LLVMBuildBitCast(
      cg->builder, raw_data_ptr, LLVMPointerType(llvm_elem_ty, 0), "data_ptr");
  array_struct = LLVMBuildInsertValue(cg->builder, array_struct, data_ptr_cast,
                                      0, "set_data");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, count_val, 1, "set_len");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, count_val, 2, "set_cap");

  return array_struct;
}
