#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cg_internal.h"
#include "tyna/ast.h"
#include "tyna/codegen.h"
#include "tyna/sema.h"
#include "tyna/utils.h"
#include "llvm-c/Types.h"

static bool cg_is_error_print_method(StringView name) {
  const char *prefix = "__tyna_error_print_";
  size_t prefix_len = strlen(prefix);
  return name.len > prefix_len && memcmp(name.data, prefix, prefix_len) == 0;
}

static LLVMValueRef cg_get_error_field_value(Codegen *cg,
                                             LLVMValueRef error_ptr,
                                             Type *error_type, Member *member) {
  LLVMTypeRef err_ty = cg_type_get_llvm(cg, error_type);
  LLVMValueRef field_ptr = LLVMBuildStructGEP2(
      cg->builder, err_ty, error_ptr, member->index, "error_field_ptr");
  LLVMTypeRef field_ty = cg_type_get_llvm(cg, member->type);
  return LLVMBuildLoad2(cg->builder, field_ty, field_ptr, "error_field");
}

static void cg_append_fmt_char(char **fmt, size_t *fmt_len, size_t *fmt_cap,
                               char ch) {
  if (*fmt_len + 2 >= *fmt_cap) {
    *fmt_cap = *fmt_cap ? *fmt_cap * 2 : 256;
    *fmt = realloc(*fmt, *fmt_cap);
  }
  (*fmt)[(*fmt_len)++] = ch;
}

static void cg_append_fmt_text(char **fmt, size_t *fmt_len, size_t *fmt_cap,
                               const char *text) {
  while (*text) {
    char ch = *text++;
    if (ch == '%') {
      cg_append_fmt_char(fmt, fmt_len, fmt_cap, '%');
      cg_append_fmt_char(fmt, fmt_len, fmt_cap, '%');
    } else {
      cg_append_fmt_char(fmt, fmt_len, fmt_cap, ch);
    }
  }
}

static void cg_append_fmt_literal(char **fmt, size_t *fmt_len, size_t *fmt_cap,
                                  const char *text) {
  while (*text) {
    cg_append_fmt_char(fmt, fmt_len, fmt_cap, *text++);
  }
}

static void cg_append_fmt_stringview(char **fmt, size_t *fmt_len,
                                     size_t *fmt_cap, StringView sv) {
  for (size_t i = 0; i < sv.len; i++) {
    char ch = sv.data[i];
    if (ch == '%') {
      cg_append_fmt_char(fmt, fmt_len, fmt_cap, '%');
      cg_append_fmt_char(fmt, fmt_len, fmt_cap, '%');
    } else {
      cg_append_fmt_char(fmt, fmt_len, fmt_cap, ch);
    }
  }
}

static void cg_push_fmt_arg(LLVMValueRef **fmt_args, size_t *arg_count,
                            LLVMValueRef value) {
  *fmt_args = realloc(*fmt_args, sizeof(LLVMValueRef) * (*arg_count + 1));
  (*fmt_args)[(*arg_count)++] = value;
}

