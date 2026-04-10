#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "tyl/ast.h"
#include "tyl/errors.h"
#include "tyl/type.h"
#include "tyl/utils.h"

typedef enum FuncStatus FuncStatus;

typedef struct Symbol Symbol;

typedef struct SemaScope SemaScope;
typedef struct SemaJump SemaJump;
typedef struct Sema Sema;

enum FuncStatus { FUNC_NONE, FUNC_FORWARD_DECL, FUNC_IMPLEMENTATION };

typedef enum {
  SYM_FIELD,
  SYM_METHOD,
  SYM_STATIC_METHOD,
  SYM_VAR,  // General variables/params
  SYM_FUNC, // General functions
} SymbolKind;

struct Symbol {
  StringView name;
  StringView original_name;
  Type *type;
  int is_const;

  AstNode *value;
  struct SemaScope *scope;
  FuncStatus func_status;
  SymbolKind kind;
};

struct SemaScope {
  SemaScope *parent;
  List symbols; // List<Symbol*>
};

struct SemaJump {
  StringView label;
  AstNode *node;
  bool is_loop;
  int defer_count;
  struct SemaJump *parent;
};

struct Sema {
  SemaScope *scope;
  SemaJump *jump;
  Type *ret_type;
  AstNode *fn_node;

  TypeContext *types;
  ErrorHandler *eh;
};

void sema_init(Sema *s, ErrorHandler *eh, TypeContext *type_ctx);

Symbol *sema_resolve(Sema *s, StringView name);
void sema_prime_types(Sema *s);
void sema_analyze(Sema *s, AstNode *root);
void sema_finish(Sema *s);

#endif // !SEMANTIC_H
