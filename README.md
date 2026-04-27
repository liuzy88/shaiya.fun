# shaiya.fun

This repo contains a chunked delivery flow for Windows executables stored under `exe/` and published from repository directories.

## Files

- `src/splitter.c`: reads `exe/<name>.exe`, calculates the full MD5, and writes chunk assets into `exe-<md5>/`
- `src/installer.c`: `install.exe` downloads manifests and chunk files, merges them, cleans up `patch/`, and launches the target executable
- `src/install.rc`: embeds `src/shaiya.ico` as the Windows executable icon
- `src/MySPC.pfx`: optional signing certificate kept alongside the Windows installer sources
- `Makefile`: builds the root output binaries `splitter` and `install.exe`

## Build splitter

```bash
make splitter
```

This writes the `splitter` executable to the repository root.

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
make install-win
```

This produces `install.exe` in the repository root with the `src/shaiya.ico` icon embedded.

## Run modes

Distribute `install.exe` by itself, then launch it with the package id:

```bash
install.exe exe-<full-md5>
```

Or launch it without arguments to read the repository root `manifest.txt`:

```bash
install.exe
```

## Download behavior

- `manifest.txt` is downloaded through the GitHub contents API
- chunk files are downloaded through raw file URLs
- proxy order is currently `https://ghfast.top/` first, then direct GitHub
- downloaded chunks are stored in `patch/`, merged, then `patch/` is deleted

## Runtime flow

1. `install.exe` downloads a manifest from either `<package_id>/manifest.txt` or repository root `manifest.txt`
2. It downloads all `part-*.bin` files into `patch/`
3. It merges the chunks back into `manifest.file_name`
4. It deletes the downloaded `patch/` directory
5. It starts the merged executable
