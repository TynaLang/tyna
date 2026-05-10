#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <stdlib.h>
#include <string.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/utils.h"

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
      bool impl_concrete = type_is_concrete(node->impl_decl.type);
      if (impl_concrete) {
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
      bool impl_concrete = type_is_concrete(node->impl_decl.type);
      if (impl_concrete) {
        for (size_t j = 0; j < node->impl_decl.members.len; j++) {
          AstNode *member = node->impl_decl.members.items[j];
          if (member->tag == NODE_FUNC_DECL) {
            cg_emit_function_body(cg, member);
          }
        }
      }
    }
  }
}

void Codegen_emit_instantiated_functions(Codegen *cg) {
  for (size_t i = 0; i < cg->type_ctx->instantiated_functions.len; i++) {
    AstNode *node = cg->type_ctx->instantiated_functions.items[i];
    if (node->tag != NODE_FUNC_DECL) {
      continue;
    }
    cg_emit_function_body(cg, node);
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

  cg_register_runtime_functions(cg);

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMTypeRef argv_ty = LLVMPointerType(
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0), 0);
  LLVMTypeRef entry_params[2] = {i32_ty, argv_ty};
  LLVMTypeRef entry_ty = LLVMFunctionType(i32_ty, entry_params, 2, 0);
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

    LLVMValueRef argc_val = LLVMGetParam(entry_func, 0);
    LLVMValueRef argv_val = LLVMGetParam(entry_func, 1);

    Type *str_type = type_get_primitive(cg->type_ctx, PRIM_STRING);
    Type *args_type = type_get_array(cg->type_ctx, str_type, 0);
    LLVMTypeRef llvm_args_ty = cg_type_get_llvm(cg, args_type);
    LLVMValueRef args_array =
        LLVMBuildAlloca(cg->builder, llvm_args_ty, "args");
    LLVMBuildStore(cg->builder, LLVMConstNull(llvm_args_ty), args_array);

    CgFunc *array_new =
        cg_find_system_function(cg, sv_from_cstr("__tyna_array_new"));
    CgFunc *array_push =
        cg_find_system_function(cg, sv_from_cstr("__tyna_array_push"));
    CgFunc *strlen_fn = cg_find_system_function(cg, sv_from_cstr("strlen"));
    if (!array_new || !array_push || !strlen_fn) {
      panic("Internal error: required runtime helpers for argv conversion are "
            "missing");
    }

    const char *data_layout = LLVMGetDataLayout(cg->module);
    LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
    LLVMTypeRef str_llvm_ty = cg_type_get_llvm(cg, str_type);
    unsigned long long str_size = LLVMABISizeOfType(td, str_llvm_ty);
    LLVMDisposeTargetData(td);

    LLVMBuildCall2(
        cg->builder, array_new->type, array_new->value,
        (LLVMValueRef[]){
            args_array,
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), str_size, 0)},
        2, "");

    LLVMBasicBlockRef loop_bb =
        LLVMAppendBasicBlockInContext(cg->context, entry_func, "argv_loop");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, entry_func, "argv_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, entry_func, "argv_end");

    // Allocate the loop counter in the entry block, not in the loop body.
    // This prevents the counter from being reset to 0 on every iteration.
    LLVMValueRef index_ptr = LLVMBuildAlloca(
        cg->builder, LLVMInt64TypeInContext(cg->context), "argv_i");
    LLVMBuildStore(cg->builder,
                   LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0),
                   index_ptr);

    LLVMBuildBr(cg->builder, loop_bb);
    LLVMPositionBuilderAtEnd(cg->builder, loop_bb);

    LLVMValueRef idx =
        LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                       index_ptr, "argv_idx");
    LLVMValueRef argc_i64 = LLVMBuildZExt(
        cg->builder, argc_val, LLVMInt64TypeInContext(cg->context), "argc_i64");
    LLVMValueRef cond =
        LLVMBuildICmp(cg->builder, LLVMIntULT, idx, argc_i64, "argv_cond");
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    LLVMTypeRef argv_elem_ty =
        LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
    LLVMValueRef argv_ptr = LLVMBuildGEP2(cg->builder, argv_elem_ty, argv_val,
                                          (LLVMValueRef[]){idx}, 1, "argv_ptr");
    LLVMValueRef cstr =
        LLVMBuildLoad2(cg->builder, argv_elem_ty, argv_ptr, "argv_str_ptr");
    LLVMValueRef len = LLVMBuildCall2(cg->builder, strlen_fn->type,
                                      strlen_fn->value, &cstr, 1, "argv_len");

    LLVMValueRef str_tmp =
        LLVMBuildAlloca(cg->builder, str_llvm_ty, "argv_str");
    LLVMValueRef str_data_ptr = LLVMBuildStructGEP2(
        cg->builder, str_llvm_ty, str_tmp, 0, "argv_data_ptr");
    LLVMBuildStore(cg->builder, cstr, str_data_ptr);
    LLVMValueRef str_len_ptr = LLVMBuildStructGEP2(cg->builder, str_llvm_ty,
                                                   str_tmp, 1, "argv_len_ptr");
    LLVMBuildStore(cg->builder, len, str_len_ptr);

    LLVMBuildCall2(
        cg->builder, array_push->type, array_push->value,
        (LLVMValueRef[]){
            args_array, str_tmp,
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), str_size, 0)},
        3, "");

    LLVMValueRef next_idx = LLVMBuildAdd(
        cg->builder, idx,
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "argv_next");
    LLVMBuildStore(cg->builder, next_idx, index_ptr);
    LLVMBuildBr(cg->builder, loop_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);

    LLVMValueRef args_value =
        LLVMBuildLoad2(cg->builder, llvm_args_ty, args_array, "argv_array");
    LLVMValueRef call_args[1] = {args_value};

    LLVMValueRef ret_val =
        LLVMBuildCall2(cg->builder, user_main->type, user_main->value,
                       call_args, 1, call_name);

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
