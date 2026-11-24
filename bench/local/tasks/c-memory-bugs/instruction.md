`strlist.c` implements a small growable list of strings (see `strlist.h` for the API
contract). It has several memory-safety bugs: the demo sometimes crashes, and building it
with AddressSanitizer (`make asan && ./demo_asan`) reports heap errors.

Fix all memory bugs in `strlist.c` so that the library is correct and AddressSanitizer /
UndefinedBehaviorSanitizer report nothing for any valid use of the API described in
`strlist.h` (not just the demo). Do not change the API in `strlist.h`. The expected demo
output is in `expected_output.txt`.