static LLVMValueRef cg_error_print_call(Codegen *cg, AstNode *node) {
  if (node->call.args.len == 0)
    panic("Error print call missing self argument");

  AstNode *self_node = node->call.args.items[0];
  LLVMValueRef self_val = cg_expression(cg, self_node);
  Type *self_type = self_node->resolved_type;
  if (!self_type) {
    panic("Error print call missing resolved self type");
  }

  bool self_is_ptr = false;
  while (self_type && self_type->kind == KIND_POINTER) {
    self_is_ptr = true;
    self_type = self_type->data.pointer_to;
  }

  if (!self_type || self_type->kind != KIND_ERROR) {
    panic("Error print call on non-error type");
  }

  Type *error_type = self_type;
  LLVMTypeRef err_ty = cg_type_get_llvm(cg, error_type);
  LLVMValueRef err_ptr = self_val;
  if (!self_is_ptr) {
    LLVMValueRef temp = cg_alloca_in_entry_uninitialized(
        cg, error_type, sv_from_cstr("error_tmp"));
    LLVMBuildStore(cg->builder, self_val, temp);
    err_ptr = temp;
  }

  StringView tmpl = error_type->error_message_template;
  char *fmt = NULL;
  size_t fmt_len = 0;
  size_t fmt_cap = 0;
  LLVMValueRef *fmt_args = NULL;
  size_t arg_count = 0;

  if (tmpl.len > 0) {
    for (size_t i = 0; i < tmpl.len; i++) {
      char c = tmpl.data[i];
      if (c != '$') {
        cg_append_fmt_char(&fmt, &fmt_len, &fmt_cap, c);
        continue;
      }

      size_t j = i + 1;
      if (j >= tmpl.len || tmpl.data[j] < '0' || tmpl.data[j] > '9') {
        cg_append_fmt_char(&fmt, &fmt_len, &fmt_cap, '$');
        continue;
      }

      int idx = 0;
      while (j < tmpl.len && tmpl.data[j] >= '0' && tmpl.data[j] <= '9') {
        idx = idx * 10 + (tmpl.data[j] - '0');
        j++;
      }

      if (idx < 0 || (size_t)idx >= error_type->members.len) {
        panic("Error message placeholder out of range");
      }

      Member *member = error_type->members.items[idx];
      LLVMValueRef field_val =
          cg_get_error_field_value(cg, err_ptr, error_type, member);
      Type *field_ty = member->type;
      if (field_ty->kind == KIND_PRIMITIVE) {
        switch (field_ty->data.primitive) {
        case PRIM_BOOL: {
          LLVMValueRef true_str =
              cg_get_string_constant_ptr(cg, sv_from_cstr("true"));
          LLVMValueRef false_str =
              cg_get_string_constant_ptr(cg, sv_from_cstr("false"));
          LLVMValueRef is_true = LLVMBuildICmp(
              cg->builder, LLVMIntEQ, field_val,
              LLVMConstInt(LLVMTypeOf(field_val), 1, 0), "bool_eq");
          LLVMValueRef selected = LLVMBuildSelect(
              cg->builder, is_true, true_str, false_str, "bool_str");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%s");
          cg_push_fmt_arg(&fmt_args, &arg_count, selected);
          break;
        }
        case PRIM_CHAR: {
          LLVMValueRef char_val =
              LLVMBuildZExt(cg->builder, field_val,
                            LLVMInt32TypeInContext(cg->context), "char_to_int");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%c");
          cg_push_fmt_arg(&fmt_args, &arg_count, char_val);
          break;
        }
        case PRIM_I8:
        case PRIM_I16:
        case PRIM_I32: {
          LLVMValueRef int_val =
              LLVMBuildSExt(cg->builder, field_val,
                            LLVMInt32TypeInContext(cg->context), "int_prom");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%d");
          cg_push_fmt_arg(&fmt_args, &arg_count, int_val);
          break;
        }
        case PRIM_U8:
        case PRIM_U16:
        case PRIM_U32: {
          LLVMValueRef uint_val =
              LLVMBuildZExt(cg->builder, field_val,
                            LLVMInt32TypeInContext(cg->context), "uint_prom");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%u");
          cg_push_fmt_arg(&fmt_args, &arg_count, uint_val);
          break;
        }
        case PRIM_I64: {
          LLVMValueRef int_val =
              LLVMBuildSExt(cg->builder, field_val,
                            LLVMInt64TypeInContext(cg->context), "i64_prom");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%ld");
          cg_push_fmt_arg(&fmt_args, &arg_count, int_val);
          break;
        }
        case PRIM_U64: {
          LLVMValueRef uint_val =
              LLVMBuildZExt(cg->builder, field_val,
                            LLVMInt64TypeInContext(cg->context), "u64_prom");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%lu");
          cg_push_fmt_arg(&fmt_args, &arg_count, uint_val);
          break;
        }
        case PRIM_F32: {
          LLVMValueRef float_val =
              LLVMBuildFPExt(cg->builder, field_val,
                             LLVMDoubleTypeInContext(cg->context), "f_prom");
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%f");
          cg_push_fmt_arg(&fmt_args, &arg_count, float_val);
          break;
        }
        case PRIM_F64: {
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%f");
          cg_push_fmt_arg(&fmt_args, &arg_count, field_val);
          break;
        }
        case PRIM_STRING: {
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%s");
          LLVMValueRef str_ptr =
              LLVMBuildExtractValue(cg->builder, field_val, 0, "str_ptr");
          cg_push_fmt_arg(&fmt_args, &arg_count, str_ptr);
          break;
        }
        default: {
          cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%p");
          LLVMValueRef ptr_val = LLVMBuildBitCast(
              cg->builder, field_val,
              LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
              "ptr_val");
          cg_push_fmt_arg(&fmt_args, &arg_count, ptr_val);
          break;
        }
        }
      } else if (field_ty->kind == KIND_POINTER &&
                 field_ty->data.pointer_to->kind == KIND_PRIMITIVE &&
                 field_ty->data.pointer_to->data.primitive == PRIM_CHAR) {
        cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%s");
        cg_push_fmt_arg(&fmt_args, &arg_count, field_val);
      } else {
        cg_append_fmt_literal(&fmt, &fmt_len, &fmt_cap, "%p");
        LLVMValueRef ptr_val = LLVMBuildBitCast(
            cg->builder, field_val,
            LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0), "ptr_val");
        cg_push_fmt_arg(&fmt_args, &arg_count, ptr_val);
      }

      i = j - 1;
    }
  } else {
    cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "Error ");
    cg_append_fmt_stringview(&fmt, &fmt_len, &fmt_cap, error_type->name);
    if (error_type->members.len > 0) {
      cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "{");
      for (size_t idx = 0; idx < error_type->members.len; idx++) {
        if (idx > 0)
          cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, ", ");
        Member *member = error_type->members.items[idx];
        cg_append_fmt_stringview(&fmt, &fmt_len, &fmt_cap,
                                 sv_from_cstr(member->name));
        cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "=");
        LLVMValueRef field_val =
            cg_get_error_field_value(cg, err_ptr, error_type, member);
        Type *field_ty = member->type;
        if (field_ty->kind == KIND_PRIMITIVE) {
          switch (field_ty->data.primitive) {
          case PRIM_BOOL: {
            LLVMValueRef true_str =
                cg_get_string_constant_ptr(cg, sv_from_cstr("true"));
            LLVMValueRef false_str =
                cg_get_string_constant_ptr(cg, sv_from_cstr("false"));
            LLVMValueRef is_true = LLVMBuildICmp(
                cg->builder, LLVMIntEQ, field_val,
                LLVMConstInt(LLVMTypeOf(field_val), 1, 0), "bool_eq");
            LLVMValueRef selected = LLVMBuildSelect(
                cg->builder, is_true, true_str, false_str, "bool_str");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%s");
            cg_push_fmt_arg(&fmt_args, &arg_count, selected);
            break;
          }
          case PRIM_CHAR: {
            LLVMValueRef char_val = LLVMBuildZExt(
                cg->builder, field_val, LLVMInt32TypeInContext(cg->context),
                "char_to_int");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%c");
            cg_push_fmt_arg(&fmt_args, &arg_count, char_val);
            break;
          }
          case PRIM_I8:
          case PRIM_I16:
          case PRIM_I32: {
            LLVMValueRef int_val =
                LLVMBuildSExt(cg->builder, field_val,
                              LLVMInt32TypeInContext(cg->context), "int_prom");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%d");
            cg_push_fmt_arg(&fmt_args, &arg_count, int_val);
            break;
          }
          case PRIM_U8:
          case PRIM_U16:
          case PRIM_U32: {
            LLVMValueRef uint_val =
                LLVMBuildZExt(cg->builder, field_val,
                              LLVMInt32TypeInContext(cg->context), "uint_prom");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%u");
            cg_push_fmt_arg(&fmt_args, &arg_count, uint_val);
            break;
          }
          case PRIM_I64: {
            LLVMValueRef int_val =
                LLVMBuildSExt(cg->builder, field_val,
                              LLVMInt64TypeInContext(cg->context), "i64_prom");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%ld");
            cg_push_fmt_arg(&fmt_args, &arg_count, int_val);
            break;
          }
          case PRIM_U64: {
            LLVMValueRef uint_val =
                LLVMBuildZExt(cg->builder, field_val,
                              LLVMInt64TypeInContext(cg->context), "u64_prom");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%lu");
            cg_push_fmt_arg(&fmt_args, &arg_count, uint_val);
            break;
          }
          case PRIM_F32: {
            LLVMValueRef float_val =
                LLVMBuildFPExt(cg->builder, field_val,
                               LLVMDoubleTypeInContext(cg->context), "f_prom");
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%f");
            cg_push_fmt_arg(&fmt_args, &arg_count, float_val);
            break;
          }
          case PRIM_F64: {
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%f");
            cg_push_fmt_arg(&fmt_args, &arg_count, field_val);
            break;
          }
          case PRIM_STRING: {
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%s");
            LLVMValueRef str_ptr =
                LLVMBuildExtractValue(cg->builder, field_val, 0, "str_ptr");
            cg_push_fmt_arg(&fmt_args, &arg_count, str_ptr);
            break;
          }
          default: {
            cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%p");
            LLVMValueRef ptr_val = LLVMBuildBitCast(
                cg->builder, field_val,
                LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
                "ptr_val");
            cg_push_fmt_arg(&fmt_args, &arg_count, ptr_val);
            break;
          }
          }
        } else if (field_ty->kind == KIND_POINTER &&
                   field_ty->data.pointer_to->kind == KIND_PRIMITIVE &&
                   field_ty->data.pointer_to->data.primitive == PRIM_CHAR) {
          cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%s");
          cg_push_fmt_arg(&fmt_args, &arg_count, field_val);
        } else {
          cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "%p");
          LLVMValueRef ptr_val = LLVMBuildBitCast(
              cg->builder, field_val,
              LLVMPointerType(LLVMInt8TypeInContext(cg->context), 0),
              "ptr_val");
          cg_push_fmt_arg(&fmt_args, &arg_count, ptr_val);
        }
      }
      cg_append_fmt_text(&fmt, &fmt_len, &fmt_cap, "}");
    }
  }

  cg_append_fmt_char(&fmt, &fmt_len, &fmt_cap, '\n');
  cg_append_fmt_char(&fmt, &fmt_len, &fmt_cap, '\0');
  StringView fmt_sv = sv_from_parts(fmt, fmt_len);
  LLVMValueRef fmt_ptr = cg_get_string_constant_ptr(cg, fmt_sv);

  CgFunc *printf_fn = cg_find_function(cg, sv_from_cstr("printf"));
  if (!printf_fn)
    panic("Missing printf for error print");

  LLVMValueRef *args = xmalloc(sizeof(LLVMValueRef) * (1 + arg_count));
  args[0] = fmt_ptr;
  for (size_t i = 0; i < arg_count; i++)
    args[i + 1] = fmt_args[i];

  LLVMValueRef result =
      LLVMBuildCall2(cg->builder, printf_fn->type, printf_fn->value, args,
                     (unsigned)(1 + arg_count), "");
  free(args);
  free(fmt_args);
  free(fmt);
  return result;
}

