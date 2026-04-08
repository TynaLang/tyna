#include <llvm-c/Types.h>

#include "codegen_private.h"
#include "tyl/codegen.h"

LLVMValueRef cg_alloca_in_entry(Codegen *cg, Type *type, StringView name) {
  LLVMBuilderRef tmp_builder = LLVMCreateBuilderInContext(cg->context);
  LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(cg->current_function);

  LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_block);
  if (first_instr) {
    LLVMPositionBuilderBefore(tmp_builder, first_instr);
  } else {
    LLVMPositionBuilderAtEnd(tmp_builder, entry_block);
  }

  LLVMTypeRef llvm_type = cg_get_llvm_type(cg, type);
  char *c_name = sv_to_cstr(name);
  LLVMValueRef alloca = LLVMBuildAlloca(tmp_builder, llvm_type, c_name);

  // If it's a fixed-size array, we need to initialize the fat pointer (slice)
  // to point to its own stack space if it's not a slice.
  if (type->kind == KIND_ARRAY && !type->data.array.is_dynamic) {

    Type *base = type;
    size_t total_elements = 1;
    while (base->kind == KIND_ARRAY && !base->data.array.is_dynamic) {
      total_elements *= base->data.array.fixed_size;
      base = base->data.array.element;
    }

    LLVMTypeRef base_llvm_ty = cg_get_llvm_type(cg, base);

    LLVMValueRef flat_backing = LLVMBuildAlloca(
        tmp_builder, LLVMArrayType(base_llvm_ty, total_elements), "flat_array");

    LLVMTypeRef i64_type = LLVMInt64TypeInContext(cg->context);
    LLVMValueRef len_val =
        LLVMConstInt(i64_type, type->data.array.fixed_size, false);

    LLVMValueRef first_elem_offset = LLVMConstInt(i64_type, 0, false);
    LLVMValueRef data_ptr =
        LLVMBuildGEP2(tmp_builder, base_llvm_ty, flat_backing,
                      &first_elem_offset, 1, "flat_decay");

    LLVMValueRef fat_ptr = LLVMGetUndef(llvm_type);
    fat_ptr =
        LLVMBuildInsertValue(tmp_builder, fat_ptr, len_val, 0, "slice_len");
    fat_ptr =
        LLVMBuildInsertValue(tmp_builder, fat_ptr, data_ptr, 1, "slice_data");

    // Fix: Dimensions pointer for static arrays
    // For static arrays, we need to provide a pointer to dimensions.
    // For simplicity in the static stack allocation case, we can use a global
    // or constant array for [len, 1] (len and stride of 1).
    // However, to keep it consistent with how we handle static arrays
    // elsewhere:
    LLVMValueRef dims_vals[] = {len_val, LLVMConstInt(i64_type, 1, false)};
    LLVMValueRef dims_arr = LLVMConstArray(i64_type, dims_vals, 2);
    LLVMValueRef global_dims =
        LLVMAddGlobal(cg->module, LLVMTypeOf(dims_arr), "static_array_dims");
    LLVMSetInitializer(global_dims, dims_arr);
    LLVMSetGlobalConstant(global_dims, true);
    LLVMSetLinkage(global_dims, LLVMInternalLinkage);

    LLVMValueRef dims_ptr = LLVMBuildBitCast(
        tmp_builder, global_dims, LLVMPointerType(i64_type, 0), "dims_ptr");
    fat_ptr =
        LLVMBuildInsertValue(tmp_builder, fat_ptr, dims_ptr, 2, "slice_dims");

    LLVMBuildStore(tmp_builder, fat_ptr, alloca);
  } else if (type->kind == KIND_PRIMITIVE &&
             type->data.primitive == PRIM_STRING) {
    // Strings are initialized to empty
    LLVMBuildStore(tmp_builder, LLVMConstNull(llvm_type), alloca);
  }

  free(c_name);
  LLVMDisposeBuilder(tmp_builder);
  return alloca;
}

