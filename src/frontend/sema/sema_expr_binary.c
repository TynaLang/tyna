#include "sema_internal.h"

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

ExprInfo sema_check_binary_arith(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_arith.left).type;
  Type *right = sema_check_expr(s, node->binary_arith.right).type;

  if (left->kind == KIND_POINTER && type_is_numeric(right)) {
    return (ExprInfo){.type = left, .category = VAL_RVALUE};
  }
  if (type_is_numeric(left) && right->kind == KIND_POINTER) {
    return (ExprInfo){.type = right, .category = VAL_RVALUE};
  }

  if (!type_is_numeric(left) || !type_is_numeric(right)) {
    sema_error(s, node,
               "Arithmetic operator on non-numeric type, got %s and %s",
               type_to_name(left), type_to_name(right));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  Type *result_type = type_rank(left) >= type_rank(right) ? left : right;
  sema_check_literal_bounds(s, node->binary_arith.left, result_type);
  sema_check_literal_bounds(s, node->binary_arith.right, result_type);

  return (ExprInfo){.type = result_type, .category = VAL_RVALUE};
}

ExprInfo sema_check_binary_logical(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_logical.left).type;
  Type *right = sema_check_expr(s, node->binary_logical.right).type;

  if (node->tag == NODE_BINARY_EQUALITY) {
    Type *str_type = type_get_string_buffer(s->types);
    bool left_is_string = left && left->kind == KIND_PRIMITIVE &&
                          left->data.primitive == PRIM_STRING;
    bool right_is_string = right && right->kind == KIND_PRIMITIVE &&
                           right->data.primitive == PRIM_STRING;
    bool left_is_str = left && left->kind == KIND_STRING_BUFFER;
    bool right_is_str = right && right->kind == KIND_STRING_BUFFER;

    if ((left_is_string && right_is_str) || (right_is_string && left_is_str)) {
      if (left_is_string) {
        node->binary_logical.left =
            AstNode_new_cast_expr(node->binary_logical.left, str_type,
                                  node->binary_logical.left->loc);
        left = sema_check_expr(s, node->binary_logical.left).type;
      } else if (!left_is_str) {
        node->binary_logical.left =
            AstNode_new_cast_expr(node->binary_logical.left, str_type,
                                  node->binary_logical.left->loc);
        left = sema_check_expr(s, node->binary_logical.left).type;
      }

      if (right_is_string) {
        node->binary_logical.right =
            AstNode_new_cast_expr(node->binary_logical.right, str_type,
                                  node->binary_logical.right->loc);
        right = sema_check_expr(s, node->binary_logical.right).type;
      } else if (!right_is_str) {
        node->binary_logical.right =
            AstNode_new_cast_expr(node->binary_logical.right, str_type,
                                  node->binary_logical.right->loc);
        right = sema_check_expr(s, node->binary_logical.right).type;
      }
    }
  }

  Type *result = type_get_primitive(s->types, PRIM_BOOL);
  if (node->tag == NODE_BINARY_LOGICAL) {
    if (!type_is_bool(left) || !type_is_bool(right)) {
      sema_error(s, node, "Logical operator requires boolean operands");
      result = type_get_primitive(s->types, PRIM_UNKNOWN);
    }
  } else if (node->tag == NODE_BINARY_COMPARE) {
    if (!type_is_numeric(left) || !type_is_numeric(right)) {
      sema_error(s, node,
                 "Comparison operator requires numeric operands, got %s and %s",
                 type_to_name(left), type_to_name(right));
      result = type_get_primitive(s->types, PRIM_UNKNOWN);
    }
  } else if (node->tag == NODE_BINARY_EQUALITY) {
    if (!type_can_implicitly_cast(left, right) &&
        !type_can_implicitly_cast(right, left)) {
      sema_error(s, node, "Equality operator on incompatible types: %s and %s",
                 type_to_name(left), type_to_name(right));
      result = type_get_primitive(s->types, PRIM_UNKNOWN);
    }
  } else {
    sema_error(s, node, "Unknown binary operator");
    result = type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  return (ExprInfo){.type = result, .category = VAL_RVALUE};
}

ExprInfo sema_check_binary_is(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_is.left).type;
  Type *right = sema_check_expr(s, node->binary_is.right).type;
  Type *result = type_get_primitive(s->types, PRIM_BOOL);

  bool right_is_error_directive = right && right->kind == KIND_ERROR_SET &&
                                  sv_eq(right->name, sv_from_cstr("Error"));

  if (left->kind == KIND_RESULT) {
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
