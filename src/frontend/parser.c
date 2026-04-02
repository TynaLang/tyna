#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include "utils.h"

// ---------- UTILITIES ---------- //

static Token *Parser_token_advance(Parser *p) {
  Token *prev_token = &p->current_token;
  p->current_token = Token_advance(p->lexer);
  return prev_token;
}

static Token Parser_token_peek(Lexer *lexer, int n) {
  size_t saved_cursor = lexer->cursor;
  Location saved_loc = lexer->loc;
  Token t = {0};

  for (int i = 0; i < n; i++) {
    t = Token_advance(lexer);
    if (t.type == TOKEN_EOF)
      break;
  }

  lexer->cursor = saved_cursor;
  lexer->loc = saved_loc;
  return t;
}

static int Parser_expect(Parser *p, TokenType type, const char *msg) {
  if (p->current_token.type != type) {
    ErrorHandler_report(p->eh, p->current_token.loc, "%s", msg);
    return 0;
  }
  Parser_token_advance(p);
  return 1;
}

static int Parser_check(AstNode *p, AstKind type) {
  if (!p)
    return 0;
  if (p->tag == type)
    return 1;
  return 0;
}

static void Parser_sync(Parser *p) {
  while (p->current_token.type != TOKEN_EOF) {
    if (p->current_token.type == TOKEN_SEMI) {
      Parser_token_advance(p);
      return;
    }
    switch (p->current_token.type) {
    case TOKEN_LET:
    case TOKEN_CONST:
    case TOKEN_PRINT:
    case TOKEN_FN:
    case TOKEN_RETURN:
      return;
    default:
      Parser_token_advance(p);
    }
  }
}

static void Parser_sync_param(Parser *p) {
  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF)
    Parser_token_advance(p);
}

static int is_binary_operator(TokenType t) {
  switch (t) {
  case TOKEN_ASSIGN:
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_STAR:
  case TOKEN_SLASH:
  case TOKEN_POWER:
  case TOKEN_MOD:
    return 1;
  default:
    return 0;
  }
}

static int is_postfix(Token t) {
  return t.type == TOKEN_LPAREN || t.type == TOKEN_LBRACKET ||
         t.type == TOKEN_DOT || t.type == TOKEN_PLUS_PLUS ||
         t.type == TOKEN_MINUS_MINUS;
}

static BinaryOp token_to_op(TokenType type) {
  switch (type) {
  case TOKEN_PLUS:
    return OP_ADD;
  case TOKEN_MINUS:
    return OP_SUB;
  case TOKEN_STAR:
    return OP_MUL;
  case TOKEN_SLASH:
    return OP_DIV;
  case TOKEN_POWER:
    return OP_POW;
  case TOKEN_MOD:
    return OP_MOD;
  default:
    fprintf(stderr, "Unexpected operator in token_to_op\n");
    exit(1);
  }
}

static BindingPower infix_binding_power(TokenType t) {
  switch (t) {
  case TOKEN_ASSIGN:
    return (BindingPower){2, 1};
  case TOKEN_PLUS:
  case TOKEN_MINUS:
    return (BindingPower){10, 11};
  case TOKEN_STAR:
  case TOKEN_SLASH:
  case TOKEN_MOD:
    return (BindingPower){20, 21};
  case TOKEN_POWER:
    return (BindingPower){30, 31};
  default:
    return (BindingPower){0, 0};
  }
}

static int is_type_token(TokenType t) {
  return (t >= TOKEN_TYPE_INT && t <= TOKEN_TYPE_VOID) ||
         t == TOKEN_TYPE_BOOLEAN;
}

static void skip_to_next_param(Parser *p) {
  Parser_sync_param(p);
  if (p->current_token.type == TOKEN_COMMA)
    Parser_token_advance(p);
}

static AstNode *Parser_parse_expression(Parser *p, int min_bp);
static AstNode *Parser_parse_statement(Parser *p);
static TypeKind Parser_parse_type(Parser *p);

// ---------- POSTFIX HANDLERS ---------- //

static AstNode *Parser_parse_call(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume '('

  List args;
  List_init(&args);

  if (p->current_token.type != TOKEN_RPAREN) {
    while (1) {
      AstNode *arg = Parser_parse_expression(p, 0);
      if (!arg)
        return NULL;
      List_push(&args, arg);

      if (p->current_token.type == TOKEN_COMMA)
        Parser_token_advance(p);
      else
        break;
    }
  }

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after arguments"))
    return NULL;

  return AstNode_new_call(expr, args, loc);
}

static AstNode *Parser_parse_index(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume '['

  AstNode *index = Parser_parse_expression(p, 0);
  if (!index)
    return NULL;
  if (!Parser_expect(p, TOKEN_RBRACKET, "Expected ']'"))
    return NULL;

  return AstNode_new_index(expr, index, loc);
}

