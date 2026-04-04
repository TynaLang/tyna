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
    init_val = cg_cast_value(cg, init_val, target_ty);

    LLVMBuildStore(cg->builder, init_val, alloca);
  }
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

    const char *fmt_str = "%p ";

    if (kind == LLVMIntegerTypeKind) {
      unsigned width = LLVMGetIntTypeWidth(val_ty);
      if (width == 1) {
        fmt_str = "%s ";
        LLVMValueRef true_str =
            LLVMBuildGlobalStringPtr(cg->builder, "true", "t_str");
        LLVMValueRef false_str =
            LLVMBuildGlobalStringPtr(cg->builder, "false", "f_str");
        LLVMValueRef is_true = LLVMBuildICmp(
            cg->builder, LLVMIntEQ, val, LLVMConstInt(val_ty, 1, 0), "is_true");
        val = LLVMBuildSelect(cg->builder, is_true, true_str, false_str,
                              "bool_ptr");
      } else if (width <= 32) {
        fmt_str = "%d ";
      } else {
        fmt_str = "%lld ";
      }
    } else if (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind) {
      fmt_str = "%f ";
    } else if (kind == LLVMPointerTypeKind) {
      fmt_str = "%s ";
    }

    LLVMValueRef fmt_val =
        LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "print_fmt");
    LLVMValueRef args[] = {fmt_val, val};
    LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, args, 2, "");
  }

  LLVMValueRef nl_fmt = LLVMBuildGlobalStringPtr(cg->builder, "\n", "nl");
  LLVMValueRef nl_args[] = {nl_fmt};
  LLVMBuildCall2(cg->builder, print_fn->type, print_fn->value, nl_args, 1, "");
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

  case NODE_PRINT_STMT:
    cg_print_stmt(cg, node);
    break;

  case NODE_RETURN_STMT: {
    if (node->return_stmt.expr) {
      LLVMValueRef ret_val = cg_expression(cg, node->return_stmt.expr);

      if (cg->current_function_ref) {
        LLVMTypeRef expected_ret_ty =
            LLVMGetReturnType(cg->current_function_ref->type);

        ret_val = cg_cast_value(cg, ret_val, expected_ret_ty);
      }
      LLVMBuildRet(cg->builder, ret_val);
    } else {
      LLVMBuildRetVoid(cg->builder);
    }
    break;
  }

  case NODE_EXPR_STMT:
    cg_expression(cg, node->expr_stmt.expr);
    break;

  default:
    break;
  }
}
