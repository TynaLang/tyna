#include "lexer.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double get_error_loc(size_t line, size_t col) {
  size_t integer_part = line;
  size_t decimal_part = col;
  double result = (double)integer_part;

  // Add the decimal part by converting it to a fraction
  // Determine how many digits are in the decimal part
  size_t temp = decimal_part;
  double divisor = 1.0;

  // Count digits in decimal_part
  if (temp == 0) {
    divisor = 10.0;
  } else {
    while (temp > 0) {
      divisor *= 10.0;
      temp /= 10;
    }
  }

  result += (double)decimal_part / divisor;
  return result;
}

static char *copy_slice(const char *str, size_t start, size_t end) {
  size_t len = end - start;
  char *out = malloc(len + 1);
  memcpy(out, str + start, len);
  out[len] = '\0';
  return out;
}

Lexer make_lexer(const char *src) {
  Lexer l;
  l.cursor = 0;
  l.col = 1;
  l.line = 1;
  l.src = src;

  return l;
}

char Lexer_peek(const Lexer *l) { return l->src[l->cursor]; }
char Lexer_advance(Lexer *l) {
  char c = Lexer_peek(l);
  l->cursor++;

  if (c == '\n') {
    l->line++;
    l->col = 1;
  } else {
    l->col++;
  }

  return c;
}

static void skip_whitespace(Lexer *l) {
  while (isspace(Lexer_peek(l)))
    Lexer_advance(l);
}

static Token read_identifier(Lexer *l) {
  size_t start = l->cursor;
  while (isalnum(Lexer_peek(l)))
    Lexer_advance(l);

  char *text = copy_slice(l->src, start, l->cursor);

  Token t;
  t.text = text;
  t.line = l->line;
  t.col = l->col;

  if (strcmp(text, "let") == 0)
    t.type = TOKEN_LET;
  else if (strcmp(text, "print") == 0)
    t.type = TOKEN_PRINT;
  else if (strcmp(text, "int") == 0)
    t.type = TOKEN_TYPE_INT;
  else if (strcmp(text, "char") == 0)
    t.type = TOKEN_TYPE_CHAR;
  else if (strcmp(text, "str") == 0)
    t.type = TOKEN_TYPE_STR;
  else
    t.type = TOKEN_IDENT;

  return t;
}

static Token read_number(Lexer *l) {
  size_t start = l->cursor;
  while (isdigit(Lexer_peek(l)) || Lexer_peek(l) == '.')
    Lexer_advance(l);

  char *text = copy_slice(l->src, start, l->cursor);

  Token t;
  t.text = text;
  t.number = strtod(text, NULL);
  t.type = TOKEN_NUMBER;
  t.line = l->line;
  t.col = l->col;
  return t;
}

static void skip_comment(Lexer *l) {
  while (Lexer_peek(l) != '\0' && Lexer_peek(l) != '\n')
    Lexer_advance(l);
}

static Token read_string(Lexer *l) {
  Lexer_advance(l); // skip opening "
  size_t start_content = l->cursor;

  while (Lexer_peek(l) != '"' && Lexer_peek(l) != '\0' &&
         Lexer_peek(l) != '\n') {
    if (Lexer_peek(l) == '\\')
      Lexer_advance(l); // skip escape
    Lexer_advance(l);
  }

  Token t;
  t.line = l->line;
  t.col = l->col;

  if (Lexer_peek(l) != '"') {
    t.type = TOKEN_ERROR;
    t.text = "unterminated string literal";
    return t;
  }

  char *text = copy_slice(l->src, start_content, l->cursor);
  Lexer_advance(l); // skip closing "

  t.text = text;
  t.type = TOKEN_STRING;
  return t;
}

static Token read_char(Lexer *l) {
  Lexer_advance(l); // skip opening '
  size_t start = l->cursor;
  int len = 0;

  while (Lexer_peek(l) != '\'' && Lexer_peek(l) != '\0' &&
         Lexer_peek(l) != '\n') {
    if (Lexer_peek(l) == '\\')
      Lexer_advance(l); // skip escape
    Lexer_advance(l);
    len++;
  }

  Token t;
  t.line = l->line;
  t.col = l->col;

  if (Lexer_peek(l) != '\'' || len != 1) {
    t.type = TOKEN_ERROR;
    t.text = "invalid char literal";
    if (Lexer_peek(l) == '\'')
      Lexer_advance(l);
    return t;
  }

  char *text = copy_slice(l->src, start, start + len);
  Lexer_advance(l); // skip closing '

  t.text = text;
  t.type = TOKEN_CHAR;
  return t;
}

Token Token_advance(Lexer *l) {
  skip_whitespace(l);

  Token t;
  t.line = l->line;
  t.col = l->col;

  char c = Lexer_peek(l);
  if (c == '\0') {
    t.type = TOKEN_EOF;
    t.text = NULL;
    return t;
  }

  if (isalpha(c))
    return read_identifier(l);
  if (isdigit(c) || c == '.')
    return read_number(l);
  if (c == '/') {
    Lexer_advance(l);
    if (Lexer_peek(l) == '/') {
      skip_comment(l);
      return Token_advance(l);
    }
    t.type = TOKEN_SLASH;
    t.text = copy_slice(l->src, l->cursor - 1, l->cursor);
    return t;
  }
  if (c == '"')
    return read_string(l);
  if (c == '\'')
    return read_char(l);

  // Single-character tokens
  Lexer_advance(l);
  t.text = copy_slice(l->src, l->cursor - 1, l->cursor);
  switch (c) {
  case ':':
    t.type = TOKEN_COLON;
    break;
  case ';':
    t.type = TOKEN_SEMI;
    break;
  case '=':
    t.type = TOKEN_ASSIGN;
    break;
  case '(':
    t.type = TOKEN_LPAREN;
    break;
  case ')':
    t.type = TOKEN_RPAREN;
    break;
  default:
    t.type = TOKEN_UNKNOWN;
    break;
  }
  return t;
}
