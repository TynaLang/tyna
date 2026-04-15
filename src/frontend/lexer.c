#include "tyl/lexer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Lexer make_lexer(const char *src, struct ErrorHandler *eh) {
  Lexer l = {0};
  l.src = src;
  l.loc.line = 1;
  l.loc.col = 1;
  l.eh = eh;
  return l;
}

static char peek(Lexer *l) { return l->src[l->cursor]; }
static char advance(Lexer *l) {
  char c = peek(l);
  l->cursor++;
  if (c == '\n') {
    l->loc.line++;
    l->loc.col = 1;
  } else
    l->loc.col++;
  return c;
}

static void skip_whitespace(Lexer *l) {
  while (isspace(peek(l)))
    advance(l);
}

static void skip_comment(Lexer *l) {
  while (peek(l) && peek(l) != '\n')
    advance(l);
}

static Token make_token(Lexer *l, TokenType type, size_t start, size_t len) {
  Token t;
  t.type = type;
  t.text = sv_from_parts(l->src + start, len);
  t.loc = l->loc;
  t.number = 0;
  return t;
}

static Token read_identifier(Lexer *l) {
  size_t start = l->cursor;
  while (isalnum(peek(l)) || peek(l) == '_')
    advance(l);
  StringView text = sv_from_parts(l->src + start, l->cursor - start);

  Token t;
  t.text = text;
  t.loc = l->loc;

  if (sv_eq_cstr(text, "let"))
    t.type = TOKEN_LET;
  else if (sv_eq_cstr(text, "const"))
    t.type = TOKEN_CONST;
  else if (sv_eq_cstr(text, "fn"))
    t.type = TOKEN_FN;
  else if (sv_eq_cstr(text, "return"))
    t.type = TOKEN_RETURN;
  else if (sv_eq_cstr(text, "print"))
    t.type = TOKEN_PRINT;
  else if (sv_eq_cstr(text, "if"))
    t.type = TOKEN_IF;
  else if (sv_eq_cstr(text, "else"))
    t.type = TOKEN_ELSE;
  else if (sv_eq_cstr(text, "defer"))
    t.type = TOKEN_DEFER;
  else if (sv_eq_cstr(text, "struct"))
    t.type = TOKEN_STRUCT;
  else if (sv_eq_cstr(text, "union"))
    t.type = TOKEN_UNION;
  else if (sv_eq_cstr(text, "loop"))
    t.type = TOKEN_LOOP;
  else if (sv_eq_cstr(text, "switch"))
    t.type = TOKEN_SWITCH;
  else if (sv_eq_cstr(text, "case"))
    t.type = TOKEN_CASE;
  else if (sv_eq_cstr(text, "while"))
    t.type = TOKEN_WHILE;
  else if (sv_eq_cstr(text, "for"))
    t.type = TOKEN_FOR;
  else if (sv_eq_cstr(text, "in"))
    t.type = TOKEN_IN;
  else if (sv_eq_cstr(text, "break"))
    t.type = TOKEN_BREAK;
  else if (sv_eq_cstr(text, "continue"))
    t.type = TOKEN_CONTINUE;
  else if (sv_eq_cstr(text, "frozen"))
    t.type = TOKEN_FROZEN;
  else if (sv_eq_cstr(text, "static"))
    t.type = TOKEN_STATIC;
  else if (sv_eq_cstr(text, "impl"))
    t.type = TOKEN_IMPL;
  else if (sv_eq_cstr(text, "new"))
    t.type = TOKEN_NEW;
  else if (sv_eq_cstr(text, "import"))
    t.type = TOKEN_IMPORT;
  else if (sv_eq_cstr(text, "export"))
    t.type = TOKEN_EXPORT;
  else if (sv_eq_cstr(text, "external"))
    t.type = TOKEN_EXTERNAL;
  else if (sv_eq_cstr(text, "int"))
    t.type = TOKEN_TYPE_INT;
  else if (sv_eq_cstr(text, "char"))
    t.type = TOKEN_TYPE_CHAR;
  else if (sv_eq_cstr(text, "str"))
    t.type = TOKEN_TYPE_STR;
  else if (sv_eq_cstr(text, "float"))
    t.type = TOKEN_TYPE_FLOAT;
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
  else if (sv_eq_cstr(text, "void"))
    t.type = TOKEN_TYPE_VOID;
  else if (sv_eq_cstr(text, "bool"))
    t.type = TOKEN_TYPE_BOOLEAN;
  else if (sv_eq_cstr(text, "true"))
    t.type = TOKEN_TRUE;
  else if (sv_eq_cstr(text, "false"))
    t.type = TOKEN_FALSE;
  else if (sv_eq_cstr(text, "null"))
    t.type = TOKEN_NULL;
  else
    t.type = TOKEN_IDENT;

  return t;
}

