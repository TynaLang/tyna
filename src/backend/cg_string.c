#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"

LLVMValueRef cg_get_string_constant_ptr(Codegen *cg, StringView str) {
  size_t idx = cg_string_pool_insert(cg, str);
  LLVMValueRef global = cg->string_globals.items[idx];
  LLVMTypeRef i8_ptr_ty =
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  return LLVMBuildBitCast(cg->builder, global, i8_ptr_ty, "str_ptr");
}

LLVMValueRef cg_string_hash(Codegen *cg, LLVMValueRef str_val) {
  CgFunc *fn = cg_find_function(cg, sv_from_cstr("__tyna_str_hash"));
  if (!fn)
    panic("Missing __tyna_str_hash runtime helper");
  return LLVMBuildCall2(cg->builder, fn->type, fn->value, &str_val, 1,
                        "str_hash");
}

LLVMValueRef cg_string_equals(Codegen *cg, LLVMValueRef a, LLVMValueRef b) {
  CgFunc *fn = cg_find_function(cg, sv_from_cstr("__tyna_str_equals"));
  if (!fn)
    panic("Missing __tyna_str_equals runtime helper");
  LLVMValueRef args[] = {a, b};
  LLVMValueRef result =
      LLVMBuildCall2(cg->builder, fn->type, fn->value, args, 2, "str_eq_int");
  return LLVMBuildICmp(cg->builder, LLVMIntEQ, result,
                       LLVMConstInt(LLVMInt32TypeInContext(cg->context), 1, 0),
                       "str_eq");
}

LLVMValueRef cg_equality_expr(Codegen *cg, LLVMValueRef lhs, LLVMValueRef rhs,
                              EqualityOp op, Type *left_ty, Type *right_ty) {
  Type *lt = left_ty;
  while (lt && lt->kind == KIND_POINTER)
    lt = lt->data.pointer_to;
  Type *rt = right_ty;
  while (rt && rt->kind == KIND_POINTER)
    rt = rt->data.pointer_to;

  bool left_is_str_like =
      lt && (lt->kind == KIND_STRING_BUFFER ||
             (lt->kind == KIND_PRIMITIVE && lt->data.primitive == PRIM_STRING));
  bool right_is_str_like =
      rt && (rt->kind == KIND_STRING_BUFFER ||
             (rt->kind == KIND_PRIMITIVE && rt->data.primitive == PRIM_STRING));

  if (left_is_str_like && right_is_str_like) {
    CgFunc *eq_fn =
        cg_find_system_function(cg, sv_from_cstr("__tyna_str_equals"));
    if (!eq_fn)
      panic("Missing __tyna_str_equals runtime helper");
    LLVMValueRef args[] = {lhs, rhs};
    LLVMValueRef res = LLVMBuildCall2(cg->builder, eq_fn->type, eq_fn->value,
                                      args, 2, "str_eq");
    LLVMValueRef one =
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 1, false);
    LLVMIntPredicate pred = (op == OP_EQ) ? LLVMIntEQ : LLVMIntNE;
    return LLVMBuildICmp(cg->builder, pred, res, one, "str_is_eq");
  }

  LLVMTypeRef type = LLVMTypeOf(lhs);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  bool is_float = (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind);

  if (is_float) {
    LLVMRealPredicate pred = (op == OP_EQ) ? LLVMRealOEQ : LLVMRealONE;
    return LLVMBuildFCmp(cg->builder, pred, lhs, rhs, "feqtmp");
  } else {
    LLVMIntPredicate pred = (op == OP_EQ) ? LLVMIntEQ : LLVMIntNE;
    return LLVMBuildICmp(cg->builder, pred, lhs, rhs, "ieqtmp");
  }
}
