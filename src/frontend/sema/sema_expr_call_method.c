#include "sema_internal.h"

static bool sema_bind_method_alias(Sema *s, Symbol *concrete_method,
                                   AstNode *field_node) {
  if (sema_resolve(s, concrete_method->name)) {
    return true;
  }

  Symbol *alias = sema_define(s, concrete_method->name, concrete_method->type,
                              true, field_node->loc);
  if (!alias) {
    return false;
  }

  alias->original_name = concrete_method->original_name;
  alias->value = concrete_method->value;
  alias->kind = concrete_method->kind;
  alias->is_export = false;
  alias->func_status = concrete_method->func_status;
  return true;
}

static AstNode *sema_build_method_self_arg(Symbol *concrete_method,
                                           AstNode *object_node,
                                           Type *orig_obj_type, Location loc) {
  AstNode *self_arg = object_node;
  if (concrete_method->kind != SYM_METHOD || !concrete_method->value ||
      concrete_method->value->tag != NODE_FUNC_DECL ||
      concrete_method->value->func_decl.params.len == 0) {
    return self_arg;
  }

  AstNode *first_param = concrete_method->value->func_decl.params.items[0];
  Type *param_type = first_param->param.type;
  if (param_type->kind == KIND_POINTER && orig_obj_type->kind != KIND_POINTER &&
      type_equals(param_type->data.pointer_to, orig_obj_type)) {
    return AstNode_new_unary(OP_ADDR_OF, object_node, loc);
  }

  return self_arg;
}

static Symbol *sema_resolve_module_path_symbol(Sema *s, AstNode *node) {
  if (!node)
    return NULL;

  if (node->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, node->var.value);
    if (sym && sym->kind == SYM_MODULE)
      return sym;
    return NULL;
  }

  if (node->tag != NODE_FIELD)
    return NULL;

  Symbol *parent = sema_resolve_module_path_symbol(s, node->field.object);
  if (!parent || parent->kind != SYM_MODULE)
    return NULL;

  return module_lookup_export(parent->module, node->field.field);
}

static bool sema_try_resolve_struct_method(Sema *s, AstNode *node,
                                           AstNode *field_node,
                                           Type *lookup_type,
                                           Type *concrete_obj_type,
                                           Type *orig_obj_type) {
  if (!lookup_type)
    return false;

  if (!concrete_obj_type)
    concrete_obj_type = lookup_type;

  for (size_t i = 0; i < lookup_type->methods.len; i++) {
    Symbol *method = lookup_type->methods.items[i];
    if (!method)
      continue;

    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (!field_node->field.field.data)
      continue;
    if (!sv_eq(lookup_name, field_node->field.field) ||
        (method->kind != SYM_METHOD && method->kind != SYM_STATIC_METHOD)) {
      continue;
    }

    Symbol *concrete_method = sema_instantiate_method_symbol(
        s, concrete_obj_type, method, field_node);
    if (!concrete_method)
      continue;

    if (!sema_bind_method_alias(s, concrete_method, field_node)) {
      continue;
    }

    List new_args;
    List_init(&new_args);

    if (concrete_method->kind == SYM_METHOD) {
      List_push(&new_args, sema_build_method_self_arg(
                               concrete_method, field_node->field.object,
                               orig_obj_type, field_node->loc));
    }

    for (size_t j = 0; j < node->call.args.len; j++) {
      List_push(&new_args, node->call.args.items[j]);
    }

    node->call.func = AstNode_new_var(concrete_method->name, field_node->loc);
    node->call.args = new_args;
    return true;
  }

  return false;
}

