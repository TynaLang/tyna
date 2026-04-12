#ifndef TYL_TYPE_H
#define TYL_TYPE_H

#include "tyl/utils.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Type Type;
typedef struct TypeContext TypeContext;

typedef enum TypeKind TypeKind;
typedef enum PrimitiveKind PrimitiveKind;

enum TypeKind {
  KIND_PRIMITIVE,
  KIND_POINTER,  // New: ptr<T>
  KIND_STRUCT,   // Concrete Instance (Sized)
  KIND_UNION,    // Concrete Union (Sized)
  KIND_TEMPLATE, // Generic Blueprint (Unsized)
};

enum PrimitiveKind {
  PRIM_I8,
  PRIM_I16,
  PRIM_I32,
  PRIM_I64,
  PRIM_U8,
  PRIM_U16,
  PRIM_U32,
  PRIM_U64,
  PRIM_F32,
  PRIM_F64,
  PRIM_CHAR,
  PRIM_BOOL,
  PRIM_VOID,
  PRIM_STRING, // Kept for literal tagging, but behavior moves to stdlib
  PRIM_UNKNOWN
};

typedef struct Member {
  char *name;
  Type *type;
  size_t offset;
  unsigned index; // New: field index for LLVM StructGEP
} Member;

struct Type {
  TypeKind kind;
  StringView name;
  size_t size;
  size_t alignment;
  uint64_t fixed_array_len;

  union {
    PrimitiveKind primitive;
    Type *pointer_to; // For KIND_POINTER
    struct {
      List placeholders; // List<StringView> (e.g. "T")
      List fields;       // Unresolved members
    } template;
    struct {
      Type *from_template;
      List generic_args; // List<Type*> (e.g. [i32])
    } instance;
  } data;

  List members;      // List<Member*> - Resolved for concrete types
  List methods;      // List<Symbol*> - New infrastructure for methods
  bool is_frozen;    // If true, fields cannot be added/modified.
  bool is_intrinsic; // Identifies types the compiler "needs" (like Array)
};

struct TypeContext {
  Type *primitives[PRIM_UNKNOWN + 1];
  List structs;   // Generic structs
  List templates; // List<Type*>
  List instances; // List<Type*>
};

// Lifecycle
TypeContext *type_context_create();
void type_context_free(TypeContext *ctx);

// Type Retrieval
Type *type_get_primitive(TypeContext *ctx, PrimitiveKind primitive);
Type *type_get_pointer(TypeContext *ctx, Type *to);
Type *type_get_named(TypeContext *ctx, StringView name);
Type *type_get_struct(TypeContext *ctx, StringView name);
Type *type_get_union(TypeContext *ctx, StringView name);
Type *type_get_union_anonymous(TypeContext *ctx, List types);
Type *type_get_template(TypeContext *ctx, StringView name);
Type *type_get_instance(TypeContext *ctx, Type *template_type, List args);
Type *type_get_instance_fixed(TypeContext *ctx, Type *template_type, List args,
                              uint64_t fixed_array_len);

// Member Management
Member *type_get_member(Type *type, StringView name);
Member *type_find_union_field(Type *type, StringView name, Type **out_owner);
void type_add_member(Type *type, const char *name, Type *member_type,
                     size_t offset);

// Comparison & Analysis
bool type_equals(Type *a, Type *b);
const char *type_to_name(Type *type);
int type_rank(Type *t);
size_t type_get_primitive_size(PrimitiveKind prim);
size_t align_to(size_t value, size_t align);

// Type Checks
bool type_is_numeric(Type *t);
bool type_is_bool(Type *t);
bool type_is_unknown(Type *t);

// Semantic Rules
int type_can_implicitly_cast(Type *to, Type *from);

#endif // TYL_TYPE_H