#ifndef CODEGEN_H
#define CODEGEN_H

#include "parser.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef struct Codegen Codegen;
typedef struct CGSymbol CGSymbol;
typedef struct CGSymbolTable CGSymbolTable;

struct CGSymbol {
  char *name;
  TypeKind type;
  LLVMValueRef value;
};

struct CGSymbolTable {
  List symbols; // List<CGSymbol*>
};

struct Codegen {
  LLVMContextRef context;
  LLVMModuleRef module;
  LLVMBuilderRef builder;

  CGSymbolTable symbols;
  LLVMValueRef printf_func;
  LLVMTypeRef printf_func_type;
};

Codegen *Codegen_new(const char *module_name);
void codegen_program(Codegen *cg, AstNode *program);
void Codegen_dump(Codegen *cg);

#endif // !CODEGEN_H
