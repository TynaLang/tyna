#ifndef TYL_AST_DUMP_H
#define TYL_AST_DUMP_H

#include <stdio.h>

#include "tyl/ast.h"
#include "tyl/sema.h"

void Ast_dump_root(FILE *out, AstNode *ast, const char *label);
void Ast_dump_modules(FILE *out, Sema *s, size_t start_index, const char *label_prefix);

#endif // TYL_AST_DUMP_H
