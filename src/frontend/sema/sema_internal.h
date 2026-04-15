#ifndef TYL_SEMA_INTERNAL_H
#define TYL_SEMA_INTERNAL_H

#include "tyl/sema.h"

// Sema Core
Symbol *sema_resolve_local(Sema *s, StringView name);
Symbol *sema_define(Sema *s, StringView name, Type *type, bool is_const,
                    Location loc);

// Sema Decl
void sema_register_func_signature(Sema *s, AstNode *node);
void sema_register_method_signature(Sema *s, AstNode *node,
                                    StringView owner_name);
StringView sema_mangle_method_name(StringView owner, StringView method);

// Sema Expr
Type *sema_coerce(Sema *s, AstNode *expr, Type *target);
Type *sema_check_expr(Sema *s, AstNode *node);
bool type_is_array_struct(Type *type);
Type *sema_find_type_by_name(Sema *s, StringView name);
Symbol *sema_instantiate_method_symbol(Sema *s, Type *obj_type, Symbol *method,
                                       AstNode *field_node);
Type *check_field(Sema *s, AstNode *node);
Type *check_static_member(Sema *s, AstNode *node);
Type *check_call(Sema *s, AstNode *node);
Type *check_index(Sema *s, AstNode *node);
Type *check_array_expr(Sema *s, AstNode *node);
Type *check_unary(Sema *s, AstNode *node);
Type *check_binary_arith(Sema *s, AstNode *node);
Type *check_binary_logical(Sema *s, AstNode *node);
Type *check_assignment(Sema *s, AstNode *node);
Type *check_cast(Sema *s, AstNode *node);
Type *check_ternary(Sema *s, AstNode *node);

// Sema Stmt
void sema_scope_push(Sema *s);
void sema_scope_pop(Sema *s);
void sema_jump_push(Sema *s, SemaJump *ctx, StringView label, AstNode *node,
                    bool is_loop, bool is_breakable);
void sema_jump_pop(Sema *s);
SemaJump *sema_find_break_jump(Sema *s);
SemaJump *sema_find_loop_jump(Sema *s);
bool type_needs_inference(Type *t);
bool type_is_condition(Type *t);
bool type_can_be_polymorphic(Type *t);
bool sema_allows_polymorphic_types(Sema *s);
bool type_is_writable(Sema *s, AstNode *target);
void sema_check_stmt(Sema *s, AstNode *node);

// Statement helpers
void sema_check_import(Sema *s, AstNode *node);
void sema_check_var_decl(Sema *s, AstNode *node);
void sema_check_func_decl(Sema *s, AstNode *node);
void sema_check_block(Sema *s, AstNode *node);
void sema_check_return_stmt(Sema *s, AstNode *node);
void sema_check_if_stmt(Sema *s, AstNode *node);
void sema_check_switch_stmt(Sema *s, AstNode *node);
void sema_check_loop_stmt(Sema *s, AstNode *node);
void sema_check_while_stmt(Sema *s, AstNode *node);
void sema_check_for_stmt(Sema *s, AstNode *node);
void sema_check_for_in_stmt(Sema *s, AstNode *node);
void sema_check_struct_decl(Sema *s, AstNode *node);
void sema_check_union_decl(Sema *s, AstNode *node);
void sema_check_impl_decl(Sema *s, AstNode *node);

// Literal helpers
bool literal_fits_in_type(AstNode *node, Type *target);
void check_literal_bounds(Sema *s, AstNode *node, Type *target);
Type *check_literal(Sema *s, AstNode *node);
Type *check_var(Sema *s, AstNode *node);

// Module helpers
Module *module_find_by_path(Sema *s, const char *abs_path);
Module *module_resolve_or_load(Sema *s, StringView import_path,
                               const char *current_file_path);
Symbol *module_lookup_export(Module *module, StringView name);

// Sema Utils
void sema_error(Sema *s, AstNode *n, const char *fmt, ...);
void sema_error_at(Sema *s, Location loc, const char *fmt, ...);
void sema_warning(Sema *s, AstNode *n, const char *fmt, ...);
void sema_warning_at(Sema *s, Location loc, const char *fmt, ...);

#endif