#ifndef CODEGEN_H
#define CODEGEN_H

#include "tyl/ast.h"
#include "tyl/type.h"
#include <llvm-c/Core.h>

typedef struct Codegen Codegen;

Codegen *Codegen_new(const char *module_name, TypeContext *type_ctx);
void Codegen_program(Codegen *cg, AstNode *ast_root);
void Codegen_dump(Codegen *cg);
void Codegen_free(Codegen *cg);

#endif
