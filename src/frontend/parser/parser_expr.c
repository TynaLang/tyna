#include "parser_internal.h"

static bool Parser_is_generic_call_start(Parser *p) {
  if (p->current_token.type != TOKEN_LT)
    return false;

  int depth = 0;
  int lookahead = 1;
  while (true) {
    Token t = Parser_token_peek(p->lexer, lookahead++);
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

  Token next = Parser_token_peek(p->lexer, lookahead);
  return next.type == TOKEN_LPAREN;
}

static List Parser_parse_generic_type_args(Parser *p) {
  List generic_args;
  List_init(&generic_args);

  if (!Parser_expect(p, TOKEN_LT, "Expected '<' to start generic arguments"))
    return generic_args;

  while (true) {
    Type *arg_type = Parser_parse_type_full(p);
    if (!arg_type) {
      List_free(&generic_args, 0);
      return generic_args;
    }
    List_push(&generic_args, arg_type);

    if (p->current_token.type == TOKEN_COMMA) {
      Parser_token_advance(p);
      continue;
    }
    break;
  }

  if (!Parser_expect(p, TOKEN_GT, "Expected '>' after generic arguments")) {
    List_free(&generic_args, 0);
    return generic_args;
  }

  return generic_args;
}

AstNode *Parser_parse_call(Parser *p, AstNode *expr, List generic_args) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p);
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

  AstNode *node = AstNode_new_call(expr, args, loc);
  node->call.generic_args = generic_args;
  return node;
}

AstNode *Parser_parse_index(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p);
  AstNode *index = Parser_parse_expression(p, 0);
  if (!index)
    return NULL;
  if (!Parser_expect(p, TOKEN_RBRACKET, "Expected ']' after index"))
    return NULL;

  return AstNode_new_index(expr, index, loc);
}

AstNode *Parser_parse_field(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume '.'

  if (p->current_token.type != TOKEN_IDENT &&
      p->current_token.type != TOKEN_NEW) {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected identifier after '.'");
    return NULL;
  }

  StringView field_name = p->current_token.text;
  Parser_token_advance(p);

  return AstNode_new_field(expr, field_name, loc);
}

AstNode *Parser_parse_postfix_inc(Parser *p, AstNode *expr) {
  Token op = p->current_token;
  Parser_token_advance(p);
  UnaryOp unary_op = (op.type == TOKEN_PLUS_PLUS) ? OP_POST_INC : OP_POST_DEC;
  if (!Parser_is(expr, NODE_VAR)) {
    ErrorHandler_report(p->eh, op.loc, "Operator '%s' requires an identifier",
                        unary_op == OP_POST_INC ? "++" : "--");
    return NULL;
  }
  return AstNode_new_unary(unary_op, expr, op.loc);
}

