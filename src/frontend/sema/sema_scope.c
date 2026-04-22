#include "sema_internal.h"

void sema_scope_push(Sema *s) {
  SemaScope *new_scope = xmalloc(sizeof(SemaScope));
  new_scope->parent = s->scope;
  List_init(&new_scope->symbols);
  new_scope->has_computed_str = false;
  s->scope = new_scope;
}

void sema_scope_pop(Sema *s) {
  if (s->scope == NULL)
    return;

  SemaScope *old = s->scope;
  if (old->has_computed_str && old->parent) {
    old->parent->has_computed_str = true;
  }

  s->scope = old->parent;

  for (size_t i = 0; i < old->symbols.len; i++) {
    free(old->symbols.items[i]);
  }
  List_free(&old->symbols, 0);
  free(old);
}

void sema_jump_push(Sema *s, SemaJump *ctx, StringView label, AstNode *node,
                    bool is_loop, bool is_breakable) {
  ctx->label = label;
  ctx->node = node;
  ctx->is_loop = is_loop;
  ctx->is_breakable = is_breakable;
  ctx->parent = s->jump;
  s->jump = ctx;
}

void sema_jump_pop(Sema *s) {
  if (s->jump) {
    s->jump = s->jump->parent;
  }
}

SemaJump *sema_find_break_jump(Sema *s) {
  for (SemaJump *j = s->jump; j != NULL; j = j->parent) {
    if (j->is_breakable) {
      return j;
    }
  }
  return NULL;
}

SemaJump *sema_find_loop_jump(Sema *s) {
  for (SemaJump *j = s->jump; j != NULL; j = j->parent) {
    if (j->is_loop) {
      return j;
    }
  }
  return NULL;
}
