#include <llvm-c/Core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/utils.h"

LLVMValueRef cg_extract_tagged_union_payload(Codegen *cg,
                                             LLVMValueRef union_val,
                                             Type *union_t,
                                             Type *variant_type);

void cg_control_flow_statement(Codegen *cg, AstNode *node) {
  switch (node->tag) {
  case NODE_BLOCK: {
    cg_push_scope(cg);
    for (size_t i = 0; i < node->block.statements.len; i++) {
      cg_statement(cg, node->block.statements.items[i]);
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      cg_emit_scope_drops(cg, cg->current_scope);
    }
    cg_pop_scope(cg);
    break;
  }
  case NODE_IF_STMT: {
    LLVMValueRef cond = cg_expression(cg, node->if_stmt.condition);
    cond = cg_cast_value(cg, cond, node->if_stmt.condition->resolved_type,
                         LLVMInt1TypeInContext(cg->context));

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef then_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "then");
    LLVMBasicBlockRef else_bb =
        node->if_stmt.else_branch
            ? LLVMAppendBasicBlockInContext(cg->context, func, "else")
            : NULL;
    LLVMBasicBlockRef merge_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "if_cont");

    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb ? else_bb : merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    cg_statement(cg, node->if_stmt.then_branch);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, merge_bb);
    }

    if (else_bb) {
      LLVMPositionBuilderAtEnd(cg->builder, else_bb);
      cg_statement(cg, node->if_stmt.else_branch);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        LLVMBuildBr(cg->builder, merge_bb);
      }
    }

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    break;
  }
  case NODE_SWITCH_STMT: {
    Type *expr_type = node->switch_stmt.expr->resolved_type;
    LLVMValueRef expr_val = cg_expression(cg, node->switch_stmt.expr);
    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef merge_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "switch_end");

    if (expr_type && ((expr_type->kind == KIND_PRIMITIVE &&
                       expr_type->data.primitive == PRIM_STRING) ||
                      expr_type->kind == KIND_STRING_BUFFER)) {
      if (expr_type->kind == KIND_STRING_BUFFER) {
        LLVMTypeRef slice_ty =
            cg_type_get_llvm(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
        LLVMValueRef undef = LLVMGetUndef(slice_ty);
        LLVMValueRef p =
            LLVMBuildExtractValue(cg->builder, expr_val, 0, "swbuf_ptr");
        LLVMValueRef l =
            LLVMBuildExtractValue(cg->builder, expr_val, 1, "swbuf_len");
        LLVMValueRef s0 =
            LLVMBuildInsertValue(cg->builder, undef, p, 0, "swbuf_slice0");
        expr_val = LLVMBuildInsertValue(cg->builder, s0, l, 1, "swbuf_slice");
      }
      bool use_hash = node->switch_stmt.cases.len > 3;
      LLVMValueRef expr_hash = NULL;
      if (use_hash) {
        expr_hash = cg_string_hash(cg, expr_val);
      }

      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        LLVMBasicBlockRef case_bb =
            LLVMAppendBasicBlockInContext(cg->context, func, "switch_case");
        LLVMBasicBlockRef next_bb = (i + 1 < node->switch_stmt.cases.len)
                                        ? LLVMAppendBasicBlockInContext(
                                              cg->context, func, "switch_next")
                                        : merge_bb;

        LLVMValueRef cond = NULL;
        if (use_hash) {
          if (case_node->case_stmt.pattern &&
              case_node->case_stmt.pattern->tag == NODE_STRING) {
            StringView value = case_node->case_stmt.pattern->string.value;
            uint64_t hash = 1469598103934665603ULL;
            for (size_t k = 0; k < value.len; k++) {
              hash ^= (uint64_t)(unsigned char)value.data[k];
              hash *= 1099511628211ULL;
            }
            if (hash == 0)
              hash = 1;
            LLVMValueRef target_hash =
                LLVMConstInt(LLVMInt64TypeInContext(cg->context), hash, 0);
            LLVMValueRef hash_eq =
                LLVMBuildICmp(cg->builder, LLVMIntEQ, expr_hash, target_hash,
                              "switch_hash_eq");
            LLVMBasicBlockRef hash_ok_bb = LLVMAppendBasicBlockInContext(
                cg->context, func, "switch_hash_ok");
            LLVMBuildCondBr(cg->builder, hash_eq, hash_ok_bb, next_bb);

            LLVMPositionBuilderAtEnd(cg->builder, hash_ok_bb);
            LLVMValueRef lit_val =
                cg_expression(cg, case_node->case_stmt.pattern);
            LLVMValueRef eq = cg_string_equals(cg, expr_val, lit_val);
            LLVMBuildCondBr(cg->builder, eq, case_bb, next_bb);
          } else if (!case_node->case_stmt.pattern) {
            LLVMBuildBr(cg->builder, case_bb);
          } else {
            LLVMBuildBr(cg->builder, next_bb);
          }
        } else {
          if (case_node->case_stmt.pattern &&
              case_node->case_stmt.pattern->tag == NODE_STRING) {
            LLVMValueRef lit_val =
                cg_expression(cg, case_node->case_stmt.pattern);
            LLVMValueRef eq = cg_string_equals(cg, expr_val, lit_val);
            LLVMBuildCondBr(cg->builder, eq, case_bb, next_bb);
          } else if (!case_node->case_stmt.pattern) {
            LLVMBuildBr(cg->builder, case_bb);
          } else {
            LLVMBuildBr(cg->builder, next_bb);
          }
        }

        LLVMPositionBuilderAtEnd(cg->builder, case_bb);
        List_push(&cg->break_stack, merge_bb);
        if (case_node->case_stmt.body) {
          cg_push_scope(cg);
          cg_statement(cg, case_node->case_stmt.body);
          cg_pop_scope(cg);
        }
        List_pop(&cg->break_stack);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, merge_bb);
        }
        if (next_bb != merge_bb) {
          LLVMPositionBuilderAtEnd(cg->builder, next_bb);
        }
      }
    } else if (expr_type && expr_type->kind == KIND_UNION &&
               expr_type->is_tagged_union) {
      LLVMValueRef tag =
          LLVMBuildExtractValue(cg->builder, expr_val, 0, "switch_union_tag");

      size_t explicit_case_count = 0;
      bool has_default = false;
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (case_node && case_node->tag == NODE_CASE &&
            !case_node->case_stmt.pattern) {
          has_default = true;
        } else {
          explicit_case_count++;
        }
      }

      LLVMBasicBlockRef default_bb = merge_bb;
      if (has_default) {
        default_bb =
            LLVMAppendBasicBlockInContext(cg->context, func, "switch_default");
      }

      LLVMValueRef switch_inst = LLVMBuildSwitch(cg->builder, tag, default_bb,
                                                 (unsigned)explicit_case_count);

      LLVMBasicBlockRef *case_blocks =
          xmalloc(sizeof(LLVMBasicBlockRef) * explicit_case_count);
      size_t case_block_index = 0;
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (!case_node || case_node->tag != NODE_CASE)
          continue;

        if (!case_node->case_stmt.pattern)
          continue;

        int variant_index = -1;
        for (size_t j = 0; j < expr_type->members.len; j++) {
          Member *m = expr_type->members.items[j];
          if (type_equals(m->type, case_node->case_stmt.pattern_type)) {
            variant_index = (int)j;
            break;
          }
        }
        LLVMValueRef expected_tag =
            LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                         (unsigned)(variant_index >= 0 ? variant_index : 0), 0);
        case_blocks[case_block_index] =
            LLVMAppendBasicBlockInContext(cg->context, func, "switch_case");
        LLVMAddCase(switch_inst, expected_tag, case_blocks[case_block_index]);
        case_block_index++;
      }

      size_t case_index = 0;
      for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
        AstNode *case_node = node->switch_stmt.cases.items[i];
        if (!case_node || case_node->tag != NODE_CASE)
          continue;

        LLVMBasicBlockRef case_bb;
        if (!case_node->case_stmt.pattern) {
          case_bb = default_bb;
        } else {
          case_bb = case_blocks[case_index];
          case_index++;
        }

        LLVMPositionBuilderAtEnd(cg->builder, case_bb);
        List_push(&cg->break_stack, merge_bb);
        cg_push_scope(cg);
        if (case_node->case_stmt.pattern &&
            case_node->case_stmt.pattern->tag == NODE_VAR) {
          LLVMValueRef payload = cg_extract_tagged_union_payload(
              cg, expr_val, expr_type, case_node->case_stmt.pattern_type);
          LLVMValueRef local_ptr =
              cg_alloca_in_entry(cg, case_node->case_stmt.pattern_type,
                                 case_node->case_stmt.pattern->var.value);
          LLVMBuildStore(cg->builder, payload, local_ptr);
          CGSymbolTable_add(cg->current_scope,
                            case_node->case_stmt.pattern->var.value,
                            case_node->case_stmt.pattern_type, local_ptr);
        }
        if (case_node->case_stmt.body) {
          cg_statement(cg, case_node->case_stmt.body);
        }
        cg_pop_scope(cg);
        List_pop(&cg->break_stack);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, merge_bb);
        }
      }

      free(case_blocks);
      LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    } else {
      // Unsupported types should already be diagnosed by semantic analysis
    }

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    break;
  }
  case NODE_LOOP_STMT: {
    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "loop_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "loop_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "loop_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, cond_bb);

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->loop.expr);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, cond_bb);
    }

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    break;
  }
  case NODE_BREAK: {
    if (cg->break_stack.len == 0)
      panic("Break outside of loop");
    LLVMBasicBlockRef target =
        List_get(&cg->break_stack, cg->break_stack.len - 1);
    LLVMBuildBr(cg->builder, target);
    break;
  }
  case NODE_CONTINUE: {
    if (cg->continue_stack.len == 0)
      panic("Continue outside of loop");
    LLVMBasicBlockRef target =
        List_get(&cg->continue_stack, cg->continue_stack.len - 1);
    LLVMBuildBr(cg->builder, target);
    break;
  }
  case NODE_FOR_IN_STMT: {
    cg_push_scope(cg);
    Type *iter_type = node->for_in_stmt.iterable->resolved_type;
    Type *elem_type = node->for_in_stmt.var->resolved_type;
    LLVMValueRef loop_var =
        cg_alloca_in_entry(cg, elem_type, node->for_in_stmt.var->var.value);
    CGSymbolTable_add(cg->current_scope, node->for_in_stmt.var->var.value,
                      elem_type, loop_var);

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_in_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_in_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_in_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, cond_bb);

    LLVMValueRef iterable_val = cg_expression(cg, node->for_in_stmt.iterable);
    LLVMValueRef index_ptr =
        cg_alloca_in_entry(cg, type_get_primitive(cg->type_ctx, PRIM_I64),
                           sv_from_cstr("for_in_index"));
    LLVMBuildStore(cg->builder,
                   LLVMConstInt(LLVMInt64TypeInContext(cg->context), 0, 0),
                   index_ptr);

    LLVMValueRef data_ptr = NULL;
    LLVMValueRef len_val = NULL;
    if (iter_type->kind == KIND_PRIMITIVE &&
        iter_type->data.primitive == PRIM_STRING) {
      LLVMValueRef str_ptr_val =
          LLVMBuildExtractValue(cg->builder, iterable_val, 0, "str_ptr");
      LLVMValueRef str_len_val =
          LLVMBuildExtractValue(cg->builder, iterable_val, 1, "str_len");
      data_ptr = str_ptr_val;
      len_val = str_len_val;
    } else if (iter_type->kind == KIND_STRING_BUFFER) {
      LLVMValueRef str_ptr_val =
          LLVMBuildExtractValue(cg->builder, iterable_val, 0, "strbuf_ptr");
      LLVMValueRef str_len_val =
          LLVMBuildExtractValue(cg->builder, iterable_val, 1, "strbuf_len");
      data_ptr = str_ptr_val;
      len_val = str_len_val;
    } else if (iter_type->kind == KIND_STRUCT &&
               iter_type->data.instance.generic_args.len > 0 &&
               sv_eq(iter_type->data.instance.from_template->name,
                     sv_from_cstr("Array"))) {
      LLVMTypeRef array_ty = cg_type_get_llvm(cg, iter_type);
      LLVMValueRef array_alloca =
          cg_alloca_in_entry(cg, iter_type, sv_from_cstr("for_in_array"));
      LLVMBuildStore(cg->builder, iterable_val, array_alloca);
      LLVMValueRef len_ptr = LLVMBuildStructGEP2(cg->builder, array_ty,
                                                 array_alloca, 1, "len_ptr");
      len_val = LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->context),
                               len_ptr, "len_val");
      LLVMValueRef data_ptr_ptr = LLVMBuildStructGEP2(
          cg->builder, array_ty, array_alloca, 0, "data_ptr_ptr");
      data_ptr = LLVMBuildLoad2(
          cg->builder,
          cg_type_get_llvm(
              cg, type_get_member(iter_type, sv_from_cstr("data"))->type),
          data_ptr_ptr, "data_ptr");
    }

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    if (!len_val) {
      panic("for-in: unsupported iterable type");
    }

    LLVMValueRef idx_cond = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), index_ptr, "idx");
    LLVMValueRef cond = LLVMBuildICmp(cg->builder, LLVMIntULT, idx_cond,
                                      len_val, "for_in_cond");
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    LLVMTypeRef llvm_elem_ty = cg_type_get_llvm(cg, elem_type);
    LLVMValueRef idx = LLVMBuildLoad2(
        cg->builder, LLVMInt64TypeInContext(cg->context), index_ptr, "idx");
    LLVMValueRef element_ptr =
        LLVMBuildGEP2(cg->builder, llvm_elem_ty, data_ptr,
                      (LLVMValueRef[]){idx}, 1, "elem_ptr");
    LLVMValueRef elem =
        LLVMBuildLoad2(cg->builder, llvm_elem_ty, element_ptr, "elem");
    LLVMBuildStore(cg->builder, elem, loop_var);

    cg_statement(cg, node->for_in_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMValueRef idx = LLVMBuildLoad2(
          cg->builder, LLVMInt64TypeInContext(cg->context), index_ptr, "idx");
      LLVMValueRef next_idx = LLVMBuildAdd(
          cg->builder, idx,
          LLVMConstInt(LLVMInt64TypeInContext(cg->context), 1, 0), "next_idx");
      LLVMBuildStore(cg->builder, next_idx, index_ptr);
      LLVMBuildBr(cg->builder, cond_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      cg_emit_scope_drops(cg, cg->current_scope);
    }
    cg_pop_scope(cg);
    break;
  }
  case NODE_WHILE_STMT: {
    cg_push_scope(cg);

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "while_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "while_body");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "while_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, cond_bb);

    LLVMBuildBr(cg->builder, cond_bb);
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);

    LLVMValueRef cond = cg_expression(cg, node->while_stmt.condition);
    cond = cg_cast_value(cg, cond, node->while_stmt.condition->resolved_type,
                         LLVMInt1TypeInContext(cg->context));
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->while_stmt.body);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, cond_bb);
    }

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      cg_emit_scope_drops(cg, cg->current_scope);
    }
    cg_pop_scope(cg);
    break;
  }
  case NODE_FOR_STMT: {
    cg_push_scope(cg);

    if (node->for_stmt.init) {
      cg_statement(cg, node->for_stmt.init);
    }

    LLVMValueRef func = cg->current_function;
    LLVMBasicBlockRef cond_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_cond");
    LLVMBasicBlockRef body_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_body");
    LLVMBasicBlockRef inc_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_inc");
    LLVMBasicBlockRef end_bb =
        LLVMAppendBasicBlockInContext(cg->context, func, "for_end");

    List_push(&cg->break_stack, end_bb);
    List_push(&cg->continue_stack, inc_bb);

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    if (node->for_stmt.condition) {
      LLVMValueRef cond_val = cg_expression(cg, node->for_stmt.condition);
      cond_val =
          cg_cast_value(cg, cond_val, node->for_stmt.condition->resolved_type,
                        LLVMInt1TypeInContext(cg->context));
      LLVMBuildCondBr(cg->builder, cond_val, body_bb, end_bb);
    } else {
      LLVMBuildBr(cg->builder, body_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    cg_statement(cg, node->for_stmt.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, inc_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
    if (node->for_stmt.increment) {
      cg_expression(cg, node->for_stmt.increment);
    }
    LLVMBuildBr(cg->builder, cond_bb);

    List_pop(&cg->break_stack);
    List_pop(&cg->continue_stack);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      cg_emit_scope_drops(cg, cg->current_scope);
    }
    cg_pop_scope(cg);
    break;
  }
  default:
    fprintf(stderr, "Codegen: Unhandled statement tag %d\n", node->tag);
    break;
  }
}
