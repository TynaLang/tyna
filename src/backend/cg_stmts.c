#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/sema.h"
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool cg_type_needs_drop(Type *type, List *seen) {
  if (!type)
    return false;

  if (type_is_array_struct(type))
    return type->fixed_array_len == 0;

  if (type_is_slice_struct(type))
    return false;

  for (size_t i = 0; i < seen->len; i++) {
    if (seen->items[i] == type)
      return false;
  }

  switch (type->kind) {
  case KIND_STRING_BUFFER:
    return true;
  case KIND_POINTER:
  case KIND_PRIMITIVE:
    return false;
  case KIND_STRUCT:
  case KIND_UNION:
    List_push(seen, type);
    for (size_t i = 0; i < type->members.len; i++) {
      Member *member = type->members.items[i];
      if (member && cg_type_needs_drop(member->type, seen)) {
        List_pop(seen);
        return true;
      }
    }
    List_pop(seen);
    return type->needs_drop;
  default:
    return type->needs_drop;
  }
}

static bool cg_symbol_needs_drop(Type *type) {
  List seen;
  List_init(&seen);
  bool needs_drop = cg_type_needs_drop(type, &seen);
  List_free(&seen, 0);
  return needs_drop;
}

static void cg_ensure_symbol_storage(Codegen *cg, CgSym *sym) {
  if (!sym || !sym->is_direct_value)
    return;

  LLVMValueRef storage =
      cg_alloca_in_entry_uninitialized(cg, sym->type, sym->name);
  LLVMBuildStore(cg->builder, sym->value, storage);
  sym->value = storage;
  sym->is_direct_value = false;
}

static void cg_emit_drop_for_value(Codegen *cg, Type *type,
                                   LLVMValueRef value_ptr) {
  if (!type || !value_ptr || !cg_symbol_needs_drop(type))
    return;

  if (type->drop_fn) {
    CgFunc *drop_func =
        cg_find_system_function(cg, sv_from_cstr(type->drop_fn));
    if (!drop_func || !drop_func->value || !drop_func->type) {
      fprintf(stderr, "Internal Error: Missing drop function '%s' for type\n",
              type->drop_fn);
      exit(1);
    }
    LLVMBuildCall2(cg->builder, drop_func->type, drop_func->value, &value_ptr,
                   1, "");
    return;
  }

  if (type_is_array_struct(type)) {
    if (type->fixed_array_len == 0) {
      CgFunc *free_func =
          cg_find_system_function(cg, sv_from_cstr("__tyna_array_free"));
      if (!free_func || !free_func->value || !free_func->type) {
        fprintf(
            stderr,
            "Internal Error: Missing array free function for dynamic array\n");
        exit(1);
      }
      LLVMBuildCall2(cg->builder, free_func->type, free_func->value, &value_ptr,
                     1, "");
    }
    return;
  }

  if (type->kind == KIND_STRUCT) {
    LLVMTypeRef struct_ty = cg_type_get_llvm(cg, type);
    for (size_t i = 0; i < type->members.len; i++) {
      Member *member = type->members.items[i];
      if (!member || !member->type || !cg_symbol_needs_drop(member->type))
        continue;

      LLVMValueRef field_ptr = LLVMBuildStructGEP2(
          cg->builder, struct_ty, value_ptr, member->index, "drop_field_ptr");
      cg_emit_drop_for_value(cg, member->type, field_ptr);
    }
  }
}

void cg_emit_drops(Codegen *cg, List *symbols_to_drop) {
  if (!cg || !symbols_to_drop)
    return;

  for (size_t i = 0; i < symbols_to_drop->len; i++) {
    Symbol *sym = symbols_to_drop->items[i];
    if (!sym || !sym->type || !cg_symbol_needs_drop(sym->type))
      continue;

    CgSym *cgsym = CGSymbolTable_find(cg->current_scope, sym->name);
    if (!cgsym) {
      panic("Internal error: missing codegen symbol for drop of '" SV_FMT "'",
            SV_ARG(sym->name));
    }

    cg_ensure_symbol_storage(cg, cgsym);
    cg_emit_drop_for_value(cg, cgsym->type ? cgsym->type : sym->type,
                           cgsym->value);
  }
}

