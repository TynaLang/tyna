#include "tyl/type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void type_free_internal(Type *t) {
  if (!t)
    return;
  for (size_t i = 0; i < t->members.len; i++) {
    Member *m = t->members.items[i];
    free(m->name);
    free(m);
  }
  List_free(&t->members, 0);
  // Note: name is usually a StringView pointing to source or interned memory
  free(t);
}

size_t type_get_primitive_size(PrimitiveKind prim) {
  switch (prim) {
  case PRIM_I8:
  case PRIM_U8:
  case PRIM_CHAR:
    return 1;
  case PRIM_I16:
  case PRIM_U16:
    return 2;
  case PRIM_I32:
  case PRIM_U32:
  case PRIM_F32:
    return 4;
  case PRIM_I64:
  case PRIM_U64:
  case PRIM_F64:
    return 8;
  case PRIM_BOOL:
    return 1;
  default:
    return 0;
  }
}

size_t align_to(size_t value, size_t align) {
  if (align == 0)
    return value;
  return (value + align - 1) & ~(align - 1);
}

static void register_array_template(TypeContext *ctx) {
  Type *array_tmpl = xcalloc(1, sizeof(Type));
  array_tmpl->kind = KIND_TEMPLATE;
  array_tmpl->name = sv_from_parts("Array", 5);

  List_init(&array_tmpl->data.template.placeholders);
  List_init(&array_tmpl->data.template.fields);
  List_init(&array_tmpl->members);

  // 1. Add Placeholders (The <T>)
  StringView *t_sv = xmalloc(sizeof(StringView));
  *t_sv = sv_from_parts("T", 1);
  List_push(&array_tmpl->data.template.placeholders, t_sv);

  // 2. Add "Ghost" Members (Unresolved Types)
  // We'll use a specific KIND_TEMPLATE type for 'T' as well
  Type *t_type = xcalloc(1, sizeof(Type));
  t_type->kind = KIND_TEMPLATE;
  t_type->name = *t_sv;

  // data: ptr<T>
  Type *ptr_t = xcalloc(1, sizeof(Type));
  ptr_t->kind = KIND_POINTER;
  ptr_t->data.pointer_to = t_type;
  ptr_t->size = 8;
  type_add_member(array_tmpl, "data", ptr_t, 0);

  // len: i64
  type_add_member(array_tmpl, "len", type_get_primitive(ctx, PRIM_I64), 8);

  // cap: i64
  type_add_member(array_tmpl, "cap", type_get_primitive(ctx, PRIM_I64), 16);

  // rank: i64
  type_add_member(array_tmpl, "rank", type_get_primitive(ctx, PRIM_I64), 24);

  // dims: ptr<i64>
  type_add_member(array_tmpl, "dims",
                  type_get_pointer(ctx, type_get_primitive(ctx, PRIM_I64)), 32);

  array_tmpl->size = 40;

  List_push(&ctx->templates, array_tmpl);
}

TypeContext *type_context_create() {
  TypeContext *ctx = xmalloc(sizeof(TypeContext));
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    ctx->primitives[i] = xcalloc(1, sizeof(Type));
    ctx->primitives[i]->kind = KIND_PRIMITIVE;
    ctx->primitives[i]->data.primitive = (PrimitiveKind)i;
    ctx->primitives[i]->size = type_get_primitive_size(i);
    ctx->primitives[i]->alignment = ctx->primitives[i]->size;
    if (i == PRIM_STRING) {
      ctx->primitives[i]->alignment = 8;
    }
    List_init(&ctx->primitives[i]->members);
    List_init(&ctx->primitives[i]->methods);
    ctx->primitives[i]->is_frozen = false;
    ctx->primitives[i]->is_intrinsic = false;
  }
  List_init(&ctx->structs);
  List_init(&ctx->templates);
  List_init(&ctx->instances);

  register_array_template(ctx);

  return ctx;
}

void type_context_free(TypeContext *ctx) {
  for (int i = 0; i <= PRIM_UNKNOWN; i++)
    type_free_internal(ctx->primitives[i]);
  for (size_t i = 0; i < ctx->templates.len; i++)
    type_free_internal(ctx->templates.items[i]);
  for (size_t i = 0; i < ctx->instances.len; i++)
    type_free_internal(ctx->instances.items[i]);
  List_free(&ctx->templates, 0);
  List_free(&ctx->instances, 0);
  free(ctx);
}

Type *type_get_primitive(TypeContext *ctx, PrimitiveKind primitive) {
  return ctx->primitives[primitive];
}

