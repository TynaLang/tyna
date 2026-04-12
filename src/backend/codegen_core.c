#include <llvm-c/Core.h>
#include <stdio.h>

#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"
#include "tyl/utils.h"

static void cg_register_system_function(Codegen *cg, StringView name,
                                        LLVMTypeRef type) {
  char *c_name = sv_to_cstr(name);
  LLVMValueRef func = LLVMAddFunction(cg->module, c_name, type);
  free(c_name);

  CGFunction *f = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f, name, func, type, true);
  List_push(&cg->system_functions, f);
}

static void cg_initiate_system_functions(Codegen *cg) {
  LLVMTypeRef printf_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef printf_type = LLVMFunctionType(
      LLVMInt32TypeInContext(cg->context), printf_args, 1, true);
  LLVMValueRef printf_val = LLVMAddFunction(cg->module, "printf", printf_type);
  CGFunction *f_print = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_print, sv_from_parts("print", 5), printf_val,
                     printf_type, true);
  List_push(&cg->system_functions, f_print);

  LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
  LLVMTypeRef pow_args[] = {double_ty, double_ty};
  LLVMTypeRef pow_type = LLVMFunctionType(double_ty, pow_args, 2, false);
  LLVMValueRef pow_val = LLVMAddFunction(cg->module, "pow", pow_type);
  CGFunction *f_pow = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_pow, sv_from_parts("pow", 3), pow_val, pow_type, true);
  List_push(&cg->system_functions, f_pow);

  // __tyl_compare_arrays(const FatPtr *a, const FatPtr *b, size_t elem_size)
  LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
  LLVMTypeRef ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  LLVMTypeRef dims_ptr_ty = LLVMPointerType(i64_ty, 0);
  LLVMTypeRef fatptr_ty = LLVMStructTypeInContext(
      cg->context, (LLVMTypeRef[]){i64_ty, ptr_ty, dims_ptr_ty}, 3, false);
  LLVMTypeRef cmp_args[] = {ptr_ty, ptr_ty, i64_ty};
  LLVMTypeRef cmp_type =
      LLVMFunctionType(LLVMInt32TypeInContext(cg->context), cmp_args, 3, false);
  LLVMValueRef cmp_val =
      LLVMAddFunction(cg->module, "__tyl_compare_arrays", cmp_type);
  CGFunction *f_cmp = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_cmp, sv_from_parts("__tyl_compare_arrays", 20), cmp_val,
                     cmp_type, true);
  List_push(&cg->system_functions, f_cmp);

  // void __tyl_str_to_array(FatPtr *out, const FatPtr *str)
  LLVMTypeRef to_arr_args[] = {ptr_ty, ptr_ty};
  LLVMTypeRef to_arr_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->context),
                                             to_arr_args, 2, false);
  LLVMValueRef to_arr_val =
      LLVMAddFunction(cg->module, "__tyl_str_to_array", to_arr_type);
  CGFunction *f_to_arr = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_to_arr, sv_from_parts("__tyl_str_to_array", 18),
                     to_arr_val, to_arr_type, true);
  List_push(&cg->system_functions, f_to_arr);

  // void __tyl_array_to_str(FatPtr *out, const FatPtr *arr)
  LLVMTypeRef to_str_args[] = {ptr_ty, ptr_ty};
  LLVMTypeRef to_str_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->context),
                                             to_str_args, 2, false);
  LLVMValueRef to_str_val =
      LLVMAddFunction(cg->module, "__tyl_array_to_str", to_str_type);
  CGFunction *f_to_str = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_to_str, sv_from_parts("__tyl_array_to_str", 18),
                     to_str_val, to_str_type, true);
  List_push(&cg->system_functions, f_to_str);

  LLVMTypeRef string_ty =
      cg_get_llvm_type(cg, cg->type_ctx->primitives[PRIM_STRING]);

  // char* __tyl_str_data(const String *s)
  LLVMTypeRef str_data_args[] = {LLVMPointerType(string_ty, 0)};
  LLVMTypeRef str_data_type =
      LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
                       str_data_args, 1, false);
  LLVMValueRef str_data_val =
      LLVMAddFunction(cg->module, "__tyl_str_data", str_data_type);
  CGFunction *f_str_data = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_str_data, sv_from_parts("__tyl_str_data", 14),
                     str_data_val, str_data_type, true);
  List_push(&cg->system_functions, f_str_data);

  // int64 __tyl_str_len(const String *s)
  LLVMTypeRef str_len_args[] = {LLVMPointerType(string_ty, 0)};
  LLVMTypeRef str_len_type = LLVMFunctionType(i64_ty, str_len_args, 1, false);
  LLVMValueRef str_len_val =
      LLVMAddFunction(cg->module, "__tyl_str_len", str_len_type);
  CGFunction *f_str_len = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_str_len, sv_from_parts("__tyl_str_len", 13), str_len_val,
                     str_len_type, true);
  List_push(&cg->system_functions, f_str_len);

  // void __tyl_str_retain(String *out, const String *s)
  LLVMTypeRef str_retain_args[] = {LLVMPointerType(string_ty, 0),
                                   LLVMPointerType(string_ty, 0)};
  LLVMTypeRef str_retain_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), str_retain_args, 2, false);
  LLVMValueRef str_retain_val =
      LLVMAddFunction(cg->module, "__tyl_str_retain", str_retain_type);
  CGFunction *f_str_retain = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_str_retain, sv_from_parts("__tyl_str_retain", 16),
                     str_retain_val, str_retain_type, true);
  List_push(&cg->system_functions, f_str_retain);

  // void __tyl_str_release(const String *s)
  LLVMTypeRef str_release_args[] = {LLVMPointerType(string_ty, 0)};
  LLVMTypeRef str_release_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), str_release_args, 1, false);
  LLVMValueRef str_release_val =
      LLVMAddFunction(cg->module, "__tyl_str_release", str_release_type);
  CGFunction *f_str_release = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_str_release, sv_from_parts("__tyl_str_release", 17),
                     str_release_val, str_release_type, true);
  List_push(&cg->system_functions, f_str_release);

  // void __tyl_array_init_fixed(void *arr, i64 elem_size, i64 len)
  LLVMTypeRef init_fixed_args[] = {ptr_ty, i64_ty, i64_ty};
  LLVMTypeRef init_fixed_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), init_fixed_args, 3, false);
  LLVMValueRef init_fixed_val =
      LLVMAddFunction(cg->module, "__tyl_array_init_fixed", init_fixed_type);
  CGFunction *f_init_fixed = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_init_fixed, sv_from_cstr("__tyl_array_init_fixed"),
                     init_fixed_val, init_fixed_type, true);
  List_push(&cg->system_functions, f_init_fixed);

  // void panic(const char *msg, ...)
  LLVMTypeRef panic_args[] = {ptr_ty};
  LLVMTypeRef panic_type =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), panic_args, 1, true);
  LLVMValueRef panic_val = LLVMAddFunction(cg->module, "panic", panic_type);
  CGFunction *f_panic = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(f_panic, sv_from_parts("panic", 5), panic_val, panic_type,
                     true);
  List_push(&cg->system_functions, f_panic);
}

