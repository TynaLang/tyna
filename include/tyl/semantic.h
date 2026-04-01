#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "ast.h"
#include "errors.h"
#include "utils.h"

typedef struct {
  StringView name;
  TypeKind type;
  int is_const;
  AstNode *value;            // For constant initializers or function bodies
  struct SymbolTable *scope; // For functions: inner scope
} Symbol;

typedef struct SymbolTable SymbolTable;

struct SymbolTable {
  SymbolTable *parent;
  List symbols; // List<Symbol*>
  ErrorHandler *eh;
};

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh);
void SymbolTable_add(SymbolTable *table, StringView name, TypeKind type,
                     int is_const);
Symbol *SymbolTable_find(SymbolTable *table, StringView name);

void Semantic_analysis(AstNode *program, SymbolTable *table);
char *type_to_name(TypeKind type);

#endif // !SEMANTIC_H
