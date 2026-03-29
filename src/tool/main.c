#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "runner.h"
#include "semantic.h"
#include "utils.h"

typedef enum { MODE_COMPILE, MODE_JIT, MODE_EMIT_IR, MODE_DUMP } RunMode;

void print_usage(const char *prog_name) {
  printf("Usage: %s [options] <file>\n", prog_name);
  printf("Options:\n");
  printf("  -c, --compile   Compile to executable (default)\n");
  printf("  -j, --jit       JIT compile and run\n");
  printf("  -e, --emit-ir   Emit LLVM IR to file\n");
  printf("  -d, --dump      Dump AST and LLVM IR to stdout\n");
  printf("  -h, --help      Show this help message\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  RunMode mode = MODE_JIT;
  const char *file_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--compile") == 0) {
      mode = MODE_COMPILE;
    } else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jit") == 0) {
      mode = MODE_JIT;
    } else if (strcmp(argv[i], "-e") == 0 ||
               strcmp(argv[i], "--emit-ir") == 0) {
      mode = MODE_EMIT_IR;
    } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dump") == 0) {
      mode = MODE_DUMP;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      printf("Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    } else {
      if (file_path == NULL) {
        file_path = argv[i];
      } else {
        printf("Error: Multiple files specified.\n");
        return 1;
      }
    }
  }

  if (!file_path) {
    printf("Error: No input file specified.\n");
    print_usage(argv[0]);
    return 1;
  }

  const char *src = read_file(file_path);
  if (!src) {
    printf("Error: Could not read file '%s'\n", file_path);
    return 1;
  }

  // Frontend
  ErrorHandler eh;
  ErrorHandler_init(&eh, src);
  Lexer lexer = make_lexer(src, &eh);
  AstNode *program = Parser_process(&lexer, &eh);

  // Ast_print(program, 2);
  // if (eh.has_errors) {
  //   ErrorHandler_show_all(&eh);
  //   return 1;
  // }
  // return 0;

  SymbolTable table;
  SymbolTable_init(&table, &eh);
  Semantic_analysis(program, &table);

  if (eh.has_errors) {
    ErrorHandler_show_all(&eh);
    return 1;
  }

  // Backend
  Codegen *cg = Codegen_new("tyl_module");
  codegen_program(cg, program);

  // Verification
  Runner_verify(cg);

  switch (mode) {
  case MODE_DUMP:
    printf("=== LLVM IR ===\n");
    Codegen_dump(cg);
    break;

  case MODE_EMIT_IR:
    Runner_emit_ir(cg, "output.ll");
    printf("Emitted output.ll\n");
    break;

  case MODE_JIT:
    Runner_jit(cg);
    break;

  case MODE_COMPILE:
  default:
    Runner_emit_object(cg, "output.o");
    Runner_link_executable("output.o", "output");
    printf("Compiled to executable 'output'\n");
    break;
  }

  return 0;
}
