#include <llvm-c/Core.h>

#include "codegen_private.h"
#include "tyl/ast.h"

static LLVMValueRef cg_const_expr(Codegen *cg, AstNode *node) {
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->resolved_type);

  switch (node->tag) {
  case NODE_NUMBER: {
    double val = node->number.value;

    if (LLVMGetTypeKind(target_ty) == LLVMDoubleTypeKind ||
        LLVMGetTypeKind(target_ty) == LLVMFloatTypeKind) {
      return LLVMConstReal(target_ty, val);
    }

    return LLVMConstInt(target_ty, (long long)val, true);
  }
  case NODE_BOOL:
    return LLVMConstInt(target_ty, node->boolean.value, false);

  case NODE_CHAR:
    return LLVMConstInt(target_ty, node->char_lit.value, false);

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

  // The semantic layer already calculated the target type for the result.
  // We cast both operands to this target type to ensure consistency.
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->resolved_type);
  lhs = cg_cast_value(cg, lhs, target_ty);
  rhs = cg_cast_value(cg, rhs, target_ty);

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
  LLVMTypeRef res_ty = cg_get_llvm_type(cg, node->resolved_type);

  if (op == OP_NEG) {
    LLVMValueRef val = cg_expression(cg, expr);
    val = cg_cast_value(cg, val, res_ty);

    if (LLVMGetTypeKind(res_ty) == LLVMDoubleTypeKind ||
        LLVMGetTypeKind(res_ty) == LLVMFloatTypeKind) {
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

  LLVMValueRef one = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind ||
                      LLVMGetTypeKind(type) == LLVMFloatTypeKind)
                         ? LLVMConstReal(type, 1.0)
                         : LLVMConstInt(type, 1, false);

  LLVMValueRef new_val;
  if (op == OP_PRE_INC || op == OP_POST_INC) {
    new_val = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind ||
               LLVMGetTypeKind(type) == LLVMFloatTypeKind)
                  ? LLVMBuildFAdd(cg->builder, old_val, one, "inc")
                  : LLVMBuildAdd(cg->builder, old_val, one, "inc");
  } else {
    new_val = (LLVMGetTypeKind(type) == LLVMDoubleTypeKind ||
               LLVMGetTypeKind(type) == LLVMFloatTypeKind)
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

  LLVMTypeRef i1_ty = LLVMInt1TypeInContext(cg->context);
  cond = cg_cast_value(cg, cond, i1_ty);

  LLVMValueRef current_func = cg->current_function_ref->value;
  LLVMBasicBlockRef then_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_then");
  LLVMBasicBlockRef else_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_else");
  LLVMBasicBlockRef merge_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_cont");

  LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);

  LLVMTypeRef res_type = cg_get_llvm_type(cg, node->resolved_type);

  LLVMPositionBuilderAtEnd(cg->builder, then_bb);
  LLVMValueRef then_val = cg_expression(cg, node->ternary.true_expr);
  then_val = cg_cast_value(cg, then_val, res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  then_bb = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, else_bb);
  LLVMValueRef else_val = cg_expression(cg, node->ternary.false_expr);
  else_val = cg_cast_value(cg, else_val, res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  else_bb = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);

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

  LLVMTypeRef target_ty =
      cg_get_llvm_type(cg, node->assign_expr.target->resolved_type);
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

static LLVMValueRef cg_array_literal(Codegen *cg, AstNode *node) {
  Type *array_ty = node->resolved_type;
  Type *elem_ty = array_ty->data.array.element;
  size_t count = node->array_literal.items.len;

  LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, elem_ty);
  LLVMTypeRef arr_type = LLVMArrayType(llvm_elem_ty, count);

  LLVMValueRef backing_ptr =
      LLVMBuildAlloca(cg->builder, arr_type, "arr_back_store");

  for (size_t i = 0; i < count; i++) {
    AstNode *item = node->array_literal.items.items[i];
    LLVMValueRef item_val = cg_expression(cg, item);
    item_val = cg_cast_value(cg, item_val, llvm_elem_ty);

    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), i, false)};

    LLVMValueRef slot_ptr = LLVMBuildGEP2(cg->builder, arr_type, backing_ptr,
                                          indices, 2, "arr_init_gep");
    LLVMBuildStore(cg->builder, item_val, slot_ptr);
  }

  LLVMTypeRef fat_ptr_type = cg_get_llvm_type(cg, array_ty);
  LLVMValueRef fat_ptr = LLVMGetUndef(fat_ptr_type);

  LLVMValueRef len_val =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, false);
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, len_val, 0, "fat_len");

  LLVMValueRef first_elem_indices[] = {
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false)};
  LLVMValueRef data_ptr = LLVMBuildGEP2(cg->builder, arr_type, backing_ptr,
                                        first_elem_indices, 2, "arr_decay_gep");
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, data_ptr, 1, "fat_data");

  return fat_ptr;
}

