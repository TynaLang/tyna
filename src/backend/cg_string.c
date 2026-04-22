#include "cg_internal.h"
#include "tyl/ast.h"
#include "tyl/codegen.h"

LLVMValueRef cg_get_string_constant_ptr(Codegen *cg, StringView str) {
  size_t idx = cg_string_pool_insert(cg, str);
  LLVMValueRef global = cg->string_globals.items[idx];
  LLVMTypeRef i8_ptr_ty =
      LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
  return LLVMBuildBitCast(cg->builder, global, i8_ptr_ty, "str_ptr");
}

LLVMValueRef cg_string_hash(Codegen *cg, LLVMValueRef str_val) {
  CGFunction *fn = cg_find_function(cg, sv_from_cstr("__tyl_str_hash"));
  if (!fn)
    panic("Missing __tyl_str_hash runtime helper");
  return LLVMBuildCall2(cg->builder, fn->type, fn->value, &str_val, 1,
                        "str_hash");
}

LLVMValueRef cg_string_equals(Codegen *cg, LLVMValueRef a, LLVMValueRef b) {
  CGFunction *fn = cg_find_function(cg, sv_from_cstr("__tyl_str_equals"));
  if (!fn)
    panic("Missing __tyl_str_equals runtime helper");
  LLVMValueRef args[] = {a, b};
  LLVMValueRef result =
      LLVMBuildCall2(cg->builder, fn->type, fn->value, args, 2, "str_eq_int");
  return LLVMBuildICmp(cg->builder, LLVMIntEQ, result,
                       LLVMConstInt(LLVMInt32TypeInContext(cg->context), 1, 0),
                       "str_eq");
}

bool cg_type_is_str_like(Type *t) {
  while (t && t->kind == KIND_POINTER)
    t = t->data.pointer_to;
  return t && (t->kind == KIND_STRING_BUFFER ||
               (t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_STRING));
}

LLVMValueRef cg_coerce_rvalue_to_str_slice(Codegen *cg, LLVMValueRef v,
                                           Type *ty) {
  while (ty && ty->kind == KIND_POINTER)
    ty = ty->data.pointer_to;
  if (!ty || ty->kind != KIND_STRING_BUFFER)
    return v;
  LLVMTypeRef str_ty =
      cg_get_llvm_type(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
  LLVMValueRef undef = LLVMGetUndef(str_ty);
  LLVMValueRef p = LLVMBuildExtractValue(cg->builder, v, 0, "buf2s_ptr");
  LLVMValueRef l = LLVMBuildExtractValue(cg->builder, v, 1, "buf2s_len");
  LLVMValueRef s0 = LLVMBuildInsertValue(cg->builder, undef, p, 0, "buf2s0");
  return LLVMBuildInsertValue(cg->builder, s0, l, 1, "buf2s");
}

LLVMValueRef cg_equality_expr(Codegen *cg, LLVMValueRef lhs, LLVMValueRef rhs,
                              EqualityOp op, Type *left_ty, Type *right_ty) {
  if (left_ty && right_ty && cg_type_is_str_like(left_ty) &&
      cg_type_is_str_like(right_ty)) {
    CGFunction *eq_fn =
        cg_find_system_function(cg, sv_from_cstr("__tyl_str_equals"));
    if (!eq_fn)
      panic("Missing __tyl_str_equals runtime helper");
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