AstNode *Parser_make_postfix(Parser *p, AstNode *expr) {
  switch (p->current_token.type) {
  case TOKEN_LPAREN:
    return Parser_parse_call(p, expr, (List){0});
  case TOKEN_LT:
    if (Parser_is_generic_call_start(p)) {
      List generic_args = Parser_parse_generic_type_args(p);
      return Parser_parse_call(p, expr, generic_args);
    }
    return expr;
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

static AstNode *Parser_parse_new_expr(Parser *p) {
  Location loc = p->current_token.loc;

  Type *target_type = Parser_parse_type_full(p);
  if (!target_type)
    return NULL;

  List args;
  List_init(&args);
  List field_inits;
  List_init(&field_inits);

  if (p->current_token.type == TOKEN_LPAREN) {
    Parser_token_advance(p);
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
    if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after constructor args"))
      return NULL;
  } else if (p->current_token.type == TOKEN_LBRACE) {
    Parser_token_advance(p);
    while (p->current_token.type != TOKEN_RBRACE &&
           p->current_token.type != TOKEN_EOF) {
      if (p->current_token.type != TOKEN_IDENT) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Expected field name in struct literal");
        return NULL;
      }
      StringView field_name = p->current_token.text;
      Location field_loc = p->current_token.loc;
      Parser_token_advance(p);
      if (!Parser_expect(p, TOKEN_COLON,
                         "Expected ':' after struct field name"))
        return NULL;
      AstNode *value = Parser_parse_expression(p, 0);
      if (!value)
        return NULL;
      AstNode *field_target = AstNode_new_var(field_name, field_loc);
      AstNode *assign = AstNode_new_assign_expr(field_target, value, field_loc);
      List_push(&field_inits, assign);
      if (p->current_token.type == TOKEN_COMMA)
        Parser_token_advance(p);
      else
        break;
    }
    if (!Parser_expect(p, TOKEN_RBRACE,
                       "Expected '}' at end of struct literal"))
      return NULL;
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected '(' or '{' after 'new' type");
    return NULL;
  }

  return AstNode_new_new_expr(target_type, args, field_inits, loc);
}

AstNode *Parser_parse_primary(Parser *p) {
  Token t = p->current_token;
  Parser_token_advance(p);

  switch (t.type) {
  case TOKEN_NUMBER:
    return AstNode_new_number(t.number, t.text, t.loc);
  case TOKEN_CHAR:
    return AstNode_new_char(t.text.data[0], t.loc);
  case TOKEN_IDENT:
    if (p->current_token.type == TOKEN_COLON_COLON) {
      Parser_token_advance(p);
      Token member = p->current_token;
      if (!Parser_expect(p, TOKEN_IDENT, "Expected member name after '::'"))
        return NULL;
      return AstNode_new_static_member(t.text, member.text, t.loc);
    }

    if (p->current_token.type == TOKEN_LBRACE) {
      Type *target_type = type_get_named(p->type_ctx, t.text);
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

      Parser_token_advance(p);
      while (p->current_token.type != TOKEN_RBRACE &&
             p->current_token.type != TOKEN_EOF) {
        if (p->current_token.type != TOKEN_IDENT) {
          ErrorHandler_report(p->eh, p->current_token.loc,
                              "Expected field name in struct literal");
          return NULL;
        }
        StringView field_name = p->current_token.text;
        Location field_loc = p->current_token.loc;
        Parser_token_advance(p);
        if (!Parser_expect(p, TOKEN_COLON,
                           "Expected ':' after struct field name"))
          return NULL;
        AstNode *value = Parser_parse_expression(p, 0);
        if (!value)
          return NULL;
        AstNode *field_target = AstNode_new_var(field_name, field_loc);
        AstNode *assign =
            AstNode_new_assign_expr(field_target, value, field_loc);
        List_push(&field_inits, assign);
        if (p->current_token.type == TOKEN_COMMA)
          Parser_token_advance(p);
        else
          break;
      }
      if (!Parser_expect(p, TOKEN_RBRACE,
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
    return Parser_parse_new_expr(p);

  case TOKEN_LPAREN: {
    Token peek = p->current_token;
    if (is_type_token(peek.type)) {
      PrimitiveKind target_kind = Parser_parse_type(p);
      Parser_expect(p, TOKEN_RPAREN, "Expected ')' after cast");
      AstNode *expr = Parser_parse_expression(p, 100);
      return AstNode_new_cast_expr(
          expr, type_get_primitive(p->type_ctx, target_kind), t.loc);
    } else {
      AstNode *expr = Parser_parse_expression(p, 0);
      Parser_expect(p, TOKEN_RPAREN, "Expected ')'");
      return expr;
    }
  }

  case TOKEN_LBRACKET: {
    List items;
    List_init(&items);
    if (p->current_token.type != TOKEN_RBRACKET) {
      AstNode *first = Parser_parse_expression(p, 0);
      if (!first)
        return NULL;

      if (p->current_token.type == TOKEN_SEMI) {
        // [value; count1, count2, ...] shorthand
        Parser_token_advance(p);
        AstNode *count_node = Parser_parse_expression(p, 0);
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
          Parser_token_advance(p);
          AstNode *next_count = Parser_parse_expression(p, 0);
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
        Parser_expect(p, TOKEN_RBRACKET, "Expected ']' at end of array repeat");
        return res;
      } else {
        List_push(&items, first);
        while (p->current_token.type == TOKEN_COMMA) {
          Parser_token_advance(p);
          if (p->current_token.type == TOKEN_RBRACKET)
            break;
          AstNode *item = Parser_parse_expression(p, 0);
          if (!item)
            return NULL;
          List_push(&items, item);
        }
      }
    }
    Parser_expect(p, TOKEN_RBRACKET, "Expected ']' at end of array literal");
    return AstNode_new_array_literal(items, t.loc);
  }

  case TOKEN_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_NEG, expr, t.loc);
  }

  case TOKEN_NOT: {
    AstNode *expr = Parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_NOT, expr, t.loc);
  }

  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS: {
    AstNode *expr = Parser_parse_expression(p, 100);
    UnaryOp op = (t.type == TOKEN_PLUS_PLUS) ? OP_PRE_INC : OP_PRE_DEC;
    if (!Parser_is(expr, NODE_VAR)) {
      ErrorHandler_report(p->eh, t.loc, "Operator requires identifier");
      return NULL;
    }
    return AstNode_new_unary(op, expr, t.loc);
  }

  case TOKEN_BIT_AND: {
    AstNode *expr = Parser_parse_expression(p, 100);
    if (!Parser_is(expr, NODE_VAR)) {
      ErrorHandler_report(p->eh, t.loc,
                          "Address-of operator requires identifier");
      return NULL;
    }
    return AstNode_new_unary(OP_ADDR_OF, expr, t.loc);
  }

  case TOKEN_STAR: {
    AstNode *expr = Parser_parse_expression(p, 100);
    return AstNode_new_unary(OP_DEREF, expr, t.loc);
  }

  default:
    ErrorHandler_report(p->eh, t.loc, "Unexpected token in expression");
    return NULL;
  }
}

AstNode *Parser_parse_expression(Parser *p, int min_bp) {
  AstNode *left = Parser_parse_primary(p);
  if (!left)
    return NULL;

  while (1) {
    Token op = p->current_token;

    if (Parser_is_postfix(p)) {
      if (100 < min_bp)
        break;
      left = Parser_make_postfix(p, left);
      continue;
    }

    if (!is_binary_operator(op.type))
      break;

    struct BindingPower bp = infix_binding_power(op.type);
    if (bp.left_bp < min_bp)
      break;

    Parser_token_advance(p);

    if (op.type == TOKEN_QUESTION) {
      AstNode *true_expr = Parser_parse_expression(p, 0);
      if (!true_expr)
        return NULL;
      if (!Parser_expect(p, TOKEN_COLON, "Expected ':' in ternary operator"))
        return NULL;
      AstNode *false_expr = Parser_parse_expression(p, bp.right_bp);
      if (!false_expr)
        return NULL;
      left = AstNode_new_ternary(left, true_expr, false_expr, op.loc);
      continue;
    }

    AstNode *right = Parser_parse_expression(p, bp.right_bp);
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
