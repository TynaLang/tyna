#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "tyl/ast.h"
#include "tyl/lexer.h"
#include "tyl/parser.h"
#include "tyl/type.h"
#include "tyl/utils.h"

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

static inline int Parser_is(AstNode *node, AstKind kind) {
  return node && node->tag == kind;
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
    case TOKEN_IF:
    case TOKEN_ELSE:
    case TOKEN_WHILE:
    case TOKEN_FOR:
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

static void skip_to_next_param(Parser *p) {
  Parser_sync_param(p);
  if (p->current_token.type == TOKEN_COMMA)
    Parser_token_advance(p);
}

static int is_type_token(TokenType t) {
  return (t >= TOKEN_TYPE_INT && t <= TOKEN_TYPE_VOID) || t == TOKEN_LBRACKET ||
         t == TOKEN_IDENT;
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
  case TOKEN_LT:
  case TOKEN_GT:
  case TOKEN_LE:
  case TOKEN_GE:
  case TOKEN_EQ:
  case TOKEN_NE:
  case TOKEN_AND:
  case TOKEN_OR:
  case TOKEN_QUESTION:
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
    return (struct BindingPower){9, 10};
  case TOKEN_LT:
  case TOKEN_GT:
  case TOKEN_LE:
  case TOKEN_GE:
    return (struct BindingPower){11, 12};

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

// ---------- FORWARD DECLARATIONS ---------- //

static AstNode *Parser_parse_expression(Parser *p, int min_bp);
static AstNode *Parser_parse_statement(Parser *p);
static PrimitiveKind Parser_parse_type(Parser *p);
static Type *Parser_parse_type_full(Parser *p);
static AstNode *Parser_parse_block(Parser *p);
static AstNode *Parser_parse_var_decl(Parser *p);

// ---------- POSTFIX HANDLERS ---------- //

static AstNode *Parser_parse_call(Parser *p, AstNode *expr) {
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

  return AstNode_new_call(expr, args, loc);
}

static AstNode *Parser_parse_index(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p);
  AstNode *index = Parser_parse_expression(p, 0);
  if (!index)
    return NULL;
  if (!Parser_expect(p, TOKEN_RBRACKET, "Expected ']' after index"))
    return NULL;

  return AstNode_new_index(expr, index, loc);
}

static AstNode *Parser_parse_field(Parser *p, AstNode *expr) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume '.'

  if (p->current_token.type != TOKEN_IDENT) {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected identifier after '.'");
    return NULL;
  }

  StringView field_name = p->current_token.text;
  Parser_token_advance(p);

  return AstNode_new_field(expr, field_name, loc);
}

static AstNode *Parser_parse_postfix_inc(Parser *p, AstNode *expr) {
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

// ---------- PARSING ---------- //

static AstNode *Parser_parse_primary(Parser *p) {
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
    return AstNode_new_var(t.text, t.loc);
  case TOKEN_STRING:
    return AstNode_new_string(t.text, t.loc);
  case TOKEN_TRUE:
    return AstNode_new_bool(1, t.loc);
  case TOKEN_FALSE:
    return AstNode_new_bool(0, t.loc);

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

    // CREATE CORRECT NODE
    switch (op.type) {
    case TOKEN_ASSIGN:
      if (!is_lvalue(left)) {
        ErrorHandler_report(p->eh, op.loc, "Invalid assignment target");
        return NULL;
      }
      if (Parser_is(right, NODE_ASSIGN_EXPR)) {
        ErrorHandler_report(p->eh, op.loc, "Chained assignment not supported");
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

    default:
      fprintf(stderr, "Unexpected binary operator\n");
      exit(1);
    }
  }

  return left;
}

static Type *Parser_parse_type_full(Parser *p) {
  Type *res = NULL;

  // Handle prefix type operators: [T; N]
  if (p->current_token.type == TOKEN_LBRACKET) {
    Parser_token_advance(p);
    Type *elementType = Parser_parse_type_full(p);
    if (!elementType)
      return NULL;

    if (p->current_token.type == TOKEN_SEMI) {
      Parser_token_advance(p);
      AstNode *count_node = Parser_parse_expression(p, 0);
      if (type_is_numeric(count_node->resolved_type)) {
        ErrorHandler_report(p->eh, p->current_token.loc, "Expected array size");
        return NULL;
      }
      // Fixed-size array [T; N] -> for now, we'll just treat it as Array<T>
    }

    if (!Parser_expect(p, TOKEN_RBRACKET, "Expected ']' for array type"))
      return NULL;

    Type *array_template =
        type_get_template(p->type_ctx, sv_from_cstr("Array"));
    if (!array_template) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Internal error: 'Array' template not registered");
      return NULL;
    }

    List args;
    List_init(&args);
    List_push(&args, elementType);
    res = type_get_instance(p->type_ctx, array_template, args);
  } else {
    PrimitiveKind kind = Token_token_to_type(p->current_token.type);
    if (kind != PRIM_UNKNOWN) {
      Parser_token_advance(p);
      res = type_get_primitive(p->type_ctx, kind);
    } else if (p->current_token.type == TOKEN_IDENT) {
      StringView name = p->current_token.text;
      Parser_token_advance(p);
      res = type_get_struct(p->type_ctx, name);
    }
  }

  if (!res) {
    ErrorHandler_report(p->eh, p->current_token.loc, "Expected type");
    return NULL;
  }

  // Handle postfix type operators: T[] for Array<T>
  while (p->current_token.type == TOKEN_LBRACKET) {
    Parser_token_advance(p);
    if (!Parser_expect(p, TOKEN_RBRACKET, "Expected ']' for array type"))
      return NULL;

    Type *array_template =
        type_get_template(p->type_ctx, sv_from_cstr("Array"));
    if (!array_template) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Internal error: 'Array' template not registered");
      return NULL;
    }

    List args;
    List_init(&args);
    List_push(&args, res);
    res = type_get_instance(p->type_ctx, array_template, args);
  }

  return res;
}

