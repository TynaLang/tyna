#include "sema_internal.h"

ExprInfo sema_check_unary(Sema *s, AstNode *node) {
  ExprInfo operand_info = sema_check_expr(s, node->unary.expr);
  Type *operand_type = operand_info.type;

  switch (node->unary.op) {
  case OP_ADDR_OF: {
    if (operand_info.category != VAL_LVALUE) {
      sema_error(s, node, "Cannot take address of non-lvalue");
    }
    sema_mark_node_requires_storage(s, node->unary.expr);

    if (operand_type->kind == KIND_POINTER) {
      // If the operand is already a pointer (e.g. a local variable which is
      // represented as ptr<T> in the sema), taking its address should return
      // ref<T> (a reference to the pointed-to type), not ptr<ptr<T>>.
      Type *inner = operand_type->data.pointer_to;
      Type *ref_template = type_get_template(s->types, sv_from_parts("ref", 3));
      if (ref_template) {
        List args;
        List_init(&args);
        List_push(&args, inner);
        Type *result_type = type_get_instance(s->types, ref_template, args);
        List_free(&args, 0);
        return (ExprInfo){.type = result_type, .category = VAL_RVALUE};
      }
      return (ExprInfo){
          .type = operand_type,
          .category = VAL_RVALUE,
      };
    }

    if (type_is_heap_or_ref(operand_type)) {
      Type *ref_target = NULL;
      Member *value_member =
          type_get_member(operand_type, sv_from_parts("value", 5));
      if (value_member && value_member->type &&
          value_member->type->kind == KIND_POINTER) {
        ref_target = value_member->type->data.pointer_to;
      } else {
        ref_target = operand_type;
      }

      Type *ref_template = type_get_template(s->types, sv_from_parts("ref", 3));
      if (!ref_template) {
        sema_error(s, node, "Internal error: 'ref' type unavailable");
        return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                          .category = VAL_RVALUE};
      }
      List args;
      List_init(&args);
      List_push(&args, ref_target);
      Type *result_type = type_get_instance(s->types, ref_template, args);
      List_free(&args, 0);
      return (ExprInfo){.type = result_type, .category = VAL_RVALUE};
    }

    Type *ref_template = type_get_template(s->types, sv_from_parts("ref", 3));
    if (!ref_template) {
      sema_error(s, node, "Internal error: 'ref' type unavailable");
      return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                        .category = VAL_RVALUE};
    }
    List args;
    List_init(&args);
    List_push(&args, operand_type);
    Type *result_type = type_get_instance(s->types, ref_template, args);
    List_free(&args, 0);
    return (ExprInfo){.type = result_type, .category = VAL_RVALUE};
  }

  case OP_DEREF: {
    if (operand_type->kind == KIND_POINTER) {
      return (ExprInfo){.type = operand_type->data.pointer_to,
                        .category = VAL_LVALUE};
    }

    if (type_is_heap_or_ref(operand_type)) {
      Member *value_member =
          type_get_member(operand_type, sv_from_parts("value", 5));
      if (value_member && value_member->type &&
          value_member->type->kind == KIND_POINTER) {
        return (ExprInfo){.type = value_member->type->data.pointer_to,
                          .category = VAL_LVALUE};
      }
    }

    sema_error(s, node, "Cannot dereference non-pointer type: %s",
               type_to_name(operand_type));
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE};
  }

  case OP_NEG: {
    if (!type_is_numeric(operand_type)) {
      sema_error(s, node, "Unary '-' operator requires numeric operand, got %s",
                 type_to_name(operand_type));
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }
    return (ExprInfo){.type = operand_type, .category = VAL_RVALUE};
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
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }
    return (ExprInfo){.type = operand_type, .category = VAL_RVALUE};
  }

  default:
    sema_error(s, node, "Unknown unary operator");
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }
}
