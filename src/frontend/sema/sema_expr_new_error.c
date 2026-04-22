#include "sema_internal.h"

ExprInfo sema_check_new_error_expr(Sema *s, AstNode *node) {
  Type *target_type = node->new_expr.target_type;

  if (node->new_expr.args.len > 0 && node->new_expr.field_inits.len > 0) {
    sema_error(s, node,
               "Cannot mix constructor arguments and error literal fields");
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE};
  }

  for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
    AstNode *assign = node->new_expr.field_inits.items[i];
    if (assign->tag != NODE_ASSIGN_EXPR ||
        assign->assign_expr.target->tag != NODE_VAR) {
      sema_error(s, assign, "Invalid error field initializer");
      continue;
    }
    StringView field_name = assign->assign_expr.target->var.value;
    Member *member = type_get_member(target_type, field_name);
    if (!member) {
      sema_error(s, assign, "Error type '%s' has no field '%s'",
                 type_to_name(target_type), field_name.data);
      continue;
    }
    assign->assign_expr.value =
        sema_coerce(s, assign->assign_expr.value, member->type);
  }

  if (s->fn_node && s->ret_type && s->ret_type->kind == KIND_RESULT &&
      s->ret_type->data.result.error_set &&
      sv_eq(s->ret_type->data.result.error_set->name,
            sv_from_parts("Error", 5))) {
    Type *error_set = s->ret_type->data.result.error_set;
    if (!error_set_contains(error_set, target_type)) {
      type_add_member(error_set, NULL, target_type, 0);
    }
  }

  return (ExprInfo){.type = target_type, .category = VAL_RVALUE};
}