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

void CGSymbolTable_add(CGSymbolTable *t, char *name, TypeKind type,
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

CGSymbol *CGSymbolTable_find(CGSymbolTable *t, char *name) {
  for (size_t i = 0; i < t->symbols.len; i++) {
    CGSymbol *s = t->symbols.items[i];
    if (strcmp(s->name, name) == 0)
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

  // int main()
  LLVMTypeRef main_type =
      LLVMFunctionType(LLVMInt32TypeInContext(cg->context), NULL, 0, 0);
  LLVMValueRef main_func = LLVMAddFunction(cg->module, "main", main_type);

  // entry block
  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->context, main_func, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry);

  // declare printf
  LLVMTypeRef printf_args[] = {
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
  LLVMTypeRef printf_type =
      LLVMFunctionType(LLVMInt32TypeInContext(cg->context), printf_args, 1, 1);

  cg->printf_func_type = printf_type;
  cg->printf_func = LLVMAddFunction(cg->module, "printf", printf_type);

  // declare pow
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
  case TYPE_INT:
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
  case TYPE_FLOAT:
  case TYPE_F32:
    return LLVMFloatTypeInContext(cg->context);
  case TYPE_DOUBLE:
  case TYPE_F64:
    return LLVMDoubleTypeInContext(cg->context);
  case TYPE_STRING:
    return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  default:
    fprintf(stderr, "Unknown type\n");
    exit(1);
  }
}

static LLVMValueRef codegen_increment(Codegen *cg, AstNode *node,
                                      int is_postfix, int negate) {
  // must be identifier (you already enforce this)
  char *name = node->unary.expr->var.value;

  CGSymbol *s = CGSymbolTable_find(&cg->symbols, name);
  if (!s) {
    fprintf(stderr, "Undefined variable '%s'\n", name);
    exit(1);
  }

  LLVMTypeRef ty = Codegen_type(cg, s->type);

  LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, ty, s->value, name);

  LLVMValueRef one;
  LLVMValueRef new_val;

  if (s->type == TYPE_FLOAT || s->type == TYPE_DOUBLE) {
    one = LLVMConstReal(ty, negate ? -1.0 : 1.0);
    new_val = LLVMBuildFAdd(cg->builder, old_val, one, "finc");
  } else {
    // constant 1 or -1
    int onevar = 1;
    if (negate)
      onevar = -1;
    one = LLVMConstInt(ty, onevar, 0);
    new_val = LLVMBuildAdd(cg->builder, old_val, one, "inc");
  }

  LLVMBuildStore(cg->builder, new_val, s->value);

  // return depending on prefix/postfix
  return is_postfix ? old_val : new_val;
}

static LLVMValueRef cast_to_type(Codegen *cg, LLVMValueRef val,
                                 LLVMTypeRef target_ty) {
  LLVMTypeRef from_ty = LLVMTypeOf(val);
  if (from_ty == target_ty)
    return val;

  LLVMTypeKind from_kind = LLVMGetTypeKind(from_ty);
  LLVMTypeKind to_kind = LLVMGetTypeKind(target_ty);

  if (from_kind == LLVMIntegerTypeKind &&
      (to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind)) {
    return LLVMBuildSIToFP(cg->builder, val, target_ty, "cast_itofp");
  } else if ((from_kind == LLVMFloatTypeKind ||
              from_kind == LLVMDoubleTypeKind) &&
             to_kind == LLVMIntegerTypeKind) {
    return LLVMBuildFPToSI(cg->builder, val, target_ty, "cast_fptosi");
  } else if (from_kind == LLVMIntegerTypeKind &&
             to_kind == LLVMIntegerTypeKind) {
    return LLVMBuildIntCast(cg->builder, val, target_ty, "cast_int");
  } else if ((from_kind == LLVMFloatTypeKind ||
              from_kind == LLVMDoubleTypeKind) &&
             (to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind)) {
    return LLVMBuildFPCast(cg->builder, val, target_ty, "cast_fp");
  }
  return val;
}

