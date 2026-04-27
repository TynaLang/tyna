#include "sema_internal.h"
#include <ctype.h>

static void sema_try_bind_static_call_from_decl_type(Sema *s, AstNode *value,
                                                     Type *decl_type) {
  if (!value || !decl_type || value->tag != NODE_CALL || !value->call.func)
    return;
  if (value->call.func->tag != NODE_STATIC_MEMBER)
    return;
  if (decl_type->kind != KIND_STRUCT || !decl_type->data.instance.from_template)
    return;

  AstNode *sm = value->call.func;
  Type *template_type = decl_type->data.instance.from_template;
  if (!sm->static_member.parent.data || !template_type->name.data)
    return;
  if (!sv_eq(sm->static_member.parent, template_type->name))
    return;

  for (size_t i = 0; i < template_type->methods.len; i++) {
    Symbol *method = template_type->methods.items[i];
    if (!method || method->kind != SYM_STATIC_METHOD)
      continue;

    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (!sm->static_member.member.data ||
        !sv_eq(lookup_name, sm->static_member.member)) {
      continue;
    }

    Symbol *concrete = sema_instantiate_method_symbol(s, decl_type, method, sm);
    if (!concrete)
      return;

    if (!sema_resolve(s, concrete->name)) {
      Symbol *alias =
          sema_define(s, concrete->name, concrete->type, true, sm->loc);
      if (alias) {
        alias->original_name = concrete->original_name;
        alias->value = concrete->value;
        alias->kind = concrete->kind;
        alias->is_export = false;
      }
    }

    value->call.func = AstNode_new_var(concrete->name, sm->loc);
    return;
  }
}

static bool sema_error_message_validate(Sema *s, AstNode *node, StringView msg,
                                        size_t field_count) {
  bool valid = true;
  for (size_t i = 0; i + 1 < msg.len; i++) {
    if (msg.data[i] != '$')
      continue;

    size_t j = i + 1;
    if (j >= msg.len || !isdigit((unsigned char)msg.data[j])) {
      sema_error(s, node,
                 "Invalid placeholder in @message: expected '$<number>'");
      valid = false;
      continue;
    }

    int idx = 0;
    while (j < msg.len && isdigit((unsigned char)msg.data[j])) {
      idx = idx * 10 + (msg.data[j] - '0');
      j++;
    }

    if ((size_t)idx >= field_count) {
      sema_error(s, node,
                 "@message placeholder '$%d' is out of bounds for error "
                 "payload with %zu field(s)",
                 idx, field_count);
      valid = false;
    }
    i = j - 1;
  }
  return valid;
}

void sema_check_var_decl(Sema *s, AstNode *node) {
  StringView name = node->var_decl.name->var.value;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate variable '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Type *decl_type = node->var_decl.declared_type;

  // First, check the RHS to infer type if needed
  Type *actual_type = type_get_primitive(s->types, PRIM_UNKNOWN);
  if (node->var_decl.value) {
    // Check the expression first to get its type
    ExprInfo rhs_info = sema_check_expr(s, node->var_decl.value);
    actual_type = rhs_info.type;

    // Handle result type unwrapping for let bindings with ? operator
    if (actual_type && actual_type->kind == KIND_RESULT &&
        actual_type->data.result.success) {
      actual_type = actual_type->data.result.success;
    }
  }

  // If no explicit type, use inferred type
  if (type_needs_inference(decl_type) && actual_type &&
      type_is_concrete(actual_type)) {
    decl_type = actual_type;
  }

  Symbol *sym =
      sema_define(s, name, decl_type, node->var_decl.is_const, node->loc);
  if (sym) {
    sym->value = node->var_decl.value;
    sym->is_export = node->var_decl.is_export;
  }

  if (node->var_decl.value) {
    sema_try_bind_static_call_from_decl_type(s, node->var_decl.value,
                                             decl_type);
    node->var_decl.value = sema_coerce(s, node->var_decl.value, decl_type);

    if (decl_type && decl_type->kind == KIND_STRING_BUFFER) {
      AstNode *rhs_var = NULL;
      if (node->var_decl.value->tag == NODE_VAR) {
        rhs_var = node->var_decl.value;
      } else if (node->var_decl.value->tag == NODE_UNARY &&
                 node->var_decl.value->unary.op == OP_ADDR_OF) {
        AstNode *inner = node->var_decl.value->unary.expr;
        if (inner && inner->tag == NODE_VAR)
          rhs_var = inner;
      }
      if (rhs_var && !sv_eq(name, rhs_var->var.value)) {
        Symbol *sym = sema_resolve(s, rhs_var->var.value);
        if (sym && sym->kind == SYM_VAR && !sym->is_const) {
          sym->is_moved = 1;
        }
      }
    }
  } else {
    if (!type_is_concrete(decl_type) && !(sema_allows_polymorphic_types(s) &&
                                          type_can_be_polymorphic(decl_type))) {
      sema_error(
          s, node,
          "Type annotation required for uninitialized variable (got '%s')",
          type_to_name(decl_type));
      return;
    }
  }

  node->var_decl.declared_type = decl_type;
  node->resolved_type = decl_type;
  if (sym) {
    sym->type = decl_type;
    sym->value = node->var_decl.value;
    sym->is_export = node->var_decl.is_export;
  }
}

