#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "codegen.h"

static int is_numeric(TypeKind t) {
  switch (t) {
  case TYPE_I8:
  case TYPE_U8:
  case TYPE_I16:
  case TYPE_U16:
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_I64:
  case TYPE_U64:
  case TYPE_F32:
  case TYPE_F64:
    return 1;
  default:
    return 0;
  }
}

static LLVMTypeRef cg_get_llvm_type(Codegen *cg, TypeKind t) {
  switch (t) {
  case TYPE_I32:
    return LLVMInt32TypeInContext(cg->context);
  case TYPE_I8:
  case TYPE_CHAR:
  case TYPE_U8:
    return LLVMInt8TypeInContext(cg->context);
  case TYPE_I16:
  case TYPE_U16:
    return LLVMInt16TypeInContext(cg->context);
  case TYPE_U32:
    return LLVMInt32TypeInContext(cg->context);
  case TYPE_I64:
  case TYPE_U64:
    return LLVMInt64TypeInContext(cg->context);
  case TYPE_F32:
    return LLVMFloatTypeInContext(cg->context);
  case TYPE_F64:
    return LLVMDoubleTypeInContext(cg->context);
  case TYPE_STRING:
    return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  default:
    return LLVMVoidTypeInContext(cg->context);
  }
}

static LLVMValueRef cg_cast_value(Codegen *cg, LLVMValueRef value,
                                  LLVMTypeRef to_ty) {
  LLVMTypeRef from_ty = LLVMTypeOf(value);
  if (from_ty == to_ty)
    return value;

  LLVMTypeKind from_kind = LLVMGetTypeKind(from_ty);
  LLVMTypeKind to_kind = LLVMGetTypeKind(to_ty);

  if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMFloatTypeKind)
    return LLVMBuildSIToFP(cg->builder, value, to_ty, "itofp");
  if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMDoubleTypeKind)
    return LLVMBuildSIToFP(cg->builder, value, to_ty, "itofp");
  if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMIntegerTypeKind)
    return LLVMBuildIntCast(cg->builder, value, to_ty, "intcast");
  if ((from_kind == LLVMFloatTypeKind || from_kind == LLVMDoubleTypeKind) &&
      to_kind == LLVMIntegerTypeKind)
    return LLVMBuildFPToSI(cg->builder, value, to_ty, "fptosi");
  if ((from_kind == LLVMFloatTypeKind || from_kind == LLVMDoubleTypeKind) &&
      (to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind))
    return LLVMBuildFPCast(cg->builder, value, to_ty, "fpcast");

  return value;
}

void CGSymbolTable_init(CGSymbolTable *t, CGSymbolTable *parent) {
  t->parent = parent;
  List_init(&t->symbols);
}

void CGSymbolTable_add(CGSymbolTable *t, StringView name, TypeKind type,
                       LLVMValueRef value) {
  CGSymbol *s = malloc(sizeof(CGSymbol));
  s->name = name;
  s->type = type;
  s->value = value;
  List_push(&t->symbols, s);
}

CGSymbol *CGSymbolTable_find(CGSymbolTable *t, StringView name) {
  while (t) {
    for (size_t i = 0; i < t->symbols.len; i++) {
      CGSymbol *s = t->symbols.items[i];
      if (sv_eq(s->name, name))
        return s;
    }
    t = t->parent;
  }
  return NULL;
}

static LLVMValueRef cg_expression(Codegen *cg, AstNode *node);
static void cg_statement(Codegen *cg, AstNode *node);

static CGFunction *cg_find_function(Codegen *cg, StringView name) {
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
}

static LLVMValueRef cg_increment(Codegen *cg, AstNode *node, int is_postfix,
                                 int negate) {
  StringView name = node->unary.expr->var.value;
  CGSymbol *s = CGSymbolTable_find(cg->current_scope, name);
  if (!s) {
    fprintf(stderr, "Undefined variable '" SV_FMT "'\n", SV_ARG(name));
    exit(1);
  }

  LLVMTypeRef ty = cg_get_llvm_type(cg, s->type);
  char *c_name = sv_to_cstr(name);
  LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, ty, s->value, c_name);
  free(c_name);

  LLVMValueRef one;
  LLVMValueRef new_val;

  if (s->type == TYPE_F32 || s->type == TYPE_F64) {
    one = LLVMConstReal(ty, negate ? -1.0 : 1.0);
    new_val = LLVMBuildFAdd(cg->builder, old_val, one, "finc");
  } else {
    one = LLVMConstInt(ty, negate ? -1 : 1, 0);
    new_val = LLVMBuildAdd(cg->builder, old_val, one, "inc");
  }

  LLVMBuildStore(cg->builder, new_val, s->value);
  return is_postfix ? old_val : new_val;
}

