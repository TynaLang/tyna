#include "sema_internal.h"
#include "tyl/semantic.h"
#include "tyl/utils.h"

static StringView sema_mangle_method_name(StringView owner, StringView method) {
  if (owner.len == 0)
    return method;

  char buf[512];
  int len = snprintf(buf, sizeof(buf), "_TZN%zu%.*s%zu%.*s", owner.len,
                     (int)owner.len, owner.data, method.len, (int)method.len,
                     method.data);
  char *heap = xmalloc(len + 1);
  memcpy(heap, buf, len + 1);
  return sv_from_parts(heap, len);
}

static StringView sema_mangle_module_symbol(const char *module_name,
                                            StringView original_name) {
  if (!module_name || module_name[0] == '\0')
    return original_name;

  size_t module_len = strlen(module_name);
  char buf[512];
  int len = snprintf(buf, sizeof(buf), "_TM_%.*s_F_%.*s", (int)module_len,
                     module_name, (int)original_name.len, original_name.data);
  char *heap = xmalloc(len + 1);
  memcpy(heap, buf, len + 1);
  return sv_from_parts(heap, len);
}

void sema_register_func_signature(Sema *s, AstNode *node) {
  StringView name = node->func_decl.name->var.value;

  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Redefinition of symbol '" SV_FMT "'", SV_ARG(name));
    return;
  }

  StringView actual_name = name;
  if (node->func_decl.is_export && !node->func_decl.is_external &&
      s->current_module) {
    actual_name = sema_mangle_module_symbol(s->current_module->name, name);
  }

  Symbol *sym =
      sema_define(s, actual_name, node->func_decl.return_type, true, node->loc);

  if (sym) {
    sym->original_name = name;
    sym->is_export = node->func_decl.is_export;
    sym->is_external = node->func_decl.is_external;
    sym->func_status = node->func_decl.body ? FUNC_IMPLEMENTATION : FUNC_NONE;
    sym->value = node;
  }

  if (sym && actual_name.data != name.data) {
    node->func_decl.name->var.value = actual_name;
  }
}

void sema_register_method_signature(Sema *s, AstNode *node,
                                    StringView owner_name) {
  StringView original_name = node->func_decl.name->var.value;
  StringView mangled_name = sema_mangle_method_name(owner_name, original_name);

  if (sema_resolve_local(s, mangled_name)) {
    sema_error(s, node, "Redefinition of symbol '" SV_FMT "'",
               SV_ARG(mangled_name));
    return;
  }

  Symbol *sym = sema_define(s, mangled_name, node->func_decl.return_type, true,
                            node->loc);

  bool is_exported_method = node->func_decl.is_export;
  Symbol *owner_sym = sema_resolve_local(s, owner_name);
  if (owner_sym && owner_sym->is_export) {
    is_exported_method = true;
  }

  if (sym) {
    sym->original_name = original_name;
    sym->func_status = node->func_decl.body ? FUNC_IMPLEMENTATION : FUNC_NONE;
    sym->value = node;
    sym->kind = node->func_decl.is_static ? SYM_STATIC_METHOD : SYM_METHOD;
    sym->is_export = is_exported_method;

    node->func_decl.name->var.value = mangled_name;
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