void sema_check_error_decl(Sema *s, AstNode *node) {
  StringView name = node->error_decl.name->var.value;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate symbol '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Type *t = type_get_error(s->types, name);
  Symbol *sym = sema_define(s, name, t, true, node->loc);
  if (sym) {
    sym->kind = SYM_ERROR;
    sym->is_export = node->error_decl.is_export;
  }

  size_t offset = 0;
  size_t max_align = 1;

  if (node->error_decl.message.len > 0) {
    sema_error_message_validate(s, node, node->error_decl.message,
                                node->error_decl.members.len);
    t->error_message_template = node->error_decl.message;
  }

  {
    StringView error_name = name;
    const char *prefix = "__tyna_error_print_";
    size_t prefix_len = strlen(prefix);
    size_t name_len = error_name.len;
    char *internal_name = xmalloc(prefix_len + name_len + 1);
    memcpy(internal_name, prefix, prefix_len);
    memcpy(internal_name + prefix_len, error_name.data, name_len);
    internal_name[prefix_len + name_len] = '\0';

    StringView method_name = sv_from_cstr(internal_name);
    AstNode *method_name_node = AstNode_new_var(method_name, node->loc);
    Type *self_ptr_type = type_get_pointer(s->types, t);
    AstNode *self_param =
        AstNode_new_param(AstNode_new_var(sv_from_cstr("self"), node->loc),
                          self_ptr_type, node->loc);
    List params;
    List_init(&params);
    List_push(&params, self_param);

    AstNode *fn_decl = AstNode_new_func_decl(
        method_name_node, params, type_get_primitive(s->types, PRIM_VOID), NULL,
        false, false, false, false, node->loc);

    Symbol *method_sym = xcalloc(1, sizeof(Symbol));
    method_sym->name = method_name;
    method_sym->original_name = sv_from_cstr("print");
    method_sym->type = type_get_primitive(s->types, PRIM_VOID);
    method_sym->kind = SYM_METHOD;
    method_sym->value = fn_decl;
    method_sym->is_const = false;
    method_sym->is_export = false;
    method_sym->is_external = false;
    method_sym->is_moved = false;
    method_sym->requires_storage = false;
    method_sym->requires_arena = false;
    method_sym->scope = NULL;
    method_sym->func_status = FUNC_NONE;
    method_sym->builtin_kind = BUILTIN_NONE;
    method_sym->module = NULL;
    List_push(&t->methods, method_sym);
  }

  for (size_t i = 0; i < node->error_decl.members.len; i++) {
    AstNode *mem = node->error_decl.members.items[i];
    if (mem->tag != NODE_VAR_DECL) {
      sema_error(s, mem, "Invalid payload member in error declaration");
      continue;
    }

    Type *mem_type = mem->var_decl.declared_type;
    if (!type_is_concrete(mem_type)) {
      sema_error(s, mem, "Error payload field '%.*s' has non-concrete type",
                 (int)mem->var_decl.name->var.value.len,
                 mem->var_decl.name->var.value.data);
      continue;
    }

    if (mem_type->kind == KIND_PRIMITIVE &&
        mem_type->data.primitive == PRIM_STRING) {
      sema_error(s, mem,
                 "Heap-allocated types are forbidden in error payloads. Use "
                 "*const char.");
    }

    if (mem_type->kind == KIND_STRING_BUFFER) {
      sema_error(s, mem,
                 "String buffers are forbidden in error payloads. Use "
                 "*const char.");
    }

    if (mem_type->kind == KIND_POINTER) {
      if (!(mem_type->data.pointer_to->kind == KIND_PRIMITIVE &&
            mem_type->data.pointer_to->data.primitive == PRIM_CHAR)) {
        sema_error(s, mem,
                   "Error payload pointers must target 'char', got '%s'",
                   type_to_name(mem_type->data.pointer_to));
      }
    } else if (mem_type->kind != KIND_PRIMITIVE) {
      sema_error(
          s, mem,
          "Error payload fields must be primitive or '*const char', got '%s'",
          type_to_name(mem_type));
    }

    char *c_mem_name = sv_to_cstr(mem->var_decl.name->var.value);
    size_t align = mem_type->alignment ? mem_type->alignment : mem_type->size;
    if (align == 0)
      align = 1;

    offset = align_to(offset, align);
    type_add_member(t, c_mem_name, mem_type, offset);
    free(c_mem_name);

    offset += mem_type->size;
    if (align > max_align)
      max_align = align;
  }

  t->alignment = max_align;
  t->size = align_to(offset, max_align);
  t->is_frozen = true;

  if (t->size > 48) {
    sema_error(s, node, "Error payload exceeds 48 byte stack limit.");
  }
}

