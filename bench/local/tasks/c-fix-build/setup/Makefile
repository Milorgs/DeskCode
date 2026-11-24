CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
OBJS = main.o geometry.o

shapes: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c geometry.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f shapes $(OBJS)

.PHONY: clean
