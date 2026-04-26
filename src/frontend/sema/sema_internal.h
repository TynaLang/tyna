#ifndef TYNA_SEMA_INTERNAL_H
#define TYNA_SEMA_INTERNAL_H

#include "tyna/sema.h"

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
typedef enum ValueCategory {
  VAL_RVALUE,
  VAL_LVALUE,
} ValueCategory;

typedef struct ExprInfo {
  Type *type;
  ValueCategory category;
} ExprInfo;

AstNode *sema_coerce(Sema *s, AstNode *expr, Type *target);
ExprInfo sema_check_expr(Sema *s, AstNode *node);
bool sema_is_writable_address(Sema *s, AstNode *node);
bool type_is_array_struct(Type *type);
bool type_is_slice_struct(Type *type);
bool error_set_contains(Type *set, Type *error);
Type *sema_find_type_by_name(Sema *s, StringView name);
Symbol *sema_instantiate_method_symbol(Sema *s, Type *obj_type, Symbol *method,
                                       AstNode *field_node);
bool sema_is_consuming_method(Symbol *method);
void sema_mark_current_function_consumes_string_arg(Sema *s);
void sema_mark_move_if_consuming_arg(Sema *s, AstNode *arg, Type *param_type,
                                     bool force_move);
Type *sema_check_builtin_call(Sema *s, AstNode *node);
bool sema_is_string_literal(AstNode *node);
void sema_fold_string_concat_literals(Sema *s, AstNode *node);
bool sema_try_resolve_method_call(Sema *s, AstNode *node);
ExprInfo sema_check_field(Sema *s, AstNode *node);
ExprInfo sema_check_static_member(Sema *s, AstNode *node);
ExprInfo sema_check_call(Sema *s, AstNode *node);
ExprInfo sema_check_index(Sema *s, AstNode *node);
ExprInfo sema_check_array_expr(Sema *s, AstNode *node);
ExprInfo sema_check_unary(Sema *s, AstNode *node);
ExprInfo sema_check_binary_arith(Sema *s, AstNode *node);
ExprInfo sema_check_binary_logical(Sema *s, AstNode *node);
ExprInfo sema_check_assignment(Sema *s, AstNode *node);
ExprInfo sema_check_cast(Sema *s, AstNode *node);
ExprInfo sema_check_ternary(Sema *s, AstNode *node);
ExprInfo sema_check_new_expr(Sema *s, AstNode *node);
ExprInfo sema_check_new_error_expr(Sema *s, AstNode *node);
ExprInfo sema_check_new_struct_expr(Sema *s, AstNode *node);
ExprInfo sema_check_binary_is(Sema *s, AstNode *node);
ExprInfo sema_check_binary_else(Sema *s, AstNode *node);
ExprInfo sema_check_expr_cache(Sema *s, AstNode *node);
bool sema_fn_decl_can_use_arena(AstNode *fn_decl);

// Sema Stmt
void sema_scope_push(Sema *s);
void sema_scope_pop(Sema *s);
void sema_jump_push(Sema *s, SemaJump *ctx, StringView label, AstNode *node,
                    bool is_loop, bool is_breakable);
void sema_jump_pop(Sema *s);
SemaJump *sema_find_break_jump(Sema *s);
SemaJump *sema_find_loop_jump(Sema *s);

// Sema Type
bool type_needs_inference(Type *t);
bool type_is_condition(Type *t);
bool type_can_be_polymorphic(Type *t);
bool sema_allows_polymorphic_types(Sema *s);
void sema_check_stmt(Sema *s, AstNode *node);

// Statement helpers
void sema_check_import(Sema *s, AstNode *node);
void sema_check_block(Sema *s, AstNode *node);
void sema_check_return_stmt(Sema *s, AstNode *node);
void sema_check_var_decl(Sema *s, AstNode *node);
void sema_check_func_decl(Sema *s, AstNode *node);
void sema_check_if_stmt(Sema *s, AstNode *node);
void sema_check_switch_stmt(Sema *s, AstNode *node);
void sema_check_loop_stmt(Sema *s, AstNode *node);
void sema_check_while_stmt(Sema *s, AstNode *node);
void sema_check_for_stmt(Sema *s, AstNode *node);
void sema_check_for_in_stmt(Sema *s, AstNode *node);
void sema_check_struct_decl(Sema *s, AstNode *node);
void sema_check_union_decl(Sema *s, AstNode *node);
void sema_check_error_decl(Sema *s, AstNode *node);
void sema_check_error_set_decl(Sema *s, AstNode *node);
void sema_check_impl_decl(Sema *s, AstNode *node);

// Literal helpers
bool literal_fits_in_type(AstNode *node, Type *target);
void sema_check_literal_bounds(Sema *s, AstNode *node, Type *target);
ExprInfo sema_check_literal(Sema *s, AstNode *node);

// Variable helpers
ExprInfo sema_check_var(Sema *s, AstNode *node);

AstNode *sema_find_underlying_var(AstNode *node);
void sema_mark_symbol_requires_storage(Symbol *sym);
void sema_mark_moved_symbol(Symbol *sym);
void sema_mark_node_requires_storage(Sema *s, AstNode *node);
void sema_get_drops_for_scope(Sema *s, SemaScope *scope,
                              List *out_symbols_to_drop);

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