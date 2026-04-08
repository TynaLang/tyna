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

  case NODE_STRING: {
    size_t pool_idx = cg_string_pool_insert(cg, node->string.value);
    LLVMValueRef global_str = cg->string_globals.items[pool_idx];
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);

    LLVMTypeRef global_type = LLVMGetAllocatedType(global_str);
    LLVMValueRef indices[] = {LLVMConstInt(i64_ty, 0, false),
                              LLVMConstInt(i64_ty, 0, false)};

    LLVMValueRef data_ptr =
        LLVMConstInBoundsGEP2(global_type, global_str, indices, 2);

    // Create a global for the dimensions array of the string
    // Standard metadata for rank-1: [dim0, stride0] where stride0 = 1.
    char dims_name[64];
    static int str_dims_count = 0;
    sprintf(dims_name, ".str_dims.%d", str_dims_count++);
    LLVMValueRef dims_vals_data[] = {
        LLVMConstInt(i64_ty, node->string.value.len, false),
        LLVMConstInt(i64_ty, 1, false) // stride0
    };
    LLVMValueRef dims_arr_val = LLVMConstArray(i64_ty, dims_vals_data, 2);
    LLVMValueRef dims_global =
        LLVMAddGlobal(cg->module, LLVMArrayType(i64_ty, 2), dims_name);
    LLVMSetInitializer(dims_global, dims_arr_val);
    LLVMSetGlobalConstant(dims_global, true);
    LLVMSetLinkage(dims_global, LLVMPrivateLinkage);

    LLVMValueRef dims_indices[] = {LLVMConstInt(i64_ty, 0, false),
                                   LLVMConstInt(i64_ty, 0, false)};
    LLVMValueRef dims_ptr = LLVMConstInBoundsGEP2(LLVMArrayType(i64_ty, 2),
                                                  dims_global, dims_indices, 2);

    LLVMValueRef values[] = {LLVMConstInt(i64_ty, 1, false), // Rank = 1
                             data_ptr, dims_ptr};
    return LLVMConstStructInContext(cg->context, values, 3, false);
  }

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
    Type *f64_t = type_get_primitive(cg->type_ctx, PRIM_F64);
    lhs = cg_cast_value(cg, lhs, NULL, double_ty);
    rhs = cg_cast_value(cg, rhs, NULL, double_ty);

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
  AstNode *left_node =
      (node->tag == NODE_BINARY_ARITH)      ? node->binary_arith.left
      : (node->tag == NODE_BINARY_COMPARE)  ? node->binary_compare.left
      : (node->tag == NODE_BINARY_EQUALITY) ? node->binary_equality.left
      : (node->tag == NODE_BINARY_LOGICAL)  ? node->binary_logical.left
                                            : NULL;

  AstNode *right_node =
      (node->tag == NODE_BINARY_ARITH)      ? node->binary_arith.right
      : (node->tag == NODE_BINARY_COMPARE)  ? node->binary_compare.right
      : (node->tag == NODE_BINARY_EQUALITY) ? node->binary_equality.right
      : (node->tag == NODE_BINARY_LOGICAL)  ? node->binary_logical.right
                                            : NULL;

  LLVMValueRef lhs = cg_expression(cg, left_node);
  if (!lhs)
    return NULL;

  if (node->tag == NODE_BINARY_LOGICAL) {
    LLVMTypeRef i1_ty = LLVMInt1TypeInContext(cg->context);
    lhs = cg_cast_value(cg, lhs, left_node->resolved_type, i1_ty);
    return cg_logical_expr(cg, node, lhs);
  }

  LLVMValueRef rhs = cg_expression(cg, right_node);
  if (!rhs)
    return NULL;

  if (node->tag == NODE_BINARY_ARITH) {
    // The semantic layer already calculated the target type for the result.
    // We cast both operands to this target type to ensure consistency.
    LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->resolved_type);
    lhs = cg_cast_value(cg, lhs, left_node->resolved_type, target_ty);
    rhs = cg_cast_value(cg, rhs, right_node->resolved_type, target_ty);
    return cg_arith_expr(cg, lhs, rhs, node->binary_arith.op);
  }

  cg_binary_sync_types(cg, &lhs, left_node->resolved_type, &rhs,
                       right_node->resolved_type);

  switch (node->tag) {
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
    val = cg_cast_value(cg, val, expr->resolved_type, res_ty);

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
  cond = cg_cast_value(cg, cond, node->ternary.condition->resolved_type, i1_ty);

  LLVMValueRef current_func = cg->current_function_ref->value;
  LLVMBasicBlockRef then_bb_start =
      LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_then");
  LLVMBasicBlockRef else_bb_start =
      LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_else");
  LLVMBasicBlockRef merge_bb =
      LLVMAppendBasicBlockInContext(cg->context, current_func, "ternary_cont");

  LLVMBuildCondBr(cg->builder, cond, then_bb_start, else_bb_start);

  LLVMTypeRef res_type = cg_get_llvm_type(cg, node->resolved_type);

  // Then block
  LLVMPositionBuilderAtEnd(cg->builder, then_bb_start);
  LLVMValueRef then_val = cg_expression(cg, node->ternary.true_expr);
  then_val = cg_cast_value(cg, then_val, node->ternary.true_expr->resolved_type,
                           res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  LLVMBasicBlockRef then_bb_end = LLVMGetInsertBlock(cg->builder);

  // Else block
  LLVMPositionBuilderAtEnd(cg->builder, else_bb_start);
  LLVMValueRef else_val = cg_expression(cg, node->ternary.false_expr);
  else_val = cg_cast_value(cg, else_val,
                           node->ternary.false_expr->resolved_type, res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  LLVMBasicBlockRef else_bb_end = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);

  LLVMValueRef phi = LLVMBuildPhi(cg->builder, res_type, "ternary_tmp");
  LLVMValueRef incoming_values[] = {then_val, else_val};
  LLVMBasicBlockRef incoming_blocks[] = {then_bb_end, else_bb_end};

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
  val =
      cg_cast_value(cg, val, node->assign_expr.value->resolved_type, target_ty);

  LLVMBuildStore(cg->builder, val, ptr);
  return val;
}

static LLVMValueRef cg_cast_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef val = cg_expression(cg, node->cast_expr.expr);
  LLVMTypeRef target_ty = cg_get_llvm_type(cg, node->cast_expr.target_type);

  return cg_cast_value(cg, val, node->cast_expr.expr->resolved_type, target_ty);
}

