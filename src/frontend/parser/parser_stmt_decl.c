#include "parser_internal.h"

static void parser_sync_param(Parser *p) {
  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF)
    parser_token_advance(p);
}

static void skip_to_next_param(Parser *p) {
  parser_sync_param(p);
  if (p->current_token.type == TOKEN_COMMA)
    parser_token_advance(p);
}

AstNode *parser_parse_var_decl(Parser *p, bool is_export) {
  int is_const = (p->current_token.type == TOKEN_CONST);
  parser_token_advance(p);

  Token ident = p->current_token;
  if (!parser_expect(p, TOKEN_IDENT,
                     "Expected identifier after 'let' or 'const'"))
    return NULL;

  AstNode *name = AstNode_new_var(ident.text, ident.loc);

  Type *declared_type = NULL;
  if (p->current_token.type == TOKEN_COLON) {
    parser_token_advance(p);
    declared_type = parser_parse_type_full(p);
    if (!declared_type)
      return NULL;
  } else {
    declared_type = type_get_primitive(p->type_ctx, PRIM_UNKNOWN);
  }

  if (p->current_token.type == TOKEN_ASSIGN) {
    parser_token_advance(p);
    AstNode *value = parser_parse_expression(p, 0);
    if (!value)
      return NULL;
    if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after value"))
      return NULL;

    return AstNode_new_var_decl(name, value, declared_type, is_const, is_export,
                                ident.loc);
  } else {
    if (declared_type->kind == KIND_PRIMITIVE &&
        declared_type->data.primitive == PRIM_UNKNOWN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Type required for uninitialized variable");
      return NULL;
    }

    if (!parser_expect(p, TOKEN_SEMI,
                       "Expected ';' or '=' after identifier/type"))
      return NULL;

    return AstNode_new_var_decl(name, NULL, declared_type, is_const, is_export,
                                ident.loc);
  }
}

AstNode *parser_parse_fn_decl(Parser *p, bool is_static, bool is_export,
                              bool is_external) {
  parser_token_advance(p); // consume 'fn'

  Token ident = p->current_token;
  Location loc = ident.loc;

  if (p->current_token.type != TOKEN_IDENT &&
      p->current_token.type != TOKEN_NEW) {
    ErrorHandler_report(p->eh, p->current_token.loc, "Expected function name");
    return NULL;
  }
  parser_token_advance(p);

  if (!parser_expect(p, TOKEN_LPAREN, "Expected '(' after function name"))
    return NULL;

  List params;
  List_init(&params);

  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF) {
    Location p_loc = p->current_token.loc;

    StringView param_name = p->current_token.text;
    AstNode *param_name_node = AstNode_new_var(param_name, p_loc);
    if (!parser_expect(p, TOKEN_IDENT, "Expected parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    if (!parser_expect(p, TOKEN_COLON, "Expected ':' after parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    Type *param_type = parser_parse_type_full(p);
    if (!param_type) {
      skip_to_next_param(p);
      continue;
    }

    AstNode *default_value = NULL;
    if (p->current_token.type == TOKEN_ASSIGN) {
      parser_token_advance(p);
      default_value = parser_parse_expression(p, 0);
      if (!default_value)
        return NULL;
    }

    AstNode *param_node = AstNode_new_param(param_name_node, param_type, p_loc);
    param_node->param.default_value = default_value;
    List_push(&params, param_node);
    if (p->current_token.type == TOKEN_COMMA)
      parser_token_advance(p);
  }

  if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after parameters"))
    return NULL;

  Type *ret_type = type_get_primitive(p->type_ctx, PRIM_VOID);
  if (p->current_token.type == TOKEN_COLON) {
    parser_token_advance(p);
    ret_type = parser_parse_type_full(p);
    if (!ret_type) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected return type after ':'");
      return NULL;
    }
  }

  AstNode *body = NULL;
  if (p->current_token.type == TOKEN_LBRACE) {
    body = parser_parse_block(p);
    if (!body)
      return NULL;
  } else if (p->current_token.type == TOKEN_SEMI) {
    parser_token_advance(p);
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected '{' or ';' after function declaration");
    parser_sync(p);
  }

  return AstNode_new_func_decl(AstNode_new_var(ident.text, ident.loc), params,
                               ret_type, body, is_static, is_export,
                               is_external, loc);
}