static LLVMValueRef cg_typeof_string(Codegen *cg, Type *type) {
  const char *type_name = type_to_name(type);
  StringView name_sv = sv_from_cstr(type_name);
  size_t pool_idx = cg_string_pool_insert(cg, name_sv);
  LLVMValueRef const_str_global = cg->string_globals.items[pool_idx];
  LLVMValueRef zero =
      LLVMConstInt(LLVMInt32TypeInContext(cg->context), 0, false);
  LLVMValueRef indices[] = {zero, zero};
  LLVMValueRef ptr = LLVMConstInBoundsGEP2(LLVMInt8TypeInContext(cg->context),
                                           const_str_global, indices, 2);
  LLVMTypeRef string_ty =
      cg_type_get_llvm(cg, type_get_primitive(cg->type_ctx, PRIM_STRING));
  LLVMValueRef len =
      LLVMConstInt(LLVMInt64TypeInContext(cg->context), name_sv.len, false);
  LLVMValueRef fields[] = {ptr, len};
  return LLVMConstNamedStruct(string_ty, fields, 2);
}

static Symbol *cg_find_method(Type *type, StringView name) {
  if (!type)
    return NULL;
  for (size_t i = 0; i < type->methods.len; i++) {
    Symbol *method = type->methods.items[i];
    if (!method)
      continue;
    StringView lookup_name =
        method->original_name.len ? method->original_name : method->name;
    if (sv_eq(lookup_name, name))
      return method;
  }
  return NULL;
}

