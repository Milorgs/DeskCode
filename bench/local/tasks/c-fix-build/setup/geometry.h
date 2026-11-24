#ifndef GEOMETRY_H
#define GEOMETRY_H

struct point {
    double x;
    double y;
};

/* Euclidean distance between two points. */
double distance(struct point a, struct point b);

/* Length of the closed polygon through pts[0..n-1] (last point connects back to the first).
   Returns 0 for n < 2. */
double perimeter(const struct point *pts, size_t n);

#endif
