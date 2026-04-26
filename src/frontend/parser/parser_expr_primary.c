#include "parser_internal.h"

static int parser_is(AstNode *node, AstKind kind) {
  return node && node->tag == kind;
}

static int is_type_token(TokenType t) {
  return (t >= TOKEN_TYPE_INT && t <= TOKEN_TYPE_VOID) || t == TOKEN_LBRACKET ||
         t == TOKEN_STAR || t == TOKEN_CONST;
}

static AstNode *parser_parse_new_expr(Parser *p) {
  Location loc = p->current_token.loc;

  Type *target_type = parser_parse_type_full(p);
  if (!target_type)
    return NULL;

  List args;
  List_init(&args);
  List field_inits;
  List_init(&field_inits);

  if (p->current_token.type == TOKEN_LPAREN) {
    parser_token_advance(p);
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
    if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after constructor args"))
      return NULL;
  } else if (p->current_token.type == TOKEN_LBRACE) {
    parser_token_advance(p);
    while (p->current_token.type != TOKEN_RBRACE &&
           p->current_token.type != TOKEN_EOF) {
      if (p->current_token.type != TOKEN_IDENT) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Expected field name in struct literal");
        return NULL;
      }
      StringView field_name = p->current_token.text;
      Location field_loc = p->current_token.loc;
      parser_token_advance(p);
      if (!parser_expect(p, TOKEN_COLON,
                         "Expected ':' after struct field name"))
        return NULL;
      AstNode *value = parser_parse_expression(p, 0);
      if (!value)
        return NULL;
      AstNode *field_target = AstNode_new_var(field_name, field_loc);
      AstNode *assign = AstNode_new_assign_expr(field_target, value, field_loc);
      List_push(&field_inits, assign);
      if (p->current_token.type == TOKEN_COMMA)
        parser_token_advance(p);
      else
        break;
    }
    if (!parser_expect(p, TOKEN_RBRACE,
                       "Expected '}' at end of struct literal"))
      return NULL;
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected '(' or '{' after 'new' type");
    return NULL;
  }

  return AstNode_new_new_expr(target_type, args, field_inits, loc);
}

