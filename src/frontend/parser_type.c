#include "parser_internal.h"

Type *Parser_parse_type_full(Parser *p) {
  Type *res = NULL;
  int pointer_depth = 0;

  while (p->current_token.type == TOKEN_STAR) {
    pointer_depth++;
    Parser_token_advance(p);
  }

  if (p->current_token.type == TOKEN_LBRACKET) {
    Parser_token_advance(p);
    Type *elementType = Parser_parse_type_full(p);
    if (!elementType)
      return NULL;

    size_t array_len = 0;
    if (p->current_token.type == TOKEN_SEMI) {
      Parser_token_advance(p);
      AstNode *len_expr = Parser_parse_expression(p, 0);
      if (!len_expr || len_expr->tag != NODE_NUMBER ||
          sv_contains(len_expr->number.raw_text, '.') ||
          sv_ends_with(len_expr->number.raw_text, "f") ||
          sv_ends_with(len_expr->number.raw_text, "F")) {
        ErrorHandler_report(p->eh, p->current_token.loc,
                            "Array length must be an integer literal");
        return NULL;
      }
      array_len = (size_t)len_expr->number.value;
    }

    if (!Parser_expect(p, TOKEN_RBRACKET, "Expected ']' for array type"))
      return NULL;

    if (array_len == 0) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Fixed array length must be greater than zero");
      return NULL;
    }

    Type *array_template =
        type_get_template(p->type_ctx, sv_from_cstr("Array"));
    if (!array_template) {
      ErrorHandler_report(p->eh, p->current_token.loc,
                          "Internal error: 'Array' template not registered");
      return NULL;
    }

    List args;
    List_init(&args);
    List_push(&args, elementType);
    res = type_get_instance_fixed(p->type_ctx, array_template, args,
                                  (uint64_t)array_len);
    List_free(&args, 0);
  } else {
    PrimitiveKind kind = Token_token_to_type(p->current_token.type);
    if (kind != PRIM_UNKNOWN) {
      Parser_token_advance(p);
      res = type_get_primitive(p->type_ctx, kind);
    } else if (p->current_token.type == TOKEN_IDENT) {
      StringView name = p->current_token.text;
      Parser_token_advance(p);
      res = type_get_struct(p->type_ctx, name);
    }
  }

  if (!res) {
    ErrorHandler_report(p->eh, p->current_token.loc, "Expected type");
    return NULL;
  }

  while (pointer_depth-- > 0) {
    res = type_get_pointer(p->type_ctx, res);
  }

  return res;
}

PrimitiveKind Parser_parse_type(Parser *p) {
  PrimitiveKind kind = Token_token_to_type(p->current_token.type);
  if (kind != PRIM_UNKNOWN)
    Parser_token_advance(p);
  return kind;
}
