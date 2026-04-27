#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#define INSTALLER_VERSION "ui-20260427-1"
#define MAX_STR 2048
#define BUFFER_SIZE 65536
#define MAX_PROXY 256
#define MAX_STAGE 64
#define MAX_EVENT 256
#define MAX_CHUNK_LINES 10
#define DEFAULT_REPO "liuzy88/shaiya.fun"
#define DEFAULT_REF "main"
#define DEFAULT_PROXY_LIST "https://ghfast.top/;DIRECT"
#define DEFAULT_JOBS 3
#define WORKSPACE_DIR_NAME "Downloads"
#define WORKSPACE_MARGIN_BYTES (256ULL * 1024ULL * 1024ULL)

typedef enum {
    STAGE_PENDING = 0,
    STAGE_RUNNING = 1,
    STAGE_DONE = 2,
    STAGE_FAILED = 3
} stage_state_t;

typedef enum {
    CHUNK_QUEUED = 0,
    CHUNK_DOWNLOADING = 1,
    CHUNK_RETRY = 2,
    CHUNK_DONE = 3,
    CHUNK_FAILED = 4
} chunk_state_t;

typedef struct {
    char package_id[MAX_STR];
    char md5[MAX_STR];
    char repo[MAX_STR];
    char ref[MAX_STR];
    char file_name[MAX_STR];
    char chunk_pattern[MAX_STR];
    char proxy_list[MAX_STR];
    unsigned long long total_size;
    unsigned long long chunk_size;
    unsigned long long chunk_count;
} manifest_t;

typedef struct {
    char *data;
    size_t size;
    DWORD status_code;
    unsigned long long content_length;
    char content_type[256];
} http_buffer_t;

typedef struct {
    int index;
    char name[64];
    chunk_state_t state;
    DWORD last_error;
    DWORD status_code;
    unsigned long long expected_size;
    unsigned long long downloaded_size;
    int attempts;
    char detail[96];
} chunk_item_t;

typedef struct {
    HANDLE file;
    CRITICAL_SECTION lock;
    char path[MAX_PATH];
} file_logger_t;

typedef struct {
    HANDLE console;
    int vt_enabled;
    CRITICAL_SECTION lock;
    int patch_live;
    int patch_lines;
    unsigned long long total_chunks;
    unsigned long long finished_chunks;
    unsigned long long failed_chunks;
    unsigned long long total_bytes;
    unsigned long long downloaded_bytes;
    int jobs;
    chunk_item_t *chunks;
} ui_state_t;

typedef struct {
    manifest_t manifest;
    char temp_file_path[MAX_PATH];
    int jobs;
    chunk_item_t *chunks;
    LONG next_index;
    LONG failed_flag;
    LONG finished_count;
    CRITICAL_SECTION chunk_lock;
    CRITICAL_SECTION file_lock;
    HANDLE temp_file;
    ui_state_t *ui;
    file_logger_t *logger;
} download_plan_t;

typedef struct {
    download_plan_t *plan;
    int worker_id;
} worker_ctx_t;

static file_logger_t g_logger;
static ui_state_t g_ui;

typedef void (*progress_callback_t)(void *ctx, const char *label, unsigned long long downloaded, unsigned long long total, int done);

typedef struct {
    ui_state_t *ui;
    const char *label;
} manifest_progress_ctx_t;

typedef struct {
    download_plan_t *plan;
    int chunk_index;
} chunk_progress_ctx_t;

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static const char *ui_color(const char *code) {
    return g_ui.vt_enabled ? code : "";
}

static const char *ui_reset(void) {
    return g_ui.vt_enabled ? "\x1b[0m" : "";
}

static const char *ui_color_stage(void) {
    return ui_color("\x1b[1;36m");
}

static const char *ui_color_ok(void) {
    return ui_color("\x1b[1;32m");
}

static const char *ui_color_err(void) {
    return ui_color("\x1b[1;31m");
}

static const char *ui_color_dim(void) {
    return ui_color("\x1b[2m");
}

static const char *ui_color_warn(void) {
    return ui_color("\x1b[1;33m");
}

static const char *chunk_state_name(chunk_state_t state) {
    switch (state) {
    case CHUNK_DOWNLOADING:
        return "GET";
    case CHUNK_RETRY:
        return "RETRY";
    case CHUNK_DONE:
        return "DONE";
    case CHUNK_FAILED:
        return "FAIL";
    default:
        return "WAIT";
    }
}

static const char *chunk_state_color(chunk_state_t state) {
    switch (state) {
    case CHUNK_DOWNLOADING:
        return ui_color_stage();
    case CHUNK_RETRY:
        return ui_color_warn();
    case CHUNK_DONE:
        return ui_color_ok();
    case CHUNK_FAILED:
        return ui_color_err();
    default:
        return ui_color_dim();
    }
}

static void format_size(unsigned long long bytes, char *buffer, size_t buffer_size) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double) bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        snprintf(buffer, buffer_size, "%llu %s", bytes, units[unit]);
    } else {
        snprintf(buffer, buffer_size, "%.1f %s", value, units[unit]);
    }
}

static void logger_write_line(file_logger_t *logger, const char *line) {
    DWORD written = 0;
    if (logger->file == INVALID_HANDLE_VALUE) {
        return;
    }
    EnterCriticalSection(&logger->lock);
    SetFilePointer(logger->file, 0, NULL, FILE_END);
    WriteFile(logger->file, line, (DWORD) strlen(line), &written, NULL);
    WriteFile(logger->file, "\r\n", 2, &written, NULL);
    LeaveCriticalSection(&logger->lock);
}

static void log_message(file_logger_t *logger, const char *fmt, ...) {
    SYSTEMTIME st;
    char text[2048];
    char line[2300];
    va_list args;

    GetLocalTime(&st);
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    snprintf(line,
             sizeof(line),
             "[%04u-%02u-%02u %02u:%02u:%02u] %s",
             st.wYear,
             st.wMonth,
             st.wDay,
             st.wHour,
             st.wMinute,
             st.wSecond,
             text);
    logger_write_line(logger, line);
}

