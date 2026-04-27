#include "tyna/type.h"
#include "tyna/ast.h"
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
  List_free(&t->field_drops, 0);
  List_free(&t->methods, 0);
  List_free(&t->impls, 0);
  // Note: name is usually a StringView pointing to source or interned memory
  free(t);
}

static void type_init_common(Type *t) {
  t->needs_drop = false;
  t->drop_fn = NULL;
  List_init(&t->members);
  List_init(&t->field_drops);
  List_init(&t->methods);
  List_init(&t->impls);
}

static bool type_needs_drop_recursive(Type *type, List *stack);

static bool type_member_needs_drop(Member *member, List *stack) {
  if (!member || !member->type)
    return false;
  return type_needs_drop_recursive(member->type, stack);
}

static bool type_needs_drop_recursive(Type *type, List *stack) {
  if (!type)
    return false;

  for (size_t i = 0; i < stack->len; i++) {
    if (stack->items[i] == type)
      return type->needs_drop;
  }

  List_push(stack, type);

  bool needs_drop = false;
  switch (type->kind) {
  case KIND_STRING_BUFFER:
    needs_drop = true;
    break;

  case KIND_STRUCT:
  case KIND_UNION:
    for (size_t i = 0; i < type->members.len; i++) {
      Member *member = type->members.items[i];
      if (type_member_needs_drop(member, stack)) {
        needs_drop = true;
        break;
      }
    }
    break;

  default:
    needs_drop = type->needs_drop;
    break;
  }

  List_pop(stack);
  type->needs_drop = needs_drop;
  return needs_drop;
}

static void type_refresh_single(Type *type) {
  if (!type)
    return;

  List_free(&type->field_drops, 0);
  List_init(&type->field_drops);

  List stack;
  List_init(&stack);
  bool needs_drop = type_needs_drop_recursive(type, &stack);
  List_free(&stack, 0);

  if (needs_drop && type->kind == KIND_STRUCT) {
    for (size_t i = 0; i < type->members.len; i++) {
      Member *member = type->members.items[i];
      List member_stack;
      List_init(&member_stack);
      if (member && member->type &&
          type_needs_drop_recursive(member->type, &member_stack)) {
        List_push(&type->field_drops, member);
      }
      List_free(&member_stack, 0);
    }
  }
}

void type_refresh_drop_metadata(TypeContext *ctx) {
  if (!ctx)
    return;

  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    type_refresh_single(ctx->primitives[i]);
  }
  type_refresh_single(ctx->string_buffer);
  for (size_t i = 0; i < ctx->templates.len; i++) {
    type_refresh_single(ctx->templates.items[i]);
  }
  for (size_t i = 0; i < ctx->instances.len; i++) {
    type_refresh_single(ctx->instances.items[i]);
  }
  for (size_t i = 0; i < ctx->structs.len; i++) {
    type_refresh_single(ctx->structs.items[i]);
  }
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
  case PRIM_STRING:
    return 16;
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

  type_init_common(array_tmpl);
  List_init(&array_tmpl->data.template.placeholders);
  List_init(&array_tmpl->data.template.fields);

  // 1. Add Placeholders (The <T>)
  StringView *t_sv = xmalloc(sizeof(StringView));
  *t_sv = sv_from_parts("T", 1);
  List_push(&array_tmpl->data.template.placeholders, t_sv);

  // 2. Add "Ghost" Members (Unresolved Types)
  // We'll use a specific KIND_TEMPLATE type for 'T' as well
  Type *t_type = xcalloc(1, sizeof(Type));
  t_type->kind = KIND_TEMPLATE;
  t_type->name = *t_sv;
  type_init_common(t_type);

  // data: ptr<T>
  Type *ptr_t = xcalloc(1, sizeof(Type));
  ptr_t->kind = KIND_POINTER;
  ptr_t->data.pointer_to = t_type;
  ptr_t->size = 8;
  type_init_common(ptr_t);
  type_add_member(array_tmpl, "data", ptr_t, 0);

  // len: i64
  type_add_member(array_tmpl, "len", type_get_primitive(ctx, PRIM_I64), 8);

  // cap: i64
  type_add_member(array_tmpl, "cap", type_get_primitive(ctx, PRIM_I64), 16);

  array_tmpl->size = 24;

  List_push(&ctx->templates, array_tmpl);
}

