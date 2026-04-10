#include "sema_internal.h"

void sema_scope_push(Sema *s) {
  SemaScope *new_scope = xmalloc(sizeof(SemaScope));
  new_scope->parent = s->scope;
  List_init(&new_scope->symbols);
  s->scope = new_scope;
}

void sema_scope_pop(Sema *s) {
  if (s->scope == NULL)
    return;

  SemaScope *old = s->scope;
  s->scope = old->parent;

  for (size_t i = 0; i < old->symbols.len; i++) {
    free(old->symbols.items[i]);
  }
  List_free(&old->symbols, 0);
  free(old);
}

static void sema_jump_push(Sema *s, SemaJump *ctx, StringView label,
                           AstNode *node, bool is_loop) {
  ctx->label = label;
  ctx->node = node;
  ctx->is_loop = is_loop;
  ctx->parent = s->jump;
  s->jump = ctx;
}

static void sema_jump_pop(Sema *s) {
  if (s->jump) {
    s->jump = s->jump->parent;
  }
}

static SemaJump *sema_find_loop_jump(Sema *s) {
  for (SemaJump *j = s->jump; j != NULL; j = j->parent) {
    if (j->is_loop) {
      return j;
    }
  }
  return NULL;
}

static bool type_needs_inference(Type *t) {
  if (!t)
    return true;

  if (t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_UNKNOWN)
    return true;

  if (t->kind == KIND_TEMPLATE)
    return true;

  if (t->kind == KIND_STRUCT && t->data.instance.from_template != NULL) {
    return false;
  }

  return false;
}

static bool type_is_concrete(Type *t) {
  if (!t)
    return false;

  switch (t->kind) {
  case KIND_PRIMITIVE:
    return t->data.primitive != PRIM_UNKNOWN;

  case KIND_POINTER:
    return type_is_concrete(t->data.pointer_to);

  case KIND_STRUCT:
    if (t->data.instance.from_template == NULL)
      return t->members.len > 0;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      Type *arg = t->data.instance.generic_args.items[i];
      if (!type_is_concrete(arg))
        return false;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (!type_is_concrete(m->type))
        return false;
    }
    return true;

  case KIND_TEMPLATE:
    return false;

  default:
    return false;
  }
}

