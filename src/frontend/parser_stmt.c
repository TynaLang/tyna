#include "parser_internal.h"

AstNode *Parser_parse_block(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume '{'

  AstNode *block = AstNode_new_block(loc);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    AstNode *stmt = Parser_parse_statement(p);
    if (!stmt) {
      Parser_sync(p);
      continue;
    }
    List_push(&block->block.statements, stmt);
  }

  if (!Parser_expect(p, TOKEN_RBRACE, "Expected '}' after block"))
    return NULL;

  return block;
}

AstNode *Parser_parse_var_decl(Parser *p) {
  int is_const = (p->current_token.type == TOKEN_CONST);
  Parser_token_advance(p);

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT,
                     "Expected identifier after 'let' or 'const'"))
    return NULL;

  AstNode *name = AstNode_new_var(ident.text, ident.loc);

  Type *declared_type = NULL;
  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p);
    declared_type = Parser_parse_type_full(p);
    if (!declared_type)
      return NULL;
  } else {
    declared_type = type_get_primitive(p->type_ctx, PRIM_UNKNOWN);
  }

  if (p->current_token.type == TOKEN_ASSIGN) {
    Parser_token_advance(p);
    AstNode *value = Parser_parse_expression(p, 0);
    if (!value)
      return NULL;
    if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after value"))
      return NULL;

    return AstNode_new_var_decl(name, value, declared_type, is_const,
                                ident.loc);
  } else {
    if (declared_type->kind == KIND_PRIMITIVE &&
        declared_type->data.primitive == PRIM_UNKNOWN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Type required for uninitialized variable");
      return NULL;
    }

    if (!Parser_expect(p, TOKEN_SEMI,
                       "Expected ';' or '=' after identifier/type"))
      return NULL;

    return AstNode_new_var_decl(name, NULL, declared_type, is_const, ident.loc);
  }
}

AstNode *Parser_parse_print(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p);

  bool has_parens = false;
  if (p->current_token.type == TOKEN_LPAREN) {
    Parser_token_advance(p);
    has_parens = true;
  }

  List values;
  List_init(&values);

  if (p->current_token.type != TOKEN_RPAREN) {
    while (1) {
      AstNode *expr = Parser_parse_expression(p, 0);
      if (!expr)
        return NULL;
      List_push(&values, expr);
      if (p->current_token.type == TOKEN_COMMA)
        Parser_token_advance(p);
      else
        break;
    }
  }

  if (has_parens) {
    if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after print"))
      return NULL;
  }

  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after print statement"))
    return NULL;

  return AstNode_new_print_stmt(values, loc);
}

AstNode *Parser_parse_fn_decl(Parser *p, bool is_static) {
  Parser_token_advance(p); // consume 'fn'

  Token ident = p->current_token;
  Location loc = ident.loc;

  if (!Parser_expect(p, TOKEN_IDENT, "Expected function name"))
    return NULL;

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after function name"))
    return NULL;

  List params;
  List_init(&params);

  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF) {
    Location p_loc = p->current_token.loc;

    StringView param_name = p->current_token.text;
    AstNode *param_name_node = AstNode_new_var(param_name, p_loc);
    if (!Parser_expect(p, TOKEN_IDENT, "Expected parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_COLON, "Expected ':' after parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    Type *param_type = Parser_parse_type_full(p);
    if (!param_type) {
      skip_to_next_param(p);
      continue;
    }

    List_push(&params, AstNode_new_param(param_name_node, param_type, p_loc));
    if (p->current_token.type == TOKEN_COMMA)
      Parser_token_advance(p);
  }

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after parameters"))
    return NULL;

  Type *ret_type = type_get_primitive(p->type_ctx, PRIM_VOID);
  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p);
    ret_type = Parser_parse_type_full(p);
    if (!ret_type) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected return type after ':'");
      return NULL;
    }
  }

  AstNode *body = NULL;
  if (p->current_token.type == TOKEN_LBRACE) {
    body = Parser_parse_block(p);
  } else if (p->current_token.type == TOKEN_SEMI) {
    Parser_token_advance(p);
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected '{' or ';' after function declaration");
    Parser_sync(p);
  }

  return AstNode_new_func_decl(AstNode_new_var(ident.text, ident.loc), params,
                               ret_type, body, is_static, loc);
}