LLVMValueRef cg_new_expr(Codegen *cg, AstNode *node) {
  Type *target_type = node->new_expr.target_type;
  if (!target_type)
    panic("new expression missing target type");

  if (target_type->kind == KIND_ERROR) {
    if (node->new_expr.args.len > 0 && node->new_expr.field_inits.len > 0) {
      panic("Cannot mix constructor arguments and error literal fields");
    }

    LLVMTypeRef struct_ty = cg_type_get_llvm(cg, target_type);
    LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, struct_ty, "error_tmp");
    LLVMBuildStore(cg->builder, LLVMConstNull(struct_ty), tmp);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
      AstNode *assign = node->new_expr.field_inits.items[i];
      if (assign->tag != NODE_ASSIGN_EXPR ||
          assign->assign_expr.target->tag != NODE_VAR) {
        panic("Invalid error field initializer in codegen");
      }
      StringView field_name = assign->assign_expr.target->var.value;
      Member *member = type_get_member(target_type, field_name);
      if (!member)
        panic("Field '%.*s' not found on type %s", (int)field_name.len,
              field_name.data, type_to_name(target_type));
      LLVMValueRef field_ptr = LLVMBuildStructGEP2(cg->builder, struct_ty, tmp,
                                                   member->index, "field_ptr");
      LLVMValueRef value = cg_expression(cg, assign->assign_expr.value);
      LLVMTypeRef field_ty = cg_type_get_llvm(cg, member->type);
      LLVMValueRef casted = cg_cast_value(
          cg, value, assign->assign_expr.value->resolved_type, field_ty);
      LLVMBuildStore(cg->builder, casted, field_ptr);
    }
    return LLVMBuildLoad2(cg->builder, struct_ty, tmp, "error_value");
  }

  LLVMTypeRef ptr_ty =
      cg_type_get_llvm(cg, type_get_pointer(cg->type_ctx, target_type));
  CgFunc *malloc_fn = cg_find_function(cg, sv_from_cstr("malloc"));
  if (!malloc_fn)
    panic("malloc not found for new expression");

  LLVMValueRef size_arg = LLVMConstInt(LLVMInt64TypeInContext(cg->context),
                                       target_type->size, false);
  LLVMValueRef malloc_args[] = {size_arg};
  LLVMValueRef raw_ptr =
      LLVMBuildCall2(cg->builder, malloc_fn->type, malloc_fn->value,
                     malloc_args, 1, "malloc_res");
  LLVMValueRef obj_ptr = cg_cast_value(cg, raw_ptr, NULL, ptr_ty);

  if (node->new_expr.args.len > 0) {
    Symbol *constructor = cg_find_method(target_type, sv_from_cstr("init"));
    if (!constructor || !constructor->name.data)
      panic("Missing constructor 'init' for type %s",
            type_to_name(target_type));
    CgFunc *fn = cg_find_function(cg, constructor->name);
    if (!fn)
      panic("Constructor function not found for %s", type_to_name(target_type));

    unsigned param_count = LLVMCountParams(fn->value);
    unsigned arg_count = (unsigned)node->new_expr.args.len + 1;
    LLVMTypeRef *param_types =
        param_count > 0 ? malloc(sizeof(LLVMTypeRef) * param_count) : NULL;
    if (param_count > 0)
      LLVMGetParamTypes(fn->type, param_types);

    LLVMValueRef *args = xmalloc(sizeof(LLVMValueRef) * arg_count);
    args[0] = obj_ptr;
    for (unsigned i = 0; i < node->new_expr.args.len; i++) {
      AstNode *arg_node = node->new_expr.args.items[i];
      LLVMValueRef arg_val = cg_expression(cg, arg_node);
      LLVMTypeRef param_ty = param_types[i + 1];
      args[i + 1] =
          cg_cast_value(cg, arg_val, arg_node->resolved_type, param_ty);
    }

    LLVMBuildCall2(cg->builder, fn->type, fn->value, args, arg_count, "");
    if (param_types)
      free(param_types);
    free(args);
    return obj_ptr;
  }

  if (node->new_expr.field_inits.len > 0) {
    LLVMTypeRef struct_ty = cg_type_get_llvm(cg, target_type);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
      AstNode *assign = node->new_expr.field_inits.items[i];
      if (assign->tag != NODE_ASSIGN_EXPR ||
          assign->assign_expr.target->tag != NODE_VAR) {
        panic("Invalid struct field initializer in codegen");
      }
      StringView field_name = assign->assign_expr.target->var.value;
      Member *member = type_get_member(target_type, field_name);
      if (!member)
        panic("Field '%.*s' not found on type %s", (int)field_name.len,
              field_name.data, type_to_name(target_type));
      LLVMValueRef field_ptr = LLVMBuildStructGEP2(
          cg->builder, struct_ty, obj_ptr, member->index, "field_ptr");
      LLVMValueRef value = cg_expression(cg, assign->assign_expr.value);
      LLVMTypeRef field_ty = cg_type_get_llvm(cg, member->type);
      LLVMValueRef casted = cg_cast_value(
          cg, value, assign->assign_expr.value->resolved_type, field_ty);
      LLVMBuildStore(cg->builder, casted, field_ptr);
    }
  }

  return obj_ptr;
}

