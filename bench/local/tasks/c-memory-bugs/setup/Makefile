CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -O1 -g

demo: demo.c strlist.c strlist.h
	$(CC) $(CFLAGS) -o $@ demo.c strlist.c

asan: demo_asan

demo_asan: demo.c strlist.c strlist.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ demo.c strlist.c

clean:
	rm -f demo demo_asan

.PHONY: asan clean
