#include "sema_internal.h"

static bool sema_is_fixed_to_dynamic_array_conversion(Type *to, Type *from) {
  if (!to || !from)
    return false;
  if (!type_is_array_struct(to) || !type_is_array_struct(from))
    return false;
  if (to->fixed_array_len != 0 || from->fixed_array_len == 0)
    return false;
  if (!to->data.instance.from_template || !from->data.instance.from_template)
    return false;
  if (to->data.instance.from_template != from->data.instance.from_template)
    return false;
  if (to->data.instance.generic_args.len == 0 ||
      from->data.instance.generic_args.len == 0)
    return false;

  return type_equals(to->data.instance.generic_args.items[0],
                     from->data.instance.generic_args.items[0]);
}

AstNode *sema_coerce(Sema *s, AstNode *expr, Type *target) {
  ExprInfo expr_info = sema_check_expr(s, expr);
  Type *expr_type = expr_info.type;
  bool target_is_tagged_union =
      target && target->kind == KIND_UNION && target->is_tagged_union;

  if (expr->tag == NODE_NONE && target && type_is_option(target)) {
    expr->resolved_type = target;
    return expr;
  }

  if (expr->tag == NODE_ARRAY_LITERAL && target &&
      target->kind == KIND_STRUCT && target->data.instance.from_template &&
      sv_eq(target->data.instance.from_template->name, sv_from_cstr("List"))) {
    if (target->data.instance.generic_args.len == 1) {
      Type *element_type = target->data.instance.generic_args.items[0];
      for (size_t i = 0; i < expr->array_literal.items.len; i++) {
        AstNode *item = expr->array_literal.items.items[i];
        if (!type_can_implicitly_cast(element_type, item->resolved_type)) {
          sema_error(s, item, "List element type mismatch: expected %s, got %s",
                     type_to_name(element_type),
                     type_to_name(item->resolved_type));
        }
        sema_check_literal_bounds(s, item, element_type);
      }
    }
    expr->resolved_type = target;
    return expr;
  }

  if (expr->tag == NODE_ARRAY_LITERAL && expr->array_literal.items.len == 0 &&
      target && target->kind == KIND_STRUCT &&
      target->data.instance.from_template && type_is_array_struct(target)) {
    expr->resolved_type = target;
    return expr;
  }

  if (expr->tag == NODE_NUMBER || expr->tag == NODE_CHAR) {
    sema_check_literal_bounds(s, expr, target);

    if (type_can_implicitly_cast(target, expr_type) ||
        (expr->tag == NODE_NUMBER && literal_fits_in_type(expr, target))) {
      if (!target_is_tagged_union) {
        expr->resolved_type = target;
      }
      return expr;
    }
  }

  if (target && target->kind == KIND_STRING_BUFFER && expr_type &&
      expr_type->kind == KIND_PRIMITIVE &&
      expr_type->data.primitive == PRIM_STRING) {
    expr = AstNode_new_cast_expr(expr, target, expr->loc);
    expr_type = target;
  }

  if (type_can_implicitly_cast(target, expr_type)) {
    // Keep fixed array literals as fixed values and materialize an explicit
    // cast so codegen can inject __tyna_array_from_stack.
    if (sema_is_fixed_to_dynamic_array_conversion(target, expr_type)) {
      AstNode *cast = AstNode_new_cast_expr(expr, target, expr->loc);
      sema_check_expr(s, cast);
      return cast;
    }

    if (!target_is_tagged_union) {
      expr->resolved_type = target;
    }
    return expr;
  }

  if (target->kind == KIND_PRIMITIVE &&
      target->data.primitive == PRIM_UNKNOWN) {
    expr->resolved_type = expr_type;
    return expr;
  }

  sema_error(s, expr, "Type mismatch: expected %s, got %s",
             type_to_name(target), type_to_name(expr_type));
  expr->resolved_type = expr_type;
  return expr;
}