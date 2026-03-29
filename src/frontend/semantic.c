#include "semantic.h"
#include "parser.h"
#include "utils.h"
#include <stddef.h>
#include <string.h>

char *type_to_name(TypeKind type) {
  switch (type) {
  case TYPE_INT:
    return "TYPE_INT";
  case TYPE_CHAR:
    return "TYPE_CHAR";
  case TYPE_I8:
    return "TYPE_I8";
  case TYPE_I16:
    return "TYPE_I16";
  case TYPE_I32:
    return "TYPE_I32";
  case TYPE_I64:
    return "TYPE_I64";
  case TYPE_U8:
    return "TYPE_U8";
  case TYPE_U16:
    return "TYPE_U16";
  case TYPE_U32:
    return "TYPE_U32";
  case TYPE_U64:
    return "TYPE_U64";
  case TYPE_FLOAT:
    return "TYPE_FLOAT";
  case TYPE_DOUBLE:
    return "TYPE_DOUBLE";
  case TYPE_F32:
    return "TYPE_F32";
  case TYPE_F64:
    return "TYPE_F64";
  case TYPE_STRING:
    return "TYPE_STRING";
  default:
    return "TYPE_UNKNOWN";
  }
}

void SymbolTable_init(SymbolTable *table, struct ErrorHandler *eh) {
  table->eh = eh;
  List_init(&table->symbols);
}

void SymbolTable_add(SymbolTable *table, char *name, TypeKind type,
                     int is_immutable) {
  Symbol *s = malloc(sizeof(Symbol));
  s->type = type;
  s->name = name;
  s->is_immutable = is_immutable;
  List_push(&table->symbols, s);
}
Symbol *SymbolTable_find(SymbolTable *table, char *name) {
  for (size_t i = 0; i < table->symbols.len; i++) {
    Symbol *s = table->symbols.items[i];
    if (strcmp(s->name, name) == 0) {
      return s;
    }
  }
  return NULL;
}

static int is_numeric(TypeKind t) {
  return (t >= TYPE_INT && t <= TYPE_F64) || t == TYPE_CHAR;
}

static int is_floating(TypeKind t) {
  return t == TYPE_FLOAT || t == TYPE_DOUBLE || t == TYPE_F32 || t == TYPE_F64;
}

static int is_integer(TypeKind t) {
  return (t >= TYPE_INT && t <= TYPE_U64) || t == TYPE_CHAR;
}

static void check_immutability(ErrorHandler *eh, AstNode *expr,
                               SymbolTable *table) {
  if (expr->tag == NODE_VAR) {
    Symbol *s = SymbolTable_find(table, expr->var.value);
    if (s && s->is_immutable) {
      ErrorHandler_report(eh, expr->loc,
                          "Cannot modify immutable variable '%s'", s->name);
    }
  }
}

