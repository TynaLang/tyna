#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"

static void codegen_statement(Codegen *cg, AstNode *node);

void CGSymbolTable_init(CGSymbolTable *t) { List_init(&t->symbols); }

void CGSymbolTable_add(CGSymbolTable *t, StringView name, TypeKind type,
                       LLVMValueRef value) {
  CGSymbol *s = malloc(sizeof(CGSymbol));
  if (!s) {
    fprintf(stderr, "Failed to allocate CGSymbol\n");
    exit(1);
  }

  s->name = name;
  s->type = type;
  s->value = value;

  List_push(&t->symbols, s);
}

CGSymbol *CGSymbolTable_find(CGSymbolTable *t, StringView name) {
  for (size_t i = 0; i < t->symbols.len; i++) {
    CGSymbol *s = t->symbols.items[i];
    if (sv_eq(s->name, name))
      return s;
  }
  return NULL;
}

Codegen *Codegen_new(const char *module_name) {
  Codegen *cg = malloc(sizeof(Codegen));
  if (!cg) {
    fprintf(stderr, "Failed to allocate Codegen\n");
    exit(1);
  }

  cg->context = LLVMContextCreate();
  cg->module = LLVMModuleCreateWithNameInContext(module_name, cg->context);
  cg->builder = LLVMCreateBuilderInContext(cg->context);

  CGSymbolTable_init(&cg->symbols);

  LLVMTypeRef main_type =
      LLVMFunctionType(LLVMInt32TypeInContext(cg->context), NULL, 0, 0);
  LLVMValueRef main_func = LLVMAddFunction(cg->module, "main", main_type);

  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->context, main_func, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry);

  LLVMTypeRef printf_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef printf_type =
      LLVMFunctionType(LLVMInt32TypeInContext(cg->context), printf_args, 1, 1);

  cg->printf_func_type = printf_type;
  cg->printf_func = LLVMAddFunction(cg->module, "printf", printf_type);

  LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
  LLVMTypeRef pow_args[] = {double_ty, double_ty};
  LLVMTypeRef pow_type = LLVMFunctionType(double_ty, pow_args, 2, 0);

  cg->pow_func_type = pow_type;
  cg->pow_func = LLVMAddFunction(cg->module, "pow", pow_type);

  return cg;
}

void Codegen_return_zero(Codegen *cg) {
  LLVMBuildRet(cg->builder,
               LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, 0));
}

void Codegen_dump(Codegen *cg) { LLVMDumpModule(cg->module); }

LLVMTypeRef Codegen_type(Codegen *cg, TypeKind t) {
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
    fprintf(stderr, "Unknown type for %d\n", t);
    exit(1);
  }
}

