#include "sema_internal.h"
#include "tyna/lexer.h"
#include "tyna/sema.h"
#include <stdio.h>

void sema_prime_types(Sema *s) {
  Type *array_tpl = type_get_template(s->types, sv_from_parts("Array", 5));
  if (!array_tpl) {
    array_tpl = xcalloc(1, sizeof(Type));
    array_tpl->kind = KIND_TEMPLATE;
    array_tpl->name = sv_from_parts("Array", 5);
    array_tpl->is_intrinsic = true;
    array_tpl->is_frozen = false;
    List_init(&array_tpl->members);
    List_init(&array_tpl->methods);
    List_init(&array_tpl->data.template.placeholders);
    List_init(&array_tpl->data.template.fields);

    StringView *t_placeholder = xmalloc(sizeof(StringView));
    *t_placeholder = sv_from_parts("T", 1);
    List_push(&array_tpl->data.template.placeholders, t_placeholder);

    Type *t_type = xcalloc(1, sizeof(Type));
    t_type->kind = KIND_TEMPLATE;
    t_type->name = *t_placeholder;

    type_add_member(array_tpl, "data", type_get_pointer(s->types, t_type), 0);
    type_add_member(array_tpl, "len", type_get_primitive(s->types, PRIM_I64),
                    8);
    type_add_member(array_tpl, "cap", type_get_primitive(s->types, PRIM_I64),
                    16);
    type_add_member(array_tpl, "rank", type_get_primitive(s->types, PRIM_I64),
                    24);
    type_add_member(
        array_tpl, "dims",
        type_get_pointer(s->types, type_get_primitive(s->types, PRIM_I64)), 32);

    array_tpl->size = 40;
    List_push(&s->types->templates, array_tpl);
  }

  if (!sema_resolve(s, array_tpl->name)) {
    sema_define(s, array_tpl->name, array_tpl, true, (Location){0, 0});
  }
}

static Symbol *sema_define_builtin(Sema *s, StringView name, Type *type,
                                   BuiltinKind builtin_kind) {
  Symbol *sym = sema_define(s, name, type, true, (Location){0, 0});
  if (!sym)
    return NULL;
  sym->kind = SYM_FUNC;
  sym->builtin_kind = builtin_kind;
  return sym;
}

static void sema_register_builtins(Sema *s) {
  sema_define_builtin(s, sv_from_parts("typeof", 6),
                      type_get_primitive(s->types, PRIM_STRING),
                      BUILTIN_TYPEOF);
  sema_define_builtin(s, sv_from_parts("free", 4),
                      type_get_primitive(s->types, PRIM_VOID), BUILTIN_FREE);
}

static void sema_check_function_bodies(Sema *s, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_FUNC_DECL:
    if (node->func_decl.body) {
      sema_check_stmt(s, node);
    }
    break;

  case NODE_STRUCT_DECL: {
    Type *old_generic_context = s->generic_context_type;
    Type *t = NULL;
    StringView name = node->struct_decl.name->var.value;
    Symbol *sym = sema_resolve(s, name);
    if (sym)
      t = sym->type;
    if (t && (t->data.instance.from_template || t->kind == KIND_TEMPLATE)) {
      s->generic_context_type = t;
    }
    for (size_t i = 0; i < node->struct_decl.members.len; i++) {
      AstNode *member = node->struct_decl.members.items[i];
      if (member->tag == NODE_FUNC_DECL && member->func_decl.body) {
        sema_check_stmt(s, member);
      }
    }
    s->generic_context_type = old_generic_context;
    break;
  }

  case NODE_IMPL_DECL: {
    Type *old_generic_context = s->generic_context_type;
    s->generic_context_type = node->impl_decl.type;
    for (size_t i = 0; i < node->impl_decl.members.len; i++) {
      AstNode *member = node->impl_decl.members.items[i];
      if (member->tag == NODE_FUNC_DECL && member->func_decl.body) {
        sema_check_stmt(s, member);
      }
    }
    s->generic_context_type = old_generic_context;
    break;
  }

  default:
    break;
  }
}

static void module_free(Module *module) {
  if (!module)
    return;

  Ast_free(module->ast);
  for (size_t i = 0; i < module->symbols.len; i++) {
    free(module->symbols.items[i]);
  }
  List_free(&module->symbols, 0);
  List_free(&module->exports, 0);
  free(module->abs_path);
  free((char *)module->name);
  free(module);
}

void sema_init(Sema *s, ErrorHandler *eh, TypeContext *type_ctx) {
  s->types = type_ctx;
  s->eh = eh;

  s->ret_type = NULL;
  s->fn_node = NULL;
  s->generic_context_type = NULL;
  s->pass = SEMA_PASS_REGISTRATION;
  s->jump = NULL;
  s->scope = NULL;
  List_init(&s->modules);
  s->current_module = NULL;
  s->entry_dir = NULL;

  sema_scope_push(s);
  sema_register_builtins(s);
}

void sema_finish(Sema *s) {
  while (s->scope) {
    sema_scope_pop(s);
  }

  for (size_t i = 0; i < s->modules.len; i++) {
    module_free(s->modules.items[i]);
  }
  List_free(&s->modules, 0);
  free(s->entry_dir);
  s->entry_dir = NULL;
}

void sema_analyze(Sema *s, AstNode *root) {
  if (!root || root->tag != NODE_AST_ROOT)
    return;

  s->pass = SEMA_PASS_REGISTRATION;
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      sema_register_func_signature(s, node);
    } else if (node->tag == NODE_STRUCT_DECL) {
      for (size_t j = 0; j < node->struct_decl.members.len; j++) {
        AstNode *member = node->struct_decl.members.items[j];
        if (member->tag == NODE_FUNC_DECL) {
          sema_register_method_signature(s, member,
                                         node->struct_decl.name->var.value);
        }
      }
    } else if (node->tag == NODE_IMPL_DECL) {
      StringView owner_name = sv_from_cstr(type_to_name(node->impl_decl.type));
      for (size_t j = 0; j < node->impl_decl.members.len; j++) {
        AstNode *member = node->impl_decl.members.items[j];
        if (member->tag == NODE_FUNC_DECL) {
          sema_register_method_signature(s, member, owner_name);
        }
      }
    }
  }

  s->pass = SEMA_PASS_DEFINITION;
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      sema_check_stmt(s, node);
    } else if (node->tag == NODE_STRUCT_DECL) {
      sema_check_struct_decl(s, node);
    } else if (node->tag == NODE_UNION_DECL) {
      sema_check_union_decl(s, node);
    } else if (node->tag == NODE_ERROR_DECL) {
      sema_check_error_decl(s, node);
    } else if (node->tag == NODE_ERROR_SET_DECL) {
      sema_check_error_set_decl(s, node);
    } else if (node->tag == NODE_IMPL_DECL) {
      sema_check_impl_decl(s, node);
    } else {
      sema_check_stmt(s, node);
    }
  }

  s->pass = SEMA_PASS_BODIES;
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    sema_check_function_bodies(s, root->ast_root.children.items[i]);
  }

  type_refresh_drop_metadata(s->types);
}