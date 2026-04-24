# shaiya.fun

This repo contains a chunked delivery flow for a large Windows `install.exe`.

## Files

- `tools/splitter.c`: splits a large installer into `100 MiB` chunks and writes `patch/manifest.txt`
- `src/installer.c`: Windows downloader that fetches release assets through public GitHub proxies, merges them, and launches the merged installer
- `Makefile`: builds the local splitter on macOS/Linux

## Build splitter

```bash
make splitter
```

## Split a large installer

```bash
./splitter \
  --input install.exe \
  --output patch \
  --repo liuzy88/shaiya.fun \
  --tag v1.0.0
```

This generates:

- `patch/part-000.bin`, `patch/part-001.bin`, ...
- `patch/manifest.txt`

Upload every file in `patch/` to a GitHub Release under the same tag.

## Build the Windows downloader

Use a Windows or MinGW toolchain to compile:

```bash
gcc -O2 -Wall -Wextra -municode -o installer.exe src/installer.c -lwinhttp
```

Put `installer.exe` next to `manifest.txt` when distributing it to users.

## Runtime flow

1. `installer.exe` reads `manifest.txt`
2. It downloads all chunks from GitHub Releases
3. It tries public proxy URLs first, then falls back to direct GitHub
4. It merges the chunks back into `install.exe`
5. It starts the merged `install.exe`
