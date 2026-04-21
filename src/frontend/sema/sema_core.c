#include "sema_internal.h"
#include "tyl/lexer.h"
#include "tyl/sema.h"
#include <stdio.h>

void sema_prime_types(Sema *s) {
  Type *array_tpl = type_get_template(s->types, sv_from_parts("Array", 5));
  if (!array_tpl) {
    // 1. Create the Template Type
    array_tpl = xcalloc(1, sizeof(Type));
    array_tpl->kind = KIND_TEMPLATE;
    array_tpl->name = sv_from_parts("Array", 5);
    array_tpl->is_intrinsic = true;
    array_tpl->is_frozen = false;
    List_init(&array_tpl->members);
    List_init(&array_tpl->methods);
    List_init(&array_tpl->data.template.placeholders);
    List_init(&array_tpl->data.template.fields);

    // 2. Add the placeholder "T"
    StringView *t_placeholder = xmalloc(sizeof(StringView));
    *t_placeholder = sv_from_parts("T", 1);
    List_push(&array_tpl->data.template.placeholders, t_placeholder);

    // 3. Define the members (using KIND_TEMPLATE for the placeholder)
    Type *t_type = xcalloc(1, sizeof(Type));
    t_type->kind = KIND_TEMPLATE;
    t_type->name = *t_placeholder;

    // data: ptr<T>
    type_add_member(array_tpl, "data", type_get_pointer(s->types, t_type), 0);
    // len: i64
    type_add_member(array_tpl, "len", type_get_primitive(s->types, PRIM_I64),
                    8);
    // cap: i64
    type_add_member(array_tpl, "cap", type_get_primitive(s->types, PRIM_I64),
                    16);
    // rank: i64
    type_add_member(array_tpl, "rank", type_get_primitive(s->types, PRIM_I64),
                    24);
    // dims: ptr<i64>
    type_add_member(
        array_tpl, "dims",
        type_get_pointer(s->types, type_get_primitive(s->types, PRIM_I64)), 32);

    array_tpl->size = 40; // ptr + i64 + i64 + i64 + ptr

    // 4. Register it in the context so check_array_expr can find it
    List_push(&s->types->templates, array_tpl);
  }

  if (!sema_resolve(s, array_tpl->name)) {
    // Define it in the current semantic scope only when needed.
    sema_define(s, array_tpl->name, array_tpl, true, (Location){0, 0});
  }
}

Symbol *sema_resolve(Sema *s, StringView name) {
  for (SemaScope *scope = s->scope; scope != NULL; scope = scope->parent) {
    for (size_t i = 0; i < scope->symbols.len; i++) {
      Symbol *sym = scope->symbols.items[i];
      if (sv_eq(sym->name, name)) {
        return sym;
      }
    }
  }
  return NULL;
}

Symbol *sema_resolve_local(Sema *s, StringView name) {
  if (!s->scope)
    return NULL;

  for (size_t i = 0; i < s->scope->symbols.len; i++) {
    Symbol *sym = s->scope->symbols.items[i];
    if (sv_eq(sym->name, name)) {
      return sym;
    }
  }
  return NULL;
}

Symbol *sema_define(Sema *s, StringView name, Type *type, bool is_const,
                    Location loc) {
  for (size_t i = 0; i < s->scope->symbols.len; i++) {
    Symbol *sym = s->scope->symbols.items[i];
    if (sv_eq(sym->name, name)) {
      ErrorHandler_report(s->eh, loc, "Redefinition of symbol in this scope");
      return NULL;
    }
  }

  Symbol *sym = xmalloc(sizeof(Symbol));
  sym->name = name;
  sym->original_name = name;
  sym->type = type;
  sym->is_const = is_const;
  sym->is_export = false;
  sym->is_external = false;
  sym->is_moved = 0;
  sym->requires_storage = 0;
  sym->requires_arena = 0;
  sym->value = NULL;
  sym->func_status = FUNC_NONE;
  sym->kind = SYM_VAR; // Default to SYM_VAR
  sym->builtin_kind = BUILTIN_NONE;
  sym->scope = s->scope;
  sym->module = NULL;

  List_push(&s->scope->symbols, sym);
  return sym;
}

Symbol *sema_define_builtin(Sema *s, StringView name, Type *type,
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

//  ----- External -----

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
    if (node->tag == NODE_FUNC_DECL)
      continue;
    sema_check_stmt(s, node);
  }

  s->pass = SEMA_PASS_BODIES;
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    sema_check_function_bodies(s, root->ast_root.children.items[i]);
  }

  s->pass = SEMA_PASS_REGISTRATION;
}
