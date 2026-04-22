#include "sema_internal.h"

static bool sema_symbol_needs_drop(Symbol *sym) {
  return sym && sym->kind == SYM_VAR && !sym->is_moved && sym->type &&
         sym->type->kind == KIND_STRING_BUFFER;
}

void sema_inject_drop_calls(Sema *s, AstNode *block, List *symbols_to_drop) {
  (void)s;
  if (!block || block->tag != NODE_BLOCK || !symbols_to_drop)
    return;

  for (size_t i = 0; i < symbols_to_drop->len; i++) {
    Symbol *sym = symbols_to_drop->items[i];
    if (!sema_symbol_needs_drop(sym))
      continue;

    Location loc = block->loc;
    if (sym->value)
      loc = sym->value->loc;

    List args;
    List_init(&args);
    AstNode *arg =
        AstNode_new_unary(OP_ADDR_OF, AstNode_new_var(sym->name, loc), loc);
    List_push(&args, arg);

    AstNode *call = AstNode_new_call(
        AstNode_new_var(sv_from_cstr("__tyl_string_free"), loc), args, loc);
    AstNode *expr_stmt = AstNode_new_expr_stmt(call, loc);
    List_push(&block->block.statements, expr_stmt);
  }
}