static int init_logger(file_logger_t *logger) {
    char exe_path[MAX_PATH];
    char *slash = NULL;
    char candidate[MAX_PATH];
    char temp_dir[MAX_PATH];
    DWORD temp_len;

    memset(logger, 0, sizeof(*logger));
    logger->file = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&logger->lock);

    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) == 0) {
        return -1;
    }
    slash = strrchr(exe_path, '\\');
    if (slash != NULL) {
        *slash = '\0';
    }
    snprintf(candidate, sizeof(candidate), "%s\\install.log", exe_path);
    logger->file = CreateFileA(candidate, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (logger->file != INVALID_HANDLE_VALUE) {
        safe_copy(logger->path, sizeof(logger->path), candidate);
        return 0;
    }

    temp_len = GetTempPathA(sizeof(temp_dir), temp_dir);
    if (temp_len == 0 || temp_len >= sizeof(temp_dir)) {
        return -1;
    }
    snprintf(candidate, sizeof(candidate), "%sinstall.log", temp_dir);
    logger->file = CreateFileA(candidate, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (logger->file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    safe_copy(logger->path, sizeof(logger->path), candidate);
    return 0;
}

static void close_logger(file_logger_t *logger) {
    if (logger->file != INVALID_HANDLE_VALUE) {
        CloseHandle(logger->file);
        logger->file = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&logger->lock);
}

static void sanitize_preview(char *buffer, size_t size) {
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned char c = (unsigned char) buffer[i];
        if ((c < 32 || c > 126) && c != '\r' && c != '\n' && c != '\t') {
            buffer[i] = '.';
        }
    }
}

static void log_buffer_preview(file_logger_t *logger, const char *label, const char *buffer, size_t size) {
    char preview[257];
    size_t copy_size = size > sizeof(preview) - 1 ? sizeof(preview) - 1 : size;
    memcpy(preview, buffer, copy_size);
    preview[copy_size] = '\0';
    sanitize_preview(preview, copy_size);
    log_message(logger, "%s preview: %s", label, preview);
}

static int init_ui(ui_state_t *ui, int jobs) {
    DWORD mode = 0;
    memset(ui, 0, sizeof(*ui));
    InitializeCriticalSection(&ui->lock);
    ui->console = GetStdHandle(STD_OUTPUT_HANDLE);
    ui->jobs = jobs;
    if (ui->console != INVALID_HANDLE_VALUE && GetConsoleMode(ui->console, &mode)) {
        if (SetConsoleMode(ui->console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            ui->vt_enabled = 1;
        }
    }
    return 0;
}

static void ui_print_line(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static void ui_print_blank(void) {
    printf("\n");
    fflush(stdout);
}

static void render_progress_bar(char *buffer, size_t buffer_size, unsigned long long downloaded, unsigned long long total) {
    const int width = 34;
    int filled = 0;
    int i;

    if (total > 0) {
        filled = (int) ((downloaded * (unsigned long long) width) / total);
        if (filled > width) {
            filled = width;
        }
    }
    if (buffer_size < (size_t) width + 3) {
        buffer[0] = '\0';
        return;
    }
    buffer[0] = '[';
    for (i = 0; i < width; ++i) {
        buffer[i + 1] = i < filled ? '=' : ' ';
    }
    buffer[width + 1] = ']';
    buffer[width + 2] = '\0';
}

static void ui_manifest_progress(void *ctx, const char *label, unsigned long long downloaded, unsigned long long total, int done) {
    manifest_progress_ctx_t *progress = (manifest_progress_ctx_t *) ctx;
    char bar[40];
    char bytes_text[32];
    int percent = 0;

    (void) progress;
    render_progress_bar(bar, sizeof(bar), downloaded, total);
    format_size(downloaded, bytes_text, sizeof(bytes_text));
    if (total > 0) {
        percent = (int) ((downloaded * 100ULL) / total);
        if (percent > 100) {
            percent = 100;
        }
        printf("\r%s%s%s %s %s%3d%%%s downloading",
               ui_color_stage(),
               label,
               ui_reset(),
               bar,
               ui_color_stage(),
               percent,
               ui_reset());
    } else {
        printf("\r%s%s%s %s %s downloading", ui_color_stage(), label, ui_reset(), bar, bytes_text);
    }
    fflush(stdout);
    if (done) {
        printf("\n");
        fflush(stdout);
    }
}

static void ui_clear_patch_locked(ui_state_t *ui) {
    int i;

    if (!ui->patch_live || !ui->vt_enabled) {
        return;
    }
    if (ui->patch_lines > 0) {
        fprintf(stdout, "\x1b[%dA", ui->patch_lines);
    }
    for (i = 0; i < ui->patch_lines; ++i) {
        fprintf(stdout, "\r\x1b[2K");
        if (i + 1 < ui->patch_lines) {
            fprintf(stdout, "\x1b[1B");
        }
    }
    if (ui->patch_lines > 1) {
        fprintf(stdout, "\x1b[%dA", ui->patch_lines - 1);
    }
    fprintf(stdout, "\r");
}

static void ui_render_patch_locked(ui_state_t *ui) {
    unsigned long long i;
    char downloaded_text[32];
    char total_text[32];

    ui_clear_patch_locked(ui);
    format_size(ui->downloaded_bytes, downloaded_text, sizeof(downloaded_text));
    format_size(ui->total_bytes, total_text, sizeof(total_text));
    printf("    %sprogress:%s %llu/%llu files | %s / %s | failed=%llu | jobs=%d\n",
           ui_color_stage(),
           ui_reset(),
           ui->finished_chunks,
           ui->total_chunks,
           downloaded_text,
           total_text,
           ui->failed_chunks,
           ui->jobs);
    for (i = 0; i < ui->total_chunks; ++i) {
        chunk_item_t *item = &ui->chunks[i];
        char item_bar[40];
        int percent = 0;

        render_progress_bar(item_bar, sizeof(item_bar), item->downloaded_size, item->expected_size);
        if (item->expected_size > 0) {
            percent = (int) ((item->downloaded_size * 100ULL) / item->expected_size);
            if (percent > 100) {
                percent = 100;
            }
        }
        printf("    %-12s %s %s%3d%%%s  %s%-5s%s  %s\n",
               item->name,
               item_bar,
               chunk_state_color(item->state),
               percent,
               ui_reset(),
               chunk_state_color(item->state),
               chunk_state_name(item->state),
               ui_reset(),
               item->detail);
    }
    fflush(stdout);
    ui->patch_live = 1;
    ui->patch_lines = (int) ui->total_chunks + 1;
}

static void ui_begin_patch(ui_state_t *ui, chunk_item_t *chunks, unsigned long long total_chunks,
                           unsigned long long total_bytes, int jobs) {
    EnterCriticalSection(&ui->lock);
    ui->chunks = chunks;
    ui->total_chunks = total_chunks;
    ui->total_bytes = total_bytes;
    ui->finished_chunks = 0;
    ui->failed_chunks = 0;
    ui->downloaded_bytes = 0;
    ui->jobs = jobs;
    ui->patch_live = 0;
    ui->patch_lines = 0;
    ui_render_patch_locked(ui);
    LeaveCriticalSection(&ui->lock);
}

static void ui_update_patch(ui_state_t *ui, unsigned long long finished, unsigned long long failed, unsigned long long bytes) {
    EnterCriticalSection(&ui->lock);
    ui->finished_chunks = finished;
    ui->failed_chunks = failed;
    ui->downloaded_bytes = bytes;
    ui_render_patch_locked(ui);
    LeaveCriticalSection(&ui->lock);
}

static void ui_finish_patch(ui_state_t *ui) {
    EnterCriticalSection(&ui->lock);
    ui_render_patch_locked(ui);
    ui->patch_live = 0;
    ui->patch_lines = 0;
    LeaveCriticalSection(&ui->lock);
    printf("\n");
    fflush(stdout);
}

static int manifest_set_value(manifest_t *manifest, const char *key, const char *value) {
    if (strcmp(key, "repo") == 0) {
        safe_copy(manifest->repo, sizeof(manifest->repo), value);
    } else if (strcmp(key, "ref") == 0) {
        safe_copy(manifest->ref, sizeof(manifest->ref), value);
    } else if (strcmp(key, "package_id") == 0) {
        safe_copy(manifest->package_id, sizeof(manifest->package_id), value);
    } else if (strcmp(key, "md5") == 0) {
        safe_copy(manifest->md5, sizeof(manifest->md5), value);
    } else if (strcmp(key, "file_name") == 0) {
        safe_copy(manifest->file_name, sizeof(manifest->file_name), value);
    } else if (strcmp(key, "chunk_pattern") == 0) {
        safe_copy(manifest->chunk_pattern, sizeof(manifest->chunk_pattern), value);
    } else if (strcmp(key, "proxy_list") == 0) {
        safe_copy(manifest->proxy_list, sizeof(manifest->proxy_list), value);
    } else if (strcmp(key, "total_size") == 0) {
        manifest->total_size = _strtoui64(value, NULL, 10);
    } else if (strcmp(key, "chunk_size") == 0) {
        manifest->chunk_size = _strtoui64(value, NULL, 10);
    } else if (strcmp(key, "chunk_count") == 0) {
        manifest->chunk_count = _strtoui64(value, NULL, 10);
    }
    return 0;
}

static int manifest_looks_valid(const manifest_t *manifest) {
    return manifest->repo[0] != '\0' && manifest->ref[0] != '\0' && manifest->package_id[0] != '\0' &&
           manifest->file_name[0] != '\0' && manifest->chunk_pattern[0] != '\0' && manifest->chunk_count != 0;
}

static int parse_manifest_buffer(const char *buffer, size_t size, manifest_t *manifest) {
    char *copy;
    char *line_ptr;
    char *next = NULL;

    memset(manifest, 0, sizeof(*manifest));
    copy = (char *) malloc(size + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, buffer, size);
    copy[size] = '\0';

    line_ptr = strtok_s(copy, "\r\n", &next);
    while (line_ptr != NULL) {
        char *eq = strchr(line_ptr, '=');
        if (eq != NULL && eq != line_ptr) {
            *eq = '\0';
            manifest_set_value(manifest, line_ptr, eq + 1);
        }
        line_ptr = strtok_s(NULL, "\r\n", &next);
    }

    free(copy);
    return manifest_looks_valid(manifest) ? 0 : -1;
}

static int ensure_directory(const char *path) {
    if (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return -1;
}

static int ensure_directory_recursive(const char *path) {
    char temp[MAX_PATH];
    size_t len;
    size_t i;

    safe_copy(temp, sizeof(temp), path);
    len = strlen(temp);
    if (len == 0) {
        return -1;
    }
    if (len > 3 && (temp[len - 1] == '\\' || temp[len - 1] == '/')) {
        temp[len - 1] = '\0';
        len--;
    }
    if (len == 3 && temp[1] == ':' && (temp[2] == '\\' || temp[2] == '/')) {
        return 0;
    }
    for (i = 3; temp[i] != '\0'; ++i) {
        if (temp[i] == '\\' || temp[i] == '/') {
            char saved = temp[i];
            temp[i] = '\0';
            if (ensure_directory(temp) != 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
                return -1;
            }
            temp[i] = saved;
        }
    }
    return ensure_directory(temp);
}

static int create_temp_output_file(file_logger_t *logger, const char *path, unsigned long long size, HANDLE *handle_out) {
    HANDLE file;
    LARGE_INTEGER file_size;

    DeleteFileA(path);
    file = CreateFileA(path,
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ,
                       NULL,
                       CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        log_message(logger, "failed to create temp file %s error=%lu", path, (unsigned long) GetLastError());
        return -1;
    }
    file_size.QuadPart = (LONGLONG) size;
    if (!SetFilePointerEx(file, file_size, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
        log_message(logger, "failed to allocate temp file %s error=%lu", path, (unsigned long) GetLastError());
        CloseHandle(file);
        DeleteFileA(path);
        return -1;
    }
    if (!SetFilePointerEx(file, (LARGE_INTEGER) {0}, NULL, FILE_BEGIN)) {
        log_message(logger, "failed to reset temp file pointer %s error=%lu", path, (unsigned long) GetLastError());
        CloseHandle(file);
        DeleteFileA(path);
        return -1;
    }
    *handle_out = file;
    return 0;
}

static int write_chunk_data(download_plan_t *plan, int chunk_index, const BYTE *data, DWORD size, unsigned long long chunk_offset) {
    LARGE_INTEGER offset;
    DWORD written = 0;
    chunk_item_t *item = &plan->chunks[chunk_index];

    offset.QuadPart = (LONGLONG) (((unsigned long long) chunk_index * plan->manifest.chunk_size) + chunk_offset);
    EnterCriticalSection(&plan->file_lock);
    if (!SetFilePointerEx(plan->temp_file, offset, NULL, FILE_BEGIN) || !WriteFile(plan->temp_file, data, size, &written, NULL) ||
        written != size) {
        LeaveCriticalSection(&plan->file_lock);
        return -1;
    }
    LeaveCriticalSection(&plan->file_lock);
    item->downloaded_size = chunk_offset + size;
    return 0;
}

static int compute_file_md5_hex(const char *path, char *md5_hex, size_t md5_hex_size) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE buffer[BUFFER_SIZE];
    DWORD bytes_read = 0;
    BYTE digest[16];
    DWORD digest_len = sizeof(digest);
    static const char hex_chars[] = "0123456789abcdef";
    DWORD i;
    int ok = -1;

    if (md5_hex_size < 33) {
        return -1;
    }
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        goto cleanup;
    }
    while (ReadFile(file, buffer, sizeof(buffer), &bytes_read, NULL) && bytes_read > 0) {
        if (!CryptHashData(hash, buffer, bytes_read, 0)) {
            goto cleanup;
        }
    }
    if (GetLastError() != ERROR_SUCCESS && GetLastError() != ERROR_HANDLE_EOF) {
        goto cleanup;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0) || digest_len != sizeof(digest)) {
        goto cleanup;
    }
    for (i = 0; i < digest_len; ++i) {
        md5_hex[i * 2] = hex_chars[(digest[i] >> 4) & 0x0F];
        md5_hex[i * 2 + 1] = hex_chars[digest[i] & 0x0F];
    }
    md5_hex[32] = '\0';
    ok = 0;

cleanup:
    if (hash != 0) {
        CryptDestroyHash(hash);
    }
    if (prov != 0) {
        CryptReleaseContext(prov, 0);
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    return ok;
}

static int join_path(char *out, size_t out_size, const char *dir, const char *file) {
    int written = snprintf(out, out_size, "%s\\%s", dir, file);
    return written > 0 && (size_t) written < out_size ? 0 : -1;
}

static int get_free_space_bytes(const char *path, unsigned long long *free_bytes) {
    ULARGE_INTEGER available;

    if (!GetDiskFreeSpaceExA(path, &available, NULL, NULL)) {
        return -1;
    }
    *free_bytes = (unsigned long long) available.QuadPart;
    return 0;
}

static int can_use_workspace_root(const char *path) {
    char probe_path[MAX_PATH];
    HANDLE probe;

    if (ensure_directory_recursive(path) != 0) {
        return -1;
    }
    if (join_path(probe_path, sizeof(probe_path), path, ".write-test.tmp") != 0) {
        return -1;
    }
    probe = CreateFileA(probe_path,
                        GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                        NULL);
    if (probe == INVALID_HANDLE_VALUE) {
        return -1;
    }
    CloseHandle(probe);
    DeleteFileA(probe_path);
    return 0;
}

static unsigned long long compute_required_space(const manifest_t *manifest) {
    return (manifest->total_size * 2ULL) + WORKSPACE_MARGIN_BYTES;
}

static int choose_workspace_root(const manifest_t *manifest, char *workspace_root, size_t workspace_root_size,
                                 unsigned long long *free_bytes_out, unsigned long long *required_bytes_out) {
    char candidate[MAX_PATH];
    DWORD drives;
    unsigned long long required = compute_required_space(manifest);
    char drive_root[4] = "C:\\";
    int pass;
    int i;

    *required_bytes_out = required;
    drives = GetLogicalDrives();
    for (pass = 0; pass < 3; ++pass) {
        for (i = 0; i < 26; ++i) {
            unsigned long long free_bytes = 0;
            UINT drive_type;

            if ((drives & (1u << i)) == 0) {
                continue;
            }

            if ((pass == 0 && i < 3) || (pass == 1 && i != 2) || (pass == 2 && i >= 3)) {
                continue;
            }

            drive_root[0] = (char) ('A' + i);
            drive_type = GetDriveTypeA(drive_root);
            if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE) {
                continue;
            }
            if (get_free_space_bytes(drive_root, &free_bytes) != 0 || free_bytes < required) {
                continue;
            }

            snprintf(candidate, sizeof(candidate), "%s%s", drive_root, WORKSPACE_DIR_NAME);
            if (can_use_workspace_root(candidate) != 0) {
                continue;
            }

            safe_copy(workspace_root, workspace_root_size, candidate);
            *free_bytes_out = free_bytes;
            return 0;
        }
    }
    return -1;
}

static int extract_proxy(const char *proxy_list, int index, char *proxy, size_t proxy_size) {
    int current = 0;
    const char *start = proxy_list;
    const char *cursor = proxy_list;

    while (1) {
        if (*cursor == ';' || *cursor == '\0') {
            if (current == index) {
                size_t len = (size_t) (cursor - start);
                if (len >= proxy_size) {
                    return -1;
                }
                memcpy(proxy, start, len);
                proxy[len] = '\0';
                return 0;
            }
            if (*cursor == '\0') {
                break;
            }
            current++;
            start = cursor + 1;
        }
        cursor++;
    }
    return -1;
}

static void build_api_asset_url(const char *repo, const char *ref, const char *package_id, const char *asset_name, char *url,
                                size_t url_size) {
    snprintf(url, url_size, "https://api.github.com/repos/%s/contents/%s/%s?ref=%s", repo, package_id, asset_name, ref);
}

static void build_api_root_manifest_url(const char *repo, const char *ref, char *url, size_t url_size) {
    snprintf(url, url_size, "https://api.github.com/repos/%s/contents/manifest.txt?ref=%s", repo, ref);
}

static void build_raw_asset_url(const char *repo, const char *ref, const char *package_id, const char *asset_name, char *url,
                                size_t url_size) {
    snprintf(url, url_size, "https://raw.githubusercontent.com/%s/%s/%s/%s", repo, ref, package_id, asset_name);
}

static void build_request_url(const char *proxy, const char *origin_url, char *url, size_t url_size) {
    if (_stricmp(proxy, "DIRECT") == 0) {
        safe_copy(url, url_size, origin_url);
    } else {
        snprintf(url, url_size, "%s%s", proxy, origin_url);
    }
}

static int split_url(const char *url, wchar_t *host, size_t host_size, wchar_t *path, size_t path_size, INTERNET_PORT *port,
                     int *secure) {
    URL_COMPONENTSW parts;
    wchar_t wide_url[MAX_STR];
    wchar_t url_path[MAX_STR];
    wchar_t extra_info[MAX_STR];

    memset(&parts, 0, sizeof(parts));
    memset(wide_url, 0, sizeof(wide_url));
    memset(url_path, 0, sizeof(url_path));
    memset(extra_info, 0, sizeof(extra_info));
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wide_url, (int) (sizeof(wide_url) / sizeof(wide_url[0])));

    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD) host_size;
    parts.lpszHostName = host;
    parts.dwUrlPathLength = (DWORD) (sizeof(url_path) / sizeof(url_path[0]));
    parts.lpszUrlPath = url_path;
    parts.dwExtraInfoLength = (DWORD) (sizeof(extra_info) / sizeof(extra_info[0]));
    parts.lpszExtraInfo = extra_info;

    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts)) {
        return -1;
    }

    host[parts.dwHostNameLength] = L'\0';
    url_path[parts.dwUrlPathLength] = L'\0';
    extra_info[parts.dwExtraInfoLength] = L'\0';
    if (_snwprintf(path, path_size, L"%ls%ls", url_path, extra_info) < 0) {
        return -1;
    }
    path[path_size - 1] = L'\0';

    *port = parts.nPort;
    *secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return 0;
}

