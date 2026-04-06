
#include "codegen_private.h"

void CGSymbolTable_init(CGSymbolTable *t, CGSymbolTable *parent) {
  t->parent = parent;
  List_init(&t->symbols);
}

void CGSymbolTable_add(CGSymbolTable *t, StringView name, Type *type,
                       LLVMValueRef value) {
  CGSymbol *s = malloc(sizeof(CGSymbol));
  s->name = name;
  s->type = type;
  s->value = value;
  List_push(&t->symbols, s);
}

CGSymbol *CGSymbolTable_find(CGSymbolTable *t, StringView name) {
  while (t) {
    for (size_t i = 0; i < t->symbols.len; i++) {
      CGSymbol *s = t->symbols.items[i];
      if (sv_eq(s->name, name))
        return s;
    }
    t = t->parent;
  }
  return NULL;
}

static void cg_free_symbol_table(CGSymbolTable *t) {
  for (size_t i = 0; i < t->symbols.len; i++) {
    free(t->symbols.items[i]);
  }
  free(t->symbols.items);
}

CGSymbolTable *cg_push_scope(Codegen *cg) {
  CGSymbolTable *new_scope = malloc(sizeof(CGSymbolTable));
  CGSymbolTable_init(new_scope, cg->current_scope);
  cg->current_scope = new_scope;
  return new_scope;
}

void cg_pop_scope(Codegen *cg) {
  if (!cg->current_scope)
    return;
  CGSymbolTable *old = cg->current_scope;
  cg->current_scope = old->parent;
  cg_free_symbol_table(old);
  free(old);
}
