CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -pedantic
WINDRES ?= x86_64-w64-mingw32-windres
MINGW_CC ?= x86_64-w64-mingw32-gcc
WIN_CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all splitter install-win clean

all: splitter

splitter: splitter.c
	$(CC) $(CFLAGS) -o $@ splitter.c

install-res.o: install.rc shaiya.ico
	$(WINDRES) install.rc -O coff -o $@

install-win: installer.c install-res.o
	$(MINGW_CC) $(WIN_CFLAGS) -o install.exe installer.c install-res.o -lwinhttp -lshell32

clean:
	rm -f splitter install-res.o