static int query_content_type(HINTERNET request, char *buffer, size_t buffer_size) {
    wchar_t wide_buffer[256];
    DWORD size = sizeof(wide_buffer);

    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, wide_buffer, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        buffer[0] = '\0';
        return -1;
    }
    WideCharToMultiByte(CP_UTF8, 0, wide_buffer, -1, buffer, (int) buffer_size, NULL, NULL);
    buffer[buffer_size - 1] = '\0';
    return 0;
}

static int download_response(file_logger_t *logger, const char *url, http_buffer_t *response, int api_mode,
                             progress_callback_t progress_cb, void *progress_ctx, const char *progress_label) {
    static const wchar_t *api_headers =
        L"User-Agent: shaiya-installer/1.0\r\n"
        L"Accept: application/vnd.github.raw\r\n";
    static const wchar_t *raw_headers =
        L"User-Agent: shaiya-installer/1.0\r\n";
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    wchar_t host[MAX_STR];
    wchar_t path[MAX_STR];
    INTERNET_PORT port = 0;
    int secure = 0;
    BYTE buffer[BUFFER_SIZE];
    DWORD bytes_read = 0;
    DWORD status_code_size;
    DWORD content_length_dword = sizeof(DWORD);
    DWORD content_length_value = 0;
    size_t capacity = 0;
    int ok = -1;

    memset(response, 0, sizeof(*response));
    status_code_size = sizeof(response->status_code);
    if (split_url(url, host, MAX_STR, path, MAX_STR, &port, &secure) != 0) {
        log_message(logger, "invalid url: %s", url);
        return -1;
    }

    log_message(logger, "request url: %s", url);
    log_message(logger, "request host=%ls path=%ls", host, path);
    session = WinHttpOpen(L"ChunkInstaller/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        goto cleanup;
    }
    connect = WinHttpConnect(session, host, port, 0);
    if (connect == NULL) {
        goto cleanup;
    }
    request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == NULL) {
        goto cleanup;
    }
    if (!WinHttpSendRequest(request, api_mode ? api_headers : raw_headers, (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        goto cleanup;
    }
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &response->status_code,
                             &status_code_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        goto cleanup;
    }
    query_content_type(request, response->content_type, sizeof(response->content_type));
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &content_length_value,
                             &content_length_dword,
                             WINHTTP_NO_HEADER_INDEX)) {
        response->content_length = 0;
    } else {
        response->content_length = (unsigned long long) content_length_value;
    }
    log_message(logger, "response status=%lu content-type=%s", (unsigned long) response->status_code,
                response->content_type[0] != '\0' ? response->content_type : "(unknown)");
    if (response->status_code != 200) {
        goto cleanup;
    }

    do {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read)) {
            goto cleanup;
        }
        if (bytes_read > 0) {
            char *new_data;
            if (response->size + bytes_read + 1 > capacity) {
                size_t new_capacity = capacity == 0 ? (size_t) bytes_read + 1 : capacity * 2;
                while (new_capacity < response->size + bytes_read + 1) {
                    new_capacity *= 2;
                }
                new_data = (char *) realloc(response->data, new_capacity);
                if (new_data == NULL) {
                    goto cleanup;
                }
                response->data = new_data;
                capacity = new_capacity;
            }
            memcpy(response->data + response->size, buffer, bytes_read);
            response->size += bytes_read;
            if (progress_cb != NULL) {
                progress_cb(progress_ctx, progress_label, (unsigned long long) response->size, response->content_length, 0);
            }
        }
    } while (bytes_read > 0);

    if (response->data == NULL) {
        response->data = (char *) malloc(1);
        if (response->data == NULL) {
            goto cleanup;
        }
    }
    response->data[response->size] = '\0';
    if (progress_cb != NULL) {
        progress_cb(progress_ctx, progress_label, (unsigned long long) response->size, response->content_length, 1);
    }
    log_message(logger, "response bytes=%lu", (unsigned long) response->size);
    ok = 0;

