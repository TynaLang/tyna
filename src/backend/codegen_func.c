#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

#include "codegen_private.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"
#include "tyl/errors.h"
#include "tyl/utils.h"

// prepared for closures and nested functions, but not implemented yet
static StringView cg_generate_anon_name(Codegen *cg) {
  static int anon_count = 0;
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "__anon_func_%d", anon_count++);
  return sv_from_parts(strdup(buf), len);
};

static LLVMTypeRef *cg_build_param_types(Codegen *cg, AstNode *func_node) {
  List params = func_node->func_decl.params;
  size_t count = params.len;
  if (count == 0)
    return NULL;
  LLVMTypeRef *types = xmalloc(sizeof(LLVMTypeRef) * count);
  for (size_t i = 0; i < count; i++) {
    AstNode *param_node = (AstNode *)params.items[i];
    if (param_node != NULL && param_node->tag == NODE_PARAM) {
      types[i] = cg_get_llvm_type(cg, param_node->param.type);
    } else {
      panic("Expected NODE_PARAM in function parameters");
    }
  }
  return types;
}

void cg_init_CGFunction(CGFunction *f, StringView name, LLVMValueRef value,
                        LLVMTypeRef type, bool is_system) {
  f->name = name;
  f->value = value;
  f->type = type;
  f->is_system = is_system;
}

void cg_define_function(Codegen *cg, AstNode *node) {
  StringView name = node->func_decl.name->var.value;
  LLVMTypeRef ret_ty = cg_get_llvm_type(cg, node->func_decl.return_type);

  size_t param_count = node->func_decl.params.len;
  LLVMTypeRef *param_types = cg_build_param_types(cg, node);

  LLVMTypeRef func_type = LLVMFunctionType(ret_ty, param_types, param_count, 0);

  char *llvm_name = sv_to_cstr(name);
  if (sv_eq(name, sv_from_cstr("main")) &&
      LLVMGetNamedFunction(cg->module, llvm_name) != NULL) {
    free(llvm_name);
    llvm_name = strdup("__tyl_main");
  }

  LLVMValueRef func = LLVMGetNamedFunction(cg->module, llvm_name);
  if (func) {
    LLVMTypeRef existing_type = LLVMGetElementType(LLVMTypeOf(func));
    if (existing_type != func_type) {
      ErrorHandler_report(
          cg->eh, node->loc,
          "Function '%s' already declared with a different signature",
          llvm_name);
      free(llvm_name);
      return;
    }
  } else {
    func = LLVMAddFunction(cg->module, llvm_name, func_type);
  }

  CGFunction *f = malloc(sizeof(CGFunction));
  cg_init_CGFunction(f, name, func, func_type, false);
  List_push(&cg->functions, f);

  free(llvm_name);
}

void cg_emit_function_body(Codegen *cg, AstNode *node) {
  if (node->func_decl.is_external) {
    // External functions are declared but implemented in runtime/linker.
    return;
  }

  CGFunction *f = cg_find_function(cg, node->func_decl.name->var.value);
  if (!f) {
    ErrorHandler_report(cg->eh, node->loc, "Function not found: " SV_FMT,
                        SV_ARG(node->func_decl.name->var.value));
    return;
  }

  CGFunction *old_func_ref = cg->current_function_ref;
  cg->current_function_ref = f;
  cg->current_function = f->value;

  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->context, f->value, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry);
  cg_push_scope(cg);

  for (size_t i = 0; i < node->func_decl.params.len; i++) {
    AstNode *param_node = node->func_decl.params.items[i];
    if (param_node->tag != NODE_PARAM) {
      continue;
    }

    StringView param_name = param_node->param.name->var.value;
    Type *param_type = param_node->param.type;

    LLVMValueRef param_val = LLVMGetParam(f->value, (unsigned)i);

    LLVMTypeRef llvm_param_type = cg_get_llvm_type(cg, param_type);

    char buf[256];
    snprintf(buf, sizeof(buf), SV_FMT ".addr", SV_ARG(param_name));
    LLVMValueRef alloca = LLVMBuildAlloca(cg->builder, llvm_param_type, buf);
    LLVMBuildStore(cg->builder, param_val, alloca);

    CGSymbolTable_add(cg->current_scope, param_name, param_type, alloca);
  }

  cg_statement(cg, node->func_decl.body);

  LLVMBasicBlockRef current_bb = LLVMGetInsertBlock(cg->builder);
  if (!LLVMGetBasicBlockTerminator(current_bb)) {
    if (LLVMGetTypeKind(LLVMGetReturnType(f->type)) == LLVMVoidTypeKind) {
      cg_pop_scope(cg);
      LLVMBuildRetVoid(cg->builder);
    } else {
      cg_pop_scope(cg);
      LLVMValueRef default_ret = LLVMConstNull(LLVMGetReturnType(f->type));
      LLVMBuildRet(cg->builder, default_ret);
    }
  } else {
    cg_pop_scope(cg);
  }

  cg->current_function_ref = old_func_ref;
}

CGFunction *cg_find_function(Codegen *cg, StringView name) {
  for (size_t i = 0; i < cg->functions.len; i++) {
    CGFunction *f = cg->functions.items[i];
    if (sv_eq(f->name, name))
      return f;
  }
  for (size_t i = 0; i < cg->system_functions.len; i++) {
    CGFunction *f = cg->system_functions.items[i];
    if (sv_eq(f->name, name))
      return f;
  }
  return NULL;
};

CGFunction *cg_find_system_function(Codegen *cg, StringView name) {
  for (size_t i = 0; i < cg->system_functions.len; i++) {
    CGFunction *f = cg->system_functions.items[i];
    if (sv_eq(f->name, name))
      return f;
  }
  return NULL;
}
