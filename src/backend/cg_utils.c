#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

#include "cg_internal.h"
#include "tyna/codegen.h"

LLVMValueRef cg_alloca_in_entry(Codegen *cg, Type *type, StringView name) {
  LLVMBuilderRef tmp_builder = LLVMCreateBuilderInContext(cg->context);
  LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(cg->current_function);

  LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_block);
  if (first_instr) {
    LLVMPositionBuilderBefore(tmp_builder, first_instr);
  } else {
    LLVMPositionBuilderAtEnd(tmp_builder, entry_block);
  }

  LLVMTypeRef llvm_type = cg_type_get_llvm(cg, type);
  char *c_name = sv_to_cstr(name);
  LLVMValueRef alloca = LLVMBuildAlloca(tmp_builder, llvm_type, c_name);

  // Align the alloca to the type's natural alignment when available.
  size_t align = type->alignment ? type->alignment : type->size;
  if (align == 0)
    align = 1;
  LLVMSetAlignment(alloca, (unsigned)align);

  free(c_name);
  LLVMDisposeBuilder(tmp_builder);
  return alloca;
}

LLVMValueRef cg_alloca_in_entry_uninitialized(Codegen *cg, Type *type,
                                              StringView name) {
  LLVMBuilderRef tmp_builder = LLVMCreateBuilderInContext(cg->context);
  LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(cg->current_function);

  LLVMValueRef first_instr = LLVMGetFirstInstruction(entry_block);
  if (first_instr) {
    LLVMPositionBuilderBefore(tmp_builder, first_instr);
  } else {
    LLVMPositionBuilderAtEnd(tmp_builder, entry_block);
  }

  LLVMTypeRef llvm_type = cg_type_get_llvm(cg, type);
  char *c_name = sv_to_cstr(name);
  LLVMValueRef alloca = LLVMBuildAlloca(tmp_builder, llvm_type, c_name);

  size_t align = type->alignment ? type->alignment : type->size;
  if (align == 0)
    align = 1;
  LLVMSetAlignment(alloca, (unsigned)align);

  free(c_name);
  LLVMDisposeBuilder(tmp_builder);
  return alloca;
}

