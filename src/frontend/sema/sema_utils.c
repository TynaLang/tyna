#include <stdarg.h>

#include "sema_internal.h"

AstNode *sema_find_underlying_var(AstNode *node) {
  if (!node)
    return NULL;
  if (node->tag == NODE_VAR)
    return node;
  if (node->tag == NODE_UNARY && node->unary.op == OP_ADDR_OF)
    return sema_find_underlying_var(node->unary.expr);
  return NULL;
}

void sema_mark_symbol_requires_storage(Symbol *sym) {
  if (!sym || sym->kind != SYM_VAR)
    return;
  sym->requires_storage = 1;
  if (sym->value && sym->value->tag == NODE_PARAM) {
    sym->value->param.requires_storage = true;
  }
}

void sema_mark_moved_symbol(Symbol *sym) {
  if (!sym || sym->kind != SYM_VAR)
    return;
  sym->is_moved = 1;
}

void sema_mark_node_requires_storage(Sema *s, AstNode *node) {
  AstNode *var_node = sema_find_underlying_var(node);
  if (!var_node)
    return;
  Symbol *sym = sema_resolve(s, var_node->var.value);
  sema_mark_symbol_requires_storage(sym);
}

void sema_error(Sema *s, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(s->eh, n->loc, "%s", msg_buf);
}

void sema_error_at(Sema *s, Location loc, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(s->eh, loc, "%s", msg_buf);
}

void sema_warning(Sema *s, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report_level(s->eh, n->loc, LEVEL_WARNING, "%s", msg_buf);
}

void sema_warning_at(Sema *s, Location loc, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report_level(s->eh, loc, LEVEL_WARNING, "%s", msg_buf);
}