void cg_emit_scope_drops(Codegen *cg, CgSymtab *scope) {
  if (!cg || !scope)
    return;

  for (int i = (int)scope->symbols.len - 1; i >= 0; i--) {
    CgSym *sym = scope->symbols.items[i];
    if (!sym || !sym->type || !cg_symbol_needs_drop(sym->type))
      continue;

    cg_ensure_symbol_storage(cg, sym);
    cg_emit_drop_for_value(cg, sym->type, sym->value);
  }
}

void cg_emit_scope_chain_drops(Codegen *cg) {
  if (!cg || !cg->current_scope)
    return;

  for (CgSymtab *scope = cg->current_scope;
       scope != NULL && scope->parent != NULL; scope = scope->parent) {
    cg_emit_scope_drops(cg, scope);
  }
}

void cg_control_flow_statement(Codegen *cg, AstNode *node);
LLVMValueRef cg_build_error_payload_array(Codegen *cg, LLVMValueRef error_val,
                                          Type *error_type, Type *result_type);
LLVMValueRef cg_build_result_value(Codegen *cg, AstNode *expr,
                                   Type *result_type);
LLVMValueRef cg_extract_tagged_union_payload(Codegen *cg,
                                             LLVMValueRef union_val,
                                             Type *union_t, Type *variant_type);

static bool cg_type_requires_zero_init(Type *t) {
  if (!t)
    return false;

  switch (t->kind) {
  case KIND_STRING_BUFFER:
  case KIND_STRUCT:
  case KIND_UNION:
    return true;
  case KIND_PRIMITIVE:
  case KIND_POINTER:
    return false;
  default:
    return type_is_array_struct(t);
  }
}

static bool cg_is_fixed_to_dynamic_array_conversion(Type *to, Type *from) {
  if (!to || !from)
    return false;
  if (!type_is_array_struct(to) || !type_is_array_struct(from))
    return false;

  if (to->fixed_array_len != 0 || from->fixed_array_len == 0)
    return false;

  if (!to->data.instance.from_template || !from->data.instance.from_template)
    return false;
  if (to->data.instance.from_template != from->data.instance.from_template)
    return false;

  if (to->data.instance.generic_args.len == 0 ||
      from->data.instance.generic_args.len == 0)
    return false;

  return type_equals(to->data.instance.generic_args.items[0],
                     from->data.instance.generic_args.items[0]);
}

static bool cg_is_dynamic_array_target(Type *t) {
  return t && type_is_array_struct(t) && t->fixed_array_len == 0 &&
         t->data.instance.generic_args.len > 0;
}

