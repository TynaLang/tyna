#include <llvm-c/Core.h>
#include <stdio.h>

#include "cg_internal.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"
#include "tyl/utils.h"

static void cg_register_system_function(Codegen *cg, StringView name,
                                        LLVMTypeRef type) {
  char *c_name = sv_to_cstr(name);
  LLVMValueRef func = LLVMAddFunction(cg->module, c_name, type);
  free(c_name);

  CgFunc *f = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f, name, func, type, true, NULL);
  List_push(&cg->system_functions, f);
}

static LLVMTypeRef cg_get_string_llvm_type(Codegen *cg) {
  const char *name = "tyl_string";
  LLVMTypeRef str_ty = LLVMGetTypeByName2(cg->context, name);
  if (!str_ty) {
    str_ty = LLVMStructCreateNamed(cg->context, name);
  }
  if (LLVMIsOpaqueStruct(str_ty)) {
    LLVMTypeRef fields[2] = {
        LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
        LLVMInt64TypeInContext(cg->context),
    };
    LLVMStructSetBody(str_ty, fields, 2, false);
  }
  return str_ty;
}

static void cg_initiate_system_functions(Codegen *cg) {
  LLVMTypeRef printf_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef printf_type = LLVMFunctionType(
      LLVMInt32TypeInContext(cg->context), printf_args, 1, true);
  LLVMValueRef printf_val = LLVMAddFunction(cg->module, "printf", printf_type);
  CgFunc *f_print = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_print, sv_from_parts("print", 5), printf_val,
                     printf_type, true, NULL);
  List_push(&cg->system_functions, f_print);

  LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
  LLVMTypeRef pow_args[] = {double_ty, double_ty};
  LLVMTypeRef pow_type = LLVMFunctionType(double_ty, pow_args, 2, false);
  LLVMValueRef pow_val = LLVMAddFunction(cg->module, "pow", pow_type);
  CgFunc *f_pow = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_pow, sv_from_parts("pow", 3), pow_val, pow_type, true,
                     NULL);
  List_push(&cg->system_functions, f_pow);

  LLVMTypeRef free_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef free_type =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), free_args, 1, false);
  LLVMValueRef free_val = LLVMAddFunction(cg->module, "free", free_type);
  CgFunc *f_free = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_free, sv_from_parts("free", 4), free_val, free_type,
                     true, NULL);
  List_push(&cg->system_functions, f_free);

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
  CgFunc *f_cmp = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_cmp, sv_from_parts("__tyl_compare_arrays", 20), cmp_val,
                     cmp_type, true, NULL);
  List_push(&cg->system_functions, f_cmp);

  // void __tyl_str_to_array(FatPtr *out, const FatPtr *str)
  LLVMTypeRef to_arr_args[] = {ptr_ty, ptr_ty};
  LLVMTypeRef to_arr_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->context),
                                             to_arr_args, 2, false);
  LLVMValueRef to_arr_val =
      LLVMAddFunction(cg->module, "__tyl_str_to_array", to_arr_type);
  CgFunc *f_to_arr = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_to_arr, sv_from_parts("__tyl_str_to_array", 18),
                     to_arr_val, to_arr_type, true, NULL);
  List_push(&cg->system_functions, f_to_arr);

  // void __tyl_array_to_str(FatPtr *out, const FatPtr *arr)
  LLVMTypeRef to_str_args[] = {ptr_ty, ptr_ty};
  LLVMTypeRef to_str_type = LLVMFunctionType(LLVMVoidTypeInContext(cg->context),
                                             to_str_args, 2, false);
  LLVMValueRef to_str_val =
      LLVMAddFunction(cg->module, "__tyl_array_to_str", to_str_type);
  CgFunc *f_to_str = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_to_str, sv_from_parts("__tyl_array_to_str", 18),
                     to_str_val, to_str_type, true, NULL);
  List_push(&cg->system_functions, f_to_str);

  LLVMTypeRef string_ty = cg_get_string_llvm_type(cg);

  // char* __tyl_str_data(const String *s)
  LLVMTypeRef str_data_args[] = {LLVMPointerType(string_ty, 0)};
  LLVMTypeRef str_data_type =
      LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
                       str_data_args, 1, false);
  LLVMValueRef str_data_val =
      LLVMAddFunction(cg->module, "__tyl_str_data", str_data_type);
  CgFunc *f_str_data = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_data, sv_from_parts("__tyl_str_data", 14),
                     str_data_val, str_data_type, true, NULL);
  List_push(&cg->system_functions, f_str_data);

  // int64 __tyl_str_len(const String *s)
  LLVMTypeRef str_len_args[] = {LLVMPointerType(string_ty, 0)};
  LLVMTypeRef str_len_type = LLVMFunctionType(i64_ty, str_len_args, 1, false);
  LLVMValueRef str_len_val =
      LLVMAddFunction(cg->module, "__tyl_str_len", str_len_type);
  CgFunc *f_str_len = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_len, sv_from_parts("__tyl_str_len", 13), str_len_val,
                     str_len_type, true, NULL);
  List_push(&cg->system_functions, f_str_len);

  // i64 __tyl_str_hash(String s)
  LLVMTypeRef str_hash_args[] = {string_ty};
  LLVMTypeRef str_hash_type = LLVMFunctionType(i64_ty, str_hash_args, 1, false);
  LLVMValueRef str_hash_val =
      LLVMAddFunction(cg->module, "__tyl_str_hash", str_hash_type);
  CgFunc *f_str_hash = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_hash, sv_from_cstr("__tyl_str_hash"), str_hash_val,
                     str_hash_type, true, NULL);
  List_push(&cg->system_functions, f_str_hash);

  // i32 __tyl_str_equals(String a, String b)
  LLVMTypeRef str_eq_args[] = {string_ty, string_ty};
  LLVMTypeRef str_eq_type = LLVMFunctionType(
      LLVMInt32TypeInContext(cg->context), str_eq_args, 2, false);
  LLVMValueRef str_eq_val =
      LLVMAddFunction(cg->module, "__tyl_str_equals", str_eq_type);
  CgFunc *f_str_eq = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_eq, sv_from_cstr("__tyl_str_equals"), str_eq_val,
                     str_eq_type, true, NULL);
  List_push(&cg->system_functions, f_str_eq);

  LLVMTypeRef buf_ty =
      cg_type_get_llvm(cg, type_get_string_buffer(cg->type_ctx));
  LLVMTypeRef buf_ptr_ty = LLVMPointerType(buf_ty, 0);

  // void __tyl_string_new(tyl_string_buf *out)
  LLVMTypeRef str_new_args[] = {buf_ptr_ty};
  LLVMTypeRef str_new_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), str_new_args, 1, false);
  LLVMValueRef str_new_val =
      LLVMAddFunction(cg->module, "__tyl_string_new", str_new_type);
  CgFunc *f_str_new = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_new, sv_from_cstr("__tyl_string_new"), str_new_val,
                     str_new_type, true, NULL);
  List_push(&cg->system_functions, f_str_new);

  // void __tyl_string_push(tyl_string_buf *buf, tyl_string piece)
  LLVMTypeRef str_push_args[] = {buf_ptr_ty, string_ty};
  LLVMTypeRef str_push_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), str_push_args, 2, false);
  LLVMValueRef str_push_val =
      LLVMAddFunction(cg->module, "__tyl_string_push", str_push_type);
  CgFunc *f_str_push = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_push, sv_from_cstr("__tyl_string_push"),
                     str_push_val, str_push_type, true, NULL);
  List_push(&cg->system_functions, f_str_push);

  // void __tyl_string_free(tyl_string_buf *buf)
  LLVMTypeRef str_free_buf_args[] = {buf_ptr_ty};
  LLVMTypeRef str_free_buf_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), str_free_buf_args, 1, false);
  LLVMValueRef str_free_buf_val =
      LLVMAddFunction(cg->module, "__tyl_string_free", str_free_buf_type);
  CgFunc *f_str_free_buf = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_free_buf, sv_from_cstr("__tyl_string_free"),
                     str_free_buf_val, str_free_buf_type, true, NULL);
  List_push(&cg->system_functions, f_str_free_buf);

  // tyl_string __tyl_string_into_str(tyl_string_buf *buf)
  LLVMTypeRef str_into_args[] = {buf_ptr_ty};
  LLVMTypeRef str_into_type =
      LLVMFunctionType(string_ty, str_into_args, 1, false);
  LLVMValueRef str_into_val =
      LLVMAddFunction(cg->module, "__tyl_string_into_str", str_into_type);
  CgFunc *f_str_into = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_into, sv_from_cstr("__tyl_string_into_str"),
                     str_into_val, str_into_type, true, NULL);
  List_push(&cg->system_functions, f_str_into);

  // tyl_string __tyl_string_clone_str(tyl_string_buf *buf)
  LLVMTypeRef str_clone_args[] = {buf_ptr_ty};
  LLVMTypeRef str_clone_type =
      LLVMFunctionType(string_ty, str_clone_args, 1, false);
  LLVMValueRef str_clone_val =
      LLVMAddFunction(cg->module, "__tyl_string_clone_str", str_clone_type);
  CgFunc *f_str_clone = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_str_clone, sv_from_cstr("__tyl_string_clone_str"),
                     str_clone_val, str_clone_type, true, NULL);
  List_push(&cg->system_functions, f_str_clone);

  // void __tyl_string_arena_push(void)
  LLVMTypeRef arena_push_type =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), NULL, 0, false);
  LLVMValueRef arena_push_val =
      LLVMAddFunction(cg->module, "__tyl_string_arena_push", arena_push_type);
  CgFunc *f_arena_push = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_arena_push, sv_from_cstr("__tyl_string_arena_push"),
                     arena_push_val, arena_push_type, true, NULL);
  List_push(&cg->system_functions, f_arena_push);

  // void __tyl_string_arena_pop(void)
  LLVMTypeRef arena_pop_type =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), NULL, 0, false);
  LLVMValueRef arena_pop_val =
      LLVMAddFunction(cg->module, "__tyl_string_arena_pop", arena_pop_type);
  CgFunc *f_arena_pop = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_arena_pop, sv_from_cstr("__tyl_string_arena_pop"),
                     arena_pop_val, arena_pop_type, true, NULL);
  List_push(&cg->system_functions, f_arena_pop);

  // const char *__tyl_as_c_ptr(tyl_string s)
  LLVMTypeRef as_cptr_args[] = {string_ty};
  LLVMTypeRef as_cptr_type =
      LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
                       as_cptr_args, 1, false);
  LLVMValueRef as_cptr_val =
      LLVMAddFunction(cg->module, "__tyl_as_c_ptr", as_cptr_type);
  CgFunc *f_as_cptr = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_as_cptr, sv_from_cstr("__tyl_as_c_ptr"), as_cptr_val,
                     as_cptr_type, true, NULL);
  List_push(&cg->system_functions, f_as_cptr);

  // i32 __tyl_ptr_eq(const char *a, const char *b)
  LLVMTypeRef ptr_eq_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef ptr_eq_type = LLVMFunctionType(
      LLVMInt32TypeInContext(cg->context), ptr_eq_args, 2, false);
  LLVMValueRef ptr_eq_val =
      LLVMAddFunction(cg->module, "__tyl_ptr_eq", ptr_eq_type);
  CgFunc *f_ptr_eq = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_ptr_eq, sv_from_cstr("__tyl_ptr_eq"), ptr_eq_val,
                     ptr_eq_type, true, NULL);
  List_push(&cg->system_functions, f_ptr_eq);

  // int64 __tyl_test_string_alloc_count(void)
  LLVMTypeRef count_type =
      LLVMFunctionType(LLVMInt64TypeInContext(cg->context), NULL, 0, false);
  LLVMValueRef alloc_count_val =
      LLVMAddFunction(cg->module, "__tyl_test_string_alloc_count", count_type);
  CgFunc *f_alloc_count = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_alloc_count,
                     sv_from_cstr("__tyl_test_string_alloc_count"),
                     alloc_count_val, count_type, true, NULL);
  List_push(&cg->system_functions, f_alloc_count);

  // int64 __tyl_test_string_free_count(void)
  LLVMValueRef free_count_val =
      LLVMAddFunction(cg->module, "__tyl_test_string_free_count", count_type);
  CgFunc *f_free_count = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_free_count, sv_from_cstr("__tyl_test_string_free_count"),
                     free_count_val, count_type, true, NULL);
  List_push(&cg->system_functions, f_free_count);

  // int64 __tyl_test_string_heap_alloc_count(void)
  LLVMValueRef heap_count_val = LLVMAddFunction(
      cg->module, "__tyl_test_string_heap_alloc_count", count_type);
  CgFunc *f_heap_count = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_heap_count,
                     sv_from_cstr("__tyl_test_string_heap_alloc_count"),
                     heap_count_val, count_type, true, NULL);
  List_push(&cg->system_functions, f_heap_count);

  // void __tyl_test_string_reset_counts(void)
  LLVMTypeRef reset_type =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), NULL, 0, false);
  LLVMValueRef reset_val =
      LLVMAddFunction(cg->module, "__tyl_test_string_reset_counts", reset_type);
  CgFunc *f_reset = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_reset, sv_from_cstr("__tyl_test_string_reset_counts"),
                     reset_val, reset_type, true, NULL);
  List_push(&cg->system_functions, f_reset);

  // void __tyl_array_init_fixed(void *arr, i64 elem_size, i64 len)
  LLVMTypeRef init_fixed_args[] = {ptr_ty, i64_ty, i64_ty};
  LLVMTypeRef init_fixed_type = LLVMFunctionType(
      LLVMVoidTypeInContext(cg->context), init_fixed_args, 3, false);
  LLVMValueRef init_fixed_val =
      LLVMAddFunction(cg->module, "__tyl_array_init_fixed", init_fixed_type);
  CgFunc *f_init_fixed = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_init_fixed, sv_from_cstr("__tyl_array_init_fixed"),
                     init_fixed_val, init_fixed_type, true, NULL);
  List_push(&cg->system_functions, f_init_fixed);

  // ptr<i64> __tyl_array_clone_dims(i64 rank, ptr<i64> dims)
  LLVMTypeRef clone_dims_args[] = {i64_ty, dims_ptr_ty};
  LLVMTypeRef clone_dims_type =
      LLVMFunctionType(LLVMPointerType(i64_ty, 0), clone_dims_args, 2, false);
  LLVMValueRef clone_dims_val =
      LLVMAddFunction(cg->module, "__tyl_array_clone_dims", clone_dims_type);
  CgFunc *f_clone_dims = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_clone_dims, sv_from_cstr("__tyl_array_clone_dims"),
                     clone_dims_val, clone_dims_type, true, NULL);
  List_push(&cg->system_functions, f_clone_dims);

  // void panic(const char *msg, ...)
  LLVMTypeRef panic_args[] = {ptr_ty};
  LLVMTypeRef panic_type =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), panic_args, 1, true);
  LLVMValueRef panic_val = LLVMAddFunction(cg->module, "panic", panic_type);
  CgFunc *f_panic = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f_panic, sv_from_parts("panic", 5), panic_val, panic_type,
                     true, NULL);
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
      CgFunc *mf = cg_find_function(cg, sv_from_parts("main", 4));
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
      if (type_is_concrete(node->impl_decl.type)) {
        for (size_t j = 0; j < node->impl_decl.members.len; j++) {
          AstNode *member = node->impl_decl.members.items[j];
          if (member->tag == NODE_FUNC_DECL) {
            cg_define_function(cg, member);
          }
        }
      }
    }
  }

  for (size_t i = 0; i < cg->type_ctx->instantiated_functions.len; i++) {
    AstNode *node = cg->type_ctx->instantiated_functions.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      cg_define_function(cg, node);
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
      if (type_is_concrete(node->impl_decl.type)) {
        for (size_t j = 0; j < node->impl_decl.members.len; j++) {
          AstNode *member = node->impl_decl.members.items[j];
          if (member->tag == NODE_FUNC_DECL) {
            cg_emit_function_body(cg, member);
          }
        }
      }
    }
  }

  for (size_t i = 0; i < cg->type_ctx->instantiated_functions.len; i++) {
    AstNode *node = cg->type_ctx->instantiated_functions.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      cg_emit_function_body(cg, node);
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

Codegen *Codegen_new(const char *module_name, TypeContext *type_ctx,
                     ErrorHandler *eh) {
  Codegen *cg = xmalloc(sizeof(Codegen));
  cg->context = LLVMContextCreate();
  cg->module = LLVMModuleCreateWithNameInContext(module_name, cg->context);
  cg->builder = LLVMCreateBuilderInContext(cg->context);
  cg->type_ctx = type_ctx;
  cg->eh = eh;
  List_init(&cg->defers);
  cg->current_scope = xmalloc(sizeof(CgSymtab));
  CGSymbolTable_init(cg->current_scope, NULL);
  cg->current_function_ref = NULL;

  List_init(&cg->functions);
  List_init(&cg->system_functions);
  List_init(&cg->format_strings);
  List_init(&cg->string_pool);
  List_init(&cg->string_globals);
  List_init(&cg->struct_types_in_progress);
  List_init(&cg->break_stack);
  List_init(&cg->continue_stack);
  cg->current_function_uses_arena = false;

  cg_initiate_system_functions(cg);

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMTypeRef entry_ty = LLVMFunctionType(i32_ty, NULL, 0, 0);
  LLVMValueRef entry_func = LLVMAddFunction(cg->module, "main", entry_ty);

  LLVMBasicBlockRef entry_bb =
      LLVMAppendBasicBlockInContext(cg->context, entry_func, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  CgFunc *entry_fn = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(entry_fn, sv_from_cstr("__system__main__"), entry_func,
                     entry_ty, true, NULL);

  List_push(&cg->system_functions, entry_fn);

  return cg;
}

void Codegen_global(Codegen *cg, AstNode *ast_root) {
  if (ast_root->tag != NODE_AST_ROOT)
    panic("Expected AST root node");

  CgFunc *entry_fn = cg_find_function(cg, sv_from_cstr("__system__main__"));

  if (!entry_fn) {
    panic("[Codegen_global] Internal error: entry function not found");
  }

  cg_type_lower_structs(cg);

  cg_declare_functions(cg, ast_root);

  CgFunc *prev_func_ref = cg->current_function_ref;
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

  CgFunc *entry_fn = cg_find_function(cg, sv_from_cstr("__system__main__"));
  if (!entry_fn)
    panic("[Codegen_program] Internal error: entry function not found");

  LLVMValueRef entry_func = entry_fn->value;

  cg->current_function = entry_func;
  cg->current_function_ref = entry_fn;

  LLVMBasicBlockRef entry_bb = LLVMGetLastBasicBlock(entry_func);
  if (!entry_bb) {
    entry_bb = LLVMAppendBasicBlockInContext(cg->context, entry_func, "entry");
  }
  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  cg_push_scope(cg);

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMValueRef exit_code = LLVMConstInt(i32_ty, 0, 0);

  CgFunc *user_main = cg_find_function(cg, sv_from_cstr("main"));

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
      LLVMConstStringInContext(cg->context, copy, str.len, false);
  LLVMValueRef global = LLVMAddGlobal(cg->module, LLVMTypeOf(const_str), name);
  LLVMSetInitializer(global, const_str);
  LLVMSetGlobalConstant(global, true);
  LLVMSetLinkage(global, LLVMPrivateLinkage);
  LLVMSetUnnamedAddr(global, true);

  List_push(&cg->string_globals, (void *)global);

  return cg->string_pool.len - 1;
}
