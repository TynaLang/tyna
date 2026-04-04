#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tyl/ast.h"
#include "tyl/semantic.h"

static void semantic_error(SymbolTable *t, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(t->eh, n->loc, "%s", msg_buf);
}

static int is_lvalue(AstNode *node) {
  return node->tag == NODE_VAR || node->tag == NODE_FIELD ||
         node->tag == NODE_INDEX;
}

static int type_rank(TypeKind t) {
  switch (t) {
  case TYPE_I8:
  case TYPE_U8:
  case TYPE_CHAR:
    return 1;
  case TYPE_I16:
  case TYPE_U16:
    return 2;
  case TYPE_I32:
  case TYPE_U32:
    return 3;
  case TYPE_I64:
  case TYPE_U64:
    return 4;
  case TYPE_F32:
    return 5;
  case TYPE_F64:
    return 6;
  default:
    return 0;
  }
}

int is_numeric(TypeKind t) { return type_rank(t) > 0; }
int is_bool(TypeKind t) { return t == TYPE_BOOL; }

static int can_implicitly_cast(TypeKind to, TypeKind from) {
  if (to == from)
    return 1;
  if (to == TYPE_VOID || from == TYPE_VOID)
    return 0;
  if (is_bool(to) || is_bool(from))
    return 0;
  if (is_numeric(to) && is_numeric(from))
    return type_rank(to) >= type_rank(from);
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
    if (decl_params.items[i] != imp_params.items[i])
      return 0;
  }
  return 1;
}

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh) {
  table->parent = NULL;
  List_init(&table->symbols);
  table->eh = eh;
}

