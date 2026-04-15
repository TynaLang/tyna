#include "sema_internal.h"

bool type_is_array_struct(Type *type) {
  if (type->kind != KIND_STRUCT)
    return false;
  if (sv_eq(type->name, sv_from_parts("Array", 5)))
    return true;
  if (type->data.instance.from_template &&
      sv_eq(type->data.instance.from_template->name, sv_from_parts("Array", 5)))
    return true;
  return false;
}

Type *check_field(Sema *s, AstNode *node) {
  Type *obj_type = sema_check_expr(s, node->field.object);
  if (!obj_type) {
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  while (obj_type->kind == KIND_POINTER) {
    obj_type = obj_type->data.pointer_to;
    if (!obj_type) {
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
  }

  if (obj_type->kind != KIND_STRUCT && obj_type->kind != KIND_UNION &&
      obj_type->kind != KIND_TEMPLATE &&
      !(obj_type->kind == KIND_PRIMITIVE &&
        obj_type->data.primitive == PRIM_STRING)) {
    sema_error(s, node->field.object,
               "Member access only allowed on structs or unions, got %s",
               type_to_name(obj_type));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Member *m = type_get_member(obj_type, node->field.field);
  if (m) {
    node->resolved_type = m->type;
    return m->type;
  }

  if (obj_type->kind == KIND_UNION) {
    Type *owner = NULL;
    m = type_find_union_field(obj_type, node->field.field, &owner);
    if (m) {
      node->resolved_type = m->type;
      return m->type;
    }
  }

  for (size_t i = 0; i < obj_type->methods.len; i++) {
    Symbol *method = obj_type->methods.items[i];
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, node->field.field)) {
      if (method->kind == SYM_METHOD) {
        node->resolved_type = method->type;
        return method->type;
      }
    }
  }

  if (obj_type->kind == KIND_STRUCT && obj_type->data.instance.from_template) {
    Type *template_type = obj_type->data.instance.from_template;
    for (size_t i = 0; i < template_type->methods.len; i++) {
      Symbol *method = template_type->methods.items[i];
      StringView lookup_name =
          method->original_name.len ? method->original_name : method->name;
      if (sv_eq(lookup_name, node->field.field)) {
        if (method->kind == SYM_METHOD) {
          node->resolved_type = method->type;
          return method->type;
        }
      }
    }
  }

  sema_error(s, node, "Type %s has no member '" SV_FMT "'",
             type_to_name(obj_type), SV_ARG(node->field.field));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}

Type *check_static_member(Sema *s, AstNode *node) {
  Symbol *parent = sema_resolve(s, node->static_member.parent);
  Type *type = NULL;
  if (parent) {
    type = parent->type;
  } else {
    type = sema_find_type_by_name(s, node->static_member.parent);
  }

  if (!type || (type->kind != KIND_STRUCT && type->kind != KIND_TEMPLATE &&
                !(type->kind == KIND_PRIMITIVE &&
                  type->data.primitive == PRIM_STRING))) {
    sema_error(s, node, "Undefined type '" SV_FMT "'",
               SV_ARG(node->static_member.parent));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  for (size_t i = 0; i < type->methods.len; i++) {
    Symbol *method = type->methods.items[i];
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, node->static_member.member)) {
      if (method->kind == SYM_STATIC_METHOD) {
        return method->type;
      }
    }
  }

  sema_error(s, node, "Type %s has no static member '" SV_FMT "'",
             type_to_name(type), SV_ARG(node->static_member.member));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}
