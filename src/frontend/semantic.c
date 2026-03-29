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
  case TYPE_STRING:
    return "TYPE_STRING";
  default:
    return "TYPE_UNKNOWN";
  }
}

void SymbolTable_init(SymbolTable *table) { List_init(&table->symbols); }
void SymbolTable_add(SymbolTable *table, char *name, TypeKind type) {
  Symbol *s = malloc(sizeof(Symbol));
  s->type = type;
  s->name = name;
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

TypeKind analyze_expression(AstNode *node, SymbolTable *table) {
  Symbol *s; // can't declare vars in switch statement
  switch (node->tag) {
  case NODE_NUMBER:
    return TYPE_INT;
  case NODE_CHAR:
    return TYPE_CHAR;
  case NODE_STRING:
    return TYPE_STRING;
  case NODE_IDENT: {
    s = SymbolTable_find(table, node->ident.value);

    if (!s) {
      fprintf(stderr, "Undefined variable %s", node->ident.value);
      exit(1);
    }

    return s->type;
  }
  case NODE_BINARY: {
    TypeKind left = analyze_expression(node->binary.left, table);
    TypeKind right = analyze_expression(node->binary.right, table);

    if (left != right) {
      fprintf(stderr, "Type mismatch in binary operation: %s vs %s\n",
              type_to_name(left), type_to_name(right));
      exit(1);
    }

    switch (node->binary.op) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_POW:
      if (left != TYPE_INT) {
        fprintf(stderr, "Operator only supports int, got %s\n",
                type_to_name(left));
        exit(1);
      }
      return TYPE_INT;
    default:
      fprintf(stderr, "Unknown binary operator\n");
      exit(1);
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
      if (expr != TYPE_INT) {
        fprintf(stderr, "Operator only supports int, got %s\n",
                type_to_name(expr));
        exit(1);
      }
      return TYPE_INT;
    default:
      fprintf(stderr, "Unknown unary operator\n");
      exit(1);
    }
  }
  case NODE_ASSIGN: {
    if (node->assign.target->tag != NODE_IDENT) {
      fprintf(stderr, "Assignment target must be an identifier\n");
      exit(1);
    }
    
    char *name = node->assign.target->ident.value;
    Symbol *target_sym = SymbolTable_find(table, name);
    if (!target_sym) {
      fprintf(stderr, "Assignment to undefined variable '%s'\n", name);
      exit(1);
    }

    TypeKind value_type = analyze_expression(node->assign.value, table);

    if (value_type != target_sym->type) {
      fprintf(stderr, "Type mismatch in assignment to '%s': expected %s, got %s\n",
              name, type_to_name(target_sym->type), type_to_name(value_type));
      exit(1);
    }

    return target_sym->type;
  }
  default:
    fprintf(stderr, "Unhandalable expression %s", node->ident.value);
    exit(1);
  }
}

void analyze_statement(AstNode *node, SymbolTable *table) {
  if (!node)
    return;

  switch (node->tag) {

  case NODE_LET: {
    char *name = node->let.name->ident.value;

    if (SymbolTable_find(table, name)) {
      fprintf(stderr, "Variable '%s' already declared\n", name);
      exit(1);
    }

    TypeKind value_type = analyze_expression(node->let.value, table);

    if (value_type != node->let.declared_type) {
      fprintf(stderr, "Type mismatch for '%s': expected %s, got %s\n", name,
              type_to_name(node->let.declared_type), type_to_name(value_type));
      exit(1);
    }

    SymbolTable_add(table, name, node->let.declared_type);

    break;
  }

  case NODE_PRINT: {
    analyze_expression(node->print.value, table);
    break;
  }

  case NODE_EXPR_STMT: {
    analyze_expression(node->expr_stmt.expr, table);
    break;
  }

  default:
    fprintf(stderr, "Unknown statement node: %d\n", node->tag);
    exit(1);
  }
}

void analyze_program(AstNode *program, SymbolTable *table) {
  List global_statements = program->program.children;
  for (size_t i = 0; i < global_statements.len; i++) {
    analyze_statement(global_statements.items[i], table);
  }
}
