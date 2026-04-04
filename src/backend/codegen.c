#include <llvm-c/Core.h>

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
}

static bool cg_has_main(AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *child = root->ast_root.children.items[i];
    if (child->tag == NODE_FUNC_DECL &&
        sv_eq_cstr(child->func_decl.name, "main")) {
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
    }
  }
}

static void cg_emit_functions(Codegen *cg, AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      cg_emit_function_body(cg, node);
    }
  }
}

static void cg_emit_global_statements(Codegen *cg, AstNode *root) {
  for (size_t i = 0; i < root->ast_root.children.len; i++) {
    AstNode *node = root->ast_root.children.items[i];

    if (node->tag == NODE_FUNC_DECL)
      continue;

    cg_statement(cg, node);
  }
}

Codegen *Codegen_new(const char *module_name) {
  Codegen *cg = xmalloc(sizeof(Codegen));
  cg->context = LLVMContextCreate();
  cg->module = LLVMModuleCreateWithNameInContext(module_name, cg->context);
  cg->builder = LLVMCreateBuilderInContext(cg->context);
  cg->current_scope = xmalloc(sizeof(CGSymbolTable));
  CGSymbolTable_init(cg->current_scope, NULL);
  cg->current_function_ref = NULL;

  List_init(&cg->functions);
  List_init(&cg->system_functions);

  cg_initiate_system_functions(cg);

  return cg;
}

void Codegen_program(Codegen *cg, AstNode *ast_root) {
  if (ast_root->tag != NODE_AST_ROOT)
    panic("Expected AST root node");

  LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->context);
  LLVMTypeRef entry_ty = LLVMFunctionType(i32_ty, NULL, 0, 0);
  LLVMValueRef entry_func = LLVMAddFunction(cg->module, "main", entry_ty);
  LLVMBasicBlockRef entry_bb =
      LLVMAppendBasicBlockInContext(cg->context, entry_func, "entry");

  cg_declare_functions(cg, ast_root);
  cg_emit_functions(cg, ast_root);

  cg->current_function = entry_func;
  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  cg_emit_global_statements(cg, ast_root);

  LLVMValueRef exit_code = LLVMConstInt(i32_ty, 0, 0);

  CGFunction *user_main = cg_find_function(cg, sv_from_parts("main", 4));

  if (user_main) {
    LLVMTypeRef return_ty = LLVMGetReturnType(user_main->type);
    bool is_void = (LLVMGetTypeKind(return_ty) == LLVMVoidTypeKind);

    const char *call_name = is_void ? "" : "ret";

    LLVMValueRef ret_val = LLVMBuildCall2(cg->builder, user_main->type,
                                          user_main->value, NULL, 0, call_name);

    if (!is_void) {
      exit_code = cg_cast_value(cg, ret_val, i32_ty);
    }
  }

  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    LLVMBuildRet(cg->builder, exit_code);
  }
}

void Codegen_dump(Codegen *cg) { LLVMDumpModule(cg->module); }

void Codegen_free(Codegen *cg) {
  LLVMDisposeBuilder(cg->builder);
  LLVMDisposeModule(cg->module);
  LLVMContextDispose(cg->context);
  free(cg);
}
