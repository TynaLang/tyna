#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "sema_internal.h"
#include "tyl/ast.h"

static bool literal_fits_in_type(AstNode *node, Type *target) {
  if (node->tag != NODE_NUMBER || !target || target->kind != KIND_PRIMITIVE)
    return false;

  if (sv_contains(node->number.raw_text, '.') ||
      sv_ends_with(node->number.raw_text, "f") ||
      sv_ends_with(node->number.raw_text, "F")) {
    return false;
  }

  double val = node->number.value;
  if (floor(val) != val)
    return false;

  switch (target->data.primitive) {
  case PRIM_I8:
    return val >= SCHAR_MIN && val <= SCHAR_MAX;
  case PRIM_U8:
    return val >= 0 && val <= UCHAR_MAX;
  case PRIM_I16:
    return val >= SHRT_MIN && val <= SHRT_MAX;
  case PRIM_U16:
    return val >= 0 && val <= USHRT_MAX;
  case PRIM_I32:
    return val >= INT_MIN && val <= INT_MAX;
  case PRIM_U32:
    return val >= 0 && val <= UINT_MAX;
  case PRIM_I64:
    return val >= (double)LLONG_MIN && val <= (double)LLONG_MAX;
  case PRIM_U64:
    return val >= 0 && val <= (double)ULLONG_MAX;
  default:
    return false;
  }
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
    if (sv_contains(node->number.raw_text, '.') ||
        sv_ends_with(node->number.raw_text, "f") ||
        sv_ends_with(node->number.raw_text, "F")) {
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

static Type *ast_substitute_type(Type *type, TypeContext *ctx,
                                 Type *template_type, List args) {
  if (!type || !ctx || !template_type)
    return type;
  return type_resolve_placeholders(ctx, type, template_type, args);
}

static List ast_clone_node_list(List *nodes, TypeContext *ctx,
                                Type *template_type, List args);

static AstNode *ast_clone_node(AstNode *node, TypeContext *ctx,
                               Type *template_type, List args) {
  if (!node)
    return NULL;

  AstNode *copy = xcalloc(1, sizeof(AstNode));
  copy->tag = node->tag;
  copy->loc = node->loc;
  copy->resolved_type =
      ast_substitute_type(node->resolved_type, ctx, template_type, args);

  switch (node->tag) {
  case NODE_AST_ROOT:
    copy->ast_root.children =
        ast_clone_node_list(&node->ast_root.children, ctx, template_type, args);
    break;

  case NODE_VAR_DECL:
    copy->var_decl.name =
        ast_clone_node(node->var_decl.name, ctx, template_type, args);
    copy->var_decl.value =
        ast_clone_node(node->var_decl.value, ctx, template_type, args);
    copy->var_decl.declared_type = ast_substitute_type(
        node->var_decl.declared_type, ctx, template_type, args);
    copy->var_decl.is_const = node->var_decl.is_const;
    copy->var_decl.is_export = node->var_decl.is_export;
    break;

  case NODE_PRINT_STMT:
    copy->print_stmt.values =
        ast_clone_node_list(&node->print_stmt.values, ctx, template_type, args);
    break;

  case NODE_NUMBER:
    copy->number.value = node->number.value;
    copy->number.raw_text = node->number.raw_text;
    break;

  case NODE_CHAR:
    copy->char_lit.value = node->char_lit.value;
    break;

  case NODE_BOOL:
    copy->boolean.value = node->boolean.value;
    break;

  case NODE_STRING:
    copy->string.value = node->string.value;
    break;

  case NODE_VAR:
    copy->var.value = node->var.value;
    break;

  case NODE_BINARY_ARITH:
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    copy->binary_arith.left =
        ast_clone_node(node->binary_arith.left, ctx, template_type, args);
    copy->binary_arith.right =
        ast_clone_node(node->binary_arith.right, ctx, template_type, args);
    copy->binary_arith.op = node->binary_arith.op;
    if (node->tag == NODE_BINARY_COMPARE)
      copy->binary_compare.op = node->binary_compare.op;
    if (node->tag == NODE_BINARY_EQUALITY)
      copy->binary_equality.op = node->binary_equality.op;
    if (node->tag == NODE_BINARY_LOGICAL)
      copy->binary_logical.op = node->binary_logical.op;
    break;

  case NODE_UNARY:
    copy->unary.op = node->unary.op;
    copy->unary.expr =
        ast_clone_node(node->unary.expr, ctx, template_type, args);
    break;

  case NODE_CAST_EXPR:
    copy->cast_expr.expr =
        ast_clone_node(node->cast_expr.expr, ctx, template_type, args);
    copy->cast_expr.target_type = ast_substitute_type(
        node->cast_expr.target_type, ctx, template_type, args);
    break;

  case NODE_ASSIGN_EXPR:
    copy->assign_expr.target =
        ast_clone_node(node->assign_expr.target, ctx, template_type, args);
    copy->assign_expr.value =
        ast_clone_node(node->assign_expr.value, ctx, template_type, args);
    break;

  case NODE_EXPR_STMT:
    copy->expr_stmt.expr =
        ast_clone_node(node->expr_stmt.expr, ctx, template_type, args);
    break;

  case NODE_TERNARY:
    copy->ternary.condition =
        ast_clone_node(node->ternary.condition, ctx, template_type, args);
    copy->ternary.true_expr =
        ast_clone_node(node->ternary.true_expr, ctx, template_type, args);
    copy->ternary.false_expr =
        ast_clone_node(node->ternary.false_expr, ctx, template_type, args);
    break;

  case NODE_CALL:
    copy->call.func = ast_clone_node(node->call.func, ctx, template_type, args);
    copy->call.args =
        ast_clone_node_list(&node->call.args, ctx, template_type, args);
    break;

  case NODE_RETURN_STMT:
    copy->return_stmt.expr =
        ast_clone_node(node->return_stmt.expr, ctx, template_type, args);
    break;

  case NODE_PARAM:
    copy->param.name =
        ast_clone_node(node->param.name, ctx, template_type, args);
    copy->param.type =
        ast_substitute_type(node->param.type, ctx, template_type, args);
    break;

  case NODE_BLOCK:
    copy->block.statements =
        ast_clone_node_list(&node->block.statements, ctx, template_type, args);
    break;

  case NODE_FIELD:
    copy->field.object =
        ast_clone_node(node->field.object, ctx, template_type, args);
    copy->field.field = node->field.field;
    break;

  case NODE_STATIC_MEMBER:
    copy->static_member.parent = node->static_member.parent;
    copy->static_member.member = node->static_member.member;
    break;

  case NODE_INDEX:
    copy->index.array =
        ast_clone_node(node->index.array, ctx, template_type, args);
    copy->index.index =
        ast_clone_node(node->index.index, ctx, template_type, args);
    break;

  case NODE_FUNC_DECL:
    copy->func_decl.name =
        ast_clone_node(node->func_decl.name, ctx, template_type, args);
    copy->func_decl.params =
        ast_clone_node_list(&node->func_decl.params, ctx, template_type, args);
    copy->func_decl.body =
        ast_clone_node(node->func_decl.body, ctx, template_type, args);
    copy->func_decl.return_type = ast_substitute_type(
        node->func_decl.return_type, ctx, template_type, args);
    copy->func_decl.is_static = node->func_decl.is_static;
    copy->func_decl.is_export = node->func_decl.is_export;
    copy->func_decl.is_external = node->func_decl.is_external;
    break;

  case NODE_STRUCT_DECL:
    copy->struct_decl.name =
        ast_clone_node(node->struct_decl.name, ctx, template_type, args);
    copy->struct_decl.members = ast_clone_node_list(&node->struct_decl.members,
                                                    ctx, template_type, args);
    List_init(&copy->struct_decl.placeholders);
    for (size_t i = 0; i < node->struct_decl.placeholders.len; i++) {
      List_push(&copy->struct_decl.placeholders,
                node->struct_decl.placeholders.items[i]);
    }
    copy->struct_decl.is_frozen = node->struct_decl.is_frozen;
    copy->struct_decl.is_export = node->struct_decl.is_export;
    break;

  case NODE_UNION_DECL:
    copy->union_decl.name =
        ast_clone_node(node->union_decl.name, ctx, template_type, args);
    copy->union_decl.members = ast_clone_node_list(&node->union_decl.members,
                                                   ctx, template_type, args);
    List_init(&copy->union_decl.placeholders);
    for (size_t i = 0; i < node->union_decl.placeholders.len; i++) {
      List_push(&copy->union_decl.placeholders,
                node->union_decl.placeholders.items[i]);
    }
    copy->union_decl.is_frozen = node->union_decl.is_frozen;
    copy->union_decl.is_export = node->union_decl.is_export;
    break;

  case NODE_IMPL_DECL:
    copy->impl_decl.type =
        ast_substitute_type(node->impl_decl.type, ctx, template_type, args);
    copy->impl_decl.members =
        ast_clone_node_list(&node->impl_decl.members, ctx, template_type, args);
    break;

  case NODE_ARRAY_LITERAL:
    copy->array_literal.items = ast_clone_node_list(&node->array_literal.items,
                                                    ctx, template_type, args);
    break;

  case NODE_IF_STMT:
    copy->if_stmt.condition =
        ast_clone_node(node->if_stmt.condition, ctx, template_type, args);
    copy->if_stmt.then_branch =
        ast_clone_node(node->if_stmt.then_branch, ctx, template_type, args);
    copy->if_stmt.else_branch =
        ast_clone_node(node->if_stmt.else_branch, ctx, template_type, args);
    break;

  case NODE_SWITCH_STMT:
    copy->switch_stmt.expr =
        ast_clone_node(node->switch_stmt.expr, ctx, template_type, args);
    copy->switch_stmt.cases =
        ast_clone_node_list(&node->switch_stmt.cases, ctx, template_type, args);
    break;

  case NODE_CASE:
    copy->case_stmt.pattern =
        ast_clone_node(node->case_stmt.pattern, ctx, template_type, args);
    copy->case_stmt.body =
        ast_clone_node(node->case_stmt.body, ctx, template_type, args);
    break;

  case NODE_DEFER:
    copy->defer.expr =
        ast_clone_node(node->defer.expr, ctx, template_type, args);
    break;

  case NODE_LOOP_STMT:
    copy->loop.expr = ast_clone_node(node->loop.expr, ctx, template_type, args);
    break;

  case NODE_WHILE_STMT:
    copy->while_stmt.condition =
        ast_clone_node(node->while_stmt.condition, ctx, template_type, args);
    copy->while_stmt.body =
        ast_clone_node(node->while_stmt.body, ctx, template_type, args);
    break;

  case NODE_FOR_STMT:
    copy->for_stmt.init =
        ast_clone_node(node->for_stmt.init, ctx, template_type, args);
    copy->for_stmt.condition =
        ast_clone_node(node->for_stmt.condition, ctx, template_type, args);
    copy->for_stmt.increment =
        ast_clone_node(node->for_stmt.increment, ctx, template_type, args);
    copy->for_stmt.body =
        ast_clone_node(node->for_stmt.body, ctx, template_type, args);
    break;

  case NODE_FOR_IN_STMT:
    copy->for_in_stmt.var =
        ast_clone_node(node->for_in_stmt.var, ctx, template_type, args);
    copy->for_in_stmt.iterable =
        ast_clone_node(node->for_in_stmt.iterable, ctx, template_type, args);
    copy->for_in_stmt.body =
        ast_clone_node(node->for_in_stmt.body, ctx, template_type, args);
    break;

  case NODE_ARRAY_REPEAT:
    copy->array_repeat.value =
        ast_clone_node(node->array_repeat.value, ctx, template_type, args);
    copy->array_repeat.count =
        ast_clone_node(node->array_repeat.count, ctx, template_type, args);
    break;

  case NODE_INTRINSIC_COMPARE:
    copy->intrinsic_compare.left =
        ast_clone_node(node->intrinsic_compare.left, ctx, template_type, args);
    copy->intrinsic_compare.right =
        ast_clone_node(node->intrinsic_compare.right, ctx, template_type, args);
    copy->intrinsic_compare.op = node->intrinsic_compare.op;
    break;

  default:
    panic("Unsupported AST node clone kind: %d", node->tag);
  }

  return copy;
}

static List ast_clone_node_list(List *nodes, TypeContext *ctx,
                                Type *template_type, List args) {
  List out;
  List_init(&out);
  for (size_t i = 0; i < nodes->len; i++) {
    AstNode *child = nodes->items[i];
    AstNode *clone = ast_clone_node(child, ctx, template_type, args);
    List_push(&out, clone);
  }
  return out;
}

static Symbol *sema_instantiate_method_symbol(Sema *s, Type *obj_type,
                                              Symbol *method,
                                              AstNode *field_node) {
  if (!obj_type || !obj_type->data.instance.from_template)
    return method;

  Type *template_type = obj_type->data.instance.from_template;
  StringView original_name =
      method->original_name.len ? method->original_name : method->name;
  StringView concrete_name = sema_mangle_method_name(
      sv_from_cstr(type_to_name(obj_type)), original_name);

  Symbol *existing = sema_resolve(s, concrete_name);
  if (existing)
    return existing;

  Type *concrete_return_type =
      type_resolve_placeholders(s->types, method->type, template_type,
                                obj_type->data.instance.generic_args);
  Symbol *alias = sema_define(s, concrete_name, concrete_return_type, true,
                              field_node->loc);
  if (!alias)
    return NULL;

  alias->original_name = original_name;
  alias->kind = method->kind;
  alias->is_export = false;
  alias->func_status = FUNC_IMPLEMENTATION;

  if (method->value && method->value->tag == NODE_FUNC_DECL) {
    AstNode *concrete_fn =
        ast_clone_node(method->value, s->types, template_type,
                       obj_type->data.instance.generic_args);
    if (concrete_fn->func_decl.name)
      concrete_fn->func_decl.name->var.value = concrete_name;

    sema_check_stmt(s, concrete_fn);
    alias->value = concrete_fn;
    List_push(&s->types->instantiated_functions, concrete_fn);
  } else {
    alias->value = method->value;
  }

  return alias;
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

  // Auto-dereference for member access (standard in many languages)
  while (obj_type->kind == KIND_POINTER) {
    obj_type = obj_type->data.pointer_to;
  }

  if (obj_type->kind != KIND_STRUCT && obj_type->kind != KIND_UNION &&
      obj_type->kind != KIND_TEMPLATE) {
    sema_error(s, node->field.object,
               "Member access only allowed on structs or unions, got %s",
               type_to_name(obj_type));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  // 1. Is it a field?
  Member *m = type_get_member(obj_type, node->field.field);
  if (m) {
    node->resolved_type = m->type;
    return m->type;
  }

  if (obj_type->kind == KIND_UNION) {
    Type *owner = NULL;
    m = type_find_union_field(obj_type, node->field.field, &owner);
    if (m) {
      node->resolved_type = m->type;
      return m->type;
    }
  }

  // 2. Is it a method?
  for (size_t i = 0; i < obj_type->methods.len; i++) {
    Symbol *method = obj_type->methods.items[i];
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, node->field.field)) {
      if (method->kind == SYM_METHOD) {
        node->resolved_type = method->type;
        return method->type;
      }
    }
  }

  if (obj_type->data.instance.from_template) {
    Type *template_type = obj_type->data.instance.from_template;
    for (size_t i = 0; i < template_type->methods.len; i++) {
      Symbol *method = template_type->methods.items[i];
      StringView lookup_name =
          method->original_name.len ? method->original_name : method->name;
      if (sv_eq(lookup_name, node->field.field)) {
        if (method->kind == SYM_METHOD) {
          node->resolved_type = method->type;
          return method->type;
        }
      }
    }
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

  if (array_type->kind == KIND_POINTER) {
    if (!type_is_numeric(index_type)) {
      sema_error(s, index_node, "Array index must be numeric, got %s",
                 type_to_name(index_type));
    }
    return array_type->data.pointer_to;
  }

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

Type *sema_find_type_by_name(Sema *s, StringView name);

static Type *check_call(Sema *s, AstNode *node) {
  // If it's a method call: instance.method(args)
  if (node->call.func->tag == NODE_FIELD) {
    AstNode *field_node = node->call.func;
    Type *orig_obj_type = sema_check_expr(s, field_node->field.object);
    Type *obj_type = orig_obj_type;

    // Auto-dereference
    while (obj_type->kind == KIND_POINTER) {
      obj_type = obj_type->data.pointer_to;
    }

    if (obj_type->kind == KIND_STRUCT || obj_type->kind == KIND_TEMPLATE ||
        (obj_type->kind == KIND_PRIMITIVE &&
         obj_type->data.primitive == PRIM_STRING)) {
      for (size_t i = 0; i < obj_type->methods.len; i++) {
        Symbol *method = obj_type->methods.items[i];
        StringView lookup_name =
            method->original_name.len ? method->original_name : method->name;
        if (sv_eq(lookup_name, field_node->field.field) &&
            method->kind == SYM_METHOD) {
          Symbol *concrete_method =
              sema_instantiate_method_symbol(s, obj_type, method, field_node);
          if (!concrete_method)
            continue;

          // Transformation: method(instance, args...)
          List new_args;
          List_init(&new_args);

          AstNode *self_arg = field_node->field.object;
          if (concrete_method->value &&
              concrete_method->value->tag == NODE_FUNC_DECL &&
              concrete_method->value->func_decl.params.len > 0) {
            AstNode *first_param =
                concrete_method->value->func_decl.params.items[0];
            Type *param_type = first_param->param.type;
            if (param_type->kind == KIND_POINTER &&
                orig_obj_type->kind != KIND_POINTER &&
                type_equals(param_type->data.pointer_to, orig_obj_type)) {
              self_arg = AstNode_new_unary(OP_ADDR_OF, field_node->field.object,
                                           field_node->loc);
            }
          }

          // Prepend self
          List_push(&new_args, self_arg);
          // Add remaining args
          for (size_t j = 0; j < node->call.args.len; j++) {
            List_push(&new_args, node->call.args.items[j]);
          }

          node->call.func =
              AstNode_new_var(concrete_method->name, field_node->loc);
          node->call.args = new_args;

          return check_call(s, node); // Recursively check the transformed call
        }
      }

      if (obj_type->data.instance.from_template) {
        Type *template_type = obj_type->data.instance.from_template;
        for (size_t i = 0; i < template_type->methods.len; i++) {
          Symbol *method = template_type->methods.items[i];
          StringView lookup_name =
              method->original_name.len ? method->original_name : method->name;
          if (sv_eq(lookup_name, field_node->field.field) &&
              method->kind == SYM_METHOD) {
            Symbol *concrete_method =
                sema_instantiate_method_symbol(s, obj_type, method, field_node);
            if (!concrete_method)
              continue;

            List new_args;
            List_init(&new_args);

            AstNode *self_arg = field_node->field.object;
            if (concrete_method->value &&
                concrete_method->value->tag == NODE_FUNC_DECL &&
                concrete_method->value->func_decl.params.len > 0) {
              AstNode *first_param =
                  concrete_method->value->func_decl.params.items[0];
              Type *param_type = first_param->param.type;
              if (param_type->kind == KIND_POINTER &&
                  orig_obj_type->kind != KIND_POINTER &&
                  type_equals(param_type->data.pointer_to, orig_obj_type)) {
                self_arg = AstNode_new_unary(
                    OP_ADDR_OF, field_node->field.object, field_node->loc);
              }
            }

            List_push(&new_args, self_arg);
            for (size_t j = 0; j < node->call.args.len; j++) {
              List_push(&new_args, node->call.args.items[j]);
            }

            node->call.func =
                AstNode_new_var(concrete_method->name, field_node->loc);
            node->call.args = new_args;

            return check_call(s, node);
          }
        }
      }
    }
  }

  if (node->call.func->tag == NODE_STATIC_MEMBER) {
    AstNode *sm = node->call.func;
    Symbol *parent = sema_resolve(s, sm->static_member.parent);
    Type *parent_type =
        parent ? parent->type
               : sema_find_type_by_name(s, sm->static_member.parent);
    if (parent_type && (parent_type->kind == KIND_STRUCT ||
                        parent_type->kind == KIND_TEMPLATE)) {
      for (size_t i = 0; i < parent_type->methods.len; i++) {
        Symbol *method = parent_type->methods.items[i];
        if (method->kind != SYM_STATIC_METHOD)
          continue;
        StringView lookup_name =
            method->original_name.len ? method->original_name : method->name;
        if (sv_eq(lookup_name, sm->static_member.member)) {
          if (!sema_resolve(s, method->name)) {
            Symbol *alias =
                sema_define(s, method->name, method->type, true, sm->loc);
            if (alias) {
              alias->original_name = method->original_name;
              alias->value = method->value;
              alias->kind = method->kind;
              alias->is_export = false;
            }
          }
          node->call.func = AstNode_new_var(method->name, sm->loc);
          return check_call(s, node);
        }
      }
    }
  }

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
        AstNode *arg_node = node->call.args.items[i];
        Type *arg_type = sema_check_expr(s, arg_node);
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

  // Monomorphize: Create Array<element_type> with fixed length for literals
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
  List_free(&args, 0); // Cleanup temporary list

  // Assign the resolved type on the literal/repeat node itself so codegen
  // can rely on it without needing extra inference later.
  if (node) {
    node->resolved_type = instance;
  }

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

  Type *rhs = sema_coerce(s, node->assign_expr.value, lhs);
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

Type *sema_find_type_by_name(Sema *s, StringView name) {
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
    if (sv_eq_cstr(name, prim_name) ||
        (i == PRIM_STRING && sv_eq_cstr(name, "String"))) {
      return t;
    }
  }

  return NULL;
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

static Type *check_static_member(Sema *s, AstNode *node) {
  Symbol *parent = sema_resolve(s, node->static_member.parent);
  Type *type = NULL;
  if (parent) {
    type = parent->type;
  } else {
    type = sema_find_type_by_name(s, node->static_member.parent);
  }

  if (!type || (type->kind != KIND_STRUCT && type->kind != KIND_TEMPLATE &&
                !(type->kind == KIND_PRIMITIVE &&
                  type->data.primitive == PRIM_STRING))) {
    sema_error(s, node, "Undefined type '" SV_FMT "'",
               SV_ARG(node->static_member.parent));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  for (size_t i = 0; i < type->methods.len; i++) {
    Symbol *method = type->methods.items[i];
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, node->static_member.member)) {
      if (method->kind == SYM_STATIC_METHOD) {
        return method->type;
      }
    }
  }

  sema_error(s, node, "Type %s has no static member '" SV_FMT "'",
             type_to_name(type), SV_ARG(node->static_member.member));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}

Type *sema_coerce(Sema *s, AstNode *expr, Type *target) {
  Type *expr_type = sema_check_expr(s, expr);
  bool target_is_tagged_union =
      target && target->kind == KIND_UNION && target->is_tagged_union;

  // Literal Bounds Check
  if (expr->tag == NODE_NUMBER || expr->tag == NODE_CHAR) {
    check_literal_bounds(s, expr, target);

    if (type_can_implicitly_cast(target, expr_type) ||
        (expr->tag == NODE_NUMBER && literal_fits_in_type(expr, target))) {
      if (!target_is_tagged_union) {
        expr->resolved_type = target;
      }
      return target;
    }
  }

  // Exact match or implicit cast
  if (type_can_implicitly_cast(target, expr_type)) {
    if (!target_is_tagged_union) {
      expr->resolved_type = target;
    }
    return target;
  }

  // Inference support
  if (target->kind == KIND_PRIMITIVE &&
      target->data.primitive == PRIM_UNKNOWN) {
    expr->resolved_type = expr_type;
    return expr_type;
  }

  sema_error(s, expr, "Type mismatch: expected %s, got %s",
             type_to_name(target), type_to_name(expr_type));
  expr->resolved_type = expr_type;
  return expr_type;
}

Type *sema_check_expr(Sema *s, AstNode *node) {
  if (!node)
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  if (node->resolved_type) {
    return node->resolved_type;
  }

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

  case NODE_STATIC_MEMBER:
    type = check_static_member(s, node);
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
    type = check_array_expr(s, node);
    break;
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