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
                           AstNode *node, bool is_loop, bool is_breakable) {
  ctx->label = label;
  ctx->node = node;
  ctx->is_loop = is_loop;
  ctx->is_breakable = is_breakable;
  ctx->parent = s->jump;
  s->jump = ctx;
}

static void sema_jump_pop(Sema *s) {
  if (s->jump) {
    s->jump = s->jump->parent;
  }
}

static SemaJump *sema_find_break_jump(Sema *s) {
  for (SemaJump *j = s->jump; j != NULL; j = j->parent) {
    if (j->is_breakable) {
      return j;
    }
  }
  return NULL;
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

static bool type_is_condition(Type *t) {
  if (!t)
    return false;

  if (type_is_bool(t))
    return true;

  if (t->kind == KIND_POINTER)
    return true;

  return false;
}

static bool type_can_be_polymorphic(Type *t) {
  if (!t)
    return false;

  switch (t->kind) {
  case KIND_TEMPLATE:
    return true;
  case KIND_POINTER:
    return type_can_be_polymorphic(t->data.pointer_to);
  case KIND_STRUCT:
  case KIND_UNION:
    if (t->data.instance.from_template == NULL)
      return false;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      if (type_can_be_polymorphic(t->data.instance.generic_args.items[i]))
        return true;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (type_can_be_polymorphic(m->type))
        return true;
    }
    return false;
  default:
    return false;
  }
}

static bool sema_allows_polymorphic_types(Sema *s) {
  return s && s->generic_context_type != NULL;
}

