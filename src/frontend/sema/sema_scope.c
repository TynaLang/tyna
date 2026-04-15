#include "sema_internal.h"

void sema_scope_push(Sema *s) {
  SemaScope *new_scope = xmalloc(sizeof(SemaScope));
  new_scope->parent = s->scope;
  List_init(&new_scope->symbols);
  s->scope = new_scope;
}

void sema_scope_pop(Sema *s) {
  if (s->scope == NULL)
    return;

  SemaScope *old = s->scope;
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

bool type_needs_inference(Type *t) {
  if (!t)
    return true;

  if (t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_UNKNOWN)
    return true;

  if (t->kind == KIND_TEMPLATE)
    return true;

  if (t->kind == KIND_STRUCT && t->data.instance.from_template != NULL) {
    return false;
  }

  return false;
}

bool type_is_condition(Type *t) {
  if (!t)
    return false;

  if (type_is_bool(t))
    return true;

  if (t->kind == KIND_POINTER)
    return true;

  return false;
}

bool type_can_be_polymorphic(Type *t) {
  if (!t)
    return false;

  switch (t->kind) {
  case KIND_TEMPLATE:
    return true;
  case KIND_POINTER:
    return type_can_be_polymorphic(t->data.pointer_to);
  case KIND_STRUCT:
  case KIND_UNION:
    if (t->data.instance.from_template == NULL)
      return false;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      if (type_can_be_polymorphic(t->data.instance.generic_args.items[i]))
        return true;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (type_can_be_polymorphic(m->type))
        return true;
    }
    return false;
  default:
    return false;
  }
}

bool sema_allows_polymorphic_types(Sema *s) {
  return s && s->generic_context_type != NULL;
}

bool type_is_writable(Sema *s, AstNode *target) {
  if (!type_is_lvalue(target))
    return false;

  if (target->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, target->var.value);

    if (sym && sym->is_const)
      return false;
    return true;
  }

  if (target->tag == NODE_INDEX) {
    Type *arr_type = target->index.array->resolved_type;

    if (arr_type && arr_type->kind == KIND_PRIMITIVE &&
        arr_type->data.primitive == PRIM_STRING) {
      return false;
    }

    return true;
  }

  if (target->tag == NODE_FIELD) {
    return true;
  }

  return true;
}
