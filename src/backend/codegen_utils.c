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

  char *c_name = sv_to_cstr(name);
  LLVMValueRef alloca =
      LLVMBuildAlloca(tmp_builder, cg_get_llvm_type(cg, type), c_name);
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
      panic("Undefined variable '" SV_FMT "'", SV_ARG(node->var.value));
    }
    return sym->value;
  }

  case NODE_INDEX: {

    LLVMValueRef fat_ptr = cg_expression(cg, node->index.array);
    LLVMValueRef data_ptr =
        LLVMBuildExtractValue(cg->builder, fat_ptr, 1, "arr_extract_ptr");

    LLVMValueRef idx_val = cg_expression(cg, node->index.index);
    Type *arr_type = node->index.array->resolved_type;
    LLVMTypeRef llvm_elem_ty =
        cg_get_llvm_type(cg, arr_type->data.array.element);

    return LLVMBuildGEP2(cg->builder, llvm_elem_ty, data_ptr, &idx_val, 1,
                         "arr_index_gep");
  }

  default:
    return NULL;
  }
}