TypeKind analyze_expression(AstNode *node, SymbolTable *table) {
  Symbol *s; // can't declare vars in switch statement
  switch (node->tag) {
  case NODE_NUMBER: {
    char *text = node->number.raw_text;
    int has_dot = 0;
    int has_f = 0;
    for (int i = 0; text[i]; i++) {
      if (text[i] == '.')
        has_dot = 1;
      if (text[i] == 'f' || text[i] == 'F')
        has_f = 1;
    }
    if (has_f)
      return TYPE_FLOAT;
    if (has_dot)
      return TYPE_DOUBLE;
    return TYPE_INT;
  }
  case NODE_CHAR:
    return TYPE_CHAR;
  case NODE_STRING:
    return TYPE_STRING;
  case NODE_VAR: {
    s = SymbolTable_find(table, node->var.value);

    if (!s) {
      ErrorHandler_report(table->eh, node->loc, "Undefined variable %s",
                          node->var.value);
      return TYPE_UNKNOWN;
    }

    return s->type;
  }
  case NODE_BINARY: {
    TypeKind left = analyze_expression(node->binary.left, table);
    TypeKind right = analyze_expression(node->binary.right, table);

    switch (node->binary.op) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_POW:
      if (!is_numeric(left) || !is_numeric(right)) {
        ErrorHandler_report(
            table->eh, node->loc,
            "Operator only supports numeric types, got %s and %s",
            type_to_name(left), type_to_name(right));
        return TYPE_UNKNOWN;
      }
      // Resulting type is the "larger" of the two for simple promotion
      if (left == TYPE_DOUBLE || right == TYPE_DOUBLE || left == TYPE_F64 ||
          right == TYPE_F64)
        return TYPE_DOUBLE;
      if (left == TYPE_FLOAT || right == TYPE_FLOAT || left == TYPE_F32 ||
          right == TYPE_F32)
        return TYPE_FLOAT;
      return left;
    default:
      ErrorHandler_report(table->eh, node->loc, "Unknown binary operator");
      return TYPE_UNKNOWN;
    }
  }
  case NODE_UNARY: {
    TypeKind expr = analyze_expression(node->unary.expr, table);
    switch (node->unary.op) {
    case OP_NEG:
    case OP_POST_INC:
    case OP_PRE_INC:
    case OP_PRE_DEC:
    case OP_POST_DEC:
      if (!is_numeric(expr)) {
        ErrorHandler_report(table->eh, node->loc,
                            "Operator only supports numeric types, got %s",
                            type_to_name(expr));
        return TYPE_UNKNOWN;
      }

      // Check for immutability
      check_immutability(table->eh, node->unary.expr, table);

      return expr;
    default:
      ErrorHandler_report(table->eh, node->loc, "Unknown unary operator");
      return TYPE_UNKNOWN;
    }
  }
  case NODE_CAST_EXPR: {
    TypeKind expr_type = analyze_expression(node->cast_expr.expr, table);
    TypeKind target_type = node->cast_expr.target_type;

    if (is_numeric(target_type)) {
      if (!is_numeric(expr_type) && expr_type != TYPE_CHAR) {
        ErrorHandler_report(table->eh, node->loc,
                            "Cannot cast %s to numeric type %s",
                            type_to_name(expr_type), type_to_name(target_type));
        return TYPE_UNKNOWN;
      }
    } else if (target_type == TYPE_CHAR) {
      if (!is_numeric(expr_type) && expr_type != TYPE_CHAR) {
        ErrorHandler_report(table->eh, node->loc, "Cannot cast %s to char",
                            type_to_name(expr_type));
        return TYPE_UNKNOWN;
      }
    }

    return target_type;
  }
  case NODE_ASSIGN_EXPR: {
    if (node->assign_expr.target->tag != NODE_VAR) {
      ErrorHandler_report(table->eh, node->loc,
                          "Assignment target must be an identifier");
      return TYPE_UNKNOWN;
    }

    char *name = node->assign_expr.target->var.value;
    Symbol *target_sym = SymbolTable_find(table, name);
    if (!target_sym) {
      ErrorHandler_report(table->eh, node->loc,
                          "Assignment to undefined variable '%s'", name);
      return TYPE_UNKNOWN;
    }

    if (target_sym->is_immutable) {
      check_immutability(table->eh, node->assign_expr.target, table);
      return TYPE_UNKNOWN;
    }

    TypeKind value_type = analyze_expression(node->assign_expr.value, table);

    if (value_type != target_sym->type) {
      // Allow implicit conversion for numeric literals to any numeric type
      if (node->assign_expr.value->tag == NODE_NUMBER &&
          is_numeric(target_sym->type)) {
        // Allow
      } else {
        ErrorHandler_report(
            table->eh, node->loc,
            "Type mismatch in assignment to '%s': expected %s, got %s", name,
            type_to_name(target_sym->type), type_to_name(value_type));
        return TYPE_UNKNOWN;
      }
    }

    return target_sym->type;
  }
  default:
    ErrorHandler_report(table->eh, node->loc, "Unhandalable expression");
    return TYPE_UNKNOWN;
  }
}

void analyze_statement(AstNode *node, SymbolTable *table) {
  if (!node)
    return;

  switch (node->tag) {

  case NODE_VAR_DECL: {
    char *name = node->var_decl.name->var.value;
    TypeKind declared = node->var_decl.declared_type;

    if (SymbolTable_find(table, name)) {
      ErrorHandler_report(table->eh, node->loc,
                          "Variable '%s' already declared", name);
      return;
    }

    TypeKind value_type = analyze_expression(node->var_decl.value, table);

    if (declared == TYPE_UNKNOWN) {
      // Inferred type
      declared = value_type;
      node->var_decl.declared_type = declared; // Update AST for codegen
    } else if (value_type != declared) {
      // Special case: Literal number to any numeric type
      if (node->var_decl.value->tag == NODE_NUMBER && is_numeric(declared)) {
        // Allow
      } else {
        ErrorHandler_report(table->eh, node->loc,
                            "Type mismatch for '%s': expected %s, got %s", name,
                            type_to_name(declared), type_to_name(value_type));
        return;
      }
    }

    SymbolTable_add(table, name, declared, node->var_decl.is_const);

    break;
  }

  case NODE_PRINT_STMT: {
    analyze_expression(node->print_stmt.value, table);
    break;
  }

  case NODE_EXPR_STMT: {
    analyze_expression(node->expr_stmt.expr, table);
    break;
  }

  default:
    ErrorHandler_report(table->eh, node->loc, "Unknown statement node: %d",
                        node->tag);
    break;
  }
}

void analyze_program(AstNode *program, SymbolTable *table) {
  List global_statements = program->program.children;
  for (size_t i = 0; i < global_statements.len; i++) {
    analyze_statement(global_statements.items[i], table);
  }
}
