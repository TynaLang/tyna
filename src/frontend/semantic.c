#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tyl/ast.h"
#include "tyl/semantic.h"

const char *type_to_name(Type *t) {
  if (!t)
    return "null";
  if (t->kind == KIND_PRIMITIVE) {
    switch (t->data.primitive) {
    case PRIM_I8:
      return "i8";
    case PRIM_I16:
      return "i16";
    case PRIM_I32:
      return "i32";
    case PRIM_I64:
      return "i64";
    case PRIM_U8:
      return "u8";
    case PRIM_U16:
      return "u16";
    case PRIM_U32:
      return "u32";
    case PRIM_U64:
      return "u64";
    case PRIM_F32:
      return "f32";
    case PRIM_F64:
      return "f64";
    case PRIM_CHAR:
      return "char";
    case PRIM_BOOL:
      return "bool";
    case PRIM_VOID:
      return "void";
    case PRIM_STRING:
      return "string";
    case PRIM_UNKNOWN:
      return "unknown";
    }
  } else if (t->kind == KIND_ARRAY) {
    static char buf[256];
    if (t->data.array.is_dynamic) {
      snprintf(buf, sizeof(buf), "[%s]", type_to_name(t->data.array.element));
    } else {
      snprintf(buf, sizeof(buf), "[%s; %zu]",
               type_to_name(t->data.array.element), t->data.array.fixed_size);
    }
    return buf;
  }
  return "unknown";
}

static void semantic_error(SymbolTable *t, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(t->eh, n->loc, "%s", msg_buf);
}

static void semantic_warning(SymbolTable *t, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report_level(t->eh, n->loc, LEVEL_WARNING, "%s", msg_buf);
}

static int is_lvalue(AstNode *node) {
  return node->tag == NODE_VAR || node->tag == NODE_FIELD ||
         node->tag == NODE_INDEX;
}

static int type_rank(Type *t) {
  if (!t || t->kind != KIND_PRIMITIVE)
    return 0;
  switch (t->data.primitive) {
  case PRIM_I8:
  case PRIM_U8:
  case PRIM_CHAR:
    return 1;
  case PRIM_I16:
  case PRIM_U16:
    return 2;
  case PRIM_I32:
  case PRIM_U32:
    return 3;
  case PRIM_I64:
  case PRIM_U64:
    return 4;
  case PRIM_F32:
    return 5;
  case PRIM_F64:
    return 6;
  default:
    return 0;
  }
}

int is_numeric(Type *t) { return t && type_rank(t) > 0; }
int is_bool(Type *t) {
  return t && t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_BOOL;
}

static int can_implicitly_cast(Type *to, Type *from) {
  if (!to || !from)
    return 0;
  if (to == from)
    return 1;
  if (to->kind == KIND_PRIMITIVE && to->data.primitive == PRIM_VOID)
    return 0;
  if (from->kind == KIND_PRIMITIVE && from->data.primitive == PRIM_VOID)
    return 0;
  if (is_bool(to) || is_bool(from))
    return 0;
  if (is_numeric(to) && is_numeric(from))
    return type_rank(to) >= type_rank(from);

  // Allow implicit cast from String to [char]
  if (from->kind == KIND_PRIMITIVE && from->data.primitive == PRIM_STRING &&
      to->kind == KIND_ARRAY &&
      to->data.array.element->kind == KIND_PRIMITIVE &&
      to->data.array.element->data.primitive == PRIM_CHAR) {
    return 1;
  }

  return 0;
}

int signature_matches(AstNode *dec, AstNode *imp) {
  if (dec->tag != NODE_FUNC_DECL || imp->tag != NODE_FUNC_DECL) {
    panic("Expected function declaration nodes in signature_matches");
    return 0;
  }

  if (dec->func_decl.return_type != imp->func_decl.return_type)
    return 0;
  if (dec->func_decl.params.len != imp->func_decl.params.len)
    return 0;

  List decl_params = dec->func_decl.params;
  List imp_params = imp->func_decl.params;
  for (int i = 0; i < decl_params.len; i++) {
    AstNode *p1 = decl_params.items[i];
    AstNode *p2 = imp_params.items[i];
    if (p1->param.type != p2->param.type)
      return 0;
  }
  return 1;
}

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh,
                      TypeContext *type_ctx) {
  table->parent = NULL;
  List_init(&table->symbols);
  table->eh = eh;
  table->type_ctx = type_ctx;
}

