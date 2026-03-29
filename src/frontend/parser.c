#include <stdio.h>
#include <unistd.h>

#include "lexer.h"
#include "parser.h"
#include "utils.h"

static Token *Parser_token_advance(Parser *p) {
  Token *prev_token = &p->current_token;
  p->current_token = Token_advance(p->lexer);
  return prev_token;
}

Token Parser_token_future_peek(Lexer *lexer, int n) {
  size_t saved_cursor = lexer->cursor;
  Location saved_loc = lexer->loc;

  Token t;
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
static TypeKind Parser_parse_type(Parser *p);

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

static int is_type_token(TokenType t) {
  return (t >= TOKEN_TYPE_INT && t <= TOKEN_TYPE_F64);
}

static AstNode *Parser_parse_expression(Parser *p, int min_bp) {
  Token t = p->current_token;
  Parser_token_advance(p);

  AstNode *left = NULL;

  switch (t.type) {
  case TOKEN_NUMBER:
    left = AstNode_new_number(t.number, t.text, t.loc);
    break;

  case TOKEN_IDENT:
    left = AstNode_new_var(t.text, t.loc);
    break;

  case TOKEN_CHAR:
    left = AstNode_new_char(t.text.data[0], t.loc);
    break;

  case TOKEN_STRING:
    left = AstNode_new_string(t.text, t.loc);
    break;

  case TOKEN_ERROR:
    ErrorHandler_report(p->eh, t.loc, "%s", t.text);
    return NULL;

  case TOKEN_LPAREN: {
    Token peek = p->current_token; // We already advanced past '('

    if (is_type_token(peek.type)) {
      TypeKind target_type = Parser_parse_type(p);

      Parser_expect(p, TOKEN_RPAREN, "Expected ')' after cast");

      AstNode *expr = Parser_parse_expression(p, 100);
      left = AstNode_new_cast_expr(expr, target_type, t.loc);
    } else {
      // Parenthesized expression
      left = Parser_parse_expression(p, 0);
      Parser_expect(p, TOKEN_RPAREN, "Expected ')'");
    }
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

    if (!Parser_check(expr, NODE_VAR)) {
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

      if (!Parser_check(left, NODE_VAR)) {
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

      if (!Parser_check(left, NODE_VAR)) {
        printf("Left-hand side of assignment must be an identifier, got %d ",
               left->tag);
        ErrorHandler_report(p->eh, op.loc, "Invalid assignment target");
        return NULL;
      }

      if (Parser_check(right, NODE_ASSIGN_EXPR)) {
        ErrorHandler_report(p->eh, op.loc,
                            "Chained assignment is not supported");
        return NULL;
      }

      left = AstNode_new_assign_expr(left, right, op.loc);
    }

    else {
      left = AstNode_new_binary(left, right, token_to_op(op.type), op.loc);
    }
  }

  return left;
}

static TypeKind Parser_parse_type(Parser *p) {
  Token type_tok = p->current_token;
  TypeKind kind = Token_token_to_type(type_tok.type);

  if (kind != TYPE_UNKNOWN) {
    Parser_token_advance(p);
  }
  return kind;
}

static AstNode *Parser_build_var_decl_statement(Parser *p) {
  int is_const = 0;
  if (p->current_token.type == TOKEN_CONST) {
    is_const = 1;
    Parser_token_advance(p);
  } else if (p->current_token.type == TOKEN_LET) {
    Parser_token_advance(p);
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected 'let' or 'const'");
    return NULL;
  }

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT,
                     "Expected identifier after 'let' or 'const'"))
    return NULL;

  AstNode *name = AstNode_new_var(ident.text, ident.loc);

  TypeKind declared_type = TYPE_UNKNOWN;

  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p); // consume ':'
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

static AstNode *Parser_build_print_stmt_statement(Parser *p) {
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

  return AstNode_new_print_stmt(value, p->current_token.loc);
}

static AstNode *Parser_parse_statement(Parser *p) {
  AstNode *node = NULL;

  switch (p->current_token.type) {
  case TOKEN_LET:
  case TOKEN_CONST:
    node = Parser_build_var_decl_statement(p);
    break;

  case TOKEN_PRINT:
    node = Parser_build_print_stmt_statement(p);
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
