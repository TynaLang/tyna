#include "cg_internal.h"

void CGSymbolTable_init(CgSymtab *t, CgSymtab *parent) {
  t->parent = parent;
  List_init(&t->symbols);
}

void CGSymbolTable_add(CgSymtab *t, StringView name, Type *type,
                       LLVMValueRef value) {
  CgSym *s = malloc(sizeof(CgSym));
  s->name = name;
  s->type = type;
  s->value = value;
  s->is_direct_value = false;
  List_push(&t->symbols, s);
}

void CGSymbolTable_add_direct(CgSymtab *t, StringView name, Type *type,
                              LLVMValueRef value) {
  CgSym *s = malloc(sizeof(CgSym));
  s->name = name;
  s->type = type;
  s->value = value;
  s->is_direct_value = true;
  List_push(&t->symbols, s);
}

CgSym *CGSymbolTable_find(CgSymtab *t, StringView name) {
  while (t) {
    for (size_t i = 0; i < t->symbols.len; i++) {
      CgSym *s = t->symbols.items[i];
      if (sv_eq(s->name, name))
        return s;
    }
    t = t->parent;
  }
  return NULL;
}

static void cg_free_symbol_table(CgSymtab *t) {
  for (size_t i = 0; i < t->symbols.len; i++) {
    free(t->symbols.items[i]);
  }
  free(t->symbols.items);
}

CgSymtab *cg_push_scope(Codegen *cg) {
  CgSymtab *new_scope = malloc(sizeof(CgSymtab));
  CGSymbolTable_init(new_scope, cg->current_scope);
  cg->current_scope = new_scope;

  List *defer_list = malloc(sizeof(List));
  List_init(defer_list);
  List_push(&cg->defers, defer_list);

  return new_scope;
}

void cg_pop_scope(Codegen *cg) {
  if (!cg->current_scope)
    return;

  List *defer_list = List_pop(&cg->defers);
  for (int i = (int)defer_list->len - 1; i >= 0; i--) {
    AstNode *defer_node = defer_list->items[i];
    if (defer_node->tag == NODE_DEFER) {
      cg_statement(cg, defer_node->defer.expr);
    } else {
      cg_statement(cg, defer_node);
    }
  }
  List_free(defer_list, 0);
  free(defer_list);

  CgSymtab *old = cg->current_scope;
  cg->current_scope = old->parent;
  cg_free_symbol_table(old);
  free(old);
}