static LLVMValueRef cg_call_expr(Codegen *cg, AstNode *node) {
  if (node->call.func->tag == NODE_VAR) {
    StringView name = node->call.func->var.value;
    if (sv_eq(name, sv_from_parts("free", 4))) {
      AstNode *arg_node = node->call.args.items[0];
      LLVMValueRef fat_ptr = cg_expression(cg, arg_node);

      LLVMValueRef data_ptr =
          LLVMBuildExtractValue(cg->builder, fat_ptr, 1, "free_ptr_extract");

      CGFunction *free_fn = cg_find_function(cg, sv_from_parts("free", 4));

      if (!free_fn) {
        LLVMTypeRef free_args[] = {
            LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0)};
        LLVMTypeRef free_type = LLVMFunctionType(
            LLVMVoidTypeInContext(cg->context), free_args, 1, false);
        LLVMValueRef free_val = LLVMAddFunction(cg->module, "free", free_type);
        CGFunction *f_free = xmalloc(sizeof(CGFunction));
        cg_init_CGFunction(f_free, sv_from_parts("free", 4), free_val,
                           free_type, true);
        List_push(&cg->system_functions, f_free);
        free_fn = f_free;
      }
      LLVMBuildCall2(cg->builder, free_fn->type, free_fn->value, &data_ptr, 1,
                     "");

      // Set original target to null if it's an lvalue (NODE_VAR/NODE_INDEX) For
      // now, only handle direct variable for simplicity.
      if (arg_node->tag == NODE_VAR) {
        LLVMValueRef addr = cg_get_address(cg, arg_node);
        if (addr) {
          LLVMTypeRef fat_ptr_type = LLVMGetAllocatedType(addr);
          LLVMValueRef zero_fat = LLVMGetUndef(fat_ptr_type);
          zero_fat = LLVMBuildInsertValue(
              cg->builder, zero_fat,
              LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, false), 0,
              "zero_rank");
          zero_fat =
              LLVMBuildInsertValue(cg->builder, zero_fat,
                                   LLVMConstPointerNull(LLVMPointerType(
                                       LLVMInt8TypeInContext(cg->context), 0)),
                                   1, "zero_ptr");
          zero_fat =
              LLVMBuildInsertValue(cg->builder, zero_fat,
                                   LLVMConstPointerNull(LLVMPointerType(
                                       LLVMInt64TypeInContext(cg->context), 0)),
                                   2, "zero_dims");
          LLVMBuildStore(cg->builder, zero_fat, addr);
        }
      }

      return NULL;
    }
  }

  if (node->call.func->tag == NODE_FIELD) {
    AstNode *obj_node = node->call.func->field.object;
    StringView member = node->call.func->field.field;

    if (obj_node->resolved_type->kind == KIND_PRIMITIVE &&
        obj_node->resolved_type->data.primitive == PRIM_STRING &&
        sv_eq(member, sv_from_parts("to_array", 8))) {
      LLVMValueRef fat_val = cg_expression(cg, obj_node);
      LLVMValueRef temp_in =
          LLVMBuildAlloca(cg->builder, LLVMTypeOf(fat_val), "to_array_in");
      LLVMBuildStore(cg->builder, fat_val, temp_in);

      LLVMValueRef temp_out =
          LLVMBuildAlloca(cg->builder, LLVMTypeOf(fat_val), "to_array_out");

      CGFunction *fn =
          cg_find_function(cg, sv_from_parts("__tyl_str_to_array", 18));
      LLVMValueRef args[] = {temp_out, temp_in};
      LLVMBuildCall2(cg->builder, fn->type, fn->value, args, 2, "");
      return LLVMBuildLoad2(cg->builder, LLVMTypeOf(fat_val), temp_out,
                            "to_array_res");
    }
  }

  if (node->call.func->tag == NODE_STATIC_MEMBER) {
    StringView parent = node->call.func->static_member.parent;
    StringView member = node->call.func->static_member.member;

    if (sv_eq(parent, sv_from_parts("String", 6)) &&
        sv_eq(member, sv_from_parts("from_array", 10))) {
      AstNode *arg_node = node->call.args.items[0];
      LLVMValueRef fat_val = cg_expression(cg, arg_node);
      LLVMValueRef temp_in =
          LLVMBuildAlloca(cg->builder, LLVMTypeOf(fat_val), "from_array_in");
      LLVMBuildStore(cg->builder, fat_val, temp_in);

      LLVMValueRef temp_out =
          LLVMBuildAlloca(cg->builder, LLVMTypeOf(fat_val), "from_array_out");

      CGFunction *fn =
          cg_find_function(cg, sv_from_parts("__tyl_array_to_str", 18));
      LLVMValueRef args[] = {temp_out, temp_in};
      LLVMBuildCall2(cg->builder, fn->type, fn->value, args, 2, "");
      return LLVMBuildLoad2(cg->builder, LLVMTypeOf(fat_val), temp_out,
                            "from_array_res");
    }
  }

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

    unsigned param_count = LLVMCountParams(fn->value);
    LLVMTypeRef *param_types =
        malloc(sizeof(LLVMTypeRef) * (param_count > 0 ? param_count : 1));
    if (param_count > 0) {
      LLVMGetParamTypes(fn->type, param_types);
    }

    for (unsigned i = 0; i < arg_count; i++) {
      AstNode *arg_node = node->call.args.items[i];
      LLVMValueRef arg_val = cg_expression(cg, arg_node);

      if (i < param_count) {
        args[i] =
            cg_cast_value(cg, arg_val, arg_node->resolved_type, param_types[i]);
      } else {
        args[i] = arg_val;
      }
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
    item_val = cg_cast_value(cg, item_val, item->resolved_type, llvm_elem_ty);

    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), i, false)};

    LLVMValueRef slot_ptr = LLVMBuildGEP2(cg->builder, arr_type, backing_ptr,
                                          indices, 2, "arr_init_gep");
    LLVMBuildStore(cg->builder, item_val, slot_ptr);
  }

  LLVMTypeRef fat_ptr_type = cg_get_llvm_type(cg, array_ty);
  LLVMValueRef fat_ptr = LLVMGetUndef(fat_ptr_type);

  LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);

  // 1. Rank = 1 for simple array literals
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr,
                                 LLVMConstInt(i64_ty, 1, false), 0, "fat_rank");

  // 2. Data Pointer
  LLVMValueRef first_elem_indices[] = {
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false)};
  LLVMValueRef data_ptr = LLVMBuildGEP2(cg->builder, arr_type, backing_ptr,
                                        first_elem_indices, 2, "arr_decay_gep");
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, data_ptr, 1, "fat_data");

  // 3. Dimensions & Strides Pointer
  // Rank 1 literal: [len, 1]
  LLVMValueRef dims_alloca = LLVMBuildAlloca(
      cg->builder, LLVMArrayType(i64_ty, 2), "dims_stride_alloca");

  LLVMValueRef len_idx[] = {LLVMConstInt(i64_ty, 0, false),
                            LLVMConstInt(i64_ty, 0, false)};
  LLVMValueRef len_gep = LLVMBuildGEP2(cg->builder, LLVMArrayType(i64_ty, 2),
                                       dims_alloca, len_idx, 2, "len_gep");
  LLVMValueRef len_store =
      LLVMBuildStore(cg->builder, LLVMConstInt(i64_ty, count, false), len_gep);
  LLVMSetAlignment(len_store, 8);

  LLVMValueRef stride_idx[] = {LLVMConstInt(i64_ty, 0, false),
                               LLVMConstInt(i64_ty, 1, false)};
  LLVMValueRef stride_gep =
      LLVMBuildGEP2(cg->builder, LLVMArrayType(i64_ty, 2), dims_alloca,
                    stride_idx, 2, "stride_gep");
  LLVMValueRef stride_store =
      LLVMBuildStore(cg->builder, LLVMConstInt(i64_ty, 1, false), stride_gep);
  LLVMSetAlignment(stride_store, 8);

  LLVMValueRef dims_ptr = LLVMBuildBitCast(
      cg->builder, dims_alloca, LLVMPointerType(i64_ty, 0), "dims_ptr");
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, dims_ptr, 2, "fat_dims");

  return fat_ptr;
}