static LLVMValueRef cg_expression(Codegen *cg, AstNode *node) {
  switch (node->tag) {
  case NODE_NUMBER: {
    StringView text = node->number.raw_text;
    bool has_dot = false;
    for (size_t i = 0; i < text.len; i++) {
      if (text.data[i] == '.')
        has_dot = true;
    }
    if (has_dot) {
      return LLVMConstReal(LLVMDoubleTypeInContext(cg->context),
                           node->number.value);
    } else {
      return LLVMConstInt(LLVMInt32TypeInContext(cg->context),
                          (int)node->number.value, 0);
    }
  }

  case NODE_CHAR:
    return LLVMConstInt(cg_get_llvm_type(cg, TYPE_CHAR),
                        (int)node->char_lit.value, 0);

  case NODE_STRING: {
    char *c_str = sv_to_cstr(node->string.value);
    LLVMValueRef val = LLVMBuildGlobalStringPtr(cg->builder, c_str, "str");
    free(c_str);
    return val;
  }

  case NODE_VAR: {
    CGSymbol *s = CGSymbolTable_find(cg->current_scope, node->var.value);
    if (!s) {
      fprintf(stderr, "Undefined variable '" SV_FMT "'\n",
              SV_ARG(node->var.value));
      exit(1);
    }
    LLVMTypeRef ty = cg_get_llvm_type(cg, s->type);
    char *c_name = sv_to_cstr(node->var.value);
    LLVMValueRef val = LLVMBuildLoad2(cg->builder, ty, s->value, c_name);
    free(c_name);
    return val;
  }

  case NODE_BINARY: {
    LLVMValueRef left = cg_expression(cg, node->binary.left);
    LLVMValueRef right = cg_expression(cg, node->binary.right);

    LLVMTypeRef left_ty = LLVMTypeOf(left);
    LLVMTypeRef right_ty = LLVMTypeOf(right);

    if (left_ty != right_ty) {
      LLVMTypeKind left_kind = LLVMGetTypeKind(left_ty);
      LLVMTypeKind right_kind = LLVMGetTypeKind(right_ty);

      if (left_kind == LLVMDoubleTypeKind || right_kind == LLVMDoubleTypeKind) {
        left = cg_cast_value(cg, left, LLVMDoubleTypeInContext(cg->context));
        right = cg_cast_value(cg, right, LLVMDoubleTypeInContext(cg->context));
      } else if (left_kind == LLVMFloatTypeKind ||
                 right_kind == LLVMFloatTypeKind) {
        left = cg_cast_value(cg, left, LLVMFloatTypeInContext(cg->context));
        right = cg_cast_value(cg, right, LLVMFloatTypeInContext(cg->context));
      }
    }

    LLVMTypeRef res_ty = LLVMTypeOf(left);
    LLVMTypeKind res_kind = LLVMGetTypeKind(res_ty);

    switch (node->binary.op) {
    case OP_ADD:
      return (res_kind == LLVMFloatTypeKind || res_kind == LLVMDoubleTypeKind)
                 ? LLVMBuildFAdd(cg->builder, left, right, "fadd")
                 : LLVMBuildAdd(cg->builder, left, right, "add");
    case OP_SUB:
      return (res_kind == LLVMFloatTypeKind || res_kind == LLVMDoubleTypeKind)
                 ? LLVMBuildFSub(cg->builder, left, right, "fsub")
                 : LLVMBuildSub(cg->builder, left, right, "sub");
    case OP_MUL:
      return (res_kind == LLVMFloatTypeKind || res_kind == LLVMDoubleTypeKind)
                 ? LLVMBuildFMul(cg->builder, left, right, "fmul")
                 : LLVMBuildMul(cg->builder, left, right, "mul");
    case OP_DIV:
      return (res_kind == LLVMFloatTypeKind || res_kind == LLVMDoubleTypeKind)
                 ? LLVMBuildFDiv(cg->builder, left, right, "fdiv")
                 : LLVMBuildSDiv(cg->builder, left, right, "div");
    case OP_POW: {
      LLVMValueRef left_d =
          cg_cast_value(cg, left, LLVMDoubleTypeInContext(cg->context));
      LLVMValueRef right_d =
          cg_cast_value(cg, right, LLVMDoubleTypeInContext(cg->context));
      LLVMValueRef args[] = {left_d, right_d};
      CGFunction *pow_func = cg_find_function(cg, sv_from_parts("pow", 3));
      LLVMValueRef res = LLVMBuildCall2(cg->builder, pow_func->type,
                                        pow_func->value, args, 2, "pow");
      return cg_cast_value(cg, res, res_ty);
    }
    default:
      fprintf(stderr, "Unknown binary op\n");
      exit(1);
    }
  }

  case NODE_CAST_EXPR: {
    LLVMValueRef val = cg_expression(cg, node->cast_expr.expr);
    LLVMTypeRef to_ty = cg_get_llvm_type(cg, node->cast_expr.target_type);
    return cg_cast_value(cg, val, to_ty);
  }

  case NODE_UNARY: {
    switch (node->unary.op) {
    case OP_PRE_INC:
      return cg_increment(cg, node, 0, 0);
    case OP_POST_INC:
      return cg_increment(cg, node, 1, 0);
    case OP_PRE_DEC:
      return cg_increment(cg, node, 0, 1);
    case OP_POST_DEC:
      return cg_increment(cg, node, 1, 1);
    case OP_NEG: {
      LLVMValueRef val = cg_expression(cg, node->unary.expr);
      LLVMTypeRef ty = LLVMTypeOf(val);
      if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind ||
          LLVMGetTypeKind(ty) == LLVMDoubleTypeKind)
        return LLVMBuildFNeg(cg->builder, val, "fneg");
      return LLVMBuildNeg(cg->builder, val, "neg");
    }
    default:
      exit(1);
    }
  }

  case NODE_ASSIGN_EXPR: {
    StringView name = node->assign_expr.target->var.value;
    CGSymbol *s = CGSymbolTable_find(cg->current_scope, name);
    if (!s) {
      exit(1);
    }
    LLVMValueRef value = cg_expression(cg, node->assign_expr.value);
    value = cg_cast_value(cg, value, cg_get_llvm_type(cg, s->type));
    LLVMBuildStore(cg->builder, value, s->value);
    return value;
  }

  case NODE_CALL: {
    CGFunction *f = cg_find_function(cg, node->call.func->var.value);
    if (!f) {
      fprintf(stderr, "Undefined function '" SV_FMT "'\n",
              SV_ARG(node->call.func->var.value));
      exit(1);
    }
    size_t arg_count = node->call.args.len;
    LLVMValueRef *args =
        malloc(sizeof(LLVMValueRef) * (arg_count > 0 ? arg_count : 1));
    for (size_t i = 0; i < arg_count; i++) {
      LLVMValueRef val = cg_expression(cg, node->call.args.items[i]);
      if (!f->is_system && i < f->params.len) {
        TypeKind param_ty = (TypeKind)(uintptr_t)f->params.items[i];
        val = cg_cast_value(cg, val, cg_get_llvm_type(cg, param_ty));
      } else if (f->is_system) {
        // For printf, we basically just pass as is (relying on varargs)
        // but for pow we need double.
        if (sv_eq(f->name, sv_from_parts("pow", 3))) {
          val = cg_cast_value(cg, val, LLVMDoubleTypeInContext(cg->context));
        }
      }
      args[i] = val;
    }
    LLVMTypeRef ret_ty = LLVMGetReturnType(f->type);
    const char *name =
        (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind) ? "" : "call";
    LLVMValueRef res =
        LLVMBuildCall2(cg->builder, f->type, f->value, args, arg_count, name);
    free(args);
    return res;
  }

  default:
    fprintf(stderr, "Unhandled expression node %d\n", node->tag);
    exit(1);
  }
}

