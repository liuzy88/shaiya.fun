#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define PATH_SEP '\\'
#define MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
#define PATH_SEP '/'
#define MKDIR(path) mkdir(path, 0755)
#endif

#define CHUNK_SIZE ((uint64_t) 100 * 1024 * 1024)
#define BUFFER_SIZE (1024 * 1024)

typedef struct {
    const char *input_path;
    const char *output_dir;
    const char *repo;
    const char *tag;
    const char *launcher_name;
} options_t;

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;

    if (slash != NULL && slash + 1 > base) {
        base = slash + 1;
    }
    if (backslash != NULL && backslash + 1 > base) {
        base = backslash + 1;
    }
    return base;
}

static int ensure_directory(const char *path) {
    if (MKDIR(path) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    fprintf(stderr, "failed to create directory %s: %s\n", path, strerror(errno));
    return -1;
}

static int copy_bytes(FILE *src, FILE *dst, uint64_t bytes_to_copy, unsigned char *buffer) {
    uint64_t remaining = bytes_to_copy;
    while (remaining > 0) {
        size_t request = remaining > BUFFER_SIZE ? BUFFER_SIZE : (size_t) remaining;
        size_t read_count = fread(buffer, 1, request, src);
        if (read_count == 0) {
            if (ferror(src)) {
                fprintf(stderr, "failed to read source file\n");
            } else {
                fprintf(stderr, "unexpected end of file while splitting\n");
            }
            return -1;
        }
        if (fwrite(buffer, 1, read_count, dst) != read_count) {
            fprintf(stderr, "failed to write output chunk\n");
            return -1;
        }
        remaining -= (uint64_t) read_count;
    }
    return 0;
}

static int write_manifest(const options_t *opts, const char *source_name, uint64_t total_size, uint64_t chunk_count) {
    char manifest_path[PATH_MAX];
    FILE *manifest;

    snprintf(manifest_path, sizeof(manifest_path), "%s%cmanifest.txt", opts->output_dir, PATH_SEP);
    manifest = fopen(manifest_path, "wb");
    if (manifest == NULL) {
        fprintf(stderr, "failed to create manifest: %s\n", strerror(errno));
        return -1;
    }

    fprintf(manifest, "repo=%s\n", opts->repo);
    fprintf(manifest, "tag=%s\n", opts->tag);
    fprintf(manifest, "file_name=%s\n", source_name);
    fprintf(manifest, "launcher_name=%s\n", opts->launcher_name);
    fprintf(manifest, "total_size=%" PRIu64 "\n", total_size);
    fprintf(manifest, "chunk_size=%" PRIu64 "\n", (uint64_t) CHUNK_SIZE);
    fprintf(manifest, "chunk_count=%" PRIu64 "\n", chunk_count);
    fprintf(manifest, "chunk_pattern=part-%%03d.bin\n");
    fprintf(manifest, "proxy_list=https://ghfast.top/;https://ghproxy.cc/;DIRECT\n");

    fclose(manifest);
    return 0;
}

static int split_file(const options_t *opts) {
    FILE *input = NULL;
    unsigned char *buffer = NULL;
    char output_path[PATH_MAX];
    struct stat st;
    uint64_t chunk_count;
    uint64_t index;
    const char *source_name = base_name(opts->input_path);

    if (stat(opts->input_path, &st) != 0) {
        fprintf(stderr, "failed to stat %s: %s\n", opts->input_path, strerror(errno));
        return -1;
    }

    if (ensure_directory(opts->output_dir) != 0) {
        return -1;
    }

    input = fopen(opts->input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", opts->input_path, strerror(errno));
        return -1;
    }

    buffer = (unsigned char *) malloc(BUFFER_SIZE);
    if (buffer == NULL) {
        fprintf(stderr, "failed to allocate buffer\n");
        fclose(input);
        return -1;
    }

    chunk_count = ((uint64_t) st.st_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    for (index = 0; index < chunk_count; ++index) {
        FILE *output;
        uint64_t written = index * CHUNK_SIZE;
        uint64_t remaining = (uint64_t) st.st_size - written;
        uint64_t current_size = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

        snprintf(output_path, sizeof(output_path), "%s%cpart-%03" PRIu64 ".bin", opts->output_dir, PATH_SEP, index);
        output = fopen(output_path, "wb");
        if (output == NULL) {
            fprintf(stderr, "failed to create %s: %s\n", output_path, strerror(errno));
            free(buffer);
            fclose(input);
            return -1;
        }

        if (copy_bytes(input, output, current_size, buffer) != 0) {
            fclose(output);
            free(buffer);
            fclose(input);
            return -1;
        }

        fclose(output);
        printf("wrote %s (%" PRIu64 " bytes)\n", output_path, current_size);
    }

    free(buffer);
    fclose(input);

    if (write_manifest(opts, source_name, (uint64_t) st.st_size, chunk_count) != 0) {
        return -1;
    }

    printf("manifest: %s%cmanifest.txt\n", opts->output_dir, PATH_SEP);
    return 0;
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s --input <install.exe> --output <patch_dir> --repo <owner/repo> --tag <release_tag> "
            "[--launcher installer.exe]\n",
            program_name);
}

int main(int argc, char **argv) {
    options_t opts;
    int i;

    memset(&opts, 0, sizeof(opts));
    opts.launcher_name = "installer.exe";

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            opts.input_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            opts.output_dir = argv[++i];
        } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            opts.repo = argv[++i];
        } else if (strcmp(argv[i], "--tag") == 0 && i + 1 < argc) {
            opts.tag = argv[++i];
        } else if (strcmp(argv[i], "--launcher") == 0 && i + 1 < argc) {
            opts.launcher_name = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (opts.input_path == NULL || opts.output_dir == NULL || opts.repo == NULL || opts.tag == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    return split_file(&opts) == 0 ? 0 : 1;
}
