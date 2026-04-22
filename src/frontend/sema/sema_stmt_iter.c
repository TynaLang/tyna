#include "sema_internal.h"

void sema_check_for_in_stmt(Sema *s, AstNode *node) {
  Type *iter_type = sema_check_expr(s, node->for_in_stmt.iterable).type;

  Type *elem_type = NULL;
  bool is_valid_iterable = false;

  if (iter_type->kind == KIND_PRIMITIVE &&
      iter_type->data.primitive == PRIM_STRING) {
    elem_type = type_get_primitive(s->types, PRIM_CHAR);
    is_valid_iterable = true;
  } else if (iter_type->kind == KIND_STRING_BUFFER) {
    elem_type = type_get_primitive(s->types, PRIM_CHAR);
    is_valid_iterable = true;
  } else if (iter_type->kind == KIND_STRUCT &&
             iter_type->data.instance.from_template &&
             sv_eq(iter_type->data.instance.from_template->name,
                   sv_from_parts("Array", 5))) {
    if (iter_type->data.instance.generic_args.len > 0) {
      elem_type = iter_type->data.instance.generic_args.items[0];
      is_valid_iterable = true;
    }
  }

  if (!is_valid_iterable) {
    sema_error(s, node->for_in_stmt.iterable, "Cannot iterate over type '%s'",
               type_to_name(iter_type));
    elem_type = type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  if (!elem_type) {
    elem_type = type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  sema_scope_push(s);

  Symbol *sym = sema_define(s, node->for_in_stmt.var->var.value, elem_type,
                            true, node->for_in_stmt.var->loc);
  if (sym) {
    node->for_in_stmt.var->resolved_type = elem_type;
  }

  SemaJump jump_ctx;
  sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
  sema_check_stmt(s, node->for_in_stmt.body);
  sema_jump_pop(s);

  sema_scope_pop(s);
}