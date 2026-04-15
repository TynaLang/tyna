#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "sema_internal.h"
#include "tyl/ast.h"

Type *sema_find_type_by_name(Sema *s, StringView name) {
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
    if (sv_eq_cstr(name, prim_name) ||
        (i == PRIM_STRING && sv_eq_cstr(name, "String"))) {
      return t;
    }
  }

  return NULL;
}

Type *sema_coerce(Sema *s, AstNode *expr, Type *target) {
  Type *expr_type = sema_check_expr(s, expr);
  bool target_is_tagged_union =
      target && target->kind == KIND_UNION && target->is_tagged_union;

  // Literal Bounds Check
  if (expr->tag == NODE_NUMBER || expr->tag == NODE_CHAR) {
    check_literal_bounds(s, expr, target);

    if (type_can_implicitly_cast(target, expr_type) ||
        (expr->tag == NODE_NUMBER && literal_fits_in_type(expr, target))) {
      if (!target_is_tagged_union) {
        expr->resolved_type = target;
      }
      return target;
    }
  }

  // Exact match or implicit cast
  if (type_can_implicitly_cast(target, expr_type)) {
    if (!target_is_tagged_union) {
      expr->resolved_type = target;
    }
    return target;
  }

  // Inference support
  if (target->kind == KIND_PRIMITIVE &&
      target->data.primitive == PRIM_UNKNOWN) {
    expr->resolved_type = expr_type;
    return expr_type;
  }

  sema_error(s, expr, "Type mismatch: expected %s, got %s",
             type_to_name(target), type_to_name(expr_type));
  expr->resolved_type = expr_type;
  return expr_type;
}

Type *sema_check_expr(Sema *s, AstNode *node) {
  if (!node)
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  if (node->resolved_type) {
    return node->resolved_type;
  }

  Type *type = type_get_primitive(s->types, PRIM_UNKNOWN);

  switch (node->tag) {
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_STRING:
  case NODE_BOOL:
    type = check_literal(s, node);
    break;

  case NODE_VAR:
    type = check_var(s, node);
    break;
  case NODE_FIELD:
    type = check_field(s, node);
    break;
  case NODE_INDEX:
    type = check_index(s, node);
    break;

  case NODE_STATIC_MEMBER:
    type = check_static_member(s, node);
    break;

  case NODE_UNARY:
    type = check_unary(s, node);
    break;
  case NODE_BINARY_ARITH:
    type = check_binary_arith(s, node);
    break;
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    type = check_binary_logical(s, node);
    break;

  case NODE_CALL:
    type = check_call(s, node);
    break;
  case NODE_ARRAY_LITERAL:
    type = check_array_expr(s, node);
    break;
  case NODE_ARRAY_REPEAT:
    type = check_array_expr(s, node);
    break;
  case NODE_ASSIGN_EXPR:
    type = check_assignment(s, node);
    break;
  case NODE_CAST_EXPR:
    type = check_cast(s, node);
    break;
  case NODE_TERNARY:
    type = check_ternary(s, node);
    break;

  default:
    sema_error(s, node, "Unhandled node type in expression checking");
    break;
  }

  node->resolved_type = type;
  return type;
}