void cg_var_decl(Codegen *cg, AstNode *node) {
  bool is_global_decl =
      cg->current_function_ref &&
      sv_eq(cg->current_function_ref->name, sv_from_cstr("__system__main__"));

  LLVMTypeRef llvm_type = cg_type_get_llvm(cg, node->var_decl.declared_type);
  LLVMValueRef ptr;

  if (is_global_decl) {
    char *name = sv_to_cstr(node->var_decl.name->var.value);
    ptr = LLVMAddGlobal(cg->module, llvm_type, name);
    free(name);

    LLVMSetInitializer(ptr, LLVMConstNull(llvm_type));
    if (node->var_decl.is_const) {
      LLVMSetGlobalConstant(ptr, true);
    }
  } else {
    ptr = cg_alloca_in_entry_uninitialized(cg, node->var_decl.declared_type,
                                           node->var_decl.name->var.value);
  }

  CGSymbolTable_add(cg->current_scope, node->var_decl.name->var.value,
                    node->var_decl.declared_type, ptr);

  if (node->var_decl.value && node->var_decl.declared_type &&
      node->var_decl.declared_type->kind == KIND_STRING_BUFFER) {
    /* Move semantics are enforced by semantic analysis; backend no longer
       tracks moved variables for cleanup. */
  }

  if (node->var_decl.value) {
    LLVMValueRef init_val = cg_expression(cg, node->var_decl.value);
    Type *target_type = node->var_decl.declared_type;
    Type *source_type = node->var_decl.value->resolved_type;
    LLVMTypeRef target_ty = cg_type_get_llvm(cg, target_type);
    LLVMTypeRef init_ty = LLVMTypeOf(init_val);

    bool needs_array_from_stack =
        cg_is_fixed_to_dynamic_array_conversion(target_type, source_type) ||
        (cg_is_dynamic_array_target(target_type) &&
         LLVMGetTypeKind(init_ty) == LLVMArrayTypeKind);

    if (needs_array_from_stack) {
      CgFunc *from_stack_fn =
          cg_find_system_function(cg, sv_from_cstr("__tyna_array_from_stack"));
      if (!from_stack_fn) {
        panic(
            "Internal error: __tyna_array_from_stack runtime function missing");
      }

      // Keep destination in a known state even if runtime conversion exits
      // early for invalid inputs.
      LLVMBuildStore(cg->builder, LLVMConstNull(target_ty), ptr);

      LLVMTypeRef source_ty = init_ty;
      LLVMValueRef stack_tmp =
          LLVMBuildAlloca(cg->builder, source_ty, "fixed_array_stack_tmp");
      LLVMBuildStore(cg->builder, init_val, stack_tmp);

      LLVMTypeRef i8_ptr =
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
      LLVMValueRef out_ptr_i8 =
          LLVMBuildBitCast(cg->builder, ptr, i8_ptr, "array_from_stack_out");
      LLVMValueRef stack_ptr_i8 = LLVMBuildBitCast(
          cg->builder, stack_tmp, i8_ptr, "array_from_stack_src");

      Type *elem_type = target_type->data.instance.generic_args.items[0];
      uint64_t fixed_count = 0;
      if (source_type && source_type->fixed_array_len > 0) {
        fixed_count = source_type->fixed_array_len;
      } else if (LLVMGetTypeKind(init_ty) == LLVMArrayTypeKind) {
        fixed_count = (uint64_t)LLVMGetArrayLength(init_ty);
      } else {
        panic("Internal error: array-from-stack source is not fixed-size");
      }
      LLVMValueRef count =
          LLVMConstInt(LLVMInt64TypeInContext(cg->context), fixed_count, 0);
      LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
      const char *data_layout = LLVMGetDataLayout(cg->module);
      LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
      unsigned long long elem_size_bytes = LLVMABISizeOfType(td, llvm_elem_ty);
      LLVMDisposeTargetData(td);
      LLVMValueRef elem_size =
          LLVMConstInt(LLVMInt64TypeInContext(cg->context), elem_size_bytes, 0);

      LLVMBuildCall2(
          cg->builder, from_stack_fn->type, from_stack_fn->value,
          (LLVMValueRef[]){out_ptr_i8, stack_ptr_i8, count, elem_size}, 4, "");
      return;
    }

    if (target_type->kind == KIND_UNION && target_type->is_tagged_union) {
      init_val = cg_make_tagged_union(cg, init_val, source_type, target_type);
    }
    if (LLVMTypeOf(init_val) != target_ty) {
      init_val = cg_cast_value(cg, init_val, source_type, target_ty);
    }

    if (is_global_decl) {
      if (LLVMIsConstant(init_val)) {
        LLVMSetInitializer(ptr, init_val);
      } else {
        LLVMBuildStore(cg->builder, init_val, ptr);
      }
    } else {
      LLVMBuildStore(cg->builder, init_val, ptr);
    }
  } else if (!is_global_decl &&
             cg_type_requires_zero_init(node->var_decl.declared_type)) {
    LLVMValueRef zero =
        LLVMConstNull(cg_type_get_llvm(cg, node->var_decl.declared_type));
    LLVMBuildStore(cg->builder, zero, ptr);
  }
}