Type *type_get_pointer(TypeContext *ctx, Type *to) {
  // Interning: Check if ptr<to> already exists
  for (size_t i = 0; i < ctx->instances.len; i++) {
    Type *t = ctx->instances.items[i];
    if (t->kind == KIND_POINTER && t->data.pointer_to == to)
      return t;
  }

  Type *new_ptr = xcalloc(1, sizeof(Type));
  new_ptr->kind = KIND_POINTER;
  new_ptr->data.pointer_to = to;
  new_ptr->size = 8; // Assuming 64-bit
  new_ptr->alignment = 8;
  List_push(&ctx->instances, new_ptr);
  return new_ptr;
}

Type *type_get_struct(TypeContext *ctx, StringView name) {
  for (size_t i = 0; i < ctx->structs.len; i++) {
    Type *t = ctx->structs.items[i];
    if (sv_eq(t->name, name))
      return t;
  }

  Type *s = xcalloc(1, sizeof(Type));
  s->kind = KIND_STRUCT;
  s->name = name;
  List_init(&s->members);
  List_push(&ctx->structs, s);
  return s;
}

Type *type_get_template(TypeContext *ctx, StringView name) {
  for (size_t i = 0; i < ctx->templates.len; i++) {
    Type *t = ctx->templates.items[i];
    if (t->kind == KIND_TEMPLATE && sv_eq(t->name, name))
      return t;
  }
  return NULL;
}

static Type *resolve_placeholder(TypeContext *ctx, Type *blueprint,
                                 List placeholders, List args) {
  if (!blueprint)
    return NULL;

  // 1. Base Case: If the blueprint IS a placeholder (e.g., "T")
  if (blueprint->kind == KIND_TEMPLATE) {
    for (size_t i = 0; i < placeholders.len; i++) {
      StringView p_name = *(StringView *)placeholders.items[i];
      if (sv_eq(blueprint->name, p_name)) {
        return args.items[i]; // Return the concrete type (e.g., i32)
      }
    }
  }

  // 2. Recursive Case: If it's a pointer to a placeholder (e.g., ptr<T>)
  if (blueprint->kind == KIND_POINTER) {
    Type *inner = resolve_placeholder(ctx, blueprint->data.pointer_to,
                                      placeholders, args);
    if (inner != blueprint->data.pointer_to) {
      return type_get_pointer(ctx, inner);
    }
  }

  // 3. Fallback: It's already a concrete type (e.g., i64)
  return blueprint;
}

// Monomorphization engine
Type *type_get_instance(TypeContext *ctx, Type *template_type, List args) {
  return type_get_instance_fixed(ctx, template_type, args, 0);
}

Type *type_get_instance_fixed(TypeContext *ctx, Type *template_type, List args,
                              uint64_t fixed_array_len) {
  // Check if instance already exists
  for (size_t i = 0; i < ctx->instances.len; i++) {
    Type *inst = ctx->instances.items[i];
    if (inst->kind == KIND_STRUCT &&
        inst->data.instance.from_template == template_type &&
        inst->fixed_array_len == fixed_array_len) {
      if (inst->data.instance.generic_args.len == args.len) {
        bool match = true;
        for (size_t j = 0; j < args.len; j++) {
          if (!type_equals(inst->data.instance.generic_args.items[j],
                           args.items[j])) {
            match = false;
            break;
          }
        }
        if (match)
          return inst;
      }
    }
  }

  Type *inst = xcalloc(1, sizeof(Type));
  inst->kind = KIND_STRUCT;
  inst->name = template_type->name; // In a real impl, concat names
  inst->fixed_array_len = fixed_array_len;
  inst->data.instance.from_template = template_type;
  List_init(&inst->data.instance.generic_args);
  for (size_t i = 0; i < args.len; i++) {
    List_push(&inst->data.instance.generic_args, args.items[i]);
  }
  List_init(&inst->members);

  // Clone members and swap placeholders
  for (size_t i = 0; i < template_type->members.len; i++) {
    Member *m = template_type->members.items[i];

    Type *m_type = resolve_placeholder(
        ctx, m->type, template_type->data.template.placeholders, args);

    type_add_member(inst, m->name, m_type, m->offset);
  }

  // Calculate size and alignment based on resolved members
  size_t current_offset = 0;
  size_t max_align = 1;
  for (size_t i = 0; i < inst->members.len; i++) {
    Member *m = inst->members.items[i];
    size_t align = m->type->alignment ? m->type->alignment : m->type->size;
    if (align == 0)
      align = 1;

    current_offset = align_to(current_offset, align);
    m->offset = current_offset;
    current_offset += m->type->size;

    if (align > max_align)
      max_align = align;
  }
  inst->size = align_to(current_offset, max_align);
  inst->alignment = max_align;

  List_push(&ctx->instances, inst);
  return inst;
}

