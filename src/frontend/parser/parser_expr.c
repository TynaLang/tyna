#include <stdio.h>
#include <stdlib.h>

#include "parser_internal.h"

static int is_binary_operator(TokenType t) {
  switch (t) {
  case TOKEN_ASSIGN:
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_STAR:
  case TOKEN_SLASH:
  case TOKEN_POWER:
  case TOKEN_MOD:
  case TOKEN_LT:
  case TOKEN_GT:
  case TOKEN_LE:
  case TOKEN_GE:
  case TOKEN_EQ:
  case TOKEN_NE:
  case TOKEN_AND:
  case TOKEN_OR:
  case TOKEN_QUESTION:
  case TOKEN_IS:
  case TOKEN_ELSE:
    return 1;
  default:
    return 0;
  }
}

bool parser_is_generic_call_start(Parser *p) {
  if (p->current_token.type != TOKEN_LT)
    return false;

  int depth = 0;
  int lookahead = 1;
  while (true) {
    Token t = parser_token_peek(p->lexer, lookahead++);
    if (t.type == TOKEN_EOF)
      return false;
    if (t.type == TOKEN_LT) {
      depth++;
    } else if (t.type == TOKEN_GT) {
      if (depth == 0)
        break;
      depth--;
    }
  }

  Token next = parser_token_peek(p->lexer, lookahead);
  return next.type == TOKEN_LPAREN;
}

static int parser_is_postfix(Parser *p) {
  Token t = p->current_token;
  if (t.type == TOKEN_LPAREN || t.type == TOKEN_LBRACKET ||
      t.type == TOKEN_DOT || t.type == TOKEN_PLUS_PLUS ||
      t.type == TOKEN_MINUS_MINUS)
    return 1;
  if (t.type == TOKEN_LT)
    return parser_is_generic_call_start(p);
  return 0;
}

static ArithmOp token_to_arithm_op(TokenType type) {
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
    fprintf(stderr, "Unexpected operator in token_to_arithm_op\n");
    exit(1);
  }
}

static CompareOp token_to_compare_op(TokenType type) {
  switch (type) {
  case TOKEN_LT:
    return OP_LT;
  case TOKEN_GT:
    return OP_GT;
  case TOKEN_LE:
    return OP_LE;
  case TOKEN_GE:
    return OP_GE;
  default:
    fprintf(stderr, "Unexpected operator in token_to_compare_op\n");
    exit(1);
  }
}

static EqualityOp token_to_equality_op(TokenType type) {
  switch (type) {
  case TOKEN_EQ:
    return OP_EQ;
  case TOKEN_NE:
    return OP_NE;
  default:
    fprintf(stderr, "Unexpected operator in token_to_equality_op\n");
    exit(1);
  }
}

static LogicalOp token_to_logical_op(TokenType type) {
  switch (type) {
  case TOKEN_AND:
    return OP_AND;
  case TOKEN_OR:
    return OP_OR;
  default:
    fprintf(stderr, "Unexpected operator in token_to_logical_op\n");
    exit(1);
  }
}

static BindingPower infix_binding_power(TokenType t) {
  switch (t) {
  case TOKEN_ASSIGN:
    return (struct BindingPower){2, 1};
  case TOKEN_QUESTION:
    return (struct BindingPower){4, 3};

  case TOKEN_OR:
    return (struct BindingPower){5, 6};
  case TOKEN_AND:
    return (struct BindingPower){7, 8};

  case TOKEN_EQ:
  case TOKEN_NE:
  case TOKEN_IS:
    return (struct BindingPower){9, 10};
  case TOKEN_LT:
  case TOKEN_GT:
  case TOKEN_LE:
  case TOKEN_GE:
    return (struct BindingPower){11, 12};

  case TOKEN_ELSE:
    return (struct BindingPower){3, 4};

  case TOKEN_PLUS:
  case TOKEN_MINUS:
    return (struct BindingPower){13, 14};
  case TOKEN_STAR:
  case TOKEN_SLASH:
  case TOKEN_MOD:
    return (struct BindingPower){15, 16};
  case TOKEN_POWER:
    return (struct BindingPower){20, 19};

  default:
    return (struct BindingPower){0, 0};
  }
}

