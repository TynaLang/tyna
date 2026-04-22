#include "sema_internal.h"

static bool get_constant_int(Sema *s, AstNode *node, long long *out) {
  if (!node)
    return false;

  if (node->tag == NODE_NUMBER) {
    *out = (long long)node->number.value;
    return true;
  }

  if (node->tag == NODE_UNARY && node->unary.op == OP_NEG) {
    long long val;
    if (get_constant_int(s, node->unary.expr, &val)) {
      *out = -val;
      return true;
    }
  }

  if (node->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, node->var.value);
    if (sym && sym->value) {
      return get_constant_int(s, sym->value, out);
    }
  }

  if (node->tag == NODE_VAR_DECL) {
    return get_constant_int(s, node->var_decl.value, out);
  }

  return false;
}

static bool get_array_size(Sema *s, AstNode *node, long long *out) {
  if (!node)
    return false;

  if (node->tag == NODE_ARRAY_LITERAL) {
    *out = (long long)node->array_literal.items.len;
    return true;
  }

  if (node->tag == NODE_ARRAY_REPEAT) {
    return get_constant_int(s, node->array_repeat.count, out);
  }

  if (node->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, node->var.value);
    if (sym && sym->value) {
      return get_array_size(s, sym->value, out);
    }
  }

  if (node->tag == NODE_VAR_DECL) {
    return get_array_size(s, node->var_decl.value, out);
  }

  return false;
}

ExprInfo sema_check_index(Sema *s, AstNode *node) {
  AstNode *array_node = node->index.array;
  AstNode *index_node = node->index.index;

  Type *array_type = sema_check_expr(s, array_node).type;
  Type *index_type = sema_check_expr(s, index_node).type;

  if (array_type->kind == KIND_POINTER) {
    if (!type_is_numeric(index_type)) {
      sema_error(s, index_node, "Array index must be numeric, got %s",
                 type_to_name(index_type));
    }
    return (ExprInfo){
        .type = array_type->data.pointer_to,
        .category = VAL_LVALUE,
    };
  }

  if (!type_is_array_struct(array_type)) {
    sema_error(s, array_node, "Indexing only allowed on arrays, got %s",
               type_to_name(array_type));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  if (!type_is_numeric(index_type)) {
    sema_error(s, index_node, "Array index must be numeric, got %s",
               type_to_name(index_type));
  }

  long long index_val;
  if (get_constant_int(s, index_node, &index_val)) {
    if (index_val < 0) {
      sema_error(s, index_node, "Negative array index: %lld", index_val);
    }

    long long array_size;
    if (get_array_size(s, array_node, &array_size)) {
      if (index_val >= array_size) {
        sema_error(s, index_node,
                   "Array index out of bounds: index %lld, size %lld",
                   index_val, array_size);
      }
    }
  }

  Type *element_type = NULL;
  if (array_type->data.instance.generic_args.len > 0) {
    element_type = array_type->data.instance.generic_args.items[0];
    return (ExprInfo){.type = element_type, .category = VAL_LVALUE};
  }

  return (ExprInfo){
      .type = type_get_primitive(s->types, PRIM_UNKNOWN),
      .category = VAL_RVALUE,
  };
}