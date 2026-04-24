#include "parser_internal.h"

bool parser_parse_visibility_modifier(Parser *p, bool *is_export,
                                      bool *is_pub_module, bool *is_external) {
  bool seen = false;
  while (p->current_token.type == TOKEN_PUB ||
         p->current_token.type == TOKEN_EXTERNAL) {
    if (p->current_token.type == TOKEN_PUB) {
      seen = true;
      *is_export = true;
      parser_token_advance(p);
      if (p->current_token.type == TOKEN_LPAREN) {
        parser_token_advance(p);
        if (p->current_token.type != TOKEN_IDENT ||
            !(sv_eq_cstr(p->current_token.text, "stdlib") ||
              sv_eq_cstr(p->current_token.text, "module"))) {
          ErrorHandler_report(p->eh, p->current_token.loc,
                              "Expected 'stdlib' or 'module' after 'pub('");
          return false;
        }
        *is_pub_module = true;
        parser_token_advance(p);
        if (!parser_expect(p, TOKEN_RPAREN,
                           "Expected ')' after pub visibility")) {
          return false;
        }
      }
      continue;
    }

    if (p->current_token.type == TOKEN_EXTERNAL) {
      *is_external = true;
      seen = true;
      parser_token_advance(p);
      continue;
    }
  }
  return seen;
}

AstNode *parser_parse_block(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume '{'

  AstNode *block = AstNode_new_block(loc);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    AstNode *stmt = parser_parse_statement(p);
    if (!stmt) {
      parser_sync(p);
      continue;
    }
    List_push(&block->block.statements, stmt);
  }

  if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after block"))
    return NULL;

  return block;
}

static bool parser_is_import_path_segment(TokenType type) {
  return type == TOKEN_IDENT || type == TOKEN_ERROR_KEYWORD ||
         type == TOKEN_ERRORS;
}

static AstNode *parser_parse_import(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'import'

  if (!parser_is_import_path_segment(p->current_token.type)) {
    ErrorHandler_report(p->eh, loc, "Expected module path after 'import'");
    return NULL;
  }

  const char *path_start = p->current_token.text.data;
  const char *path_end = p->current_token.text.data + p->current_token.text.len;
  StringView alias = p->current_token.text;

  parser_token_advance(p);
  while (p->current_token.type == TOKEN_DOT) {
    parser_token_advance(p);
    if (!parser_is_import_path_segment(p->current_token.type)) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected module path after '.'");
      return NULL;
    }
    alias = p->current_token.text;
    path_end = p->current_token.text.data + p->current_token.text.len;
    parser_token_advance(p);
  }

  StringView path = sv_from_parts(path_start, (size_t)(path_end - path_start));

  if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after import statement"))
    return NULL;

  return AstNode_new_import(path, alias, loc);
}

AstNode *parser_parse_print(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p);

  bool has_parens = false;
  if (p->current_token.type == TOKEN_LPAREN) {
    parser_token_advance(p);
    has_parens = true;
  }

  List values;
  List_init(&values);

  if (p->current_token.type != TOKEN_RPAREN) {
    while (1) {
      AstNode *expr = parser_parse_expression(p, 0);
      if (!expr)
        return NULL;
      List_push(&values, expr);
      if (p->current_token.type == TOKEN_COMMA)
        parser_token_advance(p);
      else
        break;
    }
  }

  if (has_parens) {
    if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after print"))
      return NULL;
  }

  if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after print statement"))
    return NULL;

  return AstNode_new_print_stmt(values, loc);
}

AstNode *parser_parse_return_stmt(Parser *p, bool require_semi) {
  Location loc = p->current_token.loc;
  parser_token_advance(p);

  AstNode *expr = NULL;
  if (p->current_token.type != TOKEN_SEMI) {
    expr = parser_parse_expression(p, 0);
    if (!expr) {
      parser_sync(p);
      return NULL;
    }
  }

  if (require_semi) {
    if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after return statement"))
      return NULL;
  }

  return AstNode_new_return(expr, loc);
}