static void cg_statement(Codegen *cg, AstNode *node) {
  if (!node || LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    return;

  switch (node->tag) {
  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    char *c_name = sv_to_cstr(name);
    LLVMTypeRef ty = cg_get_llvm_type(cg, node->var_decl.declared_type);
    LLVMValueRef value = cg_expression(cg, node->var_decl.value);
    value = cg_cast_value(cg, value, ty);

    LLVMValueRef var = LLVMBuildAlloca(cg->builder, ty, c_name);
    LLVMBuildStore(cg->builder, value, var);
    CGSymbolTable_add(cg->current_scope, name, node->var_decl.declared_type,
                      var);
    free(c_name);
    break;
  }

  case NODE_PRINT_STMT: {
    for (size_t i = 0; i < node->print_stmt.values.len; i++) {
      AstNode *val_node = node->print_stmt.values.items[i];
      LLVMValueRef value = cg_expression(cg, val_node);

      // Determine format
      const char *fmt_str = "%d";
      LLVMTypeRef vty = LLVMTypeOf(value);
      LLVMTypeKind vkind = LLVMGetTypeKind(vty);
      if (vkind == LLVMDoubleTypeKind || vkind == LLVMFloatTypeKind) {
        fmt_str = "%f";
        value = cg_cast_value(cg, value, LLVMDoubleTypeInContext(cg->context));
      } else if (vkind == LLVMPointerTypeKind) {
        fmt_str = "%s";
      }

      LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "fmt");
      LLVMValueRef args[] = {fmt, value};
      CGFunction *printf_func = cg_find_function(cg, sv_from_parts("print", 5));
      LLVMBuildCall2(cg->builder, printf_func->type, printf_func->value, args,
                     2, "");
    }
    LLVMValueRef nl_fmt = LLVMBuildGlobalStringPtr(cg->builder, "\n", "nl");
    LLVMValueRef nl_args[] = {nl_fmt};
    CGFunction *printf_func = cg_find_function(cg, sv_from_parts("print", 5));
    LLVMBuildCall2(cg->builder, printf_func->type, printf_func->value, nl_args,
                   1, "");
    break;
  }

  case NODE_EXPR_STMT:
    cg_expression(cg, node->expr_stmt.expr);
    break;

  case NODE_RETURN_STMT: {
    if (node->return_stmt.expr) {
      LLVMValueRef val = cg_expression(cg, node->return_stmt.expr);
      if (cg->current_function_ref) {
        val = cg_cast_value(
            cg, val,
            cg_get_llvm_type(cg, cg->current_function_ref->return_type));
      }
      LLVMBuildRet(cg->builder, val);
    } else {
      LLVMBuildRetVoid(cg->builder);
    }
    break;
  }

  case NODE_FUNC_DECL:
    break;

  default:
    fprintf(stderr, "Unhandled statement node %d\n", node->tag);
    exit(1);
  }
}

