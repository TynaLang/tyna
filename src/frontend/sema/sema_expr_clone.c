#include "sema_internal.h"
#include "tyna/ast.h"

static Type *ast_substitute_type(Type *type, TypeContext *ctx,
                                 Type *template_type, List args) {
  if (!type || !ctx)
    return type;

  // First, try to resolve placeholders using the given template
  if (template_type && template_type->kind == KIND_TEMPLATE) {
    Type *old_type = type;
    type = type_resolve_placeholders(ctx, type, template_type, args);
  }

  // Handle pointer types that point to struct instances with from_template.
  // The pointed-to type may have placeholders that need resolving.
  if (type && type->kind == KIND_POINTER && type->data.pointer_to &&
      type->data.pointer_to->kind == KIND_STRUCT &&
      type->data.pointer_to->data.instance.from_template) {
    Type *inner =
        ast_substitute_type(type->data.pointer_to, ctx, template_type, args);
    if (inner != type->data.pointer_to) {
      type = type_get_pointer(ctx, inner);
    }
    return type;
  }

  // If the type is a struct instance with its own template that has
  // placeholders, try to resolve those too. The instance's own generic
  // args may contain placeholders from the enclosing template. We need
  // to resolve each generic arg using the enclosing template first,
  // then create a new instance with the fully resolved args.
  if (type && type->kind == KIND_STRUCT && type->data.instance.from_template) {
    Type *inner_template = type->data.instance.from_template;
    if (inner_template->kind == KIND_TEMPLATE &&
        inner_template->data.template.placeholders.len > 0) {
      // Try resolving each generic arg individually using the enclosing
      // template, then create a new instance with the resolved args.
      bool changed = false;
      List resolved_args;
      List_init(&resolved_args);
      for (size_t i = 0; i < type->data.instance.generic_args.len; i++) {
        Type *arg = type->data.instance.generic_args.items[i];
        Type *resolved_arg =
            type_resolve_placeholders(ctx, arg, template_type, args);
        // If the arg is still a placeholder that didn't match the enclosing
        // template, try resolving it against any concrete nested instance that
        // was already substituted into the outer template args.
        if (resolved_arg == arg && arg->kind == KIND_TEMPLATE) {
          for (size_t j = 0; j < args.len; j++) {
            Type *candidate = args.items[j];
            if (!candidate || candidate->kind != KIND_STRUCT ||
                !candidate->data.instance.from_template ||
                candidate->data.instance.from_template->kind != KIND_TEMPLATE)
              continue;

            Type *candidate_resolved = type_resolve_placeholders(
                ctx, arg, candidate->data.instance.from_template,
                candidate->data.instance.generic_args);
            if (candidate_resolved != arg) {
              resolved_arg = candidate_resolved;
              break;
            }
          }
        }
        List_push(&resolved_args, resolved_arg);
        if (resolved_arg != arg)
          changed = true;
      }
      if (changed) {
        type = type_get_instance(ctx, inner_template, resolved_args);
      }
      List_free(&resolved_args, 0);
    }
  }

  return type;
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

  case NODE_NULL:
  case NODE_NONE:
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

  case NODE_BINARY_IS:
    copy->binary_is.left =
        ast_clone_node(node->binary_is.left, ctx, template_type, args);
    copy->binary_is.right =
        ast_clone_node(node->binary_is.right, ctx, template_type, args);
    break;

  case NODE_BINARY_ELSE:
    copy->binary_else.left =
        ast_clone_node(node->binary_else.left, ctx, template_type, args);
    copy->binary_else.right =
        ast_clone_node(node->binary_else.right, ctx, template_type, args);
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

  case NODE_SIZEOF_EXPR:
    copy->sizeof_expr.target_type = ast_substitute_type(
        node->sizeof_expr.target_type, ctx, template_type, args);
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
    if (copy->call.func)
      copy->call.func->resolved_type = NULL;
    List_init(&copy->call.generic_args);
    for (size_t i = 0; i < node->call.generic_args.len; i++) {
      Type *generic_arg = node->call.generic_args.items[i];
      List_push(&copy->call.generic_args,
                ast_substitute_type(generic_arg, ctx, template_type, args));
    }
    break;

  case NODE_NEW_EXPR:
    copy->new_expr.target_type = ast_substitute_type(node->new_expr.target_type,
                                                     ctx, template_type, args);
    copy->new_expr.args =
        ast_clone_node_list(&node->new_expr.args, ctx, template_type, args);
    copy->new_expr.field_inits = ast_clone_node_list(
        &node->new_expr.field_inits, ctx, template_type, args);
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

  case NODE_GENERIC_TYPE:
    copy->generic_type.base =
        ast_clone_node(node->generic_type.base, ctx, template_type, args);
    List_init(&copy->generic_type.generic_args);
    for (size_t i = 0; i < node->generic_type.generic_args.len; i++) {
      Type *generic_arg = node->generic_type.generic_args.items[i];
      List_push(&copy->generic_type.generic_args,
                ast_substitute_type(generic_arg, ctx, template_type, args));
    }
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

  case NODE_ERROR_DECL:
    copy->error_decl.name =
        ast_clone_node(node->error_decl.name, ctx, template_type, args);
    copy->error_decl.members = ast_clone_node_list(&node->error_decl.members,
                                                   ctx, template_type, args);
    copy->error_decl.message = node->error_decl.message;
    copy->error_decl.is_export = node->error_decl.is_export;
    break;

  case NODE_ERROR_SET_DECL:
    copy->error_set_decl.name =
        ast_clone_node(node->error_set_decl.name, ctx, template_type, args);
    copy->error_set_decl.members = ast_clone_node_list(
        &node->error_set_decl.members, ctx, template_type, args);
    copy->error_set_decl.is_export = node->error_set_decl.is_export;
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

  case NODE_TYPE_ALIAS:
    copy->type_alias_decl.name =
        ast_clone_node(node->type_alias_decl.name, ctx, template_type, args);
    copy->type_alias_decl.target_type = ast_substitute_type(
        node->type_alias_decl.target_type, ctx, template_type, args);
    copy->type_alias_decl.is_export = node->type_alias_decl.is_export;
    break;

  case NODE_IMPORT:
    copy->import.mode = node->import.mode;
    copy->import.path = node->import.path;
    copy->import.alias = node->import.alias;
    List_init(&copy->import.symbols);
    for (size_t i = 0; i < node->import.symbols.len; i++) {
      ImportSymbol *src = node->import.symbols.items[i];
      ImportSymbol *dst = xmalloc(sizeof(ImportSymbol));
      dst->name = src->name;
      dst->alias = src->alias;
      List_push(&copy->import.symbols, dst);
    }
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

  case NODE_BREAK:
  case NODE_CONTINUE:
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

// Walk the AST and re-substitute resolved_type fields in-place.
// This is used after sema_check_func_decl to fix types that were
// overwritten with generic template types from the symbol table.
static void ast_substitute_types_inplace(AstNode *node, TypeContext *ctx,
                                         Type *template_type, List args) {
  if (!node)
    return;

  node->resolved_type =
      ast_substitute_type(node->resolved_type, ctx, template_type, args);

  switch (node->tag) {
  case NODE_AST_ROOT:
    for (size_t i = 0; i < node->ast_root.children.len; i++)
      ast_substitute_types_inplace(node->ast_root.children.items[i], ctx,
                                   template_type, args);
    break;
  case NODE_VAR_DECL:
    ast_substitute_types_inplace(node->var_decl.name, ctx, template_type, args);
    ast_substitute_types_inplace(node->var_decl.value, ctx, template_type,
                                 args);
    node->var_decl.declared_type = ast_substitute_type(
        node->var_decl.declared_type, ctx, template_type, args);
    break;
  case NODE_PRINT_STMT:
    for (size_t i = 0; i < node->print_stmt.values.len; i++)
      ast_substitute_types_inplace(node->print_stmt.values.items[i], ctx,
                                   template_type, args);
    break;
  case NODE_BINARY_ARITH:
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    ast_substitute_types_inplace(node->binary_arith.left, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->binary_arith.right, ctx, template_type,
                                 args);
    break;
  case NODE_BINARY_IS:
    ast_substitute_types_inplace(node->binary_is.left, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->binary_is.right, ctx, template_type,
                                 args);
    break;
  case NODE_BINARY_ELSE:
    ast_substitute_types_inplace(node->binary_else.left, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->binary_else.right, ctx, template_type,
                                 args);
    break;
  case NODE_UNARY:
    ast_substitute_types_inplace(node->unary.expr, ctx, template_type, args);
    break;
  case NODE_CAST_EXPR:
    ast_substitute_types_inplace(node->cast_expr.expr, ctx, template_type,
                                 args);
    node->cast_expr.target_type = ast_substitute_type(
        node->cast_expr.target_type, ctx, template_type, args);
    break;
  case NODE_SIZEOF_EXPR:
    node->sizeof_expr.target_type = ast_substitute_type(
        node->sizeof_expr.target_type, ctx, template_type, args);
    break;
  case NODE_ASSIGN_EXPR:
    ast_substitute_types_inplace(node->assign_expr.target, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->assign_expr.value, ctx, template_type,
                                 args);
    break;
  case NODE_EXPR_STMT:
    ast_substitute_types_inplace(node->expr_stmt.expr, ctx, template_type,
                                 args);
    break;
  case NODE_TERNARY:
    ast_substitute_types_inplace(node->ternary.condition, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->ternary.true_expr, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->ternary.false_expr, ctx, template_type,
                                 args);
    break;
  case NODE_CALL:
    ast_substitute_types_inplace(node->call.func, ctx, template_type, args);
    for (size_t i = 0; i < node->call.args.len; i++)
      ast_substitute_types_inplace(node->call.args.items[i], ctx, template_type,
                                   args);
    break;
  case NODE_NEW_EXPR:
    node->new_expr.target_type = ast_substitute_type(node->new_expr.target_type,
                                                     ctx, template_type, args);
    for (size_t i = 0; i < node->new_expr.args.len; i++)
      ast_substitute_types_inplace(node->new_expr.args.items[i], ctx,
                                   template_type, args);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++)
      ast_substitute_types_inplace(node->new_expr.field_inits.items[i], ctx,
                                   template_type, args);
    break;
  case NODE_RETURN_STMT:
    ast_substitute_types_inplace(node->return_stmt.expr, ctx, template_type,
                                 args);
    break;
  case NODE_PARAM:
    ast_substitute_types_inplace(node->param.name, ctx, template_type, args);
    node->param.type =
        ast_substitute_type(node->param.type, ctx, template_type, args);
    break;
  case NODE_BLOCK:
    for (size_t i = 0; i < node->block.statements.len; i++)
      ast_substitute_types_inplace(node->block.statements.items[i], ctx,
                                   template_type, args);
    break;
  case NODE_FIELD:
    ast_substitute_types_inplace(node->field.object, ctx, template_type, args);
    break;
  case NODE_INDEX:
    ast_substitute_types_inplace(node->index.array, ctx, template_type, args);
    ast_substitute_types_inplace(node->index.index, ctx, template_type, args);
    break;
  case NODE_FUNC_DECL:
    ast_substitute_types_inplace(node->func_decl.name, ctx, template_type,
                                 args);
    for (size_t i = 0; i < node->func_decl.params.len; i++)
      ast_substitute_types_inplace(node->func_decl.params.items[i], ctx,
                                   template_type, args);
    ast_substitute_types_inplace(node->func_decl.body, ctx, template_type,
                                 args);
    node->func_decl.return_type = ast_substitute_type(
        node->func_decl.return_type, ctx, template_type, args);
    break;
  case NODE_IF_STMT:
    ast_substitute_types_inplace(node->if_stmt.condition, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->if_stmt.then_branch, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->if_stmt.else_branch, ctx, template_type,
                                 args);
    break;
  case NODE_SWITCH_STMT:
    ast_substitute_types_inplace(node->switch_stmt.expr, ctx, template_type,
                                 args);
    for (size_t i = 0; i < node->switch_stmt.cases.len; i++)
      ast_substitute_types_inplace(node->switch_stmt.cases.items[i], ctx,
                                   template_type, args);
    break;
  case NODE_CASE:
    ast_substitute_types_inplace(node->case_stmt.pattern, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->case_stmt.body, ctx, template_type,
                                 args);
    break;
  case NODE_DEFER:
    ast_substitute_types_inplace(node->defer.expr, ctx, template_type, args);
    break;
  case NODE_LOOP_STMT:
    ast_substitute_types_inplace(node->loop.expr, ctx, template_type, args);
    break;
  case NODE_WHILE_STMT:
    ast_substitute_types_inplace(node->while_stmt.condition, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->while_stmt.body, ctx, template_type,
                                 args);
    break;
  case NODE_FOR_STMT:
    ast_substitute_types_inplace(node->for_stmt.init, ctx, template_type, args);
    ast_substitute_types_inplace(node->for_stmt.condition, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->for_stmt.increment, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->for_stmt.body, ctx, template_type, args);
    break;
  case NODE_FOR_IN_STMT:
    ast_substitute_types_inplace(node->for_in_stmt.var, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->for_in_stmt.iterable, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->for_in_stmt.body, ctx, template_type,
                                 args);
    break;
  case NODE_ARRAY_LITERAL:
    for (size_t i = 0; i < node->array_literal.items.len; i++)
      ast_substitute_types_inplace(node->array_literal.items.items[i], ctx,
                                   template_type, args);
    break;
  case NODE_ARRAY_REPEAT:
    ast_substitute_types_inplace(node->array_repeat.value, ctx, template_type,
                                 args);
    ast_substitute_types_inplace(node->array_repeat.count, ctx, template_type,
                                 args);
    break;
  case NODE_INTRINSIC_COMPARE:
    ast_substitute_types_inplace(node->intrinsic_compare.left, ctx,
                                 template_type, args);
    ast_substitute_types_inplace(node->intrinsic_compare.right, ctx,
                                 template_type, args);
    break;
  default:
    break;
  }
}

