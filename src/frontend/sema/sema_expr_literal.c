#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "sema_internal.h"

bool literal_fits_in_type(AstNode *node, Type *target) {
  if (node->tag != NODE_NUMBER || !target || target->kind != KIND_PRIMITIVE)
    return false;

  if (sv_contains(node->number.raw_text, '.') ||
      sv_ends_with(node->number.raw_text, "f") ||
      sv_ends_with(node->number.raw_text, "F")) {
    return false;
  }

  double val = node->number.value;
  if (floor(val) != val)
    return false;

  switch (target->data.primitive) {
  case PRIM_I8:
    return val >= SCHAR_MIN && val <= SCHAR_MAX;
  case PRIM_U8:
    return val >= 0 && val <= UCHAR_MAX;
  case PRIM_I16:
    return val >= SHRT_MIN && val <= SHRT_MAX;
  case PRIM_U16:
    return val >= 0 && val <= USHRT_MAX;
  case PRIM_I32:
    return val >= INT_MIN && val <= INT_MAX;
  case PRIM_U32:
    return val >= 0 && val <= UINT_MAX;
  case PRIM_I64:
    return val >= (double)LLONG_MIN && val <= (double)LLONG_MAX;
  case PRIM_U64:
    return val >= 0 && val <= (double)ULLONG_MAX;
  default:
    return false;
  }
}

void check_literal_bounds(Sema *s, AstNode *node, Type *target) {
  if (node->tag != NODE_NUMBER || !target || target->kind != KIND_PRIMITIVE)
    return;

  double val = node->number.value;
  switch (target->data.primitive) {
  case PRIM_I8:
    if (val < SCHAR_MIN || val > SCHAR_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i8", val);
    break;

  case PRIM_U8:
    if (val < 0 || val > UCHAR_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u8", val);
    break;

  case PRIM_I16:
    if (val < SHRT_MIN || val > SHRT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i16", val);
    break;

  case PRIM_U16:
    if (val < 0 || val > USHRT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u16", val);
    break;

  case PRIM_I32:
    if (val < INT_MIN || val > INT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i32", val);
    break;

  case PRIM_U32:
    if (val < 0 || val > UINT_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u32", val);
    break;

  case PRIM_I64:
    if (val < (double)LLONG_MIN || val > (double)LLONG_MAX)
      sema_warning(s, node, "Literal %.2f out of range for i64", val);
    break;

  case PRIM_U64:
    if (val < 0 || val > (double)ULLONG_MAX)
      sema_warning(s, node, "Literal %.2f out of range for u64", val);
    break;

  case PRIM_F32:
    if (fabs(val) > FLT_MAX && !isinf(val))
      sema_warning(s, node, "Literal %.2f out of range for f32", val);
    break;

  default:
    break;
  }
}

Type *check_literal(Sema *s, AstNode *node) {
  switch (node->tag) {
  case NODE_NUMBER:
    if (sv_contains(node->number.raw_text, '.') ||
        sv_ends_with(node->number.raw_text, "f") ||
        sv_ends_with(node->number.raw_text, "F")) {
      return type_get_primitive(s->types,
                                sv_ends_with(node->number.raw_text, "f") ||
                                        sv_ends_with(node->number.raw_text, "F")
                                    ? PRIM_F32
                                    : PRIM_F64);
    } else {
      return type_get_primitive(s->types, PRIM_I32);
    }

  case NODE_CHAR:
    return type_get_primitive(s->types, PRIM_CHAR);

  case NODE_STRING:
    return type_get_primitive(s->types, PRIM_STRING);

  case NODE_BOOL:
    return type_get_primitive(s->types, PRIM_BOOL);

  case NODE_NULL:
    return type_get_pointer(s->types,
                            type_get_primitive(s->types, PRIM_UNKNOWN));

  default:
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
}

Type *check_var(Sema *s, AstNode *node) {
  Symbol *sym = sema_resolve(s, node->var.value);
  if (!sym) {
    sema_error(s, node, "Undefined variable '" SV_FMT "'",
               SV_ARG(node->var.value));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
  return sym->type;
}
