#include "tyna/ast.h"

static AstNode *AstNode_new(AstKind ast_kind, Location loc) {
  AstNode *node = xmalloc(sizeof(AstNode));
  node->tag = ast_kind;
  node->loc = loc;
  node->resolved_type = NULL;
  return node;
}

AstNode *AstNode_new_program(Location loc) {
  AstNode *node = AstNode_new(NODE_AST_ROOT, loc);
  List_init(&node->ast_root.children);
  return node;
}

AstNode *AstNode_new_var_decl(AstNode *name, AstNode *value, Type *type,
                              int is_const, bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_VAR_DECL, loc);
  node->var_decl.name = name;
  node->var_decl.value = value;
  node->var_decl.declared_type = type;
  node->var_decl.is_const = is_const;
  node->var_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_print_stmt(List values, Location loc) {
  AstNode *node = AstNode_new(NODE_PRINT_STMT, loc);
  node->print_stmt.values = values;
  return node;
}

AstNode *AstNode_new_number(double value, StringView raw_text, Location loc) {
  AstNode *node = AstNode_new(NODE_NUMBER, loc);
  node->number.value = value;
  node->number.raw_text = raw_text;
  return node;
}

AstNode *AstNode_new_bool(int value, Location loc) {
  AstNode *node = AstNode_new(NODE_BOOL, loc);
  node->boolean.value = value;
  return node;
}

AstNode *AstNode_new_char(char value, Location loc) {
  AstNode *node = AstNode_new(NODE_CHAR, loc);
  node->char_lit.value = value;
  return node;
}

AstNode *AstNode_new_string(StringView value, Location loc) {
  AstNode *node = AstNode_new(NODE_STRING, loc);
  node->string.value = value;
  return node;
}

AstNode *AstNode_new_null(Location loc) {
  AstNode *node = AstNode_new(NODE_NULL, loc);
  return node;
}

AstNode *AstNode_new_var(StringView value, Location loc) {
  AstNode *node = AstNode_new(NODE_VAR, loc);
  node->var.value = value;
  return node;
}

AstNode *AstNode_new_new_expr(Type *target_type, List args, List field_inits,
                              Location loc) {
  AstNode *node = AstNode_new(NODE_NEW_EXPR, loc);
  node->new_expr.target_type = target_type;
  node->new_expr.args = args;
  node->new_expr.field_inits = field_inits;
  return node;
}

AstNode *AstNode_new_array_literal(List items, Location loc) {
  AstNode *node = AstNode_new(NODE_ARRAY_LITERAL, loc);
  node->array_literal.items = items;
  return node;
}

AstNode *AstNode_new_binary_arith(AstNode *left, AstNode *right, ArithmOp op,
                                  Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_ARITH, loc);
  node->binary_arith.left = left;
  node->binary_arith.right = right;
  node->binary_arith.op = op;
  return node;
}

AstNode *AstNode_new_binary_compare(AstNode *left, AstNode *right, CompareOp op,
                                    Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_COMPARE, loc);
  node->binary_compare.left = left;
  node->binary_compare.right = right;
  node->binary_compare.op = op;
  return node;
}

AstNode *AstNode_new_binary_equality(AstNode *left, AstNode *right,
                                     EqualityOp op, Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_EQUALITY, loc);
  node->binary_equality.left = left;
  node->binary_equality.right = right;
  node->binary_equality.op = op;
  return node;
}

AstNode *AstNode_new_binary_logical(AstNode *left, AstNode *right, LogicalOp op,
                                    Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_LOGICAL, loc);
  node->binary_logical.left = left;
  node->binary_logical.right = right;
  node->binary_logical.op = op;
  return node;
}

AstNode *AstNode_new_binary_is(AstNode *left, AstNode *right, Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_IS, loc);
  node->binary_is.left = left;
  node->binary_is.right = right;
  return node;
}

AstNode *AstNode_new_binary_else(AstNode *left, AstNode *right, Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_ELSE, loc);
  node->binary_else.left = left;
  node->binary_else.right = right;
  return node;
}

AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_UNARY, loc);
  node->unary.expr = expr;
  node->unary.op = op;
  return node;
}

