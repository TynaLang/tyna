#include "sema_internal.h"
#include "tyna/utils.h"

StringView sema_mangle_method_name(StringView owner, StringView method) {
  if (owner.len == 0)
    return method;

  char buf[512];
  int len = snprintf(buf, sizeof(buf), "_TZN%zu%.*s%zu%.*s", owner.len,
                     (int)owner.len, owner.data, method.len, (int)method.len,
                     method.data);
  char *heap = xmalloc(len + 1);
  memcpy(heap, buf, len + 1);
  return sv_from_parts(heap, len);
}
