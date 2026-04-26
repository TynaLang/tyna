#include "sema_internal.h"

static ExprInfo ExprInfo_rvalue(Type *type) {
  return (ExprInfo){.type = type, .category = VAL_RVALUE};
}

static ExprInfo expr_info_for_cached_node(Sema *s, AstNode *node, Type *type) {
  if (!node)
    return ExprInfo_rvalue(type);

  switch (node->tag) {
  case NODE_VAR:
    return (ExprInfo){.type = type, .category = VAL_LVALUE};

  case NODE_FIELD: {
    ExprInfo object_info = sema_check_expr_cache(s, node->field.object);
    Type *obj_type = object_info.type;
    while (obj_type && obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }
    if (obj_type) {
      Member *m = type_get_member(obj_type, node->field.field);
      if (m) {
        return (ExprInfo){.type = type, .category = VAL_LVALUE};
      }
    }
    return ExprInfo_rvalue(type);
  }

  case NODE_INDEX: {
    Type *arr_type = node->index.array->resolved_type;
    if (arr_type && !(arr_type->kind == KIND_PRIMITIVE &&
                      arr_type->data.primitive == PRIM_STRING)) {
      return (ExprInfo){.type = type, .category = VAL_LVALUE};
    }
    return ExprInfo_rvalue(type);
  }

  case NODE_UNARY:
    if (node->unary.op == OP_DEREF) {
      return (ExprInfo){.type = type, .category = VAL_LVALUE};
    }
    return ExprInfo_rvalue(type);

  default:
    return ExprInfo_rvalue(type);
  }
}

static void sema_mark_current_function_requires_arena(Sema *s, AstNode *node,
                                                      Type *type) {
  if (!s || !s->fn_node || !type)
    return;
  if (node->tag != NODE_CALL)
    return;
  if (!sema_fn_decl_can_use_arena(s->fn_node))
    return;
  if (type->kind != KIND_PRIMITIVE || type->data.primitive != PRIM_STRING) {
    return;
  }
  s->scope->has_computed_str = true;
}

ExprInfo sema_check_expr_cache(Sema *s, AstNode *node) {
  if (!node)
    return ExprInfo_rvalue(type_get_primitive(s->types, PRIM_UNKNOWN));
  if (node->resolved_type) {
    return expr_info_for_cached_node(s, node, node->resolved_type);
  }

  ExprInfo info = ExprInfo_rvalue(type_get_primitive(s->types, PRIM_UNKNOWN));

  switch (node->tag) {
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_STRING:
  case NODE_BOOL:
  case NODE_NULL:
    info = sema_check_literal(s, node);
    break;

  case NODE_VAR:
    info = sema_check_var(s, node);
    break;
  case NODE_FIELD:
    info = sema_check_field(s, node);
    break;
  case NODE_INDEX:
    info = sema_check_index(s, node);
    break;

  case NODE_STATIC_MEMBER:
    info = sema_check_static_member(s, node);
    break;

  case NODE_UNARY:
    info = sema_check_unary(s, node);
    break;
  case NODE_BINARY_ARITH:
    info = sema_check_binary_arith(s, node);
    break;
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    info = sema_check_binary_logical(s, node);
    break;
  case NODE_BINARY_IS:
    info = sema_check_binary_is(s, node);
    break;
  case NODE_BINARY_ELSE:
    info = sema_check_binary_else(s, node);
    break;

  case NODE_BLOCK:
    sema_check_stmt(s, node);
    info = (ExprInfo){.type = node->resolved_type
                                  ? node->resolved_type
                                  : type_get_primitive(s->types, PRIM_VOID),
                      .category = VAL_RVALUE};
    break;

  case NODE_CALL:
    info = sema_check_call(s, node);
    break;
  case NODE_NEW_EXPR:
    info = sema_check_new_expr(s, node);
    break;
  case NODE_ARRAY_LITERAL:
    info = sema_check_array_expr(s, node);
    break;
  case NODE_ARRAY_REPEAT:
    info = sema_check_array_expr(s, node);
    break;
  case NODE_ASSIGN_EXPR:
    info = sema_check_assignment(s, node);
    break;
  case NODE_CAST_EXPR:
    info = sema_check_cast(s, node);
    break;
  case NODE_SIZEOF_EXPR: {
    Type *target = node->sizeof_expr.target_type;
    if (!target) {
      sema_error(s, node, "sizeof requires a valid type");
      info = ExprInfo_rvalue(type_get_primitive(s->types, PRIM_UNKNOWN));
      break;
    }
    info = ExprInfo_rvalue(type_get_primitive(s->types, PRIM_I64));
    break;
  }
  case NODE_TERNARY:
    info = sema_check_ternary(s, node);
    break;

  default:
    sema_error(s, node, "Unhandled node type in expression checking");
    break;
  }

  node->resolved_type = info.type;
  sema_mark_current_function_requires_arena(s, node, info.type);
  return info;
}