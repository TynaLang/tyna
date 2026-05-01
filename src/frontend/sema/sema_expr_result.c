#include "sema_internal.h"

static bool sema_result_error_set_is_wildcard(Type *error_set) {
  return error_set && error_set->kind == KIND_ERROR_SET &&
         (error_set->name.len == 0 || sv_eq_cstr(error_set->name, "Error"));
}

ExprInfo sema_check_binary_is(Sema *s, AstNode *node) {
  AstNode *right_node = node->binary_is.right;
  Type *left = sema_check_expr(s, node->binary_is.left).type;
  Type *right = sema_check_expr(s, right_node).type;
  Type *result = type_get_primitive(s->types, PRIM_BOOL);

  if (!left || !right) {
    sema_error(s, node, "Invalid operands for 'is' expression");
    return (ExprInfo){.type = result, .category = VAL_RVALUE};
  }

  bool right_is_error_directive = right && right->kind == KIND_ERROR_SET &&
                                  sv_eq(right->name, sv_from_cstr("Error"));

  if (left->kind == KIND_RESULT) {
    if (!left->data.result.error_set) {
      sema_error(s, node,
                 "Result type '%s' is missing an error set for 'is' checks",
                 type_to_name(left));
      return (ExprInfo){.type = result, .category = VAL_RVALUE};
    }
    if (right_is_error_directive) {
      return (ExprInfo){.type = result, .category = VAL_RVALUE};
    }
    if (right->kind == KIND_ERROR) {
      if (!error_set_contains(left->data.result.error_set, right)) {
        sema_error(s, node, "Type '%s' is not in error set '%s'",
                   type_to_name(right),
                   type_to_name(left->data.result.error_set));
      }
    } else if (right->kind == KIND_ERROR_SET) {
      if (!type_equals(left->data.result.error_set, right)) {
        sema_error(s, node, "Cannot compare result error set '%s' with '%s'",
                   type_to_name(left->data.result.error_set),
                   type_to_name(right));
      }
    } else {
      sema_error(s, node,
                 "Cannot compare result type '%s' with '%s' using 'is'",
                 type_to_name(left), type_to_name(right));
    }
  } else if (left->kind == KIND_ERROR) {
    if (right_is_error_directive) {
      return (ExprInfo){.type = result, .category = VAL_RVALUE};
    }
    if (right->kind == KIND_ERROR) {
      if (!type_equals(left, right)) {
        /* Different error types still produce a valid boolean expression. */
      }
    } else if (right->kind == KIND_ERROR_SET) {
      if (!error_set_contains(right, left)) {
        sema_error(s, node, "Type '%s' is not in error set '%s'",
                   type_to_name(left), type_to_name(right));
      }
    } else {
      sema_error(s, node, "Cannot compare error type '%s' with '%s' using 'is'",
                 type_to_name(left), type_to_name(right));
    }
  } else {
    Type *union_type = left;
    if (left->kind == KIND_POINTER && left->data.pointer_to &&
        left->data.pointer_to->kind == KIND_UNION &&
        left->data.pointer_to->is_tagged_union) {
      union_type = left->data.pointer_to;
    }

    if (union_type && union_type->kind == KIND_UNION &&
        union_type->is_tagged_union) {
      Type *target_type = NULL;
      Member *variant = NULL;

      if (right_node->tag == NODE_STATIC_MEMBER) {
        target_type =
            sema_find_type_by_name(s, right_node->static_member.parent);
        if (target_type && type_equals(target_type, union_type)) {
          variant =
              type_get_member(target_type, right_node->static_member.member);
        }
      } else if (right_node->tag == NODE_CALL &&
                 right_node->call.func->tag == NODE_STATIC_MEMBER) {
        target_type = sema_find_type_by_name(
            s, right_node->call.func->static_member.parent);
        if (target_type && type_equals(target_type, union_type)) {
          variant = type_get_member(
              target_type, right_node->call.func->static_member.member);
        }
      } else {
        target_type = right;
      }

      if (variant) {
        if (right_node->tag == NODE_CALL && right_node->call.args.len == 0) {
          Type *payload_type = variant->type;
          if (payload_type->kind != KIND_PRIMITIVE ||
              payload_type->data.primitive != PRIM_VOID) {
            sema_error(s, node, "Enum variant '%.*s' requires a payload",
                       (int)right_node->call.func->static_member.member.len,
                       right_node->call.func->static_member.member.data);
          }
        }
      } else if (target_type) {
        bool found_variant = false;
        for (size_t i = 0; i < union_type->members.len; i++) {
          Member *m = union_type->members.items[i];
          if (type_equals(m->type, target_type)) {
            found_variant = true;
            break;
          }
        }
        if (!found_variant) {
          sema_error(s, node, "Type '%s' is not a variant of tagged union '%s'",
                     type_to_name(right), type_to_name(union_type));
        }
      } else {
        sema_error(s, node,
                   "Cannot compare tagged union '%s' with '%s' using 'is'",
                   type_to_name(union_type), type_to_name(right));
      }
    } else {
      sema_error(
          s, node,
          "The 'is' operator requires a result or error type on the left, "
          "got '%s'",
          type_to_name(left));
    }
  }

  return (ExprInfo){.type = result, .category = VAL_RVALUE};
}

