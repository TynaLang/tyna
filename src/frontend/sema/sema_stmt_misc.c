#include "sema_internal.h"

void sema_check_import(Sema *s, AstNode *node) {
  Module *module = module_resolve_or_load(
      s, node->import.path,
      s->current_module ? s->current_module->abs_path : ".");
  if (!module)
    return;

  if (node->import.mode == IMPORT_NAMESPACE) {
    StringView name = node->import.alias;
    if (sema_resolve_local(s, name)) {
      sema_error(s, node, "Duplicate import alias '" SV_FMT "'", SV_ARG(name));
      return;
    }

    Symbol *sym = sema_define(
        s, name, type_get_primitive(s->types, PRIM_UNKNOWN), false, node->loc);
    if (sym) {
      sym->kind = SYM_MODULE;
      sym->module = module;
      sym->is_export = false;
      sym->is_external = false;
      sym->value = NULL;
    }
    return;
  }

  if (node->import.mode == IMPORT_WILDCARD) {
    for (size_t i = 0; i < module->exports.len; i++) {
      Symbol *export = module->exports.items[i];
      StringView export_name =
          export->original_name.len ? export->original_name : export->name;
      if (sema_resolve_local(s, export_name)) {
        sema_error(s, node,
                   "Wildcard import introduces colliding name '" SV_FMT "'",
                   SV_ARG(export_name));
        continue;
      }

      Symbol *import_sym = sema_define(s, export_name, export->type,
                                       export->is_const, node->loc);
      if (!import_sym)
        continue;

      import_sym->original_name = export->original_name;
      import_sym->value = export->value;
      import_sym->kind = export->kind;
      import_sym->func_status = export->func_status;
      import_sym->is_external = export->is_external;
      import_sym->is_export = false;
      import_sym->module = module;
    }
    return;
  }

  if (node->import.mode == IMPORT_SELECTIVE) {
    for (size_t i = 0; i < node->import.symbols.len; i++) {
      ImportSymbol *symbol = node->import.symbols.items[i];
      StringView requested_name = symbol->name;
      StringView local_name = symbol->alias.len ? symbol->alias : symbol->name;
      Symbol *found = NULL;
      for (size_t j = 0; j < module->exports.len; j++) {
        Symbol *export = module->exports.items[j];
        StringView export_name =
            export->original_name.len ? export->original_name : export->name;
        if (sv_eq(export_name, requested_name)) {
          found = export;
          break;
        }
      }

      if (!found) {
        sema_error(s, node, "Cannot find public symbol '" SV_FMT "' in module",
                   SV_ARG(requested_name));
        continue;
      }

      if (sema_resolve_local(s, local_name)) {
        sema_error(s, node,
                   "Selective import introduces colliding name '" SV_FMT "'",
                   SV_ARG(local_name));
        continue;
      }

      Symbol *import_sym =
          sema_define(s, local_name, found->type, found->is_const, node->loc);
      if (!import_sym)
        continue;

      import_sym->original_name = found->original_name;
      import_sym->value = found->value;
      import_sym->kind = found->kind;
      import_sym->func_status = found->func_status;
      import_sym->is_external = found->is_external;
      import_sym->is_export = false;
      import_sym->module = module;
    }
    return;
  }
}

void sema_check_block(Sema *s, AstNode *node) {
  sema_scope_push(s);

  Type *last_type = type_get_primitive(s->types, PRIM_VOID);

  for (size_t i = 0; i < node->block.statements.len; i++) {
    AstNode *stmt = node->block.statements.items[i];
    sema_check_stmt(s, node->block.statements.items[i]);
    if (stmt && stmt->tag == NODE_EXPR_STMT && stmt->expr_stmt.expr) {
      last_type = sema_check_expr(s, stmt->expr_stmt.expr).type;
    } else if (stmt && stmt->tag == NODE_RETURN_STMT) {
      last_type = s->ret_type ? s->ret_type : last_type;
    }
  }

  List_free(&node->block.symbols_to_drop, 0);
  List_init(&node->block.symbols_to_drop);
  sema_get_drops_for_scope(s, s->scope, &node->block.symbols_to_drop);

  node->resolved_type = last_type;
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

  List_free(&node->return_stmt.symbols_to_drop, 0);
  List_init(&node->return_stmt.symbols_to_drop);
  for (SemaScope *scope = s->scope; scope != NULL && scope->parent != NULL;
       scope = scope->parent) {
    sema_get_drops_for_scope(s, scope, &node->return_stmt.symbols_to_drop);
  }
}