AstNode *parser_parse_struct_decl(Parser *p, bool is_frozen, bool is_export) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'struct'

  Token name_token = p->current_token;
  if (!parser_expect(p, TOKEN_IDENT, "Expected struct name"))
    return NULL;
  AstNode *name_node = AstNode_new_var(name_token.text, name_token.loc);

  List placeholders;
  List_init(&placeholders);
  if (p->current_token.type == TOKEN_LT) {
    parser_token_advance(p);
    while (p->current_token.type != TOKEN_GT &&
           p->current_token.type != TOKEN_EOF) {
      if (p->current_token.type != TOKEN_IDENT) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Expected placeholder name");
        break;
      }
      StringView *pl = xmalloc(sizeof(StringView));
      *pl = p->current_token.text;
      List_push(&placeholders, pl);
      parser_token_advance(p);
      if (p->current_token.type == TOKEN_COMMA)
        parser_token_advance(p);
    }
    if (!parser_expect(p, TOKEN_GT, "Expected '>' after placeholders")) {
      List_free(&placeholders, 1);
      return NULL;
    }
  }

  if (placeholders.len > 0) {
    parser_placeholder_scope_push(p);
    for (size_t i = 0; i < placeholders.len; i++) {
      StringView placeholder_name = *(StringView *)placeholders.items[i];
      parser_add_placeholder(p, placeholder_name);
    }
  }

  if (!parser_expect(p, TOKEN_LBRACE, "Expected '{' after struct name"))
    goto struct_decl_error;

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    bool is_static = false;
    if (p->current_token.type == TOKEN_STATIC) {
      is_static = true;
      parser_token_advance(p);
    }

    if (p->current_token.type == TOKEN_FN) {
      AstNode *fn = parser_parse_fn_decl(p, is_static, false, false);
      if (fn) {
        List_push(&members, fn);
      } else {
        parser_sync(p);
      }
      continue;
    }

    Location mem_loc = p->current_token.loc;
    Token mem_ident = p->current_token;
    if (!parser_expect(p, TOKEN_IDENT, "Expected member name")) {
      parser_sync(p);
      continue;
    }

    if (!parser_expect(p, TOKEN_COLON, "Expected ':' after member name")) {
      parser_sync(p);
      continue;
    }

    Type *type = parser_parse_type_full(p);
    if (!type) {
      parser_sync(p);
      continue;
    }

    if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after member type")) {
      parser_sync(p);
      continue;
    }

    AstNode *mem_name = AstNode_new_var(mem_ident.text, mem_ident.loc);
    AstNode *mem_decl =
        AstNode_new_var_decl(mem_name, NULL, type, 0, false, mem_loc);
    List_push(&members, mem_decl);
  }

  if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after struct members"))
    goto struct_decl_error;

  if (placeholders.len > 0) {
    parser_placeholder_scope_pop(p);
  }

  return AstNode_new_struct_decl(name_node, members, placeholders, is_frozen,
                                 false, loc);

struct_decl_error:
  if (placeholders.len > 0) {
    parser_placeholder_scope_pop(p);
  }
  List_free(&placeholders, 1);
  return NULL;
}

AstNode *parser_parse_error_decl(Parser *p, bool is_export) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'error'

  Token name_token = p->current_token;
  if (!parser_expect(p, TOKEN_IDENT, "Expected error name"))
    return NULL;
  AstNode *name_node = AstNode_new_var(name_token.text, name_token.loc);

  List members;
  List_init(&members);

  if (p->current_token.type == TOKEN_LBRACE) {
    parser_token_advance(p);
    while (p->current_token.type != TOKEN_RBRACE &&
           p->current_token.type != TOKEN_EOF) {
      Location mem_loc = p->current_token.loc;
      Token mem_ident = p->current_token;
      if (!parser_expect(p, TOKEN_IDENT, "Expected payload field name")) {
        parser_sync(p);
        continue;
      }

      if (!parser_expect(p, TOKEN_COLON, "Expected ':' after field name")) {
        parser_sync(p);
        continue;
      }

      Type *type = parser_parse_type_full(p);
      if (!type) {
        parser_sync(p);
        continue;
      }

      if (p->current_token.type == TOKEN_SEMI ||
          p->current_token.type == TOKEN_COMMA) {
        parser_token_advance(p);
      } else if (p->current_token.type != TOKEN_RBRACE) {
        if (!parser_expect(p, TOKEN_SEMI,
                           "Expected ';' or ',' after field type")) {
          parser_sync(p);
          continue;
        }
      }

      AstNode *mem_name = AstNode_new_var(mem_ident.text, mem_ident.loc);
      AstNode *mem_decl =
          AstNode_new_var_decl(mem_name, NULL, type, 0, false, mem_loc);
      List_push(&members, mem_decl);
    }

    if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after error payload")) {
      parser_sync(p);
      return NULL;
    }
  } else {
    if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after error declaration"))
      return NULL;
  }

  return AstNode_new_error_decl(name_node, members, is_export, loc);
}

