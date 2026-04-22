#ifndef TYNA_AST_DUMP_H
#define TYNA_AST_DUMP_H

#include <stdio.h>

#include "tyna/ast.h"
#include "tyna/sema.h"

void Ast_dump_root(FILE *out, AstNode *ast, const char *label);
void Ast_dump_modules(FILE *out, Sema *s, size_t start_index,
                      const char *label_prefix);

#endif // TYNA_AST_DUMP_H
