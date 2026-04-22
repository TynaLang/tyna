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

ExprInfo sema_check_array_expr(Sema *s, AstNode *node) {
  Type *element_type = NULL;

  if (node->tag == NODE_ARRAY_LITERAL) {
    if (node->array_literal.items.len == 0) {
      sema_error(s, node, "Empty array literals are not yet supported");
      return (ExprInfo){
          .type = type_get_primitive(s->types, PRIM_UNKNOWN),
          .category = VAL_RVALUE,
      };
    }

    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      Type *item_type = sema_check_expr(s, item).type;

      if (element_type == NULL) {
        element_type = item_type;
      } else if (type_rank(item_type) > type_rank(element_type)) {
        element_type = item_type;
      }
    }

    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      if (!type_can_implicitly_cast(element_type, item->resolved_type)) {
        sema_error(s, item, "Array element type mismatch: expected %s, got %s",
                   type_to_name(element_type),
                   type_to_name(item->resolved_type));
      }
      sema_check_literal_bounds(s, item, element_type);
    }
  } else if (node->tag == NODE_ARRAY_REPEAT) {
    element_type = sema_check_expr(s, node->array_repeat.value).type;
    Type *count_type = sema_check_expr(s, node->array_repeat.count).type;
    if (!type_is_numeric(count_type)) {
      sema_error(s, node->array_repeat.count,
                 "Array repeat count must be numeric, got %s",
                 type_to_name(count_type));
    }
  }

  Type *array_template = type_get_template(s->types, sv_from_parts("Array", 5));
  if (!array_template) {
    sema_error(
        s, node,
        "Internal Error: Generic 'Array' template not found in TypeContext");
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  List args;
  List_init(&args);
  List_push(&args, element_type);

  Type *instance = NULL;
  if (node->tag == NODE_ARRAY_LITERAL) {
    size_t len = node->array_literal.items.len;
    instance = type_get_instance_fixed(s->types, array_template, args, len);
  } else if (node->tag == NODE_ARRAY_REPEAT) {
    long long repeat_count = 0;
    if (get_constant_int(s, node->array_repeat.count, &repeat_count) &&
        repeat_count > 0) {
      instance = type_get_instance_fixed(s->types, array_template, args,
                                         (uint64_t)repeat_count);
    } else {
      instance = type_get_instance(s->types, array_template, args);
    }
  } else {
    instance = type_get_instance(s->types, array_template, args);
  }
  List_free(&args, 0);

  if (node) {
    node->resolved_type = instance;
  }

  return (ExprInfo){.type = instance, .category = VAL_RVALUE};
}
