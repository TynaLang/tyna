#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "parser.h"
#include "utils.h"

typedef struct {
  char *name;
  TypeKind type;
} Symbol;

typedef struct {
  List symbols; // List<Symbol*>
} SymbolTable;

void SymbolTable_init(SymbolTable *table);
void SymbolTable_add(SymbolTable *table, char *name, TypeKind type);
Symbol *SymbolTable_find(SymbolTable *table, char *name);

void analyze_program(AstNode *program, SymbolTable *table);
TypeKind analyze_expression(AstNode *node, SymbolTable *table);
void analyze_statement(AstNode *node, SymbolTable *table);

#endif // !SEMANTIC_H