void cg_print_stmt(Codegen *cg, AstNode *node) {
  CgFunc *print_fn = cg_find_function(cg, sv_from_cstr("printf"));
  if (!print_fn)
    return;

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);

  for (size_t i = 0; i < node->print_stmt.values.len; i++) {
    AstNode *val_node = node->print_stmt.values.items[i];
    LLVMValueRef val = cg_expression(cg, val_node);
    Type *resolved_type = val_node->resolved_type;
    LLVMTypeRef val_ty = LLVMTypeOf(val);
    LLVMTypeKind kind = LLVMGetTypeKind(val_ty);

    const char *fmt_str = "%p";

    if (resolved_type) {
      if (resolved_type->kind == KIND_PRIMITIVE) {
        switch (resolved_type->data.primitive) {
        case PRIM_BOOL: {
          fmt_str = "%s";
          LLVMValueRef true_str =
              cg_get_string_constant_ptr(cg, sv_from_cstr("true"));
          LLVMValueRef false_str =
              cg_get_string_constant_ptr(cg, sv_from_cstr("false"));
          LLVMValueRef is_true =
              LLVMBuildICmp(cg->builder, LLVMIntEQ, val,
                            LLVMConstInt(val_ty, 1, 0), "is_true");
          val = LLVMBuildSelect(cg->builder, is_true, true_str, false_str,
                                "bool_ptr");
          break;
        }
        case PRIM_CHAR:
          fmt_str = "%c";
          val = LLVMBuildZExt(cg->builder, val, i32_ty, "char_val");
          break;
        case PRIM_I8:
        case PRIM_I16:
        case PRIM_I32:
          fmt_str = "%d";
          if (LLVMGetIntTypeWidth(val_ty) != 32) {
            val = LLVMBuildSExt(cg->builder, val, i32_ty, "int_prom");
          }
          break;
        case PRIM_U8:
        case PRIM_U16:
        case PRIM_U32:
          fmt_str = "%u";
          if (LLVMGetIntTypeWidth(val_ty) != 32) {
            val = LLVMBuildZExt(cg->builder, val, i32_ty, "uint_prom");
          }
          break;
        case PRIM_I64:
          fmt_str = "%ld";
          if (LLVMGetIntTypeWidth(val_ty) != 64) {
            val =
                LLVMBuildSExt(cg->builder, val,
                              LLVMInt64TypeInContext(cg->context), "i64_prom");
          }
          break;
        case PRIM_U64:
          fmt_str = "%lu";
          if (LLVMGetIntTypeWidth(val_ty) != 64) {
            val =
                LLVMBuildZExt(cg->builder, val,
                              LLVMInt64TypeInContext(cg->context), "u64_prom");
          }
          break;
        case PRIM_F32:
          fmt_str = "%f";
          val = LLVMBuildFPExt(cg->builder, val,
                               LLVMDoubleTypeInContext(cg->context), "f_prom");
          break;
        case PRIM_F64:
          fmt_str = "%f";
          break;
        case PRIM_STRING: {
          fmt_str = "%s";
          val = LLVMBuildExtractValue(cg->builder, val, 0, "str_ptr");
          break;
        }
        default:
          fmt_str = "%p";
          break;
        }
      } else if (resolved_type->kind == KIND_STRING_BUFFER) {
        fmt_str = "%s";
        LLVMValueRef ptr_val =
            LLVMBuildExtractValue(cg->builder, val, 0, "strbuf_ptr");
        LLVMValueRef len_val =
            LLVMBuildExtractValue(cg->builder, val, 1, "strbuf_len");
        LLVMTypeRef string_ty =
            cg_type_get_llvm(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
        LLVMValueRef result = LLVMGetUndef(string_ty);
        result =
            LLVMBuildInsertValue(cg->builder, result, ptr_val, 0, "buf2s0");
        val = LLVMBuildInsertValue(cg->builder, result, len_val, 1, "buf2s");
      } else if (resolved_type->kind == KIND_POINTER) {
        if (resolved_type->data.pointer_to->kind == KIND_PRIMITIVE &&
            resolved_type->data.pointer_to->data.primitive == PRIM_CHAR) {
          fmt_str = "%s";
        } else {
          fmt_str = "%p";
        }
      }
    }

    if (fmt_str == NULL) {
      fmt_str = "%p";
    }

    LLVMValueRef fmt_ptr =
        cg_get_string_constant_ptr(cg, sv_from_cstr(fmt_str));
    LLVMValueRef args[] = {fmt_ptr, val};
    LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, args, 2, "");
  }

  LLVMValueRef nl_ptr = cg_get_string_constant_ptr(cg, sv_from_cstr("\n"));
  LLVMValueRef nl_args[] = {nl_ptr};
  LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, nl_args, 1, "");
}

