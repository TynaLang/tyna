#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef struct Lexer
{
  size_t cursor, line, col;
  const char *src;
} Lexer;

typedef enum TokenType
{
  // Keywords
  TOKEN_LET,
  TOKEN_PRINT,

  // Identifier
  TOKEN_IDENT,

  // Data types
  TOKEN_TYPE_INT,
  TOKEN_TYPE_STR,
  TOKEN_TYPE_CHAR,

  TOKEN_NUMBER,
  TOKEN_CHAR,
  TOKEN_STRING,

  // Symbols
  TOKEN_COLON,
  TOKEN_ASSIGN,
  TOKEN_SEMI,
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_SLASH,

  // ETC
  TOKEN_ERROR,
  TOKEN_EOF,
  TOKEN_UNKNOWN,
} TokenType;

typedef struct Token
{
  TokenType type;
  char *text;
  double number;
  size_t line;
  size_t col;
} Token;

Lexer make_lexer(const char *src);

char Lexer_peek(const Lexer *l);
char Lexer_advance(Lexer *l);

Token Token_advance(Lexer *l);

#endif
