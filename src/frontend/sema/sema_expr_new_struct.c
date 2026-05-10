#include "sema_internal.h"

ExprInfo sema_check_new_struct_expr(Sema *s, AstNode *node) {
  Type *target_type = node->new_expr.target_type;

  // For heap<T> types, the field initializers should be checked against the
  // inner type T, not against heap<T>.
  Type *fields_type = target_type;
  if (type_is_heap_type(target_type)) {
    if (target_type->data.instance.generic_args.len > 0) {
      fields_type = target_type->data.instance.generic_args.items[0];
    }
  }

  for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
    AstNode *assign = node->new_expr.field_inits.items[i];
    if (assign->tag != NODE_ASSIGN_EXPR ||
        assign->assign_expr.target->tag != NODE_VAR) {
      sema_error(s, assign, "Invalid struct field initializer");
      continue;
    }
    StringView field_name = assign->assign_expr.target->var.value;
    Member *member = type_get_member(fields_type, field_name);
    if (!member) {
      sema_error(s, assign, "Type '%s' has no field '%s'",
                 type_to_name(fields_type), field_name.data);
      continue;
    }
    assign->assign_expr.value =
        sema_coerce(s, assign->assign_expr.value, member->type);
  }

  // For heap<T> types, return the heap<T> type directly (not wrapped in ptr).
  // The codegen will allocate on the heap and return the heap<T> struct value.
  if (type_is_heap_type(target_type)) {
    return (ExprInfo){
        .type = target_type,
        .category = VAL_RVALUE,
    };
  }

  Type *ptr_type = type_get_pointer(s->types, target_type);
  return (ExprInfo){
      .type = ptr_type,
      .category = VAL_RVALUE,
  };
}