static PrimitiveKind Parser_parse_type(Parser *p) {
  PrimitiveKind kind = Token_token_to_type(p->current_token.type);
  if (kind != PRIM_UNKNOWN)
    Parser_token_advance(p);
  return kind;
}

static AstNode *Parser_parse_block(Parser *p) {
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

static AstNode *Parser_parse_var_decl(Parser *p) {
  int is_const = (p->current_token.type == TOKEN_CONST);
  Parser_token_advance(p);

  Token ident = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT,
                     "Expected identifier after 'let' or 'const'"))
    return NULL;

  AstNode *name = AstNode_new_var(ident.text, ident.loc);

  Type *declared_type = NULL;
  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p);
    declared_type = Parser_parse_type_full(p);
    if (!declared_type)
      return NULL;
  } else {
    declared_type = type_get_primitive(p->type_ctx, PRIM_UNKNOWN);
  }

  if (p->current_token.type == TOKEN_ASSIGN) {
    Parser_token_advance(p);
    AstNode *value = Parser_parse_expression(p, 0);
    if (!value)
      return NULL;
    if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after value"))
      return NULL;

    return AstNode_new_var_decl(name, value, declared_type, is_const,
                                ident.loc);
  } else {
    if (declared_type->kind == KIND_PRIMITIVE &&
        declared_type->data.primitive == PRIM_UNKNOWN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Type required for uninitialized variable");
      return NULL;
    }

    if (!Parser_expect(p, TOKEN_SEMI,
                       "Expected ';' or '=' after identifier/type"))
      return NULL;

    return AstNode_new_var_decl(name, NULL, declared_type, is_const, ident.loc);
  }
}

static AstNode *Parser_parse_print(Parser *p) {
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

static AstNode *Parser_parse_fn_decl(Parser *p, bool is_static) {
  Parser_token_advance(p); // consume 'fn'

  Token ident = p->current_token;
  Location loc = ident.loc;

  if (!Parser_expect(p, TOKEN_IDENT, "Expected function name"))
    return NULL;

  if (!Parser_expect(p, TOKEN_LPAREN, "Expected '(' after function name"))
    return NULL;

  List params;
  List_init(&params);

  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF) {
    Location p_loc = p->current_token.loc;

    StringView param_name = p->current_token.text;
    AstNode *param_name_node = AstNode_new_var(param_name, p_loc);
    if (!Parser_expect(p, TOKEN_IDENT, "Expected parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_COLON, "Expected ':' after parameter name")) {
      skip_to_next_param(p);
      continue;
    }

    Type *param_type = Parser_parse_type_full(p);
    if (!param_type) {
      skip_to_next_param(p);
      continue;
    }

    List_push(&params, AstNode_new_param(param_name_node, param_type, p_loc));
    if (p->current_token.type == TOKEN_COMMA)
      Parser_token_advance(p);
  }

  if (!Parser_expect(p, TOKEN_RPAREN, "Expected ')' after parameters"))
    return NULL;

  Type *ret_type = type_get_primitive(p->type_ctx, PRIM_VOID);
  if (p->current_token.type == TOKEN_COLON) {
    Parser_token_advance(p);
    ret_type = Parser_parse_type_full(p);
    if (!ret_type) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected return type after ':'");
      return NULL;
    }
  }

  AstNode *body = NULL;
  if (p->current_token.type == TOKEN_LBRACE) {
    body = Parser_parse_block(p);
  } else if (p->current_token.type == TOKEN_SEMI) {
    Parser_token_advance(p);
  } else {
    ErrorHandler_report(p->eh, p->current_token.loc,
                        "Expected '{' or ';' after function declaration");
    Parser_sync(p);
  }

  return AstNode_new_func_decl(AstNode_new_var(ident.text, ident.loc), params,
                               ret_type, body, is_static, loc);
}