static bool cg_has_main(AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *child = root->ast_root.children.items[i];
    if (child->tag == NODE_FUNC_DECL &&
        sv_eq_cstr(child->func_decl.name->var.value, "main")) {
      return true;
    }
  }
  return false;
}

static LLVMValueRef cg_create_entry(Codegen *cg, bool has_main) {
  LLVMValueRef entry_func = NULL;
  if (has_main) {
    LLVMTypeRef init_type =
        LLVMFunctionType(LLVMVoidTypeInContext(cg->context), NULL, 0, 0);
    entry_func = LLVMAddFunction(cg->module, "init", init_type);
  } else {
    LLVMTypeRef main_type =
        LLVMFunctionType(LLVMInt32TypeInContext(cg->context), NULL, 0, 0);
    entry_func = LLVMAddFunction(cg->module, "main", main_type);
  }
  return entry_func;
}

static void cg_finalize_entry(Codegen *cg, LLVMValueRef entry_func,
                              bool has_main) {
  LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlockInContext(
                                            cg->context, entry_func, "entry"));

  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    if (has_main) {
      CGFunction *mf = cg_find_function(cg, sv_from_parts("main", 4));
      LLVMBuildCall2(cg->builder, mf->type, mf->value, NULL, 0, "");
      LLVMBuildRetVoid(cg->builder);
    } else {
      LLVMBuildRet(cg->builder,
                   LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0));
    }
  }
}

