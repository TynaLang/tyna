#include "parser.h"
#include "lexer.h"
#include "utils.h"
#include <stdio.h>
#include <unistd.h>

static Token *Parser_token_advance(Parser *p) {
  Token *prev_token = &p->current_token;
  printf("Consumed token: \'%s\'\n", prev_token->text);
  p->current_token = Token_advance(p->lexer);
  return prev_token;
}

static Token Parser_expect(Parser *p, TokenType type, const char *msg) {
  Token t = p->current_token;

  if (t.type != type) {
    fprintf(stderr, "%s at line %zu col %zu\n", msg, t.line, t.col);
    exit(1); // for now, hard fail is fine
  }

  Parser_token_advance(p);
  return t;
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

  printf("Progressing to token: \'%s\'\n", t.text);
  AstNode *left;

  switch (t.type) {
  case TOKEN_NUMBER:
    left = AstNode_new_number(t.number);
    break;

  case TOKEN_IDENT:
    left = AstNode_new_ident(t.text);
    break;

  case TOKEN_CHAR:
    left = AstNode_new_char(t.text[0]);
    break;

  case TOKEN_STRING:
    left = AstNode_new_string(t.text);
    break;

  case TOKEN_LPAREN: {
    left = Parser_parse_expression(p, 0);
    Parser_expect(p, TOKEN_RPAREN, "Expected ')'");
    break;
  }
  case TOKEN_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    left = AstNode_new_unary(OP_NEG, expr);
    break;
  }
  case TOKEN_PLUS_PLUS: {
    AstNode *expr = Parser_parse_expression(p, 100);

    if (expr->tag != NODE_IDENT) {
      fprintf(stderr, "++ requires an identifier\n");
      exit(1);
    }

    left = AstNode_new_unary(OP_PRE_INC, expr);
    break;
  }
  case TOKEN_MINUS_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);

    if (expr->tag != NODE_IDENT) {
      fprintf(stderr, "-- requires an identifier\n");
      exit(1);
    }

    left = AstNode_new_unary(OP_PRE_DEC, expr);
    break;
  }
  default:
    fprintf(stderr,
            "Unexpected token in expression got %d at line  %zu col %zu\n",
            t.type, t.line, t.col);
    exit(1);
  }

  while (1) {
    Token op = p->current_token;

    if (op.type == TOKEN_PLUS_PLUS) {
      Parser_token_advance(p);

      if (left->tag != NODE_IDENT) {
        fprintf(stderr, "postfix ++ requires identifier\n");
        exit(1);
      }

      left = AstNode_new_unary(OP_POST_INC, left);
      continue;
    }

    if (op.type == TOKEN_MINUS_MINUS) {
      Parser_token_advance(p);

      if (left->tag != NODE_IDENT) {
        fprintf(stderr, "postfix -- requires identifier\n");
        exit(1);
      }

      left = AstNode_new_unary(OP_POST_DEC, left);
      continue;
    }

    if (!is_binary_operator(op.type)) {
      break;
    }

    BindingPower bp = infix_binding_power(op.type);

    if (bp.left_bp < min_bp)
      break;

    Parser_token_advance(p);

    AstNode *right = Parser_parse_expression(p, bp.right_bp);

    if (op.type == TOKEN_ASSIGN) {
      if (left->tag != NODE_IDENT) {
        fprintf(stderr, "Invalid assignment target\n");
        exit(1);
      }
      left = AstNode_new_assign(left, right);
    } else {
      left = AstNode_new_binary(left, right, token_to_op(op.type));
    }
  }

  return left;
}

static AstNode *Parser_build_let_statement(Parser *p) {
  printf("building let statement\n");
  Parser_expect(p, TOKEN_LET, "Expected 'let'");

  Token ident =
      Parser_expect(p, TOKEN_IDENT, "Expected identifier after 'let'");
  AstNode *name = AstNode_new_ident(ident.text);

  Parser_expect(p, TOKEN_COLON, "Expected ':' after identifier");

  printf("building let expression\n");

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
    fprintf(stderr, "Expected type after ':' at line %zu col %zu\n",
            type_tok.line, type_tok.col);
    exit(1);
  }

  Parser_token_advance(p);

  Parser_expect(p, TOKEN_ASSIGN, "Expected '=' after type");

  AstNode *value = Parser_parse_expression(p, 0);
  if (!value)
    return NULL;

  Parser_expect(p, TOKEN_SEMI, "Expected ';' after value");

  return AstNode_new_let(name, value, declared_type);
}

static AstNode *Parser_build_print_statement(Parser *p) {
  Parser_expect(p, TOKEN_PRINT, "Expected 'print'");
  Parser_expect(p, TOKEN_LPAREN, "Expected '(' after print");

  AstNode *value = Parser_parse_expression(p, 0);
  if (!value)
    return NULL;

  Parser_expect(p, TOKEN_RPAREN, "Expected ')' after value");
  Parser_expect(p, TOKEN_SEMI, "Expected ';' after value");

  return AstNode_new_print(value);
}

static AstNode *Parser_parse_statement(Parser *p) {
  while (p->current_token.type == TOKEN_SEMI) {
    Parser_token_advance(p);
  }

  Token current_token = p->current_token;

  switch (current_token.type) {
  case TOKEN_LET:
    return Parser_build_let_statement(p);
  case TOKEN_PRINT:
    return Parser_build_print_statement(p);
  default: {
    AstNode *expr = Parser_parse_expression(p, 0);
    Parser_expect(p, TOKEN_SEMI, "Expected ';' after expression");
    return AstNode_new_expr_stmt(expr);
  }
  }
}

// ---------- PARSER BODY ------------ //

AstNode *Parser_process(Lexer *l) {
  Parser p;
  p.lexer = l;
  p.current_token = Token_advance(l);

  AstNode *program = AstNode_new_program();

  while (p.current_token.type != TOKEN_EOF) {
    AstNode *node = Parser_parse_statement(&p);
    if (!node) {
      fprintf(stderr, "Parse error at line %zu col %zu\n", p.current_token.line,
              p.current_token.col);
      break;
    }
    List_push(&program->program.children, node);
  }

  return program;
}
