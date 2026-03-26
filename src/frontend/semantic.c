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
  case NODE_IDENT:
    s = SymbolTable_find(table, node->ident.value);

    if (!s) {
      fprintf(stderr, "Undefined variable %s", node->ident.value);
      exit(1);
    }

    return s->type;
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
