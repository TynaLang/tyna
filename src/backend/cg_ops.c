#include <llvm-c/Core.h>
#include <stdio.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/utils.h"
#include "llvm-c/Types.h"
#include <llvm-c/Target.h>

LLVMValueRef cg_const_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_var_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_field_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_binary_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_binary_is_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_binary_else_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_unary_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_ternary_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_assign_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_cast_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_call_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_new_expr(Codegen *cg, AstNode *node);
LLVMValueRef cg_array_literal(Codegen *cg, AstNode *node);
LLVMValueRef cg_array_repeat(Codegen *cg, AstNode *node);

static bool cg_is_list_struct(Type *t) {
  if (!t || t->kind != KIND_STRUCT)
    return false;
  if (t->data.instance.from_template &&
      sv_eq(t->data.instance.from_template->name, sv_from_parts("List", 4)))
    return true;
  return false;
}

static bool cg_is_fixed_to_dynamic_array_conversion(Type *to, Type *from) {
  if (!to || !from)
    return false;
  if ((!type_is_array_struct(to) && !cg_is_list_struct(to)) ||
      (!type_is_array_struct(from) && !cg_is_list_struct(from)))
    return false;

  if (to->fixed_array_len != 0 || from->fixed_array_len == 0)
    return false;

  if (!to->data.instance.from_template || !from->data.instance.from_template)
    return false;
  if (to->data.instance.from_template != from->data.instance.from_template)
    return false;

  if (to->data.instance.generic_args.len == 0 ||
      from->data.instance.generic_args.len == 0)
    return false;

  return type_equals(to->data.instance.generic_args.items[0],
                     from->data.instance.generic_args.items[0]);
}

static bool cg_is_dynamic_array_target(Type *t) {
  if (!t || t->fixed_array_len != 0 || t->data.instance.generic_args.len == 0)
    return false;
  return type_is_array_struct(t) || cg_is_list_struct(t);
}

