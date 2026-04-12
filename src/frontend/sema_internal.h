#ifndef TYL_SEMA_INTERNAL_H
#define TYL_SEMA_INTERNAL_H

#include "tyl/semantic.h"

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

// Sema Stmt
void sema_scope_push(Sema *s);
void sema_scope_pop(Sema *s);
void sema_check_stmt(Sema *s, AstNode *node);

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