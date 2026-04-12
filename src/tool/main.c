#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tyl/ast.h"
#include "tyl/codegen.h"
#include "tyl/lexer.h"
#include "tyl/parser.h"
#include "tyl/runner.h"
#include "tyl/semantic.h"
#include "tyl/utils.h"

static Module *module_find_by_name(Sema *sema, StringView name) {
  for (size_t i = 0; i < sema->modules.len; i++) {
    Module *module = sema->modules.items[i];
    if (sv_eq_cstr(name, module->name))
      return module;
  }
  return NULL;
}

static bool module_is_visited(List *visited, Module *module) {
  for (size_t i = 0; i < visited->len; i++) {
    if (visited->items[i] == module)
      return true;
  }
  return false;
}

static void codegen_module_recursive(Codegen *cg, Sema *sema, Module *module,
                                     List *visited) {
  if (module_is_visited(visited, module)) {
    return;
  }

  if (module->ast && module->ast->tag == NODE_AST_ROOT) {
    for (size_t i = 0; i < module->ast->ast_root.children.len; i++) {
      AstNode *child = module->ast->ast_root.children.items[i];
      if (child->tag != NODE_IMPORT)
        continue;

      Module *dep = module_find_by_name(sema, child->import.alias);
      if (!dep)
        continue;

      codegen_module_recursive(cg, sema, dep, visited);
    }
  }

  Codegen_global(cg, module->ast);
  List_push(visited, module);
}

typedef enum { MODE_COMPILE, MODE_JIT, MODE_EMIT_IR, MODE_DUMP } RunMode;

void print_usage(const char *prog_name) {
  printf("Usage: %s [options] <file>\n", prog_name);
  printf("Options:\n");
  printf("  -c, --compile   Compile to executable\n");
  printf("  -j, --jit       JIT compile and run (default)\n");
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

  char *resolved_main_path = realpath(file_path, NULL);
  char *entry_dir = NULL;
  if (resolved_main_path) {
    entry_dir = xstrdup(resolved_main_path);
    char *slash = strrchr(entry_dir, '/');
    if (slash)
      *slash = '\0';
    else
      entry_dir[0] = '\0';
  } else {
    entry_dir = xstrdup(file_path);
    char *slash = strrchr(entry_dir, '/');
    if (slash)
      *slash = '\0';
    else
      strcpy(entry_dir, ".");
  }

  TypeContext *type_ctx = type_context_create();

  const char *std_path = "stdlib/std.tyl";
  const char *std_src = read_file(std_path);
  AstNode *std_ast = NULL;
  ErrorHandler std_eh;
  bool std_eh_init = false;
  if (std_src) {
    ErrorHandler_init(&std_eh, std_src, std_path, std_path, "stdlib");
    std_eh_init = true;
    Lexer std_lexer = make_lexer(std_src, &std_eh);
    std_ast = Parser_process(&std_lexer, &std_eh, type_ctx);
    if (std_eh.has_errors) {
      ErrorHandler_show_all(&std_eh);
      ErrorHandler_free(&std_eh);
      free(entry_dir);
      type_context_free(type_ctx);
      return 1;
    }
  }

  ErrorHandler eh;
  ErrorHandler_init(&eh, src, file_path, entry_dir, NULL);
  Lexer user_lexer = make_lexer(src, &eh);
  AstNode *user_ast = Parser_process(&user_lexer, &eh, type_ctx);

  Sema sema;
  sema_init(&sema, &eh, type_ctx);
  sema.entry_dir = entry_dir;

  if (std_ast) {
    if (std_eh_init)
      sema.eh = &std_eh;
    sema_analyze(&sema, std_ast);
    sema.eh = &eh;

    if (std_eh_init) {
      if (std_eh.has_errors) {
        ErrorHandler_show_all(&std_eh);
        ErrorHandler_free(&std_eh);
        sema_finish(&sema);
        type_context_free(type_ctx);
        return 1;
      }
      ErrorHandler_free(&std_eh);
    }
  }

  Module main_module = {0};
  main_module.name = xstrdup("main");

  if (resolved_main_path) {
    main_module.abs_path = resolved_main_path;
  } else {
    main_module.abs_path = xstrdup(file_path);
  }

  main_module.ast = user_ast;
  List_init(&main_module.symbols);
  List_init(&main_module.exports);
  main_module.is_analyzed = true;
  sema.current_module = &main_module;

  Symbol *array_sym = sema_resolve(&sema, sv_from_parts("Array", 5));
  if (!array_sym) {
    sema_prime_types(&sema);
  }

  sema_analyze(&sema, user_ast);

  // printf("====  USER AST  ====\n");
  // Ast_print(user_ast, 0);

  if (eh.has_errors) {
    ErrorHandler_show_all(&eh);
    sema_finish(&sema);
    type_context_free(type_ctx);
    return 1;
  }

  Codegen *cg = Codegen_new("tyl_module", type_ctx);

  if (std_ast) {
    Codegen_global(cg, std_ast);
  }

  List visited_modules;
  List_init(&visited_modules);
  for (size_t i = 0; i < sema.modules.len; i++) {
    Module *module = sema.modules.items[i];
    codegen_module_recursive(cg, &sema, module, &visited_modules);
  }
  List_free(&visited_modules, 0);

  Codegen_global(cg, user_ast);
  Codegen_program(cg, user_ast);

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

  sema_finish(&sema);
  free((char *)main_module.name);
  free(main_module.abs_path);
  type_context_free(type_ctx);
  return 0;
}
