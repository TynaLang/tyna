#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "sema/sema_internal.h"
#include "tyna/ast.h"
#include "tyna/errors.h"
#include "tyna/parser.h"
#include "tyna/sema.h"
#include "tyna/utils.h"

static char *path_dirname(const char *path) {
  if (!path || path[0] == '\0')
    return xstrdup(".");

  const char *slash = strrchr(path, '/');
  if (!slash)
    return xstrdup(".");

  size_t len = slash - path;
  char *dir = xmalloc(len + 1);
  memcpy(dir, path, len);
  dir[len] = '\0';
  return dir;
}

static char *path_basename(const char *path) {
  if (!path || path[0] == '\0')
    return xstrdup("");

  const char *slash = strrchr(path, '/');
  if (!slash)
    return xstrdup(path);

  return xstrdup(slash + 1);
}

static char *path_from_import(const char *import_name,
                              const char *current_file_path) {
  char pathbuf[PATH_MAX];
  char *normalized = xstrdup(import_name);
  bool has_ext = false;
  size_t len = strlen(normalized);
  if (len > 0 && normalized[len - 1] == ';') {
    normalized[len - 1] = '\0';
    len--;
  }
  if (len > 4 && strcmp(normalized + len - 4, ".tn") == 0) {
    has_ext = true;
  }

  if (strncmp(normalized, "std.", 4) == 0) {
    char *rest = normalized + 4;
    for (char *p = rest; *p; ++p) {
      if (*p == '.')
        *p = '/';
    }
    if (!has_ext)
      snprintf(pathbuf, sizeof(pathbuf), "stdlib/%s.tn", rest);
    else
      snprintf(pathbuf, sizeof(pathbuf), "stdlib/%s", rest);
  } else {
    char *dir = path_dirname(current_file_path);
    char *base = path_basename(dir);
    for (char *p = normalized; *p; ++p) {
      if (*p == '.')
        *p = '/';
    }

    size_t base_len = strlen(base);
    if (base_len > 0 && normalized[0] != '\0' &&
        strncmp(normalized, base, base_len) == 0 &&
        normalized[base_len] == '/') {
      memmove(normalized, normalized + base_len + 1,
              strlen(normalized + base_len + 1) + 1);
    }

    if (!has_ext)
      snprintf(pathbuf, sizeof(pathbuf), "%s/%s.tn", dir, normalized);
    else
      snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, normalized);
    free(base);
    free(dir);
  }

  free(normalized);

  char resolved[PATH_MAX];
  if (realpath(pathbuf, resolved)) {
    return xstrdup(resolved);
  }

  return xstrdup(pathbuf);
}

static StringView module_alias(StringView import_path) {
  size_t pos = import_path.len;
  while (pos > 0 && import_path.data[pos - 1] != '.')
    pos--;
  return sv_from_parts(import_path.data + pos, import_path.len - pos);
}

Module *module_find_by_path(Sema *s, const char *abs_path) {
  for (size_t i = 0; i < s->modules.len; i++) {
    Module *mod = s->modules.items[i];
    if (strcmp(mod->abs_path, abs_path) == 0)
      return mod;
  }
  return NULL;
}

Symbol *module_lookup_export(Module *module, StringView name) {
  for (size_t i = 0; i < module->exports.len; i++) {
    Symbol *sym = module->exports.items[i];
    StringView lookup = sym->original_name.len ? sym->original_name : sym->name;
    if (sv_eq(lookup, name))
      return sym;
  }
  return NULL;
}

static char *module_name_from_path(StringView import_path) {
  StringView alias = module_alias(import_path);
  char *name = xmalloc(alias.len + 1);
  memcpy(name, alias.data, alias.len);
  name[alias.len] = '\0';
  return name;
}

static void module_collect_exports(Module *module) {
  for (size_t i = 0; i < module->symbols.len; i++) {
    Symbol *sym = module->symbols.items[i];
    if (sym->is_export) {
      List_push(&module->exports, sym);
    }
  }
}

static void module_detach_scope_symbols(Sema *s, Module *module) {
  if (!s->scope)
    return;

  module->symbols = s->scope->symbols;
  for (size_t i = 0; i < module->symbols.len; i++) {
    Symbol *sym = module->symbols.items[i];
    sym->scope = NULL;
  }
  s->scope->symbols.items = NULL;
  s->scope->symbols.len = 0;
  s->scope->symbols.capacity = 0;
}

Module *module_resolve_or_load(Sema *s, StringView import_path,
                               const char *current_file_path) {
  char *path =
      path_from_import(sv_to_cstr_temp(import_path), current_file_path);
  if (!path)
    return NULL;

  Module *existing = module_find_by_path(s, path);
  if (existing) {
    if (!existing->is_analyzed) {
      sema_error_at(s, (Location){0, 0},
                    "Cyclic import detected for module '%s'",
                    sv_to_cstr_temp(import_path));
    }
    free(path);
    return existing;
  }

  const char *src = read_file(path);
  if (!src) {
    sema_error_at(s, (Location){0, 0}, "Could not read imported module '%s'",
                  sv_to_cstr_temp(import_path));
    free(path);
    return NULL;
  }

  ErrorHandler module_eh;
  ErrorHandler_init(&module_eh, src, path, s->entry_dir, NULL);
  Lexer lexer = make_lexer(src, &module_eh);
  AstNode *ast = parser_process(&lexer, &module_eh, s->types);
  if (module_eh.has_errors) {
    ErrorHandler_show_all(&module_eh);
    free(path);
    return NULL;
  }

  Module *module = xmalloc(sizeof(Module));
  module->name = module_name_from_path(import_path);
  module->abs_path = path;
  module->ast = ast;
  List_init(&module->symbols);
  List_init(&module->exports);
  module->is_analyzed = false;

  List_push(&s->modules, module);

  Sema module_sema;
  sema_init(&module_sema, &module_eh, s->types);
  module_sema.current_module = module;
  module_sema.modules = s->modules;
  sema_analyze(&module_sema, module->ast);

  module_detach_scope_symbols(&module_sema, module);
  module_collect_exports(module);
  module->is_analyzed = true;

  // Preserve any modules loaded during nested analysis in the parent registry.
  s->modules = module_sema.modules;
  module_sema.modules.items = NULL;
  module_sema.modules.len = 0;
  module_sema.modules.capacity = 0;
  sema_finish(&module_sema);

  return module;
}