static LLVMValueRef codegen_expression(Codegen *cg, AstNode *node) {
  if (!node) {
    fprintf(stderr, "Null expression node\n");
    exit(1);
  }

  switch (node->tag) {

  case NODE_NUMBER: {
    char *text = node->number.raw_text;
    int has_dot = 0;
    int has_f = 0;
    for (int i = 0; text[i]; i++) {
      if (text[i] == '.')
        has_dot = 1;
      if (text[i] == 'f' || text[i] == 'F')
        has_f = 1;
    }
    if (has_f) {
      return LLVMConstReal(LLVMFloatTypeInContext(cg->context),
                           node->number.value);
    } else if (has_dot) {
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

  case NODE_STRING:
    return LLVMBuildGlobalStringPtr(cg->builder, node->string.value, "str");

  case NODE_VAR: {
    CGSymbol *s = CGSymbolTable_find(&cg->symbols, node->var.value);
    if (!s) {
      fprintf(stderr, "Undefined variable '%s'\n", node->var.value);
      exit(1);
    }

    LLVMTypeRef ty = Codegen_type(cg, s->type);

    return LLVMBuildLoad2(cg->builder, ty, s->value, node->var.value);
  }

  case NODE_BINARY: {
    LLVMValueRef left = codegen_expression(cg, node->binary.left);
    LLVMValueRef right = codegen_expression(cg, node->binary.right);

    LLVMTypeRef left_ty = LLVMTypeOf(left);
    LLVMTypeRef right_ty = LLVMTypeOf(right);

    if (left_ty != right_ty) {
      LLVMTypeKind left_kind = LLVMGetTypeKind(left_ty);
      LLVMTypeKind right_kind = LLVMGetTypeKind(right_ty);

      // Promotion logic
      if (left_kind == LLVMDoubleTypeKind || right_kind == LLVMDoubleTypeKind) {
        LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
        if (left_kind == LLVMFloatTypeKind)
          left = LLVMBuildFPCast(cg->builder, left, double_ty, "promotelf");
        else if (left_kind == LLVMIntegerTypeKind)
          left = LLVMBuildSIToFP(cg->builder, left, double_ty, "promoteli");

        if (right_kind == LLVMFloatTypeKind)
          right = LLVMBuildFPCast(cg->builder, right, double_ty, "promoterf");
        else if (right_kind == LLVMIntegerTypeKind)
          right = LLVMBuildSIToFP(cg->builder, right, double_ty, "promoteri");

        left_ty = double_ty;
      } else if (left_kind == LLVMFloatTypeKind ||
                 right_kind == LLVMFloatTypeKind) {
        LLVMTypeRef float_ty = LLVMFloatTypeInContext(cg->context);
        if (left_kind == LLVMIntegerTypeKind)
          left = LLVMBuildSIToFP(cg->builder, left, float_ty, "promoteli");
        if (right_kind == LLVMIntegerTypeKind)
          right = LLVMBuildSIToFP(cg->builder, right, float_ty, "promoteri");
        left_ty = float_ty;
      } else if (left_kind == LLVMIntegerTypeKind &&
                 right_kind == LLVMIntegerTypeKind) {
        // Simple integer width promotion - not strictly correct for all cases
        // but prevents immediate crashes.
        unsigned left_width = LLVMGetIntTypeWidth(left_ty);
        unsigned right_width = LLVMGetIntTypeWidth(right_ty);
        if (left_width < right_width) {
          left = LLVMBuildSExt(cg->builder, left, right_ty, "sextl");
          left_ty = right_ty;
        } else {
          right = LLVMBuildSExt(cg->builder, right, left_ty, "sextr");
        }
      }
    }

    LLVMTypeKind kind = LLVMGetTypeKind(left_ty);

    switch (node->binary.op) {
    case OP_ADD:
      if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind)
        return LLVMBuildFAdd(cg->builder, left, right, "faddtmp");
      return LLVMBuildAdd(cg->builder, left, right, "addtmp");
    case OP_SUB:
      if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind)
        return LLVMBuildFSub(cg->builder, left, right, "fsubtmp");
      return LLVMBuildSub(cg->builder, left, right, "subtmp");
    case OP_MUL:
      if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind)
        return LLVMBuildFMul(cg->builder, left, right, "fmultmp");
      return LLVMBuildMul(cg->builder, left, right, "multmp");
    case OP_DIV:
      if (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind)
        return LLVMBuildFDiv(cg->builder, left, right, "fdivtmp");
      return LLVMBuildSDiv(cg->builder, left, right, "divtmp");
    case OP_POW: {
      left_ty = LLVMTypeOf(left);
      kind = LLVMGetTypeKind(left_ty);
      LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);

      // Potentially promote to double for external pow() call
      left = cast_to_type(cg, left, double_ty);
      right = cast_to_type(cg, right, double_ty);

      LLVMValueRef args[] = {left, right};
      LLVMValueRef res = LLVMBuildCall2(cg->builder, cg->pow_func_type,
                                        cg->pow_func, args, 2, "pow");

      // Cast back to original kind if needed
      return cast_to_type(cg, res, left_ty);
    }
    }
    break;
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
    char *name = node->assign_expr.target->var.value;
    CGSymbol *s = CGSymbolTable_find(&cg->symbols, name);
    if (!s) {
      fprintf(stderr, "Undefined variable '%s'\n", name);
      exit(1);
    }
    LLVMValueRef value = codegen_expression(cg, node->assign_expr.value);
    LLVMBuildStore(cg->builder, value, s->value);
    return value;
  }
  default:
    fprintf(stderr, "Unhandled expression node %d\n", node->tag);
    exit(1);
  }
}