static LLVMValueRef cg_block_expr(Codegen *cg, AstNode *node) {
  if (!node || node->tag != NODE_BLOCK)
    return NULL;

  cg_push_scope(cg);
  LLVMValueRef last_val = NULL;

  for (size_t i = 0; i < node->block.statements.len; i++) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      break;

    AstNode *stmt = node->block.statements.items[i];
    if (!stmt)
      continue;

    if (stmt->tag == NODE_EXPR_STMT) {
      last_val = cg_expression(cg, stmt->expr_stmt.expr);
    } else {
      cg_statement(cg, stmt);
    }
  }

  cg_pop_scope(cg);

  if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    return NULL;

  if (last_val)
    return last_val;

  if (node->resolved_type)
    return LLVMConstNull(cg_type_get_llvm(cg, node->resolved_type));

  return NULL;
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
    return is_float ? LLVMBuildFDiv(cg->builder, lhs, rhs, "fdivtmp")
                    : LLVMBuildSDiv(cg->builder, lhs, rhs, "sdivtmp");
  case OP_MOD:
    return is_float ? LLVMBuildFRem(cg->builder, lhs, rhs, "fremtmp")
                    : LLVMBuildSRem(cg->builder, lhs, rhs, "sremtmp");
  case OP_POW: {
    CgFunc *pow_fn = cg_find_function(cg, sv_from_parts("pow", 3));
    LLVMTypeRef double_ty = LLVMDoubleTypeInContext(cg->context);
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

LLVMValueRef cg_binary_expr(Codegen *cg, AstNode *node) {
  AstNode *left_node, *right_node;
  switch (node->tag) {
  case NODE_BINARY_ARITH:
    left_node = node->binary_arith.left;
    right_node = node->binary_arith.right;
    break;
  case NODE_BINARY_COMPARE:
    left_node = node->binary_compare.left;
    right_node = node->binary_compare.right;
    break;
  case NODE_BINARY_EQUALITY:
    left_node = node->binary_equality.left;
    right_node = node->binary_equality.right;
    break;
  case NODE_BINARY_LOGICAL:
    left_node = node->binary_logical.left;
    right_node = node->binary_logical.right;
    break;
  default:
    return NULL;
  }

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
    LLVMTypeRef target_ty = cg_type_get_llvm(cg, node->resolved_type);

    LLVMTypeKind lhs_kind = LLVMGetTypeKind(LLVMTypeOf(lhs));
    LLVMTypeKind rhs_kind = LLVMGetTypeKind(LLVMTypeOf(rhs));

    if ((lhs_kind == LLVMPointerTypeKind && rhs_kind == LLVMIntegerTypeKind) ||
        (rhs_kind == LLVMPointerTypeKind && lhs_kind == LLVMIntegerTypeKind)) {
      LLVMValueRef ptr_val = lhs_kind == LLVMPointerTypeKind ? lhs : rhs;
      LLVMValueRef int_val = lhs_kind == LLVMIntegerTypeKind ? lhs : rhs;

      Type *ptr_sem_type = lhs_kind == LLVMPointerTypeKind
                               ? left_node->resolved_type
                               : right_node->resolved_type;
      Type *elem_sem_type = NULL;
      if (ptr_sem_type && ptr_sem_type->kind == KIND_POINTER) {
        elem_sem_type = ptr_sem_type->data.pointer_to;
      }
      LLVMTypeRef elem_ty = elem_sem_type
                                ? cg_type_get_llvm(cg, elem_sem_type)
                                : LLVMGetElementType(LLVMTypeOf(ptr_val));

      LLVMValueRef index = cg_cast_value(cg, int_val,
                                         lhs_kind == LLVMIntegerTypeKind
                                             ? left_node->resolved_type
                                             : right_node->resolved_type,
                                         LLVMInt64TypeInContext(cg->context));
      if (node->binary_arith.op == OP_ADD) {
        return LLVMBuildGEP2(cg->builder, elem_ty, ptr_val,
                             (LLVMValueRef[]){index}, 1, "ptraddtmp");
      }
      if (node->binary_arith.op == OP_SUB && lhs_kind == LLVMPointerTypeKind) {
        LLVMValueRef neg_index = LLVMBuildNeg(cg->builder, index, "negidx");
        return LLVMBuildGEP2(cg->builder, elem_ty, ptr_val,
                             (LLVMValueRef[]){neg_index}, 1, "ptrsubtmp");
      }
    }

    lhs = cg_cast_value(cg, lhs, left_node->resolved_type, target_ty);
    rhs = cg_cast_value(cg, rhs, right_node->resolved_type, target_ty);
    return cg_arith_expr(cg, lhs, rhs, node->binary_arith.op);
  }

  cg_binary_sync_types(cg, &lhs, left_node->resolved_type, &rhs,
                       right_node->resolved_type);

  switch (node->tag) {
  case NODE_BINARY_COMPARE:
    return cg_compare_expr(cg, lhs, rhs, node->binary_compare.op);
  case NODE_BINARY_EQUALITY: {
    Type *lt = left_node->resolved_type;
    Type *rt = right_node->resolved_type;
    return cg_equality_expr(cg, lhs, rhs, node->binary_equality.op, lt, rt);
  }
  default:
    return NULL;
  }
}

