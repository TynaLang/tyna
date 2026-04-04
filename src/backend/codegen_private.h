#ifndef CODEGEN_PRIVATE_H
#define CODEGEN_PRIVATE_H

#include "tyl/codegen.h"
#include "tyl/errors.h"
#include "tyl/utils.h"

#include <llvm-c/Core.h>
#include <stdbool.h>

typedef struct CGSymbol {
  StringView name;
  TypeKind type;
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

  CGSymbolTable *current_scope;

  LLVMValueRef current_function;
  CGFunction *current_function_ref;

  List functions;        // user-defined
  List system_functions; // printf, pow, etc.
};

// ===== symbol table =====

void CGSymbolTable_init(CGSymbolTable *t, CGSymbolTable *parent);
void CGSymbolTable_add(CGSymbolTable *t, StringView name, TypeKind type,
                       LLVMValueRef value);
CGSymbol *CGSymbolTable_find(CGSymbolTable *t, StringView name);

CGSymbolTable *cg_push_scope(Codegen *cg);
void cg_pop_scope(Codegen *cg);

// ===== types =====

LLVMTypeRef cg_get_llvm_type(Codegen *cg, TypeKind t);
LLVMValueRef cg_cast_value(Codegen *cg, LLVMValueRef value, LLVMTypeRef to_ty);
void cg_binary_sync_types(Codegen *cg, LLVMValueRef *lhs, LLVMValueRef *rhs);

// ===== expressions / statements =====

LLVMValueRef cg_expression(Codegen *cg, AstNode *node);
void cg_statement(Codegen *cg, AstNode *node);

// ===== functions =====

void cg_init_CGFunction(CGFunction *f, StringView name, LLVMValueRef value,
                        LLVMTypeRef type, bool is_system);
void cg_define_function(Codegen *cg, AstNode *node);
void cg_emit_function_body(Codegen *cg, AstNode *node);
CGFunction *cg_find_function(Codegen *cg, StringView name);

LLVMValueRef cg_get_address(Codegen *cg, AstNode *node);
LLVMValueRef cg_alloca_in_entry(Codegen *cg, TypeKind type, StringView name);

#endif