static AstNode *Parser_parse_if_stmt(Parser *p) {
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

static AstNode *Parser_parse_while_stmt(Parser *p) {
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

static AstNode *Parser_parse_for_stmt(Parser *p) {
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
      init = Parser_parse_var_decl(p);
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

static AstNode *Parser_parse_struct_decl(Parser *p, bool is_frozen) {
  Location loc = p->current_token.loc;
  Parser_token_advance(p); // consume 'struct'

  Token name_token = p->current_token;
  if (!Parser_expect(p, TOKEN_IDENT, "Expected struct name"))
    return NULL;
  AstNode *name_node = AstNode_new_var(name_token.text, name_token.loc);

  List placeholders;
  List_init(&placeholders);
  if (p->current_token.type == TOKEN_LT) {
    Parser_token_advance(p);
    while (p->current_token.type != TOKEN_GT &&
           p->current_token.type != TOKEN_EOF) {
      if (p->current_token.type != TOKEN_IDENT) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Expected placeholder name");
        break;
      }
      StringView *pl = xmalloc(sizeof(StringView));
      *pl = p->current_token.text;
      List_push(&placeholders, pl);
      Parser_token_advance(p);
      if (p->current_token.type == TOKEN_COMMA)
        Parser_token_advance(p);
    }
    Parser_expect(p, TOKEN_GT, "Expected '>' after placeholders");
  }

  if (!Parser_expect(p, TOKEN_LBRACE, "Expected '{' after struct name"))
    return NULL;

  List members;
  List_init(&members);

  while (p->current_token.type != TOKEN_RBRACE &&
         p->current_token.type != TOKEN_EOF) {
    bool is_static = false;
    if (p->current_token.type == TOKEN_STATIC) {
      is_static = true;
      Parser_token_advance(p);
    }

    if (p->current_token.type == TOKEN_FN) {
      AstNode *fn = Parser_parse_fn_decl(p, is_static);
      if (fn)
        List_push(&members, fn);
      continue;
    }

    Location mem_loc = p->current_token.loc;
    Token mem_ident = p->current_token;
    if (!Parser_expect(p, TOKEN_IDENT, "Expected member name")) {
      Parser_sync(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_COLON, "Expected ':' after member name")) {
      Parser_sync(p);
      continue;
    }

    Type *type = Parser_parse_type_full(p);
    if (!type) {
      Parser_sync(p);
      continue;
    }

    if (!Parser_expect(p, TOKEN_SEMI, "Expected ';' after member type")) {
      Parser_sync(p);
      continue;
    }

    AstNode *mem_name = AstNode_new_var(mem_ident.text, mem_ident.loc);
    AstNode *mem_decl = AstNode_new_var_decl(mem_name, NULL, type, 0, mem_loc);
    List_push(&members, mem_decl);
  }

  Parser_expect(p, TOKEN_RBRACE, "Expected '}' after struct members");

  return AstNode_new_struct_decl(name_node, members, placeholders, is_frozen,
                                 loc);
}

static AstNode *Parser_parse_statement(Parser *p) {
  Location loc = p->current_token.loc;

  switch (p->current_token.type) {
  case TOKEN_EOF:
    return NULL;
  case TOKEN_LET:
  case TOKEN_CONST:
    return Parser_parse_var_decl(p);
  case TOKEN_LBRACE:
    return Parser_parse_block(p);
  case TOKEN_PRINT:
    return Parser_parse_print(p);
  case TOKEN_IF:
    return Parser_parse_if_stmt(p);
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
    return Parser_parse_fn_decl(p, false);
  case TOKEN_STRUCT:
    return Parser_parse_struct_decl(p, false);
  case TOKEN_FROZEN: {
    Parser_token_advance(p);
    if (p->current_token.type != TOKEN_STRUCT) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'struct' after 'frozen'");
      return NULL;
    }
    return Parser_parse_struct_decl(p, true);
  }
  case TOKEN_STATIC: {
    Parser_token_advance(p);
    if (p->current_token.type != TOKEN_FN) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Expected 'fn' after 'static'");
      return NULL;
    }
    return Parser_parse_fn_decl(p, true);
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

AstNode *Parser_process(Lexer *lexer, ErrorHandler *eh, TypeContext *type_ctx) {
  Parser p;
  p.lexer = lexer;
  p.eh = eh;
  p.type_ctx = type_ctx;
  p.current_token = Token_advance(lexer);

  AstNode *ast_root = AstNode_new_program(p.current_token.loc);

  while (p.current_token.type != TOKEN_EOF) {
    AstNode *stmt = Parser_parse_statement(&p);
    if (!stmt)
      continue;
    List_push(&ast_root->ast_root.children, stmt);
  }

  return ast_root;
}
