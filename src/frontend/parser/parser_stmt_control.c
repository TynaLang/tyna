#include "parser_internal.h"

static AstNode *Parser_parse_case_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'case'

  AstNode *pattern = NULL;
  Type *pattern_type = NULL;

  if (p->current_token.type == TOKEN_STRING) {
    pattern = AstNode_new_string(p->current_token.text, p->current_token.loc);
    Parser_token_advance(p);
  } else if (p->current_token.type == TOKEN_IDENT) {
    Token ident = p->current_token;
    Parser_token_advance(p);

    if (sv_eq_cstr(ident.text, "_")) {
      pattern = NULL;
    } else {
      if (!Parser_expect(p, TOKEN_COLON,
                         "Expected ':' after case variable name"))
        return NULL;
      pattern_type = Parser_parse_type_full(p);
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

  if (!Parser_expect(p, TOKEN_FAT_ARROW, "Expected '=>' after case pattern"))
    return NULL;

  AstNode *body = NULL;
  if (p->current_token.type == TOKEN_LBRACE) {
    body = Parser_parse_block(p);
  } else {
    List stmts;
    List_init(&stmts);
    while (p->current_token.type != TOKEN_CASE &&
           p->current_token.type != TOKEN_RBRACE &&
           p->current_token.type != TOKEN_EOF) {
      AstNode *stmt = Parser_parse_statement(p);
      if (!stmt)
        break;
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

AstNode *Parser_parse_switch_stmt(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'switch'

  AstNode *expr = Parser_parse_expression(p, 0);
  if (!expr)
    return NULL;

  if (!Parser_expect(p, TOKEN_LBRACE, "Expected '{' after switch expression"))
    return NULL;

  List cases;
  List_init(&cases);
  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    if (p->current_token.type != TOKEN_CASE) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'case' inside switch body");
      Parser_token_advance(p);
      continue;
    }

    AstNode *case_node = Parser_parse_case_stmt(p);
    if (case_node) {
      List_push(&cases, case_node);
    } else {
      Parser_sync(p);
    }
  }

  if (!Parser_expect(p, TOKEN_RBRACE, "Expected '}' after switch body"))
    return NULL;

  return AstNode_new_switch_stmt(expr, cases, loc);
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
      init = Parser_parse_var_decl(p, false);
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
