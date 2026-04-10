#include <llvm-c/Core.h>
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

  // Align the alloca to the type's natural alignment when available.
  size_t align = type->alignment ? type->alignment : type->size;
  if (align == 0)
    align = 1;
  LLVMSetAlignment(alloca, (unsigned)align);

  // Initialize with zero/null for safety
  LLVMBuildStore(tmp_builder, LLVMConstNull(llvm_type), alloca);

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

  case NODE_FIELD: {
    LLVMValueRef obj_ptr = cg_get_address(cg, node->field.object);
    if (!obj_ptr) {
      return NULL;
    }
    Type *obj_type = node->field.object->resolved_type;
    if (node->field.object->tag == NODE_VAR) {
      CGSymbol *sym =
          CGSymbolTable_find(cg->current_scope, node->field.object->var.value);
      if (sym && sym->type) {
        obj_type = sym->type;
      }
    }
    if (obj_type && obj_type->kind == KIND_POINTER) {
      if (!obj_type->data.pointer_to) {
        panic("Pointer object has no target type");
      }
      LLVMTypeRef load_ty = cg_get_llvm_type(cg, obj_type);
      if (LLVMGetTypeKind(load_ty) != LLVMPointerTypeKind) {
        panic("Expected pointer value type for pointer object");
      }
      obj_ptr = LLVMBuildLoad2(cg->builder, load_ty, obj_ptr, "deref_ptr");
      obj_type = obj_type->data.pointer_to;
    }
    if (!obj_type || obj_type->kind != KIND_STRUCT) {
      panic("Expected FIELD object to resolve to a struct type");
    }
    LLVMTypeRef obj_ptr_ty = LLVMTypeOf(obj_ptr);
    if (LLVMGetTypeKind(obj_ptr_ty) != LLVMPointerTypeKind) {
      panic("Expected field object address to be a pointer");
    }
    Member *m = type_get_member(obj_type, node->field.field);
    if (!m)
      return NULL;
    LLVMTypeRef field_type = cg_get_llvm_type(cg, obj_type);
    LLVMValueRef field_addr = LLVMBuildStructGEP2(
        cg->builder, field_type, obj_ptr, m->index, "field_addr");
    return field_addr;
  }

  case NODE_UNARY: {
    if (node->unary.op == OP_DEREF) {
      return cg_expression(cg, node->unary.expr);
    }
    return NULL;
  }

  case NODE_INDEX: {
    LLVMValueRef array_struct_ptr = cg_get_address(cg, node->index.array);
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

    // Call panic("Index out of bounds")
    CGFunction *panic_fn = cg_find_function(cg, sv_from_parts("panic", 5));
    LLVMValueRef msg_ptr = LLVMBuildGlobalStringPtr(
        cg->builder, "Panic: Array Index Out of Bounds", "bounds_err_msg");
    LLVMBuildCall2(cg->builder, panic_fn->type, panic_fn->value,
                   (LLVMValueRef[]){msg_ptr}, 1, "");

    LLVMBuildUnreachable(cg->builder);

    // Ok block
    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);

    // 5. If this is a multi-dimensional access, we need to adjust the child's
    // metadata.
    // However, Node Index in Tyl seems to only handle one level at a time.
    // If we have arr[i][j], the inner `arr[i]` returns an Array struct.
    // We must ensure the returned Array struct has rank-1 and shifted dims.

    LLVMValueRef data_ptr_gep = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 0, "data_field_ptr");
    LLVMTypeRef elem_ptr_ty = cg_get_llvm_type(
        cg, type_get_pointer(cg->type_ctx, node->resolved_type));
    LLVMValueRef actual_data_ptr =
        LLVMBuildLoad2(cg->builder, elem_ptr_ty, data_ptr_gep, "load_data_ptr");

    LLVMValueRef element_addr = LLVMBuildGEP2(
        cg->builder, cg_get_llvm_type(cg, node->resolved_type), actual_data_ptr,
        (LLVMValueRef[]){index_i64}, 1, "element_ptr");

    // If the element is itself an array struct, we need to "monomorphize" it to
    // the slice.
    if (type_is_array_struct(node->resolved_type)) {
      // Element is an array struct. We need to load it and fix rank/dims.
      LLVMValueRef sub_array =
          LLVMBuildLoad2(cg->builder, cg_get_llvm_type(cg, node->resolved_type),
                         element_addr, "sub_array_load");

      // new_rank = rank - 1
      LLVMValueRef new_rank = LLVMBuildSub(
          cg->builder, rank_val,
          LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "new_rank");
      sub_array =
          LLVMBuildInsertValue(cg->builder, sub_array, new_rank, 3, "set_rank");

      // new_dims = dims + 1
      LLVMValueRef new_dims = LLVMBuildGEP2(
          cg->builder, LLVMInt64TypeInContext(cg->context), dims_ptr,
          (LLVMValueRef[]){
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0)},
          1, "new_dims_ptr");
      sub_array =
          LLVMBuildInsertValue(cg->builder, sub_array, new_dims, 4, "set_dims");

      // Since we need to return an ADDRESS of this struct (if it's an l-value),
      // we must spill it.
      LLVMValueRef temp_alloca = cg_alloca_in_entry(
          cg, node->resolved_type, sv_from_cstr("sub_array_slice"));
      LLVMBuildStore(cg->builder, sub_array, temp_alloca);
      return temp_alloca;
    }

    return element_addr;
  }

  default:
    return NULL;
  }
}