AstNode *AstNode_new_cast_expr(AstNode *expr, Type *type, Location loc) {
  AstNode *node = AstNode_new(NODE_CAST_EXPR, loc);
  node->cast_expr.expr = expr;
  node->cast_expr.target_type = type;
  return node;
}

AstNode *AstNode_new_sizeof_expr(Type *target_type, Location loc) {
  AstNode *node = AstNode_new(NODE_SIZEOF_EXPR, loc);
  node->sizeof_expr.target_type = target_type;
  return node;
}

AstNode *AstNode_new_assign_expr(AstNode *target, AstNode *value,
                                 Location loc) {
  AstNode *node = AstNode_new(NODE_ASSIGN_EXPR, loc);
  node->assign_expr.target = target;
  node->assign_expr.value = value;
  return node;
}

AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_EXPR_STMT, loc);
  node->expr_stmt.expr = expr;
  return node;
}

AstNode *AstNode_new_call(AstNode *func, List args, Location loc) {
  AstNode *node = AstNode_new(NODE_CALL, loc);
  node->call.func = func;
  node->call.args = args;
  List_init(&node->call.generic_args);
  return node;
}

AstNode *AstNode_new_func_decl(AstNode *name, List params, Type *ret_type,
                               AstNode *body, bool is_static, bool is_export,
                               bool is_pub_module, bool is_external,
                               Location loc) {
  AstNode *node = AstNode_new(NODE_FUNC_DECL, loc);
  node->func_decl.name = name;
  node->func_decl.params = params;
  node->func_decl.return_type = ret_type;
  node->func_decl.body = body;
  node->func_decl.is_static = is_static;
  node->func_decl.is_export = is_export;
  node->func_decl.is_pub_module = is_pub_module;
  node->func_decl.is_external = is_external;
  node->func_decl.requires_arena = false;
  node->func_decl.consumes_string_arg = false;
  return node;
}

AstNode *AstNode_new_return(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_RETURN_STMT, loc);
  node->return_stmt.expr = expr;
  List_init(&node->return_stmt.symbols_to_drop);
  return node;
}

AstNode *AstNode_new_param(AstNode *name, Type *type, Location loc) {
  AstNode *node = AstNode_new(NODE_PARAM, loc);
  node->param.name = name;
  node->param.type = type;
  node->param.default_value = NULL;
  node->param.requires_storage = false;
  return node;
}

AstNode *AstNode_new_block(Location loc) {
  AstNode *node = AstNode_new(NODE_BLOCK, loc);
  List_init(&node->block.statements);
  List_init(&node->block.symbols_to_drop);
  return node;
}

AstNode *AstNode_new_if_stmt(AstNode *condition, AstNode *then_branch,
                             AstNode *else_branch, Location loc) {
  AstNode *node = AstNode_new(NODE_IF_STMT, loc);
  node->if_stmt.condition = condition;
  node->if_stmt.then_branch = then_branch;
  node->if_stmt.else_branch = else_branch;
  return node;
}

AstNode *AstNode_new_switch_stmt(AstNode *expr, List cases, Location loc) {
  AstNode *node = AstNode_new(NODE_SWITCH_STMT, loc);
  node->switch_stmt.expr = expr;
  node->switch_stmt.cases = cases;
  return node;
}

AstNode *AstNode_new_case_stmt(AstNode *pattern, Type *pattern_type,
                               AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_CASE, loc);
  node->case_stmt.pattern = pattern;
  node->case_stmt.pattern_type = pattern_type;
  node->case_stmt.body = body;
  return node;
}

AstNode *AstNode_new_import(StringView path, StringView alias, ImportMode mode,
                            List symbols, Location loc) {
  AstNode *node = AstNode_new(NODE_IMPORT, loc);
  node->import.path = path;
  node->import.alias = alias;
  node->import.mode = mode;
  node->import.symbols = symbols;
  return node;
}

AstNode *AstNode_new_defer(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_DEFER, loc);
  node->defer.expr = expr;
  return node;
}

AstNode *AstNode_new_loop(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_LOOP_STMT, loc);
  node->loop.expr = expr;
  return node;
}

