#ifndef TYNA_PARSER_INTERNAL_H
#define TYNA_PARSER_INTERNAL_H

#include "tyna/parser.h"

/* parser_core.c */
Token *parser_token_advance(Parser *p);
Token parser_token_peek(Lexer *lexer, int n);
int parser_expect(Parser *p, TokenType type, const char *msg);
void parser_sync(Parser *p);
Type *parser_find_placeholder(Parser *p, StringView name);
Type *parser_add_placeholder(Parser *p, StringView name);
void parser_placeholder_scope_push(Parser *p);
void parser_placeholder_scope_pop(Parser *p);

/* parser_expr.c */
bool parser_is_generic_call_start(Parser *p);
AstNode *parser_parse_expression(Parser *p, int min_bp);

/* parser_expr_primary.c */
AstNode *parser_parse_primary(Parser *p);

/* parser_expr_postfix.c */
AstNode *parser_make_postfix(Parser *p, AstNode *expr);

/* parser_type.c */
PrimitiveKind parser_parse_type(Parser *p);
Type *parser_parse_type_full(Parser *p);
Type *parser_resolve_named_type(Parser *p, StringView name,
                                bool create_if_missing);
bool parser_add_type_alias(Parser *p, StringView alias, Type *target,
                           Location loc);

/* parser_stmt_misc.c */
bool parser_parse_visibility_modifier(Parser *p, bool *is_export,
                                     bool *is_pub_module,
                                     bool *is_external);
AstNode *parser_parse_return_stmt(Parser *p, bool require_semi);
AstNode *parser_parse_statement(Parser *p);
AstNode *parser_parse_block(Parser *p);

/* parser_stmt_decl.c */
AstNode *parser_parse_var_decl(Parser *p, bool is_export);
AstNode *parser_parse_fn_decl(Parser *p, bool is_static, bool is_export,
                              bool is_pub_module, bool is_external);
AstNode *parser_parse_type_alias(Parser *p, bool is_export);
AstNode *parser_parse_struct_decl(Parser *p, bool is_frozen, bool is_export);
AstNode *parser_parse_union_decl(Parser *p, bool is_frozen, bool is_export);
AstNode *parser_parse_error_decl(Parser *p, bool is_export);
AstNode *parser_parse_error_set_decl(Parser *p, bool is_export);
AstNode *parser_parse_impl_decl(Parser *p);

/* parser_stmt_control.c */
AstNode *parser_parse_if_stmt(Parser *p);
AstNode *parser_parse_while_stmt(Parser *p);
AstNode *parser_parse_for_stmt(Parser *p);
AstNode *parser_parse_switch_stmt(Parser *p);

#endif // TYNA_PARSER_INTERNAL_H
