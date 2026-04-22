#include <stdio.h>

#include "tyna/ast_dump.h"

void Ast_dump_root(FILE *out, AstNode *ast, const char *label) {
  if (!ast)
    return;

  fprintf(out, "=== AST: %s ===\n", label);
  Ast_print_to_stream(out, ast, 0);
}

void Ast_dump_modules(FILE *out, Sema *s, size_t start_index,
                      const char *label_prefix) {
  for (size_t i = start_index; i < s->modules.len; i++) {
    Module *module = s->modules.items[i];
    if (!module || !module->ast)
      continue;

    fprintf(out, "=== AST: %s (%s) ===\n", label_prefix,
            module->name ? module->name : "<unknown>");
    Ast_print_to_stream(out, module->ast, 0);
  }
}
