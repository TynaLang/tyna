#include <llvm-c/Core.h>

#include "codegen_private.h"
#include "tyl/ast.h"

static LLVMValueRef cg_const_expr(Codegen *cg, AstNode *node) {
  switch (node->tag) {
  case NODE_NUMBER: {
    double val = node->number.value;

    // Check if the double is actually a whole number
    if (val == (double)((long long)val)) {
      LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
      return LLVMConstInt(i64_ty, (long long)val, true);
    }

    return LLVMConstReal(LLVMDoubleTypeInContext(cg->context), val);
  }
  case NODE_BOOL:
    return LLVMConstInt(LLVMInt1TypeInContext(cg->context), node->boolean.value,
                        false);

  case NODE_CHAR:
    return LLVMConstInt(LLVMInt8TypeInContext(cg->context),
                        node->char_lit.value, false);

  case NODE_STRING:
    return LLVMBuildGlobalStringPtr(cg->builder, sv_to_cstr(node->string.value),
                                    "str_lit");

  default:
    return NULL;
  }
}

static LLVMValueRef cg_var_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef ptr = cg_get_address(cg, node);
  if (!ptr) {
    panic("Undefined variable during codegen");
  }

  LLVMTypeRef type = LLVMGetAllocatedType(ptr);
  return LLVMBuildLoad2(cg->builder, type, ptr, "var_load");
}

static LLVMValueRef cg_arith_expr(Codegen *cg, LLVMValueRef lhs,
                                  LLVMValueRef rhs, ArithmOp op) {
  LLVMTypeRef type = LLVMTypeOf(lhs);
  LLVMTypeKind kind = LLVMGetTypeKind(type);

  bool is_float = (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind);

  switch (op) {
  case OP_ADD:
    return is_float ? LLVMBuildFAdd(cg->builder, lhs, rhs, "faddtmp")
                    : LLVMBuildAdd(cg->builder, lhs, rhs, "addtmp");
  case OP_SUB:
    return is_float ? LLVMBuildFSub(cg->builder, lhs, rhs, "fsubtmp")
                    : LLVMBuildSub(cg->builder, lhs, rhs, "subtmp");
  case OP_MUL:
    return is_float ? LLVMBuildFMul(cg->builder, lhs, rhs, "fmultmp")
                    : LLVMBuildMul(cg->builder, lhs, rhs, "multmp");
  case OP_DIV:
    if (is_float) {
      return LLVMBuildFDiv(cg->builder, lhs, rhs, "fdivtmp");
    } else {
      // We use SDiv (Signed Divide). For Unsigned, you'd use UDiv.
      return LLVMBuildSDiv(cg->builder, lhs, rhs, "sdivtmp");
    }
  case OP_MOD:
    return is_float ? LLVMBuildFRem(cg->builder, lhs, rhs, "fremtmp")
                    : LLVMBuildSRem(cg->builder, lhs, rhs, "sremtmp");

  case OP_POW: {
    CGFunction *pow_fn = cg_find_function(cg, sv_from_parts("pow", 3));

    LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
    lhs = cg_cast_value(cg, lhs, double_ty);
    rhs = cg_cast_value(cg, rhs, double_ty);

    LLVMValueRef args[] = {lhs, rhs};
    return LLVMBuildCall2(cg->builder, pow_fn->type, pow_fn->value, args, 2,
                          "powtmp");
  }

  default:
    return NULL;
  }
}

static LLVMValueRef cg_compare_expr(Codegen *cg, LLVMValueRef lhs,
                                    LLVMValueRef rhs, CompareOp op) {
  LLVMTypeRef type = LLVMTypeOf(lhs);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  bool is_float = (kind == LLVMDoubleTypeKind || kind == LLVMFloatTypeKind);

  if (is_float) {
    LLVMRealPredicate pred;
    switch (op) {
    case OP_LT:
      pred = LLVMRealOLT;
      break;
    case OP_GT:
      pred = LLVMRealOGT;
      break;
    case OP_LE:
      pred = LLVMRealOLE;
      break;
    case OP_GE:
      pred = LLVMRealOGE;
      break;
    default:
      return NULL;
    }
    return LLVMBuildFCmp(cg->builder, pred, lhs, rhs, "fcmptmp");
  } else {
    LLVMIntPredicate pred;
    switch (op) {
    case OP_LT:
      pred = LLVMIntSLT;
      break;
    case OP_GT:
      pred = LLVMIntSGT;
      break;
    case OP_LE:
      pred = LLVMIntSLE;
      break;
    case OP_GE:
      pred = LLVMIntSGE;
      break;
    default:
      return NULL;
    }
    return LLVMBuildICmp(cg->builder, pred, lhs, rhs, "icmptmp");
  }
}