static LLVMValueRef cg_array_repeat(Codegen *cg, AstNode *node) {
  Type *array_ty = node->resolved_type;
  LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);

  // 1. Collect dimensions from outermost to innermost
  List dims;
  List_init(&dims);
  AstNode *current = node;
  while (current->tag == NODE_ARRAY_REPEAT) {
    LLVMValueRef count = cg_expression(cg, current->array_repeat.count);
    // Push at index 0 to ensure outermost (first) dimensions are at the start
    // of the list
    List_insert(
        &dims, 0,
        (void *)cg_cast_value(
            cg, count, current->array_repeat.count->resolved_type, i64_ty));
    if (current->array_repeat.value->tag == NODE_ARRAY_REPEAT) {
      current = current->array_repeat.value;
    } else {
      break;
    }
  }

  // 2. Calculate total elements for malloc
  LLVMValueRef total_elements = LLVMConstInt(i64_ty, 1, false);
  for (size_t i = 0; i < dims.len; i++) {
    total_elements = LLVMBuildMul(cg->builder, total_elements,
                                  (LLVMValueRef)dims.items[i], "tmp_total");
  }

  // Find the true base type (scalar)
  Type *base_ty = current->array_repeat.value->resolved_type;
  LLVMTypeRef llvm_base_ty = cg_get_llvm_type(cg, base_ty);

  // 3. Allocation
  LLVMValueRef elem_size = LLVMSizeOf(llvm_base_ty);
  LLVMValueRef total_bytes =
      LLVMBuildMul(cg->builder, total_elements, elem_size, "total_bytes");

  CGFunction *malloc_fn = cg_find_function(cg, sv_from_parts("malloc", 6));
  if (!malloc_fn) {
    LLVMTypeRef malloc_args[] = {i64_ty};
    LLVMTypeRef malloc_type =
        LLVMFunctionType(LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
                         malloc_args, 1, false);
    LLVMValueRef malloc_val =
        LLVMAddFunction(cg->module, "malloc", malloc_type);
    CGFunction *f_malloc = xmalloc(sizeof(CGFunction));
    cg_init_CGFunction(f_malloc, sv_from_parts("malloc", 6), malloc_val,
                       malloc_type, true);
    List_push(&cg->system_functions, f_malloc);
    malloc_fn = f_malloc;
  }

  LLVMValueRef backing_ptr =
      LLVMBuildCall2(cg->builder, malloc_fn->type, malloc_fn->value,
                     &total_bytes, 1, "malloc_call");

  // --- Allocate and Store Dimensions & Strides Array ---
  // Store layout: [dim0, dim1, ..., dimN-1, stride0, stride1, ..., strideN-1]
  LLVMValueRef dims_count = LLVMConstInt(i64_ty, dims.len, false);
  LLVMValueRef total_metadata_count = LLVMConstInt(i64_ty, dims.len * 2, false);
  LLVMValueRef metadata_bytes = LLVMBuildMul(
      cg->builder, total_metadata_count, LLVMSizeOf(i64_ty), "metadata_bytes");
  LLVMValueRef dims_malloc =
      LLVMBuildCall2(cg->builder, malloc_fn->type, malloc_fn->value,
                     &metadata_bytes, 1, "dims_malloc");
  LLVMValueRef dims_ptr = LLVMBuildBitCast(
      cg->builder, dims_malloc, LLVMPointerType(i64_ty, 0), "dims_ptr");

  for (size_t i = 0; i < dims.len; i++) {
    LLVMValueRef idx = LLVMConstInt(i64_ty, i, false);
    LLVMValueRef gep =
        LLVMBuildGEP2(cg->builder, i64_ty, dims_ptr, &idx, 1, "dim_gep");
    LLVMValueRef store =
        LLVMBuildStore(cg->builder, (LLVMValueRef)dims.items[i], gep);
    LLVMSetAlignment(store, 8);
  }

  // Precompute strides [stride0, ..., strideN-1]
  // stride[i] = product of dimensions [i+1, ..., N-1]
  // stride[N-1] = 1
  LLVMValueRef current_stride = LLVMConstInt(i64_ty, 1, false);
  for (int i = (int)dims.len - 1; i >= 0; i--) {
    LLVMValueRef stride_idx = LLVMConstInt(i64_ty, dims.len + i, false);
    LLVMValueRef stride_gep = LLVMBuildGEP2(cg->builder, i64_ty, dims_ptr,
                                            &stride_idx, 1, "stride_gep");
    LLVMValueRef store =
        LLVMBuildStore(cg->builder, current_stride, stride_gep);
    LLVMSetAlignment(store, 8);

    if (i > 0) {
      // next stride[i-1] = stride[i] * dim[i]
      current_stride =
          LLVMBuildMul(cg->builder, current_stride, (LLVMValueRef)dims.items[i],
                       "next_stride_pre");
    }
  }

  // Cast backing_ptr to a pointer to the base element type
  LLVMValueRef cast_backing =
      LLVMBuildBitCast(cg->builder, backing_ptr,
                       LLVMPointerType(llvm_base_ty, 0), "cast_backing");

  AstNode *base_expr = current->array_repeat.value;
  LLVMValueRef item_val = cg_expression(cg, base_expr);
  item_val =
      cg_cast_value(cg, item_val, base_expr->resolved_type, llvm_base_ty);

  // Initialization (Memset 0 or loop)
  if (LLVMIsAConstantInt(item_val) && LLVMConstIntGetZExtValue(item_val) == 0) {
    CGFunction *memset_fn = cg_find_function(cg, sv_from_parts("memset", 6));
    if (!memset_fn) {
      LLVMTypeRef memset_args[] = {
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
          LLVMInt32TypeInContext(cg->context),
          LLVMInt64TypeInContext(cg->context)};
      LLVMTypeRef memset_type = LLVMFunctionType(
          LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0), memset_args,
          3, false);
      LLVMValueRef memset_val =
          LLVMAddFunction(cg->module, "memset", memset_type);
      CGFunction *f_memset = xmalloc(sizeof(CGFunction));
      cg_init_CGFunction(f_memset, sv_from_parts("memset", 6), memset_val,
                         memset_type, true);
      List_push(&cg->system_functions, f_memset);
      memset_fn = f_memset;
    }

    LLVMValueRef m_args[] = {
        backing_ptr,
        LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false),
        total_bytes};
    LLVMBuildCall2(cg->builder, memset_fn->type, memset_fn->value, m_args, 3,
                   "");
  } else {
    // Generic loop for non-zero scalars
    LLVMBasicBlockRef current_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef func = LLVMGetBasicBlockParent(current_bb);
    LLVMBasicBlockRef loop_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "init_loop");
    LLVMBasicBlockRef after_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "init_done");

    LLVMBuildBr(cg->builder, loop_bb);
    LLVMPositionBuilderAtEnd(cg->builder, loop_bb);

    LLVMValueRef phi_idx = LLVMBuildPhi(cg->builder, i64_ty, "init_loop_idx");
    LLVMValueRef next_idx = LLVMBuildAdd(
        cg->builder, phi_idx, LLVMConstInt(i64_ty, 1, false), "next_idx");

    LLVMValueRef slot_ptr = LLVMBuildGEP2(
        cg->builder, llvm_base_ty, cast_backing, &phi_idx, 1, "arr_init_gep");
    LLVMBuildStore(cg->builder, item_val, slot_ptr);

    LLVMValueRef cond = LLVMBuildICmp(cg->builder, LLVMIntULT, next_idx,
                                      total_elements, "init_loop_cond");
    LLVMBuildCondBr(cg->builder, cond, loop_bb, after_bb);

    LLVMValueRef incoming_vals[] = {LLVMConstInt(i64_ty, 0, false), next_idx};
    LLVMBasicBlockRef incoming_blocks[] = {current_bb, loop_bb};
    LLVMAddIncoming(phi_idx, incoming_vals, incoming_blocks, 2);

    LLVMPositionBuilderAtEnd(cg->builder, after_bb);
  }

  // 4. Create the Fat Pointer
  LLVMTypeRef fat_ptr_type = cg_get_llvm_type(cg, array_ty);
  LLVMValueRef fat_ptr = LLVMGetUndef(fat_ptr_type);

  // Rank = overall number of dimensions
  LLVMValueRef rank = LLVMConstInt(i64_ty, dims.len, false);
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, rank, 0, "fat_rank");
  fat_ptr =
      LLVMBuildInsertValue(cg->builder, fat_ptr, cast_backing, 1, "fat_data");
  fat_ptr = LLVMBuildInsertValue(cg->builder, fat_ptr, dims_ptr, 2, "fat_dims");

  List_free(&dims, false);
  return fat_ptr;
}

