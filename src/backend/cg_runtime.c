#include "cg_internal.h"

typedef struct {
  const char *name;
  LLVMTypeRef ret_type;
  bool is_vararg;
  int arg_count;
  LLVMTypeRef args[4];
} RuntimeBinding;

static LLVMTypeRef cg_get_string_llvm_type(Codegen *cg) {
  const char *name = "tyna_string";
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

void cg_register_runtime_functions(Codegen *cg) {
  LLVMTypeRef void_ty = LLVMVoidTypeInContext(cg->context);
  LLVMTypeRef i8_ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
  LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->context);
  LLVMTypeRef str_ty = cg_get_string_llvm_type(cg);
  LLVMTypeRef buf_ty =
      cg_type_get_llvm(cg, type_get_string_buffer(cg->type_ctx));
  LLVMTypeRef buf_ptr_ty = LLVMPointerType(buf_ty, 0);

  RuntimeBinding bindings[] = {
      // C Standard Library
      {"printf", i32_ty, true, 1, {i8_ptr}},
      {"puts", i32_ty, false, 1, {i8_ptr}},
      {"free", void_ty, false, 1, {i8_ptr}},
      {"pow", f64_ty, false, 2, {f64_ty, f64_ty}},
      {"malloc", i8_ptr, false, 1, {i64_ty}},

      // Array Runtime
      {"__tyna_array_init_fixed", void_ty, false, 3, {i8_ptr, i64_ty, i64_ty}},
      {"__tyna_array_clone_dims",
       i8_ptr,
       false,
       2,
       {i64_ty, LLVMPointerType(i64_ty, 0)}},
      {"__tyna_compare_arrays", i32_ty, false, 3, {i8_ptr, i8_ptr, i64_ty}},

      // String Runtime (Pass str_ty, buf_ptr_ty, i8_ptr directly)
      {"__tyna_str_equals", i32_ty, false, 2, {str_ty, str_ty}},
      {"__tyna_str_hash", i64_ty, false, 1, {str_ty}},
      {"__tyna_as_c_ptr", i8_ptr, false, 1, {str_ty}},
      {"__tyna_str_len", i64_ty, false, 1, {str_ty}},
      {"__tyna_str_slice", str_ty, false, 3, {str_ty, i64_ty, i64_ty}},

      {"__tyna_string_new", void_ty, false, 1, {buf_ptr_ty}},
      {"__tyna_string_with_capacity", void_ty, false, 2, {buf_ptr_ty, i64_ty}},
      {"__tyna_string_push", void_ty, false, 2, {buf_ptr_ty, str_ty}},
      {"__tyna_string_as_mut_ptr", i8_ptr, false, 1, {buf_ptr_ty}},
      {"__tyna_string_set_len", void_ty, false, 2, {buf_ptr_ty, i64_ty}},
      {"__tyna_string_free", void_ty, false, 1, {buf_ptr_ty}},
      {"__tyna_drop_String", void_ty, false, 1, {buf_ptr_ty}},
      {"__tyna_string_into_str", str_ty, false, 1, {buf_ptr_ty}},
      {"__tyna_string_clone_str", str_ty, false, 1, {buf_ptr_ty}},
      {"__tyna_string_promote_if_arena", void_ty, false, 1, {buf_ptr_ty}},

      {"__tyna_string_arena_push", void_ty, false, 0, {}},
      {"__tyna_string_arena_pop", void_ty, false, 0, {}},

      // Testing helpers
      {"__tyna_test_string_alloc_count", i64_ty, false, 0, {}},
      {"__tyna_test_string_free_count", i64_ty, false, 0, {}},
      {"__tyna_test_string_heap_alloc_count", i64_ty, false, 0, {}},
      {"__tyna_test_string_reset_counts", void_ty, false, 0, {}},

      // Misc
      {"panic", void_ty, true, 1, {i8_ptr}},
  };

  for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
    RuntimeBinding *b = &bindings[i];
    LLVMTypeRef func_type =
        LLVMFunctionType(b->ret_type, b->args, b->arg_count, b->is_vararg);
    LLVMValueRef func_val = LLVMAddFunction(cg->module, b->name, func_type);

    CgFunc *cg_func = xmalloc(sizeof(CgFunc));
    cg_init_CGFunction(cg_func, sv_from_cstr(b->name), func_val, func_type,
                       true, NULL);
    List_push(&cg->system_functions, cg_func);
  }
}