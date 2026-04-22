#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "sema_internal.h"
#include "tyl/ast.h"

Type *sema_find_type_by_name(Sema *s, StringView name) {
  if (sv_eq_cstr(name, "String")) {
    return type_get_string_buffer(s->types);
  }

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
    if (sv_eq_cstr(name, prim_name)) {
      return t;
    }
  }

  return NULL;
}

static ExprInfo ExprInfo_rvalue(Type *type) {
  return (ExprInfo){.type = type, .category = VAL_RVALUE};
}

static ExprInfo ExprInfo_lvalue(Type *type) {
  return (ExprInfo){.type = type, .category = VAL_LVALUE};
}

bool sema_is_writable_address(Sema *s, AstNode *node) {
  if (!node)
    return false;

  switch (node->tag) {
  case NODE_VAR: {
    Symbol *sym = sema_resolve(s, node->var.value);
    return sym && (sym->kind == SYM_VAR || sym->kind == SYM_FIELD) &&
           !sym->is_const;
  }

  case NODE_FIELD: {
    ExprInfo object_info = sema_check_expr(s, node->field.object);
    if (object_info.category != VAL_LVALUE)
      return false;

    Type *obj_type = object_info.type;
    while (obj_type && obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }
    if (!obj_type)
      return false;

    Member *m = type_get_member(obj_type, node->field.field);
    if (m)
      return true;

    if (obj_type->kind == KIND_UNION) {
      Type *owner = NULL;
      m = type_find_union_field(obj_type, node->field.field, &owner);
      return m != NULL;
    }
    return false;
  }

  case NODE_INDEX: {
    ExprInfo array_info = sema_check_expr(s, node->index.array);
    if (array_info.category != VAL_LVALUE)
      return false;

    Type *arr_type = node->index.array->resolved_type;
    if (!arr_type)
      return false;
    if (arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING) {
      return false;
    }
    return arr_type->kind == KIND_POINTER || type_is_array_struct(arr_type);
  }

  case NODE_UNARY:
    if (node->unary.op == OP_DEREF) {
      Type *operand_type = sema_check_expr(s, node->unary.expr).type;
      return operand_type && operand_type->kind == KIND_POINTER;
    }
    return false;

  default:
    return false;
  }
}

static ExprInfo expr_info_for_cached_node(Sema *s, AstNode *node, Type *type) {
  if (!node)
    return ExprInfo_rvalue(type);

  switch (node->tag) {
  case NODE_VAR: {
    return ExprInfo_lvalue(type);
  }

  case NODE_FIELD: {
    ExprInfo object_info = sema_check_expr(s, node->field.object);
    Type *obj_type = object_info.type;
    while (obj_type && obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }
    if (obj_type) {
      Member *m = type_get_member(obj_type, node->field.field);
      if (m) {
        return ExprInfo_lvalue(type);
      }
    }
    return ExprInfo_rvalue(type);
  }

  case NODE_INDEX: {
    Type *arr_type = node->index.array->resolved_type;
    if (arr_type && !(arr_type->kind == KIND_PRIMITIVE &&
                      arr_type->data.primitive == PRIM_STRING)) {
      return ExprInfo_lvalue(type);
    }
    return ExprInfo_rvalue(type);
  }

  case NODE_UNARY:
    if (node->unary.op == OP_DEREF) {
      return ExprInfo_lvalue(type);
    }
    return ExprInfo_rvalue(type);

  default:
    return ExprInfo_rvalue(type);
  }
}

bool sema_fn_decl_can_use_arena(AstNode *fn_decl) {
  if (!fn_decl || fn_decl->tag != NODE_FUNC_DECL)
    return false;

  if (fn_decl->func_decl.return_type &&
      ((fn_decl->func_decl.return_type->kind == KIND_PRIMITIVE &&
        fn_decl->func_decl.return_type->data.primitive == PRIM_STRING) ||
       fn_decl->func_decl.return_type->kind == KIND_STRING_BUFFER)) {
    return false;
  }

  for (size_t i = 0; i < fn_decl->func_decl.params.len; i++) {
    AstNode *param_node = fn_decl->func_decl.params.items[i];
    if (!param_node || param_node->tag != NODE_PARAM)
      continue;
    Type *param_type = param_node->param.type;
    if (!param_type)
      continue;

    Type *resolved = param_type;
    while (resolved && resolved->kind == KIND_POINTER) {
      resolved = resolved->data.pointer_to;
    }
    if (resolved && resolved->kind == KIND_STRING_BUFFER) {
      return false;
    }
  }

  return true;
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

AstNode *sema_coerce(Sema *s, AstNode *expr, Type *target) {
  ExprInfo expr_info = sema_check_expr(s, expr);
  Type *expr_type = expr_info.type;
  bool target_is_tagged_union =
      target && target->kind == KIND_UNION && target->is_tagged_union;

  // Literal Bounds Check
  if (expr->tag == NODE_NUMBER || expr->tag == NODE_CHAR) {
    sema_check_literal_bounds(s, expr, target);

    if (type_can_implicitly_cast(target, expr_type) ||
        (expr->tag == NODE_NUMBER && literal_fits_in_type(expr, target))) {
      if (!target_is_tagged_union) {
        expr->resolved_type = target;
      }
      return expr;
    }
  }

  // Cast String to str explicitly when assigning to a string slice.
  if (target && target->kind == KIND_STRING_BUFFER && expr_type &&
      expr_type->kind == KIND_PRIMITIVE &&
      expr_type->data.primitive == PRIM_STRING) {
    expr = AstNode_new_cast_expr(expr, target, expr->loc);
    expr_type = target;
  }

  // Exact match or implicit cast
  if (type_can_implicitly_cast(target, expr_type)) {
    if (!target_is_tagged_union) {
      expr->resolved_type = target;
    }
    return expr;
  }

  // Inference support
  if (target->kind == KIND_PRIMITIVE &&
      target->data.primitive == PRIM_UNKNOWN) {
    expr->resolved_type = expr_type;
    return expr;
  }

  sema_error(s, expr, "Type mismatch: expected %s, got %s",
             type_to_name(target), type_to_name(expr_type));
  expr->resolved_type = expr_type;
  return expr;
}

ExprInfo sema_check_expr(Sema *s, AstNode *node) {
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