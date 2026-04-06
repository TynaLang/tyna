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
  // Future: KIND_POINTER
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

typedef struct Member Member;

struct Member {
  char *name;
  Type *type;
  BuiltinMember builtin_id;
  size_t offset;
};

struct Type {
  TypeKind kind;
  union {
    PrimitiveKind primitive; // If KIND_PRIMITIVE
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
  List array_types; // List of Type*
};

TypeContext *type_context_create();
void type_context_free(TypeContext *ctx);

Type *type_get_primitive(TypeContext *ctx, PrimitiveKind primitive);
Type *type_get_array(TypeContext *ctx, Type *element, bool is_dynamic,
                     size_t fixed_size);
Type *type_get_struct(TypeContext *ctx, const char *name);

Member *type_get_member(Type *type, StringView name);
void type_add_builtin_member(Type *type, const char *name, Type *member_type,
                             BuiltinMember builtin_id, size_t offset);
void type_add_member(Type *type, const char *name, Type *member_type,
                     size_t offset);

bool type_equals(Type *a, Type *b);

#endif // TYL_TYPE_H
