#include <stdio.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "runner.h"
#include "semantic.h"
#include "utils.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("No entrypoint was provided. Exiting\n");
    return 1;
  }

  const char *src = read_file(argv[1]);

  Lexer lexer = make_lexer(src);
  AstNode *program = Parser_process(&lexer);

  SymbolTable table;
  SymbolTable_init(&table);

  analyze_program(program, &table);

  Codegen *cg = Codegen_new("tyl_module");

  codegen_program(cg, program);

  printf("=== LLVM IR ===\n");
  Codegen_dump(cg);

  Runner_emit_object(cg, "output.o");
  Runner_link_executable("output.o", "output");

  return 0;
}
