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

#define BUFFER_SIZE (1024 * 1024)

typedef struct {
    const char *input_path;
    const char *repo;
    const char *tag;
    uint64_t chunk_size;
} options_t;

typedef struct {
    char md5_hex[33];
    char package_id[40];
    char output_dir[PATH_MAX];
} package_info_t;

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

static int run_md5_command(const char *input_path, char *md5_hex, size_t md5_hex_size) {
    FILE *pipe = NULL;
    char command[PATH_MAX + 32];

#ifdef _WIN32
    snprintf(command, sizeof(command), "certutil -hashfile \"%s\" MD5", input_path);
#elif __APPLE__
    snprintf(command, sizeof(command), "md5 -q \"%s\"", input_path);
#else
    snprintf(command, sizeof(command), "md5sum \"%s\"", input_path);
#endif

    pipe = popen(command, "r");
    if (pipe == NULL) {
        return -1;
    }

#ifdef _WIN32
    while (fgets(md5_hex, (int) md5_hex_size, pipe) != NULL) {
        if (strspn(md5_hex, "0123456789abcdefABCDEF\r\n") >= 32) {
            break;
        }
    }
#else
    if (fgets(md5_hex, (int) md5_hex_size, pipe) == NULL) {
        pclose(pipe);
        return -1;
    }
#endif

    pclose(pipe);
    md5_hex[32] = '\0';
    if (strlen(md5_hex) < 32) {
        return -1;
    }
    md5_hex[32] = '\0';
    return 0;
}

static int build_package_info(const char *input_path, package_info_t *package) {
    if (run_md5_command(input_path, package->md5_hex, sizeof(package->md5_hex)) != 0) {
        fprintf(stderr, "failed to calculate md5 for %s\n", input_path);
        return -1;
    }

    snprintf(package->package_id, sizeof(package->package_id), "exe-%s", package->md5_hex);
    snprintf(package->output_dir, sizeof(package->output_dir), "%s", package->package_id);
    return 0;
}

static int write_manifest(const options_t *opts, const package_info_t *package, const char *source_name, uint64_t total_size,
                          uint64_t chunk_count) {
    char manifest_path[PATH_MAX];
    FILE *manifest;

    snprintf(manifest_path, sizeof(manifest_path), "%s%cmanifest.txt", package->output_dir, PATH_SEP);
    manifest = fopen(manifest_path, "wb");
    if (manifest == NULL) {
        fprintf(stderr, "failed to create manifest: %s\n", strerror(errno));
        return -1;
    }

    fprintf(manifest, "package_id=%s\n", package->package_id);
    fprintf(manifest, "md5=%s\n", package->md5_hex);
    fprintf(manifest, "repo=%s\n", opts->repo);
    fprintf(manifest, "ref=%s\n", opts->tag);
    fprintf(manifest, "file_name=%s\n", source_name);
    fprintf(manifest, "total_size=%" PRIu64 "\n", total_size);
    fprintf(manifest, "chunk_size=%" PRIu64 "\n", opts->chunk_size);
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
    package_info_t package;

    if (stat(opts->input_path, &st) != 0) {
        fprintf(stderr, "failed to stat %s: %s\n", opts->input_path, strerror(errno));
        return -1;
    }

    if (build_package_info(opts->input_path, &package) != 0) {
        return -1;
    }

    if (ensure_directory(package.output_dir) != 0) {
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

    chunk_count = ((uint64_t) st.st_size + opts->chunk_size - 1) / opts->chunk_size;
    for (index = 0; index < chunk_count; ++index) {
        FILE *output;
        uint64_t written = index * opts->chunk_size;
        uint64_t remaining = (uint64_t) st.st_size - written;
        uint64_t current_size = remaining > opts->chunk_size ? opts->chunk_size : remaining;

        snprintf(output_path, sizeof(output_path), "%s%cpart-%03" PRIu64 ".bin", package.output_dir, PATH_SEP, index);
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

    if (write_manifest(opts, &package, source_name, (uint64_t) st.st_size, chunk_count) != 0) {
        return -1;
    }

    printf("package_id: %s\n", package.package_id);
    printf("manifest: %s%cmanifest.txt\n", package.output_dir, PATH_SEP);
    return 0;
}

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s --input <install.exe> --output <patch_dir> --repo <owner/repo> --tag <release_tag> "
            "[--chunk-size bytes]\n",
            program_name);
}

static int parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0) {
        return -1;
    }
    *value = (uint64_t) parsed;
    return 0;
}

int main(int argc, char **argv) {
    options_t opts;
    int i;

    memset(&opts, 0, sizeof(opts));
    opts.chunk_size = (uint64_t) 100 * 1024 * 1024;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            opts.input_path = argv[++i];
        } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            opts.repo = argv[++i];
        } else if (strcmp(argv[i], "--tag") == 0 && i + 1 < argc) {
            opts.tag = argv[++i];
        } else if (strcmp(argv[i], "--chunk-size") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &opts.chunk_size) != 0) {
                fprintf(stderr, "invalid --chunk-size value\n");
                return 1;
            }
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (opts.input_path == NULL || opts.repo == NULL || opts.tag == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    return split_file(&opts) == 0 ? 0 : 1;
}