LLVMValueRef cg_binary_is_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef left = cg_expression(cg, node->binary_is.left);
  if (!left)
    return NULL;

  Type *left_type = node->binary_is.left->resolved_type;
  Type *right_type = node->binary_is.right->resolved_type;
  LLVMTypeRef bool_ty = LLVMInt1TypeInContext(cg->context);

  if (left_type && left_type->kind == KIND_RESULT) {
    LLVMValueRef tag =
        LLVMBuildExtractValue(cg->builder, left, 1, "result_tag");
    LLVMTypeRef tag_ty = LLVMInt16TypeInContext(cg->context);
    if (node->binary_is.right->tag == NODE_VAR &&
        sv_eq_cstr(node->binary_is.right->var.value, "Error")) {
      return LLVMBuildICmp(cg->builder, LLVMIntNE, tag,
                           LLVMConstInt(tag_ty, 0, false), "is_error");
    }
    if (right_type && right_type->kind == KIND_ERROR) {
      int idx = cg_result_error_tag_index(left_type, right_type);
      return LLVMBuildICmp(cg->builder, LLVMIntEQ, tag,
                           LLVMConstInt(tag_ty, idx, false), "is_error_type");
    }
    if (right_type && right_type->kind == KIND_ERROR_SET) {
      return LLVMBuildICmp(cg->builder, LLVMIntNE, tag,
                           LLVMConstInt(tag_ty, 0, false), "is_error_set");
    }
  }

  if (left_type && left_type->kind == KIND_ERROR) {
    if (node->binary_is.right->tag == NODE_VAR &&
        sv_eq_cstr(node->binary_is.right->var.value, "Error")) {
      return LLVMConstInt(bool_ty, 1, false);
    }
    if (right_type && right_type->kind == KIND_ERROR_SET) {
      return LLVMConstInt(bool_ty, 1, false);
    }
    if (right_type && right_type->kind == KIND_ERROR) {
      LLVMValueRef right = cg_expression(cg, node->binary_is.right);
      return cg_equality_expr(cg, left, right, OP_EQ, left_type, right_type);
    }
  }

  if (left_type && left_type->kind == KIND_POINTER &&
      left_type->data.pointer_to &&
      left_type->data.pointer_to->kind == KIND_UNION &&
      left_type->data.pointer_to->is_tagged_union) {
    LLVMTypeRef ptr_ty = LLVMTypeOf(left);
    LLVMTypeRef union_ty = cg_type_get_llvm(cg, left_type->data.pointer_to);
    left = LLVMBuildLoad2(cg->builder, union_ty, left, "deref_tagged_union");
    left_type = left_type->data.pointer_to;
  }

  if (left_type && left_type->kind == KIND_UNION &&
      left_type->is_tagged_union) {
    AstNode *right_node = node->binary_is.right;
    AstNode *static_member = NULL;
    if (right_node->tag == NODE_STATIC_MEMBER) {
      static_member = right_node;
    } else if (right_node->tag == NODE_CALL && right_node->call.func &&
               right_node->call.func->tag == NODE_STATIC_MEMBER) {
      static_member = right_node->call.func;
    }

    if (static_member) {
      Type *parent_type =
          type_get_named(cg->type_ctx, static_member->static_member.parent);
      if (parent_type && parent_type->kind == KIND_UNION &&
          parent_type->is_tagged_union && type_equals(parent_type, left_type)) {
        Member *variant =
            type_get_member(parent_type, static_member->static_member.member);
        if (variant) {
          LLVMValueRef tag =
              LLVMBuildExtractValue(cg->builder, left, 0, "tagged_union_tag");
          int variant_index =
              cg_tagged_union_variant_index_by_member(parent_type, variant);
          if (variant_index >= 0) {
            LLVMTypeRef tag_ty = LLVMTypeOf(tag);
            return LLVMBuildICmp(cg->builder, LLVMIntEQ, tag,
                                 LLVMConstInt(tag_ty, variant_index, 0),
                                 "tagged_union_is");
          }
        }
      }
    } else if (right_type) {
      int variant_index = cg_tagged_union_variant_index(left_type, right_type);
      if (variant_index >= 0) {
        LLVMValueRef tag =
            LLVMBuildExtractValue(cg->builder, left, 0, "tagged_union_tag");
        LLVMTypeRef tag_ty = LLVMTypeOf(tag);
        return LLVMBuildICmp(cg->builder, LLVMIntEQ, tag,
                             LLVMConstInt(tag_ty, variant_index, 0),
                             "tagged_union_is");
      }
    }
  }

  return LLVMConstInt(bool_ty, 0, false);
}

