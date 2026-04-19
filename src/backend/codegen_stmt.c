#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cg_var_decl(Codegen *cg, AstNode *node) {
  bool is_global_decl =
      cg->current_function_ref &&
      sv_eq(cg->current_function_ref->name, sv_from_cstr("__system__main__"));

  LLVMTypeRef llvm_type = cg_get_llvm_type(cg, node->var_decl.declared_type);
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
    ptr = cg_alloca_in_entry(cg, node->var_decl.declared_type,
                             node->var_decl.name->var.value);
  }

  CGSymbolTable_add(cg->current_scope, node->var_decl.name->var.value,
                    node->var_decl.declared_type, ptr);

  if (node->var_decl.value) {
    LLVMValueRef init_val = cg_expression(cg, node->var_decl.value);
    Type *target_type = node->var_decl.declared_type;
    LLVMTypeRef target_ty = cg_get_llvm_type(cg, target_type);
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
  }
}

static void cg_print_stmt(Codegen *cg, AstNode *node) {
  CGFunction *print_fn = cg_find_function(cg, sv_from_cstr("print"));
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
              LLVMBuildGlobalStringPtr(cg->builder, "true", "t_str");
          LLVMValueRef false_str =
              LLVMBuildGlobalStringPtr(cg->builder, "false", "f_str");
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
        LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "p_fmt");
    LLVMValueRef args[] = {fmt_ptr, val};
    LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, args, 2, "");
  }

  // Print newline after variadic print
  LLVMValueRef nl_ptr = LLVMBuildGlobalStringPtr(cg->builder, "\n", "nl_fmt");
  LLVMValueRef nl_args[] = {nl_ptr};
  LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, nl_args, 1, "");
}

static LLVMValueRef cg_build_error_payload_array(Codegen *cg,
                                                 LLVMValueRef error_val,
                                                 Type *error_type,
                                                 Type *result_type) {
  LLVMTypeRef result_ty = cg_get_llvm_type(cg, result_type);
  LLVMTypeRef payload_ty = LLVMStructGetTypeAtIndex(result_ty, 2);
  LLVMValueRef tmp =
      LLVMBuildAlloca(cg->builder, payload_ty, "result_error_payload");
  LLVMBuildStore(cg->builder, LLVMConstNull(payload_ty), tmp);

  LLVMTypeRef error_ty = cg_get_llvm_type(cg, error_type);
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

static LLVMValueRef cg_build_result_value(Codegen *cg, AstNode *expr,
                                          Type *result_type) {
  if (!expr) {
    return LLVMConstNull(cg_get_llvm_type(cg, result_type));
  }

  LLVMValueRef expr_val = cg_expression(cg, expr);
  Type *expr_type = expr->resolved_type;
  if (!expr_type) {
    expr_type = type_get_primitive(cg->type_ctx, PRIM_UNKNOWN);
  }

  if (expr_type->kind == KIND_RESULT) {
    return expr_val;
  }

  LLVMTypeRef result_ty = cg_get_llvm_type(cg, result_type);
  if (result_type->kind != KIND_RESULT)
    return expr_val;

  LLVMValueRef result = LLVMGetUndef(result_ty);
  if (expr_type->kind != KIND_ERROR) {
    LLVMValueRef success_val =
        cg_cast_value(cg, expr_val, expr_type,
                      cg_get_llvm_type(cg, result_type->data.result.success));
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

static LLVMValueRef cg_string_hash(Codegen *cg, LLVMValueRef str_val) {
  CGFunction *fn = cg_find_function(cg, sv_from_cstr("__tyl_str_hash"));
  if (!fn)
    panic("Missing __tyl_str_hash runtime helper");
  return LLVMBuildCall2(cg->builder, fn->type, fn->value, &str_val, 1,
                        "str_hash");
}

static LLVMValueRef cg_string_equals(Codegen *cg, LLVMValueRef a,
                                     LLVMValueRef b) {
  CGFunction *fn = cg_find_function(cg, sv_from_cstr("__tyl_str_equals"));
  if (!fn)
    panic("Missing __tyl_str_equals runtime helper");
  LLVMValueRef args[] = {a, b};
  LLVMValueRef result =
      LLVMBuildCall2(cg->builder, fn->type, fn->value, args, 2, "str_eq_int");
  return LLVMBuildICmp(cg->builder, LLVMIntEQ, result,
                       LLVMConstInt(LLVMInt32TypeInContext(cg->context), 1, 0),
                       "str_eq");
}

static LLVMValueRef cg_extract_tagged_union_payload(Codegen *cg,
                                                    LLVMValueRef union_val,
                                                    Type *union_t,
                                                    Type *variant_type) {
  LLVMTypeRef union_ty = cg_get_llvm_type(cg, union_t);
  LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, union_ty, "tagged_union_tmp");
  LLVMBuildStore(cg->builder, union_val, tmp);
  LLVMValueRef payload_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp, 1,
                                                 "tagged_union_payload_ptr");
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, variant_type);
  LLVMValueRef store_ptr =
      LLVMBuildBitCast(cg->builder, payload_ptr, LLVMPointerType(target_ty, 0),
                       "tagged_union_payload_cast");
  return LLVMBuildLoad2(cg->builder, target_ty, store_ptr,
                        "tagged_union_value");
}

