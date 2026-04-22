#include "sema_internal.h"
#include "tyna/ast.h"

static Type *ast_substitute_type(Type *type, TypeContext *ctx,
                                 Type *template_type, List args) {
  if (!type || !ctx || !template_type)
    return type;
  return type_resolve_placeholders(ctx, type, template_type, args);
}

static AstNode *ast_clone_node(AstNode *node, TypeContext *ctx,
                               Type *template_type, List args);

static List ast_clone_node_list(List *nodes, TypeContext *ctx,
                                Type *template_type, List args) {
  List out;
  List_init(&out);
  for (size_t i = 0; i < nodes->len; i++) {
    AstNode *child = nodes->items[i];
    AstNode *clone = NULL;
    if (child)
      clone = ast_clone_node(child, ctx, template_type, args);
    List_push(&out, clone);
  }
  return out;
}

AstNode *ast_clone_node(AstNode *node, TypeContext *ctx, Type *template_type,
                        List args) {
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

Symbol *sema_instantiate_method_symbol(Sema *s, Type *obj_type, Symbol *method,
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
