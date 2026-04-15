#include "sema_internal.h"

Type *check_binary_arith(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_arith.left);
  Type *right = sema_check_expr(s, node->binary_arith.right);

  if (left->kind == KIND_POINTER && type_is_numeric(right)) {
    return left;
  }
  if (type_is_numeric(left) && right->kind == KIND_POINTER) {
    return right;
  }

  if (!type_is_numeric(left) || !type_is_numeric(right)) {
    sema_error(s, node,
               "Arithmetic operator on non-numeric type, got %s and %s",
               type_to_name(left), type_to_name(right));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Type *result_type = type_rank(left) >= type_rank(right) ? left : right;
  check_literal_bounds(s, node->binary_arith.left, result_type);
  check_literal_bounds(s, node->binary_arith.right, result_type);

  return result_type;
}

Type *check_binary_logical(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_logical.left);
  Type *right = sema_check_expr(s, node->binary_logical.right);

  if (node->tag == NODE_BINARY_LOGICAL) {
    if (!type_is_bool(left) || !type_is_bool(right)) {
      sema_error(s, node, "Logical operator requires boolean operands");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return type_get_primitive(s->types, PRIM_BOOL);
  } else if (node->tag == NODE_BINARY_COMPARE) {
    if (!type_is_numeric(left) || !type_is_numeric(right)) {
      sema_error(s, node,
                 "Comparison operator requires numeric operands, got %s and %s",
                 type_to_name(left), type_to_name(right));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return type_get_primitive(s->types, PRIM_BOOL);
  } else if (node->tag == NODE_BINARY_EQUALITY) {
    if (!type_can_implicitly_cast(left, right) &&
        !type_can_implicitly_cast(right, left)) {
      sema_error(s, node, "Equality operator on incompatible types: %s and %s",
                 type_to_name(left), type_to_name(right));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return type_get_primitive(s->types, PRIM_BOOL);
  } else {
    sema_error(s, node, "Unknown binary operator");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}