static LLVMValueRef cg_array_repeat(Codegen *cg, AstNode *node) {
  Type *array_ty = node->resolved_type;
  Type *elem_ty = array_ty->data.array.element;
  size_t count = array_ty->data.array.fixed_size;

  LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, elem_ty);
  LLVMTypeRef arr_type = LLVMArrayType(llvm_elem_ty, count);

  LLVMValueRef backing_ptr =
      LLVMBuildAlloca(cg->builder, arr_type, "arr_back_store");

  LLVMValueRef item_val = cg_expression(cg, node->array_repeat.value);
  item_val = cg_cast_value(cg, item_val, llvm_elem_ty);

  for (size_t i = 0; i < count; i++) {
    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), i, false)};

    LLVMValueRef slot_ptr = LLVMBuildGEP2(cg->builder, arr_type, backing_ptr,
                                          indices, 2, "arr_init_gep");
    LLVMBuildStore(cg->builder, item_val, slot_ptr);
  }

  LLVMTypeRef fat_ptr_type = cg_get_llvm_type(cg, array_ty);
  LLVMValueRef fat_ptr = LLVMGetUndef(fat_ptr_type);

  LLVMValueRef len_val =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), count, false);
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, len_val, 0, "fat_len");

  LLVMValueRef first_elem_indices[] = {
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false)};
  LLVMValueRef data_ptr = LLVMBuildGEP2(cg->builder, arr_type, backing_ptr,
                                        first_elem_indices, 2, "arr_decay_gep");
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, data_ptr, 1, "fat_data");

  return fat_ptr;
}

static LLVMValueRef cg_index_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef fat_ptr = cg_expression(cg, node->index.array);
  LLVMValueRef index = cg_expression(cg, node->index.index);

  Type *arr_ty = node->index.array->resolved_type;
  LLVMTypeRef llvm_elem_ty = cg_get_llvm_type(cg, arr_ty->data.array.element);

  LLVMValueRef len =
      LLVMBuildExtractValue(cg->builder, fat_ptr, 0, "arr_len_check");
  LLVMValueRef data_ptr =
      LLVMBuildExtractValue(cg->builder, fat_ptr, 1, "arr_extract_ptr");

  LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
  index = cg_cast_value(cg, index, i64_ty);

  LLVMValueRef is_lt =
      LLVMBuildICmp(cg->builder, LLVMIntULT, index, len, "bounds_check_lt");

  LLVMBasicBlockRef current_bb = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef func = LLVMGetBasicBlockParent(current_bb);

  LLVMBasicBlockRef ok_bb =
      LLVMAppendBasicBlockInContext(cg->context, func, "index_ok");
  LLVMBasicBlockRef fail_bb =
      LLVMAppendBasicBlockInContext(cg->context, func, "index_fail");

  LLVMBuildCondBr(cg->builder, is_lt, ok_bb, fail_bb);

  // On failure, abort the program. In a real implementation, we might want to
  // return an Option or something instead.
  LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
  // For now, just trap. Later we can print a nice error.
  LLVMValueRef trap = LLVMGetNamedFunction(cg->module, "llvm.trap");
  LLVMTypeRef trap_ty =
      LLVMFunctionType(LLVMVoidTypeInContext(cg->context), NULL, 0, false);
  if (!trap) {
    trap = LLVMAddFunction(cg->module, "llvm.trap", trap_ty);
  }
  LLVMBuildCall2(cg->builder, trap_ty, trap, NULL, 0, "");
  LLVMBuildUnreachable(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, ok_bb);

  LLVMValueRef element_ptr = LLVMBuildGEP2(cg->builder, llvm_elem_ty, data_ptr,
                                           &index, 1, "arr_index_gep");

  return LLVMBuildLoad2(cg->builder, llvm_elem_ty, element_ptr, "arr_load");
}

static LLVMValueRef cg_field_expr(Codegen *cg, AstNode *node) {
  AstNode *obj_node = node->field.object;
  Type *obj_type = obj_node->resolved_type;

  if (obj_type->kind == KIND_ARRAY) {
    if (sv_eq_cstr(node->field.field, "len")) {
      LLVMValueRef fat_ptr = cg_expression(cg, obj_node);
      return LLVMBuildExtractValue(cg->builder, fat_ptr, 0, "arr_len_extract");
    }
  }

  return NULL;
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

  case NODE_ARRAY_LITERAL:
    return cg_array_literal(cg, node);

  case NODE_ARRAY_REPEAT:
    return cg_array_repeat(cg, node);

  case NODE_INDEX:
    return cg_index_expr(cg, node);

  case NODE_FIELD:
    return cg_field_expr(cg, node);

  default:
    fprintf(stderr, "Codegen: Unhandled expression tag %d\n", node->tag);
    return NULL;
  }
}
