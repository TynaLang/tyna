#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#include "sema_internal.h"
#include "tyl/ast.h"

static void check_literal_bounds(Sema *s, AstNode *node, Type *target) {
  if (node->tag != NODE_NUMBER || !target || target->kind != KIND_PRIMITIVE)
    return;

  double val = node->number.value;
  switch (target->data.primitive) {
  case PRIM_I8:
    if (val < SCHAR_MIN || val > SCHAR_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i8", val);
    break;

  case PRIM_U8:
    if (val < 0 || val > UCHAR_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u8", val);
    break;

  case PRIM_I16:
    if (val < SHRT_MIN || val > SHRT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i16", val);
    break;

  case PRIM_U16:
    if (val < 0 || val > USHRT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u16", val);
    break;

  case PRIM_I32:
    if (val < INT_MIN || val > INT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i32", val);
    break;

  case PRIM_U32:
    if (val < 0 || val > UINT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u32", val);
    break;

  case PRIM_I64:
    if (val < (double)LLONG_MIN || val > (double)LLONG_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i64", val);
    break;

  case PRIM_U64:
    if (val < 0 || val > (double)ULLONG_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u64", val);
    break;

  case PRIM_F32:
    if (fabs(val) > FLT_MAX && !isinf(val))
      sema_warning(s, node, "Literal %.2f out of range for f32", val);
    break;

  default:
    break;
  }
}

static Type *check_literal(Sema *s, AstNode *node) {
  switch (node->tag) {
  case NODE_NUMBER:
    if (sv_contains(node->number.raw_text, '.')) {
      return type_get_primitive(s->types,
                                sv_ends_with(node->number.raw_text, "f") ||
                                        sv_ends_with(node->number.raw_text, "F")
                                    ? PRIM_F32
                                    : PRIM_F64);
    } else {
      return type_get_primitive(s->types, PRIM_I32);
    }

  case NODE_CHAR:
    return type_get_primitive(s->types, PRIM_CHAR);

#ifndef TYNA_TEST_PRIMITIVES_ONLY
  case NODE_STRING:
    return type_get_primitive(s->types, PRIM_STRING);
#endif

  case NODE_BOOL:
    return type_get_primitive(s->types, PRIM_BOOL);

  default:
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

static Type *check_var(Sema *s, AstNode *node) {
  Symbol *sym = sema_resolve(s, node->var.value);
  if (!sym) {
    sema_error(s, node, "Undefined variable '" SV_FMT "'",
               SV_ARG(node->var.value));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
  return sym->type;
}

bool type_is_array_struct(Type *type) {
  if (type->kind != KIND_STRUCT)
    return false;
  if (sv_eq(type->name, sv_from_parts("Array", 5)))
    return true;
  if (type->data.instance.from_template &&
      sv_eq(type->data.instance.from_template->name, sv_from_parts("Array", 5)))
    return true;
  return false;
}

static Type *check_field(Sema *s, AstNode *node) {
  Type *obj_type = sema_check_expr(s, node->field.object);

  if (obj_type->kind != KIND_STRUCT) {
    sema_error(s, node->field.object,
               "Member access only allowed on structs, got %s",
               type_to_name(obj_type));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Member *m = type_get_member(obj_type, node->field.field);
  if (m) {
    return m->type;
  }

  sema_error(s, node, "Type %s has no member '" SV_FMT "'",
             type_to_name(obj_type), SV_ARG(node->field.field));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}

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
    // Support CONST and also variables with initializers that are constant
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

static Type *check_index(Sema *s, AstNode *node) {
  AstNode *array_node = node->index.array;
  AstNode *index_node = node->index.index;

  Type *array_type = sema_check_expr(s, array_node);
  Type *index_type = sema_check_expr(s, index_node);

  if (!type_is_array_struct(array_type)) {
    sema_error(s, array_node, "Indexing only allowed on arrays, got %s",
               type_to_name(array_type));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  if (!type_is_numeric(index_type)) {
    sema_error(s, index_node, "Array index must be numeric, got %s",
               type_to_name(index_type));
  }

  // Bounds checking
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

  // Array<T> has element type T as its first generic argument
  if (array_type->data.instance.generic_args.len > 0) {
    return array_type->data.instance.generic_args.items[0];
  }

  return type_get_primitive(s->types, PRIM_UNKNOWN);
}

static Type *check_unary(Sema *s, AstNode *node) {
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
    // Check for immutability (strings/constants)
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

static Type *check_binary_arith(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_arith.left);
  Type *right = sema_check_expr(s, node->binary_arith.right);

  // Pointer arithmetic: ptr<T> + numeric = ptr<T>
  if (left->kind == KIND_POINTER && type_is_numeric(right)) {
    return left;
  }
  // numeric + ptr<T> = ptr<T>
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

static Type *check_binary_logical(Sema *s, AstNode *node) {
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

static Type *check_call(Sema *s, AstNode *node) {
  Type *func_type = sema_check_expr(s, node->call.func);
  if (func_type->kind == KIND_PRIMITIVE &&
      func_type->data.primitive == PRIM_UNKNOWN) {
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  if (node->call.func->tag == NODE_VAR) {
    StringView name = node->call.func->var.value;
#ifdef TYNA_TEST_PRIMITIVES_ONLY
    if (sv_eq(name, sv_from_parts("free", 4))) {
      // Special case for free(): check it takes 1 array
      if (node->call.args.len != 1) {
        sema_error(s, node, "free() expects 1 argument, got %zu",
                   node->call.args.len);
      } else {
        Type *arg_ty = sema_check_expr(s, node->call.args.items[0]);
        if (!type_is_array_struct(arg_ty)) {
          sema_error(s, node->call.args.items[0],
                     "free() expects an array, got %s", type_to_name(arg_ty));
        }
      }
      return type_get_primitive(s->types, PRIM_VOID);
    }
#endif

    Symbol *symbol = sema_resolve(s, name);
    if (!symbol) {
      sema_error(s, node, "Call to undefined function '" SV_FMT "'",
                 SV_ARG(node->call.func->var.value));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    if (symbol->value && symbol->value->tag == NODE_FUNC_DECL) {
      AstNode *fn_decl = symbol->value;
      if (node->call.args.len != fn_decl->func_decl.params.len) {
        sema_error(s, node, "Function '" SV_FMT "' expects %zu args, got %zu",
                   SV_ARG(name), fn_decl->func_decl.params.len,
                   node->call.args.len);
      }
      for (size_t i = 0;
           i < node->call.args.len && i < fn_decl->func_decl.params.len; i++) {
        Type *arg_type = sema_check_expr(s, node->call.args.items[i]);
        AstNode *param_node = fn_decl->func_decl.params.items[i];
        Type *param_type = param_node->param.type;
        if (!type_can_implicitly_cast(param_type, arg_type)) {
          sema_error(s, node->call.args.items[i],
                     "Type mismatch: expected %s, got %s",
                     type_to_name(param_type), type_to_name(arg_type));
        }
        check_literal_bounds(s, node->call.args.items[i], param_type);
      }
    }

    return symbol->type;
  } else {
    // It was a field access or static member that resolved to a type.
    // E.g. s.to_array() or String::from_array().
    // For now, we assume these are blessed.
    return func_type;
  }
}

static Type *check_array_expr(Sema *s, AstNode *node) {
  Type *element_type = NULL;

  if (node->tag == NODE_ARRAY_LITERAL) {
    if (node->array_literal.items.len == 0) {
      sema_error(s, node, "Empty array literals are not yet supported");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    // Determine common element type (infer from first element or highest rank)
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      Type *item_type = sema_check_expr(s, item);

      if (element_type == NULL) {
        element_type = item_type;
      } else {
        if (type_rank(item_type) > type_rank(element_type)) {
          element_type = item_type;
        }
      }
    }

    // Coerce all elements to that type
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      if (!type_can_implicitly_cast(element_type, item->resolved_type)) {
        sema_error(s, item, "Array element type mismatch: expected %s, got %s",
                   type_to_name(element_type),
                   type_to_name(item->resolved_type));
      }
      check_literal_bounds(s, item, element_type);
    }
  } else if (node->tag == NODE_ARRAY_REPEAT) {
    // RECURSIVE CHECK: If node->array_repeat.value is another ARRAY_REPEAT,
    // sema_check_expr will call check_array_expr again, returning Array<T>.
    element_type = sema_check_expr(s, node->array_repeat.value);
    Type *count_type = sema_check_expr(s, node->array_repeat.count);
    if (!type_is_numeric(count_type)) {
      sema_error(s, node->array_repeat.count,
                 "Array repeat count must be numeric, got %s",
                 type_to_name(count_type));
    }
  }

  // Find the "Array" template
  Type *array_template = type_get_template(s->types, sv_from_parts("Array", 5));
  if (!array_template) {
    sema_error(
        s, node,
        "Internal Error: Generic 'Array' template not found in TypeContext");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  // Monomorphize: Create Array<element_type>
  List args;
  List_init(&args);
  List_push(&args, element_type);

  Type *instance = type_get_instance(s->types, array_template, args);
  List_free(&args, 0); // Cleanup temporary list

  // RECURSIVE CHECK: If this is an ARRAY_REPEAT, we must ensure we return
  // Array<T> where T is the result of the inner repeat. The parser generates
  // these nested for [value; c1, c2, c3]
  return instance;
}

static bool type_is_writable(Sema *s, AstNode *target) {
  if (!type_is_lvalue(target))
    return false;

  if (target->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, target->var.value);

    if (sym && sym->is_const)
      return false;
    return true;
  }

  if (target->tag == NODE_INDEX) {
    Type *arr_type = target->index.array->resolved_type;

    if (arr_type && arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING) {
      return false;
    }

    return true;
  }

  if (target->tag == NODE_FIELD) {
    return true;
  }

  return true;
}

static Type *check_assignment(Sema *s, AstNode *node) {
  AstNode *target = node->assign_expr.target;
  Type *lhs = sema_check_expr(s, target);

  if (!type_is_writable(s, target)) {
    if (!type_is_lvalue(target)) {
      sema_error(s, node, "Invalid assignment target");
    } else {
      sema_error(s, node, "Cannot assign to read-only l-value");
    }
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Type *rhs = sema_check_expr(s, node->assign_expr.value);
  if (!type_can_implicitly_cast(lhs, rhs)) {
    sema_error(s, node, "Type mismatch in assignment: expected %s, got %s",
               type_to_name(lhs), type_to_name(rhs));
  }

  return lhs;
}

static Type *check_cast(Sema *s, AstNode *node) {
  sema_check_expr(s, node->cast_expr.expr);
  return node->cast_expr.target_type;
}

static Type *check_ternary(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->ternary.condition);
  if (!type_is_bool(cond)) {
    sema_error(s, node->ternary.condition,
               "Ternary condition must be boolean, got %s", type_to_name(cond));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
  Type *t_expr = sema_check_expr(s, node->ternary.true_expr);
  Type *f_expr = sema_check_expr(s, node->ternary.false_expr);

  if (t_expr->kind == KIND_PRIMITIVE &&
      t_expr->data.primitive == PRIM_UNKNOWN) {
    node->ternary.true_expr->resolved_type = f_expr;
    return f_expr;
  }
  if (f_expr->kind == KIND_PRIMITIVE &&
      f_expr->data.primitive == PRIM_UNKNOWN) {
    node->ternary.false_expr->resolved_type = t_expr;
    return t_expr;
  }

  if (type_can_implicitly_cast(t_expr, f_expr)) {
    check_literal_bounds(s, node->ternary.true_expr, t_expr);
    check_literal_bounds(s, node->ternary.false_expr, t_expr);
    return t_expr;
  }
  if (type_can_implicitly_cast(f_expr, t_expr)) {
    check_literal_bounds(s, node->ternary.true_expr, f_expr);
    check_literal_bounds(s, node->ternary.false_expr, f_expr);
    return f_expr;
  }
  sema_error(s, node,
             "Ternary branches must have compatible types: got %s and %s",
             type_to_name(t_expr), type_to_name(f_expr));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}

Type *sema_coerce(Sema *s, AstNode *expr, Type *target) {
  Type *expr_type = sema_check_expr(s, expr);

  //  Literal Bounds Check
  if (expr->tag == NODE_NUMBER || expr->tag == NODE_CHAR) {
    check_literal_bounds(s, expr, target);

    if (type_can_implicitly_cast(target, expr_type)) {
      expr->resolved_type = target;
      return target;
    }
  }

  // Exact match or implicit cast
  if (type_can_implicitly_cast(target, expr_type)) {
    return target;
  }

  // Inference support
  if (target->kind == KIND_PRIMITIVE &&
      target->data.primitive == PRIM_UNKNOWN) {
    return expr_type;
  }

  sema_error(s, expr, "Type mismatch: expected %s, got %s",
             type_to_name(target), type_to_name(expr_type));
  return expr_type;
}

Type *sema_check_expr(Sema *s, AstNode *node) {
  if (!node)
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  if (node->resolved_type)
    return node->resolved_type;

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