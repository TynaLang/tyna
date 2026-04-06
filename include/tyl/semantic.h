#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "tyl/ast.h"
#include "tyl/errors.h"
#include "tyl/type.h"
#include "tyl/utils.h"

typedef enum FuncStatus FuncStatus;

typedef struct Symbol Symbol;

typedef struct SymbolTable SymbolTable;

enum FuncStatus { FUNC_NONE, FUNC_FORWARD_DECL, FUNC_IMPLEMENTATION };

struct Symbol {
  StringView name;
  Type *type;
  int is_const;

  AstNode *value;            // For constant initializers or function bodies
  struct SymbolTable *scope; // For functions: inner scope

  FuncStatus func_status;
};

struct SymbolTable {
  SymbolTable *parent;
  List symbols; // List<Symbol*>
  ErrorHandler *eh;
  TypeContext *type_ctx;
};

void SymbolTable_init(SymbolTable *table, ErrorHandler *eh,
                      TypeContext *type_ctx);
void SymbolTable_add(SymbolTable *table, StringView name, Type *type,
                     int is_const);
Symbol *SymbolTable_find(SymbolTable *table, StringView name);

void Semantic_analysis(AstNode *ast_root, SymbolTable *table);
const char *type_to_name(Type *type);
int is_numeric(Type *type);
int is_bool(Type *type);

#endif // !SEMANTIC_H
