# shaiya.fun

This repo contains a chunked delivery flow for Windows executables stored under `exe/` and published from repository directories.

## Files

- `splitter.c`: reads `exe/<name>.exe`, calculates the full MD5, and writes chunk assets into `exe-<md5>/`
- `installer.c`: `install.exe` downloads repository files through public GitHub proxies, merges them, and launches the target executable
- `Makefile`: builds the local splitter on macOS/Linux

## Build splitter

```bash
make splitter
```

## Split a large installer

```bash
./splitter \
  --input exe/shaiya.exe \
  --repo liuzy88/shaiya.fun \
  --tag main
```

This generates:

- `exe-<full-md5>/part-000.bin`, `exe-<full-md5>/part-001.bin`, ...
- `exe-<full-md5>/manifest.txt`

Commit that directory to the repository under the same ref.

For download-flow testing, you can override the chunk size:

```bash
./splitter \
  --input exe/geek.exe \
  --repo liuzy88/shaiya.fun \
  --tag main \
  --chunk-size 1048576
```

## Build the Windows downloader

Use a Windows or MinGW toolchain to compile:

```bash
x86_64-w64-mingw32-windres install.rc -O coff -o install-res.o
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o install.exe installer.c install-res.o -lwinhttp -lshell32
```

Distribute `install.exe` by itself, then launch it with the package id:

```bash
install.exe exe-<full-md5>
```

## Runtime flow

1. `install.exe` downloads `<package_id>/manifest.txt`
2. It downloads all `<package_id>/part-*.bin` files from the repository
3. It tries public proxy URLs first, then falls back to direct GitHub
4. It merges the chunks back into `manifest.file_name`
5. It starts the merged executable
