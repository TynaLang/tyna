#include "sema_internal.h"

void sema_check_impl_decl(Sema *s, AstNode *node) {
  Type *t = node->impl_decl.type;
  StringView name = sv_from_cstr(type_to_name(t));
  if (!t ||
      (t->kind != KIND_STRUCT && t->kind != KIND_UNION &&
       t->kind != KIND_TEMPLATE &&
       !(t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_STRING))) {
    sema_error(s, node,
               "Undefined type '" SV_FMT "' or it is not a struct/union",
               SV_ARG(name));
    return;
  }

  s->generic_context_type = t;
  for (size_t i = 0; i < node->impl_decl.members.len; i++) {
    AstNode *mem = node->impl_decl.members.items[i];
    if (mem->tag != NODE_FUNC_DECL) {
      sema_error(s, mem, "Only functions are allowed in impl blocks");
      continue;
    }

    StringView method_name = mem->func_decl.name->var.value;
    bool duplicate = false;
    for (size_t j = 0; j < t->methods.len; j++) {
      Symbol *existing_method = t->methods.items[j];
      if (sv_eq(existing_method->name, method_name)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      sema_error(s, mem, "Duplicate method '" SV_FMT "' on type '" SV_FMT "'",
                 SV_ARG(method_name), SV_ARG(name));
      continue;
    }

    Symbol *resolved = sema_resolve(s, method_name);
    StringView original_name = method_name;
    if (resolved && resolved->original_name.len)
      original_name = resolved->original_name;

    Symbol *method_sym = xmalloc(sizeof(Symbol));
    method_sym->name = method_name;
    method_sym->original_name = original_name;
    method_sym->type = mem->func_decl.return_type;
    method_sym->kind =
        mem->func_decl.is_static ? SYM_STATIC_METHOD : SYM_METHOD;
    method_sym->value = mem;
    List_push(&t->methods, method_sym);

    if ((t->kind == KIND_STRUCT || t->kind == KIND_UNION) &&
        t->data.instance.from_template) {
      Type *template_type = t->data.instance.from_template;
      Symbol *template_method_sym = xmalloc(sizeof(Symbol));
      *template_method_sym = *method_sym;
      List_push(&template_type->methods, template_method_sym);
    }

    if (mem->func_decl.body && s->pass == SEMA_PASS_BODIES) {
      sema_check_stmt(s, mem);
    }
  }
  s->generic_context_type = NULL;
}
