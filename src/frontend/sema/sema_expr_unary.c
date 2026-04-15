#include "sema_internal.h"

Type *check_unary(Sema *s, AstNode *node) {
  Type *operand_type = sema_check_expr(s, node->unary.expr);

  switch (node->unary.op) {
  case OP_ADDR_OF:
    if (!type_is_lvalue(node->unary.expr)) {
      sema_error(s, node, "Cannot take address of non-lvalue");
    }
    return type_get_pointer(s->types, operand_type);

  case OP_DEREF:
    if (operand_type->kind != KIND_POINTER) {
      sema_error(s, node, "Cannot dereference non-pointer type: %s",
                 type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return operand_type->data.pointer_to;

  case OP_NEG: {
    if (!type_is_numeric(operand_type)) {
      sema_error(s, node, "Unary '-' operator requires numeric operand, got %s",
                 type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return operand_type;
  }

  case OP_PRE_INC:
  case OP_POST_INC:
  case OP_PRE_DEC:
  case OP_POST_DEC: {
    if (!type_is_lvalue(node->unary.expr)) {
      sema_error(s, node, "Increment/decrement operator requires an l-value");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    if (!type_is_numeric(operand_type)) {
      sema_error(
          s, node,
          "Increment/decrement operator requires numeric operand, got %s",
          type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    if (node->unary.expr->tag == NODE_VAR) {
      Symbol *sym = sema_resolve(s, node->unary.expr->var.value);
      if (sym && sym->is_const) {
        sema_error(s, node, "Cannot increment/decrement constant '" SV_FMT "'",
                   SV_ARG(sym->name));
      }
    } else if (node->unary.expr->tag == NODE_INDEX) {
#ifdef TYNA_TEST_PRIMITIVES_ONLY
      Type *arr_type = node->unary.expr->index.array->resolved_type;
      if (arr_type && arr_type->kind == KIND_PRIMITIVE &&
          arr_type->data.primitive == PRIM_STRING) {
        sema_error(s, node, "Strings in tyl are immutable");
      }
#endif
    }
    return operand_type;
  }

  case OP_NOT: {
    if (!type_is_bool(operand_type)) {
      sema_error(s, node,
                 "Logical '!' operator requires boolean operand, got %s",
                 type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return operand_type;
  }

  default:
    sema_error(s, node, "Unknown unary operator");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}
