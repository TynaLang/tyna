#include "sema_internal.h"

ExprInfo sema_check_binary_is(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_is.left).type;
  Type *right = sema_check_expr(s, node->binary_is.right).type;
  Type *result = type_get_primitive(s->types, PRIM_BOOL);

  if (!left || !right) {
    sema_error(s, node, "Invalid operands for 'is' expression");
    return (ExprInfo){.type = result, .category = VAL_RVALUE};
  }

  bool right_is_error_directive = right && right->kind == KIND_ERROR_SET &&
                                  sv_eq(right->name, sv_from_cstr("Error"));

  if (left->kind == KIND_RESULT) {
    if (!left->data.result.error_set) {
      sema_error(s, node,
                 "Result type '%s' is missing an error set for 'is' checks",
                 type_to_name(left));
      return (ExprInfo){.type = result, .category = VAL_RVALUE};
    }
    if (right_is_error_directive) {
      return (ExprInfo){.type = result, .category = VAL_RVALUE};
    }
    if (right->kind == KIND_ERROR) {
      if (!error_set_contains(left->data.result.error_set, right)) {
        sema_error(s, node, "Type '%s' is not in error set '%s'",
                   type_to_name(right),
                   type_to_name(left->data.result.error_set));
      }
    } else if (right->kind == KIND_ERROR_SET) {
      if (!type_equals(left->data.result.error_set, right)) {
        sema_error(s, node, "Cannot compare result error set '%s' with '%s'",
                   type_to_name(left->data.result.error_set),
                   type_to_name(right));
      }
    } else {
      sema_error(s, node,
                 "Cannot compare result type '%s' with '%s' using 'is'",
                 type_to_name(left), type_to_name(right));
    }
  } else if (left->kind == KIND_ERROR) {
    if (right_is_error_directive) {
      return (ExprInfo){.type = result, .category = VAL_RVALUE};
    }
    if (right->kind == KIND_ERROR) {
      if (!type_equals(left, right)) {
        /* Different error types still produce a valid boolean expression. */
      }
    } else if (right->kind == KIND_ERROR_SET) {
      if (!error_set_contains(right, left)) {
        sema_error(s, node, "Type '%s' is not in error set '%s'",
                   type_to_name(left), type_to_name(right));
      }
    } else {
      sema_error(s, node, "Cannot compare error type '%s' with '%s' using 'is'",
                 type_to_name(left), type_to_name(right));
    }
  } else {
    sema_error(s, node,
               "The 'is' operator requires a result or error type on the left, "
               "got '%s'",
               type_to_name(left));
  }

  return (ExprInfo){.type = result, .category = VAL_RVALUE};
}

ExprInfo sema_check_binary_else(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_else.left).type;
  Type *right = sema_check_expr(s, node->binary_else.right).type;

  if (!left || !right) {
    sema_error(s, node, "Invalid operands for 'else' expression");
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE};
  }

  if (left->kind == KIND_RESULT) {
    if (type_can_implicitly_cast(left->data.result.success, right)) {
      return (ExprInfo){.type = left->data.result.success,
                        .category = VAL_RVALUE};
    }
    sema_error(s, node,
               "Else expression type '%s' not compatible with result success "
               "type '%s'",
               type_to_name(right), type_to_name(left->data.result.success));
    return (ExprInfo){.type = left->data.result.success,
                      .category = VAL_RVALUE};
  }

  sema_error(s, node,
             "The 'else' operator requires a result type on the left, got '%s'",
             type_to_name(left));
  return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                    .category = VAL_RVALUE};
}