void type_add_member(Type *type, const char *name, Type *member_type,
                     size_t offset) {
  Member *m = xcalloc(1, sizeof(Member));
  m->name = xstrdup(name);
  m->type = member_type;
  m->offset = offset;
  m->index = (unsigned)type->members.len;
  List_push(&type->members, m);
}

Member *type_get_member(Type *type, StringView name) {
  if (!type)
    return NULL;

  for (size_t i = 0; i < type->members.len; i++) {
    Member *m = type->members.items[i];
    if (sv_eq(sv_from_cstr(m->name), name))
      return m;
  }
  return NULL;
}

bool type_equals(Type *a, Type *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->kind != b->kind)
    return false;

  switch (a->kind) {
  case KIND_POINTER:
    return type_equals(a->data.pointer_to, b->data.pointer_to);
  case KIND_PRIMITIVE:
    return a->data.primitive == b->data.primitive;
  case KIND_STRUCT:
    if (!sv_eq(a->name, b->name))
      return false;
    if (a->data.instance.from_template != b->data.instance.from_template)
      return false;
    if (a->fixed_array_len != b->fixed_array_len)
      return false;
    return true;
  default:
    return false;
  }
}

const char *type_to_name(Type *t) {
  if (!t)
    return "unknown";

  static int depth = 0;

  if (depth > 10)
    return "...";

  depth++;
  switch (t->kind) {
  case KIND_PRIMITIVE:
    depth--;
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
    default:
      return "primitive";
    }
  case KIND_POINTER: {
    static char pointer_buf[1024];
    snprintf(pointer_buf, sizeof(pointer_buf), "ptr<%s>",
             type_to_name(t->data.pointer_to));
    depth--;
    return pointer_buf;
  }
  case KIND_STRUCT: {
    static char struct_buf[1024];
    if (t->data.instance.from_template) {
      char args_buf[512] = {0};
      strcat(args_buf, "<");
      for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
        strcat(args_buf, type_to_name(t->data.instance.generic_args.items[i]));
        if (i < t->data.instance.generic_args.len - 1)
          strcat(args_buf, ", ");
      }
      if (t->fixed_array_len > 0) {
        char len_buf[32];
        snprintf(len_buf, sizeof(len_buf), ";%llu",
                 (unsigned long long)t->fixed_array_len);
        strcat(args_buf, len_buf);
      }
      strcat(args_buf, ">");
      snprintf(struct_buf, sizeof(struct_buf), SV_FMT "%s", SV_ARG(t->name),
               args_buf);
    } else {
      snprintf(struct_buf, sizeof(struct_buf), SV_FMT, SV_ARG(t->name));
    }
    depth--;
    return struct_buf;
  }
  default:
    depth--;
    return "complex";
  }
}

int type_rank(Type *t) {
  if (!t || t->kind != KIND_PRIMITIVE)
    return 0;
  return (int)t->data.primitive + 1;
}

bool type_is_numeric(Type *t) {
  if (!t || t->kind != KIND_PRIMITIVE)
    return false;
  return t->data.primitive <= PRIM_F64;
}

bool type_is_bool(Type *t) {
  return t && t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_BOOL;
}

bool type_is_unknown(Type *t) {
  return t && t->kind == KIND_PRIMITIVE && t->data.primitive == PRIM_UNKNOWN;
}

int type_can_implicitly_cast(Type *to, Type *from) {
  if (type_equals(to, from))
    return 1;

  // Array (Struct with 'data' field) to Pointer Decay
  if (to->kind == KIND_POINTER && from->kind == KIND_STRUCT) {
    Member *data_field = type_get_member(from, sv_from_parts("data", 4));
    if (data_field && type_equals(to, data_field->type)) {
      return 1;
    }
  }

  if (type_is_numeric(to) && type_is_numeric(from)) {
    return to->size >= from->size;
  }
  // Pointer to void* is usually allowed
  if (to->kind == KIND_POINTER && to->data.pointer_to->kind == KIND_PRIMITIVE &&
      to->data.pointer_to->data.primitive == PRIM_VOID)
    return 1;

  return 0;
}