static LLVMValueRef cast_to_type(Codegen *cg, LLVMValueRef value,
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

static LLVMValueRef codegen_increment(Codegen *cg, AstNode *node,
                                      int is_postfix, int negate) {
  StringView name = node->unary.expr->var.value;
  char *c_name = sv_to_cstr(name);

  CGSymbol *s = CGSymbolTable_find(&cg->symbols, name);
  if (!s) {
    fprintf(stderr, "Undefined variable '" SV_FMT "'\n", SV_ARG(name));
    exit(1);
  }

  LLVMTypeRef ty = Codegen_type(cg, s->type);
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

static LLVMValueRef codegen_expression(Codegen *cg, AstNode *node) {
  switch (node->tag) {
  case NODE_NUMBER: {
    StringView text = node->number.raw_text;
    int has_dot = 0;
    for (size_t i = 0; i < text.len; i++) {
      if (text.data[i] == '.')
        has_dot = 1;
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
    return LLVMConstInt(Codegen_type(cg, TYPE_CHAR), (int)node->char_lit.value,
                        0);

  case NODE_STRING: {
    char *c_str = sv_to_cstr(node->string.value);
    LLVMValueRef val = LLVMBuildGlobalStringPtr(cg->builder, c_str, "str");
    free(c_str);
    return val;
  }

  case NODE_VAR: {
    CGSymbol *s = CGSymbolTable_find(&cg->symbols, node->var.value);
    if (!s) {
      fprintf(stderr, "Undefined variable '" SV_FMT "'\n",
              SV_ARG(node->var.value));
      exit(1);
    }

    LLVMTypeRef ty = Codegen_type(cg, s->type);
    char *c_name = sv_to_cstr(node->var.value);
    LLVMValueRef val = LLVMBuildLoad2(cg->builder, ty, s->value, c_name);
    free(c_name);
    return val;
  }

  case NODE_BINARY: {
    LLVMValueRef left = codegen_expression(cg, node->binary.left);
    LLVMValueRef right = codegen_expression(cg, node->binary.right);

    LLVMTypeRef left_ty = LLVMTypeOf(left);
    LLVMTypeRef right_ty = LLVMTypeOf(right);

    if (left_ty != right_ty) {
      LLVMTypeKind left_kind = LLVMGetTypeKind(left_ty);
      LLVMTypeKind right_kind = LLVMGetTypeKind(right_ty);

      if (left_kind == LLVMDoubleTypeKind || right_kind == LLVMDoubleTypeKind) {
        left = cast_to_type(cg, left, LLVMDoubleTypeInContext(cg->context));
        right = cast_to_type(cg, right, LLVMDoubleTypeInContext(cg->context));
      } else if (left_kind == LLVMFloatTypeKind ||
                 right_kind == LLVMFloatTypeKind) {
        left = cast_to_type(cg, left, LLVMFloatTypeInContext(cg->context));
        right = cast_to_type(cg, right, LLVMFloatTypeInContext(cg->context));
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
          cast_to_type(cg, left, LLVMDoubleTypeInContext(cg->context));
      LLVMValueRef right_d =
          cast_to_type(cg, right, LLVMDoubleTypeInContext(cg->context));
      LLVMValueRef args[] = {left_d, right_d};
      LLVMValueRef res = LLVMBuildCall2(cg->builder, cg->pow_func_type,
                                        cg->pow_func, args, 2, "pow");
      return cast_to_type(cg, res, res_ty);
    }
    default:
      fprintf(stderr, "Unknown binary op\n");
      exit(1);
    }
  }

  case NODE_CAST_EXPR: {
    LLVMValueRef val = codegen_expression(cg, node->cast_expr.expr);
    LLVMTypeRef to_ty = Codegen_type(cg, node->cast_expr.target_type);
    return cast_to_type(cg, val, to_ty);
  }

  case NODE_UNARY: {
    switch (node->unary.op) {
    case OP_PRE_INC:
      return codegen_increment(cg, node, 0, 0);
    case OP_POST_INC:
      return codegen_increment(cg, node, 1, 0);
    case OP_PRE_DEC:
      return codegen_increment(cg, node, 0, 1);
    case OP_POST_DEC:
      return codegen_increment(cg, node, 1, 1);
    case OP_NEG: {
      LLVMValueRef val = codegen_expression(cg, node->unary.expr);
      LLVMTypeRef ty = LLVMTypeOf(val);
      if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind ||
          LLVMGetTypeKind(ty) == LLVMDoubleTypeKind)
        return LLVMBuildFNeg(cg->builder, val, "fneg");
      return LLVMBuildNeg(cg->builder, val, "neg");
    }
    default:
      fprintf(stderr, "Unknown unary op\n");
      exit(1);
    }
  }

  case NODE_ASSIGN_EXPR: {
    StringView name = node->assign_expr.target->var.value;
    CGSymbol *s = CGSymbolTable_find(&cg->symbols, name);
    if (!s) {
      char *cname = sv_to_cstr(name);
      fprintf(stderr, "Undefined variable %s\n", cname);
      free(cname);
      exit(1);
    }
    LLVMValueRef value = codegen_expression(cg, node->assign_expr.value);
    value = cast_to_type(cg, value, Codegen_type(cg, s->type));
    LLVMBuildStore(cg->builder, value, s->value);
    return value;
  }

  default:
    fprintf(stderr, "Unhandled expression node %d\n", node->tag);
    exit(1);
  }
}

static void codegen_statement(Codegen *cg, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_VAR_DECL: {
    StringView name = node->var_decl.name->var.value;
    char *c_name = sv_to_cstr(name);
    LLVMTypeRef ty = Codegen_type(cg, node->var_decl.declared_type);
    LLVMValueRef value = codegen_expression(cg, node->var_decl.value);
    value = cast_to_type(cg, value, ty);

    LLVMValueRef var = LLVMBuildAlloca(cg->builder, ty, c_name);
    LLVMBuildStore(cg->builder, value, var);
    CGSymbolTable_add(&cg->symbols, name, node->var_decl.declared_type, var);
    free(c_name);
    break;
  }

  case NODE_PRINT_STMT: {
    for (size_t i = 0; i < node->print_stmt.values.len; i++) {
      AstNode *val_node = node->print_stmt.values.items[i];
      LLVMValueRef value = codegen_expression(cg, val_node);

      // Determine type for format string
      TypeKind t = TYPE_I32;
      if (val_node->tag == NODE_VAR) {
        CGSymbol *s = CGSymbolTable_find(&cg->symbols, val_node->var.value);
        if (s)
          t = s->type;
      } else if (val_node->tag == NODE_NUMBER) {
        LLVMTypeRef ty = LLVMTypeOf(value);
        if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind)
          t = TYPE_F32;
        else if (LLVMGetTypeKind(ty) == LLVMDoubleTypeKind)
          t = TYPE_F64;
      } else if (val_node->tag == NODE_CHAR) {
        t = TYPE_CHAR;
      } else if (val_node->tag == NODE_STRING) {
        t = TYPE_STRING;
      } else if (val_node->tag == NODE_CAST_EXPR) {
        t = val_node->cast_expr.target_type;
      }

      const char *fmt_str = "%d";
      if (t == TYPE_CHAR) {
        fmt_str = "%c";
        value = LLVMBuildSExt(cg->builder, value, Codegen_type(cg, TYPE_I32),
                              "char_to_int");
      } else if (t == TYPE_STRING) {
        fmt_str = "%s";
      } else if (t == TYPE_F32 || t == TYPE_F64) {
        fmt_str = "%f";
        value = LLVMBuildFPCast(cg->builder, value, Codegen_type(cg, TYPE_F64),
                                "fptofp");
      } else if (t == TYPE_U32 || t == TYPE_U64 || t == TYPE_U16 ||
                 t == TYPE_U8) {
        fmt_str = "%u";
        if (t == TYPE_U64)
          fmt_str = "%lu";
      } else if (t == TYPE_I64) {
        fmt_str = "%ld";
      }

      LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "fmt");
      LLVMValueRef args[] = {fmt, value};
      LLVMBuildCall2(cg->builder, cg->printf_func_type, cg->printf_func, args,
                     2, "");
    }

    // Print newline at the end
    LLVMValueRef nl_fmt = LLVMBuildGlobalStringPtr(cg->builder, "\n", "nl");
    LLVMValueRef nl_args[] = {nl_fmt};
    LLVMBuildCall2(cg->builder, cg->printf_func_type, cg->printf_func, nl_args,
                   1, "");
    break;
  }

  case NODE_EXPR_STMT:
    codegen_expression(cg, node->expr_stmt.expr);
    break;

  default:
    fprintf(stderr, "Unhandled statement node %d\n", node->tag);
    exit(1);
  }
}

void codegen_program(Codegen *cg, AstNode *program) {
  for (size_t i = 0; i < program->program.children.len; i++) {
    codegen_statement(cg, program->program.children.items[i]);
  }
  Codegen_return_zero(cg);
}
