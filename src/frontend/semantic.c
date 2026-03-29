#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "semantic.h"

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh) {
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
  for (size_t i = 0; i < table->symbols.len; i++) {
    Symbol *symbol = table->symbols.items[i];
    if (sv_eq(symbol->name, name)) {
      return symbol;
    }
  }
  return NULL;
}

static void semantic_error(SymbolTable *table, AstNode *node, const char *msg) {
  ErrorHandler_report(table->eh, node->loc, "%s", msg);
}

static TypeKind check_expression(SymbolTable *table, AstNode *node) {
  switch (node->tag) {
  case NODE_NUMBER:
    return TYPE_I32;
  case NODE_CHAR:
    return TYPE_CHAR;
  case NODE_STRING:
    return TYPE_STRING;
  case NODE_VAR: {
    Symbol *symbol = SymbolTable_find(table, node->var.value);
    if (!symbol) {
      ErrorHandler_report(table->eh, node->loc,
                          "Undefined variable '" SV_FMT "'",
                          SV_ARG(node->var.value));
      return TYPE_UNKNOWN;
    }
    return symbol->type;
  }
  case NODE_BINARY: {
    TypeKind left = check_expression(table, node->binary.left);
    TypeKind right = check_expression(table, node->binary.right);

    if (left == TYPE_UNKNOWN || right == TYPE_UNKNOWN)
      return TYPE_UNKNOWN;

    if (left != right) {
      if ((left == TYPE_F32 || left == TYPE_F64) ||
          (right == TYPE_F32 || right == TYPE_F64)) {
        return (left == TYPE_F64 || right == TYPE_F64) ? TYPE_F64 : TYPE_F32;
      }
    }
    return left;
  }
  case NODE_UNARY: {
    TypeKind t = check_expression(table, node->unary.expr);
    if (t == TYPE_UNKNOWN)
      return TYPE_UNKNOWN;

    if (node->unary.op == OP_PRE_INC || node->unary.op == OP_POST_INC ||
        node->unary.op == OP_PRE_DEC || node->unary.op == OP_POST_DEC) {
      if (node->unary.expr->tag != NODE_VAR) {
        semantic_error(table, node,
                       "Increment/decrement operand must be a variable");
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
    Symbol *symbol =
        SymbolTable_find(table, node->assign_expr.target->var.value);
    if (!symbol) {
      ErrorHandler_report(table->eh, node->loc,
                          "Undefined variable '" SV_FMT "'",
                          SV_ARG(node->assign_expr.target->var.value));
      return TYPE_UNKNOWN;
    }
    if (symbol->is_const) {
      ErrorHandler_report(table->eh, node->loc,
                          "Cannot reassign constant '" SV_FMT "'",
                          SV_ARG(symbol->name));
      return TYPE_UNKNOWN;
    }
    check_expression(table, node->assign_expr.value);
    return symbol->type;
  }
  case NODE_CAST_EXPR:
    check_expression(table, node->cast_expr.expr);
    return node->cast_expr.target_type;
  default:
    return TYPE_I32;
  }
}

void Semantic_analysis(AstNode *node, SymbolTable *table) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_PROGRAM:
    for (size_t i = 0; i < node->program.children.len; i++) {
      Semantic_analysis(node->program.children.items[i], table);
    }
    break;

  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    if (SymbolTable_find(table, name)) {
      ErrorHandler_report(table->eh, node->loc,
                          "Duplicate variable '" SV_FMT "'", SV_ARG(name));
      return;
    }

    TypeKind val_type = check_expression(table, node->var_decl.value);
    TypeKind decl_type = node->var_decl.declared_type;

    if (decl_type == TYPE_UNKNOWN) {
      decl_type = val_type;
    }

    SymbolTable_add(table, name, decl_type, node->var_decl.is_const);
    break;
  }

  case NODE_PRINT_STMT:
    check_expression(table, node->print_stmt.value);
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
  case TYPE_STRING:
    return "string";
  default:
    return "unknown";
  }
}