cleanup:
    if (request != NULL) {
        WinHttpCloseHandle(request);
    }
    if (connect != NULL) {
        WinHttpCloseHandle(connect);
    }
    if (session != NULL) {
        WinHttpCloseHandle(session);
    }
    if (ok != 0) {
        log_message(logger, "request failed for %s", url);
        free(response->data);
        response->data = NULL;
        response->size = 0;
    }
    return ok;
}

static int write_buffer_to_file(const char *path, const char *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        DeleteFileA(path);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int download_manifest_from_url(file_logger_t *logger, const char *url, const char *preview_label, manifest_t *manifest) {
    http_buffer_t response;
    manifest_progress_ctx_t progress_ctx;

    progress_ctx.ui = &g_ui;
    progress_ctx.label = preview_label;
    if (download_response(logger, url, &response, 1, ui_manifest_progress, &progress_ctx, "manifest.txt") != 0) {
        return -1;
    }
    log_buffer_preview(logger, preview_label, response.data, response.size);
    if (strstr(response.data, "<html") != NULL || strstr(response.data, "<!DOCTYPE html") != NULL) {
        free(response.data);
        return -1;
    }
    if (parse_manifest_buffer(response.data, response.size, manifest) != 0) {
        free(response.data);
        return -1;
    }
    log_message(logger,
                "manifest parsed: package=%s file=%s chunks=%llu chunk_size=%llu total_size=%llu",
                manifest->package_id,
                manifest->file_name,
                manifest->chunk_count,
                manifest->chunk_size,
                manifest->total_size);
    free(response.data);
    return 0;
}

static int download_manifest_with_fallback(file_logger_t *logger, const char *repo, const char *ref, const char *package_id,
                                           const char *proxy_list, manifest_t *manifest) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_PROXY];
    int proxy_index = 0;

    build_api_asset_url(repo, ref, package_id, "manifest.txt", origin_url, sizeof(origin_url));
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message(logger, "downloading manifest.txt via %s", proxy);
        if (download_manifest_from_url(logger, request_url, "manifest", manifest) == 0) {
            return 0;
        }
    }
    return -1;
}