void cg_statement(Codegen *cg, AstNode *node) {
  if (!node)
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
  case NODE_BLOCK: {
    cg_push_scope(cg);
    for (size_t i = 0; i < node->block.statements.len; i++) {
      cg_statement(cg, node->block.statements.items[i]);
    }
    cg_pop_scope(cg);
    break;
  }
  case NODE_IF_STMT: {
    LLVMValueRef cond = cg_expression(cg, node->if_stmt.condition);
    cond = cg_cast_value(cg, cond, node->if_stmt.condition->resolved_type,
                         LLVMInt1TypeInContext(cg->context));

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef then_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "then");
    LLVMBasicBlockRef else_bb =
        node->if_stmt.else_branch
            ? LLVMAppendBasicBlockInContext(cg->context, func, "else")
            : NULL;
    LLVMBasicBlockRef merge_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "if_cont");

    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb ? else_bb : merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    cg_statement(cg, node->if_stmt.then_branch);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, merge_bb);
    }

    if (else_bb) {
      LLVMPositionBuilderAtEnd(cg->builder, else_bb);
      cg_statement(cg, node->if_stmt.else_branch);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        LLVMBuildBr(cg->builder, merge_bb);
      }
    }

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    break;
  }
  case NODE_SWITCH_STMT: {
    Type *expr_type = node->switch_stmt.expr->resolved_type;
    LLVMValueRef expr_val = cg_expression(cg, node->switch_stmt.expr);
    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef merge_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "switch_end");

    if (expr_type && expr_type->kind == KIND_PRIMITIVE &&
        expr_type->data.primitive == PRIM_STRING) {
      bool use_hash = node->switch_stmt.cases.len > 3;
      LLVMValueRef expr_hash = NULL;
      if (use_hash) {
        expr_hash = cg_string_hash(cg, expr_val);
      }

      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        LLVMBasicBlockRef case_bb =
            LLVMAppendBasicBlockInContext(cg->context, func, "switch_case");
        LLVMBasicBlockRef next_bb = (i + 1 < node->switch_stmt.cases.len)
                                        ? LLVMAppendBasicBlockInContext(
                                              cg->context, func, "switch_next")
                                        : merge_bb;

        LLVMValueRef cond = NULL;
        if (use_hash) {
          if (case_node->case_stmt.pattern &&
              case_node->case_stmt.pattern->tag == NODE_STRING) {
            StringView value = case_node->case_stmt.pattern->string.value;
            uint64_t hash = 1469598103934665603ULL;
            for (size_t k = 0; k < value.len; k++) {
              hash ^= (uint64_t)(unsigned char)value.data[k];
              hash *= 1099511628211ULL;
            }
            if (hash == 0)
              hash = 1;
            LLVMValueRef target_hash =
                LLVMConstInt(LLVMInt64TypeInContext(cg->context), hash, 0);
            LLVMValueRef hash_eq =
                LLVMBuildICmp(cg->builder, LLVMIntEQ, expr_hash, target_hash,
                              "switch_hash_eq");
            LLVMBasicBlockRef hash_ok_bb = LLVMAppendBasicBlockInContext(
                cg->context, func, "switch_hash_ok");
            LLVMBuildCondBr(cg->builder, hash_eq, hash_ok_bb, next_bb);

            LLVMPositionBuilderAtEnd(cg->builder, hash_ok_bb);
            LLVMValueRef lit_val =
                cg_expression(cg, case_node->case_stmt.pattern);
            LLVMValueRef eq = cg_string_equals(cg, expr_val, lit_val);
            LLVMBuildCondBr(cg->builder, eq, case_bb, next_bb);
          } else if (!case_node->case_stmt.pattern) {
            LLVMBuildBr(cg->builder, case_bb);
          } else {
            LLVMBuildBr(cg->builder, next_bb);
          }
        } else {
          if (case_node->case_stmt.pattern &&
              case_node->case_stmt.pattern->tag == NODE_STRING) {
            LLVMValueRef lit_val =
                cg_expression(cg, case_node->case_stmt.pattern);
            LLVMValueRef eq = cg_string_equals(cg, expr_val, lit_val);
            LLVMBuildCondBr(cg->builder, eq, case_bb, next_bb);
          } else if (!case_node->case_stmt.pattern) {
            LLVMBuildBr(cg->builder, case_bb);
          } else {
            LLVMBuildBr(cg->builder, next_bb);
          }
        }

        LLVMPositionBuilderAtEnd(cg->builder, case_bb);
        List_push(&cg->break_stack, merge_bb);
        if (case_node->case_stmt.body) {
          cg_push_scope(cg);
          cg_statement(cg, case_node->case_stmt.body);
          cg_pop_scope(cg);
        }
        List_pop(&cg->break_stack);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, merge_bb);
        }
        if (next_bb != merge_bb) {
          LLVMPositionBuilderAtEnd(cg->builder, next_bb);
        }
      }
    } else if (expr_type && expr_type->kind == KIND_UNION &&
               expr_type->is_tagged_union) {
      LLVMValueRef tag =
          LLVMBuildExtractValue(cg->builder, expr_val, 0, "switch_union_tag");

      size_t explicit_case_count = 0;
      bool has_default = false;
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (case_node && case_node->tag == NODE_CASE &&
            !case_node->case_stmt.pattern) {
          has_default = true;
        } else {
          explicit_case_count++;
        }
      }

      LLVMBasicBlockRef default_bb = merge_bb;
      if (has_default) {
        default_bb =
            LLVMAppendBasicBlockInContext(cg->context, func, "switch_default");
      }

      LLVMValueRef switch_inst = LLVMBuildSwitch(cg->builder, tag, default_bb,
                                                 (unsigned)explicit_case_count);

      LLVMBasicBlockRef *case_blocks =
          xmalloc(sizeof(LLVMBasicBlockRef) * explicit_case_count);
      size_t case_block_index = 0;
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (!case_node || case_node->tag != NODE_CASE)
          continue;

        if (!case_node->case_stmt.pattern)
          continue;

        int variant_index = -1;
        for (size_t j = 0; j < expr_type->members.len; j++) {
          Member *m = expr_type->members.items[j];
          if (type_equals(m->type, case_node->case_stmt.pattern_type)) {
            variant_index = (int)j;
            break;
          }
        }
        LLVMValueRef expected_tag =
            LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                         (unsigned)(variant_index >= 0 ? variant_index : 0), 0);
        case_blocks[case_block_index] =
            LLVMAppendBasicBlockInContext(cg->context, func, "switch_case");
        LLVMAddCase(switch_inst, expected_tag, case_blocks[case_block_index]);
        case_block_index++;
      }

      size_t case_index = 0;
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (!case_node || case_node->tag != NODE_CASE)
          continue;

        LLVMBasicBlockRef case_bb;
        if (!case_node->case_stmt.pattern) {
          case_bb = default_bb;
        } else {
          case_bb = case_blocks[case_index];
          case_index++;
        }

        LLVMPositionBuilderAtEnd(cg->builder, case_bb);
        List_push(&cg->break_stack, merge_bb);
        cg_push_scope(cg);
        if (case_node->case_stmt.pattern &&
            case_node->case_stmt.pattern->tag == NODE_VAR) {
          LLVMValueRef payload = cg_extract_tagged_union_payload(
              cg, expr_val, expr_type, case_node->case_stmt.pattern_type);
          LLVMValueRef local_ptr =
              cg_alloca_in_entry(cg, case_node->case_stmt.pattern_type,
                                 case_node->case_stmt.pattern->var.value);
          LLVMBuildStore(cg->builder, payload, local_ptr);
          CGSymbolTable_add(cg->current_scope,
                            case_node->case_stmt.pattern->var.value,
                            case_node->case_stmt.pattern_type, local_ptr);
        }
        if (case_node->case_stmt.body) {
          cg_statement(cg, case_node->case_stmt.body);
        }
        cg_pop_scope(cg);
        List_pop(&cg->break_stack);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, merge_bb);
        }
      }

      free(case_blocks);
      LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    } else {
      // Unsupported types should already be diagnosed by semantic analysis
    }

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    break;
  }
  case NODE_LOOP_STMT: {
    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "loop_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "loop_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "loop_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, cond_bb);

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    // In Tyl, "loop" is "loop { ... }" (infinite unless break)
    // We could just branch to body, but we'll keep the cond_bb for continue
    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->loop.expr); // The body is an expression/node

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, cond_bb);
    }

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    break;
  }
  case NODE_BREAK: {
    if (cg->break_stack.len == 0)
      panic("Break outside of loop");
    LLVMBasicBlockRef target =
        List_get(&cg->break_stack, cg->break_stack.len - 1);
    LLVMBuildBr(cg->builder, target);
    break;
  }
  case NODE_CONTINUE: {
    if (cg->continue_stack.len == 0)
      panic("Continue outside of loop");
    LLVMBasicBlockRef target =
        List_get(&cg->continue_stack, cg->continue_stack.len - 1);
    LLVMBuildBr(cg->builder, target);
    break;
  }
  case NODE_RETURN_STMT: {
    if (node->return_stmt.expr) {
      LLVMValueRef val;
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
    // Declarations and module imports do not emit code directly.
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
  case NODE_FOR_IN_STMT: {
    cg_push_scope(cg);
    Type *iter_type = node->for_in_stmt.iterable->resolved_type;
    Type *elem_type = node->for_in_stmt.var->resolved_type;
    LLVMValueRef loop_var =
        cg_alloca_in_entry(cg, elem_type, node->for_in_stmt.var->var.value);
    CGSymbolTable_add(cg->current_scope, node->for_in_stmt.var->var.value,
                      elem_type, loop_var);

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_in_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_in_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_in_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, cond_bb);

    LLVMValueRef iterable_val = cg_expression(cg, node->for_in_stmt.iterable);
    LLVMValueRef index_ptr =
        cg_alloca_in_entry(cg, type_get_primitive(cg->type_ctx, PRIM_I64),
                           sv_from_cstr("for_in_index"));
    LLVMBuildStore(cg->builder,
                   LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0),
                   index_ptr);

    LLVMValueRef data_ptr = NULL;
    LLVMTypeRef string_ptr_ty = NULL;
    LLVMValueRef len_val = NULL;
    LLVMValueRef string_ptr_var = NULL;
    if (iter_type->kind == KIND_PRIMITIVE &&
        iter_type->data.primitive == PRIM_STRING) {
      string_ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
      string_ptr_var = LLVMBuildAlloca(cg->builder, string_ptr_ty, "str_ptr");
      LLVMBuildStore(cg->builder, iterable_val, string_ptr_var);
      data_ptr = string_ptr_var;
      len_val = NULL;
    } else if (iter_type->kind == KIND_STRUCT &&
               iter_type->data.instance.generic_args.len > 0 &&
               sv_eq(iter_type->data.instance.from_template->name,
                     sv_from_cstr("Array"))) {
      LLVMTypeRef array_ty = cg_get_llvm_type(cg, iter_type);
      LLVMValueRef array_alloca =
          cg_alloca_in_entry(cg, iter_type, sv_from_cstr("for_in_array"));
      LLVMBuildStore(cg->builder, iterable_val, array_alloca);
      LLVMValueRef len_ptr = LLVMBuildStructGEP2(cg->builder, array_ty,
                                                 array_alloca, 1, "len_ptr");
      len_val = LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                               len_ptr, "len_val");
      LLVMValueRef data_ptr_ptr = LLVMBuildStructGEP2(
          cg->builder, array_ty, array_alloca, 0, "data_ptr_ptr");
      data_ptr = LLVMBuildLoad2(
          cg->builder,
          cg_get_llvm_type(
              cg, type_get_member(iter_type, sv_from_cstr("data"))->type),
          data_ptr_ptr, "data_ptr");
    }

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    LLVMValueRef cond = NULL;
    if (len_val) {
      LLVMValueRef idx = LLVMBuildLoad2(
          cg->builder, LLVMInt64TypeInContext(cg->context), index_ptr, "idx");
      cond =
          LLVMBuildICmp(cg->builder, LLVMIntULT, idx, len_val, "for_in_cond");
    } else {
      LLVMValueRef current_ptr =
          LLVMBuildLoad2(cg->builder, string_ptr_ty, data_ptr, "str_ptr_load");
      LLVMValueRef char_val = LLVMBuildLoad2(
          cg->builder, LLVMInt8TypeInContext(cg->context), current_ptr, "ch");
      cond = LLVMBuildICmp(
          cg->builder, LLVMIntNE, char_val,
          LLVMConstInt(LLVMInt8TypeInContext(cg->context), 0, 0), "str_cond");
    }
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    if (len_val) {
      LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, elem_type);
      LLVMValueRef idx = LLVMBuildLoad2(
          cg->builder, LLVMInt64TypeInContext(cg->context), index_ptr, "idx");
      LLVMValueRef element_ptr =
          LLVMBuildGEP2(cg->builder, llvm_elem_ty, data_ptr,
                        (LLVMValueRef[]){idx}, 1, "elem_ptr");
      LLVMValueRef elem =
          LLVMBuildLoad2(cg->builder, llvm_elem_ty, element_ptr, "elem");
      LLVMBuildStore(cg->builder, elem, loop_var);
    } else {
      LLVMValueRef current_ptr =
          LLVMBuildLoad2(cg->builder, string_ptr_ty, data_ptr, "str_ptr_load");
      LLVMValueRef ch =
          LLVMBuildLoad2(cg->builder, LLVMInt8TypeInContext(cg->context),
                         current_ptr, "ch_val");
      LLVMBuildStore(cg->builder, ch, loop_var);
    }

    cg_statement(cg, node->for_in_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      if (len_val) {
        LLVMValueRef idx = LLVMBuildLoad2(
            cg->builder, LLVMInt64TypeInContext(cg->context), index_ptr, "idx");
        LLVMValueRef next_idx = LLVMBuildAdd(
            cg->builder, idx,
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0),
            "next_idx");
        LLVMBuildStore(cg->builder, next_idx, index_ptr);
      } else {
        LLVMValueRef current_ptr = LLVMBuildLoad2(cg->builder, string_ptr_ty,
                                                  data_ptr, "str_ptr_load");
        LLVMValueRef next_ptr = LLVMBuildGEP2(
            cg->builder, LLVMInt8TypeInContext(cg->context), current_ptr,
            (LLVMValueRef[]){
                LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0)},
            1, "next_ptr");
        LLVMBuildStore(cg->builder, next_ptr, string_ptr_var);
      }
      LLVMBuildBr(cg->builder, cond_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);
    cg_pop_scope(cg);
    break;
  }
  case NODE_WHILE_STMT: {
    cg_push_scope(cg);

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "while_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "while_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "while_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, cond_bb);

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    LLVMValueRef cond = cg_expression(cg, node->while_stmt.condition);
    cond = cg_cast_value(cg, cond, node->while_stmt.condition->resolved_type,
                         LLVMInt1TypeInContext(cg->context));
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->while_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, cond_bb);
    }

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg_pop_scope(cg);
    break;
  }
  case NODE_FOR_STMT: {
    cg_push_scope(cg);

    if (node->for_stmt.init) {
      cg_statement(cg, node->for_stmt.init);
    }

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_body");
    LLVMBasicBlockRef inc_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_inc");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, inc_bb);

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    if (node->for_stmt.condition) {
      LLVMValueRef cond_val = cg_expression(cg, node->for_stmt.condition);
      cond_val =
          cg_cast_value(cg, cond_val, node->for_stmt.condition->resolved_type,
                        LLVMInt1TypeInContext(cg->context));
      LLVMBuildCondBr(cg->builder, cond_val, body_bb, end_bb);
    } else {
      LLVMBuildBr(cg->builder, body_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->for_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, inc_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    if (node->for_stmt.increment) {
      cg_expression(cg, node->for_stmt.increment);
    }
    LLVMBuildBr(cg->builder, cond_bb);

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    cg_pop_scope(cg);
    break;
  }
  default:
    fprintf(stderr, "Codegen: Unhandled statement tag %d\n", node->tag);
    break;
  }
}
