#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  void **items;
  size_t len;
  size_t capacity;
} List;

void List_init(List *list);
void List_push(List *list, void *item);
void *List_get(List *list, size_t index);
void List_free(List *list, int free_items);

void panic(const char *msg);

char *read_file(const char *filepath);

#endif // UTILS_H
