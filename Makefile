CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -pedantic

.PHONY: all splitter clean

all: splitter

splitter: splitter.c
	$(CC) $(CFLAGS) -o $@ splitter.c

clean:
	rm -f splitter