void sema_check_stmt(Sema *s, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {

  case NODE_BLOCK: {
    sema_scope_push(s);
    for (size_t i = 0; i < node->block.statements.len; i++) {
      sema_check_stmt(s, node->block.statements.items[i]);
    }
    sema_scope_pop(s);
    break;
  }

  case NODE_VAR_DECL: {
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
    } else {
      if (!type_is_concrete(decl_type)) {
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
    }
    break;
  }

  case NODE_FUNC_DECL: {
    if (!node->func_decl.body) {
      // Forward declaration - just register signature (already done in
      // sema_analyze)
      return;
    }

    Type *old_ret = s->ret_type;
    AstNode *old_fn = s->fn_node;

    s->ret_type = node->func_decl.return_type;
    s->fn_node = node;

    sema_scope_push(s);

    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      AstNode *param = node->func_decl.params.items[i];

      if (!type_is_concrete(param->param.type)) {
        sema_error(s, param,
                   "Function parameter type must be concrete (got '%s')",
                   type_to_name(param->param.type));
      }

      sema_define(s, param->param.name->var.value, param->param.type, false,
                  param->loc);
    }

    sema_check_stmt(s, node->func_decl.body);

    if (type_is_unknown(s->ret_type)) {
      s->ret_type = type_get_primitive(s->types, PRIM_VOID);
      node->func_decl.return_type = s->ret_type;
    }

    sema_scope_pop(s);

    s->ret_type = old_ret;
    s->fn_node = old_fn;
    break;
  }

  case NODE_RETURN_STMT: {
    if (!s->fn_node) {
      sema_error(s, node, "Return statement outside of function");
      return;
    }

    Type *expr_type = node->return_stmt.expr
                          ? sema_check_expr(s, node->return_stmt.expr)
                          : type_get_primitive(s->types, PRIM_VOID);

    if (type_is_unknown(s->ret_type)) {
      s->ret_type = expr_type;
      s->fn_node->func_decl.return_type = expr_type;
    } else {
      if (!type_can_implicitly_cast(s->ret_type, expr_type)) {
        sema_error(s, node, "Type mismatch: function returns '%s' but got '%s'",
                   type_to_name(s->ret_type), type_to_name(expr_type));
      }
    }
    break;
  }

  case NODE_IF_STMT: {
    Type *cond = sema_check_expr(s, node->if_stmt.condition);

    if (!type_is_bool(cond)) {
      sema_error(s, node->if_stmt.condition,
                 "if condition must be bool, got '%s'", type_to_name(cond));
    }

    sema_check_stmt(s, node->if_stmt.then_branch);

    if (node->if_stmt.else_branch) {
      sema_check_stmt(s, node->if_stmt.else_branch);
    }
    break;
  }

  case NODE_LOOP_STMT: {
    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);
    sema_check_stmt(s, node->loop.expr);
    sema_jump_pop(s);
    break;
  }

  case NODE_WHILE_STMT: {
    Type *cond = sema_check_expr(s, node->while_stmt.condition);

    if (!type_is_bool(cond)) {
      sema_error(s, node->while_stmt.condition,
                 "while condition must be bool, got '%s'", type_to_name(cond));
    }

    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);
    sema_check_stmt(s, node->while_stmt.body);
    sema_jump_pop(s);
    break;
  }

  case NODE_FOR_STMT: {
    sema_scope_push(s);

    if (node->for_stmt.init) {
      sema_check_stmt(s, node->for_stmt.init);
    }

    if (node->for_stmt.condition) {
      Type *cond = sema_check_expr(s, node->for_stmt.condition);
      if (!type_is_bool(cond)) {
        sema_error(s, node->for_stmt.condition,
                   "for condition must be bool, got '%s'", type_to_name(cond));
      }
    }

    if (node->for_stmt.increment) {
      sema_check_expr(s, node->for_stmt.increment);
    }

    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);
    sema_check_stmt(s, node->for_stmt.body);
    sema_jump_pop(s);

    sema_scope_pop(s);
    break;
  }

  case NODE_FOR_IN_STMT: {
    Type *iter_type = sema_check_expr(s, node->for_in_stmt.iterable);

    // Determine element type based on iterable type
    Type *elem_type = NULL;
    bool is_valid_iterable = false;

    if (iter_type->kind == KIND_PRIMITIVE &&
        iter_type->data.primitive == PRIM_STRING) {
      elem_type = type_get_primitive(s->types, PRIM_CHAR);
      is_valid_iterable = true;
    } else if (iter_type->kind == KIND_STRUCT &&
               iter_type->data.instance.from_template &&
               sv_eq(iter_type->data.instance.from_template->name,
                     sv_from_parts("Array", 5))) {
      if (iter_type->data.instance.generic_args.len > 0) {
        elem_type = iter_type->data.instance.generic_args.items[0];
        is_valid_iterable = true;
      }
    }

    if (!is_valid_iterable) {
      sema_error(s, node->for_in_stmt.iterable, "Cannot iterate over type '%s'",
                 type_to_name(iter_type));
      elem_type = type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    if (!elem_type) {
      elem_type = type_get_primitive(s->types, PRIM_UNKNOWN);
    }

    sema_scope_push(s);

    Symbol *sym = sema_define(s, node->for_in_stmt.var->var.value, elem_type,
                              true, node->for_in_stmt.var->loc);
    if (sym) {
      node->for_in_stmt.var->resolved_type = elem_type;
    }

    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true);
    sema_check_stmt(s, node->for_in_stmt.body);
    sema_jump_pop(s);

    sema_scope_pop(s);
    break;
  }

  case NODE_STRUCT_DECL: {
    StringView name = node->struct_decl.name->var.value;
    Symbol *existing = sema_resolve(s, name);
    Type *t = NULL;

    if (existing) {
      if (!existing->type->is_intrinsic) {
        sema_error(s, node, "Duplicate symbol '" SV_FMT "'", SV_ARG(name));
        break;
      }
      t = existing->type;
    } else {
      // Create new type (simplified; ideally handles templates vs structs)
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
          break;
        }
      }
      sema_define(s, name, t, true, node->loc);
    }

    t->is_frozen = node->struct_decl.is_frozen;

    // Separate fields and methods
    size_t offset = (existing && existing->type->is_intrinsic) ? t->size : 0;

    for (size_t i = 0; i < node->struct_decl.members.len; i++) {
      AstNode *mem = node->struct_decl.members.items[i];

      if (mem->tag == NODE_FUNC_DECL) {
        // Handle method registration
        StringView method_name = mem->func_decl.name->var.value;
        Symbol *resolved = sema_resolve(s, method_name);
        StringView original_name = method_name;
        if (resolved && resolved->original_name.len)
          original_name = resolved->original_name;

        Symbol *method_sym = xmalloc(sizeof(Symbol));
        method_sym->name = method_name;
        method_sym->original_name = original_name;
        method_sym->type = mem->func_decl.return_type; // Placeholder for now
        method_sym->kind =
            mem->func_decl.is_static ? SYM_STATIC_METHOD : SYM_METHOD;
        method_sym->value = mem;
        List_push(&t->methods, method_sym);

        if (mem->func_decl.body) {
          sema_check_stmt(s, mem);
        }
        continue;
      }

      // Handle fields
      if (t->is_intrinsic && existing && node->struct_decl.is_frozen) {
        // Technically we are "filling" an intrinsic that is already frozen
        // But if the stdlib defines it, we allow it if it matches or we are
        // initializing it. For this "Big Bang", we allow adding fields to
        // intrinsics if they are not already populated.
      }

      Type *mem_type = mem->var_decl.declared_type;
      char *c_mem_name = sv_to_cstr(mem->var_decl.name->var.value);
      size_t align = mem_type->alignment ? mem_type->alignment : mem_type->size;
      if (align == 0)
        align = 1;

      // Check for duplicate field
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
    break;
  }

  case NODE_IMPL_DECL: {
    StringView name = node->impl_decl.name->var.value;
    Symbol *existing = sema_resolve(s, name);
    if (!existing || (existing->type->kind != KIND_STRUCT &&
                      existing->type->kind != KIND_TEMPLATE)) {
      sema_error(s, node, "Undefined type '%" SV_FMT "' or it is not a struct",
                 SV_ARG(name));
      break;
    }
    Type *t = existing->type;

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
        sema_error(s, mem, "Duplicate method '%" SV_FMT "' on type %" SV_FMT "",
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
      method_sym->type = mem->func_decl.return_type; // Placeholder for now
      method_sym->kind =
          mem->func_decl.is_static ? SYM_STATIC_METHOD : SYM_METHOD;
      method_sym->value = mem;
      List_push(&t->methods, method_sym);

      // Analyze the method body now that the signature is registered.
      if (mem->func_decl.body) {
        sema_check_stmt(s, mem);
      }
    }
    break;
  }

  case NODE_PRINT_STMT: {
    for (size_t i = 0; i < node->print_stmt.values.len; i++) {
      sema_check_expr(s, node->print_stmt.values.items[i]);
    }
    break;
  }

  case NODE_EXPR_STMT: {
    sema_check_expr(s, node->expr_stmt.expr);
    break;
  }

  case NODE_DEFER: {
    sema_check_stmt(s, node->defer.expr);
    break;
  }

  case NODE_BREAK: {
    SemaJump *loop = sema_find_loop_jump(s);
    if (!loop) {
      sema_error(s, node, "break statement outside of loop");
    }
    break;
  }

  case NODE_CONTINUE: {
    SemaJump *loop = sema_find_loop_jump(s);
    if (!loop) {
      sema_error(s, node, "continue statement outside of loop");
    }
    break;
  }

  default:
    break;
  }
}