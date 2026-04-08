#ifndef CODEGEN_PRIVATE_H
#define CODEGEN_PRIVATE_H

#include "tyl/codegen.h"
#include "tyl/errors.h"
#include "tyl/type.h"
#include "tyl/utils.h"

#include <llvm-c/Core.h>
#include <stdbool.h>

typedef struct CGSymbol {
  StringView name;
  Type *type;
  LLVMValueRef value;
} CGSymbol;

typedef struct CGSymbolTable {
  struct CGSymbolTable *parent;
  List symbols; // List<CGSymbol*>
} CGSymbolTable;

typedef struct CGFunction {
  StringView name;
  LLVMValueRef value;
  LLVMTypeRef type;

  bool is_system;
} CGFunction;

struct Codegen {
  LLVMContextRef context;
  LLVMModuleRef module;
  LLVMBuilderRef builder;

  ErrorHandler *eh;
  TypeContext *type_ctx;

  CGSymbolTable *current_scope;
  List defers; // List<List*> - A stack of defer lists per block level

  LLVMValueRef current_function;
  CGFunction *current_function_ref;

  List functions;        // user-defined
  List system_functions; // printf, pow, etc.

  List string_pool; // List<char*>

  List string_globals; // List<LLVMValueRef>

  // Format string map for deduplication
  List format_strings; // List<struct { Type* t; LLVMValueRef val; }>

  List break_stack;    // List<LLVMBasicBlockRef>
  List continue_stack; // List<LLVMBasicBlockRef>
};

// ===== symbol table =====

void CGSymbolTable_init(CGSymbolTable *t, CGSymbolTable *parent);
void CGSymbolTable_add(CGSymbolTable *t, StringView name, Type *type,
                       LLVMValueRef value);
CGSymbol *CGSymbolTable_find(CGSymbolTable *t, StringView name);

CGSymbolTable *cg_push_scope(Codegen *cg);
void cg_pop_scope(Codegen *cg);

// ===== types =====

LLVMTypeRef cg_get_llvm_type(Codegen *cg, Type *t);
LLVMValueRef cg_cast_value(Codegen *cg, LLVMValueRef value, Type *from_ty,
                           LLVMTypeRef to_ty);
void cg_binary_sync_types(Codegen *cg, LLVMValueRef *lhs, Type *l_ty,
                          LLVMValueRef *rhs, Type *r_ty);

// ===== expressions / statements =====

LLVMValueRef cg_expression(Codegen *cg, AstNode *node);
void cg_statement(Codegen *cg, AstNode *node);

// ===== functions =====

void cg_init_CGFunction(CGFunction *f, StringView name, LLVMValueRef value,
                        LLVMTypeRef type, bool is_system);
void cg_define_function(Codegen *cg, AstNode *node);
void cg_emit_function_body(Codegen *cg, AstNode *node);
CGFunction *cg_find_function(Codegen *cg, StringView name);

size_t cg_string_pool_insert(Codegen *cg, StringView str);

LLVMValueRef cg_get_address(Codegen *cg, AstNode *node);
LLVMValueRef cg_alloca_in_entry(Codegen *cg, Type *type, StringView name);

#endif