static void cg_declare_functions(Codegen *cg, AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      cg_define_function(cg, node);
    } else if (node->tag == NODE_STRUCT_DECL) {
      for (size_t j = 0; j < node->struct_decl.members.len; j++) {
        AstNode *member = node->struct_decl.members.items[j];
        if (member->tag == NODE_FUNC_DECL) {
          cg_define_function(cg, member);
        }
      }
    } else if (node->tag == NODE_IMPL_DECL) {
      for (size_t j = 0; j < node->impl_decl.members.len; j++) {
        AstNode *member = node->impl_decl.members.items[j];
        if (member->tag == NODE_FUNC_DECL) {
          cg_define_function(cg, member);
        }
      }
    }
  }
}

static void cg_emit_functions(Codegen *cg, AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      cg_emit_function_body(cg, node);
    } else if (node->tag == NODE_STRUCT_DECL) {
      for (size_t j = 0; j < node->struct_decl.members.len; j++) {
        AstNode *member = node->struct_decl.members.items[j];
        if (member->tag == NODE_FUNC_DECL) {
          cg_emit_function_body(cg, member);
        }
      }
    } else if (node->tag == NODE_IMPL_DECL) {
      for (size_t j = 0; j < node->impl_decl.members.len; j++) {
        AstNode *member = node->impl_decl.members.items[j];
        if (member->tag == NODE_FUNC_DECL) {
          cg_emit_function_body(cg, member);
        }
      }
    }
  }
}

static void cg_emit_global_statements(Codegen *cg, AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];

    if (node->tag == NODE_FUNC_DECL || node->tag == NODE_STRUCT_DECL ||
        node->tag == NODE_IMPL_DECL)
      continue;

    cg_statement(cg, node);
  }
}

