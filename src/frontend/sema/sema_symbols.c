#include "sema_internal.h"

Symbol *sema_resolve(Sema *s, StringView name) {
  for (SemaScope *scope = s->scope; scope != NULL; scope = scope->parent) {
    for (size_t i = 0; i < scope->symbols.len; i++) {
      Symbol *sym = scope->symbols.items[i];
      if (sv_eq(sym->name, name)) {
        return sym;
      }
    }
  }
  return NULL;
}

Symbol *sema_resolve_local(Sema *s, StringView name) {
  if (!s->scope)
    return NULL;

  for (size_t i = 0; i < s->scope->symbols.len; i++) {
    Symbol *sym = s->scope->symbols.items[i];
    if (sv_eq(sym->name, name)) {
      return sym;
    }
  }
  return NULL;
}

Symbol *sema_define(Sema *s, StringView name, Type *type, bool is_const,
                    Location loc) {
  for (size_t i = 0; i < s->scope->symbols.len; i++) {
    Symbol *sym = s->scope->symbols.items[i];
    if (sv_eq(sym->name, name)) {
      ErrorHandler_report(s->eh, loc, "Redefinition of symbol in this scope");
      return NULL;
    }
  }

  Symbol *sym = xmalloc(sizeof(Symbol));
  sym->name = name;
  sym->original_name = name;
  sym->type = type;
  sym->is_const = is_const;
  sym->is_export = false;
  sym->is_pub_module = false;
  sym->is_external = false;
  sym->is_moved = false;
  sym->requires_storage = 0;
  sym->requires_arena = 0;
  sym->value = NULL;
  sym->func_status = FUNC_NONE;
  sym->kind = SYM_VAR;
  sym->builtin_kind = BUILTIN_NONE;
  sym->scope = s->scope;
  sym->module = s->current_module;

  List_push(&s->scope->symbols, sym);
  return sym;
}