static int download_default_manifest_with_fallback(file_logger_t *logger, const char *repo, const char *ref, const char *proxy_list,
                                                   manifest_t *manifest) {
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_PROXY];
    int proxy_index = 0;

    build_api_root_manifest_url(repo, ref, origin_url, sizeof(origin_url));
    while (extract_proxy(proxy_list, proxy_index++, proxy, sizeof(proxy)) == 0) {
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message(logger, "downloading default manifest.txt via %s", proxy);
        if (download_manifest_from_url(logger, request_url, "default manifest", manifest) == 0) {
            return 0;
        }
    }
    return -1;
}

static void chunk_set_state(download_plan_t *plan, int idx, chunk_state_t state, DWORD status_code, DWORD error_code,
                            unsigned long long downloaded_size, const char *detail) {
    unsigned long long finished = 0;
    unsigned long long failed = 0;
    unsigned long long bytes = 0;
    unsigned long long total = plan->manifest.chunk_count;
    unsigned long long i;
    chunk_item_t *item;
    EnterCriticalSection(&plan->chunk_lock);
    item = &plan->chunks[idx];
    item->state = state;
    item->status_code = status_code;
    item->last_error = error_code;
    item->downloaded_size = downloaded_size;
    if (detail != NULL) {
        safe_copy(item->detail, sizeof(item->detail), detail);
    }

    for (i = 0; i < total; ++i) {
        bytes += plan->chunks[i].downloaded_size;
        if (plan->chunks[i].state == CHUNK_DONE) {
            finished++;
        } else if (plan->chunks[i].state == CHUNK_FAILED) {
            failed++;
        }
    }
    LeaveCriticalSection(&plan->chunk_lock);

    log_message(plan->logger,
                "chunk %s state=%s attempts=%d status=%lu error=%lu bytes=%llu expected=%llu detail=%s",
                item->name,
                chunk_state_name(state),
                item->attempts,
                (unsigned long) status_code,
                (unsigned long) error_code,
                downloaded_size,
                item->expected_size,
                item->detail);
    ui_update_patch(plan->ui, finished, failed, bytes);
}