void SymbolTable_add(SymbolTable *table, StringView name, Type *type,
                     int is_const) {
  Symbol *symbol = xmalloc(sizeof(Symbol));
  if (!symbol) {
    fprintf(stderr, "Failed to allocate symbol\n");
    exit(1);
  }
  symbol->name = name;
  symbol->type = type;
  symbol->is_const = is_const;
  symbol->func_status = FUNC_NONE;
  symbol->scope = table;
  List_push(&table->symbols, symbol);
}

Symbol *SymbolTable_add_func(SymbolTable *table, StringView name,
                             AstNode *func) {
  Symbol *existing = SymbolTable_find(table, name);
  AstNode *body = func->func_decl.body;
  FuncStatus status = body ? FUNC_IMPLEMENTATION : FUNC_FORWARD_DECL;

  if (existing) {
    if (existing->func_status == FUNC_FORWARD_DECL &&
        status == FUNC_IMPLEMENTATION) {
      if (!signature_matches(existing->value, func)) {
        ErrorHandler_report(table->eh, func->loc,
                            "Function signature mismatch");
        return NULL;
      }
      existing->func_status = FUNC_IMPLEMENTATION;
      existing->value = func;
      existing->type = func->func_decl.return_type;
      return existing;
    } else {
      ErrorHandler_report(table->eh, func->loc, "Function already defined");
      return NULL;
    }
  }

  Symbol *sym = xmalloc(sizeof(Symbol));
  sym->name = name;
  sym->type = func->func_decl.return_type;
  sym->func_status = status;
  sym->value = func;
  sym->scope = table;
  SymbolTable *fn_scope = xmalloc(sizeof(SymbolTable));
  SymbolTable_init(fn_scope, table->eh, table->type_ctx);

  /* Register function symbol in the current table so calls can resolve */
  List_push(&table->symbols, sym);

  return sym;
}

Symbol *SymbolTable_find(SymbolTable *table, StringView name) {
  for (SymbolTable *t = table; t != NULL; t = t->parent) {
    for (size_t i = 0; i < t->symbols.len; i++) {
      Symbol *symbol = t->symbols.items[i];
      if (sv_eq(symbol->name, name))
        return symbol;
    }
  }
  return NULL;
}

