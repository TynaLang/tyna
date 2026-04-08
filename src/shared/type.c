#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tyl/type.h"
#include "tyl/utils.h"

static void type_free_internal(Type *t) {
  if (!t)
    return;
  for (size_t i = 0; i < t->members.len; i++) {
    Member *m = t->members.items[i];
    free(m->name);
    free(m);
  }
  List_free(&t->members, 0);
  if (t->kind == KIND_STRUCT)
    free(t->data.structure.name);
  free(t);
}

// ---------- Lifecycle ---------- //

TypeContext *type_context_create() {
  TypeContext *ctx = malloc(sizeof(TypeContext));
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    ctx->primitives[i] = malloc(sizeof(Type));
    ctx->primitives[i]->kind = KIND_PRIMITIVE;
    ctx->primitives[i]->data.primitive = (PrimitiveKind)i;
    List_init(&ctx->primitives[i]->members);
  }

  type_add_builtin_member(ctx->primitives[PRIM_STRING], "len",
                          ctx->primitives[PRIM_I64], BUILTIN_ARRAY_LEN, 0);

  List_init(&ctx->array_types);
  List_init(&ctx->struct_types);
  return ctx;
}

void type_context_free(TypeContext *ctx) {
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    type_free_internal(ctx->primitives[i]);
  }
  for (size_t i = 0; i < ctx->array_types.len; i++) {
    type_free_internal(List_get(&ctx->array_types, i));
  }
  for (size_t i = 0; i < ctx->struct_types.len; i++) {
    type_free_internal(List_get(&ctx->struct_types, i));
  }
  List_free(&ctx->array_types, 0);
  List_free(&ctx->struct_types, 0);
  free(ctx);
}

// ---------- Type Retrieval ---------- //

Type *type_get_primitive(TypeContext *ctx, PrimitiveKind primitive) {
  if (primitive < 0 || primitive > PRIM_UNKNOWN)
    return ctx->primitives[PRIM_UNKNOWN];
  return ctx->primitives[primitive];
}

Type *type_get_array(TypeContext *ctx, Type *element, bool is_dynamic,
                     size_t fixed_size) {
  for (size_t i = 0; i < ctx->array_types.len; i++) {
    Type *t = List_get(&ctx->array_types, i);
    if (t->data.array.element == element &&
        t->data.array.is_dynamic == is_dynamic &&
        (is_dynamic || t->data.array.fixed_size == fixed_size)) {
      return t;
    }
  }

  Type *new_type = calloc(1, sizeof(Type));
  new_type->kind = KIND_ARRAY;
  new_type->data.array.element = element;
  new_type->data.array.is_dynamic = is_dynamic;
  new_type->data.array.fixed_size = fixed_size;
  List_init(&new_type->members);

  type_add_builtin_member(new_type, "len", ctx->primitives[PRIM_I64],
                          BUILTIN_ARRAY_LEN, 0);

  List_push(&ctx->array_types, new_type);
  return new_type;
}

Type *type_get_struct(TypeContext *ctx, const char *name) {
  for (size_t i = 0; i < ctx->struct_types.len; i++) {
    Type *t = List_get(&ctx->struct_types, i);
    if (strcmp(t->data.structure.name, name) == 0) {
      return t;
    }
  }

  Type *new_type = calloc(1, sizeof(Type));
  new_type->kind = KIND_STRUCT;
  new_type->data.structure.name = strdup(name);
  List_init(&new_type->members);

  List_push(&ctx->struct_types, new_type);
  return new_type;
}

// ---------- Member Management ---------- //

Member *type_get_member(Type *type, StringView name) {
  for (size_t i = 0; i < type->members.len; i++) {
    Member *m = type->members.items[i];
    if (sv_eq_cstr(name, m->name))
      return m;
  }
  return NULL;
}