AstNode *Parser_parse_if_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // Consume 'if'

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after 'if'"))
    return NULL;

  AstNode *condition = Parser_parse_expression(p, 0);
  if (!condition)
    return NULL;

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after condition"))
    return NULL;

  AstNode *then_branch = Parser_parse_statement(p);
  if (!then_branch)
    return NULL;

  AstNode *else_branch = NULL;
  if (p->current_token.type == TOKEN_ELSE) {
    Parser_token_advance(p); // Consume 'else'
    else_branch = Parser_parse_statement(p);
  }

  return AstNode_new_if_stmt(condition, then_branch, else_branch, loc);
}

AstNode *Parser_parse_while_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // Consume 'while'

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after 'while'"))
    return NULL;

  AstNode *condition = Parser_parse_expression(p, 0);
  if (!condition)
    return NULL;

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after condition"))
    return NULL;

  AstNode *body = Parser_parse_statement(p);
  if (!body)
    return NULL;

  return AstNode_new_while(condition, body, loc);
}

AstNode *Parser_parse_for_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'for'

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after 'for'"))
    return NULL;

  if (p->current_token.type == TOKEN_IDENT &&
      Parser_token_peek(p->lexer, 1).type == TOKEN_IN) {
    AstNode *var = AstNode_new_var(p->current_token.text, p->current_token.loc);
    Parser_token_advance(p); // consume name
    Parser_token_advance(p); // consume 'in'

    AstNode *iterable = Parser_parse_expression(p, 0);
    if (!iterable)
      return NULL;

    if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after iterable"))
      return NULL;

    AstNode *body = Parser_parse_statement(p);
    if (!body)
      return NULL;

    return AstNode_new_for_in(var, iterable, body, loc);
  }

  AstNode *init = NULL;
  if (p->current_token.type != TOKEN_SEMI) {
    if (p->current_token.type == TOKEN_LET) {
      init = Parser_parse_var_decl(p);
    } else {
      init = Parser_parse_expression(p, 0);
      if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after for init"))
        return NULL;
    }
  } else {
    Parser_token_advance(p); // consume ';'
  }

  AstNode *cond = NULL;
  if (p->current_token.type != TOKEN_SEMI) {
    cond = Parser_parse_expression(p, 0);
  }
  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after for condition"))
    return NULL;

  AstNode *inc = NULL;
  if (p->current_token.type != TOKEN_RPAREN) {
    inc = Parser_parse_expression(p, 0);
  }

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after for loop header"))
    return NULL;

  AstNode *body = Parser_parse_statement(p);
  if (!body)
    return NULL;

  return AstNode_new_for(init, cond, inc, body, loc);
}

AstNode *Parser_parse_struct_decl(Parser *p, bool is_frozen) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'struct'

  Token name_token = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT, "Expected struct name"))
    return NULL;
  AstNode *name_node = AstNode_new_var(name_token.text, name_token.loc);

  List placeholders;
  List_init(&placeholders);
  if (p->current_token.type == TOKEN_LT) {
    Parser_token_advance(p);
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
      Parser_token_advance(p);
      if (p->current_token.type == TOKEN_COMMA)
        Parser_token_advance(p);
    }
    Parser_expect(p, TOKEN_GT, "Expected '>' after placeholders");
  }

  if (!Parser_expect(p, TOKEN_LBRACE, "Expected '{' after struct name"))
    return NULL;

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    bool is_static = false;
    if (p->current_token.type == TOKEN_STATIC) {
      is_static = true;
      Parser_token_advance(p);
    }

    if (p->current_token.type == TOKEN_FN) {
      AstNode *fn = Parser_parse_fn_decl(p, is_static);
      if (fn)
        List_push(&members, fn);
      continue;
    }

    Location mem_loc = p->current_token.loc;
    Token mem_ident = p->current_token;
    if (!Parser_expect(p, TOKEN_IDENT, "Expected member name")) {
      Parser_sync(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_COLON, "Expected ':' after member name")) {
      Parser_sync(p);
      continue;
    }

    Type *type = Parser_parse_type_full(p);
    if (!type) {
      Parser_sync(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after member type")) {
      Parser_sync(p);
      continue;
    }

    AstNode *mem_name = AstNode_new_var(mem_ident.text, mem_ident.loc);
    AstNode *mem_decl = AstNode_new_var_decl(mem_name, NULL, type, 0, mem_loc);
    List_push(&members, mem_decl);
  }

  Parser_expect(p, TOKEN_RBRACE, "Expected '}' after struct members");

  return AstNode_new_struct_decl(name_node, members, placeholders, is_frozen,
                                 loc);
}

