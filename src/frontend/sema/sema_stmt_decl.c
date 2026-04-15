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

void sema_check_var_decl(Sema *s, AstNode *node) {
  StringView name = node->var_decl.name->var.value;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate variable '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Type *decl_type = node->var_decl.declared_type;

  if (node->var_decl.value) {
    Type *actual_type = sema_coerce(s, node->var_decl.value, decl_type);

    if (type_needs_inference(decl_type)) {
      decl_type = actual_type;
    } else if (!type_is_concrete(decl_type) ||
               (decl_type->kind == KIND_STRUCT &&
                decl_type->data.instance.from_template == NULL)) {
      decl_type = actual_type;
    }
  } else {
    if (!type_is_concrete(decl_type) && !(sema_allows_polymorphic_types(s) &&
                                          type_can_be_polymorphic(decl_type))) {
      sema_error(
          s, node,
          "Type annotation required for uninitialized variable (got '%s')",
          type_to_name(decl_type));
      return;
    }
  }

  node->var_decl.declared_type = decl_type;
  node->resolved_type = decl_type;

  Symbol *sym =
      sema_define(s, name, decl_type, node->var_decl.is_const, node->loc);
  if (sym) {
    sym->value = node->var_decl.value;
    sym->is_export = node->var_decl.is_export;
  }
}

void sema_check_func_decl(Sema *s, AstNode *node) {
  if (!node->func_decl.body) {
    return;
  }

  Type *old_ret = s->ret_type;
  AstNode *old_fn = s->fn_node;

  s->ret_type = node->func_decl.return_type;
  s->fn_node = node;

  sema_scope_push(s);

  for (size_t i = 0; i < node->func_decl.params.len; i++) {
    AstNode *param = node->func_decl.params.items[i];
    bool concrete_ok = type_is_concrete(param->param.type);
    bool polymorphic_ok = sema_allows_polymorphic_types(s) &&
                          type_can_be_polymorphic(param->param.type);

    if (!concrete_ok && !polymorphic_ok) {
      sema_error(s, param,
                 "Function parameter type must be concrete (got '%s')",
                 type_to_name(param->param.type));
    }

    sema_define(s, param->param.name->var.value, param->param.type, false,
                param->loc);
  }

  sema_check_stmt(s, node->func_decl.body);

  if (type_is_unknown(s->ret_type)) {
    s->ret_type = type_get_primitive(s->types, PRIM_VOID);
    node->func_decl.return_type = s->ret_type;
  }

  sema_scope_pop(s);

  s->ret_type = old_ret;
  s->fn_node = old_fn;
}

void sema_check_block(Sema *s, AstNode *node) {
  sema_scope_push(s);
  for (size_t i = 0; i < node->block.statements.len; i++) {
    sema_check_stmt(s, node->block.statements.items[i]);
  }
  sema_scope_pop(s);
}

void sema_check_return_stmt(Sema *s, AstNode *node) {
  if (!s->fn_node) {
    sema_error(s, node, "Return statement outside of function");
    return;
  }

  Type *expr_type = node->return_stmt.expr
                        ? sema_check_expr(s, node->return_stmt.expr)
                        : type_get_primitive(s->types, PRIM_VOID);

  if (type_is_unknown(s->ret_type)) {
    s->ret_type = expr_type;
    s->fn_node->func_decl.return_type = expr_type;
  } else if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
    sema_error(s, node, "Type mismatch: function returns '%s' but got '%s'",
               type_to_name(s->ret_type), type_to_name(expr_type));
  }
}