static void codegen_var_decl(Codegen *cg, AstNode *node) {
  char *name = node->var_decl.name->var.value;
  LLVMTypeRef ty = Codegen_type(cg, node->var_decl.declared_type);
  LLVMValueRef value = codegen_expression(cg, node->var_decl.value);

  // Cast if needed (e.g. from literal to declared type)
  LLVMTypeRef val_ty = LLVMTypeOf(value);
  if (val_ty != ty) {
    LLVMTypeKind val_kind = LLVMGetTypeKind(val_ty);
    LLVMTypeKind target_kind = LLVMGetTypeKind(ty);

    if (val_kind == LLVMIntegerTypeKind &&
        (target_kind == LLVMFloatTypeKind ||
         target_kind == LLVMDoubleTypeKind)) {
      value = LLVMBuildSIToFP(cg->builder, value, ty, "var_itofp");
    } else if ((val_kind == LLVMFloatTypeKind ||
                val_kind == LLVMDoubleTypeKind) &&
               target_kind == LLVMIntegerTypeKind) {
      value = LLVMBuildFPToSI(cg->builder, value, ty, "var_fptosi");
    } else if (val_kind == LLVMIntegerTypeKind &&
               target_kind == LLVMIntegerTypeKind) {
      value = LLVMBuildIntCast(cg->builder, value, ty, "var_intcast");
    } else if ((val_kind == LLVMFloatTypeKind ||
                val_kind == LLVMDoubleTypeKind) &&
               (target_kind == LLVMFloatTypeKind ||
                target_kind == LLVMDoubleTypeKind)) {
      value = LLVMBuildFPCast(cg->builder, value, ty, "var_fpcast");
    }
  }

  // allocate variable
  LLVMValueRef var = LLVMBuildAlloca(cg->builder, ty, name);

  // store value
  LLVMBuildStore(cg->builder, value, var);
  CGSymbolTable_add(&cg->symbols, name, node->var_decl.declared_type, var);
}

static void codegen_print_stmt(Codegen *cg, AstNode *node) {
  LLVMValueRef value = codegen_expression(cg, node->print_stmt.value);

  TypeKind t = TYPE_INT;

  if (node->print_stmt.value->tag == NODE_VAR) {
    CGSymbol *s =
        CGSymbolTable_find(&cg->symbols, node->print_stmt.value->var.value);

    if (!s) {
      fprintf(stderr, "Undefined variable '%s'\n",
              node->print_stmt.value->var.value);
      exit(1);
    }

    t = s->type;
  } else if (node->print_stmt.value->tag == NODE_CHAR) {
    t = TYPE_CHAR;
  } else if (node->print_stmt.value->tag == NODE_STRING) {
    t = TYPE_STRING;
  }

  const char *fmt_str = "%d\n";

  if (t == TYPE_CHAR) {
    fmt_str = "%c\n";
    // varargs in C promote char to int for printf %c
    value = LLVMBuildSExt(cg->builder, value, Codegen_type(cg, TYPE_INT),
                          "char_to_int");
  } else if (t == TYPE_STRING) {
    fmt_str = "%s\n";
  } else if (t == TYPE_FLOAT || t == TYPE_DOUBLE) {
    fmt_str = "%f\n";
    // promote float to double for printf
    value = LLVMBuildFPCast(cg->builder, value, Codegen_type(cg, TYPE_DOUBLE),
                            "fptofp");
  }

  LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "fmt");

  LLVMValueRef args[] = {fmt, value};

  LLVMBuildCall2(cg->builder, cg->printf_func_type, cg->printf_func, args, 2,
                 "");
}

