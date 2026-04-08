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

struct Symbol {
  StringView name;
  Type *type;
  int is_const;

  AstNode *value;
  struct SemaScope *scope;
  FuncStatus func_status;
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

void Sema_init(Sema *s, ErrorHandler *eh, TypeContext *type_ctx);
void Sema_analyze(Sema *s, AstNode *root);
void Sema_finish(Sema *s);

#endif // !SEMANTIC_H