LLVMValueRef cg_binary_else_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef left = cg_expression(cg, node->binary_else.left);
  if (!left)
    return NULL;
  Type *left_type = node->binary_else.left->resolved_type;
  if (!left_type || left_type->kind != KIND_RESULT)
    return NULL;

  bool out_is_void = node->resolved_type &&
                     node->resolved_type->kind == KIND_PRIMITIVE &&
                     node->resolved_type->data.primitive == PRIM_VOID;

  LLVMValueRef tag = LLVMBuildExtractValue(cg->builder, left, 1, "result_tag");
  LLVMTypeRef tag_ty = LLVMInt16TypeInContext(cg->context);
  LLVMValueRef is_error =
      LLVMBuildICmp(cg->builder, LLVMIntNE, tag, LLVMConstInt(tag_ty, 0, false),
                    "result_is_error");

  if (!node->binary_else.right) {
    LLVMBasicBlockRef error_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "question_error");
    LLVMBasicBlockRef success_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "question_success");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "question_merge");

    LLVMBuildCondBr(cg->builder, is_error, error_bb, success_bb);

    LLVMPositionBuilderAtEnd(cg->builder, error_bb);
    LLVMBuildRet(cg->builder, left);

    LLVMPositionBuilderAtEnd(cg->builder, success_bb);
    if (!out_is_void) {
      LLVMValueRef success_raw =
          LLVMBuildExtractValue(cg->builder, left, 0, "result_success");
      LLVMValueRef success_val =
          cg_cast_value(cg, success_raw, left_type->data.result.success,
                        cg_type_get_llvm(cg, node->resolved_type));
      LLVMBuildBr(cg->builder, merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
      LLVMValueRef phi =
          LLVMBuildPhi(cg->builder, cg_type_get_llvm(cg, node->resolved_type),
                       "question_val");
      LLVMAddIncoming(phi, &success_val, &success_bb, 1);
      return phi;
    }

    LLVMBuildBr(cg->builder, merge_bb);
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    return NULL;
  }

  if (node->binary_else.right &&
      node->binary_else.right->tag == NODE_RETURN_STMT) {
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef error_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "else_error");
    LLVMBasicBlockRef success_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "else_success");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "else_merge");

    (void)current_block;
    LLVMBuildCondBr(cg->builder, is_error, error_bb, success_bb);

    LLVMPositionBuilderAtEnd(cg->builder, error_bb);
    cg_statement(cg, node->binary_else.right);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, merge_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, success_bb);
    if (!out_is_void) {
      LLVMValueRef success_raw =
          LLVMBuildExtractValue(cg->builder, left, 0, "result_success");
      LLVMValueRef success_val =
          cg_cast_value(cg, success_raw, left_type->data.result.success,
                        cg_type_get_llvm(cg, node->resolved_type));
      LLVMBuildBr(cg->builder, merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
      LLVMValueRef phi = LLVMBuildPhi(
          cg->builder, cg_type_get_llvm(cg, node->resolved_type), "else_val");
      LLVMAddIncoming(phi, &success_val, &success_bb, 1);
      return phi;
    }

    LLVMBuildBr(cg->builder, merge_bb);
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    return NULL;
  }

  if (node->binary_else.right && node->binary_else.right->tag == NODE_BLOCK) {
    LLVMBasicBlockRef error_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "else_error");
    LLVMBasicBlockRef success_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "else_success");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
        cg->context, cg->current_function, "else_merge");

    LLVMBuildCondBr(cg->builder, is_error, error_bb, success_bb);

    LLVMPositionBuilderAtEnd(cg->builder, error_bb);
    LLVMValueRef block_val = cg_expression(cg, node->binary_else.right);
    bool error_terminated =
        LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)) != NULL;
    if (!error_terminated) {
      if (block_val && node->resolved_type) {
        block_val =
            cg_cast_value(cg, block_val, node->binary_else.right->resolved_type,
                          cg_type_get_llvm(cg, node->resolved_type));
      }
      if (!block_val && node->resolved_type) {
        block_val = LLVMConstNull(cg_type_get_llvm(cg, node->resolved_type));
      }
      LLVMBuildBr(cg->builder, merge_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, success_bb);
    if (!out_is_void) {
      LLVMValueRef success_raw =
          LLVMBuildExtractValue(cg->builder, left, 0, "result_success");
      LLVMValueRef success_val =
          cg_cast_value(cg, success_raw, left_type->data.result.success,
                        cg_type_get_llvm(cg, node->resolved_type));
      LLVMBuildBr(cg->builder, merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
      LLVMTypeRef out_ty = cg_type_get_llvm(cg, node->resolved_type);
      LLVMValueRef phi = LLVMBuildPhi(cg->builder, out_ty, "else_val_block");
      if (!error_terminated) {
        LLVMAddIncoming(phi, &block_val, &error_bb, 1);
      }
      LLVMAddIncoming(phi, &success_val, &success_bb, 1);
      return phi;
    }

    LLVMBuildBr(cg->builder, merge_bb);
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    return NULL;
  }

  LLVMValueRef right = cg_expression(cg, node->binary_else.right);
  LLVMValueRef success_raw =
      LLVMBuildExtractValue(cg->builder, left, 0, "result_success");
  LLVMValueRef success_val =
      cg_cast_value(cg, success_raw, left_type->data.result.success,
                    cg_type_get_llvm(cg, node->resolved_type));
  if (right && node->resolved_type) {
    right = cg_cast_value(cg, right, node->binary_else.right->resolved_type,
                          cg_type_get_llvm(cg, node->resolved_type));
  }

  return LLVMBuildSelect(cg->builder, is_error, right, success_val, "else_val");
}

