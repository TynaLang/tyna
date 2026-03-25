#include "lexer.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double get_error_loc(size_t line, size_t col)
{
  size_t integer_part = line;
  size_t decimal_part = col;
  double result = (double)integer_part;

  // Add the decimal part by converting it to a fraction
  // Determine how many digits are in the decimal part
  size_t temp = decimal_part;
  double divisor = 1.0;

  // Count digits in decimal_part
  if (temp == 0)
  {
    divisor = 10.0;
  }
  else
  {
    while (temp > 0)
    {
      divisor *= 10.0;
      temp /= 10;
    }
  }

  result += (double)decimal_part / divisor;
  return result;
}

static char *copy_slice(const char *str, size_t start, size_t end)
{
  size_t len = end - start;
  char *out = malloc(len + 1);
  memcpy(out, str + start, len);
  out[len] = '\0';
  return out;
}

Lexer make_lexer(const char *src)
{
  Lexer l;
  l.cursor = 0;
  l.col = 1;
  l.line = 1;
  l.src = src;

  return l;
}

char peek(const Lexer *l) { return l->src[l->cursor]; }
char advance(Lexer *l)
{
  char c = peek(l);
  l->cursor++;

  if (c == '\n')
  {
    l->line++;
    l->col = 1;
  }
  else
  {
    l->col++;
  }

  return c;
}

Token next_token(Lexer *l)
{
  while (isspace(peek(l)))
  {
    advance(l);
  }

  Token t;
  t.line = l->line;
  t.col = l->col;

  size_t start = l->cursor;

  if (peek(l) == '\0')
  {
    t.type = TOKEN_EOF;
    t.text = NULL;
    return t;
  }

  if (isalpha(peek(l)))
  {
    while (isalnum(peek(l)))
    {
      advance(l);
    }

    t.text = copy_slice(l->src, start, l->cursor);

    if (strcmp(t.text, "let") == 0)
    {
      t.type = TOKEN_LET;
    }
    else if (strcmp(t.text, "print") == 0)
    {
      t.type = TOKEN_PRINT;
    }
    else if (strcmp(t.text, "int") == 0)
    {
      t.type = TOKEN_TYPE_INT;
    }
    else if (strcmp(t.text, "char") == 0)
    {
      t.type = TOKEN_TYPE_CHAR;
    }
    else if (strcmp(t.text, "str") == 0)
    {
      t.type = TOKEN_TYPE_STR;
    }
    else
    {
      t.type = TOKEN_IDENT;
    }

    return t;
  }

  if (isdigit(peek(l)) || peek(l) == '.')
  {
    do
    {
      advance(l);
    } while (isdigit(peek(l)) || peek(l) == '.');

    t.text = copy_slice(l->src, start, l->cursor);
    t.number = strtod(t.text, 0);
    t.type = TOKEN_NUMBER;
    return t;
  }

  if (peek(l) == '/')
  {
    advance(l);
    if (peek(l) == '/')
    {
      do
      {
        advance(l);
      } while (peek(l) != '\0' && peek(l) != '\n' && peek(l) != '\r');

      if (peek(l) != '\0')
      {
        return next_token(l);
      }
    }

    t.text = copy_slice(l->src, start, l->cursor);
    t.type = TOKEN_SLASH;
    return t;
  }

  if (peek(l) == '\'')
  {

    advance(l);

    size_t len = 0;
    while (peek(l) != '\0' && peek(l) != '\n' && peek(l) != '\r' &&
           peek(l) != '\'')
    {
      if (peek(l) == '\\')
      {
        advance(l);
        if (peek(l) == '\0' || peek(l) == '\n' || peek(l) == '\r')
        {
          t.type = TOKEN_ERROR;
          t.text = "unterminated escape";
          return t;
        }
        advance(l);
        len += 1;
      }
      else
      {
        advance(l);
        len += 1;
      }
    }

    int did_close = peek(l) == '\'';
    if (!did_close || len != 1)
    {
      t.type = TOKEN_ERROR;
      t.text = "not a valid char";
      if (did_close)
        advance(l);
      return t;
    }

    advance(l);
    t.text = copy_slice(l->src, start + 1, start + 1 + len);
    t.type = TOKEN_CHAR;
    return t;
  }

  if (peek(l) == '"')
  {
    advance(l);

    size_t start_content = l->cursor;
    while (peek(l) != '\0' && peek(l) != '\n' && peek(l) != '\r' &&
           peek(l) != '"')
    {
      if (peek(l) == '\\')
      {
        advance(l);
        if (peek(l) == '\0' || peek(l) == '\n' || peek(l) == '\r')
        {
          t.type = TOKEN_ERROR;
          t.text = "unterminated escape in string";
          return t;
        }
        advance(l);
      }
      else
      {
        advance(l);
      }
    }

    if (peek(l) != '"')
    {
      t.type = TOKEN_ERROR;
      t.text = "unterminated string literal";
      return t;
    }

    advance(l);

    t.type = TOKEN_STRING;
    t.text = copy_slice(l->src, start_content, l->cursor - 1);
    return t;
  }

  if (peek(l) == ':')
  {
    advance(l);
    t.text = copy_slice(l->src, start, l->cursor);
    t.type = TOKEN_COLON;
    return t;
  }

  if (peek(l) == ';')
  {
    advance(l);
    t.text = copy_slice(l->src, start, l->cursor);
    t.type = TOKEN_SEMI;
    return t;
  }

  if (peek(l) == '=')
  {
    advance(l);
    t.text = copy_slice(l->src, start, l->cursor);
    t.type = TOKEN_ASSIGN;
    return t;
  }

  if (peek(l) == '(')
  {
    advance(l);
    t.text = copy_slice(l->src, start, l->cursor);
    t.type = TOKEN_LPAREN;
    return t;
  }

  if (peek(l) == ')')
  {
    advance(l);
    t.text = copy_slice(l->src, start, l->cursor);
    t.type = TOKEN_RPAREN;
    return t;
  }

  t.type = TOKEN_UNKNOWN;
  t.text = copy_slice(l->src, start, start + 1);
  advance(l);
  return t;
}
