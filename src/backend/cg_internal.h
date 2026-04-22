#ifndef CODEGEN_PRIVATE_H
#define CODEGEN_PRIVATE_H

#include "tyl/codegen.h"
#include "tyl/errors.h"
#include "tyl/type.h"
#include "tyl/utils.h"

#include <llvm-c/Core.h>
#include <stdbool.h>

typedef struct CgSym {
  StringView name;
  Type *type;
  LLVMValueRef value;
  bool is_direct_value;
} CgSym;

typedef struct CgSymtab {
  struct CgSymtab *parent;
  List symbols; // List<CgSym*>
} CgSymtab;

typedef struct CgFunc {
  StringView name;
  LLVMValueRef value;
  LLVMTypeRef type;
  AstNode *decl;

  bool is_system;
} CgFunc;

struct Codegen {
  LLVMContextRef context;
  LLVMModuleRef module;
  LLVMBuilderRef builder;

  ErrorHandler *eh;
  TypeContext *type_ctx;

  CgSymtab *current_scope;
  List defers; // List<List*> - A stack of defer lists per block level

  LLVMValueRef current_function;
  CgFunc *current_function_ref;

  List functions;        // user-defined
  List system_functions; // printf, pow, etc.

  List string_pool;              // List<char*>
  List struct_types_in_progress; // Type* values being lowered to LLVM

  List string_globals; // List<LLVMValueRef>

  // Format string map for deduplication
  List format_strings; // List<struct { Type* t; LLVMValueRef val; }>

  List break_stack;    // List<LLVMBasicBlockRef>
  List continue_stack; // List<LLVMBasicBlockRef>
  bool current_function_uses_arena;
};

// ===== symbol table =====

void CGSymbolTable_init(CgSymtab *t, CgSymtab *parent);
void CGSymbolTable_add(CgSymtab *t, StringView name, Type *type,
                       LLVMValueRef value);
void CGSymbolTable_add_direct(CgSymtab *t, StringView name, Type *type,
                              LLVMValueRef value);
CgSym *CGSymbolTable_find(CgSymtab *t, StringView name);

CgSymtab *cg_push_scope(Codegen *cg);
void cg_pop_scope(Codegen *cg);

// ===== types =====

LLVMTypeRef cg_type_get_llvm(Codegen *cg, Type *t);
/// Tag index for `error_type` within `result_type`'s error set (1-based), or 0.
int cg_result_error_tag_index(Type *result_type, Type *error_type);
void cg_type_lower_structs(Codegen *cg);
LLVMValueRef cg_cast_value(Codegen *cg, LLVMValueRef value, Type *from_ty,
                           LLVMTypeRef to_ty);
LLVMValueRef cg_make_tagged_union(Codegen *cg, LLVMValueRef value,
                                  Type *from_ty, Type *union_ty);
void cg_binary_sync_types(Codegen *cg, LLVMValueRef *lhs, Type *l_ty,
                          LLVMValueRef *rhs, Type *r_ty);
CgFunc *cg_find_system_function(Codegen *cg, StringView name);
// ===== expressions / statements =====

LLVMValueRef cg_expression(Codegen *cg, AstNode *node);
void cg_statement(Codegen *cg, AstNode *node);

// ===== functions =====

void cg_init_CGFunction(CgFunc *f, StringView name, LLVMValueRef value,
                        LLVMTypeRef type, bool is_system, AstNode *decl);
void cg_define_function(Codegen *cg, AstNode *node);
void cg_emit_function_body(Codegen *cg, AstNode *node);
CgFunc *cg_find_function(Codegen *cg, StringView name);
CgFunc *cg_find_system_function(Codegen *cg, StringView name);

size_t cg_string_pool_insert(Codegen *cg, StringView str);

LLVMValueRef cg_get_address(Codegen *cg, AstNode *node);
LLVMValueRef cg_alloca_in_entry(Codegen *cg, Type *type, StringView name);
LLVMValueRef cg_alloca_in_entry_uninitialized(Codegen *cg, Type *type,
                                              StringView name);
LLVMValueRef cg_get_string_constant_ptr(Codegen *cg, StringView str);
LLVMValueRef cg_string_hash(Codegen *cg, LLVMValueRef str_val);
LLVMValueRef cg_string_equals(Codegen *cg, LLVMValueRef a, LLVMValueRef b);
LLVMValueRef cg_equality_expr(Codegen *cg, LLVMValueRef lhs, LLVMValueRef rhs,
                              EqualityOp op, Type *left_ty, Type *right_ty);

bool type_is_array_struct(Type *type);

#endif
