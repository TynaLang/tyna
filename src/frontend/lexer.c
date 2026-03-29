#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"

Lexer make_lexer(const char *src, struct ErrorHandler *eh) {
  Lexer l;
  l.cursor = 0;
  l.loc.line = 1;
  l.loc.col = 1;
  l.src = src;
  l.eh = eh;

  return l;
}

char Lexer_peek(const Lexer *l) { return l->src[l->cursor]; }
char Lexer_advance(Lexer *l) {
  char c = Lexer_peek(l);
  l->cursor++;

  if (c == '\n') {
    l->loc.line++;
    l->loc.col = 1;
  } else {
    l->loc.col++;
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

  StringView text = sv_from_parts(l->src + start, l->cursor - start);

  Token t;
  t.text = text;
  t.loc = l->loc;

  if (sv_eq_cstr(text, "let"))
    t.type = TOKEN_LET;
  else if (sv_eq_cstr(text, "const"))
    t.type = TOKEN_CONST;
  else if (sv_eq_cstr(text, "print"))
    t.type = TOKEN_PRINT;
  else if (sv_eq_cstr(text, "int"))
    t.type = TOKEN_TYPE_INT;
  else if (sv_eq_cstr(text, "char"))
    t.type = TOKEN_TYPE_CHAR;
  else if (sv_eq_cstr(text, "str"))
    t.type = TOKEN_TYPE_STR;
  else if (sv_eq_cstr(text, "float"))
    t.type = TOKEN_TYPE_FLOAT;
  else if (sv_eq_cstr(text, "double"))
    t.type = TOKEN_TYPE_DOUBLE;
  else if (sv_eq_cstr(text, "i8"))
    t.type = TOKEN_TYPE_I8;
  else if (sv_eq_cstr(text, "i16"))
    t.type = TOKEN_TYPE_I16;
  else if (sv_eq_cstr(text, "i32"))
    t.type = TOKEN_TYPE_I32;
  else if (sv_eq_cstr(text, "i64"))
    t.type = TOKEN_TYPE_I64;
  else if (sv_eq_cstr(text, "u8"))
    t.type = TOKEN_TYPE_U8;
  else if (sv_eq_cstr(text, "u16"))
    t.type = TOKEN_TYPE_U16;
  else if (sv_eq_cstr(text, "u32"))
    t.type = TOKEN_TYPE_U32;
  else if (sv_eq_cstr(text, "u64"))
    t.type = TOKEN_TYPE_U64;
  else if (sv_eq_cstr(text, "f32"))
    t.type = TOKEN_TYPE_F32;
  else if (sv_eq_cstr(text, "f64"))
    t.type = TOKEN_TYPE_F64;
  else if (sv_eq_cstr(text, "unsigned"))
    t.type = TOKEN_TYPE_UNSIGNED;
  else if (sv_eq_cstr(text, "signed"))
    t.type = TOKEN_TYPE_SIGNED;
  else if (sv_eq_cstr(text, "long"))
    t.type = TOKEN_TYPE_LONG;
  else if (sv_eq_cstr(text, "short"))
    t.type = TOKEN_TYPE_SHORT;
  else
    t.type = TOKEN_IDENT;

  return t;
}

static Token read_number(Lexer *l) {
  size_t start = l->cursor;
  int has_dot = 0;

  while (isdigit(Lexer_peek(l)) || Lexer_peek(l) == '.') {
    if (Lexer_peek(l) == '.') {
      if (has_dot)
        break; // Only one dot allowed
      has_dot = 1;
    }
    Lexer_advance(l);
  }

  // Check for 'f' suffix for floats
  if (Lexer_peek(l) == 'f' || Lexer_peek(l) == 'F') {
    Lexer_advance(l);
  }

  StringView text = sv_from_parts(l->src + start, l->cursor - start);
  char *tmp = sv_to_cstr(text);
  double val = strtod(tmp, NULL);
  free(tmp);

  Token t;
  t.text = text;
  t.number = val;
  t.type = TOKEN_NUMBER;
  t.loc = l->loc;
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
  t.loc = l->loc;

  if (Lexer_peek(l) != '"') {
    t.type = TOKEN_ERROR;
    t.text = sv_from_cstr("unterminated string literal");
    return t;
  }

  StringView text =
      sv_from_parts(l->src + start_content, l->cursor - start_content);
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
  t.loc = l->loc;

  if (Lexer_peek(l) != '\'' || len != 1) {
    t.type = TOKEN_ERROR;
    t.text = sv_from_cstr("invalid char literal");
    if (Lexer_peek(l) == '\'')
      Lexer_advance(l);
    return t;
  }

  StringView text = sv_from_parts(l->src + start, len);
  Lexer_advance(l); // skip closing '

  t.text = text;
  t.type = TOKEN_CHAR;
  return t;
}

Token Token_advance(Lexer *l) {
  skip_whitespace(l);

  Token t;
  t.loc = l->loc;

  char c = Lexer_peek(l);
  if (c == '\0') {
    t.type = TOKEN_EOF;
    t.text = (StringView){NULL, 0};
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
    t.text = sv_from_parts(l->src + l->cursor - 1, 1);
    return t;
  }
  if (c == '"')
    return read_string(l);
  if (c == '\'')
    return read_char(l);

  // Single-character tokens
  Lexer_advance(l);
  t.text = sv_from_parts(l->src + l->cursor - 1, 1);
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
  case '+':
    t.type = TOKEN_PLUS;
    if (Lexer_peek(l) == '+') {
      Lexer_advance(l);
      t.text = sv_from_parts(t.text.data, 2);
      t.type = TOKEN_PLUS_PLUS;
    }
    break;
  case '-':
    t.type = TOKEN_MINUS;
    if (Lexer_peek(l) == '-') {
      Lexer_advance(l);
      t.text = sv_from_parts(t.text.data, 2);
      t.type = TOKEN_MINUS_MINUS;
    }
    break;
  case '*':
    t.type = TOKEN_STAR;
    break;
  case '^':
    t.type = TOKEN_POWER;
    break;
  default:
    t.type = TOKEN_ERROR;
    t.text = sv_from_cstr("unknown character");
    break;
  }
  return t;
}
