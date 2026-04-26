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

bool type_is_slice_struct(Type *type) {
  if (type->kind != KIND_STRUCT)
    return false;
  if (sv_eq(type->name, sv_from_parts("Slice", 5)))
    return true;
  if (type->data.instance.from_template &&
      sv_eq(type->data.instance.from_template->name, sv_from_parts("Slice", 5)))
    return true;
  return false;
}

bool error_set_contains(Type *set, Type *error) {
  if (!set || set->kind != KIND_ERROR_SET || !error ||
      error->kind != KIND_ERROR)
    return false;
  for (size_t i = 0; i < set->members.len; i++) {
    Member *m = set->members.items[i];
    if (type_equals(m->type, error))
      return true;
  }
  return false;
}

bool type_needs_inference(Type *t) {
  if (!t)
    return true;

  if (t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_UNKNOWN)
    return true;

  if (t->kind == KIND_TEMPLATE)
    return true;

  if (t->kind == KIND_STRUCT && t->data.instance.from_template != NULL) {
    return false;
  }

  return false;
}

bool type_is_condition(Type *t) {
  if (!t)
    return false;

  if (type_is_bool(t))
    return true;

  if (t->kind == KIND_POINTER)
    return true;

  return false;
}

bool type_can_be_polymorphic(Type *t) {
  if (!t)
    return false;

  switch (t->kind) {
  case KIND_TEMPLATE:
    return true;
  case KIND_POINTER:
    return type_can_be_polymorphic(t->data.pointer_to);
  case KIND_STRUCT:
  case KIND_UNION:
    if (t->data.instance.from_template == NULL)
      return false;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      if (type_can_be_polymorphic(t->data.instance.generic_args.items[i]))
        return true;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (type_can_be_polymorphic(m->type))
        return true;
    }
    return false;
  default:
    return false;
  }
}

bool sema_allows_polymorphic_types(Sema *s) {
  return s && s->generic_context_type != NULL;
}
