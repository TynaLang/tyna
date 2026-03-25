#include "utils.h"

void List_init(List *list) {
  list->items = NULL;
  list->len = 0;
  list->capacity = 0;
}

void List_push(List *list, void *item) {
  if (list->len == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 4;
    void **new_items = realloc(list->items, sizeof(void *) * list->capacity);
    if (!new_items) {
      panic("Failed to realloc list");
    }
    list->items = new_items;
  }
  list->items[list->len++] = item;
}

void *List_get(List *list, size_t index) {
  if (index >= list->len) {
    panic("List index out of bounds");
  }
  return list->items[index];
}

/*
 * free_items: if 1, frees each pointer inside the list before freeing the array
 */
void List_free(List *list, int free_items) {
  if (free_items) {
    for (size_t i = 0; i < list->len; i++) {
      free(list->items[i]);
    }
  }
  free(list->items);
  list->items = NULL;
  list->len = 0;
  list->capacity = 0;
}

void panic(const char *msg) {
  fprintf(stderr, "[PANIC] %s\n", msg);
  exit(1);
}