void type_add_builtin_member(Type *type, const char *name, Type *member_type,
                             BuiltinMember builtin_id, size_t offset) {
  Member *m = calloc(1, sizeof(Member));
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

// ---------- Comparison & Properties ---------- //

bool type_equals(Type *a, Type *b) { return a == b; }

const char *type_to_name(Type *t) {
  if (!t)
    return "null";

  switch (t->kind) {
  case KIND_PRIMITIVE:
    switch (t->data.primitive) {
    case PRIM_I8:
      return "i8";
    case PRIM_I16:
      return "i16";
    case PRIM_I32:
      return "i32";
    case PRIM_I64:
      return "i64";
    case PRIM_U8:
      return "u8";
    case PRIM_U16:
      return "u16";
    case PRIM_U32:
      return "u32";
    case PRIM_U64:
      return "u64";
    case PRIM_F32:
      return "f32";
    case PRIM_F64:
      return "f64";
    case PRIM_CHAR:
      return "char";
    case PRIM_BOOL:
      return "bool";
    case PRIM_VOID:
      return "void";
    case PRIM_STRING:
      return "string";
    case PRIM_UNKNOWN:
      return "unknown";
    }
  case KIND_ARRAY: {
    static char buf[256];
    if (t->data.array.is_dynamic) {
      snprintf(buf, sizeof(buf), "[%s]", type_to_name(t->data.array.element));
    } else {
      snprintf(buf, sizeof(buf), "[%s; %zu]",
               type_to_name(t->data.array.element), t->data.array.fixed_size);
    }
    return buf;
  }
  case KIND_STRUCT:
    return t->data.structure.name;
  default:
    return "???";
  }
}

int type_rank(Type *t) {
  if (!t || t->kind != KIND_PRIMITIVE)
    return 0;
  switch (t->data.primitive) {
  case PRIM_I8:
    return 1;
  case PRIM_U8:
    return 2;
  case PRIM_I16:
    return 3;
  case PRIM_U16:
    return 4;
  case PRIM_I32:
    return 5;
  case PRIM_U32:
    return 6;
  case PRIM_I64:
    return 7;
  case PRIM_U64:
    return 8;
  case PRIM_F32:
    return 9;
  case PRIM_F64:
    return 10;
  case PRIM_CHAR:
    return 1; // Treat char like i8/u8 for rank
  default:
    return 0;
  }
}

// ----------- Type Checking ----------- //

bool type_is_numeric(Type *t) { return t && type_rank(t) > 0; }

bool type_is_bool(Type *t) {
  return t && t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_BOOL;
}

bool type_is_unknown(Type *t) {
  return t && t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_UNKNOWN;
}

bool type_is_reference(Type *t) {
  if (!t)
    return false;
  return t->kind == KIND_ARRAY || t->kind == KIND_STRUCT ||
         (t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_STRING);
}

// ----------- Semantic Rules ----------- //

int type_can_implicitly_cast(Type *to, Type *from) {
  if (!to || !from)
    return 0;
  if (to == from)
    return 1;

  if (to->kind == KIND_PRIMITIVE &&
      (to->data.primitive == PRIM_VOID || to->data.primitive == PRIM_UNKNOWN))
    return 0;

  if (type_is_numeric(to) && type_is_numeric(from)) {
    PrimitiveKind p_to = to->data.primitive;
    PrimitiveKind p_from = from->data.primitive;

    // Prevent cross-casting between signed and unsigned of same/smaller size
    // e.g., i32 to u32 is blocked. u32 to i32 is blocked.
    bool from_signed = (p_from == PRIM_I8 || p_from == PRIM_I16 ||
                        p_from == PRIM_I32 || p_from == PRIM_I64);
    bool to_signed = (p_to == PRIM_I8 || p_to == PRIM_I16 || p_to == PRIM_I32 ||
                      p_to == PRIM_I64);

    if (from_signed != to_signed) {
      // Only allow unsigned -> signed if the signed type is strictly larger
      // e.g., u32 (max ~4bil) fits in i64 (max ~9quintillion)
      return type_rank(to) > type_rank(from);
    }

    return type_rank(to) >= type_rank(from);
  }

  if (from->kind == KIND_PRIMITIVE && from->data.primitive == PRIM_STRING &&
      to->kind == KIND_ARRAY &&
      to->data.array.element->kind == KIND_PRIMITIVE &&
      to->data.array.element->data.primitive == PRIM_CHAR) {
    return 1;
  }

  return 0;
}