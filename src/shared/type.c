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
    List_init(&ctx->primitives[i]->members);
  }

  type_add_builtin_member(ctx->primitives[PRIM_STRING], "len",
                          type_get_primitive(ctx, PRIM_I64), BUILTIN_ARRAY_LEN,
                          0);

  List_init(&ctx->array_types);
  return ctx;
}

static void type_free_members(Type *t) {
  for (size_t i = 0; i < t->members.len; i++) {
    Member *m = t->members.items[i];
    free(m->name);
    free(m);
  }
  List_free(&t->members, 0);
}

void type_context_free(TypeContext *ctx) {
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    type_free_members(ctx->primitives[i]);
    free(ctx->primitives[i]);
  }

  for (size_t i = 0; i < ctx->array_types.len; i++) {
    Type *t = List_get(&ctx->array_types, i);
    type_free_members(t);
    free(t);
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
  List_init(&new_type->members);

  // Add .len member for arrays
  type_add_builtin_member(new_type, "len", type_get_primitive(ctx, PRIM_I64),
                          BUILTIN_ARRAY_LEN, 0);

  List_push(&ctx->array_types, new_type);
  return new_type;
}

Member *type_get_member(Type *type, StringView name) {
  for (size_t i = 0; i < type->members.len; i++) {
    Member *m = type->members.items[i];
    if (sv_eq_cstr(name, m->name)) {
      return m;
    }
  }
  return NULL;
}

void type_add_builtin_member(Type *type, const char *name, Type *member_type,
                             BuiltinMember builtin_id, size_t offset) {
  Member *m = malloc(sizeof(Member));
  m->name = strdup(name);
  m->type = member_type;
  m->builtin_id = builtin_id;
  m->offset = offset;
  List_push(&type->members, m);
}

void type_add_member(Type *type, const char *name, Type *member_type,
                     size_t offset) {
  type_add_builtin_member(type, name, member_type, BUILTIN_NONE, offset);
}

Type *type_get_struct(TypeContext *ctx, const char *name) {
  Type *new_type = malloc(sizeof(Type));
  new_type->kind = KIND_STRUCT;
  new_type->data.structure.name = strdup(name);
  List_init(&new_type->members);

  // We should ideally intern these in the TypeContext as well
  // For now, simplicity.
  return new_type;
}

bool type_equals(Type *a, Type *b) { return a == b; }