Codegen *Codegen_new(const char *module_name, TypeContext *type_ctx) {
  Codegen *cg = xmalloc(sizeof(Codegen));
  cg->context = LLVMContextCreate();
  cg->module = LLVMModuleCreateWithNameInContext(module_name, cg->context);
  cg->builder = LLVMCreateBuilderInContext(cg->context);
  cg->type_ctx = type_ctx;
  List_init(&cg->defers);
  cg->current_scope = xmalloc(sizeof(CGSymbolTable));
  CGSymbolTable_init(cg->current_scope, NULL);
  cg->current_function_ref = NULL;

  List_init(&cg->functions);
  List_init(&cg->system_functions);
  List_init(&cg->format_strings);
  List_init(&cg->string_pool);
  List_init(&cg->string_globals);
  List_init(&cg->break_stack);
  List_init(&cg->continue_stack);

  cg_initiate_system_functions(cg);

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMTypeRef entry_ty = LLVMFunctionType(i32_ty, NULL, 0, 0);
  LLVMValueRef entry_func = LLVMAddFunction(cg->module, "main", entry_ty);

  LLVMBasicBlockRef entry_bb =
      LLVMAppendBasicBlockInContext(cg->context, entry_func, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  CGFunction *entry_fn = xmalloc(sizeof(CGFunction));
  cg_init_CGFunction(entry_fn, sv_from_cstr("__system__main__"), entry_func,
                     entry_ty, true);

  List_push(&cg->system_functions, entry_fn);

  return cg;
}

void Codegen_global(Codegen *cg, AstNode *ast_root) {
  if (ast_root->tag != NODE_AST_ROOT)
    panic("Expected AST root node");

  CGFunction *entry_fn = cg_find_function(cg, sv_from_cstr("__system__main__"));

  if (!entry_fn) {
    Codegen_dump(cg);
    panic("[Codegen_global] Internal error: entry function not found");
  }

  cg_lower_all_structs(cg);

  cg_declare_functions(cg, ast_root);

  CGFunction *prev_func_ref = cg->current_function_ref;
  LLVMValueRef prev_func = cg->current_function;

  cg->current_function = entry_fn->value;
  cg->current_function_ref = entry_fn;

  LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(entry_fn->value);
  LLVMPositionBuilderAtEnd(cg->builder, bb);

  cg_emit_global_statements(cg, ast_root);

  cg->current_function = prev_func;
  cg->current_function_ref = prev_func_ref;

  cg_emit_functions(cg, ast_root);
}

void Codegen_program(Codegen *cg, AstNode *ast_root) {
  if (ast_root->tag != NODE_AST_ROOT)
    panic("Expected AST root node");

  CGFunction *entry_fn = cg_find_function(cg, sv_from_cstr("__system__main__"));
  if (!entry_fn)
    panic("[Codegen_program] Internal error: entry function not found");

  LLVMValueRef entry_func = entry_fn->value;

  cg->current_function = entry_func;
  cg->current_function_ref = entry_fn;

  cg_push_scope(cg);

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMValueRef exit_code = LLVMConstInt(i32_ty, 0, 0);

  CGFunction *user_main = cg_find_function(cg, sv_from_cstr("main"));

  if (user_main) {
    LLVMTypeRef return_ty = LLVMGetReturnType(user_main->type);
    bool is_void = (LLVMGetTypeKind(return_ty) == LLVMVoidTypeKind);

    const char *call_name = is_void ? "" : "ret";

    LLVMValueRef ret_val = LLVMBuildCall2(cg->builder, user_main->type,
                                          user_main->value, NULL, 0, call_name);

    if (!is_void) {
      exit_code = cg_cast_value(cg, ret_val, NULL, i32_ty);
    }
  }

  cg_pop_scope(cg);

  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    LLVMBuildRet(cg->builder, exit_code);
  }

  free(cg->current_function_ref);
  cg->current_function_ref = NULL;
}

void Codegen_dump(Codegen *cg) { LLVMDumpModule(cg->module); }

void Codegen_free(Codegen *cg) {
  for (size_t i = 0; i < cg->format_strings.len; i++) {
    free(cg->format_strings.items[i]);
  }
  List_free(&cg->format_strings, 0);

  for (size_t i = 0; i < cg->string_pool.len; i++) {
    free(cg->string_pool.items[i]);
  }
  List_free(&cg->string_pool, 0);
  List_free(&cg->string_globals, 0);

  LLVMDisposeBuilder(cg->builder);
  LLVMDisposeModule(cg->module);
  LLVMContextDispose(cg->context);
  free(cg);
}

size_t cg_string_pool_insert(Codegen *cg, StringView str) {
  for (size_t i = 0; i < cg->string_pool.len; i++) {
    char *existing = cg->string_pool.items[i];
    if (strlen(existing) == str.len &&
        memcmp(existing, str.data, str.len) == 0) {
      return i;
    }
  }

  char *copy = sv_to_cstr(str);
  List_push(&cg->string_pool, copy);

  // Generate the global variable immediately
  char name[32];
  sprintf(name, ".str.%zu", cg->string_pool.len - 1);

  LLVMValueRef const_str =
      LLVMConstStringInContext(cg->context, copy, str.len + 1, false);
  LLVMValueRef global = LLVMAddGlobal(cg->module, LLVMTypeOf(const_str), name);
  LLVMSetInitializer(global, const_str);
  LLVMSetGlobalConstant(global, true);
  LLVMSetLinkage(global, LLVMPrivateLinkage);
  LLVMSetUnnamedAddr(global, true);

  List_push(&cg->string_globals, (void *)global);

  return cg->string_pool.len - 1;
}
