#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "utils.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>

typedef struct Codegen Codegen;
typedef struct CGSymbol CGSymbol;
typedef struct CGSymbolTable CGSymbolTable;
typedef struct CGFunction CGFunction;

struct CGSymbol {
  StringView name;
  TypeKind type;
  LLVMValueRef value;
};

struct CGSymbolTable {
  CGSymbolTable *parent;
  List symbols; // List<CGSymbol*>
};

struct CGFunction {
  StringView name;
  LLVMValueRef value;
  LLVMTypeRef type;
  TypeKind return_type;
  List params; // List<TypeKind>
  bool is_system;
};

struct Codegen {
  LLVMContextRef context;
  LLVMModuleRef module;
  LLVMBuilderRef builder;

  CGSymbolTable *current_scope;
  LLVMValueRef current_function;
  CGFunction *current_function_ref;

  List functions;        // List<CGFunction*>
  List system_functions; // List<CGFunction*>
};

void CGSymbolTable_init(CGSymbolTable *t, CGSymbolTable *parent);
void CGSymbolTable_add(CGSymbolTable *t, StringView name, TypeKind type,
                       LLVMValueRef value);
CGSymbol *CGSymbolTable_find(CGSymbolTable *t, StringView name);

Codegen *Codegen_new(const char *module_name);
void Codegen_program(Codegen *cg, AstNode *program);
void Codegen_dump(Codegen *cg);
void Codegen_free(Codegen *cg);

#endif