AstNode *parser_parse_statement(Parser *p) {
  Location loc = p->current_token.loc;
  bool is_export = false;
  bool is_pub_module = false;
  bool is_external = false;

  if (!parser_parse_visibility_modifier(p, &is_export, &is_pub_module,
                                        &is_external)) {
    if (p->current_token.type == TOKEN_PUB)
      return NULL;
  }

  switch (p->current_token.type) {
  case TOKEN_EOF:
    return NULL;
  case TOKEN_IMPORT:
    return parser_parse_import(p);
  case TOKEN_LET:
  case TOKEN_CONST:
    return parser_parse_var_decl(p, is_export);
  case TOKEN_LBRACE:
    return parser_parse_block(p);
  case TOKEN_PRINT:
    return parser_parse_print(p);
  case TOKEN_IF:
    return parser_parse_if_stmt(p);
  case TOKEN_SWITCH:
    return parser_parse_switch_stmt(p);
  case TOKEN_WHILE:
    return parser_parse_while_stmt(p);
  case TOKEN_FOR:
    return parser_parse_for_stmt(p);
  case TOKEN_DEFER: {
    parser_token_advance(p);
    AstNode *stmt = parser_parse_statement(p);
    if (!stmt) {
      parser_sync(p);
      return NULL;
    }
    return AstNode_new_defer(stmt, loc);
  }
  case TOKEN_LOOP: {
    parser_token_advance(p);
    AstNode *stmt = parser_parse_statement(p);
    if (!stmt) {
      parser_sync(p);
      return NULL;
    }
    return AstNode_new_loop(stmt, loc);
  }
  case TOKEN_RETURN: {
    return parser_parse_return_stmt(p, true);
  }
  case TOKEN_FN:
    return parser_parse_fn_decl(p, false, is_export, is_pub_module,
                                is_external);
  case TOKEN_STRUCT:
    return parser_parse_struct_decl(p, false, is_export);
  case TOKEN_UNION:
    return parser_parse_union_decl(p, false, is_export);
  case TOKEN_AT:
    return parser_parse_error_decl(p, is_export);
  case TOKEN_ERROR_KEYWORD:
    return parser_parse_error_decl(p, is_export);
  case TOKEN_ERRORS:
    return parser_parse_error_set_decl(p, is_export);
  case TOKEN_IMPL:
    return parser_parse_impl_decl(p);
  case TOKEN_FROZEN: {
    parser_token_advance(p);
    if (p->current_token.type != TOKEN_STRUCT &&
        p->current_token.type != TOKEN_UNION) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'struct' or 'union' after 'frozen'");
      return NULL;
    }
    if (p->current_token.type == TOKEN_STRUCT)
      return parser_parse_struct_decl(p, true, is_export);
    return parser_parse_union_decl(p, true, is_export);
  }
  case TOKEN_STATIC: {
    parser_token_advance(p);
    if (p->current_token.type != TOKEN_FN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'fn' after 'static'");
      return NULL;
    }
    return parser_parse_fn_decl(p, true, is_export, is_pub_module, is_external);
  }
  case TOKEN_BREAK: {
    parser_token_advance(p);
    if (parser_expect(p, TOKEN_SEMI, "Expected ';' after break"))
      return AstNode_new_break(loc);

    parser_sync(p);
    return NULL;
  }
  case TOKEN_CONTINUE: {
    parser_token_advance(p);
    if (parser_expect(p, TOKEN_SEMI, "Expected ';' after continue"))
      return AstNode_new_continue(loc);

    parser_sync(p);
    return NULL;
  }
  default: {
    AstNode *expr = parser_parse_expression(p, 0);
    if (!expr) {
      parser_sync(p);
      return NULL;
    }
    if (parser_expect(p, TOKEN_SEMI, "Expected ';' after expression"))
      return AstNode_new_expr_stmt(expr, loc);

    parser_sync(p);
    return NULL;
  }
  }
}
