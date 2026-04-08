#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"

void cg_var_decl(Codegen *cg, AstNode *node) {
  LLVMValueRef alloca = cg_alloca_in_entry(cg, node->var_decl.declared_type,
                                           node->var_decl.name->var.value);

  CGSymbolTable_add(cg->current_scope, node->var_decl.name->var.value,
                    node->var_decl.declared_type, alloca);

  if (node->var_decl.value) {
    LLVMValueRef init_val = cg_expression(cg, node->var_decl.value);

    LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->var_decl.declared_type);
    init_val = cg_cast_value(cg, init_val, node->var_decl.value->resolved_type,
                             target_ty);

    LLVMBuildStore(cg->builder, init_val, alloca);
  }
}

typedef struct CGFormatString {
  const char *fmt;
  LLVMValueRef value;
} CGFormatString;

static LLVMValueRef cg_get_format_string(Codegen *cg, const char *fmt_str) {
  // Deduplicate by string content
  for (size_t i = 0; i < cg->format_strings.len; i++) {
    CGFormatString *fs = cg->format_strings.items[i];
    if (strcmp(fs->fmt, fmt_str) == 0) {
      return fs->value;
    }
  }

  LLVMValueRef val =
      LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "print_fmt");
  CGFormatString *fs = xmalloc(sizeof(CGFormatString));
  fs->fmt = xstrdup(fmt_str);
  fs->value = val;
  List_push(&cg->format_strings, fs);
  return val;
}

static void cg_print_stmt(Codegen *cg, AstNode *node) {
  CGFunction *print_fn = cg_find_function(cg, sv_from_parts("print", 5));
  if (!print_fn)
    return;

  for (size_t i = 0; i < node->print_stmt.values.len; i++) {
    AstNode *val_node = node->print_stmt.values.items[i];
    LLVMValueRef val = cg_expression(cg, val_node);
    LLVMTypeRef val_ty = LLVMTypeOf(val);
    LLVMTypeKind kind = LLVMGetTypeKind(val_ty);

    const char *fmt_str = "%p";
    Type *resolved_type = val_node->resolved_type;

    if (kind == LLVMStructTypeKind) {
      if (resolved_type &&
          ((resolved_type->kind == KIND_ARRAY &&
            resolved_type->data.array.element->kind == KIND_PRIMITIVE &&
            resolved_type->data.array.element->data.primitive == PRIM_CHAR) ||
           (resolved_type->kind == KIND_PRIMITIVE &&
            resolved_type->data.primitive == PRIM_STRING))) {
        // String/char array: extract the data pointer (index 1) which is i8*
        fmt_str = "%s";
        val = LLVMBuildExtractValue(cg->builder, val, 1, "str_ptr_extract");

        // C variadic promotion: ensure it's i8* (bitcast from generic if
        // needed)
        LLVMTypeRef i8_ptr =
            LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
        val = LLVMBuildBitCast(cg->builder, val, i8_ptr, "str_ptr_cast");
      } else {
        fmt_str = "%p";
      }
    } else if (kind == LLVMIntegerTypeKind) {
      unsigned width = LLVMGetIntTypeWidth(val_ty);
      if (width == 1) {
        fmt_str = "%s";
        LLVMValueRef true_str =
            LLVMBuildGlobalStringPtr(cg->builder, "true", "t_str");
        LLVMValueRef false_str =
            LLVMBuildGlobalStringPtr(cg->builder, "false", "f_str");
        LLVMValueRef is_true = LLVMBuildICmp(
            cg->builder, LLVMIntEQ, val, LLVMConstInt(val_ty, 1, 0), "is_true");
        val = LLVMBuildSelect(cg->builder, is_true, true_str, false_str,
                              "bool_ptr");
      } else if (width <= 8) {
        fmt_str = "%c";
        val = LLVMBuildZExt(cg->builder, val,
                            LLVMInt32TypeInContext(cg->context), "zext_char");
      } else if (width <= 32) {
        fmt_str = "%d";
      } else {
        fmt_str = "%lld";
      }
    } else if (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind) {
      fmt_str = "%f";
      // C variadic promotion: float -> double
      if (kind == LLVMFloatTypeKind) {
        val =
            LLVMBuildFPExt(cg->builder, val,
                           LLVMDoubleTypeInContext(cg->context), "fpext_varg");
      }
    } else if (kind == LLVMPointerTypeKind) {
      fmt_str = "%s";
    }

    if (i > 0) {
      LLVMValueRef space_fmt = cg_get_format_string(cg, " ");
      LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, &space_fmt,
                     1, "");
    }

    LLVMValueRef fmt_val = cg_get_format_string(cg, fmt_str);
    LLVMValueRef args[] = {fmt_val, val};
    LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, args, 2, "");
  }

  LLVMValueRef nl_fmt = cg_get_format_string(cg, "\n");
  LLVMValueRef nl_args[] = {nl_fmt};
  LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, nl_args, 1, "");
}

