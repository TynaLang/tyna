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

static bool Parser_is_import_path_segment(TokenType type) {
  return type == TOKEN_IDENT || type == TOKEN_ERROR_KEYWORD ||
         type == TOKEN_ERRORS;
}

static AstNode *Parser_parse_import(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'import'

  if (!Parser_is_import_path_segment(p->current_token.type)) {
    ErrorHandler_report(p->eh, loc, "Expected module path after 'import'");
    return NULL;
  }

  const char *path_start = p->current_token.text.data;
  const char *path_end = p->current_token.text.data + p->current_token.text.len;
  StringView alias = p->current_token.text;

  Parser_token_advance(p);
  while (p->current_token.type == TOKEN_DOT) {
    Parser_token_advance(p);
    if (!Parser_is_import_path_segment(p->current_token.type)) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected module path after '.'");
      return NULL;
    }
    alias = p->current_token.text;
    path_end = p->current_token.text.data + p->current_token.text.len;
    Parser_token_advance(p);
  }

  StringView path = sv_from_parts(path_start, (size_t)(path_end - path_start));

  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after import statement"))
    return NULL;

  return AstNode_new_import(path, alias, loc);
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

AstNode *Parser_parse_statement(Parser *p) {
  Location loc = p->current_token.loc;
  bool is_export = false;
  bool is_external = false;

  while (p->current_token.type == TOKEN_EXPORT ||
         p->current_token.type == TOKEN_EXTERNAL) {
    if (p->current_token.type == TOKEN_EXPORT) {
      is_export = true;
      Parser_token_advance(p);
      continue;
    }
    if (p->current_token.type == TOKEN_EXTERNAL) {
      is_external = true;
      Parser_token_advance(p);
      continue;
    }
  }

  switch (p->current_token.type) {
  case TOKEN_EOF:
    return NULL;
  case TOKEN_IMPORT:
    return Parser_parse_import(p);
  case TOKEN_LET:
  case TOKEN_CONST:
    return Parser_parse_var_decl(p, is_export);
  case TOKEN_LBRACE:
    return Parser_parse_block(p);
  case TOKEN_PRINT:
    return Parser_parse_print(p);
  case TOKEN_IF:
    return Parser_parse_if_stmt(p);
  case TOKEN_SWITCH:
    return Parser_parse_switch_stmt(p);
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
    return Parser_parse_fn_decl(p, false, is_export, is_external);
  case TOKEN_STRUCT:
    return Parser_parse_struct_decl(p, false, is_export);
  case TOKEN_UNION:
    return Parser_parse_union_decl(p, false, is_export);
  case TOKEN_ERROR_KEYWORD:
    return Parser_parse_error_decl(p, is_export);
  case TOKEN_ERRORS:
    return Parser_parse_error_set_decl(p, is_export);
  case TOKEN_IMPL:
    return Parser_parse_impl_decl(p);
  case TOKEN_FROZEN: {
    Parser_token_advance(p);
    if (p->current_token.type != TOKEN_STRUCT &&
        p->current_token.type != TOKEN_UNION) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'struct' or 'union' after 'frozen'");
      return NULL;
    }
    if (p->current_token.type == TOKEN_STRUCT)
      return Parser_parse_struct_decl(p, true, is_export);
    return Parser_parse_union_decl(p, true, is_export);
  }
  case TOKEN_STATIC: {
    Parser_token_advance(p);
    if (p->current_token.type != TOKEN_FN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'fn' after 'static'");
      return NULL;
    }
    return Parser_parse_fn_decl(p, true, is_export, is_external);
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