static AstNode *Parser_parse_field(Parser *p, AstNode *expr) {
  Parser_token_advance(p); // consume '.'

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT, "Expected field name after '.'"))
    return NULL;

  return AstNode_new_field(expr, ident.text, ident.loc);
}

static AstNode *Parser_parse_postfix_inc(Parser *p, AstNode *expr) {
  Token op = p->current_token;
  Parser_token_advance(p);

  UnaryOp unary_op = (op.type == TOKEN_PLUS_PLUS) ? OP_POST_INC : OP_POST_DEC;
  if (expr->tag != NODE_VAR) {
    ErrorHandler_report(p->eh, op.loc, "Operator '%s' requires an identifier",
                        unary_op == OP_POST_INC ? "++" : "--");
    return NULL;
  }

  return AstNode_new_unary(unary_op, expr, op.loc);
}

static AstNode *Parser_make_postfix(Parser *p, AstNode *expr) {
  switch (p->current_token.type) {
  case TOKEN_LPAREN:
    return Parser_parse_call(p, expr);
  case TOKEN_LBRACKET:
    return Parser_parse_index(p, expr);
  case TOKEN_DOT:
    return Parser_parse_field(p, expr);
  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS:
    return Parser_parse_postfix_inc(p, expr);
  default:
    return expr;
  }
}

// ---------- PARSER BODY ---------- //

static AstNode *Parser_parse_primary(Parser *p) {
  Token t = p->current_token;
  Parser_token_advance(p);

  switch (t.type) {
  case TOKEN_NUMBER:
    return AstNode_new_number(t.number, t.text, t.loc);
  case TOKEN_CHAR:
    return AstNode_new_char(t.text.data[0], t.loc);
  case TOKEN_STRING:
    return AstNode_new_string(t.text, t.loc);
  case TOKEN_IDENT:
    return AstNode_new_var(t.text, t.loc);

  case TOKEN_TRUE:
    return AstNode_new_bool(1, t.loc);
  case TOKEN_FALSE:
    return AstNode_new_bool(0, t.loc);

  case TOKEN_LPAREN: {
    Token peek = p->current_token;
    if (is_type_token(peek.type)) {
      TypeKind target_type = Parser_parse_type(p);
      Parser_expect(p, TOKEN_RPAREN, "Expected ')' after cast");
      AstNode *expr = Parser_parse_expression(p, 100);
      return AstNode_new_cast_expr(expr, target_type, t.loc);
    } else {
      AstNode *expr = Parser_parse_expression(p, 0);
      Parser_expect(p, TOKEN_RPAREN, "Expected ')'");
      return expr;
    }
  }

  case TOKEN_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_NEG, expr, t.loc);
  }

  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    UnaryOp op = (t.type == TOKEN_PLUS_PLUS) ? OP_PRE_INC : OP_PRE_DEC;
    if (!Parser_check(expr, NODE_VAR)) {
      ErrorHandler_report(p->eh, t.loc, "Operator requires an identifier");
      return NULL;
    }
    return AstNode_new_unary(op, expr, t.loc);
  }

  default:
    ErrorHandler_report(p->eh, t.loc, "Unexpected token in expression");
    return NULL;
  }
}

static AstNode *Parser_parse_expression(Parser *p, int min_bp) {
  AstNode *left = Parser_parse_primary(p);
  if (!left)
    return NULL;

  while (1) {
    Token op = p->current_token;

    if (is_postfix(op)) {
      if (100 < min_bp)
        break;
      left = Parser_make_postfix(p, left);
      continue;
    }

    if (!is_binary_operator(op.type))
      break;
    BindingPower bp = infix_binding_power(op.type);
    if (bp.left_bp < min_bp)
      break;

    Parser_token_advance(p);
    AstNode *right = Parser_parse_expression(p, bp.right_bp);
    if (!right)
      return NULL;

    if (op.type == TOKEN_ASSIGN) {
      if (!Parser_check(left, NODE_VAR)) {
        ErrorHandler_report(p->eh, op.loc, "Invalid assignment target");
        return NULL;
      }
      if (Parser_check(right, NODE_ASSIGN_EXPR)) {
        ErrorHandler_report(p->eh, op.loc, "Chained assignment not supported");
        return NULL;
      }
      left = AstNode_new_assign_expr(left, right, op.loc);
    } else {
      left = AstNode_new_binary(left, right, token_to_op(op.type), op.loc);
    }
  }

  return left;
}