static LLVMValueRef cg_equality_expr(Codegen *cg, LLVMValueRef lhs,
                                     LLVMValueRef rhs, EqualityOp op) {
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

static LLVMValueRef cg_logical_expr(Codegen *cg, AstNode *node,
                                    LLVMValueRef lhs) {
  LLVMBasicBlockRef original_block = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef function = cg->current_function;

  LLVMBasicBlockRef rhs_block =
      LLVMAppendBasicBlockInContext(cg->context, function, "log_rhs");
  LLVMBasicBlockRef merge_block =
      LLVMAppendBasicBlockInContext(cg->context, function, "log_merge");

  if (node->binary_logical.op == OP_AND) {
    LLVMBuildCondBr(cg->builder, lhs, rhs_block, merge_block);
  } else {
    LLVMBuildCondBr(cg->builder, lhs, merge_block, rhs_block);
  }

  LLVMPositionBuilderAtEnd(cg->builder, rhs_block);
  LLVMValueRef rhs = cg_expression(cg, node->binary_logical.right);
  LLVMBuildBr(cg->builder, merge_block);
  LLVMBasicBlockRef rhs_end_block = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_block);
  LLVMValueRef phi =
      LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->context), "log_phi");

  LLVMValueRef incoming_values[2];
  LLVMBasicBlockRef incoming_blocks[2];

  incoming_values[0] = LLVMConstInt(LLVMInt1TypeInContext(cg->context),
                                    (node->binary_logical.op == OP_OR), false);
  incoming_blocks[0] = original_block;

  incoming_values[1] = rhs;
  incoming_blocks[1] = rhs_end_block;

  LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
  return phi;
}

static LLVMValueRef cg_binary_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef lhs = cg_expression(cg, node->binary_arith.left);
  if (!lhs)
    return NULL;

  if (node->tag == NODE_BINARY_LOGICAL) {
    return cg_logical_expr(cg, node, lhs);
  }

  LLVMValueRef rhs = cg_expression(cg, node->binary_arith.right);
  if (!rhs)
    return NULL;

  cg_binary_sync_types(cg, &lhs, &rhs);

  switch (node->tag) {
  case NODE_BINARY_ARITH:
    return cg_arith_expr(cg, lhs, rhs, node->binary_arith.op);
  case NODE_BINARY_COMPARE:
    return cg_compare_expr(cg, lhs, rhs, node->binary_compare.op);
  case NODE_BINARY_EQUALITY:
    return cg_equality_expr(cg, lhs, rhs, node->binary_equality.op);
  default:
    return NULL;
  }
}

static LLVMValueRef cg_unary_expr(Codegen *cg, AstNode *node) {
  UnaryOp op = node->unary.op;
  AstNode *expr = node->unary.expr;

  if (op == OP_NEG) {
    LLVMValueRef val = cg_expression(cg, expr);
    if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMDoubleTypeKind) {
      return LLVMBuildFNeg(cg->builder, val, "fnegtmp");
    }
    return LLVMBuildNeg(cg->builder, val, "negtmp");
  }

  LLVMValueRef ptr = cg_get_address(cg, expr);
  if (!ptr) {
    panic("Invalid unary operand target");
  }

  // We need the type to specify correctly
  LLVMTypeRef type = LLVMGetAllocatedType(ptr);

  LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, type, ptr, "u_load");

  LLVMValueRef one = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind)
                         ? LLVMConstReal(type, 1.0)
                         : LLVMConstInt(type, 1, false);

  LLVMValueRef new_val;
  if (op == OP_PRE_INC || op == OP_POST_INC) {
    new_val = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind)
                  ? LLVMBuildFAdd(cg->builder, old_val, one, "inc")
                  : LLVMBuildAdd(cg->builder, old_val, one, "inc");
  } else {
    new_val = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind)
                  ? LLVMBuildFSub(cg->builder, old_val, one, "dec")
                  : LLVMBuildSub(cg->builder, old_val, one, "dec");
  }

  LLVMBuildStore(cg->builder, new_val, ptr);

  if (op == OP_PRE_INC || op == OP_PRE_DEC) {
    return new_val;
  } else {
    return old_val;
  }
}

