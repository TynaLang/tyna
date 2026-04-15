#include "sema_internal.h"

void sema_check_struct_decl(Sema *s, AstNode *node) {
  StringView name = node->struct_decl.name->var.value;
  Symbol *existing = sema_resolve(s, name);
  Type *t = NULL;

  if (existing) {
    if (!existing->type->is_intrinsic) {
      sema_error(s, node, "Duplicate symbol '" SV_FMT "'", SV_ARG(name));
      return;
    }
    t = existing->type;
  } else {
    if (node->struct_decl.placeholders.len > 0) {
      t = xcalloc(1, sizeof(Type));
      t->kind = KIND_TEMPLATE;
      t->name = name;
      List_init(&t->members);
      List_init(&t->methods);
      List_init(&t->data.template.placeholders);
      List_init(&t->data.template.fields);

      for (size_t i = 0; i < node->struct_decl.placeholders.len; i++) {
        List_push(&t->data.template.placeholders,
                  node->struct_decl.placeholders.items[i]);
      }
      List_push(&s->types->templates, t);
    } else {
      t = type_get_struct(s->types, name);
      if (!t) {
        sema_error(s, node, "Failed to create struct type");
        return;
      }
    }
    Symbol *sym = sema_define(s, name, t, true, node->loc);
    if (sym) {
      sym->is_export = node->struct_decl.is_export;
    }
  }

  t->is_frozen = node->struct_decl.is_frozen;

  Type *old_generic_context = s->generic_context_type;
  if (t->data.instance.from_template || t->kind == KIND_TEMPLATE) {
    s->generic_context_type = t;
  }

  size_t offset = (existing && existing->type->is_intrinsic) ? t->size : 0;

  for (size_t i = 0; i < node->struct_decl.members.len; i++) {
    AstNode *mem = node->struct_decl.members.items[i];

    if (mem->tag == NODE_FUNC_DECL) {
      StringView method_name = mem->func_decl.name->var.value;
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

      if (mem->func_decl.body && s->pass == SEMA_PASS_BODIES) {
        sema_check_stmt(s, mem);
      }
      continue;
    }

    Type *mem_type = mem->var_decl.declared_type;
    char *c_mem_name = sv_to_cstr(mem->var_decl.name->var.value);
    size_t align = mem_type->alignment ? mem_type->alignment : mem_type->size;
    if (align == 0)
      align = 1;

    if (type_get_member(t, mem->var_decl.name->var.value)) {
      if (!t->is_intrinsic) {
        sema_error(s, mem, "Duplicate field '" SV_FMT "'",
                   SV_ARG(mem->var_decl.name->var.value));
      }
      free(c_mem_name);
      continue;
    }

    offset = align_to(offset, align);
    type_add_member(t, c_mem_name, mem_type, offset);
    free(c_mem_name);
    offset += mem_type->size;
    if (align > t->alignment)
      t->alignment = align;
  }

  if (!existing || !existing->type->is_intrinsic) {
    t->size = align_to(offset, t->alignment ? t->alignment : 1);
  }

  s->generic_context_type = old_generic_context;
}
