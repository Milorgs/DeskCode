#include <stdio.h>
#include <stdlib.h>

#include "strlist.h"

int main(void)
{
    struct strlist list;
    const char *words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};

    strlist_init(&list);
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (strlist_push(&list, words[i]) != 0)
            return 1;
    strlist_remove(&list, 1);

    char *joined = strlist_join(&list, ", ");
    if (!joined)
        return 1;
    printf("%zu items: %s\n", list.len, joined);
    free(joined);
    strlist_free(&list);
    return 0;
}
