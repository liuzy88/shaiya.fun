#include "install.h"

#ifdef _WIN32

static const char *ui_color(const char *code) {
    return g_ui.vt_enabled ? code : "";
}

const char *ui_reset(void) {
    return g_ui.vt_enabled ? "\x1b[0m" : "";
}

const char *ui_color_stage(void) {
    return ui_color("\x1b[1;36m");
}

const char *ui_color_ok(void) {
    return ui_color("\x1b[1;32m");
}

const char *ui_color_err(void) {
    return ui_color("\x1b[1;31m");
}

const char *ui_color_dim(void) {
    return ui_color("\x1b[2m");
}

const char *ui_color_warn(void) {
    return ui_color("\x1b[1;33m");
}

const char *chunk_state_name(chunk_state_t state) {
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
int init_ui(ui_state_t *ui, int jobs) {
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

void ui_print_line(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void ui_print_blank(void) {
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

void ui_manifest_progress(void *ctx, const char *label, unsigned long long downloaded, unsigned long long total, int done) {
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
    char speed_text[32];
    ULONGLONG now = GetTickCount64();

    ui_clear_patch_locked(ui);
    format_size(ui->downloaded_bytes, downloaded_text, sizeof(downloaded_text));
    format_size(ui->total_bytes, total_text, sizeof(total_text));
    format_speed(ui->downloaded_bytes, ui->started_tick, now, speed_text, sizeof(speed_text));
    printf("    %sprogress:%s %llu/%llu files | %s / %s | failed=%llu | jobs=%d | speed=%s\n",
           ui_color_stage(),
           ui_reset(),
           ui->finished_chunks,
           ui->total_chunks,
           downloaded_text,
           total_text,
           ui->failed_chunks,
           ui->jobs,
           speed_text);
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

void ui_begin_patch(ui_state_t *ui, chunk_item_t *chunks, unsigned long long total_chunks,
                           unsigned long long total_bytes, int jobs) {
    EnterCriticalSection(&ui->lock);
    ui->chunks = chunks;
    ui->total_chunks = total_chunks;
    ui->total_bytes = total_bytes;
    ui->finished_chunks = 0;
    ui->failed_chunks = 0;
    ui->downloaded_bytes = 0;
    ui->started_tick = GetTickCount64();
    ui->jobs = jobs;
    ui->patch_live = 0;
    ui->patch_lines = 0;
    ui_render_patch_locked(ui);
    LeaveCriticalSection(&ui->lock);
}

void ui_update_patch(ui_state_t *ui, unsigned long long finished, unsigned long long failed, unsigned long long bytes) {
    EnterCriticalSection(&ui->lock);
    ui->finished_chunks = finished;
    ui->failed_chunks = failed;
    ui->downloaded_bytes = bytes;
    ui_render_patch_locked(ui);
    LeaveCriticalSection(&ui->lock);
}

void ui_finish_patch(ui_state_t *ui) {
    EnterCriticalSection(&ui->lock);
    ui_render_patch_locked(ui);
    ui->patch_live = 0;
    ui->patch_lines = 0;
    LeaveCriticalSection(&ui->lock);
    printf("\n");
    fflush(stdout);
}

#endif
