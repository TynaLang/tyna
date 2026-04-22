#include "sema_internal.h"
#include "tyna/ast.h"

ExprInfo sema_check_expr(Sema *s, AstNode *node) {
  return sema_check_expr_cache(s, node);
}