static TypeKind Parser_parse_type(Parser *p) {
  TypeKind kind = Token_token_to_type(p->current_token.type);
  if (kind != TYPE_UNKNOWN)
    Parser_token_advance(p);
  return kind;
}

static AstNode *Parser_parse_var_decl(Parser *p) {
  int is_const = (p->current_token.type == TOKEN_CONST);
  Parser_token_advance(p);

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT,
                     "Expected identifier after 'let' or 'const'"))
    return NULL;

  AstNode *name = AstNode_new_var(ident.text, ident.loc);

  TypeKind declared_type = TYPE_UNKNOWN;
  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p);
    declared_type = Parser_parse_type(p);
    if (declared_type == TYPE_UNKNOWN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected type after ':'");
      return NULL;
    }
  }

  if (!Parser_expect(p, TOKEN_ASSIGN, "Expected '=' after identifier/type"))
    return NULL;
  AstNode *value = Parser_parse_expression(p, 0);
  if (!value)
    return NULL;
  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after value"))
    return NULL;

  return AstNode_new_var_decl(name, value, declared_type, is_const, ident.loc);
}

static AstNode *Parser_parse_print(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p);

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after print"))
    return NULL;

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

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after print"))
    return NULL;
  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after print statement"))
    return NULL;

  return AstNode_new_print_stmt(values, loc);
}

static AstNode *Parser_parse_fn_decl(Parser *p) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'fn'

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT, "Expected function name"))
    return NULL;

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after function name"))
    return NULL;

  List params;
  List_init(&params);

  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF) {
    Location loc = p->current_token.loc;

    StringView param_name = p->current_token.text;
    if (!Parser_expect(p, TOKEN_IDENT, "Expected parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_COLON, "Expected ':' after parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    TypeKind param_type = Parser_parse_type(p);
    if (param_type == TYPE_UNKNOWN) {
      skip_to_next_param(p);
      continue;
    }

    List_push(&params, AstNode_new_param(param_name, param_type, loc));
    if (p->current_token.type == TOKEN_COMMA)
      Parser_token_advance(p);
  }

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after parameters"))
    return NULL;

  TypeKind ret_type = TYPE_UNKNOWN;
  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p);
    ret_type = Parser_parse_type(p);
    if (ret_type == TYPE_UNKNOWN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected return type after ':'");
      return NULL;
    }
  }

  AstNode *body = NULL;
  if (p->current_token.type == TOKEN_LBRACE) {
    Parser_token_advance(p);
    body = AstNode_new_program(loc);
    while (p->current_token.type != TOKEN_RBRACE &&
           p->current_token.type != TOKEN_EOF) {
      AstNode *stmt = Parser_parse_statement(p);
      if (!stmt) {
        Parser_sync(p);
        continue;
      }
      List_push(&body->program.children, stmt);
    }
    if (!Parser_expect(p, TOKEN_RBRACE, "Expected '}' after function body"))
      return NULL;
  } else if (p->current_token.type == TOKEN_SEMI) {
    Parser_token_advance(p);
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected '{' or ';' after function declaration");
    Parser_sync(p);
  }

  return AstNode_new_func_decl(ident.text, params, ret_type, body, loc);
}

static AstNode *Parser_parse_statement(Parser *p) {
  switch (p->current_token.type) {
  case TOKEN_LET:
  case TOKEN_CONST:
    return Parser_parse_var_decl(p);
  case TOKEN_FN:
    return Parser_parse_fn_decl(p);
  case TOKEN_PRINT:
    return Parser_parse_print(p);
  case TOKEN_RETURN: {
    Parser_token_advance(p);
    Location loc = p->current_token.loc;
    AstNode *expr = Parser_parse_expression(p, 0);
    if (Parser_expect(p, TOKEN_SEMI, "Expected ';' after return statement"))
      return AstNode_new_return(expr, loc);

    Parser_sync(p);
    return NULL;
  }
  case TOKEN_EOF:
    return NULL;
  default: {
    Location loc = p->current_token.loc;
    AstNode *expr = Parser_parse_expression(p, 0);
    if (Parser_expect(p, TOKEN_SEMI, "Expected ';' after expression"))
      return AstNode_new_expr_stmt(expr, loc);

    Parser_sync(p);
    return NULL;
  }
  }
}

AstNode *Parser_process(Lexer *lexer, ErrorHandler *eh) {
  Parser p;
  p.lexer = lexer;
  p.eh = eh;
  p.current_token = Token_advance(lexer);

  AstNode *program = AstNode_new_program(p.current_token.loc);

  while (p.current_token.type != TOKEN_EOF) {
    AstNode *stmt = Parser_parse_statement(&p);
    if (!stmt)
      continue;
    List_push(&program->program.children, stmt);
  }

  return program;
}
