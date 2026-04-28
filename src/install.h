#ifndef INSTALL_H
#define INSTALL_H

#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stddef.h>
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
    ULONGLONG started_tick;
    ULONGLONG finished_tick;
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
    ULONGLONG started_tick;
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

typedef void (*progress_callback_t)(void *ctx, const char *label, unsigned long long downloaded,
                                    unsigned long long total, int done);

typedef struct {
    ui_state_t *ui;
    const char *label;
} manifest_progress_ctx_t;

typedef struct {
    download_plan_t *plan;
    int chunk_index;
} chunk_progress_ctx_t;

extern file_logger_t g_logger;
extern ui_state_t g_ui;

void safe_copy(char *dst, size_t dst_size, const char *src);
int checked_format(char *dst, size_t dst_size, const char *fmt, ...);
void format_size(unsigned long long bytes, char *buffer, size_t buffer_size);
void format_speed(unsigned long long bytes, ULONGLONG started_tick, ULONGLONG ended_tick, char *buffer, size_t buffer_size);
int is_safe_output_file_name(const char *file_name);
int ensure_directory_recursive(const char *path);
int join_path(char *out, size_t out_size, const char *dir, const char *file);

int init_logger(file_logger_t *logger);
void close_logger(file_logger_t *logger);
void log_message(file_logger_t *logger, const char *fmt, ...);
void log_buffer_preview(file_logger_t *logger, const char *label, const char *buffer, size_t size);

const char *ui_reset(void);
const char *ui_color_stage(void);
const char *ui_color_ok(void);
const char *ui_color_err(void);
const char *ui_color_dim(void);
const char *ui_color_warn(void);
const char *chunk_state_name(chunk_state_t state);
int init_ui(ui_state_t *ui, int jobs);
void ui_print_line(const char *fmt, ...);
void ui_print_blank(void);
void ui_manifest_progress(void *ctx, const char *label, unsigned long long downloaded,
                          unsigned long long total, int done);
void ui_begin_patch(ui_state_t *ui, chunk_item_t *chunks, unsigned long long total_chunks,
                    unsigned long long total_bytes, int jobs);
void ui_update_patch(ui_state_t *ui, unsigned long long finished, unsigned long long failed, unsigned long long bytes);
void ui_finish_patch(ui_state_t *ui);

int download_response(file_logger_t *logger, const char *url, http_buffer_t *response, int api_mode,
                      progress_callback_t progress_cb, void *progress_ctx, const char *progress_label);
int split_url(const char *url, wchar_t *host, size_t host_size, wchar_t *path, size_t path_size, INTERNET_PORT *port,
              int *secure);

int extract_proxy(const char *proxy_list, int index, char *proxy, size_t proxy_size);
int build_raw_asset_url(const char *repo, const char *ref, const char *package_id, const char *asset_name, char *url,
                        size_t url_size);
int build_request_url(const char *proxy, const char *origin_url, char *url, size_t url_size);
int download_manifest_with_fallback(file_logger_t *logger, const char *repo, const char *ref, const char *package_id,
                                    const char *proxy_list, manifest_t *manifest);
int download_default_manifest_with_fallback(file_logger_t *logger, const char *repo, const char *ref,
                                            const char *proxy_list, manifest_t *manifest);

int create_temp_output_file(file_logger_t *logger, const char *path, unsigned long long size, HANDLE *handle_out);
int compute_file_md5_hex(const char *path, char *md5_hex, size_t md5_hex_size);
int choose_workspace_root(const manifest_t *manifest, char *workspace_root, size_t workspace_root_size,
                          unsigned long long *free_bytes_out, unsigned long long *required_bytes_out);
int run_parallel_downloads(download_plan_t *plan);
void cleanup_temp_file(file_logger_t *logger, const char *temp_file_path);
int launch_program(file_logger_t *logger, const char *path);

#endif

#endif
