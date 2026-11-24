#include "geometry.h"

double distance(struct point a, struct point b)
{
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

double perimeter(const struct point *pts, size_t n)
{
    double total = 0.0;
    if (n < 2)
        return 0.0;
    for (int i = 0; i < n; i++)
        total += distance(pts[i], pts[(i + 1) % n]);
    return total;
}