LLVMValueRef cg_unary_expr(Codegen *cg, AstNode *node) {
  UnaryOp op = node->unary.op;
  AstNode *expr = node->unary.expr;
  LLVMTypeRef res_ty = NULL;
  if (op == OP_NEG || op == OP_DEREF) {
    if (!node->resolved_type) {
      panic("Unary expression has no resolved type");
    }
    res_ty = cg_type_get_llvm(cg, node->resolved_type);
  }

  if (op == OP_NEG) {
    LLVMValueRef val = cg_expression(cg, expr);
    val = cg_cast_value(cg, val, expr->resolved_type, res_ty);
    if (LLVMGetTypeKind(res_ty) == LLVMDoubleTypeKind ||
        LLVMGetTypeKind(res_ty) == LLVMFloatTypeKind) {
      return LLVMBuildFNeg(cg->builder, val, "fnegtmp");
    }
    return LLVMBuildNeg(cg->builder, val, "negtmp");
  }

  if (op == OP_NOT) {
    LLVMValueRef val = cg_expression(cg, expr);
    LLVMValueRef bool_val = cg_cast_value(cg, val, expr->resolved_type,
                                          LLVMInt1TypeInContext(cg->context));
    return LLVMBuildNot(cg->builder, bool_val, "nottmp");
  }

  if (op == OP_DEREF) {
    LLVMValueRef ptr = cg_expression(cg, expr);
    return LLVMBuildLoad2(cg->builder, res_ty, ptr, "deref_load");
  }

  if (op == OP_ADDR_OF) {
    return cg_get_address(cg, expr);
  }

  LLVMValueRef ptr = cg_get_address(cg, expr);
  if (!ptr)
    panic("Invalid unary operand target");

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
  return (op == OP_PRE_INC || op == OP_PRE_DEC) ? new_val : old_val;
}

LLVMValueRef cg_ternary_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef cond = cg_expression(cg, node->ternary.condition);
  LLVMTypeRef i1_ty = LLVMInt1TypeInContext(cg->context);
  cond = cg_cast_value(cg, cond, node->ternary.condition->resolved_type, i1_ty);

  LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "ternary_then");
  LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "ternary_else");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(
      cg->context, cg->current_function, "ternary_cont");

  LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);
  LLVMTypeRef res_type = cg_type_get_llvm(cg, node->resolved_type);

  LLVMPositionBuilderAtEnd(cg->builder, then_bb);
  LLVMValueRef then_val = cg_expression(cg, node->ternary.true_expr);
  then_val = cg_cast_value(cg, then_val, node->ternary.true_expr->resolved_type,
                           res_type);
  LLVMBuildBr(cg->builder, merge_bb);
  LLVMBasicBlockRef then_bb_end = LLVMGetInsertBlock(cg->builder);

  LLVMPositionBuilderAtEnd(cg->builder, else_bb);
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

