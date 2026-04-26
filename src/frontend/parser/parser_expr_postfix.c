#include "parser_internal.h"

static int parser_is(AstNode *node, AstKind kind) {
  return node && node->tag == kind;
}

static List parser_parse_generic_type_args(Parser *p) {
  List generic_args;
  List_init(&generic_args);

  if (!parser_expect(p, TOKEN_LT, "Expected '<' to start generic arguments"))
    return generic_args;

  while (true) {
    Type *arg_type = parser_parse_type_full(p);
    if (!arg_type) {
      List_free(&generic_args, 0);
      return generic_args;
    }
    List_push(&generic_args, arg_type);

    if (p->current_token.type == TOKEN_COMMA) {
      parser_token_advance(p);
      continue;
    }
    break;
  }

  if (!parser_expect(p, TOKEN_GT, "Expected '>' after generic arguments")) {
    List_free(&generic_args, 0);
    return generic_args;
  }

  return generic_args;
}

static AstNode *parser_parse_call(Parser *p, AstNode *expr, List generic_args) {
  Location loc = p->current_token.loc;
  parser_token_advance(p);
  List args;
  List_init(&args);

  if (p->current_token.type != TOKEN_RPAREN) {
    while (1) {
      AstNode *arg = parser_parse_expression(p, 0);
      if (!arg)
        return NULL;
      List_push(&args, arg);
      if (p->current_token.type == TOKEN_COMMA)
        parser_token_advance(p);
      else
        break;
    }
  }

  if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after arguments"))
    return NULL;

  AstNode *node = AstNode_new_call(expr, args, loc);
  node->call.generic_args = generic_args;
  return node;
}

static AstNode *parser_parse_index(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  parser_token_advance(p);
  AstNode *index = parser_parse_expression(p, 0);
  if (!index)
    return NULL;
  if (!parser_expect(p, TOKEN_RBRACKET, "Expected ']' after index"))
    return NULL;

  return AstNode_new_index(expr, index, loc);
}

static bool parser_is_identifier_like(TokenType type) {
  return type == TOKEN_IDENT || type == TOKEN_PRINT || type == TOKEN_FN ||
         type == TOKEN_RETURN || type == TOKEN_IF || type == TOKEN_ELSE ||
         type == TOKEN_IMPORT || type == TOKEN_EXTERNAL ||
         type == TOKEN_ERROR_KEYWORD || type == TOKEN_ERRORS ||
         type == TOKEN_IS || type == TOKEN_NEW || type == TOKEN_DEFER ||
         type == TOKEN_STRUCT || type == TOKEN_UNION || type == TOKEN_SWITCH ||
         type == TOKEN_CASE || type == TOKEN_FOR || type == TOKEN_WHILE ||
         type == TOKEN_LOOP || type == TOKEN_IN || type == TOKEN_BREAK ||
         type == TOKEN_CONTINUE || type == TOKEN_FROZEN ||
         type == TOKEN_STATIC || type == TOKEN_IMPL || type == TOKEN_TYPE ||
         type == TOKEN_TYPE_INT ||
         type == TOKEN_TYPE_STR || type == TOKEN_TYPE_BOOLEAN ||
         type == TOKEN_TYPE_CHAR || type == TOKEN_TYPE_FLOAT ||
         type == TOKEN_TYPE_I8 || type == TOKEN_TYPE_I16 ||
         type == TOKEN_TYPE_I32 || type == TOKEN_TYPE_I64 ||
         type == TOKEN_TYPE_U8 || type == TOKEN_TYPE_U16 ||
         type == TOKEN_TYPE_U32 || type == TOKEN_TYPE_U64 ||
         type == TOKEN_TYPE_F32 || type == TOKEN_TYPE_F64 ||
         type == TOKEN_TYPE_VOID || type == TOKEN_TRUE || type == TOKEN_FALSE ||
         type == TOKEN_NULL;
}

static AstNode *parser_parse_field(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  parser_token_advance(p); // consume '.'

  if (!parser_is_identifier_like(p->current_token.type)) {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected identifier after '.'");
    return NULL;
  }

  StringView field_name = p->current_token.text;
  parser_token_advance(p);

  return AstNode_new_field(expr, field_name, loc);
}

static AstNode *parser_parse_postfix_inc(Parser *p, AstNode *expr) {
  Token op = p->current_token;
  parser_token_advance(p);
  UnaryOp unary_op = (op.type == TOKEN_PLUS_PLUS) ? OP_POST_INC : OP_POST_DEC;
  if (!parser_is(expr, NODE_VAR)) {
    ErrorHandler_report(p->eh, op.loc, "Operator '%s' requires an identifier",
                        unary_op == OP_POST_INC ? "++" : "--");
    return NULL;
  }
  return AstNode_new_unary(unary_op, expr, op.loc);
}

AstNode *parser_make_postfix(Parser *p, AstNode *expr) {
  switch (p->current_token.type) {
  case TOKEN_LPAREN:
    return parser_parse_call(p, expr, (List){0});
  case TOKEN_LT:
    if (parser_is_generic_call_start(p)) {
      List generic_args = parser_parse_generic_type_args(p);
      return parser_parse_call(p, expr, generic_args);
    }
    return expr;
  case TOKEN_LBRACKET:
    return parser_parse_index(p, expr);
  case TOKEN_DOT:
    return parser_parse_field(p, expr);
  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS:
    return parser_parse_postfix_inc(p, expr);
  default:
    return expr;
  }
}