static LLVMValueRef cg_index_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef fat_ptr = cg_expression(cg, node->index.array);
  LLVMValueRef index = cg_expression(cg, node->index.index);

  Type *arr_ty = node->index.array->resolved_type;
  LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
  index = cg_cast_value(cg, index, node->index.index->resolved_type, i64_ty);

  // 1. Extract Metadata
  LLVMValueRef rank =
      LLVMBuildExtractValue(cg->builder, fat_ptr, 0, "arr_rank");
  LLVMValueRef data_ptr =
      LLVMBuildExtractValue(cg->builder, fat_ptr, 1, "arr_ptr");
  LLVMValueRef dims_ptr =
      LLVMBuildExtractValue(cg->builder, fat_ptr, 2, "arr_dims");

  // Current dimension length is at dims_ptr[0]
  LLVMValueRef len_load =
      LLVMBuildLoad2(cg->builder, i64_ty, dims_ptr, "arr_len");
  LLVMSetAlignment(len_load, 8);
  LLVMValueRef len = len_load;

  // 2. Bounds Check
  LLVMValueRef is_lt =
      LLVMBuildICmp(cg->builder, LLVMIntULT, index, len, "bounds_check");
  LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  LLVMBasicBlockRef ok_bb =
      LLVMAppendBasicBlockInContext(cg->context, func, "index_ok");
  LLVMBasicBlockRef fail_bb =
      LLVMAppendBasicBlockInContext(cg->context, func, "index_fail");
  LLVMBuildCondBr(cg->builder, is_lt, ok_bb, fail_bb);

  // Fail Block
  LLVMPositionBuilderAtEnd(cg->builder, fail_bb);
  CGFunction *trap = cg_find_function(cg, sv_from_parts("llvm.trap", 9));
  if (!trap) {
    LLVMTypeRef trap_ty =
        LLVMFunctionType(LLVMVoidTypeInContext(cg->context), NULL, 0, false);
    LLVMValueRef trap_val = LLVMAddFunction(cg->module, "llvm.trap", trap_ty);
    CGFunction *f_trap = xmalloc(sizeof(CGFunction));
    cg_init_CGFunction(f_trap, sv_from_parts("llvm.trap", 9), trap_val, trap_ty,
                       true);
    List_push(&cg->system_functions, f_trap);
    trap = f_trap;
  }
  LLVMBuildCall2(cg->builder, trap->type, trap->value, NULL, 0, "");
  LLVMBuildUnreachable(cg->builder);

  // OK Block
  LLVMPositionBuilderAtEnd(cg->builder, ok_bb);

  // 3. Calculate Pointer to Element
  Type *elem_ty = node->resolved_type;
  Type *base_scalar = elem_ty;
  while (base_scalar->kind == KIND_ARRAY) {
    base_scalar = base_scalar->data.array.element;
  }
  LLVMTypeRef llvm_base_ty = cg_get_llvm_type(cg, base_scalar);

  if (elem_ty->kind != KIND_ARRAY) {
    // LEAF: No stride calculation needed, stride = 1
    LLVMValueRef element_ptr = LLVMBuildGEP2(
        cg->builder, llvm_base_ty, data_ptr, &index, 1, "element_ptr");
    return LLVMBuildLoad2(cg->builder, llvm_base_ty, element_ptr, "elem_load");
  }

  // SUB-ARRAY: Load precomputed stride
  // stride[0] for this dimension is at dims_ptr[rank]
  LLVMValueRef stride_idx = rank;
  LLVMValueRef stride_gep = LLVMBuildGEP2(cg->builder, i64_ty, dims_ptr,
                                          &stride_idx, 1, "stride_gep");
  LLVMValueRef stride =
      LLVMBuildLoad2(cg->builder, i64_ty, stride_gep, "stride");
  LLVMSetAlignment(stride, 8);

  LLVMValueRef flat_offset =
      LLVMBuildMul(cg->builder, index, stride, "flat_offset");

  LLVMValueRef element_ptr = LLVMBuildGEP2(cg->builder, llvm_base_ty, data_ptr,
                                           &flat_offset, 1, "element_ptr");

  // Sub-slice: Create a new fat pointer for the next dimension
  LLVMTypeRef fat_ptr_type = cg_get_llvm_type(cg, elem_ty);
  LLVMValueRef sub_fat = LLVMGetUndef(fat_ptr_type);

  // sub_rank = rank - 1
  LLVMValueRef sub_rank = LLVMBuildSub(
      cg->builder, rank, LLVMConstInt(i64_ty, 1, false), "sub_rank");
  sub_fat = LLVMBuildInsertValue(cg->builder, sub_fat, sub_rank, 0, "sub_rank");

  // sub_data = element_ptr
  sub_fat =
      LLVMBuildInsertValue(cg->builder, sub_fat, element_ptr, 1, "sub_data");

  // sub_dims = dims_ptr + 1
  LLVMValueRef one = LLVMConstInt(i64_ty, 1, false);
  LLVMValueRef sub_dims =
      LLVMBuildGEP2(cg->builder, i64_ty, dims_ptr, &one, 1, "sub_dims");
  sub_fat = LLVMBuildInsertValue(cg->builder, sub_fat, sub_dims, 2, "sub_dims");

  return sub_fat;
}