AstNode *parser_parse_primary(Parser *p) {
  Token t = p->current_token;
  parser_token_advance(p);

  switch (t.type) {
  case TOKEN_NUMBER:
    return AstNode_new_number(t.number, t.text, t.loc);
  case TOKEN_CHAR:
    return AstNode_new_char(t.text.data[0], t.loc);
  case TOKEN_IDENT:
    if (sv_eq_cstr(t.text, "sizeof") && p->current_token.type == TOKEN_LPAREN) {
      parser_token_advance(p); // consume '('
      Type *target_type = parser_parse_type_full(p);
      if (!target_type)
        return NULL;
      if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after sizeof type"))
        return NULL;
      return AstNode_new_sizeof_expr(target_type, t.loc);
    }

    if (p->current_token.type == TOKEN_COLON_COLON) {
      parser_token_advance(p);
      Token member = p->current_token;
      if (member.type == TOKEN_IDENT || member.type == TOKEN_NEW) {
        parser_token_advance(p);
      } else {
        ErrorHandler_report(p->eh, member.loc, "Expected member name after '::'");
        return NULL;
      }
      return AstNode_new_static_member(t.text, member.text, t.loc);
    }

    if (p->current_token.type == TOKEN_LBRACE) {
      Type *target_type = parser_resolve_named_type(p, t.text, false);
      if (!target_type)
        target_type = type_get_struct(p->type_ctx, t.text);
      if (!target_type)
        target_type = type_get_union(p->type_ctx, t.text);

      if (!target_type) {
        ErrorHandler_report(p->eh, t.loc, "Unknown type '%.*s' in constructor",
                            (int)t.text.len, t.text.data);
        return NULL;
      }

      List args;
      List_init(&args);
      List field_inits;
      List_init(&field_inits);

      parser_token_advance(p);
      while (p->current_token.type != TOKEN_RBRACE &&
             p->current_token.type != TOKEN_EOF) {
        if (p->current_token.type != TOKEN_IDENT) {
          ErrorHandler_report(p->eh, p->current_token.loc,
                              "Expected field name in struct literal");
          return NULL;
        }
        StringView field_name = p->current_token.text;
        Location field_loc = p->current_token.loc;
        parser_token_advance(p);
        if (!parser_expect(p, TOKEN_COLON,
                           "Expected ':' after struct field name"))
          return NULL;
        AstNode *value = parser_parse_expression(p, 0);
        if (!value)
          return NULL;
        AstNode *field_target = AstNode_new_var(field_name, field_loc);
        AstNode *assign =
            AstNode_new_assign_expr(field_target, value, field_loc);
        List_push(&field_inits, assign);
        if (p->current_token.type == TOKEN_COMMA)
          parser_token_advance(p);
        else
          break;
      }
      if (!parser_expect(p, TOKEN_RBRACE,
                         "Expected '}' at end of struct literal"))
        return NULL;

      return AstNode_new_new_expr(target_type, args, field_inits, t.loc);
    }

    return AstNode_new_var(t.text, t.loc);
  case TOKEN_STRING:
    return AstNode_new_string(t.text, t.loc);
  case TOKEN_TRUE:
    return AstNode_new_bool(1, t.loc);
  case TOKEN_FALSE:
    return AstNode_new_bool(0, t.loc);
  case TOKEN_NULL:
    return AstNode_new_null(t.loc);
  case TOKEN_NEW:
    return parser_parse_new_expr(p);

  case TOKEN_LPAREN: {
    Token peek = p->current_token;
    if (is_type_token(peek.type)) {
      Type *target_type = parser_parse_type_full(p);
      if (!target_type)
        return NULL;
      if (!parser_expect(p, TOKEN_RPAREN, "Expected ')' after cast"))
        return NULL;
      AstNode *expr = parser_parse_expression(p, 100);
      if (!expr)
        return NULL;
      return AstNode_new_cast_expr(expr, target_type, t.loc);
    } else {
      AstNode *expr = parser_parse_expression(p, 0);
      if (!parser_expect(p, TOKEN_RPAREN, "Expected ')'"))
        return NULL;
      return expr;
    }
  }

  case TOKEN_LBRACKET: {
    List items;
    List_init(&items);
    if (p->current_token.type != TOKEN_RBRACKET) {
      AstNode *first = parser_parse_expression(p, 0);
      if (!first)
        return NULL;

      if (p->current_token.type == TOKEN_SEMI) {
        parser_token_advance(p);
        AstNode *count_node = parser_parse_expression(p, 0);
        if (!count_node) {
          ErrorHandler_report(
              p->eh, p->current_token.loc,
              "Expected count expression after ';' in array literal");
          return NULL;
        }

        AstNode *res = first;

        List counts;
        List_init(&counts);
        List_push(&counts, count_node);

        while (p->current_token.type == TOKEN_COMMA) {
          parser_token_advance(p);
          AstNode *next_count = parser_parse_expression(p, 0);
          if (!next_count) {
            ErrorHandler_report(p->eh, p->current_token.loc,
                                "Expected count expression after ','");
            return NULL;
          }
          List_push(&counts, next_count);
        }

        for (int i = (int)counts.len - 1; i >= 0; i--) {
          res = AstNode_new_array_repeat(res, counts.items[i], t.loc);
        }

        List_free(&counts, 0);

        List_free(&items, 0);
        if (!parser_expect(p, TOKEN_RBRACKET,
                           "Expected ']' at end of array repeat"))
          return NULL;
        return res;
      } else {
        List_push(&items, first);
        while (p->current_token.type == TOKEN_COMMA) {
          parser_token_advance(p);
          if (p->current_token.type == TOKEN_RBRACKET)
            break;
          AstNode *item = parser_parse_expression(p, 0);
          if (!item)
            return NULL;
          List_push(&items, item);
        }
      }
    }
    if (!parser_expect(p, TOKEN_RBRACKET,
                       "Expected ']' at end of array literal"))
      return NULL;
    return AstNode_new_array_literal(items, t.loc);
  }

  case TOKEN_MINUS: {
    AstNode *expr = parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_NEG, expr, t.loc);
  }

  case TOKEN_NOT: {
    AstNode *expr = parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_NOT, expr, t.loc);
  }

  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS: {
    AstNode *expr = parser_parse_expression(p, 100);
    UnaryOp op = (t.type == TOKEN_PLUS_PLUS) ? OP_PRE_INC : OP_PRE_DEC;
    if (!parser_is(expr, NODE_VAR)) {
      ErrorHandler_report(p->eh, t.loc, "Operator requires identifier");
      return NULL;
    }
    return AstNode_new_unary(op, expr, t.loc);
  }

  case TOKEN_BIT_AND: {
    AstNode *expr = parser_parse_expression(p, 100);
    if (!parser_is(expr, NODE_VAR)) {
      ErrorHandler_report(p->eh, t.loc,
                          "Address-of operator requires identifier");
      return NULL;
    }
    return AstNode_new_unary(OP_ADDR_OF, expr, t.loc);
  }

  case TOKEN_STAR: {
    AstNode *expr = parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_DEREF, expr, t.loc);
  }

  default:
    ErrorHandler_report(p->eh, t.loc, "Unexpected token in expression");
    return NULL;
  }
}
