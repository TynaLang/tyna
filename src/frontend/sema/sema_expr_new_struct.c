#include "sema_internal.h"

ExprInfo sema_check_new_struct_expr(Sema *s, AstNode *node) {
  Type *target_type = node->new_expr.target_type;

  for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
    AstNode *assign = node->new_expr.field_inits.items[i];
    if (assign->tag != NODE_ASSIGN_EXPR ||
        assign->assign_expr.target->tag != NODE_VAR) {
      sema_error(s, assign, "Invalid struct field initializer");
      continue;
    }
    StringView field_name = assign->assign_expr.target->var.value;
    Member *member = type_get_member(target_type, field_name);
    if (!member) {
      sema_error(s, assign, "Type '%s' has no field '%s'",
                 type_to_name(target_type), field_name.data);
      continue;
    }
    assign->assign_expr.value =
        sema_coerce(s, assign->assign_expr.value, member->type);
  }

  return (ExprInfo){
      .type = type_get_pointer(s->types, target_type),
      .category = VAL_RVALUE,
  };
}