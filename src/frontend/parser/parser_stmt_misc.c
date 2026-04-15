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

static AstNode *Parser_parse_import(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'import'

  StringView path;
  StringView alias;
  size_t cursor = p->lexer->cursor - p->current_token.text.len;

  AstNode *path_expr = Parser_parse_expression(p, 0);

  if (!path_expr) {
    ErrorHandler_report(p->eh, loc, "Expected module path after 'import'");
    return NULL;
  }

  if (path_expr->tag == NODE_VAR) {
    path = path_expr->var.value;
    alias = path_expr->var.value;
  } else if (path_expr->tag == NODE_FIELD) {
    AstNode *root = path_expr;
    while (root->tag == NODE_FIELD) {
      root = root->field.object;
    }
    if (root->tag != NODE_VAR) {
      ErrorHandler_report(
          p->eh, loc,
          "Expected module path to be an identifier or dotted path");
      return NULL;
    }

    alias = path_expr->field.field;
    const char *end = alias.data + alias.len;
    path = (StringView){.data = p->lexer->src + cursor,
                        .len = (size_t)(end - (p->lexer->src + cursor))};
  } else {
    ErrorHandler_report(
        p->eh, loc, "Expected module path to be an identifier or dotted path");
    return NULL;
  }

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
