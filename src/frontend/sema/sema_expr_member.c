#include "sema_internal.h"

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

ExprInfo sema_check_field(Sema *s, AstNode *node) {
  if (!node->field.field.data || !node->field.field.len) {
    sema_error(s, node, "Member access requires a non-empty member name");
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  Symbol *module_sym = sema_resolve_module_path_symbol(s, node->field.object);
  if (module_sym) {
    // Resolve namespaced module access, e.g. std.fs.read
    Symbol *target_sym =
        module_lookup_export(module_sym->module, node->field.field);
    if (!target_sym) {
      sema_error(s, node, "Module does not contain '" SV_FMT "'",
                 SV_ARG(node->field.field));
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }

    if (!target_sym->is_export) {
      sema_error(s, node,
                 "'" SV_FMT "' is not accessible from module namespace",
                 SV_ARG(node->field.field));
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }

    node->resolved_type = target_sym->type;
    return (ExprInfo){.type = target_sym->type, .category = VAL_RVALUE};
  }

  ExprInfo object_info = sema_check_expr(s, node->field.object);
  Type *obj_type = object_info.type;
  if (!obj_type) {
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  while (obj_type->kind == KIND_POINTER) {
    obj_type = obj_type->data.pointer_to;
    if (!obj_type) {
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }
  }

  if (obj_type->kind != KIND_STRUCT && obj_type->kind != KIND_UNION &&
      obj_type->kind != KIND_TEMPLATE && obj_type->kind != KIND_ERROR &&
      obj_type->kind != KIND_STRING_BUFFER &&
      !(obj_type->kind == KIND_PRIMITIVE &&
        obj_type->data.primitive == PRIM_STRING)) {
    sema_error(s, node->field.object,
               "Member access only allowed on structs or unions, got %s",
               type_to_name(obj_type));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  Member *m = type_get_member(obj_type, node->field.field);
  if (m) {
    node->resolved_type = m->type;
    return (ExprInfo){.type = m->type, .category = VAL_LVALUE};
  }

  if (obj_type->kind == KIND_UNION) {
    Type *owner = NULL;
    m = type_find_union_field(obj_type, node->field.field, &owner);
    if (m) {
      node->resolved_type = m->type;
      return (ExprInfo){.type = m->type, .category = VAL_LVALUE};
    }
  }

  for (size_t i = 0; i < obj_type->methods.len; i++) {
    Symbol *method = obj_type->methods.items[i];
    if (!method)
      continue;
    if (!method->original_name.data && !method->name.data)
      continue;
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, node->field.field)) {
      if (method->kind == SYM_METHOD || method->kind == SYM_STATIC_METHOD) {
        node->resolved_type = method->type;
        return (ExprInfo){.type = method->type, .category = VAL_RVALUE};
      }
    }
  }

  if (obj_type->kind == KIND_STRUCT && obj_type->data.instance.from_template) {
    Type *template_type = obj_type->data.instance.from_template;
    for (size_t i = 0; i < template_type->methods.len; i++) {
      Symbol *method = template_type->methods.items[i];
      if (!method)
        continue;
      StringView lookup_name =
          method->original_name.len ? method->original_name : method->name;
      if (sv_eq(lookup_name, node->field.field)) {
        if (method->kind == SYM_METHOD) {
          node->resolved_type = method->type;
          return (ExprInfo){.type = method->type, .category = VAL_RVALUE};
        }
      }
    }
  }

  sema_error(s, node, "Type %s has no member '" SV_FMT "'",
             type_to_name(obj_type), SV_ARG(node->field.field));
  return (ExprInfo){
      .type = type_get_primitive(s->types, PRIM_UNKNOWN),
      .category = VAL_RVALUE,
  };
}

ExprInfo sema_check_static_member(Sema *s, AstNode *node) {
  if (!node->static_member.member.data || !node->static_member.member.len) {
    sema_error(s, node, "Static member access requires a non-empty name");
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  Symbol *parent = sema_resolve(s, node->static_member.parent);
  if (parent && parent->kind == SYM_MODULE) {
    Symbol *target_sym =
        module_lookup_export(parent->module, node->static_member.member);
    if (!target_sym) {
      sema_error(s, node, "Module '" SV_FMT "' has no symbol '" SV_FMT "'",
                 SV_ARG(parent->name), SV_ARG(node->static_member.member));
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }
    return (ExprInfo){.type = target_sym->type, .category = VAL_RVALUE};
  }

  Type *type = NULL;
  if (parent) {
    type = parent->type;
  } else {
    type = sema_find_type_by_name(s, node->static_member.parent);
  }

  if (!type || (type->kind != KIND_STRUCT && type->kind != KIND_TEMPLATE &&
                type->kind != KIND_STRING_BUFFER &&
                !(type->kind == KIND_PRIMITIVE &&
                  type->data.primitive == PRIM_STRING))) {
    sema_error(s, node, "Undefined type '" SV_FMT "'",
               SV_ARG(node->static_member.parent));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  for (size_t i = 0; i < type->methods.len; i++) {
    Symbol *method = type->methods.items[i];
    if (!method)
      continue;
    if (!method->original_name.data && !method->name.data)
      continue;
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, node->static_member.member)) {
      if (method->kind == SYM_STATIC_METHOD) {
        return (ExprInfo){.type = method->type, .category = VAL_RVALUE};
      }
    }
  }

  sema_error(s, node, "Type %s has no static member '" SV_FMT "'",
             type_to_name(type), SV_ARG(node->static_member.member));
  return (ExprInfo){
      .type = type_get_primitive(s->types, PRIM_UNKNOWN),
      .category = VAL_RVALUE,
  };
}
