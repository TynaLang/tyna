#include "sema_internal.h"

static Type *check_builtin_call(Sema *s, AstNode *node) {
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
      Type *arg_ty = sema_check_expr(s, node->call.args.items[0]);
      if (!type_is_array_struct(arg_ty)) {
        sema_error(s, node->call.args.items[0],
                   "free() expects an array, got %s",
                   type_to_name(arg_ty));
      }
    }
    return type_get_primitive(s->types, PRIM_VOID);

  default:
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

Type *check_call(Sema *s, AstNode *node) {
  if (!node || !node->call.func) {
    sema_error(s, node, "Invalid function call expression");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Type *builtin_type = check_builtin_call(s, node);
  if (builtin_type) {
    return builtin_type;
  }

  if (node->call.func->tag == NODE_FIELD) {
    AstNode *field_node = node->call.func;
    if (!field_node->field.object) {
      sema_error(s, node, "Invalid method call object");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    Type *orig_obj_type = sema_check_expr(s, field_node->field.object);
    if (!orig_obj_type) {
      orig_obj_type = type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    Type *obj_type = orig_obj_type;

    while (obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }

    if (obj_type->kind == KIND_STRUCT || obj_type->kind == KIND_TEMPLATE ||
        (obj_type->kind == KIND_PRIMITIVE &&
         obj_type->data.primitive == PRIM_STRING)) {
      for (size_t i = 0; i < obj_type->methods.len; i++) {
        Symbol *method = obj_type->methods.items[i];
        if (!method)
          continue;
        StringView lookup_name =
            method->original_name.len ? method->original_name : method->name;
        if (!field_node->field.field.data)
          continue;
        if (sv_eq(lookup_name, field_node->field.field) &&
            method->kind == SYM_METHOD) {
          Symbol *concrete_method =
              sema_instantiate_method_symbol(s, obj_type, method, field_node);
          if (!concrete_method)
            continue;

          if (!sema_resolve(s, concrete_method->name)) {
            Symbol *alias =
                sema_define(s, concrete_method->name, concrete_method->type,
                            true, field_node->loc);
            if (alias) {
              alias->original_name = concrete_method->original_name;
              alias->value = concrete_method->value;
              alias->kind = concrete_method->kind;
              alias->is_export = false;
              alias->func_status = concrete_method->func_status;
            }
          }

          List new_args;
          List_init(&new_args);

          AstNode *self_arg = field_node->field.object;
          if (concrete_method->value &&
              concrete_method->value->tag == NODE_FUNC_DECL &&
              concrete_method->value->func_decl.params.len > 0) {
            AstNode *first_param =
                concrete_method->value->func_decl.params.items[0];
            Type *param_type = first_param->param.type;
            if (param_type->kind == KIND_POINTER &&
                orig_obj_type->kind != KIND_POINTER &&
                type_equals(param_type->data.pointer_to, orig_obj_type)) {
              self_arg = AstNode_new_unary(OP_ADDR_OF, field_node->field.object,
                                           field_node->loc);
            }
          }

          List_push(&new_args, self_arg);
          for (size_t j = 0; j < node->call.args.len; j++) {
            List_push(&new_args, node->call.args.items[j]);
          }

          node->call.func =
              AstNode_new_var(concrete_method->name, field_node->loc);
          node->call.args = new_args;

          return check_call(s, node);
        }
      }

      if (obj_type->kind == KIND_STRUCT &&
          obj_type->data.instance.from_template) {
        Type *template_type = obj_type->data.instance.from_template;
        for (size_t i = 0; i < template_type->methods.len; i++) {
          Symbol *method = template_type->methods.items[i];
          if (!method)
            continue;
          StringView lookup_name =
              method->original_name.len ? method->original_name : method->name;
          if (!field_node->field.field.data)
            continue;
          if (sv_eq(lookup_name, field_node->field.field) &&
              method->kind == SYM_METHOD) {
            Symbol *concrete_method =
                sema_instantiate_method_symbol(s, obj_type, method, field_node);
            if (!concrete_method)
              continue;

            if (!sema_resolve(s, concrete_method->name)) {
              Symbol *alias =
                  sema_define(s, concrete_method->name, concrete_method->type,
                              true, field_node->loc);
              if (alias) {
                alias->original_name = concrete_method->original_name;
                alias->value = concrete_method->value;
                alias->kind = concrete_method->kind;
                alias->is_export = false;
                alias->func_status = concrete_method->func_status;
              }
            }

            List new_args;
            List_init(&new_args);

            AstNode *self_arg = field_node->field.object;
            if (concrete_method->value &&
                concrete_method->value->tag == NODE_FUNC_DECL &&
                concrete_method->value->func_decl.params.len > 0) {
              AstNode *first_param =
                  concrete_method->value->func_decl.params.items[0];
              Type *param_type = first_param->param.type;
              if (param_type->kind == KIND_POINTER &&
                  orig_obj_type->kind != KIND_POINTER &&
                  type_equals(param_type->data.pointer_to, orig_obj_type)) {
                self_arg = AstNode_new_unary(
                    OP_ADDR_OF, field_node->field.object, field_node->loc);
              }
            }

            List_push(&new_args, self_arg);
            for (size_t j = 0; j < node->call.args.len; j++) {
              List_push(&new_args, node->call.args.items[j]);
            }

            node->call.func =
                AstNode_new_var(concrete_method->name, field_node->loc);
            node->call.args = new_args;

            return check_call(s, node);
          }
        }
      }
    }
  }

  if (node->call.func->tag == NODE_STATIC_MEMBER) {
    AstNode *sm = node->call.func;
    Symbol *parent = sema_resolve(s, sm->static_member.parent);
    Type *parent_type =
        parent ? parent->type
               : sema_find_type_by_name(s, sm->static_member.parent);
    if (parent_type && (parent_type->kind == KIND_STRUCT ||
                        parent_type->kind == KIND_TEMPLATE ||
                        (parent_type->kind == KIND_PRIMITIVE &&
                         parent_type->data.primitive == PRIM_STRING))) {
      for (size_t i = 0; i < parent_type->methods.len; i++) {
        Symbol *method = parent_type->methods.items[i];
        if (!method || method->kind != SYM_STATIC_METHOD)
          continue;
        StringView lookup_name =
            method->original_name.len ? method->original_name : method->name;
        if (!sm->static_member.member.data)
          continue;
        if (sv_eq(lookup_name, sm->static_member.member)) {
          if (!sema_resolve(s, method->name)) {
            Symbol *alias =
                sema_define(s, method->name, method->type, true, sm->loc);
            if (alias) {
              alias->original_name = method->original_name;
              alias->value = method->value;
              alias->kind = method->kind;
              alias->is_export = false;
            }
          }
          node->call.func = AstNode_new_var(method->name, sm->loc);
          return check_call(s, node);
        }
      }
    }
  }

  Type *func_type = sema_check_expr(s, node->call.func);
  if (func_type->kind == KIND_PRIMITIVE &&
      func_type->data.primitive == PRIM_UNKNOWN) {
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  if (node->call.func->tag == NODE_VAR) {
    StringView name = node->call.func->var.value;
    Symbol *symbol = sema_resolve(s, name);
    if (!symbol) {
      sema_error(s, node, "Call to undefined function '" SV_FMT "'",
                 SV_ARG(node->call.func->var.value));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    if (symbol->value && symbol->value->tag == NODE_FUNC_DECL) {
      AstNode *fn_decl = symbol->value;
      if (node->call.args.len != fn_decl->func_decl.params.len) {
        sema_error(s, node, "Function '" SV_FMT "' expects %zu args, got %zu",
                   SV_ARG(name), fn_decl->func_decl.params.len,
                   node->call.args.len);
      }
      for (size_t i = 0;
           i < node->call.args.len && i < fn_decl->func_decl.params.len; i++) {
        AstNode *arg_node = node->call.args.items[i];
        Type *arg_type = sema_check_expr(s, arg_node);
        AstNode *param_node = fn_decl->func_decl.params.items[i];
        Type *param_type = param_node->param.type;
        if (!type_can_implicitly_cast(param_type, arg_type)) {
          sema_error(s, node->call.args.items[i],
                     "Type mismatch: expected %s, got %s",
                     type_to_name(param_type), type_to_name(arg_type));
        }
        check_literal_bounds(s, node->call.args.items[i], param_type);
      }
    }

    return symbol->type;
  } else {
    return func_type;
  }
}
