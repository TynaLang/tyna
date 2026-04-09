#include "sema_internal.h"
#include "tyl/semantic.h"

void sema_register_func_signature(Sema *s, AstNode *node) {
  StringView name = node->func_decl.name->var.value;

  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Redefinition of symbol '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Symbol *sym =
      sema_define(s, name, node->func_decl.return_type, true, node->loc);

  if (sym) {
    sym->func_status = node->func_decl.body ? FUNC_IMPLEMENTATION : FUNC_NONE;
    sym->value = node;
  }
}

static int signature_matches(AstNode *dec, AstNode *imp) {
  if (dec->tag != NODE_FUNC_DECL || imp->tag != NODE_FUNC_DECL)
    return 0;

  if (dec->func_decl.return_type != imp->func_decl.return_type)
    return 0;

  if (dec->func_decl.params.len != imp->func_decl.params.len)
    return 0;

  List decl_params = dec->func_decl.params;
  List imp_params = imp->func_decl.params;
  for (size_t i = 0; i < decl_params.len; i++) {
    AstNode *p1 = decl_params.items[i];
    AstNode *p2 = imp_params.items[i];
    if (p1->param.type != p2->param.type)
      return 0;
  }
  return 1;
}
