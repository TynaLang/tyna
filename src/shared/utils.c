#include <stdarg.h>
#include <stdnoreturn.h>

#include "tyna/utils.h"

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

void *List_pop(List *list) {
  if (list->len == 0) {
    panic("List pop from empty list");
  }
  return list->items[--list->len];
}

/*
 * free_items: if 1, frees each pointer inside the list before freeing the array
 */
void List_insert(List *list, size_t index, void *item) {
  if (index > list->len) {
    panic("List index out of bounds during insert");
  }

  if (list->len == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 4;
    void **new_items = realloc(list->items, sizeof(void *) * list->capacity);
    if (!new_items) {
      panic("Failed to realloc list");
    }
    list->items = new_items;
  }

  if (index < list->len) {
    memmove(&list->items[index + 1], &list->items[index],
            sizeof(void *) * (list->len - index));
  }

  list->items[index] = item;
  list->len++;
}

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

void *List_remove_at(List *list, size_t index) {
  if (index >= list->len) {
    panic("List index out of bounds during remove");
  }

  void *removed_item = list->items[index];

  size_t num_to_move = list->len - index - 1;

  if (num_to_move > 0) {
    memmove(&list->items[index], &list->items[index + 1],
            sizeof(void *) * num_to_move);
  }

  list->len--;
  return removed_item;
}

int List_index_of(List *list, void *item) {
  for (size_t i = 0; i < list->len; i++) {
    if (list->items[i] == item) {
      return i;
    }
  }
  return -1;
}

noreturn void panic(const char *msg, ...) {
  va_list args;
  va_start(args, msg);
  fprintf(stderr, "[PANIC] ");
  vfprintf(stderr, msg, args);
  fprintf(stderr, "\n");
  va_end(args);
  exit(1);
}

char *read_file(const char *filename) {
  FILE *fp = fopen(filename, "rb");
  if (!fp) {
    perror("Error opening file");
    return NULL;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    perror("fseek failed");
    fclose(fp);
    return NULL;
  }

  long filesize = ftell(fp);
  if (filesize < 0) {
    perror("ftell failed");
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  char *buffer = xmalloc(filesize + 1);

  size_t read = fread(buffer, 1, filesize, fp);
  if (read != filesize) {
    perror("fread failed");
    free(buffer);
    fclose(fp);
    return NULL;
  }

  buffer[filesize] = '\0';
  fclose(fp);
  return buffer;
}