LLVMValueRef cg_get_address(Codegen *cg, AstNode *node) {
  if (!node)
    return NULL;

  switch (node->tag) {
  case NODE_VAR: {
    CGSymbol *sym = CGSymbolTable_find(cg->current_scope, node->var.value);
    if (!sym) {
      panic("Undefined variable '" SV_FMT "' during address-of",
            SV_ARG(node->var.value));
    }
    return sym->value;
  }

  case NODE_INDEX: {
    LLVMValueRef fat_ptr = cg_expression(cg, node->index.array);
    LLVMValueRef idx_val = cg_expression(cg, node->index.index);
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
    LLVMValueRef index =
        cg_cast_value(cg, idx_val, node->index.index->resolved_type, i64_ty);

    LLVMValueRef rank =
        LLVMBuildExtractValue(cg->builder, fat_ptr, 0, "arr_rank");
    LLVMValueRef data_ptr =
        LLVMBuildExtractValue(cg->builder, fat_ptr, 1, "arr_ptr");
    LLVMValueRef dims_ptr =
        LLVMBuildExtractValue(cg->builder, fat_ptr, 2, "arr_dims");

    // Calculate stride (product of dims[1] through dims[rank-1])
    LLVMBasicBlockRef current_bb_stride = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef func_stride = LLVMGetBasicBlockParent(current_bb_stride);
    LLVMBasicBlockRef stride_loop_bb = LLVMAppendBasicBlockInContext(
        cg->context, func_stride, "addr_stride_loop");
    LLVMBasicBlockRef stride_after_bb = LLVMAppendBasicBlockInContext(
        cg->context, func_stride, "addr_stride_done");

    LLVMBuildBr(cg->builder, stride_loop_bb);
    LLVMPositionBuilderAtEnd(cg->builder, stride_loop_bb);

    LLVMValueRef phi_idx_stride =
        LLVMBuildPhi(cg->builder, i64_ty, "stride_idx");
    LLVMValueRef phi_val_stride =
        LLVMBuildPhi(cg->builder, i64_ty, "stride_val");

    LLVMValueRef dim_gep = LLVMBuildGEP2(cg->builder, i64_ty, dims_ptr,
                                         &phi_idx_stride, 1, "dim_gep");
    LLVMValueRef dim_val =
        LLVMBuildLoad2(cg->builder, i64_ty, dim_gep, "dim_val");
    LLVMValueRef next_val_stride =
        LLVMBuildMul(cg->builder, phi_val_stride, dim_val, "next_stride");
    LLVMValueRef next_idx_stride =
        LLVMBuildAdd(cg->builder, phi_idx_stride,
                     LLVMConstInt(i64_ty, 1, false), "next_idx");

    LLVMValueRef stride_cond = LLVMBuildICmp(
        cg->builder, LLVMIntULT, next_idx_stride, rank, "stride_cond");
    LLVMBuildCondBr(cg->builder, stride_cond, stride_loop_bb, stride_after_bb);

    LLVMAddIncoming(
        phi_idx_stride,
        (LLVMValueRef[]){LLVMConstInt(i64_ty, 1, false), next_idx_stride},
        (LLVMBasicBlockRef[]){current_bb_stride, stride_loop_bb}, 2);
    LLVMAddIncoming(
        phi_val_stride,
        (LLVMValueRef[]){LLVMConstInt(i64_ty, 1, false), next_val_stride},
        (LLVMBasicBlockRef[]){current_bb_stride, stride_loop_bb}, 2);

    LLVMPositionBuilderAtEnd(cg->builder, stride_after_bb);
    LLVMValueRef rank_gt_1 =
        LLVMBuildICmp(cg->builder, LLVMIntUGT, rank,
                      LLVMConstInt(i64_ty, 1, false), "rank_gt_1");
    LLVMValueRef stride =
        LLVMBuildSelect(cg->builder, rank_gt_1, next_val_stride,
                        LLVMConstInt(i64_ty, 1, false), "final_stride");

    LLVMValueRef offset =
        LLVMBuildMul(cg->builder, index, stride, "flat_offset");

    Type *arr_type = node->index.array->resolved_type;
    Type *base = (arr_type->kind == KIND_PRIMITIVE &&
                  arr_type->data.primitive == PRIM_STRING)
                     ? type_get_primitive(cg->type_ctx, PRIM_CHAR)
                     : arr_type->data.array.element;
    while (base->kind == KIND_ARRAY) {
      base = base->data.array.element;
    }

    return LLVMBuildGEP2(cg->builder, cg_get_llvm_type(cg, base), data_ptr,
                         &offset, 1, "arr_index_gep");
  }

  default:
    fprintf(stderr, "Codegen: Cannot take address of node tag %d\n", node->tag);
    return NULL;
  }
}