static void register_slice_template(TypeContext *ctx) {
  Type *slice_tmpl = xcalloc(1, sizeof(Type));
  slice_tmpl->kind = KIND_TEMPLATE;
  slice_tmpl->name = sv_from_parts("Slice", 5);

  type_init_common(slice_tmpl);
  List_init(&slice_tmpl->data.template.placeholders);
  List_init(&slice_tmpl->data.template.fields);

  StringView *t_sv = xmalloc(sizeof(StringView));
  *t_sv = sv_from_parts("T", 1);
  List_push(&slice_tmpl->data.template.placeholders, t_sv);

  Type *t_type = xcalloc(1, sizeof(Type));
  t_type->kind = KIND_TEMPLATE;
  t_type->name = *t_sv;
  type_init_common(t_type);

  Type *ptr_t = xcalloc(1, sizeof(Type));
  ptr_t->kind = KIND_POINTER;
  ptr_t->data.pointer_to = t_type;
  ptr_t->size = 8;
  type_init_common(ptr_t);
  type_add_member(slice_tmpl, "data", ptr_t, 0);
  type_add_member(slice_tmpl, "len", type_get_primitive(ctx, PRIM_I64), 8);

  slice_tmpl->size = 16;

  List_push(&ctx->templates, slice_tmpl);
}

Type *type_get_string_buffer(TypeContext *ctx) { return ctx->string_buffer; }

TypeContext *type_context_create() {
  TypeContext *ctx = xmalloc(sizeof(TypeContext));
  ctx->string_buffer = NULL;
  for (int i = 0; i <= PRIM_UNKNOWN; i++) {
    ctx->primitives[i] = xcalloc(1, sizeof(Type));
    ctx->primitives[i]->kind = KIND_PRIMITIVE;
    ctx->primitives[i]->data.primitive = (PrimitiveKind)i;
    ctx->primitives[i]->size = type_get_primitive_size(i);
    ctx->primitives[i]->alignment = ctx->primitives[i]->size;
    if (i == PRIM_STRING) {
      ctx->primitives[i]->alignment = 8;
    }
    type_init_common(ctx->primitives[i]);
    ctx->primitives[i]->is_frozen = false;
    ctx->primitives[i]->is_intrinsic = false;
  }
  List_init(&ctx->structs);
  List_init(&ctx->templates);
  List_init(&ctx->instances);
  List_init(&ctx->instantiated_functions);

  register_array_template(ctx);
  register_slice_template(ctx);

  Type *sb = xcalloc(1, sizeof(Type));
  sb->kind = KIND_STRING_BUFFER;
  sb->name = sv_from_cstr("String");
  sb->size = 24;
  sb->alignment = 8;
  type_init_common(sb);
  sb->is_frozen = true;
  sb->is_intrinsic = true;
  sb->needs_drop = true;
  sb->drop_fn = "__tyna_drop_String";
  ctx->string_buffer = sb;

  return ctx;
}

void type_context_free(TypeContext *ctx) {
  for (int i = 0; i <= PRIM_UNKNOWN; i++)
    type_free_internal(ctx->primitives[i]);
  type_free_internal(ctx->string_buffer);
  for (size_t i = 0; i < ctx->templates.len; i++)
    type_free_internal(ctx->templates.items[i]);
  for (size_t i = 0; i < ctx->instances.len; i++)
    type_free_internal(ctx->instances.items[i]);
  for (size_t i = 0; i < ctx->instantiated_functions.len; i++)
    Ast_free(ctx->instantiated_functions.items[i]);
  List_free(&ctx->templates, 0);
  List_free(&ctx->instances, 0);
  List_free(&ctx->instantiated_functions, 0);
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
  type_init_common(new_ptr);
  List_push(&ctx->instances, new_ptr);
  return new_ptr;
}

Type *type_get_named(TypeContext *ctx, StringView name) {
  for (size_t i = 0; i < ctx->structs.len; i++) {
    Type *t = ctx->structs.items[i];
    if (sv_eq(t->name, name))
      return t;
  }
  for (size_t i = 0; i < ctx->templates.len; i++) {
    Type *t = ctx->templates.items[i];
    if (sv_eq(t->name, name))
      return t;
  }
  return NULL;
}

Type *type_get_struct(TypeContext *ctx, StringView name) {
  Type *existing = type_get_named(ctx, name);
  if (existing)
    return existing;

  Type *s = xcalloc(1, sizeof(Type));
  s->kind = KIND_STRUCT;
  s->name = name;
  type_init_common(s);
  s->alignment = 0;
  s->size = 0;
  s->is_frozen = false;
  s->is_intrinsic = false;
  List_push(&ctx->structs, s);
  return s;
}

