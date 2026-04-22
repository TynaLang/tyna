#ifndef CODEGEN_H
#define CODEGEN_H

#include "tyna/ast.h"
#include "tyna/errors.h"
#include "tyna/type.h"
#include <llvm-c/Core.h>

typedef struct Codegen Codegen;

Codegen *Codegen_new(const char *module_name, TypeContext *type_ctx,
                     ErrorHandler *eh);
void Codegen_global(Codegen *cg, AstNode *ast_root);
void Codegen_program(Codegen *cg, AstNode *ast_root);
void Codegen_dump(Codegen *cg);
void Codegen_free(Codegen *cg);

#endif
