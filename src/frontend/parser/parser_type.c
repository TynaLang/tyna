#include "parser_internal.h"

Type *parser_parse_type_full(Parser *p) {
  Type *res = NULL;
  int pointer_depth = 0;

  while (p->current_token.type == TOKEN_CONST) {
    parser_token_advance(p);
  }

  while (p->current_token.type == TOKEN_STAR) {
    pointer_depth++;
    parser_token_advance(p);
    while (p->current_token.type == TOKEN_CONST) {
      parser_token_advance(p);
    }
  }

  if (p->current_token.type == TOKEN_LBRACKET) {
    parser_token_advance(p);
    Type *elementType = parser_parse_type_full(p);
    if (!elementType)
      return NULL;

    size_t array_len = 0;
    bool has_fixed_length = false;
    if (p->current_token.type == TOKEN_SEMI) {
      has_fixed_length = true;
      parser_token_advance(p);
      AstNode *len_expr = parser_parse_expression(p, 0);
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

    if (!parser_expect(p, TOKEN_RBRACKET, "Expected ']' for array type"))
      return NULL;

    if (has_fixed_length && array_len == 0) {
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

    res = type_get_array(p->type_ctx, elementType,
                         has_fixed_length ? (uint64_t)array_len : 0);
  } else {
    PrimitiveKind kind = Token_token_to_type(p->current_token.type);
    if (kind != PRIM_UNKNOWN) {
      parser_token_advance(p);
      res = type_get_primitive(p->type_ctx, kind);
    } else if (p->current_token.type == TOKEN_IDENT) {
      StringView name = p->current_token.text;
      Type *placeholder = parser_find_placeholder(p, name);
      parser_token_advance(p);

      if (placeholder) {
        return placeholder;
      }

      if (p->current_token.type == TOKEN_LT) {
        parser_token_advance(p);
        List args;
        List_init(&args);

        while (true) {
          Type *arg_type = parser_parse_type_full(p);
          if (!arg_type) {
            List_free(&args, 0);
            return NULL;
          }
          List_push(&args, arg_type);

          if (p->current_token.type == TOKEN_COMMA) {
            parser_token_advance(p);
            continue;
          }
          break;
        }

        if (!parser_expect(p, TOKEN_GT, "Expected '>' after generic type")) {
          List_free(&args, 0);
          return NULL;
        }

        if (sv_eq_cstr(name, "ptr")) {
          if (args.len != 1) {
            ErrorHandler_report(p->eh, p->current_token.loc,
                                "ptr<> must have exactly one type argument");
            List_free(&args, 0);
            return NULL;
          }
          Type *target = args.items[0];
          res = type_get_pointer(p->type_ctx, target);
          List_free(&args, 0);
        } else {
          Type *template_type = type_get_template(p->type_ctx, name);
          if (!template_type) {
            res = type_get_struct(p->type_ctx, name);
            if (!res)
              res = type_get_union(p->type_ctx, name);
          } else {
            res = type_get_instance(p->type_ctx, template_type, args);
          }
        }
      } else {
        if (sv_eq_cstr(name, "String")) {
          res = type_get_string_buffer(p->type_ctx);
        } else if (sv_eq_cstr(name, "Error")) {
          res = type_get_error_set_anonymous(p->type_ctx);
        } else {
          res = type_get_named(p->type_ctx, name);
          if (!res)
            res = type_get_struct(p->type_ctx, name);
        }
      }
    }
  }

  if (!res) {
    ErrorHandler_report(p->eh, p->current_token.loc, "Expected type");
    return NULL;
  }

  while (pointer_depth-- > 0) {
    res = type_get_pointer(p->type_ctx, res);
  }

  if (p->current_token.type == TOKEN_NOT) {
    parser_token_advance(p);
    Type *error_set = parser_parse_type_full(p);
    if (!error_set)
      return NULL;
    res = type_get_result(p->type_ctx, res, error_set);
  }

  if (p->current_token.type == TOKEN_BIT_OR) {
    List union_members;
    List_init(&union_members);

    if (res->kind == KIND_UNION) {
      for (size_t i = 0; i < res->members.len; i++) {
        Member *m = res->members.items[i];
        List_push(&union_members, m->type);
      }
    } else {
      List_push(&union_members, res);
    }

    while (p->current_token.type == TOKEN_BIT_OR) {
      parser_token_advance(p);
      Type *next_type = parser_parse_type_full(p);
      if (!next_type) {
        List_free(&union_members, 0);
        return NULL;
      }
      if (next_type->kind == KIND_UNION) {
        for (size_t i = 0; i < next_type->members.len; i++) {
          Member *m = next_type->members.items[i];
          List_push(&union_members, m->type);
        }
      } else {
        List_push(&union_members, next_type);
      }
    }

    res = type_get_union_anonymous(p->type_ctx, union_members);
    List_free(&union_members, 0);
  }

  while (pointer_depth-- > 0) {
    res = type_get_pointer(p->type_ctx, res);
  }

  return res;
}

PrimitiveKind parser_parse_type(Parser *p) {
  PrimitiveKind kind = Token_token_to_type(p->current_token.type);
  if (kind != PRIM_UNKNOWN)
    parser_token_advance(p);
  return kind;
}