static bool sema_block_has_return(AstNode *block) {
  if (!block || block->tag != NODE_BLOCK)
    return false;
  for (size_t i = 0; i < block->block.statements.len; i++) {
    AstNode *stmt = block->block.statements.items[i];
    if (stmt && stmt->tag == NODE_RETURN_STMT)
      return true;
  }
  return false;
}

ExprInfo sema_check_binary_else(Sema *s, AstNode *node) {
  Type *left = sema_check_expr(s, node->binary_else.left).type;

  if (!left) {
    sema_error(s, node, "Invalid operands for 'else' expression");
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE};
  }

  if (left->kind == KIND_RESULT) {
    if (!node->binary_else.right) {
      if (s->fn_node) {
        if (s->ret_type && s->ret_type->kind == KIND_RESULT &&
            s->ret_type->data.result.error_set &&
            sema_result_error_set_is_wildcard(
                s->ret_type->data.result.error_set) &&
            left->data.result.error_set &&
            left->data.result.error_set->kind == KIND_ERROR_SET) {
          for (size_t i = 0; i < left->data.result.error_set->members.len;
               i++) {
            Member *member = left->data.result.error_set->members.items[i];
            if (member && member->type && member->type->kind == KIND_ERROR &&
                !error_set_contains(s->ret_type->data.result.error_set,
                                    member->type)) {
              type_add_member(s->ret_type->data.result.error_set, NULL,
                              member->type, 0);
            }
          }
        }
        return (ExprInfo){.type = left->data.result.success,
                          .category = VAL_RVALUE};
      }
      sema_error(s, node, "The '?' operator requires a function context",
                 type_to_name(left));
      return (ExprInfo){.type = left->data.result.success,
                        .category = VAL_RVALUE};
    }

    if (node->binary_else.right &&
        node->binary_else.right->tag == NODE_RETURN_STMT) {
      sema_check_stmt(s, node->binary_else.right);
      return (ExprInfo){.type = left->data.result.success,
                        .category = VAL_RVALUE};
    }

    if (node->binary_else.right && node->binary_else.right->tag == NODE_BLOCK) {
      sema_check_stmt(s, node->binary_else.right);
      return (ExprInfo){.type = left->data.result.success,
                        .category = VAL_RVALUE};
    }

    Type *right = sema_check_expr(s, node->binary_else.right).type;
    if (!right) {
      sema_error(s, node, "Invalid operands for 'else' expression");
      return (ExprInfo){.type = left->data.result.success,
                        .category = VAL_RVALUE};
    }

    if (type_can_implicitly_cast(left->data.result.success, right)) {
      return (ExprInfo){.type = left->data.result.success,
                        .category = VAL_RVALUE};
    }
    sema_error(s, node,
               "Else expression type '%s' not compatible with result success "
               "type '%s'",
               type_to_name(right), type_to_name(left->data.result.success));
    return (ExprInfo){.type = left->data.result.success,
                      .category = VAL_RVALUE};
  }

  sema_error(s, node,
             "The 'else' operator requires a result type on the left, got '%s'",
             type_to_name(left));
  return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                    .category = VAL_RVALUE};
}