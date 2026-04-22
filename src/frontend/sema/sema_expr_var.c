#include "sema_internal.h"

ExprInfo sema_check_var(Sema *s, AstNode *node) {
  Symbol *sym = sema_resolve(s, node->var.value);
  if (!sym) {
    if (sv_eq_cstr(node->var.value, "Error")) {
      Type *error_set = type_get_error_set(s->types, node->var.value);
      return (ExprInfo){.type = error_set, .category = VAL_RVALUE};
    }

    Type *type = sema_find_type_by_name(s, node->var.value);
    if (type) {
      node->resolved_type = type;
      return (ExprInfo){.type = type, .category = VAL_RVALUE};
    }

    sema_error(s, node, "Undefined variable '" SV_FMT "'",
               SV_ARG(node->var.value));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  node->resolved_type = sym->type;
  bool is_lvalue = sym->kind == SYM_VAR || sym->kind == SYM_FIELD;
  if (sym->is_moved && is_lvalue) {
    sema_error(s, node, "Use of moved value '%.*s'", (int)node->var.value.len,
               node->var.value.data);
  }
  return (ExprInfo){.type = sym->type,
                    .category = is_lvalue ? VAL_LVALUE : VAL_RVALUE};
}