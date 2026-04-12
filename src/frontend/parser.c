#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "parser_internal.h"

// ---------- UTILITIES ---------- //

Token *Parser_token_advance(Parser *p) {
  Token *prev_token = &p->current_token;
  p->current_token = Token_advance(p->lexer);
  return prev_token;
}

Token Parser_token_peek(Lexer *lexer, int n) {
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

int Parser_expect(Parser *p, TokenType type, const char *msg) {
  if (p->current_token.type != type) {
    ErrorHandler_report(p->eh, p->current_token.loc, "%s", msg);
    return 0;
  }
  Parser_token_advance(p);
  return 1;
}

int Parser_is(AstNode *node, AstKind kind) { return node && node->tag == kind; }

void Parser_sync(Parser *p) {
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
    case TOKEN_IMPL:
      return;
    default:
      Parser_token_advance(p);
    }
  }
}

void Parser_sync_param(Parser *p) {
  while (p->current_token.type != TOKEN_RPAREN &&
         p->current_token.type != TOKEN_EOF)
    Parser_token_advance(p);
}

void skip_to_next_param(Parser *p) {
  Parser_sync_param(p);
  if (p->current_token.type == TOKEN_COMMA)
    Parser_token_advance(p);
}

int is_type_token(TokenType t) {
  return (t >= TOKEN_TYPE_INT && t <= TOKEN_TYPE_VOID) || t == TOKEN_LBRACKET ||
         t == TOKEN_STAR;
}

int is_binary_operator(TokenType t) {
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

int is_postfix(Token t) {
  return t.type == TOKEN_LPAREN || t.type == TOKEN_LBRACKET ||
         t.type == TOKEN_DOT || t.type == TOKEN_PLUS_PLUS ||
         t.type == TOKEN_MINUS_MINUS;
}

ArithmOp token_to_arithm_op(TokenType type) {
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

CompareOp token_to_compare_op(TokenType type) {
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

EqualityOp token_to_equality_op(TokenType type) {
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

LogicalOp token_to_logical_op(TokenType type) {
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

BindingPower infix_binding_power(TokenType t) {
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

int is_lvalue(AstNode *node) {
  return node->tag == NODE_VAR || node->tag == NODE_FIELD ||
         node->tag == NODE_INDEX ||
         (node->tag == NODE_UNARY && node->unary.op == OP_DEREF);
}

Type *Parser_find_placeholder(Parser *p, StringView name) {
  for (size_t i = 0; i < p->type_placeholders.len; i++) {
    Type *t = p->type_placeholders.items[i];
    if (sv_eq(t->name, name))
      return t;
  }
  return NULL;
}

void Parser_placeholder_scope_push(Parser *p) {
  size_t *marker = xmalloc(sizeof(size_t));
  *marker = p->type_placeholders.len;
  List_push(&p->placeholder_scope_stack, marker);
}

void Parser_placeholder_scope_pop(Parser *p) {
  if (p->placeholder_scope_stack.len == 0)
    return;
  size_t *marker =
      p->placeholder_scope_stack.items[p->placeholder_scope_stack.len - 1];
  size_t target = *marker;
  free(marker);
  List_pop(&p->placeholder_scope_stack);
  while (p->type_placeholders.len > target)
    List_pop(&p->type_placeholders);
}

Type *Parser_add_placeholder(Parser *p, StringView name) {
  Type *existing = Parser_find_placeholder(p, name);
  if (existing)
    return existing;

  Type *placeholder = xcalloc(1, sizeof(Type));
  placeholder->kind = KIND_TEMPLATE;
  placeholder->name = name;
  List_init(&placeholder->members);
  List_init(&placeholder->methods);
  List_init(&placeholder->impls);
  placeholder->is_frozen = false;
  placeholder->is_intrinsic = false;

  List_push(&p->type_placeholders, placeholder);
  return placeholder;
}

AstNode *Parser_process(Lexer *lexer, ErrorHandler *eh, TypeContext *type_ctx) {
  Parser p;
  p.lexer = lexer;
  p.eh = eh;
  p.type_ctx = type_ctx;
  p.current_token = Token_advance(lexer);
  List_init(&p.type_placeholders);
  List_init(&p.placeholder_scope_stack);
  p.placeholder_mode = false;

  AstNode *ast_root = AstNode_new_program(p.current_token.loc);

  while (p.current_token.type != TOKEN_EOF) {
    AstNode *stmt = Parser_parse_statement(&p);
    if (!stmt)
      continue;
    List_push(&ast_root->ast_root.children, stmt);
  }

  List_free(&p.type_placeholders, 0);
  List_free(&p.placeholder_scope_stack, 1);

  return ast_root;
}