AstNode *parser_parse_union_decl(Parser *p, bool is_frozen, bool is_export) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'union'

  Token name_token = p->current_token;
  if (!parser_expect(p, TOKEN_IDENT, "Expected union name"))
    return NULL;
  AstNode *name_node = AstNode_new_var(name_token.text, name_token.loc);

  List placeholders;
  List_init(&placeholders);
  if (p->current_token.type == TOKEN_LT) {
    parser_token_advance(p);
    while (p->current_token.type != TOKEN_GT &&
           p->current_token.type != TOKEN_EOF) {
      if (p->current_token.type != TOKEN_IDENT) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Expected placeholder name");
        break;
      }
      StringView *pl = xmalloc(sizeof(StringView));
      *pl = p->current_token.text;
      List_push(&placeholders, pl);
      parser_token_advance(p);
      if (p->current_token.type == TOKEN_COMMA)
        parser_token_advance(p);
    }
    if (!parser_expect(p, TOKEN_GT, "Expected '>' after placeholders")) {
      List_free(&placeholders, 1);
      return NULL;
    }
  }

  if (placeholders.len > 0) {
    parser_placeholder_scope_push(p);
    for (size_t i = 0; i < placeholders.len; i++) {
      StringView placeholder_name = *(StringView *)placeholders.items[i];
      parser_add_placeholder(p, placeholder_name);
    }
  }

  if (!parser_expect(p, TOKEN_LBRACE, "Expected '{' after union name"))
    goto union_decl_error;

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    bool is_static = false;
    if (p->current_token.type == TOKEN_STATIC) {
      is_static = true;
      parser_token_advance(p);
    }

    if (p->current_token.type == TOKEN_FN) {
      AstNode *fn = parser_parse_fn_decl(p, is_static, false, false);
      if (fn) {
        List_push(&members, fn);
      } else {
        parser_sync(p);
      }
      continue;
    }

    Location mem_loc = p->current_token.loc;
    Token mem_ident = p->current_token;
    if (!parser_expect(p, TOKEN_IDENT, "Expected member name")) {
      parser_sync(p);
      continue;
    }

    if (!parser_expect(p, TOKEN_COLON, "Expected ':' after member name")) {
      parser_sync(p);
      continue;
    }

    Type *type = parser_parse_type_full(p);
    if (!type) {
      parser_sync(p);
      continue;
    }

    if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after member type")) {
      parser_sync(p);
      continue;
    }

    AstNode *mem_name = AstNode_new_var(mem_ident.text, mem_ident.loc);
    AstNode *mem_decl =
        AstNode_new_var_decl(mem_name, NULL, type, 0, false, mem_loc);
    List_push(&members, mem_decl);
  }

  if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after union members"))
    goto union_decl_error;

  if (placeholders.len > 0) {
    parser_placeholder_scope_pop(p);
  }

  return AstNode_new_union_decl(name_node, members, placeholders, is_frozen,
                                false, loc);

union_decl_error:
  if (placeholders.len > 0) {
    parser_placeholder_scope_pop(p);
  }
  List_free(&placeholders, 1);
  return NULL;
}

AstNode *parser_parse_error_set_decl(Parser *p, bool is_export) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'errors'

  Token name_token = p->current_token;
  if (!parser_expect(p, TOKEN_IDENT, "Expected error set name"))
    return NULL;
  AstNode *name_node = AstNode_new_var(name_token.text, name_token.loc);

  if (!parser_expect(p, TOKEN_ASSIGN, "Expected '=' after error set name"))
    return NULL;

  if (!parser_expect(p, TOKEN_LBRACE, "Expected '{' after error set name"))
    return NULL;

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    if (p->current_token.type != TOKEN_IDENT) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected error type name in error set");
      parser_sync(p);
      break;
    }

    AstNode *member =
        AstNode_new_var(p->current_token.text, p->current_token.loc);
    List_push(&members, member);
    parser_token_advance(p);

    if (p->current_token.type == TOKEN_COMMA)
      parser_token_advance(p);
  }

  if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after error set"))
    return NULL;
  if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after error set"))
    return NULL;

  return AstNode_new_error_set_decl(name_node, members, is_export, loc);
}