LLVMValueRef cg_assign_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef ptr = cg_get_address(cg, node->assign_expr.target);
  if (!ptr)
    panic("Invalid assignment target");
  LLVMValueRef val = cg_expression(cg, node->assign_expr.value);

  Type *target_type = node->assign_expr.target->resolved_type;
  if (!target_type) {
    AstNode *target = node->assign_expr.target;
    if (target->tag == NODE_VAR) {
      CgSym *sym = CGSymbolTable_find(cg->current_scope, target->var.value);
      if (sym)
        target_type = sym->type;
    } else if (target->tag == NODE_FIELD) {
      Type *obj_type = target->field.object->resolved_type;
      if (!obj_type && target->field.object->tag == NODE_VAR) {
        CgSym *sym = CGSymbolTable_find(cg->current_scope,
                                        target->field.object->var.value);
        if (sym)
          obj_type = sym->type;
      }
      if (obj_type && obj_type->kind == KIND_STRUCT) {
        Member *m = type_get_member(obj_type, target->field.field);
        if (m)
          target_type = m->type;
      } else if (obj_type && obj_type->kind == KIND_UNION) {
        Type *owner = NULL;
        Member *m =
            type_find_union_field(obj_type, target->field.field, &owner);
        if (m)
          target_type = m->type;
      }
    } else if (target->tag == NODE_INDEX) {
      Type *array_type = target->index.array->resolved_type;
      if (array_type) {
        if (array_type->kind == KIND_POINTER) {
          target_type = array_type->data.pointer_to;
        } else if (type_is_array_struct(array_type) &&
                   array_type->data.instance.generic_args.len > 0) {
          target_type = array_type->data.instance.generic_args.items[0];
        }
      }
    }
  }

  if (!target_type) {
    target_type = node->assign_expr.target->resolved_type;
  }
  if (!target_type) {
    panic("Assignment target has no resolved type");
  }
  if (!node->assign_expr.value->resolved_type) {
    panic("Assignment RHS has no resolved type");
  }
  Type *source_type = node->assign_expr.value->resolved_type;
  LLVMTypeRef target_ty = cg_type_get_llvm(cg, target_type);
  LLVMTypeRef source_llvm_ty = LLVMTypeOf(val);

  bool needs_array_from_stack =
      cg_is_fixed_to_dynamic_array_conversion(target_type, source_type) ||
      (cg_is_dynamic_array_target(target_type) &&
       LLVMGetTypeKind(source_llvm_ty) == LLVMArrayTypeKind);

  if (needs_array_from_stack) {
    CgFunc *from_stack_fn =
        cg_find_system_function(cg, sv_from_cstr("__tyna_array_from_stack"));
    if (!from_stack_fn) {
      panic("Internal error: __tyna_array_from_stack runtime function missing");
    }

    // Keep destination in a known state even if runtime conversion exits early.
    LLVMBuildStore(cg->builder, LLVMConstNull(target_ty), ptr);

    LLVMTypeRef source_ty = source_llvm_ty;
    LLVMValueRef stack_tmp =
        LLVMBuildAlloca(cg->builder, source_ty, "fixed_array_assign_stack_tmp");
    LLVMBuildStore(cg->builder, val, stack_tmp);

    LLVMTypeRef i8_ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
    LLVMValueRef out_ptr_i8 = LLVMBuildBitCast(cg->builder, ptr, i8_ptr,
                                               "array_assign_from_stack_out");
    LLVMValueRef stack_ptr_i8 = LLVMBuildBitCast(cg->builder, stack_tmp, i8_ptr,
                                                 "array_assign_from_stack_src");

    Type *elem_type = target_type->data.instance.generic_args.items[0];
    uint64_t fixed_count = 0;
    if (source_type && source_type->fixed_array_len > 0) {
      fixed_count = source_type->fixed_array_len;
    } else if (LLVMGetTypeKind(source_llvm_ty) == LLVMArrayTypeKind) {
      fixed_count = (uint64_t)LLVMGetArrayLength(source_llvm_ty);
    } else {
      panic("Internal error: array-from-stack assignment source is not "
            "fixed-size");
    }
    LLVMValueRef count =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), fixed_count, 0);
    LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
    const char *data_layout = LLVMGetDataLayout(cg->module);
    LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
    unsigned long long elem_size_bytes = LLVMABISizeOfType(td, llvm_elem_ty);
    LLVMDisposeTargetData(td);
    LLVMValueRef elem_size =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), elem_size_bytes, 0);

    LLVMBuildCall2(cg->builder, from_stack_fn->type, from_stack_fn->value,
                   (LLVMValueRef[]){out_ptr_i8, stack_ptr_i8, count, elem_size},
                   4, "");
    return LLVMBuildLoad2(cg->builder, target_ty, ptr, "array_assign_conv_val");
  }

  if (target_type->kind == KIND_UNION && target_type->is_tagged_union) {
    int variant_index = cg_tagged_union_variant_index(target_type, source_type);
    if (variant_index >= 0) {
      val = cg_make_tagged_union(cg, val, source_type, target_type);
    } else {
      val = cg_cast_value(cg, val, source_type, target_ty);
    }
  } else {
    val = cg_cast_value(cg, val, source_type, target_ty);
  }

  if (node->assign_expr.target->tag == NODE_INDEX) {
    Type *array_type = node->assign_expr.target->index.array->resolved_type;
    if (type_is_array_struct(array_type) && array_type->fixed_array_len == 0) {
      LLVMValueRef array_struct_ptr =
          cg_get_address(cg, node->assign_expr.target->index.array);
      if (!array_struct_ptr)
        panic("Invalid array assignment target");

      LLVMTypeRef array_ty = cg_type_get_llvm(cg, array_type);
      LLVMValueRef len_ptr = LLVMBuildStructGEP2(
          cg->builder, array_ty, array_struct_ptr, 1, "array_len_ptr");
      LLVMValueRef curr_len =
          LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                         len_ptr, "array_curr_len");
      LLVMValueRef idx_val =
          cg_expression(cg, node->assign_expr.target->index.index);
      LLVMValueRef idx_i64 = cg_cast_value(
          cg, idx_val, node->assign_expr.target->index.index->resolved_type,
          LLVMInt64TypeInContext(cg->context));
      LLVMValueRef is_append = LLVMBuildICmp(cg->builder, LLVMIntEQ, idx_i64,
                                             curr_len, "array_is_append");
      LLVMBasicBlockRef append_bb = LLVMAppendBasicBlockInContext(
          cg->context, cg->current_function, "array_append");
      LLVMBasicBlockRef skip_append_bb = LLVMAppendBasicBlockInContext(
          cg->context, cg->current_function, "array_skip_append");
      LLVMBasicBlockRef append_merge_bb = LLVMAppendBasicBlockInContext(
          cg->context, cg->current_function, "array_append_merge");
      LLVMBuildCondBr(cg->builder, is_append, append_bb, skip_append_bb);

      LLVMPositionBuilderAtEnd(cg->builder, append_bb);
      LLVMValueRef next_len =
          LLVMBuildAdd(cg->builder, curr_len,
                       LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0),
                       "array_next_len");
      LLVMBuildStore(cg->builder, next_len, len_ptr);
      LLVMBuildBr(cg->builder, append_merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, skip_append_bb);
      LLVMBuildBr(cg->builder, append_merge_bb);

      LLVMPositionBuilderAtEnd(cg->builder, append_merge_bb);
    }
  }

  LLVMBuildStore(cg->builder, val, ptr);
  return val;
}