void sema_check_stmt(Sema *s, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {

  case NODE_IMPORT: {
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

    Symbol *sym = sema_define(
        s, name, type_get_primitive(s->types, PRIM_UNKNOWN), false, node->loc);
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

      Symbol *export_sym = sema_define(s, export_name, export->type,
                                       export->is_const, node->loc);
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
      if (!type_is_concrete(decl_type) &&
          !(sema_allows_polymorphic_types(s) &&
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
      bool concrete_ok = type_is_concrete(param->param.type);
      bool polymorphic_ok = sema_allows_polymorphic_types(s) &&
                            type_can_be_polymorphic(param->param.type);

      if (!concrete_ok && !polymorphic_ok) {
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

  case NODE_BLOCK: {
    sema_scope_push(s);
    for (size_t i = 0; i < node->block.statements.len; i++) {
      sema_check_stmt(s, node->block.statements.items[i]);
    }
    sema_scope_pop(s);
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

    if (!type_is_condition(cond)) {
      sema_error(s, node->if_stmt.condition,
                 "if condition must be bool or pointer, got '%s'",
                 type_to_name(cond));
    }

    sema_check_stmt(s, node->if_stmt.then_branch);

    if (node->if_stmt.else_branch) {
      sema_check_stmt(s, node->if_stmt.else_branch);
    }
    break;
  }

  case NODE_SWITCH_STMT: {
    Type *expr_type = sema_check_expr(s, node->switch_stmt.expr);
    if (!expr_type)
      break;

    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, false, true);

    if (expr_type->kind == KIND_PRIMITIVE &&
        expr_type->data.primitive == PRIM_STRING) {
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (!case_node || case_node->tag != NODE_CASE) {
          continue;
        }
        if (case_node->case_stmt.pattern &&
            case_node->case_stmt.pattern->tag != NODE_STRING) {
          sema_error(s, case_node,
                     "String switch cases must use string literals or '_'");
          continue;
        }
        sema_check_stmt(s, case_node->case_stmt.body);
      }
    } else if (expr_type->kind == KIND_UNION && expr_type->is_tagged_union) {
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (!case_node || case_node->tag != NODE_CASE) {
          continue;
        }
        if (!case_node->case_stmt.pattern) {
          sema_check_stmt(s, case_node->case_stmt.body);
          continue;
        }
        if (case_node->case_stmt.pattern->tag != NODE_VAR ||
            !case_node->case_stmt.pattern_type) {
          sema_error(s, case_node,
                     "Union switch cases must use 'case name: Type => ...'");
          continue;
        }

        Type *variant_type = case_node->case_stmt.pattern_type;
        bool found = false;
        for (size_t j = 0; j < expr_type->members.len; j++) {
          Member *m = expr_type->members.items[j];
          if (type_equals(m->type, variant_type)) {
            found = true;
            break;
          }
        }
        if (!found) {
          sema_error(s, case_node, "Type '%s' is not a variant of union '%s'",
                     type_to_name(variant_type), type_to_name(expr_type));
          continue;
        }

        sema_scope_push(s);
        sema_define(s, case_node->case_stmt.pattern->var.value, variant_type,
                    false, case_node->loc);
        sema_check_stmt(s, case_node->case_stmt.body);
        sema_scope_pop(s);
      }
    } else {
      sema_error(s, node,
                 "Switch expression must be a string or tagged union, got '%s'",
                 type_to_name(expr_type));
    }

    sema_jump_pop(s);
    break;
  }
  case NODE_LOOP_STMT: {
    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
    sema_check_stmt(s, node->loop.expr);
    sema_jump_pop(s);
    break;
  }

  case NODE_WHILE_STMT: {
    Type *cond = sema_check_expr(s, node->while_stmt.condition);

    if (!type_is_condition(cond)) {
      sema_error(s, node->while_stmt.condition,
                 "while condition must be bool or pointer, got '%s'",
                 type_to_name(cond));
    }

    SemaJump jump_ctx;
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
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
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
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
    sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
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
      Symbol *sym = sema_define(s, name, t, true, node->loc);
      if (sym) {
        sym->is_export = node->struct_decl.is_export;
      }
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
  case NODE_UNION_DECL: {
    StringView name = node->union_decl.name->var.value;
    Symbol *existing = sema_resolve(s, name);
    Type *t = NULL;

    if (existing) {
      if (!existing->type->is_intrinsic) {
        sema_error(s, node, "Duplicate symbol '" SV_FMT "'", SV_ARG(name));
        break;
      }
      t = existing->type;
    } else {
      if (node->union_decl.placeholders.len > 0) {
        t = xcalloc(1, sizeof(Type));
        t->kind = KIND_TEMPLATE;
        t->name = name;
        List_init(&t->members);
        List_init(&t->methods);
        List_init(&t->data.template.placeholders);
        List_init(&t->data.template.fields);

        for (size_t i = 0; i < node->union_decl.placeholders.len; i++) {
          List_push(&t->data.template.placeholders,
                    node->union_decl.placeholders.items[i]);
        }
        List_push(&s->types->templates, t);
      } else {
        t = type_get_union(s->types, name);
        if (!t) {
          sema_error(s, node, "Failed to create union type");
          break;
        }
      }
      Symbol *sym = sema_define(s, name, t, true, node->loc);
      if (sym) {
        sym->is_export = node->union_decl.is_export;
      }
    }

    t->is_frozen = node->union_decl.is_frozen;

    size_t max_size = 0;
    size_t max_align = 1;

    for (size_t i = 0; i < node->union_decl.members.len; i++) {
      AstNode *mem = node->union_decl.members.items[i];

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

        if (mem->func_decl.body) {
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

      type_add_member(t, c_mem_name, mem_type, 0);
      free(c_mem_name);
      if (mem_type->size > max_size)
        max_size = mem_type->size;
      if (align > max_align)
        max_align = align;
    }

    if (!existing || !existing->type->is_intrinsic) {
      t->alignment = max_align;
      t->size = align_to(max_size, max_align);
    }
    break;
  }
  case NODE_IMPL_DECL: {
    Type *t = node->impl_decl.type;
    StringView name = sv_from_cstr(type_to_name(t));
    if (!t ||
        (t->kind != KIND_STRUCT && t->kind != KIND_UNION &&
         t->kind != KIND_TEMPLATE &&
         !(t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_STRING))) {
      sema_error(s, node,
                 "Undefined type '" SV_FMT "' or it is not a struct/union",
                 SV_ARG(name));
      break;
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

      if (t->data.instance.from_template) {
        Type *template_type = t->data.instance.from_template;
        Symbol *template_method_sym = xmalloc(sizeof(Symbol));
        *template_method_sym = *method_sym;
        List_push(&template_type->methods, template_method_sym);
      }

      // Analyze the method body now that the signature is registered.
      if (mem->func_decl.body) {
        AstNode *method_copy = Ast_clone(mem);
        sema_check_stmt(s, method_copy);
        Ast_free(method_copy);
      }
    }
    s->generic_context_type = NULL;
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
    SemaJump *jump = sema_find_break_jump(s);
    if (!jump) {
      sema_error(s, node, "break statement outside of loop or switch");
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