LLVMValueRef cg_build_error_payload_array(Codegen *cg, LLVMValueRef error_val,
                                          Type *error_type, Type *result_type) {
  LLVMTypeRef result_ty = cg_type_get_llvm(cg, result_type);
  LLVMTypeRef payload_ty = LLVMStructGetTypeAtIndex(result_ty, 2);
  LLVMValueRef tmp =
      LLVMBuildAlloca(cg->builder, payload_ty, "result_error_payload");
  LLVMBuildStore(cg->builder, LLVMConstNull(payload_ty), tmp);

  LLVMTypeRef error_ty = cg_type_get_llvm(cg, error_type);
  LLVMValueRef error_struct = error_val;
  if (LLVMGetTypeKind(LLVMTypeOf(error_val)) == LLVMPointerTypeKind) {
    error_struct =
        LLVMBuildLoad2(cg->builder, error_ty, error_val, "error_struct_load");
  }
  LLVMValueRef error_ptr = LLVMBuildBitCast(
      cg->builder, tmp, LLVMPointerType(error_ty, 0), "result_error_tmp");
  LLVMBuildStore(cg->builder, error_struct, error_ptr);

  return LLVMBuildLoad2(cg->builder, payload_ty, tmp, "result_payload");
}

LLVMValueRef cg_build_result_value(Codegen *cg, AstNode *expr,
                                   Type *result_type) {
  if (!expr) {
    return LLVMConstNull(cg_type_get_llvm(cg, result_type));
  }

  LLVMValueRef expr_val = cg_expression(cg, expr);
  Type *expr_type = expr->resolved_type;
  if (!expr_type) {
    expr_type = type_get_primitive(cg->type_ctx, PRIM_UNKNOWN);
  }

  if (expr_type->kind == KIND_RESULT) {
    return expr_val;
  }

  LLVMTypeRef result_ty = cg_type_get_llvm(cg, result_type);
  if (result_type->kind != KIND_RESULT)
    return expr_val;

  LLVMValueRef result = LLVMGetUndef(result_ty);
  if (expr_type->kind != KIND_ERROR) {
    LLVMValueRef success_val =
        cg_cast_value(cg, expr_val, expr_type,
                      cg_type_get_llvm(cg, result_type->data.result.success));
    result = LLVMBuildInsertValue(cg->builder, result, success_val, 0,
                                  "result_success");
  }

  LLVMTypeRef tag_ty = LLVMInt16TypeInContext(cg->context);
  LLVMValueRef tag = LLVMConstInt(tag_ty, 0, false);
  if (expr_type->kind == KIND_ERROR) {
    int idx = cg_result_error_tag_index(result_type, expr_type);
    if (idx <= 0) {
      panic("error type '%s' not in function result error set",
            type_to_name(expr_type));
    }
    tag = LLVMConstInt(tag_ty, (unsigned)idx, false);
  }
  result = LLVMBuildInsertValue(cg->builder, result, tag, 1, "result_tag");

  if (expr_type->kind == KIND_ERROR) {
    LLVMValueRef payload_val =
        cg_build_error_payload_array(cg, expr_val, expr_type, result_type);
    result = LLVMBuildInsertValue(cg->builder, result, payload_val, 2,
                                  "result_payload");
  }
  return result;
}

LLVMValueRef cg_extract_tagged_union_payload(Codegen *cg,
                                             LLVMValueRef union_val,
                                             Type *union_t,
                                             Type *variant_type) {
  LLVMTypeRef union_ty = cg_type_get_llvm(cg, union_t);
  LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, union_ty, "tagged_union_tmp");
  LLVMBuildStore(cg->builder, union_val, tmp);
  LLVMValueRef payload_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 1,
                                                 "tagged_union_payload_ptr");
  LLVMTypeRef target_ty = cg_type_get_llvm(cg, variant_type);
  LLVMValueRef store_ptr =
      LLVMBuildBitCast(cg->builder, payload_ptr, LLVMPointerType(target_ty, 0),
                       "tagged_union_payload_cast");
  return LLVMBuildLoad2(cg->builder, target_ty, store_ptr,
                        "tagged_union_value");
}

