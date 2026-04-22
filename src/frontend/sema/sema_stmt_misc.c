#include "sema_internal.h"

void sema_check_import(Sema *s, AstNode *node) {
  Module *module = module_resolve_or_load(
      s, node->import.path,
      s->current_module ? s->current_module->abs_path : ".");
  if (!module)
    return;

  StringView name = node->import.alias;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate import alias '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Symbol *sym = sema_define(s, name, type_get_primitive(s->types, PRIM_UNKNOWN),
                            false, node->loc);
  if (sym) {
    sym->kind = SYM_MODULE;
    sym->module = module;
    sym->is_export = false;
    sym->is_external = false;
    sym->value = NULL;
  }

  for (size_t i = 0; i < module->exports.len; i++) {
    Symbol *export = module->exports.items[i];
    StringView export_name =
        export->original_name.len ? export->original_name : export->name;
    if (sema_resolve_local(s, export_name)) {
      sema_error(s, node, "Duplicate imported symbol '" SV_FMT "'",
                 SV_ARG(export_name));
      continue;
    }

    Symbol *export_sym =
        sema_define(s, export_name, export->type, export->is_const, node->loc);
    if (!export_sym)
      continue;

    export_sym->original_name = export->original_name;
    export_sym->value = export->value;
    export_sym->kind = export->kind;
    export_sym->func_status = export->func_status;
    export_sym->is_external = export->is_external;
    export_sym->is_export = false;
    export_sym->module = module;
  }
}

typedef struct {
  size_t index;
  AstNode *return_expr;
} ReturnDropInfo;

static size_t sema_insert_drop_calls_before_return(Sema *s, AstNode *block,
                                                   List *symbols, size_t count,
                                                   size_t index,
                                                   AstNode *return_expr) {
  if (!block || block->tag != NODE_BLOCK)
    return 0;

  AstNode *return_var = sema_find_underlying_var(return_expr);
  StringView return_name = {NULL, 0};
  if (return_var && return_var->tag == NODE_VAR)
    return_name = return_var->var.value;

  size_t inserted = 0;
  for (size_t i = 0; i < count; i++) {
    Symbol *sym = symbols->items[i];
    if (!sym || sym->kind != SYM_VAR || sym->is_moved || !sym->type ||
        sym->type->kind != KIND_STRING_BUFFER)
      continue;
    if (return_name.len && sv_eq(sym->name, return_name))
      continue;

    List args;
    List_init(&args);
    AstNode *arg = AstNode_new_unary(
        OP_ADDR_OF, AstNode_new_var(sym->name, block->loc), block->loc);
    List_push(&args, arg);
    AstNode *call = AstNode_new_call(
        AstNode_new_var(sv_from_cstr("__tyna_string_free"), block->loc), args,
        block->loc);
    AstNode *expr_stmt = AstNode_new_expr_stmt(call, block->loc);
    List_insert(&block->block.statements, index + inserted, expr_stmt);
    sema_check_stmt(s, expr_stmt);
    inserted++;
  }
  return inserted;
}

void sema_check_block(Sema *s, AstNode *node) {
  sema_scope_push(s);

  List return_points;
  List_init(&return_points);

  for (size_t i = 0; i < node->block.statements.len; i++) {
    AstNode *stmt = node->block.statements.items[i];
    if (stmt && stmt->tag == NODE_RETURN_STMT) {
      ReturnDropInfo *info = xmalloc(sizeof(ReturnDropInfo));
      info->index = i;
      info->return_expr = stmt->return_stmt.expr;
      List_push(&return_points, info);
    }
    sema_check_stmt(s, node->block.statements.items[i]);
  }

  size_t symbols_to_drop = s->scope->symbols.len;
  for (int j = (int)return_points.len - 1; j >= 0; j--) {
    ReturnDropInfo *info = return_points.items[j];
    sema_insert_drop_calls_before_return(s, node, &s->scope->symbols,
                                         symbols_to_drop, info->index,
                                         info->return_expr);
    free(info);
  }
  List_free(&return_points, 0);

  size_t injected_start = node->block.statements.len;
  sema_inject_drop_calls(s, node, &s->scope->symbols);
  for (size_t j = injected_start; j < node->block.statements.len; j++) {
    sema_check_stmt(s, node->block.statements.items[j]);
  }

  sema_scope_pop(s);
}

void sema_check_return_stmt(Sema *s, AstNode *node) {
  if (!s->fn_node) {
    sema_error(s, node, "Return statement outside of function");
    return;
  }

  Type *expr_type = node->return_stmt.expr
                        ? sema_check_expr(s, node->return_stmt.expr).type
                        : type_get_primitive(s->types, PRIM_VOID);

  if (type_is_unknown(s->ret_type)) {
    s->ret_type = expr_type;
    s->fn_node->func_decl.return_type = expr_type;
  } else if (s->ret_type->kind == KIND_RESULT &&
             s->ret_type->data.result.error_set &&
             s->ret_type->data.result.error_set->kind == KIND_ERROR_SET &&
             s->ret_type->data.result.error_set->name.len == 0) {
    if (expr_type->kind == KIND_ERROR) {
      Type *error_set = s->ret_type->data.result.error_set;
      bool found = false;
      for (size_t i = 0; i < error_set->members.len; i++) {
        Member *m = error_set->members.items[i];
        if (type_equals(m->type, expr_type)) {
          found = true;
          break;
        }
      }
      if (!found) {
        type_add_member(error_set, NULL, expr_type, 0);
      }
    } else if (expr_type->kind == KIND_RESULT &&
               expr_type->data.result.error_set &&
               expr_type->data.result.error_set->kind == KIND_ERROR_SET) {
      Type *error_set = s->ret_type->data.result.error_set;
      for (size_t i = 0; i < expr_type->data.result.error_set->members.len;
           i++) {
        Member *member = expr_type->data.result.error_set->members.items[i];
        Type *error_ty = member->type;
        bool found = false;
        for (size_t j = 0; j < error_set->members.len; j++) {
          Member *m = error_set->members.items[j];
          if (type_equals(m->type, error_ty)) {
            found = true;
            break;
          }
        }
        if (!found) {
          type_add_member(error_set, NULL, error_ty, 0);
        }
      }
    }

    if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
      sema_error(s, node, "Type mismatch: function returns '%s' but got '%s'",
                 type_to_name(s->ret_type), type_to_name(expr_type));
    }
  } else if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
    sema_error(s, node, "Type mismatch: function returns '%s' but got '%s'",
               type_to_name(s->ret_type), type_to_name(expr_type));
  }
}