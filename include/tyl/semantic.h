#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "errors.h"
#include "parser.h"
#include "utils.h"

typedef struct {
  char *name;
  TypeKind type;
  int is_immutable;
} Symbol;

typedef struct {
  List symbols; // List<Symbol*>
  ErrorHandler *eh;
} SymbolTable;

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh);
void SymbolTable_add(SymbolTable *table, char *name, TypeKind type,
                     int is_immutable);
Symbol *SymbolTable_find(SymbolTable *table, char *name);

void analyze_program(AstNode *program, SymbolTable *table);
TypeKind analyze_expression(AstNode *node, SymbolTable *table);
void analyze_statement(AstNode *node, SymbolTable *table);

char *type_to_name(TypeKind type);

#endif // !SEMANTIC_H