static void ui_chunk_progress(void *ctx, const char *label, unsigned long long downloaded, unsigned long long total, int done) {
    chunk_progress_ctx_t *progress = (chunk_progress_ctx_t *) ctx;
    download_plan_t *plan = progress->plan;
    unsigned long long finished = 0;
    unsigned long long failed = 0;
    unsigned long long bytes = 0;
    unsigned long long i;
    chunk_item_t *item;

    (void) label;
    (void) done;
    EnterCriticalSection(&plan->chunk_lock);
    item = &plan->chunks[progress->chunk_index];
    item->state = CHUNK_DOWNLOADING;
    item->downloaded_size = downloaded;
    if (total > 0) {
        item->expected_size = total;
    }
    safe_copy(item->detail, sizeof(item->detail), "downloading");
    for (i = 0; i < plan->manifest.chunk_count; ++i) {
        bytes += plan->chunks[i].downloaded_size;
        if (plan->chunks[i].state == CHUNK_DONE) {
            finished++;
        } else if (plan->chunks[i].state == CHUNK_FAILED) {
            failed++;
        }
    }
    LeaveCriticalSection(&plan->chunk_lock);
    ui_update_patch(plan->ui, finished, failed, bytes);
}

static int download_chunk_to_target(download_plan_t *plan, int chunk_index, const char *asset_name, int *attempts_out,
                                    DWORD *status_out, DWORD *error_out, unsigned long long *size_out) {
    file_logger_t *logger = plan->logger;
    const manifest_t *manifest = &plan->manifest;
    char origin_url[MAX_STR];
    char request_url[MAX_STR];
    char proxy[MAX_PROXY];
    int proxy_index = 0;
    int attempts = 0;
    int ok = -1;

    build_raw_asset_url(manifest->repo, manifest->ref, manifest->package_id, asset_name, origin_url, sizeof(origin_url));
    while (extract_proxy(manifest->proxy_list[0] ? manifest->proxy_list : DEFAULT_PROXY_LIST, proxy_index++, proxy, sizeof(proxy)) == 0) {
        HINTERNET session = NULL;
        HINTERNET connect = NULL;
        HINTERNET request = NULL;
        wchar_t host[MAX_STR];
        wchar_t path[MAX_STR];
        INTERNET_PORT port = 0;
        int secure = 0;
        BYTE buffer[BUFFER_SIZE];
        DWORD bytes_read = 0;
        DWORD status_code_size = sizeof(DWORD);
        DWORD content_length_dword = sizeof(DWORD);
        DWORD content_length_value = 0;
        DWORD status_code = 0;
        unsigned long long downloaded = 0;
        chunk_progress_ctx_t progress_ctx;

        attempts++;
        build_request_url(proxy, origin_url, request_url, sizeof(request_url));
        log_message(logger, "downloading %s via %s", asset_name, proxy);

        if (split_url(request_url, host, MAX_STR, path, MAX_STR, &port, &secure) != 0) {
            continue;
        }
        session = WinHttpOpen(L"ChunkInstaller/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == NULL) {
            goto request_cleanup;
        }
        connect = WinHttpConnect(session, host, port, 0);
        if (connect == NULL) {
            goto request_cleanup;
        }
        request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == NULL) {
            goto request_cleanup;
        }
        if (!WinHttpSendRequest(request, L"User-Agent: shaiya-installer/1.0\r\n", (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, NULL)) {
            goto request_cleanup;
        }
        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status_code,
                                 &status_code_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            goto request_cleanup;
        }
        *status_out = status_code;
        if (status_code != 200) {
            goto request_cleanup;
        }
        if (WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &content_length_value,
                                &content_length_dword,
                                WINHTTP_NO_HEADER_INDEX)) {
            plan->chunks[chunk_index].expected_size = (unsigned long long) content_length_value;
        }

        progress_ctx.plan = plan;
        progress_ctx.chunk_index = chunk_index;
        while (WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0) {
            if (write_chunk_data(plan, chunk_index, buffer, bytes_read, downloaded) != 0) {
                goto request_cleanup;
            }
            downloaded += bytes_read;
            ui_chunk_progress(&progress_ctx, asset_name, downloaded, plan->chunks[chunk_index].expected_size, 0);
        }
        ui_chunk_progress(&progress_ctx, asset_name, downloaded, plan->chunks[chunk_index].expected_size, 1);
        *attempts_out = attempts;
        *error_out = 0;
        *size_out = downloaded;
        ok = 0;

request_cleanup:
        if (request != NULL) {
            WinHttpCloseHandle(request);
        }
        if (connect != NULL) {
            WinHttpCloseHandle(connect);
        }
        if (session != NULL) {
            WinHttpCloseHandle(session);
        }
        if (ok == 0) {
            return 0;
        }
    }

    *attempts_out = attempts;
    *error_out = GetLastError();
    *size_out = 0;
    return -1;
}

