#!/usr/bin/env bash
# Reference solution for c-fix-build.
set -eu
python3 - <<'PY'
import pathlib
h = pathlib.Path("geometry.h")
h.write_text(h.read_text().replace("#define GEOMETRY_H\n", "#define GEOMETRY_H\n\n#include <stddef.h>\n", 1))
c = pathlib.Path("geometry.c")
c.write_text(c.read_text().replace('#include "geometry.h"\n', '#include <math.h>\n\n#include "geometry.h"\n', 1)
             .replace("for (int i = 0; i < n; i++)", "for (size_t i = 0; i < n; i++)"))
m = pathlib.Path("main.c")
m.write_text(m.read_text().replace("    int unused;\n", "")
             .replace('printf("perimeter=%d\\n", perimeter(triangle));',
                      'printf("perimeter=%.2f\\n", perimeter(triangle, 3));'))
mk = pathlib.Path("Makefile")
mk.write_text(mk.read_text().replace("-o $@ $(OBJS)\n", "-o $@ $(OBJS) -lm\n"))
PY