LLVMValueRef cg_call_expr(Codegen *cg, AstNode *node) {
  if (node->call.func->tag != NODE_VAR)
    panic("Function calls must be by name");
  StringView fn_name = node->call.func->var.value;
  if (sv_eq(fn_name, sv_from_cstr("typeof"))) {
    Type *arg_type = NULL;
    if (node->call.args.len > 0) {
      AstNode *arg_node = node->call.args.items[0];
      arg_type = arg_node->resolved_type;
    }
    if (!arg_type)
      arg_type = type_get_primitive(cg->type_ctx, PRIM_UNKNOWN);
    return cg_typeof_string(cg, arg_type);
  }

  if (cg_is_error_print_method(fn_name)) {
    return cg_error_print_call(cg, node);
  }

  CgFunc *fn = cg_find_function(cg, fn_name);
  if (!fn)
    panic("Call to undefined function '" SV_FMT "'", SV_ARG(fn_name));

  unsigned arg_count = (unsigned)node->call.args.len;
  LLVMValueRef *args =
      arg_count > 0 ? malloc(sizeof(LLVMValueRef) * arg_count) : NULL;
  unsigned param_count = LLVMCountParams(fn->value);
  LLVMTypeRef *param_types =
      param_count > 0 ? malloc(sizeof(LLVMTypeRef) * param_count) : NULL;
  if (param_count > 0)
    LLVMGetParamTypes(fn->type, param_types);

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

  LLVMTypeRef ret_ty = LLVMGetReturnType(fn->type);
  const char *call_name =
      (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind) ? "" : "calltmp";
  LLVMValueRef result = LLVMBuildCall2(cg->builder, fn->type, fn->value, args,
                                       arg_count, call_name);

  if (args)
    free(args);
  if (param_types)
    free(param_types);

  if (node->resolved_type) {
    LLVMTypeRef expected_ty = cg_type_get_llvm(cg, node->resolved_type);
    if (expected_ty != ret_ty) {
      result = cg_cast_value(cg, result, NULL, expected_ty);
    }
  }
  return result;
}