Type *type_get_union(TypeContext *ctx, StringView name) {
  Type *existing = type_get_named(ctx, name);
  if (existing) {
    if (existing->kind == KIND_UNION)
      return existing;
    if (existing->kind == KIND_STRUCT && existing->members.len == 0 &&
        !existing->is_intrinsic) {
      existing->kind = KIND_UNION;
      return existing;
    }
    return existing;
  }

  Type *u = xcalloc(1, sizeof(Type));
  u->kind = KIND_UNION;
  u->name = name;
  type_init_common(u);
  u->alignment = 0;
  u->size = 0;
  u->is_frozen = false;
  u->is_intrinsic = false;
  List_push(&ctx->structs, u);
  return u;
}

Type *type_get_error(TypeContext *ctx, StringView name) {
  Type *existing = type_get_named(ctx, name);
  if (existing) {
    if (existing->kind == KIND_ERROR)
      return existing;
    if (existing->kind == KIND_STRUCT && existing->members.len == 0 &&
        !existing->is_intrinsic) {
      existing->kind = KIND_ERROR;
      return existing;
    }
    return existing;
  }

  Type *err = xcalloc(1, sizeof(Type));
  err->kind = KIND_ERROR;
  err->name = name;
  type_init_common(err);
  err->alignment = 0;
  err->size = 0;
  err->is_frozen = false;
  err->is_intrinsic = false;
  List_push(&ctx->structs, err);
  return err;
}

Type *type_get_error_set(TypeContext *ctx, StringView name) {
  Type *existing = type_get_named(ctx, name);
  if (existing) {
    if (existing->kind == KIND_ERROR_SET)
      return existing;
    if (existing->kind == KIND_STRUCT && existing->members.len == 0 &&
        !existing->is_intrinsic) {
      existing->kind = KIND_ERROR_SET;
      return existing;
    }
    return existing;
  }

  Type *set = xcalloc(1, sizeof(Type));
  set->kind = KIND_ERROR_SET;
  set->name = name;
  type_init_common(set);
  set->alignment = 0;
  set->size = 0;
  set->is_frozen = false;
  set->is_intrinsic = false;
  List_push(&ctx->structs, set);
  return set;
}

Type *type_get_error_set_anonymous(TypeContext *ctx) {
  Type *set = xcalloc(1, sizeof(Type));
  set->kind = KIND_ERROR_SET;
  set->name = sv_from_parts("", 0);
  type_init_common(set);
  set->alignment = 0;
  set->size = 0;
  set->is_frozen = false;
  set->is_intrinsic = false;
  List_push(&ctx->instances, set);
  return set;
}

Type *type_get_result(TypeContext *ctx, Type *success, Type *error_set) {
  for (size_t i = 0; i < ctx->instances.len; i++) {
    Type *t = ctx->instances.items[i];
    if (t->kind == KIND_RESULT && t->data.result.success == success &&
        t->data.result.error_set == error_set) {
      return t;
    }
  }

  Type *res = xcalloc(1, sizeof(Type));
  res->kind = KIND_RESULT;
  res->data.result.success = success;
  res->data.result.error_set = error_set;
  res->alignment = 0;
  res->size = 0;
  type_init_common(res);
  res->is_frozen = true;
  res->is_intrinsic = false;
  List_push(&ctx->instances, res);
  return res;
}