void sema_check_error_set_decl(Sema *s, AstNode *node) {
  StringView name = node->error_set_decl.name->var.value;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate symbol '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Type *set = type_get_error_set(s->types, name);
  Symbol *sym = sema_define(s, name, set, true, node->loc);
  if (sym) {
    sym->is_export = node->error_set_decl.is_export;
  }

  for (size_t i = 0; i < node->error_set_decl.members.len; i++) {
    AstNode *member = node->error_set_decl.members.items[i];
    if (member->tag != NODE_VAR) {
      sema_error(s, member, "Error set entries must be error type names");
      continue;
    }

    Type *error_type = sema_find_type_by_name(s, member->var.value);
    if (!error_type || error_type->kind != KIND_ERROR) {
      sema_error(s, member, "Unknown error type '%.*s' in error set",
                 (int)member->var.value.len, member->var.value.data);
      continue;
    }

    type_add_member(set, NULL, error_type, 0);
  }
}

void sema_check_func_decl(Sema *s, AstNode *node) {
  if (!node->func_decl.body) {
    return;
  }

  Type *old_ret = s->ret_type;
  AstNode *old_fn = s->fn_node;

  s->ret_type = node->func_decl.return_type;
  s->fn_node = node;

  sema_scope_push(s);

  for (size_t i = 0; i < node->func_decl.params.len; i++) {
    AstNode *param = node->func_decl.params.items[i];
    bool concrete_ok = type_is_concrete(param->param.type);
    bool polymorphic_ok = sema_allows_polymorphic_types(s) &&
                          type_can_be_polymorphic(param->param.type);

    if (!concrete_ok && !polymorphic_ok) {
      sema_error(s, param,
                 "Function parameter type must be concrete (got '%s')",
                 type_to_name(param->param.type));
    }

    Symbol *param_sym = sema_define(s, param->param.name->var.value,
                                    param->param.type, false, param->loc);
    if (param_sym)
      param_sym->value = param;
  }

  sema_check_stmt(s, node->func_decl.body);

  if (type_is_unknown(s->ret_type)) {
    s->ret_type = type_get_primitive(s->types, PRIM_VOID);
    node->func_decl.return_type = s->ret_type;
  }

  bool body_has_computed_str = s->scope ? s->scope->has_computed_str : false;
  sema_scope_pop(s);
  node->func_decl.requires_arena =
      body_has_computed_str && sema_fn_decl_can_use_arena(node);

  s->ret_type = old_ret;
  s->fn_node = old_fn;
}
