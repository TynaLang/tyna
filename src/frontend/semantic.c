#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tyl/ast.h"
#include "tyl/lexer.h"
#include "tyl/semantic.h"
#include "tyl/type.h"
#include "tyl/utils.h"

static void Sema_error(Sema *s, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(s->eh, n->loc, "%s", msg_buf);
}

static void sema_warning(Sema *s, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report_level(s->eh, n->loc, LEVEL_WARNING, "%s", msg_buf);
}

static int signature_matches(AstNode *dec, AstNode *imp) {
  if (dec->tag != NODE_FUNC_DECL || imp->tag != NODE_FUNC_DECL)
    return 0;

  if (dec->func_decl.return_type != imp->func_decl.return_type)
    return 0;

  if (dec->func_decl.params.len != imp->func_decl.params.len)
    return 0;

  List decl_params = dec->func_decl.params;
  List imp_params = imp->func_decl.params;
  for (size_t i = 0; i < decl_params.len; i++) {
    AstNode *p1 = decl_params.items[i];
    AstNode *p2 = imp_params.items[i];
    if (p1->param.type != p2->param.type)
      return 0;
  }
  return 1;
}

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
    if (val < LLONG_MIN || val > LLONG_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i64", val);
    break;

  case PRIM_U64:
    if (val < 0 || (unsigned long long)val > ULLONG_MAX)
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

// ----- Forward Refs -----
void Sema_scope_push(Sema *s);
void Sema_scope_pop(Sema *s);

void Sema_jump_push(Sema *s, SemaJump *ctx, StringView label, AstNode *node,
                    bool is_loop);
void Sema_jump_pop(Sema *s);

static Type *Sema_check_expression(Sema *s, AstNode *node);
// ----- Forward Refs -----

void Sema_init(Sema *s, ErrorHandler *eh, TypeContext *type_ctx) {
  s->types = type_ctx;
  s->eh = eh;

  s->ret_type = NULL;
  s->fn_node = NULL;
  s->jump = NULL;
  s->scope = NULL;

  Sema_scope_push(s);
}

void Sema_finish(Sema *s) {
  while (s->scope) {
    Sema_scope_pop(s);
  }
}

void Sema_scope_push(Sema *s) {
  SemaScope *new_scope = xmalloc(sizeof(SemaScope));
  new_scope->parent = s->scope;

  List_init(&new_scope->symbols);

  s->scope = new_scope;
}

void Sema_scope_pop(Sema *s) {
  if (s->scope == NULL)
    return;

  SemaScope *old = s->scope;
  s->scope = old->parent;

  for (size_t i = 0; i < old->symbols.len; i++) {
    free(old->symbols.items[i]);
  }

  List_free(&old->symbols, 0);
  free(old);
}

void Sema_jump_push(Sema *s, SemaJump *ctx, StringView label, AstNode *node,
                    bool is_loop) {
  ctx->label = label;
  ctx->node = node;
  ctx->is_loop = is_loop;

  ctx->parent = s->jump;

  s->jump = ctx;
}

void Sema_jump_pop(Sema *s) {
  if (s->jump) {
    s->jump = s->jump->parent;
  }
}

static Symbol *Sema_resolve(Sema *s, StringView name) {
  for (SemaScope *scope = s->scope; scope != NULL; scope = scope->parent) {
    for (size_t i = 0; i < scope->symbols.len; i++) {
      Symbol *sym = scope->symbols.items[i];
      if (sv_eq(sym->name, name)) {
        return sym;
      }
    }
  }
  return NULL;
}

static Symbol *Sema_resolve_local(Sema *s, StringView name) {
  if (!s->scope)
    return NULL;

  for (size_t i = 0; i < s->scope->symbols.len; i++) {
    Symbol *sym = s->scope->symbols.items[i];
    if (sv_eq(sym->name, name)) {
      return sym;
    }
  }
  return NULL;
}

static Symbol *Sema_define(Sema *s, StringView name, Type *type, bool is_const,
                           Location loc) {
  for (size_t i = 0; i < s->scope->symbols.len; i++) {
    Symbol *sym = s->scope->symbols.items[i];
    if (sv_eq(sym->name, name)) {
      ErrorHandler_report(s->eh, loc, "Redefinition of symbol in this scope");
      return NULL;
    }
  }

  Symbol *sym = xmalloc(sizeof(Symbol));
  sym->name = name;
  sym->type = type;
  sym->is_const = is_const;
  sym->func_status = FUNC_NONE;
  sym->scope = s->scope;

  List_push(&s->scope->symbols, sym);
  return sym;
}

void Sema_register_func_signature(Sema *s, AstNode *node) {
  StringView name = node->func_decl.name;

  if (Sema_resolve_local(s, name)) {
    Sema_error(s, node, "Redefinition of symbol '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Symbol *sym =
      Sema_define(s, name, node->func_decl.return_type, true, node->loc);

  if (sym) {
    sym->func_status = node->func_decl.body ? FUNC_IMPLEMENTATION : FUNC_NONE;
    sym->value = node;
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

  case NODE_STRING:
    return type_get_primitive(s->types, PRIM_STRING);

  case NODE_BOOL:
    return type_get_primitive(s->types, PRIM_BOOL);

  default:
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

static Type *check_var(Sema *s, AstNode *node) {
  Symbol *sym = Sema_resolve(s, node->var.value);
  if (!sym) {
    Sema_error(s, node, "Undefined variable '" SV_FMT "'",
               SV_ARG(node->var.value));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
  return sym->type;
}

static Type *check_field(Sema *s, AstNode *node) {
  Type *obj_type = Sema_check_expression(s, node->field.object);
  Member *m = type_get_member(obj_type, node->field.field);
  if (m) {
    return m->type;
  } else if (obj_type->kind == KIND_PRIMITIVE &&
             obj_type->data.primitive == PRIM_STRING &&
             sv_eq(node->field.field, sv_from_parts("to_array", 8))) {
    return type_get_array(s->types, type_get_primitive(s->types, PRIM_CHAR),
                          false, 0);
  } else {
    Sema_error(s, node, "Type %s has no member '" SV_FMT "'",
               type_to_name(obj_type), SV_ARG(node->field.field));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

static Type *check_index(Sema *s, AstNode *node) {
  Type *arr_type = Sema_check_expression(s, node->index.array);
  Type *idx_type = Sema_check_expression(s, node->index.index);

  if (arr_type->kind != KIND_ARRAY &&
      !(arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING)) {
    Sema_error(s, node->index.array,
               "Indexing target must be an array or string, got %s",
               type_to_name(arr_type));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  if (!type_is_numeric(idx_type)) {
    Sema_error(s, node->index.index, "Array index must be numeric, got %s",
               type_to_name(idx_type));
  }

  if (arr_type->kind == KIND_PRIMITIVE &&
      arr_type->data.primitive == PRIM_STRING) {
    return type_get_primitive(s->types, PRIM_CHAR);
  } else {
    return arr_type->data.array.element;
  }
}

static Type *check_unary(Sema *s, AstNode *node) {
  Type *operand_type = Sema_check_expression(s, node->unary.expr);
  if (node->unary.op == OP_NEG) {
    if (!type_is_numeric(operand_type)) {
      Sema_error(s, node, "Unary '-' operator requires numeric operand, got %s",
                 type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return operand_type;
  } else if (node->unary.op == OP_PRE_INC || node->unary.op == OP_POST_INC ||
             node->unary.op == OP_PRE_DEC || node->unary.op == OP_POST_DEC) {
    if (!type_is_lvalue(node->unary.expr)) {
      Sema_error(s, node, "Increment/decrement operator requires an l-value");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    if (!type_is_numeric(operand_type)) {
      Sema_error(
          s, node,
          "Increment/decrement operator requires numeric operand, got %s",
          type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    // Check for immutability (strings/constants)
    if (node->unary.expr->tag == NODE_VAR) {
      Symbol *sym = Sema_resolve(s, node->unary.expr->var.value);
      if (sym && sym->is_const) {
        Sema_error(s, node, "Cannot increment/decrement constant '" SV_FMT "'",
                   SV_ARG(sym->name));
      }
    } else if (node->unary.expr->tag == NODE_INDEX) {
      Type *arr_type = node->unary.expr->index.array->resolved_type;
      if (arr_type && arr_type->kind == KIND_PRIMITIVE &&
          arr_type->data.primitive == PRIM_STRING) {
        Sema_error(s, node, "Strings in tyl are immutable");
      }
    }
    return operand_type;
  } else if (node->unary.op == OP_NOT) {
    if (!type_is_bool(operand_type)) {
      Sema_error(s, node,
                 "Logical '!' operator requires boolean operand, got %s",
                 type_to_name(operand_type));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return operand_type;
  } else {
    Sema_error(s, node, "Unknown unary operator");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

static Type *check_binary_arith(Sema *s, AstNode *node) {
  Type *left = Sema_check_expression(s, node->binary_arith.left);
  Type *right = Sema_check_expression(s, node->binary_arith.right);

  if (!type_is_numeric(left) || !type_is_numeric(right)) {
    Sema_error(s, node, "Arithmetic operator on non-numeric type");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Type *result_type = type_rank(left) >= type_rank(right) ? left : right;

  check_literal_bounds(s, node->binary_arith.left, result_type);
  check_literal_bounds(s, node->binary_arith.right, result_type);

  return result_type;
}

static Type *check_binary_logical(Sema *s, AstNode *node) {
  Type *left = Sema_check_expression(s, node->binary_logical.left);
  Type *right = Sema_check_expression(s, node->binary_logical.right);

  if (node->tag == NODE_BINARY_LOGICAL) {
    if (!type_is_bool(left) || !type_is_bool(right)) {
      Sema_error(s, node, "Logical operator requires boolean operands");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return type_get_primitive(s->types, PRIM_BOOL);
  } else if (node->tag == NODE_BINARY_COMPARE) {
    if (!type_is_numeric(left) || !type_is_numeric(right)) {
      Sema_error(s, node,
                 "Comparison operator requires numeric operands, got %s and %s",
                 type_to_name(left), type_to_name(right));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return type_get_primitive(s->types, PRIM_BOOL);
  } else if (node->tag == NODE_BINARY_EQUALITY) {
    if (!type_can_implicitly_cast(left, right) &&
        !type_can_implicitly_cast(right, left)) {
      Sema_error(s, node, "Equality operator on incompatible types: %s and %s",
                 type_to_name(left), type_to_name(right));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }
    return type_get_primitive(s->types, PRIM_BOOL);
  } else {
    Sema_error(s, node, "Unknown binary operator");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

static Type *check_call(Sema *s, AstNode *node) {
  Type *func_type = Sema_check_expression(s, node->call.func);
  if (func_type->kind == KIND_PRIMITIVE &&
      func_type->data.primitive == PRIM_UNKNOWN) {
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  if (node->call.func->tag == NODE_VAR) {
    StringView name = node->call.func->var.value;
    if (sv_eq(name, sv_from_parts("free", 4))) {
      // Special case for free(): check it takes 1 array
      if (node->call.args.len != 1) {
        Sema_error(s, node, "free() expects 1 argument, got %zu",
                   node->call.args.len);
      } else {
        Type *arg_ty = Sema_check_expression(s, node->call.args.items[0]);
        if (arg_ty->kind != KIND_ARRAY) {
          Sema_error(s, node->call.args.items[0],
                     "free() expects an array, got %s", type_to_name(arg_ty));
        }
      }
      return type_get_primitive(s->types, PRIM_VOID);
    }

    Symbol *symbol = Sema_resolve(s, name);
    if (!symbol) {
      Sema_error(s, node, "Call to undefined function '" SV_FMT "'",
                 SV_ARG(node->call.func->var.value));
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    if (symbol->value && symbol->value->tag == NODE_FUNC_DECL) {
      AstNode *fn_decl = symbol->value;
      if (node->call.args.len != fn_decl->func_decl.params.len) {
        Sema_error(s, node, "Function '" SV_FMT "' expects %zu args, got %zu",
                   SV_ARG(name), fn_decl->func_decl.params.len,
                   node->call.args.len);
      }
      for (size_t i = 0;
           i < node->call.args.len && i < fn_decl->func_decl.params.len; i++) {
        Type *arg_type = Sema_check_expression(s, node->call.args.items[i]);
        AstNode *param_node = fn_decl->func_decl.params.items[i];
        Type *param_type = param_node->param.type;
        if (!type_can_implicitly_cast(param_type, arg_type)) {
          Sema_error(s, node->call.args.items[i],
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
  if (node->tag == NODE_ARRAY_LITERAL) {
    if (node->array_literal.items.len == 0) {
      Sema_error(s, node, "Empty array literals are not yet supported");
      return type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    Type *element_type = NULL;
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      AstNode *item = node->array_literal.items.items[i];
      Type *item_type = Sema_check_expression(s, item);

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
      if (!type_can_implicitly_cast(element_type, item->resolved_type)) {
        Sema_error(s, item, "Array element type mismatch: expected %s, got %s",
                   type_to_name(element_type),
                   type_to_name(item->resolved_type));
      }
      check_literal_bounds(s, item, element_type);
    }

    // Create fixed-size array type
    return type_get_array(s->types, element_type, false,
                          node->array_literal.items.len);
  } else if (node->tag == NODE_ARRAY_REPEAT) {
    Type *elem_type = Sema_check_expression(s, node->array_repeat.value);
    Type *count_type = Sema_check_expression(s, node->array_repeat.count);

    if (!type_is_numeric(count_type)) {
      Sema_error(s, node->array_repeat.count,
                 "Array repeat count must be numeric, got %s",
                 type_to_name(count_type));
    }

    size_t count = 0;
    bool is_dynamic = true;
    if (node->array_repeat.count->tag == NODE_NUMBER) {
      double c = node->array_repeat.count->number.value;
      if (c < 0) {
        Sema_error(s, node->array_repeat.count,
                   "Array repeat count cannot be negative: %g", c);
      }
      count = (size_t)c;
      is_dynamic = false;
    } else {
      // If it's a variable or expression, it's a dynamic array
      is_dynamic = true;
    }

    return type_get_array(s->types, elem_type, is_dynamic, count);
  } else {
    Sema_error(s, node, "Expected array literal or repeat expression");
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

static bool type_is_writable(Sema *s, AstNode *target) {
  if (!type_is_lvalue(target))
    return false;

  if (target->tag == NODE_VAR) {
    Symbol *sym = Sema_resolve(s, target->var.value);
    if (sym && sym->is_const)
      return false;
  }

  if (target->tag == NODE_INDEX) {
    Type *arr_type = target->index.array->resolved_type;
    if (arr_type && arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING)
      return false;
  }

  Type *type = target->resolved_type;
  if (type && type->kind == KIND_PRIMITIVE &&
      type->data.primitive == PRIM_STRING)
    return false;

  return true;
}

static Type *check_assignment(Sema *s, AstNode *node) {
  AstNode *target = node->assign_expr.target;
  Type *lhs = Sema_check_expression(s, target);

  if (!type_is_writable(s, target)) {
    if (!type_is_lvalue(target)) {
      Sema_error(s, node, "Invalid assignment target");
    } else {
      Sema_error(s, node, "Cannot assign to read-only l-value");
    }
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Type *rhs = Sema_check_expression(s, node->assign_expr.value);
  if (!type_can_implicitly_cast(lhs, rhs)) {
    Sema_error(s, node, "Type mismatch in assignment: expected %s, got %s",
               type_to_name(lhs), type_to_name(rhs));
  }

  return lhs;
}

static Type *check_cast(Sema *s, AstNode *node) {
  Sema_check_expression(s, node->cast_expr.expr);
  return node->cast_expr.target_type;
}

static Type *check_ternary(Sema *s, AstNode *node) {
  Type *cond = Sema_check_expression(s, node->ternary.condition);
  if (!type_is_bool(cond)) {
    Sema_error(s, node->ternary.condition,
               "Ternary condition must be boolean, got %s", type_to_name(cond));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
  Type *t_expr = Sema_check_expression(s, node->ternary.true_expr);
  Type *f_expr = Sema_check_expression(s, node->ternary.false_expr);

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
  Sema_error(s, node,
             "Ternary branches must have compatible types: got %s and %s",
             type_to_name(t_expr), type_to_name(f_expr));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}

static Type *Sema_check_expression(Sema *s, AstNode *node) {
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
    Sema_error(s, node, "Unhandled node type in expression checking");
    break;
  }

  node->resolved_type = type;
  return type;
}

static void Sema_check_node(Sema *s, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_BLOCK: {
    Sema_scope_push(s);
    for (size_t i = 0; i < node->block.statements.len; i++)
      Sema_check_node(s, node->block.statements.items[i]);
    Sema_scope_pop(s);
    break;
  }

  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    if (Sema_resolve_local(s, name)) {
      Sema_error(s, node, "Duplicate variable '" SV_FMT "'", SV_ARG(name));
      return;
    }

    Type *val_type = Sema_check_expression(s, node->var_decl.value);
    Type *decl_type = node->var_decl.declared_type;

    if (decl_type->kind == KIND_PRIMITIVE &&
        decl_type->data.primitive == PRIM_UNKNOWN) {
      decl_type = val_type;
    } else if (node->var_decl.value) {
      if (!type_can_implicitly_cast(decl_type, val_type)) {
        Sema_error(s, node->var_decl.value,
                   "Type mismatch: expected %s, got %s",
                   type_to_name(decl_type), type_to_name(val_type));
      }
    } else {
      Type *t = decl_type;
      while (t && t->kind == KIND_ARRAY) {
        if (t->data.array.is_dynamic) {
          Sema_error(s, node, "Cannot declare uninitialized dynamic array");
          break;
        }
        t = t->data.array.element;
      }
    }

    node->var_decl.declared_type = decl_type;
    if (node->var_decl.value)
      check_literal_bounds(s, node->var_decl.value, decl_type);

    Sema_define(s, name, decl_type, node->var_decl.is_const, node->loc);
    break;
  }

  case NODE_FUNC_DECL: {
    if (!node->func_decl.body)
      return;

    Type *old_ret = s->ret_type;
    s->ret_type = node->func_decl.return_type;
    AstNode *old_fn = s->fn_node;
    s->fn_node = node;

    Sema_scope_push(s);

    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      AstNode *param = node->func_decl.params.items[i];
      Sema_define(s, param->param.name, param->param.type, false, param->loc);
    }

    Sema_check_node(s, node->func_decl.body);

    if (type_is_unknown(s->ret_type)) {
      s->ret_type = type_get_primitive(s->types, PRIM_VOID);
      node->func_decl.return_type = s->ret_type;
    }

    Sema_scope_pop(s);
    s->ret_type = old_ret;
    s->fn_node = old_fn;
    break;
  }

  case NODE_RETURN_STMT: {
    if (!s->ret_type) {
      Sema_error(s, node, "Return statement outside of function");
      return;
    }

    Type *expr_type = node->return_stmt.expr
                          ? Sema_check_expression(s, node->return_stmt.expr)
                          : type_get_primitive(s->types, PRIM_VOID);

    if (type_is_unknown(s->ret_type)) {
      s->ret_type = expr_type;
      s->fn_node->func_decl.return_type = expr_type;
    } else {
      if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
        Sema_error(s, node, "Type mismatch: function returns %s but got %s",
                   type_to_name(s->ret_type), type_to_name(expr_type));
      }
    }
    break;
  }

  case NODE_IF_STMT: {
    Type *cond = Sema_check_expression(s, node->if_stmt.condition);
    if (!type_is_bool(cond))
      Sema_error(s, node->if_stmt.condition, "if condition must be bool");

    Sema_check_node(s, node->if_stmt.then_branch);
    if (node->if_stmt.else_branch)
      Sema_check_node(s, node->if_stmt.else_branch);
    break;
  }

  case NODE_LOOP_STMT: {
    SemaJump jump_ctx;

    Sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);

    Sema_check_node(s, node->loop.expr);

    Sema_jump_pop(s);
    break;
  }

  case NODE_WHILE_STMT: {
    Type *cond = Sema_check_expression(s, node->while_stmt.condition);
    if (!type_is_bool(cond))
      Sema_error(s, node->while_stmt.condition, "while condition must be bool");

    SemaJump jump_ctx;
    Sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);

    Sema_check_node(s, node->while_stmt.body);

    Sema_jump_pop(s);
    break;
  }

  case NODE_FOR_STMT: {
    Sema_scope_push(s);
    if (node->for_stmt.init) {
      Sema_check_node(s, node->for_stmt.init);
    }
    if (node->for_stmt.condition) {
      Type *cond = Sema_check_expression(s, node->for_stmt.condition);
      if (!type_is_bool(cond)) {
        Sema_error(s, node->for_stmt.condition, "for condition must be bool");
      }
    }
    if (node->for_stmt.increment) {
      Sema_check_expression(s, node->for_stmt.increment);
    }

    SemaJump jump_ctx;
    Sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);
    Sema_check_node(s, node->for_stmt.body);
    Sema_jump_pop(s);

    Sema_scope_pop(s);
    break;
  }

  case NODE_FOR_IN_STMT: {
    Type *iter_type = Sema_check_expression(s, node->for_in_stmt.iterable);
    if (iter_type->kind != KIND_ARRAY &&
        (iter_type->kind != KIND_PRIMITIVE ||
         iter_type->data.primitive != PRIM_STRING)) {
      Sema_error(s, node->for_in_stmt.iterable,
                 "Cannot iterate over non-array/string type");
    }

    Type *elem_type = NULL;
    if (iter_type->kind == KIND_ARRAY) {
      elem_type = iter_type->data.array.element;
    } else {
      elem_type = type_get_primitive(s->types, PRIM_I8);
    }

    Sema_scope_push(s);
    Symbol *sym = Sema_define(s, node->for_in_stmt.var->var.value, elem_type,
                              true, node->for_in_stmt.var->loc);
    if (sym) {
      node->for_in_stmt.var->resolved_type = elem_type;
    }

    SemaJump jump_ctx;
    Sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);
    Sema_check_node(s, node->for_in_stmt.body);
    Sema_jump_pop(s);

    Sema_scope_pop(s);
    break;
  }

  case NODE_STRUCT_DECL: {
    StringView name = node->struct_decl.name->var.value;
    if (Sema_resolve_local(s, name)) {
      Sema_error(s, node, "Duplicate symbol '" SV_FMT "'", SV_ARG(name));
      break;
    }

    char *c_name = sv_to_cstr(name);
    Type *struct_type = type_get_struct(s->types, c_name);
    free(c_name);

    Sema_define(s, name, struct_type, true, node->loc);

    for (size_t i = 0; i < node->struct_decl.members.len; i++) {
      AstNode *mem = node->struct_decl.members.items[i];
      char *c_mem_name = sv_to_cstr(mem->var_decl.name->var.value);
      type_add_member(struct_type, c_mem_name, mem->var_decl.declared_type, 0);
      free(c_mem_name);
    }
    break;
  }

  case NODE_PRINT_STMT:
    for (size_t i = 0; i < node->print_stmt.values.len; i++)
      Sema_check_expression(s, node->print_stmt.values.items[i]);
    break;

  case NODE_EXPR_STMT:
    Sema_check_expression(s, node->expr_stmt.expr);
    break;

  case NODE_DEFER:
    Sema_check_node(s, node->defer.expr);
    break;

  default:
    break;
  }
}

void Sema_analyze(Sema *s, AstNode *root) {
  if (!root || root->tag != NODE_AST_ROOT)
    return;

  Type *string_type = type_get_primitive(s->types, PRIM_STRING);
  Sema_define(s, sv_from_parts("String", 6), string_type, true, root->loc);

  Type *void_type = type_get_primitive(s->types, PRIM_VOID);
  Sema_define(s, sv_from_parts("free", 4), void_type, true, root->loc);

  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      Sema_register_func_signature(s, node);
    }
  }

  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    Sema_check_node(s, root->ast_root.children.items[i]);
  }
}
