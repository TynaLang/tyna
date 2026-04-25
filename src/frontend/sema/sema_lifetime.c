#include "sema_internal.h"

static bool sema_type_needs_drop(Type *type, List *seen) {
  if (!type)
    return false;

  for (size_t i = 0; i < seen->len; i++) {
    if (seen->items[i] == type)
      return false;
  }

  switch (type->kind) {
  case KIND_STRING_BUFFER:
    return true;
  case KIND_POINTER:
  case KIND_PRIMITIVE:
    return false;
  case KIND_STRUCT:
  case KIND_UNION:
    List_push(seen, type);
    for (size_t i = 0; i < type->members.len; i++) {
      Member *member = type->members.items[i];
      if (member && sema_type_needs_drop(member->type, seen)) {
        List_pop(seen);
        return true;
      }
    }
    List_pop(seen);
    return false;
  default:
    return type->needs_drop;
  }
}

static bool sema_symbol_needs_drop(Symbol *sym) {
  if (!sym || sym->kind != SYM_VAR || sym->is_moved || !sym->type)
    return false;

  List seen;
  List_init(&seen);
  bool needs_drop = sema_type_needs_drop(sym->type, &seen);
  List_free(&seen, 0);
  return needs_drop;
}

void sema_get_drops_for_scope(Sema *s, SemaScope *scope,
                              List *out_symbols_to_drop) {
  (void)s;
  if (!scope || !out_symbols_to_drop)
    return;

  for (size_t i = 0; i < scope->symbols.len; i++) {
    Symbol *sym = scope->symbols.items[i];
    if (sema_symbol_needs_drop(sym)) {
      List_push(out_symbols_to_drop, sym);
    }
  }
}
