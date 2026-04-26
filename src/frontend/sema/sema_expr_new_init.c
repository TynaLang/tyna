#include "sema_internal.h"

ExprInfo sema_check_new_expr(Sema *s, AstNode *node) {
  Type *target_type = node->new_expr.target_type;
  if (!target_type) {
    sema_error(s, node, "Missing target type in new expression");
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  if (target_type->kind == KIND_TEMPLATE && s->generic_context_type &&
      sv_eq(target_type->name, s->generic_context_type->name)) {
    target_type = s->generic_context_type;
    node->new_expr.target_type = target_type;
  }

  if (target_type->kind == KIND_TEMPLATE && s->generic_context_type &&
      s->generic_context_type->kind == KIND_STRUCT &&
      s->generic_context_type->data.instance.generic_args.len > 0) {
    Type *resolved_target =
        type_get_instance(s->types, target_type,
                          s->generic_context_type->data.instance.generic_args);
    if (resolved_target) {
      target_type = resolved_target;
      node->new_expr.target_type = target_type;
    }
  }

  if (target_type->kind == KIND_ERROR) {
    return sema_check_new_error_expr(s, node);
  }

  if (target_type->kind != KIND_STRUCT && target_type->kind != KIND_UNION) {
    sema_error(s, node,
               "Cannot allocate type '%s' with new; expected struct or union",
               type_to_name(target_type));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  if (node->new_expr.args.len > 0 && node->new_expr.field_inits.len > 0) {
    sema_error(s, node,
               "Cannot mix constructor arguments and struct literal fields");
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE};
  }

  if (node->new_expr.args.len > 0) {
    Symbol *constructor = NULL;
    for (size_t i = 0; i < target_type->methods.len; i++) {
      Symbol *method = target_type->methods.items[i];
      if (!method)
        continue;
      StringView lookup_name =
          method->original_name.len ? method->original_name : method->name;
      if (sv_eq_cstr(lookup_name, "init")) {
        constructor = method;
        break;
      }
    }

    if (!constructor || !constructor->value ||
        constructor->value->tag != NODE_FUNC_DECL) {
      sema_error(s, node, "Type '%s' has no constructor 'init' available",
                 type_to_name(target_type));
      return (ExprInfo){
          .type = target_type,
          .category = VAL_RVALUE,
      };
    }

    AstNode *fn_decl = constructor->value;
    size_t param_count = fn_decl->func_decl.params.len;
    if (param_count == 0) {
      sema_error(s, node, "Constructor 'init' on '%s' has no self parameter",
                 type_to_name(target_type));
      return (ExprInfo){
          .type = target_type,
          .category = VAL_RVALUE,
      };
    }

    size_t provided_args = node->new_expr.args.len;
    if (provided_args + 1 > param_count) {
      sema_error(s, node, "Constructor '%s' expects %zu arguments, got %zu",
                 type_to_name(target_type), param_count - 1, provided_args);
      return (ExprInfo){
          .type = target_type,
          .category = VAL_RVALUE,
      };
    }

    for (size_t i = provided_args + 1; i < param_count; i++) {
      AstNode *param_node = fn_decl->func_decl.params.items[i];
      if (!param_node->param.default_value) {
        sema_error(s, node, "Constructor '%s' expects %zu arguments, got %zu",
                   type_to_name(target_type), param_count - 1, provided_args);
        return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                          .category = VAL_RVALUE};
      }
    }

    for (size_t i = 0; i < provided_args; i++) {
      AstNode *arg_node = node->new_expr.args.items[i];
      AstNode *param_node = fn_decl->func_decl.params.items[i + 1];
      node->new_expr.args.items[i] =
          sema_coerce(s, arg_node, param_node->param.type);
    }
    for (size_t i = provided_args; i < param_count; i++) {
      AstNode *param_node = fn_decl->func_decl.params.items[i];
      if (param_node->param.default_value) {
        param_node->param.default_value = sema_coerce(
            s, param_node->param.default_value, param_node->param.type);
      }
    }
    return (ExprInfo){
        .type = target_type,
        .category = VAL_RVALUE,
    };
  } else {
    return sema_check_new_struct_expr(s, node);
  }
}
