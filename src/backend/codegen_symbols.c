
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
  s->is_moved = 0;
  s->is_direct_value = false;
  List_push(&t->symbols, s);
}

void CGSymbolTable_add_direct(CGSymbolTable *t, StringView name, Type *type,
                              LLVMValueRef value) {
  CGSymbol *s = malloc(sizeof(CGSymbol));
  s->name = name;
  s->type = type;
  s->value = value;
  s->is_moved = 0;
  s->is_direct_value = true;
  List_push(&t->symbols, s);
}

void cg_mark_symbol_moved(CGSymbolTable *t, StringView name) {
  while (t) {
    for (size_t i = 0; i < t->symbols.len; i++) {
      CGSymbol *s = t->symbols.items[i];
      if (sv_eq(s->name, name)) {
        s->is_moved = 1;
        return;
      }
    }
    t = t->parent;
  }
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

  List *defer_list = malloc(sizeof(List));
  List_init(defer_list);
  List_push(&cg->defers, defer_list);

  return new_scope;
}

void cg_cleanup_string_vars_in_scope(Codegen *cg, CGSymbolTable *scope) {
  if (!cg || !scope)
    return;

  CGFunction *free_fn = cg_find_function(cg, sv_from_cstr("__tyl_string_free"));
  if (!free_fn)
    return;

  for (size_t i = 0; i < scope->symbols.len; i++) {
    CGSymbol *sym = scope->symbols.items[i];
    if (sym->type && sym->type->kind == KIND_STRING_BUFFER && !sym->is_moved) {
      LLVMValueRef args[] = {sym->value};
      LLVMBuildCall2(cg->builder, free_fn->type, free_fn->value, args, 1, "");
    }
  }
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
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    cg_cleanup_string_vars_in_scope(cg, cg->current_scope);
  }
  List_free(defer_list, 0);
  free(defer_list);

  CGSymbolTable *old = cg->current_scope;
  cg->current_scope = old->parent;
  cg_free_symbol_table(old);
  free(old);
}
