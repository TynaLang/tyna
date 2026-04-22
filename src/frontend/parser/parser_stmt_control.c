#include "parser_internal.h"

static AstNode *parser_parse_case_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'case'

  AstNode *pattern = NULL;
  Type *pattern_type = NULL;

  if (p->current_token.type == TOKEN_STRING) {
    pattern = AstNode_new_string(p->current_token.text, p->current_token.loc);
    parser_token_advance(p);
  } else if (p->current_token.type == TOKEN_IDENT) {
    Token ident = p->current_token;
    parser_token_advance(p);

    if (sv_eq_cstr(ident.text, "_")) {
      pattern = NULL;
    } else {
      if (!parser_expect(p, TOKEN_COLON,
                         "Expected ':' after case variable name"))
        return NULL;
      pattern_type = parser_parse_type_full(p);
      if (!pattern_type)
        return NULL;
      pattern = AstNode_new_var(ident.text, ident.loc);
    }
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected string literal, '_' default, or typed case "
                        "variable after 'case'");
    return NULL;
  }

  if (!parser_expect(p, TOKEN_FAT_ARROW, "Expected '=>' after case pattern"))
    return NULL;

  AstNode *body = NULL;
  if (p->current_token.type == TOKEN_LBRACE) {
    body = parser_parse_block(p);
    if (!body)
      return NULL;
  } else {
    List stmts;
    List_init(&stmts);
    while (p->current_token.type != TOKEN_CASE &&
           p->current_token.type != TOKEN_RBRACE &&
           p->current_token.type != TOKEN_EOF) {
      AstNode *stmt = parser_parse_statement(p);
      if (!stmt)
        return NULL;
      List_push(&stmts, stmt);
    }
    if (stmts.len == 1) {
      body = stmts.items[0];
    } else {
      body = AstNode_new_block(loc);
      body->block.statements = stmts;
    }
  }

  return AstNode_new_case_stmt(pattern, pattern_type, body, loc);
}

AstNode *parser_parse_switch_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'switch'

  AstNode *expr = parser_parse_expression(p, 0);
  if (!expr)
    return NULL;

  if (!parser_expect(p, TOKEN_LBRACE, "Expected '{' after switch expression"))
    return NULL;

  List cases;
  List_init(&cases);
  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    if (p->current_token.type != TOKEN_CASE) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'case' inside switch body");
      parser_token_advance(p);
      continue;
    }

    AstNode *case_node = parser_parse_case_stmt(p);
    if (case_node) {
      List_push(&cases, case_node);
    } else {
      parser_sync(p);
    }
  }

  if (!parser_expect(p, TOKEN_RBRACE, "Expected '}' after switch body"))
    return NULL;

  return AstNode_new_switch_stmt(expr, cases, loc);
}

AstNode *parser_parse_if_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // Consume 'if'

  if (!parser_expect(p, TOKEN_LPAREN, "Expected '(' after 'if'"))
    return NULL;

  AstNode *condition = parser_parse_expression(p, 0);
  if (!condition)
    return NULL;

  if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after condition"))
    return NULL;

  AstNode *then_branch = parser_parse_statement(p);
  if (!then_branch)
    return NULL;

  AstNode *else_branch = NULL;
  if (p->current_token.type == TOKEN_ELSE) {
    parser_token_advance(p); // Consume 'else'
    else_branch = parser_parse_statement(p);
    if (!else_branch)
      return NULL;
  }

  return AstNode_new_if_stmt(condition, then_branch, else_branch, loc);
}

AstNode *parser_parse_while_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // Consume 'while'

  if (!parser_expect(p, TOKEN_LPAREN, "Expected '(' after 'while'"))
    return NULL;

  AstNode *condition = parser_parse_expression(p, 0);
  if (!condition)
    return NULL;

  if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after condition"))
    return NULL;

  AstNode *body = parser_parse_statement(p);
  if (!body)
    return NULL;

  return AstNode_new_while(condition, body, loc);
}

AstNode *parser_parse_for_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume 'for'

  if (!parser_expect(p, TOKEN_LPAREN, "Expected '(' after 'for'"))
    return NULL;

  if (p->current_token.type == TOKEN_IDENT &&
      parser_token_peek(p->lexer, 1).type == TOKEN_IN) {
    AstNode *var = AstNode_new_var(p->current_token.text, p->current_token.loc);
    parser_token_advance(p); // consume name
    parser_token_advance(p); // consume 'in'

    AstNode *iterable = parser_parse_expression(p, 0);
    if (!iterable)
      return NULL;

    if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after iterable"))
      return NULL;

    AstNode *body = parser_parse_statement(p);
    if (!body)
      return NULL;

    return AstNode_new_for_in(var, iterable, body, loc);
  }

  AstNode *init = NULL;
  if (p->current_token.type != TOKEN_SEMI) {
    if (p->current_token.type == TOKEN_LET) {
      init = parser_parse_var_decl(p, false);
      if (!init)
        return NULL;
    } else {
      init = parser_parse_expression(p, 0);
      if (!init)
        return NULL;
      if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after for init"))
        return NULL;
    }
  } else {
    parser_token_advance(p); // consume ';'
  }

  AstNode *cond = NULL;
  if (p->current_token.type != TOKEN_SEMI) {
    cond = parser_parse_expression(p, 0);
    if (!cond)
      return NULL;
  }
  if (!parser_expect(p, TOKEN_SEMI, "Expected ';' after for condition"))
    return NULL;

  AstNode *inc = NULL;
  if (p->current_token.type != TOKEN_RPAREN) {
    inc = parser_parse_expression(p, 0);
    if (!inc)
      return NULL;
  }

  if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after for loop header"))
    return NULL;

  AstNode *body = parser_parse_statement(p);
  if (!body)
    return NULL;

  return AstNode_new_for(init, cond, inc, body, loc);
}