static Token read_number(Lexer *l) {
  size_t start = l->cursor;
  int has_dot = 0;

  while (isdigit(peek(l)) || peek(l) == '.') {
    if (peek(l) == '.') {
      if (has_dot)
        break;
      has_dot = 1;
    }
    advance(l);
  }

  if (peek(l) == 'f' || peek(l) == 'F')
    advance(l);

  StringView text = sv_from_parts(l->src + start, l->cursor - start);
  char *tmp = sv_to_cstr(text);
  double val = strtod(tmp, NULL);
  free(tmp);

  Token t = {TOKEN_NUMBER, text, val, l->loc};
  return t;
}

static Token read_string(Lexer *l) {
  advance(l); // skip opening "
  size_t start = l->cursor;

  while (peek(l) != '"' && peek(l) && peek(l) != '\n') {
    if (peek(l) == '\\')
      advance(l);
    advance(l);
  }

  Token t = {0};
  t.loc = l->loc;

  if (peek(l) != '"') {
    t.type = TOKEN_ERROR;
    t.text = sv_from_cstr("unterminated string literal");
    return t;
  }

  t.text = sv_from_parts(l->src + start, l->cursor - start);
  t.type = TOKEN_STRING;
  advance(l); // skip closing "
  return t;
}

static Token read_char(Lexer *l) {
  advance(l); // skip opening '
  size_t start = l->cursor;
  int len = 0;

  while (peek(l) != '\'' && peek(l) && peek(l) != '\n') {
    if (peek(l) == '\\')
      advance(l);
    advance(l);
    len++;
  }

  Token t = {0};
  t.loc = l->loc;

  if (peek(l) != '\'' || len != 1) {
    t.type = TOKEN_ERROR;
    t.text = sv_from_cstr("invalid char literal");
    if (peek(l) == '\'')
      advance(l);
    return t;
  }

  t.text = sv_from_parts(l->src + start, len);
  t.type = TOKEN_CHAR;
  advance(l);
  return t;
}

Token Token_advance(Lexer *l) {
  skip_whitespace(l);

  Token t;
  t.loc = l->loc;

  char c = peek(l);
  if (!c) {
    t.type = TOKEN_EOF;
    t.text = (StringView){NULL, 0};
    return t;
  }

  if (isalpha(c) || c == '_')
    return read_identifier(l);
  if (isdigit(c) || c == '.' && isdigit(l->src[l->cursor + 1]))
    return read_number(l);
  if (c == '/') {
    advance(l);
    if (peek(l) == '/') {
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

  advance(l);
  t.text = sv_from_parts(l->src + l->cursor - 1, 1);

  switch (c) {
  case '?':
    t.type = TOKEN_QUESTION;
    break;
  case ':':
    if (peek(l) == ':') {
      advance(l);
      t.type = TOKEN_COLON_COLON;
    } else {
      t.type = TOKEN_COLON;
    }
    break;
  case ';':
    t.type = TOKEN_SEMI;
    break;
  case '(':
    t.type = TOKEN_LPAREN;
    break;
  case ')':
    t.type = TOKEN_RPAREN;
    break;
  case '{':
    t.type = TOKEN_LBRACE;
    break;
  case '}':
    t.type = TOKEN_RBRACE;
    break;
  case '[':
    t.type = TOKEN_LBRACKET;
    break;
  case ']':
    t.type = TOKEN_RBRACKET;
    break;
  case ',':
    t.type = TOKEN_COMMA;
    break;
  case '.':
    t.type = TOKEN_DOT;
    break;

  // Arithmetic
  case '+':
    t.type = TOKEN_PLUS;
    if (peek(l) == '+') {
      advance(l);
      t.type = TOKEN_PLUS_PLUS;
      t.text.len = 2;
    }
    break;
  case '-':
    t.type = TOKEN_MINUS;
    if (peek(l) == '-') {
      advance(l);
      t.type = TOKEN_MINUS_MINUS;
      t.text.len = 2;
    }
    break;
  case '*':
    t.type = TOKEN_STAR;
    break;
  case '^':
    t.type = TOKEN_POWER;
    break;
  case '%':
    t.type = TOKEN_MOD;
    break;

  // Comparisons
  case '=':
    if (peek(l) == '=') {
      advance(l);
      t.type = TOKEN_EQ;
    } else if (peek(l) == '>') {
      advance(l);
      t.type = TOKEN_FAT_ARROW;
      t.text.len = 2;
    } else {
      t.type = TOKEN_ASSIGN;
    }
    break;
  case '!':
    t.type = (peek(l) == '=') ? (advance(l), TOKEN_NE) : TOKEN_NOT;
    break;
  case '<':
    t.type = (peek(l) == '=') ? (advance(l), TOKEN_LE) : TOKEN_LT;
    break;
  case '>':
    t.type = (peek(l) == '=') ? (advance(l), TOKEN_GE) : TOKEN_GT;
    break;

  // Boolean logic
  case '&':
    t.type = (peek(l) == '&') ? (advance(l), TOKEN_AND) : TOKEN_BIT_AND;
    break;
  case '|':
    t.type = (peek(l) == '|') ? (advance(l), TOKEN_OR) : TOKEN_BIT_OR;
    break;

  default:
    t.type = TOKEN_ERROR;
    t.text = sv_from_cstr("unknown character");
    break;
  }

  return t;
}