static LLVMValueRef cg_ternary_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef cond = cg_expression(cg, node->ternary.condition);

  LLVMTypeRef cond_ty = LLVMTypeOf(cond);
  LLVMTypeKind cond_kind = LLVMGetTypeKind(cond_ty);
  if (cond_kind != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(cond_ty) != 1) {
      cond = LLVMBuildTruncOrBitCast(cg->builder, cond, LLVMInt1TypeInContext(cg->context), "cond_i1");
  }

  LLVMValueRef current_func = cg->current_function_ref->value;
  LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_then");
  LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_else");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_cont");

  LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);

  LLVMPositionBuilderAtEnd(cg->builder, then_bb);
  LLVMValueRef then_val = cg_expression(cg, node->ternary.true_expr);
  LLVMBuildBr(cg->builder, merge_bb);
  then_bb = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, else_bb);
  LLVMValueRef else_val = cg_expression(cg, node->ternary.false_expr);
  LLVMBuildBr(cg->builder, merge_bb);
  else_bb = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);

  LLVMTypeRef res_type = LLVMTypeOf(then_val);
  
  if (LLVMTypeOf(then_val) != LLVMTypeOf(else_val)) {
    // If one is double and other is an integer type (implicit cast allowed from semantic phase)
    if (LLVMGetTypeKind(LLVMTypeOf(then_val)) == LLVMDoubleTypeKind) {
      else_val = cg_cast_value(cg, else_val, res_type);
    } else if (LLVMGetTypeKind(LLVMTypeOf(else_val)) == LLVMDoubleTypeKind) {
      res_type = LLVMTypeOf(else_val);
      then_val = cg_cast_value(cg, then_val, res_type);
    } else {
        // cast else_val to then_val type since semantic analysis verified compatibility
        else_val = cg_cast_value(cg, else_val, res_type);
    }
  }

  LLVMValueRef phi = LLVMBuildPhi(cg->builder, res_type, "ternary_tmp");
  LLVMValueRef incoming_values[] = {then_val, else_val};
  LLVMBasicBlockRef incoming_blocks[] = {then_bb, else_bb};

  LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
  return phi;
}

static LLVMValueRef cg_assign_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef ptr = cg_get_address(cg, node->assign_expr.target);
  if (!ptr)
    panic("Invalid assignment target");

  LLVMValueRef val = cg_expression(cg, node->assign_expr.value);

  // We need the type to cast correctly
  LLVMTypeRef target_ty = LLVMGetAllocatedType(ptr);
  val = cg_cast_value(cg, val, target_ty);

  LLVMBuildStore(cg->builder, val, ptr);
  return val;
}

static LLVMValueRef cg_cast_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef val = cg_expression(cg, node->cast_expr.expr);
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->cast_expr.target_type);

  return cg_cast_value(cg, val, target_ty);
}

static LLVMValueRef cg_call_expr(Codegen *cg, AstNode *node) {
  if (node->call.func->tag != NODE_VAR) {
    panic("Function calls must currently be by name (NODE_VAR)");
  }

  StringView fn_name = node->call.func->var.value;
  CGFunction *fn = cg_find_function(cg, fn_name);

  if (!fn) {
    panic("Call to undefined function '" SV_FMT "'", SV_ARG(fn_name));
  }

  unsigned arg_count = (unsigned)node->call.args.len;
  LLVMValueRef *args = NULL;

  if (arg_count > 0) {
    args = malloc(sizeof(LLVMValueRef) * arg_count);

    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * arg_count);
    LLVMGetParamTypes(fn->type, param_types);

    for (unsigned i = 0; i < arg_count; i++) {
      AstNode *arg_node = node->call.args.items[i];
      LLVMValueRef arg_val = cg_expression(cg, arg_node);

      args[i] = cg_cast_value(cg, arg_val, param_types[i]);
    }

    free(param_types);
  }

  LLVMTypeRef ret_ty = LLVMGetReturnType(fn->type);
  const char *call_name =
      (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind) ? "" : "calltmp";

  LLVMValueRef result = LLVMBuildCall2(cg->builder, fn->type, fn->value, args,
                                       arg_count, call_name);

  if (args)
    free(args);
  return result;
}

LLVMValueRef cg_expression(Codegen *cg, AstNode *node) {
  if (!node)
    return NULL;

  switch (node->tag) {
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_BOOL:
  case NODE_STRING:
    return cg_const_expr(cg, node);

  case NODE_VAR:
    return cg_var_expr(cg, node);

  case NODE_BINARY_ARITH:
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    return cg_binary_expr(cg, node);

  case NODE_UNARY:
    return cg_unary_expr(cg, node);

  case NODE_TERNARY:
    return cg_ternary_expr(cg, node);

  case NODE_ASSIGN_EXPR:
    return cg_assign_expr(cg, node);

  case NODE_CAST_EXPR:
    return cg_cast_expr(cg, node);

  case NODE_CALL:
    return cg_call_expr(cg, node);

  case NODE_FIELD:
  case NODE_INDEX:
    return NULL;

  default:
    fprintf(stderr, "Codegen: Unhandled expression tag %d\n", node->tag);
    return NULL;
  }
}
