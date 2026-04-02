#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "semantic.h"

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh) {
  table->parent = NULL;
  List_init(&table->symbols);
  table->eh = eh;
}

void SymbolTable_add(SymbolTable *table, StringView name, TypeKind type,
                     int is_const) {
  Symbol *symbol = malloc(sizeof(Symbol));
  if (!symbol) {
    fprintf(stderr, "Failed to allocate symbol\n");
    exit(1);
  }
  symbol->name = name;
  symbol->type = type;
  symbol->is_const = is_const;
  List_push(&table->symbols, symbol);
}

Symbol *SymbolTable_find(SymbolTable *table, StringView name) {
  for (SymbolTable *t = table; t != NULL; t = t->parent) {
    for (size_t i = 0; i < t->symbols.len; i++) {
      Symbol *symbol = t->symbols.items[i];
      if (sv_eq(symbol->name, name)) {
        return symbol;
      }
    }
  }
  return NULL;
}

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

static int is_numeric(TypeKind t) { return type_rank(t) > 0; }

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

  case NODE_VAR: {
    Symbol *s = SymbolTable_find(table, node->var.value);
    if (!s) {
      semantic_error(table, node, "Undefined variable '" SV_FMT "'",
                     SV_ARG(node->var.value));
      return TYPE_UNKNOWN;
    }
    return s->type;
  }

  case NODE_BINARY: {
    TypeKind left = check_expression(table, node->binary.left);
    TypeKind right = check_expression(table, node->binary.right);

    if (left == TYPE_UNKNOWN || right == TYPE_UNKNOWN)
      return TYPE_UNKNOWN;

    if (is_numeric(left) && is_numeric(right)) {
      return type_rank(left) >= type_rank(right) ? left : right;
    }

    semantic_error(table, node, "Binary operator on non-numeric types");
    return TYPE_UNKNOWN;
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
    }

    return t;
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

    if (lhs != TYPE_UNKNOWN && rhs != TYPE_UNKNOWN &&
        !can_implicitly_cast(lhs, rhs)) {
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

void Semantic_analysis(AstNode *node, SymbolTable *table) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_PROGRAM:
    for (size_t i = 0; i < node->program.children.len; i++)
      Semantic_analysis(node->program.children.items[i], table);
    break;

  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    if (SymbolTable_find(table, name)) {
      semantic_error(table, node, "Duplicate variable '" SV_FMT "'",
                     SV_ARG(name));
      return;
    }

    TypeKind val_type = check_expression(table, node->var_decl.value);
    TypeKind decl_type = node->var_decl.declared_type;

    if (decl_type == TYPE_UNKNOWN) {
      decl_type = val_type;
      node->var_decl.declared_type = decl_type;
    }

    SymbolTable_add(table, name, decl_type, node->var_decl.is_const);
    break;
  }

  case NODE_FUNC_DECL: {
    StringView fn_name = node->func_decl.name;

    if (SymbolTable_find(table, fn_name)) {
      semantic_error(table, node, "Duplicate function '%.*s'", (int)fn_name.len,
                     fn_name.data);
      return;
    }

    Symbol *fn_symbol = malloc(sizeof(Symbol));
    fn_symbol->name = fn_name;
    fn_symbol->is_const = 1;
    fn_symbol->value = node;
    fn_symbol->type = node->func_decl.return_type;
    fn_symbol->scope = malloc(sizeof(SymbolTable));
    SymbolTable_init(fn_symbol->scope, table->eh);
    fn_symbol->scope->parent = table;
    SymbolTable_add(table, fn_name, fn_symbol->type, 1);

    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      AstNode *param = node->func_decl.params.items[i];
      if (SymbolTable_find(fn_symbol->scope, param->param.name)) {
        semantic_error(table, node, "Duplicate param name '%.*s'",
                       (int)fn_name.len, fn_name.data);
        continue;
      }
      SymbolTable_add(fn_symbol->scope, param->param.name, param->param.type,
                      0);
    }

    if (node->func_decl.body)
      Semantic_analysis(node->func_decl.body, fn_symbol->scope);

    // Infer return type if unknown
    if (node->func_decl.return_type == TYPE_UNKNOWN) {
      TypeKind inferred = TYPE_VOID;
      bool set = false;
      if (node->func_decl.body) {
        for (size_t i = 0; i < node->func_decl.body->program.children.len;
             i++) {
          AstNode *stmt = node->func_decl.body->program.children.items[i];
          if (stmt->tag == NODE_RETURN_STMT && stmt->return_stmt.expr) {
            TypeKind t =
                check_expression(fn_symbol->scope, stmt->return_stmt.expr);
            if (!set) {
              inferred = t;
              set = true;
            } else if (inferred != t) {
              inferred = TYPE_UNKNOWN; // multiple inconsistent returns
            }
          }
        }
      }
      node->func_decl.return_type = inferred;
      fn_symbol->type = inferred;

      Symbol *added = SymbolTable_find(table, fn_name);
      if (added)
        added->type = inferred;
    }

    // Validate return statements
    if (node->func_decl.body) {
      for (size_t i = 0; i < node->func_decl.body->program.children.len; i++) {
        AstNode *stmt = node->func_decl.body->program.children.items[i];
        if (stmt->tag == NODE_RETURN_STMT) {
          TypeKind t =
              check_expression(fn_symbol->scope, stmt->return_stmt.expr);
          if (node->func_decl.return_type == TYPE_VOID) {
            if (stmt->return_stmt.expr != NULL) {
              semantic_error(fn_symbol->scope, stmt,
                             "Void function '%.*s' cannot return a value",
                             (int)fn_name.len, fn_name.data);
            }
          } else if (node->func_decl.return_type != TYPE_UNKNOWN &&
                     !can_implicitly_cast(node->func_decl.return_type, t)) {
            semantic_error(fn_symbol->scope, stmt,
                           "Type mismatch in return: expected %s, got %s in "
                           "function '%.*s'",
                           type_to_name(node->func_decl.return_type),
                           type_to_name(t), (int)fn_name.len, fn_name.data);
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