void SymbolTable_add(SymbolTable *table, StringView name, TypeKind type,
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
  sym->scope = xmalloc(sizeof(SymbolTable));
  SymbolTable_init(sym->scope, table->eh);
  sym->scope->parent = table;

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

static TypeKind check_expression(SymbolTable *table, AstNode *node) {
  if (!node)
    return TYPE_UNKNOWN;

  switch (node->tag) {
  case NODE_NUMBER:
    return TYPE_I32;
  case NODE_CHAR:
    return TYPE_CHAR;
  case NODE_STRING:
    return TYPE_STRING;
  case NODE_BOOL:
    return TYPE_BOOL;

  case NODE_VAR: {
    Symbol *s = SymbolTable_find(table, node->var.value);
    if (!s) {
      semantic_error(table, node, "Undefined variable '" SV_FMT "'",
                     SV_ARG(node->var.value));
      return TYPE_UNKNOWN;
    }
    return s->type;
  }

  case NODE_UNARY: {
    TypeKind t = check_expression(table, node->unary.expr);
    if (t == TYPE_UNKNOWN)
      return TYPE_UNKNOWN;

    if (node->unary.op == OP_PRE_INC || node->unary.op == OP_POST_INC ||
        node->unary.op == OP_PRE_DEC || node->unary.op == OP_POST_DEC) {
      if (!is_lvalue(node->unary.expr)) {
        semantic_error(table, node,
                       "Increment/decrement operand must be an lvalue");
        return TYPE_UNKNOWN;
      }
      Symbol *s = SymbolTable_find(table, node->unary.expr->var.value);
      if (s && s->is_const) {
        semantic_error(table, node,
                       "Cannot increment/decrement a constant variable");
        return TYPE_UNKNOWN;
      }
    } else if (node->unary.op == OP_NEG) {
      if (!is_numeric(t)) {
        semantic_error(table, node, "Unary '-' applied to non-numeric type");
        return TYPE_UNKNOWN;
      }
    }

    return t;
  }

  case NODE_BINARY_ARITH: {
    TypeKind left = check_expression(table, node->binary_arith.left);
    TypeKind right = check_expression(table, node->binary_arith.right);
    if (!is_numeric(left) || !is_numeric(right)) {
      semantic_error(table, node, "Arithmetic operator on non-numeric type");
      return TYPE_UNKNOWN;
    }
    return type_rank(left) >= type_rank(right) ? left : right;
  }

  case NODE_BINARY_COMPARE: {
    TypeKind left = check_expression(table, node->binary_compare.left);
    TypeKind right = check_expression(table, node->binary_compare.right);
    if (!is_numeric(left) || !is_numeric(right)) {
      semantic_error(table, node,
                     "Comparison operator requires numeric operands");
      return TYPE_UNKNOWN;
    }
    return TYPE_BOOL;
  }

  case NODE_BINARY_EQUALITY: {
    TypeKind left = check_expression(table, node->binary_equality.left);
    TypeKind right = check_expression(table, node->binary_equality.right);
    if (!can_implicitly_cast(left, right) &&
        !can_implicitly_cast(right, left)) {
      semantic_error(table, node, "Equality operator on incompatible types");
      return TYPE_UNKNOWN;
    }
    return TYPE_BOOL;
  }

  case NODE_BINARY_LOGICAL: {
    TypeKind left = check_expression(table, node->binary_logical.left);
    TypeKind right = check_expression(table, node->binary_logical.right);
    if (!is_bool(left) || !is_bool(right)) {
      semantic_error(table, node, "Logical operator requires boolean operands");
      return TYPE_UNKNOWN;
    }
    return TYPE_BOOL;
  }

  case NODE_ASSIGN_EXPR: {
    AstNode *target = node->assign_expr.target;
    if (!is_lvalue(target)) {
      semantic_error(table, node, "Invalid assignment target");
      return TYPE_UNKNOWN;
    }

    check_expression(table, target);

    if (target->tag != NODE_VAR) {
      semantic_error(table, node, "Only variable assignment supported for now");
      return TYPE_UNKNOWN;
    }

    Symbol *s = SymbolTable_find(table, target->var.value);
    if (!s) {
      semantic_error(table, node, "Undefined variable '" SV_FMT "'",
                     SV_ARG(target->var.value));
      return TYPE_UNKNOWN;
    }
    if (s->is_const) {
      semantic_error(table, node, "Cannot assign to constant '" SV_FMT "'",
                     SV_ARG(s->name));
      return TYPE_UNKNOWN;
    }

    TypeKind lhs = s->type;
    TypeKind rhs = check_expression(table, node->assign_expr.value);
    if (!can_implicitly_cast(lhs, rhs)) {
      semantic_error(table, node,
                     "Type mismatch in assignment: expected %s, got %s",
                     type_to_name(lhs), type_to_name(rhs));
    }
    return lhs;
  }

  case NODE_CAST_EXPR: {
    check_expression(table, node->cast_expr.expr);
    return node->cast_expr.target_type;
  }

  case NODE_CALL: {
    Symbol *s = SymbolTable_find(table, node->call.func->var.value);
    if (!s) {
      semantic_error(table, node, "Call to undefined function '%.*s'",
                     (int)node->call.func->var.value.len,
                     node->call.func->var.value.data);
      return TYPE_UNKNOWN;
    }

    if (s->scope && node->call.args.len != s->scope->symbols.len) {
      semantic_error(table, node,
                     "Function '%.*s' expects %zu arguments, got %zu",
                     (int)s->name.len, s->name.data, s->scope->symbols.len,
                     node->call.args.len);
    }

    for (size_t i = 0; i < node->call.args.len; i++) {
      TypeKind arg_type = check_expression(table, node->call.args.items[i]);
      if (s->scope && i < s->scope->symbols.len) {
        Symbol *param = s->scope->symbols.items[i];
        if (!can_implicitly_cast(param->type, arg_type)) {
          semantic_error(table, node,
                         "Argument %zu type mismatch: expected %s, got %s", i,
                         type_to_name(param->type), type_to_name(arg_type));
        }
      }
    }
    return s->type;
  }

  default:
    return TYPE_UNKNOWN;
  }
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
    Semantic_deduplicate_top_level(node, table);
    for (size_t i = 0; i < node->ast_root.children.len; i++)
      Semantic_analysis(node->ast_root.children.items[i], table);
    break;

  case NODE_BLOCK: {
    SymbolTable block_table;
    SymbolTable_init(&block_table, table->eh);
    block_table.parent = table;

    for (size_t i = 0; i < node->block.statements.len; i++)
      Semantic_analysis(node->block.statements.items[i], &block_table);
    break;
  }

  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    if (SymbolTable_find(table, name)) {
      semantic_error(table, node, "Duplicate variable '" SV_FMT "'",
                     SV_ARG(name));
      return;
    }
    TypeKind val_type = check_expression(table, node->var_decl.value);
    TypeKind decl_type = node->var_decl.declared_type;
    if (decl_type == TYPE_UNKNOWN)
      decl_type = val_type;
    node->var_decl.declared_type = decl_type;
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

    if (node->func_decl.return_type == TYPE_UNKNOWN && node->func_decl.body) {
      TypeKind inferred = TYPE_VOID;
      int set = 0;
      for (size_t i = 0; i < node->func_decl.body->ast_root.children.len; i++) {
        AstNode *stmt = node->func_decl.body->ast_root.children.items[i];
        if (stmt->tag == NODE_RETURN_STMT && stmt->return_stmt.expr) {
          TypeKind t =
              check_expression(fn_symbol->scope, stmt->return_stmt.expr);
          if (!set) {
            inferred = t;
            set = 1;
          } else if (inferred != t) {
            inferred = TYPE_UNKNOWN; // multiple inconsistent return types
          }
        }
      }
      node->func_decl.return_type = inferred;
      fn_symbol->type = inferred;
    }

    if (node->func_decl.body) {
      for (size_t i = 0; i < node->func_decl.body->ast_root.children.len; i++) {
        AstNode *stmt = node->func_decl.body->ast_root.children.items[i];
        if (stmt->tag == NODE_RETURN_STMT) {
          TypeKind t =
              check_expression(fn_symbol->scope, stmt->return_stmt.expr);
          if (node->func_decl.return_type == TYPE_VOID &&
              stmt->return_stmt.expr) {
            semantic_error(fn_symbol->scope, stmt,
                           "Void function '" SV_FMT "' cannot return a value",
                           SV_ARG(fn_name));
          } else if (node->func_decl.return_type != TYPE_UNKNOWN &&
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

  default:
    break;
  }
}

char *type_to_name(TypeKind type) {
  switch (type) {
  case TYPE_I8:
    return "i8";
  case TYPE_I16:
    return "i16";
  case TYPE_I32:
    return "i32";
  case TYPE_I64:
    return "i64";
  case TYPE_U8:
    return "u8";
  case TYPE_U16:
    return "u16";
  case TYPE_U32:
    return "u32";
  case TYPE_U64:
    return "u64";
  case TYPE_F32:
    return "f32";
  case TYPE_F64:
    return "f64";
  case TYPE_CHAR:
    return "char";
  case TYPE_BOOL:
    return "bool";
  case TYPE_STRING:
    return "string";
  case TYPE_VOID:
    return "void";
  default:
    return "unknown";
  }
}
