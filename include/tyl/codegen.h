#ifndef CODEGEN_H
#define CODEGEN_H

#include "tyl/ast.h"
#include <llvm-c/Core.h>

typedef struct Codegen Codegen;

Codegen *Codegen_new(const char *module_name);
void Codegen_program(Codegen *cg, AstNode *ast_root);
void Codegen_dump(Codegen *cg);
void Codegen_free(Codegen *cg);

#endif
