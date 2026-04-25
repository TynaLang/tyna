#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "parser_internal.h"

// ---------- UTILITIES ---------- //

Token *parser_token_advance(Parser *p) {
  Token *prev_token = &p->current_token;
  p->current_token = Token_advance(p->lexer);
  return prev_token;
}

Token parser_token_peek(Lexer *lexer, int n) {
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

int parser_expect(Parser *p, TokenType type, const char *msg) {
  if (p->current_token.type != type) {
    ErrorHandler_report(p->eh, p->current_token.loc, "%s", msg);
    return 0;
  }
  parser_token_advance(p);
  return 1;
}

int parser_is(AstNode *node, AstKind kind) { return node && node->tag == kind; }

void parser_sync(Parser *p) {
  parser_token_advance(p);

  while (p->current_token.type != TOKEN_EOF) {
    if (p->current_token.type == TOKEN_SEMI)
      return;

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
      parser_token_advance(p);
    }
  }
}

Type *parser_find_placeholder(Parser *p, StringView name) {
  for (size_t i = 0; i < p->type_placeholders.len; i++) {
    Type *t = p->type_placeholders.items[i];
    if (sv_eq(t->name, name))
      return t;
  }
  return NULL;
}

void parser_placeholder_scope_push(Parser *p) {
  size_t *marker = xmalloc(sizeof(size_t));
  *marker = p->type_placeholders.len;
  List_push(&p->placeholder_scope_stack, marker);
}

void parser_placeholder_scope_pop(Parser *p) {
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

Type *parser_add_placeholder(Parser *p, StringView name) {
  Type *existing = parser_find_placeholder(p, name);
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

AstNode *parser_process(Lexer *lexer, ErrorHandler *eh, TypeContext *type_ctx) {
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
    TokenType start_token = p.current_token.type;
    AstNode *stmt = parser_parse_statement(&p);
    if (!stmt) {
      parser_sync(&p);
      if (p.current_token.type == start_token &&
          p.current_token.type != TOKEN_EOF) {
        parser_token_advance(&p);
      }
      continue;
    }
    List_push(&ast_root->ast_root.children, stmt);
  }

  List_free(&p.type_placeholders, 0);
  List_free(&p.placeholder_scope_stack, 1);

  return ast_root;
}
