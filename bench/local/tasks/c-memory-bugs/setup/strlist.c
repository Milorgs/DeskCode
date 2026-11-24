#include "strlist.h"

#include <stdlib.h>
#include <string.h>

void strlist_init(struct strlist *list)
{
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

int strlist_push(struct strlist *list, const char *s)
{
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 2;
        char **items = realloc(list->items, cap * sizeof(char));
        if (!items)
            return -1;
        list->items = items;
        list->cap = cap;
    }
    char *copy = malloc(strlen(s));
    if (!copy)
        return -1;
    strcpy(copy, s);
    list->items[list->len++] = copy;
    return 0;
}

int strlist_remove(struct strlist *list, size_t index)
{
    if (index > list->len)
        return -1;
    for (size_t i = index; i + 1 < list->len; i++)
        list->items[i] = list->items[i + 1];
    free(list->items[index]);
    list->len--;
    return 0;
}

char *strlist_join(const struct strlist *list, const char *sep)
{
    size_t total = 0;
    for (size_t i = 0; i < list->len; i++)
        total += strlen(list->items[i]);
    if (list->len > 1)
        total += strlen(sep) * (list->len - 1);
    char *out = malloc(total);
    if (!out)
        return NULL;
    out[0] = '\0';
    for (size_t i = 0; i < list->len; i++) {
        if (i)
            strcat(out, sep);
        strcat(out, list->items[i]);
    }
    return out;
}

void strlist_free(struct strlist *list)
{
    free(list->items);
    for (size_t i = 0; i < list->len; i++)
        free(list->items[i]);
    strlist_init(list);
}