void cg_statement(Codegen *cg, AstNode *node) {
  if (!node)
    return;

  if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    return;

  switch (node->tag) {
  case NODE_VAR_DECL:
    cg_var_decl(cg, node);
    break;
  case NODE_PRINT_STMT:
    cg_print_stmt(cg, node);
    break;
  case NODE_EXPR_STMT:
    cg_expression(cg, node->expr_stmt.expr);
    break;
  case NODE_RETURN_STMT: {
    LLVMTypeRef fn_ret_ty = NULL;
    bool fn_returns_void = false;
    if (cg->current_function_ref && cg->current_function_ref->decl &&
        cg->current_function_ref->decl->func_decl.return_type) {
      fn_ret_ty = cg_type_get_llvm(
          cg, cg->current_function_ref->decl->func_decl.return_type);
      fn_returns_void = LLVMGetTypeKind(fn_ret_ty) == LLVMVoidTypeKind;
    }

    LLVMValueRef val = NULL;
    bool has_value = node->return_stmt.expr != NULL;
    if (has_value && !fn_returns_void) {
      if (cg->current_function_ref && cg->current_function_ref->decl &&
          cg->current_function_ref->decl->func_decl.return_type &&
          cg->current_function_ref->decl->func_decl.return_type->kind ==
              KIND_RESULT) {
        val = cg_build_result_value(
            cg, node->return_stmt.expr,
            cg->current_function_ref->decl->func_decl.return_type);
      } else {
        val = cg_expression(cg, node->return_stmt.expr);
      }
    } else if (has_value && fn_returns_void) {
      cg_expression(cg, node->return_stmt.expr);
      has_value = false;
    }

    if (has_value && cg->current_function_uses_arena &&
        node->return_stmt.expr && node->return_stmt.expr->resolved_type &&
        node->return_stmt.expr->resolved_type->kind == KIND_STRING_BUFFER) {
      CgFunc *promote_fn = cg_find_system_function(
          cg, sv_from_cstr("__tyna_string_promote_if_arena"));
      if (promote_fn) {
        LLVMValueRef tmp =
            cg_alloca_in_entry(cg, node->return_stmt.expr->resolved_type,
                               sv_from_cstr("return_string_buf"));
        LLVMBuildStore(cg->builder, val, tmp);
        LLVMBuildCall2(cg->builder, promote_fn->type, promote_fn->value,
                       (LLVMValueRef[]){tmp}, 1, "");
        val = LLVMBuildLoad2(
            cg->builder,
            cg_type_get_llvm(cg, node->return_stmt.expr->resolved_type), tmp,
            "promoted_return_string_buf");
      }
    }
    if (cg->current_function_uses_arena) {
      CgFunc *arena_pop =
          cg_find_system_function(cg, sv_from_cstr("__tyna_string_arena_pop"));
      if (arena_pop) {
        LLVMBuildCall2(cg->builder, arena_pop->type, arena_pop->value, NULL, 0,
                       "");
      }
    }
    if (has_value && !fn_returns_void) {
      LLVMBuildRet(cg->builder, val);
    } else {
      LLVMBuildRetVoid(cg->builder);
    }
    break;
  }
  case NODE_STRUCT_DECL:
  case NODE_UNION_DECL:
  case NODE_ERROR_DECL:
  case NODE_ERROR_SET_DECL:
  case NODE_TYPE_ALIAS:
  case NODE_IMPORT:
    break;
  case NODE_DEFER: {
    if (cg->defers.len == 0)
      panic("Defer outside of scope");

    List *defer_list = List_get(&cg->defers, cg->defers.len - 1);
    List_push(defer_list, node);
    break;
  }
  case NODE_INTRINSIC_COMPARE:
    panic("Intrinsic compare is not supported");
    break;
  case NODE_BLOCK:
  case NODE_IF_STMT:
  case NODE_SWITCH_STMT:
  case NODE_LOOP_STMT:
  case NODE_BREAK:
  case NODE_CONTINUE:
  case NODE_FOR_IN_STMT:
  case NODE_WHILE_STMT:
  case NODE_FOR_STMT:
    cg_control_flow_statement(cg, node);
    break;
  default:
    fprintf(stderr, "Codegen: Unhandled statement tag %d\n", node->tag);
    break;
  }
}