static LLVMValueRef cg_field_expr(Codegen *cg, AstNode *node) {
  AstNode *obj_node = node->field.object;
  Type *obj_type = obj_node->resolved_type;
  Member *m = type_get_member(obj_type, node->field.field);

  if (m && m->builtin_id == BUILTIN_ARRAY_LEN) {
    LLVMValueRef fat_ptr = cg_expression(cg, obj_node);
    LLVMValueRef dims_ptr =
        LLVMBuildExtractValue(cg->builder, fat_ptr, 2, "arr_dims_ptr");
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->context);
    LLVMValueRef len_load =
        LLVMBuildLoad2(cg->builder, i64_ty, dims_ptr, "arr_len_extract");
    LLVMSetAlignment(len_load, 8);
    return len_load;
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

  case NODE_FIELD:
    return cg_field_expr(cg, node);

  case NODE_INTRINSIC_COMPARE: {
    LLVMValueRef lhs_ptr = cg_get_address(cg, node->intrinsic_compare.left);
    LLVMValueRef rhs_ptr = cg_get_address(cg, node->intrinsic_compare.right);

    // If they aren't lvalues, we need to store them in a temporary
    if (!lhs_ptr) {
      LLVMValueRef lhs_val = cg_expression(cg, node->intrinsic_compare.left);
      lhs_ptr = LLVMBuildAlloca(cg->builder, LLVMTypeOf(lhs_val), "tmp_lhs");
      LLVMBuildStore(cg->builder, lhs_val, lhs_ptr);
    }
    if (!rhs_ptr) {
      LLVMValueRef rhs_val = cg_expression(cg, node->intrinsic_compare.right);
      rhs_ptr = LLVMBuildAlloca(cg->builder, LLVMTypeOf(rhs_val), "tmp_rhs");
      LLVMBuildStore(cg->builder, rhs_val, rhs_ptr);
    }

    Type *arr_type = node->intrinsic_compare.left->resolved_type;
    Type *base_scalar = (arr_type->kind == KIND_PRIMITIVE &&
                         arr_type->data.primitive == PRIM_STRING)
                            ? type_get_primitive(cg->type_ctx, PRIM_CHAR)
                            : arr_type->data.array.element;
    while (base_scalar->kind == KIND_ARRAY) {
      base_scalar = base_scalar->data.array.element;
    }

    LLVMTypeRef llvm_scalar_ty = cg_get_llvm_type(cg, base_scalar);
    LLVMValueRef elem_size =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, false);

    CGFunction *cmp_fn =
        cg_find_function(cg, sv_from_parts("__tyl_compare_arrays", 20));
    LLVMValueRef args[] = {lhs_ptr, rhs_ptr, elem_size};
    LLVMValueRef res = LLVMBuildCall2(cg->builder, cmp_fn->type, cmp_fn->value,
                                      args, 3, "cmptmp");

    if (node->intrinsic_compare.op == OP_EQ) {
      return LLVMBuildICmp(
          cg->builder, LLVMIntEQ, res,
          LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, true), "eqtmp");
    } else {
      return LLVMBuildICmp(
          cg->builder, LLVMIntNE, res,
          LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, true), "netmp");
    }
  }

  case NODE_INDEX:
    return cg_index_expr(cg, node);

  default:
    fprintf(stderr, "Codegen: Unhandled expression tag %d\n", node->tag);
    return NULL;
  }
}
