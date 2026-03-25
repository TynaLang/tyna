#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Generic dynamically growing list.
 * Stores void* items, can be typecast to whatever you want.
 */
typedef struct {
  void **items;
  size_t len;      // number of elements
  size_t capacity; // allocated capacity
} List;

/* List operations */
void List_init(List *list);               // Initialize the list
void List_push(List *list, void *item);   // Append an item
void *List_get(List *list, size_t index); // Get item at index
void List_free(
    List *list,
    int free_items); // Free list; if free_items=1, also free each item

/* Optional: error helper */
void panic(const char *msg);

#endif // UTILS_H
