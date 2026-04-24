CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -pedantic

.PHONY: all splitter clean

all: splitter

splitter: tools/splitter.c
	$(CC) $(CFLAGS) -o $@ tools/splitter.c

clean:
	rm -f splitter
