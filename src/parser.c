#include "parser.h"
#include "lexer.h"
#include "utils.h"
#include <unistd.h>

static Token *Parser_token_advance(Parser *parser) {
  Token *prev_token = &parser->current_token;
  parser->current_token = Token_advance(parser->lexer);
  return prev_token;
}

static Token Parser_expect(Parser *parser, TokenType type, const char *msg) {
  Token t = parser->current_token;

  if (t.type != type) {
    fprintf(stderr, "%s at line %zu col %zu\n", msg, t.line, t.col);
    exit(1); // for now, hard fail is fine
  }

  Parser_token_advance(parser);
  return t;
}

static AstNode *AstNode_new(AstKind ast_kind) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node) {
    fprintf(stderr, "Failed to allocate AST node\n");
    exit(1);
  }
  node->tag = ast_kind;
  return node;
}

static AstNode *AstNode_new_program() {
  AstNode *node = AstNode_new(NODE_PROGRAM);
  List_init(&node->program.children);
  return node;
}

static AstNode *AstNode_new_let(AstNode *name, AstNode *value, TypeKind type) {
  AstNode *node = AstNode_new(NODE_LET);
  node->let.name = name;
  node->let.value = value;
  node->let.declared_type = type;
  return node;
}

static AstNode *AstNode_new_print(AstNode *value) {
  AstNode *node = AstNode_new(NODE_PRINT);
  node->print.value = value;
  return node;
}

static AstNode *AstNode_new_number(double value) {
  AstNode *node = AstNode_new(NODE_NUMBER);
  node->number.value = value;
  return node;
}

static AstNode *AstNode_new_char(char value) {
  AstNode *node = AstNode_new(NODE_CHAR);
  node->char_lit.value = value;
  return node;
}

static AstNode *AstNode_new_string(char *value) {
  AstNode *node = AstNode_new(NODE_STRING);
  node->string.value = value;
  return node;
}

static AstNode *AstNode_new_ident(char *value) {
  AstNode *node = AstNode_new(NODE_IDENT);
  node->ident.value = value;
  return node;
}

// ---------- PARSER BODY ------------ //

static AstNode *Parser_parse_expression(Parser *p) {
  Token t = p->current_token;

  switch (t.type) {
  case TOKEN_NUMBER:
    Parser_token_advance(p);
    return AstNode_new_number(t.number);

  case TOKEN_CHAR:
    Parser_token_advance(p);
    return AstNode_new_char(t.text[0]);

  case TOKEN_STRING:
    Parser_token_advance(p);
    return AstNode_new_string(t.text);

  case TOKEN_IDENT:
    Parser_token_advance(p);
    return AstNode_new_ident(t.text);

  default:
    fprintf(stderr, "Unexpected expression at line %zu col %zu\n", t.line,
            t.col);
    exit(1);
  }
}

static AstNode *Parser_build_let_statement(Parser *p) {
  Parser_expect(p, TOKEN_LET, "Expected 'let'");

  Token ident =
      Parser_expect(p, TOKEN_IDENT, "Expected identifier after 'let'");
  AstNode *name = AstNode_new_ident(ident.text);

  Parser_expect(p, TOKEN_COLON, "Expected ':' after identifier");

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

  AstNode *value = Parser_parse_expression(p);
  if (!value)
    return NULL;

  Parser_expect(p, TOKEN_SEMI, "Expected ';' after value");

  return AstNode_new_let(name, value, declared_type);
}

static AstNode *Parser_build_print_statement(Parser *p) {
  Parser_expect(p, TOKEN_PRINT, "Expected 'print'");
  Parser_expect(p, TOKEN_LPAREN, "Expected '(' after print");

  AstNode *value = Parser_parse_expression(p);
  if (!value)
    return NULL;

  Parser_expect(p, TOKEN_RPAREN, "Expected ')' after value");
  Parser_expect(p, TOKEN_SEMI, "Expected ';' after value");

  return AstNode_new_print(value);
}

static AstNode *Parser_parse_statement(Parser *parser) {
  while (parser->current_token.type == TOKEN_SEMI) {
    Parser_token_advance(parser);
  }

  Token current_token = parser->current_token;

  switch (current_token.type) {
  case TOKEN_LET:
    return Parser_build_let_statement(parser);
  case TOKEN_PRINT:
    return Parser_build_print_statement(parser);
  default:
    fprintf(stderr, "Unexpected token %d at line %zu col %zu\n",
            current_token.type, current_token.line, current_token.col);
    return NULL;
  }
}

// ---------- PARSER BODY ------------ //

AstNode *Parser_process(Lexer *lexer) {
  Parser parser;
  parser.lexer = lexer;
  parser.current_token = Token_advance(lexer);

  AstNode *program = AstNode_new_program();

  while (parser.current_token.type != TOKEN_EOF) {
    AstNode *node = Parser_parse_statement(&parser);
    if (!node) {
      fprintf(stderr, "Parse error at line %zu col %zu\n",
              parser.current_token.line, parser.current_token.col);
      break;
    }
    List_push(&program->program.children, node);
  }

  return program;
}
