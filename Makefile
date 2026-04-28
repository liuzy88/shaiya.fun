CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -pedantic
WINDRES ?= x86_64-w64-mingw32-windres
MINGW_CC ?= x86_64-w64-mingw32-gcc
WIN_CFLAGS ?= -O2 -Wall -Wextra

INSTALL_SRCS := \
	src/main.c \
	src/util.c \
	src/log.c \
	src/ui.c \
	src/http.c \
	src/manifest.c \
	src/download.c

.PHONY: all splitter install-win clean

all: splitter

splitter: src/splitter.c
	$(CC) $(CFLAGS) -o $@ src/splitter.c

src/install-res.o: src/install.rc src/shaiya.ico
	$(WINDRES) src/install.rc -O coff -o src/install-res.o

install-win: $(INSTALL_SRCS) src/install.h src/install-res.o
	$(MINGW_CC) $(WIN_CFLAGS) -o install.exe $(INSTALL_SRCS) src/install-res.o -lwinhttp -lshell32 -ladvapi32

clean:
	rm -f splitter install.exe src/install-res.o
