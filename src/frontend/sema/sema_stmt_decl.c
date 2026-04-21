#include "sema_internal.h"

void sema_check_import(Sema *s, AstNode *node) {
  Module *module = module_resolve_or_load(
      s, node->import.path,
      s->current_module ? s->current_module->abs_path : ".");
  if (!module)
    return;

  StringView name = node->import.alias;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate import alias '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Symbol *sym = sema_define(s, name, type_get_primitive(s->types, PRIM_UNKNOWN),
                            false, node->loc);
  if (sym) {
    sym->kind = SYM_MODULE;
    sym->module = module;
    sym->is_export = false;
    sym->is_external = false;
    sym->value = NULL;
  }

  for (size_t i = 0; i < module->exports.len; i++) {
    Symbol *export = module->exports.items[i];
    StringView export_name =
        export->original_name.len ? export->original_name : export->name;
    if (sema_resolve_local(s, export_name)) {
      sema_error(s, node, "Duplicate imported symbol '" SV_FMT "'",
                 SV_ARG(export_name));
      continue;
    }

    Symbol *export_sym =
        sema_define(s, export_name, export->type, export->is_const, node->loc);
    if (!export_sym)
      continue;

    export_sym->original_name = export->original_name;
    export_sym->value = export->value;
    export_sym->kind = export->kind;
    export_sym->func_status = export->func_status;
    export_sym->is_external = export->is_external;
    export_sym->is_export = false;
    export_sym->module = module;
  }
}

void sema_check_var_decl(Sema *s, AstNode *node) {
  StringView name = node->var_decl.name->var.value;
  if (sema_resolve_local(s, name)) {
    sema_error(s, node, "Duplicate variable '" SV_FMT "'", SV_ARG(name));
    return;
  }

  Type *decl_type = node->var_decl.declared_type;

  if (node->var_decl.value) {
    Type *actual_type = sema_coerce(s, node->var_decl.value, decl_type);

    if (type_needs_inference(decl_type)) {
      decl_type = actual_type;
    } else if (!type_is_concrete(decl_type) ||
               (decl_type->kind == KIND_STRUCT &&
                decl_type->data.instance.from_template == NULL)) {
      decl_type = actual_type;
    }

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

  Symbol *sym =
      sema_define(s, name, decl_type, node->var_decl.is_const, node->loc);
  if (sym) {
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

  sema_scope_pop(s);

  s->ret_type = old_ret;
  s->fn_node = old_fn;
}

void sema_check_block(Sema *s, AstNode *node) {
  sema_scope_push(s);
  for (size_t i = 0; i < node->block.statements.len; i++) {
    sema_check_stmt(s, node->block.statements.items[i]);
  }
  sema_scope_pop(s);
}

void sema_check_return_stmt(Sema *s, AstNode *node) {
  if (!s->fn_node) {
    sema_error(s, node, "Return statement outside of function");
    return;
  }

  Type *expr_type = node->return_stmt.expr
                        ? sema_check_expr(s, node->return_stmt.expr).type
                        : type_get_primitive(s->types, PRIM_VOID);

  if (type_is_unknown(s->ret_type)) {
    s->ret_type = expr_type;
    s->fn_node->func_decl.return_type = expr_type;
  } else if (s->ret_type->kind == KIND_RESULT &&
             s->ret_type->data.result.error_set &&
             s->ret_type->data.result.error_set->kind == KIND_ERROR_SET &&
             s->ret_type->data.result.error_set->name.len == 0) {
    if (expr_type->kind == KIND_ERROR) {
      Type *error_set = s->ret_type->data.result.error_set;
      bool found = false;
      for (size_t i = 0; i < error_set->members.len; i++) {
        Member *m = error_set->members.items[i];
        if (type_equals(m->type, expr_type)) {
          found = true;
          break;
        }
      }
      if (!found) {
        type_add_member(error_set, NULL, expr_type, 0);
      }
    } else if (expr_type->kind == KIND_RESULT &&
               expr_type->data.result.error_set &&
               expr_type->data.result.error_set->kind == KIND_ERROR_SET) {
      Type *error_set = s->ret_type->data.result.error_set;
      for (size_t i = 0; i < expr_type->data.result.error_set->members.len;
           i++) {
        Member *member = expr_type->data.result.error_set->members.items[i];
        Type *error_ty = member->type;
        bool found = false;
        for (size_t j = 0; j < error_set->members.len; j++) {
          Member *m = error_set->members.items[j];
          if (type_equals(m->type, error_ty)) {
            found = true;
            break;
          }
        }
        if (!found) {
          type_add_member(error_set, NULL, error_ty, 0);
        }
      }
    }

    if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
      sema_error(s, node, "Type mismatch: function returns '%s' but got '%s'",
                 type_to_name(s->ret_type), type_to_name(expr_type));
    }
  } else if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
    sema_error(s, node, "Type mismatch: function returns '%s' but got '%s'",
               type_to_name(s->ret_type), type_to_name(expr_type));
  }
}
