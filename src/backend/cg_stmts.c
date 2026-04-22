#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cg_control_flow_statement(Codegen *cg, AstNode *node);
LLVMValueRef cg_build_error_payload_array(Codegen *cg,
                                          LLVMValueRef error_val,
                                          Type *error_type,
                                          Type *result_type);
LLVMValueRef cg_build_result_value(Codegen *cg, AstNode *expr,
                                   Type *result_type);
LLVMValueRef cg_extract_tagged_union_payload(Codegen *cg,
                                             LLVMValueRef union_val,
                                             Type *union_t,
                                             Type *variant_type);

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
    LLVMTypeRef target_ty = cg_type_get_llvm(cg, target_type);
    if (target_type->kind == KIND_UNION && target_type->is_tagged_union) {
      init_val = cg_make_tagged_union(
          cg, init_val, node->var_decl.value->resolved_type, target_type);
    }
    if (LLVMTypeOf(init_val) != target_ty) {
      init_val = cg_cast_value(cg, init_val,
                               node->var_decl.value->resolved_type, target_ty);
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
    LLVMValueRef val = NULL;
    bool has_value = node->return_stmt.expr != NULL;
    if (has_value) {
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
    }
    if (cg->current_function_uses_arena) {
      CgFunc *arena_pop =
          cg_find_system_function(cg, sv_from_cstr("__tyna_string_arena_pop"));
      if (arena_pop) {
        LLVMBuildCall2(cg->builder, arena_pop->type, arena_pop->value, NULL, 0,
                       "");
      }
    }
    if (has_value) {
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