Codegen *Codegen_new(const char *module_name) {
  Codegen *cg = malloc(sizeof(Codegen));
  cg->context = LLVMContextCreate();
  cg->module = LLVMModuleCreateWithNameInContext(module_name, cg->context);
  cg->builder = LLVMCreateBuilderInContext(cg->context);
  cg->current_scope = malloc(sizeof(CGSymbolTable));
  CGSymbolTable_init(cg->current_scope, NULL);
  cg->current_function_ref = NULL;

  List_init(&cg->functions);
  List_init(&cg->system_functions);

  // Setup printf
  LLVMTypeRef printf_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef printf_type =
      LLVMFunctionType(LLVMInt32TypeInContext(cg->context), printf_args, 1, 1);
  CGFunction *pf = malloc(sizeof(CGFunction));
  pf->name = sv_from_parts("print", 5);
  pf->value = LLVMAddFunction(cg->module, "printf", printf_type);
  pf->type = printf_type;
  pf->is_system = true;
  List_push(&cg->system_functions, pf);

  // Setup pow
  LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
  LLVMTypeRef pow_args[] = {double_ty, double_ty};
  LLVMTypeRef pow_type = LLVMFunctionType(double_ty, pow_args, 2, 0);
  CGFunction *pw = malloc(sizeof(CGFunction));
  pw->name = sv_from_parts("pow", 3);
  pw->value = LLVMAddFunction(cg->module, "pow", pow_type);
  pw->type = pow_type;
  pw->is_system = true;
  List_push(&cg->system_functions, pw);

  return cg;
}

static void cg_define_function(Codegen *cg, AstNode *node) {
  StringView name = node->func_decl.name;
  LLVMTypeRef ret_ty = cg_get_llvm_type(cg, node->func_decl.return_type);

  size_t param_count = node->func_decl.params.len;
  LLVMTypeRef *param_types =
      malloc(sizeof(LLVMTypeRef) * (param_count > 0 ? param_count : 1));
  for (size_t i = 0; i < param_count; i++) {
    AstNode *param = node->func_decl.params.items[i];
    param_types[i] = cg_get_llvm_type(cg, param->param.type);
  }

  LLVMTypeRef func_type = LLVMFunctionType(ret_ty, param_types, param_count, 0);
  char *c_name = sv_to_cstr(name);
  LLVMValueRef func = LLVMAddFunction(cg->module, c_name, func_type);
  free(c_name);

  CGFunction *f = malloc(sizeof(CGFunction));
  f->name = name;
  f->value = func;
  f->type = func_type;
  f->return_type = node->func_decl.return_type;
  f->is_system = false;
  List_init(&f->params);
  for (size_t i = 0; i < param_count; i++) {
    AstNode *param = node->func_decl.params.items[i];
    List_push(&f->params, (void *)(uintptr_t)param->param.type);
  }
  List_push(&cg->functions, f);
  free(param_types);
}

