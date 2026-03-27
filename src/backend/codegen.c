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
    return LLVMInt32TypeInContext(cg->context);
  case TYPE_CHAR:
    return LLVMInt8TypeInContext(cg->context);
  case TYPE_STRING:
    return LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  default:
    fprintf(stderr, "Unknown type\n");
    exit(1);
  }
}

static LLVMValueRef codegen_increment(Codegen *cg, AstNode *node,
                                      int is_postfix) {
  // must be identifier (you already enforce this)
  char *name = node->unary.expr->ident.value;

  CGSymbol *s = CGSymbolTable_find(&cg->symbols, name);
  if (!s) {
    fprintf(stderr, "Undefined variable '%s'\n", name);
    exit(1);
  }

  LLVMTypeRef ty = Codegen_type(cg, s->type);

  LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, ty, s->value, name);

  // constant 1
  LLVMValueRef one = LLVMConstInt(ty, 1, 0);

  LLVMValueRef new_val = LLVMBuildAdd(cg->builder, old_val, one, "inc");

  LLVMBuildStore(cg->builder, new_val, s->value);

  // return depending on prefix/postfix
  return is_postfix ? old_val : new_val;
}

static LLVMValueRef codegen_expression(Codegen *cg, AstNode *node) {
  if (!node) {
    fprintf(stderr, "Null expression node\n");
    exit(1);
  }

  switch (node->tag) {

  case NODE_NUMBER:
    return LLVMConstInt(Codegen_type(cg, TYPE_INT), (int)node->number.value, 0);

  case NODE_CHAR:
    return LLVMConstInt(Codegen_type(cg, TYPE_CHAR), (int)node->char_lit.value,
                        0);

  case NODE_STRING:
    return LLVMBuildGlobalStringPtr(cg->builder, node->string.value, "str");

  case NODE_IDENT: {
    CGSymbol *s = CGSymbolTable_find(&cg->symbols, node->ident.value);
    if (!s) {
      fprintf(stderr, "Undefined variable '%s'\n", node->ident.value);
      exit(1);
    }

    LLVMTypeRef ty = Codegen_type(cg, s->type);

    return LLVMBuildLoad2(cg->builder, ty, s->value, node->ident.value);
  }

  case NODE_BINARY: {
    LLVMValueRef left = codegen_expression(cg, node->binary.left);
    LLVMValueRef right = codegen_expression(cg, node->binary.right);

    switch (node->binary.op) {
    case OP_ADD:
      return LLVMBuildAdd(cg->builder, left, right, "addtmp");
    case OP_SUB:
      return LLVMBuildSub(cg->builder, left, right, "subtmp");
    case OP_MUL:
      return LLVMBuildMul(cg->builder, left, right, "multmp");
    case OP_DIV:
      return LLVMBuildSDiv(cg->builder, left, right, "divtmp");
    case OP_POW: {
      LLVMValueRef left = codegen_expression(cg, node->binary.left);
      LLVMValueRef right = codegen_expression(cg, node->binary.right);

      // int → double
      left = LLVMBuildSIToFP(cg->builder, left,
                             LLVMDoubleTypeInContext(cg->context), "");
      right = LLVMBuildSIToFP(cg->builder, right,
                              LLVMDoubleTypeInContext(cg->context), "");

      LLVMValueRef args[] = {left, right};

      LLVMValueRef res = LLVMBuildCall2(cg->builder, cg->pow_func_type,
                                        cg->pow_func, args, 2, "pow");

      // double → int
      return LLVMBuildFPToSI(cg->builder, res,
                             LLVMInt32TypeInContext(cg->context), "");
    }
    }
  }
  case NODE_UNARY: {
    switch (node->unary.op) {
    case OP_PRE_INC:
      return codegen_increment(cg, node, 0);
    case OP_POST_INC:
      return codegen_increment(cg, node, 1);
    case OP_NEG: {
      LLVMValueRef val = codegen_expression(cg, node->unary.expr);
      return LLVMBuildNeg(cg->builder, val, "neg");
    }
    default:
      fprintf(stderr, "Unknown unary op\n");
      exit(1);
    }
  }
  default:
    fprintf(stderr, "Unhandled expression node %d\n", node->tag);
    exit(1);
  }
}

static void codegen_let(Codegen *cg, AstNode *node) {
  char *name = node->let.name->ident.value;

  LLVMTypeRef ty = Codegen_type(cg, node->let.declared_type);

  LLVMValueRef value = codegen_expression(cg, node->let.value);

  // allocate variable
  LLVMValueRef var = LLVMBuildAlloca(cg->builder, ty, name);

  // store value
  LLVMBuildStore(cg->builder, value, var);
  CGSymbolTable_add(&cg->symbols, name, node->let.declared_type, var);
}

static void codegen_print(Codegen *cg, AstNode *node) {
  LLVMValueRef value = codegen_expression(cg, node->print.value);

  TypeKind t = TYPE_INT;

  if (node->print.value->tag == NODE_IDENT) {
    CGSymbol *s =
        CGSymbolTable_find(&cg->symbols, node->print.value->ident.value);

    if (!s) {
      fprintf(stderr, "Undefined variable '%s'\n",
              node->print.value->ident.value);
      exit(1);
    }

    t = s->type;
  } else if (node->print.value->tag == NODE_CHAR) {
    t = TYPE_CHAR;
  } else if (node->print.value->tag == NODE_STRING) {
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

  case NODE_LET:
    codegen_let(cg, node);
    break;

  case NODE_PRINT:
    codegen_print(cg, node);
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
