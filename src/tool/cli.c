#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tyl/cli.h"

void cli_init_options(CliOptions *opts) {
  opts->mode = MODE_JIT;
  opts->ast_print_entrypoint = false;
  opts->ast_print_lib = false;
  opts->ast_print_before_sema = false;
  opts->show_help = false;
  opts->ast_output_path = NULL;
  opts->input_path = NULL;
}

void cli_print_usage(const char *prog_name) {
  printf("Usage: %s [options] <file>\n", prog_name);
  printf("Options:\n");
  printf("  -c, --compile        Compile to executable\n");
  printf("  -j, --jit            JIT compile and run (default)\n");
  printf("  -e, --emit-ir        Emit LLVM IR to file\n");
  printf("  -d, --dump           Dump AST and LLVM IR to stdout\n");
  printf("  --ast                Print AST for the entrypoint\n");
  printf("  --ast-lib            Print AST for the stdlib\n");
  printf("  --ast-no-sema        Print AST before semantic analysis\n");
  printf(
      "  --ast-to=PATH        Redirect AST output to PATH instead of stdout\n");
  printf("  -h, --help           Show this help message\n");
}

bool cli_parse_options(int argc, char **argv, CliOptions *opts) {
  cli_init_options(opts);

  if (argc < 2) {
    return false;
  }

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "-c") == 0 || strcmp(arg, "--compile") == 0) {
      opts->mode = MODE_COMPILE;
    } else if (strcmp(arg, "-j") == 0 || strcmp(arg, "--jit") == 0) {
      opts->mode = MODE_JIT;
    } else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--emit-ir") == 0) {
      opts->mode = MODE_EMIT_IR;
    } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--dump") == 0) {
      opts->mode = MODE_DUMP;
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      opts->show_help = true;
      return true;
    } else if (strcmp(arg, "--ast") == 0) {
      opts->ast_print_entrypoint = true;
    } else if (strcmp(arg, "--ast-lib") == 0) {
      opts->ast_print_lib = true;
    } else if (strcmp(arg, "--ast-no-sema") == 0) {
      opts->ast_print_before_sema = true;
    } else if (strncmp(arg, "--ast-to=", 9) == 0) {
      opts->ast_output_path = arg + 9;
      if (opts->ast_output_path[0] == '\0') {
        return false;
      }
    } else if (arg[0] == '-') {
      printf("Unknown option: %s\n", arg);
      return false;
    } else {
      if (opts->input_path == NULL) {
        opts->input_path = arg;
      } else {
        printf("Error: Multiple files specified.\n");
        return false;
      }
    }
  }

  if (!opts->input_path) {
    printf("Error: No input file specified.\n");
    return false;
  }

  return true;
}

FILE *cli_open_ast_output(const CliOptions *opts) {
  if (!opts->ast_output_path) {
    return stdout;
  }

  FILE *out = fopen(opts->ast_output_path, "w");
  if (!out) {
    perror("Failed to open AST output file");
    return NULL;
  }

  return out;
}
