#ifndef TYL_PARSER_INTERNAL_H
#define TYL_PARSER_INTERNAL_H

#include "tyl/parser.h"

Token *Parser_token_advance(Parser *p);
Token Parser_token_peek(Lexer *lexer, int n);
int Parser_expect(Parser *p, TokenType type, const char *msg);
int Parser_is(AstNode *node, AstKind kind);
void Parser_sync(Parser *p);
void Parser_sync_param(Parser *p);
void skip_to_next_param(Parser *p);
int is_type_token(TokenType t);
int is_binary_operator(TokenType t);
int is_postfix(Token t);
ArithmOp token_to_arithm_op(TokenType type);
CompareOp token_to_compare_op(TokenType type);
EqualityOp token_to_equality_op(TokenType type);
LogicalOp token_to_logical_op(TokenType type);
BindingPower infix_binding_power(TokenType t);
int is_lvalue(AstNode *node);

AstNode *Parser_parse_expression(Parser *p, int min_bp);
AstNode *Parser_parse_statement(Parser *p);
PrimitiveKind Parser_parse_type(Parser *p);
Type *Parser_find_placeholder(Parser *p, StringView name);
Type *Parser_add_placeholder(Parser *p, StringView name);
void Parser_placeholder_scope_push(Parser *p);
void Parser_placeholder_scope_pop(Parser *p);
Type *Parser_parse_type_full(Parser *p);
AstNode *Parser_parse_block(Parser *p);
AstNode *Parser_parse_var_decl(Parser *p, bool is_export);
AstNode *Parser_parse_print(Parser *p);
AstNode *Parser_parse_fn_decl(Parser *p, bool is_static, bool is_export,
                              bool is_external);
AstNode *Parser_parse_if_stmt(Parser *p);
AstNode *Parser_parse_while_stmt(Parser *p);
AstNode *Parser_parse_for_stmt(Parser *p);
AstNode *Parser_parse_switch_stmt(Parser *p);
AstNode *Parser_parse_struct_decl(Parser *p, bool is_frozen, bool is_export);
AstNode *Parser_parse_union_decl(Parser *p, bool is_frozen, bool is_export);
AstNode *Parser_parse_impl_decl(Parser *p);

AstNode *Parser_parse_call(Parser *p, AstNode *expr);
AstNode *Parser_parse_index(Parser *p, AstNode *expr);
AstNode *Parser_parse_field(Parser *p, AstNode *expr);
AstNode *Parser_parse_postfix_inc(Parser *p, AstNode *expr);
AstNode *Parser_make_postfix(Parser *p, AstNode *expr);
AstNode *Parser_parse_primary(Parser *p);

#endif // TYL_PARSER_INTERNAL_H
