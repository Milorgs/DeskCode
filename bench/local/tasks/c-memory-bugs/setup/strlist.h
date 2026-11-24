#ifndef STRLIST_H
#define STRLIST_H

#include <stddef.h>

/* A growable list of owned, NUL-terminated strings. */
struct strlist {
    char **items;
    size_t len;
    size_t cap;
};

/* Initialise an empty list. */
void strlist_init(struct strlist *list);

/* Append a private copy of s. Returns 0 on success, -1 on allocation failure. */
int strlist_push(struct strlist *list, const char *s);

/* Remove the item at index (shifting later items left) and free it.
   Returns 0 on success, -1 if index is out of range. */
int strlist_remove(struct strlist *list, size_t index);

/* Return a newly allocated string with all items joined by sep (caller frees).
   An empty list yields an empty string. Returns NULL on allocation failure. */
char *strlist_join(const struct strlist *list, const char *sep);

/* Free every item and the list storage; the list becomes empty and reusable. */
void strlist_free(struct strlist *list);

#endif