AstNode *Parser_parse_impl_decl(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'impl'

  Token type_token = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT, "Expected type name after 'impl'"))
    return NULL;
  AstNode *name_node = AstNode_new_var(type_token.text, type_token.loc);

  if (!Parser_expect(p, TOKEN_LBRACE, "Expected '{' after impl type name"))
    return NULL;

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    bool is_static = false;
    if (p->current_token.type == TOKEN_STATIC) {
      is_static = true;
      Parser_token_advance(p);
    }

    if (p->current_token.type == TOKEN_FN) {
      AstNode *fn = Parser_parse_fn_decl(p, is_static);
      if (fn)
        List_push(&members, fn);
      continue;
    }

    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected function declaration inside impl block");
    Parser_sync(p);
  }

  Parser_expect(p, TOKEN_RBRACE, "Expected '}' after impl members");
  return AstNode_new_impl_decl(name_node, members, loc);
}

AstNode *Parser_parse_statement(Parser *p) {
  Location loc = p->current_token.loc;

  switch (p->current_token.type) {
  case TOKEN_EOF:
    return NULL;
  case TOKEN_LET:
  case TOKEN_CONST:
    return Parser_parse_var_decl(p);
  case TOKEN_LBRACE:
    return Parser_parse_block(p);
  case TOKEN_PRINT:
    return Parser_parse_print(p);
  case TOKEN_IF:
    return Parser_parse_if_stmt(p);
  case TOKEN_WHILE:
    return Parser_parse_while_stmt(p);
  case TOKEN_FOR:
    return Parser_parse_for_stmt(p);
  case TOKEN_DEFER: {
    Parser_token_advance(p);
    return AstNode_new_defer(Parser_parse_statement(p), loc);
  }
  case TOKEN_LOOP: {
    Parser_token_advance(p);
    return AstNode_new_loop(Parser_parse_statement(p), loc);
  }
  case TOKEN_RETURN: {
    Parser_token_advance(p);

    AstNode *expr = NULL;
    if (p->current_token.type != TOKEN_SEMI) {
      expr = Parser_parse_expression(p, 0);
    }

    if (Parser_expect(p, TOKEN_SEMI, "Expected ';' after return statement"))
      return AstNode_new_return(expr, loc);

    Parser_sync(p);
    return NULL;
  }
  case TOKEN_FN:
    return Parser_parse_fn_decl(p, false);
  case TOKEN_STRUCT:
    return Parser_parse_struct_decl(p, false);
  case TOKEN_IMPL:
    return Parser_parse_impl_decl(p);
  case TOKEN_FROZEN: {
    Parser_token_advance(p);
    if (p->current_token.type != TOKEN_STRUCT) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'struct' after 'frozen'");
      return NULL;
    }
    return Parser_parse_struct_decl(p, true);
  }
  case TOKEN_STATIC: {
    Parser_token_advance(p);
    if (p->current_token.type != TOKEN_FN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'fn' after 'static'");
      return NULL;
    }
    return Parser_parse_fn_decl(p, true);
  }
  case TOKEN_BREAK: {
    Parser_token_advance(p);
    if (Parser_expect(p, TOKEN_SEMI, "Expected ';' after break"))
      return AstNode_new_break(loc);

    Parser_sync(p);
    return NULL;
  }
  case TOKEN_CONTINUE: {
    Parser_token_advance(p);
    if (Parser_expect(p, TOKEN_SEMI, "Expected ';' after continue"))
      return AstNode_new_continue(loc);

    Parser_sync(p);
    return NULL;
  }
  default: {
    AstNode *expr = Parser_parse_expression(p, 0);
    if (Parser_expect(p, TOKEN_SEMI, "Expected ';' after expression"))
      return AstNode_new_expr_stmt(expr, loc);

    Parser_sync(p);
    return NULL;
  }
  }
}