static void cg_if_stmt(Codegen *cg, AstNode *node) {
  LLVMValueRef condition = cg_expression(cg, node->if_stmt.condition);
  condition =
      cg_cast_value(cg, condition, node->if_stmt.condition->resolved_type,
                    LLVMInt1TypeInContext(cg->context));

  LLVMValueRef current_fn = cg->current_function;

  LLVMBasicBlockRef then_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_fn, "then");
  LLVMBasicBlockRef else_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_fn, "else");
  LLVMBasicBlockRef merge_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_fn, "ifcont");

  LLVMBuildCondBr(cg->builder, condition, then_bb, else_bb);

  LLVMPositionBuilderAtEnd(cg->builder, then_bb);
  cg_statement(cg, node->if_stmt.then_branch);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    LLVMBuildBr(cg->builder, merge_bb);
  }

  LLVMPositionBuilderAtEnd(cg->builder, else_bb);
  if (node->if_stmt.else_branch) {
    cg_statement(cg, node->if_stmt.else_branch);
  }
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    LLVMBuildBr(cg->builder, merge_bb);
  }

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}

void cg_statement(Codegen *cg, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_VAR_DECL:
    cg_var_decl(cg, node);
    break;

  case NODE_BLOCK: {
    cg_push_scope(cg);
    for (size_t i = 0; i < node->block.statements.len; i++) {
      cg_statement(cg, node->block.statements.items[i]);
    }
    cg_pop_scope(cg);
    break;
  }

  case NODE_EXPR_STMT:
    cg_expression(cg, node->expr_stmt.expr);
    break;

  case NODE_PRINT_STMT:
    cg_print_stmt(cg, node);
    break;

  case NODE_IF_STMT:
    cg_if_stmt(cg, node);
    break;

  case NODE_DEFER: {
    List *current_defers = cg->defers.items[cg->defers.len - 1];
    List_push(current_defers, node->defer.expr);
    break;
  }

  case NODE_RETURN_STMT: {
    if (node->return_stmt.expr) {
      LLVMValueRef ret_val = cg_expression(cg, node->return_stmt.expr);

      if (cg->current_function_ref) {
        LLVMTypeRef expected_ret_ty =
            LLVMGetReturnType(cg->current_function_ref->type);

        ret_val =
            cg_cast_value(cg, ret_val, node->return_stmt.expr->resolved_type,
                          expected_ret_ty);
      }

      // Emit all defers in reverse order
      for (int i = (int)cg->defers.len - 1; i >= 0; i--) {
        List *defer_list = cg->defers.items[i];
        for (int j = (int)defer_list->len - 1; j >= 0; j--) {
          AstNode *defer_node = defer_list->items[j];
          cg_statement(cg, defer_node);
        }
      }

      LLVMBuildRet(cg->builder, ret_val);
    } else {
      for (int i = (int)cg->defers.len - 1; i >= 0; i--) {
        List *defer_list = cg->defers.items[i];
        for (int j = (int)defer_list->len - 1; j >= 0; j--) {
          AstNode *defer_node = defer_list->items[j];
          cg_statement(cg, defer_node);
        }
      }
      LLVMBuildRetVoid(cg->builder);
    }
    break;
  }

  case NODE_LOOP_STMT: {
    LLVMBasicBlockRef loop_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "loop");
    LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "after_loop");

    List_push(&cg->continue_stack, loop_bb);
    List_push(&cg->break_stack, after_bb);

    LLVMBuildBr(cg->builder, loop_bb);
    LLVMPositionBuilderAtEnd(cg->builder, loop_bb);

    cg_statement(cg, node->loop.expr);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, loop_bb);
    }

    List_pop(&cg->continue_stack);
    List_pop(&cg->break_stack);

    LLVMPositionBuilderAtEnd(cg->builder, after_bb);
    break;
  }

  case NODE_WHILE_STMT: {
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "while_cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "while_body");
    LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "while_after");

    List_push(&cg->continue_stack, cond_bb);
    List_push(&cg->break_stack, after_bb);

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    LLVMValueRef cond = cg_expression(cg, node->while_stmt.condition);
    cond = cg_cast_value(cg, cond, node->while_stmt.condition->resolved_type,
                         LLVMInt1TypeInContext(cg->context));
    LLVMBuildCondBr(cg->builder, cond, body_bb, after_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->while_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, cond_bb);
    }

    List_pop(&cg->continue_stack);
    List_pop(&cg->break_stack);

    LLVMPositionBuilderAtEnd(cg->builder, after_bb);
    break;
  }

  case NODE_FOR_STMT: {
    LLVMBasicBlockRef init_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_init");
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_body");
    LLVMBasicBlockRef inc_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_inc");
    LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_after");

    LLVMBuildBr(cg->builder, init_bb);
    LLVMPositionBuilderAtEnd(cg->builder, init_bb);

    if (node->for_stmt.init) {
      cg_statement(cg, node->for_stmt.init);
    }
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    if (node->for_stmt.condition) {
      LLVMValueRef cond = cg_expression(cg, node->for_stmt.condition);
      cond = cg_cast_value(cg, cond, node->for_stmt.condition->resolved_type,
                           LLVMInt1TypeInContext(cg->context));
      LLVMBuildCondBr(cg->builder, cond, body_bb, after_bb);
    } else {
      LLVMBuildBr(cg->builder, body_bb);
    }

    List_push(&cg->continue_stack, inc_bb);
    List_push(&cg->break_stack, after_bb);

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

    List_pop(&cg->continue_stack);
    List_pop(&cg->break_stack);

    LLVMPositionBuilderAtEnd(cg->builder, after_bb);
    break;
  }

  case NODE_FOR_IN_STMT: {
    LLVMValueRef iterable = cg_expression(cg, node->for_in_stmt.iterable);
    Type *iter_type = node->for_in_stmt.iterable->resolved_type;
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);

    LLVMValueRef dims_ptr =
        LLVMBuildExtractValue(cg->builder, iterable, 2, "iter_dims");
    LLVMValueRef len =
        LLVMBuildLoad2(cg->builder, i64_ty, dims_ptr, "iter_len");

    LLVMValueRef index_alloca =
        cg_alloca_in_entry(cg, type_get_primitive(cg->type_ctx, PRIM_I64),
                           sv_from_cstr("for_in_idx"));
    LLVMBuildStore(cg->builder, LLVMConstInt(i64_ty, 0, false), index_alloca);

    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_in_cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_in_body");
    LLVMBasicBlockRef inc_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_in_inc");
    LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "for_in_after");

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    LLVMValueRef curr_idx =
        LLVMBuildLoad2(cg->builder, i64_ty, index_alloca, "curr_idx");
    LLVMValueRef cond =
        LLVMBuildICmp(cg->builder, LLVMIntULT, curr_idx, len, "loop_cond");
    LLVMBuildCondBr(cg->builder, cond, body_bb, after_bb);

    List_push(&cg->continue_stack, inc_bb);
    List_push(&cg->break_stack, after_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_push_scope(cg);

    // let var = iterable[curr_idx]
    Type *elem_type = (iter_type->kind == KIND_ARRAY)
                          ? iter_type->data.array.element
                          : type_get_primitive(cg->type_ctx, PRIM_I8);
    LLVMValueRef data_ptr =
        LLVMBuildExtractValue(cg->builder, iterable, 1, "iter_data");
    LLVMValueRef elem_ptr =
        LLVMBuildGEP2(cg->builder, cg_get_llvm_type(cg, elem_type), data_ptr,
                      &curr_idx, 1, "elem_ptr");
    LLVMValueRef elem_val = LLVMBuildLoad2(
        cg->builder, cg_get_llvm_type(cg, elem_type), elem_ptr, "elem_val");

    LLVMValueRef var_alloca =
        cg_alloca_in_entry(cg, elem_type, node->for_in_stmt.var->var.value);
    CGSymbolTable_add(cg->current_scope, node->for_in_stmt.var->var.value,
                      elem_type, var_alloca);
    LLVMBuildStore(cg->builder, elem_val, var_alloca);

    cg_statement(cg, node->for_in_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, inc_bb);
    }
    cg_pop_scope(cg);

    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    LLVMValueRef next_idx = LLVMBuildAdd(
        cg->builder, curr_idx, LLVMConstInt(i64_ty, 1, false), "next_idx");
    LLVMBuildStore(cg->builder, next_idx, index_alloca);
    LLVMBuildBr(cg->builder, cond_bb);

    List_pop(&cg->continue_stack);
    List_pop(&cg->break_stack);

    LLVMPositionBuilderAtEnd(cg->builder, after_bb);
    break;
  }

  case NODE_BREAK: {
    if (cg->break_stack.len == 0) {
      panic("Break statement outside of loop");
    }
    LLVMBasicBlockRef break_target =
        cg->break_stack.items[cg->break_stack.len - 1];
    LLVMBuildBr(cg->builder, break_target);

    // IMPORTANT: After a break, we are in a "dead" block.
    // We create a dummy block to hold any unreachable instructions
    // until the next legitimate block starts.
    LLVMBasicBlockRef dead_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "dead_code");
    LLVMPositionBuilderAtEnd(cg->builder, dead_bb);
    break;
  }

  case NODE_CONTINUE: {
    if (cg->continue_stack.len == 0) {
      panic("Continue statement outside of loop");
    }
    LLVMBasicBlockRef cont_target =
        cg->continue_stack.items[cg->continue_stack.len - 1];
    LLVMBuildBr(cg->builder, cont_target);

    LLVMBasicBlockRef dead_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "dead_code");
    LLVMPositionBuilderAtEnd(cg->builder, dead_bb);
    break;
  }

  default:
    break;
  }
}