Type *type_get_union_anonymous(TypeContext *ctx, List types) {
  // Deduplicate identical anonymous unions if possible.
  for (size_t i = 0; i < ctx->instances.len; i++) {
    Type *t = ctx->instances.items[i];
    if (t->kind != KIND_UNION)
      continue;
    if (t->members.len != types.len)
      continue;
    bool match = true;
    for (size_t j = 0; j < types.len; j++) {
      Member *m = t->members.items[j];
      if (!type_equals(m->type, types.items[j])) {
        match = false;
        break;
      }
    }
    if (match)
      return t;
  }

  Type *u = xcalloc(1, sizeof(Type));
  u->kind = KIND_UNION;
  u->name = sv_from_parts("<anonymous union>", 16);
  type_init_common(u);

  size_t max_size = 0;
  size_t max_align = 1;
  for (size_t i = 0; i < types.len; i++) {
    Type *member_type = types.items[i];
    type_add_member(u, NULL, member_type, 0);
    if (member_type->size > max_size)
      max_size = member_type->size;
    size_t align =
        member_type->alignment ? member_type->alignment : member_type->size;
    if (align == 0)
      align = 1;
    if (align > max_align)
      max_align = align;
  }

  u->alignment = max_align;
  u->size = align_to(max_size, max_align);
  u->is_tagged_union = true;
  List_push(&ctx->instances, u);
  return u;
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

  // 3. Recursive Case: If it's a generic instance type (e.g. Array<T> or
  // Array<Array<T>>)
  if (blueprint->kind == KIND_STRUCT &&
      blueprint->data.instance.from_template) {
    bool changed = false;
    List resolved_args;
    List_init(&resolved_args);
    for (size_t i = 0; i < blueprint->data.instance.generic_args.len; i++) {
      Type *resolved = resolve_placeholder(
          ctx, blueprint->data.instance.generic_args.items[i], placeholders,
          args);
      List_push(&resolved_args, resolved);
      if (resolved != blueprint->data.instance.generic_args.items[i])
        changed = true;
    }
    if (changed) {
      Type *result = type_get_instance(
          ctx, blueprint->data.instance.from_template, resolved_args);
      List_free(&resolved_args, 0);
      return result;
    }
    List_free(&resolved_args, 0);
  }

  // 4. Fallback: It's already a concrete type (e.g., i64)
  return blueprint;
}

Type *type_resolve_placeholders(TypeContext *ctx, Type *blueprint,
                                Type *template_type, List args) {
  if (!template_type || template_type->kind != KIND_TEMPLATE ||
      !template_type->data.template.placeholders.len)
    return blueprint;
  return resolve_placeholder(ctx, blueprint,
                             template_type->data.template.placeholders, args);
}

// Monomorphization engine
Type *type_get_instance(TypeContext *ctx, Type *template_type, List args) {
  return type_get_instance_fixed(ctx, template_type, args, 0);
}

Type *type_get_array(TypeContext *ctx, Type *element_type,
                     uint64_t fixed_array_len) {
  Type *array_template = type_get_template(ctx, sv_from_parts("Array", 5));
  if (!array_template)
    return NULL;
  List args;
  List_init(&args);
  List_push(&args, element_type);
  Type *result =
      type_get_instance_fixed(ctx, array_template, args, fixed_array_len);
  List_free(&args, 0);
  return result;
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
  type_init_common(inst);

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
  if (name)
    m->name = xstrdup(name);
  else
    m->name = NULL;
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
    if (!m->name)
      continue;
    if (sv_eq(sv_from_cstr(m->name), name))
      return m;
  }
  return NULL;
}