LLVMValueRef cg_get_address(Codegen *cg, AstNode *node) {
  if (!node)
    return NULL;

  switch (node->tag) {
  case NODE_VAR: {
    CgSym *sym = CGSymbolTable_find(cg->current_scope, node->var.value);
    if (!sym) {
      panic("Undefined variable '" SV_FMT "' during address-of",
            SV_ARG(node->var.value));
    }

    if (sym->type && sym->type->kind == KIND_RESULT && node->resolved_type &&
        node->resolved_type != sym->type) {
      LLVMTypeRef result_ty = cg_type_get_llvm(cg, sym->type);
      LLVMValueRef result_ptr = sym->value;
      if (type_equals(node->resolved_type, sym->type->data.result.success)) {
        LLVMValueRef success_field = LLVMBuildStructGEP2(
            cg->builder, result_ty, result_ptr, 0, "result_success_addr");
        LLVMTypeRef target_ptr_ty =
            LLVMPointerType(cg_type_get_llvm(cg, node->resolved_type), 0);
        return LLVMBuildBitCast(cg->builder, success_field, target_ptr_ty,
                                "result_success_ptr");
      }
      if (node->resolved_type->kind == KIND_ERROR) {
        LLVMValueRef payload_field = LLVMBuildStructGEP2(
            cg->builder, result_ty, result_ptr, 2, "result_payload_addr");
        LLVMTypeRef target_ptr_ty =
            LLVMPointerType(cg_type_get_llvm(cg, node->resolved_type), 0);
        return LLVMBuildBitCast(cg->builder, payload_field, target_ptr_ty,
                                "result_payload_ptr");
      }
    }

    if (sym->is_direct_value) {
      LLVMValueRef direct_alloca =
          cg_alloca_in_entry_uninitialized(cg, sym->type, sym->name);

      LLVMBuilderRef tmp_builder = LLVMCreateBuilderInContext(cg->context);
      LLVMValueRef next = LLVMGetNextInstruction(direct_alloca);
      if (next) {
        LLVMPositionBuilderBefore(tmp_builder, next);
      } else {
        LLVMPositionBuilderAtEnd(tmp_builder,
                                 LLVMGetEntryBasicBlock(cg->current_function));
      }
      LLVMBuildStore(tmp_builder, sym->value, direct_alloca);
      LLVMDisposeBuilder(tmp_builder);

      sym->value = direct_alloca;
      sym->is_direct_value = false;
      return direct_alloca;
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
      CgSym *sym =
          CGSymbolTable_find(cg->current_scope, node->field.object->var.value);
      if (sym && sym->type) {
        obj_type = sym->type;
      }
    }
    if (!obj_type) {
      panic("Expected FIELD object to have a resolved type");
    }
    LLVMTypeRef llvm_obj_type = cg_type_get_llvm(cg, obj_type);
    (void)llvm_obj_type;
    if (obj_type->kind == KIND_POINTER) {
      if (!obj_type->data.pointer_to) {
        panic("Pointer object has no target type");
      }
      Type *target = obj_type->data.pointer_to;
      LLVMTypeRef load_ty = cg_type_get_llvm(cg, obj_type);
      obj_ptr = LLVMBuildLoad2(cg->builder, load_ty, obj_ptr, "deref_ptr");
      obj_type = target;
    }
    if (obj_type->kind != KIND_STRUCT && obj_type->kind != KIND_UNION) {
      panic(
          "Expected FIELD object to resolve to a struct or union type, got %s",
          type_to_name(obj_type));
    }
    LLVMTypeRef obj_ptr_ty = LLVMTypeOf(obj_ptr);
    if (LLVMGetTypeKind(obj_ptr_ty) != LLVMPointerTypeKind) {
      panic("Expected field object address to be a pointer");
    }

    if (obj_type->kind == KIND_UNION) {
      Type *owner = NULL;
      Member *m = type_find_union_field(obj_type, node->field.field, &owner);
      if (!m)
        return NULL;
      if (!m->type) {
        panic("Field member has no type");
      }
      LLVMTypeRef owner_ty = cg_type_get_llvm(cg, owner);
      LLVMValueRef owner_ptr =
          LLVMBuildBitCast(cg->builder, obj_ptr, LLVMPointerType(owner_ty, 0),
                           "union_owner_ptr");
      LLVMTypeRef field_ty = cg_type_get_llvm(cg, m->type);
      if (owner == obj_type || owner->kind == KIND_UNION) {
        return LLVMBuildBitCast(cg->builder, owner_ptr,
                                LLVMPointerType(field_ty, 0),
                                "union_field_addr");
      }
      LLVMValueRef field_addr = LLVMBuildStructGEP2(
          cg->builder, owner_ty, owner_ptr, m->index, "union_field_addr");
      return field_addr;
    }

    LLVMTypeRef struct_ty = cg_type_get_llvm(cg, obj_type);
    Member *m = type_get_member(obj_type, node->field.field);
    if (!m)
      return NULL;
    if (!m->type) {
      panic("Field member has no type");
    }
    LLVMValueRef field_addr = LLVMBuildStructGEP2(
        cg->builder, struct_ty, obj_ptr, m->index, "field_addr");
    return field_addr;
  }

  case NODE_UNARY: {
    if (node->unary.op == OP_DEREF) {
      return cg_expression(cg, node->unary.expr);
    }
    return NULL;
  }

  case NODE_INDEX: {
    LLVMValueRef array_ptr_addr = cg_get_address(cg, node->index.array);
    Type *array_type = node->index.array->resolved_type;
    if (!array_type) {
      panic("Internal error: indexed expression has no resolved type");
    }

    LLVMValueRef index_val = cg_expression(cg, node->index.index);
    LLVMValueRef index_i64 =
        cg_cast_value(cg, index_val, node->index.index->resolved_type,
                      LLVMInt64TypeInContext(cg->context));

    if (array_type->kind == KIND_POINTER) {
      LLVMTypeRef array_ty = cg_type_get_llvm(cg, array_type);
      LLVMValueRef array_ptr =
          LLVMBuildLoad2(cg->builder, array_ty, array_ptr_addr, "pointer_load");
      LLVMTypeRef elem_ty = cg_type_get_llvm(cg, array_type->data.pointer_to);
      return LLVMBuildGEP2(cg->builder, elem_ty, array_ptr,
                           (LLVMValueRef[]){index_i64}, 1, "ptr_index");
    }

    if (type_is_array_struct(array_type) && array_type->fixed_array_len > 0) {
      if (array_type->data.instance.generic_args.len == 0) {
        panic("Internal error: fixed array index missing element type");
      }

      LLVMValueRef len_i64 = LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                                          array_type->fixed_array_len, 0);

      LLVMValueRef in_bounds = LLVMBuildICmp(cg->builder, LLVMIntULT, index_i64,
                                             len_i64, "in_bounds");

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

      LLVMTypeRef fixed_array_ty = cg_type_get_llvm(cg, array_type);
      return LLVMBuildGEP2(
          cg->builder, fixed_array_ty, array_ptr_addr,
          (LLVMValueRef[]){
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0),
              index_i64},
          2, "fixed_index_ptr");
    }

    if (type_is_slice_struct(array_type)) {
      LLVMTypeRef slice_ty = cg_type_get_llvm(cg, array_type);
      LLVMValueRef len_ptr = LLVMBuildStructGEP2(cg->builder, slice_ty,
                                                 array_ptr_addr, 1, "len_ptr");
      LLVMValueRef len_val = LLVMBuildLoad2(
          cg->builder, LLVMInt64TypeInContext(cg->context), len_ptr, "len_val");

      LLVMValueRef in_bounds = LLVMBuildICmp(cg->builder, LLVMIntULT, index_i64,
                                             len_val, "in_bounds");

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
          cg->builder, slice_ty, array_ptr_addr, 0, "data_field_ptr");
      LLVMTypeRef elem_ptr_ty = cg_type_get_llvm(
          cg, type_get_pointer(cg->type_ctx, node->resolved_type));
      LLVMValueRef actual_data_ptr = LLVMBuildLoad2(
          cg->builder, elem_ptr_ty, data_ptr_gep, "load_data_ptr");
      return LLVMBuildGEP2(
          cg->builder, cg_type_get_llvm(cg, node->resolved_type),
          actual_data_ptr, (LLVMValueRef[]){index_i64}, 1, "element_ptr");
    }

    LLVMValueRef array_struct_ptr = array_ptr_addr;
    LLVMTypeRef array_struct_ty = cg_type_get_llvm(cg, array_type);

    // Dynamic arrays use their runtime length field for bounds checks.
    LLVMValueRef len_ptr = LLVMBuildStructGEP2(cg->builder, array_struct_ty,
                                               array_struct_ptr, 1, "len_ptr");
    LLVMValueRef dim0_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), len_ptr, "len_val");

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

    return element_addr;
  }

  default:
    return NULL;
  }
}
