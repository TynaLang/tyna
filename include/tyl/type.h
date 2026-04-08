#ifndef TYL_TYPE_H
#define TYL_TYPE_H

#include "tyl/utils.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct Type Type;
typedef struct TypeContext TypeContext;

typedef enum TypeKind TypeKind;
typedef enum PrimitiveKind PrimitiveKind;

enum TypeKind {
  KIND_PRIMITIVE,
  KIND_ARRAY,
  KIND_STRUCT,
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
  PRIM_STRING,
  PRIM_UNKNOWN
};

typedef enum {
  BUILTIN_NONE,
  BUILTIN_ARRAY_LEN,
} BuiltinMember;

typedef struct Member {
  char *name;
  Type *type;
  BuiltinMember builtin_id;
  size_t offset;
} Member;

struct Type {
  TypeKind kind;
  union {
    PrimitiveKind primitive;
    struct {
      Type *element;
      bool is_dynamic;
      size_t fixed_size;
    } array;
    struct {
      char *name;
    } structure;
  } data;
  List members; // List<Member*>
};

struct TypeContext {
  Type *primitives[PRIM_UNKNOWN + 1];
  List array_types;  // List<Type*>
  List struct_types; // List<Type*>
};

// Lifecycle
TypeContext *type_context_create();
void type_context_free(TypeContext *ctx);

// Type Retrieval (Interned)
Type *type_get_primitive(TypeContext *ctx, PrimitiveKind primitive);
Type *type_get_array(TypeContext *ctx, Type *element, bool is_dynamic,
                     size_t fixed_size);
Type *type_get_struct(TypeContext *ctx, const char *name);

// Member Management
Member *type_get_member(Type *type, StringView name);
void type_add_builtin_member(Type *type, const char *name, Type *member_type,
                             BuiltinMember builtin_id, size_t offset);
void type_add_member(Type *type, const char *name, Type *member_type,
                     size_t offset);

// Comparison & Analysis
bool type_equals(Type *a, Type *b);
const char *type_to_name(Type *type);
int type_rank(Type *t);

// Type Checks
bool type_is_numeric(Type *t);
bool type_is_bool(Type *t);
bool type_is_unknown(Type *t);
bool type_is_reference(Type *t);

// Semantic Rules
int type_can_implicitly_cast(Type *to, Type *from);

#endif // TYL_TYPE_H