Member *type_find_union_field(Type *type, StringView name, Type **out_owner) {
  if (!type || type->kind != KIND_UNION)
    return NULL;

  // First, try direct named union members.
  for (size_t i = 0; i < type->members.len; i++) {
    Member *m = type->members.items[i];
    if (m->name && sv_eq(sv_from_cstr(m->name), name)) {
      if (out_owner)
        *out_owner = type;
      return m;
    }
  }

  // Then search each variant's fields.
  for (size_t i = 0; i < type->members.len; i++) {
    Member *variant = type->members.items[i];
    Type *variant_type = variant->type;
    if (!variant_type)
      continue;
    if (variant_type->kind == KIND_STRUCT || variant_type->kind == KIND_UNION) {
      Member *inner = type_get_member(variant_type, name);
      if (inner) {
        if (out_owner)
          *out_owner = variant_type;
        return inner;
      }
    }
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
  case KIND_UNION:
  case KIND_ERROR:
    if (!sv_eq(a->name, b->name))
      return false;
    if (a->data.instance.from_template != b->data.instance.from_template)
      return false;
    if (a->fixed_array_len != b->fixed_array_len)
      return false;
    return true;
  case KIND_ERROR_SET:
    if (a->name.len == 0 || b->name.len == 0)
      return a == b;
    if (!sv_eq(a->name, b->name))
      return false;
    if (a->data.instance.from_template != b->data.instance.from_template)
      return false;
    if (a->fixed_array_len != b->fixed_array_len)
      return false;
    return true;
  case KIND_RESULT:
    return type_equals(a->data.result.success, b->data.result.success) &&
           type_equals(a->data.result.error_set, b->data.result.error_set);
  case KIND_STRING_BUFFER:
    return a == b;
  case KIND_TEMPLATE:
    if (!sv_eq(a->name, b->name))
      return false;
    if (a->data.template.placeholders.len != b->data.template.placeholders.len)
      return false;
    for (size_t i = 0; i < a->data.template.placeholders.len; i++) {
      StringView a_name = *(StringView *)a->data.template.placeholders.items[i];
      StringView b_name = *(StringView *)b->data.template.placeholders.items[i];
      if (!sv_eq(a_name, b_name))
        return false;
    }
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
      return "str";
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
  case KIND_ERROR: {
    static char error_buf[1024];
    snprintf(error_buf, sizeof(error_buf), "error %.*s", (int)t->name.len,
             t->name.data);
    depth--;
    return error_buf;
  }
  case KIND_ERROR_SET: {
    static char set_buf[1024];
    if (t->name.len > 0) {
      snprintf(set_buf, sizeof(set_buf), "errors %.*s", (int)t->name.len,
               t->name.data);
    } else if (t->members.len == 0) {
      snprintf(set_buf, sizeof(set_buf), "errors {}");
    } else {
      char members_buf[512] = {0};
      strcat(members_buf, "{");
      for (size_t i = 0; i < t->members.len; i++) {
        if (i > 0)
          strcat(members_buf, ", ");
        Member *m = t->members.items[i];
        if (m->type)
          strcat(members_buf, type_to_name(m->type));
      }
      strcat(members_buf, "}");
      snprintf(set_buf, sizeof(set_buf), "errors %s", members_buf);
    }
    depth--;
    return set_buf;
  }
  case KIND_RESULT: {
    static char result_buf[1024];
    snprintf(result_buf, sizeof(result_buf), "%s ! %s",
             type_to_name(t->data.result.success),
             type_to_name(t->data.result.error_set));
    depth--;
    return result_buf;
  }
  case KIND_UNION: {
    static char union_buf[1024];
    if (t->name.len > 0 &&
        !sv_eq(t->name, sv_from_parts("<anonymous union>", 16))) {
      snprintf(union_buf, sizeof(union_buf), "union %.*s", (int)t->name.len,
               t->name.data);
    } else {
      char members_buf[512] = {0};
      for (size_t i = 0; i < t->members.len; i++) {
        if (i > 0)
          strcat(members_buf, " | ");
        Member *m = t->members.items[i];
        if (m->type)
          strcat(members_buf, type_to_name(m->type));
      }
      snprintf(union_buf, sizeof(union_buf), "union(%s)", members_buf);
    }
    depth--;
    return union_buf;
  }
  case KIND_TEMPLATE: {
    static char template_buf[256];
    snprintf(template_buf, sizeof(template_buf), SV_FMT, SV_ARG(t->name));
    depth--;
    return template_buf;
  }
  case KIND_STRING_BUFFER:
    depth--;
    return "String";
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

static bool error_set_contains_all(Type *to, Type *from) {
  if (!to || !from || to->kind != KIND_ERROR_SET ||
      from->kind != KIND_ERROR_SET)
    return false;
  for (size_t i = 0; i < from->members.len; i++) {
    Member *member = from->members.items[i];
    if (!member || !member->type || member->type->kind != KIND_ERROR)
      return false;
    bool found = false;
    for (size_t j = 0; j < to->members.len; j++) {
      Member *candidate = to->members.items[j];
      if (candidate && candidate->type &&
          type_equals(candidate->type, member->type)) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

bool type_is_concrete(Type *t) {
  if (!t)
    return false;

  switch (t->kind) {
  case KIND_PRIMITIVE:
    return t->data.primitive != PRIM_UNKNOWN;
  case KIND_POINTER:
    return type_is_concrete(t->data.pointer_to);
  case KIND_STRUCT:
  case KIND_UNION:
    if (t->data.instance.from_template == NULL)
      return t->members.len > 0;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      Type *arg = t->data.instance.generic_args.items[i];
      if (!type_is_concrete(arg))
        return false;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (!type_is_concrete(m->type))
        return false;
    }
    return true;

  case KIND_ERROR:
    if (t->data.instance.from_template == NULL)
      return true;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      Type *arg = t->data.instance.generic_args.items[i];
      if (!type_is_concrete(arg))
        return false;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (!type_is_concrete(m->type))
        return false;
    }
    return true;

  case KIND_ERROR_SET:
    if (t->data.instance.from_template == NULL)
      return t->members.len > 0;
    for (size_t i = 0; i < t->data.instance.generic_args.len; i++) {
      Type *arg = t->data.instance.generic_args.items[i];
      if (!type_is_concrete(arg))
        return false;
    }
    for (size_t i = 0; i < t->members.len; i++) {
      Member *m = t->members.items[i];
      if (!type_is_concrete(m->type))
        return false;
    }
    return true;
  case KIND_RESULT:
    return type_is_concrete(t->data.result.success) &&
           type_is_concrete(t->data.result.error_set);
  case KIND_TEMPLATE:
    return false;
  case KIND_STRING_BUFFER:
    return true;
  default:
    return false;
  }
}

int type_can_implicitly_cast(Type *to, Type *from) {
  if (type_equals(to, from))
    return 1;

  if (!to || !from)
    return 0;

  if (to->kind == KIND_TEMPLATE || from->kind == KIND_TEMPLATE)
    return 1;

  if (to->kind == KIND_STRUCT && from->kind == KIND_STRUCT &&
      to->data.instance.from_template && from->data.instance.from_template &&
      to->data.instance.from_template == from->data.instance.from_template) {
    if (to->fixed_array_len == 0 && from->fixed_array_len != 0) {
      return 1;
    }
    if (to->fixed_array_len != 0 && from->fixed_array_len == 0) {
      return 0;
    }
    if (to->fixed_array_len == from->fixed_array_len) {
      if (to->data.instance.generic_args.len ==
          from->data.instance.generic_args.len) {
        bool all_args_match = true;
        for (size_t i = 0; i < to->data.instance.generic_args.len; i++) {
          if (!type_can_implicitly_cast(
                  to->data.instance.generic_args.items[i],
                  from->data.instance.generic_args.items[i])) {
            all_args_match = false;
            break;
          }
        }
        if (all_args_match)
          return 1;
      }
    }
  }

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

  if (to->kind == KIND_POINTER && from->kind == KIND_POINTER) {
    if (type_is_unknown(to->data.pointer_to) ||
        type_is_unknown(from->data.pointer_to))
      return 1;
    if (from->data.pointer_to->kind == KIND_PRIMITIVE &&
        from->data.pointer_to->data.primitive == PRIM_VOID)
      return 1;
  }

  if (to->kind == KIND_ERROR_SET && from->kind == KIND_ERROR) {
    for (size_t i = 0; i < to->members.len; i++) {
      Member *m = to->members.items[i];
      if (type_equals(m->type, from))
        return 1;
    }
  }

  if (to->kind == KIND_RESULT) {
    if (type_equals(to->data.result.success, from))
      return 1;
    if (from->kind == KIND_RESULT) {
      if (type_equals(to, from))
        return 1;
      if (to->data.result.error_set &&
          to->data.result.error_set->kind == KIND_ERROR_SET &&
          type_can_implicitly_cast(to->data.result.success,
                                   from->data.result.success)) {
        if (sv_eq(to->data.result.error_set->name, sv_from_parts("Error", 5))) {
          return 1;
        }
        if (from->data.result.error_set &&
            from->data.result.error_set->kind == KIND_ERROR_SET &&
            error_set_contains_all(to->data.result.error_set,
                                   from->data.result.error_set)) {
          return 1;
        }
      }
      return 0;
    }
    if (from->kind == KIND_ERROR && to->data.result.error_set) {
      Type *error_set = to->data.result.error_set;
      if (error_set->kind == KIND_ERROR_SET) {
        if (sv_eq(error_set->name, sv_from_parts("Error", 5)))
          return 1;
        for (size_t i = 0; i < error_set->members.len; i++) {
          Member *m = error_set->members.items[i];
          if (type_equals(m->type, from))
            return 1;
        }
      }
    }
  }

  if (to->kind == KIND_UNION && to->is_tagged_union) {
    for (size_t i = 0; i < to->members.len; i++) {
      Member *m = to->members.items[i];
      if (type_equals(m->type, from))
        return 1;
    }
  }

  if (to->kind == KIND_PRIMITIVE && to->data.primitive == PRIM_STRING &&
      from->kind == KIND_STRING_BUFFER) {
    return 1;
  }

  if (to->kind == KIND_POINTER && from->kind == KIND_STRING_BUFFER) {
    return 0;
  }

  return 0;
}