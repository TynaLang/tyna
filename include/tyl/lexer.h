#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

struct ErrorHandler;

typedef struct Location {
  size_t line;
  size_t col;
} Location;

typedef struct Lexer {
  size_t cursor;
  Location loc;
  const char *src;
  struct ErrorHandler *eh;
} Lexer;

typedef enum TokenType {
  // Keywords
  TOKEN_LET,
  TOKEN_CONST,
  TOKEN_PRINT,

  // Identifier
  TOKEN_IDENT,

  // Data Types
  TOKEN_TYPE_INT,
  TOKEN_TYPE_STR,
  TOKEN_TYPE_CHAR,
  TOKEN_TYPE_FLOAT,
  TOKEN_TYPE_DOUBLE,
  TOKEN_TYPE_I8,
  TOKEN_TYPE_I16,
  TOKEN_TYPE_I32,
  TOKEN_TYPE_I64,
  TOKEN_TYPE_U8,
  TOKEN_TYPE_U16,
  TOKEN_TYPE_U32,
  TOKEN_TYPE_U64,
  TOKEN_TYPE_F32,
  TOKEN_TYPE_F64,
  TOKEN_TYPE_UNSIGNED,
  TOKEN_TYPE_SIGNED,
  TOKEN_TYPE_LONG,
  TOKEN_TYPE_SHORT,
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
  TOKEN_PLUS,
  TOKEN_PLUS_PLUS,
  TOKEN_MINUS,
  TOKEN_MINUS_MINUS,
  TOKEN_STAR,
  TOKEN_POWER,

  // ETC
  TOKEN_ERROR,
  TOKEN_EOF,
  TOKEN_UNKNOWN,
} TokenType;

typedef struct Token {
  TokenType type;
  char *text;
  double number;
  Location loc;
} Token;

Lexer make_lexer(const char *src, struct ErrorHandler *eh);

char Lexer_peek(const Lexer *l);
char Lexer_advance(Lexer *l);

Token Token_advance(Lexer *l);

#endif
