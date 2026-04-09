#ifndef LEXER_H
#define LEXER_H

#include "tyl/utils.h"
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
  TOKEN_FN,
  TOKEN_RETURN,
  TOKEN_IF,
  TOKEN_ELSE,
  TOKEN_DEFER,
  TOKEN_STRUCT,
  TOKEN_FOR,
  TOKEN_WHILE,
  TOKEN_LOOP,
  TOKEN_IN,
  TOKEN_BREAK,
  TOKEN_CONTINUE,
  TOKEN_FROZEN,
  TOKEN_STATIC,

  // Identifiers
  TOKEN_IDENT,

  // Data Types
  TOKEN_TYPE_INT,
  TOKEN_TYPE_STR,
  TOKEN_TYPE_BOOLEAN,
  TOKEN_TYPE_CHAR,
  TOKEN_TYPE_FLOAT,
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
  TOKEN_TYPE_VOID,
  TOKEN_NUMBER,
  TOKEN_CHAR,
  TOKEN_STRING,
  TOKEN_TRUE,
  TOKEN_FALSE,

  // Symbols
  TOKEN_COLON,
  TOKEN_COLON_COLON,
  TOKEN_ASSIGN,
  TOKEN_SEMI,
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_LBRACE,
  TOKEN_RBRACE,
  TOKEN_LBRACKET,
  TOKEN_RBRACKET,
  TOKEN_COMMA,
  TOKEN_DOT,
  TOKEN_QUESTION,

  // Arithmetic
  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_STAR,
  TOKEN_SLASH,
  TOKEN_POWER,
  TOKEN_MOD,
  TOKEN_PLUS_PLUS,
  TOKEN_MINUS_MINUS,

  // Comparison
  TOKEN_EQ,
  TOKEN_NE,
  TOKEN_LT,
  TOKEN_LE,
  TOKEN_GT,
  TOKEN_GE,

  // Boolean logic
  TOKEN_AND,
  TOKEN_OR,
  TOKEN_NOT,

  // Bitwise logic
  TOKEN_BIT_AND,
  TOKEN_BIT_OR,

  // Special
  TOKEN_ERROR,
  TOKEN_EOF,
  TOKEN_UNKNOWN
} TokenType;

typedef struct Token {
  TokenType type;
  StringView text;
  double number;
  Location loc;
} Token;

Lexer make_lexer(const char *src, struct ErrorHandler *eh);
Token Token_advance(Lexer *l);

#endif