AstNode *AstNode_new_while(AstNode *condition, AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_WHILE_STMT, loc);
  node->while_stmt.condition = condition;
  node->while_stmt.body = body;
  return node;
}

AstNode *AstNode_new_for(AstNode *init, AstNode *cond, AstNode *inc,
                         AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_FOR_STMT, loc);
  node->for_stmt.init = init;
  node->for_stmt.condition = cond;
  node->for_stmt.increment = inc;
  node->for_stmt.body = body;
  return node;
}

AstNode *AstNode_new_for_in(AstNode *var, AstNode *iterable, AstNode *body,
                            Location loc) {
  AstNode *node = AstNode_new(NODE_FOR_IN_STMT, loc);
  node->for_in_stmt.var = var;
  node->for_in_stmt.iterable = iterable;
  node->for_in_stmt.body = body;
  return node;
}

AstNode *AstNode_new_ternary(AstNode *condition, AstNode *true_expr,
                             AstNode *false_expr, Location loc) {
  AstNode *node = AstNode_new(NODE_TERNARY, loc);
  node->ternary.condition = condition;
  node->ternary.true_expr = true_expr;
  node->ternary.false_expr = false_expr;
  return node;
}

AstNode *AstNode_new_struct_decl(AstNode *name, List members, List placeholders,
                                 bool is_frozen, bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_STRUCT_DECL, loc);
  node->struct_decl.name = name;
  node->struct_decl.members = members;
  node->struct_decl.placeholders = placeholders;
  node->struct_decl.is_frozen = is_frozen;
  node->struct_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_union_decl(AstNode *name, List members, List placeholders,
                                bool is_frozen, bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_UNION_DECL, loc);
  node->union_decl.name = name;
  node->union_decl.members = members;
  node->union_decl.placeholders = placeholders;
  node->union_decl.is_frozen = is_frozen;
  node->union_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_error_decl(AstNode *name, List members, StringView message,
                                bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_ERROR_DECL, loc);
  node->error_decl.name = name;
  node->error_decl.members = members;
  node->error_decl.message = message;
  node->error_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_error_set_decl(AstNode *name, List members, bool is_export,
                                    Location loc) {
  AstNode *node = AstNode_new(NODE_ERROR_SET_DECL, loc);
  node->error_set_decl.name = name;
  node->error_set_decl.members = members;
  node->error_set_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_impl_decl(Type *type, List members, Location loc) {
  AstNode *node = AstNode_new(NODE_IMPL_DECL, loc);
  node->impl_decl.type = type;
  node->impl_decl.members = members;
  return node;
}

AstNode *AstNode_new_type_alias(AstNode *name, Type *target_type, bool is_export,
                                Location loc) {
  AstNode *node = AstNode_new(NODE_TYPE_ALIAS, loc);
  node->type_alias_decl.name = name;
  node->type_alias_decl.target_type = target_type;
  node->type_alias_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_index(AstNode *expr, AstNode *index, Location loc) {
  AstNode *node = AstNode_new(NODE_INDEX, loc);
  node->index.array = expr;
  node->index.index = index;
  return node;
}

AstNode *AstNode_new_field(AstNode *expr, StringView field, Location loc) {
  AstNode *node = AstNode_new(NODE_FIELD, loc);
  node->field.object = expr;
  node->field.field = field;
  return node;
}

AstNode *AstNode_new_static_member(StringView parent, StringView member,
                                   Location loc) {
  AstNode *node = AstNode_new(NODE_STATIC_MEMBER, loc);
  node->static_member.parent = parent;
  node->static_member.member = member;
  return node;
}

AstNode *AstNode_new_array_repeat(AstNode *value, AstNode *count,
                                  Location loc) {
  AstNode *node = AstNode_new(NODE_ARRAY_REPEAT, loc);
  node->array_repeat.value = value;
  node->array_repeat.count = count;
  return node;
}

AstNode *AstNode_new_break(Location loc) {
  AstNode *node = AstNode_new(NODE_BREAK, loc);
  return node;
}

AstNode *AstNode_new_continue(Location loc) {
  AstNode *node = AstNode_new(NODE_CONTINUE, loc);
  return node;
}