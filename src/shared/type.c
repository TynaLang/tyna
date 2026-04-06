#include <stdlib.h>
#include <string.h>

#include "tyl/type.h"
#include "tyl/utils.h"

TypeContext *type_context_create() {
  TypeContext *ctx = malloc(sizeof(TypeContext));
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    ctx->primitives[i] = malloc(sizeof(Type));
    ctx->primitives[i]->kind = KIND_PRIMITIVE;
    ctx->primitives[i]->data.primitive = (PrimitiveKind)i;
  }
  List_init(&ctx->array_types);
  return ctx;
}

void type_context_free(TypeContext *ctx) {
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    free(ctx->primitives[i]);
  }
  // TODO: iterate and free array types if List stores pointers that need
  // freeing For now assuming a simple list of Type*
  for (size_t i = 0; i < ctx->array_types.len; i++) {
    free(List_get(&ctx->array_types, i));
  }
  List_free(&ctx->array_types, 0);
  free(ctx);
}

Type *type_get_primitive(TypeContext *ctx, PrimitiveKind primitive) {
  if (primitive > PRIM_UNKNOWN)
    return ctx->primitives[PRIM_UNKNOWN];
  return ctx->primitives[primitive];
}

Type *type_get_array(TypeContext *ctx, Type *element, bool is_dynamic,
                     size_t fixed_size) {
  for (size_t i = 0; i < ctx->array_types.len; i++) {
    Type *t = List_get(&ctx->array_types, i);
    if (t->kind == KIND_ARRAY && t->data.array.element == element &&
        t->data.array.is_dynamic == is_dynamic &&
        (is_dynamic || t->data.array.fixed_size == fixed_size)) {
      return t;
    }
  }

  Type *new_type = malloc(sizeof(Type));
  new_type->kind = KIND_ARRAY;
  new_type->data.array.element = element;
  new_type->data.array.is_dynamic = is_dynamic;
  new_type->data.array.fixed_size = fixed_size;
  List_push(&ctx->array_types, new_type);
  return new_type;
}

bool type_equals(Type *a, Type *b) { return a == b; }
