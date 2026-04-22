#include "sema_internal.h"

Type *sema_find_type_by_name(Sema *s, StringView name) {
  if (sv_eq_cstr(name, "String")) {
    return type_get_string_buffer(s->types);
  }

  for (size_t i = 0; i < s->types->structs.len; i++) {
    Type *t = s->types->structs.items[i];
    if (sv_eq(t->name, name))
      return t;
  }
  for (size_t i = 0; i < s->types->instances.len; i++) {
    Type *t = s->types->instances.items[i];
    if (sv_eq(t->name, name) || sv_eq_cstr(name, type_to_name(t)))
      return t;
  }

  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    Type *t = s->types->primitives[i];
    if (!t)
      continue;
    const char *prim_name = type_to_name(t);
    if (sv_eq_cstr(name, prim_name)) {
      return t;
    }
  }

  return NULL;
}

bool sema_is_writable_address(Sema *s, AstNode *node) {
  if (!node)
    return false;

  switch (node->tag) {
  case NODE_VAR: {
    Symbol *sym = sema_resolve(s, node->var.value);
    return sym && (sym->kind == SYM_VAR || sym->kind == SYM_FIELD) &&
           !sym->is_const;
  }

  case NODE_FIELD: {
    ExprInfo object_info = sema_check_expr(s, node->field.object);
    if (object_info.category != VAL_LVALUE)
      return false;

    Type *obj_type = object_info.type;
    while (obj_type && obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }
    if (!obj_type)
      return false;

    Member *m = type_get_member(obj_type, node->field.field);
    if (m)
      return true;

    if (obj_type->kind == KIND_UNION) {
      Type *owner = NULL;
      m = type_find_union_field(obj_type, node->field.field, &owner);
      return m != NULL;
    }
    return false;
  }

  case NODE_INDEX: {
    ExprInfo array_info = sema_check_expr(s, node->index.array);
    if (array_info.category != VAL_LVALUE)
      return false;

    Type *arr_type = node->index.array->resolved_type;
    if (!arr_type)
      return false;
    if (arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING) {
      return false;
    }
    return arr_type->kind == KIND_POINTER || type_is_array_struct(arr_type);
  }

  case NODE_UNARY:
    if (node->unary.op == OP_DEREF) {
      Type *operand_type = sema_check_expr(s, node->unary.expr).type;
      return operand_type && operand_type->kind == KIND_POINTER;
    }
    return false;

  default:
    return false;
  }
}

bool sema_fn_decl_can_use_arena(AstNode *fn_decl) {
  if (!fn_decl || fn_decl->tag != NODE_FUNC_DECL)
    return false;

  if (fn_decl->func_decl.return_type &&
      ((fn_decl->func_decl.return_type->kind == KIND_PRIMITIVE &&
        fn_decl->func_decl.return_type->data.primitive == PRIM_STRING) ||
       fn_decl->func_decl.return_type->kind == KIND_STRING_BUFFER)) {
    return false;
  }

  for (size_t i = 0; i < fn_decl->func_decl.params.len; i++) {
    AstNode *param_node = fn_decl->func_decl.params.items[i];
    if (!param_node || param_node->tag != NODE_PARAM)
      continue;
    Type *param_type = param_node->param.type;
    if (!param_type)
      continue;

    Type *resolved = param_type;
    while (resolved && resolved->kind == KIND_POINTER) {
      resolved = resolved->data.pointer_to;
    }
    if (resolved && resolved->kind == KIND_STRING_BUFFER) {
      return false;
    }
  }

  return true;
}