AstNode *parser_parse_impl_decl(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'impl'

  List placeholders;
  List_init(&placeholders);
  Type *impl_type = NULL;
  StringView owner_name = {0};

  if (p->current_token.type == TOKEN_IDENT) {
    owner_name = p->current_token.text;
    parser_token_advance(p);

    if (p->current_token.type == TOKEN_LT) {
      parser_token_advance(p);
      while (p->current_token.type != TOKEN_GT &&
             p->current_token.type != TOKEN_EOF) {
        if (p->current_token.type != TOKEN_IDENT) {
          ErrorHandler_report(p->eh, p->current_token.loc,
                              "Expected placeholder name");
          break;
        }
        StringView *pl = xmalloc(sizeof(StringView));
        *pl = p->current_token.text;
        List_push(&placeholders, pl);
        parser_token_advance(p);
        if (p->current_token.type == TOKEN_COMMA)
          parser_token_advance(p);
      }
      if (!parser_expect(p, TOKEN_GT, "Expected '>' after placeholders")) {
        List_free(&placeholders, 1);
        return NULL;
      }
    }

    if (placeholders.len > 0) {
      parser_placeholder_scope_push(p);
      for (size_t i = 0; i < placeholders.len; i++) {
        StringView placeholder_name = *(StringView *)placeholders.items[i];
        parser_add_placeholder(p, placeholder_name);
      }
    }

    if (sv_eq_cstr(owner_name, "String")) {
      impl_type = type_get_string_buffer(p->type_ctx);
    } else if (sv_eq_cstr(owner_name, "str")) {
      impl_type = type_get_primitive(p->type_ctx, PRIM_STRING);
    } else if (placeholders.len > 0) {
      Type *template_type = type_get_template(p->type_ctx, owner_name);
      if (!template_type) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Undefined generic type '%.*s' for impl",
                            (int)owner_name.len, owner_name.data);
        parser_sync(p);
        if (placeholders.len > 0)
          parser_placeholder_scope_pop(p);
        return NULL;
      }

      List args;
      List_init(&args);
      for (size_t i = 0; i < placeholders.len; i++) {
        StringView placeholder_name = *(StringView *)placeholders.items[i];
        Type *placeholder = parser_find_placeholder(p, placeholder_name);
        List_push(&args, placeholder);
      }
      impl_type = type_get_instance(p->type_ctx, template_type, args);
      List_free(&args, 0);
    } else {
      impl_type = type_get_named(p->type_ctx, owner_name);
      if (!impl_type)
        impl_type = type_get_struct(p->type_ctx, owner_name);
      if (!impl_type)
        impl_type = type_get_union(p->type_ctx, owner_name);
    }
  } else {
    impl_type = parser_parse_type_full(p);
    if (!impl_type) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected type name after 'impl'");
      parser_sync(p);
      return NULL;
    }
  }

  if (!impl_type) {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Undefined type '%.*s' for impl", (int)owner_name.len,
                        owner_name.data);
    parser_sync(p);
    if (placeholders.len > 0)
      parser_placeholder_scope_pop(p);
    return NULL;
  }

  if (!parser_expect(p, TOKEN_LBRACE, "Expected '{' after impl type name")) {
    if (placeholders.len > 0)
      parser_placeholder_scope_pop(p);
    List_free(&placeholders, 1);
    return NULL;
  }

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    bool is_static = false;
    if (p->current_token.type == TOKEN_STATIC) {
      is_static = true;
      parser_token_advance(p);
    }

    if (p->current_token.type == TOKEN_FN) {
      AstNode *fn = parser_parse_fn_decl(p, is_static, false, false);
      if (fn) {
        List_push(&members, fn);
      } else {
        parser_sync(p);
      }
      continue;
    }

    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected function declaration inside impl block");
    parser_sync(p);
  }

  if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after impl members")) {
    if (placeholders.len > 0)
      parser_placeholder_scope_pop(p);
    List_free(&placeholders, 1);
    return NULL;
  }

  if (placeholders.len > 0) {
    parser_placeholder_scope_pop(p);
  }

  return AstNode_new_impl_decl(impl_type, members, loc);
}