static bool sema_try_resolve_static_method(Sema *s, AstNode *node, AstNode *sm,
                                           Type *parent_type) {
  Type *lookup_type = parent_type;
  if (lookup_type && lookup_type->kind == KIND_STRUCT &&
      lookup_type->data.instance.from_template) {
    lookup_type = lookup_type->data.instance.from_template;
  }

  for (size_t i = 0; i < lookup_type->methods.len; i++) {
    Symbol *method = lookup_type->methods.items[i];
    if (!method || method->kind != SYM_STATIC_METHOD)
      continue;

    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (!sm->static_member.member.data ||
        !sv_eq(lookup_name, sm->static_member.member)) {
      continue;
    }

    Symbol *resolved_method =
        sema_instantiate_method_symbol(s, parent_type, method, sm);
    if (!resolved_method)
      continue;

    if (!sema_resolve(s, resolved_method->name)) {
      Symbol *alias = sema_define(s, resolved_method->name,
                                  resolved_method->type, true, sm->loc);
      if (alias) {
        alias->original_name = resolved_method->original_name;
        alias->value = resolved_method->value;
        alias->kind = resolved_method->kind;
        alias->is_export = false;
      }
    }
    node->call.func = AstNode_new_var(resolved_method->name, sm->loc);
    return true;
  }

  // Fallback: method signatures are registered before impl bodies are checked,
  // so static calls can be resolved even when lookup_type->methods is not
  // populated yet.
  StringView owner_candidates[2];
  size_t owner_count = 0;
  owner_candidates[owner_count++] = sv_from_cstr(type_to_name(lookup_type));

  if (lookup_type->kind == KIND_TEMPLATE &&
      lookup_type->data.template.placeholders.len > 0) {
    size_t cap = lookup_type->name.len + 3;
    for (size_t i = 0; i < lookup_type->data.template.placeholders.len; i++) {
      StringView p =
          *(StringView *)lookup_type->data.template.placeholders.items[i];
      cap += p.len + 2;
    }

    char *owner_buf = xmalloc(cap);
    size_t pos = 0;
    memcpy(owner_buf + pos, lookup_type->name.data, lookup_type->name.len);
    pos += lookup_type->name.len;
    owner_buf[pos++] = '<';
    for (size_t i = 0; i < lookup_type->data.template.placeholders.len; i++) {
      if (i > 0) {
        owner_buf[pos++] = ',';
        owner_buf[pos++] = ' ';
      }
      StringView p =
          *(StringView *)lookup_type->data.template.placeholders.items[i];
      memcpy(owner_buf + pos, p.data, p.len);
      pos += p.len;
    }
    owner_buf[pos++] = '>';
    owner_buf[pos] = '\0';
    owner_candidates[owner_count++] = sv_from_parts(owner_buf, pos);
  }

  for (size_t i = 0; i < owner_count; i++) {
    StringView mangled =
        sema_mangle_method_name(owner_candidates[i], sm->static_member.member);
    Symbol *method = sema_resolve(s, mangled);
    if (!method || method->kind != SYM_STATIC_METHOD)
      continue;

    Symbol *resolved_method =
        sema_instantiate_method_symbol(s, parent_type, method, sm);
    if (!resolved_method)
      continue;

    if (!sema_resolve(s, resolved_method->name)) {
      Symbol *alias = sema_define(s, resolved_method->name,
                                  resolved_method->type, true, sm->loc);
      if (alias) {
        alias->original_name = resolved_method->original_name;
        alias->value = resolved_method->value;
        alias->kind = resolved_method->kind;
        alias->is_export = false;
      }
    }

    node->call.func = AstNode_new_var(resolved_method->name, sm->loc);
    return true;
  }

  return false;
}

bool sema_try_resolve_method_call(Sema *s, AstNode *node) {
  if (!node || !node->call.func)
    return false;

  if (node->call.func->tag == NODE_FIELD) {
    AstNode *field_node = node->call.func;
    if (!field_node->field.object) {
      sema_error(s, node, "Invalid method call object");
      return true;
    }
    if (!field_node->field.field.data || !field_node->field.field.len) {
      sema_error(s, node, "Method call requires a non-empty method name");
      return true;
    }

    ExprInfo orig_obj_info = sema_check_expr(s, field_node->field.object);
    Type *orig_obj_type = orig_obj_info.type;
    if (!orig_obj_type) {
      orig_obj_type = type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    Type *obj_type = orig_obj_type;

    while (obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }

    if (obj_type->kind == KIND_STRUCT || obj_type->kind == KIND_TEMPLATE ||
        obj_type->kind == KIND_ERROR || obj_type->kind == KIND_STRING_BUFFER ||
        (obj_type->kind == KIND_PRIMITIVE &&
         obj_type->data.primitive == PRIM_STRING)) {
      if (sema_try_resolve_struct_method(s, node, field_node, obj_type,
                                         obj_type, orig_obj_type)) {
        return true;
      }

      if (obj_type->kind == KIND_STRUCT &&
          obj_type->data.instance.from_template) {
        Type *template_type = obj_type->data.instance.from_template;
        if (sema_try_resolve_struct_method(s, node, field_node, template_type,
                                           obj_type, orig_obj_type)) {
          return true;
        }
      }
    }

    Symbol *module_sym =
        sema_resolve_module_path_symbol(s, field_node->field.object);
    if (module_sym) {
      Symbol *target_sym =
          module_lookup_export(module_sym->module, field_node->field.field);
      if (!target_sym) {
        sema_error(s, node, "Module does not contain '" SV_FMT "'",
                   SV_ARG(field_node->field.field));
        return true;
      }

      if (!sema_bind_method_alias(s, target_sym, field_node)) {
        return true;
      }

      node->call.func = AstNode_new_var(target_sym->name, field_node->loc);
      return true;
    }
  }

  if (node->call.func->tag == NODE_STATIC_MEMBER) {
    AstNode *sm = node->call.func;
    if (!sm->static_member.member.data || !sm->static_member.member.len) {
      sema_error(s, node, "Static method call requires a non-empty name");
      return true;
    }
    Symbol *parent = sema_resolve(s, sm->static_member.parent);
    Type *parent_type =
        parent ? parent->type
               : sema_find_type_by_name(s, sm->static_member.parent);
    if (parent_type && (parent_type->kind == KIND_STRUCT ||
                        parent_type->kind == KIND_TEMPLATE ||
                        parent_type->kind == KIND_ERROR ||
                        parent_type->kind == KIND_STRING_BUFFER ||
                        (parent_type->kind == KIND_PRIMITIVE &&
                         parent_type->data.primitive == PRIM_STRING))) {
      if (sema_try_resolve_static_method(s, node, sm, parent_type)) {
        return true;
      }
    }
  }

  return false;
}