#include <llvm-c/Core.h>
#include <stdio.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/utils.h"

static void cg_register_system_function(Codegen *cg, StringView name,
                                        LLVMTypeRef type) {
  char *c_name = sv_to_cstr(name);
  LLVMValueRef func = LLVMAddFunction(cg->module, c_name, type);
  free(c_name);

  CgFunc *f = xmalloc(sizeof(CgFunc));
  cg_init_CGFunction(f, name, func, type, true, NULL);
  List_push(&cg->system_functions, f);
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

  cg_register_runtime_functions(cg);

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
