#include "parser.h"
#include "lexer.h"
#include "utils.h"
#include <stdio.h>
#include <unistd.h>

static Token *Parser_token_advance(Parser *p) {
  Token *prev_token = &p->current_token;
  p->current_token = Token_advance(p->lexer);
  return prev_token;
}

static int Parser_expect(Parser *p, TokenType type, const char *msg) {
  if (p->current_token.type != type) {
    ErrorHandler_report(p->eh, p->current_token.loc, "%s", msg);
    return 0;
  }

  Parser_token_advance(p);
  return 1;
}

static int Parser_check(AstNode *p, TokenType type) {
  if (!p)
    return 0;
  if (p->tag == type)
    return 1;
}

static void Parser_sync(Parser *p) {
  while (p->current_token.type != TOKEN_EOF) {
    if (p->current_token.type == TOKEN_SEMI) {
      Parser_token_advance(p);
      return;
    }

    switch (p->current_token.type) {
    case TOKEN_LET:
    case TOKEN_PRINT:
      return; // start of new statement
    default:
      Parser_token_advance(p);
    }
  }
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
  default:
    fprintf(stderr, "Undefined behaviour, this should not happen");
    exit(1); // for now, hard fail is fine
  }
}

static int is_binary_operator(TokenType t) {
  switch (t) {
  case TOKEN_ASSIGN:
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_STAR:
  case TOKEN_SLASH:
  case TOKEN_POWER:
    return 1;
  default:
    return 0;
  }
}

// ---------- PARSER BODY ------------ //
static AstNode *Parser_parse_expression(Parser *p, int min_bp);

static BindingPower infix_binding_power(TokenType t) {
  switch (t) {
  case TOKEN_ASSIGN:
    return (BindingPower){2, 1};
  case TOKEN_PLUS:
  case TOKEN_MINUS:
    return (BindingPower){10, 11};

  case TOKEN_STAR:
  case TOKEN_SLASH:
    return (BindingPower){20, 21};
  case TOKEN_POWER:
    return (BindingPower){30, 31};
  default:
    return (BindingPower){0, 0};
  }
}

static AstNode *Parser_parse_expression(Parser *p, int min_bp) {
  Token t = p->current_token;
  Parser_token_advance(p);

  AstNode *left = NULL;

  switch (t.type) {
  case TOKEN_NUMBER:
    left = AstNode_new_number(t.number, t.loc);
    break;

  case TOKEN_IDENT:
    left = AstNode_new_ident(t.text, t.loc);
    break;

  case TOKEN_CHAR:
    left = AstNode_new_char(t.text[0], t.loc);
    break;

  case TOKEN_STRING:
    left = AstNode_new_string(t.text, t.loc);
    break;

  case TOKEN_ERROR:
    ErrorHandler_report(p->eh, t.loc, "%s", t.text);
    return NULL;

  case TOKEN_LPAREN: {
    left = Parser_parse_expression(p, 0);

    if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')'"))
      return NULL;

    break;
  }

  case TOKEN_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    if (!expr)
      return NULL;

    left = AstNode_new_unary(OP_NEG, expr, t.loc);
    break;
  }

  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    if (!expr)
      return NULL;

    const char *op_str = (t.type == TOKEN_PLUS_PLUS) ? "++" : "--";
    UnaryOp op = (t.type == TOKEN_PLUS_PLUS) ? OP_PRE_INC : OP_PRE_DEC;

    if (!Parser_check(expr, NODE_IDENT)) {
      ErrorHandler_report(p->eh, t.loc, "Operator '%s' requires an identifier",
                          op_str);
      return NULL;
    }

    left = AstNode_new_unary(op, expr, t.loc);
    break;
  }

  default:
    ErrorHandler_report(p->eh, t.loc, "Unexpected token in expression");
    return NULL;
  }

  while (1) {
    Token op = p->current_token;

    if (op.type == TOKEN_PLUS_PLUS || op.type == TOKEN_MINUS_MINUS) {
      int POSTFIX_BP = 100;

      if (POSTFIX_BP < min_bp)
        break;

      Parser_token_advance(p);

      const char *op_str = (op.type == TOKEN_PLUS_PLUS) ? "++" : "--";
      UnaryOp unary_op =
          (op.type == TOKEN_PLUS_PLUS) ? OP_POST_INC : OP_POST_DEC;

      if (!Parser_check(left, NODE_IDENT)) {
        ErrorHandler_report(p->eh, op.loc,
                            "Operator '%s' requires an identifier", op_str);
        return NULL;
      }

      left = AstNode_new_unary(unary_op, left, op.loc);
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

      if (!Parser_check(left, NODE_IDENT)) {
        printf("Left-hand side of assignment must be an identifier, got %d ",
               left->tag);
        ErrorHandler_report(p->eh, op.loc, "Invalid assignment target");
        return NULL;
      }

      if (Parser_check(right, NODE_ASSIGN)) {
        ErrorHandler_report(p->eh, op.loc,
                            "Chained assignment is not supported");
        return NULL;
      }

      left = AstNode_new_assign(left, right, op.loc);
    }

    else {
      left = AstNode_new_binary(left, right, token_to_op(op.type), op.loc);
    }
  }

  return left;
}