static int is_lvalue(AstNode *node) {
  return node->tag == NODE_VAR || node->tag == NODE_FIELD ||
         node->tag == NODE_INDEX ||
         (node->tag == NODE_UNARY && node->unary.op == OP_DEREF);
}

static bool token_starts_expression(TokenType type) {
  switch (type) {
  case TOKEN_IDENT:
  case TOKEN_NUMBER:
  case TOKEN_CHAR:
  case TOKEN_STRING:
  case TOKEN_TRUE:
  case TOKEN_FALSE:
  case TOKEN_NULL:
  case TOKEN_NEW:
  case TOKEN_LPAREN:
  case TOKEN_LBRACKET:
  case TOKEN_ERROR_KEYWORD:
    return true;
  default:
    return false;
  }
}

AstNode *parser_parse_expression(Parser *p, int min_bp) {
  AstNode *left = parser_parse_primary(p);
  if (!left)
    return NULL;

  while (1) {
    Token op = p->current_token;

    if (parser_is_postfix(p)) {
      if (100 < min_bp)
        break;
      left = parser_make_postfix(p, left);
      continue;
    }

    if (!is_binary_operator(op.type))
      break;

    struct BindingPower bp = infix_binding_power(op.type);
    if (bp.left_bp < min_bp)
      break;

    parser_token_advance(p);

    if (op.type == TOKEN_QUESTION) {
      if (!token_starts_expression(p->current_token.type)) {
        left = AstNode_new_binary_else(left, NULL, op.loc);
        continue;
      }
      AstNode *true_expr = parser_parse_expression(p, 0);
      if (!true_expr)
        return NULL;
      if (!parser_expect(p, TOKEN_COLON, "Expected ':' in ternary operator"))
        return NULL;
      AstNode *false_expr = parser_parse_expression(p, bp.right_bp);
      if (!false_expr)
        return NULL;
      left = AstNode_new_ternary(left, true_expr, false_expr, op.loc);
      continue;
    }

    AstNode *right = NULL;
    if (op.type == TOKEN_ELSE && p->current_token.type == TOKEN_RETURN) {
      right = parser_parse_return_stmt(p, false);
    } else if (op.type == TOKEN_ELSE && p->current_token.type == TOKEN_LBRACE) {
      right = parser_parse_block(p);
    } else {
      right = parser_parse_expression(p, bp.right_bp);
    }
    if (!right)
      return NULL;

    switch (op.type) {
    case TOKEN_ASSIGN:
      if (!is_lvalue(left)) {
        ErrorHandler_report(p->eh, op.loc, "Invalid assignment target");
        return NULL;
      }
      left = AstNode_new_assign_expr(left, right, op.loc);
      break;

    case TOKEN_PLUS:
    case TOKEN_MINUS:
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_MOD:
    case TOKEN_POWER:
      left = AstNode_new_binary_arith(left, right, token_to_arithm_op(op.type),
                                      op.loc);
      break;

    case TOKEN_LT:
    case TOKEN_GT:
    case TOKEN_LE:
    case TOKEN_GE:
      left = AstNode_new_binary_compare(left, right,
                                        token_to_compare_op(op.type), op.loc);
      break;

    case TOKEN_EQ:
    case TOKEN_NE:
      left = AstNode_new_binary_equality(left, right,
                                         token_to_equality_op(op.type), op.loc);
      break;

    case TOKEN_AND:
    case TOKEN_OR:
      left = AstNode_new_binary_logical(left, right,
                                        token_to_logical_op(op.type), op.loc);
      break;

    case TOKEN_IS:
      left = AstNode_new_binary_is(left, right, op.loc);
      break;

    case TOKEN_ELSE:
      left = AstNode_new_binary_else(left, right, op.loc);
      break;

    default:
      fprintf(stderr, "Unexpected binary operator\n");
      exit(1);
    }
  }

  return left;
}
