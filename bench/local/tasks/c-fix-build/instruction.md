This C project does not build: `make` fails with compiler and linker errors.

Fix the source files and/or the Makefile so that `make` builds `./shapes` without any
warnings or errors. Keep the strict flags `-std=c11 -Wall -Wextra -Werror` in the Makefile
(do not remove or weaken them) and do not change the public function names in
`geometry.h`.

When it works, `./shapes` must print exactly:

```
distance=5.00
perimeter=12.00
```
