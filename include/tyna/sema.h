#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "tyna/ast.h"
#include "tyna/errors.h"
#include "tyna/type.h"
#include "tyna/utils.h"

typedef enum FuncStatus FuncStatus;

typedef struct Symbol Symbol;
typedef struct Module Module;

typedef struct SemaScope SemaScope;
typedef struct SemaJump SemaJump;
typedef struct Sema Sema;

typedef enum {
  SEMA_PASS_REGISTRATION,
  SEMA_PASS_DEFINITION,
  SEMA_PASS_BODIES,
} SemaPass;

enum FuncStatus { FUNC_NONE, FUNC_FORWARD_DECL, FUNC_IMPLEMENTATION };

typedef enum {
  SYM_FIELD,
  SYM_METHOD,
  SYM_STATIC_METHOD,
  SYM_VAR,   // General variables/params
  SYM_ERROR, // Declared error tag (no storage; not an assignable lvalue)
  SYM_FUNC,  // General functions
  SYM_MODULE,
} SymbolKind;

typedef enum {
  BUILTIN_NONE,
  BUILTIN_TYPEOF,
  BUILTIN_FREE,
  BUILTIN_SOME,
  BUILTIN_HEAP,
  BUILTIN_REF,
} BuiltinKind;

struct Symbol {
  StringView name;
  StringView original_name;
  Type *type;
  int is_const;
  int is_export;
  int is_pub_module;
  int is_external;
  int is_moved;
  int requires_storage;
  int requires_arena;

  AstNode *value;
  struct SemaScope *scope;
  FuncStatus func_status;
  SymbolKind kind;
  BuiltinKind builtin_kind;
  struct Module *module; // for SYM_MODULE symbols
};

typedef struct Module {
  const char *name;
  char *abs_path;
  AstNode *ast;
  List symbols; // List<Symbol*>
  List exports; // List<Symbol*>
  bool is_analyzed;
  bool is_stdlib;
} Module;

struct SemaScope {
  SemaScope *parent;
  List symbols; // List<Symbol*>
  bool has_computed_str;
};

struct SemaJump {
  StringView label;
  AstNode *node;
  bool is_loop;
  bool is_breakable;
  int defer_count;
  struct SemaJump *parent;
};

struct Sema {
  SemaScope *scope;
  SemaJump *jump;
  Type *ret_type;
  AstNode *fn_node;
  Type *generic_context_type;
  SemaPass pass;

  List modules; // List<Module*>
  Module *current_module;

  TypeContext *types;
  ErrorHandler *eh;
  char *entry_dir;
};

void sema_init(Sema *s, ErrorHandler *eh, TypeContext *type_ctx);

Symbol *sema_resolve(Sema *s, StringView name);
void sema_prime_types(Sema *s);
void sema_analyze(Sema *s, AstNode *root);
void sema_finish(Sema *s);

#endif // !SEMANTIC_H
