#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"
#include <stdio.h>
#include <string.h>

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

static void cg_print_stmt(Codegen *cg, AstNode *node) {
  CGFunction *print_fn = cg_find_function(cg, sv_from_parts("print", 5));
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

    if (kind == LLVMIntegerTypeKind) {
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
        val = LLVMBuildZExt(cg->builder, val, i32_ty, "z_char");
      } else if (width <= 32) {
        fmt_str = "%d";
      } else {
        fmt_str = "%ld";
      }
    } else if (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind) {
      fmt_str = "%f";
      if (kind == LLVMFloatTypeKind) {
        val = LLVMBuildFPExt(cg->builder, val,
                             LLVMDoubleTypeInContext(cg->context), "f_prom");
      }
    } else if (kind == LLVMPointerTypeKind) {
      if (resolved_type && resolved_type->kind == KIND_PRIMITIVE &&
          resolved_type->data.primitive == PRIM_STRING) {
        fmt_str = "%s";
      } else {
        fmt_str = "%p";
      }
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
      LLVMValueRef val = cg_expression(cg, node->return_stmt.expr);
      LLVMBuildRet(cg->builder, val);
    } else {
      LLVMBuildRetVoid(cg->builder);
    }
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
