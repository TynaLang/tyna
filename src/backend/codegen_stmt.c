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
  condition = cg_cast_value(cg, condition, LLVMInt1TypeInContext(cg->context));

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

        ret_val = cg_cast_value(cg, ret_val, expected_ret_ty);
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

  default:
    break;
  }
}