LLVMValueRef cg_cast_expr(Codegen *cg, AstNode *node) {
  LLVMValueRef val = cg_expression(cg, node->cast_expr.expr);
  Type *expr_type = node->cast_expr.expr->resolved_type;
  Type *target_type = node->cast_expr.target_type;
  LLVMTypeRef target_ty = cg_type_get_llvm(cg, target_type);
  LLVMTypeRef source_llvm_ty = LLVMTypeOf(val);

  bool needs_array_from_stack =
      cg_is_fixed_to_dynamic_array_conversion(target_type, expr_type) ||
      (cg_is_dynamic_array_target(target_type) &&
       LLVMGetTypeKind(source_llvm_ty) == LLVMArrayTypeKind);

  if (needs_array_from_stack) {
    CgFunc *from_stack_fn =
        cg_find_system_function(cg, sv_from_cstr("__tyna_array_from_stack"));
    if (!from_stack_fn) {
      panic("Internal error: __tyna_array_from_stack runtime function missing");
    }

    LLVMValueRef out_tmp =
        LLVMBuildAlloca(cg->builder, target_ty, "fixed_to_dynamic_cast_out");
    LLVMBuildStore(cg->builder, LLVMConstNull(target_ty), out_tmp);

    LLVMTypeRef source_ty = source_llvm_ty;
    LLVMValueRef stack_tmp =
        LLVMBuildAlloca(cg->builder, source_ty, "fixed_to_dynamic_cast_src");
    LLVMBuildStore(cg->builder, val, stack_tmp);

    LLVMTypeRef i8_ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0);
    LLVMValueRef out_ptr_i8 =
        LLVMBuildBitCast(cg->builder, out_tmp, i8_ptr, "cast_array_out");
    LLVMValueRef stack_ptr_i8 =
        LLVMBuildBitCast(cg->builder, stack_tmp, i8_ptr, "cast_array_src");

    Type *elem_type = target_type->data.instance.generic_args.items[0];
    uint64_t fixed_count = 0;
    if (expr_type && expr_type->fixed_array_len > 0) {
      fixed_count = expr_type->fixed_array_len;
    } else if (LLVMGetTypeKind(source_llvm_ty) == LLVMArrayTypeKind) {
      fixed_count = (uint64_t)LLVMGetArrayLength(source_llvm_ty);
    } else {
      panic("Internal error: array-from-stack cast source is not fixed-size");
    }
    LLVMValueRef count =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), fixed_count, 0);
    LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
    const char *data_layout = LLVMGetDataLayout(cg->module);
    LLVMTargetDataRef td = LLVMCreateTargetData(data_layout);
    unsigned long long elem_size_bytes = LLVMABISizeOfType(td, llvm_elem_ty);
    LLVMDisposeTargetData(td);
    LLVMValueRef elem_size =
        LLVMConstInt(LLVMInt64TypeInContext(cg->context), elem_size_bytes, 0);

    LLVMBuildCall2(cg->builder, from_stack_fn->type, from_stack_fn->value,
                   (LLVMValueRef[]){out_ptr_i8, stack_ptr_i8, count, elem_size},
                   4, "");
    return LLVMBuildLoad2(cg->builder, target_ty, out_tmp,
                          "fixed_to_dynamic_cast_val");
  }

  if (expr_type && expr_type->kind == KIND_UNION &&
      expr_type->is_tagged_union) {
    int variant_index =
        cg_tagged_union_variant_index_llvm(cg, expr_type, target_ty);
    if (variant_index >= 0)
      return cg_extract_tagged_union(cg, val, expr_type, target_type,
                                     target_ty);
  }

  if (target_type && target_type->kind == KIND_UNION &&
      target_type->is_tagged_union) {
    int variant_index = cg_tagged_union_variant_index(target_type, expr_type);
    if (variant_index >= 0)
      return cg_make_tagged_union(cg, val, expr_type, target_type);
  }

  return cg_cast_value(cg, val, expr_type, target_ty);
}

