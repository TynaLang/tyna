#include "sema_internal.h"

static AstNode *sema_find_underlying_var(AstNode *node) {
  if (!node)
    return NULL;
  if (node->tag == NODE_VAR)
    return node;
  if (node->tag == NODE_UNARY && node->unary.op == OP_ADDR_OF)
    return sema_find_underlying_var(node->unary.expr);
  return NULL;
}

static void sema_mark_symbol_requires_storage(Symbol *sym) {
  if (!sym || sym->kind != SYM_VAR)
    return;
  sym->requires_storage = 1;
  if (sym->value && sym->value->tag == NODE_PARAM) {
    sym->value->param.requires_storage = true;
  }
}

static void sema_mark_node_requires_storage(Sema *s, AstNode *node) {
  AstNode *var_node = sema_find_underlying_var(node);
  if (!var_node)
    return;
  Symbol *sym = sema_resolve(s, var_node->var.value);
  sema_mark_symbol_requires_storage(sym);
}

ExprInfo check_unary(Sema *s, AstNode *node) {
  ExprInfo operand_info = sema_check_expr(s, node->unary.expr);
  Type *operand_type = operand_info.type;

  switch (node->unary.op) {
  case OP_ADDR_OF:
    if (operand_info.category != VAL_LVALUE) {
      sema_error(s, node, "Cannot take address of non-lvalue");
    }
    sema_mark_node_requires_storage(s, node->unary.expr);
    return (ExprInfo){.type = type_get_pointer(s->types, operand_type),
                      .category = VAL_RVALUE,
                      };

  case OP_DEREF:
    if (operand_type->kind != KIND_POINTER) {
      sema_error(s, node, "Cannot dereference non-pointer type: %s",
                 type_to_name(operand_type));
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE};
    }
    return (ExprInfo){.type = operand_type->data.pointer_to,
                      .category = VAL_LVALUE};

  case OP_NEG: {
    if (!type_is_numeric(operand_type)) {
      sema_error(s, node, "Unary '-' operator requires numeric operand, got %s",
                 type_to_name(operand_type));
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE,
                        };
    }
    return (ExprInfo){
        .type = operand_type, .category = VAL_RVALUE};
  }

  case OP_PRE_INC:
  case OP_POST_INC:
  case OP_PRE_DEC:
  case OP_POST_DEC: {
    if (operand_info.category != VAL_LVALUE) {
      sema_error(s, node, "Increment/decrement operator requires an l-value");
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE};
    }
    if (!type_is_numeric(operand_type)) {
      sema_error(
          s, node,
          "Increment/decrement operator requires numeric operand, got %s",
          type_to_name(operand_type));
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE};
    }
    if (!sema_is_writable_address(s, node->unary.expr)) {
      sema_error(s, node, "Cannot increment/decrement read-only l-value");
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE};
    }
    return (ExprInfo){.type = operand_type, .category = VAL_RVALUE};
  }

  case OP_NOT: {
    if (!type_is_bool(operand_type)) {
      sema_error(s, node,
                 "Logical '!' operator requires boolean operand, got %s",
                 type_to_name(operand_type));
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE,
                        };
    }
    return (ExprInfo){
        .type = operand_type, .category = VAL_RVALUE};
  }

  default:
    sema_error(s, node, "Unknown unary operator");
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE,
                      };
  }
}