static void cg_emit_function_body(Codegen *cg, AstNode *node) {
  CGFunction *f = cg_find_function(cg, node->func_decl.name);
  CGFunction *old_func_ref = cg->current_function_ref;
  cg->current_function_ref = f;
  cg->current_function = f->value;

  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->context, f->value, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry);

  CGSymbolTable *old_scope = cg->current_scope;
  CGSymbolTable *new_scope = malloc(sizeof(CGSymbolTable));
  CGSymbolTable_init(new_scope, old_scope);
  cg->current_scope = new_scope;

  for (size_t i = 0; i < node->func_decl.params.len; i++) {
    AstNode *param_node = node->func_decl.params.items[i];
    LLVMValueRef param_val = LLVMGetParam(f->value, i);
    LLVMTypeRef pty = cg_get_llvm_type(cg, param_node->param.type);
    char *pname = sv_to_cstr(param_node->param.name);
    LLVMValueRef alloca = LLVMBuildAlloca(cg->builder, pty, pname);
    LLVMBuildStore(cg->builder, param_val, alloca);
    CGSymbolTable_add(cg->current_scope, param_node->param.name,
                      param_node->param.type, alloca);
    free(pname);
  }

  for (size_t i = 0; i < node->func_decl.body->program.children.len; i++) {
    cg_statement(cg, node->func_decl.body->program.children.items[i]);
  }

  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    if (f->return_type == TYPE_UNKNOWN || f->return_type == TYPE_VOID) {
      LLVMBuildRetVoid(cg->builder);
    } else {
      LLVMValueRef zero =
          LLVMConstInt(cg_get_llvm_type(cg, f->return_type), 0, 0);
      if (is_numeric(f->return_type) &&
          (f->return_type == TYPE_F32 || f->return_type == TYPE_F64)) {
        zero = LLVMConstReal(cg_get_llvm_type(cg, f->return_type), 0.0);
      }
      LLVMBuildRet(cg->builder, zero);
    }
  }

  cg->current_function_ref = old_func_ref;
  cg->current_scope = old_scope;
}

void Codegen_program(Codegen *cg, AstNode *program) {
  bool has_main = false;
  for (size_t i = 0; i < program->program.children.len; i++) {
    AstNode *node = program->program.children.items[i];
    if (node->tag == NODE_FUNC_DECL &&
        sv_eq(node->func_decl.name, sv_from_parts("main", 4))) {
      has_main = true;
    }
  }

  for (size_t i = 0; i < program->program.children.len; i++) {
    AstNode *node = program->program.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      cg_define_function(cg, node);
    }
  }

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

  LLVMBasicBlockRef entry_bb =
      LLVMAppendBasicBlockInContext(cg->context, entry_func, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);
  // We don't change cg->current_function yet, or rather, the global entry is
  // this func.
  LLVMValueRef outer_func = entry_func;
  cg->current_function = outer_func;
  cg->current_function_ref = NULL; // Globals are not in a regular function

  for (size_t i = 0; i < program->program.children.len; i++) {
    AstNode *node = program->program.children.items[i];
    if (node->tag == NODE_FUNC_DECL) {
      // Switch builder to dedicated function
      cg_emit_function_body(cg, node);
      // Switch back to outer_func for remaining global statements
      LLVMPositionBuilderAtEnd(cg->builder, LLVMGetLastBasicBlock(outer_func));
    } else {
      cg_statement(cg, node);
    }
  }

  // Ensure terminator for outer_func
  LLVMPositionBuilderAtEnd(cg->builder, LLVMGetLastBasicBlock(outer_func));
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

void Codegen_dump(Codegen *cg) { LLVMDumpModule(cg->module); }

void Codegen_free(Codegen *cg) {
  LLVMDisposeBuilder(cg->builder);
  LLVMDisposeModule(cg->module);
  LLVMContextDispose(cg->context);
  free(cg);
}