LLVMValueRef cg_expression(Codegen *cg, AstNode *node) {
  if (!node)
    return NULL;
  switch (node->tag) {
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_BOOL:
  case NODE_STRING:
  case NODE_NULL:
  case NODE_NONE:
    return cg_const_expr(cg, node);
  case NODE_VAR:
    return cg_var_expr(cg, node);
  case NODE_FIELD:
    return cg_field_expr(cg, node);
  case NODE_STATIC_MEMBER: {
    Type *parent_type =
        type_get_named(cg->type_ctx, node->static_member.parent);
    if (parent_type && parent_type->kind == KIND_UNION &&
        parent_type->is_tagged_union) {
      Member *variant =
          type_get_member(parent_type, node->static_member.member);
      if (variant) {
        LLVMTypeRef union_ty = cg_type_get_llvm(cg, parent_type);
        LLVMValueRef tmp =
            LLVMBuildAlloca(cg->builder, union_ty, "tagged_union_tmp");
        LLVMValueRef tag_ptr = LLVMBuildStructGEP2(cg->builder, union_ty, tmp,
                                                   0, "tagged_union_tag_ptr");
        int variant_index = -1;
        for (size_t i = 0; i < parent_type->members.len; i++) {
          if (parent_type->members.items[i] == variant) {
            variant_index = (int)i;
            break;
          }
        }
        if (variant_index < 0)
          variant_index = 0;
        LLVMBuildStore(
            cg->builder,
            LLVMConstInt(LLVMInt64TypeInContext(cg->context), variant_index, 0),
            tag_ptr);
        return LLVMBuildLoad2(cg->builder, union_ty, tmp, "tagged_union_val");
      }
    }
    return cg_var_expr(cg, node);
  }
  case NODE_INDEX: {
    if (!node->resolved_type) {
      panic("Internal error: indexed expression has no resolved type");
    }
    LLVMValueRef element_ptr = cg_expression_addr(cg, node);
    return LLVMBuildLoad2(cg->builder,
                          cg_type_get_llvm(cg, node->resolved_type),
                          element_ptr, "load_element");
  }
  case NODE_BINARY_ARITH:
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    return cg_binary_expr(cg, node);
  case NODE_BINARY_IS:
    return cg_binary_is_expr(cg, node);
  case NODE_BINARY_ELSE:
    return cg_binary_else_expr(cg, node);
  case NODE_BLOCK:
    return cg_block_expr(cg, node);
  case NODE_UNARY:
    return cg_unary_expr(cg, node);
  case NODE_TERNARY:
    return cg_ternary_expr(cg, node);
  case NODE_ASSIGN_EXPR:
    return cg_assign_expr(cg, node);
  case NODE_CAST_EXPR:
    return cg_cast_expr(cg, node);
  case NODE_SIZEOF_EXPR: {
    Type *target = node->sizeof_expr.target_type;
    if (!target) {
      panic("sizeof target type is missing");
    }
    return LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                        (uint64_t)target->size, false);
  }
  case NODE_CALL:
    return cg_call_expr(cg, node);
  case NODE_NEW_EXPR:
    return cg_new_expr(cg, node);
  case NODE_ARRAY_LITERAL:
    return cg_array_literal(cg, node);
  case NODE_ARRAY_REPEAT:
    return cg_array_repeat(cg, node);
  default:
    fprintf(stderr, "Codegen: Unhandled expression tag %d\n", node->tag);
    return NULL;
  }
}
