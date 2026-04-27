#include "sema_internal.h"

ExprInfo sema_check_call(Sema *s, AstNode *node) {
  if (!node || !node->call.func) {
    sema_error(s, node, "Invalid function call expression");
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  sema_fold_string_concat_literals(s, node);

  if (node->tag == NODE_STRING) {
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_STRING),
                      .category = VAL_RVALUE};
  }

  Type *builtin_type = sema_check_builtin_call(s, node);
  if (builtin_type) {
    return (ExprInfo){.type = builtin_type, .category = VAL_RVALUE};
  }

  if (sema_try_resolve_method_call(s, node)) {
    return sema_check_call(s, node);
  }

  ExprInfo func_info = sema_check_expr(s, node->call.func);
  Type *func_type = func_info.type;
  if (func_type->kind == KIND_PRIMITIVE &&
      func_type->data.primitive == PRIM_UNKNOWN) {
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  bool consumes = false;
  AstNode *fn_decl = NULL;
  if (node->call.func->tag == NODE_VAR) {
    StringView name = node->call.func->var.value;
    Symbol *symbol = sema_resolve(s, name);
    if (!symbol) {
      sema_error(s, node, "Call to undefined function '" SV_FMT "'",
                 SV_ARG(node->call.func->var.value));
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }

    if (symbol->value && symbol->value->tag == NODE_FUNC_DECL) {
      fn_decl = symbol->value;
      node->call.func->var.value = fn_decl->func_decl.name->var.value;
      size_t param_count = fn_decl->func_decl.params.len;
      if (node->call.args.len > param_count) {
        sema_error(s, node, "Function '" SV_FMT "' expects %zu args, got %zu",
                   SV_ARG(name), param_count, node->call.args.len);
      }

      size_t provided_args = node->call.args.len;
      if (provided_args < param_count) {
        bool defaults_ok = true;
        for (size_t i = provided_args; i < param_count; i++) {
          AstNode *param_node = fn_decl->func_decl.params.items[i];
          if (!param_node) {
            defaults_ok = false;
            break;
          }
          if (!param_node->param.default_value) {
            defaults_ok = false;
            break;
          }
        }
        if (!defaults_ok) {
          sema_error(s, node, "Function '" SV_FMT "' expects %zu args, got %zu",
                     SV_ARG(name), param_count, provided_args);
        } else {
          for (size_t i = provided_args; i < param_count; i++) {
            AstNode *param_node = fn_decl->func_decl.params.items[i];
            if (!param_node || !param_node->param.default_value) {
              continue;
            }
            AstNode *default_arg = Ast_clone(param_node->param.default_value);
            List_push(&node->call.args, default_arg);
          }
        }
      }

      consumes = sema_is_consuming_method(symbol);
      for (size_t i = 0;
           i < node->call.args.len && i < fn_decl->func_decl.params.len; i++) {
        AstNode *arg_node = node->call.args.items[i];
        Type *arg_type = sema_check_expr(s, arg_node).type;
        AstNode *param_node = fn_decl->func_decl.params.items[i];
        if (!param_node) {
          sema_error(s, node, "Function '%s' has a malformed parameter list",
                     type_to_name(symbol->type));
          continue;
        }
        Type *param_type = param_node->param.type;

        if (param_type && param_type->kind == KIND_STRING_BUFFER && arg_type &&
            arg_type->kind == KIND_PRIMITIVE &&
            arg_type->data.primitive == PRIM_STRING) {
          AstNode *cast_arg =
              AstNode_new_cast_expr(arg_node, param_type, arg_node->loc);
          node->call.args.items[i] = cast_arg;
          arg_node = cast_arg;
          arg_type = sema_check_expr(s, arg_node).type;
        }

        if (!type_can_implicitly_cast(param_type, arg_type)) {
          sema_error(s, node->call.args.items[i],
                     "Type mismatch: expected %s, got %s",
                     type_to_name(param_type), type_to_name(arg_type));
        }
        sema_check_literal_bounds(s, node->call.args.items[i], param_type);
        sema_mark_move_if_consuming_arg(s, arg_node, param_type, consumes);
      }
    }

    if (s->fn_node &&
        (consumes || (symbol->value && symbol->value->tag == NODE_FUNC_DECL &&
                      symbol->value->func_decl.consumes_string_arg))) {
      sema_mark_current_function_consumes_string_arg(s);
    }

    Type *return_type = fn_decl ? fn_decl->func_decl.return_type : symbol->type;
    if (return_type && return_type->kind == KIND_PRIMITIVE &&
        return_type->data.primitive == PRIM_STRING) {
      if (s->scope) {
        s->scope->has_computed_str = true;
      }
    }

    return (ExprInfo){.type = return_type, .category = VAL_RVALUE};
  } else {
    // For non-var calls (e.g., NODE_STATIC_MEMBER), get return type from
    // checked expr
    Type *call_return_type = func_type;
    if (func_type && func_type->kind == KIND_PRIMITIVE &&
        func_type->data.primitive == PRIM_STRING) {
      if (s->scope) {
        s->scope->has_computed_str = true;
      }
    }
    return (ExprInfo){.type = call_return_type, .category = VAL_RVALUE};
  }
}
