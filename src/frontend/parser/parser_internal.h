#ifndef TYNA_PARSER_INTERNAL_H
#define TYNA_PARSER_INTERNAL_H

#include "tyna/parser.h"

Token *parser_token_advance(Parser *p);
Token parser_token_peek(Lexer *lexer, int n);
int parser_expect(Parser *p, TokenType type, const char *msg);
int Parser_is(AstNode *node, AstKind kind);
void parser_sync(Parser *p);
void parser_sync_param(Parser *p);
void skip_to_next_param(Parser *p);
int is_type_token(TokenType t);
int is_binary_operator(TokenType t);
int parser_is_postfix(Parser *p);
ArithmOp token_to_arithm_op(TokenType type);
CompareOp token_to_compare_op(TokenType type);
EqualityOp token_to_equality_op(TokenType type);
LogicalOp token_to_logical_op(TokenType type);
BindingPower infix_binding_power(TokenType t);
int is_lvalue(AstNode *node);

AstNode *parser_parse_expression(Parser *p, int min_bp);
AstNode *parser_parse_statement(Parser *p);
PrimitiveKind parser_parse_type(Parser *p);
Type *parser_find_placeholder(Parser *p, StringView name);
Type *parser_add_placeholder(Parser *p, StringView name);
void parser_placeholder_scope_push(Parser *p);
void parser_placeholder_scope_pop(Parser *p);
Type *parser_parse_type_full(Parser *p);
AstNode *parser_parse_block(Parser *p);
AstNode *parser_parse_var_decl(Parser *p, bool is_export);
AstNode *parser_parse_print(Parser *p);
AstNode *parser_parse_fn_decl(Parser *p, bool is_static, bool is_export,
                              bool is_external);
AstNode *parser_parse_if_stmt(Parser *p);
AstNode *parser_parse_while_stmt(Parser *p);
AstNode *parser_parse_for_stmt(Parser *p);
AstNode *parser_parse_switch_stmt(Parser *p);
AstNode *parser_parse_struct_decl(Parser *p, bool is_frozen, bool is_export);
AstNode *parser_parse_union_decl(Parser *p, bool is_frozen, bool is_export);
AstNode *parser_parse_error_decl(Parser *p, bool is_export);
AstNode *parser_parse_error_set_decl(Parser *p, bool is_export);
AstNode *parser_parse_impl_decl(Parser *p);

AstNode *parser_parse_call(Parser *p, AstNode *expr, List generic_args);
AstNode *parser_parse_index(Parser *p, AstNode *expr);
AstNode *parser_parse_field(Parser *p, AstNode *expr);
AstNode *parser_parse_postfix_inc(Parser *p, AstNode *expr);
AstNode *parser_make_postfix(Parser *p, AstNode *expr);
AstNode *parser_parse_primary(Parser *p);

#endif // TYNA_PARSER_INTERNAL_H