Symbol *sema_instantiate_method_symbol(Sema *s, Type *obj_type, Symbol *method,
                                       AstNode *field_node) {
  if (!obj_type || obj_type->kind != KIND_STRUCT ||
      !obj_type->data.instance.from_template ||
      obj_type->data.instance.from_template->kind != KIND_TEMPLATE)
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

    // After cloning, materialize all struct instances in sizeof expressions
    // to ensure their sizes are computed correctly. This is needed because
    // the cloned function may contain sizeof(T) where T is a struct instance
    // with placeholder generic args that were resolved by ast_substitute_type,
    // but the resolved type's size may not have been computed yet.
    {
      // Walk the cloned function body looking for sizeof expressions
      AstNode *body = concrete_fn->func_decl.body;
      if (body && body->tag == NODE_BLOCK) {
        for (size_t i = 0; i < body->block.statements.len; i++) {
          AstNode *stmt = body->block.statements.items[i];
          if (stmt && stmt->tag == NODE_EXPR_STMT && stmt->expr_stmt.expr) {
            AstNode *expr = stmt->expr_stmt.expr;
            if (expr->tag == NODE_CALL) {
              for (size_t j = 0; j < expr->call.args.len; j++) {
                AstNode *arg = expr->call.args.items[j];
                if (arg && arg->tag == NODE_SIZEOF_EXPR &&
                    arg->sizeof_expr.target_type) {
                  Type *target = arg->sizeof_expr.target_type;
                  if (target->kind == KIND_STRUCT &&
                      target->data.instance.from_template &&
                      target->size == 0) {
                    type_materialize_instances_from_template(
                        s->types, target->data.instance.from_template);
                  }
                }
              }
            }
          }
        }
      }
    }

    if (s->pass == SEMA_PASS_BODIES) {
      bool old_ignore_cached_types = s->ignore_cached_types;
      s->ignore_cached_types = true;
      sema_check_func_decl(s, concrete_fn);
      s->ignore_cached_types = old_ignore_cached_types;

      // Re-substitute types after re-sema, since sema_check_func_decl may
      // have overwritten resolved_type fields with generic template types
      // from the symbol table (e.g., ptr<Map<K, V>> instead of
      // ptr<Map<str, FileId>>). This ensures the codegen uses concrete
      // LLVM types with correct sizes for GEP.
      ast_substitute_types_inplace(concrete_fn, s->types, template_type,
                                   obj_type->data.instance.generic_args);

      // Debug: check if the self parameter type was properly substituted
      if (concrete_fn->func_decl.params.len > 0) {
        AstNode *self_param = concrete_fn->func_decl.params.items[0];
      }
    }

    alias->value = concrete_fn;
    List_push(&s->types->instantiated_functions, concrete_fn);
  } else {
    alias->value = method->value;
  }

  return alias;
}
