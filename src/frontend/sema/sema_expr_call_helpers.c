#include "sema_internal.h"

bool sema_is_consuming_method(Symbol *method) {
  if (!method)
    return false;

  StringView method_name =
      method->original_name.len ? method->original_name : method->name;
  return sv_eq_cstr(method_name, "into_str") || sv_eq_cstr(method_name, "free");
}

void sema_mark_current_function_consumes_string_arg(Sema *s) {
  if (!s || !s->fn_node)
    return;
  s->fn_node->func_decl.consumes_string_arg = true;
}

void sema_mark_move_if_consuming_arg(Sema *s, AstNode *arg, Type *param_type,
                                     bool force_move) {
  if (!arg)
    return;
  if (!force_move && (!param_type || param_type->kind != KIND_STRING_BUFFER))
    return;

  AstNode *var_node = sema_find_underlying_var(arg);
  if (!var_node)
    return;

  Symbol *sym = sema_resolve(s, var_node->var.value);
  if (sym && sym->kind == SYM_VAR && !sym->is_const) {
    sema_mark_moved_symbol(sym);
  }
}

Type *sema_check_builtin_call(Sema *s, AstNode *node) {
  if (!node || !node->call.func || node->call.func->tag != NODE_VAR) {
    return NULL;
  }

  Symbol *symbol = sema_resolve(s, node->call.func->var.value);
  if (!symbol || symbol->builtin_kind == BUILTIN_NONE) {
    return NULL;
  }

  switch (symbol->builtin_kind) {
  case BUILTIN_TYPEOF:
    if (node->call.args.len != 1) {
      sema_error(s, node, "typeof() expects 1 argument, got %zu",
                 node->call.args.len);
    } else {
      sema_check_expr(s, node->call.args.items[0]);
    }
    return type_get_primitive(s->types, PRIM_STRING);

  case BUILTIN_FREE:
    if (node->call.args.len != 1) {
      sema_error(s, node, "free() expects 1 argument, got %zu",
                 node->call.args.len);
    } else {
      Type *arg_ty = sema_check_expr(s, node->call.args.items[0]).type;
      if (!arg_ty || arg_ty->kind != KIND_POINTER) {
        sema_error(s, node->call.args.items[0],
                   "free() expects a pointer, got %s", type_to_name(arg_ty));
      }
    }
    return type_get_primitive(s->types, PRIM_VOID);

  case BUILTIN_SOME: {
    if (node->call.args.len != 1) {
      sema_error(s, node, "Some() expects 1 argument, got %zu",
                 node->call.args.len);
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    Type *arg_ty = sema_check_expr(s, node->call.args.items[0]).type;
    if (!arg_ty) {
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    Type *option_tpl = type_get_template(s->types, sv_from_parts("Option", 6));
    if (!option_tpl) {
      sema_error(s, node, "Internal error: Option type unavailable");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    List args;
    List_init(&args);
    List_push(&args, arg_ty);
    Type *option_type = type_get_instance(s->types, option_tpl, args);
    List_free(&args, 0);
    return option_type;
  }

  case BUILTIN_HEAP:
  case BUILTIN_REF: {
    if (node->call.args.len != 1) {
      sema_error(s, node, "%.*s() expects 1 argument, got %zu",
                 (int)node->call.func->var.value.len,
                 node->call.func->var.value.data, node->call.args.len);
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    Type *arg_ty = sema_check_expr(s, node->call.args.items[0]).type;
    if (!arg_ty) {
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    Type *template_type =
        type_get_template(s->types, node->call.func->var.value);
    if (!template_type) {
      sema_error(s, node, "Internal error: %.*s type unavailable",
                 (int)node->call.func->var.value.len,
                 node->call.func->var.value.data);
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    List args;
    List_init(&args);
    List_push(&args, arg_ty);
    Type *result_type = type_get_instance(s->types, template_type, args);
    List_free(&args, 0);
    return result_type;
  }

  default:
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

bool sema_is_string_literal(AstNode *node) {
  return node && node->tag == NODE_STRING;
}

void sema_fold_string_concat_literals(Sema *s, AstNode *node) {
  if (!node || node->call.args.len == 0)
    return;

  AstNode *left = NULL;
  AstNode *right = NULL;

  if (node->call.func->tag == NODE_VAR && node->call.args.len == 2) {
    StringView fn_name = node->call.func->var.value;
    if (!sv_eq(fn_name, sv_from_cstr("__tyna_str_concat")))
      return;
    left = node->call.args.items[0];
    right = node->call.args.items[1];
  } else if (node->call.func->tag == NODE_FIELD && node->call.args.len == 1) {
    AstNode *field_node = node->call.func;
    if (!field_node->field.field.data ||
        !sv_eq_cstr(field_node->field.field, "concat")) {
      return;
    }
    left = field_node->field.object;
    right = node->call.args.items[0];
  } else {
    return;
  }

  if (!sema_is_string_literal(left) || !sema_is_string_literal(right))
    return;

  size_t len = left->string.value.len + right->string.value.len;
  char *buf = xmalloc(len + 1);
  memcpy(buf, left->string.value.data, left->string.value.len);
  memcpy(buf + left->string.value.len, right->string.value.data,
         right->string.value.len);
  buf[len] = '\0';

  node->tag = NODE_STRING;
  node->string.value = sv_from_parts(buf, len);
  node->resolved_type = type_get_primitive(s->types, PRIM_STRING);
}