static void codegen_statement(Codegen *cg, AstNode *node) {
  if (!node) {
    fprintf(stderr, "Null statement node\n");
    exit(1);
  }

  switch (node->tag) {

  case NODE_VAR_DECL: {
    char *name = node->var_decl.name->var.value;
    LLVMTypeRef ty = Codegen_type(cg, node->var_decl.declared_type);
    LLVMValueRef value = codegen_expression(cg, node->var_decl.value);

    // Cast if needed (e.g. from literal to declared type)
    value = cast_to_type(cg, value, ty);

    // allocate variable
    LLVMValueRef var = LLVMBuildAlloca(cg->builder, ty, name);

    // store value
    LLVMBuildStore(cg->builder, value, var);
    CGSymbolTable_add(&cg->symbols, name, node->var_decl.declared_type, var);
    break;
  }

  case NODE_PRINT_STMT: {
    LLVMValueRef value = codegen_expression(cg, node->print_stmt.value);
    TypeKind t = TYPE_INT;

    // Determine type for format string
    if (node->print_stmt.value->tag == NODE_VAR) {
      CGSymbol *s =
          CGSymbolTable_find(&cg->symbols, node->print_stmt.value->var.value);
      if (s)
        t = s->type;
    } else if (node->print_stmt.value->tag == NODE_NUMBER) {
      // Lit numbers are trickier, but let's assume they're numeric
      LLVMTypeRef ty = LLVMTypeOf(value);
      if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind)
        t = TYPE_FLOAT;
      else if (LLVMGetTypeKind(ty) == LLVMDoubleTypeKind)
        t = TYPE_DOUBLE;
      else
        t = TYPE_INT;
    } else if (node->print_stmt.value->tag == NODE_CHAR) {
      t = TYPE_CHAR;
    } else if (node->print_stmt.value->tag == NODE_STRING) {
      t = TYPE_STRING;
    } else if (node->print_stmt.value->tag == NODE_CAST_EXPR) {
      t = node->print_stmt.value->cast_expr.target_type;
    }

    const char *fmt_str = "%d\n";

    if (t == TYPE_CHAR) {
      fmt_str = "%c\n";
      value = LLVMBuildSExt(cg->builder, value, Codegen_type(cg, TYPE_INT),
                            "char_to_int");
    } else if (t == TYPE_STRING) {
      fmt_str = "%s\n";
    } else if (t == TYPE_FLOAT || t == TYPE_DOUBLE || t == TYPE_F32 ||
               t == TYPE_F64) {
      fmt_str = "%f\n";
      // promote float to double for printf
      value = LLVMBuildFPCast(cg->builder, value, Codegen_type(cg, TYPE_DOUBLE),
                              "fptofp");
    } else if (t == TYPE_U32 || t == TYPE_U64 || t == TYPE_U16 ||
               t == TYPE_U8) {
      fmt_str = "%u\n";
      if (t == TYPE_U64)
        fmt_str = "%lu\n";
    } else if (t == TYPE_I64) {
      fmt_str = "%ld\n";
    }

    LLVMValueRef fmt = LLVMBuildGlobalStringPtr(cg->builder, fmt_str, "fmt");
    LLVMValueRef args[] = {fmt, value};
    LLVMBuildCall2(cg->builder, cg->printf_func_type, cg->printf_func, args, 2,
                   "");
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
    AstNode *stmt = program->program.children.items[i];

    if (!stmt) {
      fprintf(stderr, "Null statement in AST\n");
      exit(1);
    }

    codegen_statement(cg, stmt);
  }

  Codegen_return_zero(cg);
}
