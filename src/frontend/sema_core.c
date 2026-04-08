#include "sema_internal.h"
#include "tyl/semantic.h"

static void sema_prime_types(Sema *s) {
  // 1. Create the Template Type
  Type *array_tpl = xcalloc(1, sizeof(Type));
  array_tpl->kind = KIND_TEMPLATE;
  array_tpl->name = sv_from_parts("Array", 5);
  List_init(&array_tpl->members);
  List_init(&array_tpl->data.template.placeholders);
  List_init(&array_tpl->data.template.fields);

  // 2. Add the placeholder "T"
  StringView *t_placeholder = xmalloc(sizeof(StringView));
  *t_placeholder = sv_from_parts("T", 1);
  List_push(&array_tpl->data.template.placeholders, t_placeholder);

  // 3. Define the members (using PRIM_UNKNOWN/name "T" for the placeholder)
  Type *t_type = xcalloc(1, sizeof(Type));
  t_type->kind = KIND_PRIMITIVE;
  t_type->data.primitive = PRIM_UNKNOWN;
  t_type->name = *t_placeholder;

  // data: ptr<T>
  type_add_member(array_tpl, "data", type_get_pointer(s->types, t_type), 0);
  // len: i64
  type_add_member(array_tpl, "len", type_get_primitive(s->types, PRIM_I64), 8);
  // cap: i64
  type_add_member(array_tpl, "cap", type_get_primitive(s->types, PRIM_I64), 16);
  // rank: i64
  type_add_member(array_tpl, "rank", type_get_primitive(s->types, PRIM_I64), 24);
  // dims: ptr<i64>
  type_add_member(array_tpl, "dims",
                  type_get_pointer(s->types, type_get_primitive(s->types, PRIM_I64)),
                  32);

  array_tpl->size = 40; // ptr + i64 + i64 + i64 + ptr

  // 4. Register it in the context so check_array_expr can find it
  List_push(&s->types->templates, array_tpl);
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
  sym->type = type;
  sym->is_const = is_const;
  sym->func_status = FUNC_NONE;
  sym->scope = s->scope;

  List_push(&s->scope->symbols, sym);
  return sym;
}

//  ----- External -----

void sema_init(Sema *s, ErrorHandler *eh, TypeContext *type_ctx) {
  s->types = type_ctx;
  s->eh = eh;

  s->ret_type = NULL;
  s->fn_node = NULL;
  s->jump = NULL;
  s->scope = NULL;

  sema_scope_push(s);
  sema_prime_types(s);
}

void sema_finish(Sema *s) {
  while (s->scope) {
    sema_scope_pop(s);
  }
}

void sema_analyze(Sema *s, AstNode *root) {
  if (!root || root->tag != NODE_AST_ROOT)
    return;

  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      sema_register_func_signature(s, node);
    }
  }

  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    sema_check_stmt(s, root->ast_root.children.items[i]);
  }
}