static DWORD WINAPI chunk_worker_thread(LPVOID param) {
    worker_ctx_t *ctx = (worker_ctx_t *) param;
    download_plan_t *plan = ctx->plan;

    for (;;) {
        LONG idx = InterlockedIncrement(&plan->next_index) - 1;
        char asset_name[64];
        int attempts = 0;
        DWORD status_code = 0;
        DWORD error_code = 0;
        unsigned long long size_out = 0;

        if ((unsigned long long) idx >= plan->manifest.chunk_count) {
            break;
        }

        snprintf(asset_name, sizeof(asset_name), plan->manifest.chunk_pattern, (int) idx);
        chunk_set_state(plan, (int) idx, CHUNK_DOWNLOADING, 0, 0, 0, "starting");
        if (download_chunk_to_target(plan, (int) idx, asset_name, &attempts, &status_code, &error_code, &size_out) == 0) {
            plan->chunks[idx].attempts = attempts;
            chunk_set_state(plan, (int) idx, CHUNK_DONE, status_code, 0, size_out, attempts > 1 ? "downloaded after retry" : "downloaded");
            InterlockedIncrement(&plan->finished_count);
        } else {
            plan->chunks[idx].attempts = attempts;
            chunk_set_state(plan, (int) idx, CHUNK_FAILED, status_code, error_code, size_out, "download failed");
            InterlockedExchange(&plan->failed_flag, 1);
        }
    }

    return 0;
}

static int run_parallel_downloads(download_plan_t *plan) {
    HANDLE *threads;
    worker_ctx_t *contexts;
    int created = 0;
    int i;
    int result = -1;

    threads = (HANDLE *) calloc(plan->jobs, sizeof(HANDLE));
    contexts = (worker_ctx_t *) calloc(plan->jobs, sizeof(worker_ctx_t));
    if (threads == NULL || contexts == NULL) {
        free(threads);
        free(contexts);
        return -1;
    }

    for (i = 0; i < plan->jobs; ++i) {
        contexts[i].plan = plan;
        contexts[i].worker_id = i + 1;
        threads[i] = CreateThread(NULL, 0, chunk_worker_thread, &contexts[i], 0, NULL);
        if (threads[i] == NULL) {
            InterlockedExchange(&plan->failed_flag, 1);
            break;
        }
        created++;
    }

    if (created > 0) {
        WaitForMultipleObjects(created, threads, TRUE, INFINITE);
    }
    result = (created == plan->jobs && InterlockedCompareExchange(&plan->failed_flag, 0, 0) == 0) ? 0 : -1;

    for (i = 0; i < created; ++i) {
        if (threads[i] != NULL) {
            CloseHandle(threads[i]);
        }
    }
    free(threads);
    free(contexts);
    return result;
}

static void cleanup_temp_file(file_logger_t *logger, const char *temp_file_path) {
    if (DeleteFileA(temp_file_path)) {
        log_message(logger, "removed temp file %s", temp_file_path);
    }
}

static int launch_program(file_logger_t *logger, const char *path) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char command[MAX_STR];
    char full_path[MAX_PATH];
    DWORD resolved;
    DWORD error_code;
    SHELLEXECUTEINFOA exec_info;

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    memset(full_path, 0, sizeof(full_path));
    resolved = GetFullPathNameA(path, sizeof(full_path), full_path, NULL);
    if (resolved == 0 || resolved >= sizeof(full_path)) {
        log_message(logger, "failed to resolve full path for %s", path);
        return -1;
    }

    log_message(logger, "launch target: %s", full_path);
    snprintf(command, sizeof(command), "\"%s\"", full_path);
    if (CreateProcessA(full_path, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

    error_code = GetLastError();
    log_message(logger, "CreateProcess failed for %s, error=%lu", full_path, (unsigned long) error_code);

    memset(&exec_info, 0, sizeof(exec_info));
    exec_info.cbSize = sizeof(exec_info);
    exec_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    exec_info.lpFile = full_path;
    exec_info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&exec_info)) {
        error_code = GetLastError();
        log_message(logger, "ShellExecuteEx failed for %s, error=%lu", full_path, (unsigned long) error_code);
        return -1;
    }
    if (exec_info.hProcess != NULL) {
        CloseHandle(exec_info.hProcess);
    }
    return 0;
}

static int parse_jobs_arg(const char *value) {
    int jobs = atoi(value);
    if (jobs <= 0) {
        jobs = DEFAULT_JOBS;
    }
    if (jobs > 16) {
        jobs = 16;
    }
    return jobs;
}