static AstNode *Parser_build_let_statement(Parser *p) {
  if (!Parser_expect(p, TOKEN_LET, "Expected 'let'"))
    return NULL;

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT, "Expected identifier after 'let'"))
    return NULL;

  AstNode *name = AstNode_new_ident(ident.text, ident.loc);

  if (!Parser_expect(p, TOKEN_COLON, "Expected ':' after identifier"))
    return NULL;

  Token type_tok = p->current_token;
  TypeKind declared_type = TYPE_UNKNOWN;

  switch (type_tok.type) {
  case TOKEN_TYPE_INT:
    declared_type = TYPE_INT;
    break;
  case TOKEN_TYPE_CHAR:
    declared_type = TYPE_CHAR;
    break;
  case TOKEN_TYPE_STR:
    declared_type = TYPE_STRING;
    break;
  default:
    ErrorHandler_report(p->eh, type_tok.loc, "Expected type after ':'");
    return NULL;
  }

  Parser_token_advance(p);

  if (!Parser_expect(p, TOKEN_ASSIGN, "Expected '=' after type"))
    return NULL;

  AstNode *value = Parser_parse_expression(p, 0);
  if (!value)
    return NULL;

  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after value"))
    return NULL;

  return AstNode_new_let(name, value, declared_type, ident.loc);
}

static AstNode *Parser_build_print_statement(Parser *p) {
  if (!Parser_expect(p, TOKEN_PRINT, "Expected 'print'"))
    return NULL;
  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after print"))
    return NULL;

  AstNode *value = Parser_parse_expression(p, 0);
  if (!value)
    return NULL;

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after value"))
    return NULL;
  if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after value"))
    return NULL;

  return AstNode_new_print(value, p->current_token.loc);
}

static AstNode *Parser_parse_statement(Parser *p) {
  AstNode *node = NULL;

  switch (p->current_token.type) {
  case TOKEN_LET:
    node = Parser_build_let_statement(p);
    break;

  case TOKEN_PRINT:
    node = Parser_build_print_statement(p);
    break;

  case TOKEN_EOF:
    return NULL;

  default: {
    AstNode *expr = Parser_parse_expression(p, 0);

    if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after expression")) {
      Parser_sync(p);
      return NULL;
    }

    node = AstNode_new_expr_stmt(expr, p->current_token.loc);
  }
  }

  if (!node) {
    Parser_sync(p);
  }

  return node;
}

// ---------- PARSER BODY ------------ //

AstNode *Parser_process(Lexer *l, ErrorHandler *eh) {
  Parser p;
  p.lexer = l;
  p.current_token = Token_advance(l);
  p.eh = eh;

  AstNode *program = AstNode_new_program(p.current_token.loc);

  while (p.current_token.type != TOKEN_EOF) {
    AstNode *node = Parser_parse_statement(&p);
    if (!node) {
      continue;
    }
    List_push(&program->program.children, node);
  }

  return program;
}
