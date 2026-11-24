#include <stdio.h>

#include "geometry.h"

int main(void)
{
    struct point triangle[] = {{0, 0}, {3, 0}, {3, 4}};
    int unused;

    printf("distance=%.2f\n", distance(triangle[0], triangle[2]));
    printf("perimeter=%d\n", perimeter(triangle));
    return 0;
}