int main(int argc, char **argv) {
    manifest_t manifest;
    download_plan_t plan;
    char package_id[MAX_PATH] = "";
    char workspace_root[MAX_PATH] = "";
    char temp_file_path[MAX_PATH] = "";
    char merged_path[MAX_PATH];
    char actual_md5[33];
    unsigned long long free_bytes = 0;
    unsigned long long required_bytes = 0;
    int jobs = DEFAULT_JOBS;
    int i;

    memset(&plan, 0, sizeof(plan));
    plan.temp_file = INVALID_HANDLE_VALUE;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc) {
            jobs = parse_jobs_arg(argv[++i]);
        } else if (argv[i][0] != '-') {
            safe_copy(package_id, sizeof(package_id), argv[i]);
        }
    }
    if (init_logger(&g_logger) != 0) {
        g_logger.file = INVALID_HANDLE_VALUE;
    }
    init_ui(&g_ui, jobs);
    log_message(&g_logger, "ChunkInstaller %s", INSTALLER_VERSION);
    log_message(&g_logger, "log file: %s", g_logger.path[0] ? g_logger.path : "(disabled)");

    if (package_id[0] != '\0') {
        log_message(&g_logger, "mode=package package_id=%s jobs=%d", package_id, jobs);
        ui_print_line("%s==> 1/4 [META]%s resolving...", ui_color_stage(), ui_reset());
        ui_print_line("    %ssource:%s package %s", ui_color_dim(), ui_reset(), package_id);
        if (download_manifest_with_fallback(&g_logger, DEFAULT_REPO, DEFAULT_REF, package_id, DEFAULT_PROXY_LIST, &manifest) != 0) {
            ui_print_line("%s==> 1/4 [META]%s error", ui_color_err(), ui_reset());
            close_logger(&g_logger);
            return 1;
        }
        if (_stricmp(manifest.package_id, package_id) != 0) {
            log_message(&g_logger, "manifest package_id mismatch");
            ui_print_line("%s==> 1/4 [META]%s package_id mismatch", ui_color_err(), ui_reset());
            close_logger(&g_logger);
            return 1;
        }
    } else {
        log_message(&g_logger, "mode=default jobs=%d", jobs);
        ui_print_line("%s==> 1/4 [META]%s resolving...", ui_color_stage(), ui_reset());
        ui_print_line("    %ssource:%s manifest.txt", ui_color_dim(), ui_reset());
        if (download_default_manifest_with_fallback(&g_logger, DEFAULT_REPO, DEFAULT_REF, DEFAULT_PROXY_LIST, &manifest) != 0) {
            ui_print_line("%s==> 1/4 [META]%s error", ui_color_err(), ui_reset());
            close_logger(&g_logger);
            return 1;
        }
    }
    ui_print_line("%s==> 1/4 [META]%s done", ui_color_ok(), ui_reset());
    ui_print_blank();
    ui_print_line("%s==> 2/4 [PART]%s inspecting...", ui_color_stage(), ui_reset());
    ui_print_line("    %spackage:%s %s", ui_color_dim(), ui_reset(), manifest.package_id);
    ui_print_line("    %starget :%s %s", ui_color_dim(), ui_reset(), manifest.file_name);
    ui_print_line("    %schunks :%s %llu", ui_color_dim(), ui_reset(), manifest.chunk_count);
    ui_print_line("    %ssplit  :%s %llu bytes", ui_color_dim(), ui_reset(), manifest.chunk_size);
    ui_print_line("    %stotal  :%s %llu bytes", ui_color_dim(), ui_reset(), manifest.total_size);
    if (choose_workspace_root(&manifest, workspace_root, sizeof(workspace_root), &free_bytes, &required_bytes) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s insufficient disk space", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }
    ui_print_line("    %swork   :%s %s", ui_color_dim(), ui_reset(), workspace_root);
    log_message(&g_logger, "workspace selected: %s free=%llu required=%llu", workspace_root, free_bytes, required_bytes);

    if (join_path(temp_file_path, sizeof(temp_file_path), workspace_root, manifest.package_id) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error building temp path", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }
    if (join_path(merged_path, sizeof(merged_path), workspace_root, manifest.file_name) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error building output path", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }

    if (ensure_directory_recursive(workspace_root) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error creating workspace", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }
    if (create_temp_output_file(&g_logger, temp_file_path, manifest.total_size, &plan.temp_file) != 0) {
        ui_print_line("%s==> 2/4 [PART]%s error preparing temp file", ui_color_err(), ui_reset());
        close_logger(&g_logger);
        return 1;
    }

    plan.manifest = manifest;
    plan.jobs = jobs;
    plan.chunks = (chunk_item_t *) calloc((size_t) manifest.chunk_count, sizeof(chunk_item_t));
    plan.ui = &g_ui;
    plan.logger = &g_logger;
    safe_copy(plan.temp_file_path, sizeof(plan.temp_file_path), temp_file_path);
    InitializeCriticalSection(&plan.chunk_lock);
    InitializeCriticalSection(&plan.file_lock);
    if (plan.chunks == NULL) {
        CloseHandle(plan.temp_file);
        cleanup_temp_file(&g_logger, temp_file_path);
        close_logger(&g_logger);
        return 1;
    }
    for (i = 0; (unsigned long long) i < manifest.chunk_count; ++i) {
        plan.chunks[i].index = i;
        snprintf(plan.chunks[i].name, sizeof(plan.chunks[i].name), manifest.chunk_pattern, i);
        plan.chunks[i].state = CHUNK_QUEUED;
        plan.chunks[i].expected_size =
            (i == (int) manifest.chunk_count - 1)
                ? (manifest.total_size - (manifest.chunk_size * (unsigned long long) i))
                : manifest.chunk_size;
        safe_copy(plan.chunks[i].detail, sizeof(plan.chunks[i].detail), "queued");
    }

    ui_print_line("%s==> 2/4 [PART]%s downloading...", ui_color_stage(), ui_reset());
    ui_begin_patch(&g_ui, plan.chunks, manifest.chunk_count, manifest.total_size, jobs);
    if (run_parallel_downloads(&plan) != 0) {
        ui_finish_patch(&g_ui);
        ui_print_line("%s==> 2/4 [PART]%s error", ui_color_err(), ui_reset());
        CloseHandle(plan.temp_file);
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    ui_finish_patch(&g_ui);
    ui_print_line("%s==> 2/4 [PART]%s done", ui_color_ok(), ui_reset());

    ui_print_blank();
    CloseHandle(plan.temp_file);
    plan.temp_file = INVALID_HANDLE_VALUE;
    ui_print_line("%s==> 3/4 [VERIFY]%s validating...", ui_color_stage(), ui_reset());
    if (compute_file_md5_hex(temp_file_path, actual_md5, sizeof(actual_md5)) != 0) {
        ui_print_line("%s==> 3/4 [VERIFY]%s error", ui_color_err(), ui_reset());
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    if (_stricmp(actual_md5, manifest.md5) != 0) {
        log_message(&g_logger, "md5 mismatch expected=%s actual=%s", manifest.md5, actual_md5);
        ui_print_line("%s==> 3/4 [VERIFY]%s checksum mismatch", ui_color_err(), ui_reset());
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    if (!MoveFileExA(temp_file_path, merged_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        log_message(&g_logger, "failed to finalize output %s error=%lu", merged_path, (unsigned long) GetLastError());
        ui_print_line("%s==> 3/4 [VERIFY]%s error finalizing output", ui_color_err(), ui_reset());
        cleanup_temp_file(&g_logger, temp_file_path);
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    ui_print_line("%s==> 3/4 [VERIFY]%s done", ui_color_ok(), ui_reset());
    ui_print_line("    %soutput :%s %s", ui_color_dim(), ui_reset(), merged_path);

    ui_print_blank();
    ui_print_line("%s==> 4/4 [RUN]%s launching...", ui_color_stage(), ui_reset());
    if (launch_program(&g_logger, merged_path) != 0) {
        ui_print_line("%s==> 4/4 [RUN]%s error", ui_color_err(), ui_reset());
        free(plan.chunks);
        DeleteCriticalSection(&plan.chunk_lock);
        DeleteCriticalSection(&plan.file_lock);
        close_logger(&g_logger);
        return 1;
    }
    ui_print_line("%s==> 4/4 [RUN]%s done", ui_color_ok(), ui_reset());

    free(plan.chunks);
    DeleteCriticalSection(&plan.chunk_lock);
    DeleteCriticalSection(&plan.file_lock);
    close_logger(&g_logger);
    return 0;
}

#else
int main(void) {
    return 1;
}
#endif
