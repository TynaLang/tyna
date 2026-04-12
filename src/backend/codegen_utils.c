#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
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
    if (!obj_type) {
      panic("Expected FIELD object to have a resolved type");
    }
    LLVMTypeRef llvm_obj_type = cg_get_llvm_type(cg, obj_type);
    (void)llvm_obj_type;
    if (obj_type->kind == KIND_POINTER) {
      if (!obj_type->data.pointer_to) {
        panic("Pointer object has no target type");
      }
      Type *target = obj_type->data.pointer_to;
      LLVMTypeRef load_ty = cg_get_llvm_type(cg, obj_type);
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
      LLVMTypeRef owner_ty = cg_get_llvm_type(cg, owner);
      LLVMValueRef owner_ptr =
          LLVMBuildBitCast(cg->builder, obj_ptr, LLVMPointerType(owner_ty, 0),
                           "union_owner_ptr");
      LLVMTypeRef field_ty = cg_get_llvm_type(cg, m->type);
      if (owner == obj_type || owner->kind == KIND_UNION) {
        return LLVMBuildBitCast(cg->builder, owner_ptr,
                                LLVMPointerType(field_ty, 0),
                                "union_field_addr");
      }
      LLVMValueRef field_addr = LLVMBuildStructGEP2(
          cg->builder, owner_ty, owner_ptr, m->index, "union_field_addr");
      return field_addr;
    }

    LLVMTypeRef struct_ty = cg_get_llvm_type(cg, obj_type);
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

    if (array_type->kind == KIND_POINTER) {
      LLVMTypeRef array_ty = cg_get_llvm_type(cg, array_type);
      LLVMValueRef array_ptr =
          LLVMBuildLoad2(cg->builder, array_ty, array_ptr_addr, "pointer_load");
      LLVMValueRef index_val = cg_expression(cg, node->index.index);
      LLVMValueRef index_i64 =
          cg_cast_value(cg, index_val, node->index.index->resolved_type,
                        LLVMInt64TypeInContext(cg->context));
      LLVMTypeRef elem_ty = cg_get_llvm_type(cg, array_type->data.pointer_to);
      return LLVMBuildGEP2(cg->builder, elem_ty, array_ptr,
                           (LLVMValueRef[]){index_i64}, 1, "ptr_index");
    }

    LLVMValueRef array_struct_ptr = array_ptr_addr;
    LLVMTypeRef array_struct_ty = cg_get_llvm_type(cg, array_type);

    // 1. Load rank
    LLVMValueRef rank_ptr = LLVMBuildStructGEP2(
        cg->builder, array_struct_ty, array_struct_ptr, 3, "rank_ptr");
    LLVMValueRef rank_val = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), rank_ptr, "rank_val");

    LLVMValueRef dim0_val = NULL;
    LLVMValueRef dims_ptr = NULL;
    if (array_type->fixed_array_len > 0) {
      dim0_val = LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                              array_type->fixed_array_len, 0);
    } else {
      // 2. Load dims pointer
      LLVMValueRef dims_ptr_ptr = LLVMBuildStructGEP2(
          cg->builder, array_struct_ty, array_struct_ptr, 4, "dims_ptr_ptr");
      dims_ptr = LLVMBuildLoad2(
          cg->builder, LLVMPointerType(LLVMInt64TypeInContext(cg->context), 0),
          dims_ptr_ptr, "dims_ptr");
      if (!dims_ptr) {
        panic("Internal error: array dims pointer is NULL");
      }
      dim0_val =
          LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                         dims_ptr, "dim0_val");
    }

    // 3. Current index
    LLVMValueRef index_val = cg_expression(cg, node->index.index);
    LLVMValueRef index_i64 =
        cg_cast_value(cg, index_val, node->index.index->resolved_type,
                      LLVMInt64TypeInContext(cg->context));

    // If this array has a fixed compile-time length, ensure its runtime
    // metadata is initialized before accessing data.
    if (array_type->fixed_array_len > 0) {
      Type *elem_type = array_type->data.instance.generic_args.items[0];
      CGFunction *init_fixed =
          cg_find_function(cg, sv_from_cstr("__tyl_array_init_fixed"));
      if (!init_fixed) {
        panic("Internal error: fixed array init function missing");
      }
      LLVMTypeRef void_ptr_ty =
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
      LLVMValueRef array_struct_ptr_i8 = LLVMBuildBitCast(
          cg->builder, array_struct_ptr, void_ptr_ty, "fixed_array_ptr");
      LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, elem_type);
      const char *data_layout = LLVMGetDataLayout(cg->module);
      LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
      unsigned long long elem_size_bytes = LLVMABISizeOfType(td, llvm_elem_ty);
      LLVMDisposeTargetData(td);
      LLVMValueRef elem_size =
          LLVMConstInt(LLVMInt64TypeInContext(cg->context), elem_size_bytes, 0);
      LLVMValueRef fixed_len = LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                                            array_type->fixed_array_len, 0);
      LLVMBuildCall2(
          cg->builder, init_fixed->type, init_fixed->value,
          (LLVMValueRef[]){array_struct_ptr_i8, elem_size, fixed_len}, 3, "");
    }

    // 4. Bounds check: index_i64 < dim0
    if (!dim0_val) {
      panic("Internal error: fixed array bounds value not set");
    }

    LLVMValueRef in_bounds = LLVMBuildICmp(cg->builder, LLVMIntULT, index_i64,
                                           dim0_val, "in_bounds");

    LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "bounds_fail");
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "bounds_ok");

    LLVMBuildCondBr(cg->builder, in_bounds, ok_bb, fail_bb);
    LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
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

    // If the element is itself an array struct, we can return its address
    // directly. The load path will adjust rank/dims if needed.
    if (type_is_array_struct(node->resolved_type)) {
      return element_addr;
    }

    return element_addr;
  }

  default:
    return NULL;
  }
}