static void check_literal_bounds(SymbolTable *table, AstNode *node,
                                 Type *target) {
  if (node->tag != NODE_NUMBER || !target || target->kind != KIND_PRIMITIVE)
    return;

  double val = node->number.value;
  switch (target->data.primitive) {
  case PRIM_I8:
    if (val < SCHAR_MIN || val > SCHAR_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for i8", val);
    break;
  case PRIM_U8:
    if (val < 0 || val > UCHAR_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for u8", val);
    break;
  case PRIM_I16:
    if (val < SHRT_MIN || val > SHRT_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for i16", val);
    break;
  case PRIM_U16:
    if (val < 0 || val > USHRT_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for u16", val);
    break;
  case PRIM_I32:
    if (val < INT_MIN || val > INT_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for i32", val);
    break;
  case PRIM_U32:
    if (val < 0 || val > UINT_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for u32", val);
    break;
  case PRIM_I64:
    if (val < LLONG_MIN || val > LLONG_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for i64", val);
    break;
  case PRIM_U64:
    if (val < 0 || (unsigned long long)val > ULLONG_MAX)
      semantic_warning(table, node, "Literal %.2f out of range for u64", val);
    break;
  case PRIM_F32:
    if (fabs(val) > FLT_MAX && !isinf(val))
      semantic_warning(table, node, "Literal %.2f out of range for f32", val);
    break;
  default:
    break;
  }
}

static Type *check_expression(SymbolTable *table, AstNode *node) {
  if (!node)
    return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);

  if (node->resolved_type) {
    return node->resolved_type;
  }

  Type *type = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);

  switch (node->tag) {
  case NODE_NUMBER:
    if (sv_contains(node->number.raw_text, '.')) {
      type = type_get_primitive(table->type_ctx,
                                sv_ends_with(node->number.raw_text, "f") ||
                                        sv_ends_with(node->number.raw_text, "F")
                                    ? PRIM_F32
                                    : PRIM_F64);
    } else {
      type = type_get_primitive(table->type_ctx, PRIM_I32);
    }
    node->resolved_type = type;
    break;
  case NODE_CHAR:
    type = type_get_primitive(table->type_ctx, PRIM_CHAR);
    node->resolved_type = type;
    break;
  case NODE_STRING:
    type = type_get_primitive(table->type_ctx, PRIM_STRING);
    node->resolved_type = type;
    break;
  case NODE_BOOL:
    type = type_get_primitive(table->type_ctx, PRIM_BOOL);
    node->resolved_type = type;
    break;

  case NODE_VAR: {
    Symbol *s = SymbolTable_find(table, node->var.value);
    if (!s) {
      semantic_error(table, node, "Undefined variable '" SV_FMT "'",
                     SV_ARG(node->var.value));
      type = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      node->resolved_type = type;
      return type;
    }
    type = s->type;
    node->resolved_type = type;
    break;
  }

  case NODE_UNARY: {
    Type *t = check_expression(table, node->unary.expr);

    if (!t) {
      type = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      node->resolved_type = type;
      return type;
    }
    if (t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_UNKNOWN) {
      node->resolved_type = t;
      return t;
    }
    type = t;
    node->resolved_type = type;
    break;
  }

  case NODE_BINARY_ARITH: {
    Type *left = check_expression(table, node->binary_arith.left);
    Type *right = check_expression(table, node->binary_arith.right);

    if (!is_numeric(left) || !is_numeric(right)) {
      semantic_error(table, node, "Arithmetic operator on non-numeric type");
      type = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      node->resolved_type = type;
      return type;
    }

    type = type_rank(left) >= type_rank(right) ? left : right;

    // If this is part of a larger expression being assigned to a specific
    // target, the target type might be even higher (handled in
    // assignments/decl)

    // Ensure both operands are updated to the result type
    // This allows Codegen to use node->resolved_type for all casts
    if (left != type) {
      check_literal_bounds(table, node->binary_arith.left, type);
    }
    if (right != type) {
      check_literal_bounds(table, node->binary_arith.right, type);
    }

    node->resolved_type = type;
    break;
  }

  case NODE_BINARY_COMPARE: {
    Type *left = check_expression(table, node->binary_compare.left);
    Type *right = check_expression(table, node->binary_compare.right);
    if (!is_numeric(left) || !is_numeric(right)) {
      semantic_error(table, node,
                     "Comparison operator requires numeric operands");
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }
    type = type_get_primitive(table->type_ctx, PRIM_BOOL);
    node->resolved_type = type;
    break;
  }

  case NODE_BINARY_EQUALITY: {
    Type *left = check_expression(table, node->binary_equality.left);
    Type *right = check_expression(table, node->binary_equality.right);
    if (!can_implicitly_cast(left, right) &&
        !can_implicitly_cast(right, left)) {
      semantic_error(table, node, "Equality operator on incompatible types");
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }

    if (left->kind == KIND_ARRAY ||
        (left->kind == KIND_PRIMITIVE && left->data.primitive == PRIM_STRING)) {
      // Lower to intrinsic compare
      AstNode *left_node = node->binary_equality.left;
      AstNode *right_node = node->binary_equality.right;
      EqualityOp op = node->binary_equality.op;

      node->tag = NODE_INTRINSIC_COMPARE;
      node->intrinsic_compare.left = left_node;
      node->intrinsic_compare.right = right_node;
      node->intrinsic_compare.op = op;
    }

    type = type_get_primitive(table->type_ctx, PRIM_BOOL);
    node->resolved_type = type;
    break;
  }

  case NODE_BINARY_LOGICAL: {
    Type *left = check_expression(table, node->binary_logical.left);
    Type *right = check_expression(table, node->binary_logical.right);
    if (!is_bool(left) || !is_bool(right)) {
      semantic_error(table, node, "Logical operator requires boolean operands");
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }
    type = type_get_primitive(table->type_ctx, PRIM_BOOL);
    node->resolved_type = type;
    break;
  }

  case NODE_ARRAY_LITERAL: {
    if (node->array_literal.items.len == 0) {
      semantic_error(table, node, "Empty array literals are not yet supported");
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }

    Type *element_type = NULL;
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      Type *item_type = check_expression(table, item);

      if (element_type == NULL) {
        element_type = item_type;
      } else {
        if (type_rank(item_type) > type_rank(element_type)) {
          element_type = item_type;
        }
      }
    }

    // Check elements against the determined element type
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      if (!can_implicitly_cast(element_type, item->resolved_type)) {
        semantic_error(
            table, item, "Array element type mismatch: expected %s, got %s",
            type_to_name(element_type), type_to_name(item->resolved_type));
      }
      check_literal_bounds(table, item, element_type);
    }

    // Create fixed-size array type
    type = type_get_array(table->type_ctx, element_type, false,
                          node->array_literal.items.len);
    node->resolved_type = type;
    break;
  }

  case NODE_ARRAY_REPEAT: {
    Type *elem_type = check_expression(table, node->array_repeat.value);
    Type *count_type = check_expression(table, node->array_repeat.count);

    if (!is_numeric(count_type)) {
      semantic_error(table, node->array_repeat.count,
                     "Array repeat count must be numeric, got %s",
                     type_to_name(count_type));
    }

    size_t count = 0;
    bool is_dynamic = true;
    if (node->array_repeat.count->tag == NODE_NUMBER) {
      double c = node->array_repeat.count->number.value;
      if (c < 0) {
        semantic_error(table, node->array_repeat.count,
                       "Array repeat count cannot be negative: %g", c);
      }
      count = (size_t)c;
      is_dynamic = false;
    }

    type = type_get_array(table->type_ctx, elem_type, is_dynamic, count);
    node->resolved_type = type;
    break;
  }

  case NODE_INDEX: {
    Type *arr_type = check_expression(table, node->index.array);
    Type *idx_type = check_expression(table, node->index.index);

    if (arr_type->kind != KIND_ARRAY &&
        !(arr_type->kind == KIND_PRIMITIVE &&
          arr_type->data.primitive == PRIM_STRING)) {
      semantic_error(table, node->index.array,
                     "Indexing target must be an array or string, got %s",
                     type_to_name(arr_type));
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }

    if (!is_numeric(idx_type)) {
      semantic_error(table, node->index.index,
                     "Array index must be numeric, got %s",
                     type_to_name(idx_type));
    }

    // Result of indexing a string is a char
    if (arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING) {
      type = type_get_primitive(table->type_ctx, PRIM_CHAR);
      node->resolved_type = type;
    } else {
      type = arr_type->data.array.element;
      node->resolved_type = type;

      // Constant bounds check (only for non-strings and explicit fixed size
      // arrays)
      if (node->index.index->tag == NODE_NUMBER) {
        double idx_val = node->index.index->number.value;
        if (!arr_type->data.array.is_dynamic &&
            arr_type->data.array.fixed_size > 0) {
          if (idx_val < 0 ||
              (size_t)idx_val >= arr_type->data.array.fixed_size) {
            semantic_error(
                table, node->index.index,
                "Array index %g is out of bounds for array of size %zu",
                idx_val, arr_type->data.array.fixed_size);
          }
        }
      }
    }
    break;
  }

  case NODE_FIELD: {
    Type *obj_type = check_expression(table, node->field.object);
    Member *m = type_get_member(obj_type, node->field.field);
    if (m) {
      type = m->type;
      node->resolved_type = type;

    } else if (obj_type->kind == KIND_PRIMITIVE &&
               obj_type->data.primitive == PRIM_STRING &&
               sv_eq(node->field.field, sv_from_parts("to_array", 8))) {
      // Blessed method s.to_array() -> [char]
      type =
          type_get_array(table->type_ctx,
                         type_get_primitive(table->type_ctx, PRIM_CHAR), 0, 0);
      node->resolved_type = type;
    } else {
      semantic_error(table, node, "Type %s has no member '" SV_FMT "'",
                     type_to_name(obj_type), SV_ARG(node->field.field));
      type = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      node->resolved_type = type;
    }
    break;
  }

  case NODE_STATIC_MEMBER: {
    if (sv_eq(node->static_member.parent, sv_from_parts("String", 6)) &&
        sv_eq(node->static_member.member, sv_from_parts("from_array", 10))) {
      // Blessed static method String::from_array([char]) -> String
      // Return a dummy type for now, but really we want a function type.
      type = type_get_primitive(table->type_ctx, PRIM_STRING);
      node->resolved_type = type;
    } else {
      semantic_error(table, node,
                     "Unknown static member '" SV_FMT "::" SV_FMT "'",
                     SV_ARG(node->static_member.parent),
                     SV_ARG(node->static_member.member));
      type = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      node->resolved_type = type;
    }
    break;
  }

  case NODE_TERNARY: {
    Type *cond = check_expression(table, node->ternary.condition);
    if (!is_bool(cond)) {
      semantic_error(table, node->ternary.condition,
                     "Ternary condition must be boolean");
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }
    Type *t_expr = check_expression(table, node->ternary.true_expr);
    Type *f_expr = check_expression(table, node->ternary.false_expr);
    if ((t_expr->kind == KIND_PRIMITIVE &&
         t_expr->data.primitive == PRIM_UNKNOWN) ||
        (f_expr->kind == KIND_PRIMITIVE &&
         f_expr->data.primitive == PRIM_UNKNOWN)) {
      type = t_expr;
      node->resolved_type = type;
      break;
    }
    if (can_implicitly_cast(t_expr, f_expr)) {
      type = t_expr;
      node->resolved_type = type;
      break;
    }
    if (can_implicitly_cast(f_expr, t_expr)) {
      type = f_expr;
      node->resolved_type = type;
      break;
    }
    semantic_error(table, node, "Ternary branches must have compatible types");
    return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
  }

  case NODE_ASSIGN_EXPR: {
    AstNode *target = node->assign_expr.target;
    if (!is_lvalue(target)) {
      semantic_error(table, node, "Invalid assignment target");
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }

    Type *lhs = check_expression(table, target);

    if (target->tag == NODE_INDEX) {
      Type *arr_type = target->index.array->resolved_type;
      if (arr_type && arr_type->kind == KIND_PRIMITIVE &&
          arr_type->data.primitive == PRIM_STRING) {
        semantic_error(table, node, "Strings in tyl are immutable");
        return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      }
    } else if (lhs->kind == KIND_PRIMITIVE &&
               lhs->data.primitive == PRIM_STRING) {
    }

    if (target->tag == NODE_VAR) {
      Symbol *s = SymbolTable_find(table, target->var.value);
      if (s && s->is_const) {
        semantic_error(table, node, "Cannot assign to constant '" SV_FMT "'",
                       SV_ARG(s->name));
        return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      }
    }

    Type *rhs = check_expression(table, node->assign_expr.value);
    if (!can_implicitly_cast(lhs, rhs)) {
      semantic_error(table, node,
                     "Type mismatch in assignment: expected %s, got %s",
                     type_to_name(lhs), type_to_name(rhs));
    }

    node->resolved_type = lhs;
    break;
  }

  case NODE_CAST_EXPR: {
    check_expression(table, node->cast_expr.expr);
    type = node->cast_expr.target_type;
    node->resolved_type = type;
    break;
  }

  case NODE_CALL: {
    Type *func_type = check_expression(table, node->call.func);
    if (func_type->kind == KIND_PRIMITIVE &&
        func_type->data.primitive == PRIM_UNKNOWN) {
      return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
    }

    if (node->call.func->tag != NODE_VAR) {
      // For now, only named calls are supported, but we might have complex
      // exprs returning symbols or method calls being resolved. Already handled
      // by check_expression above.
    }

    if (node->call.func->tag == NODE_VAR) {
      StringView name = node->call.func->var.value;
      if (sv_eq(name, sv_from_parts("free", 4))) {
        // Special case for free(): check it takes 1 array
        if (node->call.args.len != 1) {
          semantic_error(table, node, "free() expects 1 argument, got %zu",
                         node->call.args.len);
        } else {
          Type *arg_ty = check_expression(table, node->call.args.items[0]);
          if (arg_ty->kind != KIND_ARRAY) {
            semantic_error(table, node->call.args.items[0],
                           "free() expects an array, got %s",
                           type_to_name(arg_ty));
          }

          AstNode *arg_node = node->call.args.items[0];
          if (arg_node->tag != NODE_VAR) {
            // In a real system, we might warn that zeroing will only affect the
            // temp.
          }
        }
        type = type_get_primitive(table->type_ctx, PRIM_VOID);
        node->resolved_type = type;
        break;
      }

      Symbol *s = SymbolTable_find(table, name);
      if (!s) {
        semantic_error(table, node, "Call to undefined function '" SV_FMT "'",
                       SV_ARG(node->call.func->var.value));
        return type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
      }

      if (s->value && s->value->tag == NODE_FUNC_DECL) {
        AstNode *fn_node = s->value;
        if (node->call.args.len != fn_node->func_decl.params.len) {
          semantic_error(table, node,
                         "Function '" SV_FMT "' expects %zu arguments, got %zu",
                         SV_ARG(s->name), fn_node->func_decl.params.len,
                         node->call.args.len);
        }

        for (size_t i = 0; i < node->call.args.len; i++) {
          Type *arg_type = check_expression(table, node->call.args.items[i]);
          if (i < fn_node->func_decl.params.len) {
            AstNode *param_node = fn_node->func_decl.params.items[i];
            Type *param_type = param_node->param.type;
            if (!can_implicitly_cast(param_type, arg_type)) {
              semantic_error(table, node,
                             "Argument %zu type mismatch: expected %s, got %s",
                             i, type_to_name(param_type),
                             type_to_name(arg_type));
            }
          }
        }
      } else if (s->scope) {
        // Fallback or external function?
        if (node->call.args.len != s->scope->symbols.len) {
          semantic_error(table, node,
                         "Function '" SV_FMT "' expects %zu arguments, got %zu",
                         SV_ARG(s->name), s->scope->symbols.len,
                         node->call.args.len);
        }
        for (size_t i = 0; i < node->call.args.len; i++) {
          check_expression(table, node->call.args.items[i]);
        }
      } else {
        // Just check args
        for (size_t i = 0; i < node->call.args.len; i++) {
          check_expression(table, node->call.args.items[i]);
        }
      }
      type = s->type;
    } else {
      // It was a field access or static member that resolved to a type.
      // E.g. s.to_array() or String::from_array().
      // For now, we assume these are blessed.
      type = func_type;
    }
    node->resolved_type = type;
    break;
  }

  default:
    break;
  }
  node->resolved_type = type;
  return type;
}

static void Semantic_deduplicate_top_level(AstNode *root,
                                           SymbolTable *global_table) {
  if (root->tag != NODE_AST_ROOT)
    return;

  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag != NODE_FUNC_DECL)
      continue;

    StringView name = node->func_decl.name;
    Symbol *existing = SymbolTable_find(global_table, name);

    if (existing && existing->func_status == FUNC_FORWARD_DECL &&
        node->func_decl.body) {

      AstNode *original = (AstNode *)existing->value;
      original->func_decl.body = node->func_decl.body;
      original->func_decl.return_type = node->func_decl.return_type;
      existing->func_status = FUNC_IMPLEMENTATION;

      List_remove_at(&root->ast_root.children, i);
      node->func_decl.body = NULL;
      Ast_free(node);

      i--;
    } else if (existing && existing->func_status == FUNC_IMPLEMENTATION &&
               node->func_decl.body) {

      semantic_error(global_table, node,
                     "Redefinition of function '" SV_FMT "'", SV_ARG(name));
    } else {
      SymbolTable_add_func(global_table, name, node);
    }
  }
}

void Semantic_analysis(AstNode *node, SymbolTable *table) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_AST_ROOT:

    Type *string_type = type_get_primitive(table->type_ctx, PRIM_STRING);
    SymbolTable_add(table, sv_from_parts("String", 6), string_type, true);

    SymbolTable_add(table, sv_from_parts("free", 4),
                    type_get_primitive(table->type_ctx, PRIM_VOID), true);

    Semantic_deduplicate_top_level(node, table);
    for (size_t i = 0; i < node->ast_root.children.len; i++)
      Semantic_analysis(node->ast_root.children.items[i], table);
    break;

  case NODE_BLOCK: {
    SymbolTable block_table;
    SymbolTable_init(&block_table, table->eh, table->type_ctx);
    block_table.parent = table;

    for (size_t i = 0; i < node->block.statements.len; i++)
      Semantic_analysis(node->block.statements.items[i], &block_table);
    break;
  }

  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    Symbol *existing = SymbolTable_find(table, name);
    if (existing && existing->scope == table) {
      // If we are in a loop or block, we might want to allow this if it's not
      // the same scope. but SymbolTable should be separate for each block.
      // Top-level REPL-like behavior allows re-declaration if NOT the same
      // block. But for now, we just check if it's literally the same table
      // pointer.
      semantic_error(table, node, "Duplicate variable '" SV_FMT "'",
                     SV_ARG(name));
      return;
    }
    Type *val_type = check_expression(table, node->var_decl.value);
    Type *decl_type = node->var_decl.declared_type;
    if (decl_type->kind == KIND_PRIMITIVE &&
        decl_type->data.primitive == PRIM_UNKNOWN) {
      decl_type = val_type;
    } else if (node->var_decl.value) {
      // If we have a declared type and a value, check if value can be cast to
      // it
      if (!can_implicitly_cast(decl_type, val_type)) {
        semantic_error(
            table, node->var_decl.value,
            "Type mismatch in variable declaration: expected %s, got %s",
            type_to_name(decl_type), type_to_name(val_type));
      }
    } else {
      // No initial value. Check if type contains placeholders (e.g. [int; _])
      Type *t = decl_type;
      while (t && t->kind == KIND_ARRAY) {
        if (t->data.array.is_dynamic) {
          semantic_error(table, node,
                         "Cannot declare uninitialized array with dynamic "
                         "dimension (placeholder '_')");
          break;
        }
        t = t->data.array.element;
      }
    }

    node->var_decl.declared_type = decl_type;

    // Check bounds for initial value literal against the variable type
    if (node->var_decl.value) {
      check_literal_bounds(table, node->var_decl.value, decl_type);
    }

    SymbolTable_add(table, name, decl_type, node->var_decl.is_const);
    break;
  }

  case NODE_FUNC_DECL: {
    StringView fn_name = node->func_decl.name;
    Symbol *fn_symbol = SymbolTable_find(table, fn_name);

    if (!fn_symbol) {
      // This should theoretically only happen if it's a local function
      // not caught by top-level deduplication.
      fn_symbol = SymbolTable_add_func(table, fn_name, node);
    }

    if (!fn_symbol)
      return;

    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      AstNode *param = node->func_decl.params.items[i];
      if (SymbolTable_find(fn_symbol->scope, param->param.name)) {
        semantic_error(fn_symbol->scope, node,
                       "Duplicate parameter name '" SV_FMT
                       "' in function '" SV_FMT "'",
                       SV_ARG(param->param.name), SV_ARG(fn_name));
        continue;
      }
      SymbolTable_add(fn_symbol->scope, param->param.name, param->param.type,
                      0);
    }

    if (node->func_decl.body)
      Semantic_analysis(node->func_decl.body, fn_symbol->scope);

    if (node->func_decl.return_type->kind == KIND_PRIMITIVE &&
        node->func_decl.return_type->data.primitive == PRIM_UNKNOWN &&
        node->func_decl.body) {
      Type *inferred = type_get_primitive(table->type_ctx, PRIM_VOID);
      int set = 0;
      for (size_t i = 0; i < node->func_decl.body->block.statements.len; i++) {
        AstNode *stmt = node->func_decl.body->block.statements.items[i];
        if (stmt->tag == NODE_RETURN_STMT && stmt->return_stmt.expr) {
          Type *t = check_expression(fn_symbol->scope, stmt->return_stmt.expr);
          if (!set) {
            inferred = t;
            set = 1;
          } else if (inferred != t) {
            inferred = type_get_primitive(table->type_ctx, PRIM_UNKNOWN);
          }
        }
      }
      node->func_decl.return_type = inferred;
      fn_symbol->type = inferred;
    }

    if (node->func_decl.body) {
      for (size_t i = 0; i < node->func_decl.body->block.statements.len; i++) {
        AstNode *stmt = node->func_decl.body->block.statements.items[i];
        if (stmt->tag == NODE_RETURN_STMT) {
          Type *t = check_expression(fn_symbol->scope, stmt->return_stmt.expr);
          if (node->func_decl.return_type->kind == KIND_PRIMITIVE &&
              node->func_decl.return_type->data.primitive == PRIM_VOID &&
              stmt->return_stmt.expr) {
            semantic_error(fn_symbol->scope, stmt,
                           "Void function '" SV_FMT "' cannot return a value",
                           SV_ARG(fn_name));
          } else if (!(node->func_decl.return_type->kind == KIND_PRIMITIVE &&
                       node->func_decl.return_type->data.primitive ==
                           PRIM_UNKNOWN) &&
                     !can_implicitly_cast(node->func_decl.return_type, t)) {
            semantic_error(fn_symbol->scope, stmt,
                           "Type mismatch in return: expected %s, got %s in "
                           "function '" SV_FMT "'",
                           type_to_name(node->func_decl.return_type),
                           type_to_name(t), SV_ARG(fn_name));
          }
        }
      }
    }
    break;
  }

  case NODE_PRINT_STMT:
    for (size_t i = 0; i < node->print_stmt.values.len; i++)
      check_expression(table, node->print_stmt.values.items[i]);
    break;

  case NODE_EXPR_STMT:
    check_expression(table, node->expr_stmt.expr);
    break;

  case NODE_IF_STMT: {
    Type *cond_type = check_expression(table, node->if_stmt.condition);
    if (!is_bool(cond_type)) {
      semantic_error(table, node->if_stmt.condition,
                     "if condition must be a boolean");
    }
    Semantic_analysis(node->if_stmt.then_branch, table);
    if (node->if_stmt.else_branch) {
      Semantic_analysis(node->if_stmt.else_branch, table);
    }
    break;
  }

  case NODE_DEFER:
    Semantic_analysis(node->defer.expr, table);
    break;

  case NODE_STRUCT_DECL: {
    StringView name = node->struct_decl.name->var.value;
    char *c_name = sv_to_cstr(name);
    Type *struct_type = type_get_struct(table->type_ctx, c_name);
    free(c_name);

    if (SymbolTable_find(table, name)) {
      semantic_error(table, node, "Duplicate symbol '" SV_FMT "'",
                     SV_ARG(name));
      break;
    }
    SymbolTable_add(table, name, struct_type, true);

    for (size_t i = 0; i < node->struct_decl.members.len; i++) {
      AstNode *mem_decl = node->struct_decl.members.items[i];
      StringView mem_name = mem_decl->var_decl.name->var.value;
      Type *mem_type = mem_decl->var_decl.declared_type;

      char *c_mem_name = sv_to_cstr(mem_name);
      type_add_member(struct_type, c_mem_name, mem_type, 0); // Offset 0 for now
      free(c_mem_name);
    }
    break;
  }

  default:
    break;
  }
}
