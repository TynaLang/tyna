#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/utils.h"

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

  LLVMValueRef payload_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 1,
                                                 "tagged_union_payload_ptr");
  LLVMTypeRef from_ty = cg_type_get_llvm(cg, from_t);
  LLVMValueRef store_ptr =
      LLVMBuildBitCast(cg->builder, payload_ptr, LLVMPointerType(from_ty, 0),
                       "tagged_union_payload_cast");
  LLVMBuildStore(cg->builder, val, store_ptr);
  return LLVMBuildLoad2(cg->builder, union_ty, tmp, "tagged_union_val");
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

  size_t count = node->array_literal.items.len;
  Member *data_mem = type_get_member(inst_type, sv_from_parts("data", 4));
  if (!data_mem) {
    panic("Internal error: array literal type '%s' has no data member",
          type_to_name(inst_type));
  }
  Type *ptr_type = data_mem->type;
  Type *elem_type = ptr_type->data.pointer_to;

  LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
  LLVMTypeRef array_ty = LLVMArrayType(llvm_elem_ty, count);
  LLVMValueRef raw_data_ptr =
      LLVMBuildAlloca(cg->builder, array_ty, "array_lit_storage");

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

  LLVMTypeRef struct_ty = cg_type_get_llvm(cg, inst_type);
  LLVMValueRef array_struct = LLVMGetUndef(struct_ty);
  LLVMValueRef data_ptr_cast = LLVMBuildBitCast(
      cg->builder, raw_data_ptr, cg_type_get_llvm(cg, ptr_type), "data_ptr");

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

  LLVMValueRef rank_val =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0);
  LLVMValueRef dims_ptr = NULL;

  if (type_is_array_struct(elem_type)) {
    LLVMTypeRef inner_ty = cg_type_get_llvm(cg, elem_type);
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

    LLVMValueRef one = LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0);
    LLVMValueRef inner_dim_ptr = inner_dims_ptr;
    LLVMValueRef target_ptr = NULL;
    (void)one;
    (void)inner_dim_ptr;
    (void)target_ptr;

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
              LLVMConstInt(LLVMInt32TypeInContext(cg->context), i - 1, 0)},
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

  LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
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

  array_struct = LLVMBuildInsertValue(cg->builder, array_struct, raw_data_ptr,
                                      0, "set_data");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, count_val, 1, "set_len");
  array_struct =
      LLVMBuildInsertValue(cg->builder, array_struct, count_val, 2, "set_cap");

  if (type_is_array_struct(elem_type)) {
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

    LLVMValueRef dims_mem =
        LLVMBuildArrayAlloca(cg->builder, LLVMInt64TypeInContext(cg->context),
                             outer_rank, "repeat_dims_mem");

    LLVMValueRef d0_idx[] = {
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0)};
    LLVMValueRef d0_ptr =
        LLVMBuildGEP2(cg->builder, LLVMInt64TypeInContext(cg->context),
                      dims_mem, d0_idx, 1, "d0_ptr");
    LLVMBuildStore(cg->builder, count_val, d0_ptr);

    LLVMValueRef inner_dims_ptr_ptr = LLVMBuildStructGEP2(
        cg->builder, llvm_elem_ty, raw_data_ptr, 4, "inner_dims_ptr_ptr");
    LLVMValueRef inner_dims = LLVMBuildLoad2(
        cg->builder, LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0),
        inner_dims_ptr_ptr, "inner_dims");

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

    LLVMValueRef di_ptr =
        LLVMBuildGEP2(cg->builder, LLVMInt64TypeInContext(cg->context),
                      inner_dims, (LLVMValueRef[]){di_phi}, 1, "di_ptr");
    LLVMValueRef dim_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), di_ptr, "dim_val");

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
