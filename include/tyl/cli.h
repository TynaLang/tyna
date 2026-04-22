#ifndef TYL_CLI_H
#define TYL_CLI_H

#include <stdbool.h>
#include <stdio.h>

typedef enum {
  MODE_COMPILE,
  MODE_JIT,
  MODE_EMIT_IR,
  MODE_DUMP,
} RunMode;

typedef struct {
  RunMode mode;
  bool ast_print_entrypoint;
  bool ast_print_lib;
  bool ast_print_before_sema;
  bool show_help;
  const char *ast_output_path;
  const char *input_path;
} CliOptions;

void cli_init_options(CliOptions *opts);
bool cli_parse_options(int argc, char **argv, CliOptions *opts);
void cli_print_usage(const char *prog_name);
FILE *cli_open_ast_output(const CliOptions *opts);

#endif // TYL_CLI_H
